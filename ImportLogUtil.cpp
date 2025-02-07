#include "ToolsCommon.h"
#include <memory>
#include <regex>
#include "ImportLogUtil.h"

namespace
{
// ローカル形式をタイムシフトする
bool TxtToLocalFormat(LPCTSTR srcPath, const std::function<FILE *(unsigned int &)> &onChatTag)
{
	FILE *fp;
	size_t len = _tcslen(srcPath);
	if (len >= 5 && !ComparePath(&srcPath[len - 4], TEXT(".txt")) && srcPath[len - 5] != TEXT('/') &&
#ifdef _WIN32
	    srcPath[len - 5] != TEXT('\\') && !_tfopen_s(&fp, srcPath, TEXT("rN"))
#else
	    !!(fp = fopen(srcPath, "r"))
#endif
	    ) {
		std::unique_ptr<FILE, fclose_deleter> fpSrc(fp);
		const std::regex re("<chat(?= )[^>]*? date=\"(\\d+)\"[^]*\n");
		std::cmatch m;
		char buf[8192];
		bool bFound = false;
		while (fgets(buf, sizeof(buf), fpSrc.get())) {
			if (std::regex_match(buf, m, re)) {
				// chatタグが1行以上見つかれば成功
				bFound = true;
				unsigned int tm = strtoul(m[1].first, nullptr, 10);
				fp = onChatTag(tm);
				if (fp) {
					fwrite(buf, 1, m[1].first - buf, fp);
					fprintf(fp, "%u", tm);
					fwrite(m[1].second, 1, m[0].second - m[1].second - (*(m[0].second - 2) == '\r' ? 2 : 1), fp);
					// 必ずCRLF
					fputs("\r\n", fp);
				}
			}
		}
		return bFound;
	}
	return false;
}

void WriteChatTag(FILE *fp, const std::cmatch &m, unsigned int tm)
{
	fwrite(m[0].first, 1, m[1].first - m[0].first, fp);
	fprintf(fp, "%u", tm);
	const char *p = m[1].second;
	size_t len = 0;
	while (p + len < m[0].second) {
		// 改行文字は数値文字参照に置換
		if (p[len] == '\n' || p[len] == '\r') {
			fwrite(p, 1, len, fp);
			fprintf(fp, "&#%d;", p[len]);
			p += len + 1;
			len = 0;
		} else {
			++len;
		}
	}
	fwrite(p, 1, len, fp);
	// 必ずCRLF
	fputs("\r\n", fp);
}

// JikkyoRec.jklをローカル形式に変換する
bool JklToLocalFormat(LPCTSTR srcPath, const std::function<FILE *(unsigned int &)> &onChatTag)
{
	FILE *fp;
	size_t len = _tcslen(srcPath);
	if (len >= 5 && !ComparePath(&srcPath[len - 4], TEXT(".jkl")) && srcPath[len - 5] != TEXT('/') &&
#ifdef _WIN32
	    srcPath[len - 5] != TEXT('\\') && !_tfopen_s(&fp, srcPath, TEXT("rbN"))
#else
	    !!(fp = fopen(srcPath, "r"))
#endif
	    ) {
		std::unique_ptr<FILE, fclose_deleter> fpSrc(fp);
		char buf[8192];
		if (fread(buf, 1, 10, fpSrc.get()) == 10 && !memcmp(buf, "<JikkyoRec", 10)) {
			// 空行まで読み飛ばす
			int c;
			for (int d = '\0'; (c = fgetc(fpSrc.get())) != EOF && !(d=='\n' && (c=='\n' || c=='\r')); d = c);

			const std::regex re("<chat(?= )[^>]*? date=\"(\\d+)\"[^]*?(?:/>|</chat>)");
			std::cmatch m;
			size_t bufLen = 0;
			while ((c = fgetc(fpSrc.get())) != EOF) {
				if (bufLen >= sizeof(buf)) {
					bufLen = 0;
					continue;
				}
				buf[bufLen++] = static_cast<char>(c);
				if (c == '\0') {
					if (std::regex_search(buf, m, re)) {
						unsigned int tm = strtoul(m[1].first, nullptr, 10);
						fp = onChatTag(tm);
						if (fp) {
							WriteChatTag(fp, m, tm);
						}
					}
					bufLen = 0;
				}
			}
			return true;
		}
	}
	return false;
}

// ニコニコ実況コメントビューア.xmlをローカル形式に変換する
bool XmlToLocalFormat(LPCTSTR srcPath, const std::function<FILE *(unsigned int &)> &onChatTag)
{
	FILE *fp;
	size_t len = _tcslen(srcPath);
	if (len >= 5 && !ComparePath(&srcPath[len - 4], TEXT(".xml")) && srcPath[len - 5] != TEXT('/') &&
#ifdef _WIN32
	    srcPath[len - 5] != TEXT('\\') && !_tfopen_s(&fp, srcPath, TEXT("rN"))
#else
	    !!(fp = fopen(srcPath, "r"))
#endif
	    ) {
		std::unique_ptr<FILE, fclose_deleter> fpSrc(fp);
		char buf[8192];
		if (fgets(buf, sizeof(buf), fpSrc.get()) && strstr(buf, "<?xml")) {
			const std::regex re("<chat(?= )[^>]*? date=\"(\\d+)\"[^]*?(?:/>|</chat>)");
			std::cmatch m;
			size_t bufLen = 0;
			while (fgets(buf + bufLen, static_cast<int>(sizeof(buf) - bufLen), fpSrc.get())) {
				bufLen += strlen(buf + bufLen);
				if (bufLen >= sizeof(buf) - 1) {
					bufLen = 0;
					continue;
				}
				if (std::regex_search(buf, m, re)) {
					unsigned int tm = strtoul(m[1].first, nullptr, 10);
					fp = onChatTag(tm);
					if (fp) {
						WriteChatTag(fp, m, tm);
					}
					bufLen = 0;
				}
			}
			return true;
		}
	}
	return false;
}
}

bool ImportLogfile(LPCTSTR srcPath, const std::function<FILE *(unsigned int &)> &onChatTag)
{
	return JklToLocalFormat(srcPath, onChatTag) ||
	       XmlToLocalFormat(srcPath, onChatTag) ||
	       TxtToLocalFormat(srcPath, onChatTag);
}

bool ImportLogfile(LPCTSTR srcPath, LPCTSTR destPath, unsigned int tmNew)
{
	std::unique_ptr<FILE, fclose_deleter> fpDest;
	unsigned int tmOld = 0;
	bool bFirst = true;
	bool bRet = ImportLogfile(srcPath, [=, &fpDest, &tmOld, &bFirst](unsigned int &tm) -> FILE * {
		if (bFirst) {
#ifdef _WIN32
			FILE *fp;
			if (!_tfopen_s(&fp, destPath, TEXT("wbN"))) {
				fpDest.reset(fp);
			}
#else
			fpDest.reset(fopen(destPath, "w"));
#endif
			tmOld = tm;
			bFirst = false;
		}
		if (tmNew != 0) {
			tm = tm - tmOld + tmNew;
		}
		return fpDest.get();
	});
	return bRet && fpDest;
}
