#include "../Common.h"
#include "../JKStream.h"
#include "../JKTransfer.h"
#include "../JKIDNameTable.h"
#include "../NetworkServiceIDTable.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <algorithm>
#include <map>
#include <regex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
// 処理できるchatタグの最大文字数(char)
// (既定値はコメントの制限を1024文字として、これが実体参照であった場合の*5にマージンを加えた値)
const int CHAT_TAG_MAX = 1024 * 6;
// チューナーの状態をチェックする間隔
const int CHECK_TUNER_INTERVAL = 3000;
// コメントサーバ切断をチェックして再接続する間隔(あんまり短くしちゃダメ!)
const int JK_WATCHDOG_INTERVAL = 20000;
// チャンネル変更などでコメントサーバ切断してから再接続するまでの猶予
const int JK_WATCHDOG_RECONNEC_DELAY = 3000;
// ログファイルフォルダの更新をチェックする間隔
const int READ_LOG_FOLDER_INTERVAL = 3000;
// 投稿できる最大コメント文字数(たぶん安易に変更しないほうがいい)
const int POST_COMMENT_MAX = 76;
// 連投制限(短いと規制されるとのウワサ)
const int POST_COMMENT_INTERVAL = 2000;

int CreateLockfile(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if (fd >= 0) {
		struct stat st[2];
		if (flock(fd, LOCK_EX | LOCK_NB) != 0 || fstat(fd, st) != 0 || stat(path, st + 1) != 0 || st[0].st_ino != st[1].st_ino) {
			close(fd);
			fd = -1;
		}
	}
	return fd;
}

std::string GetPrivateProfileToString(const char *appName, const char *keyName, const char *lpDefault, const char *fileName)
{
	if (appName && keyName) {
		std::unique_ptr<FILE, fclose_deleter> fp(fopen(fileName, "re"));
		if (fp) {
			static const auto comp = [](char a, char b) { return ('a' <= a && a <= 'z' ? a - 'a' + 'A' : a) == ('a' <= b && b <= 'z' ? b - 'a' + 'A' : b); };
			size_t appNameLen = strlen(appName);
			size_t keyNameLen = strlen(keyName);
			std::string line;
			bool isApp = false;
			int c;
			do {
				c = fgetc(fp.get());
				c = c >= 0 && static_cast<char>(c) ? c : -1;
				if (c >= 0 && static_cast<char>(c) != '\n') {
					line += static_cast<char>(c);
					continue;
				}
				size_t n = line.find_last_not_of("\t\r ");
				line.erase(n == std::string::npos ? 0 : n + 1);
				line.erase(0, line.find_first_not_of("\t\r "));
				if (line.size() == appNameLen + 2 && line[0] == '[' && line.back() == ']' && std::equal(appName, appName + appNameLen, line.begin() + 1, comp)) {
					isApp = true;
				} else if (line[0] == '[') {
					isApp = false;
				} else if (isApp && (n = line.find('=')) != std::string::npos) {
					size_t m = line.find_last_not_of("\t\r =", n);
					if ((m == std::string::npos ? 0 : m + 1) == keyNameLen && std::equal(keyName, keyName + keyNameLen, line.begin(), comp)) {
						line.erase(0, line.find_first_not_of("\t\r ", n + 1));
						if (line.size() > 1 && (line[0] == '"' || line[0] == '\'') && line[0] == line.back()) {
							line.pop_back();
							line.erase(line.begin());
						}
						return line;
					}
				}
				line.clear();
			} while (c >= 0);
		}
	}
	return lpDefault ? lpDefault : "";
}

int GetPrivateProfileInt(const char *appName, const char *keyName, int nDefault, const char *fileName)
{
	std::string ret = GetPrivateProfileToString(appName, keyName, "", fileName);
	char *endp;
	int n = static_cast<int>(strtol(ret.c_str(), &endp, 10));
	return endp != ret.c_str() ? n : nDefault;
}

DWORD ArrayToDWORD(const unsigned char *data)
{
	return data[0] | data[1] << 8 | data[2] << 16 | static_cast<DWORD>(data[3]) << 24;
}

bool SendCtrlCmd(const unsigned char *cmd, size_t cmdSize, std::vector<unsigned char> &resData)
{
	// 接続
	int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (sock < 0) {
		return false;
	}
	sockaddr_un addr;
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, EDCB_INI_ROOT "/EpgTimerSrvPipe");
	if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		if (errno != EINPROGRESS) {
			close(sock);
			return false;
		}
		pollfd pfd;
		pfd.fd = sock;
		pfd.events = POLLOUT;
		if (poll(&pfd, 1, 5000) <= 0 || (pfd.revents & POLLOUT) == 0) {
			close(sock);
			return false;
		}
	}
	int x = 0;
	ioctl(sock, FIONBIO, &x);

	// 送信
	if (send(sock, cmd, cmdSize, 0) != static_cast<ssize_t>(cmdSize)) {
		close(sock);
		return false;
	}

	// 受信
	unsigned char head[8];
	size_t n = 0;
	for (ssize_t m; n < sizeof(head) && (m = recv(sock, head + n, sizeof(head) - n, 0)) > 0; n += m);
	if (n != sizeof(head)) {
		close(sock);
		return false;
	}
	resData.resize(ArrayToDWORD(head + 4));
	n = 0;
	for (ssize_t m; n < resData.size() && (m = recv(sock, resData.data() + n, resData.size() - n, 0)) > 0; n += m);

	close(sock);

	// 受信完了かつCMD_SUCCESSかどうか
	return n == resData.size() && ArrayToDWORD(head) == 1;
}

void ParseChSet5(const char *s, std::unordered_map<DWORD, std::vector<int>> &r)
{
	for (;;) {
		// ネットワークIDとTSIDとサービスIDの項目だけ取得
		size_t n = strcspn(s, "\n");
		size_t m = strcspn(s, "\t");
		if (m < n) {
			m += 1 + strcspn(s + m + 1, "\t");
			if (m < n) {
				unsigned long nid = strtoul(s + m + 1, nullptr, 10);
				m += 1 + strcspn(s + m + 1, "\t");
				if (nid <= 65535 && m < n) {
					unsigned long tsid = strtoul(s + m + 1, nullptr, 10);
					m += 1 + strcspn(s + m + 1, "\t");
					if (tsid <= 65535 && m < n) {
						unsigned long sid = strtoul(s + m + 1, nullptr, 10);
						if (sid <= 65535) {
							r[static_cast<DWORD>(nid << 16 | tsid)].push_back(static_cast<int>(sid));
						}
					}
				}
			}
		}
		if (!s[n]) {
			break;
		}
		s += n + 1;
	}
}

void ParseTunerProcessStatusInfoList(std::vector<unsigned char> &data, std::unordered_map<int, std::pair<DWORD, bool>> &r)
{
	if (data.size() < 8 || data.size() < ArrayToDWORD(&data[0])) {
		return;
	}
	data.resize(ArrayToDWORD(&data[0]));
	DWORD pos = 8;
	for (DWORD i = 0; i < ArrayToDWORD(&data[4]); ++i) {
		if (data.size() < pos + 52 || ArrayToDWORD(&data[pos]) < 52) {
			break;
		}
		// プロセスIDとネットワークIDとTSIDと録画中フラグの項目だけ取得
		int processID = ArrayToDWORD(&data[pos + 8]);
		int nid = ArrayToDWORD(&data[pos + 40]);
		int tsid = ArrayToDWORD(&data[pos + 44]);
		bool recFlag = data[pos + 48] != 0;
		bool epgCapFlag = data[pos + 49] != 0;
		if (nid >= 0 && nid <= 65535 && tsid >= 0 && tsid <= 65535 && !epgCapFlag) {
			r[processID] = std::make_pair(static_cast<DWORD>(nid << 16 | tsid), recFlag);
		}
		pos += ArrayToDWORD(&data[pos]);
	}
}

struct LOGFILE_CONTEXT
{
	int jkID;
	std::chrono::steady_clock::time_point tick;
	std::unique_ptr<FILE, fclose_deleter> file;
	int lockfile;
	std::string buf;
};

// 指定した実況IDのログファイルに書き込む
// jkIDが負値のときはログファイルを閉じる
void WriteToLogfile(LOGFILE_CONTEXT &ctx, int jkID, const char *text = nullptr, int logfileMode = 0, bool bRecording = false)
{
	if (logfileMode == 0 || (logfileMode == 1 && !bRecording)) {
		// ログを記録しない
		jkID = -1;
	}
	if (ctx.jkID >= 0 && ctx.jkID != jkID) {
		// 閉じる
		ctx.file.reset();
		// ロックファイルを削除
		char path[sizeof(NICOJK_LOG_DIR) + 64];
		sprintf(path, "%s/jk%d/lockfile", NICOJK_LOG_DIR, ctx.jkID);
		unlink(path);
		close(ctx.lockfile);
		ctx.jkID = -1;
	}
	if (ctx.jkID < 0 && jkID >= 0) {
		auto tick = std::chrono::steady_clock::now();
		static const std::regex re("^<chat(?= )[^>]*? date=\"(\\d+)\"");
		std::cmatch m;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(tick - ctx.tick).count() >= READ_LOG_FOLDER_INTERVAL && std::regex_search(text, m, re)) {
			ctx.tick = tick;
			time_t tmUnix = static_cast<time_t>(strtoul(m[1].first, nullptr, 10));
			char path[sizeof(NICOJK_LOG_DIR) + 64];
			sprintf(path, "%s/jk%d", NICOJK_LOG_DIR, jkID);
			if (mkdir(path, S_IRWXU | S_IRWXG | S_IRWXO) == 0) {
				fprintf(stderr, "Info: Created jk%d directory\n", jkID);
			}
			// ロックファイルを開く
			sprintf(path, "%s/jk%d/lockfile", NICOJK_LOG_DIR, jkID);
			ctx.lockfile = CreateLockfile(path);
			if (ctx.lockfile >= 0) {
				// 開く
				sprintf(path, "%s/jk%d/%010u.txt", NICOJK_LOG_DIR, jkID, static_cast<unsigned int>(tmUnix));
				ctx.file.reset(fopen(path, "we"));
				if (ctx.file) {
					setvbuf(ctx.file.get(), nullptr, _IONBF, 0);
					// ヘッダを書き込む(別に無くてもいい)
					tm tmLocal;
					if (!localtime_r(&tmUnix, &tmLocal)) {
						tm tmZero = {};
						tmLocal = tmZero;
						tmLocal.tm_year -= 1900;
						tmLocal.tm_mon -= 1;
					}
					fprintf(ctx.file.get(), "<!-- jktask logfile from %04d-%02d-%02dT%02d:%02d:%02d -->\r\n",
					        tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
					        tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec);
					ctx.jkID = jkID;
					ctx.tick = std::chrono::steady_clock::time_point();
				} else {
					sprintf(path, "%s/jk%d/lockfile", NICOJK_LOG_DIR, jkID);
					unlink(path);
					close(ctx.lockfile);
				}
			}
		}
	} else {
		ctx.tick = std::chrono::steady_clock::time_point();
	}
	// 開いてたら書き込む
	if (ctx.jkID >= 0) {
		ctx.buf = text;
		// 必ずCRLF
		ctx.buf += "\r\n";
		fputs(ctx.buf.c_str(), ctx.file.get());
	}
}

struct PER_TUNER_CONTEXT
{
	std::thread workerThread;
	CAutoResetEvent recvEvent;
	int jkID;
	std::string chatStreamID;
	std::string refugeChatStreamID;
	bool bRecording;
	bool bStop;
	bool bStopped;

	// 設定
	int logfileMode;
	std::string execGetCookie;
	std::string refugeUri;
	bool bDropForwardedComment;
	bool bRefugeMixing;
	bool bAnonymity;
};

void PerTunerThread(std::recursive_mutex &workerLock, int processID, PER_TUNER_CONTEXT &ctx)
{
	CJKTransfer jkTransfer;
	if (!jkTransfer.Open(&ctx.recvEvent, true, processID)) {
		fprintf(stderr, "Error: CJKTransfer::Open().\n");
		lock_recursive_mutex lock(workerLock);
		ctx.bStopped = true;
		return;
	}

	// 必要ならサーバに渡すCookieを取得
	char cookie[2048];
	cookie[0] = '\0';
	// 避難所のみに接続するときは実行しない
	if (!ctx.execGetCookie.empty() && (ctx.refugeUri.empty() || ctx.bRefugeMixing)) {
		std::string execGetCookie = "env --default-signal timeout 10s " + ctx.execGetCookie;
		fprintf(stderr, "Info: Executing popen(%s).\n", execGetCookie.c_str());
		FILE *fp = popen(execGetCookie.c_str(), "re");
		int exitStatus = -2;
		if (fp) {
			size_t n = fread(cookie, 1, sizeof(cookie), fp);
			exitStatus = pclose(fp);
			// 0でない終了ステータスや出力が多すぎるときは失敗。末尾に';'を置くため-1
			if (exitStatus || n >= sizeof(cookie) - 1) {
				cookie[0] = '\0';
			} else {
				cookie[n] = '\0';
			}
		}
		size_t i = strspn(cookie, " \t\n\r");
		for (size_t j = strlen(cookie); j > i && strchr(" \t\n\r", cookie[j - 1]); cookie[--j] = '\0');
		// 改行->';'
		size_t len = 0;
		for (; cookie[i]; ++i) {
			if (cookie[i] != '\r') {
				cookie[len++] = cookie[i] == '\n' ? ';' : cookie[i];
			}
		}
		if (len > 0 && cookie[len - 1] != ';') {
			cookie[len++] = ';';
		}
		cookie[len] = '\0';
		if (len > 0) {
			fprintf(stderr, "Info: Executed execGetCookie (len=%d).\n", static_cast<int>(len));
		} else {
			fprintf(stderr, "Warning: Execution of execGetCookie failed. popen() exit status is %d.\n", exitStatus);
		}
	}

	CJKStream jkStream;
	int jkID = -1;
	int jkIDToGet = -1;
	std::string chatStreamID;
	std::string refugeChatStreamID;
	bool bRecording = false;
	LOGFILE_CONTEXT logfileCtx;
	logfileCtx.jkID = -1;
	std::string lastPostComm;
	auto lastPostTick = std::chrono::steady_clock::now();
	auto watchdogTick = lastPostTick;
	int waitMsec = JK_WATCHDOG_RECONNEC_DELAY;
	bool bNicoReceivingPastChat = false;
	bool bRefugeReceivingPastChat = false;
	std::vector<char> jkBuf;
	for (;;) {
		auto elapsedMsec = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - watchdogTick);
		if (ctx.recvEvent.WaitOne(std::max(waitMsec - static_cast<int>(elapsedMsec.count()), 0))) {
			bool bReconnect = false;
			{
				lock_recursive_mutex lock(workerLock);
				if (ctx.bStop) {
					break;
				}
				if (jkIDToGet != ctx.jkID) {
					jkIDToGet = ctx.jkID;
					chatStreamID = ctx.chatStreamID;
					refugeChatStreamID = ctx.refugeChatStreamID;
					bReconnect = true;
				}
				bRecording = ctx.bRecording;
			}
			if (bReconnect) {
				jkStream.Shutdown();
				watchdogTick = std::chrono::steady_clock::now();
				waitMsec = JK_WATCHDOG_RECONNEC_DELAY;
			}

			jkBuf.clear();
			int ret = jkStream.ProcessRecv(jkBuf);
			if (ret < 0) {
				// 切断
				fprintf(stderr, "Info: Disconnected(%d) from jk%d.\n", ret, jkID);
				WriteToLogfile(logfileCtx, -1);
				jkID = -1;
			} else {
				// 受信中
				for (std::vector<char>::iterator it = jkBuf.begin();;) {
					std::vector<char>::iterator itEnd = std::find(it, jkBuf.end(), '\n');
					if (itEnd == jkBuf.end()) {
						break;
					}
					*itEnd = '\0';
					const char *rpl = &*it;
					if (!strncmp(rpl, "<chat ", 6)) {
						static const std::regex reChat("^<chat(?= )[^>]*? date=\"\\d+\".*>.*?</chat>");
						static const std::regex reRefuge("^<[^>]*? nx_jikkyo=\"1\"| x_refuge=\"1\"");
						if (itEnd - it < CHAT_TAG_MAX && std::regex_match(rpl, reChat)) {
							// nx_jikkyo|x_refuge属性(有志の避難所等による拡張)
							bool bRefuge = std::regex_search(rpl, reRefuge);
							bool bReceivingPastChat = bRefuge ? bRefugeReceivingPastChat : bNicoReceivingPastChat;
							// ログの不整合を避けるため過去のコメントは保存しない
							if (!bReceivingPastChat) {
								WriteToLogfile(logfileCtx, jkID, rpl, ctx.logfileMode, bRecording);
							}
							jkTransfer.SendChat(jkID, rpl);
						}
					} else {
						static const std::regex reChatResult("^<chat_result(?= )[^>]*? status=\"\\d+\"");
						static const std::regex reXRoom("^<x_room ");
						static const std::regex reIsRefuge("^<[^>]*? refuge=\"1\"");
						static const std::regex reXDisconnect("^<x_disconnect(?= )[^>]*? status=\"\\d+\"");
						static const std::regex reXPastChatBegin("^<x_past_chat_begin ");
						static const std::regex reXPastChatEnd("^<x_past_chat_end ");
						if (std::regex_search(rpl, reChatResult)) {
							// コメント投稿の応答を取得した
							jkTransfer.SendChat(jkID, rpl);
						} else if (std::regex_search(rpl, reXRoom)) {
							// 接続情報を取得した
							jkTransfer.SendChat(jkID, rpl);
						} else if (std::regex_search(rpl, reXDisconnect)) {
							// 混合接続時に個々切断した
							bool isRefuge = std::regex_search(rpl, reIsRefuge);
							(isRefuge ? bRefugeReceivingPastChat : bNicoReceivingPastChat) = false;
							jkTransfer.SendChat(jkID, rpl);
						} else if (std::regex_search(rpl, reXPastChatBegin)) {
							// 過去のコメントの出力開始
							bool isRefuge = std::regex_search(rpl, reIsRefuge);
							(isRefuge ? bRefugeReceivingPastChat : bNicoReceivingPastChat) = true;
						} else if (std::regex_search(rpl, reXPastChatEnd)) {
							// 過去のコメントの出力終了
							bool isRefuge = std::regex_search(rpl, reIsRefuge);
							(isRefuge ? bRefugeReceivingPastChat : bNicoReceivingPastChat) = false;
						}
					}
					it = itEnd + 1;
				}
			}

			std::string u8post = jkTransfer.ProcessRecvPost();
			size_t mailEndPos = u8post.find(']');
			if (mailEndPos != std::string::npos && u8post[0] == '[') {
				size_t u8commLen = u8post.size() - mailEndPos - 1;
				if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastPostTick).count() < POST_COMMENT_INTERVAL) {
					jkTransfer.SendChat(jkID, "<!-- M=Post error! Short interval. -->");
				} else if (u8commLen > 0) {
					// コードポイント換算の文字数
					size_t commLen = std::count_if(u8post.begin() + mailEndPos + 1, u8post.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80 || static_cast<unsigned char>(c) > 0xBF; });
					if (commLen > POST_COMMENT_MAX) {
						jkTransfer.SendChat(jkID, "<!-- M=Post error! Too long. -->");
					} else if (!u8post.compare(mailEndPos + 1, u8commLen, lastPostComm)) {
						jkTransfer.SendChat(jkID, "<!-- M=Post error! Same as previous. -->");
					} else {
						if (ctx.bAnonymity) {
							u8post.insert(mailEndPos, " 184");
							mailEndPos += 4;
						}
						u8post[mailEndPos] = '\0';
						fprintf(stderr, "Info: POST %s] (u8len=%d,len=%d).\n", u8post.c_str(), static_cast<int>(u8commLen), static_cast<int>(commLen));
						u8post[mailEndPos] = ']';
						// コメント投稿
						if (jkStream.Send(&ctx.recvEvent, '+', u8post.c_str())) {
							lastPostTick = std::chrono::steady_clock::now();
							lastPostComm.assign(u8post, mailEndPos + 1, u8commLen);
						} else {
							jkTransfer.SendChat(jkID, "<!-- M=Post error! Not connected. -->");
						}
					}
				}
			}
		} else {
			if (jkIDToGet >= 0) {
				if (!refugeChatStreamID.empty() && !ctx.refugeUri.empty()) {
					// 避難所に接続
					std::string uri = ctx.refugeUri;
					for (size_t i; (i = uri.find("{jkID}")) != std::string::npos;) {
						char text[16];
						sprintf(text, "jk%d", jkIDToGet);
						uri.replace(i, sizeof("{jkID}") - 1, text);
					}
					for (size_t i; (i = uri.find("{chatStreamID}")) != std::string::npos;) {
						uri.replace(i, sizeof("{chatStreamID}") - 1, refugeChatStreamID);
					}
					bool bMix = ctx.bRefugeMixing && !chatStreamID.empty();
					if (jkStream.Send(&ctx.recvEvent, 'R',
					                  ((ctx.bDropForwardedComment || bMix ? "2 " : "1 ") + uri +
					                   (bMix ? " " + chatStreamID + " " + cookie : "")).c_str())) {
						jkID = jkIDToGet;
						fprintf(stderr, "Info: Connecting to jk%d (%s%srefuge).\n", jkID, bMix ? chatStreamID.c_str() : "", bMix ? "+" : "");
						// 過去のコメントの出力状態をリセット
						bNicoReceivingPastChat = false;
						bRefugeReceivingPastChat = false;
					}
				} else if (!chatStreamID.empty() && (ctx.refugeUri.empty() || ctx.bRefugeMixing)) {
					// ニコニコ実況に接続
					if (jkStream.Send(&ctx.recvEvent, 'L', (chatStreamID + " " + cookie).c_str())) {
						jkID = jkIDToGet;
						fprintf(stderr, "Info: Connecting to jk%d (%s).\n", jkID, chatStreamID.c_str());
						// 過去のコメントの出力状態をリセット
						bNicoReceivingPastChat = false;
						bRefugeReceivingPastChat = false;
					}
				}
			}
			watchdogTick = std::chrono::steady_clock::now();
			waitMsec = std::max(JK_WATCHDOG_INTERVAL, 10000);
		}
	}

	WriteToLogfile(logfileCtx, -1);
	jkStream.Close();
	jkTransfer.Close();
	lock_recursive_mutex lock(workerLock);
	ctx.bStopped = true;
}
}

int main(int argc, char **argv)
{
	static_cast<void>(argc);
	static_cast<void>(argv);

	// このスレッドへの特定のシグナルの配送を止める
	sigset_t sset;
	sigemptyset(&sset);
	sigaddset(&sset, SIGHUP);
	sigaddset(&sset, SIGINT);
	sigaddset(&sset, SIGPIPE);
	sigaddset(&sset, SIGTERM);
	if (pthread_sigmask(SIG_BLOCK, &sset, nullptr) != 0) {
		fprintf(stderr, "Error: Signal block.\n");
		return 1;
	}
	std::recursive_mutex workerLock;
	CAutoResetEvent quitEvent;

	int globalLock = CreateLockfile(JKTASK_BASE_DIR "/jktask.lock");
	if (globalLock < 0) {
		fprintf(stderr, "Error: Cannot create lockfile.\n");
		return 1;
	}

	std::thread mainThread([&workerLock, &quitEvent]() {
		std::vector<unsigned char> resData;
		std::unordered_map<DWORD, std::vector<int>> chMap;
		std::unordered_map<int, std::pair<DWORD, bool>> processMap, lastProcessMap;
		std::map<int, std::unique_ptr<PER_TUNER_CONTEXT>> perTunerMap;
		int retryCount = 0;
		while (!quitEvent.WaitOne(CHECK_TUNER_INTERVAL)) {
			if (chMap.empty()) {
				static const unsigned char cmdEpgSrvFileCopy[] = {
					0x24, 0x04, 0, 0, // CMD2_EPG_SRV_FILE_COPY
					26, 0, 0, 0, // cmd size
					26, 0, 0, 0, // str size
					'C', 0, 'h', 0, 'S', 0, 'e', 0, 't', 0, '5', 0, '.', 0, 't', 0, 'x', 0, 't', 0, 0, 0 // "ChSet5.txt\0"
				};
				if (SendCtrlCmd(cmdEpgSrvFileCopy, sizeof(cmdEpgSrvFileCopy), resData) && resData.size() > 0) {
					resData.push_back(0);
					ParseChSet5(reinterpret_cast<const char *>(resData.data()), chMap);
					if (!chMap.empty()) {
						fprintf(stderr, "Info: Channel information has been retrieved.\n");
					}
				}
			} else {
				static const unsigned char cmdEpgSrvEnumTunerProcess[] = {
					0x2A, 0x04, 0, 0, // CMD2_EPG_SRV_ENUM_TUNER_PROCESS
					0, 0, 0, 0 // cmd size
				};
				if (SendCtrlCmd(cmdEpgSrvEnumTunerProcess, sizeof(cmdEpgSrvEnumTunerProcess), resData)) {
					retryCount = 0;
					lastProcessMap = processMap;
					processMap.clear();
					ParseTunerProcessStatusInfoList(resData, processMap);
				} else if (++retryCount > 10) {
					fprintf(stderr, "Warning: Failed to retrieve tuner information.\n");
					retryCount = 0;
					lastProcessMap = processMap;
					processMap.clear();
				}
			}

			for (auto it = perTunerMap.begin(); it != perTunerMap.end();) {
				if (!processMap.count(it->first)) {
					if (!it->second->bStop) {
						fprintf(stderr, "Info: Stopping the thread for tuner process %d.\n", it->first);
						lock_recursive_mutex lock(workerLock);
						it->second->bStop = true;
						it->second->recvEvent.Set();
					}
					bool bStopped;
					{
						lock_recursive_mutex lock(workerLock);
						bStopped = it->second->bStopped;
					}
					if (bStopped) {
						it->second->workerThread.join();
						fprintf(stderr, "Info: Stopped the thread for tuner process %d.\n", it->first);
						perTunerMap.erase(it++);
					} else {
						++it;
					}
				} else {
					++it;
				}
			}

			static const char iniPath[] = JKTASK_BASE_DIR "/jktask.ini";
			for (auto process : processMap) {
				auto it = perTunerMap.find(process.first);
				auto itLast = lastProcessMap.find(process.first);
				DWORD chID = process.second.first;
				bool bRecording = process.second.second;
				int jkID = -1;
				if (it != perTunerMap.end() && itLast != lastProcessMap.end() && itLast->second.first == chID) {
					// チャンネル変化なしのため省略
					jkID = it->second->jkID;
				} else if (chMap.count(chID)) {
					int nid = chID >> 16;
					for (int sid : chMap[chID]) {
						DWORD ntsID = 0x7880 <= nid && nid <= 0x7FEF ? static_cast<DWORD>(sid & ~0x0187) << 16 | 0x000F : static_cast<DWORD>(sid) << 16 | nid;
						const NETWORK_SERVICE_ID_ELEM *pEnd = DEFAULT_NTSID_TABLE + sizeof(DEFAULT_NTSID_TABLE) / sizeof(DEFAULT_NTSID_TABLE[0]);
						const NETWORK_SERVICE_ID_ELEM *p = lower_bound_first(DEFAULT_NTSID_TABLE, pEnd, ntsID);
						if (p != pEnd && p->first == ntsID) {
							jkID = p->jkID;
						}
						// 設定ファイルの定義では上位と下位をひっくり返しているので補正
						DWORD nsID = (ntsID << 16) | (ntsID >> 16);
						char key[16];
						sprintf(key, "0x%x", nsID);
						jkID = GetPrivateProfileInt("Channels", key, jkID, iniPath);
						if (jkID >= 0) {
							break;
						}
					}
				}

				std::string chatStreamID;
				std::string refugeChatStreamID;
				if (jkID >= 0 && (it == perTunerMap.end() || it->second->jkID != jkID)) {
					const JKID_NAME_ELEM *pEnd = DEFAULT_JKID_NAME_TABLE + sizeof(DEFAULT_JKID_NAME_TABLE) / sizeof(DEFAULT_JKID_NAME_TABLE[0]);
					const JKID_NAME_ELEM *p = lower_bound_first(DEFAULT_JKID_NAME_TABLE, pEnd, jkID);
					char key[16];
					sprintf(key, "%d", jkID);
					bool bFirstVal = true;
					for (char c : GetPrivateProfileToString("ChatStreams", key, p != pEnd && p->first == jkID && p->chatStreamID ? p->chatStreamID : "", iniPath)) {
						if (('0' <= c && c <= '9') || ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z')) {
							refugeChatStreamID += c;
							if (bFirstVal) {
								chatStreamID += c;
							}
						} else if (c == ',' && bFirstVal) {
							// カンマ区切りがあれば後者が避難所の値
							refugeChatStreamID.clear();
							bFirstVal = false;
						} else {
							chatStreamID.clear();
							refugeChatStreamID.clear();
							fprintf(stderr, "Warning: The key %s of [ChatStreams] is invalid.\n", key);
							break;
						}
					}
				}

				if (it == perTunerMap.end()) {
					it = perTunerMap.emplace(process.first, std::unique_ptr<PER_TUNER_CONTEXT>(new PER_TUNER_CONTEXT())).first;
					it->second->jkID = jkID;
					it->second->chatStreamID.swap(chatStreamID);
					it->second->refugeChatStreamID.swap(refugeChatStreamID);
					it->second->bRecording = bRecording;
					it->second->bStop = false;
					it->second->bStopped = false;
					it->second->logfileMode = GetPrivateProfileInt("Setting", "logfileMode", 0, iniPath);
					it->second->execGetCookie = GetPrivateProfileToString("Setting", "execGetCookie", "", iniPath);
					it->second->refugeUri = GetPrivateProfileToString("Setting", "refugeUri", "", iniPath);
					it->second->bDropForwardedComment = GetPrivateProfileInt("Setting", "dropForwardedComment", 0, iniPath) != 0;
					it->second->bRefugeMixing = !it->second->refugeUri.empty() && GetPrivateProfileInt("Setting", "refugeMixing", 0, iniPath) != 0;
					it->second->bAnonymity = GetPrivateProfileInt("Setting", "anonymity", 1, iniPath) != 0;
					it->second->recvEvent.Set();
					it->second->workerThread = std::thread([&workerLock, it]() { PerTunerThread(workerLock, it->first, *it->second); });
					fprintf(stderr, "Info: Started the thread for tuner process %d (jk%d,recording=%s).\n", it->first, jkID, bRecording ? "true" : "false");
				} else if (!it->second->bStop) {
					if (it->second->jkID != jkID) {
						fprintf(stderr, "Info: The channel state of tuner process %d has changed from jk%d to jk%d.\n", it->first, it->second->jkID, jkID);
						lock_recursive_mutex lock(workerLock);
						it->second->jkID = jkID;
						it->second->chatStreamID.swap(chatStreamID);
						it->second->refugeChatStreamID.swap(refugeChatStreamID);
						it->second->recvEvent.Set();
					}
					if (it->second->bRecording != bRecording) {
						fprintf(stderr, "Info: The recording state of tuner process %d has changed to %s.\n", it->first, bRecording ? "true" : "false");
						lock_recursive_mutex lock(workerLock);
						it->second->bRecording = bRecording;
						it->second->recvEvent.Set();
					}
				}
			}
		}

		for (auto &ctx : perTunerMap) {
			if (!ctx.second->bStop) {
				fprintf(stderr, "Info: Stopping the thread for tuner process %d.\n", ctx.first);
				lock_recursive_mutex lock(workerLock);
				ctx.second->bStop = true;
				ctx.second->recvEvent.Set();
			}
		}
		for (const auto &ctx : perTunerMap) {
			ctx.second->workerThread.join();
			fprintf(stderr, "Info: Stopped the thread for tuner process %d.\n", ctx.first);
		}
	});

	for (;;) {
		// 特定のシグナルを待つ
		int signum = sigwaitinfo(&sset, nullptr);
		if (signum == SIGHUP || signum == SIGINT || signum == SIGTERM) {
			fprintf(stderr, "Info: Received signal %d.\n", signum);
			break;
		}
	}

	quitEvent.Set();
	mainThread.join();

	unlink(JKTASK_BASE_DIR "/jktask.lock");
	close(globalLock);
	return 0;
}
