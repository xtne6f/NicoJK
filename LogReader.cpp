#include "ToolsCommon.h"
#include "LogReader.h"
#include "zlib1/unzip.h"
#include <regex>

CLogReader::CLogReader()
	: currentJKID_(-1)
	, bLatestLogfile_(false)
	, bReadLogTextNext_(false)
	, tmReadLogText_(0)
	, checkInterval_(1000)
	, tmZippedLogfileCachedLast_(0)
{
	readLogText_[0][0] = '\0';
	readLogText_[1][0] = '\0';
}

void CLogReader::SetLogDirectory(LPCTSTR path)
{
	logDirectory_ = path;
}

void CLogReader::SetJK0LogfilePath(LPCTSTR path)
{
	jk0LogfilePath_ = path;
}

void CLogReader::SetCheckIntervalMsec(DWORD checkInterval)
{
	checkInterval_ = checkInterval;
}

void CLogReader::ResetCheckInterval()
{
	checkTick_ = std::chrono::steady_clock::now();
}

bool CLogReader::Read(int jkID, const std::function<void(LPCTSTR)> &onMessage, const char **text, unsigned int tmToRead)
{
	if (jkID != 0 && logDirectory_.empty()) {
		// ログを読まない
		jkID = -1;
	}
	std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
	if (currentJKID_ >= 0 && currentJKID_ != jkID) {
		// 閉じる
		readLogfile_.Close();
		checkTick_ = tick;
		currentJKID_ = -1;
		if (onMessage) {
			onMessage(TEXT("Closed logfile."));
		}
	}

	TCHAR jkDir[16];
#ifdef _WIN32
	_stprintf_s(jkDir, TEXT("\\jk%d"), jkID);
#else
	sprintf(jkDir, "/jk%d", jkID);
#endif
	if (tick >= checkTick_ && currentJKID_ < 0 && jkID >= 0) {
		// ファイルチェックを大量に繰りかえすのを防ぐ
		checkTick_ = tick + std::chrono::milliseconds(checkInterval_);
		tstring path;
		const char *zippedName = nullptr;
		LOGFILE_NAME latestZipBeforeTarget = {};
		if (jkID == 0) {
			// 指定ファイル再生
			path = jk0LogfilePath_;
			bLatestLogfile_ = false;
		} else {
			// jkIDのログファイル一覧を得る
			// tmToRead以前でもっとも新しいログファイルを探す
			TCHAR target[16];
#ifdef _WIN32
			_stprintf_s(
#else
			sprintf(
#endif
				target, TEXT("%010u."), tmToRead + (checkInterval_ / 1000 + 2));
			LOGFILE_NAME latestBeforeTarget = {};
			bLatestLogfile_ = true;
			EnumLogFile((logDirectory_ + jkDir).c_str(), [&](const LOGFILE_NAME &n) {
				bool bBeforeTarget = _tcscmp(n.name, target) < 0;
				if (!ComparePath(n.name + 10, TEXT(".txt"))) {
					// テキスト形式のログ
					if (!bBeforeTarget) {
						bLatestLogfile_ = false;
					} else if (_tcscmp(n.name, latestBeforeTarget.name) > 0) {
						latestBeforeTarget = n;
					}
				} else {
					// アーカイブされたログ
					if (bBeforeTarget && _tcscmp(n.name, latestZipBeforeTarget.name) > 0) {
						latestZipBeforeTarget = n;
					}
				}
			});
			if (latestBeforeTarget.name[0]) {
				// 見つかった
				path = logDirectory_ + jkDir + jkDir[0] + latestBeforeTarget.name;
			} else {
				bLatestLogfile_ = false;
			}
		}

		// まずテキスト形式のログを探す
		if (!path.empty()) {
			if (readLogfile_.Open(path.c_str())) {
				// readLogText_[!bReadLogTextNext_]はメソッド内で汎用できる
				char (&last)[CHAT_TAG_MAX] = readLogText_[!bReadLogTextNext_];
				unsigned int tmLast;
				// 最終行がtmToReadより過去なら読む価値無し
				if (!readLogfile_.ReadLastLine(last, CHAT_TAG_MAX) || !GetChatDate(&tmLast, last) || tmLast < tmToRead) {
					// 閉じる
					readLogfile_.Close();
				} else {
					// まず2分探索
					for (LONGLONG scale = 2; ; scale *= 2) {
						char (&middle)[CHAT_TAG_MAX] = readLogText_[!bReadLogTextNext_];
						int sign = 0;
						for (;;) {
							if (!readLogfile_.ReadLine(middle, CHAT_TAG_MAX)) {
								break;
							}
							unsigned int tmMiddle;
							if (GetChatDate(&tmMiddle, middle)) {
								sign = tmMiddle + 10 > tmToRead ? -1 : 1;
								break;
							}
						}
						// 行の時刻が得られないか最初の行がすでに未来ならリセット
						if (sign == 0 || (sign < 0 && scale == 2)) {
							readLogfile_.ResetPointer();
							break;
						}
						LONGLONG moveSize = readLogfile_.Seek(sign * scale);
#if defined(_WIN32) && defined(_DEBUG)
						TCHAR debug[64];
						_stprintf_s(debug, TEXT("CLogReader::Read() moveSize=%lld\n"), moveSize);
						OutputDebugString(debug);
#endif
						// 移動量が小さくなれば打ち切り
						if (-32 * 1024 < moveSize && moveSize < 32 * 1024) {
							// tmToReadよりも確実に過去になる位置まで戻す
							readLogfile_.Seek(-scale);
							// シーク直後の中途半端な1行を読み飛ばす
							readLogfile_.ReadLine(middle, 1);
							break;
						}
						readLogfile_.ReadLine(middle, 1);
					}
				}
			}
		}
		// テキスト形式のログがなければアーカイブされたログを探す
		if (!readLogfile_.IsOpen() && latestZipBeforeTarget.name[0]) {
			path = logDirectory_ + jkDir + jkDir[0] + latestZipBeforeTarget.name;
			bool bSameResult;
			zippedName = FindZippedLogfile(findZippedLogfileCache_, bSameResult, path.c_str(),
			                               tmToRead + (checkInterval_ / 1000 + 2), true);
			if (zippedName) {
				// 前回と同じ結果のとき、キャッシュした最終行の時刻があれば使う
				if (!bSameResult || tmZippedLogfileCachedLast_ == 0 || tmZippedLogfileCachedLast_ > tmToRead) {
					// 読む必要がある。シークはできない
#if defined(_WIN32) && defined(_DEBUG)
					OutputDebugString(TEXT("OpenZippedFile()\n"));
#endif
					readLogfile_.OpenZippedFile(path.c_str(), zippedName);
					tmZippedLogfileCachedLast_ = 0;
				}
			}
		}
		if (readLogfile_.IsOpen()) {
			// tmToReadより過去の行を読み飛ばす
			char (&next)[CHAT_TAG_MAX] = readLogText_[bReadLogTextNext_];
			unsigned int tm = 0;
			for (;;) {
				if (!readLogfile_.ReadLine(next, CHAT_TAG_MAX)) {
					// 閉じる
					readLogfile_.Close();
					if (zippedName) {
						// 最終行の時刻をキャッシュする
						tmZippedLogfileCachedLast_ = tm;
#if defined(_WIN32) && defined(_DEBUG)
						TCHAR debug[64];
						_stprintf_s(debug, TEXT("tmZippedLogfileCachedLast_=%u\n"), tm);
						OutputDebugString(debug);
#endif
					}
					break;
				} else if (GetChatDate(&tmReadLogText_, next)) {
					if (tmReadLogText_ > tmToRead) { // >=はダメ
						currentJKID_ = jkID;

						if (onMessage) {
							TCHAR log[256];
#ifdef _WIN32
							size_t lastSep = path.find_last_of(TEXT("/\\"));
							_stprintf_s(log, TEXT("Started reading logfile: jk%d/%.63s%s%S"),
#else
							size_t lastSep = path.find_last_of("/");
							sprintf(log, "Started reading logfile: jk%d/%.63s%s%s",
#endif
								jkID, &path.c_str()[lastSep == tstring::npos ? 0 : lastSep + 1],
								zippedName ? TEXT(":") : TEXT(""), zippedName ? zippedName : "");
							onMessage(log);
						}
						break;
					}
					tm = tmReadLogText_;
				}
			}
		}
	}
	bool bRet = false;
	// 開いてたら読み込む
	if (currentJKID_ >= 0) {
		if (readLogText_[bReadLogTextNext_][0] && tmReadLogText_ <= tmToRead) {
			*text = readLogText_[bReadLogTextNext_];
			bReadLogTextNext_ = !bReadLogTextNext_;
			readLogText_[bReadLogTextNext_][0] = '\0';
			bRet = true;
		}
		char (&next)[CHAT_TAG_MAX] = readLogText_[bReadLogTextNext_];
		if (!next[0]) {
			for (;;) {
				if (!readLogfile_.ReadLine(next, CHAT_TAG_MAX)) {
					// 閉じる
					readLogfile_.Close();
					checkTick_ = tick;
					currentJKID_ = -1;
					if (onMessage) {
						onMessage(TEXT("Closed logfile."));
					}
					break;
				} else if (GetChatDate(&tmReadLogText_, next)) {
					break;
				}
			}
		}
	}
	return bRet;
}

unsigned int CLogReader::FindNextReadableTime(int jkID, unsigned int tmToRead) const
{
	if (logDirectory_.empty()) {
		return 0;
	}
	// jkIDのログファイル一覧を得る
	// tmToRead以後でもっとも古いログファイルを探す
	TCHAR jkDir[16];
	TCHAR target[16];
#ifdef _WIN32
	_stprintf_s(jkDir, TEXT("\\jk%d"), jkID);
	_stprintf_s(
#else
	sprintf(jkDir, "/jk%d", jkID);
	sprintf(
#endif
		target, TEXT("%010u."), tmToRead);
	LOGFILE_NAME oldestAfterTarget = {};
	LOGFILE_NAME oldestZipAfterTarget = {};
	LOGFILE_NAME latestZipBeforeTarget = {};
	EnumLogFile((logDirectory_ + jkDir).c_str(), [&](const LOGFILE_NAME &n) {
		bool bBeforeTarget = _tcscmp(n.name, target) < 0;
		if (!ComparePath(n.name + 10, TEXT(".txt"))) {
			// テキスト形式のログ
			if (!bBeforeTarget && (!oldestAfterTarget.name[0] || _tcscmp(n.name, oldestAfterTarget.name) < 0)) {
				oldestAfterTarget = n;
			}
		} else {
			// アーカイブされたログ
			if (!bBeforeTarget && (!oldestZipAfterTarget.name[0] || _tcscmp(n.name, oldestZipAfterTarget.name) < 0)) {
				oldestZipAfterTarget = n;
			} else if (bBeforeTarget && _tcscmp(n.name, latestZipBeforeTarget.name) > 0) {
				latestZipBeforeTarget = n;
			}
		}
	});

	// アーカイブのうちtmToRead以後でもっとも古いログファイルを探す
	unsigned int tmZipped = 0;
	if (latestZipBeforeTarget.name[0]) {
		FIND_LOGFILE_CACHE cache;
		bool bSameResult;
		tstring path = logDirectory_ + jkDir + jkDir[0] + latestZipBeforeTarget.name;
		const char *zippedName = FindZippedLogfile(cache, bSameResult, path.c_str(), tmToRead, false);
		if (zippedName) {
			tmZipped = strtoul(zippedName, nullptr, 10);
		}
	}
	if (tmZipped == 0 && oldestZipAfterTarget.name[0]) {
		tmZipped = _tcstoul(oldestZipAfterTarget.name, nullptr, 10);
	}

	unsigned int tm = tmZipped;
	if (oldestAfterTarget.name[0]) {
		tm = _tcstoul(oldestAfterTarget.name, nullptr, 10);
		if (tmZipped != 0 && tm > tmZipped) {
			tm = tmZipped;
		}
	}
	return tm < tmToRead ? 0 : tm;
}

bool CLogReader::GetChatDate(unsigned int *tm, const char *tag)
{
	// TODO: dateは秒精度しかないので独自に属性値つけるかvposを解釈するとよりよいかも
	static const std::regex re("^<chat(?= )[^>]*? date=\"(\\d+)\"");
	std::cmatch m;
	if (std::regex_search(tag, m, re)) {
		*tm = strtoul(m[1].first, nullptr, 10);
		return true;
	}
	return false;
}

const char *CLogReader::FindZippedLogfile(FIND_LOGFILE_CACHE &cache, bool &bSameResult, LPCTSTR zipPath, unsigned int tm, bool bBeforeOrAfter)
{
	// アーカイブ内ファイルの列挙は比較的重いのでキャッシュする
	if (ComparePath(zipPath, cache.path.c_str())) {
		cache.path = zipPath;
		cache.list.clear();
		zlib_filefunc64_def def;
		fill_fopen64_filefunc(&def);
#ifdef _WIN32
		def.zopen64_file = CTextFileReader::TfopenSFileFuncForZlib;
#endif
		unzFile f = unzOpen2_64(zipPath, &def);
		if (f) {
			if (unzGoToFirstFile(f) == UNZ_OK) {
				do {
					FIND_LOGFILE_ELEM e = {};
					if (unzGetCurrentFileInfo64(f, nullptr, e.name, 15, nullptr, 0, nullptr, 0) == UNZ_OK &&
					    strlen(e.name) == 14 &&
					    !strchr(e.name, '/') &&
					    !unzStringFileNameCompare(e.name + 10, ".txt", 0)) {
						cache.list.push_back(e);
					}
				} while (unzGoToNextFile(f) == UNZ_OK);
			}
			unzClose(f);
		}
		cache.index = cache.list.size();
	}

	// tmより前/以後でもっとも新しい/古いログファイルを探す
	char target[16];
#ifdef _WIN32
	sprintf_s(
#else
	sprintf(
#endif
		target, "%010u.", tm);
	const char *name = nullptr;
	size_t lastIndex = cache.index;
	cache.index = cache.list.size();
	for (size_t i = 0; i < cache.list.size(); ++i) {
		if ((strcmp(cache.list[i].name, target) < 0) == bBeforeOrAfter &&
		    (!name || (strcmp(cache.list[i].name, name) > 0) == bBeforeOrAfter)) {
			name = cache.list[i].name;
			cache.index = i;
		}
	}
	bSameResult = name && cache.index == lastIndex;
	return name;
}
