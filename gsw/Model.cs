using System.Text.Json.Nodes;

namespace GsoStatus;

static class Model
{
    public static JsonObject LegacyCodex(JsonObject codex)
    {
        var copy=(JsonObject)codex.DeepClone();var sessions=new JsonArray();
        if(copy["sessions"] is JsonArray items)foreach(var item in items){if(item is JsonObject session)sessions.Add(new JsonObject{["id"]=session["id"]?.DeepClone(),["title"]=session["title"]?.DeepClone()??JsonValue.Create("会话名称不可用"),["state"]=session["state"]?.DeepClone()});}
        copy["sessions"]=sessions;return copy;
    }
    public static JsonObject Missing(string reason) => new() { ["value"] = null, ["reason"] = reason };
    public static JsonObject Metric(double? value, string reason = "sensor_unsupported") =>
        value is { } n && double.IsFinite(n) ? new() { ["value"] = n, ["reason"] = null } : Missing(reason);
    public static JsonObject Bytes(ulong? value, string reason = "sensor_unavailable") =>
        value is { } n ? new() { ["value"] = n, ["reason"] = null } : Missing(reason);
    public static JsonObject Source(string quality, string? error = null) => new()
    {
        ["quality"] = quality, ["observed_at"] = quality == "ok" ? DateTimeOffset.UtcNow.ToString("O") : null,
        ["source_updated_at"] = null, ["age_ms"] = quality == "ok" ? JsonValue.Create(0) : null, ["last_error_code"] = error
    };
    public static JsonObject Hardware(string? name = null) => new()
    {
        ["model"] = name, ["load_percent"] = Missing("sensor_unavailable"),
        ["temperature_c"] = Missing("sensor_unsupported"), ["power_w"] = Missing("sensor_unsupported"),
        ["memory_used_bytes"] = Missing("sensor_unavailable"), ["memory_total_bytes"] = Missing("sensor_unavailable")
    };
    public static JsonObject Telemetry(string boot, long seq, JsonObject windows) => new()
    {
        ["schema_version"] = "1.0", ["boot_id"] = boot, ["seq"] = seq,
        ["sampled_at"] = DateTimeOffset.UtcNow.ToString("O"), ["windows"] = windows,
        ["game"] = new JsonObject
        {
            ["source"] = Source("unconfigured", "game_adapter_not_connected"), ["process_name"] = null, ["pid"] = null,
            ["rendering"] = null, ["fps"] = Missing("game_adapter_not_connected"),
            ["render_latency_ms"] = Missing("game_adapter_not_connected"), ["fps_history"] = new JsonArray()
        },
        ["codex"] = new JsonObject
        {
            ["source"] = Source("unconfigured", "codex_adapter_not_connected"), ["state"] = "unknown",
            ["quotas"] = new JsonArray(), ["sessions"] = new JsonArray()
        }
    };
}
