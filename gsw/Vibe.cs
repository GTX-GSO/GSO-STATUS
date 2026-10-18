using System.Text.Json.Nodes;

namespace GsoStatus;

// The v1 contract keeps a single "codex" VIBE object. Claude sessions share it with "claude-" ids and the
// "claude" quota bucket, so the deployed GSN and older screens keep validating the same schema.
static class Vibe
{
    internal sealed record Observation(string State,JsonArray Sessions,DateTime[] Recency);
    static int Rank(string? state)=>state=="running"?2:state=="unknown"?1:0;
    static DateTimeOffset Started(JsonNode? session)=>session?["started_at"] is JsonValue v&&v.TryGetValue<string>(out var text)&&DateTimeOffset.TryParse(text,out var at)?at:DateTimeOffset.MinValue;
    internal static JsonObject Merge(JsonObject codexQuota,Observation? codex,(JsonObject Source,JsonArray Windows)? claudeQuota,Observation? claude)
    {
        var result=(JsonObject)codexQuota.DeepClone();
        // Lifecycle: observers that are absent (tool not installed) do not make the aggregate unknown.
        var present=new[]{codex,claude}.Where(o=>o is not null&&o.State!="absent").Select(o=>o!).ToArray();
        if(codex is not null||claude is not null)
        {
            result["state"]=present.Length==0?"unknown":present.Any(o=>o.State=="running")?"running":present.Any(o=>o.State=="unknown")?"unknown":"idle";
            var sessions=new List<(JsonNode Node,int Rank,DateTimeOffset Started,DateTime Recent,int Order)>();int order=0;
            foreach(var observation in present)for(int i=0;i<observation.Sessions.Count;i++){var node=observation.Sessions[i];if(node is null)continue;sessions.Add((node.DeepClone(),Rank(node["state"]?.GetValue<string>()),Started(node),i<observation.Recency.Length?observation.Recency[i]:DateTime.MinValue,order++));}
            result["sessions"]=new JsonArray(sessions.OrderByDescending(s=>s.Rank).ThenByDescending(s=>s.Rank==2?s.Started.UtcDateTime:s.Recent).ThenBy(s=>s.Order).Take(4).Select(s=>s.Node).ToArray());
        }
        if(claudeQuota is {} claudeData)
        {
            var quotas=new JsonArray();
            foreach(var window in claudeData.Windows)if(window is not null)quotas.Add(window.DeepClone());
            // Keep the Codex bucket that screens display before any other Codex limit buckets.
            if(result["quotas"] is JsonArray existing)foreach(var window in existing.Where(w=>w is not null).OrderBy(w=>w!["bucket"]?.GetValue<string>()=="codex"?0:1)){if(quotas.Count>=8)break;quotas.Add(window!.DeepClone());}
            result["quotas"]=quotas;
            result["source"]=MergeSource(result["source"] as JsonObject,claudeData.Source);
        }
        return result;
    }
    internal static int Quality(JsonObject? source)=>(source?["quality"]?.GetValue<string>()) switch{"ok"=>5,"stale"=>4,"offline"=>3,"error"=>2,"unsupported"=>1,_=>0};
    internal static JsonObject MergeSource(JsonObject? codex,JsonObject claude)
    {
        // The quota source is healthy when either provider currently delivers quota data.
        if(codex is null)return (JsonObject)claude.DeepClone();
        int a=Quality(codex),b=Quality(claude);
        if(a==0&&b==0)return (JsonObject)codex.DeepClone();
        return (JsonObject)(b>a?claude:codex).DeepClone();
    }
}
