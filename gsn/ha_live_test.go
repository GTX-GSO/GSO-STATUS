package main

// Explicit opt-in verification of real HA reads. Never starts HomeCommands.
import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"sync/atomic"
	"testing"
	"time"
)

type countHAReads struct {
	base   http.RoundTripper
	states atomic.Int32
}

func (c *countHAReads) RoundTrip(r *http.Request) (*http.Response, error) {
	if len(r.URL.Path) >= 12 && r.URL.Path[:12] == "/api/states/" {
		c.states.Add(1)
	}
	return c.base.RoundTrip(r)
}
func TestHALiveReadOnlySubscription(t *testing.T) {
	path := os.Getenv("GSO_HA_LIVE_CONFIG")
	if path == "" {
		t.Skip("explicit private HA config required")
	}
	cfg, err := loadConfig(path)
	if err != nil {
		t.Fatal("private configuration rejected")
	}
	cfg.RouterURL = ""
	cfg.Mihomo = MihomoConfig{}
	u := NewUpstreams(cfg)
	counter := &countHAReads{base: u.client.Transport}
	u.client.Transport = counter
	ctx, cancel := context.WithTimeout(context.Background(), 24*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- u.haStreamSession(ctx, u.haBindings()) }()
	started := time.Now()
	for !u.streamHealthy() {
		select {
		case err := <-done:
			t.Fatalf("authenticated subscription failed: %v", err)
		default:
		}
		if time.Since(started) > 5*time.Second {
			t.Fatal("initial subscription not ready")
		}
		time.Sleep(10 * time.Millisecond)
	}
	u.mu.Lock()
	first := u.streamVerified
	u.mu.Unlock()
	timer := time.NewTimer(17 * time.Second)
	defer timer.Stop()
	select {
	case err := <-done:
		t.Fatalf("subscription ended before heartbeat: %v", err)
	case <-timer.C:
	}
	u.mu.Lock()
	healthy := u.streamReady && u.streamVerified.After(first)
	allStates := true
	missingRoom := 0
	for key, observation := range u.items {
		if observation.code != "" || observation.at.IsZero() {
			allStates = false
		}
		if len(key) > 13 && key[:13] == "home:climate." && observation.entity.Attributes["current_temperature"] == nil {
			missingRoom++
		}
	}
	count := len(u.streamKnown)
	u.mu.Unlock()
	if !healthy || !allStates {
		t.Fatal("subscription freshness or entity validation failed")
	}
	u.poll(ctx)
	if counter.states.Load() != 0 {
		t.Fatal("healthy subscription redundantly polled HA entities")
	}
	snapshot := u.Snapshot(time.Now())
	if err := u.schema.Validate(snapshot); err != nil {
		t.Fatalf("live snapshot violates contract: %v", err)
	}
	netDetails := object(snapshot, "net")
	detailsAvailable := true
	for field, role := range map[string]string{"subscription_updated_at": "vpn_updated", "antiad_rules_updated_at": "antiad_rules_updated", "antiad_rule_count": "antiad_rule_count", "dhcp_active_leases": "dhcp_active_leases"} {
		if cfg.HA.Entities[role] == "" {
			continue
		}
		v := netDetails[field]
		if field == "antiad_rule_count" || field == "dhcp_active_leases" {
			v = object(netDetails, field)["value"]
		}
		if v == nil {
			detailsAvailable = false
		}
	}
	if !detailsAvailable {
		t.Fatal("configured NET metadata unavailable")
	}
	payload, _ := json.Marshal(snapshot)
	if len(payload) > maxPayload {
		t.Fatal("rich HOME exceeds payload bound")
	}
	advanced := 0
	for _, item := range object(snapshot, "home")["entities"].([]any) {
		if len(item.(map[string]any)["actions"].([]any)) > 2 {
			advanced++
		}
	}
	report := map[string]any{"scope": "Actual authenticated HA WebSocket reads only; no service calls", "configured_observation_keys": count, "home_bindings": len(cfg.HA.Home), "advanced_home_cards": advanced, "snapshot_bytes": len(payload), "net_weather_sun_bindings": len(cfg.HA.Entities), "authenticated_subscription": true, "protocol_heartbeat_verified": true, "healthy_stream_rest_reads": counter.states.Load(), "climate_missing_room_temperature_preserved": missingRoom, "contract_valid": true, "net_detail_fields_available": detailsAvailable, "physical_actions": 0, "raw_states_or_tokens_saved": false, "passed": true}
	if output := os.Getenv("GSO_HA_LIVE_REPORT"); output != "" {
		raw, _ := json.MarshalIndent(report, "", "  ")
		if err := os.WriteFile(output, append(raw, '\n'), 0600); err != nil {
			t.Fatal(err)
		}
	}
	cancel()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("subscription did not shut down")
	}
	t.Logf("Verified %d real HA observation keys; no household actions", count)
}
