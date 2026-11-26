/*
 * WinPE-Shell: Windows 7 style two-column Start menu.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

// Enabled with WinXShell.jcfg  JS_STARTMENU.style = "win7"  (default "classic" = the cascading ReactOS menu).
// Left column : pinned programs (default: shortcuts directly in Start Menu\Programs), "All Programs" (the whole
//               Programs tree, folders expand in place), search box (filters all shortcuts; Enter with no match
//               runs the text as a command).
// Right column: configurable links (JS_STARTMENU.win7.right), Shut down button + arrow (Restart, Restart shell).
// Documented in README.md.

#pragma once

bool StartMenu7_Enabled();
void StartMenu7_Toggle(HWND hwndStartButton);
bool StartMenu7_IsVisible();
void StartMenu7_Hide();
void StartMenu7_Prefetch();      // shell start: first background scan of JS_STARTMENU.extra_folders
