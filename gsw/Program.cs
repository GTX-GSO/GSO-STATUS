using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GsoStatus;

sealed record Config
{
    public string SnapshotBaseUrl { get; init; } = "http://127.0.0.1:17890";
    public string AgentId { get; init; } = "desktop";
    public string WriteToken { get; init; } = "";
    public int GpuIndex { get; init; }
    public string CodexExecutable { get; init; } = "";
    public bool ObserveCodexTasks { get; init; }
    public bool ObserveClaudeTasks { get; init; } = true;
    public string[] ClaudeProjectDirectories { get; init; } = [];
    public string ClaudeRateLimitsFile { get; init; } = "";
    public bool ClaudeUsageApi { get; init; }
    public string[] GameExecutables { get; init; } = [];
    public bool StartRtssOnDemand { get; init; } = true;
    public bool AutoDetectGames { get; init; } = true;
    public bool HideRtssOverlay { get; init; } = true;
    public string[] ExcludedGameExecutables { get; init; } = ["chrome.exe","msedge.exe","firefox.exe","ChatGPT.exe","Code.exe","explorer.exe","dwm.exe","GSW.exe","Sidecar-LVGL.exe","RTSS.exe","Discord.exe","steam.exe","steamwebhelper.exe","pwsh.exe","powershell.exe","cmd.exe","WindowsTerminal.exe","ApplicationFrameHost.exe","SearchHost.exe","Taskmgr.exe","WeChat.exe","Weixin.exe","WeChatAppEx.exe","WeChatApp.exe"];
    public string[] Drives { get; init; } = ["C:\\", "D:\\", "E:\\"];
}
static class Program
{
    static readonly CancellationTokenSource Stop = new();
    static string Status = "启动中";
    [STAThread] static int Main(string[] args)
    {
        if(args.Length==1&&args[0]=="--cpu-sensor-worker")return CpuSensorWorker.Run();
        if(args.Length==1&&args[0]=="--cpu-sensor-stop"){CpuSensorClient.StopWorker();return 0;}
        if(args.Length==1&&args[0]=="--hide-rtss-overlay")return RtssGame.HideOverlayOnce();
        string Option(string name, string fallback) { var index=Array.IndexOf(args,name);return index>=0&&index+1<args.Length?args[index+1]:fallback; }
        if(args.Length>=1&&args[0] is "--install-claude-statusline" or "--uninstall-claude-statusline")
        {
            var settings=Option("--settings",ClaudeQuota.DefaultSettingsPath());var exe=Option("--gsw",Environment.ProcessPath??"");
            using var output=new StreamWriter(Console.OpenStandardOutput(),new UTF8Encoding(false));
            if(!Path.IsPathFullyQualified(settings)||!Path.IsPathFullyQualified(exe)){output.WriteLine("路径必须是完整路径");return 2;}
            var (code,message)=ClaudeQuota.ConfigureStatusLine(settings,exe,args[0]=="--install-claude-statusline",args.Contains("--force"));
            output.WriteLine(message);return code;
        }
        if(args.Length>=1&&args[0]=="--claude-statusline")
        {
            // Claude Code status line command; never opens the tray, config or network.
            var bridge=Option("--output",ClaudeQuota.DefaultPath());if(!Path.IsPathFullyQualified(bridge))bridge=ClaudeQuota.DefaultPath();
            using var input=new StreamReader(Console.OpenStandardInput(),new UTF8Encoding(false));
            using var output=new StreamWriter(Console.OpenStandardOutput(),new UTF8Encoding(false));
            try{return ClaudeQuota.RunStatusLine(input,output,bridge);}catch(IOException){return 0;}
        }
        var configPath=Path.GetFullPath(Option("--config",Path.Combine(AppContext.BaseDirectory,"gsw.json")));
        try
        {
            var config=JsonSerializer.Deserialize<Config>(File.ReadAllText(configPath),new JsonSerializerOptions { UnmappedMemberHandling=System.Text.Json.Serialization.JsonUnmappedMemberHandling.Disallow }) ?? throw new InvalidDataException("配置为空");
            if (!Regex.IsMatch(config.AgentId,"^[A-Za-z0-9_-]{1,64}$")||config.WriteToken.Length is <32 or >256||config.WriteToken.StartsWith("REPLACE_",StringComparison.Ordinal)||config.WriteToken.Any(ch=>ch<'!'||ch>'~')||config.GpuIndex<0||config.Drives.Length>3||config.Drives.Distinct(StringComparer.OrdinalIgnoreCase).Count()!=config.Drives.Length) throw new InvalidDataException("请检查AgentId、令牌、GPU索引与最多三个不同盘符");
            if(config.GameExecutables.Length>16||config.GameExecutables.Any(n=>n.Length>160||Path.GetFileName(n)!=n||!n.EndsWith(".exe",StringComparison.OrdinalIgnoreCase)))throw new InvalidDataException("游戏配置必须是最多16个可执行文件名");
            if(config.ClaudeProjectDirectories.Length>8||config.ClaudeProjectDirectories.Any(d=>d.Length is 0 or >260||!Path.IsPathFullyQualified(d))||(config.ClaudeRateLimitsFile!=""&&(config.ClaudeRateLimitsFile.Length>260||!Path.IsPathFullyQualified(config.ClaudeRateLimitsFile))))throw new InvalidDataException("Claude目录与额度文件必须是最多8个完整路径");
            if(config.ExcludedGameExecutables.Length>64||config.ExcludedGameExecutables.Any(n=>n.Length>160||Path.GetFileName(n)!=n))throw new InvalidDataException("排除列表必须是最多64个可执行文件名");
            if (!Uri.TryCreate(config.SnapshotBaseUrl,UriKind.Absolute,out var uri)||uri.Scheme is not ("http" or "https")||uri.UserInfo!=""||uri.Query!=""||uri.Fragment!=""||uri.AbsolutePath!="/") throw new InvalidDataException("SnapshotBaseUrl必须是HTTP(S)服务根地址");
            using var stopEvent=new EventWaitHandle(false,EventResetMode.ManualReset,@"Local\GSOStatusGSW.Stop."+config.AgentId);
            if(args.Contains("--stop")){stopEvent.Set();return 0;}
            using var mutex=new Mutex(true,@"Local\GSOStatusGSW.Run."+config.AgentId,out var ownsMutex);
            if(!ownsMutex)return 2;
            stopEvent.Reset();
            var stopWait=ThreadPool.RegisterWaitForSingleObject(stopEvent,(_,_)=>Stop.Cancel(),null,Timeout.Infinite,true);
            try
            {
                var task=Run(config,Stop.Token);
                try { NativeTray.Run(Stop,task,()=>Status,()=>
                {
                    try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(configPath){UseShellExecute=true}); }
                    catch(Exception ex){Log("open_config_error:"+ex.GetType().Name);}
                }); }
                finally { Stop.Cancel();task.GetAwaiter().GetResult(); }
                return 0;
            }
            finally { stopWait.Unregister(null); }
        }
        catch(Exception ex) { Log("startup_error:"+ex.GetType().Name);NativeTray.ShowError("GSW未启动，请检查本地配置及日志。");return 1; }
    }
    static async Task<int> Run(Config config,CancellationToken token)
    {
        using var collector=new WindowsCollector(config.GpuIndex,config.Drives);
        await using var tasks=new CodexTasks(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),".codex","sessions"),config.ObserveCodexTasks);
        await using var quotas=new CodexQuota(config.CodexExecutable,config.ObserveCodexTasks?tasks.QuotaSnapshot:null);
        await using var claudeTasks=new ClaudeTasks(config.ClaudeProjectDirectories.Length>0?config.ClaudeProjectDirectories:ClaudeTasks.DefaultRoots(),config.ObserveClaudeTasks);
        using var claudeUsage=config.ObserveClaudeTasks&&config.ClaudeUsageApi?new ClaudeUsage(busy:()=>claudeTasks.Snapshot().State=="running"):null;
        claudeUsage?.Start();
        var claudeQuota=config.ObserveClaudeTasks?new ClaudeQuota(config.ClaudeRateLimitsFile==""?ClaudeQuota.DefaultPath():config.ClaudeRateLimitsFile,null,claudeUsage):null;
        using var activity=config.ObserveCodexTasks||config.ObserveClaudeTasks?new ClientActivity(config.ObserveClaudeTasks?(config.ClaudeProjectDirectories.Length>0?config.ClaudeProjectDirectories:ClaudeTasks.DefaultRoots()):[],config.ObserveCodexTasks?Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),".codex","sessions"):""):null;
        using var game=new RtssGame(config.GameExecutables,config.StartRtssOnDemand,config.AutoDetectGames,config.ExcludedGameExecutables,config.HideRtssOverlay);
        using var http=new HttpClient(new HttpClientHandler {AllowAutoRedirect=false,UseProxy=false}) {Timeout=TimeSpan.FromMilliseconds(800),MaxResponseContentBufferSize=32768};
        http.DefaultRequestHeaders.Authorization=new AuthenticationHeaderValue("Bearer",config.WriteToken);
        var boot=Guid.NewGuid().ToString(); long seq=0;string lastStatus="";bool sessionMetadata=false,hardwareClocks=false,gameIcon=false,vibeClients=false,diskActivity=false,diskThroughput=false;
        using var timer=new PeriodicTimer(TimeSpan.FromSeconds(1));
        try
        {
            do
            {
                try
                {
                    var telemetry=Model.Telemetry(boot,seq++,collector.Sample());Vibe.Observation? codexObservation=null,claudeObservation=null;if(config.ObserveCodexTasks){var observation=tasks.Snapshot();codexObservation=new(observation.State,observation.Sessions,tasks.Recency());}if(config.ObserveClaudeTasks){var observation=claudeTasks.Snapshot();claudeObservation=new(observation.State,observation.Sessions,claudeTasks.Recency());}var codex=Vibe.Merge(quotas.Snapshot(),codexObservation,claudeQuota?.Snapshot(),claudeObservation);telemetry["codex"]=sessionMetadata?codex:Model.LegacyCodex(codex);if(vibeClients&&activity is not null)telemetry["codex"]!["clients"]=activity.Snapshot();telemetry["game"]=game.Sample();if(!gameIcon)telemetry["game"]!.AsObject().Remove("icon_argb8888");if(telemetry["windows"]?["disks"] is System.Text.Json.Nodes.JsonArray diskList)foreach(var entry in diskList)if(entry is System.Text.Json.Nodes.JsonObject disk){if(!diskThroughput)foreach(var field in new[]{"response_ms","read_bytes_per_second","write_bytes_per_second"})disk.Remove(field);if(!diskActivity)foreach(var field in new[]{"busy_percent","read_ms","write_ms"})disk.Remove(field);}if(!hardwareClocks&&telemetry["windows"] is System.Text.Json.Nodes.JsonObject win)foreach(var key in new[]{"cpu","gpu"})if(win[key] is System.Text.Json.Nodes.JsonObject h)foreach(var field in new[]{"clock_mhz","memory_clock_mhz","memory_rate_mts","memory_clock_source"})h.Remove(field);var data=telemetry.ToJsonString();
                    using var body=new StringContent(data,Encoding.UTF8,"application/json");
                    using var response=await http.PostAsync(config.SnapshotBaseUrl.TrimEnd('/')+"/api/v1/agents/"+config.AgentId+"/telemetry",body,token);
                    sessionMetadata=response.Headers.TryGetValues("X-GSO-Session-Metadata",out var features)&&features.Contains("1");
                    hardwareClocks=response.Headers.TryGetValues("X-GSO-Hardware-Clocks",out var clockFeatures)&&clockFeatures.Contains("1");
                    gameIcon=response.Headers.TryGetValues("X-GSO-Game-Icon",out var iconFeatures)&&iconFeatures.Contains("1");
                    diskActivity=response.Headers.TryGetValues("X-GSO-Disk-Activity",out var diskFeatures)&&diskFeatures.Contains("1");
                    diskThroughput=response.Headers.TryGetValues("X-GSO-Disk-Throughput",out var throughputFeatures)&&throughputFeatures.Contains("1");
                    vibeClients=response.Headers.TryGetValues("X-GSO-Vibe-Clients",out var clientFeatures)&&clientFeatures.Contains("1");
                    Status=response.IsSuccessStatusCode?"已连接":"上报失败 HTTP "+(int)response.StatusCode;
                }
                catch(OperationCanceledException) when(!token.IsCancellationRequested) {Status="连接超时";}
                catch(HttpRequestException){Status="服务离线";}
                catch(Exception ex) when(ex is not OperationCanceledException) {Status="采集失败 "+ex.GetType().Name;}
                if(Status!=lastStatus){Log(Status);lastStatus=Status;}
            }while(await timer.WaitForNextTickAsync(token));
        }
        catch(OperationCanceledException)when(token.IsCancellationRequested) { }
        return Status=="已连接"||token.IsCancellationRequested?0:2;
    }
    static void Log(string message)
    {
        try {
            var directory=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"GSO STATUS WIN","logs");Directory.CreateDirectory(directory);
            var path=Path.Combine(directory,"gsw.log");
            if(File.Exists(path)&&new FileInfo(path).Length>1024*1024){if(File.Exists(path+".1"))File.Move(path+".1",path+".2",true);File.Move(path,path+".1",true);}
            File.AppendAllText(path,DateTimeOffset.Now.ToString("O")+" "+message+Environment.NewLine);
        }catch { }
    }
}
