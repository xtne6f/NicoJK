/*
	NicoJK
		TVTest ニコニコ実況プラグイン
*/

#include "stdafx.h"
#include "ImportLogUtil.h"
#include "Util.h"
#include "JKStream.h"
#include "JKTransfer.h"
#include "LogReader.h"
#include "CommentWindow.h"
#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "TVTestPlugin.h"
#include "resource.h"
#include "NetworkServiceIDTable.h"
#include "JKIDNameTable.h"
#include "NicoJK.h"
#include <dwmapi.h>
#include <shellapi.h>

#pragma comment(lib, "dwmapi.lib")

#ifdef _DEBUG
#include <stdarg.h>
inline void dprintf_real( const _TCHAR * fmt, ... )
{
  _TCHAR buf[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsntprintf_s(buf, 1024, fmt, ap);
  va_end(ap);
  OutputDebugString(buf);
}
#  define dprintf dprintf_real
#else
#  define dprintf __noop
#endif

// 通信用
#define WMS_FORCE (WM_APP + 101)
#define WMS_JK (WM_APP + 102)
#define WMS_TRANSFER (WM_APP + 103)

#define WM_RESET_STREAM (WM_APP + 105)
#define WM_UPDATE_LIST (WM_APP + 106)
#define WM_SET_ZORDER (WM_APP + 107)
#define WM_POST_COMMENT (WM_APP + 108)

enum {
	TIMER_UPDATE = 1,
	TIMER_JK_WATCHDOG,
	TIMER_FORWARD,
	TIMER_SETUP_CURJK,
	TIMER_OPEN_DROPFILE,
	TIMER_DONE_MOVE,
	TIMER_DONE_SIZE,
	TIMER_DONE_POSCHANGE,
	TIMER_UPDATE_LIST,
};

enum {
	COMMAND_HIDE_FORCE,
	COMMAND_HIDE_COMMENT,
	COMMAND_FORWARD_A,
};

void CNicoJK::RPL_ELEM::SetEnabled(bool b)
{
	if (!pattern.empty()) {
		if (b && TEXT('A') <= pattern[0] && pattern[0] <= TEXT('Z')) {
			pattern[0] = pattern[0] - TEXT('A') + TEXT('a');
		} else if (!b && TEXT('a') <= pattern[0] && pattern[0] <= TEXT('z')) {
			pattern[0] = pattern[0] - TEXT('a') + TEXT('A');
		}
	}
}

bool CNicoJK::RPL_ELEM::SetPattern(LPCTSTR patt)
{
	// 入力パターンはsedコマンド等の形式をまねたもの
	// ただし今のところ's/{regex}/{replace}/g'のみ対応(拡張可能)
	// 先頭文字が大文字の場合はそのパターンが無効状態であることを示す
	static const std::regex reBrace("[Ss](.)(.+?)\\1(.*?)\\1g");
	std::vector<char> utf8(WideCharToMultiByte(CP_UTF8, 0, patt, -1, nullptr, 0, nullptr, nullptr));
	if (utf8.empty() || WideCharToMultiByte(CP_UTF8, 0, patt, -1, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr) == 0) {
		return false;
	}
	std::cmatch m;
	if (!std::regex_match(utf8.data(), m, reBrace)) {
		return false;
	}
	try {
		re.assign(m[2].first, m[2].length());
	} catch (std::regex_error&) {
		return false;
	}
	pattern = patt;
	fmt.assign(m[3].first, m[3].length());
	return true;
}

CNicoJK::CNicoJK()
	: bDragAcceptFiles_(false)
	, hPanel_(nullptr)
	, hForce_(nullptr)
	, hForceFont_(nullptr)
	, bDisplayLogList_(false)
	, logListDisplayedSize_(0)
	, bPendingTimerUpdateList_(false)
	, lastUpdateListTick_(0)
	, lastCalcWidth_(0)
	, forwardTick_(0)
	, bQuitSyncThread_(false)
	, bPendingTimerForward_(false)
	, bHalfSkip_(false)
	, bFlipFlop_(false)
	, forwardOffset_(0)
	, forwardOffsetDelta_(0)
	, currentJKToGet_(-1)
	, currentJK_(-1)
	, currentJKChatCount_(0)
	, currentJKForceByChatCount_(-1)
	, currentJKForceByChatCountTick_(0)
	, lastPostTick_(0)
	, bRecording_(false)
	, hQuitCheckRecordingEvent_(nullptr)
	, bUsingLogfileDriver_(false)
	, bSetStreamCallback_(false)
	, bResyncComment_(false)
	, currentLogfileJK_(-1)
	, hLogfile_(INVALID_HANDLE_VALUE)
	, hLogfileLock_(INVALID_HANDLE_VALUE)
	, llftTot_(-1)
	, pcr_(0)
	, pcrTick_(0)
	, pcrPid_(-1)
	, bSpecFile_(false)
	, dropFileTimeout_(0)
{
	cookie_[0] = '\0';
	lastPostComm_[0] = TEXT('\0');
	logReader_.SetCheckIntervalMsec(READ_LOG_FOLDER_INTERVAL);
	SETTINGS s = {};
	s_ = s;
	pcrPids_[0] = -1;
}

bool CNicoJK::GetPluginInfo(TVTest::PluginInfo *pInfo)
{
	// プラグインの情報を返す
	pInfo->Type           = TVTest::PLUGIN_TYPE_NORMAL;
	pInfo->Flags          = 0;
	pInfo->pszPluginName  = L"NicoJK";
	pInfo->pszCopyright   = L"Public Domain";
	pInfo->pszDescription = L"ニコニコ実況をSDKで表示";
	return true;
}

bool CNicoJK::Initialize()
{
	// ウィンドウクラスを登録
	WNDCLASSEX wcPanel = {};
	wcPanel.cbSize = sizeof(wcPanel);
	wcPanel.style = 0;
	wcPanel.lpfnWndProc = PanelWindowProc;
	wcPanel.hInstance = g_hinstDLL;
	wcPanel.lpszClassName = TEXT("ru.jk.panel");
	if (RegisterClassEx(&wcPanel) == 0) {
		return false;
	}
	WNDCLASSEX wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_VREDRAW | CS_HREDRAW;
	wc.lpfnWndProc = ForceWindowProc;
	wc.hInstance = g_hinstDLL;
	wc.hbrBackground = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
	wc.lpszClassName = TEXT("ru.jk.force");
	if (RegisterClassEx(&wc) == 0) {
		return false;
	}
	// 初期化処理
	TCHAR path[MAX_PATH];
	iniFileName_.clear();
	tmpSpecFileName_.clear();
	if (GetLongModuleFileName(g_hinstDLL, path, _countof(path))) {
		iniFileName_ = path;
		size_t lastSep = iniFileName_.find_last_of(TEXT("/\\."));
		if (lastSep != tstring::npos && iniFileName_[lastSep] == TEXT('.')) {
			iniFileName_.erase(lastSep);
		}
		tmpSpecFileName_ = iniFileName_;
		iniFileName_ += TEXT(".ini");
		TCHAR ext[32];
		_stprintf_s(ext, TEXT("_%u.tmp"), GetCurrentProcessId());
		tmpSpecFileName_ += ext;
		logReader_.SetJK0LogfilePath(tmpSpecFileName_.c_str());
	}
	// OsdCompositorは他プラグインと共用することがあるので、有効にするならFinalize()まで破棄しない
	bool bEnableOsdCompositor = GetPrivateProfileInt(TEXT("Setting"), TEXT("enableOsdCompositor"), 0, iniFileName_.c_str()) != 0;
	// フィルタグラフを取得できないバージョンではAPIフックを使う
	bool bSetHookOsdCompositor = m_pApp->GetVersion() < TVTest::MakeVersion(0, 9, 0);
	if (!commentWindow_.Initialize(g_hinstDLL, &bEnableOsdCompositor, bSetHookOsdCompositor)) {
		return false;
	}
	if (bEnableOsdCompositor) {
		m_pApp->AddLog(L"OsdCompositorを初期化しました。");
	}
	int dpi = m_pApp->GetDPIFromWindow(m_pApp->GetAppWindow());
	int iconWidth = 16 * dpi / 96;
	int iconHeight = 16 * dpi / 96;
	m_pApp->GetStyleValuePixels(L"side-bar.item.icon.width", dpi, &iconWidth);
	m_pApp->GetStyleValuePixels(L"side-bar.item.icon.height", dpi, &iconHeight);
	bool bSmallIcon = iconWidth <= 16 && iconHeight <= 16;
	// アイコンを登録
	m_pApp->RegisterPluginIconFromResource(g_hinstDLL, MAKEINTRESOURCE(IDB_ICON));

	// パネル項目を登録
	s_.bUsePanel = GetPrivateProfileInt(TEXT("Setting"), TEXT("usePanel"), 1, iniFileName_.c_str()) != 0;
	if (s_.bUsePanel) {
		TVTest::PanelItemInfo pi;
		pi.Size = sizeof(pi);
		pi.Flags = 0;
		pi.Style = TVTest::PANEL_ITEM_STYLE_NEEDFOCUS;
		pi.ID = 1;
		pi.pszIDText = L"NicoJK";
		pi.pszTitle = L"NicoJK";
		pi.hbmIcon = static_cast<HBITMAP>(LoadImage(g_hinstDLL, MAKEINTRESOURCE(IDB_ICON), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
		s_.bUsePanel = m_pApp->RegisterPanelItem(&pi);
		DeleteObject(pi.hbmIcon);
	}
	// コマンドを登録
	TVTest::PluginCommandInfo ci;
	ci.Size = sizeof(ci);
	ci.Flags = TVTest::PLUGIN_COMMAND_FLAG_ICONIZE;
	ci.State = TVTest::PLUGIN_COMMAND_STATE_DISABLED;

	ci.ID = COMMAND_HIDE_FORCE;
	ci.pszText = L"HideForce";
	ci.pszDescription = ci.pszName = L"勢いウィンドウの表示切替";
	ci.hbmIcon = static_cast<HBITMAP>(LoadImage(g_hinstDLL, MAKEINTRESOURCE(bSmallIcon ? IDB_FORCE16 : IDB_FORCE), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
	if (!s_.bUsePanel && !m_pApp->RegisterPluginCommand(&ci)) {
		m_pApp->RegisterCommand(ci.ID, ci.pszText, ci.pszName);
	}
	DeleteObject(ci.hbmIcon);

	ci.ID = COMMAND_HIDE_COMMENT;
	ci.pszText = L"HideComment";
	ci.pszDescription = ci.pszName = L"実況コメントの表示切替";
	ci.hbmIcon = static_cast<HBITMAP>(LoadImage(g_hinstDLL, MAKEINTRESOURCE(bSmallIcon ? IDB_COMMENT16 : IDB_COMMENT), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
	if (!m_pApp->RegisterPluginCommand(&ci)) {
		m_pApp->RegisterCommand(ci.ID, ci.pszText, ci.pszName);
	}
	DeleteObject(ci.hbmIcon);

	memset(s_.forwardList, 0, sizeof(s_.forwardList));
	for (int i = 0; i < _countof(s_.forwardList); ++i) {
		TCHAR key[16], name[32];
		_stprintf_s(key, TEXT("Forward%c"), TEXT('A') + i);
		_stprintf_s(name, TEXT("実況コメントの前進:%c"), TEXT('A') + i);
		if ((s_.forwardList[i] = GetPrivateProfileInt(TEXT("Setting"), key, INT_MAX, iniFileName_.c_str())) == INT_MAX) {
			break;
		}
		m_pApp->RegisterCommand(COMMAND_FORWARD_A + i, key, name);
	}
	// イベントコールバック関数を登録
	m_pApp->SetEventCallback(EventCallback, this);
	return true;
}

bool CNicoJK::Finalize()
{
	// 終了処理
	TogglePlugin(false);
	// パネルウィンドウを破棄
	if (hPanel_) {
		DestroyWindow(hPanel_);
	}
	// 本体や他プラグインとの干渉を防ぐため、一旦有効にしたD&Dは最後まで維持する
	if (bDragAcceptFiles_) {
		DragAcceptFiles(m_pApp->GetAppWindow(), FALSE);
		bDragAcceptFiles_ = false;
	}
	commentWindow_.Finalize();
	return true;
}

bool CNicoJK::TogglePlugin(bool bEnabled)
{
	if (bEnabled) {
		if ((!s_.bUsePanel || hPanel_) && !hForce_) {
			LoadFromIni();
			LoadForceListFromIni(s_.logfileFolder);

			// 必要ならサーバに渡すCookieを取得
			cookie_[0] = '\0';
			if (!s_.execGetCookie.empty()) {
				std::string strCookie = GetCookieString(s_.execGetCookie.c_str(), s_.execGetV10Key.c_str(), cookie_, _countof(cookie_), 10000);
				if (strCookie.empty()) {
					m_pApp->AddLog(L"execGetCookieの実行に失敗しました。", TVTest::LOG_TYPE_ERROR);
				}
				strncpy_s(cookie_, strCookie.c_str(), _TRUNCATE);
			}
			// 破棄のタイミングがややこしいので勢い窓のフォントはここで作る
			if (!hForceFont_) {
				LOGFONT lf = {};
				HDC hdc = GetDC(nullptr);
				lf.lfHeight = -(s_.forceFontSize * GetDeviceCaps(hdc, LOGPIXELSY) / 72);
				ReleaseDC(nullptr, hdc);
				lf.lfCharSet = SHIFTJIS_CHARSET;
				_tcscpy_s(lf.lfFaceName, s_.forceFontName);
				hForceFont_ = CreateFontIndirect(&lf);
			}

			// 勢い窓作成
			if (hPanel_) {
				hForce_ = CreateWindowEx(0, TEXT("ru.jk.force"), TEXT("NicoJK - ニコニコ実況勢い"),
				                         WS_CHILD, 0, 0, 320, 240, hPanel_, nullptr, g_hinstDLL, this);
			} else {
				hForce_ = CreateWindowEx(WS_EX_WINDOWEDGE | WS_EX_TOOLWINDOW, TEXT("ru.jk.force"), TEXT("NicoJK - ニコニコ実況勢い"),
				                         WS_CAPTION | WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
				                         CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr, g_hinstDLL, this);
			}
			if (hForce_) {
				// ウィンドウコールバック関数を登録
				m_pApp->SetWindowMessageCallback(WindowMsgCallback, this);
				// ストリームコールバック関数を登録(指定ファイル再生機能のために常に登録)
				ToggleStreamCallback(true);
				// DWMの更新タイミングでTIMER_FORWARDを呼ぶスレッドを開始(Vista以降)
				if (s_.timerInterval < 0) {
					OSVERSIONINFOEX vi;
					vi.dwOSVersionInfoSize = sizeof(vi);
					vi.dwMajorVersion = 6;
					BOOL bCompEnabled;
					// ここで"dwmapi.dll"を遅延読み込みしていることに注意(つまりXPではDwm*()を踏んではいけない)
					if (VerifyVersionInfo(&vi, VER_MAJORVERSION, VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL)) &&
					    SUCCEEDED(DwmIsCompositionEnabled(&bCompEnabled)) && bCompEnabled) {
						bQuitSyncThread_ = false;
						syncThread_ = std::thread([this]() { SyncThread(); });
						SetThreadPriority(syncThread_.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
					}
					if (!syncThread_.joinable()) {
						m_pApp->AddLog(L"Aeroが無効のため設定timerIntervalのリフレッシュ同期機能はオフになります。");
						SetTimer(hForce_, TIMER_FORWARD, 166667 / -s_.timerInterval, nullptr);
					}
				}
				if (s_.dropLogfileMode != 0) {
					DragAcceptFiles(m_pApp->GetAppWindow(), TRUE);
					bDragAcceptFiles_ = true;
				}
			}
		}
		return hForce_ != nullptr;
	} else {
		if (hForce_) {
			DestroyWindow(hForce_);
		}
		if (hForceFont_) {
			DeleteFont(hForceFont_);
			hForceFont_ = nullptr;
		}
		return true;
	}
}

void CNicoJK::SyncThread()
{
	DWORD count = 0;
	int timeout = 0;
	while (!bQuitSyncThread_) {
		if (FAILED(DwmFlush())) {
			// ビジーに陥らないように
			Sleep(500);
		}
		if (count >= 10000) {
			// 捌き切れない量のメッセージを送らない
			if (bPendingTimerForward_ && --timeout >= 0) {
				continue;
			}
			count -= 10000;
			timeout = 30;
			bPendingTimerForward_ = true;
			SendNotifyMessage(hForce_, WM_TIMER, TIMER_FORWARD, 0);
		}
		count += bHalfSkip_ ? -s_.timerInterval / 2 : -s_.timerInterval;
	}
}

void CNicoJK::CheckRecordingThread(DWORD processID)
{
	const DWORD CMD_SUCCESS = TRUE;
	const DWORD VIEW_APP_ST_REC = 2;
	const DWORD cmdViewAppGetStatus[] = {207, 0};
	TCHAR pipeName[64];
	_stprintf_s(pipeName, TEXT("\\\\.\\pipe\\View_Ctrl_BonNoWaitPipe_%d"), processID);

	while (WaitForSingleObject(hQuitCheckRecordingEvent_, 2000) == WAIT_TIMEOUT) {
		// EDCBのCtrlCmdインタフェースにアクセスしてその録画状態を調べる
		HANDLE pipe = CreateFile(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (pipe != INVALID_HANDLE_VALUE) {
			DWORD n;
			if (WriteFile(pipe, cmdViewAppGetStatus, sizeof(cmdViewAppGetStatus), &n, nullptr) && n == sizeof(cmdViewAppGetStatus)) {
				DWORD res[3];
				n = 0;
				for (DWORD m; n < sizeof(res) && ReadFile(pipe, reinterpret_cast<BYTE*>(res) + n, sizeof(res) - n, &m, nullptr); n += m);
				if (n == sizeof(res) && res[0] == CMD_SUCCESS) {
					bRecording_ = res[2] == VIEW_APP_ST_REC;
				}
			}
			CloseHandle(pipe);
		} else if (GetLastError() != ERROR_PIPE_BUSY) {
			break;
		}
	}
	bRecording_ = false;
}

void CNicoJK::ToggleStreamCallback(bool bSet)
{
	if (bSet) {
		if (!bSetStreamCallback_) {
			bSetStreamCallback_ = true;
			pcrPid_ = -1;
			pcrPids_[0] = -1;
			m_pApp->SetStreamCallback(0, StreamCallback, this);
		}
	} else {
		if (bSetStreamCallback_) {
			m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback);
			bSetStreamCallback_ = false;
		}
	}
}

std::vector<NETWORK_SERVICE_ID_ELEM>::iterator CNicoJK::LowerBoundNetworkServiceID(std::vector<NETWORK_SERVICE_ID_ELEM>::iterator first,
                                                                                   std::vector<NETWORK_SERVICE_ID_ELEM>::iterator last, DWORD ntsID)
{
	NETWORK_SERVICE_ID_ELEM e;
	e.ntsID = ntsID;
	return std::lower_bound(first, last, e, [](const NETWORK_SERVICE_ID_ELEM &a, const NETWORK_SERVICE_ID_ELEM &b) { return a.ntsID < b.ntsID; });
}

std::vector<CNicoJK::FORCE_ELEM>::iterator CNicoJK::LowerBoundJKID(std::vector<FORCE_ELEM>::iterator first,
                                                                   std::vector<FORCE_ELEM>::iterator last, int jkID)
{
	FORCE_ELEM e;
	e.jkID = jkID;
	return std::lower_bound(first, last, e, [](const FORCE_ELEM &a, const FORCE_ELEM &b) { return a.jkID < b.jkID; });
}

void CNicoJK::LoadFromIni()
{
	// iniはセクション単位で読むと非常に速い。起動時は処理が混み合うのでとくに有利
	std::vector<TCHAR> buf = GetPrivateProfileSectionBuffer(TEXT("Setting"), iniFileName_.c_str());
	s_.hideForceWindow		= GetBufferedProfileInt(buf.data(), TEXT("hideForceWindow"), 0);
	s_.forceFontSize		= GetBufferedProfileInt(buf.data(), TEXT("forceFontSize"), 10);
	GetBufferedProfileString(buf.data(), TEXT("forceFontName"), TEXT("Meiryo UI"), s_.forceFontName, _countof(s_.forceFontName));
	s_.timerInterval		= GetBufferedProfileInt(buf.data(), TEXT("timerInterval"), -10000);
	s_.halfSkipThreshold	= GetBufferedProfileInt(buf.data(), TEXT("halfSkipThreshold"), 9999);
	s_.commentLineMargin	= GetBufferedProfileInt(buf.data(), TEXT("commentLineMargin"), 125);
	s_.commentFontOutline	= GetBufferedProfileInt(buf.data(), TEXT("commentFontOutline"), 0);
	s_.commentSize			= GetBufferedProfileInt(buf.data(), TEXT("commentSize"), 100);
	s_.commentSizeMin		= GetBufferedProfileInt(buf.data(), TEXT("commentSizeMin"), 16);
	s_.commentSizeMax		= GetBufferedProfileInt(buf.data(), TEXT("commentSizeMax"), 9999);
	GetBufferedProfileString(buf.data(), TEXT("commentFontName"), TEXT("メイリオ"), s_.commentFontName, _countof(s_.commentFontName));
	GetBufferedProfileString(buf.data(), TEXT("commentFontNameMulti"), TEXT("メイリオ"), s_.commentFontNameMulti, _countof(s_.commentFontNameMulti));
	GetBufferedProfileString(buf.data(), TEXT("commentFontNameEmoji"), TEXT(""), s_.commentFontNameEmoji, _countof(s_.commentFontNameEmoji));
	s_.bCommentFontBold		= GetBufferedProfileInt(buf.data(), TEXT("commentFontBold"), 1) != 0;
	s_.bCommentFontAntiAlias = GetBufferedProfileInt(buf.data(), TEXT("commentFontAntiAlias"), 1) != 0;
	s_.commentDuration		= GetBufferedProfileInt(buf.data(), TEXT("commentDuration"), CCommentWindow::DISPLAY_DURATION);
	s_.commentDrawLineCount = GetBufferedProfileInt(buf.data(), TEXT("commentDrawLineCount"), CCommentWindow::DEFAULT_LINE_DRAW_COUNT);
	s_.commentShareMode		= GetBufferedProfileInt(buf.data(), TEXT("commentShareMode"), 0);
	s_.logfileMode			= GetBufferedProfileInt(buf.data(), TEXT("logfileMode"), 0);
	s_.bCheckProcessRecording = GetBufferedProfileInt(buf.data(), TEXT("checkProcessRecording"), 1) != 0;
	s_.logfileDrivers		= GetBufferedProfileToString(buf.data(), TEXT("logfileDrivers"),
							                             TEXT("BonDriver_UDP.dll:BonDriver_TCP.dll:BonDriver_File.dll:BonDriver_RecTask.dll:BonDriver_TsTask.dll:")
							                             TEXT("BonDriver_NetworkPipe.dll:BonDriver_Pipe.dll:BonDriver_Pipe2.dll"));
	s_.nonTunerDrivers		= GetBufferedProfileToString(buf.data(), TEXT("nonTunerDrivers"),
							                             TEXT("BonDriver_UDP.dll:BonDriver_TCP.dll:BonDriver_File.dll:BonDriver_RecTask.dll:BonDriver_TsTask.dll:")
							                             TEXT("BonDriver_NetworkPipe.dll:BonDriver_Pipe.dll:BonDriver_Pipe2.dll"));
	s_.execGetCookie		= GetBufferedProfileToString(buf.data(), TEXT("execGetCookie"), TEXT("cmd /c echo ;"));
	s_.execGetV10Key		= GetBufferedProfileToString(buf.data(), TEXT("execGetV10Key"), TEXT(""));
	tstring val				= GetBufferedProfileToString(buf.data(), TEXT("channelsUri"), TEXT(""));
	s_.channelsUri.clear();
	for (size_t i = 0; i < val.size(); ++i) {
		if (TEXT('!') <= val[i] && val[i] <= TEXT('~')) {
			s_.channelsUri += static_cast<char>(val[i]);
		}
	}
	s_.mailDecorations		= GetBufferedProfileToString(buf.data(), TEXT("mailDecorations"), TEXT("[cyan]:[red]:[green small]:[orange]::"));
	s_.bAnonymity			= GetBufferedProfileInt(buf.data(), TEXT("anonymity"), 1) != 0;
	s_.bUseOsdCompositor	= GetBufferedProfileInt(buf.data(), TEXT("useOsdCompositor"), 0) != 0;
	s_.bUseTexture			= GetBufferedProfileInt(buf.data(), TEXT("useTexture"), 1) != 0;
	s_.bUseDrawingThread	= GetBufferedProfileInt(buf.data(), TEXT("useDrawingThread"), 1) != 0;
	s_.bSetChannel			= GetBufferedProfileInt(buf.data(), TEXT("setChannel"), 1) != 0;
	s_.maxAutoReplace		= GetBufferedProfileInt(buf.data(), TEXT("maxAutoReplace"), 20);
	s_.abone				= GetBufferedProfileToString(buf.data(), TEXT("abone"), TEXT("### NG ### &"));
	s_.dropLogfileMode		= GetBufferedProfileInt(buf.data(), TEXT("dropLogfileMode"), 0);
	s_.defaultPlaybackDelay	= GetBufferedProfileInt(buf.data(), TEXT("defaultPlaybackDelay"), 500);
	// 実況ログフォルダのパスを作成
	TCHAR path[MAX_PATH];
	GetBufferedProfileString(buf.data(), TEXT("logfileFolder"), TEXT("Plugins\\NicoJK"), path, _countof(path));
	if (path[0] && !_tcschr(TEXT("/\\"), path[0]) && path[1] != TEXT(':')) {
		// 相対パス
		TCHAR dir[MAX_PATH];
		if (GetLongModuleFileName(nullptr, dir, _countof(dir))) {
			s_.logfileFolder = dir;
			size_t lastSep = s_.logfileFolder.find_last_of(TEXT("/\\"));
			if (lastSep != tstring::npos) {
				s_.logfileFolder.erase(lastSep + 1);
			}
			s_.logfileFolder += path;
		} else {
			s_.logfileFolder.clear();
		}
	} else {
		s_.logfileFolder = path;
	}
	DWORD attr = GetFileAttributes(s_.logfileFolder.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		s_.logfileFolder.clear();
	}
	logReader_.SetLogDirectory(s_.logfileFolder.c_str());

	buf = GetPrivateProfileSectionBuffer(TEXT("Window"), iniFileName_.c_str());
	s_.rcForce.left			= GetBufferedProfileInt(buf.data(), TEXT("ForceX"), 0);
	s_.rcForce.top			= GetBufferedProfileInt(buf.data(), TEXT("ForceY"), 0);
	s_.rcForce.right		= GetBufferedProfileInt(buf.data(), TEXT("ForceWidth"), 0) + s_.rcForce.left;
	s_.rcForce.bottom		= GetBufferedProfileInt(buf.data(), TEXT("ForceHeight"), 0) + s_.rcForce.top;
	s_.forceOpacity			= GetBufferedProfileInt(buf.data(), TEXT("ForceOpacity"), 255);
	s_.commentOpacity		= GetBufferedProfileInt(buf.data(), TEXT("CommentOpacity"), 255);
	s_.headerMask			= GetBufferedProfileInt(buf.data(), TEXT("HeaderMask"), 0);
	s_.bSetRelative			= GetBufferedProfileInt(buf.data(), TEXT("SetRelative"), 0) != 0;

	ntsIDList_.assign(DEFAULT_NTSID_TABLE, DEFAULT_NTSID_TABLE + _countof(DEFAULT_NTSID_TABLE));
	// 設定ファイルのネットワーク/サービスID-実況ID対照表を、ソートを維持しながらマージ
	buf = GetPrivateProfileSectionBuffer(TEXT("Channels"), iniFileName_.c_str());
	for (LPCTSTR p = buf.data(); *p; p += _tcslen(p) + 1) {
		NETWORK_SERVICE_ID_ELEM e;
		bool bPrior = _stscanf_s(p, TEXT("0x%x=+%d"), &e.ntsID, &e.jkID) == 2;
		if (bPrior) {
			e.jkID |= NETWORK_SERVICE_ID_ELEM::JKID_PRIOR;
		}
		if (bPrior || _stscanf_s(p, TEXT("0x%x=%d"), &e.ntsID, &e.jkID) == 2) {
			// 設定ファイルの定義では上位と下位をひっくり返しているので補正
			e.ntsID = (e.ntsID<<16) | (e.ntsID>>16);
			std::vector<NETWORK_SERVICE_ID_ELEM>::iterator it = LowerBoundNetworkServiceID(ntsIDList_.begin(), ntsIDList_.end(), e.ntsID);
			if (it != ntsIDList_.end() && it->ntsID == e.ntsID) {
				*it = e;
			} else {
				ntsIDList_.insert(it, e);
			}
		}
	}

	rplList_.clear();
	LoadRplListFromIni(TEXT("AutoReplace"), &rplList_);
	LoadRplListFromIni(TEXT("CustomReplace"), &rplList_);
}

void CNicoJK::SaveToIni()
{
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceX"), s_.rcForce.left, iniFileName_.c_str());
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceY"), s_.rcForce.top, iniFileName_.c_str());
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceWidth"), s_.rcForce.right - s_.rcForce.left, iniFileName_.c_str());
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceHeight"), s_.rcForce.bottom - s_.rcForce.top, iniFileName_.c_str());
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceOpacity"), s_.forceOpacity, iniFileName_.c_str());
	WritePrivateProfileInt(TEXT("Window"), TEXT("CommentOpacity"), s_.commentOpacity, iniFileName_.c_str());
	WritePrivateProfileInt(TEXT("Window"), TEXT("HeaderMask"), s_.headerMask, iniFileName_.c_str());
	WritePrivateProfileInt(TEXT("Window"), TEXT("SetRelative"), s_.bSetRelative, iniFileName_.c_str());
}

void CNicoJK::LoadForceListFromIni(const tstring &logfileFolder)
{
	// chatStreamIDをもつチャンネルのみ追加
	forceList_.clear();
	for (size_t i = 0; i < _countof(DEFAULT_JKID_NAME_TABLE); ++i) {
		if (DEFAULT_JKID_NAME_TABLE[i].chatStreamID) {
			FORCE_ELEM e;
			e.jkID = DEFAULT_JKID_NAME_TABLE[i].jkID;
			e.chatStreamID = DEFAULT_JKID_NAME_TABLE[i].chatStreamID;
			e.force = -1;
			e.bFixedName = false;
			forceList_.push_back(e);
		}
	}

	// chatStreamIDの追加の対照表をマージ
	std::vector<TCHAR> buf = GetPrivateProfileSectionBuffer(TEXT("ChatStreams"), iniFileName_.c_str());
	for (LPCTSTR p = buf.data(); *p; p += _tcslen(p) + 1) {
		FORCE_ELEM e;
		e.jkID = _tcstol(p, nullptr, 10);
		if (e.jkID > 0) {
			TCHAR key[16];
			_stprintf_s(key, TEXT("%d"), e.jkID);
			tstring val = GetBufferedProfileToString(buf.data(), key, TEXT("!"));
			if (val != TEXT("!")) {
				for (size_t i = 0; i < val.size(); ++i) {
					if ((TEXT('0') <= val[i] && val[i] <= TEXT('9')) ||
					    (TEXT('A') <= val[i] && val[i] <= TEXT('Z')) ||
					    (TEXT('a') <= val[i] && val[i] <= TEXT('z'))) {
						e.chatStreamID += static_cast<char>(val[i]);
					} else {
						e.chatStreamID.clear();
						TCHAR text[64];
						_stprintf_s(text, TEXT("[ChatStreams]のキー%sの値が不正です。"), key);
						m_pApp->AddLog(text, TVTest::LOG_TYPE_ERROR);
						break;
					}
				}
				e.force = -1;
				e.bFixedName = false;
				// まだなければ追加
				std::vector<FORCE_ELEM>::iterator it = LowerBoundJKID(forceList_.begin(), forceList_.end(), e.jkID);
				if (it == forceList_.end() || it->jkID != e.jkID) {
					if (!e.chatStreamID.empty()) {
						forceList_.insert(it, e);
					}
				} else if (e.chatStreamID.empty()) {
					forceList_.erase(it);
				} else {
					*it = e;
				}
			}
		}
	}

	// ログフォルダにあるチャンネルを追加
	if (!logfileFolder.empty()) {
		EnumFindFile((logfileFolder + TEXT("\\jk*")).c_str(), [this](const WIN32_FIND_DATA &fd) {
			FORCE_ELEM e;
			if (!_tcsnicmp(fd.cFileName, TEXT("jk"), 2) && (e.jkID = _tcstol(&fd.cFileName[2], nullptr, 10)) > 0) {
				e.force = -1;
				e.bFixedName = false;
				// まだなければ追加
				std::vector<FORCE_ELEM>::iterator it = LowerBoundJKID(forceList_.begin(), forceList_.end(), e.jkID);
				if (it == forceList_.end() || it->jkID != e.jkID) {
					forceList_.insert(it, e);
				}
			}
		});
	}

	// とりあえず組み込みのチャンネル名をセットしておく
	for (auto it = forceList_.begin(); it != forceList_.end(); ++it) {
		JKID_NAME_ELEM e;
		e.jkID = it->jkID;
		const JKID_NAME_ELEM *p = std::lower_bound(
			DEFAULT_JKID_NAME_TABLE, DEFAULT_JKID_NAME_TABLE + _countof(DEFAULT_JKID_NAME_TABLE), e,
			[](const JKID_NAME_ELEM &a, const JKID_NAME_ELEM &b) { return a.jkID < b.jkID; });
		if (p && p->jkID == e.jkID) {
			it->name = p->name;
		}
	}

	// チャンネル名が指定されていれば上書き
	buf = GetPrivateProfileSectionBuffer(TEXT("ChannelNames"), iniFileName_.c_str());
	for (LPCTSTR p = buf.data(); *p; p += _tcslen(p) + 1) {
		int jkID = _tcstol(p, nullptr, 10);
		std::vector<FORCE_ELEM>::iterator it = LowerBoundJKID(forceList_.begin(), forceList_.end(), jkID);
		if (it != forceList_.end() && it->jkID == jkID) {
			TCHAR key[16];
			_stprintf_s(key, TEXT("%d"), jkID);
			it->name = GetBufferedProfileToString(buf.data(), key, TEXT(""));
			it->bFixedName = true;
		}
	}
}

void CNicoJK::LoadRplListFromIni(LPCTSTR section, std::vector<RPL_ELEM> *pRplList)
{
	std::vector<TCHAR> buf = GetPrivateProfileSectionBuffer(section, iniFileName_.c_str());
	size_t lastSize = pRplList->size();
	for (LPCTSTR p = buf.data(); *p; p += _tcslen(p) + 1) {
		RPL_ELEM e;
		if (!_tcsnicmp(p, TEXT("Pattern"), 7)) {
			LPTSTR endp;
			e.key = _tcstol(&p[7], &endp, 10);
			if (endp != &p[7]) {
				e.section = section;
				TCHAR key[32];
				_stprintf_s(key, TEXT("Comment%d"), e.key);
				e.comment = GetBufferedProfileToString(buf.data(), key, TEXT(""));
				_stprintf_s(key, TEXT("Pattern%d"), e.key);
				tstring val = GetBufferedProfileToString(buf.data(), key, TEXT(""));
				if (!e.SetPattern(val.c_str())) {
					TCHAR text[64];
					_stprintf_s(text, TEXT("%sの正規表現が異常です。"), key);
					m_pApp->AddLog(text, TVTest::LOG_TYPE_ERROR);
				} else {
					pRplList->push_back(e);
				}
			}
		}
	}
	std::sort(pRplList->begin() + lastSize, pRplList->end(), [](const RPL_ELEM &a, const RPL_ELEM &b) { return a.key < b.key; });
}

void CNicoJK::SaveRplListToIni(LPCTSTR section, const std::vector<RPL_ELEM> &rplList, bool bClearSection)
{
	if (bClearSection) {
		WritePrivateProfileString(section, nullptr, nullptr, iniFileName_.c_str());
	}
	for (auto it = rplList.cbegin(); it != rplList.end(); ++it) {
		if (it->section == section) {
			TCHAR key[32];
			_stprintf_s(key, TEXT("Pattern%d"), it->key);
			WritePrivateProfileString(section, key, it->pattern.c_str(), iniFileName_.c_str());
		}
	}
}

HWND CNicoJK::GetFullscreenWindow()
{
	TVTest::HostInfo hostInfo;
	if (m_pApp->GetFullscreen() && m_pApp->GetHostInfo(&hostInfo)) {
		TCHAR className[64];
		_tcsncpy_s(className, hostInfo.pszAppName, 47);
		_tcscat_s(className, TEXT(" Fullscreen"));

		HWND hwnd = nullptr;
		while ((hwnd = FindWindowEx(nullptr, hwnd, className, nullptr)) != nullptr) {
			DWORD pid;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == GetCurrentProcessId()) {
				return hwnd;
			}
		}
	}
	return nullptr;
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
	std::pair<HWND, LPCTSTR> *params = reinterpret_cast<std::pair<HWND, LPCTSTR>*>(lParam);
	TCHAR className[64];
	if (GetClassName(hwnd, className, _countof(className)) && !_tcscmp(className, params->second)) {
		// 見つかった
		params->first = hwnd;
		return FALSE;
	}
	return TRUE;
}

// TVTestのVideo Containerウィンドウを探す
HWND CNicoJK::FindVideoContainer()
{
	std::pair<HWND, LPCTSTR> params(nullptr, nullptr);
	TVTest::HostInfo hostInfo;
	if (m_pApp->GetHostInfo(&hostInfo)) {
		TCHAR searchName[64];
		_tcsncpy_s(searchName, hostInfo.pszAppName, 31);
		_tcscat_s(searchName, TEXT(" Video Container"));

		params.second = searchName;
		HWND hwndFull = GetFullscreenWindow();
		EnumChildWindows(hwndFull ? hwndFull : m_pApp->GetAppWindow(), EnumWindowsProc, reinterpret_cast<LPARAM>(&params));
	}
	return params.first;
}

// 再生中のストリームのネットワーク/サービスIDを取得する
DWORD CNicoJK::GetCurrentNetworkServiceID()
{
	TVTest::ServiceInfo si;
	int index = m_pApp->GetService();
	if (index >= 0 && m_pApp->GetServiceInfo(index, &si)) {
		TVTest::ChannelInfo ci;
		if (m_pApp->GetCurrentChannelInfo(&ci) && ci.NetworkID) {
			if (0x7880 <= ci.NetworkID && ci.NetworkID <= 0x7FEF) {
				// 地上波のサービス種別とサービス番号はマスクする
				return (static_cast<DWORD>(si.ServiceID&~0x0187) << 16) | 0x000F;
			}
			return (static_cast<DWORD>(si.ServiceID) << 16) | ci.NetworkID;
		}
		// チャンネルスキャンしていないとGetCurrentChannelInfo()もネットワークIDの取得に失敗するよう
		if (si.ServiceID >= 0x0400) {
			// 地上波っぽいのでマスクする
			return (static_cast<DWORD>(si.ServiceID&~0x0187) << 16) | 0;
		}
		return (static_cast<DWORD>(si.ServiceID) << 16) | 0;
	}
	return 0;
}

// 指定チャンネルのネットワーク/サービスIDを取得する
bool CNicoJK::GetChannelNetworkServiceID(int tuningSpace, int channelIndex, DWORD *pNtsID)
{
	TVTest::ChannelInfo ci;
	if (m_pApp->GetChannelInfo(tuningSpace, channelIndex, &ci)) {
		if (ci.NetworkID && ci.ServiceID) {
			if (0x7880 <= ci.NetworkID && ci.NetworkID <= 0x7FEF) {
				// 地上波のサービス種別とサービス番号はマスクする
				*pNtsID = (static_cast<DWORD>(ci.ServiceID&~0x0187) << 16) | 0x000F;
				return true;
			}
			*pNtsID = (static_cast<DWORD>(ci.ServiceID) << 16) | ci.NetworkID;
			return true;
		}
		*pNtsID = 0;
		return true;
	}
	return false;
}

// 再生中のストリームのTOT時刻(取得からの経過時間で補正済み)をUTCで取得する
LONGLONG CNicoJK::GetCurrentTot()
{
	lock_recursive_mutex lock(streamLock_);
	DWORD tick = GetTickCount();
	if (llftTot_ < 0) {
		// TOTを取得できていない
		return -1;
	} else if (tick - pcrTick_ >= 2000) {
		// 2秒以上PCRを取得できていない→ポーズ中?
		return llftTot_ - s_.defaultPlaybackDelay * FILETIME_MILLISECOND;
	} else if (llftTotLast_ < 0) {
		// 再生速度は分からない
		return llftTot_ + (static_cast<int>(tick - totTick_) - s_.defaultPlaybackDelay) * FILETIME_MILLISECOND;
	} else {
		DWORD delta = totTick_ - totTickLast_;
		// 再生速度(10%～1000%)
		LONGLONG speed = !delta ? FILETIME_MILLISECOND : (llftTot_ - llftTotLast_) / delta;
		speed = min(max(speed, FILETIME_MILLISECOND / 10), FILETIME_MILLISECOND * 10);
		return llftTot_ + (static_cast<int>(tick - totTick_) - s_.defaultPlaybackDelay) * speed;
	}
}

// 現在のBonDriverが':'区切りのリストに含まれるかどうか調べる
bool CNicoJK::IsMatchDriverName(LPCTSTR drivers)
{
	std::vector<TCHAR> path(m_pApp->GetDriverName(nullptr, 0) + 1);
	m_pApp->GetDriverName(path.data(), static_cast<int>(path.size()));
	LPCTSTR name = path.data() + _tcslen(path.data());
	size_t len = 0;
	for (; name != path.data() && !_tcschr(TEXT("/\\"), name[-1]); --name, ++len);
	if (len > 0) {
		for (LPCTSTR p = drivers; *p; ++p) {
			if ((p == drivers || p[-1] == TEXT(':')) && !_tcsnicmp(p, name, len) && (!p[len] || p[len] == TEXT(':'))) {
				return true;
			}
		}
	}
	return false;
}

// 指定した実況IDのログファイルに書き込む
// jkIDが負値のときはログファイルを閉じる
void CNicoJK::WriteToLogfile(int jkID, const char *text)
{
	if (s_.logfileFolder.empty() || s_.logfileMode == 0 || s_.logfileMode == 1 && !bRecording_) {
		// ログを記録しない
		jkID = -1;
	}
	if (currentLogfileJK_ >= 0 && currentLogfileJK_ != jkID) {
		// 閉じる
		CloseHandle(hLogfile_);
		CloseHandle(hLogfileLock_);
		// ロックファイルを削除
		TCHAR name[64];
		_stprintf_s(name, TEXT("\\jk%d\\lockfile"), currentLogfileJK_);
		DeleteFile((s_.logfileFolder + name).c_str());
		currentLogfileJK_ = -1;
		OutputMessageLog(TEXT("ログファイルの書き込みを終了しました。"));
	}
	if (currentLogfileJK_ < 0 && jkID >= 0) {
		unsigned int tm;
		TCHAR name[64];
		_stprintf_s(name, TEXT("\\jk%d"), jkID);
		tstring path = s_.logfileFolder + name;
		if (CLogReader::GetChatDate(&tm, text) &&
		    (GetFileAttributes(path.c_str()) != INVALID_FILE_ATTRIBUTES || CreateDirectory(path.c_str(), nullptr))) {
			// ロックファイルを開く
			path += TEXT("\\lockfile");
			hLogfileLock_ = CreateFile(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hLogfileLock_ != INVALID_HANDLE_VALUE) {
				// 開く
				_stprintf_s(name, TEXT("\\jk%d\\%010u.txt"), jkID, tm);
				hLogfile_ = CreateFile((s_.logfileFolder + name).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (hLogfile_ != INVALID_HANDLE_VALUE) {
					// ヘッダを書き込む(別に無くてもいい)
					FILETIME ft, ftUtc = LongLongToFileTime(UnixTimeToFileTime(tm));
					FileTimeToLocalFileTime(&ftUtc, &ft);
					SYSTEMTIME st;
					FileTimeToSystemTime(&ft, &st);
					char header[128];
					int len = sprintf_s(header, "<!-- NicoJK logfile from %04d-%02d-%02dT%02d:%02d:%02d -->\r\n",
					                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
					DWORD written;
					WriteFile(hLogfile_, header, len, &written, nullptr);
					currentLogfileJK_ = jkID;
					OutputMessageLog((tstring(TEXT("ログ\"")) + &name[1] + TEXT("\"の書き込みを開始しました。")).c_str());
				} else {
					CloseHandle(hLogfileLock_);
					DeleteFile(path.c_str());
				}
			}
		}
	}
	// 開いてたら書き込む
	if (currentLogfileJK_ >= 0) {
		DWORD written;
		WriteFile(hLogfile_, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
		WriteFile(hLogfile_, "\r\n", 2, &written, nullptr);
	}
}

static inline int CounterDiff(DWORD a, DWORD b)
{
	return (a - b) & 0x80000000 ? -static_cast<int>(b - a - 1) - 1 : static_cast<int>(a - b);
}

bool CNicoJK::ReadFromLogfile(int jkID, const char **text, unsigned int tmToRead)
{
	return logReader_.Read(jkID, [this](LPCTSTR message) {
		static const TCHAR started[] = TEXT("Started reading logfile: ");
		if (!_tcsncmp(message, started, _tcslen(started))) {
			TCHAR log[256];
			_stprintf_s(log, TEXT("ログ\"%.191s\"の読み込みを開始しました。"), message + _tcslen(started));
			OutputMessageLog(log);
		} else if (!_tcscmp(message, TEXT("Closed logfile."))) {
			OutputMessageLog(TEXT("ログファイルの読み込みを終了しました。"));
		} else {
			OutputMessageLog(message);
		}
	}, text, tmToRead);
}

static int GetWindowHeight(HWND hwnd)
{
	RECT rc;
	return hwnd && GetWindowRect(hwnd, &rc) ? rc.bottom - rc.top : 0;
}

// イベントコールバック関数
// 何かイベントが起きると呼ばれる
LRESULT CALLBACK CNicoJK::EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData)
{
	static_cast<void>(lParam2);
	CNicoJK *pThis = static_cast<CNicoJK*>(pClientData);
	switch (Event) {
	case TVTest::EVENT_PLUGINENABLE:
		// プラグインの有効状態が変化した
		{
			pThis->TogglePlugin(lParam1 != 0);
			// パネルの有効状態は(hForce_の有無にかかわらず)常にTVTestのプラグイン状態フラグに合わせる
			TVTest::PanelItemSetInfo psi;
			psi.Size = sizeof(psi);
			psi.Mask = TVTest::PANEL_ITEM_SET_INFO_MASK_STATE;
			psi.ID = 1;
			psi.StateMask = TVTest::PANEL_ITEM_STATE_ENABLED | (pThis->hPanel_ ? TVTest::PANEL_ITEM_STATE_ACTIVE : 0);
			psi.State = lParam1 ? psi.StateMask : 0;
			pThis->m_pApp->SetPanelItem(&psi);
		}
		return TRUE;
	case TVTest::EVENT_PANELITEM_NOTIFY:
		// パネル項目の通知
		{
			TVTest::PanelItemEventInfo *pei = reinterpret_cast<TVTest::PanelItemEventInfo*>(lParam1);
			switch (pei->Event) {
			case TVTest::PANEL_ITEM_EVENT_CREATE:
				{
					TVTest::PanelItemCreateEventInfo *pcei = reinterpret_cast<TVTest::PanelItemCreateEventInfo*>(lParam1);
					pThis->hPanel_ = CreateWindowEx(0, TEXT("ru.jk.panel"), nullptr, WS_CHILD | WS_VISIBLE,
					                                pcei->ItemRect.left, pcei->ItemRect.top,
					                                pcei->ItemRect.right - pcei->ItemRect.left, pcei->ItemRect.bottom - pcei->ItemRect.top,
					                                pcei->hwndParent, nullptr, g_hinstDLL, pThis);
					if (pThis->hPanel_) {
						pcei->hwndItem = pThis->hPanel_;
						// このイベントはTVTest::EVENT_PLUGINENABLEよりも遅れるため
						pThis->TogglePlugin(pThis->m_pApp->IsPluginEnabled());
						return TRUE;
					}
				}
				return FALSE;
			}
		}
		break;
	case TVTest::EVENT_COLORCHANGE:
		// 色の設定が変化した
		if (pThis->hPanel_ && pThis->hForce_) {
			DeleteBrush(SetClassLongPtr(pThis->hForce_, GCLP_HBRBACKGROUND,
				reinterpret_cast<LONG_PTR>(CreateSolidBrush(pThis->m_pApp->GetColor(L"ControlPanelMargin")))));
			InvalidateRect(pThis->hForce_, nullptr, TRUE);
		}
		break;
	case TVTest::EVENT_RECORDSTATUSCHANGE:
		// 録画状態が変化した
		pThis->bRecording_ = lParam1 != TVTest::RECORD_STATUS_NOTRECORDING;
		break;
	case TVTest::EVENT_FULLSCREENCHANGE:
		// 全画面表示状態が変化した
		if (pThis->hForce_) {
			// オーナーが変わるのでコメントウィンドウを作りなおす
			pThis->commentWindow_.Destroy();
			if (pThis->commentWindow_.GetOpacity() != 0 && pThis->m_pApp->GetPreview()) {
				HWND hwnd = pThis->FindVideoContainer();
				pThis->commentWindow_.Create(hwnd);
				pThis->bHalfSkip_ = GetWindowHeight(hwnd) >= pThis->s_.halfSkipThreshold;
			}
			// 全画面遷移時は隠れたほうが使い勝手がいいので呼ばない
			if (!lParam1) {
				SendMessage(pThis->hForce_, WM_SET_ZORDER, 0, 0);
			}
		}
		break;
	case TVTest::EVENT_PREVIEWCHANGE:
		// プレビュー表示状態が変化した
		if (pThis->hForce_) {
			if (pThis->commentWindow_.GetOpacity() != 0 && lParam1 != 0) {
				HWND hwnd = pThis->FindVideoContainer();
				pThis->commentWindow_.Create(hwnd);
				pThis->bHalfSkip_ = GetWindowHeight(hwnd) >= pThis->s_.halfSkipThreshold;
				pThis->ProcessChatTag("<!--<chat date=\"0\" mail=\"cyan ue\" user_id=\"-\">(NicoJK ON)</chat>-->");
			} else {
				pThis->commentWindow_.Destroy();
			}
		}
		break;
	case TVTest::EVENT_DRIVERCHANGE:
		// ドライバが変更された
		if (pThis->hForce_) {
			pThis->bUsingLogfileDriver_ = pThis->IsMatchDriverName(pThis->s_.logfileDrivers.c_str());
		}
		// FALL THROUGH!
	case TVTest::EVENT_CHANNELCHANGE:
		// チャンネルが変更された
		if (pThis->hForce_) {
			PostMessage(pThis->hForce_, WM_RESET_STREAM, 0, 0);
		}
		// FALL THROUGH!
	case TVTest::EVENT_SERVICECHANGE:
		// サービスが変更された
		if (pThis->hForce_) {
			// 重複やザッピング対策のためタイマで呼ぶ
			SetTimer(pThis->hForce_, TIMER_SETUP_CURJK, SETUP_CURJK_DELAY, nullptr);
		}
		break;
	case TVTest::EVENT_SERVICEUPDATE:
		// サービスの構成が変化した(再生ファイルを切り替えたときなど)
		if (pThis->hForce_) {
			// ユーザの自発的なチャンネル変更(EVENT_CHANNELCHANGE)を捉えるのが原則だが
			// 非チューナ系のBonDriverだとこれでは不十分なため
			if (pThis->IsMatchDriverName(pThis->s_.nonTunerDrivers.c_str())) {
				SetTimer(pThis->hForce_, TIMER_SETUP_CURJK, SETUP_CURJK_DELAY, nullptr);
			}
		}
		break;
	case TVTest::EVENT_COMMAND:
		// コマンドが選択された
		if (pThis->hForce_) {
			switch (lParam1) {
			case COMMAND_HIDE_FORCE:
				if (IsWindowVisible(pThis->hForce_)) {
					ShowWindow(pThis->hForce_, SW_HIDE);
				} else {
					ShowWindow(pThis->hForce_, SW_SHOWNA);
				}
				SendMessage(pThis->hForce_, WM_UPDATE_LIST, TRUE, 0);
				SendMessage(pThis->hForce_, WM_SET_ZORDER, 0, 0);
				PostMessage(pThis->hForce_, WM_TIMER, TIMER_UPDATE, 0);
				break;
			case COMMAND_HIDE_COMMENT:
					if (pThis->commentWindow_.GetOpacity() == 0 && pThis->m_pApp->GetPreview()) {
						pThis->commentWindow_.ClearChat();
						HWND hwnd = pThis->FindVideoContainer();
						pThis->commentWindow_.Create(hwnd);
						pThis->bHalfSkip_ = GetWindowHeight(hwnd) >= pThis->s_.halfSkipThreshold;
						pThis->commentWindow_.AddChat(TEXT("(Comment ON)"), RGB(0x00,0xFF,0xFF), CCommentWindow::CHAT_POS_UE);
						// 非表示前の不透明度を復元する
						BYTE newOpacity = static_cast<BYTE>(pThis->s_.commentOpacity>>8);
						pThis->commentWindow_.SetOpacity(newOpacity == 0 ? 255 : newOpacity);
						pThis->m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, TVTest::COMMAND_ICON_STATE_CHECKED);
					} else {
						pThis->commentWindow_.Destroy();
						// 8-15bitに非表示前の不透明度を記憶しておく
						pThis->s_.commentOpacity = (pThis->s_.commentOpacity&~0xFF00) | (pThis->commentWindow_.GetOpacity()<<8);
						pThis->commentWindow_.SetOpacity(0);
						pThis->m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, 0);
					}
					SendDlgItemMessage(pThis->hForce_, IDC_SLIDER_OPACITY, TBM_SETPOS, TRUE, (pThis->commentWindow_.GetOpacity() * 10 + 254) / 255);
				break;
			default:
				if (COMMAND_FORWARD_A <= lParam1 && lParam1 < COMMAND_FORWARD_A + _countof(pThis->s_.forwardList)) {
					int forward = pThis->s_.forwardList[lParam1 - COMMAND_FORWARD_A];
					if (forward == 0) {
						pThis->forwardOffsetDelta_ = -pThis->forwardOffset_;
					} else {
						pThis->forwardOffsetDelta_ += forward;
					}
				}
				break;
			}
		}
		break;
	case TVTest::EVENT_FILTERGRAPH_INITIALIZED:
		// フィルタグラフの初期化終了
		pThis->commentWindow_.OnFilterGraphInitialized(reinterpret_cast<const TVTest::FilterGraphInfo*>(lParam1)->pGraphBuilder);
		break;
	case TVTest::EVENT_FILTERGRAPH_FINALIZE:
		// フィルタグラフの終了処理開始
		pThis->commentWindow_.OnFilterGraphFinalize(reinterpret_cast<const TVTest::FilterGraphInfo*>(lParam1)->pGraphBuilder);
		break;
	}
	return 0;
}

BOOL CALLBACK CNicoJK::WindowMsgCallback(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *pResult, void *pUserData)
{
	static_cast<void>(hwnd);
	static_cast<void>(lParam);
	static_cast<void>(pResult);
	CNicoJK *pThis = static_cast<CNicoJK*>(pUserData);
	switch (uMsg) {
	case WM_ACTIVATE:
		if (LOWORD(wParam) != WA_INACTIVE) {
			SendMessage(pThis->hForce_, WM_SET_ZORDER, 0, 0);
		}
		break;
	case WM_WINDOWPOSCHANGED:
		// WM_ACTIVATEされないZオーダーの変化を捉える。フルスクリーンでもなぜか送られてくるので注意
		SetTimer(pThis->hForce_, TIMER_DONE_POSCHANGE, 1000, nullptr);
		break;
	case WM_MOVE:
		pThis->commentWindow_.OnParentMove();
		// 実際に捉えたいVideo Containerウィンドウの変化はすこし遅れるため
		SetTimer(pThis->hForce_, TIMER_DONE_MOVE, 500, nullptr);
		break;
	case WM_SIZE:
		pThis->commentWindow_.OnParentSize();
		SetTimer(pThis->hForce_, TIMER_DONE_SIZE, 500, nullptr);
		break;
	case WM_DROPFILES:
		if (pThis->s_.dropLogfileMode == 0) {
			break;
		}
		if (pThis->m_pApp->GetFullscreen()) {
			// ファイルダイアログ等でのD&Dを無視するため(確実ではない)
			HWND hwndActive = GetActiveWindow();
			if (hwndActive && (GetWindowLong(GetAncestor(hwndActive, GA_ROOT), GWL_EXSTYLE) & WS_EX_DLGMODALFRAME) != 0) {
				break;
			}
		}
		// 読み込み可能な拡張子をもつ最初にみつかったファイルを開く
		pThis->dropFileTimeout_ = 0;
		for (UINT i = DragQueryFile(reinterpret_cast<HDROP>(wParam), 0xFFFFFFFF, nullptr, 0); i != 0; --i) {
			std::vector<TCHAR> buf(DragQueryFile(reinterpret_cast<HDROP>(wParam), i - 1, nullptr, 0) + 1);
			if (DragQueryFile(reinterpret_cast<HDROP>(wParam), i - 1, buf.data(), (UINT)buf.size())) {
				pThis->dropFileName_ = buf.data();
				if (pThis->dropFileName_.size() >= 5 && !_tcschr(TEXT("/\\"), *(pThis->dropFileName_.end() - 5)) &&
				    (!_tcsicmp(&pThis->dropFileName_.c_str()[pThis->dropFileName_.size() - 4], TEXT(".jkl")) ||
				     !_tcsicmp(&pThis->dropFileName_.c_str()[pThis->dropFileName_.size() - 4], TEXT(".xml")) ||
				     !_tcsicmp(&pThis->dropFileName_.c_str()[pThis->dropFileName_.size() - 4], TEXT(".txt")))) {
					if (pThis->bSpecFile_) {
						pThis->ReadFromLogfile(-1);
						DeleteFile(pThis->tmpSpecFileName_.c_str());
						pThis->bSpecFile_ = false;
					}
					SendDlgItemMessage(pThis->hForce_, IDC_CHECK_SPECFILE, BM_SETCHECK, BST_UNCHECKED, 0);
					if (pThis->s_.dropLogfileMode == 2) {
						// ウィンドウの左右どちらにD&DされたかでRelチェックボックスを変える
						RECT rc;
						HWND hwndFull = pThis->GetFullscreenWindow();
						GetClientRect(hwndFull ? hwndFull : pThis->m_pApp->GetAppWindow(), &rc);
						POINT pt = {};
						DragQueryPoint(reinterpret_cast<HDROP>(wParam), &pt);
						SendDlgItemMessage(pThis->hForce_, IDC_CHECK_RELATIVE, BM_SETCHECK, pt.x > rc.right / 2 ? BST_CHECKED : BST_UNCHECKED, 0);
					}
					bool bRel = SendDlgItemMessage(pThis->hForce_, IDC_CHECK_RELATIVE, BM_GETCHECK, 0, 0) == BST_CHECKED;
					pThis->dropFileTimeout_ = 10;
					SetTimer(pThis->hForce_, TIMER_OPEN_DROPFILE, bRel ? 2000 : 0, nullptr);
					break;
				}
			}
		}
		// DragFinish()せずに本体のデフォルトプロシージャに任せる
		break;
	}
	return FALSE;
}

// コメント(chatタグ)1行を解釈してコメントウィンドウに送る
bool CNicoJK::ProcessChatTag(const char *tag, bool bShow, int showDelay)
{
	static const std::regex reChat("<chat(?= )(.*)>(.*?)</chat>");
	static const std::regex reMail(" mail=\"(.*?)\"");
	static const std::regex reAbone(" abone=\"1\"");
	static const std::regex reYourpost(" yourpost=\"1\"");
	static const std::regex reInsertAt(" insert_at=\"last\"");
	static const std::regex reAlign(" align=\"(left|right)");
	static const std::regex reUserID(" user_id=\"([0-9A-Za-z\\-_]{0,27})");
	static const std::regex reNo(" no=\"(\\d+)\"");
	static const std::regex reLogcmd(" logcmd=\"(.*?)\"");
	// 置換
	std::string rpl[2];
	if (!rplList_.empty()) {
		rpl[1] = tag;
		int i = 0;
		for (auto it = rplList_.cbegin(); it != rplList_.end(); ++it) {
			if (it->IsEnabled()) {
				try {
					rpl[i % 2] = std::regex_replace(rpl[(i + 1) % 2], it->re, it->fmt);
				} catch (std::regex_error&) {
					// 置換フォーマット異常のため無視する
					continue;
				}
				tag = rpl[i++ % 2].c_str();
			}
		}
	}
	std::cmatch m, mm;
	unsigned int tm;
	if (std::regex_match(tag, m, reChat) && CLogReader::GetChatDate(&tm, tag)) {
		TCHAR text[CHAT_TEXT_MAX];
		int len = MultiByteToWideChar(CP_UTF8, 0, m[2].first, static_cast<int>(m[2].length()), text, _countof(text) - 1);
		text[len] = TEXT('\0');
		DecodeEntityReference(text);
		// mail属性は無いときもある
		char mail[256];
		mail[0] = '\0';
		if (std::regex_search(m[1].first, m[1].second, mm, reMail)) {
			strncpy_s(mail, mm[1].first, min(static_cast<size_t>(mm[1].length()), _countof(mail) - 1));
		}
		// abone属性(ローカル拡張)
		bool bAbone = std::regex_search(m[1].first, m[1].second, reAbone);
		if (bShow && !bAbone) {
			bool bYourpost = std::regex_search(m[1].first, m[1].second, reYourpost);
			// insert_at属性(ローカル拡張)
			bool bInsertLast = std::regex_search(m[1].first, m[1].second, reInsertAt);
			// align属性(ローカル拡張)
			CCommentWindow::CHAT_ALIGN align = CCommentWindow::CHAT_ALIGN_CENTER;
			if (std::regex_search(m[1].first, m[1].second, mm, reAlign)) {
				align = mm[1].first[0] == 'l' ? CCommentWindow::CHAT_ALIGN_LEFT : CCommentWindow::CHAT_ALIGN_RIGHT;
			}
			commentWindow_.AddChat(text, GetColor(mail), HasToken(mail, "shita") ? CCommentWindow::CHAT_POS_SHITA :
			                       HasToken(mail, "ue") ? CCommentWindow::CHAT_POS_UE : CCommentWindow::CHAT_POS_DEFAULT,
			                       HasToken(mail, "small") ? CCommentWindow::CHAT_SIZE_SMALL : CCommentWindow::CHAT_SIZE_DEFAULT,
			                       align, bInsertLast, bYourpost ? 160 : 0, showDelay);
		}

		// リストボックスのログ表示キューに追加
		LOG_ELEM e;
		FILETIME ft, ftUtc = LongLongToFileTime(UnixTimeToFileTime(tm));
		FileTimeToLocalFileTime(&ftUtc, &ft);
		FileTimeToSystemTime(&ft, &e.st);
		e.no = 0;
		e.cr = RGB(0xFF, 0xFF, 0xFF);
		e.marker[0] = TEXT('\0');
		if (!bShow) {
			_tcscpy_s(e.marker, TEXT("."));
		} else if (std::regex_search(m[1].first, m[1].second, mm, reUserID)) {
			len = MultiByteToWideChar(CP_UTF8, 0, mm[1].first, static_cast<int>(mm[1].length()), e.marker, _countof(e.marker) - 1);
			e.marker[len] = TEXT('\0');
			if (std::regex_search(m[1].first, m[1].second, mm, reNo)) {
				e.no = strtol(mm[1].first, nullptr, 10);
			}
			// logcmd属性(ローカル拡張)
			char logcmd[256];
			if (std::regex_search(m[1].first, m[1].second, mm, reLogcmd)) {
				strncpy_s(logcmd, mm[1].first, min(static_cast<size_t>(mm[1].length()), _countof(logcmd) - 1));
				e.cr = GetColor(logcmd);
			}
		}
		if (bAbone) {
			e.text = s_.abone;
			// 末尾の'&'は元コメントに置き換える (TODO: 末尾以外にも対応したほうがいいかも)
			if (!e.text.empty() && e.text.back() == TEXT('&')) {
				e.text.replace(e.text.size() - 1, 1, text);
			}
		} else {
			e.text = text;
		}
		logList_.push_back(std::move(e));
		return true;
	}
	return false;
}

// ログウィンドウにユーザへのメッセージログを出す
void CNicoJK::OutputMessageLog(LPCTSTR text)
{
	// リストボックスのログ表示キューに追加
	LOG_ELEM e;
	GetLocalTime(&e.st);
	e.no = 0;
	e.cr = RGB(0xFF, 0xFF, 0xFF);
	_tcscpy_s(e.marker, TEXT("#"));
	e.text = text;
	logList_.push_back(std::move(e));
	if (hForce_) {
		SendMessage(hForce_, WM_UPDATE_LIST, FALSE, 0);
	}
}

// コメント投稿欄の文字列を取得する
void CNicoJK::GetPostComboBoxText(LPTSTR comm, size_t commSize, LPTSTR mail, size_t mailSize)
{
	TCHAR text[512];
	if (!GetDlgItemText(hForce_, IDC_CB_POST, text, _countof(text))) {
		text[0] = TEXT('\0');
	}
	if (mail) {
		mail[0] = TEXT('\0');
	}
	// []で囲われた部分はmail属性値とする
	size_t i = 0;
	if (text[0] == TEXT('[')) {
		i = _tcscspn(text, TEXT("]"));
		if (text[i] == TEXT(']')) {
			if (mail) {
				_tcsncpy_s(mail, mailSize, &text[1], min(i - 1, mailSize - 1));
			}
			++i;
		}
	}
	_tcsncpy_s(comm, commSize, &text[i], _TRUNCATE);
}

// コメント投稿欄のローカルコマンドを処理する
void CNicoJK::ProcessLocalPost(LPCTSTR comm)
{
	// パラメータ分割
	TCHAR cmd[16];
	size_t cmdLen = _tcscspn(comm, TEXT(" "));
	_tcsncpy_s(cmd, comm, min(cmdLen, _countof(cmd) - 1));
	LPCTSTR arg = &comm[cmdLen] + _tcsspn(&comm[cmdLen], TEXT(" "));
	LPTSTR endp;
	LONGLONG llArg = _tcstoi64(arg, &endp, 10);
	if (endp == arg) {
		llArg = LLONG_MAX;
	}
	int nArg = static_cast<int>(min<LONGLONG>(max<LONGLONG>(llArg, INT_MIN), INT_MAX));
	if (!_tcsicmp(cmd, TEXT("help"))) {
		static const TCHAR text[] =
			TEXT("@help\tヘルプを表示")
			TEXT("\n@fopa N\t勢い窓の透過レベル1～10(Nを省略すると10)。")
			TEXT("\n@mask N\tログの時間(ID)部の省略マスク(Nを省略すると0)。")
			TEXT("\n@opa N\tコメントの透過レベル0～10(Nを省略すると10)。")
			TEXT("\n@fwd N\tコメントをNミリ秒だけ前進")
			TEXT("\n@fwds N\tコメントをN秒だけ前進")
			TEXT("\n@jmp yyMMddHHmm\tコメントを指定の年月日時分に移動")
			TEXT("\n@size N\tコメントの文字サイズをN%にする(Nを省略すると100%)。")
			TEXT("\n@speed N\tコメントの速度をN%にする(Nを省略すると100%)。")
			TEXT("\n@rl\t置換リストのすべてのCommentをリストする")
			TEXT("\n@rr\t置換リストを設定ファイルから再読み込みする")
			TEXT("\n@ra N\tPatternN0～N9を有効にする")
			TEXT("\n@rm N\tPatternN0～N9を無効にする")
			TEXT("\n@debug N\tデバッグ0～15");
		MessageBox(hForce_, text, TEXT("NicoJK - ローカルコマンド"), MB_OK);
	} else if (!_tcsicmp(cmd, TEXT("fopa"))) {
		s_.forceOpacity = 0 < nArg && nArg < 10 ? nArg * 255 / 10 : 255;
		LONG style = GetWindowLong(hForce_, GWL_EXSTYLE);
		SetWindowLong(hForce_, GWL_EXSTYLE, s_.forceOpacity == 255 ? style & ~WS_EX_LAYERED : style | WS_EX_LAYERED);
		SetLayeredWindowAttributes(hForce_, 0, static_cast<BYTE>(s_.forceOpacity), LWA_ALPHA);
	} else if (!_tcsicmp(cmd, TEXT("mask"))) {
		s_.headerMask = 0 < nArg && nArg < INT_MAX ? nArg : 0;
		TCHAR text[64];
		_stprintf_s(text, TEXT("現在の省略マスクは%d(0x%04x)です。"), s_.headerMask, s_.headerMask);
		OutputMessageLog(text);
	} else if (!_tcsicmp(cmd, TEXT("opa"))) {
		int opa = 0 <= nArg && nArg < 10 ? nArg : 10;
		SendDlgItemMessage(hForce_, IDC_SLIDER_OPACITY, TBM_SETPOS, TRUE, opa);
		SendMessage(hForce_, WM_HSCROLL, MAKEWPARAM(SB_THUMBTRACK, opa), reinterpret_cast<LPARAM>(GetDlgItem(hForce_, IDC_SLIDER_OPACITY)));
		TCHAR text[64];
		_stprintf_s(text, TEXT("現在の透過レベルは%dです。"), opa);
		OutputMessageLog(text);
	} else if ((!_tcsicmp(cmd, TEXT("fwd")) || !_tcsicmp(cmd, TEXT("fwds"))) && nArg != INT_MAX) {
		if (nArg == 0) {
			forwardOffsetDelta_ = -forwardOffset_;
		} else {
			forwardOffsetDelta_ += cmd[3] ? nArg * 1000LL : nArg;
		}
	} else if (!_tcsicmp(cmd, TEXT("jmp")) && llArg == LLONG_MAX) {
		// リセット
		forwardOffsetDelta_ = -forwardOffset_;
	} else if (!_tcsicmp(cmd, TEXT("jmp")) && 0 <= llArg && llArg < 10000000000) {
		LONGLONG llft = GetCurrentTot();
		FILETIME ft = LongLongToFileTime(llft);
		SYSTEMTIME st;
		if (llft >= 0 && FileTimeToSystemTime(&ft, &st)) {
			// 年の上位2桁を適当に補う
			LONGLONG adjustYear = st.wYear / 100 * 100 + llArg / 100000000;
			adjustYear += adjustYear - st.wYear > 50 ? -100 : adjustYear - st.wYear < -50 ? 100 : 0;
			st.wYear = static_cast<WORD>(adjustYear);
			st.wMonth = static_cast<WORD>(llArg / 1000000 % 100);
			st.wDay = static_cast<WORD>(llArg / 10000 % 100);
			st.wHour = static_cast<WORD>(llArg / 100 % 100);
			st.wMinute = static_cast<WORD>(llArg % 100);
			st.wSecond = 0;
			st.wMilliseconds = 0;
			FILETIME ftUtc;
			if (SystemTimeToFileTime(&st, &ft) && LocalFileTimeToFileTime(&ft, &ftUtc)) {
				forwardOffsetDelta_ = FileTimeToLongLong(ftUtc) / FILETIME_MILLISECOND - llft / FILETIME_MILLISECOND - forwardOffset_;
			} else {
				OutputMessageLog(TEXT("Error:引数の日時が不正です。"));
			}
		} else {
			OutputMessageLog(TEXT("Error:ストリームの時刻が不明です。"));
		}
	} else if (!_tcsicmp(cmd, TEXT("size"))) {
		int rate = min(max(nArg == INT_MAX ? 100 : nArg, 10), 1000);
		commentWindow_.SetCommentSize(s_.commentSize * rate / 100, s_.commentSizeMin, s_.commentSizeMax, s_.commentLineMargin);
		TCHAR text[64];
		_stprintf_s(text, TEXT("現在のコメントの文字サイズは%d%%です。"), rate);
		OutputMessageLog(text);
	} else if (!_tcsicmp(cmd, TEXT("speed"))) {
		commentWindow_.SetDisplayDuration(s_.commentDuration * 100 / (nArg <= 0 || nArg == INT_MAX ? 100 : nArg));
		TCHAR text[64];
		_stprintf_s(text, TEXT("現在のコメントの表示期間は%dmsecです。"), commentWindow_.GetDisplayDuration());
		OutputMessageLog(text);
	} else if (!_tcsicmp(cmd, TEXT("rl"))) {
		tstring text;
		for (auto it = rplList_.cbegin(); it != rplList_.end(); ++it) {
			if (!it->comment.empty() && it->section == TEXT("CustomReplace")) {
				TCHAR key[64];
				_stprintf_s(key, TEXT("%sPattern%d="), it->IsEnabled() ? TEXT("") : TEXT("#"), it->key);
				text += key + it->comment + TEXT('\n');
			}
		}
		MessageBox(hForce_, text.c_str(), TEXT("NicoJK - ローカルコマンド"), MB_OK);
	} else if (!_tcsicmp(cmd, TEXT("rr"))) {
		rplList_.clear();
		LoadRplListFromIni(TEXT("AutoReplace"), &rplList_);
		LoadRplListFromIni(TEXT("CustomReplace"), &rplList_);
		OutputMessageLog(TEXT("置換リストを再読み込みしました。"));
	} else if (!_tcsicmp(cmd, TEXT("ra")) || !_tcsicmp(cmd, TEXT("rm"))) {
		bool bFound = false;
		for (auto it = rplList_.begin(); it != rplList_.end(); ++it) {
			if (it->key / 10 == nArg && it->section == TEXT("CustomReplace")) {
				bFound = true;
				it->SetEnabled(cmd[1] == TEXT('a'));
				TCHAR key[64];
				_stprintf_s(key, TEXT("Pattern%d("), it->key);
				OutputMessageLog((key + it->comment + TEXT(")を") + (it->IsEnabled() ? TEXT('有') : TEXT('無')) + TEXT("効にしました。")).c_str());
			}
		}
		if (bFound) {
			SaveRplListToIni(TEXT("CustomReplace"), rplList_, false);
		} else {
			OutputMessageLog(TEXT("Error:パターンが見つかりません。"));
		}
	} else if (!_tcsicmp(cmd, TEXT("debug"))) {
		commentWindow_.SetDebugFlags(nArg);
	} else {
		OutputMessageLog(TEXT("Error:不明なローカルコマンドです。"));
	}
}

// サブクラス化した勢いリストのプロシージャ
static LRESULT CALLBACK ForceListBoxProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_ERASEBKGND:
		{
			int n = ListBox_GetCount(hwnd);
			RECT rcLast;
			if (n > 0 && ListBox_GetItemRect(hwnd, n - 1, &rcLast) != LB_ERR) {
				// 背景消去範囲を限定することでちらつきを抑える
				RECT rc;
				GetClientRect(hwnd, &rc);
				if (rc.bottom > rcLast.bottom) {
					rc.top = rcLast.bottom;
					FillRect(reinterpret_cast<HDC>(wParam), &rc, reinterpret_cast<HBRUSH>(GetClassLongPtr(hwnd, GCLP_HBRBACKGROUND)));
				}
				return TRUE;
			}
		}
		break;
	case WM_GETDLGCODE:
		// 本体のアクセラレータを抑制するため
		return DLGC_WANTARROWS;
	}
	return CallWindowProc(reinterpret_cast<WNDPROC>(GetProp(hwnd, TEXT("DefProc"))), hwnd, uMsg, wParam, lParam);
}

// サブクラス化した投稿欄のプロシージャ
static LRESULT CALLBACK ForcePostEditBoxProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_CHAR:
		if (wParam == VK_RETURN) {
			// 改行入力
			if ((lParam & 0x40000000) == 0) {
				SendMessage(static_cast<HWND>(GetProp(hwnd, TEXT("Root"))), WM_POST_COMMENT, 0, 0);
			}
			return 0;
		} else if (wParam == 0x16) {
			// Ctrl+'V'入力
			HWND hwndRoot = static_cast<HWND>(GetProp(hwnd, TEXT("Root")));
			int len = GetWindowTextLength(GetDlgItem(hwndRoot, IDC_CB_POST));
			LONG selRange = static_cast<LONG>(SendDlgItemMessage(hwndRoot, IDC_CB_POST, CB_GETEDITSEL, 0, 0));
			// 入力欄が空になるときだけ処理
			if (len == 0 || MAKELONG(0, len) == selRange) {
				// クリップボードを取得
				TCHAR clip[512];
				clip[0] = TEXT('\0');
				if (OpenClipboard(nullptr)) {
					HGLOBAL hg = GetClipboardData(CF_UNICODETEXT);
					if (hg) {
						LPWSTR pg = static_cast<LPWSTR>(GlobalLock(hg));
						if (pg) {
							_tcsncpy_s(clip, pg, _TRUNCATE);
							GlobalUnlock(hg);
						}
					}
					CloseClipboard();
				}
				// 改行->レコードセパレータ
				LPTSTR q = clip;
				bool bLF = false;
				bool bMultiLine = false;
				for (LPCTSTR p = q; *p; ++p) {
					if (*p == TEXT('\n')) {
						*q++ = TEXT('\x1e');
						bLF = true;
					} else if (*p != TEXT('\r')) {
						*q++ = *p;
						bMultiLine = bLF;
					}
				}
				*q = TEXT('\0');
				// 複数行のペーストだけ独自に処理
				if (bMultiLine) {
					SetDlgItemText(hwndRoot, IDC_CB_POST, clip);
					SendMessage(hwndRoot, WM_COMMAND, MAKEWPARAM(IDC_CB_POST, CBN_EDITCHANGE), 0);
					return 0;
				}
			}
		}
		break;
	case WM_GETDLGCODE:
		// 本体のアクセラレータを抑制するため
		return DLGC_WANTALLKEYS;
	}
	return CallWindowProc(reinterpret_cast<WNDPROC>(GetProp(hwnd, TEXT("DefProc"))), hwnd, uMsg, wParam, lParam);
}

// サブクラス化したボタンのプロシージャ
static LRESULT CALLBACK TVTestPanelButtonProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_PAINT:
		{
			TVTest::CTVTestApp *pApp = reinterpret_cast<TVTest::CTVTestApp*>(GetProp(hwnd, TEXT("App")));
			int state = Button_GetState(hwnd);
			LPCWSTR style = state & BST_HOT ? L"control-panel.item.hot" : state & BST_CHECKED ? L"control-panel.item.checked" : L"control-panel.item";
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			RECT rc;
			GetClientRect(hwnd, &rc);
			pApp->ThemeDrawBackground(style, hdc, rc);
			TCHAR text[256];
			if (GetWindowText(hwnd, text, _countof(text))) {
				HFONT hFont = reinterpret_cast<HFONT>(SendMessage(hwnd, WM_GETFONT, 0, 0));
				HFONT hFontOld = nullptr;
				if (hFont) {
					hFontOld = SelectFont(hdc, hFont);
				}
				int oldBkMode = SetBkMode(hdc, TRANSPARENT);
				pApp->ThemeDrawText(style, hdc, text, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
				SetBkMode(hdc, oldBkMode);
				if (hFont) {
					SelectFont(hdc, hFontOld);
				}
			}
			EndPaint(hwnd, &ps);
		}
		return 0;
	}
	return CallWindowProc(reinterpret_cast<WNDPROC>(GetProp(hwnd, TEXT("DefProc"))), hwnd, uMsg, wParam, lParam);
}

static void SetTVTestPanelItem(HWND hButton, TVTest::CTVTestApp *pApp, LRESULT (CALLBACK *pProc)(HWND, UINT, WPARAM, LPARAM))
{
	SetProp(hButton, TEXT("App"), pApp);
	SetProp(hButton, TEXT("DefProc"), reinterpret_cast<HANDLE>(GetWindowLongPtr(hButton, GWLP_WNDPROC)));
	SetWindowLongPtr(hButton, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pProc));
}

static void ResetTVTestPanelItem(HWND hButton)
{
	SetWindowLongPtr(hButton, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GetProp(hButton, TEXT("DefProc"))));
	RemoveProp(hButton, TEXT("DefProc"));
	RemoveProp(hButton, TEXT("App"));
}

LRESULT CALLBACK CNicoJK::PanelWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_CREATE) {
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams));
	}
	CNicoJK *pThis = reinterpret_cast<CNicoJK*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (pThis) {
		switch (uMsg) {
		case WM_CREATE:
			SetProp(hwnd, TEXT("IsHide"), reinterpret_cast<HANDLE>('N'));
			SetTimer(hwnd, 1, 5000, nullptr);
			return TRUE;
		case WM_DESTROY:
			RemoveProp(hwnd, TEXT("IsHide"));
			pThis->hPanel_ = nullptr;
			break;
		case WM_TIMER:
			if (wParam == 1) {
				// パネルの表示非表示を捕捉する手段が思いつかないので苦肉の策
				bool bHide = pThis->hForce_ && !IsWindowVisible(pThis->hForce_);
				if (!bHide && GetProp(hwnd, TEXT("IsHide")) == reinterpret_cast<HANDLE>('Y')) {
					SetProp(hwnd, TEXT("IsHide"), reinterpret_cast<HANDLE>('N'));
					if (pThis->hForce_) {
						SendMessage(pThis->hForce_, WM_UPDATE_LIST, TRUE, 0);
						PostMessage(pThis->hForce_, WM_TIMER, TIMER_UPDATE, 0);
					}
				} else if (bHide && GetProp(hwnd, TEXT("IsHide")) == reinterpret_cast<HANDLE>('N')) {
					SetProp(hwnd, TEXT("IsHide"), reinterpret_cast<HANDLE>('Y'));
				}
			}
			break;
		case WM_SIZE:
			if (pThis->hForce_) {
				MoveWindow(pThis->hForce_, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
			}
			break;
		default:
			PostMessage(hwnd, WM_TIMER, 1, 0);
			break;
		}
	}
	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK CNicoJK::ForceWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_CREATE) {
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams));
	}
	CNicoJK *pThis = reinterpret_cast<CNicoJK*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	return pThis ? pThis->ForceWindowProcMain(hwnd, uMsg, wParam, lParam) : DefWindowProc(hwnd, uMsg, wParam, lParam);
}

bool CNicoJK::CreateForceWindowItems(HWND hwnd)
{
	int padding = hPanel_ ? 0 : 4;
	if (CreateWindowEx(0, TEXT("BUTTON"), TEXT("勢い"), WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
	        padding, padding, 60, 24, hwnd, reinterpret_cast<HMENU>(IDC_RADIO_FORCE), g_hinstDLL, nullptr) &&
	    CreateWindowEx(0, TEXT("BUTTON"), TEXT("ログ"), WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
	        padding + 60, padding, 60, 24, hwnd, reinterpret_cast<HMENU>(IDC_RADIO_LOG), g_hinstDLL, nullptr) &&
	    CreateWindowEx(0, TEXT("BUTTON"), TEXT("File"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
	        padding + 124, padding + 4, 50, 16, hwnd, reinterpret_cast<HMENU>(IDC_CHECK_SPECFILE), g_hinstDLL, nullptr) &&
	    CreateWindowEx(0, TEXT("BUTTON"), TEXT("Rel"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
	        padding + 174, padding + 4, 50, 16, hwnd, reinterpret_cast<HMENU>(IDC_CHECK_RELATIVE), g_hinstDLL, nullptr) &&
	    // TODO: (描画がとても面倒なので)スライダーはパネルでは表示しない
	    CreateWindowEx(0, TRACKBAR_CLASS, TEXT("不透明度"), WS_CHILD | WS_VISIBLE | TBS_BOTH | TBS_NOTICKS | TBS_TOOLTIPS,
	        padding + 224, hPanel_ ? -100 : padding + 4, 64, 21, hwnd, reinterpret_cast<HMENU>(IDC_SLIDER_OPACITY), g_hinstDLL, nullptr) &&
	    CreateWindowEx(WS_EX_ACCEPTFILES, TEXT("LISTBOX"), nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOTIFY,
	        padding, padding + 24, 100, 100, hwnd, reinterpret_cast<HMENU>(IDC_FORCELIST), g_hinstDLL, nullptr) &&
	    CreateWindowEx(0, TEXT("COMBOBOX"), nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_HASSTRINGS,
	        padding, padding + 124, 100, 50, hwnd, reinterpret_cast<HMENU>(IDC_CB_POST), g_hinstDLL, nullptr))
	{
		if (hForceFont_) {
			SendDlgItemMessage(hwnd, IDC_RADIO_FORCE, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_RADIO_LOG, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_FORCELIST, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_CB_POST, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
		}
		return true;
	}
	return false;
}

LRESULT CNicoJK::ForceWindowProcMain(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_CREATE:
		if (CreateForceWindowItems(hwnd)) {
			logList_.clear();
			logListDisplayedSize_ = 0;
			bPendingTimerUpdateList_ = false;
			lastCalcText_.clear();
			commentWindow_.SetStyle(s_.commentFontName, s_.commentFontNameMulti, s_.commentFontNameEmoji, s_.bCommentFontBold, s_.bCommentFontAntiAlias,
			                        s_.commentFontOutline, s_.bUseOsdCompositor, s_.bUseTexture, s_.bUseDrawingThread);
			commentWindow_.SetCommentSize(s_.commentSize, s_.commentSizeMin, s_.commentSizeMax, s_.commentLineMargin);
			commentWindow_.SetDisplayDuration(s_.commentDuration);
			commentWindow_.SetDrawLineCount(s_.commentDrawLineCount);
			commentWindow_.SetOpacity(static_cast<BYTE>(s_.commentOpacity));
			m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, commentWindow_.GetOpacity() != 0 ? TVTest::COMMAND_ICON_STATE_CHECKED : 0);
			if (commentWindow_.GetOpacity() != 0 && m_pApp->GetPreview()) {
				HWND hwndContainer = FindVideoContainer();
				commentWindow_.Create(hwndContainer);
				bHalfSkip_ = GetWindowHeight(hwndContainer) >= s_.halfSkipThreshold;
				ProcessChatTag("<!--<chat date=\"0\" mail=\"cyan ue\" user_id=\"-\">(NicoJK ON)</chat>-->");
			}
			bDisplayLogList_ = (s_.hideForceWindow & 2) != 0;
			forwardTick_ = timeGetTime();
			forwardOffset_ = 0;
			forwardOffsetDelta_ = 0;
			currentJKToGet_ = -1;
			lastPostComm_[0] = TEXT('\0');
			bUsingLogfileDriver_ = IsMatchDriverName(s_.logfileDrivers.c_str());
			logReader_.ResetCheckInterval();
			bSpecFile_ = false;
			dropFileTimeout_ = 0;
			SendMessage(hwnd, WM_RESET_STREAM, 0, 0);

			SendDlgItemMessage(hwnd, bDisplayLogList_ ? IDC_RADIO_LOG : IDC_RADIO_FORCE, BM_SETCHECK, BST_CHECKED, 0);
			SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, BM_SETCHECK, s_.bSetRelative ? BST_CHECKED : BST_UNCHECKED, 0);
			SendDlgItemMessage(hwnd, IDC_SLIDER_OPACITY, TBM_SETRANGE, TRUE, MAKELPARAM(0, 10));
			SendDlgItemMessage(hwnd, IDC_SLIDER_OPACITY, TBM_SETPOS, TRUE, (commentWindow_.GetOpacity() * 10 + 254) / 255);
			SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(IDC_CB_POST, CBN_EDITCHANGE), 0);
			SetTimer(hwnd, TIMER_UPDATE, max(UPDATE_FORCE_INTERVAL, 10000), nullptr);
			if (s_.timerInterval >= 0) {
				SetTimer(hwnd, TIMER_FORWARD, s_.timerInterval, nullptr);
			}
			SetTimer(hwnd, TIMER_SETUP_CURJK, SETUP_CURJK_DELAY, nullptr);
			PostMessage(hwnd, WM_TIMER, TIMER_UPDATE, 0);
			PostMessage(hwnd, WM_TIMER, TIMER_JK_WATCHDOG, 0);
			if (hPanel_) {
				// パネルウィンドウに連動
				DeleteBrush(SetClassLongPtr(hwnd, GCLP_HBRBACKGROUND,
					reinterpret_cast<LONG_PTR>(CreateSolidBrush(m_pApp->GetColor(L"PanelBack")))));
				GetClientRect(hPanel_, &s_.rcForce);
			} else {
				// 位置を復元
				HMONITOR hMon = MonitorFromRect(&s_.rcForce, MONITOR_DEFAULTTONEAREST);
				MONITORINFO mi;
				mi.cbSize = sizeof(MONITORINFO);
				if (s_.rcForce.right <= s_.rcForce.left || !GetMonitorInfo(hMon, &mi) ||
				    s_.rcForce.right < mi.rcMonitor.left + 20 || mi.rcMonitor.right - 20 < s_.rcForce.left ||
				    s_.rcForce.bottom < mi.rcMonitor.top + 20 || mi.rcMonitor.bottom - 20 < s_.rcForce.top) {
					GetWindowRect(hwnd, &s_.rcForce);
				}
			}
			MoveWindow(hwnd, 0, 0, 64, 64, FALSE);
			MoveWindow(hwnd, s_.rcForce.left, s_.rcForce.top, s_.rcForce.right - s_.rcForce.left, s_.rcForce.bottom - s_.rcForce.top, FALSE);
			// 不透明度を復元
			LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);
			SetWindowLong(hwnd, GWL_EXSTYLE, s_.forceOpacity == 255 ? style & ~WS_EX_LAYERED : style | WS_EX_LAYERED);
			SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(s_.forceOpacity), LWA_ALPHA);

			m_pApp->SetPluginCommandState(COMMAND_HIDE_FORCE, 0);
			if (hPanel_ || (s_.hideForceWindow & 1) == 0) {
				ShowWindow(hwnd, SW_SHOWNA);
				SendMessage(hwnd, WM_SET_ZORDER, 0, 0);
			}
			// TVTest起動直後はVideo Containerウィンドウの配置が定まっていないようなので再度整える
			SetTimer(hwnd, TIMER_DONE_SIZE, 500, nullptr);

			// 勢いリストのサブクラス化
			HWND hList = GetDlgItem(hwnd, IDC_FORCELIST);
			SetProp(hList, TEXT("DefProc"), reinterpret_cast<HANDLE>(GetWindowLongPtr(hList, GWLP_WNDPROC)));
			SetWindowLongPtr(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ForceListBoxProc));
			// 投稿欄のサブクラス化
			COMBOBOXINFO cbi = {};
			cbi.cbSize = sizeof(cbi);
			if (GetComboBoxInfo(GetDlgItem(hwnd, IDC_CB_POST), &cbi)) {
				SetProp(cbi.hwndItem, TEXT("Root"), hwnd);
				SetProp(cbi.hwndItem, TEXT("DefProc"), reinterpret_cast<HANDLE>(GetWindowLongPtr(cbi.hwndItem, GWLP_WNDPROC)));
				SetWindowLongPtr(cbi.hwndItem, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ForcePostEditBoxProc));
			}
			// パネルアイテムのサブクラス化
			if (hPanel_) {
				SetTVTestPanelItem(GetDlgItem(hwnd, IDC_RADIO_FORCE), m_pApp, TVTestPanelButtonProc);
				SetTVTestPanelItem(GetDlgItem(hwnd, IDC_RADIO_LOG), m_pApp, TVTestPanelButtonProc);
				SetTVTestPanelItem(GetDlgItem(hwnd, IDC_CHECK_SPECFILE), m_pApp, TVTestPanelButtonProc);
				SetTVTestPanelItem(GetDlgItem(hwnd, IDC_CHECK_RELATIVE), m_pApp, TVTestPanelButtonProc);
			}

			if (s_.commentShareMode == 1 || s_.commentShareMode == 2 || s_.bCheckProcessRecording) {
				// コマンドラインで指定されていればコメント共有の検索用プロセスIDを上書きする
				DWORD processID = 0;
				int argc;
				LPTSTR *argv = CommandLineToArgvW(GetCommandLine(), &argc);
				if (argv) {
					for (int i = 1; i + 1 < argc; ++i) {
						if (_tcsicmp(argv[i], TEXT("/jkshpid")) == 0 ||
						    _tcsicmp(argv[i], TEXT("-jkshpid")) == 0) {
							processID = _tcstoul(argv[i + 1], nullptr, 10);
							if (processID != 0 && s_.bCheckProcessRecording) {
								// 録画状態をチェックするスレッドを起動
								hQuitCheckRecordingEvent_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
								if (hQuitCheckRecordingEvent_) {
									checkRecordingThread_ = std::thread([this, processID]() { CheckRecordingThread(processID); });
								}
							}
							break;
						}
					}
					LocalFree(argv);
				}
				if (processID == 0) {
					processID = GetCurrentProcessId();
				}
				if (s_.commentShareMode == 1 || s_.commentShareMode == 2) {
					// 投稿機能は投稿欄を表示しているときだけ
					if (!jkTransfer_.Open(hwnd, WMS_TRANSFER, s_.commentShareMode == 2 && cookie_[0], processID)) {
						m_pApp->AddLog(L"設定commentShareModeを有効にできませんでした。", TVTest::LOG_TYPE_WARNING);
					}
				}
			}
			return TRUE;
		}
		return FALSE;
	case WM_DESTROY:
		{
			// 録画状態をチェックするスレッドを終了
			if (hQuitCheckRecordingEvent_) {
				SetEvent(hQuitCheckRecordingEvent_);
				checkRecordingThread_.join();
				CloseHandle(hQuitCheckRecordingEvent_);
				hQuitCheckRecordingEvent_ = nullptr;
			}
			// パネルアイテムのサブクラス化を解除
			if (hPanel_) {
				ResetTVTestPanelItem(GetDlgItem(hwnd, IDC_RADIO_FORCE));
				ResetTVTestPanelItem(GetDlgItem(hwnd, IDC_RADIO_LOG));
				ResetTVTestPanelItem(GetDlgItem(hwnd, IDC_CHECK_SPECFILE));
				ResetTVTestPanelItem(GetDlgItem(hwnd, IDC_CHECK_RELATIVE));
			}
			// 投稿欄のサブクラス化を解除
			COMBOBOXINFO cbi = {};
			cbi.cbSize = sizeof(cbi);
			if (GetComboBoxInfo(GetDlgItem(hwnd, IDC_CB_POST), &cbi)) {
				SetWindowLongPtr(cbi.hwndItem, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GetProp(cbi.hwndItem, TEXT("DefProc"))));
				RemoveProp(cbi.hwndItem, TEXT("DefProc"));
				RemoveProp(cbi.hwndItem, TEXT("Root"));
			}
			// 勢いリストのサブクラス化を解除
			HWND hList = GetDlgItem(hwnd, IDC_FORCELIST);
			SetWindowLongPtr(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GetProp(hList, TEXT("DefProc"))));
			RemoveProp(hList, TEXT("DefProc"));

			// 位置を保存
			GetWindowRect(hwnd, &s_.rcForce);
			s_.commentOpacity = (s_.commentOpacity&~0xFF) | commentWindow_.GetOpacity();
			s_.bSetRelative = SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, BM_GETCHECK, 0, 0) == BST_CHECKED;
			// ログファイルを閉じる
			WriteToLogfile(-1);
			ReadFromLogfile(-1);
			if (bSpecFile_) {
				DeleteFile(tmpSpecFileName_.c_str());
			}
			commentWindow_.Destroy();

			channelStream_.BeginClose();
			jkStream_.BeginClose();
			jkTransfer_.BeginClose();
			channelStream_.Close();
			jkStream_.Close();
			jkTransfer_.Close();
			currentJK_ = -1;

			if (syncThread_.joinable()) {
				bQuitSyncThread_ = true;
				syncThread_.join();
			}
			ToggleStreamCallback(false);
			m_pApp->SetWindowMessageCallback(nullptr);
			SaveToIni();
			m_pApp->SetPluginCommandState(COMMAND_HIDE_FORCE, TVTest::PLUGIN_COMMAND_STATE_DISABLED);
			m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, TVTest::PLUGIN_COMMAND_STATE_DISABLED);
			hForce_ = nullptr;
		}
		break;
	case WM_MEASUREITEM:
		{
			LPMEASUREITEMSTRUCT lpmis = reinterpret_cast<LPMEASUREITEMSTRUCT>(lParam);
			if (lpmis->CtlID == IDC_FORCELIST && hForceFont_) {
				HWND hItem = GetDlgItem(hwnd, IDC_FORCELIST);
				HDC hdc = GetDC(hItem);
				HFONT hFontOld = SelectFont(hdc, hForceFont_);
				TEXTMETRIC tm;
				GetTextMetrics(hdc, &tm);
				SelectFont(hdc, hFontOld);
				ReleaseDC(hItem, hdc);
				lpmis->itemHeight = tm.tmHeight + 1;
				return TRUE;
			}
		}
		break;
	case WM_CLOSE:
		// 隠すだけ
		ShowWindow(hwnd, SW_HIDE);
		return 0;
	case WM_DROPFILES:
		{
			dropFileTimeout_ = 0;
			std::vector<TCHAR> buf(DragQueryFile(reinterpret_cast<HDROP>(wParam), 0, nullptr, 0) + 1);
			if (DragQueryFile(reinterpret_cast<HDROP>(wParam), 0, buf.data(), (UINT)buf.size())) {
				dropFileName_ = buf.data();
				if (bSpecFile_) {
					ReadFromLogfile(-1);
					DeleteFile(tmpSpecFileName_.c_str());
					bSpecFile_ = false;
				}
				SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, BM_SETCHECK, BST_UNCHECKED, 0);
				dropFileTimeout_ = 1;
				SetTimer(hwnd, TIMER_OPEN_DROPFILE, 0, nullptr);
			}
		}
		break;
	case WM_HSCROLL:
		if (reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, IDC_SLIDER_OPACITY) && LOWORD(wParam) == SB_THUMBTRACK) {
			BYTE newOpacity = static_cast<BYTE>(HIWORD(wParam) * 255 / 10);
			if (commentWindow_.GetOpacity() == 0 && newOpacity != 0 && m_pApp->GetPreview()) {
				commentWindow_.ClearChat();
				HWND hwndContainer = FindVideoContainer();
				commentWindow_.Create(hwndContainer);
				bHalfSkip_ = GetWindowHeight(hwndContainer) >= s_.halfSkipThreshold;
			} else if (commentWindow_.GetOpacity() != 0 && newOpacity == 0) {
				commentWindow_.Destroy();
			}
			commentWindow_.SetOpacity(newOpacity);
			m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, newOpacity != 0 ? TVTest::COMMAND_ICON_STATE_CHECKED : 0);
		}
		break;
	case WM_DRAWITEM:
		{
			LPDRAWITEMSTRUCT lpdis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
			if (lpdis->CtlType == ODT_LISTBOX) {
				bool bSelected = (lpdis->itemState & ODS_SELECTED) != 0;
				HBRUSH hbr = CreateSolidBrush(bSelected ? GetSysColor(COLOR_HIGHLIGHT) : GetBkColor(lpdis->hDC));
				FillRect(lpdis->hDC, &lpdis->rcItem, hbr);
				DeleteBrush(hbr);

				TCHAR text[1024];
				if (ListBox_GetTextLen(lpdis->hwndItem, lpdis->itemID) < _countof(text)) {
					int textLen = ListBox_GetText(lpdis->hwndItem, lpdis->itemID, text);
					if (textLen >= 0) {
						LPCTSTR pText = text;
						bool bEmphasis = false;
						if (pText[0] == TEXT('#')) {
							// 文字列の強調色表示
							bEmphasis = true;
							++pText;
						}
						COLORREF crBk = RGB(0xFF, 0xFF, 0xFF);
						if (pText[0] == TEXT('[')) {
							// 右側文字列の背景色指定
							LPCTSTR p = _tcschr(++pText, TEXT(']'));
							if (p) {
								crBk = _tcstol(pText, nullptr, 10);
								pText = p + 1;
							}
						}
						int oldBkMode = SetBkMode(lpdis->hDC, TRANSPARENT);
						COLORREF crOld = SetTextColor(lpdis->hDC, bSelected ? GetSysColor(COLOR_HIGHLIGHTTEXT) :
						                                          bEmphasis ? RGB(0xFF, 0, 0) : GetTextColor(lpdis->hDC));
						RECT rc = lpdis->rcItem;
						rc.left += 1;
						if (pText[0] == TEXT('{')) {
							// 左側文字列の描画幅指定
							size_t fixedLen = _tcscspn(&pText[1], TEXT("}"));
							if (text + textLen >= pText + 2 + 2 * fixedLen) {
								tstring calcText(&pText[1], &pText[1 + fixedLen]);
								tstring drawText(&pText[2 + fixedLen], &pText[2 + 2 * fixedLen]);
								pText += 2 + 2 * fixedLen;
								int mask = s_.headerMask;
								for (size_t i = 0; i < calcText.size(); mask >>= 1) {
									if (mask & 1) {
										calcText.erase(i, 1);
										drawText.erase(i, 1);
									} else {
										++i;
									}
								}
								if (!calcText.empty()) {
									if (lastCalcText_ != calcText) {
										RECT rcCalc = rc;
										DrawText(lpdis->hDC, calcText.c_str(), -1, &rcCalc, DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX | DT_CALCRECT);
										lastCalcText_ = calcText;
										lastCalcWidth_ = rcCalc.right - rcCalc.left;
									}
									DrawText(lpdis->hDC, drawText.c_str(), -1, &rc, DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
									rc.left += lastCalcWidth_;
								}
							}
						}
						COLORREF crBkOld = SetBkColor(lpdis->hDC, crBk);
						if (!bSelected && crBk != RGB(0xFF, 0xFF, 0xFF)) {
							SetBkMode(lpdis->hDC, OPAQUE);
							// TODO: ホントは黒文字を仮定してはいけない(ハイコントラストテーマとか)
							if (3*GetRValue(crBk) + 6*GetGValue(crBk) + GetBValue(crBk) < 255) {
								SetTextColor(lpdis->hDC, RGB(0xFF, 0xFF, 0xFF));
							}
						}
						DrawText(lpdis->hDC, pText, -1, &rc, DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
						SetBkColor(lpdis->hDC, crBkOld);
						SetTextColor(lpdis->hDC, crOld);
						SetBkMode(lpdis->hDC, oldBkMode);
					}
				}
				return TRUE;
			}
		}
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_RADIO_FORCE:
		case IDC_RADIO_LOG:
			bDisplayLogList_ = SendDlgItemMessage(hwnd, IDC_RADIO_LOG, BM_GETCHECK, 0, 0 ) == BST_CHECKED;
			SendMessage(hwnd, WM_UPDATE_LIST, TRUE, 0);
			PostMessage(hwnd, WM_TIMER, TIMER_UPDATE, 0);
			break;
		case IDC_CHECK_SPECFILE:
			if (bSpecFile_ != (SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, BM_GETCHECK, 0, 0) == BST_CHECKED)) {
				if (bSpecFile_) {
					ReadFromLogfile(-1);
					DeleteFile(tmpSpecFileName_.c_str());
					bSpecFile_ = false;
				} else {
					LONGLONG llft = 0;
					TCHAR path[MAX_PATH];
					bool bRel = SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, BM_GETCHECK, 0, 0) == BST_CHECKED;
					// ダイアログを開いている間にD&Dされるかもしれない
					if ((!bRel || (llft = GetCurrentTot()) >= 0) &&
					    FileOpenDialog(hwnd, TEXT("実況ログ(*.jkl;*.xml)\0*.jkl;*.xml\0すべてのファイル\0*.*\0"), path, _countof(path)) &&
					    !bSpecFile_ && ImportLogfile(path, tmpSpecFileName_.c_str(), bRel ? FileTimeToUnixTime(llft) + 2 : 0))
					{
						logReader_.ResetCheckInterval();
						bSpecFile_ = true;
					}
				}
				SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, BM_SETCHECK, bSpecFile_ ? BST_CHECKED : BST_UNCHECKED, 0);
			}
			break;
		case IDC_FORCELIST:
			if (HIWORD(wParam) == LBN_SELCHANGE) {
				if (!bDisplayLogList_) {
					// 勢いリスト表示中
					int index = ListBox_GetCurSel((HWND)lParam);
					int jkID = -1;
					if (0 <= index && index < (int)forceList_.size()) {
						jkID = forceList_[index].jkID;
					}
					if (currentJKToGet_ != jkID) {
						currentJKToGet_ = jkID;
						jkStream_.Shutdown();
						commentWindow_.ClearChat();
						SetTimer(hwnd, TIMER_JK_WATCHDOG, 1000, nullptr);
					}
					if (s_.bSetChannel && !bUsingLogfileDriver_ && !bRecording_ && jkID > 0) {
						// 本体のチャンネル切り替えをする
						int currentTuning = m_pApp->GetTuningSpace();
						for (int stage = 0; stage < 2; ++stage) {
							DWORD ntsID;
							for (int i = 0; GetChannelNetworkServiceID(currentTuning, i, &ntsID); ++i) {
								std::vector<NETWORK_SERVICE_ID_ELEM>::const_iterator it = LowerBoundNetworkServiceID(ntsIDList_.begin(), ntsIDList_.end(), ntsID);
								int chJK = it != ntsIDList_.end() && it->ntsID == ntsID ? it->jkID : -1;
								// 実況IDが一致するチャンネルに切替
								// 実況IDからチャンネルへの対応は一般に一意ではないので優先度を設ける
								if ((stage > 0 || (chJK & NETWORK_SERVICE_ID_ELEM::JKID_PRIOR)) && jkID == (chJK & ~NETWORK_SERVICE_ID_ELEM::JKID_PRIOR)) {
									// すでに表示中なら切り替えない
									if (ntsID != GetCurrentNetworkServiceID()) {
										m_pApp->SetChannel(currentTuning, i, ntsID >> 16);
									}
									stage = 2;
									break;
								}
							}
						}
					}
				}
			} else if (HIWORD(wParam) == LBN_DBLCLK) {
				int index = ListBox_GetCurSel((HWND)lParam);
				if (bDisplayLogList_ && 0 <= index && index < (int)logList_.size()) {
					std::list<LOG_ELEM>::const_iterator it = logList_.begin();
					std::advance(it, index);
					if (it->marker[0] != TEXT('#') && it->marker[0] != TEXT('.')) {
						// ユーザーNGの置換パターンをつくる
						RPL_ELEM e;
						e.section = TEXT("AutoReplace");
						// 14文字で切っているのは単に表現を短くするため。深い理由はない
						TCHAR pattern[256];
						_stprintf_s(pattern, TEXT("s/^<chat(?=.*? user_id=\"%.14s%s.*>.*<)/<chat abone=\"1\"/g"),
						            it->marker, _tcslen(it->marker) > 14 ? TEXT("") : TEXT("\""));
						if (e.SetPattern(pattern)) {
							// 既存パターンかどうか調べる
							std::vector<RPL_ELEM> autoRplList;
							LoadRplListFromIni(TEXT("AutoReplace"), &autoRplList);
							auto jt = std::find_if(autoRplList.cbegin(), autoRplList.cend(), [&](const RPL_ELEM &a) { return a.pattern == e.pattern; });
							// メッセージボックスで確認
							TCHAR header[_countof(it->marker) + 32];
							_stprintf_s(header, TEXT(">>%d ID:%s\n"), it->no, it->marker);
							if (jt != autoRplList.end()) {
								if (MessageBox(hwnd, (header + it->text).c_str(), TEXT("NicoJK - NG【解除】します"), MB_OKCANCEL) == IDOK) {
									autoRplList.erase(jt);
									for (int i = 0; i < (int)autoRplList.size(); autoRplList[i].key = i, ++i);
									SaveRplListToIni(TEXT("AutoReplace"), autoRplList);
								}
							} else {
								if (MessageBox(hwnd, (header + it->text).c_str(), TEXT("NicoJK - NG登録します"), MB_OKCANCEL) == IDOK) {
									autoRplList.push_back(e);
									while ((int)autoRplList.size() > max(s_.maxAutoReplace, 0)) {
										autoRplList.erase(autoRplList.begin());
									}
									for (int i = 0; i < (int)autoRplList.size(); autoRplList[i].key = i, ++i);
									SaveRplListToIni(TEXT("AutoReplace"), autoRplList);
								}
							}
							// 置換リストを更新
							rplList_ = autoRplList;
							LoadRplListFromIni(TEXT("CustomReplace"), &rplList_);
						}
					}
				}
			}
			break;
		case IDC_CB_POST:
			if (HIWORD(wParam) == CBN_EDITCHANGE) {
				// コメント装飾例を作成する
				TCHAR comm[POST_COMMENT_MAX + 32];
				GetPostComboBoxText(comm, _countof(comm));
				while (SendDlgItemMessage(hwnd, IDC_CB_POST, CB_DELETESTRING, 0, 0) > 0);
				for (LPCTSTR p = s_.mailDecorations.c_str(); *p; ) {
					size_t len = _tcscspn(p, TEXT(":"));
					TCHAR text[_countof(comm) + 64];
					_tcsncpy_s(text, p, min<size_t>(len, 63));
					_tcscat_s(text, comm);
					SendDlgItemMessage(hwnd, IDC_CB_POST, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
					p += p[len] ? len + 1 : len;
				}
				// 文字数警告する
				int excess = static_cast<int>(_tcslen(comm)) - (POST_COMMENT_MAX - 1);
				if (excess > 0) {
					TCHAR text[64];
					_stprintf_s(text, TEXT("Warning:%d文字を超えています(+%d)。"), POST_COMMENT_MAX - 1, excess);
					OutputMessageLog(text);
				}
			}
			break;
		}
		break;
	case WM_TIMER:
		switch (wParam) {
		case TIMER_UPDATE:
			if (currentJK_ >= 0) {
				// 自力計算による勢い情報を更新する
				DWORD tick = GetTickCount();
				if (tick - currentJKForceByChatCountTick_ >= 30000) {
					// 初回は10コメ引く
					int count = max(currentJKChatCount_ - (currentJKForceByChatCount_ < 0 ? 10 : 0), 0);
					currentJKForceByChatCount_ = count * 60 / ((tick - currentJKForceByChatCountTick_) / 1000);
					currentJKChatCount_ = 0;
					currentJKForceByChatCountTick_ = tick;
				}
			}
			if (!bDisplayLogList_ && IsWindowVisible(hwnd)) {
				// 勢いを更新する
				if (!s_.channelsUri.empty() &&
				    channelStream_.Send(hwnd, WMS_FORCE, 'G', s_.channelsUri.c_str())) {
					channelBuf_.clear();
				} else {
					for (auto it = forceList_.begin(); it != forceList_.end(); ++it) {
						// 自力計算による勢い情報を使う
						it->force = it->jkID == currentJK_ ? currentJKForceByChatCount_ : -1;
					}
					SendMessage(hwnd, WM_UPDATE_LIST, 2, 0);
				}
			}
			break;
		case TIMER_JK_WATCHDOG:
			SetTimer(hwnd, TIMER_JK_WATCHDOG, max(JK_WATCHDOG_INTERVAL, 10000), nullptr);
			if (currentJKToGet_ >= 0 && !bUsingLogfileDriver_) {
				// chatStreamIDに変換
				std::vector<FORCE_ELEM>::const_iterator it = LowerBoundJKID(forceList_.begin(), forceList_.end(), currentJKToGet_);
				if (it != forceList_.end() && it->jkID == currentJKToGet_ && !it->chatStreamID.empty()) {
					if (jkStream_.Send(hwnd, WMS_JK, 'L', (it->chatStreamID + " " + cookie_).c_str())) {
						currentJK_ = currentJKToGet_;
						currentJKChatCount_ = 0;
						currentJKForceByChatCount_ = -1;
						currentJKForceByChatCountTick_ = GetTickCount();
						OutputMessageLog(TEXT("コメントサーバに接続開始しました。"));
					}
				}
			}
			break;
		case TIMER_FORWARD:
			bFlipFlop_ = !bFlipFlop_;
			if (syncThread_.joinable() || !bHalfSkip_ || bFlipFlop_) {
				bool resyncComment = false;
				{
					lock_recursive_mutex lock(streamLock_);
					if (bResyncComment_) {
						resyncComment = true;
						bResyncComment_ = false;
					}
				}
				// オフセットを調整する
				bool bNotify = false;
				if (0 < forwardOffsetDelta_ && forwardOffsetDelta_ <= 30000) {
					// 前進させて調整
					int delta = min(static_cast<int>(forwardOffsetDelta_), forwardOffsetDelta_ < 10000 ? 500 : 2000);
					forwardOffset_ += delta;
					forwardOffsetDelta_ -= delta;
					bNotify = forwardOffsetDelta_ == 0;
					commentWindow_.Forward(delta);
				} else if (forwardOffsetDelta_ != 0) {
					// ログファイルを閉じて一気に調整
					forwardOffset_ += forwardOffsetDelta_;
					forwardOffsetDelta_ = 0;
					bNotify = true;
					ReadFromLogfile(-1);
					commentWindow_.ClearChat();
				} else if (resyncComment) {
					// シーク時のコメント再生位置の再調整
					ReadFromLogfile(-1);
					commentWindow_.ClearChat();
				}
				if (bNotify) {
					TCHAR text[64];
					int sign = forwardOffset_ < 0 ? -1 : 1;
					LONGLONG absSec = sign * forwardOffset_ / 1000;
					LONGLONG absMin = absSec / 60;
					LONGLONG absHour = absMin / 60;
					if (absSec < 60) {
						_stprintf_s(text, TEXT("(Offset %lld)"), sign * absSec);
					} else if (absMin < 60) {
						_stprintf_s(text, TEXT("(Offset %lld:%02lld)"), sign * absMin, absSec % 60);
					} else if (absHour < 24) {
						_stprintf_s(text, TEXT("(Offset %lld:%02lld:%02lld)"), sign * absHour, absMin % 60, absSec % 60);
					} else {
						_stprintf_s(text, TEXT("(Offset %lld'%02lld:%02lld:%02lld)"), sign * (absHour / 24), absHour % 24, absMin % 60, absSec % 60);
					}
					commentWindow_.AddChat(text, RGB(0x00,0xFF,0xFF), CCommentWindow::CHAT_POS_UE);
				}
				// コメントの表示を進める
				DWORD tick = timeGetTime();
				commentWindow_.Forward(min(static_cast<int>(tick - forwardTick_), 5000));
				forwardTick_ = tick;
				// 過去ログがあれば処理する
				LONGLONG llft = GetCurrentTot();
				if (llft >= 0) {
					bool bRead = false;
					const char *text;
					LONGLONG tm = FileTimeToUnixTime(llft);
					tm = min(max(forwardOffset_ < 0 ? tm - (-forwardOffset_ / 1000) : tm + forwardOffset_ / 1000, 0LL), UINT_MAX - 3600LL);
					while (ReadFromLogfile(bSpecFile_ ? 0 : bUsingLogfileDriver_ ? currentJKToGet_ : -1, &text, static_cast<unsigned int>(tm))) {
						ProcessChatTag(text);
						bRead = true;
						if (!logReader_.IsOpen()) {
							// 次の読み込みは確実に失敗するので省略
							break;
						}
					}
					if (bRead) {
						// date属性値は秒精度しかないのでコメント表示が団子にならないよう適当にごまかす
						commentWindow_.ScatterLatestChats(1000);
						PostMessage(hwnd, WM_UPDATE_LIST, FALSE, 0);
					}
				}
				commentWindow_.Update();
				bPendingTimerForward_ = false;
			}
			break;
		case TIMER_SETUP_CURJK:
			{
				// 視聴状態が変化したので視聴中のサービスに対応する実況IDを調べて変更する
				KillTimer(hwnd, TIMER_SETUP_CURJK);
				DWORD ntsID = GetCurrentNetworkServiceID();
				std::vector<NETWORK_SERVICE_ID_ELEM>::const_iterator it = LowerBoundNetworkServiceID(ntsIDList_.begin(), ntsIDList_.end(), ntsID);
				int jkID = it != ntsIDList_.end() && (it->ntsID == ntsID || (!(ntsID & 0xFFFF) && ntsID == (it->ntsID & 0xFFFF0000))) && it->jkID > 0 ?
					(it->jkID & ~NETWORK_SERVICE_ID_ELEM::JKID_PRIOR) : -1;
				if (currentJKToGet_ != jkID) {
					currentJKToGet_ = jkID;
					jkStream_.Shutdown();
					commentWindow_.ClearChat();
					SetTimer(hwnd, TIMER_JK_WATCHDOG, 1000, nullptr);
					// 選択項目を更新するため
					SendMessage(hwnd, WM_UPDATE_LIST, TRUE, 0);
				}
			}
			break;
		case TIMER_OPEN_DROPFILE:
			// D&Dされた実況ログファイルを開く
			// TSファイルとの同時D&Dを考慮してRelチェック時は基準とするTOTの取得タイミングを遅らせる
			if (--dropFileTimeout_ < 0 || bSpecFile_) {
				KillTimer(hwnd, TIMER_OPEN_DROPFILE);
			} else {
				LONGLONG llft = 0;
				bool bRel = SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, BM_GETCHECK, 0, 0) == BST_CHECKED;
				if (!bRel || (llft = GetCurrentTot()) >= 0) {
					KillTimer(hwnd, TIMER_OPEN_DROPFILE);
					if (ImportLogfile(dropFileName_.c_str(), tmpSpecFileName_.c_str(), bRel ? FileTimeToUnixTime(llft) + 2 : 0)) {
						logReader_.ResetCheckInterval();
						bSpecFile_ = true;
						SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, BM_SETCHECK, BST_CHECKED, 0);
					}
				}
			}
			break;
		case TIMER_DONE_MOVE:
			KillTimer(hwnd, TIMER_DONE_MOVE);
			commentWindow_.OnParentMove();
			break;
		case TIMER_DONE_SIZE:
			KillTimer(hwnd, TIMER_DONE_SIZE);
			commentWindow_.OnParentSize();
			bHalfSkip_ = GetWindowHeight(FindVideoContainer()) >= s_.halfSkipThreshold;
			break;
		case TIMER_DONE_POSCHANGE:
			KillTimer(hwnd, TIMER_DONE_POSCHANGE);
			if (!m_pApp->GetFullscreen() && ((s_.hideForceWindow & 4) || (GetWindowLong(m_pApp->GetAppWindow(), GWL_STYLE) & WS_MAXIMIZE))) {
				SendMessage(hwnd, WM_SET_ZORDER, 0, 0);
			}
			break;
		case TIMER_UPDATE_LIST:
			KillTimer(hwnd, TIMER_UPDATE_LIST);
			lastUpdateListTick_ = 0;
			bPendingTimerUpdateList_ = false;
			SendMessage(hwnd, WM_UPDATE_LIST, FALSE, 0);
			break;
		}
		break;
	case WM_RESET_STREAM:
		dprintf(TEXT("CNicoJK::ForceDialogProcMain() WM_RESET_STREAM\n")); // DEBUG
		{
			lock_recursive_mutex lock(streamLock_);
			llftTot_ = -1;
		}
		ReadFromLogfile(-1);
		return TRUE;
	case WM_UPDATE_LIST:
		{
			if (!wParam) {
				// 再描画の頻度を抑える
				DWORD tick = lastUpdateListTick_;
				lastUpdateListTick_ = GetTickCount();
				if (bPendingTimerUpdateList_) {
					return TRUE;
				}
				if (lastUpdateListTick_ - tick < COMMENT_REDRAW_INTERVAL) {
					bPendingTimerUpdateList_ = true;
					SetTimer(hwnd, TIMER_UPDATE_LIST, COMMENT_REDRAW_INTERVAL - (lastUpdateListTick_ - tick), nullptr);
					return TRUE;
				}
			}
			HWND hList = GetDlgItem(hwnd, IDC_FORCELIST);
			if (!bDisplayLogList_ || !IsWindowVisible(hwnd)) {
				// リストが増え続けないようにする
				for (; logList_.size() > COMMENT_TRIMEND; logList_.pop_front());
				logListDisplayedSize_ = 0;
			}
			if (!IsWindowVisible(hwnd)) {
				// 非表示中はサボる
				if (ListBox_GetCount(hList) != 0) {
					ListBox_ResetContent(hList);
				}
				return TRUE;
			} else if (!bDisplayLogList_ && !wParam) {
				// 勢いリスト表示中は差分更新(wParam==FALSE)しない
				return TRUE;
			}
			// 描画を一時停止
			SendMessage(hList, WM_SETREDRAW, FALSE, 0);
			int iTopItemIndex = ListBox_GetTopIndex(hList);
			// wParam!=FALSEのときはリストの内容をリセットする
			if (wParam) {
				ListBox_ResetContent(hList);
				// wParam==2のときはスクロール位置を保存する
				if (wParam != 2) {
					iTopItemIndex = 0;
				}
			}
			if (bDisplayLogList_) {
				// ログリスト表示中
				int iSelItemIndex = ListBox_GetCurSel(hList);
				if (logList_.size() < logListDisplayedSize_ || ListBox_GetCount(hList) != logListDisplayedSize_) {
					ListBox_ResetContent(hList);
					logListDisplayedSize_ = 0;
				}
				// logList_とリストボックスの内容が常に同期するように更新する
				std::list<LOG_ELEM>::const_iterator it = logList_.end();
				for (size_t i = logList_.size() - logListDisplayedSize_; i > 0; --i, --it);
				for (; it != logList_.end(); ++it) {
					TCHAR text[256];
					int len = _stprintf_s(text, TEXT("%s[%u]{00:00:00 (MMM)}%02d:%02d:%02d (%.3s)%s"),
					                      it->marker[0] == TEXT('#') ? TEXT("#") : TEXT(""), static_cast<DWORD>(it->cr),
					                      it->st.wHour, it->st.wMinute, it->st.wSecond,
					                      it->marker, &TEXT("   ")[min<size_t>(_tcslen(it->marker), 3)]);
					_tcsncpy_s(text + len, _countof(text) - len, it->text.c_str(), _TRUNCATE);
					ListBox_AddString(hList, text);
					++logListDisplayedSize_;
				}
				while (logList_.size() > COMMENT_TRIMEND) {
					logList_.pop_front();
					ListBox_DeleteString(hList, 0);
					--logListDisplayedSize_;
					--iSelItemIndex;
					--iTopItemIndex;
				}
				if (iSelItemIndex < 0) {
					ListBox_SetTopIndex(hList, ListBox_GetCount(hList) - 1);
				} else {
					ListBox_SetCurSel(hList, iSelItemIndex);
					ListBox_SetTopIndex(hList, max(iTopItemIndex, 0));
				}
			} else {
				// 勢いリスト表示中
				for (auto it = forceList_.cbegin(); it != forceList_.end(); ++it) {
					TCHAR text[256];
					if (it->force < 0) {
						_stprintf_s(text, TEXT("jk%d 勢? (%.63s%s%.63S)"),
						            it->jkID, it->name.c_str(),
						            it->chatStreamID.empty() ? TEXT("") : TEXT("-"), it->chatStreamID.c_str());
					} else {
						_stprintf_s(text, TEXT("jk%d 勢%d (%.63s%s%.63S)"),
						            it->jkID, it->force, it->name.c_str(),
						            it->chatStreamID.empty() ? TEXT("") : TEXT("-"), it->chatStreamID.c_str());
					}
					ListBox_AddString(hList, text);
					if (it->jkID == currentJKToGet_) {
						ListBox_SetCurSel(hList, ListBox_GetCount(hList) - 1);
					}
				}
				ListBox_SetTopIndex(hList, iTopItemIndex);
			}
			// 描画を再開
			SendMessage(hList, WM_SETREDRAW, TRUE, 0);
			InvalidateRect(hList, nullptr, FALSE);
		}
		return TRUE;
	case WMS_FORCE:
		{
			static const std::regex reChannel("<((?:bs_|radio_)?channel)>([^]*?)</\\1>");
			static const std::regex reVideo("<video>jk(\\d+)</video>");
			static const std::regex reForce("<force>(\\d+)</force>");
			static const std::regex reName("<name>([^<]*)</name>");

			int ret = channelStream_.ProcessRecv(channelBuf_);
			if (ret < 0) {
				// 切断
				if (ret == -2) {
					channelBuf_.push_back('\0');
					std::cmatch m;
					const char *p = channelBuf_.data();
					const char *pLast = &p[strlen(p)];
					for (; std::regex_search(p, pLast, m, reChannel); p = m[0].second) {
						std::cmatch mVideo;
						if (std::regex_search(m[2].first, m[2].second, mVideo, reVideo)) {
							int jkID = strtol(mVideo[1].first, nullptr, 10);
							std::vector<FORCE_ELEM>::iterator it = LowerBoundJKID(forceList_.begin(), forceList_.end(), jkID);
							if (it != forceList_.end() && it->jkID == jkID) {
								// 勢いと(もしあれば)名前を上書き
								std::cmatch mForce, mName;
								it->force = std::regex_search(m[2].first, m[2].second, mForce, reForce) ? strtol(mForce[1].first, nullptr, 10) : -1;
								if (!it->bFixedName && std::regex_search(m[2].first, m[2].second, mName, reName)) {
									TCHAR szName[64];
									int len = MultiByteToWideChar(CP_UTF8, 0, mName[1].first, static_cast<int>(mName[1].length()), szName, _countof(szName) - 1);
									szName[len] = TEXT('\0');
									DecodeEntityReference(szName);
									it->name = szName;
								}
							}
						}
					}
				}
				SendMessage(hwnd, WM_UPDATE_LIST, 2, 0);
			}
		}
		return TRUE;
	case WMS_JK:
		{
			static const std::regex reChatResult("^<chat_result(?= )[^>]*? status=\"(\\d+)\"");
			static const std::regex reXRoom("^<x_room ");
			static const std::regex reNickname("^<x_room(?= )[^>]*? nickname=\"(.*?)\"");
			static const std::regex reIsLoggedIn("^<x_room(?= )[^>]*? is_logged_in=\"1\"");

			jkBuf_.clear();
			int ret = jkStream_.ProcessRecv(jkBuf_);
			if (ret < 0) {
				// 切断
				OutputMessageLog(TEXT("コメントサーバとの通信を切断しました。"));
				WriteToLogfile(-1);
				currentJK_ = -1;
			} else {
				// 受信中
				bool bRead = false;
				for (std::vector<char>::iterator it = jkBuf_.begin(); ; ) {
					std::vector<char>::iterator itEnd = std::find(it, jkBuf_.end(), '\n');
					if (itEnd == jkBuf_.end()) {
						break;
					}
					*itEnd = '\0';
					if (itEnd - it >= CLogReader::CHAT_TAG_MAX) {
						*(it + CLogReader::CHAT_TAG_MAX - 1) = '\0';
					}
					const char *rpl = &*it;
					if (!strncmp(rpl, "<chat ", 6)) {
						// 指定ファイル再生中は混じると鬱陶しいので表示しない。後退指定はある程度反映
						if (ProcessChatTag(rpl, !bSpecFile_, static_cast<int>(min(max(-forwardOffset_, 0LL), 30000LL)))) {
							dprintf(TEXT("#")); // DEBUG
							WriteToLogfile(currentJK_, rpl);
							jkTransfer_.SendChat(currentJK_, rpl);
							++currentJKChatCount_;
							bRead = true;
						}
					} else {
						std::cmatch m;
						if (std::regex_search(rpl, m, reChatResult)) {
							// コメント投稿の応答を取得した
							int status = strtol(m[1].first, nullptr, 10);
							if (status != 0) {
								TCHAR text[64];
								_stprintf_s(text, TEXT("Error:コメント投稿に失敗しました(status=%d)。"), status);
								OutputMessageLog(text);
							}
							jkTransfer_.SendChat(currentJK_, rpl);
						} else if (std::regex_search(rpl, reXRoom)) {
							// 接続情報を取得した
							TCHAR nickname[64];
							nickname[0] = TEXT('\0');
							if (std::regex_search(rpl, m, reNickname)) {
								int len = MultiByteToWideChar(CP_UTF8, 0, m[1].first, static_cast<int>(m[1].length()), nickname, _countof(nickname) - 1);
								nickname[len] = TEXT('\0');
								DecodeEntityReference(nickname);
							}
							bool isLoggedIn = std::regex_search(rpl, reIsLoggedIn);
							TCHAR text[128];
							_stprintf_s(text, TEXT("コメントサーバに接続しました(%s%s)。"), isLoggedIn ? TEXT("login=") : TEXT(""), nickname);
							OutputMessageLog(text);
						}
					}
#ifdef _DEBUG
					TCHAR debug[512];
					int debugLen = MultiByteToWideChar(CP_UTF8, 0, rpl, -1, debug, _countof(debug) - 1);
					debug[debugLen] = TEXT('\0');
					dprintf(TEXT("%s\n"), debug); // DEBUG
#endif
					it = itEnd + 1;
				}
				if (bRead && bDisplayLogList_) {
					SendMessage(hwnd, WM_UPDATE_LIST, FALSE, 0);
				}
			}
		}
		return TRUE;
	case WMS_TRANSFER:
		{
			std::string u8post = jkTransfer_.ProcessRecvPost();
			size_t mailEndPos = u8post.find(']');
			if (mailEndPos != std::string::npos && u8post[0] == '[') {
				if (GetTickCount() - lastPostTick_ < POST_COMMENT_INTERVAL) {
					OutputMessageLog(TEXT("Error:投稿間隔が短すぎます。"));
					jkTransfer_.SendChat(currentJK_, "<!-- M=Post error! Short interval. -->");
				} else if (u8post.size() - mailEndPos - 1 > 0) {
					TCHAR comm[POST_COMMENT_MAX + 1];
					int len = MultiByteToWideChar(CP_UTF8, 0, u8post.c_str() + mailEndPos + 1, -1, comm, _countof(comm) - 1);
					comm[len] = TEXT('\0');
					if (!comm[0]) {
						OutputMessageLog(TEXT("Error:投稿コメントが長すぎます。"));
						jkTransfer_.SendChat(currentJK_, "<!-- M=Post error! Too long. -->");
					} else if (!_tcscmp(comm, lastPostComm_)) {
						OutputMessageLog(TEXT("Error:投稿コメントが前回と同じです。"));
						jkTransfer_.SendChat(currentJK_, "<!-- M=Post error! Same as previous. -->");
					} else {
						if (s_.bAnonymity) {
							u8post.insert(mailEndPos, " 184");
						}
						// コメント投稿
						if (jkStream_.Send(hwnd, WMS_JK, '+', u8post.c_str())) {
							lastPostTick_ = GetTickCount();
							_tcscpy_s(lastPostComm_, comm);
						} else {
							OutputMessageLog(TEXT("Error:コメントサーバに接続していません。"));
							jkTransfer_.SendChat(currentJK_, "<!-- M=Post error! Not connected. -->");
						}
					}
				}
			}
		}
		return TRUE;
	case WM_SET_ZORDER:
		// 全画面や最大化時は前面のほうが都合がよいはず
		if ((s_.hideForceWindow & 4) || m_pApp->GetFullscreen() || (GetWindowLong(m_pApp->GetAppWindow(), GWL_STYLE) & WS_MAXIMIZE)) {
			// TVTestウィンドウの前面にもってくる
			SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
			SetWindowPos(hwnd, m_pApp->GetFullscreen() || m_pApp->GetAlwaysOnTop() ? HWND_TOPMOST : HWND_TOP,
			             0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
		} else {
			// TVTestウィンドウの背面にもってくる
			SetWindowPos(hwnd, m_pApp->GetAppWindow(), 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
		}
		return TRUE;
	case WM_POST_COMMENT:
		{
			TCHAR comm[POST_COMMENT_MAX + 1];
			if (GetDlgItemText(hwnd, IDC_CB_POST, comm, _countof(comm)) && comm[0] == TEXT('@')) {
				// ローカルコマンドとして処理
				ProcessLocalPost(&comm[1]);
				return TRUE;
			}
			TCHAR mail[64];
			GetPostComboBoxText(comm, _countof(comm), mail, _countof(mail));
			if (GetTickCount() - lastPostTick_ < POST_COMMENT_INTERVAL) {
				OutputMessageLog(TEXT("Error:投稿間隔が短すぎます。"));
			} else if (_tcslen(comm) >= POST_COMMENT_MAX) {
				OutputMessageLog(TEXT("Error:投稿コメントが長すぎます。"));
			} else if (comm[0] && !_tcscmp(comm, lastPostComm_)) {
				OutputMessageLog(TEXT("Error:投稿コメントが前回と同じです。"));
			} else if (comm[0]) {
				TCHAR post[POST_COMMENT_MAX + 128];
				_stprintf_s(post, TEXT("[%s%s]%s"), mail, s_.bAnonymity ? TEXT(" 184") : TEXT(""), comm);
				size_t j = 0;
				for (size_t i = 0; post[i]; ++i) {
					// Tab文字or改行->レコードセパレータ
					post[j] = post[i] == TEXT('\t') || post[i] == TEXT('\n') ? TEXT('\x1e') : post[i];
					if (post[j] != TEXT('\r')) ++j;
				}
				post[j] = TEXT('\0');
				// 文字コード変換
				char u8post[_countof(post) * 3];
				int len = WideCharToMultiByte(CP_UTF8, 0, post, -1, u8post, _countof(u8post) - 1, nullptr, nullptr);
				u8post[len] = '\0';
				// コメント投稿
				if (jkStream_.Send(hwnd, WMS_JK, '+', u8post)) {
					lastPostTick_ = GetTickCount();
					GetPostComboBoxText(lastPostComm_, _countof(lastPostComm_));
					// アンドゥできるように選択削除で消す
					if (SendDlgItemMessage(hwnd, IDC_CB_POST, CB_SETEDITSEL, 0, MAKELPARAM(0, -1)) == TRUE) {
						SendDlgItemMessage(hwnd, IDC_CB_POST, WM_CLEAR, 0, 0);
					}
					dprintf(TEXT("##POST##%s\n"), post); // DEBUG
				} else {
					OutputMessageLog(TEXT("Error:コメントサーバに接続していません。"));
				}
			}
		}
		return TRUE;
	case WM_SHOWWINDOW:
		m_pApp->SetPluginCommandState(COMMAND_HIDE_FORCE, wParam != 0 ? TVTest::COMMAND_ICON_STATE_CHECKED : 0);
		// FALL THROUGH!
	case WM_SIZE:
		{
			RECT rcParent, rc;
			GetClientRect(hwnd, &rcParent);
			HWND hItem = GetDlgItem(hwnd, IDC_CB_POST);
			GetWindowRect(hItem, &rc);
			MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&rc), 2);
			int padding = rc.left;
			if (!cookie_[0]) {
				// クッキーが設定されていなければ間違いなく投稿不能なので入力ボックスを表示しない
				SetWindowPos(hItem, nullptr, rc.left, rcParent.bottom, rcParent.right-rc.left*2, rc.bottom-rc.top, SWP_NOZORDER);
			} else {
				padding += 6 + static_cast<int>(SendMessage(hItem, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
				SetWindowPos(hItem, nullptr, rc.left, rcParent.bottom-padding, rcParent.right-rc.left*2, rc.bottom-rc.top, SWP_NOZORDER);
			}
			hItem = GetDlgItem(hwnd, IDC_FORCELIST);
			GetWindowRect(hItem, &rc);
			MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&rc), 2);
			if (cookie_[0]) {
				// ボタン類が入力ボックスと被らないようにする
				int swShow = rcParent.bottom-rc.top-padding < -4 ? SW_HIDE : SW_SHOW;
				if (uMsg == WM_SHOWWINDOW || (GetWindowLong(GetDlgItem(hwnd, IDC_RADIO_FORCE), GWL_STYLE) & WS_VISIBLE ? true : false) != (swShow != SW_HIDE)) {
					ShowWindow(GetDlgItem(hwnd, IDC_RADIO_FORCE), swShow);
					ShowWindow(GetDlgItem(hwnd, IDC_RADIO_LOG), swShow);
					ShowWindow(GetDlgItem(hwnd, IDC_CHECK_SPECFILE), swShow);
					ShowWindow(GetDlgItem(hwnd, IDC_CHECK_RELATIVE), swShow);
					ShowWindow(GetDlgItem(hwnd, IDC_SLIDER_OPACITY), swShow);
				}
			}
			SetWindowPos(hItem, nullptr, 0, 0, rcParent.right-rc.left*2, rcParent.bottom-rc.top-padding, SWP_NOMOVE | SWP_NOZORDER);
		}
		break;
	}
	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ストリームコールバック(別スレッド)
BOOL CALLBACK CNicoJK::StreamCallback(BYTE *pData, void *pClientData)
{
	CNicoJK *pThis = static_cast<CNicoJK*>(pClientData);
	int pid = ((pData[1]&0x1F)<<8) | pData[2];
	BYTE bTransportError = pData[1]&0x80;
	BYTE bPayloadUnitStart = pData[1]&0x40;
	BYTE bHasAdaptation = pData[3]&0x20;
	BYTE bHasPayload = pData[3]&0x10;
	BYTE bAdaptationLength = pData[4];
	BYTE bPcrFlag = pData[5]&0x10;

	// シークやポーズを検出するためにPCRを調べる
	if (bHasAdaptation && bAdaptationLength >= 5 && bPcrFlag && !bTransportError) {
		DWORD pcr = (static_cast<DWORD>(pData[5+1])<<24) | (pData[5+2]<<16) | (pData[5+3]<<8) | pData[5+4];
		// 参照PIDのPCRが現れることなく5回別のPCRが出現すれば、参照PIDを変更する
		if (pid != pThis->pcrPid_) {
			int i = 0;
			for (; pThis->pcrPids_[i] >= 0; ++i) {
				if (pThis->pcrPids_[i] == pid) {
					if (++pThis->pcrPidCounts_[i] >= 5) {
						pThis->pcrPid_ = pid;
					}
					break;
				}
			}
			if (pThis->pcrPids_[i] < 0 && i + 1 < _countof(pThis->pcrPids_)) {
				pThis->pcrPids_[i] = pid;
				pThis->pcrPidCounts_[i] = 1;
				pThis->pcrPids_[++i] = -1;
			}
		}
		if (pid == pThis->pcrPid_) {
			pThis->pcrPids_[0] = -1;
		}
		//dprintf(TEXT("CNicoJK::StreamCallback() PCR\n")); // DEBUG
		lock_recursive_mutex lock(pThis->streamLock_);
		DWORD tick = GetTickCount();
		// 2秒以上PCRを取得できていない→ポーズから回復?
		bool bReset = tick - pThis->pcrTick_ >= 2000;
		pThis->pcrTick_ = tick;
		if (pid == pThis->pcrPid_) {
			long long pcrDiff = CounterDiff(pcr, pThis->pcr_);
			// ラップアラウンド近傍を特別扱いする必要はない(またいでシークする場合だってある)

			if (bReset || 0 <= pcrDiff && pcrDiff < 45000) {
				// 1秒以内は通常の再生と見なす
			} else if (abs(pcrDiff) < 15 * 60 * 45000) {
				// -15～0分、+1秒～15分PCRが飛んでいる場合、シークとみなし、
				// シークした分だけTOTをずらして読み込み直す
				if (pThis->llftTot_ >= 0 && pThis->llftTotPending_ != -1) {
					long long totDiff = pcrDiff * FILETIME_MILLISECOND / 45;
					pThis->llftTot_ += totDiff;
					pThis->totPcr_ += pcr - pThis->pcr_;
					if (pThis->llftTotLast_ >= 0) {
						pThis->llftTotLast_ += totDiff;
					}
					// 保留中のTOTはシーク後に取得した可能性があるので捨てる(再生速度の推定が狂ってコメントが大量に流れたりするのを防ぐため)
					pThis->llftTotPending_ = -2;
					pThis->bResyncComment_ = true;
				} else {
					bReset = true;
				}
			} else {
				// それ以上飛んでたら別ストリームと見なしてリセット
				bReset = true;
			}
			// 保留中のTOTはPCRの取得後に利用可能(llftTot_にシフト)にする
			if (pThis->llftTot_ >= 0) {
				if (pThis->llftTotPending_ >= 0) {
					pThis->llftTotLast_ = pThis->llftTot_;
					pThis->llftTot_ = pThis->llftTotPending_;
					pThis->totTickLast_ = pThis->totTick_;
					pThis->totTick_ = pThis->totTickPending_;
					// TOTの変化と対応するPCRの変化が5秒以上食い違っている場合、TOTがジャンプしたとみなして前回TOTを捨てる
					if (abs((pThis->llftTot_ - pThis->llftTotLast_) / FILETIME_MILLISECOND - CounterDiff(pcr, pThis->totPcr_) / 45) >= 5000) {
						pThis->llftTotLast_ = -1;
						// 状況はシークとそう違わないので読み直しも必要
						// 食い違い地点をまたいでシークすると二度読み直しが発生するが、解決は簡単ではなさそうなので保留
						pThis->bResyncComment_ = true;
					}
				}
				if (pThis->llftTotPending_ != -2) {
					// llftTot_に対応するPCRを取得済みであることを示す
					pThis->llftTotPending_ = -2;
					pThis->totPcr_ = pcr;
				}
			}
			pThis->pcr_ = pcr;
		}
		if (bReset) {
			// TOTを取得できていないことを表す
			pThis->llftTot_ = -1;
			PostMessage(pThis->hForce_, WM_RESET_STREAM, 0, 0);
		}
	}

	// TOTパケットは地上波の実測で6秒に1個程度
	// ARIB規格では最低30秒に1個
	if (pid == 0x14 && bPayloadUnitStart && bHasPayload && !bTransportError) {
		BYTE *pPayload = pData + 4;
		if (bHasAdaptation) {
			// アダプテーションフィールドをスキップする
			if (bAdaptationLength > 182) {
				pPayload = nullptr;
			} else {
				pPayload += 1 + bAdaptationLength;
			}
		}
		if (pPayload) {
			BYTE *pTable = pPayload + 1 + pPayload[0];
			// TOT or TDT (ARIB STD-B10)
			if (pTable + 7 < pData + 188 && (pTable[0] == 0x73 || pTable[0] == 0x70)) {
				// TOT時刻とTickカウントを記録する
				LONGLONG llft = AribToFileTime(&pTable[3]);
				if (llft >= 0) {
					// UTCに変換
					llft += -32400000LL * FILETIME_MILLISECOND;
					dprintf(TEXT("CNicoJK::StreamCallback() TOT\n")); // DEBUG
					lock_recursive_mutex lock(pThis->streamLock_);
					// 時刻が変化したときだけ
					if (llft != pThis->llftTot_) {
						pThis->llftTotPending_ = llft;
						pThis->totTickPending_ = GetTickCount();
						if (pThis->llftTot_ < 0) {
							// 初回だけ速やかに取得
							pThis->llftTot_ = pThis->llftTotPending_;
							pThis->totTick_ = pThis->totTickPending_;
							pThis->llftTotLast_ = -1;
							pThis->llftTotPending_ = -1;
							pThis->totPcr_ = 0;
						}
					}
				}
			}
		}
	}
	return TRUE;
}

TVTest::CTVTestPlugin *CreatePluginClass()
{
	return new CNicoJK();
}
