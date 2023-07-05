#pragma once

#include "TextFileReader.h"
#include <functional>
#include <string>
#include <vector>

// ログファイル読み込み
class CLogReader
{
public:
	// 処理できるchatタグの最大文字数(char)
	// (既定値はコメントの制限を1024文字として、これが実体参照であった場合の*5にマージンを加えた値)
	static const int CHAT_TAG_MAX = 1024 * 6;
	CLogReader();
	void SetLogDirectory(LPCTSTR path);
	void SetJK0LogfilePath(LPCTSTR path);
	void SetCheckIntervalMsec(DWORD checkInterval);
	void ResetCheckInterval();
	bool IsOpen() const { return currentJKID_ >= 0; }
	bool IsLatestLogfile() const { return bLatestLogfile_; }
	// 指定した実況IDの指定時刻のログ1行を読み込む
	// jkIDが負値のときはログファイルを閉じる
	// jkID==0は指定ファイル再生(JK0Logfile)を表す特殊な実況IDとする
	// jkID>=0のときtextは必須。戻り値が真のとき*textは次にこのメソッドを呼ぶまで有効
	bool Read(int jkID, const std::function<void(LPCTSTR)> &onMessage = nullptr, const char **text = nullptr, unsigned int tmToRead = 0);
	// 次に読み込みが可能になる見込みの時刻を探す
	// 失敗のときは0が返る
	unsigned int FindNextReadableTime(int jkID, unsigned int tmToRead) const;
	static bool GetChatDate(unsigned int *tm, const char *tag);
private:
	struct FIND_LOGFILE_ELEM {
		char name[16];
	};
	struct FIND_LOGFILE_CACHE {
		std::basic_string<TCHAR> path;
		std::vector<FIND_LOGFILE_ELEM> list;
		size_t index;
	};
	// tmより前/以後でもっとも新しい/古いログファイルをアーカイブから探す
	static const char *FindZippedLogfile(FIND_LOGFILE_CACHE &cache, bool &bSameResult, LPCTSTR zipPath, unsigned int tm, bool bBeforeOrAfter);

	int currentJKID_;
	CTextFileReader readLogfile_;
	bool bLatestLogfile_;
	char readLogText_[2][CHAT_TAG_MAX];
	bool bReadLogTextNext_;
	unsigned int tmReadLogText_;
	DWORD checkInterval_;
	DWORD checkTick_;
	FIND_LOGFILE_CACHE findZippedLogfileCache_;
	unsigned int tmZippedLogfileCachedLast_;
	std::basic_string<TCHAR> jk0LogfilePath_;
	std::basic_string<TCHAR> logDirectory_;
};
