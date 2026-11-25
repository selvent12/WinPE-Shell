# Changelog

## Unreleased (2026-11)

Base: PExplorer branch `WinXShell_shellpart` @ `5f8b886` (2018-08-19).

### Shell (`src/`)
- **01** Toolset v140 -> v143, Windows SDK 8.1 -> 10.0.26100.0.
- **02** Removed `-install` (it wrote `Winlogon\Shell` in the registry).
- **03** `startup()`: no `Run`/`RunOnce` processing, no `PendingFileRenameOperations`.
- **04** No `wsock32` delay-load (unused network library); obj/pch moved to the intermediate directory.
- **05** `ShellItemEnumerator`: empty or not-ready drives are shown as empty folders instead of raising an
  exception dialog.
- **06** x64: all pointer/handle truncations fixed (C4311/C4312/C4302/C4244/C4267/C4090); these warnings are now
  compile errors. `DWLP_`/`GCLP_` indexes, `%p` for pointers, an uninitialised variable (C4701) fixed.
- **07** Desktop context menu: Display settings, System properties, Open command window here, Restart shell,
  Restart..., Shut down... (configurable, see docs/CONFIGURATION.md). "Restart shell" = clean exit (a launcher or
  watchdog restarts the shell), with an 8 s safety timeout.
- **08** Windows 7 style two-column Start menu (`JS_STARTMENU.style = "win7"`, `taskbar/startmenu7.cpp`): pinned
  programs, All Programs tree, search (Enter without a match runs the text), configurable right column, Shut down +
  Restart menu; `JS_STARTMENU.extra_folders` adds programs found at run time (any drive, background scan). The
  classic menu stays the default.
- **09** Windows 11 look (`JS_TASKBAR.style = "win11"`, `taskbar/win11style.cpp`): icon-only task buttons with
  rounded hover/active backgrounds and a running indicator (accent pill for the active window, gray dot for the
  others), Windows logo Start button, 1 px top border, Segoe UI clock (time over date, right aligned), quick launch
  icons scaled and centered; `JS_TASKBAR.align = "left"` (default) or `"center"` (Start + task buttons centered as
  one group). `JS_STARTMENU.style = "win11"`: the two-column menu with the search box on top, rounded corners and
  highlights, theme colors, floating above the taskbar (centered on the screen with `align: center`). The classic
  look stays the default.
- Repository layout: shell sources moved to `src/`.

### UI utility (`ui/`, new)
- Shutdown dialog (overlay on every monitor, Restart / Shut down, Esc cancels).
- Display window: Extend / Duplicate / single display, resolution, refresh rate, rotation, main display, 15 s
  automatic revert, brightness (laptop panels via WMI, external monitors via DDC/CI).
- Network: OFF at boot, `net on/off/toggle/status`, tray icon, Wi-Fi window (Native Wifi API, temporary profiles
  only), DNS client handling.
- Handler for the `ms-settings:` and `ms-availablenetworks:` protocols.

### Test tools (`tools/test/`, new)
- `DlgTest` (file dialog reproduction, 5 modes), `MyPEStress` (window/shell stress), `MyPEDump` (minidump writer).
