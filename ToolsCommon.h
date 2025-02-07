#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tchar.h>
#define my_fseek _fseeki64
#define my_ftell _ftelli64
#define ComparePath _tcsicmp
#else
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <dirent.h>
#include <limits.h>
#define my_fseek fseeko
#define my_ftell ftello
#define ComparePath strcmp
#define _tcscmp strcmp
#define _tcslen strlen
#define _tcstod strtod
#define _tcstol strtol
#define _tcstoul strtoul
#define TEXT(quote) quote
#if INT_MAX != 0x7FFFFFFF || LLONG_MAX != 0x7FFFFFFFFFFFFFFF
#error Fundamental types have incompatible sizes.
#endif
typedef char TCHAR;
typedef const char* LPCTSTR;
typedef unsigned int DWORD;
typedef long long LONGLONG;
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string>

typedef std::basic_string<TCHAR> tstring;

struct fclose_deleter {
	void operator()(FILE *fp) { fclose(fp); }
};

struct LOGFILE_NAME {
	TCHAR name[16];
};

template<class P>
void EnumLogFile(LPCTSTR dirPath, P enumProc)
{
#ifdef _WIN32
	WIN32_FIND_DATA findData;
	HANDLE hFind = FindFirstFile((tstring(dirPath) + TEXT("\\??????????.???")).c_str(), &findData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && _tcslen(findData.cFileName) == 14 &&
			    (!ComparePath(findData.cFileName + 10, TEXT(".txt")) ||
			     !ComparePath(findData.cFileName + 10, TEXT(".zip")))) {
				LOGFILE_NAME n;
				_tcscpy_s(n.name, findData.cFileName);
				enumProc(n);
			}
		} while (FindNextFile(hFind, &findData));
		FindClose(hFind);
	}
#else
	DIR *dir = opendir(dirPath);
	if (dir) {
		dirent *ent;
		while (!!(ent = readdir(dir))) {
			if (ent->d_type != DT_DIR && strlen(ent->d_name) == 14 &&
			    (!ComparePath(ent->d_name + 10, ".txt") ||
			     !ComparePath(ent->d_name + 10, ".zip"))) {
				LOGFILE_NAME n;
				strcpy(n.name, ent->d_name);
				enumProc(n);
			}
		}
		closedir(dir);
	}
#endif
}
