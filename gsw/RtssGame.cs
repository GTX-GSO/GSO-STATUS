using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Text;
using System.Text.Json.Nodes;
using System.Runtime.InteropServices;

namespace GsoStatus;

// The RTSS SDK provides a continuously measured interval, not a process-count FPS estimate.
sealed class RtssGame : IDisposable
{
    internal static bool IsDesktopApp(string name)=>new[]{"WeChat.exe","Weixin.exe","WeChatAppEx.exe","WeChatApp.exe","Typora.exe","Code.exe","Codex.exe","explorer.exe","chrome.exe","msedge.exe","firefox.exe"}.Contains(name,StringComparer.OrdinalIgnoreCase);
    readonly Func<uint,bool> fullscreen;
    [StructLayout(LayoutKind.Sequential)] struct Rect {public int Left,Top,Right,Bottom;}
    [StructLayout(LayoutKind.Sequential)] struct Point {public int X,Y;}
    [StructLayout(LayoutKind.Sequential)] struct MonitorInfo {public int Size;public Rect Monitor,Work;public uint Flags;}
    delegate bool EnumWindow(IntPtr window,IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindow callback,IntPtr data);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window,out Rect rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr window,ref Point point);
    [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] static extern IntPtr GetWindowLongPtr(IntPtr window,int index);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromWindow(IntPtr window,uint flags);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern bool GetMonitorInfo(IntPtr monitor,ref MonitorInfo info);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    internal static bool FullscreenBounds(int x,int y,int width,int height,int left,int top,int right,int bottom,long style)=>
        width>0&&height>0&&(style&0x00c40000)==0&&Math.Abs((long)x-left)<=2&&Math.Abs((long)y-top)<=2&&Math.Abs((long)x+width-right)<=2&&Math.Abs((long)y+height-bottom)<=2;
    static bool HasFullscreenWindow(uint pid){
        bool found=false;
        var previousDpi=SetThreadDpiAwarenessContext(new IntPtr(-4));
        try{EnumWindows((window,_)=>{
            GetWindowThreadProcessId(window,out uint owner);if(owner!=pid||!IsWindowVisible(window)||IsIconic(window)||!GetClientRect(window,out var rect))return true;
            var origin=new Point();if(!ClientToScreen(window,ref origin))return true;
            var monitor=new MonitorInfo{Size=Marshal.SizeOf<MonitorInfo>()};if(!GetMonitorInfo(MonitorFromWindow(window,2),ref monitor))return true;
            found=FullscreenBounds(origin.X,origin.Y,rect.Right-rect.Left,rect.Bottom-rect.Top,monitor.Monitor.Left,monitor.Monitor.Top,monitor.Monitor.Right,monitor.Monitor.Bottom,GetWindowLongPtr(window,-16).ToInt64());
            return !found;
        },IntPtr.Zero);}finally{if(previousDpi!=IntPtr.Zero)SetThreadDpiAwarenessContext(previousDpi);}
        return found;
    }
    readonly string[] allowed;
    readonly string mappingName;
    readonly bool startOnDemand;
    readonly bool autoDetect;
    readonly HashSet<string> excluded;
    readonly bool hideOverlay;
    Process? overlayHelper;
    long overlayHelperStarted;
    int overlayServerPid;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate uint SetFlags(uint mask,uint xor);

    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    readonly Queue<double> history=new();
    readonly PresentMon presentMon=new();
    uint iconPid;string? icon,iconProcess;long iconRetry;
    DateTimeOffset lastLaunch=DateTimeOffset.MinValue;
    DateTimeOffset? observed;
    long observedTicks;
    uint lastEnd,lastPid;
    string? current;
    double? fps;
    public RtssGame(string[] allowed,bool startOnDemand,bool autoDetect,string[] excluded,bool hideOverlay,string mappingName="RTSSSharedMemoryV2",Func<uint,bool>? fullscreen=null){this.mappingName=mappingName;this.allowed=allowed;this.startOnDemand=startOnDemand;this.autoDetect=autoDetect;this.hideOverlay=hideOverlay;this.excluded=new HashSet<string>(excluded,StringComparer.OrdinalIgnoreCase);this.fullscreen=fullscreen??HasFullscreenWindow;}
    public JsonObject Sample()
    {
        var result=SampleRtss();uint pid=result["pid"]?.GetValue<uint>()??0;bool rendering=result["rendering"]?.GetValue<bool>()==true;
        string? process=result["process_name"]?.GetValue<string>();
        if(pid!=iconPid||process!=iconProcess||(pid!=0&&icon is null&&Stopwatch.GetTimestamp()>=iconRetry)){iconPid=pid;iconProcess=process;icon=pid==0?null:GameIcon.Read(pid);iconRetry=Stopwatch.GetTimestamp()+30*Stopwatch.Frequency;}
        result["icon_argb8888"]=icon;result["render_latency_ms"]=presentMon.Sample(pid,result["process_name"]?.GetValue<string>(),rendering);return result;
    }
    JsonObject SampleRtss()
    {
        var empty=new JsonObject{["source"]=Model.Source("unconfigured","game_allowlist_empty"),["process_name"]=null,["pid"]=null,["rendering"]=null,["fps"]=Model.Missing("game_not_observed"),["render_latency_ms"]=Model.Missing("presentmon_not_connected"),["fps_history"]=new JsonArray()};
        if(allowed.Length==0&&!autoDetect)return empty;
        EnsureStarted();
        HideOverlay();
        var processes=new Dictionary<int,string>();
        foreach(var name in allowed)
            foreach(var process in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(name)))
                using(process){processes.TryAdd(process.Id,name);}
        GetWindowThreadProcessId(GetForegroundWindow(),out uint foregroundPid);
        if(!autoDetect&&processes.Count==0)
        {
            current=null;lastPid=lastEnd=0;observed=null;fps=null;history.Clear();
            empty["source"]=Model.Source("ok");empty["rendering"]=false;empty["fps"]=Model.Missing("no_allowed_game_running");return empty;
        }
        string? code=null;
        try
        {
            using var map=MemoryMappedFile.OpenExisting(mappingName,MemoryMappedFileRights.Read);
            using var header=map.CreateViewAccessor(0,20,MemoryMappedFileAccess.Read);
            if(header.ReadUInt32(0)!=0x52545353||(header.ReadUInt32(4)>>16)!=2)throw new InvalidDataException();
            uint size=header.ReadUInt32(8),offset=header.ReadUInt32(12),count=header.ReadUInt32(16);
            if(size<284||size>65536||offset<20||offset>67108864||count>256||(long)offset+(long)size*count>67108864)throw new InvalidDataException();
            using var entries=map.CreateViewAccessor(offset,(long)size*count,MemoryMappedFileAccess.Read);
            bool found=false;var bytes=new byte[284];uint selectedPid=0,selectedEnd=0;double selectedRate=0;string? selectedName=null;int bestRank=int.MaxValue;
            for(uint i=0;i<count;i++)
            {
                long at=(long)i*size;uint pid=entries.ReadUInt32(at);if(pid==0)continue;
                string? expected;
                if(allowed.Length==0&&autoDetect){
                    try{using var process=Process.GetProcessById((int)pid);expected=process.ProcessName+".exe";if(excluded.Contains(expected)||IsDesktopApp(expected))continue;}
                    catch(ArgumentException){continue;}catch(InvalidOperationException){continue;}catch(System.ComponentModel.Win32Exception){continue;}
                }else if(!processes.TryGetValue((int)pid,out expected))continue;
                if(IsDesktopApp(expected)||excluded.Contains(expected)||!fullscreen(pid))continue;
                entries.ReadArray(at,bytes,0,bytes.Length);
                var name=Encoding.ASCII.GetString(bytes,4,Array.IndexOf(bytes,(byte)0,4,260) is var end&&end>=4?end-4:260);
                if(!Path.GetFileName(name).Equals(expected,StringComparison.OrdinalIgnoreCase)||pid!=entries.ReadUInt32(at))continue;
                uint time0=BitConverter.ToUInt32(bytes,268),time1=BitConverter.ToUInt32(bytes,272),frames=BitConverter.ToUInt32(bytes,276);
                if(time1!=entries.ReadUInt32(at+272)||!TryInterval(time0,time1,frames,unchecked((uint)Environment.TickCount),out var rate))continue;
                int rank=pid==foregroundPid?0:pid==lastPid?1:2;
                if(rank<bestRank){bestRank=rank;selectedPid=pid;selectedEnd=time1;selectedRate=rate;selectedName=expected;found=true;}
            }
            if(found&&(selectedPid!=lastPid||selectedEnd!=lastEnd)){
                if(selectedPid!=lastPid)history.Clear();lastPid=selectedPid;lastEnd=selectedEnd;current=selectedName;fps=selectedRate;observed=DateTimeOffset.UtcNow;observedTicks=Stopwatch.GetTimestamp();history.Enqueue(selectedRate);while(history.Count>24)history.Dequeue();
            }
            if(!found&&observed is null){empty["source"]=Model.Source("ok");empty["rendering"]=false;empty["fps"]=Model.Missing("no_rendering_game");return empty;}
            if(!found)code="rtss_valid_game_sample_missing";
        }
        catch(FileNotFoundException){code="rtss_mapping_missing";EnsureStarted();}
        catch(UnauthorizedAccessException){code="rtss_access_denied";}
        catch{code="rtss_layout_invalid";}
        var age=observed is null?(long?)null:(long)Stopwatch.GetElapsedTime(observedTicks).TotalMilliseconds;
        var quality=observed is null?"error":age>10000?"offline":age>3000?"stale":code is null?"ok":"error";
        empty["source"]=new JsonObject{["quality"]=quality,["observed_at"]=observed?.ToString("O"),["source_updated_at"]=null,["age_ms"]=age,["last_error_code"]=code};
        empty["process_name"]=current;empty["pid"]=lastPid==0?null:JsonValue.Create(lastPid);empty["rendering"]=quality=="ok";
        empty["fps"]=Model.Metric(fps,"rtss_valid_game_sample_missing");var points=new JsonArray();foreach(var point in history)points.Add(Model.Metric(point));empty["fps_history"]=points;return empty;
    }
    void HideOverlay()
    {
        if(!hideOverlay)return;
        try{
            if(overlayHelper is not null){
                if(!overlayHelper.HasExited&&Stopwatch.GetElapsedTime(overlayHelperStarted).TotalSeconds<2)return;
                if(!overlayHelper.HasExited)overlayHelper.Kill(true);
                overlayHelper.Dispose();overlayHelper=null;
            }
            var running=Process.GetProcessesByName("RTSS");
            int pid;try{if(running.Length==0){overlayServerPid=0;return;}pid=running[0].Id;}finally{foreach(var p in running)p.Dispose();}
            if(pid==overlayServerPid)return;
            overlayServerPid=pid;
            overlayHelper=Process.Start(new ProcessStartInfo(Environment.ProcessPath!,"--hide-rtss-overlay"){UseShellExecute=false,CreateNoWindow=true,WindowStyle=ProcessWindowStyle.Hidden});
            overlayHelperStarted=Stopwatch.GetTimestamp();
        }catch{ /* Overlay configuration cannot block sampling. */ }
    }
    internal static int HideOverlayOnce()
    {
        IntPtr library=IntPtr.Zero;
        try{
            var path=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),@"RivaTuner Statistics Server\RTSSHooks64.dll");
            library=NativeLibrary.Load(path);
            var set=Marshal.GetDelegateForFunctionPointer<SetFlags>(NativeLibrary.GetExport(library,"SetFlags"));
            set(0xfffffffe,0);return 0;
        }catch{return 1;}
        finally{if(library!=IntPtr.Zero)NativeLibrary.Free(library);}
    }
    public void Dispose(){presentMon.Dispose();if(overlayHelper is not null){try{if(!overlayHelper.HasExited)overlayHelper.Kill(true);}catch{}overlayHelper.Dispose();overlayHelper=null;}}
    void EnsureStarted()
    {
        if(!startOnDemand||DateTimeOffset.UtcNow-lastLaunch<TimeSpan.FromSeconds(30))return;
        lastLaunch=DateTimeOffset.UtcNow;
        var executable=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),@"RivaTuner Statistics Server\RTSS.exe");
        if(!File.Exists(executable))return;
        var existing=Process.GetProcessesByName("RTSS");try{if(existing.Length>0)return;}finally{foreach(var p in existing)p.Dispose();}
        using var child=Process.Start(new ProcessStartInfo(executable){UseShellExecute=false,CreateNoWindow=true,WindowStyle=ProcessWindowStyle.Hidden,WorkingDirectory=Path.GetDirectoryName(executable)});
    }
    internal static bool TryInterval(uint start,uint end,uint frames,uint now,out double fps)
    {
        fps=0;uint duration=unchecked(end-start),age=unchecked(now-end);
        if(start==0||duration==0||duration>5000||age>3000||frames==0)return false;
        fps=1000.0*frames/duration;return fps<=100000;
    }
}
