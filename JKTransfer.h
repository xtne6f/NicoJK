#pragma once
#include "Util.h"
#include <thread>

// プロセス間でコメントと投稿を送受信する
class CJKTransfer
{
public:
	static const DWORD CHAT_BUFFER_SIZE = 8192;
	static const DWORD POST_BUFFER_SIZE = 1024;
	// コメントがあるときの書き込み間隔
	static const DWORD WRITE_PENDING_MIN = 200;
	// コメントがないときヘッダだけを書き込む間隔
	static const DWORD WRITE_PENDING_MAX = 1000;
	CJKTransfer();
	~CJKTransfer();
	void BeginClose();
	void Close();
	bool Open(HWND hwnd, UINT msg, bool bEnablePost, DWORD processID);
	// コメントを送る
	bool SendChat(int jkID, const char *text);
	// 投稿を受信する
	std::string ProcessRecvPost();
private:
	bool CreateWorker(HWND hwnd, UINT msg, bool bEnablePost, DWORD processID);
	void WorkerThread(HWND hwnd, UINT msg, bool bEnablePost, DWORD processID);

	recursive_mutex_ workerLock_;
	std::thread workerThread_;
	HANDLE hWorkerEvent_;
	bool bWorkerCreated_;
	bool bContinueWorker_;
	bool bStopWroker_;
	int currentJKID_;
	std::vector<char> chatBuf_;
	std::string postStr_;
};
