#pragma once
#include <thread>
#include <vector>

// 実況データ送受信用のプロセスと通信する
class CJKStream
{
public:
	CJKStream();
	~CJKStream();
	void BeginClose();
	void Close();
#ifdef _WIN32
	bool Send(HWND hwnd, UINT msg, char command, const char *buf);
#else
	bool Send(CAutoResetEvent *recvEvent, char command, const char *buf);
#endif
	int ProcessRecv(std::vector<char> &recvBuf);
	bool Shutdown();
private:
	bool CreateWorker();
#ifdef _WIN32
	static bool CreateJKProcess(HANDLE &hProcess, HANDLE &hAsyncReadPipe, HANDLE &hWritePipe);
#else
	static bool CreateJKProcess(int &processID, int &asyncReadPipe, int &writePipe);
#endif
	void WorkerThread();

	std::recursive_mutex workerLock_;
	std::thread workerThread_;
	CAutoResetEvent workerEvent_;
#ifdef _WIN32
	HANDLE hProcess_;
	HWND hwndRecv_;
	UINT recvMsg_;
#else
	int processID_;
	CAutoResetEvent *recvEvent_;
	bool bWorkerDestroyed_;
#endif
	bool bContinueWorker_;
	bool bStopWorker_;
	bool bOpened_;
	bool bShutdown_;
	bool bShutdownSent_;
	std::vector<char> sendBuf_;
	std::vector<char> recvBuf_;
};
