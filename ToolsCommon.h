#pragma once

#ifdef _WIN32
#define NOMINMAX
#endif
#include "Common.h"
#ifndef _WIN32
#include <dirent.h>
#endif

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
