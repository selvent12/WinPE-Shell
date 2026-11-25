/*
 * WinPE-Shell: Windows 11 look for the taskbar and the Start menu (JS_TASKBAR.style = "win11").
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

#pragma once

// JS_TASKBAR.style: "classic" (default, the upstream look) | "win11"
//   win11 = icon-only task buttons with rounded hover/active backgrounds and a running indicator under the icon,
//   Windows logo Start button, a 1 px top border, Segoe UI clock (time + date, right aligned).
// JS_TASKBAR.align: "left" (default) | "center" (win11 only: Start button + task buttons centered as one group)
bool W11_Enabled();
bool W11_Center();
bool W11_Dark();                // JS_TASKBAR.theme != "light"

enum W11Color {
    W11C_BORDER,                // taskbar top border
    W11C_HOVER,                 // button background under the mouse
    W11C_ACTIVE,                // button background of the active window
    W11C_PRESSED,               // button background while pressed
    W11C_ACCENT,                // running indicator of the active window, Start logo
    W11C_INDICATOR,             // running indicator of the other windows
};
COLORREF W11_Color(W11Color c);

HFONT W11_Font();               // Segoe UI 9 pt (DPI scaled), shared, never deleted

int W11_TaskButtonWidth();      // fixed width of a task button (DPI scaled)
int W11_StartButtonWidth();

// rounded rectangle filled with one color (no outline)
void W11_FillRound(HDC hdc, const RECT &rc, int radius, COLORREF c);
// rounded rectangle outline (1 px)
void W11_FrameRound(HDC hdc, const RECT &rc, int radius, COLORREF c);
// Windows 11 logo (four squares) centered in rc, size = edge length of the whole logo
void W11_DrawStartLogo(HDC hdc, const RECT &rc, int size, COLORREF c);
