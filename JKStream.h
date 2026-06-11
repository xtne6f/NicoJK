#pragma once
#include <thread>

// 実況データ送受信用のプロセスと通信する
class CJKStream
{
public:
	CJKStream();
	~CJKStream();
	void BeginClose();
	void Close();
	bool Send(HWND hwnd, UINT msg, char command, const char *buf);
	int ProcessRecv(std::vector<char> &recvBuf);
	bool Shutdown();
private:
	bool CreateWorker();
	static bool CreateJKProcess(HANDLE &hProcess, HANDLE &hAsyncReadPipe, HANDLE &hWritePipe);
	void WorkerThread();

	std::recursive_mutex workerLock_;
	std::thread workerThread_;
	CAutoResetEvent workerEvent_;
	HANDLE hProcess_;
	HWND hwndRecv_;
	UINT recvMsg_;
	bool bContinueWorker_;
	bool bStopWorker_;
	bool bOpened_;
	bool bShutdown_;
	bool bShutdownSent_;
	std::vector<char> sendBuf_;
	std::vector<char> recvBuf_;
};
