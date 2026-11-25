# Features

## Shell (`WinXShell.exe`)
- Desktop with icons, wallpaper or solid colour, standard shell view menus (View, Sort by, Refresh, Paste, New).
- Taskbar with Start button, quick launch, task buttons and notification area (tray), clock.
- Start menu: programs, run, restart and shutdown commands (configurable).
- Desktop context menu entries (configurable, can be hidden one by one):
  **Display settings**, **System properties**, **Open command window here**, **Restart shell**, **Restart...**,
  **Shut down...**.
- Alt+F4 on the desktop opens the shutdown dialog.
- Folder windows can be handed to a third-party file manager (for example Explorer++), see CONFIGURATION.md.

## UI utility (`MyPEUI.exe`)

| Command | What it does |
|---|---|
| `MyPEUI shutdown [/dryrun]` | Shutdown dialog: semi-opaque overlay on every monitor, two round buttons (Restart, Shut down) on the main one. Esc or a click outside cancels. `/dryrun` only logs. |
| `MyPEUI restart [/dryrun]` | Same dialog, keyboard focus on Restart. |
| `MyPEUI open display [/select N]` | Display window (below). |
| `MyPEUI open network` | Network window (below). |
| `MyPEUI open sysinfo` | System information (placeholder; pluggable). |
| `MyPEUI uri <uri>` | Handler for `ms-settings:` and `ms-availablenetworks:` (`display`, `screenrotation` -> Display window; `network*`, `ms-availablenetworks:` -> network window; others -> "not available"). |
| `MyPEUI net on\|off\|toggle\|status [/quiet] [/boot]` | Network control. |
| `MyPEUI tray-net` | Network icon in the notification area (single instance). |

### Display window
- Monitors drawn by their real position; click to select.
- Mode: Extend, Duplicate, only display N.
- Per display: resolution (only modes reported by the driver), refresh rate, orientation 0/90/180/270, main display.
- Every change is applied with a **"Keep changes?" countdown; without confirmation it reverts after 15 s**.
- Brightness row, shown only when brightness can be controlled: laptop panels (WMI `WmiMonitorBrightness`) or
  external monitors with DDC/CI (`dxva2.dll`, loaded at run time).
- Scale for all displays, **applied live**: 100 % up to the largest scale a display accepts (1920x1080: 175 %,
  4K: 300 %+), each display clamped to its own maximum; the shell restarts so the taskbar is drawn sharp at the new
  DPI. Uses the per-monitor scale of the display configuration API (as the Settings app does), no new session needed.
  Command line: `MyPEUI.exe scale <percent> [/norestart]`. Default at start: `"scale"` in `display.json` (see WINPE.md).
- "Save as default" writes the layout to the session's `display.json`; "Restart shell now".
- Works with the Microsoft Basic Display Adapter (one display) without errors.

### Network
- Designed for images where the network must stay **off until the user turns it on**.
- `net off /boot` (startup step) verifies the OFF state: no IPv4 address, network services stopped.
- `net on`: installs drivers from the image's driver store for network controllers without a driver, starts the
  network services in dependency order (including the DNS client), `wpeutil InitializeNetwork`, waits for a DHCP lease,
  checks DNS and logs the DNS servers.
- `net off`: disconnects Wi-Fi, releases the lease, disables the physical adapters until reboot, stops the services,
  verifies.
- Tray icon: OFF / turning on / connected `<IP>`; left click opens the network window, right click: Network ON/OFF, Wi-Fi.
- Network window: adapters, IP, gateway, DNS servers; Wi-Fi list (signal, security), connect with password
  (WPA2/WPA3-Personal) through a **temporary profile** that is never stored, disconnect.
- All steps are logged.

## Test tools (`tools/test/`)
- `DlgTest`: opens the file dialog in 5 ways (IFileOpenDialog, GetOpenFileName with and without hooks,
  SHBrowseForFolder) and closes it again; reports success, time, crashes.
- `MyPEStress`: opens/closes/switches windows and the Start menu for a given time (shell stability).
- `MyPEDump`: writes a minidump of a process (`MiniDumpWriteDump`).
