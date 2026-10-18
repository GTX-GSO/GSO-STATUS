package main

import (
	"encoding/json"
	"gso-status/contracts"
	"strings"
	"testing"
)

func TestGameIconAndRenderLatency(t *testing.T) {
	c := config()
	s := NewServer(c)
	o := fixture(t)
	game := object(o, "game")
	icon := strings.Repeat("/4BA/4BA", 384)
	game["icon_argb8888"] = icon
	game["render_latency_ms"] = contracts.Object{"value": 0.25, "reason": nil}
	body, _ := json.Marshal(o)
	w := request(s, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, body)
	if w.Code != 200 || w.Header().Get("X-GSO-Game-Icon") != "1" {
		t.Fatalf("game ingestion: %d", w.Code)
	}
	w = request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil)
	model, e := contracts.Decode(w.Body.Bytes())
	if e != nil || contracts.Load("snapshot").Validate(model) != nil {
		t.Fatal("invalid snapshot")
	}
	result := object(model, "game")
	if result["icon_argb8888"] != icon || object(result, "render_latency_ms")["value"].(json.Number) != "0.25" {
		t.Fatal("game icon or sub-ms latency lost")
	}
	if len(w.Body.Bytes()) > maxPayload {
		t.Fatal("game data exceeded snapshot bound")
	}
}
