using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace GsoStatus;

// Claude subscription limits come only from Claude Code's documented status line input (rate_limits).
// GSW never reads Claude credentials or calls Anthropic endpoints; `GSW.exe --claude-statusline` stores a
// bounded copy of the two windows, and the collector reads that bridge file.
sealed class ClaudeQuota
{
    internal const string Bucket="claude";
    internal static readonly TimeSpan StaleAfter=TimeSpan.FromMinutes(30);
    readonly string path;
    readonly Func<DateTimeOffset> utcNow;
    DateTime lastWrite;long lastLength=-1;
    JsonObject? bridge;string? error;
    readonly ClaudeUsage? usage;
    public ClaudeQuota(string path,Func<DateTimeOffset>? utcNow=null,ClaudeUsage? usage=null){this.path=path;this.utcNow=utcNow??(()=>DateTimeOffset.UtcNow);this.usage=usage;}
    internal static string DefaultPath()=>Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"GSO STATUS WIN","claude-rate-limits.json");
    /// Terminal Claude Code (status line bridge) and the desktop app (local usage file + on-demand server) are
    /// independent sources; the healthier one wins and, when both are healthy, the one with the newer observation.
    public (JsonObject Source,JsonArray Windows) Snapshot()
    {
        var bridgeData=BridgeSnapshot();
        if(usage is null)return bridgeData;
        var usageData=usage.Snapshot();
        int a=Vibe.Quality(usageData.Source),b=Vibe.Quality(bridgeData.Source);
        if(a!=b)return a>b?usageData:bridgeData;
        if(a==5&&Updated(bridgeData.Source)>Updated(usageData.Source))return bridgeData;
        return usageData;
    }
    static DateTimeOffset Updated(JsonObject source)=>source["source_updated_at"] is JsonValue v&&v.TryGetValue<string>(out var text)&&DateTimeOffset.TryParse(text,CultureInfo.InvariantCulture,DateTimeStyles.AssumeUniversal,out var at)?at:DateTimeOffset.MinValue;
    (JsonObject Source,JsonArray Windows) BridgeSnapshot()
    {
        var now=utcNow();
        try
        {
            var info=new FileInfo(path);
            if(!info.Exists){bridge=null;error=null;lastLength=-1;return (Source("unconfigured",null,null,"claude_statusline_not_configured"),new JsonArray());}
            if(info.LastWriteTimeUtc!=lastWrite||info.Length!=lastLength)
            {
                lastWrite=info.LastWriteTimeUtc;lastLength=info.Length;
                if(info.Length>4096)throw new InvalidDataException();
                bridge=JsonNode.Parse(File.ReadAllText(path,Encoding.UTF8)) as JsonObject??throw new InvalidDataException();error=null;
            }
        }
        catch(Exception ex)when(ex is IOException or UnauthorizedAccessException or JsonException or InvalidDataException or FormatException){bridge=null;error="claude_quota_bridge_invalid";}
        if(bridge is null)return (Source("error",null,null,error??"claude_quota_bridge_invalid"),new JsonArray());
        if(!TryTime(bridge["observed_at"],out var observed)||observed>now.AddMinutes(1))return (Source("error",null,null,"claude_quota_bridge_invalid"),new JsonArray());
        var windows=new JsonArray();bool stale=now-observed>StaleAfter;
        foreach(var (key,minutes) in new[]{("five_hour",300),("seven_day",10080)})
        {
            if(bridge[key] is not JsonObject window)continue;
            double? used=window["used_percentage"] is JsonValue u&&u.TryGetValue<double>(out var value)&&double.IsFinite(value)&&value>=0?value:null;
            DateTimeOffset? reset=window["resets_at"] is JsonValue r&&r.TryGetValue<long>(out var epoch)&&epoch>0&&epoch<253402300799?DateTimeOffset.FromUnixTimeSeconds(epoch):null;
            // A passed reset means the stored percentage no longer describes the current window.
            string reason=reset is {} passed&&passed<=now?"quota_window_reset":stale?"quota_stale":"quota_value_unavailable";
            double? remaining=used is {} v&&reset is {} upcoming&&upcoming>now&&!stale?Math.Clamp(100-v,0,100):null;
            windows.Add(new JsonObject{["bucket"]=Bucket,["window_duration_mins"]=minutes,["remaining_percent"]=Model.Metric(remaining,reason),["resets_at"]=reset is {} t&&t>now?t.ToString("O"):null});
        }
        if(windows.Count==0)return (Source("error",null,null,"claude_rate_limits_missing"),windows);
        // The bridge was read now; source_updated_at keeps the real status-line observation time.
        return (Source(stale?"stale":"ok",now,observed,stale?"claude_quota_stale":null),windows);
    }
    internal static JsonObject Source(string quality,DateTimeOffset? observed,DateTimeOffset? updated,string? code)=>new()
    {
        ["quality"]=quality,["observed_at"]=observed?.ToString("O"),["source_updated_at"]=updated?.ToString("O"),
        ["age_ms"]=observed is null?null:JsonValue.Create(0),["last_error_code"]=code
    };
    static bool TryTime(JsonNode? node,out DateTimeOffset value)
    {
        value=default;return node is JsonValue v&&v.TryGetValue<string>(out var text)&&DateTimeOffset.TryParse(text,CultureInfo.InvariantCulture,DateTimeStyles.AssumeUniversal,out value);
    }
    internal static string DefaultSettingsPath()
    {
        var configured=Environment.GetEnvironmentVariable("CLAUDE_CONFIG_DIR");
        var directory=!string.IsNullOrWhiteSpace(configured)&&Path.IsPathFullyQualified(configured)?configured:Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),".claude");
        return Path.Combine(directory,"settings.json");
    }
    internal static string StatusLineCommand(string exePath)=>"\""+exePath.Replace('\\','/')+"\" --claude-statusline";
    /// Adds (or removes) only the statusLine entry of the user's Claude Code settings; every other key is preserved.
    internal static (int Code,string Message) ConfigureStatusLine(string settingsPath,string exePath,bool install,bool force,Func<DateTimeOffset>? clock=null)
    {
        var directory=Path.GetDirectoryName(settingsPath)!;
        if(!Directory.Exists(directory))return (4,"未找到Claude Code配置目录："+directory+"；请先安装并运行一次Claude Code。");
        JsonObject root;
        try{root=File.Exists(settingsPath)?JsonNode.Parse(File.ReadAllText(settingsPath,Encoding.UTF8),documentOptions:new JsonDocumentOptions{CommentHandling=JsonCommentHandling.Skip,AllowTrailingCommas=true}) as JsonObject??throw new InvalidDataException():new JsonObject();}
        catch(Exception ex)when(ex is JsonException or InvalidDataException){return (5,"settings.json不是有效的JSON对象，未修改。");}
        var command=StatusLineCommand(exePath);
        bool ours=root["statusLine"] is JsonObject existing&&existing["command"] is JsonValue current&&current.TryGetValue<string>(out var text)&&text.Contains("--claude-statusline",StringComparison.Ordinal);
        if(install)
        {
            if(ours&&root["statusLine"]!["command"]!.GetValue<string>()==command)return (0,"Claude额度桥接已接入，无需修改。");
            if(root["statusLine"] is not null&&!ours&&!force)return (3,"已有其他statusLine配置，未覆盖；确认后加 -Force 替换（会先备份）。");
            root["statusLine"]=new JsonObject{["type"]="command",["command"]=command};
        }
        else
        {
            if(!ours)return (0,"未发现GSW的statusLine配置，无需移除。");
            root.Remove("statusLine");
        }
        if(File.Exists(settingsPath))File.Copy(settingsPath,settingsPath+".gso-backup-"+(clock??(()=>DateTimeOffset.Now))().ToString("yyyyMMdd-HHmmss"),true);
        var options=new JsonSerializerOptions{WriteIndented=true,Encoder=System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping};
        var temp=settingsPath+".gso.tmp";File.WriteAllText(temp,root.ToJsonString(options)+"\n",new UTF8Encoding(false));File.Move(temp,settingsPath,true);
        return (0,install?"已接入Claude额度桥接："+command:"已移除GSW的statusLine配置。");
    }
    /// Status line command: reads Claude Code's JSON from stdin, keeps only rate limit windows, prints one short line.
    internal static int RunStatusLine(TextReader input,TextWriter output,string path,Func<DateTimeOffset>? clock=null)
    {
        var now=(clock??(()=>DateTimeOffset.UtcNow))();
        string text;
        {var buffer=new char[1<<20];int total=0,read;while(total<buffer.Length&&(read=input.Read(buffer,total,buffer.Length-total))>0)total+=read;text=new string(buffer,0,total);}
        JsonObject? limits=null;string? model=null;
        try{var root=JsonNode.Parse(text,documentOptions:new JsonDocumentOptions{MaxDepth=32}) as JsonObject;limits=root?["rate_limits"] as JsonObject;model=root?["model"]?["display_name"] is JsonValue m&&m.TryGetValue<string>(out var name)?name:null;}
        catch(JsonException){}
        var stored=new JsonObject{["observed_at"]=now.ToString("O")};var parts=new List<string>();
        foreach(var (key,label) in new[]{("five_hour","5h"),("seven_day","7d")})
        {
            if(limits?[key] is not JsonObject window)continue;
            if(window["used_percentage"] is not JsonValue u||!u.TryGetValue<double>(out var used)||!double.IsFinite(used)||used<0)continue;
            long? reset=window["resets_at"] is JsonValue r&&r.TryGetValue<double>(out var seconds)&&double.IsFinite(seconds)&&seconds>0&&seconds<253402300799?(long)seconds:null;
            stored[key]=new JsonObject{["used_percentage"]=Math.Round(Math.Min(used,1000),2),["resets_at"]=reset};
            parts.Add(label+" "+Math.Clamp(100-used,0,100).ToString("0",CultureInfo.InvariantCulture)+"%");
        }
        if(parts.Count>0)
        {
            try{
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                var temp=path+"."+Environment.ProcessId+".tmp";File.WriteAllText(temp,stored.ToJsonString(),new UTF8Encoding(false));File.Move(temp,path,true);
            }catch(Exception ex)when(ex is IOException or UnauthorizedAccessException){}
        }
        // ASCII only: the status line pipe is not guaranteed to share the console code page.
        var line=(model is null?"":"["+string.Concat(model.Where(ch=>ch>=' '&&ch<='~').Take(40))+"] ")+(parts.Count>0?string.Join(" | ",parts)+" left":"GSO VIBE");
        output.WriteLine(line);output.Flush();return 0;
    }
}
