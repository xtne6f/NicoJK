#pragma once
#include <thread>
#include <vector>

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
#ifdef _WIN32
	bool Open(HWND hwnd, UINT msg, bool bEnablePost, int processID);
#else
	bool Open(CAutoResetEvent *recvPostEvent, bool bEnablePost, int processID);
#endif
	// コメントを送る
	bool SendChat(int jkID, const char *text);
	// 投稿を受信する
	std::string ProcessRecvPost();
private:
	bool CreateWorker(bool bEnablePost, int processID);
	void WorkerThread(bool bEnablePost, int processID);

	std::recursive_mutex workerLock_;
	std::thread workerThread_;
	CAutoResetEvent workerEvent_;
#ifdef _WIN32
	HWND hwndRecvPost_;
	UINT recvPostMsg_;
#else
	CAutoResetEvent *recvPostEvent_;
#endif
	bool bContinueWorker_;
	bool bStopWorker_;
	int currentJKID_;
	std::vector<char> chatBuf_;
	std::string postStr_;
};
