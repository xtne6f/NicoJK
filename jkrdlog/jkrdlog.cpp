#include "../ToolsCommon.h"
#include "../LogReader.h"
#include "../NetworkServiceIDTable.h"
#include <algorithm>
#include <chrono>
#ifndef _WIN32
#include <thread>
#ifndef NICOJK_LOG_DIR
#define NICOJK_LOG_DIR "/var/local/nicojk"
#endif
#endif

#ifdef _WIN32
int _tmain(int argc, TCHAR **argv)
#else
int main(int argc, char **argv)
#endif
{
	int readRatePerMille = 0;
	int jkID = 0;
	unsigned int tmReadFrom = 0;
	unsigned int tmReadTo = 0;

	if (argc < 4) {
		fprintf(stderr, "Usage: jkrdlog [-r readrate] jkid_or_ns{nid<<16+sid} from_unixtime to_unixtime_or_0.\n");
		return 2;
	}
	for (int i = 1; i < argc; ++i) {
		TCHAR c = TEXT('\0');
		if (argv[i][0] == TEXT('-') && argv[i][1] && !argv[i][2]) {
			c = argv[i][1];
		}
		bool bInvalid = false;
		if (i < argc - 3) {
			if (c == 'r') {
				double percent = _tcstod(argv[++i], nullptr);
				bInvalid = !(0 <= percent && percent <= 1000);
				if (!bInvalid) {
					readRatePerMille = static_cast<int>(percent * 10);
				}
			}
		} else if (i == argc - 3) {
			if (argv[i][0] == TEXT('n') && argv[i][1] == TEXT('s')) {
				NETWORK_SERVICE_ID_ELEM e;
				e.ntsID = _tcstoul(argv[i] + 2, nullptr, 10);
				// 上位と下位をひっくり返しているので補正
				e.ntsID = (e.ntsID << 16) | (e.ntsID >> 16);
				const NETWORK_SERVICE_ID_ELEM *pEnd = DEFAULT_NTSID_TABLE + sizeof(DEFAULT_NTSID_TABLE) / sizeof(DEFAULT_NTSID_TABLE[0]);
				const NETWORK_SERVICE_ID_ELEM *p = std::lower_bound(DEFAULT_NTSID_TABLE, pEnd, e,
					[](const NETWORK_SERVICE_ID_ELEM &a, const NETWORK_SERVICE_ID_ELEM &b) { return a.ntsID < b.ntsID; });
				if (p != pEnd && p->ntsID == e.ntsID) {
					jkID = p->jkID;
				}
			} else if (argv[i][0] == TEXT('j') && argv[i][1] == TEXT('k')) {
				jkID = _tcstol(argv[i] + 2, nullptr, 10);
			}
			bInvalid = jkID <= 0;
		} else if (i == argc - 2) {
			tmReadFrom = _tcstoul(argv[i], nullptr, 10);
			bInvalid = tmReadFrom == 0;
		} else {
			tmReadTo = _tcstoul(argv[i], nullptr, 10);
			bInvalid = (tmReadTo == 0 && readRatePerMille <= 0) || (tmReadTo != 0 && tmReadTo < tmReadFrom);
		}
		if (bInvalid) {
			fprintf(stderr, "Error: Argument %d is invalid.\n", i);
			return 1;
		}
	}
	if (jkID <= 0) {
		fprintf(stderr, "Error: Not enough arguments.\n");
		return 1;
	}

	CLogReader logReader;
#ifdef _WIN32
	TCHAR dir[MAX_PATH];
	DWORD nRet = GetModuleFileName(nullptr, dir, _countof(dir));
	if (nRet && nRet < _countof(dir)) {
		size_t i = _tcslen(dir);
		while (i > 0 && !_tcschr(TEXT("/\\"), dir[i - 1])) {
			dir[--i] = TEXT('\0');
		}
		if (i > 0) {
			dir[--i] = TEXT('\0');
		}
	} else {
		dir[0] = TEXT('\0');
	}
	if (!dir[0]) {
		fprintf(stderr, "Error: Unexpected.\n");
		return 1;
	}
	logReader.SetLogDirectory(dir);
#else
	logReader.SetLogDirectory(NICOJK_LOG_DIR);
#endif
	logReader.ResetCheckInterval();

	std::chrono::high_resolution_clock::time_point baseTime;
	if (readRatePerMille > 0) {
		// 毎秒速やかに出力するためバッファリングしない
		if (setvbuf(stdout, nullptr, _IONBF, 0) != 0) {
			fprintf(stderr, "Error: Unexpected.\n");
			return 1;
		}
		baseTime = std::chrono::high_resolution_clock::now();
	} else {
		// ウェイト無しなのでファイルチェックを間引かない
		logReader.SetCheckIntervalMsec(0);
	}
	std::string buf;
	std::string textBuf;
	bool bSkip = true;
	bool bRetry = false;
	unsigned int tmReading = tmReadFrom - 1;
	for (unsigned int tm = tmReadFrom; tmReadTo == 0 || tm < tmReadTo; ++tm) {
		// 秒ごとにまとめて出力
		buf.assign(76, ' ');
		buf += "-->\n";
		bool bFindNext = false;
		for (; tmReading <= tm; ++tmReading) {
			textBuf.clear();
			const char *text;
			while (logReader.Read(jkID, [&buf, &textBuf](LPCTSTR message) {
#ifdef _WIN32
				char utf8[512];
				if (WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, sizeof(utf8), nullptr, nullptr) != 0)
#else
				const char *utf8 = message;
#endif
				{
					buf += "<!-- M=";
					buf += utf8;
					buf += " -->\n";
				}
			}, &text, tmReading)) {
				textBuf += text;
				textBuf += '\n';
				if (!logReader.IsOpen()) {
					// 次の読み込みは確実に失敗するので省略
					break;
				}
			}
			if (bSkip) {
				// 1秒だけ手前から読むため
				bSkip = false;
				continue;
			}
			if (readRatePerMille <= 0) {
				// ウェイト無しなのでファイルチェックを毎秒行うと高負荷になるため
				if (!logReader.IsOpen()) {
					bSkip = true;
					bFindNext = true;
					break;
				}
			} else if (!bRetry && !textBuf.empty() && !logReader.IsOpen() && logReader.IsLatestLogfile()) {
				// もっとも新しいログファイルなので追記されるかもしれない
				// この時刻のログは追記中に読み込まれて不完全かもしれないのでリトライする
				bSkip = true;
				bRetry = true;
				--tmReading;
				break;
			}
			bRetry = false;
			buf += textBuf;
		}
		// 80文字のヘッダをつける
		char head[77];
#ifdef _WIN32
		int i = sprintf_s(
#else
		int i = sprintf(
#endif
			head, "<!-- J=%d;T=%u;L=%d;N=%d", jkID, tm, static_cast<int>(buf.size()) - 80,
			static_cast<int>(std::count(buf.begin(), buf.end(), '\n')) - 1);
		buf.replace(0, i, head);
		if (fputs(buf.c_str(), stdout) < 0) {
			break;
		}
		if (bFindNext) {
			// 次に読み込める時刻まで飛ぶ
			unsigned int tmJumpTo = logReader.FindNextReadableTime(jkID, tm + 1);
			if (tmJumpTo == 0 || tmJumpTo > tmReadTo) {
				tmJumpTo = tmReadTo;
			}
			bool bError = false;
			for (++tm; tm < tmJumpTo; ++tm, ++tmReading) {
				buf.assign(76, ' ');
				buf += "-->\n";
#ifdef _WIN32
				i = sprintf_s(
#else
				i = sprintf(
#endif
					head, "<!-- J=%d;T=%u;L=0;N=0", jkID, tm);
				buf.replace(0, i, head);
				if (fputs(buf.c_str(), stdout) < 0) {
					bError = true;
					break;
				}
			}
			if (bError) {
				break;
			}
			--tm;
		}
		if (readRatePerMille > 0) {
			for (;;) {
				auto elapsedMsec = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - baseTime);
				if ((tm - tmReadFrom) - elapsedMsec.count() * readRatePerMille / 1000000 < 0) {
					break;
				}
#ifdef _WIN32
				Sleep(100);
#else
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
			}
		}
	}
	fflush(stdout);
	return 0;
}
