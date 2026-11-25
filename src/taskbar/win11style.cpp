/*
 * WinPE-Shell: Windows 11 look for the taskbar and the Start menu (see win11style.h).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <precomp.h>

#include "win11style.h"
#include "../jconfig/jcfg.h"

// case-insensitive: a hand-edited "Win11" / "Center" must not silently give the classic look
static bool CfgIs(const TCHAR *key, const TCHAR *value, const TCHAR *def)
{
    return _wcsicmp(JCfg_GetValue(&g_JCfg, TEXT("JS_TASKBAR"), key, Value(def)).ToString(std::wstring(def)).c_str(), value) == 0;
}

// Feature marker for image builders: the exe is searched for this UTF-16 string to know that it supports the
// win11 taskbar / Start menu style and align (instead of searching for any "win11" text). Referenced through a
// volatile pointer so the linker keeps it.
static const wchar_t s_featureMarker[] = L"WINPESHELL_FEATURES=taskbar-win11;taskbar-align;startmenu-win11";
const wchar_t *volatile g_W11FeatureMarker = s_featureMarker;

bool W11_Enabled()
{
    static int v = -1;              // the configuration is read once at start; a change needs a shell restart
    if (v < 0) v = CfgIs(TEXT("style"), TEXT("win11"), TEXT("classic")) ? 1 : 0;
    return v == 1;
}

bool W11_Center()
{
    static int v = -1;
    if (v < 0) v = (W11_Enabled() && CfgIs(TEXT("align"), TEXT("center"), TEXT("left"))) ? 1 : 0;
    return v == 1;
}

bool W11_Dark()
{
    static int v = -1;
    if (v < 0) v = CfgIs(TEXT("theme"), TEXT("light"), TEXT("dark")) ? 0 : 1;
    return v == 1;
}

// background shades derived from the configured taskbar color: lighter on a dark bar, darker on a light one
static COLORREF Shade(int delta)
{
    COLORREF bk = TASKBAR_BKCOLOR();
    if (!W11_Dark())
        delta = -delta;
    int r = GetRValue(bk) + delta, g = GetGValue(bk) + delta, b = GetBValue(bk) + delta;
    return RGB(max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)));
}

COLORREF W11_Color(W11Color c)
{
    bool dark = W11_Dark();
    switch (c) {
    case W11C_BORDER:    return Shade(dark ? 28 : 22);
    case W11C_HOVER:     return Shade(dark ? 24 : 16);
    case W11C_ACTIVE:    return Shade(dark ? 16 : 10);
    case W11C_PRESSED:   return Shade(dark ? 10 : 22);
    case W11C_ACCENT:    return dark ? RGB(76, 194, 255) : RGB(0, 95, 184);
    case W11C_INDICATOR: return dark ? RGB(160, 160, 160) : RGB(118, 118, 118);
    }
    return TASKBAR_BKCOLOR();
}

HFONT W11_Font()
{
    static HFONT font = NULL;
    if (!font) {
        LOGFONT lf = {};
        lf.lfHeight = -MulDiv(9, g_JCfg_DPI_SY, 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lstrcpy(lf.lfFaceName, TEXT("Segoe UI"));
        font = CreateFontIndirect(&lf);
        if (!font)
            font = GetStockFont(DEFAULT_GUI_FONT);
    }
    return font;
}

int W11_TaskButtonWidth()
{
    return max(DPI_SX(44), TASKBAR_ICON_SIZE + DPI_SX(20));
}

int W11_StartButtonWidth()
{
    return W11_TaskButtonWidth();
}

void W11_FillRound(HDC hdc, const RECT &rc, int radius, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    // with a NULL pen the right and bottom edges are excluded: +1
    RoundRect(hdc, rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius * 2, radius * 2);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(br);
}

void W11_FrameRound(HDC hdc, const RECT &rc, int radius, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void W11_DrawStartLogo(HDC hdc, const RECT &rc, int size, COLORREF c)
{
    int gap = max(1, size / 14);
    int sq = (size - gap) / 2;
    int x = (rc.left + rc.right - (sq * 2 + gap)) / 2;
    int y = (rc.top + rc.bottom - (sq * 2 + gap)) / 2;
    HBRUSH br = CreateSolidBrush(c);
    RECT r[4] = {
        { x, y, x + sq, y + sq },
        { x + sq + gap, y, x + sq * 2 + gap, y + sq },
        { x, y + sq + gap, x + sq, y + sq * 2 + gap },
        { x + sq + gap, y + sq + gap, x + sq * 2 + gap, y + sq * 2 + gap },
    };
    for (auto &q : r)
        FillRect(hdc, &q, br);
    DeleteObject(br);
}
