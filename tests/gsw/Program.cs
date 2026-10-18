using System.Text.Json.Nodes;
using GsoStatus;

if(args.Contains("--game-probe")){await GameProbe.Run();return;}
static void Check(bool condition,string name){if(!condition)throw new Exception(name);}
if(args.Contains("--native-tray-smoke"))
{
    foreach(var byCancellation in new[]{false,true})
    {
        using var stop=new CancellationTokenSource();
        var trayClock=System.Diagnostics.Stopwatch.StartNew();var reads=0;
        var task=byCancellation?Task.Delay(Timeout.Infinite,stop.Token):Task.Delay(1500);
        if(byCancellation)stop.CancelAfter(1500);
        NativeTray.Run(stop,task,()=>{reads++;return "受控托盘验证";},()=>throw new Exception("unexpected config action"));
        Check(stop.IsCancellationRequested&&reads>=2&&trayClock.Elapsed.TotalSeconds<5,"native timer shutdown and cleanup");
    }
    Console.WriteLine("Native tray task completion and cancellation passed; no config or household action");return;
}
if(args.Contains("--observe-local")){
 await using var local=new CodexTasks(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),".codex","sessions"),false);
 for(int scan=0;scan<40;scan++)local.Scan();var observed=local.Snapshot();
 Console.WriteLine(new JsonObject{["state"]=observed.State,["sessions"]=new JsonArray(observed.Sessions.Select(x=>(JsonNode)new JsonObject{["state"]=x?["state"]?.DeepClone(),["has_title"]=x?["title"] is not null,["duration_ms"]=x?["running_duration_ms"]?.DeepClone()}).ToArray())}.ToJsonString());return;
}
var folder=Path.GetFullPath(Path.Combine("work","gsw-tests",Guid.NewGuid().ToString()));
var install=Path.Combine(folder,"codex-bin");Directory.CreateDirectory(Path.Combine(install,"new"));var exe=Path.Combine(install,"new","codex.exe");File.WriteAllText(exe,"");
Check(CodexQuota.ResolveExecutable(Path.Combine(folder,"removed","codex.exe"),install)==exe,"rotated Codex installation is rediscovered");
Check(CodexQuota.ResolveExecutable(exe,install)==exe,"existing explicit executable retained");
Check(CodexQuota.ResolveExecutable("",Path.Combine(folder,"missing"))=="","missing installation bounded failure");
var day=DateTime.UtcNow;var date=Path.Combine(folder,day.ToString("yyyy"),day.ToString("MM"),day.ToString("dd"));Directory.CreateDirectory(date);var file=Path.Combine(date,"test.jsonl");File.WriteAllText(file,"");
await using var tasks=new CodexTasks(folder,false);
void Append(string type,string turn,bool newline=true){var item=new JsonObject{["type"]="event_msg",["timestamp"]=DateTimeOffset.UtcNow.ToString("O"),["payload"]=new JsonObject{["type"]=type,["turn_id"]=turn}};File.AppendAllText(file,item.ToJsonString()+(newline?"\n":""));tasks.Scan();}
tasks.Scan();Check(tasks.Snapshot().State=="idle","healthy scan without tasks is ready");
Append("task_started","one");Check(tasks.Snapshot().State=="running","start");
Append("task_started","one");Check(tasks.Snapshot().State=="running","duplicate start");
Append("task_started","two");Append("task_complete","one");Check(tasks.Snapshot().State=="running","parallel task remains");
Append("task_complete","two",false);Check(tasks.Snapshot().State=="running","partial line ignored");File.AppendAllText(file,"\n");tasks.Scan();Check(tasks.Snapshot().State=="idle","completed tasks return ready");Check(tasks.Snapshot().Sessions[0]?["state"]?.GetValue<string>()=="idle","paired local completion");
Append("task_complete","two");Check(tasks.Snapshot().Sessions[0]?["state"]?.GetValue<string>()=="idle","duplicate completion");
Append("task_started","two");Check(tasks.Snapshot().State=="idle","late duplicate start cannot resurrect");
Append("task_started","three");Append("turn_aborted","three");Check(tasks.Snapshot().State=="idle","aborted task is no longer running");
File.WriteAllText(file,"\n");tasks.Scan();Check(tasks.Snapshot().State=="idle","truncate clears finished task observation");
Append("task_started","four");File.AppendAllText(file,"{\"type\":5}\n");tasks.Scan();Check(tasks.Snapshot().State=="unknown","malformed event type cannot preserve running");
Append("task_started","recovery");Append("task_complete","recovery");Check(tasks.Snapshot().State=="idle","valid task events recover after malformed input");
await using var missingTasks=new CodexTasks(Path.Combine(folder,"missing"),false);missingTasks.Scan();Check(missingTasks.Snapshot().State=="unknown","missing observation directory remains unknown");
var quotas=CodexQuota.ParseWindows(JsonNode.Parse("""{"rateLimitsByLimitId":{"codex":{"primary":{"usedPercent":0,"windowDurationMins":300,"resetsAt":1800000000},"secondary":{"usedPercent":null,"windowDurationMins":10080,"resetsAt":null}}}}""")!.AsObject());
Check(quotas.Count==2,"quota windows");Check(quotas[0]?["remaining_percent"]?["value"]?.GetValue<double>()==100,"zero used is 100 remaining");Check(quotas[1]?["remaining_percent"]?["value"] is null,"missing quota not zero");Check(quotas[1]?["resets_at"] is null,"missing reset not fabricated");
Console.WriteLine("PASS: lifecycle duplicate/parallel/partial/abort/truncate cases and quota null/window mapping");

// Claude Code transcripts: synthetic records only, isolated folder, no real sessions read.
var claudeRoot=Path.Combine(folder,"claude-projects");var claudeProject=Path.Combine(claudeRoot,"C--work-demo");Directory.CreateDirectory(claudeProject);
const string claudeSession="0f8fad5b-d9cb-469f-a165-70867728950e";var transcript=Path.Combine(claudeProject,claudeSession+".jsonl");File.WriteAllText(transcript,"");
var claudeClock=DateTimeOffset.UtcNow.AddMinutes(-30);long claudeMono=1000;
await using var claude=new ClaudeTasks([claudeRoot],false,()=>claudeClock,()=>claudeMono);
void Advance(int seconds){claudeClock=claudeClock.AddSeconds(seconds);claudeMono+=seconds*1000L;}
void Line(JsonObject item,bool newline=true){item["timestamp"]??=claudeClock.ToString("O");item["sessionId"]??=claudeSession;File.AppendAllText(transcript,item.ToJsonString()+(newline?"\n":""));claude.Scan();}
JsonObject User(JsonNode content)=>new(){["type"]="user",["message"]=new JsonObject{["role"]="user",["content"]=content}};
JsonObject Assistant(string? stop,string block="text")=>new(){["type"]="assistant",["message"]=new JsonObject{["role"]="assistant",["stop_reason"]=stop,["content"]=new JsonArray(new JsonObject{["type"]=block})}};
string ClaudeState()=>claude.Snapshot().State;JsonNode ClaudeFirst()=>claude.Snapshot().Sessions[0]!;
claude.Scan();Check(ClaudeState()=="idle","empty Claude transcript is ready");
await using(var absent=new ClaudeTasks([Path.Combine(folder,"no-claude")],false)){absent.Scan();Check(absent.Snapshot().State=="absent","missing Claude root is absent, not unknown");}
Line(User("<command-name>/model</command-name>"));Check(ClaudeState()=="idle","local slash command is not a task");
Line(new JsonObject{["type"]="user",["isMeta"]=true,["message"]=new JsonObject{["content"]="caveat"}});Check(ClaudeState()=="idle","meta record is not a task");
Line(User("实现VIBE"));Check(ClaudeState()=="running","prompt starts Claude turn");
Check(ClaudeFirst()["id"]!.GetValue<string>().StartsWith("claude-")&&ClaudeFirst()["id"]!.GetValue<string>().Length==23,"Claude id is prefixed and hashed");
Check(ClaudeFirst()["title"] is null&&ClaudeFirst()["title_reason"]!.GetValue<string>()=="session_title_unavailable","prompt text never becomes a title");
Line(new JsonObject{["type"]="custom-title",["customTitle"]="other",["sessionId"]="11111111-2222-3333-4444-555555555555"});Check(ClaudeFirst()["title"] is null,"foreign title ignored");
Line(new JsonObject{["type"]="ai-title",["aiTitle"]="自动标题"});Check(ClaudeFirst()["title"]!.GetValue<string>()=="自动标题","AI title used");
Line(new JsonObject{["type"]="custom-title",["customTitle"]="VIBE接入\u0007Claude"});Check(ClaudeFirst()["title"]!.GetValue<string>()=="VIBE接入Claude","custom title wins and is sanitized");
Advance(5);Line(Assistant("tool_use","tool_use"));Line(User(new JsonArray(new JsonObject{["type"]="tool_result",["tool_use_id"]="t",["content"]="ok"})));
Check(ClaudeState()=="running"&&ClaudeFirst()["running_duration_ms"]!.GetValue<long>()==5000,"tool round trip keeps turn and duration");
Line(Assistant(null));Check(ClaudeState()=="running","streamed block without stop reason continues");
Line(new JsonObject{["type"]="assistant",["isSidechain"]=true,["message"]=new JsonObject{["stop_reason"]="end_turn"}});Check(ClaudeState()=="running","subagent completion does not end parent turn");
Advance(1);Line(Assistant("end_turn"));Check(ClaudeState()=="idle"&&ClaudeFirst()["duration_reason"]!.GetValue<string>()=="not_running"&&ClaudeFirst()["running_duration_ms"] is null,"end_turn returns ready and clears timer");
Line(User("again"));Check(ClaudeState()=="running","next prompt");Advance(100);claude.Scan();Check(ClaudeState()=="running","100 s without events is still working");Advance(21);claude.Scan();Check(ClaudeState()=="idle","120 s without events is ready");
Line(User("third"));Check(ClaudeState()=="running"&&ClaudeFirst()["running_duration_ms"]!.GetValue<long>()==0,"prompt after timeout starts a new turn");
Line(User(new JsonArray(new JsonObject{["type"]="text",["text"]="[Request interrupted by user]"})));Check(ClaudeState()=="idle","interrupt ends turn");
Line(User("x"));Line(new JsonObject{["type"]="system",["subtype"]="turn_duration",["durationMs"]=10});Check(ClaudeState()=="idle","turn_duration ends turn");
Line(User("y"));Line(new JsonObject{["type"]="assistant",["isApiErrorMessage"]=true,["message"]=new JsonObject{["content"]=new JsonArray()}});Check(ClaudeState()=="idle","API error ends turn");
Line(User(new string('a',70000)));Check(ClaudeState()=="running","oversized user record bounded and observed");Line(Assistant("end_turn"));Check(ClaudeState()=="idle","recovered after oversized record");
Line(User("partial"),false);Check(ClaudeState()=="idle","partial line ignored");File.AppendAllText(transcript,"\n");claude.Scan();Check(ClaudeState()=="running","completed line applied");
File.AppendAllText(transcript,"{bad json\n");claude.Scan();Check(ClaudeState()=="unknown","malformed record cannot stay running");
Line(User("z"));Check(ClaudeState()=="running","new prompt recovers");Line(Assistant("end_turn"));
File.WriteAllText(transcript,"");claude.Scan();Check(ClaudeState()=="idle","truncation resets idle transcript");
File.SetLastWriteTimeUtc(transcript,DateTime.UtcNow.AddDays(-5));claude.Scan();Check(claude.Snapshot().Sessions.Count==0&&ClaudeState()=="idle","old transcripts outside 3-day window ignored");
Console.WriteLine("PASS: Claude prompt/tool/stream/sidechain/end/interrupt/timeout/oversized/partial/malformed/title cases");

JsonObject ToolUse(string name,string id)=>new(){["type"]="assistant",["message"]=new JsonObject{["role"]="assistant",["stop_reason"]="tool_use",["content"]=new JsonArray(new JsonObject{["type"]="tool_use",["id"]=id,["name"]=name})}};
JsonObject ToolResult(string id)=>User(new JsonArray(new JsonObject{["type"]="tool_result",["tool_use_id"]=id}));
Line(User("hand off"));Line(ToolUse("Bash","t1"));Line(ToolResult("t1"));Advance(10);claude.Scan();Check(ClaudeState()=="running","ordinary tool round trip keeps the event window");
Line(ToolUse("mcp__ccd_turn__wrap_up","t2"));Line(ToolResult("t2"));Check(ClaudeState()=="running","wrap_up result is still inside the short window");
Advance(4);claude.Scan();Check(ClaudeState()=="idle","wrap_up hands the turn back after 3 s");
Line(User("next prompt"));Check(ClaudeState()=="running"&&ClaudeFirst()["running_duration_ms"]!.GetValue<long>()==0,"prompt after a hand-off starts a new turn with the event window");Advance(10);claude.Scan();Check(ClaudeState()=="running","new turn is not cut short by the earlier hand-off");
Line(ToolUse("AskUserQuestion","t3"));Advance(4);claude.Scan();Check(ClaudeState()=="idle","waiting for the user is ready after 3 s");
Line(ToolResult("t3"));Check(ClaudeState()=="running","answer resumes the work");Advance(10);claude.Scan();Check(ClaudeState()=="running","answered turn uses the event window again");
Line(Assistant("end_turn"));Check(ClaudeState()=="idle","end_turn still ends immediately");
Line(User("long build"));Line(ToolUse("Bash","t4"));Advance(300);claude.Scan();Check(ClaudeState()=="running","a tool call that is still running keeps the turn going for minutes");
Advance(301);claude.Scan();Check(ClaudeState()=="idle","a tool that never returns does not keep the turn forever");
Line(User("again"));Line(ToolUse("Bash","t5"));Advance(200);Line(ToolResult("t5"));Advance(100);claude.Scan();Check(ClaudeState()=="running","the long window ends with the tool result");Advance(30);claude.Scan();Check(ClaudeState()=="idle","after the result the normal 120 s window applies");
Line(User("cancel"));Line(ToolUse("Bash","t6"));Line(User(new JsonArray(new JsonObject{["type"]="text",["text"]="[Request interrupted by user for tool use]"})));Check(ClaudeState()=="idle","cancelling a running tool ends the turn at once");
Console.WriteLine("PASS: Claude hand-off tools (wrap_up, ask, plan) show ready after 3 s; ordinary tools keep the turn running up to 10 minutes");
var bridgePath=Path.Combine(folder,"claude-rate-limits.json");var quotaClock=DateTimeOffset.FromUnixTimeSeconds(1800000000);
var claudeQuota=new ClaudeQuota(bridgePath,()=>quotaClock);
Check(claudeQuota.Snapshot().Source["quality"]!.GetValue<string>()=="unconfigured","no bridge is unconfigured");
var statusOut=new StringWriter();ClaudeQuota.RunStatusLine(new StringReader("{\"model\":{\"display_name\":\"Opus\"},\"session_name\":\"secret\",\"rate_limits\":{\"five_hour\":{\"used_percentage\":23.5,\"resets_at\":1800003600},\"seven_day\":{\"used_percentage\":41.2,\"resets_at\":1800300000}}}"),statusOut,bridgePath,()=>quotaClock);
Check(statusOut.ToString().Trim()=="[Opus] 5h 77% | 7d 59% left","status line output");Check(!File.ReadAllText(bridgePath).Contains("secret")&&!File.ReadAllText(bridgePath).Contains("Opus"),"bridge stores only limits");
var cq=claudeQuota.Snapshot();Check(cq.Source["quality"]!.GetValue<string>()=="ok"&&cq.Windows.Count==2,"bridge read");
Check(cq.Windows[0]!["bucket"]!.GetValue<string>()=="claude"&&cq.Windows[0]!["window_duration_mins"]!.GetValue<int>()==300&&cq.Windows[0]!["remaining_percent"]!["value"]!.GetValue<double>()==76.5,"five hour window");
Check(cq.Windows[1]!["window_duration_mins"]!.GetValue<int>()==10080&&Math.Abs(cq.Windows[1]!["remaining_percent"]!["value"]!.GetValue<double>()-58.8)<1e-9&&cq.Windows[1]!["resets_at"] is not null,"weekly window");
ClaudeQuota.RunStatusLine(new StringReader("{\"model\":{\"display_name\":\"Opus\"}}"),new StringWriter(),bridgePath,()=>quotaClock.AddSeconds(10));Check(claudeQuota.Snapshot().Windows[0]!["remaining_percent"]!["value"] is not null,"status line without limits keeps last bridge");
quotaClock=quotaClock.AddMinutes(31);cq=claudeQuota.Snapshot();Check(cq.Source["quality"]!.GetValue<string>()=="stale"&&cq.Windows[0]!["remaining_percent"]!["value"] is null&&cq.Windows[0]!["remaining_percent"]!["reason"]!.GetValue<string>()=="quota_stale","stale bridge hides values");
quotaClock=DateTimeOffset.FromUnixTimeSeconds(1800003700);cq=claudeQuota.Snapshot();Check(cq.Windows[0]!["resets_at"] is null&&cq.Windows[0]!["remaining_percent"]!["reason"]!.GetValue<string>()=="quota_window_reset","passed reset not reused");
File.WriteAllText(bridgePath,"nonsense");cq=claudeQuota.Snapshot();Check(cq.Source["quality"]!.GetValue<string>()=="error"&&cq.Windows.Count==0,"invalid bridge is error");
Console.WriteLine("PASS: Claude status line bridge, privacy, stale, reset and invalid cases");
var settingsDir=Path.Combine(folder,"claude-config");var settingsFile=Path.Combine(settingsDir,"settings.json");const string fakeGsw=@"C:\Tools\GSO STATUS\GSW.exe";
Check(ClaudeQuota.ConfigureStatusLine(settingsFile,fakeGsw,true,false).Code==4&&!Directory.Exists(settingsDir),"missing Claude config directory left untouched");
Directory.CreateDirectory(settingsDir);File.WriteAllText(settingsFile,"{\n  // user comment\n  \"model\": \"opus\",\n  \"permissions\": {\"allow\": [\"Bash(ls)\"]},\n  \"statusLine\": {\"type\": \"command\", \"command\": \"mine\"},\n}");
Check(ClaudeQuota.ConfigureStatusLine(settingsFile,fakeGsw,true,false).Code==3&&File.ReadAllText(settingsFile).Contains("\"mine\""),"foreign status line not replaced without force");
Check(ClaudeQuota.ConfigureStatusLine(settingsFile,fakeGsw,true,true).Code==0,"forced install");
var settingsJson=JsonNode.Parse(File.ReadAllText(settingsFile))!;Check(settingsJson["statusLine"]!["command"]!.GetValue<string>()=="\"C:/Tools/GSO STATUS/GSW.exe\" --claude-statusline"&&settingsJson["model"]!.GetValue<string>()=="opus"&&settingsJson["permissions"]!["allow"]![0]!.GetValue<string>()=="Bash(ls)","install keeps other settings and quotes forward-slash path");
Check(Directory.GetFiles(settingsDir,"settings.json.gso-backup-*").Length==1,"settings backed up before change");
Check(ClaudeQuota.ConfigureStatusLine(settingsFile,fakeGsw,true,false).Message.Contains("无需修改"),"install is idempotent");
Check(ClaudeQuota.ConfigureStatusLine(settingsFile,fakeGsw,false,false).Code==0&&JsonNode.Parse(File.ReadAllText(settingsFile))!["statusLine"] is null,"uninstall removes only GSW status line");
Console.WriteLine("PASS: Claude Code settings statusLine install/force/idempotent/uninstall");
{
    // Desktop token cache: Electron safeStorage layout, profile scope, unexpired, wrong key rejected.
    var desktop=Path.Combine(folder,"claude-desktop");Directory.CreateDirectory(desktop);var aesKey=System.Security.Cryptography.RandomNumberGenerator.GetBytes(32);var cacheClock=DateTimeOffset.FromUnixTimeSeconds(1800000000);
    File.WriteAllText(Path.Combine(desktop,"Local State"),new JsonObject{["os_crypt"]=new JsonObject{["encrypted_key"]=Convert.ToBase64String(System.Text.Encoding.ASCII.GetBytes("DPAPI").Concat(aesKey).ToArray())}}.ToJsonString());
    string Encrypt(string text){var nonce=System.Security.Cryptography.RandomNumberGenerator.GetBytes(12);var plain=System.Text.Encoding.UTF8.GetBytes(text);var cipher=new byte[plain.Length];var tag=new byte[16];using var gcm=new System.Security.Cryptography.AesGcm(aesKey,16);gcm.Encrypt(nonce,plain,cipher,tag);return Convert.ToBase64String(System.Text.Encoding.ASCII.GetBytes("v10").Concat(nonce).Concat(cipher).Concat(tag).ToArray());}
    long Ms(TimeSpan offset)=>cacheClock.Add(offset).ToUnixTimeMilliseconds();
    var entries=new JsonObject
    {
        ["acct:u|a:org:https://api.anthropic.com:user:inference user:profile"]=new JsonObject{["token"]="token-profile-valid-000000001",["expiresAt"]=Ms(TimeSpan.FromDays(30))},
        ["acct:u|b:org:https://api.anthropic.com:user:inference"]=new JsonObject{["token"]="token-no-profile-scope-00002",["expiresAt"]=Ms(TimeSpan.FromDays(30))},
        ["acct:u|c:org:https://api.anthropic.com:user:profile"]=new JsonObject{["token"]="token-profile-expired-0000003",["expiresAt"]=Ms(TimeSpan.FromMinutes(-5))},
        ["acct:u|d:org:https://example.com:user:profile"]=new JsonObject{["token"]="token-foreign-host-000000004",["expiresAt"]=Ms(TimeSpan.FromDays(30))},
        ["acct:u|e:org:https://api.anthropic.com:user:profile"]=new JsonObject{["token"]="token-profile-only-0000000005",["expiresAt"]=Ms(TimeSpan.FromDays(30))}
    };
    File.WriteAllText(Path.Combine(desktop,"config.json"),new JsonObject{["oauth:tokenCacheV2"]=Encrypt(entries.ToJsonString())}.ToJsonString());
    var found=ClaudeUsage.ReadDesktopTokens(desktop,b=>b,()=>cacheClock);
    Check(found.Count==2&&found[0]=="token-profile-only-0000000005"&&found[1]=="token-profile-valid-000000001","only unexpired Anthropic tokens with profile scope are used, the profile-only token first");
    bool rejected=false;try{ClaudeUsage.ReadDesktopTokens(desktop,b=>new byte[32],()=>cacheClock);}catch(System.Security.Cryptography.CryptographicException){rejected=true;}
    Check(rejected,"wrong key never yields tokens");
    Check(ClaudeUsage.ReadDesktopTokens(Path.Combine(folder,"no-desktop"),b=>b,()=>cacheClock).Count==0,"missing desktop app is quiet");

    // Local usage history: newest sample wins, v2 shape.
    var history=Path.Combine(folder,"plan-usage-history.json");
    File.WriteAllText(history,"{\"version\":2,\"samples\":[{\"t\":1800000000000,\"org\":\"o\",\"u\":{\"fh\":10,\"sd\":3}},{\"t\":1800000600000,\"org\":\"o\",\"u\":{\"fh\":25.5,\"sd\":4}},{\"t\":1799999000000,\"org\":\"o\",\"u\":{\"fh\":99,\"sd\":99}}]}");
    var latestLocal=ClaudeUsage.ReadLocalHistory(history);
    Check(latestLocal is {} l&&l.At.ToUnixTimeMilliseconds()==1800000600000&&l.FiveHour==25.5&&l.SevenDay==4,"newest local usage sample");
    Check(ClaudeUsage.ReadLocalHistory(Path.Combine(folder,"missing-history.json")) is null,"missing local history is null");

    // Local first, server on demand.
    var uClock=DateTimeOffset.FromUnixTimeSeconds(1800000000);var fiveReset=uClock.AddHours(3);var sevenReset=uClock.AddDays(4);int calls=0,status=200;(DateTimeOffset,double?,double?)? sample=null;
    string Body()=>new JsonObject{["five_hour"]=new JsonObject{["utilization"]=37.0,["resets_at"]=fiveReset.ToString("O")},["seven_day"]=new JsonObject{["utilization"]=6.0,["resets_at"]=sevenReset.ToString("O")},["extra_usage"]=new JsonObject{["is_enabled"]=false}}.ToJsonString();
    double Remaining(JsonArray windows,int i)=>windows[i]!["remaining_percent"]!["value"]!.GetValue<double>();
    string Quality(JsonObject source)=>source["quality"]!.GetValue<string>();
    using var usage=new ClaudeUsage(()=>["secret-token-never-in-output"],(token,ct)=>{calls++;return Task.FromResult((status,status==200?Body():""));},()=>uClock,()=>sample);
    Check(usage.NeedsServer()&&Quality(usage.Snapshot().Source)=="unconfigured","nothing known asks the server");
    await usage.PollOnce(CancellationToken.None);var us=usage.Snapshot();
    Check(calls==1&&Quality(us.Source)=="ok"&&us.Windows.Count==2&&Remaining(us.Windows,0)==63&&Remaining(us.Windows,1)==94&&us.Windows[0]!["resets_at"] is not null&&us.Windows[1]!["window_duration_mins"]!.GetValue<int>()==10080,"server answer maps both windows with reset times");
    Check(!us.Source.ToJsonString().Contains("secret-token")&&!us.Windows.ToJsonString().Contains("secret-token"),"token never appears in output");
    Check(!usage.NeedsServer(),"fresh data does not ask again");
    uClock=uClock.AddMinutes(4);Check(!usage.NeedsServer(),"idle: percentage under 5 minutes old stays quiet");
    sample=(uClock.AddSeconds(-30),52.0,7.0);us=usage.Snapshot();
    Check(Remaining(us.Windows,0)==48&&Remaining(us.Windows,1)==93&&us.Windows[0]!["resets_at"] is not null&&calls==1,"newer local sample replaces the percentage without a request, reset time kept");
    uClock=uClock.AddMinutes(2);Check(!usage.NeedsServer()&&calls==1,"a fresh local sample keeps the server quiet");
    sample=null;uClock=uClock.AddMinutes(11);
    Check(usage.NeedsServer(),"no fresh percentage anywhere asks the server");
    us=usage.Snapshot();Check(Quality(us.Source)=="stale"&&us.Windows[0]!["remaining_percent"]!["value"] is null&&us.Windows[0]!["resets_at"] is not null,"stale percentages hidden, reset time kept");
    await usage.PollOnce(CancellationToken.None);Check(calls==2&&Quality(usage.Snapshot().Source)=="ok","refreshed on demand");
    uClock=fiveReset.AddMinutes(1);Check(usage.NeedsServer(),"a passed reset asks the server");
    us=usage.Snapshot();Check(us.Windows[0]!["resets_at"] is null&&us.Windows[0]!["remaining_percent"]!["reason"]!.GetValue<string>()=="quota_window_reset"&&us.Windows[1]!["resets_at"] is not null,"passed reset not reused");
    status=401;await usage.PollOnce(CancellationToken.None);Check(!usage.NeedsServer(),"failed request is not retried at once");
    uClock=uClock.AddMinutes(3);Check(usage.NeedsServer(),"failed request retries after two minutes");

    // Live refresh while Claude works, one more right after it stops, 429 backs off.
    {
        var busyClock=DateTimeOffset.FromUnixTimeSeconds(1800000000);bool working=false;int code=200;int busyCalls=0;
        using var live=new ClaudeUsage(()=>["secret-token-never-in-output"],(token,ct)=>{busyCalls++;return Task.FromResult((code,code==200?Body():""));},()=>busyClock,()=>null,()=>working);
        Check(live.NeedsServer(),"first poll");await live.PollOnce(CancellationToken.None);busyClock=busyClock.AddSeconds(6);Check(!live.NeedsServer(),"idle right after a poll stays quiet");
        working=true;busyClock=busyClock.AddSeconds(10);Check(!live.NeedsServer(),"working: not before 60 s");busyClock=busyClock.AddSeconds(55);Check(live.NeedsServer(),"working: refresh every 60 s");
        await live.PollOnce(CancellationToken.None);busyClock=busyClock.AddSeconds(61);Check(live.NeedsServer(),"working: next refresh");await live.PollOnce(CancellationToken.None);
        working=false;busyClock=busyClock.AddSeconds(3);Check(!live.NeedsServer(),"work ended: waits 5 s since the last request");
        busyClock=busyClock.AddSeconds(3);Check(live.NeedsServer(),"work ended: one final refresh");await live.PollOnce(CancellationToken.None);busyClock=busyClock.AddSeconds(40);Check(!live.NeedsServer(),"idle after the final refresh stays quiet");
        working=true;busyClock=busyClock.AddSeconds(40);code=429;Check(live.NeedsServer(),"working again");await live.PollOnce(CancellationToken.None);busyClock=busyClock.AddSeconds(60);Check(!live.NeedsServer(),"HTTP 429 backs off for 5 minutes");busyClock=busyClock.AddMinutes(5);Check(live.NeedsServer(),"retries after the 429 back-off");
        // Every token limited: the back-off grows 5, 10, 20 minutes (capped at 20) instead of repeating the busy cadence.
        var growClock=DateTimeOffset.FromUnixTimeSeconds(1800100000);int growStatus=429;
        using var grow=new ClaudeUsage(()=>["limited-token-aaaaaaaaaaaa"],(token,ct)=>Task.FromResult((growStatus,growStatus==200?Body():"")),()=>growClock,()=>null,()=>true);
        foreach(var minutes in new[]{5,10,20,20,20})
        {
            Check(grow.NeedsServer(),"backoff round starts");await grow.PollOnce(CancellationToken.None);
            growClock=growClock.AddMinutes(minutes).AddSeconds(-5);Check(!grow.NeedsServer(),$"still backing off before {minutes} min");growClock=growClock.AddSeconds(10);
        }
        Check(grow.NeedsServer(),"retries once the capped back-off has passed");growStatus=200;await grow.PollOnce(CancellationToken.None);Check(Quality(grow.Snapshot().Source)=="ok","recovers after a rate limit");
        // A rate-limited token does not stop the next token from being tried.
        int attempts=0;using var failover=new ClaudeUsage(()=>["limited-token-aaaaaaaaaaaa","good-token-bbbbbbbbbbbbbb"],(token,ct)=>{attempts++;return Task.FromResult(token.StartsWith("limited")?(429,""):(200,Body()));},()=>busyClock,()=>null);
        await failover.PollOnce(CancellationToken.None);Check(attempts==2&&Quality(failover.Snapshot().Source)=="ok","a rate-limited token falls back to the next one");
    }
    status=401;calls=0;using var denied=new ClaudeUsage(()=>["secret-token-never-in-output"],(token,ct)=>{calls++;return Task.FromResult((status,""));},()=>uClock,()=>null);
    await denied.PollOnce(CancellationToken.None);var ds=denied.Snapshot();
    Check(Quality(ds.Source)=="error"&&ds.Source["last_error_code"]!.GetValue<string>()=="claude_usage_unauthorized"&&ds.Windows.Count==0,"rejected token is an explicit error");
    using var missingToken=new ClaudeUsage(()=>[],(token,ct)=>throw new Exception("must not be called"),()=>uClock,()=>null);
    await missingToken.PollOnce(CancellationToken.None);Check(missingToken.Snapshot().Source["last_error_code"]!.GetValue<string>()=="claude_usage_token_unavailable","no desktop token is reported");

    status=200;using var healthy=new ClaudeUsage(()=>["secret-token-never-in-output"],(token,ct)=>Task.FromResult((200,Body())),()=>fiveReset.AddHours(-1),()=>null);
    await healthy.PollOnce(CancellationToken.None);
    var combined=new ClaudeQuota(Path.Combine(folder,"no-bridge.json"),()=>fiveReset.AddHours(-1),healthy).Snapshot();
    Check(Quality(combined.Source)=="ok"&&combined.Windows.Count==2&&combined.Windows[0]!["bucket"]!.GetValue<string>()=="claude","desktop usage fills in when the status line bridge is absent");
    Console.WriteLine("PASS: Claude desktop token cache, local usage first, on-demand server, stale/reset/failure and bridge fallback");
}

var codexQuotaObject=new JsonObject{["source"]=new JsonObject{["quality"]="error",["observed_at"]=null,["source_updated_at"]=null,["age_ms"]=null,["last_error_code"]="quota_executable_missing"},["state"]="unknown",["quotas"]=new JsonArray(),["sessions"]=new JsonArray()};
var claudeWindows=new JsonArray(new JsonObject{["bucket"]="claude",["window_duration_mins"]=300,["remaining_percent"]=Model.Metric(50),["resets_at"]=null});
var okSource=new JsonObject{["quality"]="ok",["observed_at"]=DateTimeOffset.UtcNow.ToString("O"),["source_updated_at"]=null,["age_ms"]=0,["last_error_code"]=null};
var idleCodex=new Vibe.Observation("idle",new JsonArray(new JsonObject{["id"]="aaaa",["title"]="codex",["state"]="idle"}),[DateTime.UtcNow.AddMinutes(-1)]);
var runningClaude=new Vibe.Observation("running",new JsonArray(new JsonObject{["id"]="claude-bbbb",["title"]="claude",["state"]="running",["started_at"]=DateTimeOffset.UtcNow.ToString("O")}),[DateTime.UtcNow]);
var merged=Vibe.Merge(codexQuotaObject,idleCodex,(okSource,claudeWindows),runningClaude);
Check(merged["state"]!.GetValue<string>()=="running"&&merged["sessions"]![0]!["id"]!.GetValue<string>()=="claude-bbbb"&&merged["sessions"]!.AsArray().Count==2,"running Claude session first");
Check(merged["quotas"]!.AsArray().Count==1&&merged["source"]!["quality"]!.GetValue<string>()=="ok","Claude quota keeps source healthy when Codex RPC fails");
merged=Vibe.Merge(codexQuotaObject,idleCodex,null,new Vibe.Observation("absent",new JsonArray(),[]));Check(merged["state"]!.GetValue<string>()=="idle","absent Claude does not make VIBE unknown");
merged=Vibe.Merge(codexQuotaObject,new Vibe.Observation("unknown",new JsonArray(),[]),null,new Vibe.Observation("idle",new JsonArray(),[]));Check(merged["state"]!.GetValue<string>()=="unknown","unknown observer stays unknown");
merged=Vibe.Merge(codexQuotaObject,null,null,null);Check(merged["state"]!.GetValue<string>()=="unknown"&&merged["source"]!["quality"]!.GetValue<string>()=="error","no observers keeps legacy behaviour");
var legacyVibe=Model.LegacyCodex(Vibe.Merge(codexQuotaObject,idleCodex,(okSource,claudeWindows),runningClaude));Check(legacyVibe["sessions"]![0]!["id"]!.GetValue<string>()=="claude-bbbb"&&legacyVibe["sessions"]![0]!["started_at"] is null,"legacy GSN form keeps provider prefix");
Console.WriteLine("PASS: VIBE Codex/Claude merge, ordering, absent observer and legacy form");
Check(RtssGame.TryInterval(1000,2000,144,2100,out var fps)&&fps==144,"RTSS interval FPS");
Check(!RtssGame.TryInterval(1000,2000,144,6000,out _),"stale RTSS sample");
Check(!RtssGame.TryInterval(0,2000,144,2100,out _),"RTSS uninitialized interval");
Check(!RtssGame.TryInterval(2000,2000,144,2100,out _),"RTSS empty interval");
Check(RtssGame.TryInterval(uint.MaxValue-500,499,120,600,out fps)&&fps==120,"RTSS timer wrap");
Console.WriteLine("PASS: RTSS interval, stale input, invalid period and timer wrap");

// Synthetic SDK producer verifies selection when the rendering process is not foreground.
var mapName="GSO.RTSS.Test."+Guid.NewGuid();
using(var map=System.IO.MemoryMappedFiles.MemoryMappedFile.CreateNew(mapName,2396256+12416*256))
using(var view=map.CreateViewAccessor())
{
    using var process=System.Diagnostics.Process.GetCurrentProcess();var processName=process.ProcessName+".exe";
    uint tick=unchecked((uint)Environment.TickCount);
    view.Write(0,0x52545353u);view.Write(4,0x00020000u);view.Write(8,12416u);view.Write(12,2396256u);view.Write(16,256u);
    const long entry=2396256;view.Write(entry,(uint)process.Id);var nameBytes=System.Text.Encoding.ASCII.GetBytes(processName);view.WriteArray(entry+4,nameBytes,0,nameBytes.Length);
    view.Write(entry+268,tick-1000);view.Write(entry+272,tick);view.Write(entry+276,144u);
    using var windowed=new RtssGame([],false,true,[],false,mapName,_=>false);Check(!windowed.Sample()["rendering"]!.GetValue<bool>(),"windowed RTSS renderer cannot trigger game mode");
    using var game=new RtssGame([],false,true,[],false,mapName,_=>true);var sample=game.Sample();
    Check(sample["rendering"]!.GetValue<bool>()&&sample["fps"]!["value"]!.GetValue<double>()==144,"background RTSS renderer selected");
    Check(sample["process_name"]!.GetValue<string>()==processName,"SDK process identity verified");
    using var excludedGame=new RtssGame([],false,true,[processName],false,mapName);
    Check(!excludedGame.Sample()["rendering"]!.GetValue<bool>(),"excluded renderer ignored");
    view.Write(entry+272,tick-10000);
    using var staleGame=new RtssGame([],false,true,[],false,mapName);
    Check(!staleGame.Sample()["rendering"]!.GetValue<bool>(),"stale SDK entries cannot trigger game mode");
}
Console.WriteLine("PASS: isolated RTSS shared memory, background renderer, exclusions and stale entries");
Check(RtssGame.IsDesktopApp("Typora.exe"),"Typora excluded even in full screen");
Check(RtssGame.FullscreenBounds(0,0,1920,1080,0,0,1920,1080,0x80000000),"borderless full monitor accepted");
Check(!RtssGame.FullscreenBounds(0,0,1920,1040,0,0,1920,1080,0x80000000),"work area is not full monitor");
Check(!RtssGame.FullscreenBounds(0,0,1920,1080,0,0,1920,1080,0x00cf0000),"maximized decorated window rejected");
Check(RtssGame.FullscreenBounds(-1920,0,1920,1080,-1920,0,0,1080,0x80000000),"negative-coordinate second monitor accepted");

var cpuFrame=CpuSensorClient.Validate(2,2,2000,1000,3,67.5,85.2);
Check(cpuFrame.Temperature==67.5&&cpuFrame.Power==85.2&&cpuFrame.Error is null,"CPU sensor frame values");
Check(CpuSensorClient.Validate(2,4,2000,1000,3,67.5,85.2).Temperature is null,"torn CPU frame rejected");
Check(CpuSensorClient.Validate(3,3,2000,1000,3,67.5,85.2).Temperature is null,"in-progress CPU frame rejected");
Check(CpuSensorClient.Validate(2,2,11001,1000,3,67.5,85.2).Error=="cpu_sensor_stale","stale CPU frame rejected");
Check(CpuSensorClient.Validate(2,2,2000,1000,3,double.NaN,-1).Temperature is null&&CpuSensorClient.Validate(2,2,2000,1000,3,67,-1).Power is null,"invalid CPU readings rejected");
Console.WriteLine("PASS: CPU shared frame consistency, freshness and numeric validation");

var namedRoot=Path.Combine(folder,"named");var namedSessions=Path.Combine(namedRoot,"sessions");var namedDay=Path.Combine(namedSessions,day.ToString("yyyy"),day.ToString("MM"),day.ToString("dd"));Directory.CreateDirectory(namedDay);
const string namedId="12345678-1234-1234-1234-123456789012";var namedFile=Path.Combine(namedDay,"rollout-"+namedId+".jsonl");var indexFile=Path.Combine(namedRoot,"session_index.jsonl");
var clock=DateTimeOffset.UtcNow;long taskTick=0;
File.WriteAllText(indexFile,new JsonObject{["id"]=namedId,["thread_name"]="真实工作会话"}.ToJsonString()+"\n");
void NamedEvent(string type,string id,DateTimeOffset at){File.AppendAllText(namedFile,new JsonObject{["type"]="event_msg",["timestamp"]=at.ToString("O"),["payload"]=new JsonObject{["type"]=type,["turn_id"]=id}}.ToJsonString()+"\n");}
NamedEvent("task_started","named",clock.AddSeconds(-30));
await using var namedTasks=new CodexTasks(namedSessions,false,()=>clock,()=>taskTick);namedTasks.Scan();
Check(namedTasks.Snapshot().Sessions[0]?["title"]?.GetValue<string>()=="真实工作会话","actual session-index title");
Check(namedTasks.Snapshot().Sessions[0]?["running_duration_ms"]?.GetValue<long>()==30000,"duration from actual start");
taskTick=7000;clock=clock.AddMinutes(-2);namedTasks.Scan();Check(namedTasks.Snapshot().Sessions[0]?["running_duration_ms"]?.GetValue<long>()==37000,"clock rollback cannot decrease duration");
File.WriteAllText(indexFile,new JsonObject{["id"]=namedId,["thread_name"]="已重命名"}.ToJsonString()+"\n");taskTick=14000;namedTasks.Scan();Check(namedTasks.Snapshot().Sessions[0]?["title"]?.GetValue<string>()=="已重命名","rename refresh");
clock=clock.AddMinutes(3);NamedEvent("task_started","named",clock);namedTasks.Scan();Check(namedTasks.Snapshot().Sessions[0]?["running_duration_ms"]?.GetValue<long>()==44000,"duplicate start retains duration");
await using(var restarted=new CodexTasks(namedSessions,false,()=>clock,()=>taskTick)){restarted.Scan();Check(restarted.Snapshot().Sessions[0]?["running_duration_ms"]?.GetValue<long>()==90000,"restart reconstructs real start");}
File.AppendAllText(namedFile,new JsonObject{["type"]="response_item",["payload"]=new string('x',100000)}.ToJsonString()+"\n");namedTasks.Scan();Check(namedTasks.Snapshot().State=="running","large unrelated response does not destroy lifecycle");
NamedEvent("task_complete","named",clock);namedTasks.Scan();Check(namedTasks.Snapshot().Sessions[0]?["running_duration_ms"] is null,"completion clears duration");
File.Delete(indexFile);taskTick=21000;namedTasks.Scan();Check(namedTasks.Snapshot().Sessions[0]?["title"] is null,"missing title is null, never a synthetic name");
Console.WriteLine("PASS: session title, rename, start duration, clock rollback, duplicate, restart and completion");

var legacySource=new JsonObject{["sessions"]=new JsonArray(new JsonObject{["id"]="a",["title"]=null,["state"]="running",["started_at"]="2026-10-16T00:00:00Z",["running_duration_ms"]=10})};
var legacy=Model.LegacyCodex(legacySource);Check(legacy["sessions"]?[0]?["started_at"] is null&&legacy["sessions"]?[0]?["title"]?.GetValue<string>()=="会话名称不可用","old NAS compatible sessions");Check(legacySource["sessions"]?[0]?["running_duration_ms"] is not null,"legacy conversion does not mutate observation");
Console.WriteLine("PASS: legacy NAS compatibility without dropping Windows telemetry");

var eventQuota=JsonNode.Parse("""{"limit_id":"codex","primary":{"used_percent":12,"window_minutes":300,"resets_at":1800000000},"secondary":{"used_percent":null,"window_minutes":10080,"resets_at":null}}""")!.AsObject();
var mappedQuota=CodexQuota.ParseEventWindows(eventQuota);Check(mappedQuota[0]?["remaining_percent"]?["value"]?.GetValue<double>()==88,"local quota used-to-remaining and bucket mapping");Check(mappedQuota[1]?["remaining_percent"]?["value"] is null,"local missing quota stays missing");
eventQuota.Remove("limit_id");Check(CodexQuota.ParseEventWindows(eventQuota).Count==0,"unidentified quota cannot become Codex quota");
await using(var eventAdapter=new CodexQuota("missing-not-an-executable",()=>new LocalQuotaSnapshot(DateTimeOffset.UtcNow.AddSeconds(-5),mappedQuota))){
 for(int n=0;n<50&&eventAdapter.Snapshot()["source"]?["quality"]?.GetValue<string>()!="ok";n++)await Task.Delay(10);
 var eventSnapshot=eventAdapter.Snapshot();Check(eventSnapshot["source"]?["quality"]?.GetValue<string>()=="ok","fresh local quota avoids unavailable RPC executable");Check(eventSnapshot["source"]?["age_ms"]?.GetValue<long>()>=4900,"local quota observation age preserved");
}
Console.WriteLine("PASS: event quota mapping, missing bucket/value, source age and RPC avoidance");

NamedEvent("task_started","long-live",clock.AddHours(-1));
File.AppendAllText(namedFile,new JsonObject{["type"]="response_item",["timestamp"]=clock.ToString("O"),["payload"]=new JsonObject{["type"]="message"}}.ToJsonString()+"\n");
for(int n=0;n<5;n++)File.WriteAllText(Path.Combine(namedDay,"stale-"+n+".jsonl"),new JsonObject{["type"]="event_msg",["timestamp"]=clock.AddMinutes(-20).ToString("O"),["payload"]=new JsonObject{["type"]="task_started",["turn_id"]="stale"}}.ToJsonString()+"\n");
namedTasks.Scan();Check(namedTasks.Snapshot().State=="running"&&namedTasks.Snapshot().Sessions[0]?["state"]?.GetValue<string>()=="running","stale active sessions cannot evict real current session");
var quotaAt=clock.AddSeconds(-2);File.AppendAllText(namedFile,new JsonObject{["type"]="event_msg",["timestamp"]=quotaAt.ToString("O"),["payload"]=new JsonObject{["type"]="token_count",["rate_limits"]=JsonNode.Parse("""{"limit_id":"codex","primary":{"used_percent":12,"window_minutes":300,"resets_at":1800000000}}""")}}.ToJsonString()+"\n");namedTasks.Scan();
Check(namedTasks.QuotaSnapshot()?.Windows[0]?["remaining_percent"]?["value"]?.GetValue<double>()==88,"quota extracted while observing lifecycle");Check(namedTasks.QuotaSnapshot()?.ObservedAt==quotaAt,"quota event timestamp preserved");
Console.WriteLine("PASS: active session priority and bounded event quota extraction");

File.AppendAllText(namedFile,new JsonObject{["type"]="event_msg",["timestamp"]="invalid-quota-time",["payload"]=new JsonObject{["type"]="token_count",["rate_limits"]=new JsonArray()}}.ToJsonString()+"\n");namedTasks.Scan();Check(namedTasks.Snapshot().State=="running","malformed quota cannot break task lifecycle");Check(namedTasks.QuotaSnapshot()?.ObservedAt==quotaAt,"malformed quota cannot replace last valid quota");
Console.WriteLine("PASS: quota and task failure isolation");

var rotationRoot=Path.Combine(folder,"rotation");
var rotationDay=Path.Combine(rotationRoot,day.ToString("yyyy"),day.ToString("MM"),day.ToString("dd"));
Directory.CreateDirectory(rotationDay);
var rotationPath=Path.Combine(rotationDay,"lifecycle.jsonl");
string RotationEvent(string kind,string turn)=>new JsonObject{["type"]="event_msg",["timestamp"]=DateTimeOffset.UtcNow.ToString("O"),["payload"]=new JsonObject{["type"]=kind,["turn_id"]=turn}}.ToJsonString()+"\n";
File.WriteAllText(rotationPath,RotationEvent("task_started","old"));
await using(var rotated=new CodexTasks(rotationRoot,false)){
 rotated.Scan();Check(rotated.Snapshot().State=="running","rotation initial active turn");
 var replacement=Path.Combine(rotationDay,"replacement.tmp");
 File.WriteAllText(replacement,RotationEvent("task_complete","old")+new string(' ',1024)+"\n");
 Check(new FileInfo(replacement).Length>new FileInfo(rotationPath).Length,"replacement is larger than previous offset");
 File.Move(replacement,rotationPath,true);rotated.Scan();
 Check(rotated.Snapshot().State=="idle","atomic file replacement cannot retain old active state");
 File.AppendAllText(rotationPath,"{\"type\":\"event_msg\",\"payload\":{");rotated.Scan();
 File.WriteAllText(replacement,RotationEvent("task_started","new")+new string(' ',2048)+"\n");
 File.Move(replacement,rotationPath,true);rotated.Scan();
 Check(rotated.Snapshot().State=="running","replacement clears partial record before parsing new start");
 Check(rotated.Snapshot().Sessions.Count==1&&rotated.Snapshot().Sessions[0]?["running_duration_ms"] is not null,"replacement keeps one correlated duration");
 File.AppendAllText(rotationPath,RotationEvent("task_complete","new"));rotated.Scan();
 Check(rotated.Snapshot().State=="idle"&&rotated.Snapshot().Sessions[0]?["running_duration_ms"] is null,"new file completion clears new duration");
}
Console.WriteLine("PASS: 128-bit file identity, larger atomic replacement and partial-line rotation");

foreach(var invalidTurn in new[]{"",new string('x',161)}){
 File.WriteAllText(rotationPath,RotationEvent("task_started",invalidTurn));
 await using var invalidObserver=new CodexTasks(rotationRoot,false);invalidObserver.Scan();
 Check(invalidObserver.Snapshot().State=="unknown","invalid turn identity cannot report ready");
}
File.WriteAllText(rotationPath,new JsonObject{["type"]="event_msg",["timestamp"]=DateTimeOffset.UtcNow.ToString("O"),["payload"]=new JsonObject{["type"]=null}}.ToJsonString()+"\n");
await using(var invalidObserver=new CodexTasks(rotationRoot,false)){
 invalidObserver.Scan();Check(invalidObserver.Snapshot().State=="unknown","null event type cannot report ready");
 File.AppendAllText(rotationPath,RotationEvent("task_started","recovered"));invalidObserver.Scan();
 Check(invalidObserver.Snapshot().State=="running","valid lifecycle recovers invalid identity");
 File.AppendAllText(rotationPath,RotationEvent("task_complete","recovered"));invalidObserver.Scan();
 Check(invalidObserver.Snapshot().State=="idle","valid completion restores ready");
}
Console.WriteLine("PASS: empty/oversized turn identity and null event type remain unknown until valid lifecycle");

var horizonRoot=Path.Combine(folder,"horizon");
var horizonDay=Path.Combine(horizonRoot,day.ToString("yyyy"),day.ToString("MM"),day.ToString("dd"));Directory.CreateDirectory(horizonDay);
var horizonPath=Path.Combine(horizonDay,"long.jsonl");
var horizonNow=DateTimeOffset.UtcNow;long horizonTick=0;
string HorizonEvent(string kind)=>new JsonObject{["type"]="event_msg",["timestamp"]=horizonNow.ToString("O"),["payload"]=new JsonObject{["type"]=kind,["turn_id"]="long"}}.ToJsonString()+"\n";
File.WriteAllText(horizonPath,HorizonEvent("task_started"));
await using(var longObserver=new CodexTasks(horizonRoot,false,()=>horizonNow,()=>horizonTick)){
 longObserver.Scan();Check(longObserver.Snapshot().State=="running","long horizon initial start");
 horizonNow=horizonNow.AddDays(4);horizonTick=4L*24*3600*1000;
 File.AppendAllText(horizonPath,new JsonObject{["type"]="response_item",["timestamp"]=horizonNow.ToString("O"),["payload"]=new JsonObject{["type"]="message"}}.ToJsonString()+"\n");
 longObserver.Scan();Check(longObserver.Snapshot().State=="running","tracked long task survives three-date discovery window");
 Check(longObserver.Snapshot().Sessions[0]?["running_duration_ms"]?.GetValue<long>()>=horizonTick,"four-day duration remains correlated");
 File.Move(horizonPath,horizonPath+".old");longObserver.Scan();
 Check(longObserver.Snapshot().State=="unknown","missing active log cannot report ready");
 File.WriteAllText(horizonPath,"");longObserver.Scan();
 Check(longObserver.Snapshot().State=="unknown","empty replacement cannot assert old task finished");
 File.AppendAllText(horizonPath,HorizonEvent("task_complete"));longObserver.Scan();
 Check(longObserver.Snapshot().State=="idle","confirmed completion in restored old-date path recovers ready");
}
var overflowRoot=Path.Combine(folder,"overflow");var overflowDay=Path.Combine(overflowRoot,day.ToString("yyyy"),day.ToString("MM"),day.ToString("dd"));Directory.CreateDirectory(overflowDay);
for(int i=0;i<33;i++)File.WriteAllText(Path.Combine(overflowDay,i+".jsonl"),"");
await using(var overflowObserver=new CodexTasks(overflowRoot,false)){
 overflowObserver.Scan();Check(overflowObserver.Snapshot().State=="unknown","unobserved files beyond budget cannot report all idle");
 Check(overflowObserver.Snapshot().Sessions.Count<=4,"overflow summary remains bounded");
}
Console.WriteLine("PASS: four-day active task, lost/empty/restored log and observation-budget uncertainty");

File.WriteAllText(rotationPath,RotationEvent("task_started","truncated"));
await using(var truncated=new CodexTasks(rotationRoot,false)){
 truncated.Scan();File.WriteAllText(rotationPath,"");truncated.Scan();
 Check(truncated.Snapshot().State=="unknown","truncating active log cannot invent completion");
 File.AppendAllText(rotationPath,RotationEvent("task_complete","truncated"));truncated.Scan();
 Check(truncated.Snapshot().State=="idle","lifecycle completion recovers active truncation");
}
Console.WriteLine("PASS: active truncation remains unknown until lifecycle recovery");

string TimedRotationEvent(string kind,string turn,DateTimeOffset at)=>new JsonObject{["type"]="event_msg",["timestamp"]=at.ToString("O"),["payload"]=new JsonObject{["type"]=kind,["turn_id"]=turn}}.ToJsonString()+"\n";
var replayNow=DateTimeOffset.UtcNow;
File.WriteAllText(rotationPath,TimedRotationEvent("task_started","current",replayNow.AddSeconds(-2)));
await using(var reordered=new CodexTasks(rotationRoot,false)){
 reordered.Scan();File.AppendAllText(rotationPath,TimedRotationEvent("task_started","current",replayNow.AddMinutes(-20)));reordered.Scan();
 Check(reordered.Snapshot().State=="running","old duplicate cannot regress lifecycle heartbeat");
}
File.WriteAllText(rotationPath,TimedRotationEvent("task_complete","evicted",replayNow.AddSeconds(-100)));
for(int i=0;i<65;i++)File.AppendAllText(rotationPath,TimedRotationEvent("task_complete","finished-"+i,replayNow.AddSeconds(-90+i)));
await using(var boundedReplay=new CodexTasks(rotationRoot,false)){
 boundedReplay.Scan();Check(boundedReplay.Snapshot().State=="idle","bounded terminal history starts ready");
 File.AppendAllText(rotationPath,TimedRotationEvent("task_started","evicted",replayNow.AddSeconds(-101)));boundedReplay.Scan();
 Check(boundedReplay.Snapshot().State=="unknown","replay outside tombstone history cannot resurrect completed task");
 Check(boundedReplay.Snapshot().Sessions[0]?["running_duration_ms"] is null,"unproven historical task has no live duration");
}
Console.WriteLine("PASS: reordered duplicate heartbeat and bounded terminal-history replay guard");

File.WriteAllText(rotationPath,RotationEvent("task_started","copy-truncate"));
await using(var rewritten=new CodexTasks(rotationRoot,false)){
 rewritten.Scan();var previousSize=new FileInfo(rotationPath).Length;
 File.WriteAllText(rotationPath,RotationEvent("task_complete","copy-truncate")+new string(' ',4096)+"\n");
 Check(new FileInfo(rotationPath).Length>previousSize,"copy-truncate regrows before next scan");
 rewritten.Scan();Check(rewritten.Snapshot().State=="idle","checkpoint catches same-identity regrown lifecycle file");
 File.AppendAllText(rotationPath,RotationEvent("task_started","after-rewrite"));rewritten.Scan();
 Check(rewritten.Snapshot().State=="running","normal append after rewrite remains healthy");
}
Console.WriteLine("PASS: bounded checkpoint detects copy-truncate regrowth between scans");

var timeoutRoot=Path.Combine(folder,"silence");var timeoutNow=DateTimeOffset.UtcNow;
var timeoutDay=Path.Combine(timeoutRoot,timeoutNow.ToString("yyyy"),timeoutNow.ToString("MM"),timeoutNow.ToString("dd"));Directory.CreateDirectory(timeoutDay);
string TimeoutEvent(string kind,string id)=>new JsonObject{["type"]="event_msg",["timestamp"]=timeoutNow.ToString("O"),["payload"]=new JsonObject{["type"]=kind,["turn_id"]=id}}.ToJsonString()+"\n";
var timeoutFile=Path.Combine(timeoutDay,"active.jsonl");File.WriteAllText(timeoutFile,TimeoutEvent("task_started","silent"));
await using(var timeoutObserver=new CodexTasks(timeoutRoot,false,()=>timeoutNow)){
 timeoutObserver.Scan();timeoutNow=timeoutNow.AddSeconds(61);
 for(int n=0;n<5;n++)File.WriteAllText(Path.Combine(timeoutDay,$"idle{n}.jsonl"),TimeoutEvent("task_complete","idle"));
 timeoutObserver.Scan();var timed=timeoutObserver.Snapshot();
 Check(timed.State=="idle","no new events restores ready under user policy");
 File.AppendAllText(timeoutFile,TimeoutEvent("agent_reasoning","silent"));timeoutObserver.Scan();Check(timeoutObserver.Snapshot().State=="running","new event resumes an unfinished task");
 File.AppendAllText(timeoutFile,TimeoutEvent("task_complete","silent"));timeoutObserver.Scan();Check(timeoutObserver.Snapshot().State=="idle","confirmed completion restores ready");
}
Console.WriteLine("PASS: 60-second quiet policy, fresh event resume and completion recovery");

Check(RtssGame.IsDesktopApp("WeChat.exe")&&RtssGame.IsDesktopApp("weixin.EXE")&&RtssGame.IsDesktopApp("WeChatAppEx.exe")&&!RtssGame.IsDesktopApp("game.exe"),"WeChat desktop rendering excluded from auto game detection");

var logicalTitleId=Guid.NewGuid().ToString();
using(var metadataStream=new MemoryStream(System.Text.Encoding.UTF8.GetBytes(new JsonObject{["type"]="session_meta",["payload"]=new JsonObject{["id"]=Guid.NewGuid().ToString(),["session_id"]=logicalTitleId}}.ToJsonString()+"\n"))){metadataStream.Position=2;Check(CodexTasks.ReadTitleIdentity(metadataStream,"fallback")==logicalTitleId,"logical session identity used for title lookup");Check(metadataStream.Position==2,"metadata read preserves event cursor");}
using(var oldMetadata=new MemoryStream(System.Text.Encoding.UTF8.GetBytes("{\"type\":\"session_meta\",\"payload\":{}}\n"))){Check(CodexTasks.ReadTitleIdentity(oldMetadata,"legacy-id")=="legacy-id","older logs retain filename identity");}
var transientCpu=new CpuSensorFilter();
var thermal=transientCpu.Apply(new CpuSensorReading(60,80,null,SampledTick:1000),1000);Check(thermal.Temperature==60&&thermal.Power==80,"valid CPU frame accepted");
thermal=transientCpu.Apply(new CpuSensorReading(null,null,"cpu_sensor_update_in_progress"),2000);Check(thermal.Temperature==60&&thermal.Power==80,"transient CPU null frame filtered within freshness window");
thermal=transientCpu.Apply(new CpuSensorReading(null,90,null,SampledTick:3000),3000);Check(thermal.Temperature==60&&thermal.Power==90,"CPU metrics retain independent source timestamps");
thermal=transientCpu.Apply(new CpuSensorReading(double.NaN,-1,null,SampledTick:4500),4500);Check(thermal.Temperature==60&&thermal.Power==90,"slow hardware update retains last actual CPU readings");
thermal=transientCpu.Apply(new CpuSensorReading(null,null,"cpu_sensor_stale"),11001);Check(thermal.Temperature is null&&thermal.Power==90,"invalid samples cannot renew independent CPU sample timestamps");
thermal=transientCpu.Apply(new CpuSensorReading(null,null,"cpu_sensor_stale"),13001);Check(thermal.Temperature is null&&thermal.Power is null,"missing CPU source expires without indefinite frozen values");
Check(CpuSensorClient.Validate(2,2,5500,1000,3,60,80).Temperature==60,"valid hardware frame survives a 4.5 second update interval");
Check(CpuSensorClient.Validate(2,2,11001,1000,3,60,80).Error=="cpu_sensor_stale","hardware frame expires beyond ten seconds");
Console.WriteLine("PASS: transient CPU sample filtering, independent ages, NaN rejection and bounded expiry");

{
    // Client activity: online detection and today's tokens, counted from local logs only.
    var activityFolder=Path.Combine(folder,"activity");var activityClaude=Path.Combine(activityFolder,"claude","projects","proj");var activityCodex=Path.Combine(activityFolder,"codex","sessions","2026","10","17");
    Directory.CreateDirectory(activityClaude);Directory.CreateDirectory(activityCodex);
    var now=DateTime.Now;var today=now.Date.AddHours(12);var yesterday=today.AddDays(-1);
    string Stamp(DateTime t)=>new DateTimeOffset(t).UtcDateTime.ToString("yyyy-MM-ddTHH:mm:ss.fffZ");
    string ClaudeLine(DateTime t,string id,int input,int write,int read,int output)=>$"{{\"type\":\"assistant\",\"timestamp\":\"{Stamp(t)}\",\"requestId\":\"req-{id}\",\"message\":{{\"id\":\"{id}\",\"usage\":{{\"input_tokens\":{input},\"cache_creation_input_tokens\":{write},\"cache_read_input_tokens\":{read},\"output_tokens\":{output}}}}}}}\n";
    string CodexLine(DateTime t,long total,long cached)=>$"{{\"timestamp\":\"{Stamp(t)}\",\"type\":\"event_msg\",\"payload\":{{\"type\":\"token_count\",\"info\":{{\"total_token_usage\":{{\"input_tokens\":{total},\"cached_input_tokens\":{cached},\"output_tokens\":0,\"total_tokens\":{total}}}}}}}}}\n";
    var activityTranscript=Path.Combine(activityClaude,"a.jsonl");
    File.WriteAllText(activityTranscript,ClaudeLine(today,"m1",2,1000,50000,300)+ClaudeLine(today,"m1",2,1000,50000,300)+ClaudeLine(today,"m2",1,0,90000,99)+ClaudeLine(yesterday,"old",5,5,5,5)+"{\"type\":\"user\",\"usage\":1}\n{broken usage\n");
    var activityResumed=Path.Combine(activityClaude,"b.jsonl");File.WriteAllText(activityResumed,ClaudeLine(today,"m1",2,1000,50000,300));
    var activitySession=Path.Combine(activityCodex,"rollout-test.jsonl");
    File.WriteAllText(activitySession,CodexLine(yesterday,10000,2000)+CodexLine(today,15000,4000)+CodexLine(today,15000,4000)+CodexLine(today,18000,4000));
    var activityClock=today;
    using var activity=new ClientActivity([Path.Combine(activityFolder,"claude","projects")],Path.Combine(activityFolder,"codex","sessions"),false,()=>activityClock,()=>[new ClientActivity.ProcessInfo("claude",""),new ClientActivity.ProcessInfo("ChatGPT",@"C:\Program Files\WindowsApps\OpenAI.Codex_1\app\ChatGPT.exe")]);
    Check(activity.Snapshot()[1]!["tokens_today"] is null,"tokens stay unknown until the first scan has finished");
    activity.RefreshProcesses();activity.Scan();var clients=activity.Snapshot();
    Check(clients[1]!["id"]!.GetValue<string>()=="claude"&&clients[1]!["tokens_today"]!.GetValue<long>()==2+1000+300+1+99,"Claude counts input, cache write and output once per message; cache reads, old days and activityResumed copies excluded");
    // Codex: (15000-4000)-(10000-2000) is not today's use alone; the running value grows 8000 -> 11000 -> 14000, and only today's growth counts.
    Check(clients[0]!["id"]!.GetValue<string>()=="codex"&&clients[0]!["tokens_today"]!.GetValue<long>()==(11000-8000)+(14000-11000),"Codex counts today's growth of the running total without cached reads or duplicates");
    Check(clients[0]!["online"]!.GetValue<bool>()&&clients[1]!["online"]!.GetValue<bool>(),"both clients detected from their processes");
    File.AppendAllText(activityTranscript,ClaudeLine(today,"m3",10,0,0,20));File.AppendAllText(activitySession,CodexLine(today,18500,4000));activity.Scan();
    clients=activity.Snapshot();Check(clients[1]!["tokens_today"]!.GetValue<long>()==1402+30&&clients[0]!["tokens_today"]!.GetValue<long>()==6500,"appended records are counted incrementally");
    // Lines larger than one read chunk and a half-written last line.
    var bigLine=ClaudeLine(today,"big",7,0,0,0).TrimEnd('\n');bigLine=bigLine.Insert(bigLine.Length-1,",\"pad\":\""+new string('x',700000)+"\"");
    var partial=ClaudeLine(today,"half",100,0,0,0);
    File.AppendAllText(activityTranscript,bigLine+"\n"+partial[..40]);activity.Scan();
    Check(activity.Snapshot()[1]!["tokens_today"]!.GetValue<long>()==1402+30+7,"a line larger than the read chunk is counted and a half-written line waits");
    File.AppendAllText(activityTranscript,partial[40..]);activity.Scan();
    Check(activity.Snapshot()[1]!["tokens_today"]!.GetValue<long>()==1402+30+7+100,"the completed line is counted exactly once");
    activity.Scan();Check(activity.Snapshot()[1]!["tokens_today"]!.GetValue<long>()==1402+30+7+100,"an unchanged file is not counted again");
    activityClock=activityClock.AddDays(1);activity.Scan();
    Check(activity.Snapshot()[1]!["tokens_today"]!.GetValue<long>()==0||File.GetLastWriteTime(activityTranscript)>=activityClock.Date,"the day rollover restarts the counters");
    var kinds=ClientActivity.Classify([new ClientActivity.ProcessInfo("ChatGPT",@"C:\x\ChatGPT.exe"),new ClientActivity.ProcessInfo("codex-windows-sandbox-service","")]);
    Check(!kinds.Claude&&!kinds.Codex,"an unrelated ChatGPT window or the sandbox service does not count as online");
    Check(ClientActivity.Classify([new ClientActivity.ProcessInfo("codex","")]).Codex,"Codex CLI counts as online");
    {   // the toolhelp listing agrees with the Process API about which clients are running right now
        var listed=ClientActivity.ListProcesses();var found=ClientActivity.Classify(listed);
        bool claudeRunning=System.Diagnostics.Process.GetProcessesByName("claude").Length>0,codexCli=System.Diagnostics.Process.GetProcessesByName("codex").Length>0;
        Check(found.Claude==claudeRunning,"the process snapshot sees claude.exe exactly when the Process API does");
        Check(!codexCli||found.Codex,"the process snapshot sees a running codex.exe");
        Check(listed.All(p=>p.Name.Length>0&&!p.Name.EndsWith(".exe",StringComparison.OrdinalIgnoreCase)),"listed names carry no extension");
    }
    Console.WriteLine("PASS: client online detection and today's token counting");
}
