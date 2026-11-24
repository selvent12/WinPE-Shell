# Configuration

## Shell: `WinXShell.jcfg`
JSON file next to `WinXShell.exe`. The keys are those of the upstream shell (see `src/WinXShell.jcfg` for the full
list with comments). Commonly used:

```json
{
  "JS_FILEEXPLORER": { "3rd_filename": "X:\\Program Files\\Explorer++\\Explorer++.exe",
                       "3rd_open_arguments": "\"%s\"", "hook_GetShellWindow": false },
  "JS_DESKTOP": { "bkcolor": [30, 34, 48], "wallpaper": "X:\\wallpaper.jpg", "wallpaperstyle": 0,
                  "menu": {
                    "display":  { "command": "ms-settings:display" },
                    "sysprops": { "command": "X:\\Tools\\MyPEUI.exe", "parameters": "open sysinfo" },
                    "cmdhere":  { "hide": true }
                  } },
  "JS_TASKBAR": { "theme": "dark", "height": 40 },
  "JS_STARTMENU": { "commands": {
      "reboot":   { "command": "X:\\Tools\\MyPEUI.exe", "parameters": "restart" },
      "shutdown": { "command": "X:\\Tools\\MyPEUI.exe", "parameters": "shutdown" } } }
}
```

- `JS_FILEEXPLORER.3rd_filename`: program used for folder windows (`%s` = folder). `hook_GetShellWindow: false`
  is recommended (the hook patches `user32!GetShellWindow` in memory and is only needed for the built-in folder window).
- `JS_DESKTOP.menu.<key>` with keys `display`, `sysprops`, `cmdhere`, `restartshell`, `restart`, `shutdown`.
  Fields: `command`, `parameters`, `text` (menu caption), `hide` (true/false).
  Without `command`, `restart`/`shutdown` use the `JS_STARTMENU.commands` entries and `restartshell` ends the shell
  process (a launcher/watchdog must restart it). URI commands (`ms-settings:...`) are started with the protocol class.
- `JS_STARTMENU.commands.reboot` / `shutdown`: what Restart / Turn Off run.

## UI utility: `mypeui.ini`
UTF-16 INI file, currently at `X:\MyPE\config\mypeui.ini` (log: `X:\MyPE\logs\mypeui.log`; both paths will become
configurable).

```ini
[features]
display=enabled           ; enabled = built-in window, empty = "not available yet", other = command line to run
network=enabled
sysinfo=

[network]
mode=manual               ; manual | full-off | noNicKeepTcpip
onServices=BFE,MpsSvc,nsi,Dhcp,Dnscache,NlaSvc,lmhosts,LanmanWorkstation,WlanSvc
offServices=WlanSvc,LanmanWorkstation,lmhosts,NlaSvc,Dhcp
bootDisabled=Dnscache     ; services that must stay disabled at boot and are enabled by "net on"
waitSeconds=60
dnsTestName=              ; optional public name resolved after "net on" (empty = no external query)
```

Network state (intent) is kept in `HKLM\SOFTWARE\MyPE\Network` (`State` = on/off/turning-on/turning-off) in the
RAM registry of the running PE.
