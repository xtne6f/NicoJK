#pragma once

#include <stdio.h>
#include <memory>

// マルチバイトテキストファイル読み込み
class CTextFileReader
{
public:
	static const size_t BUF_SIZE = 512;
	CTextFileReader();
	bool Open(LPCTSTR path);
	bool OpenZippedFile(LPCTSTR zipPath, const char *fileName);
	void Close();
	void ResetPointer();
	size_t ReadLine(char *text, size_t textMax);
	size_t ReadLastLine(char *text, size_t textMax);
	LONGLONG Seek(LONGLONG scale);
	bool IsOpen() const { return fp_ || zipf_; }
	static void *TfopenSFileFuncForZlib(void *opaque, const void *filename, int mode);
private:
	std::unique_ptr<FILE, decltype(&fclose)> fp_;
	std::unique_ptr<void, int(*)(void *)> zipf_;
	bool bEof_;
	char buf_[BUF_SIZE];
};
