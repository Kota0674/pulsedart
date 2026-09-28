param([int]$Seconds = 10)
# Measures how often mouse reports reach Windows, per device (Raw Input).
# Move the mouse in circles the whole time. Prints the report rate and the gaps between
# reports; the typical gap is the update interval of that link (USB poll, BLE connection
# interval, dongle).
# Windows 11 24H2+ throttles raw input of background windows to ~125 Hz, so the script shows a
# small window on top; keep it focused while measuring.
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

public class RawInterval : Form {
    [StructLayout(LayoutKind.Sequential)] struct RID { public ushort page, usage; public uint flags; public IntPtr hwnd; }
    [DllImport("user32.dll")] static extern bool RegisterRawInputDevices(RID[] r, uint n, uint sz);
    [DllImport("user32.dll")] static extern uint GetRawInputData(IntPtr h, uint cmd, byte[] data, ref uint sz, uint hdr);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern uint GetRawInputDeviceInfo(IntPtr h, uint cmd, StringBuilder data, ref uint sz);
    public Dictionary<IntPtr, List<double>> t = new Dictionary<IntPtr, List<double>>();
    public Dictionary<IntPtr, string> names = new Dictionary<IntPtr, string>();
    Stopwatch sw = Stopwatch.StartNew();
    public RawInterval(int seconds) {
        Text = "rawinterval: move the mouse, keep this window in front"; Width = 520; Height = 120;
        TopMost = true; StartPosition = FormStartPosition.CenterScreen;
        Controls.Add(new Label { Text = "Measuring... keep this window focused and move the mouse in circles.", Dock = DockStyle.Fill, TextAlign = System.Drawing.ContentAlignment.MiddleCenter });
        Shown += (s, e) => {
            Activate();
            RID[] r = { new RID { page = 1, usage = 2, flags = 0x100, hwnd = Handle } };
            RegisterRawInputDevices(r, 1, (uint)Marshal.SizeOf(typeof(RID)));
            var tm = new Timer { Interval = seconds * 1000 };
            tm.Tick += (a, b) => Close();
            tm.Start();
        };
    }
    protected override void WndProc(ref Message m) {
        if (m.Msg == 0x00FF) {
            double now = sw.Elapsed.TotalMilliseconds;
            uint sz = 0, hdr = (uint)(8 + 2 * IntPtr.Size);
            GetRawInputData(m.LParam, 0x10000003, null, ref sz, hdr);
            byte[] b = new byte[sz];
            if (GetRawInputData(m.LParam, 0x10000003, b, ref sz, hdr) == sz && BitConverter.ToInt32(b, 0) == 0) {
                IntPtr dev = IntPtr.Size == 8 ? (IntPtr)BitConverter.ToInt64(b, 8) : (IntPtr)BitConverter.ToInt32(b, 8);
                if (!t.ContainsKey(dev)) { t[dev] = new List<double>(); names[dev] = DevName(dev); }
                t[dev].Add(now);
            }
        }
        base.WndProc(ref m);
    }
    public static string DevName(IntPtr dev) {
        uint sz = 512; var sb = new StringBuilder(512);
        GetRawInputDeviceInfo(dev, 0x20000007, sb, ref sz);
        return sb.ToString();
    }
}
'@
$f = New-Object RawInterval $Seconds
[System.Windows.Forms.Application]::Run($f)
foreach ($k in $f.t.Keys) {
    $ts = $f.t[$k]
    if ($ts.Count -lt 20) { continue }
    $gaps = @(); for ($i = 1; $i -lt $ts.Count; $i++) { $gaps += $ts[$i] - $ts[$i - 1] }
    $s = $gaps | Sort-Object
    $pct = { param($p) $s[[math]::Min($s.Count - 1, [int]($p * $s.Count))] }
    $big = @($s | Where-Object { $_ -gt 2 })
    $bm = if ($big.Count) { $big[[int]($big.Count / 2)] } else { 0 }
    "{0}" -f $f.names[$k]
    "  reports {0} over {1:N1} s = {2:N0}/s" -f $ts.Count, (($ts[-1] - $ts[0]) / 1000), ($ts.Count / (($ts[-1] - $ts[0]) / 1000))
    "  gap median {0:N2} ms, p90 {1:N2} ms, p99 {2:N2} ms, max {3:N1} ms" -f (& $pct 0.5), (& $pct 0.9), (& $pct 0.99), $s[-1]
    "  gaps > 2 ms: {0} ({1:N0}%), their median {2:N2} ms" -f $big.Count, (100 * $big.Count / $s.Count), $bm
}
