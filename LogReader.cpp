#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tchar.h>
#include "LogReader.h"
#include "unzip.h"
#include <regex>

namespace
{
// FindFirstFile()の結果を列挙する
template<class P>
void EnumFindFile(LPCTSTR pattern, P enumProc)
{
	WIN32_FIND_DATA findData;
	HANDLE hFind = FindFirstFile(pattern, &findData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			enumProc(findData);
		} while (FindNextFile(hFind, &findData));
		FindClose(hFind);
	}
}
}

CLogReader::CLogReader()
	: currentJKID_(-1)
	, bLatestLogfile_(false)
	, bReadLogTextNext_(false)
	, tmReadLogText_(0)
	, checkInterval_(1000)
	, checkTick_(0)
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
	checkTick_ = GetTickCount();
}

bool CLogReader::Read(int jkID, const std::function<void(LPCTSTR)> &onMessage, const char **text, unsigned int tmToRead)
{
	if (jkID != 0 && logDirectory_.empty()) {
		// ログを読まない
		jkID = -1;
	}
	DWORD tick = GetTickCount();
	if (currentJKID_ >= 0 && currentJKID_ != jkID) {
		// 閉じる
		readLogfile_.Close();
		checkTick_ = tick;
		currentJKID_ = -1;
		if (onMessage) {
			onMessage(TEXT("Closed logfile."));
		}
	}
	if (!((tick - checkTick_) & 0x80000000) && currentJKID_ < 0 && jkID >= 0) {
		// ファイルチェックを大量に繰りかえすのを防ぐ
		checkTick_ = tick + checkInterval_;
		std::basic_string<TCHAR> path;
		const char *zippedName = nullptr;
		TCHAR latestZipBeforeTarget[16] = {};
		if (jkID == 0) {
			// 指定ファイル再生
			path = jk0LogfilePath_;
			bLatestLogfile_ = false;
		} else {
			// jkIDのログファイル一覧を得る
			TCHAR pattern[64];
			_stprintf_s(pattern, TEXT("\\jk%d\\??????????.???"), jkID);
			// tmToRead以前でもっとも新しいログファイルを探す
			TCHAR target[16];
			_stprintf_s(target, TEXT("%010u."), tmToRead + (checkInterval_ / 1000 + 2));
			TCHAR latestBeforeTarget[16] = {};
			bLatestLogfile_ = true;
			EnumFindFile((logDirectory_ + pattern).c_str(), [&](const WIN32_FIND_DATA &fd) {
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && _tcslen(fd.cFileName) == 14) {
					bool bBeforeTarget = _tcscmp(fd.cFileName, target) < 0;
					if (!_tcsicmp(fd.cFileName + 10, TEXT(".txt"))) {
						// テキスト形式のログ
						if (!bBeforeTarget) {
							bLatestLogfile_ = false;
						} else if (_tcscmp(fd.cFileName, latestBeforeTarget) > 0) {
							_tcscpy_s(latestBeforeTarget, fd.cFileName);
						}
					} else if (!_tcsicmp(fd.cFileName + 10, TEXT(".zip"))) {
						// アーカイブされたログ
						if (bBeforeTarget && _tcscmp(fd.cFileName, latestZipBeforeTarget) > 0) {
							_tcscpy_s(latestZipBeforeTarget, fd.cFileName);
						}
					}
				}
			});
			if (latestBeforeTarget[0]) {
				// 見つかった
				_stprintf_s(pattern, TEXT("\\jk%d\\%s"), jkID, latestBeforeTarget);
				path = logDirectory_ + pattern;
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
				if (!readLogfile_.ReadLastLine(last, _countof(last)) || !GetChatDate(&tmLast, last) || tmLast < tmToRead) {
					// 閉じる
					readLogfile_.Close();
				} else {
					// まず2分探索
					for (LONGLONG scale = 2; ; scale *= 2) {
						char (&middle)[CHAT_TAG_MAX] = readLogText_[!bReadLogTextNext_];
						int sign = 0;
						for (;;) {
							if (!readLogfile_.ReadLine(middle, _countof(middle))) {
								break;
							}
							unsigned int tmMiddle;
							if (GetChatDate(&tmMiddle, middle)) {
								sign = tmMiddle + 10 > tmToRead ? -1 : 1;
								break;
							}
						}
						// 行の時刻が得られないか最初の行がすでに未来ならリセット
						if (sign == 0 || sign < 0 && scale == 2) {
							readLogfile_.ResetPointer();
							break;
						}
						LONGLONG moveSize = readLogfile_.Seek(sign * scale);
#ifdef _DEBUG
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
		if (!readLogfile_.IsOpen() && latestZipBeforeTarget[0]) {
			TCHAR pattern[64];
			_stprintf_s(pattern, TEXT("\\jk%d\\%s"), jkID, latestZipBeforeTarget);
			path = logDirectory_ + pattern;
			bool bSameResult;
			zippedName = FindZippedLogfile(findZippedLogfileCache_, bSameResult, path.c_str(),
			                               tmToRead + (checkInterval_ / 1000 + 2), true);
			if (zippedName) {
				// 前回と同じ結果のとき、キャッシュした最終行の時刻があれば使う
				if (!bSameResult || tmZippedLogfileCachedLast_ == 0 || tmZippedLogfileCachedLast_ > tmToRead) {
					// 読む必要がある。シークはできない
#ifdef _DEBUG
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
				if (!readLogfile_.ReadLine(next, _countof(next))) {
					// 閉じる
					readLogfile_.Close();
					if (zippedName) {
						// 最終行の時刻をキャッシュする
						tmZippedLogfileCachedLast_ = tm;
#ifdef _DEBUG
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
							size_t lastSep = path.find_last_of(TEXT("/\\"));
							_stprintf_s(log, TEXT("Started reading logfile: jk%d\\%.63s%s%S"),
							            jkID, &path.c_str()[lastSep == std::basic_string<TCHAR>::npos ? 0 : lastSep + 1],
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
				if (!readLogfile_.ReadLine(next, _countof(next))) {
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
	TCHAR pattern[64];
	_stprintf_s(pattern, TEXT("\\jk%d\\??????????.???"), jkID);
	// tmToRead以後でもっとも古いログファイルを探す
	TCHAR target[16];
	_stprintf_s(target, TEXT("%010u."), tmToRead);
	TCHAR oldestAfterTarget[16] = {};
	TCHAR oldestZipAfterTarget[16] = {};
	TCHAR latestZipBeforeTarget[16] = {};
	EnumFindFile((logDirectory_ + pattern).c_str(), [&](const WIN32_FIND_DATA &fd) {
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && _tcslen(fd.cFileName) == 14) {
			bool bBeforeTarget = _tcscmp(fd.cFileName, target) < 0;
			if (!_tcsicmp(fd.cFileName + 10, TEXT(".txt"))) {
				// テキスト形式のログ
				if (!bBeforeTarget && (!oldestAfterTarget[0] || _tcscmp(fd.cFileName, oldestAfterTarget) < 0)) {
					_tcscpy_s(oldestAfterTarget, fd.cFileName);
				}
			} else if (!_tcsicmp(fd.cFileName + 10, TEXT(".zip"))) {
				// アーカイブされたログ
				if (!bBeforeTarget && (!oldestZipAfterTarget[0] || _tcscmp(fd.cFileName, oldestZipAfterTarget) < 0)) {
					_tcscpy_s(oldestZipAfterTarget, fd.cFileName);
				} else if (bBeforeTarget && _tcscmp(fd.cFileName, latestZipBeforeTarget) > 0) {
					_tcscpy_s(latestZipBeforeTarget, fd.cFileName);
				}
			}
		}
	});

	// アーカイブのうちtmToRead以後でもっとも古いログファイルを探す
	unsigned int tmZipped = 0;
	if (latestZipBeforeTarget[0]) {
		_stprintf_s(pattern, TEXT("\\jk%d\\%s"), jkID, latestZipBeforeTarget);
		FIND_LOGFILE_CACHE cache;
		bool bSameResult;
		const char *zippedName = FindZippedLogfile(cache, bSameResult, (logDirectory_ + pattern).c_str(), tmToRead, false);
		if (zippedName) {
			tmZipped = strtoul(zippedName, nullptr, 10);
		}
	}
	if (tmZipped == 0 && oldestZipAfterTarget[0]) {
		tmZipped = _tcstoul(oldestZipAfterTarget, nullptr, 10);
	}

	unsigned int tm = tmZipped;
	if (oldestAfterTarget[0]) {
		tm = _tcstoul(oldestAfterTarget, nullptr, 10);
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
	if (_tcsicmp(zipPath, cache.path.c_str())) {
		cache.path = zipPath;
		cache.list.clear();
		zlib_filefunc64_def def;
		fill_fopen64_filefunc(&def);
		def.zopen64_file = CTextFileReader::TfopenSFileFuncForZlib;
		unzFile f = unzOpen2_64(zipPath, &def);
		if (f) {
			if (unzGoToFirstFile(f) == UNZ_OK) {
				do {
					char name[16] = {};
					if (unzGetCurrentFileInfo64(f, nullptr, name, 15, nullptr, 0, nullptr, 0) == UNZ_OK &&
					    strlen(name) == 14 &&
					    !strchr(name, '/') &&
					    !unzStringFileNameCompare(name + 10, ".txt", 0)) {
						cache.list.resize(cache.list.size() + 1);
						strcpy_s(cache.list.back().name, name);
					}
				} while (unzGoToNextFile(f) == UNZ_OK);
			}
			unzClose(f);
		}
		cache.index = cache.list.size();
	}

	// tmより前/以後でもっとも新しい/古いログファイルを探す
	char target[16];
	sprintf_s(target, "%010u.", tm);
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
