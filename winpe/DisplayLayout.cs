// DisplayLayout - display layout helper for WinPE (WinPE-Shell, winpe/). Own code, GNU LGPL 2.1 or later. Win32 only: QueryDisplayConfig / SetDisplayConfig / EnumDisplaySettings / ChangeDisplaySettingsEx.
// Compiled from this source with the Microsoft csc.exe of .NET Framework 4 (build.ps1, or by the image builder); nothing precompiled is stored.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class DisplayLayout {
    const uint QDC_ONLY_ACTIVE_PATHS = 2, QDC_ALL_PATHS = 1;
    const uint SDC_TOPOLOGY_EXTEND = 0x4, SDC_APPLY = 0x80, SDC_ALLOW_CHANGES = 0x400, SDC_SAVE_TO_DATABASE = 0x200;
    const int ENUM_CURRENT_SETTINGS = -1, CDS_UPDATEREGISTRY = 1, CDS_NORESET = 0x10000000, CDS_SET_PRIMARY = 0x10;
    const int DM_POSITION = 0x20, DM_PELSWIDTH = 0x80000, DM_PELSHEIGHT = 0x100000, DM_DISPLAYFREQUENCY = 0x400000;

    [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
    [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint np, [Out] byte[] paths, ref uint nm, [Out] byte[] modes, IntPtr topo);
    [DllImport("user32.dll")] static extern int SetDisplayConfig(uint np, IntPtr paths, uint nm, IntPtr modes, uint flags);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DISPLAY_DEVICE { public int cb; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString; public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE { [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName; public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields; public int dmPositionX, dmPositionY; public int dmDisplayOrientation, dmDisplayFixedOutput; public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName; public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency,
        dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplayDevices(string dev, int i, ref DISPLAY_DEVICE dd, int flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettingsEx(string dev, ref DEVMODE dm, IntPtr hwnd, int flags, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettingsEx(string dev, IntPtr dm, IntPtr hwnd, int flags, IntPtr lp);

    public class Monitor { public string Device; public string Adapter; public bool Active; public bool Primary; public int X, Y, Width, Height, Hz; public List<string> Modes = new List<string>(); }

    public static int ActivePathCount() {
        uint np, nm; if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, out np, out nm) != 0) return -1;
        var p = new byte[np * 72]; var m = new byte[nm * 64];
        return QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ref np, p, ref nm, m, IntPtr.Zero) == 0 ? (int)np : -1;
    }
    public static int AllPathCount() { uint np, nm; return GetDisplayConfigBufferSizes(QDC_ALL_PATHS, out np, out nm) == 0 ? (int)np : -1; }

    // Equivalent of DisplaySwitch.exe /extend
    public static int Extend() { return SetDisplayConfig(0, IntPtr.Zero, 0, IntPtr.Zero, SDC_TOPOLOGY_EXTEND | SDC_APPLY | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES); }

    public static List<Monitor> List() {
        var r = new List<Monitor>();
        for (int i = 0; ; i++) {
            var dd = new DISPLAY_DEVICE(); dd.cb = Marshal.SizeOf(dd);
            if (!EnumDisplayDevices(null, i, ref dd, 0)) break;
            var m = new Monitor { Device = dd.DeviceName, Adapter = dd.DeviceString, Active = (dd.StateFlags & 1) != 0, Primary = (dd.StateFlags & 4) != 0 };
            var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(dm);
            if (EnumDisplaySettings(dd.DeviceName, ENUM_CURRENT_SETTINGS, ref dm)) { m.X = dm.dmPositionX; m.Y = dm.dmPositionY; m.Width = dm.dmPelsWidth; m.Height = dm.dmPelsHeight; m.Hz = dm.dmDisplayFrequency; }
            for (int k = 0; ; k++) { var d2 = new DEVMODE(); d2.dmSize = (short)Marshal.SizeOf(d2); if (!EnumDisplaySettings(dd.DeviceName, k, ref d2)) break;
                string s = d2.dmPelsWidth + "x" + d2.dmPelsHeight + "@" + d2.dmDisplayFrequency; if (!m.Modes.Contains(s)) m.Modes.Add(s); }
            r.Add(m);
        }
        return r;
    }
    // Stage one monitor (no reset); call Apply() after all monitors are staged.
    public static int Stage(string device, int width, int height, int hz, int x, int y, bool primary) {
        var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(dm);
        if (!EnumDisplaySettings(device, ENUM_CURRENT_SETTINGS, ref dm)) EnumDisplaySettings(device, -2, ref dm);
        dm.dmPelsWidth = width; dm.dmPelsHeight = height; dm.dmPositionX = x; dm.dmPositionY = y; dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
        if (hz > 0) { dm.dmDisplayFrequency = hz; dm.dmFields |= DM_DISPLAYFREQUENCY; }
        int flags = CDS_UPDATEREGISTRY | CDS_NORESET | (primary ? CDS_SET_PRIMARY : 0);
        return ChangeDisplaySettingsEx(device, ref dm, IntPtr.Zero, flags, IntPtr.Zero);
    }
    public static int Apply() { return ChangeDisplaySettingsEx(null, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero); }
}
