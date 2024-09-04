// stdafx.h : 標準のシステム インクルード ファイルのインクルード ファイル、または
// 参照回数が多く、かつあまり変更されない、プロジェクト専用のインクルード ファイル
// を記述します。
//

#pragma once

#define _WIN32_WINNT _WIN32_WINNT_VISTA

#include <sdkddkver.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <WindowsX.h>
#include <objbase.h>
#include <vector>
#include <list>
#include <regex>
#include <memory>
#include <utility>
#include <algorithm>
#include <tchar.h>

#pragma comment(lib, "winmm.lib")

// NOMINMAXではGdiPlus.hが通らない
#undef min
#undef max
using std::min;
using std::max;

typedef std::basic_string<TCHAR> tstring;
