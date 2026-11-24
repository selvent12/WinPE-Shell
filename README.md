# WinPE-Shell

A lightweight desktop shell for **Windows PE** (WinPE / WinRE based images, Windows 11 24H2/25H2, x64):
desktop, taskbar, Start menu and notification area, plus a small companion utility for the things a recovery
environment usually lacks: a shutdown dialog, a Display settings window for multi-monitor setups, and a network
switch that keeps the network **off until you turn it on**.

WinPE-Shell is a fork of the shell part of [PExplorer](https://github.com/slorelee/PExplorer) (WinXShell shell part,
by slorelee), which itself is based on the ReactOS Explorer. It is built entirely from source with the Microsoft
build tools; no prebuilt third-party binaries are used.

> Status: works in our own WinPE build (tested in VMware and on real hardware: AMD GPU with two monitors, Realtek
> Ethernet). The companion utility still carries its working name `MyPEUI` and a few `X:\MyPE\...` paths; both will
> be renamed/made configurable before a 1.0 release.

## What is in the box

| Component | Path | Output | What it does |
|---|---|---|---|
| Shell | `src/` | `WinXShell.exe` | Desktop, taskbar, Start menu, tray, desktop context menu |
| UI utility | `ui/` | `MyPEUI.exe` | Shutdown dialog, Display window, network ON/OFF + Wi-Fi, `ms-settings:` handler |
| Test tools | `tools/test/` | `DlgTest.exe`, `MyPEStress.exe`, `MyPEDump.exe` | Automated checks inside a test VM (optional) |

Main changes compared with the upstream shell (see [CHANGELOG.md](CHANGELOG.md)):
- builds with Visual Studio 2022 (v143) and the Windows 11 SDK;
- **x64 clean**: all pointer/handle truncation warnings fixed and turned into compile errors;
- no writes to `Winlogon\Shell`, no `Run`/`RunOnce` processing, no network library;
- empty or not-ready drives no longer raise exception dialogs;
- a richer desktop context menu (Display settings, System properties, Open command window here, Restart shell,
  Restart..., Shut down...);
- an optional Windows 7 style Start menu (pinned programs, All Programs, search, shut down menu) that also lists
  programs found at run time in configurable folders on any drive (`JS_STARTMENU.style`, `extra_folders`).

![Windows 7 style Start menu](docs/img/startmenu-win7-shutdown.png)

Features of the UI utility: [docs/FEATURES.md](docs/FEATURES.md).

## Build

Requirements: Windows 10/11 x64, **Visual Studio 2022 Build Tools** with *Desktop development with C++* (MSVC v143)
and the **Windows 11 SDK 10.0.26100**.

```powershell
git clone https://github.com/selvent12/WinPE-Shell.git
cd WinPE-Shell
.\build.ps1              # add -TestTools for the VM test tools, -VsPath <dir> if vswhere cannot find Build Tools
```

Binaries land in `out\` together with `out\build-sha256.csv`. Details: [docs/BUILD.md](docs/BUILD.md).

## Use in a WinPE image

The shell and the utility need a few Microsoft files and registry entries that a plain WinPE image does not have
(icon resources, protocol registration, startup). See [docs/WINPE.md](docs/WINPE.md) and the configuration
reference in [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

## License

- Shell (`src/`): GNU LGPL 2.1 or later, as the upstream sources (see the file headers).
- UI utility (`ui/`), test tools (`tools/test/`), build scripts and documentation: GNU LGPL 2.1 or later.
- `src/vendor/json.*`: MIT (see `src/vendor/LICENSE`).

Full text: [LICENSE](LICENSE). Credits: [NOTICE.md](NOTICE.md).
