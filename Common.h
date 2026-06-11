#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tchar.h>
#define my_fseek _fseeki64
#define my_ftell _ftelli64
#define ComparePath _tcsicmp
#else
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
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
#ifndef NICOJK_LOG_DIR
#define NICOJK_LOG_DIR "/var/local/nicojk"
#endif
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <mutex>
#include <string>

typedef std::lock_guard<std::recursive_mutex> lock_recursive_mutex;

typedef std::basic_string<TCHAR> tstring;

struct fclose_deleter {
	void operator()(FILE *fp) { fclose(fp); }
};

#include <stdexcept>

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <chrono>
#include <thread>

inline void Sleep(DWORD msec)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(msec));
}
#endif

class CAutoResetEvent
{
public:
#ifdef _WIN32
	CAutoResetEvent(bool initialState = false) {
		m_h = CreateEvent(nullptr, FALSE, initialState, nullptr);
		if (!m_h) throw std::runtime_error("");
	}
	~CAutoResetEvent() { CloseHandle(m_h); }
	void Set() { SetEvent(m_h); }
	void Reset() { ResetEvent(m_h); }
	HANDLE Handle() { return m_h; }
	bool WaitOne(DWORD timeout = 0xFFFFFFFF) { return WaitForSingleObject(m_h, timeout) == WAIT_OBJECT_0; }
#else
	CAutoResetEvent(bool initialState = false) {
		m_efd = eventfd(initialState, EFD_CLOEXEC | EFD_NONBLOCK);
		if (m_efd == -1) throw std::runtime_error("");
	}
	~CAutoResetEvent() { close(m_efd); }
	void Set() {
		LONGLONG n = 1;
		while (write(m_efd, &n, sizeof(n)) < 0) {
			if (errno != EAGAIN) throw std::runtime_error("");
		}
	}
	void Reset() { WaitOne(0); }
	int Handle() { return m_efd; }
	bool WaitOne(DWORD timeout = 0xFFFFFFFF) {
		LONGLONG n;
		while (read(m_efd, &n, sizeof(n)) < 0) {
			if (errno != EAGAIN) throw std::runtime_error("");
			if (!timeout) return false;
			pollfd pfd;
			pfd.fd = m_efd;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, timeout < 0x80000000 ? (int)timeout : -1) < 0 && errno != EINTR) {
				throw std::runtime_error("");
			}
			if (timeout < 0x80000000) {
				// シグナル発生時や競合時はtimeoutよりも早くタイムアウトするので注意
				timeout = 0;
			}
		}
		return true;
	}
#endif
private:
	CAutoResetEvent(const CAutoResetEvent&);
	CAutoResetEvent& operator=(const CAutoResetEvent&);
#ifdef _WIN32
	HANDLE m_h;
#else
	int m_efd;
#endif
};
