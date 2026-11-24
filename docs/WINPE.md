# Using WinPE-Shell in a WinPE image

These notes come from building a Windows 11 25H2 WinPE (base: `boot.wim` index 2 / WinRE) that uses this shell.
All files mentioned are **Microsoft files taken from the matching `install.wim`**; nothing third-party is needed.

## 1. Files
- `WinXShell.exe` + `WinXShell.jcfg` (for example in `X:\Program Files\WinXShell\`).
- `MyPEUI.exe` + `mypeui.ini` (currently `X:\MyPE\` and `X:\MyPE\config\`).
- **Icon resources (`Windows\SystemResources\*.mun`)**: Windows 11 keeps the icons of system DLLs in `.mun` files.
  A WinPE base image ships only ~19 of them. Without the `.mun` of `imageres`, `shell32`, `ExplorerFrame`, `mmcbase`,
  `mmcndmgr`, `dmdskres` (and of every other DLL you add), the **modern Open/Save dialog crashes** (exit 255 /
  0xC0000005) and MMC reports "Unable to create new document". Copy the `.mun` of every binary present in the image.
- Language resources (`en-US\*.mui`) of every file you add.
- For the Display window brightness on external monitors: `System32\dxva2.dll`.
- For the network window: the WLAN stack (WlanSvc, `wlanapi.dll`, `nwifi.sys`, ...) and the network services you
  want to start; WinRE has most of them, a plain ADK WinPE does not.

## 2. Registry (offline, in the image hives)
- `SOFTWARE\Classes\ms-settings` and `ms-availablenetworks`: `URL Protocol` + `shell\open\command` =
  `"X:\MyPE\MyPEUI.exe" uri "%1"`. (WinPE's own `ms-settings` points to `control.exe`.)
- Remove leftover desktop background verbs from other shells (`DesktopBackground\Shell\*`) to avoid duplicates.
- Shell runs as SYSTEM: per-user settings live in `HKU\.DEFAULT` (`config\DEFAULT`), e.g.
  `Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced` `Hidden=2`, `ShowSuperHidden=0` (hides `desktop.ini`).
- Network OFF by default: `Unattend.xml` with `Microsoft-Windows-Setup/EnableNetwork=false` for `wpeinit`, network
  services on demand start, the DNS client (`Dnscache`) disabled at boot and allowed to be reconfigured by SYSTEM
  (its default security descriptor denies that), no trigger start for `Dnscache`.

## 3. Startup
`winpeshl.ini` should start one long-running launcher that runs `wpeinit`, the startup steps (e.g.
`MyPEUI net off /boot`, `MyPEUI tray-net`, a display layout script) and then starts and watches the shell
(restarts it if it exits; "Restart shell" relies on that). If the last program in `winpeshl.ini` exits, WinPE reboots.

## 4. Known limitations
- `start ms-settings:...` typed in `cmd` fails in WinPE ("The specified module could not be found"): a bare URI goes
  through the URL parser in `ieframe.dll`, which WinPE lacks. Programs that use `ShellExecuteEx` with the protocol
  class (as the desktop menu does) work.
- DPI scaling is read at session start only; choose it when building the image (`LogPixels`).
- The Windows 11 Explorer (`explorer.exe`) itself cannot run as the WinPE shell (its taskbar needs AppX/XAML
  infrastructure and a real logon session); that is why a separate shell is used.
