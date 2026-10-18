using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Microsoft.Win32.SafeHandles;

namespace GsoStatus;

/// <summary>Per-disk activity from IOCTL_DISK_PERFORMANCE (the counters Performance Monitor shows as % Disk Time and
/// Avg. Disk sec/Read and sec/Write). Every sample is the change since the previous one: busy percent of the interval and the
/// mean service time of the reads and writes that finished in it. The query only reads kernel counters and sends no I/O to the disk.</summary>
sealed class DiskPerf : IDisposable
{
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFile(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle handle,uint code,byte[]? input,uint inputSize,byte[] output,uint outputSize,out uint returned,IntPtr overlap);
    const uint IoctlDiskPerformance=0x00070020, IoctlVolumeDiskExtents=0x00560000;
    sealed class State
    {
        public SafeFileHandle? Handle;
        public DateTimeOffset Tried;
        public bool HavePrevious;
        public long ReadTime,WriteTime,IdleTime,QueryTime,BytesRead,BytesWritten;
        public uint ReadCount,WriteCount;
    }
    readonly Dictionary<string,State> states=new();

    /// <summary>The disk number of a volume that lives on exactly one physical disk, or null.</summary>
    static uint? PhysicalNumber(string mount)
    {
        try
        {
            var letter=Path.GetPathRoot(mount)?.TrimEnd('\\');if(letter is null||letter.Length!=2)return null;
            using var volume=CreateFile(@"\\.\"+letter,0,3,IntPtr.Zero,3,0,IntPtr.Zero);
            if(volume.IsInvalid)return null;
            var extents=new byte[1024];
            if(!DeviceIoControl(volume,IoctlVolumeDiskExtents,null,0,extents,(uint)extents.Length,out var size,IntPtr.Zero)||size<32||BitConverter.ToUInt32(extents,0)!=1)return null;
            return BitConverter.ToUInt32(extents,8);
        }
        catch { return null; }
    }
    static JsonObject Unavailable() => Model.Missing("disk_perf_unavailable");

    public sealed record Activity(JsonObject Busy,JsonObject Read,JsonObject Write,JsonObject Response,JsonObject ReadRate,JsonObject WriteRate);
    static Activity All(Func<JsonObject> make)=>new(make(),make(),make(),make(),make(),make());

    /// <summary>Busy percent, read/write/overall response time and read/write throughput of the disk behind the mount point.</summary>
    public Activity Sample(string mount)
    {
        var none=All(Unavailable);
        if(!states.TryGetValue(mount,out var s)){s=new State();states[mount]=s;}
        if(s.Handle is null||s.Handle.IsInvalid||s.Handle.IsClosed)
        {
            // A failed open is only retried once a minute.
            if(s.Tried!=default&&DateTimeOffset.UtcNow-s.Tried<TimeSpan.FromSeconds(60))return none;
            s.Tried=DateTimeOffset.UtcNow;s.HavePrevious=false;
            var number=PhysicalNumber(mount);
            if(number is null)return none;
            s.Handle=CreateFile(@"\\.\PhysicalDrive"+number,0,3,IntPtr.Zero,3,0,IntPtr.Zero);
            if(s.Handle.IsInvalid)return none;
        }
        var data=new byte[88];
        if(!DeviceIoControl(s.Handle,IoctlDiskPerformance,null,0,data,(uint)data.Length,out var size,IntPtr.Zero)||size<64)
        {
            s.Handle.Dispose();s.Handle=null;s.HavePrevious=false;return none;
        }
        long bytesRead=BitConverter.ToInt64(data,0),bytesWritten=BitConverter.ToInt64(data,8),readTime=BitConverter.ToInt64(data,16),writeTime=BitConverter.ToInt64(data,24),idleTime=BitConverter.ToInt64(data,32),query=BitConverter.ToInt64(data,56);
        uint readCount=BitConverter.ToUInt32(data,40),writeCount=BitConverter.ToUInt32(data,44);
        var result=All(()=>Model.Missing("warming_up"));
        if(s.HavePrevious&&query>s.QueryTime&&readTime>=s.ReadTime&&writeTime>=s.WriteTime&&idleTime>=s.IdleTime&&bytesRead>=s.BytesRead&&bytesWritten>=s.BytesWritten)
        {
            var interval=(double)(query-s.QueryTime);
            // Active time is the share of the interval in which at least one request was outstanding.
            result=result with{Busy=Model.Metric(Math.Clamp(100.0*(1.0-(idleTime-s.IdleTime)/interval),0,100),"disk_perf_unavailable")};
            uint reads=unchecked(readCount-s.ReadCount),writes=unchecked(writeCount-s.WriteCount);
            // Times are in 100 ns units; a disk that served no request of that kind has no response time to report.
            result=result with{
                Read=reads>0?Model.Metric((readTime-s.ReadTime)/(double)reads/10000.0,"disk_perf_unavailable"):Model.Missing("disk_no_io"),
                Write=writes>0?Model.Metric((writeTime-s.WriteTime)/(double)writes/10000.0,"disk_perf_unavailable"):Model.Missing("disk_no_io"),
                Response=(ulong)reads+writes>0?Model.Metric(((readTime-s.ReadTime)+(writeTime-s.WriteTime))/((double)reads+writes)/10000.0,"disk_perf_unavailable"):Model.Missing("disk_no_io"),
                ReadRate=Model.Metric((bytesRead-s.BytesRead)*10000000.0/interval,"disk_perf_unavailable"),
                WriteRate=Model.Metric((bytesWritten-s.BytesWritten)*10000000.0/interval,"disk_perf_unavailable")};
        }
        s.HavePrevious=true;s.ReadTime=readTime;s.WriteTime=writeTime;s.IdleTime=idleTime;s.QueryTime=query;s.BytesRead=bytesRead;s.BytesWritten=bytesWritten;s.ReadCount=readCount;s.WriteCount=writeCount;
        return result;
    }
    public void Dispose(){foreach(var s in states.Values)s.Handle?.Dispose();states.Clear();}
}
