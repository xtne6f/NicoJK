#include "../ToolsCommon.h"
#ifdef _WIN32
#include <share.h>
#else
#include <sys/stat.h>
#ifndef NICOJK_LOG_DIR
#define NICOJK_LOG_DIR "/var/local/nicojk"
#endif
#endif
#include <limits.h>
#include <time.h>
#include <algorithm>
#include <memory>
#include <regex>
#include "../ImportLogUtil.h"

namespace
{
// 最終行のchatタグのdate属性値を読み込む
unsigned int ReadLastChatDate(LPCTSTR path)
{
#ifdef _WIN32
	std::unique_ptr<FILE, fclose_deleter> fp(_tfsopen(path, TEXT("rb"), _SH_DENYNO));
#else
	std::unique_ptr<FILE, fclose_deleter> fp(fopen(path, "r"));
#endif
	if (!fp) {
		return 0;
	}
	setvbuf(fp.get(), nullptr, _IONBF, 0);

	char text[8192];
	// バイナリモードでのSEEK_ENDは厳密には議論あるが、Windowsでは問題ない
	if (my_fseek(fp.get(), 0, SEEK_END) != 0 ||
	    my_fseek(fp.get(), -std::min<LONGLONG>(sizeof(text) - 1, my_ftell(fp.get())), SEEK_END) != 0) {
		return 0;
	}
	size_t readLen = fread(text, 1, sizeof(text) - 1, fp.get());
	text[readLen] = '\0';
	size_t textPos = strlen(text);
	if (textPos >= 1 && text[textPos - 1] == '\n') {
		text[--textPos] = '\0';
	}
	if (textPos >= 1 && text[textPos - 1] == '\r') {
		text[--textPos] = '\0';
	}
	for (; textPos > 0; --textPos) {
		if (text[textPos - 1] == '\n') {
			break;
		}
	}

	const std::regex re("^<chat(?= )[^>]*? date=\"(\\d+)\"");
	std::cmatch m;
	if (std::regex_search(text + textPos, m, re)) {
		return strtoul(m[1].first, nullptr, 10);
	}
	return 0;
}

void FormatUnixTimeAsLocalTime(char (&fmtTime)[80], unsigned int tmUnix)
{
	time_t t = tmUnix;
#ifdef _WIN32
	tm tmLocal;
	tm *ptm = &tmLocal;
	if (localtime_s(ptm, &t) == 0) {
		sprintf_s(
#else
	tm *ptm = localtime(&t);
	if (ptm) {
		sprintf(
#endif
			fmtTime, "%04d-%02d-%02dT%02d:%02d:%02d",
			ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
			ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
	}
}
}

#ifdef _WIN32
int _tmain(int argc, TCHAR **argv)
#else
int main(int argc, char **argv)
#endif
{
	if (argc != 2) {
		printf("Usage: jkimlog src_path.\n");
		return 2;
	}
	LPCTSTR srcPath = argv[1];

	// 入力ファイル名からjkIDを抽出する
	unsigned int jkID = 0;
	for (size_t i = _tcslen(srcPath); i > 0 && srcPath[i - 1] != TEXT('/')
#ifdef _WIN32
		&& srcPath[i - 1] != TEXT('\\')
#endif
		; ) {
		--i;
		if ((srcPath[i] == TEXT('J') || srcPath[i] == TEXT('j')) &&
		    (srcPath[i + 1] == TEXT('K') || srcPath[i + 1] == TEXT('k'))) {
			jkID = _tcstoul(srcPath + i + 2, nullptr, 10);
			if (jkID != 0) {
				break;
			}
		}
	}
	if (jkID == 0) {
		printf("Error: Cannot determine jkID from src_path.\n");
		return 1;
	}

#ifdef _WIN32
	TCHAR destRoot[MAX_PATH];
	DWORD nRet = GetModuleFileName(nullptr, destRoot, _countof(destRoot));
	if (nRet && nRet < _countof(destRoot)) {
		size_t i = _tcslen(destRoot);
		while (i > 0 && !_tcschr(TEXT("/\\"), destRoot[i - 1])) {
			destRoot[--i] = TEXT('\0');
		}
		if (i > 0) {
			destRoot[--i] = TEXT('\0');
		}
	} else {
		destRoot[0] = TEXT('\0');
	}
	if (!destRoot[0]) {
		printf("Error: Unexpected.\n");
		return 1;
	}
#else
	const char *destRoot = NICOJK_LOG_DIR;
#endif

	std::unique_ptr<FILE, fclose_deleter> fpDest;
	bool bFirst = true;
	bool bCreated = false;
	bool bTrimmed = false;
	unsigned int tmLast = 0;
	// これ以上のchatタグから書き込む(tmMax==0なら書き込まない)
	unsigned int tmMax = 0;
	// これ未満のchatタグまで書き込む
	unsigned int tmMin = 0;

	TCHAR jkDir[16];
#ifdef _WIN32
	_stprintf_s(jkDir, TEXT("\\jk%d"), jkID);
#else
	sprintf(jkDir, "/jk%d", jkID);
#endif
	ImportLogfile(srcPath, [&, jkID](unsigned int &tm) -> FILE * {
		if (bFirst) {
			// jkIDのログファイル一覧を得る
			// tm以前で最大と、tmより後で最小のログファイルを探す
			TCHAR target[16];
#ifdef _WIN32
			_stprintf_s(
#else
			sprintf(
#endif
				target, TEXT("%010u.txt"), tm);
			LOGFILE_NAME maxTxt = {};
			LOGFILE_NAME minTxt = {};
			EnumLogFile((tstring(destRoot) + jkDir).c_str(), [&](const LOGFILE_NAME &n) {
				if (!ComparePath(n.name + 10, TEXT(".txt"))) {
					// テキスト形式のログ
					if (_tcscmp(n.name, target) <= 0) {
						if (_tcscmp(n.name, maxTxt.name) > 0) {
							maxTxt = n;
						}
					} else {
						if (!minTxt.name[0] || _tcscmp(n.name, minTxt.name) < 0) {
							minTxt = n;
						}
					}
				}
			});

			tmMax = _tcstoul(maxTxt.name, nullptr, 10);
			if (tmMax == 0 || tmMax > tm) {
				// 先頭から書き込む
				tmMax = tm;
			} else {
				tmMax = ReadLastChatDate((tstring(destRoot) + jkDir + jkDir[0] + maxTxt.name).c_str());
				if (tmMax != 0) {
					++tmMax;
				}
			}
			tmMin = _tcstoul(minTxt.name, nullptr, 10);
			if (tmMin == 0) {
				// 末尾まで書き込む
				tmMin = UINT_MAX;
			}
		}

		if (tmMax != 0 && tm >= tmMax && tm < tmMin) {
			if (!bCreated) {
				// jkフォルダがなければ作る
				tstring path = tstring(destRoot) + jkDir;
#ifdef _WIN32
				if (CreateDirectory(path.c_str(), nullptr)) {
#else
				if (mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) == 0) {
#endif
					printf("Created jk%d\n", jkID);
				}
				printf("Creating jk%d/%010u.txt\n", jkID, tm);
				TCHAR target[16];
#ifdef _WIN32
				_stprintf_s(
#else
				sprintf(
#endif
					target, TEXT("%010u.txt"), tm);
				path += jkDir[0];
				path += target;
				FILE *fp;
#ifdef _WIN32
				if (!_tfopen_s(&fp, path.c_str(), TEXT("wbx"))) {
#else
				if (!!(fp = fopen(path.c_str(), "wx"))) {
#endif
					fpDest.reset(fp);

					char fmtTime[80] = "0000-00-00T00:00:00";
					FormatUnixTimeAsLocalTime(fmtTime, tm);
					fprintf(fpDest.get(), "<!-- jkimlog imported logfile from %s -->\r\n", fmtTime);
					printf("Sta date=\"%u\" [%s]%s\n", tm, fmtTime, bFirst ? "" : " (trimmed)");
				}
				bCreated = true;
			}
		} else {
			if (fpDest) {
				fpDest.reset();
				bTrimmed = true;
			}
		}
		bFirst = false;

		if (fpDest) {
			tmLast = tm;
		}
		return fpDest.get();
	});

	if (!bCreated) {
		printf("Warning: Nothing to write, or duplicated log. Ignored.\n");
		return 3;
	}
	if (!fpDest && !bTrimmed) {
		printf("Error: Cannot write to destination file.\n");
		return 1;
	}
	fpDest.reset();

	char fmtTime[80] = "0000-00-00T00:00:00";
	FormatUnixTimeAsLocalTime(fmtTime, tmLast);
	printf("End date=\"%u\" [%s]%s\n", tmLast, fmtTime, bTrimmed ? " (trimmed)" : "");
	printf("Done.\n");
	return 0;
}
