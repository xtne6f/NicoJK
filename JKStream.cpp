#include "stdafx.h"
#include "JKStream.h"

CJKStream::CJKStream()
	: bWorkerCreated_(false)
{
}

CJKStream::~CJKStream()
{
	Close();
}

void CJKStream::BeginClose()
{
	if (bWorkerCreated_) {
		{
			lock_recursive_mutex lock(workerLock_);
			bStopWroker_ = true;
		}
		SetEvent(hWorkerEvent_);
	}
}

void CJKStream::Close()
{
	if (bWorkerCreated_) {
		BeginClose();
		if (WaitForSingleObject(hProcess_, 10000) == WAIT_TIMEOUT) {
			TerminateProcess(hProcess_, 1);
		}
		HANDLE hWorkerThread = workerThread_.native_handle();
		if (WaitForSingleObject(hWorkerThread, 10000) == WAIT_TIMEOUT) {
			workerThread_.detach();
			TerminateThread(hWorkerThread, 1);
			CloseHandle(hWorkerThread);
		} else {
			workerThread_.join();
		}
		CloseHandle(hProcess_);
		CloseHandle(hWorkerEvent_);
		bWorkerCreated_ = false;
	}
}

bool CJKStream::CreateWorker(HWND hwnd, UINT msg)
{
	if (bWorkerCreated_) {
		return true;
	}
	hWorkerEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (hWorkerEvent_) {
		workerThread_ = std::thread([this, hwnd, msg]() { WorkerThread(hwnd, msg); });
		// 初期化を待つ
		WaitForSingleObject(hWorkerEvent_, INFINITE);
		if (bWorkerCreated_) {
			lock_recursive_mutex lock(workerLock_);
			bContinueWorker_ = true;
			return true;
		}
		workerThread_.join();
		CloseHandle(hWorkerEvent_);
	}
	return false;
}

bool CJKStream::CreateJKProcess(HANDLE &hProcess, HANDLE &hAsyncReadPipe, HANDLE &hWritePipe)
{
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = nullptr;
	sa.bInheritHandle = TRUE;
	HANDLE hStdInput;
	if (CreatePipe(&hStdInput, &hWritePipe, &sa, 0)) {
		// 標準出力は非同期にする
		TCHAR pipeName[64];
		_stprintf_s(pipeName, TEXT("\\\\.\\pipe\\anon_%08x_%08x"), GetCurrentProcessId(), GetCurrentThreadId());
		HANDLE hStdOutput = CreateNamedPipe(pipeName, PIPE_ACCESS_OUTBOUND, 0, 1, 8192, 8192, 0, &sa);
		if (hStdOutput != INVALID_HANDLE_VALUE) {
			hAsyncReadPipe = CreateFile(pipeName, GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
			if (hAsyncReadPipe != INVALID_HANDLE_VALUE) {
				TCHAR jkcnslPath[MAX_PATH + 16];
				if (GetLongModuleFileName(nullptr, jkcnslPath, MAX_PATH)) {
					for (size_t i = _tcslen(jkcnslPath); i > 0 && !_tcschr(TEXT("/\\"), jkcnslPath[i - 1]); ) {
						jkcnslPath[--i] = TEXT('\0');
					}
					_tcscat_s(jkcnslPath, TEXT("jkcnsl.exe"));
				}
				// 不正終了時に自力で落ちてもらうためにプロセスIDを渡す
				TCHAR args[32];
				_stprintf_s(args, TEXT(" -p %u"), GetCurrentProcessId());
				STARTUPINFO si = {};
				si.cb = sizeof(si);
				si.dwFlags = STARTF_USESTDHANDLES;
				si.hStdInput = hStdInput;
				si.hStdOutput = hStdOutput;
				// 標準エラー出力は捨てる
				si.hStdError = CreateFile(TEXT("nul"), GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				PROCESS_INFORMATION pi;
				if (CreateProcess(jkcnslPath, args, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
					if (si.hStdError != INVALID_HANDLE_VALUE) {
						CloseHandle(si.hStdError);
					}
					CloseHandle(hStdOutput);
					CloseHandle(hStdInput);
					CloseHandle(pi.hThread);
					hProcess = pi.hProcess;
					return true;
				}
				if (si.hStdError != INVALID_HANDLE_VALUE) {
					CloseHandle(si.hStdError);
				}
				CloseHandle(hAsyncReadPipe);
			}
			CloseHandle(hStdOutput);
		}
		CloseHandle(hWritePipe);
		CloseHandle(hStdInput);
	}
	return false;
}

void CJKStream::WorkerThread(HWND hwnd, UINT msg)
{
	HANDLE olEvents[] = {hWorkerEvent_, CreateEvent(nullptr, TRUE, TRUE, nullptr)};
	if (!olEvents[1]) {
		// 親スレッドに初期化失敗を通知
		SetEvent(hWorkerEvent_);
		return;
	}
	HANDLE hReadPipe, hWritePipe;
	if (!CreateJKProcess(hProcess_, hReadPipe, hWritePipe)) {
		// 親スレッドに初期化失敗を通知
		SetEvent(hWorkerEvent_);
		CloseHandle(olEvents[1]);
		return;
	}
	bWorkerCreated_ = true;
	bContinueWorker_ = false;
	bStopWroker_ = false;
	bOpened_ = false;
	SetEvent(hWorkerEvent_);
	// 親スレッドからの続行許可を待つ
	for (;;) {
		Sleep(0);
		lock_recursive_mutex lock(workerLock_);
		if (bContinueWorker_) {
			break;
		}
	}

	OVERLAPPED ol = {};
	char olBuf[8192];
	for (;;) {
		DWORD dwRet = WaitForMultipleObjects(2, olEvents, FALSE, INFINITE);
		if (dwRet == WAIT_OBJECT_0 + 1) {
			bool bPost = false;
			if (ol.hEvent) {
				// 非同期読み込み完了
				DWORD xferred;
				if (GetOverlappedResult(hReadPipe, &ol, &xferred, FALSE)) {
					lock_recursive_mutex lock(workerLock_);
					if (bOpened_) {
						recvBuf_.insert(recvBuf_.end(), olBuf, olBuf + xferred);
						bPost = true;
					}
				}
			}
			ol.hEvent = olEvents[1];
			while (ReadFile(hReadPipe, olBuf, sizeof(olBuf), nullptr, &ol)) {
				DWORD xferred;
				if (GetOverlappedResult(hReadPipe, &ol, &xferred, FALSE)) {
					lock_recursive_mutex lock(workerLock_);
					if (bOpened_) {
						recvBuf_.insert(recvBuf_.end(), olBuf, olBuf + xferred);
						bPost = true;
					}
				}
			}
			if (GetLastError() != ERROR_IO_PENDING) {
				// エラーor閉じられた
				ol.hEvent = nullptr;
				break;
			}
			// 非同期読み込み開始
			if (bPost) {
				// ウィンドウに受信を通知
				PostMessage(hwnd, msg, 0, 0);
			}
		} else {
			lock_recursive_mutex lock(workerLock_);
			if (bStopWroker_) {
				DWORD dwWritten;
				WriteFile(hWritePipe, "q\r\n", 3, &dwWritten, nullptr);
				break;
			}
			if (bOpened_) {
				if (bShutdown_ && !bShutdownSent_) {
					DWORD dwWritten;
					if (!WriteFile(hWritePipe, "c\r\n", 3, &dwWritten, nullptr) || dwWritten != 3) {
						// エラーor閉じられた
						break;
					}
					bShutdownSent_ = true;
				}
				if (sendBuf_.empty() == false && !bShutdownSent_) {
					DWORD dwWritten;
					if (!WriteFile(hWritePipe, sendBuf_.data(), static_cast<DWORD>(sendBuf_.size()), &dwWritten, nullptr) ||
					    dwWritten != sendBuf_.size()) {
						// エラーor閉じられた
						break;
					}
					sendBuf_.clear();
				}
			}
		}
	}

	if (ol.hEvent) {
		CancelIo(hReadPipe);
	}
	CloseHandle(hWritePipe);
	CloseHandle(hReadPipe);
	CloseHandle(olEvents[1]);
}

// 非同期通信を開始する
// すでに開始しているときは失敗するが、command=='+'のときは開いているストリームに送信データを追加する
bool CJKStream::Send(HWND hwnd, UINT msg, char command, const char *buf)
{
	if (!strchr(buf, '\n') && !strchr(buf, '\r')) {
		if (command == '+' && bWorkerCreated_ && bOpened_ && !bShutdown_) {
			// 前のデータを送信済みのときだけ送信データを追加できる
			lock_recursive_mutex lock(workerLock_);
			if (sendBuf_.empty()) {
				sendBuf_.push_back(command);
				sendBuf_.insert(sendBuf_.end(), buf, buf + strlen(buf));
				sendBuf_.push_back('\r');
				sendBuf_.push_back('\n');
				SetEvent(hWorkerEvent_);
				return true;
			}
		} else if (command != '+' && (!bWorkerCreated_ || !bOpened_)) {
			if (CreateWorker(hwnd, msg)) {
				// ストリームを開く
				lock_recursive_mutex lock(workerLock_);
				recvBuf_.clear();
				sendBuf_.clear();
				sendBuf_.push_back(command);
				sendBuf_.insert(sendBuf_.end(), buf, buf + strlen(buf));
				sendBuf_.push_back('\r');
				sendBuf_.push_back('\n');
				bShutdown_ = false;
				bShutdownSent_ = false;
				bOpened_ = true;
				SetEvent(hWorkerEvent_);
				return true;
			}
		}
	}
	return false;
}

// ウィンドウメッセージを処理してデータを受信する
// 受信データはrecvBufに追記される
// 戻り値: 負値=切断した(-2=正常,-1=中断), 0=正常に処理した
int CJKStream::ProcessRecv(std::vector<char> &recvBuf)
{
	if (bWorkerCreated_ && bOpened_) {
		lock_recursive_mutex lock(workerLock_);
		std::vector<char>::iterator it = recvBuf_.begin();
		for (;;) {
			// LF単位で受信
			std::vector<char>::iterator itEnd = std::find(it, recvBuf_.end(), '\n');
			if (itEnd == recvBuf_.end()) {
				break;
			}
			++itEnd;
			char c = *it;
			if (c == '.' || c == '!' || c == '?') {
				// プロセス側がストリームを閉じた
				recvBuf_.erase(recvBuf_.begin(), itEnd);
				bOpened_ = false;
				return bShutdown_ || c != '.' ? -1 : -2;
			}
			if (c == '-') {
				// 受信
				for (++it; it != itEnd; ++it) {
					// CRは無視
					if (*it != '\r') {
						recvBuf.push_back(*it);
					}
				}
			}
			it = itEnd;
		}
		recvBuf_.erase(recvBuf_.begin(), it);
		if (WaitForSingleObject(workerThread_.native_handle(), 0) != WAIT_TIMEOUT) {
			bOpened_ = false;
			return -1;
		}
	}
	return 0;
}

// 送受信停止を要求する
// 呼び出し後ProcessRecv()が負値を返すと完了(ストリームも閉じられる)
bool CJKStream::Shutdown()
{
	if (bWorkerCreated_ && bOpened_) {
		if (!bShutdown_) {
			lock_recursive_mutex lock(workerLock_);
			bShutdown_ = true;
			SetEvent(hWorkerEvent_);
		}
		return true;
	}
	return false;
}
