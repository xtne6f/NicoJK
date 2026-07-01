#include "Common.h"
#include "JKTransfer.h"
#include <algorithm>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#endif

CJKTransfer::CJKTransfer()
{
}

CJKTransfer::~CJKTransfer()
{
	Close();
}

void CJKTransfer::BeginClose()
{
	if (workerThread_.joinable()) {
		{
			lock_recursive_mutex lock(workerLock_);
			bStopWorker_ = true;
		}
		workerEvent_.Set();
	}
}

void CJKTransfer::Close()
{
	if (workerThread_.joinable()) {
		BeginClose();
#ifdef _WIN32
		HANDLE hWorkerThread = workerThread_.native_handle();
		if (WaitForSingleObject(hWorkerThread, 10000) == WAIT_TIMEOUT) {
			workerThread_.detach();
			TerminateThread(hWorkerThread, 1);
			CloseHandle(hWorkerThread);
		} else
#endif
		{
			workerThread_.join();
		}
	}
}

bool CJKTransfer::CreateWorker(bool bEnablePost, int processID)
{
	if (workerThread_.joinable()) {
		return true;
	}
	bContinueWorker_ = true;
	workerEvent_.Reset();
	workerThread_ = std::thread([this, bEnablePost, processID]() { WorkerThread(bEnablePost, processID); });
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

void CJKTransfer::WorkerThread(bool bEnablePost, int processID)
{
#ifdef _WIN32
	HANDLE hChatPipe = INVALID_HANDLE_VALUE;
	HANDLE hPostPipe = INVALID_HANDLE_VALUE;
	HANDLE olEvents[] = {workerEvent_.Handle(), CreateEvent(nullptr, TRUE, TRUE, nullptr), CreateEvent(nullptr, TRUE, TRUE, nullptr)};
	if (olEvents[1] && olEvents[2]) {
		TCHAR pipeName[64];
		_stprintf_s(pipeName, TEXT("\\\\.\\pipe\\chat_d7b64ac2_%d"), processID);
		hChatPipe = CreateNamedPipe(pipeName, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED, 0, 1, CHAT_BUFFER_SIZE, 0, 0, nullptr);
		if (bEnablePost) {
			_stprintf_s(pipeName, TEXT("\\\\.\\pipe\\post_d7b64ac2_%d"), processID);
			hPostPipe = CreateNamedPipe(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, 0, 1, 0, POST_BUFFER_SIZE, 0, nullptr);
		}
	}
	if (hChatPipe == INVALID_HANDLE_VALUE || (bEnablePost && hPostPipe == INVALID_HANDLE_VALUE)) {
		// 親スレッドに初期化失敗を通知
		workerEvent_.Set();
		if (hPostPipe != INVALID_HANDLE_VALUE) CloseHandle(hPostPipe);
		if (hChatPipe != INVALID_HANDLE_VALUE) CloseHandle(hChatPipe);
		if (olEvents[2]) CloseHandle(olEvents[2]);
		if (olEvents[1]) CloseHandle(olEvents[1]);
		return;
	}
#else
	char chatPipeName[sizeof(JKTASK_BASE_DIR) + 32];
	char chatClosingName[sizeof(JKTASK_BASE_DIR) + 32];
	char postPipeName[sizeof(JKTASK_BASE_DIR) + 32];
	sprintf(chatPipeName, "%s/chat_%d.fifo", JKTASK_BASE_DIR, processID);
	sprintf(chatClosingName, "%s/chat_%d.fif_", JKTASK_BASE_DIR, processID);
	sprintf(postPipeName, "%s/post_%d.fifo", JKTASK_BASE_DIR, processID);
	int postPipe = -1;
	if (bEnablePost) {
		if (mkfifo(postPipeName, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) == 0 || errno == EEXIST) {
			// read()が速やかにEOFを返すことで待機できなくなるのを避けるためO_RDWR
			postPipe = open(postPipeName, O_RDWR | O_NONBLOCK | O_CLOEXEC);
			if (postPipe < 0) {
				unlink(postPipeName);
			}
		}
		if (postPipe < 0) {
			// 親スレッドに初期化失敗を通知
			workerEvent_.Set();
			return;
		}
	}
	if (mkfifo(chatPipeName, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) != 0 && errno != EEXIST) {
		if (postPipe >= 0) {
			unlink(postPipeName);
			close(postPipe);
		}
		// 親スレッドに初期化失敗を通知
		workerEvent_.Set();
		return;
	}
#endif

	bContinueWorker_ = false;
	bStopWorker_ = false;
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
	OVERLAPPED olChat = {};
	OVERLAPPED olPost = {};
	bool bConnectChat = false;
	bool bConnectPost = false;
	std::vector<char> olChatBuf;
	std::vector<char> olPostBuf;
	DWORD nextWait = 0;
	DWORD waitTick = GetTickCount();
	for (;;) {
		DWORD dwRet = WaitForMultipleObjects(bEnablePost ? 3 : 2, olEvents, FALSE, nextWait);
		if (dwRet == WAIT_OBJECT_0 + 1) {
			if (olChat.hEvent) {
				if (bConnectChat) {
					// 非同期書き込み完了
					DWORD xferred;
					if (!GetOverlappedResult(hChatPipe, &olChat, &xferred, FALSE) || xferred != olChatBuf.size()) {
						DisconnectNamedPipe(hChatPipe);
						bConnectChat = false;
					}
				} else {
					// 接続した
					bConnectChat = true;
				}
				ResetEvent(olChat.hEvent);
				olChat.hEvent = nullptr;
			}
			if (!bConnectChat) {
				olChat.hEvent = olEvents[1];
				if (ConnectNamedPipe(hChatPipe, &olChat)) {
					SetEvent(olChat.hEvent);
				} else {
					DWORD err = GetLastError();
					if (err == ERROR_PIPE_CONNECTED) {
						SetEvent(olChat.hEvent);
					} else if (err != ERROR_IO_PENDING) {
						// 失敗。以後何もしない
						ResetEvent(olChat.hEvent);
						olChat.hEvent = nullptr;
					}
				}
			}
		} else if (dwRet == WAIT_OBJECT_0 + 2) {
			if (olPost.hEvent) {
				if (bConnectPost) {
					// 非同期読み込み完了
					DWORD xferred;
					if (!GetOverlappedResult(hPostPipe, &olPost, &xferred, FALSE) || xferred == 0) {
						DisconnectNamedPipe(hPostPipe);
						bConnectPost = false;
					} else {
						olPostBuf.resize(olPostBuf.size() - POST_BUFFER_SIZE + xferred);
						auto it = std::find(olPostBuf.begin(), olPostBuf.end(), '\n');
						if (it != olPostBuf.end()) {
							// 改行がきたので切断
							DisconnectNamedPipe(hPostPipe);
							bConnectPost = false;
							olPostBuf.erase(it, olPostBuf.end());
							if (!olPostBuf.empty() && olPostBuf.back() == '\r') {
								olPostBuf.pop_back();
							}
							if (!olPostBuf.empty()) {
								lock_recursive_mutex lock(workerLock_);
								olPostBuf.push_back('\0');
								postStr_ = olPostBuf.data();
								// ウィンドウに通知
								PostMessage(hwndRecvPost_, recvPostMsg_, 0, 0);
							}
						}
					}
				} else {
					// 接続した
					bConnectPost = true;
					olPostBuf.clear();
				}
			}
			olPost.hEvent = olEvents[2];
			if (bConnectPost) {
				olPostBuf.resize(olPostBuf.size() + POST_BUFFER_SIZE);
				if (!ReadFile(hPostPipe, olPostBuf.data() + olPostBuf.size() - POST_BUFFER_SIZE, POST_BUFFER_SIZE, nullptr, &olPost) &&
				    GetLastError() != ERROR_IO_PENDING) {
					// 失敗。後で再接続
					DisconnectNamedPipe(hPostPipe);
					bConnectPost = false;
					SetEvent(olPost.hEvent);
					olPost.hEvent = nullptr;
				}
			} else {
				if (ConnectNamedPipe(hPostPipe, &olPost)) {
					SetEvent(olPost.hEvent);
				} else {
					DWORD err = GetLastError();
					if (err == ERROR_PIPE_CONNECTED) {
						SetEvent(olPost.hEvent);
					} else if (err != ERROR_IO_PENDING) {
						// 失敗。以後何もしない
						ResetEvent(olPost.hEvent);
						olPost.hEvent = nullptr;
					}
				}
			}
		}

		DWORD tick = GetTickCount();
		int jkID;
		{
			lock_recursive_mutex lock(workerLock_);
			if (bStopWorker_) {
				break;
			}
			if (tick - waitTick >= (chatBuf_.empty() ? WRITE_PENDING_MAX : WRITE_PENDING_MIN)) {
				if (!bConnectChat) {
					chatBuf_.clear();
				} else if (!olChat.hEvent) {
					// 80文字(\rを除く)のヘッダをつける
					olChatBuf.assign(76, ' ');
					olChatBuf.push_back('-');
					olChatBuf.push_back('-');
					olChatBuf.push_back('>');
					olChatBuf.push_back('\r');
					olChatBuf.push_back('\n');
					olChatBuf.insert(olChatBuf.end(), chatBuf_.begin(), chatBuf_.end());
					chatBuf_.clear();
				}
				waitTick = tick;
			} else if (!olChat.hEvent) {
				olChatBuf.clear();
			}
			nextWait = (chatBuf_.empty() ? WRITE_PENDING_MAX : WRITE_PENDING_MIN) - (tick - waitTick);
			jkID = currentJKID_;
		}

		if (bConnectChat && !olChat.hEvent && !olChatBuf.empty()) {
			// 接続済みかつ書き込みを開始する必要がある
			int n = static_cast<int>(std::count(olChatBuf.begin(), olChatBuf.end(), '\n')) - 1;
			int i = sprintf_s(olChatBuf.data(), 76, "<!-- J=%d;L=%d;N=%d", jkID, static_cast<int>(olChatBuf.size()) - 81 - n, n);
			olChatBuf[i] = ' ';
			olChat.hEvent = olEvents[1];
			if (!WriteFile(hChatPipe, olChatBuf.data(), static_cast<DWORD>(olChatBuf.size()), nullptr, &olChat) &&
			    GetLastError() != ERROR_IO_PENDING) {
				// 失敗。後で再接続
				DisconnectNamedPipe(hChatPipe);
				bConnectChat = false;
				SetEvent(olChat.hEvent);
				olChat.hEvent = nullptr;
			}
		}
	}

	if (bEnablePost) {
		if (olPost.hEvent) {
			CancelIo(hPostPipe);
			WaitForSingleObject(olPost.hEvent, INFINITE);
		}
		CloseHandle(hPostPipe);
	}
	if (olChat.hEvent) {
		CancelIo(hChatPipe);
		WaitForSingleObject(olChat.hEvent, INFINITE);
	}
	CloseHandle(hChatPipe);
	CloseHandle(olEvents[2]);
	CloseHandle(olEvents[1]);
#else
	int chatPipe = -1;
	std::vector<char> olChatBuf;
	std::vector<char> olPostBuf;
	auto waitTick = std::chrono::steady_clock::now();
	for (;;) {
		if (chatPipe < 0) {
			chatPipe = open(chatPipeName, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		}
		if (chatPipe >= 0 && !olChatBuf.empty()) {
			ssize_t n = write(chatPipe, olChatBuf.data(), olChatBuf.size());
			if (n == 0 || (n < 0 && errno != EAGAIN)) {
				// 切断
				close(chatPipe);
				chatPipe = -1;
				olChatBuf.clear();
			} else if (n > 0) {
				olChatBuf.erase(olChatBuf.begin(), olChatBuf.begin() + n);
			}
		}
		if (postPipe >= 0) {
			olPostBuf.resize(olPostBuf.size() + POST_BUFFER_SIZE);
			ssize_t n = read(postPipe, olPostBuf.data() + olPostBuf.size() - POST_BUFFER_SIZE, POST_BUFFER_SIZE);
			if (n == 0 || (n < 0 && errno != EAGAIN)) {
				// 終了または失敗。原則ここは踏まない
				break;
			}
			if (n > 0) {
				olPostBuf.resize(olPostBuf.size() - POST_BUFFER_SIZE + n);
				for (;;) {
					auto it = std::find(olPostBuf.begin(), olPostBuf.end(), '\n');
					if (it == olPostBuf.end()) {
						break;
					}
					*(it - (it != olPostBuf.begin() && *(it - 1) == '\r')) = '\0';
					if (olPostBuf[0]) {
						lock_recursive_mutex lock(workerLock_);
						postStr_ = olPostBuf.data();
						// 通知
						recvPostEvent_->Set();
					}
					olPostBuf.erase(olPostBuf.begin(), it + 1);
				}
			} else {
				olPostBuf.resize(olPostBuf.size() - POST_BUFFER_SIZE);
			}
		}

		// 待機
		pollfd pfds[3];
		pfds[0].fd = postPipe;
		pfds[0].events = POLLIN;
		pfds[1].fd = workerEvent_.Handle();
		pfds[1].events = POLLIN;
		pfds[2].fd = chatPipe;
		pfds[2].events = POLLOUT;
		if (poll(pfds + (postPipe < 0), 3 - (postPipe < 0) - (chatPipe < 0 || olChatBuf.empty()), WRITE_PENDING_MIN) < 0 && errno != EINTR) {
			// 失敗
			break;
		}
		workerEvent_.Reset();

		auto tick = std::chrono::steady_clock::now();
		{
			lock_recursive_mutex lock(workerLock_);
			if (bStopWorker_) {
				break;
			}
			if (std::chrono::duration_cast<std::chrono::milliseconds>(tick - waitTick).count() >= static_cast<int>(chatBuf_.empty() ? WRITE_PENDING_MAX : WRITE_PENDING_MIN)) {
				if (chatPipe < 0) {
					chatBuf_.clear();
				} else if (olChatBuf.empty()) {
					// 80文字のヘッダをつける
					olChatBuf.assign(76, ' ');
					olChatBuf.push_back('-');
					olChatBuf.push_back('-');
					olChatBuf.push_back('>');
					olChatBuf.push_back('\n');
					olChatBuf.insert(olChatBuf.end(), chatBuf_.begin(), chatBuf_.end());
					int n = static_cast<int>(std::count(olChatBuf.begin(), olChatBuf.end(), '\n')) - 1;
					int i = sprintf(olChatBuf.data(), "<!-- J=%d;L=%d;N=%d", currentJKID_, static_cast<int>(olChatBuf.size()) - 80, n);
					olChatBuf[i] = ' ';
					chatBuf_.clear();
				}
				waitTick = tick;
			}
		}
	}

	if (postPipe >= 0) {
		unlink(postPipeName);
		close(postPipe);
	}
	if (chatPipe < 0) {
		// 確実に切断させるため拡張子の末尾を"_"にリネームして接続できないようにしてから開いてみる
		if( rename(chatPipeName, chatClosingName) == 0 ){
			strcpy(chatPipeName, chatClosingName);
		}
		chatPipe = open(chatPipeName, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	}
	unlink(chatPipeName);
	if (chatPipe >= 0) {
		close(chatPipe);
	}
#endif
}

#ifdef _WIN32
bool CJKTransfer::Open(HWND hwnd, UINT msg, bool bEnablePost, int processID)
#else
bool CJKTransfer::Open(CAutoResetEvent *recvPostEvent, bool bEnablePost, int processID)
#endif
{
	Close();
	currentJKID_ = -1;
	chatBuf_.clear();
	postStr_.clear();
#ifdef _WIN32
	hwndRecvPost_ = hwnd;
	recvPostMsg_ = msg;
#else
	recvPostEvent_ = recvPostEvent;
#endif
	return CreateWorker(bEnablePost, processID);
}

bool CJKTransfer::SendChat(int jkID, const char *text)
{
	if (workerThread_.joinable()) {
		lock_recursive_mutex lock(workerLock_);
		currentJKID_ = jkID;
		chatBuf_.insert(chatBuf_.end(), text, text + strlen(text));
#ifdef _WIN32
		chatBuf_.push_back('\r');
#endif
		chatBuf_.push_back('\n');
		workerEvent_.Set();
		return true;
	}
	return false;
}

std::string CJKTransfer::ProcessRecvPost()
{
	std::string ret;
	if (workerThread_.joinable()) {
		lock_recursive_mutex lock(workerLock_);
		ret.swap(postStr_);
	}
	return ret;
}
