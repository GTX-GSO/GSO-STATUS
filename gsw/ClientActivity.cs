using System.Buffers;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace GsoStatus;

// Per-client facts for the VIBE detail page: is the AI client running, and how many tokens did it use today (local
// calendar day). Tokens come from the clients' own local logs, counted incrementally and never published as text:
//   Claude: assistant "usage" records in ~/.claude/projects (input + cache write + output, deduplicated by message id,
//           so resumed or forked transcripts do not count twice).
//   Codex:  token_count events in ~/.codex/sessions (input without cached reads + output, as growth of the running total).
// Cache reads are excluded for both: they are re-reads of earlier context and would dwarf the real consumption.
sealed class ClientActivity:IDisposable
{
    internal readonly record struct ProcessInfo(string Name,string Path);
    const int ChunkSize=256*1024,MaxLine=4*1024*1024;
    const long ScanBudgetMs=1500;
    readonly string[] claudeRoots;
    readonly string codexRoot;
    readonly Func<DateTime> localNow;
    readonly Func<IReadOnlyList<ProcessInfo>> processes;
    readonly object gate=new();
    readonly CancellationTokenSource stop=new();
    readonly Task? loop;
    DateTime day;
    // Claude: message key -> counted tokens; transcript offsets.
    readonly Dictionary<string,long> claudeMessages=new();
    readonly Dictionary<string,long> claudeOffsets=new();
    long claudeTotal;
    // Codex: per-session-file offset, previous running value and today's sum.
    readonly Dictionary<string,(long Offset,long Previous,long Sum)> codexFiles=new();
    bool claudeReady,codexReady;
    bool claudeOnline,codexOnline;
    public ClientActivity(string[] claudeRoots,string codexRoot,bool start=true,Func<DateTime>? localNow=null,Func<IReadOnlyList<ProcessInfo>>? processes=null)
    {
        this.claudeRoots=claudeRoots;this.codexRoot=codexRoot;this.localNow=localNow??(()=>DateTime.Now);this.processes=processes??ListProcesses;
        day=this.localNow().Date;
        loop=start?Task.Run(Loop):null;
    }
    // One pass per second: processes every 3 s, logs every 5 s. Both run off the 1 Hz telemetry thread.
    async Task Loop()
    {
        long nextScan=0,nextProcesses=0;
        while(!stop.IsCancellationRequested)
        {
            long now=Environment.TickCount64;
            if(now>=nextProcesses){RefreshProcesses();nextProcesses=now+3000;}
            if(now>=nextScan){Scan();nextScan=Environment.TickCount64+5000;}
            try{await Task.Delay(1000,stop.Token);}catch(OperationCanceledException){}
        }
    }
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct ProcessEntry
    {
        public uint Size,Usage,ProcessId;public UIntPtr DefaultHeap;public uint ModuleId,Threads,ParentProcessId;public int PriorityBase;public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=260)] public string ExeFile;
    }
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags,uint processId);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool Process32FirstW(IntPtr snapshot,ref ProcessEntry entry);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool Process32NextW(IntPtr snapshot,ref ProcessEntry entry);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,uint processId);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool QueryFullProcessImageNameW(IntPtr process,uint flags,System.Text.StringBuilder name,ref uint size);
    // One toolhelp snapshot of the process names; Process.GetProcessesByName reads every process and thread, which cost
    // more CPU than the rest of this class. Only the Codex desktop app (ChatGPT.exe) needs its install path.
    internal static IReadOnlyList<ProcessInfo> ListProcesses()
    {
        var list=new List<ProcessInfo>();
        var snapshot=CreateToolhelp32Snapshot(2,0);   // TH32CS_SNAPPROCESS
        if(snapshot==new IntPtr(-1))throw new InvalidOperationException("process snapshot failed");
        try{
            var entry=new ProcessEntry{Size=(uint)Marshal.SizeOf<ProcessEntry>()};
            for(bool more=Process32FirstW(snapshot,ref entry);more;more=Process32NextW(snapshot,ref entry))
            {
                var file=entry.ExeFile;
                if(!file.EndsWith(".exe",StringComparison.OrdinalIgnoreCase))continue;
                var name=file[..^4];
                if(name.Equals("claude",StringComparison.OrdinalIgnoreCase)||name.Equals("codex",StringComparison.OrdinalIgnoreCase)||name.StartsWith("codex-",StringComparison.OrdinalIgnoreCase))list.Add(new ProcessInfo(name,""));
                else if(name.Equals("ChatGPT",StringComparison.OrdinalIgnoreCase))list.Add(new ProcessInfo(name,ImagePath(entry.ProcessId)));
            }
        }finally{CloseHandle(snapshot);}
        return list;
    }
    static string ImagePath(uint processId)
    {
        var handle=OpenProcess(0x1000,false,processId);   // PROCESS_QUERY_LIMITED_INFORMATION
        if(handle==IntPtr.Zero)return "";
        try{var text=new System.Text.StringBuilder(1024);uint size=(uint)text.Capacity;return QueryFullProcessImageNameW(handle,0,text,ref size)?text.ToString():"";}
        finally{CloseHandle(handle);}
    }
    // The Codex desktop app runs as ChatGPT.exe inside the OpenAI.Codex package; Claude (desktop and Code CLI) is claude.exe.
    internal static (bool Claude,bool Codex) Classify(IEnumerable<ProcessInfo> list)
    {
        bool claude=false,codex=false;
        foreach(var p in list)
        {
            if(p.Name.Equals("claude",StringComparison.OrdinalIgnoreCase))claude=true;
            else if(p.Name.Equals("codex",StringComparison.OrdinalIgnoreCase)||p.Name.StartsWith("codex-",StringComparison.OrdinalIgnoreCase)&&!p.Name.Contains("sandbox",StringComparison.OrdinalIgnoreCase)||p.Name.Equals("ChatGPT",StringComparison.OrdinalIgnoreCase)&&p.Path.Contains("OpenAI.Codex",StringComparison.OrdinalIgnoreCase))codex=true;
        }
        return (claude,codex);
    }
    internal void RefreshProcesses()
    {
        try{var found=Classify(processes());lock(gate){claudeOnline=found.Claude;codexOnline=found.Codex;}}
        catch(Exception ex)when(ex is InvalidOperationException or System.ComponentModel.Win32Exception){ /* keep the last value */ }
    }
    public void Scan()
    {
        try{
            var today=localNow().Date;
            if(today!=day){lock(gate){day=today;claudeMessages.Clear();claudeOffsets.Clear();claudeTotal=0;codexFiles.Clear();claudeReady=codexReady=false;}}
            var budget=Stopwatch.StartNew();
            ScanClaude(today,budget);ScanCodex(today,budget);
        }catch(Exception ex)when(ex is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException){ /* retry next scan */ }
    }
    // Files touched today. The directory enumeration already carries size and time, so an unchanged file costs no extra system call.
    static List<FileInfo> TodayFiles(string root,DateTime today)
    {
        var result=new List<FileInfo>();
        if(!Directory.Exists(root))return result;
        try{
            foreach(var file in new DirectoryInfo(root).EnumerateFiles("*.jsonl",new EnumerationOptions{RecurseSubdirectories=true,IgnoreInaccessible=true,AttributesToSkip=FileAttributes.ReparsePoint}))
                if(file.LastWriteTime>=today)result.Add(file);
        }catch(Exception ex)when(ex is IOException or UnauthorizedAccessException){ /* a partial list is fine */ }
        return result;
    }
    delegate void LineHandler(ReadOnlyMemory<byte> line,long start);
    // Streams the complete new lines of one file in 256 KiB chunks through a pooled buffer (lines up to 4 MiB; a longer
    // one is skipped). Returns the offset after the last complete line, whether the file restarted from zero, and
    // whether the time budget ended the pass early.
    static (long Offset,bool Reset,bool More) ReadLines(string path,long offset,Stopwatch budget,LineHandler onLine)
    {
        using var stream=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete,1,FileOptions.SequentialScan);
        bool reset=stream.Length<offset;if(reset)offset=0;
        stream.Position=offset;
        var buffer=ArrayPool<byte>.Shared.Rent(ChunkSize*2);
        try{
            long origin=offset;   // absolute offset of buffer[0], the start of the unfinished tail
            int fill=0;bool dropping=false;
            while(true)
            {
                if(budget.ElapsedMilliseconds>ScanBudgetMs)return (origin,reset,true);
                if(buffer.Length-fill<ChunkSize){
                    if(buffer.Length>=MaxLine+ChunkSize){origin+=fill;fill=0;dropping=true;}   // oversized line: skip to its end
                    else{var bigger=ArrayPool<byte>.Shared.Rent(buffer.Length*2);Buffer.BlockCopy(buffer,0,bigger,0,fill);ArrayPool<byte>.Shared.Return(buffer);buffer=bigger;}
                }
                int read=stream.Read(buffer,fill,ChunkSize);
                if(read==0)return (origin,reset,false);
                int end=fill+read,start=0;
                for(int i=fill;i<end;i++)
                {
                    if(buffer[i]!=10)continue;
                    if(!dropping&&i>start)onLine(buffer.AsMemory(start,i-start),origin+start);
                    dropping=false;start=i+1;
                }
                origin+=start;
                int tail=end-start;
                if(dropping){origin+=tail;tail=0;}
                else if(tail>0&&start>0)Buffer.BlockCopy(buffer,start,buffer,0,tail);
                fill=tail;
            }
        }finally{ArrayPool<byte>.Shared.Return(buffer);}
    }
    static readonly byte[] UsageMarker=Encoding.UTF8.GetBytes("\"usage\""),TokenMarker=Encoding.UTF8.GetBytes("\"token_count\"");
    static bool Contains(ReadOnlyMemory<byte> line,byte[] marker)=>line.Span.IndexOf(marker)>=0;
    static long Field(JsonElement element,string name)=>element.TryGetProperty(name,out var v)&&v.ValueKind==JsonValueKind.Number&&v.TryGetInt64(out var n)&&n>0?n:0;
    void ScanClaude(DateTime today,Stopwatch budget)
    {
        bool behind=false;
        foreach(var root in claudeRoots)
            foreach(var file in TodayFiles(root,today))
            {
                string path=file.FullName;long offset;lock(gate)offset=claudeOffsets.GetValueOrDefault(path);
                if(file.Length==offset)continue;
                var found=new List<(string Key,long Tokens)>();
                (long Offset,bool Reset,bool More) result;
                try{
                    result=ReadLines(path,offset,budget,(line,start)=>{
                        if(!Contains(line,UsageMarker))return;
                        try{
                            using var document=JsonDocument.Parse(line,new JsonDocumentOptions{MaxDepth=32});var r=document.RootElement;
                            if(!r.TryGetProperty("type",out var type)||type.GetString()!="assistant"||!r.TryGetProperty("message",out var message)||message.ValueKind!=JsonValueKind.Object||!message.TryGetProperty("usage",out var usage)||usage.ValueKind!=JsonValueKind.Object)return;
                            if(!r.TryGetProperty("timestamp",out var stamp)||stamp.ValueKind!=JsonValueKind.String||!DateTimeOffset.TryParse(stamp.GetString(),out var at)||at.ToLocalTime().Date!=today)return;
                            long tokens=Field(usage,"input_tokens")+Field(usage,"cache_creation_input_tokens")+Field(usage,"output_tokens");
                            string id=message.TryGetProperty("id",out var m)&&m.ValueKind==JsonValueKind.String?m.GetString()!:"";
                            string request=r.TryGetProperty("requestId",out var q)&&q.ValueKind==JsonValueKind.String?q.GetString()!:"";
                            found.Add((id.Length>0?id+"|"+request:path+"|"+start,tokens));
                        }catch(Exception ex)when(ex is JsonException or InvalidOperationException){ /* skip damaged record */ }
                    });
                }catch(IOException){continue;}
                behind|=result.More;
                lock(gate){
                    claudeOffsets[path]=result.Offset;
                    foreach(var (key,tokens) in found){long before=claudeMessages.GetValueOrDefault(key);if(tokens>before){claudeMessages[key]=tokens;claudeTotal+=tokens-before;}}
                }
            }
        lock(gate)claudeReady=!behind;
    }
    void ScanCodex(DateTime today,Stopwatch budget)
    {
        bool behind=false;
        foreach(var file in TodayFiles(codexRoot,today))
        {
            string path=file.FullName;(long Offset,long Previous,long Sum) state;lock(gate)state=codexFiles.GetValueOrDefault(path);
            if(file.Length==state.Offset)continue;
            if(file.Length<state.Offset)state=(0,0,0);   // restarted file: recount it from zero
            long previous=state.Previous,sum=state.Sum;
            (long Offset,bool Reset,bool More) result;
            try{
                result=ReadLines(path,state.Offset,budget,(line,start)=>{
                    if(!Contains(line,TokenMarker))return;
                    try{
                        using var document=JsonDocument.Parse(line,new JsonDocumentOptions{MaxDepth=32});var r=document.RootElement;
                        if(!r.TryGetProperty("payload",out var payload)||payload.ValueKind!=JsonValueKind.Object||!payload.TryGetProperty("type",out var type)||type.GetString()!="token_count"||!payload.TryGetProperty("info",out var info)||info.ValueKind!=JsonValueKind.Object||!info.TryGetProperty("total_token_usage",out var total)||total.ValueKind!=JsonValueKind.Object)return;
                        long value=Math.Max(0,Field(total,"total_tokens")-Field(total,"cached_input_tokens"));
                        if(value<previous){previous=value;return;}
                        long delta=value-previous;previous=value;
                        if(delta>0&&r.TryGetProperty("timestamp",out var stamp)&&stamp.ValueKind==JsonValueKind.String&&DateTimeOffset.TryParse(stamp.GetString(),out var at)&&at.ToLocalTime().Date==today)sum+=delta;
                    }catch(Exception ex)when(ex is JsonException or InvalidOperationException){ /* skip damaged record */ }
                });
            }catch(IOException){continue;}
            behind|=result.More;
            lock(gate)codexFiles[path]=(result.Offset,previous,sum);
        }
        lock(gate)codexReady=!behind;
    }
    // [{id, online, tokens_today}] for the contract's optional codex.clients; tokens_today is null until the first pass ends.
    public JsonArray Snapshot()
    {
        lock(gate)return new JsonArray(
            new JsonObject{["id"]="codex",["online"]=codexOnline,["tokens_today"]=codexReady?JsonValue.Create(codexFiles.Values.Sum(v=>v.Sum)):null},
            new JsonObject{["id"]="claude",["online"]=claudeOnline,["tokens_today"]=claudeReady?JsonValue.Create(claudeTotal):null});
    }
    public void Dispose(){stop.Cancel();try{loop?.GetAwaiter().GetResult();}catch(OperationCanceledException){}stop.Dispose();}
}
