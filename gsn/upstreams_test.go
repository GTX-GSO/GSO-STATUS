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

func TestHAClashModeTakesPriority(t *testing.T) {
	calls := 0
	mock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.URL.Path != "/api/states/sensor.gayso_mihomo_mode" {
			t.Error("direct Mihomo must not override HA")
			w.WriteHeader(500)
			return
		}
		json.NewEncoder(w).Encode(entityState{State: "rule", LastUpdated: time.Now().UTC().Format(time.RFC3339Nano), Attributes: map[string]any{}})
	}))
	defer mock.Close()
	c := Config{HA: HAConfig{BaseURL: mock.URL, Token: "secret", Entities: map[string]string{"clash": "sensor.gayso_mihomo_mode"}}, Mihomo: MihomoConfig{BaseURL: mock.URL, Token: "secret"}}
	if validateUpstreams(c) != nil {
		t.Fatal("HA clash mapping rejected")
	}
	u := NewUpstreams(c)
	u.poll(context.Background())
	snapshot := u.Snapshot(time.Now())
	clash := object(object(object(snapshot, "net"), "services"), "clash")
	if calls != 1 || clash["raw_state"] != "rule" || object(clash, "source")["quality"] != "ok" {
		t.Fatal("HA mode not retained", clash)
	}
	if contracts.Load("snapshot").Validate(snapshot) != nil {
		t.Fatal("snapshot contract rejected HA mode")
	}
}

func TestTrustedHAWeatherLocation(t *testing.T) {
	u := NewUpstreams(Config{HA: HAConfig{Entities: map[string]string{"weather": "weather.home"}}})
	u.items["weather"] = observation{at: time.Now(), entity: entityState{State: "sunny", LastUpdated: time.Now().UTC().Format(time.RFC3339Nano), Attributes: map[string]any{"friendly_name": "HA home", "temperature": 25.5, "temperature_unit": "°C"}}}
	snapshot := u.Snapshot(time.Now())
	weather := object(snapshot, "weather")
	if weather["location"] != "HA home" || number(object(weather, "temperature_c")["value"]) != 25.5 {
		t.Fatal("HA weather location or temperature discarded", weather)
	}
	if contracts.Load("snapshot").Validate(snapshot) != nil {
		t.Fatal("trusted weather violates contract")
	}
}

func TestReadOnlyUpstreams(t *testing.T) {
	authorized := true
	mock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "GET" {
			t.Error("adapter sent a write")
		}
		if r.Header.Get("Authorization") != "Bearer secret" {
			w.WriteHeader(401)
			return
		}
		if !authorized {
			w.WriteHeader(401)
			return
		}
		if r.URL.Path == "/version" {
			w.Write([]byte(`{"version":"test"}`))
			return
		}
		state := "运行中"
		attributes := map[string]any{}
		switch r.URL.Path {
		case "/api/states/sensor.vpn":
			state = "1.5"
			attributes["unit_of_measurement"] = "GiB"
		case "/api/states/weather.test":
			state = "clear-night"
			attributes["temperature"] = 25.5
			attributes["temperature_unit"] = "°C"
		case "/api/states/sensor.rules":
			state = "123"
		}
		json.NewEncoder(w).Encode(map[string]any{"state": state, "last_updated": "2026-10-15T00:00:00Z", "attributes": attributes})
	}))
	defer mock.Close()
	c := config()
	c.HA = HAConfig{BaseURL: mock.URL, Token: "secret", Entities: map[string]string{"proxy": "sensor.proxy", "vpn_remaining": "sensor.vpn", "weather": "weather.test", "antiad_rule_count": "sensor.rules"}}
	c.Mihomo = MihomoConfig{mock.URL, "secret"}
	u := NewUpstreams(c)
	u.poll(context.Background())
	model := u.Snapshot(time.Now())
	if e := contracts.Load("snapshot").Validate(model); e != nil {
		t.Fatal(e)
	}
	net := object(model, "net")
	vpn := object(net, "vpn")
	if number(object(vpn, "remaining_bytes")["value"]) != 1610612736 {
		t.Fatal("GiB conversion")
	}
	if object(vpn, "total_bytes")["value"] != nil {
		t.Fatal("invented VPN total")
	}
	if object(object(object(net, "services"), "clash"), "source")["quality"] != "ok" {
		t.Fatal("mihomo probe")
	}
	if object(object(model, "weather"), "temperature_c")["value"] != 25.5 {
		t.Fatal("weather read")
	}
	if object(vpn, "source")["source_updated_at"] != "2026-10-15T00:00:00Z" {
		t.Fatal("upstream timestamp overwritten")
	}
	authorized = false
	u.poll(context.Background())
	model = u.Snapshot(time.Now())
	source := object(object(object(object(model, "net"), "services"), "proxy"), "source")
	if source["quality"] != "error" || source["last_error_code"] != "source_auth_failed" {
		t.Fatal("authentication failure hidden")
	}
	// Latest retained values remain visible with explicit source error.
	if object(object(object(model, "net"), "vpn"), "remaining_bytes")["value"] == nil {
		t.Fatal("last good value lost")
	}
}
func TestConfiguredBindings(t *testing.T) {
	c := config()
	c.HA.Entities = map[string]string{"arbitrary": "light.home"}
	if validateUpstreams(c) == nil {
		t.Fatal("unknown role accepted")
	}
	c.HA = HAConfig{BaseURL: "http://example.test", Token: "secret", Entities: map[string]string{"proxy": "sensor.proxy"}}
	if e := validateUpstreams(c); e != nil {
		t.Fatal(e)
	}
}
