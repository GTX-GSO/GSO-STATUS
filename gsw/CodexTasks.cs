using System.Text.RegularExpressions;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace GsoStatus;

// Bounded lifecycle observation plus explicit session-index titles; never publish event bodies.
sealed class CodexTasks : IAsyncDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    struct FileIdentity { public ulong Volume,Low,High; }
    [DllImport("kernel32.dll",SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool GetFileInformationByHandleEx(SafeFileHandle file,int informationClass,out FileIdentity identity,uint size);
    static FileIdentity Identity(FileStream stream)
    {
        // FILE_ID_INFO includes the full 128-bit identity on both NTFS and ReFS.
        if(!GetFileInformationByHandleEx(stream.SafeFileHandle,18,out var identity,24))throw new IOException("File identity unavailable");
        return identity;
    }
    sealed class FileState
    {
        public long Offset;
        public bool DropLine;
        public readonly List<byte> Line=new();
        public readonly Dictionary<string,(DateTimeOffset Start,long Tick,long InitialMs)> Active=new();
        public string SessionId="";
        public string TitleSessionId="";
        public readonly Dictionary<string,DateTimeOffset> Terminal=new();
        public DateTimeOffset? EvictedTerminalUntil;
        public bool Invalid;
        public bool CatchingUp;
        public FileIdentity Identity;
        public byte[] Checkpoint=[];
        public DateTime LastEvent=DateTime.UtcNow;
    }
    static bool MatchesCheckpoint(FileStream stream,FileState state)
    {
        if(state.Checkpoint.Length==0)return true;
        Span<byte> bytes=stackalloc byte[64];
        stream.Position=state.Offset-state.Checkpoint.Length;
        stream.ReadExactly(bytes[..state.Checkpoint.Length]);
        return bytes[..state.Checkpoint.Length].SequenceEqual(state.Checkpoint);
    }
    static void SaveCheckpoint(FileStream stream,FileState state)
    {
        // Detect copy/truncate/rewrite even if it regrows between two scans.
        int count=(int)Math.Min(64,state.Offset);
        state.Checkpoint=new byte[count];
        stream.Position=state.Offset-count;
        stream.ReadExactly(state.Checkpoint);
    }
    readonly string directory;
    readonly Func<DateTimeOffset> utcNow;
    readonly Func<long> monotonicMs;
    readonly Dictionary<string,string> titles=new();
    long nextTitleRead;
    readonly Dictionary<string,FileState> files=new();
    readonly HashSet<string> lostActiveFiles=new();
    bool lostActiveOverflow;
    readonly CancellationTokenSource stop=new();
    readonly object gate=new();
    readonly Task task;
    JsonArray summaries=new();
    DateTime[] recency=[];
    string aggregate="unknown";
    LocalQuotaSnapshot? quota;
    public LocalQuotaSnapshot? QuotaSnapshot(){lock(gate)return quota is null?null:new LocalQuotaSnapshot(quota.ObservedAt,(JsonArray)quota.Windows.DeepClone());}
    public CodexTasks(string directory,bool start=true,Func<DateTimeOffset>? utcNow=null,Func<long>? monotonicMs=null){this.directory=directory;this.utcNow=utcNow??(()=>DateTimeOffset.UtcNow);this.monotonicMs=monotonicMs??(()=>Environment.TickCount64);task=start?Task.Run(Loop):Task.CompletedTask;}
    public (string State,JsonArray Sessions) Snapshot(){lock(gate)return (aggregate,(JsonArray)summaries.DeepClone());}
    internal DateTime[] Recency(){lock(gate)return (DateTime[])recency.Clone();}
    async Task Loop(){while(!stop.IsCancellationRequested){Scan();try{await Task.Delay(1000,stop.Token);}catch(OperationCanceledException){}}}
    public void Scan()
    {
        try
        {
            if(!Directory.Exists(directory)){lock(gate){aggregate="unknown";summaries=new JsonArray();}return;}
            ReadTitles();
            // Three dates, 32 files; at most 8 MiB backfill per file, 256 KiB per scan.
            var candidates=new List<string>();bool scopeIncomplete=false;
            for(int day=0;day<3;day++){
                var date=utcNow().UtcDateTime.AddDays(-day);var folder=Path.Combine(directory,date.ToString("yyyy"),date.ToString("MM"),date.ToString("dd"));
                if(Directory.Exists(folder)){var entries=Directory.EnumerateFiles(folder,"*.jsonl").Take(257).ToArray();scopeIncomplete|=entries.Length>256;candidates.AddRange(entries.Take(256));}
            }
            // Keep already-observed long tasks across the discovery date window.
            candidates.AddRange(files.Where(p=>p.Value.Active.Count>0&&File.Exists(p.Key)).Select(p=>p.Key));
            candidates.AddRange(lostActiveFiles.Where(File.Exists));
            candidates=candidates.Distinct().ToList();scopeIncomplete|=candidates.Count>32;
            var selected=candidates.OrderByDescending(File.GetLastWriteTimeUtc).Take(32).ToArray();
            foreach(var removed in files.Keys.Except(selected).ToArray()){
                if(files[removed].Active.Count>0){if(lostActiveFiles.Count<32)lostActiveFiles.Add(removed);else lostActiveOverflow=true;}
                files.Remove(removed);
            }
            foreach(var path in selected)
            {
                using var stream=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete);
                var identity=Identity(stream);
                if(!files.TryGetValue(path,out var state)||!state.Identity.Equals(identity))
                {
                    bool previouslyLost=lostActiveFiles.Remove(path);
                    bool lostActive=(state?.Active.Count??0)>0||previouslyLost;
                    long length=stream.Length;state=new FileState{Identity=identity,Offset=Math.Max(0,length-8388608),DropLine=length>8388608,Invalid=length>8388608||lostActive,SessionId=Regex.Match(Path.GetFileNameWithoutExtension(path),@"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$").Value};files[path]=state;
                    state.TitleSessionId=ReadTitleIdentity(stream,state.SessionId);
                }
                bool rewritten=stream.Length>=state.Offset&&!MatchesCheckpoint(stream,state);
                if(stream.Length<state.Offset||rewritten){bool wasUncertain=state.Invalid||state.Active.Count>0||rewritten;state.Offset=0;state.Active.Clear();state.Terminal.Clear();state.EvictedTerminalUntil=null;state.Invalid=wasUncertain;state.Line.Clear();state.DropLine=false;state.Checkpoint=[];}
                stream.Seek(state.Offset,SeekOrigin.Begin);var bytes=new byte[32768];int remaining=262144;
                while(remaining>0)
                {
                    int size=stream.Read(bytes,0,Math.Min(bytes.Length,remaining));if(size==0)break;remaining-=size;state.Offset+=size;
                    for(int i=0;i<size;i++)
                    {
                        byte b=bytes[i];if(b==10){if(!state.DropLine)Event(state,state.Line.ToArray());state.Line.Clear();state.DropLine=false;}
                        else if(!state.DropLine){if(state.Line.Count>=65536){if(!IgnoredRecord(state.Line.ToArray(),true)){state.Invalid=true;state.Active.Clear();}else Touch(state,state.Line.ToArray());state.Line.Clear();state.DropLine=true;}else state.Line.Add(b);}
                    }
                }
                SaveCheckpoint(stream,state);
            }
            foreach(var entry in files){entry.Value.CatchingUp=entry.Value.Offset<new FileInfo(entry.Key).Length;}
            string Status(FileState state)=>state.Invalid||state.CatchingUp?"unknown":state.Active.Count==0?"idle":utcNow().UtcDateTime-state.LastEvent>TimeSpan.FromSeconds(60)?"idle":"running";
            var result=new JsonArray();var order=new List<DateTime>();bool running=false,uncertain=scopeIncomplete||lostActiveFiles.Count>0||lostActiveOverflow;
            foreach(var pair in files.OrderByDescending(p=>Status(p.Value)=="running"?2:Status(p.Value)=="unknown"?1:0).ThenByDescending(p=>p.Value.Active.Count>0?p.Value.Active.Values.Max(v=>v.Start.UtcDateTime):p.Value.LastEvent))
            {
                var state=pair.Value;var status=Status(state);
                running|=status=="running";uncertain|=status=="unknown";
                if(result.Count<4){
                    var id=Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(state.SessionId.Length>0?state.SessionId:Path.GetFileName(pair.Key))))[..16];
                    string? title=titles.GetValueOrDefault(state.TitleSessionId.Length>0?state.TitleSessionId:state.SessionId);var current=state.Active.Count>0?state.Active.Values.MaxBy(v=>v.Start):default;
                    bool timed=status=="running";long elapsed=timed?Math.Min(9007199254740991L,current.InitialMs+Math.Max(0,monotonicMs()-current.Tick)):0;
                    result.Add(new JsonObject{["id"]=id,["title"]=title,["title_reason"]=title is null?"session_title_unavailable":null,["state"]=status,["started_at"]=timed?current.Start.ToString("O"):null,["running_duration_ms"]=timed?JsonValue.Create(elapsed):null,["duration_reason"]=timed?null:status=="idle"?"not_running":state.Invalid?"task_log_invalid":state.CatchingUp?"task_log_catching_up":"task_event_timeout"});order.Add(state.Active.Count>0?state.Active.Values.Max(v=>v.Start.UtcDateTime):state.LastEvent);
                }
            }
            // Successful observation with no active local turns is ready; faults stay unknown.
            lock(gate){summaries=result;recency=order.ToArray();aggregate=running?"running":uncertain?"unknown":"idle";}
        }
        catch{lock(gate){aggregate="unknown";summaries=new JsonArray();}}
    }
    void Event(FileState state,byte[] line)
    {
        if(line.All(b=>b is 9 or 13 or 32))return;
        if(IgnoredRecord(line)){Touch(state,line);return;}
        try
        {
            using var document=JsonDocument.Parse(line,new JsonDocumentOptions{MaxDepth=32});var root=document.RootElement;
            if(!root.TryGetProperty("type",out var kind)||kind.GetString()!="event_msg"||!root.TryGetProperty("payload",out var payload)||!payload.TryGetProperty("type",out var type))return;
            if(type.ValueKind!=JsonValueKind.String){state.Invalid=true;state.Active.Clear();return;}
            string? name=type.GetString();
            if(string.IsNullOrEmpty(name)||name.Length>160){state.Invalid=true;state.Active.Clear();return;}
            if(name=="token_count"){
                // Quota corruption cannot invalidate an independently observed task lifecycle.
                try{
                    if(root.TryGetProperty("timestamp",out var tokenStamp)&&tokenStamp.ValueKind==JsonValueKind.String&&DateTimeOffset.TryParse(tokenStamp.GetString(),out var quotaAt)&&quotaAt<=utcNow()&&payload.TryGetProperty("rate_limits",out var limits)&&limits.ValueKind==JsonValueKind.Object&&JsonNode.Parse(limits.GetRawText()) is JsonObject obj){
                        var parsedWindows=CodexQuota.ParseEventWindows(obj);if(parsedWindows.Count>0)lock(gate){if(quota is null||quotaAt>quota.ObservedAt)quota=new LocalQuotaSnapshot(quotaAt,parsedWindows);}
                    }
                }catch(Exception ex)when(ex is JsonException or InvalidOperationException or FormatException){}
                Touch(state,line);return;
            }
            if(!root.TryGetProperty("timestamp",out var stamp)||stamp.ValueKind!=JsonValueKind.String||!DateTimeOffset.TryParse(stamp.GetString(),out var parsed)){state.Invalid=true;state.Active.Clear();return;}DateTime at=parsed.UtcDateTime;
            if(at>utcNow().UtcDateTime){state.Invalid=true;return;}
            bool previouslyActive=state.Active.Count>0;
            if(previouslyActive&&at>state.LastEvent)state.LastEvent=at;
            if(name is not ("task_started" or "task_complete" or "turn_aborted"))return;
            if(!payload.TryGetProperty("turn_id",out var identifier)||identifier.ValueKind!=JsonValueKind.String){state.Invalid=true;state.Active.Clear();return;}
            var id=identifier.GetString();if(string.IsNullOrEmpty(id)||id.Length>160){state.Invalid=true;state.Active.Clear();return;}
            if(state.Terminal.ContainsKey(id))return;
            if(name=="task_started"&&!state.Active.ContainsKey(id)&&state.EvictedTerminalUntil is {} floor&&parsed<=floor){state.Invalid=true;state.Active.Clear();return;}
            if(name=="task_started"){if(state.Active.ContainsKey(id))return;if(state.Active.Count>=16){state.Active.Clear();state.Invalid=true;return;}state.Active.Add(id,(parsed,monotonicMs(),Math.Max(0,(long)(utcNow()-parsed).TotalMilliseconds)));state.Invalid=false;}
            else{state.Active.Remove(id);if(state.Terminal.Count>=64){var oldest=state.Terminal.MinBy(p=>p.Value);state.Terminal.Remove(oldest.Key);if(state.EvictedTerminalUntil is null||oldest.Value>state.EvictedTerminalUntil)state.EvictedTerminalUntil=oldest.Value;}state.Terminal[id]=parsed;state.Invalid=false;}
            if(!previouslyActive||at>state.LastEvent)state.LastEvent=at;
        }
        catch(Exception ex)when(ex is JsonException or InvalidOperationException or FormatException){state.Invalid=true;state.Active.Clear();}
    }
    void Touch(FileState state,byte[] prefix)
    {
        if(state.Active.Count==0)return;
        try{var reader=new Utf8JsonReader(prefix,isFinalBlock:false,state:default);while(reader.Read())if(reader.TokenType==JsonTokenType.PropertyName&&reader.CurrentDepth==1&&reader.ValueTextEquals("timestamp")){
            if(reader.Read()&&reader.TokenType==JsonTokenType.String&&reader.TryGetDateTimeOffset(out var stamp)&&stamp<=utcNow()&&stamp.UtcDateTime>state.LastEvent)state.LastEvent=stamp.UtcDateTime;return;
        }}catch(JsonException){}
    }
    static bool IgnoredRecord(byte[] prefix,bool oversized=false)
    {
        try{
            var reader=new Utf8JsonReader(prefix,isFinalBlock:false,state:default);
            while(reader.Read()){
                if(reader.TokenType==JsonTokenType.PropertyName&&reader.CurrentDepth==1&&reader.ValueTextEquals("type")){
                    if(!reader.Read()||reader.TokenType!=JsonTokenType.String)return false;
                    if(reader.ValueTextEquals("response_item")||reader.ValueTextEquals("turn_context")||reader.ValueTextEquals("session_meta")||reader.ValueTextEquals("compacted"))return true;
                    if(!reader.ValueTextEquals("event_msg"))return false;
                    while(reader.Read())if(reader.TokenType==JsonTokenType.PropertyName&&reader.CurrentDepth==2&&reader.ValueTextEquals("type")){if(!reader.Read()||reader.TokenType!=JsonTokenType.String)return false;return (oversized&&reader.ValueTextEquals("token_count"))||reader.ValueTextEquals("item_completed")||reader.ValueTextEquals("item_started")||reader.ValueTextEquals("agent_message")||reader.ValueTextEquals("agent_reasoning");}
                    return false;
                }
            }
        }catch(JsonException){}return false;
    }
    internal static string ReadTitleIdentity(Stream stream,string fallback)
    {
        long previous=stream.Position;
        try{
            stream.Position=0;var buffer=new byte[(int)Math.Min(65536,stream.Length)];int size=stream.Read(buffer);int end=Array.IndexOf(buffer,(byte)10,0,size);if(end<0)end=size;
            using var document=JsonDocument.Parse(buffer.AsMemory(0,end),new JsonDocumentOptions{MaxDepth=16});var root=document.RootElement;
            if(root.TryGetProperty("type",out var type)&&type.ValueKind==JsonValueKind.String&&type.GetString()=="session_meta"&&root.TryGetProperty("payload",out var payload)&&payload.ValueKind==JsonValueKind.Object&&payload.TryGetProperty("session_id",out var id)&&id.ValueKind==JsonValueKind.String&&Guid.TryParse(id.GetString(),out var logical))return logical.ToString();
        }catch(JsonException){}finally{stream.Position=previous;}
        return fallback;
    }
    void ReadTitles()
    {
        if(monotonicMs()<nextTitleRead)return;nextTitleRead=monotonicMs()+5000;
        // This index contains only id/title/update metadata. Never fall back to message text.
        try{
            var index=Path.Combine(Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(directory))??directory,"session_index.jsonl");
            using var stream=new FileStream(index,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete);
            bool partial=stream.Length>262144;stream.Seek(Math.Max(0,stream.Length-262144),SeekOrigin.Begin);
            using var reader=new StreamReader(stream,Encoding.UTF8,false,4096);if(partial)reader.ReadLine();
            var updated=new Dictionary<string,string>();string? line;
            while((line=reader.ReadLine()) is not null){if(line.Length>8192)continue;try{
                using var doc=JsonDocument.Parse(line,new JsonDocumentOptions{MaxDepth=8});var item=doc.RootElement;
                if(!item.TryGetProperty("id",out var id)||!item.TryGetProperty("thread_name",out var name)||id.ValueKind!=JsonValueKind.String||name.ValueKind!=JsonValueKind.String)continue;
                string? key=id.GetString(),value=name.GetString();if(key is null||key.Length>160||string.IsNullOrWhiteSpace(value))continue;
                value=string.Concat(value.EnumerateRunes().Where(r=>!System.Text.Rune.IsControl(r)).Take(160).Select(r=>r.ToString()));
                if(value.Length>0)updated[key]=value;
            }catch(JsonException){}}
            titles.Clear();foreach(var pair in updated)titles[pair.Key]=pair.Value;
        }catch(IOException){titles.Clear();}catch(UnauthorizedAccessException){titles.Clear();}
    }
    public async ValueTask DisposeAsync(){stop.Cancel();await task;stop.Dispose();}
}
