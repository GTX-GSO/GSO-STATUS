package main

import (
	"context"
	"encoding/json"
	"gso-status/contracts"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestRichHomeControls(t *testing.T) {
	c := homeTestConfig("http://127.0.0.1:1")
	c.HA.Home = append(c.HA.Home, HomeBinding{EntityID: "fan.test"}, HomeBinding{EntityID: "humidifier.test"}, HomeBinding{EntityID: "switch.test"})
	u := NewUpstreams(c)
	h := NewHomeCommands(c, u)
	seedHome(u, "light.test", "on", map[string]any{"supported_color_modes": []any{"color_temp"}, "brightness": float64(204), "color_temp_kelvin": float64(4000), "min_color_temp_kelvin": float64(2700), "max_color_temp_kelvin": float64(6500)})
	seedHome(u, "fan.test", "on", map[string]any{"supported_features": float64(59), "preset_modes": []any{"直吹风", "自然风"}, "preset_mode": "直吹风", "percentage": float64(50), "percentage_step": float64(25)})
	seedHome(u, "climate.test", "cool", map[string]any{"supported_features": float64(385), "hvac_modes": []any{"cool", "off"}, "temperature": float64(25), "min_temp": float64(16), "max_temp": float64(30), "target_temp_step": float64(1), "temperature_unit": "°C"})
	seedHome(u, "humidifier.test", "on", map[string]any{"supported_features": float64(1), "available_modes": []any{"智能模式", "sleep"}, "mode": "智能模式", "min_humidity": float64(30), "max_humidity": float64(70), "humidity": float64(40)})
	seedHome(u, "switch.test", "off", nil)
	if err := contracts.Load("snapshot").Validate(u.Snapshot(time.Now())); err != nil {
		t.Fatal(err)
	}
	v := 75.0
	k := 4500.0
	mode := "自然风"
	cases := []HomeRequest{{EntityID: "light.test", Action: "set_brightness", Brightness: &v}, {EntityID: "light.test", Action: "set_color_temperature", Kelvin: &k}, {EntityID: "fan.test", Action: "set_percentage", Percentage: &v}, {EntityID: "fan.test", Action: "set_preset_mode", Mode: &mode}, {EntityID: "switch.test", Action: "turn_on"}}
	for _, r := range cases {
		data, code := h.plan(r, time.Now())
		if code != "" || data["entity_id"] != r.EntityID {
			t.Fatalf("control %s rejected: %s", r.Action, code)
		}
	}
	badMode := "injected"
	cases = []HomeRequest{{EntityID: "fan.test", Action: "set_preset_mode", Mode: &badMode}, {EntityID: "light.test", Action: "turn_on", Brightness: &v}, {EntityID: "light.test", Action: "set_brightness", Brightness: &v, Kelvin: &k}, {EntityID: "fan.test", Action: "set_percentage", Percentage: &k}}
	for _, r := range cases {
		if _, code := h.plan(r, time.Now()); code == "" {
			t.Fatalf("invalid parameter accepted: %s", r.Action)
		}
	}
	o, _ := u.homeObservation("fan.test")
	r := HomeRequest{EntityID: "fan.test", Action: "set_preset_mode", Mode: &mode}
	if h.confirmed(r, o) {
		t.Fatal("service request invented state confirmation")
	}
	o.entity.Attributes["preset_mode"] = mode
	if !h.confirmed(r, o) {
		t.Fatal("actual mode not confirmed")
	}
	if homeService("set_brightness") != "turn_on" || homeService("set_color_temperature") != "turn_on" {
		t.Fatal("light HA routing")
	}
}
func TestGroupedCurtains(t *testing.T) {
	c := homeTestConfig("http://127.0.0.1:1")
	c.HA.Home = []HomeBinding{{EntityID: "cover.group", Members: []string{"cover.left", "cover.right"}}}
	if validateUpstreams(c) != nil {
		t.Fatal("group binding rejected")
	}
	u := NewUpstreams(c)
	h := NewHomeCommands(c, u)
	seedHome(u, "cover.left", "open", map[string]any{"supported_features": float64(15), "current_position": float64(100)})
	seedHome(u, "cover.right", "closed", map[string]any{"supported_features": float64(15), "current_position": float64(0)})
	o, _ := u.homeObservation("cover.group")
	r := HomeRequest{EntityID: "cover.group", Action: "open_cover"}
	if h.confirmed(r, o) || o.entity.State != "mixed" {
		t.Fatal("partial group falsely confirmed")
	}
	data, code := h.plan(r, time.Now())
	if code != "" || len(data["entity_id"].([]string)) != 2 {
		t.Fatal("group dispatch not limited to both bound members")
	}
	seedHome(u, "cover.right", "open", map[string]any{"supported_features": float64(15), "current_position": float64(100)})
	o, _ = u.homeObservation("cover.group")
	if !h.confirmed(r, o) {
		t.Fatal("both members not confirmed")
	}
	if len(u.haBindings()) != 2 {
		t.Fatal("subscribed virtual group instead of actual members")
	}
	c.HA.Home = append(c.HA.Home, HomeBinding{EntityID: "cover.left"})
	if validateUpstreams(c) == nil {
		t.Fatal("duplicate group member binding accepted")
	}
}

func TestHomeRestReadCannotInvalidateNewerHealthyStream(t *testing.T) {
	old := time.Now().Add(-time.Minute).UTC().Format(time.RFC3339Nano)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(map[string]any{"state": "on", "last_updated": old, "attributes": map[string]any{}})
	}))
	defer server.Close()
	u := NewUpstreams(homeTestConfig(server.URL))
	seedHome(u, "light.test", "off", nil)
	u.streamReady = true
	u.streamVerified = time.Now()
	u.streamKnown = map[string]bool{"home:light.test": true}
	o := u.fetchHome(context.Background(), "light.test")
	if o.code != "" || o.entity.State != "off" || sourceFrom(o, true, time.Now())["quality"] != "ok" {
		t.Fatal("old REST response invalidated newer authenticated stream")
	}
	u.streamReady = false
	o = u.fetchHome(context.Background(), "light.test")
	if o.code != "entity_state_regressed" {
		t.Fatal("unverified regression was silently accepted")
	}
}

func TestGroupedCoverReversalSupersedesMovement(t *testing.T) {
	c := homeTestConfig("http://127.0.0.1:1")
	u := NewUpstreams(c)
	h := NewHomeCommands(c, u)
	seedHome(u, "cover.test", "opening", map[string]any{"supported_features": float64(15), "current_position": float64(25)})
	_, status, code := h.Submit(homeBody(h, 1, "cover.test", "open_cover", time.Now().Add(20*time.Second), nil), time.Now())
	if status != 202 || code != "" {
		t.Fatal("open not queued")
	}
	_, status, code = h.Submit(homeBody(h, 2, "cover.test", "close_cover", time.Now().Add(20*time.Second), nil), time.Now())
	if status != 202 || code != "" {
		t.Fatal("opposite direction blocked")
	}
	old, _ := h.Get("00000000-0000-4000-8000-000000000001")
	if old["status"] != "superseded" {
		t.Fatal("old movement not superseded")
	}
}

func TestKnownReadOnlyControlsCannotBeCalled(t *testing.T) {
	c := homeTestConfig("http://127.0.0.1:1")
	c.HA.Home = []HomeBinding{{EntityID: "humidifier.test", DisabledActions: []string{"set_mode", "set_humidity"}}}
	u := NewUpstreams(c)
	seedHome(u, "humidifier.test", "on", map[string]any{"supported_features": float64(1), "available_modes": []any{"auto", "sleep"}, "mode": "auto", "min_humidity": float64(30), "max_humidity": float64(70), "humidity": float64(40)})
	h := NewHomeCommands(c, u)
	mode := "sleep"
	r := HomeRequest{EntityID: "humidifier.test", Action: "set_mode", Mode: &mode}
	if _, code := h.plan(r, time.Now()); code != "action_not_supported_or_source_stale" {
		t.Fatal("read-only override bypassed")
	}
	entities := object(u.Snapshot(time.Now()), "home")["entities"].([]any)
	for _, action := range entities[0].(contracts.Object)["actions"].([]any) {
		if action == "set_mode" || action == "set_humidity" {
			t.Fatal("read-only action advertised")
		}
	}
}
