#include "stdafx.h"
#include "JKTransfer.h"

CJKTransfer::CJKTransfer()
	: bWorkerCreated_(false)
{
}

CJKTransfer::~CJKTransfer()
{
	Close();
}

void CJKTransfer::BeginClose()
{
	if (bWorkerCreated_) {
		{
			lock_recursive_mutex lock(workerLock_);
			bStopWroker_ = true;
		}
		SetEvent(hWorkerEvent_);
	}
}

void CJKTransfer::Close()
{
	if (bWorkerCreated_) {
		BeginClose();
		HANDLE hWorkerThread = workerThread_.native_handle();
		if (WaitForSingleObject(hWorkerThread, 10000) == WAIT_TIMEOUT) {
			workerThread_.detach();
			TerminateThread(hWorkerThread, 1);
			CloseHandle(hWorkerThread);
		} else {
			workerThread_.join();
		}
		CloseHandle(hWorkerEvent_);
		bWorkerCreated_ = false;
	}
}

bool CJKTransfer::CreateWorker(HWND hwnd, UINT msg, bool bEnablePost, DWORD processID)
{
	if (bWorkerCreated_) {
		return true;
	}
	hWorkerEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (hWorkerEvent_) {
		workerThread_ = std::thread([this, hwnd, msg, bEnablePost, processID]() { WorkerThread(hwnd, msg, bEnablePost, processID); });
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

void CJKTransfer::WorkerThread(HWND hwnd, UINT msg, bool bEnablePost, DWORD processID)
{
	HANDLE hChatPipe = INVALID_HANDLE_VALUE;
	HANDLE hPostPipe = INVALID_HANDLE_VALUE;
	HANDLE olEvents[] = {hWorkerEvent_, CreateEvent(nullptr, TRUE, TRUE, nullptr), CreateEvent(nullptr, TRUE, TRUE, nullptr)};
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
		SetEvent(hWorkerEvent_);
		if (hPostPipe != INVALID_HANDLE_VALUE) CloseHandle(hPostPipe);
		if (hChatPipe != INVALID_HANDLE_VALUE) CloseHandle(hChatPipe);
		if (olEvents[2]) CloseHandle(olEvents[2]);
		if (olEvents[1]) CloseHandle(olEvents[1]);
		return;
	}
	bWorkerCreated_ = true;
	bContinueWorker_ = false;
	bStopWroker_ = false;
	SetEvent(hWorkerEvent_);
	// 親スレッドからの続行許可を待つ
	for (;;) {
		Sleep(0);
		lock_recursive_mutex lock(workerLock_);
		if (bContinueWorker_) {
			break;
		}
	}

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
								PostMessage(hwnd, msg, 0, 0);
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
			if (bStopWroker_) {
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
}

bool CJKTransfer::Open(HWND hwnd, UINT msg, bool bEnablePost, DWORD processID)
{
	Close();
	currentJKID_ = -1;
	chatBuf_.clear();
	postStr_.clear();
	return CreateWorker(hwnd, msg, bEnablePost, processID);
}

bool CJKTransfer::SendChat(int jkID, const char *text)
{
	if (bWorkerCreated_) {
		lock_recursive_mutex lock(workerLock_);
		currentJKID_ = jkID;
		chatBuf_.insert(chatBuf_.end(), text, text + strlen(text));
		chatBuf_.push_back('\r');
		chatBuf_.push_back('\n');
		SetEvent(hWorkerEvent_);
		return true;
	}
	return false;
}

std::string CJKTransfer::ProcessRecvPost()
{
	std::string ret;
	if (bWorkerCreated_) {
		lock_recursive_mutex lock(workerLock_);
		ret.swap(postStr_);
	}
	return ret;
}
