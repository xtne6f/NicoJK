#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tchar.h>
#include "TextFileReader.h"
#include "unzip.h"
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <algorithm>

CTextFileReader::CTextFileReader()
	: fp_(nullptr, fclose)
	, zipf_(nullptr, unzClose)
	, bEof_(false)
{
	buf_[0] = '\0';
}

bool CTextFileReader::Open(LPCTSTR path)
{
	Close();
	// 継承を無効にするため低水準で開く
	int fd;
	if (_tsopen_s(&fd, path, _O_BINARY | _O_NOINHERIT | _O_RDONLY | _O_SEQUENTIAL, _SH_DENYNO, 0) == 0) {
		fp_.reset(_tfdopen(fd, TEXT("rb")));
		if (fp_) {
			if (setvbuf(fp_.get(), nullptr, _IONBF, 0) == 0) {
				return true;
			}
			fp_.reset();
		} else {
			_close(fd);
		}
	}
	return false;
}

bool CTextFileReader::OpenZippedFile(LPCTSTR zipPath, const char *fileName)
{
	Close();
	zlib_filefunc64_def def;
	fill_fopen64_filefunc(&def);
	def.zopen64_file = TfopenSFileFuncForZlib;
	zipf_.reset(unzOpen2_64(zipPath, &def));
	if (zipf_) {
		if (unzLocateFile(zipf_.get(), fileName, 0) == UNZ_OK && unzOpenCurrentFile(zipf_.get()) == UNZ_OK) {
			return true;
		}
		zipf_.reset();
	}
	return false;
}

void CTextFileReader::Close()
{
	fp_.reset();
	zipf_.reset();
	bEof_ = false;
	buf_[0] = '\0';
}

// ファイルポインタを先頭に戻す
void CTextFileReader::ResetPointer()
{
	if (IsOpen()) {
		if (zipf_) {
			unzOpenCurrentFile(zipf_.get());
		} else {
			rewind(fp_.get());
		}
		bEof_ = false;
		buf_[0] = '\0';
	}
}

// 1行またはNULを含む最大textMax(>0)バイト読み込む
// 改行文字は取り除く
// 戻り値はNULを含む読み込まれたバイト数、終端に達すると0を返す
size_t CTextFileReader::ReadLine(char *text, size_t textMax)
{
	if (!IsOpen()) {
		return 0;
	}
	size_t textLen = 0;
	for (;;) {
		if (!bEof_) {
			size_t bufLen = strlen(buf_);
			size_t readLen;
			if (zipf_) {
				int n = unzReadCurrentFile(zipf_.get(), buf_ + bufLen, static_cast<unsigned int>(BUF_SIZE - bufLen - 1));
				readLen = n < 0 ? 0 : n;
			} else {
				readLen = fread(buf_ + bufLen, 1, BUF_SIZE - bufLen - 1, fp_.get());
			}
			if (readLen == 0) {
				buf_[bufLen] = '\0';
				bEof_ = true;
			} else {
				buf_[bufLen + readLen] = '\0';
				if (strlen(buf_) < BUF_SIZE - 1) {
					bEof_ = true;
				}
			}
		}
		if (!textLen && !buf_[0]) {
			return 0;
		}
		size_t lineLen = strcspn(buf_, "\n");
		size_t copyNum = std::min(lineLen, textMax - textLen - 1);
		strncpy_s(text + textLen, textMax - textLen, buf_, copyNum);
		textLen += copyNum;
		if (lineLen < BUF_SIZE - 1) {
			if (buf_[lineLen] == '\n') ++lineLen;
			memmove(buf_, buf_ + lineLen, sizeof(buf_) - lineLen);
			if (textLen >= 1 && text[textLen-1] == '\r') {
				text[--textLen] = '\0';
			}
			return textLen + 1;
		}
		buf_[0] = '\0';
	}
}

// 最終行を1行またはNULを含む最大textMax(>0)バイト(収まらない場合行頭側をカット)読み込む
// 改行文字は取り除く
// ファイルポインタは先頭に戻る
// 戻り値はNULを含む読み込まれたバイト数
size_t CTextFileReader::ReadLastLine(char *text, size_t textMax)
{
	if (!fp_) {
		return 0;
	}
	// バイナリモードでのSEEK_ENDは厳密には議論あるが、Windowsでは問題ない
	if (_fseeki64(fp_.get(), 0, SEEK_END) != 0 ||
	    _fseeki64(fp_.get(), -std::min<LONGLONG>(textMax - 1, _ftelli64(fp_.get())), SEEK_END) != 0) {
		ResetPointer();
		return 0;
	}
	size_t readLen = fread(text, 1, textMax - 1, fp_.get());
	if (readLen == 0) {
		ResetPointer();
		return 0;
	}
	text[readLen] = '\0';
	size_t textLen = strlen(text);
	if (textLen >= 1 && text[textLen-1] == '\n') {
		text[--textLen] = '\0';
	}
	if (textLen >= 1 && text[textLen-1] == '\r') {
		text[--textLen] = '\0';
	}
	for (size_t i = textLen; i > 0; --i) {
		if (text[i - 1] == '\n') {
			memmove(text, text + i, textLen - i + 1);
			textLen -= i;
			break;
		}
	}
	ResetPointer();
	return textLen + 1;
}

// 現在位置からファイルサイズ/scaleだけシークする
// 戻り値はファイルポインタの移動バイト数
LONGLONG CTextFileReader::Seek(LONGLONG scale)
{
	if (!fp_ || scale == 0) {
		return 0;
	}
	LONGLONG filePos = _ftelli64(fp_.get());
	if (_fseeki64(fp_.get(), 0, SEEK_END) != 0) {
		_fseeki64(fp_.get(), filePos, SEEK_SET);
		return 0;
	}
	LONGLONG fileSize = _ftelli64(fp_.get());
	LONGLONG nextPos = fileSize / (scale < 0 ? -scale : scale) * (scale < 0 ? -1 : 1) + filePos;
	nextPos = std::min(std::max<LONGLONG>(nextPos, 0), fileSize);
	if (_fseeki64(fp_.get(), nextPos, SEEK_SET) != 0) {
		_fseeki64(fp_.get(), filePos, SEEK_SET);
		return 0;
	}
	bEof_ = false;
	buf_[0] = '\0';
	return nextPos - filePos;
}

// zlib_filefunc64_def構造体のzopen64_file関数のTCHAR版
void *CTextFileReader::TfopenSFileFuncForZlib(void *opaque, const void *filename, int mode)
{
	static_cast<void>(opaque);
	LPCTSTR tmode = (mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) == ZLIB_FILEFUNC_MODE_READ ? TEXT("rbN") :
	                (mode & ZLIB_FILEFUNC_MODE_EXISTING) != 0 ? TEXT("r+bN") :
	                (mode & ZLIB_FILEFUNC_MODE_CREATE) != 0 ? TEXT("wbN") : nullptr;
	FILE *fp;
	if (filename && tmode && _tfopen_s(&fp, static_cast<LPCTSTR>(filename), tmode) == 0) {
		return fp;
	}
	return nullptr;
}
