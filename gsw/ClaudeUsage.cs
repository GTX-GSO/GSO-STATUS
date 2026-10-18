using System.Globalization;
using System.Net.Http.Headers;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace GsoStatus;

// Optional Claude plan usage for the desktop app, which never runs Claude Code's status line. Local data comes first:
// used percentages come from the desktop app's plan-usage-history.json when it holds the newest sample. The server
// (the endpoint the desktop app itself polls) is asked only on demand: no data yet, a window reset has passed, or
// no fresh percentage exists locally. While Claude is working the percentage is refreshed every BusyInterval (60 s) and once more
// when the work ends; otherwise at most every MinInterval. Reset times are cached until they pass. The OAuth token comes from the desktop app's own token cache
// (DPAPI key + AES-GCM, current Windows user only). The token lives in memory for one request, is never
// refreshed, logged, stored or forwarded, and is sent only to api.anthropic.com over HTTPS.
sealed class ClaudeUsage:IDisposable
{
    internal const string Endpoint="https://api.anthropic.com/api/oauth/usage";
    internal static readonly TimeSpan MinInterval=TimeSpan.FromMinutes(5),BusyInterval=TimeSpan.FromSeconds(60),EdgeSpacing=TimeSpan.FromSeconds(5),RetryAfterFailure=TimeSpan.FromMinutes(2),StaleAfter=TimeSpan.FromMinutes(16),Tick=TimeSpan.FromSeconds(5);
    static readonly HttpClient SharedClient=new(new HttpClientHandler{AllowAutoRedirect=false}){Timeout=TimeSpan.FromSeconds(15),MaxResponseContentBufferSize=65536};
    readonly Func<IReadOnlyList<string>> tokens;
    readonly Func<string,CancellationToken,Task<(int Status,string Body)>> fetch;
    readonly Func<(DateTimeOffset At,double? FiveHour,double? SevenDay)?> local;
    readonly Func<bool>? busy;
    bool wasBusy,finalPending;int rateLimitStreak;
    readonly Func<DateTimeOffset> utcNow;
    readonly CancellationTokenSource stop=new();
    readonly object gate=new();
    JsonObject? latest;DateTimeOffset observed,attempted;string? error;Task? loop;
    public ClaudeUsage(Func<IReadOnlyList<string>>? tokens=null,Func<string,CancellationToken,Task<(int Status,string Body)>>? fetch=null,Func<DateTimeOffset>? utcNow=null,Func<(DateTimeOffset At,double? FiveHour,double? SevenDay)?>? local=null,Func<bool>? busy=null)
    {
        this.busy=busy;
        this.tokens=tokens??(()=>ReadDesktopTokens());this.fetch=fetch??DefaultFetch;this.utcNow=utcNow??(()=>DateTimeOffset.UtcNow);this.local=local??CachedHistory;
    }
    static async Task<(int Status,string Body)> DefaultFetch(string token,CancellationToken ct)
    {
        using var request=new HttpRequestMessage(HttpMethod.Get,Endpoint);
        request.Headers.Authorization=new AuthenticationHeaderValue("Bearer",token);
        request.Headers.Add("anthropic-beta","oauth-2025-04-20");
        using var response=await SharedClient.SendAsync(request,HttpCompletionOption.ResponseContentRead,ct);
        return ((int)response.StatusCode,response.IsSuccessStatusCode?await response.Content.ReadAsStringAsync(ct):"");
    }
    public void Start()
    {
        loop??=Task.Run(async()=>
        {
            try
            {
                while(!stop.IsCancellationRequested)
                {
                    try{if(NeedsServer())await PollOnce(stop.Token);}
                    catch(OperationCanceledException)when(stop.IsCancellationRequested){return;}
                    catch(Exception){lock(gate){error="claude_usage_failed";attempted=utcNow();}}
                    await Task.Delay(Tick,stop.Token);
                }
            }
            catch(OperationCanceledException){}
        });
    }
    /// The server is only worth asking when local data cannot answer: Claude is working (keep the percentage live),
    /// work just ended (final value), nothing known yet, a reset has passed, or no percentage newer than MinInterval
    /// exists. Failed requests back off (RetryAfterFailure, MinInterval after HTTP 429).
    internal bool NeedsServer()
    {
        var now=utcNow();JsonObject? data;DateTimeOffset last;string? code;
        lock(gate){data=latest;last=attempted;code=error;}
        bool working=false;try{working=busy?.Invoke()??false;}catch(Exception){}
        if(wasBusy&&!working)finalPending=true;wasBusy=working;bool ended=finalPending;
        int streak;lock(gate)streak=rateLimitStreak;
        // After HTTP 429 every token was limited: back off 5, 10, then 20 minutes (the cap) instead of retrying at the busy cadence.
        var spacing=code is not null?(code=="claude_usage_http_429"?TimeSpan.FromTicks(MinInterval.Ticks<<Math.Max(0,streak-1)):RetryAfterFailure):working?BusyInterval:ended?EdgeSpacing:MinInterval;
        if(last!=default&&now-last<spacing)return false;
        if(working||ended||data is null)return true;
        var view=Windows(data,now);
        if(view.Any(w=>w.Reset is {} reset&&reset<=now))return true;
        return view.Count==0||now-view.Max(w=>w.PercentAt)>=MinInterval;
    }
    internal async Task PollOnce(CancellationToken ct)
    {
        string? failure="claude_usage_token_unavailable";JsonObject? parsed=null;IReadOnlyList<string> candidates;
        try{candidates=tokens();}
        catch(Exception ex)when(ex is IOException or UnauthorizedAccessException or CryptographicException or JsonException or FormatException or InvalidDataException or ArgumentException){candidates=[];}
        foreach(var token in candidates)
        {
            try
            {
                var (status,body)=await fetch(token,ct);
                if(status is 401 or 403){failure="claude_usage_unauthorized";continue;}
                // The endpoint rate-limits per token: a limited token does not stop the next one from being tried.
                if(status==429){failure="claude_usage_http_429";continue;}
                if(status!=200){failure="claude_usage_http_"+status;break;}
                var root=JsonNode.Parse(body,documentOptions:new JsonDocumentOptions{MaxDepth=16}) as JsonObject;
                if(root is null){failure="claude_usage_invalid";break;}
                // Only the two plan windows are kept; the rest of the response is dropped immediately.
                parsed=new JsonObject();
                foreach(var key in new[]{"five_hour","seven_day"})if(root[key] is JsonObject window)parsed[key]=window.DeepClone();
                failure=null;break;
            }
            catch(Exception ex)when(!ct.IsCancellationRequested&&ex is HttpRequestException or TaskCanceledException or IOException or JsonException)
            {
                failure=ex is JsonException?"claude_usage_invalid":"claude_usage_unreachable";break;
            }
        }
        lock(gate){finalPending=false;attempted=utcNow();if(failure is null){latest=parsed;observed=attempted;}error=failure;rateLimitStreak=failure=="claude_usage_http_429"?Math.Min(rateLimitStreak+1,3):0;}
    }
    sealed record WindowView(int Minutes,double? Percent,DateTimeOffset PercentAt,DateTimeOffset? Reset);
    // Reset times always come from the last server answer; the used percentage comes from whichever of the server
    // answer and the desktop app's local sample is newer.
    List<WindowView> Windows(JsonObject? server,DateTimeOffset now)
    {
        DateTimeOffset serverAt;lock(gate)serverAt=observed;
        (DateTimeOffset At,double? FiveHour,double? SevenDay)? sample=null;
        try{sample=local();}catch(Exception ex)when(ex is IOException or UnauthorizedAccessException or JsonException or InvalidDataException or FormatException){}
        if(sample is {} future&&future.At>now.AddMinutes(1))sample=null;
        var result=new List<WindowView>();
        foreach(var (key,minutes) in new[]{("five_hour",300),("seven_day",10080)})
        {
            var window=server?[key] as JsonObject;
            double? percent=window?["utilization"] is JsonValue u&&u.TryGetValue<double>(out var value)&&double.IsFinite(value)&&value>=0?value:null;
            DateTimeOffset? reset=window?["resets_at"] is JsonValue r&&r.TryGetValue<string>(out var text)&&DateTimeOffset.TryParse(text,CultureInfo.InvariantCulture,DateTimeStyles.AssumeUniversal,out var parsedReset)?parsedReset:null;
            var at=percent is null?DateTimeOffset.MinValue:serverAt;
            double? localPercent=minutes==300?sample?.FiveHour:sample?.SevenDay;
            if(sample is {} known&&localPercent is {} lp&&known.At>at){percent=lp;at=known.At;}
            if(percent is null&&window is null)continue;
            result.Add(new WindowView(minutes,percent,at,reset));
        }
        return result;
    }
    public (JsonObject Source,JsonArray Windows) Snapshot()
    {
        JsonObject? data;string? code;
        lock(gate){data=latest;code=error;}
        var now=utcNow();var view=Windows(data,now);
        if(view.Count==0)return data is null?(ClaudeQuota.Source(code is null?"unconfigured":"error",null,null,code??"claude_usage_pending"),new JsonArray()):(ClaudeQuota.Source("error",null,null,"claude_usage_windows_missing"),new JsonArray());
        var windows=new JsonArray();var newest=view.Max(w=>w.PercentAt);bool stale=now-newest>StaleAfter;
        foreach(var w in view)
        {
            bool passed=w.Reset is {} at&&at<=now;
            string reason=passed?"quota_window_reset":stale?"quota_stale":"quota_value_unavailable";
            double? remaining=w.Percent is {} v&&!passed&&!stale?Math.Clamp(100-v,0,100):null;
            windows.Add(new JsonObject{["bucket"]=ClaudeQuota.Bucket,["window_duration_mins"]=w.Minutes,["remaining_percent"]=Model.Metric(remaining,reason),["resets_at"]=w.Reset is {} t&&t>now?t.ToString("O"):null});
        }
        if(newest==DateTimeOffset.MinValue)return (ClaudeQuota.Source(code is null?"unconfigured":"error",null,null,code??"claude_usage_pending"),new JsonArray());
        return (ClaudeQuota.Source(stale?"stale":"ok",now,newest,stale?"claude_quota_stale":null),windows);
    }
    // plan-usage-history.json can reach a megabyte; it is only re-read when the file actually changed.
    DateTime historyWrite;long historyLength=-1;(DateTimeOffset At,double? FiveHour,double? SevenDay)? history;
    (DateTimeOffset At,double? FiveHour,double? SevenDay)? CachedHistory()
    {
        var info=new FileInfo(DefaultHistoryPath());
        if(!info.Exists){history=null;historyLength=-1;return null;}
        if(info.LastWriteTimeUtc!=historyWrite||info.Length!=historyLength){historyWrite=info.LastWriteTimeUtc;historyLength=info.Length;history=ReadLocalHistory();}
        return history;
    }
    internal static string DefaultHistoryPath()=>Path.Combine(DefaultDesktopDirectory(),"plan-usage-history.json");
    /// Newest sample of the desktop app's own usage history: percentages only, no reset time.
    internal static (DateTimeOffset At,double? FiveHour,double? SevenDay)? ReadLocalHistory(string? path=null)
    {
        path??=DefaultHistoryPath();var info=new FileInfo(path);
        if(!info.Exists||info.Length>4*1024*1024)return null;
        if(JsonNode.Parse(File.ReadAllText(path,Encoding.UTF8)) is not JsonObject root||root["samples"] is not JsonArray samples)return null;
        long bestTime=-1;double? five=null,seven=null;
        foreach(var node in samples)
        {
            if(node is not JsonObject sample||sample["t"] is not JsonValue tv||!tv.TryGetValue<long>(out var t)||t<=bestTime)continue;
            var u=sample["u"] as JsonObject??sample;
            bestTime=t;five=Number(u["fh"]);seven=Number(u["sd"]);
        }
        return bestTime<0?null:(DateTimeOffset.FromUnixTimeMilliseconds(bestTime),five,seven);
        static double? Number(JsonNode? n)=>n is JsonValue v&&v.TryGetValue<double>(out var d)&&double.IsFinite(d)&&d>=0?d:null;
    }
    public void Dispose(){stop.Cancel();stop.Dispose();}

    internal static string DefaultDesktopDirectory()=>Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),"Claude");
    /// Unexpired desktop OAuth tokens that carry the profile scope, in cache order. Never throws for a missing app.
    internal static IReadOnlyList<string> ReadDesktopTokens(string? directory=null,Func<byte[],byte[]>? unprotect=null,Func<DateTimeOffset>? clock=null)
    {
        directory??=DefaultDesktopDirectory();var stateFile=Path.Combine(directory,"Local State");var configFile=Path.Combine(directory,"config.json");
        if(!File.Exists(stateFile)||!File.Exists(configFile)||new FileInfo(stateFile).Length>65536||new FileInfo(configFile).Length>262144)return [];
        var wrapped=Convert.FromBase64String(JsonNode.Parse(File.ReadAllText(stateFile,Encoding.UTF8))?["os_crypt"]?["encrypted_key"]?.GetValue<string>()??throw new InvalidDataException());
        if(wrapped.Length<6||Encoding.ASCII.GetString(wrapped,0,5)!="DPAPI")throw new InvalidDataException();
        var key=(unprotect??Dpapi.Unprotect)(wrapped[5..]);
        if(key.Length!=32)throw new InvalidDataException();
        var cached=JsonNode.Parse(File.ReadAllText(configFile,Encoding.UTF8))?["oauth:tokenCacheV2"]?.GetValue<string>();
        if(cached is null)return [];
        var plain=Decrypt(key,Convert.FromBase64String(cached));
        var now=(clock??(()=>DateTimeOffset.UtcNow))().ToUnixTimeMilliseconds();var result=new List<string>();var shared=new List<string>();
        if(JsonNode.Parse(plain) is not JsonObject entries)return [];
        foreach(var (name,entry) in entries)
        {
            const string marker=":https://api.anthropic.com:";var split=name.IndexOf(marker,StringComparison.Ordinal);
            if(split<0||!name[(split+marker.Length)..].Split(' ').Contains("user:profile")||entry is not JsonObject item)continue;
            if(item["expiresAt"] is not JsonValue expiry||!expiry.TryGetValue<long>(out var expires)||expires<=now+60000)continue;
            if(item["token"] is JsonValue token&&token.TryGetValue<string>(out var text)&&text.Length is >=20 and <=4096&&text.All(ch=>ch>'!'-1&&ch<='~'))
            {
                // A profile-only token is not used for inference, so its rate limit is not shared with Claude itself.
                if(name[(split+marker.Length)..].Split(' ').Contains("user:inference"))shared.Add(text);else result.Add(text);
            }
            if(result.Count+shared.Count>=4)break;
        }
        result.AddRange(shared);
        return result.Take(3).ToList();
    }
    // Chromium/Electron safeStorage layout: "v10" + 12-byte nonce + ciphertext + 16-byte tag.
    internal static byte[] Decrypt(byte[] key,byte[] blob)
    {
        if(blob.Length<31||Encoding.ASCII.GetString(blob,0,3)!="v10")throw new InvalidDataException();
        var plain=new byte[blob.Length-31];
        using var gcm=new AesGcm(key,16);gcm.Decrypt(blob.AsSpan(3,12),blob.AsSpan(15,plain.Length),blob.AsSpan(blob.Length-16,16),plain);
        return plain;
    }
}

static class Dpapi
{
    [StructLayout(LayoutKind.Sequential)]struct Blob{public int Length;public IntPtr Data;}
    [DllImport("crypt32.dll",SetLastError=true)]static extern bool CryptUnprotectData(ref Blob input,IntPtr description,IntPtr entropy,IntPtr reserved,IntPtr prompt,int flags,out Blob output);
    [DllImport("kernel32.dll")]static extern IntPtr LocalFree(IntPtr handle);
    internal static byte[] Unprotect(byte[] data)
    {
        var handle=GCHandle.Alloc(data,GCHandleType.Pinned);
        try
        {
            var input=new Blob{Length=data.Length,Data=handle.AddrOfPinnedObject()};
            // 0x1 = CRYPTPROTECT_UI_FORBIDDEN: never show a prompt from the background collector.
            if(!CryptUnprotectData(ref input,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,0x1,out var output))throw new CryptographicException(Marshal.GetLastWin32Error());
            try{var result=new byte[output.Length];Marshal.Copy(output.Data,result,0,result.Length);return result;}
            finally{LocalFree(output.Data);}
        }
        finally{handle.Free();}
    }
}
