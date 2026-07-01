#include "Common.h"
#include "JKStream.h"
#include <algorithm>
#ifdef _WIN32
#include "Util.h"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#endif

CJKStream::CJKStream()
{
}

CJKStream::~CJKStream()
{
	Close();
}

void CJKStream::BeginClose()
{
	if (workerThread_.joinable()) {
		{
			lock_recursive_mutex lock(workerLock_);
			bStopWorker_ = true;
		}
		workerEvent_.Set();
	}
}

void CJKStream::Close()
{
	if (workerThread_.joinable()) {
		BeginClose();
#ifdef _WIN32
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
#else
		for (int i = 0; waitpid(processID_, nullptr, WNOHANG) == 0; i++) {
			if (i > 1000) {
				kill(processID_, 9);
				waitpid(processID_, nullptr, 0);
				break;
			}
			Sleep(10);
		}
		workerThread_.join();
#endif
	}
}

bool CJKStream::CreateWorker()
{
	if (workerThread_.joinable()) {
		return true;
	}
	bContinueWorker_ = true;
	workerEvent_.Reset();
	workerThread_ = std::thread([this]() { WorkerThread(); });
	// 初期化を待つ
	workerEvent_.WaitOne();
	if (!bContinueWorker_) {
		lock_recursive_mutex lock(workerLock_);
		bContinueWorker_ = true;
		return true;
	}
	workerThread_.join();
	return false;
}

#ifdef _WIN32
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
#else
bool CJKStream::CreateJKProcess(int &processID, int &asyncReadPipe, int &writePipe)
{
	int upfd[2], dnfd[2];
	if (pipe2(upfd, O_CLOEXEC) == 0) {
		if (pipe2(dnfd, O_CLOEXEC) == 0) {
			// 標準出力は非同期にする
			if (fcntl(dnfd[0], F_SETFL, O_NONBLOCK) != -1) {
				char parentPid[16];
				sprintf(parentPid, "%d", static_cast<int>(getpid()));
				processID = static_cast<int>(fork());
				if (processID == 0) {
					bool failed = false;
					close(upfd[1]);
					if (upfd[0] != STDIN_FILENO) {
						failed = dup2(upfd[0], STDIN_FILENO) == -1;
						close(upfd[0]);
					}
					close(dnfd[0]);
					if (dnfd[1] != STDOUT_FILENO) {
						failed = dup2(dnfd[1], STDOUT_FILENO) == -1 || failed;
						close(dnfd[1]);
					}
					if (!failed) {
						// シグナルマスクを初期化
						sigset_t sset;
						sigemptyset(&sset);
						if (pthread_sigmask(SIG_SETMASK, &sset, nullptr) == 0) {
							// 不正終了時に自力で落ちてもらうためにプロセスIDを渡す
							execlp("jkcnsl", "jkcnsl", "-p", parentPid,
#ifdef JKCNSL_UNIX_BASE_DIR
							       "-d", JKCNSL_UNIX_BASE_DIR,
#endif
							       nullptr);
						}
					}
					exit(EXIT_FAILURE);
				}
				if (processID != -1) {
					asyncReadPipe = dnfd[0];
					writePipe = upfd[1];
					close(dnfd[1]);
					close(upfd[0]);
					return true;
				}
			}
			close(dnfd[1]);
			close(dnfd[0]);
		}
		close(upfd[1]);
		close(upfd[0]);
	}
	return false;
}
#endif

void CJKStream::WorkerThread()
{
#ifdef _WIN32
	HANDLE olEvents[] = {workerEvent_.Handle(), CreateEvent(nullptr, TRUE, TRUE, nullptr)};
	if (!olEvents[1]) {
		// 親スレッドに初期化失敗を通知
		workerEvent_.Set();
		return;
	}
	HANDLE hReadPipe, hWritePipe;
	if (!CreateJKProcess(hProcess_, hReadPipe, hWritePipe)) {
		// 親スレッドに初期化失敗を通知
		workerEvent_.Set();
		CloseHandle(olEvents[1]);
		return;
	}
#else
	int readPipe, writePipe;
	if (!CreateJKProcess(processID_, readPipe, writePipe)) {
		// 親スレッドに初期化失敗を通知
		workerEvent_.Set();
		return;
	}
	bWorkerDestroyed_ = false;
#endif

	bContinueWorker_ = false;
	bStopWorker_ = false;
	bOpened_ = false;
	workerEvent_.Set();
	// 親スレッドからの続行許可を待つ
	for (;;) {
		Sleep(0);
		lock_recursive_mutex lock(workerLock_);
		if (bContinueWorker_) {
			break;
		}
	}

#ifdef _WIN32
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
				PostMessage(hwndRecv_, recvMsg_, 0, 0);
			}
		} else {
			lock_recursive_mutex lock(workerLock_);
			if (bStopWorker_) {
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
#else
	char buf[8192];
	for (;;) {
		pollfd pfds[2];
		pfds[0].fd = readPipe;
		pfds[0].events = POLLIN;
		pfds[1].fd = workerEvent_.Handle();
		pfds[1].events = POLLIN;
		if (poll(pfds, 2, -1) < 0 && errno != EINTR) {
			// エラー
			break;
		}
		ssize_t n = read(readPipe, buf, sizeof(buf));
		if (n == 0 || (n < 0 && errno != EAGAIN)) {
			break;
		}
		if (n > 0) {
			lock_recursive_mutex lock(workerLock_);
			if (bOpened_) {
				recvBuf_.insert(recvBuf_.end(), buf, buf + n);
				// 受信を通知
				recvEvent_->Set();
			}
		}
		if (workerEvent_.WaitOne(0)) {
			lock_recursive_mutex lock(workerLock_);
			if (bStopWorker_) {
				n = write(writePipe, "q\n", 2);
				static_cast<void>(n);
				break;
			}
			if (bOpened_) {
				if (bShutdown_ && !bShutdownSent_) {
					if (write(writePipe, "c\n", 2) != 2) {
						// エラーor閉じられた
						break;
					}
					bShutdownSent_ = true;
				}
				if (sendBuf_.empty() == false && !bShutdownSent_) {
					if (write(writePipe, sendBuf_.data(), sendBuf_.size()) != static_cast<ssize_t>(sendBuf_.size())) {
						// エラーor閉じられた
						break;
					}
					sendBuf_.clear();
				}
			}
		}
	}
	close(writePipe);
	close(readPipe);
	lock_recursive_mutex lock(workerLock_);
	bWorkerDestroyed_ = true;
#endif
}

// 非同期通信を開始する
// すでに開始しているときは失敗するが、command=='+'のときは開いているストリームに送信データを追加する
#ifdef _WIN32
bool CJKStream::Send(HWND hwnd, UINT msg, char command, const char *buf)
#else
bool CJKStream::Send(CAutoResetEvent *recvEvent, char command, const char *buf)
#endif
{
	if (!strchr(buf, '\n') && !strchr(buf, '\r')) {
		if (command == '+' && workerThread_.joinable() && bOpened_ && !bShutdown_) {
			// 前のデータを送信済みのときだけ送信データを追加できる
			lock_recursive_mutex lock(workerLock_);
			if (sendBuf_.empty()) {
				sendBuf_.push_back(command);
				sendBuf_.insert(sendBuf_.end(), buf, buf + strlen(buf));
#ifdef _WIN32
				sendBuf_.push_back('\r');
#endif
				sendBuf_.push_back('\n');
				workerEvent_.Set();
				return true;
			}
		} else if (command != '+' && (!workerThread_.joinable() || !bOpened_)) {
			if (!workerThread_.joinable()) {
#ifdef _WIN32
				hwndRecv_ = hwnd;
				recvMsg_ = msg;
#else
				recvEvent_ = recvEvent;
#endif
			}
			if (CreateWorker()) {
				// ストリームを開く
				lock_recursive_mutex lock(workerLock_);
				recvBuf_.clear();
				sendBuf_.clear();
				sendBuf_.push_back(command);
				sendBuf_.insert(sendBuf_.end(), buf, buf + strlen(buf));
#ifdef _WIN32
				sendBuf_.push_back('\r');
#endif
				sendBuf_.push_back('\n');
				bShutdown_ = false;
				bShutdownSent_ = false;
				bOpened_ = true;
				workerEvent_.Set();
				return true;
			}
		}
	}
	return false;
}

// データを受信する
// 受信データはrecvBufに追記される
// 戻り値: 負値=切断した(-2=正常,-1=中断), 0=正常に処理した
int CJKStream::ProcessRecv(std::vector<char> &recvBuf)
{
	if (workerThread_.joinable() && bOpened_) {
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
#ifdef _WIN32
		if (WaitForSingleObject(workerThread_.native_handle(), 0) != WAIT_TIMEOUT) {
#else
		if (bWorkerDestroyed_) {
#endif
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
	if (workerThread_.joinable() && bOpened_) {
		if (!bShutdown_) {
			lock_recursive_mutex lock(workerLock_);
			bShutdown_ = true;
			workerEvent_.Set();
		}
		return true;
	}
	return false;
}
