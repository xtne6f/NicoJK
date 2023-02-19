#pragma once

#include <mutex>

typedef std::recursive_mutex recursive_mutex_;
typedef std::lock_guard<recursive_mutex_> lock_recursive_mutex;

#define FILETIME_MILLISECOND 10000LL

std::vector<TCHAR> GetPrivateProfileSectionBuffer(LPCTSTR lpAppName, LPCTSTR lpFileName);
void GetBufferedProfileString(LPCTSTR lpBuff, LPCTSTR lpKeyName, LPCTSTR lpDefault, LPTSTR lpReturnedString, DWORD nSize);
tstring GetBufferedProfileToString(LPCTSTR lpBuff, LPCTSTR lpKeyName, LPCTSTR lpDefault);
int GetBufferedProfileInt(LPCTSTR lpBuff, LPCTSTR lpKeyName, int nDefault);
BOOL WritePrivateProfileInt(LPCTSTR lpAppName, LPCTSTR lpKeyName, int value, LPCTSTR lpFileName);
DWORD GetLongModuleFileName(HMODULE hModule, LPTSTR lpFileName, DWORD nSize);
bool HasToken(const char *str, const char *substr);
void DecodeEntityReference(TCHAR *str);
COLORREF GetColor(const char *command);
LONGLONG UnixTimeToFileTime(unsigned int tm);
unsigned int FileTimeToUnixTime(LONGLONG ll);
LONGLONG AribToFileTime(const BYTE *pData);
BOOL FileOpenDialog(HWND hwndOwner, LPCTSTR lpstrFilter, LPTSTR lpstrFile, DWORD nMaxFile);
bool GetProcessOutput(LPCTSTR commandLine, LPCTSTR currentDir, char *buf, size_t bufSize, int timeout = INT_MAX);
std::string UnprotectDpapiToString(const char *src);
std::string UnprotectV10ToString(const char *src, const char *v10Key, char *buf, size_t bufSize);
std::string GetCookieString(LPCTSTR execGetCookie, LPCTSTR execGetV10Key, char *buf, size_t bufSize, int timeout);

inline FILETIME LongLongToFileTime(LONGLONG ll)
{
	FILETIME ft;
	ft.dwLowDateTime = static_cast<DWORD>(ll);
	ft.dwHighDateTime = static_cast<DWORD>(ll >> 32);
	return ft;
}

inline LONGLONG FileTimeToLongLong(FILETIME ft)
{
	return ft.dwLowDateTime | static_cast<LONGLONG>(ft.dwHighDateTime) << 32;
}

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
