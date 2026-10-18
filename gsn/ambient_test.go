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

func TestAmbientReadOnlyAndSchema(t *testing.T) {
	calls := 0
	h := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.Method != "GET" || r.URL.Path != "/api/states/sensor.study_lux" {
			t.Error("unexpected request")
		}
		json.NewEncoder(w).Encode(entityState{State: "31", LastUpdated: time.Now().UTC().Format(time.RFC3339Nano), Attributes: map[string]any{"unit_of_measurement": "lx"}})
	}))
	defer h.Close()
	c := Config{HA: HAConfig{BaseURL: h.URL, Token: "hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh", Entities: map[string]string{"ambient_light": "sensor.study_lux"}}}
	if err := validateUpstreams(c); err != nil {
		t.Fatal(err)
	}
	u := NewUpstreams(c)
	u.poll(context.Background())
	snapshot := u.Snapshot(time.Now())
	a := object(snapshot, "ambient")
	if calls != 1 || number(object(a, "illuminance_lux")["value"]) != 31 || object(a, "source")["quality"] != "ok" {
		t.Fatal(a)
	}
	if err := contracts.Load("snapshot").Validate(snapshot); err != nil {
		t.Fatal(err)
	}
	if len(u.haBindings()["sensor.study_lux"]) != 1 {
		t.Fatal("ambient missing from shared HA subscription")
	}
}
func TestAmbientValidityFreshness(t *testing.T) {
	now := time.Now()
	u := NewUpstreams(Config{HA: HAConfig{Entities: map[string]string{"ambient_light": "sensor.study_lux"}}})
	for _, test := range []struct {
		state, unit string
		valid       bool
	}{{"0", "lx", true}, {"31", "lx", true}, {"200000", "lx", true}, {"-1", "lx", false}, {"NaN", "lx", false}, {"Inf", "lx", false}, {"200001", "lx", false}, {"31", "%", false}, {"31", "", false}, {"unknown", "lx", false}, {"unavailable", "lx", false}} {
		u.items["ambient_light"] = observation{at: now, entity: entityState{State: test.state, LastUpdated: now.Add(-time.Hour).Format(time.RFC3339Nano), Attributes: map[string]any{"unit_of_measurement": test.unit}}}
		a := object(u.Snapshot(now), "ambient")
		valid := object(a, "illuminance_lux")["value"] != nil
		if valid != test.valid {
			t.Fatalf("%+v: %v", test, a)
		}
	}
	u.items["ambient_light"] = observation{at: now, entity: entityState{State: "31", LastUpdated: now.Format(time.RFC3339Nano), Attributes: map[string]any{"unit_of_measurement": "lx"}}}
	for _, test := range []struct {
		age     time.Duration
		quality string
	}{{46 * time.Second, "stale"}, {121 * time.Second, "offline"}} {
		a := object(u.Snapshot(now.Add(test.age)), "ambient")
		if object(a, "source")["quality"] != test.quality || object(a, "illuminance_lux")["value"] != nil {
			t.Fatal("stale lux retained", a)
		}
	}
	u.streamReady = true
	u.streamKnown = map[string]bool{"ambient_light": true}
	u.streamVerified = now.Add(2 * time.Hour)
	a := object(u.Snapshot(u.streamVerified), "ambient")
	if object(a, "source")["quality"] != "ok" || object(a, "illuminance_lux")["value"] == nil {
		t.Fatal("healthy unchanged lux should remain valid", a)
	}
}
