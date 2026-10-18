using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json.Nodes;

namespace GsoStatus;

// Read only the selected RTSS process. Frames stay in a pipe, never a capture file.
sealed class PresentMon : IDisposable
{
    readonly string executable;
    readonly object gate=new();
    Process? child;
    Task? reader;
    uint target;
    string? name;
    long retryAt,received;
    double? latency;
    string reason="presentmon_waiting_for_game";
    bool disposed;
    string? session;
    readonly CancellationTokenSource stop=new();
    readonly Task worker;
    uint desiredPid;
    string? desiredName;
    bool desiredRendering;
    internal const string Digest="b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af";
    public PresentMon(string? executable=null){this.executable=executable??Path.Combine(AppContext.BaseDirectory,"PresentMon-2.6.0-x64.exe");worker=Task.Run(Observe);}
    public JsonObject Sample(uint pid,string? process,bool rendering)
    {
        lock(gate){desiredPid=pid;desiredName=process;desiredRendering=rendering;
            if(!rendering||pid==0)return Model.Missing("game_not_rendering");
            return target==pid&&name==process&&latency.HasValue&&Stopwatch.GetElapsedTime(received).TotalSeconds<=3?Model.Metric(latency):Model.Missing(target==pid?reason:"presentmon_waiting_for_frames");}
    }
    async Task Observe()
    {
        try{while(!stop.IsCancellationRequested){uint pid;string? process;bool rendering;lock(gate){pid=desiredPid;process=desiredName;rendering=desiredRendering;}Capture(pid,process,rendering);await Task.Delay(200,stop.Token);}}
        catch(OperationCanceledException)when(stop.IsCancellationRequested){}
        finally{Stop();}
    }
    JsonObject Capture(uint pid,string? process,bool rendering)
    {
        if(!rendering||pid==0){Stop();return Model.Missing("game_not_rendering");}
        if(target!=pid||name!=process){Stop();lock(gate){target=pid;name=process;}retryAt=0;}
        if(child is not null&&child.HasExited){long retry=retryAt;Stop();lock(gate){target=pid;name=process;}retryAt=retry;reason="presentmon_capture_exited";}
        if(child is null&&Stopwatch.GetTimestamp()>=retryAt)Start();
        lock(gate){return latency.HasValue&&Stopwatch.GetElapsedTime(received).TotalSeconds<=3?Model.Metric(latency):Model.Missing(reason);}
    }
    void Start()
    {
        retryAt=Stopwatch.GetTimestamp()+30*Stopwatch.Frequency;
        try{
            if(!File.Exists(executable)){reason="presentmon_not_installed";return;}
            using(var stream=File.OpenRead(executable))if(!Convert.ToHexString(SHA256.HashData(stream)).Equals(Digest,StringComparison.OrdinalIgnoreCase)){reason="presentmon_hash_mismatch";return;}
            var info=new ProcessStartInfo(executable){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};
            session="GSO-STATUS-"+Environment.ProcessId+"-"+Guid.NewGuid().ToString("N");
            foreach(var arg in new[]{"--process_id",target.ToString(CultureInfo.InvariantCulture),"--output_stdout","--no_console_stats","--no_track_input","--qpc_time","--session_name",session,"--terminate_on_proc_exit"})info.ArgumentList.Add(arg);
            var running=Process.Start(info)!;child=running;reason="presentmon_waiting_for_frames";
            // Discard diagnostics without retaining process names, paths or raw frames.
            running.ErrorDataReceived+=(_,_)=>{};running.BeginErrorReadLine();
            uint expected=target;string expectedName=name??"";
            reader=Task.Run(async()=>{
                try{
                    string[]? columns=null;var buffer=new char[4096];var pending=new System.Text.StringBuilder(4096);
                    int count;
                    while((count=await running.StandardOutput.ReadAsync(buffer))>0){
                        for(int i=0;i<count;i++){
                            char ch=buffer[i];if(ch!='\n'){if(pending.Length>=4096)throw new InvalidDataException();pending.Append(ch);continue;}
                            string line=pending.ToString().TrimEnd('\r');pending.Clear();
                            if(line.StartsWith("Application,ProcessID,",StringComparison.Ordinal)){columns=line.Split(',');continue;}
                            if(columns is null)continue;
                            if(TryFrame(columns,line,expected,expectedName,Stopwatch.GetTimestamp(),Stopwatch.Frequency,out double value))lock(gate){if(target==expected&&!disposed){latency=value;received=Stopwatch.GetTimestamp();reason="presentmon_frames_stale";}}
                        }
                    }
                }catch{lock(gate){if(target==expected)reason="presentmon_capture_failed";}}
                finally{lock(gate){if(target==expected)latency=null;}}
            });
        }catch{reason="presentmon_start_failed";}
    }
    internal static bool TryFrame(string[] columns,string line,uint pid,string name,long now,long frequency,out double latency)
    {
        latency=0;if(line.Length>4096)return false;var values=line.Split(',');if(values.Length!=columns.Length)return false;
        string Field(string key){int i=Array.IndexOf(columns,key);return i<0?"":values[i].Trim('"');}
        return uint.TryParse(Field("ProcessID"),out uint actual)&&actual==pid&&Field("Application").Equals(name,StringComparison.OrdinalIgnoreCase)
            &&long.TryParse(Field("TimeInQPC"),out long stamp)&&stamp>0&&stamp<=now&&(double)(now-stamp)/frequency<=3
            &&double.TryParse(Field("MsRenderPresentLatency"),NumberStyles.Float,CultureInfo.InvariantCulture,out latency)&&double.IsFinite(latency)&&latency>0&&latency<=10000;
    }
    void Stop()
    {
        lock(gate){target=0;latency=null;name=null;}
        var old=child;child=null;
        if(old is not null){try{if(!old.HasExited){old.Kill(true);old.WaitForExit(1000);}}catch{}try{reader?.Wait(1000);}catch{}old.Dispose();}
        reader=null;
        if(session is not null){try{var info=new ProcessStartInfo(executable){UseShellExecute=false,CreateNoWindow=true};info.ArgumentList.Add("--session_name");info.ArgumentList.Add(session);info.ArgumentList.Add("--terminate_existing_session");using var cleanup=Process.Start(info);if(cleanup is not null&&!cleanup.WaitForExit(1000))cleanup.Kill(true);}catch{}session=null;}
    }
    public void Dispose(){disposed=true;stop.Cancel();try{worker.GetAwaiter().GetResult();}finally{stop.Dispose();}}
}
