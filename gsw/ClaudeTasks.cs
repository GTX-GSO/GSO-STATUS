using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace GsoStatus;

// Claude Code transcripts: <root>/<project>/<session-uuid>.jsonl, append-only JSONL.
// Only lifecycle markers, timestamps and explicit title records are read; message text never leaves this class
// and is never used as a title fallback.
sealed class ClaudeTasks : IAsyncDisposable
{
    internal const string IdPrefix="claude-";
    // The transcript only grows when a record is finished, so a long answer or a long thought leaves it silent for a while.
    static readonly TimeSpan EventTimeout=TimeSpan.FromSeconds(120);
    // After an ordinary tool call the transcript stays silent until the tool returns (builds, tests, installs, a subagent):
    // that is still work, not a finished turn. The user cancelling writes an interrupt record, which ends the turn at once.
    static readonly TimeSpan ToolRunQuiet=TimeSpan.FromMinutes(10);
    // Tools that hand the turn back to the user: the desktop app writes no end_turn record after them, so the
    // quiet window would keep VIBE "running" long after the reply is finished.
    static readonly TimeSpan HandOffQuiet=TimeSpan.FromSeconds(3);
    static bool WaitsForUser(string name)=>name is "AskUserQuestion" or "ExitPlanMode";
    static bool EndsTurnWithResult(string name)=>name is "mcp__ccd_turn__wrap_up";
    sealed class FileState
    {
        public long Offset;
        public bool DropLine;
        public readonly List<byte> Line=new();
        public byte[] Checkpoint=[];
        public string SessionId="";
        public bool Invalid,CatchingUp,Active;
        public DateTimeOffset TurnStart;
        public long TurnTick,TurnInitialMs;
        public DateTime LastEvent=DateTime.MinValue;
        public TimeSpan Quiet=EventTimeout;
        public readonly HashSet<string> HandOffIds=new();
        public string? CustomTitle,AiTitle,Summary;
        public string? Title=>CustomTitle??AiTitle??Summary;
    }
    readonly string[] roots;
    readonly Func<DateTimeOffset> utcNow;
    readonly Func<long> monotonicMs;
    readonly Dictionary<string,FileState> files=new(StringComparer.OrdinalIgnoreCase);
    readonly CancellationTokenSource stop=new();
    readonly object gate=new();
    readonly Task task;
    JsonArray summaries=new();
    DateTime[] recency=[];
    string aggregate="unknown";
    public ClaudeTasks(IEnumerable<string> roots,bool start=true,Func<DateTimeOffset>? utcNow=null,Func<long>? monotonicMs=null)
    {
        this.roots=roots.Where(r=>!string.IsNullOrWhiteSpace(r)).Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).Take(8).ToArray();
        this.utcNow=utcNow??(()=>DateTimeOffset.UtcNow);this.monotonicMs=monotonicMs??(()=>Environment.TickCount64);
        task=start?Task.Run(Loop):Task.CompletedTask;
    }
    internal static string[] DefaultRoots()
    {
        var list=new List<string>();
        var configured=Environment.GetEnvironmentVariable("CLAUDE_CONFIG_DIR");
        if(!string.IsNullOrWhiteSpace(configured)&&Path.IsPathFullyQualified(configured))list.Add(Path.Combine(configured,"projects"));
        list.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),".claude","projects"));
        return list.ToArray();
    }
    /// State is running/idle/unknown, or "absent" when no transcript root exists (Claude Code not installed).
    public (string State,JsonArray Sessions) Snapshot(){lock(gate)return (aggregate,(JsonArray)summaries.DeepClone());}
    internal DateTime[] Recency(){lock(gate)return (DateTime[])recency.Clone();}
    async Task Loop(){while(!stop.IsCancellationRequested){Scan();try{await Task.Delay(1000,stop.Token);}catch(OperationCanceledException){}}}
    public void Scan()
    {
        try
        {
            var existing=roots.Where(Directory.Exists).ToArray();
            if(existing.Length==0){lock(gate){aggregate="absent";summaries=new JsonArray();recency=[];}return;}
            var now=utcNow();var horizon=now.UtcDateTime.AddDays(-3);bool scopeIncomplete=false;
            var candidates=new List<(string Path,DateTime Written)>();
            foreach(var root in existing)
            {
                var projects=new DirectoryInfo(root).EnumerateDirectories().Take(513).ToArray();scopeIncomplete|=projects.Length>512;
                foreach(var project in projects.Take(512))
                {
                    try{
                        var entries=project.EnumerateFiles("*.jsonl").Take(513).ToArray();scopeIncomplete|=entries.Length>512;
                        candidates.AddRange(entries.Take(512).Where(f=>f.LastWriteTimeUtc>=horizon).Select(f=>(f.FullName,f.LastWriteTimeUtc)));
                    }catch(IOException){scopeIncomplete=true;}catch(UnauthorizedAccessException){scopeIncomplete=true;}
                }
            }
            // Keep a turn already seen as active even when discovery would drop it.
            foreach(var pair in files.Where(p=>p.Value.Active&&File.Exists(p.Key)))candidates.Add((pair.Key,File.GetLastWriteTimeUtc(pair.Key)));
            var selected=candidates.GroupBy(c=>c.Path,StringComparer.OrdinalIgnoreCase).Select(g=>g.First()).OrderByDescending(c=>c.Written).ToArray();
            scopeIncomplete|=selected.Length>32;selected=selected.Take(32).ToArray();
            foreach(var removed in files.Keys.Except(selected.Select(s=>s.Path),StringComparer.OrdinalIgnoreCase).ToArray())files.Remove(removed);
            foreach(var (path,written) in selected)Read(path,written);
            string Status(FileState state)=>state.Invalid||state.CatchingUp?"unknown":!state.Active?"idle":now.UtcDateTime-state.LastEvent>state.Quiet?"idle":"running";
            var result=new JsonArray();var order=new List<DateTime>();bool running=false,uncertain=scopeIncomplete;
            foreach(var pair in files.OrderByDescending(p=>Status(p.Value)=="running"?2:Status(p.Value)=="unknown"?1:0).ThenByDescending(p=>p.Value.LastEvent))
            {
                var state=pair.Value;var status=Status(state);running|=status=="running";uncertain|=status=="unknown";
                if(result.Count>=4)continue;
                string identity=state.SessionId.Length>0?state.SessionId:Path.GetFileName(pair.Key);
                var id=IdPrefix+Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes("claude:"+identity)))[..16].ToLowerInvariant();
                bool timed=status=="running";long elapsed=timed?Math.Min(9007199254740991L,state.TurnInitialMs+Math.Max(0,monotonicMs()-state.TurnTick)):0;
                string? title=state.Title;
                result.Add(new JsonObject{["id"]=id,["title"]=title,["title_reason"]=title is null?"session_title_unavailable":null,["state"]=status,
                    ["started_at"]=timed?state.TurnStart.ToString("O"):null,["running_duration_ms"]=timed?JsonValue.Create(elapsed):null,
                    ["duration_reason"]=timed?null:status=="idle"?(state.Active?"task_event_timeout":"not_running"):state.Invalid?"task_log_invalid":"task_log_catching_up"});
                order.Add(state.LastEvent);
            }
            lock(gate){summaries=result;recency=order.ToArray();aggregate=running?"running":uncertain?"unknown":"idle";}
        }
        catch(Exception ex)when(ex is IOException or UnauthorizedAccessException or System.Security.SecurityException){lock(gate){aggregate="unknown";summaries=new JsonArray();recency=[];}}
    }
    static bool MatchesCheckpoint(FileStream stream,FileState state)
    {
        if(state.Checkpoint.Length==0)return true;
        Span<byte> bytes=stackalloc byte[64];stream.Position=state.Offset-state.Checkpoint.Length;stream.ReadExactly(bytes[..state.Checkpoint.Length]);
        return bytes[..state.Checkpoint.Length].SequenceEqual(state.Checkpoint);
    }
    void Read(string path,DateTime written)
    {
        using var stream=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete);
        long length=stream.Length;
        if(!files.TryGetValue(path,out var state))
        {
            // At most 8 MiB backfill; a cut first line is dropped, never parsed.
            state=new FileState{Offset=Math.Max(0,length-8388608),DropLine=length>8388608,SessionId=Regex.Match(Path.GetFileNameWithoutExtension(path),@"^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$").Value.ToLowerInvariant()};
            files[path]=state;
        }
        if(length<state.Offset||!MatchesCheckpoint(stream,state))
        {
            // Rewritten or truncated: restart; an interrupted active turn becomes unknown until a new marker arrives.
            bool wasActive=state.Active;var fresh=new FileState{SessionId=state.SessionId,Invalid=wasActive};files[path]=state=fresh;
        }
        stream.Seek(state.Offset,SeekOrigin.Begin);var bytes=new byte[32768];int remaining=262144;
        while(remaining>0)
        {
            int size=stream.Read(bytes,0,Math.Min(bytes.Length,remaining));if(size==0)break;remaining-=size;state.Offset+=size;
            for(int i=0;i<size;i++)
            {
                byte b=bytes[i];
                if(b==10){if(!state.DropLine)Event(state,state.Line.ToArray(),written);state.Line.Clear();state.DropLine=false;}
                else if(!state.DropLine){if(state.Line.Count>=65536){Oversized(state,state.Line.ToArray(),written);state.Line.Clear();state.DropLine=true;}else state.Line.Add(b);}
            }
        }
        int count=(int)Math.Min(64,state.Offset);state.Checkpoint=new byte[count];stream.Position=state.Offset-count;stream.ReadExactly(state.Checkpoint);
        state.CatchingUp=state.Offset<length;
    }
    static string? CleanTitle(JsonElement value)
    {
        if(value.ValueKind!=JsonValueKind.String)return null;var text=value.GetString();if(string.IsNullOrWhiteSpace(text)||text.Length>4096)return null;
        var clean=string.Concat(text.EnumerateRunes().Where(r=>!Rune.IsControl(r)).Take(160).Select(r=>r.ToString())).Trim();
        return clean.Length>0?clean:null;
    }
    static bool Terminal(string reason)=>reason is "end_turn" or "stop_sequence" or "max_tokens" or "refusal" or "model_context_window_exceeded";
    static bool LocalCommand(string text)=>text.StartsWith("<command-name>",StringComparison.Ordinal)||text.StartsWith("<command-message>",StringComparison.Ordinal)||text.StartsWith("<local-command-",StringComparison.Ordinal)||text.StartsWith("<bash-input>",StringComparison.Ordinal)||text.StartsWith("<bash-stdout>",StringComparison.Ordinal)||text.StartsWith("<bash-stderr>",StringComparison.Ordinal);
    static bool Interrupted(string text)=>text.StartsWith("[Request interrupted by user",StringComparison.Ordinal);
    void Start(FileState state,DateTimeOffset at)
    {
        // A prompt after the event timeout opens a new turn instead of extending the stale one.
        if(state.Active&&at.UtcDateTime-state.LastEvent<=state.Quiet)return;
        state.Quiet=EventTimeout;state.HandOffIds.Clear();
        state.Active=true;state.Invalid=false;state.TurnStart=at;state.TurnTick=monotonicMs();state.TurnInitialMs=Math.Max(0,(long)(utcNow()-at).TotalMilliseconds);
    }
    static void Touch(FileState state,DateTime at){if(at>state.LastEvent)state.LastEvent=at;}
    void Event(FileState state,byte[] line,DateTime written)
    {
        if(line.All(b=>b is 9 or 13 or 32))return;
        JsonDocument document;
        try{document=JsonDocument.Parse(line,new JsonDocumentOptions{MaxDepth=64});}
        catch(JsonException){state.Invalid=true;state.Active=false;return;}
        using(document)
        {
            var root=document.RootElement;
            if(root.ValueKind!=JsonValueKind.Object||!root.TryGetProperty("type",out var kind)||kind.ValueKind!=JsonValueKind.String)return;
            var type=kind.GetString();
            bool otherSession=root.TryGetProperty("sessionId",out var sid)&&sid.ValueKind==JsonValueKind.String&&state.SessionId.Length>0&&!string.Equals(sid.GetString(),state.SessionId,StringComparison.OrdinalIgnoreCase);
            switch(type)
            {
                case "custom-title":if(!otherSession&&root.TryGetProperty("customTitle",out var custom))state.CustomTitle=CleanTitle(custom)??state.CustomTitle;return;
                case "ai-title":if(!otherSession&&(root.TryGetProperty("aiTitle",out var ai)||root.TryGetProperty("title",out ai)))state.AiTitle=CleanTitle(ai)??state.AiTitle;return;
                case "summary":if(root.TryGetProperty("summary",out var summary))state.Summary=CleanTitle(summary)??state.Summary;return;
                case "user" or "assistant" or "system" or "progress":break;
                default:return;
            }
            if(otherSession)return;
            if(!root.TryGetProperty("timestamp",out var stamp)||stamp.ValueKind!=JsonValueKind.String||!DateTimeOffset.TryParse(stamp.GetString(),System.Globalization.CultureInfo.InvariantCulture,System.Globalization.DateTimeStyles.AssumeUniversal,out var parsed))return;
            if(parsed>utcNow().AddSeconds(5))return;
            DateTime at=parsed.UtcDateTime;
            // Subagent (sidechain) records only prove the parent turn is still working.
            if(root.TryGetProperty("isSidechain",out var side)&&side.ValueKind==JsonValueKind.True){if(state.Active)Touch(state,at);return;}
            if(type=="user")
            {
                if((root.TryGetProperty("isMeta",out var meta)&&meta.ValueKind==JsonValueKind.True)||(root.TryGetProperty("isCompactSummary",out var compact)&&compact.ValueKind==JsonValueKind.True)){if(state.Active)Touch(state,at);return;}
                if(!root.TryGetProperty("message",out var message)||!message.TryGetProperty("content",out var content))return;
                bool toolResult=false,interrupted=false,command=false,prompt=false,handedOff=false;
                if(content.ValueKind==JsonValueKind.String){var text=content.GetString()??"";interrupted=Interrupted(text);command=LocalCommand(text);prompt=!interrupted&&!command;}
                else if(content.ValueKind==JsonValueKind.Array)
                {
                    foreach(var block in content.EnumerateArray())
                    {
                        if(block.ValueKind!=JsonValueKind.Object||!block.TryGetProperty("type",out var bt)||bt.ValueKind!=JsonValueKind.String)continue;
                        var blockType=bt.GetString();
                        if(blockType=="tool_result"){toolResult=true;if(block.TryGetProperty("tool_use_id",out var resultId)&&resultId.ValueKind==JsonValueKind.String&&state.HandOffIds.Remove(resultId.GetString()!))handedOff=true;}
                        else if(blockType=="text"&&block.TryGetProperty("text",out var tx)&&tx.ValueKind==JsonValueKind.String){var text=tx.GetString()??"";if(Interrupted(text))interrupted=true;else if(LocalCommand(text))command=true;else prompt=true;}
                        else if(blockType is "image" or "document")prompt=true;
                    }
                }
                if(interrupted){state.Active=false;state.Invalid=false;Touch(state,at);return;}
                if(toolResult){if(!state.Active)Start(state,parsed);state.Quiet=handedOff?HandOffQuiet:EventTimeout;Touch(state,at);return;}
                if(prompt){Start(state,parsed);state.Quiet=EventTimeout;Touch(state,at);}
                return;
            }
            if(type=="assistant")
            {
                bool apiError=root.TryGetProperty("isApiErrorMessage",out var err)&&err.ValueKind==JsonValueKind.True;
                string? reason=root.TryGetProperty("message",out var message)&&message.ValueKind==JsonValueKind.Object&&message.TryGetProperty("stop_reason",out var sr)&&sr.ValueKind==JsonValueKind.String?sr.GetString():null;
                if(apiError||(reason is not null&&Terminal(reason))){state.Active=false;state.Invalid=false;Touch(state,at);return;}
                if(!state.Active)Start(state,parsed);
                bool handOff=false,runsTool=false;
                if(message.ValueKind==JsonValueKind.Object&&message.TryGetProperty("content",out var parts)&&parts.ValueKind==JsonValueKind.Array)
                    foreach(var block in parts.EnumerateArray())
                    {
                        if(block.ValueKind!=JsonValueKind.Object||!block.TryGetProperty("type",out var bt)||bt.ValueKind!=JsonValueKind.String||bt.GetString()!="tool_use"||!block.TryGetProperty("name",out var name)||name.ValueKind!=JsonValueKind.String)continue;
                        var toolName=name.GetString()!;
                        if(WaitsForUser(toolName))handOff=true;
                        else if(!EndsTurnWithResult(toolName))runsTool=true;
                        else if(EndsTurnWithResult(toolName)){handOff=true;if(block.TryGetProperty("id",out var toolId)&&toolId.ValueKind==JsonValueKind.String&&state.HandOffIds.Count<16)state.HandOffIds.Add(toolId.GetString()!);}
                    }
                state.Quiet=handOff?HandOffQuiet:runsTool?ToolRunQuiet:EventTimeout;
                Touch(state,at);return;
            }
            string? subtype=root.TryGetProperty("subtype",out var st)&&st.ValueKind==JsonValueKind.String?st.GetString():null;
            if(type=="system"&&subtype is "turn_duration" or "stop_hook_summary"){state.Active=false;state.Invalid=false;Touch(state,at);return;}
            if(state.Active)Touch(state,at);
        }
    }
    void Oversized(FileState state,byte[] prefix,DateTime written)
    {
        // Large tool output or pasted images: only the top-level kind is inspected, and the file write time
        // bounds the activity time because the timestamp property may lie beyond the retained prefix.
        string? type=null;bool sidechain=false;
        try{var reader=new Utf8JsonReader(prefix,isFinalBlock:false,state:default);
            while(reader.Read())if(reader.TokenType==JsonTokenType.PropertyName&&reader.CurrentDepth==1){
                if(reader.ValueTextEquals("type")){if(reader.Read()&&reader.TokenType==JsonTokenType.String)type=reader.GetString();}
                else if(reader.ValueTextEquals("isSidechain")){if(reader.Read())sidechain=reader.TokenType==JsonTokenType.True;}
                else if(!reader.TrySkip())break;
            }}catch(JsonException){}catch(InvalidOperationException){}
        if(type is not ("user" or "assistant"))return;
        var at=written<utcNow().UtcDateTime?written:utcNow().UtcDateTime;
        if(!sidechain&&!state.Active)Start(state,new DateTimeOffset(at,TimeSpan.Zero));
        if(state.Active)Touch(state,at);
    }
    public async ValueTask DisposeAsync(){stop.Cancel();await task;stop.Dispose();}
}
