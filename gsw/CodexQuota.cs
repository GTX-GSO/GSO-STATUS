using System.Diagnostics;
using System.Text;
using System.Text.Json.Nodes;

namespace GsoStatus;

internal sealed record LocalQuotaSnapshot(DateTimeOffset ObservedAt,JsonArray Windows);

// Reuse fresh local quota events; read-only stdio RPC is the fallback, never model work.
sealed class CodexQuota : IAsyncDisposable
{
    readonly CancellationTokenSource stop = new();
    readonly object gate = new();
    readonly Task task;
    JsonArray windows = new();
    DateTimeOffset? observed;
    long observedTicks;
    string? error = "quota_adapter_not_configured";
    readonly string executable;
    readonly Func<LocalQuotaSnapshot?>? localQuota;
    DateTimeOffset nextRpc;
    public CodexQuota(string executable,Func<LocalQuotaSnapshot?>? localQuota=null)
    {
        this.executable=executable;this.localQuota=localQuota;
        if(executable!=""||localQuota is not null)error="waiting_for_quota";
        task=string.IsNullOrEmpty(executable)&&localQuota is null?Task.CompletedTask:Task.Run(Loop);
    }
    public JsonObject Snapshot()
    {
        lock(gate)
        {
            var source = new JsonObject
            {
                ["quality"]=observed is null?(executable==""&&localQuota is null?"unconfigured":"error"):(error is null?"ok":"error"),
                ["observed_at"]=observed?.ToString("O"), ["source_updated_at"]=null,
                ["age_ms"]=observed is null?null:JsonValue.Create((long)Stopwatch.GetElapsedTime(observedTicks).TotalMilliseconds),
                ["last_error_code"]=error
            };
            // Quota health is independent of lifecycle inference.
            return new JsonObject { ["source"]=source,["state"]="unknown",["quotas"]=windows.DeepClone(),["sessions"]=new JsonArray() };
        }
    }
    internal static JsonArray ParseWindows(JsonObject result)
    {
        JsonObject? buckets=result["rateLimitsByLimitId"] as JsonObject;
        if(buckets is null && result["rateLimits"] is JsonObject legacy)
            buckets=new JsonObject{[legacy["limitId"]?.GetValue<string>()??"legacy"]=legacy.DeepClone()};
        var output=new JsonArray();var seen=new HashSet<string>();if(buckets is null)return output;
        foreach(var pair in buckets)
        {
            if(pair.Value is not JsonObject bucket || pair.Key.Length is 0 or >160)continue;
            foreach(var key in new[]{"primary","secondary"})
            {
                if(bucket[key] is not JsonObject window)continue;
                if(!int.TryParse(window["windowDurationMins"]?.ToJsonString(),out var minutes)||minutes<1)continue;
                if(!seen.Add(pair.Key+":"+minutes))continue;
                double? remain=null;
                if(double.TryParse(window["usedPercent"]?.ToJsonString(),System.Globalization.NumberStyles.Float,System.Globalization.CultureInfo.InvariantCulture,out var used)&&double.IsFinite(used)&&used>=0&&used<=100)remain=100-used;
                string? reset=null;
                if(long.TryParse(window["resetsAt"]?.ToJsonString(),out var timestamp))
                    try{reset=DateTimeOffset.FromUnixTimeSeconds(timestamp).ToString("O");}catch(ArgumentOutOfRangeException){}
                output.Add(new JsonObject{["bucket"]=pair.Key,["window_duration_mins"]=minutes,["remaining_percent"]=Model.Metric(remain,"quota_value_unavailable"),["resets_at"]=reset});
                if(output.Count==8)return output;
            }
        }
        return output;
    }
    internal static JsonArray ParseEventWindows(JsonObject limits)
    {
        // Explicit bucket and units only; never assume another limit is the Codex bucket.
        if(limits["limit_id"] is not JsonValue id||!id.TryGetValue<string>(out var bucket)||string.IsNullOrWhiteSpace(bucket)||bucket.Length>160)return new JsonArray();
        var mapped=new JsonObject();foreach(string key in new[]{"primary","secondary"})if(limits[key] is JsonObject window)mapped[key]=new JsonObject{["usedPercent"]=window["used_percent"]?.DeepClone(),["windowDurationMins"]=window["window_minutes"]?.DeepClone(),["resetsAt"]=window["resets_at"]?.DeepClone()};
        return ParseWindows(new JsonObject{["rateLimitsByLimitId"]=new JsonObject{[bucket]=mapped}});
    }
    bool AdoptLocal()
    {
        var candidate=localQuota?.Invoke();if(candidate is null||candidate.Windows.Count==0)return false;
        var now=DateTimeOffset.UtcNow;var age=now-candidate.ObservedAt;if(age<TimeSpan.Zero||age>TimeSpan.FromSeconds(90))return false;
        lock(gate){
            if(observed is null||candidate.ObservedAt>observed){windows=(JsonArray)candidate.Windows.DeepClone();observed=candidate.ObservedAt;observedTicks=Stopwatch.GetTimestamp()-(long)(age.TotalSeconds*Stopwatch.Frequency);error=null;}
        }
        return true;
    }
    internal static string ResolveExecutable(string configured,string? installRoot=null)
    {
        if(Path.IsPathFullyQualified(configured)&&File.Exists(configured)&&configured.EndsWith(".exe",StringComparison.OrdinalIgnoreCase))return configured;
        // Desktop updates rotate the version directory; search only the installed product root.
        var root=installRoot??Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"OpenAI","Codex","bin");
        try{return Directory.EnumerateDirectories(root).Select(d=>Path.Combine(d,"codex.exe")).Where(File.Exists).OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault()??"";}catch(IOException){return "";}catch(UnauthorizedAccessException){return "";}
    }
    async Task Loop()
    {
        // Give bounded JSONL backfill time to find current quota events before spawning RPC.
        if(localQuota is not null){for(int n=0;n<40&&!stop.IsCancellationRequested;n++){if(AdoptLocal())break;try{await Task.Delay(1000,stop.Token);}catch(OperationCanceledException){return;}}}
        while(!stop.IsCancellationRequested)
        {
            if(AdoptLocal()||DateTimeOffset.UtcNow<nextRpc||string.IsNullOrEmpty(executable)){
                try{await Task.Delay(1000,stop.Token);}catch(OperationCanceledException){}continue;
            }
            nextRpc=DateTimeOffset.UtcNow.AddSeconds(60);
            Process? process=null;
            try
            {
                var resolved=ResolveExecutable(executable);if(resolved=="")throw new InvalidDataException("quota_executable_missing");
                process=new Process{StartInfo=new ProcessStartInfo(resolved){UseShellExecute=false,CreateNoWindow=true,RedirectStandardInput=true,RedirectStandardOutput=true,RedirectStandardError=true}};
                process.StartInfo.ArgumentList.Add("app-server");process.StartInfo.ArgumentList.Add("--stdio");
                if(!process.Start())throw new IOException();
                var stderr=Drain(process.StandardError,stop.Token);var reader=new BoundedLines(process.StandardOutput);
                async Task<JsonObject> Request(int id,string method,JsonObject? parameters=null)
                {
                    using var timeout=CancellationTokenSource.CreateLinkedTokenSource(stop.Token);timeout.CancelAfter(TimeSpan.FromSeconds(12));
                    var request=new JsonObject{["id"]=id,["method"]=method};if(parameters!=null)request["params"]=parameters;
                    await process.StandardInput.WriteLineAsync(request.ToJsonString().AsMemory(),timeout.Token);await process.StandardInput.FlushAsync(timeout.Token);
                    for(int n=0;n<64;n++)
                    {
                        var line=await reader.Read(timeout.Token);var response=JsonNode.Parse(line) as JsonObject;
                        if(response?["id"]?.ToJsonString()!=id.ToString())continue;
                        if(response["result"] is not JsonObject result){var message=response["error"]?["message"]?.GetValue<string>()?.ToLowerInvariant()??"";throw new InvalidDataException(message.Contains("auth")||message.Contains("login")||message.Contains("logged in")?"quota_auth_failed":"rpc_failed");}return result;
                    }
                    throw new InvalidDataException("rpc_response_missing");
                }
                await Request(1,"initialize",new JsonObject{["clientInfo"]=new JsonObject{["name"]="gso_status_win_readonly",["title"]="GSO STATUS WIN",["version"]=typeof(CodexQuota).Assembly.GetName().Version?.ToString(3)??"unknown"},["capabilities"]=new JsonObject{["experimentalApi"]=false}});
                await process.StandardInput.WriteLineAsync("{\"method\":\"initialized\"}");await process.StandardInput.FlushAsync();
                var result=await Request(2,"account/rateLimits/read");var parsed=ParseWindows(result);
                if(parsed.Count==0)throw new InvalidDataException("quota_windows_missing");
                lock(gate){windows=parsed;observed=DateTimeOffset.UtcNow;observedTicks=Stopwatch.GetTimestamp();error=null;}
                // Close this private process immediately; retain only bounded quota metadata.
            }
            catch(OperationCanceledException)when(stop.IsCancellationRequested){}
            catch(Exception ex){lock(gate){error=ex is OperationCanceledException?"quota_rpc_timeout":ex.Message=="quota_auth_failed"?"quota_auth_failed":ex.Message=="quota_executable_missing"?"quota_executable_missing":"quota_rpc_unavailable";}}
            finally
            {
                if(process is not null)
                {
                    try{process.StandardInput.Close();using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(2));await process.WaitForExitAsync(timeout.Token);}catch{try{process.Kill(true);}catch{}}
                    process.Dispose();
                }
            }
            try{await Task.Delay(TimeSpan.FromSeconds(1),stop.Token);}catch(OperationCanceledException){}
        }
    }
    static async Task Drain(StreamReader reader,CancellationToken token){try{var buffer=new char[1024];while(await reader.ReadAsync(buffer.AsMemory(),token)>0){}}catch(OperationCanceledException){}catch(IOException){}catch(ObjectDisposedException){}}
    sealed class BoundedLines(StreamReader reader)
    {
        readonly char[] buffer=new char[4096];int position,length;
        public async Task<string> Read(CancellationToken token)
        {
            var line=new StringBuilder();
            for(;;)
            {
                if(position==length){length=await reader.ReadAsync(buffer.AsMemory(),token);position=0;if(length==0)throw new EndOfStreamException();}
                char ch=buffer[position++];if(ch=='\n')return line.ToString();line.Append(ch);if(line.Length>65536)throw new InvalidDataException("quota_response_too_large");
            }
        }
    }
    public async ValueTask DisposeAsync(){stop.Cancel();await task;stop.Dispose();}
}
