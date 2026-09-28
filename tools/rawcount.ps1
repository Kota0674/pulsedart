param([int]$Seconds = 20)
# Counts WM_INPUT mouse packets per device (background sink), prints a summary.
Add-Type -ReferencedAssemblies System.Windows.Forms -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

public class RawCount : NativeWindow {
    [StructLayout(LayoutKind.Sequential)] struct RID { public ushort page, usage; public uint flags; public IntPtr hwnd; }
    [DllImport("user32.dll")] static extern bool RegisterRawInputDevices(RID[] r, uint n, uint sz);
    [DllImport("user32.dll")] static extern uint GetRawInputData(IntPtr h, uint cmd, byte[] data, ref uint sz, uint hdr);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern uint GetRawInputDeviceInfo(IntPtr h, uint cmd, StringBuilder data, ref uint sz);
    public Dictionary<IntPtr, int> count = new Dictionary<IntPtr, int>();
    public Dictionary<IntPtr, long> first = new Dictionary<IntPtr, long>();
    public Dictionary<IntPtr, long> last = new Dictionary<IntPtr, long>();
    public System.Diagnostics.Stopwatch sw = System.Diagnostics.Stopwatch.StartNew();
    public RawCount() {
        CreateHandle(new CreateParams());
        RID[] r = { new RID { page = 1, usage = 2, flags = 0x100, hwnd = Handle } };
        if (!RegisterRawInputDevices(r, 1, (uint)Marshal.SizeOf(typeof(RID)))) throw new Exception("register failed");
    }
    protected override void WndProc(ref Message m) {
        if (m.Msg == 0x00FF) {
            uint sz = 0, hdr = (uint)(8 + 2 * IntPtr.Size);
            GetRawInputData(m.LParam, 0x10000003, null, ref sz, hdr);
            byte[] b = new byte[sz];
            if (GetRawInputData(m.LParam, 0x10000003, b, ref sz, hdr) == sz && BitConverter.ToInt32(b, 0) == 0) {
                IntPtr dev = IntPtr.Size == 8 ? (IntPtr)BitConverter.ToInt64(b, 8) : (IntPtr)BitConverter.ToInt32(b, 8);
                int c; count.TryGetValue(dev, out c); count[dev] = c + 1;
                if (!first.ContainsKey(dev)) first[dev] = sw.ElapsedMilliseconds;
                last[dev] = sw.ElapsedMilliseconds;
            }
        }
        base.WndProc(ref m);
    }
    public static string Name(IntPtr dev) {
        uint sz = 512; var sb = new StringBuilder(512);
        GetRawInputDeviceInfo(dev, 0x20000007, sb, ref sz);
        return sb.ToString();
    }
}
'@
$rc = New-Object RawCount
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 5 }
foreach ($k in $rc.count.Keys) {
    "{0,6} packets  t={1}..{2} ms  {3}" -f $rc.count[$k], $rc.first[$k], $rc.last[$k], [RawCount]::Name($k)
}
"done"
