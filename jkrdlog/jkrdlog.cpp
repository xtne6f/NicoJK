#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include "../LogReader.h"
#include "../NetworkServiceIDTable.h"
#include <algorithm>
#include <string>

int _tmain(int argc, TCHAR **argv)
{
	int readRatePerMille = 0;
	int jkID = 0;
	unsigned int tmReadFrom = 0;
	unsigned int tmReadTo = 0;

	if (argc < 4) {
		_ftprintf(stderr, TEXT("Usage: jkrdlog [-r readrate] jkid_or_ns{nid<<16+sid} from_unixtime to_unixtime_or_0.\n"));
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
				const NETWORK_SERVICE_ID_ELEM *p = std::lower_bound(DEFAULT_NTSID_TABLE, DEFAULT_NTSID_TABLE + _countof(DEFAULT_NTSID_TABLE), e,
					[](const NETWORK_SERVICE_ID_ELEM &a, const NETWORK_SERVICE_ID_ELEM &b) { return a.ntsID < b.ntsID; });
				if (p != DEFAULT_NTSID_TABLE + _countof(DEFAULT_NTSID_TABLE) && p->ntsID == e.ntsID) {
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
			_ftprintf(stderr, TEXT("Error: Argument %d is invalid.\n"), i);
			return 1;
		}
	}
	if (jkID <= 0) {
		_ftprintf(stderr, TEXT("Error: Not enough arguments.\n"));
		return 1;
	}

	TCHAR dir[MAX_PATH];
	DWORD nRet = GetModuleFileName(nullptr, dir, _countof(dir));
	if (nRet && nRet < _countof(dir)) {
		for (size_t i = _tcslen(dir); i > 0 && !_tcschr(TEXT("/\\"), dir[i - 1]); ) {
			dir[--i] = TEXT('\0');
		}
		if (dir[0]) {
			dir[_tcslen(dir) - 1] = TEXT('\0');
		}
	} else {
		dir[0] = TEXT('\0');
	}
	if (!dir[0]) {
		_ftprintf(stderr, TEXT("Error: Unexpected.\n"));
		return 1;
	}

	CLogReader logReader;
	logReader.SetLogDirectory(dir);
	logReader.ResetCheckInterval();

	LARGE_INTEGER liFreq = {};
	LARGE_INTEGER liBase = {};
	if (readRatePerMille > 0) {
		// 毎秒速やかに出力するためバッファリングしない
		if (setvbuf(stdout, nullptr, _IONBF, 0) != 0 ||
		    !QueryPerformanceFrequency(&liFreq) ||
		    !QueryPerformanceCounter(&liBase)) {
			_ftprintf(stderr, TEXT("Error: Unexpected.\n"));
			return 1;
		}
		liFreq.QuadPart *= 1000;
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
				char utf8[512];
				if (WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, sizeof(utf8), nullptr, nullptr) != 0) {
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
		int i = sprintf_s(head, "<!-- J=%d;T=%u;L=%d;N=%d", jkID, tm, static_cast<int>(buf.size()) - 80,
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
				i = sprintf_s(head, "<!-- J=%d;T=%u;L=0;N=0", jkID, tm);
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
				LARGE_INTEGER liNow;
				if (!QueryPerformanceCounter(&liNow)) {
					_ftprintf(stderr, TEXT("Error: Unexpected.\n"));
					return 1;
				}
				if ((tm - tmReadFrom) - (liNow.QuadPart - liBase.QuadPart) * readRatePerMille / liFreq.QuadPart < 0) {
					break;
				}
				Sleep(100);
			}
		}
	}
	fflush(stdout);
	return 0;
}
