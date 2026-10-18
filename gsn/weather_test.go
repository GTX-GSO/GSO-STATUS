package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"testing"
	"time"
)

func TestWeatherForecastReadAndExpiry(t *testing.T) {
	calls := 0
	mock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.Method != "POST" || r.URL.Path != "/api/services/weather/get_forecasts" || r.URL.RawQuery != "return_response" {
			t.Error("unexpected weather operation")
		}
		w.Write([]byte(`{"service_response":{"weather.home":{"forecast":[{"datetime":"2026-10-16T00:00:00Z","condition":"cloudy","temperature":31.9,"templow":25.1}]}}}`))
	}))
	defer mock.Close()
	u := NewUpstreams(Config{HA: HAConfig{BaseURL: mock.URL, Token: "weather-token", TemperatureUnit: "°C", Entities: map[string]string{"weather": "weather.home"}}})
	if !u.pollForecast(context.Background()) || len(u.forecast) != 1 {
		t.Fatal("forecast unavailable")
	}
	if !u.pollForecastKind(context.Background(), "hourly") || len(u.hourly) != 1 {
		t.Fatal("hourly forecast unavailable")
	}
	for i := 0; i < 20; i++ {
		u.Snapshot(time.Now())
	}
	if calls != 2 {
		t.Fatal("snapshot reads polled forecasts")
	}
	if len(object(u.Snapshot(time.Now().Add(3*time.Hour)), "weather")["forecast"].([]any)) != 0 {
		t.Fatal("expired forecast exposed")
	}
	o := observation{entity: entityState{Attributes: map[string]any{"pressure": 101690.0}}}
	if number(weatherNumber(o, "pressure", 0, 2000, 0.01)["value"]) != 1016.9 {
		t.Fatal("pressure conversion")
	}
	if weatherNumber(o, "pressure", 0, 2000, 0)["value"] != nil {
		t.Fatal("unknown units silently converted")
	}
}
func TestHAWeatherLiveRead(t *testing.T) {
	path := os.Getenv("GSO_HA_LIVE_CONFIG")
	if path == "" {
		t.Skip("opt-in HA weather read")
	}
	cfg, e := loadConfig(path)
	if e != nil {
		t.Fatal("private configuration invalid")
	}
	u := NewUpstreams(cfg)
	id := cfg.HA.Entities["weather"]
	var state entityState
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if code := u.fetch(ctx, cfg.HA.BaseURL+"/api/states/"+id, cfg.HA.Token, &state); code != "" {
		t.Fatal(code)
	}
	u.items["weather"] = observation{entity: state, at: time.Now()}
	if !u.pollForecast(ctx) || !u.pollForecastKind(ctx, "hourly") {
		t.Fatal("HA daily forecast read failed")
	}
	s := u.Snapshot(time.Now())
	if u.schema.Validate(s) != nil {
		t.Fatal("actual weather violates contract")
	}
	w := object(s, "weather")
	report := map[string]any{"passed": true, "scope": "Actual HA weather and read-only daily/hourly forecast actions", "forecast_days": len(u.forecast), "forecast_hours": len(u.hourly), "humidity_available": object(w, "humidity_percent")["value"] != nil, "pressure_available": object(w, "pressure_hpa")["value"] != nil, "wind_available": object(w, "wind_speed_kmh")["value"] != nil, "household_actions": 0, "raw_states_saved": false}
	if target := os.Getenv("GSO_WEATHER_LIVE_REPORT"); target != "" {
		raw, _ := json.MarshalIndent(report, "", "  ")
		if os.WriteFile(target, append(raw, 10), 0600) != nil {
			t.Fatal("report write failed")
		}
	}
	t.Logf("Verified actual weather with %d forecast days", len(u.forecast))
}
