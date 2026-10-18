package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"regexp"
	"testing"
	"time"
)

// Explicit live diagnostic; only aggregate rate changes and timer declarations leave memory.
func TestRouterLiveRefreshCadence(t *testing.T) {
	cfgPath := os.Getenv("GSO_ROUTER_LIVE_CONFIG")
	if cfgPath == "" {
		t.Skip("opt-in live router test")
	}
	cfg, e := loadConfig(cfgPath)
	if e != nil {
		t.Fatal("invalid private config")
	}
	r := NewRouter(cfg.RouterURL)
	r.credentialsPath = os.Getenv("GSO_ROUTER_LIVE_CREDENTIALS")
	defer r.client.CloseIdleConnections()
	ctx, cancel := context.WithTimeout(context.Background(), 55*time.Second)
	defer cancel()
	if !r.pollOnce(ctx) {
		if code := r.authenticate(ctx); code != "" {
			t.Fatal(code)
		}
	}
	request, _ := http.NewRequestWithContext(ctx, "GET", r.url, nil)
	resp, e := r.client.Do(request)
	if e != nil {
		t.Fatal("router page unavailable")
	}
	raw, e := io.ReadAll(io.LimitReader(resp.Body, 262145))
	resp.Body.Close()
	if e != nil || len(raw) > 262144 || resp.StatusCode != 200 {
		t.Fatal("page limit or HTTP failure")
	}
	timers := regexp.MustCompile(`(?i)set(?:Timeout|Interval)\s*\([^;\r\n]{0,160}`).FindAll(raw, 32)
	// Record only numeric timer delays; no page, callback text, cookies or credentials.
	declarations := []int{}
	delayPattern := regexp.MustCompile(`,\s*([0-9]{2,6})\s*\)`)
	for _, timer := range timers {
		m := delayPattern.FindSubmatch(timer)
		if m != nil {
			var n int
			json.Unmarshal(m[1], &n)
			declarations = append(declarations, n)
		}
	}
	var priorDown, priorUp uint64
	var changes []float64
	valid := 0
	started := time.Now()
	for i := 0; i < 32; i++ {
		if ctx.Err() != nil {
			break
		}
		if r.pollOnce(ctx) {
			valid++
			if valid > 1 && (r.down != priorDown || r.up != priorUp) {
				changes = append(changes, time.Since(started).Seconds())
			}
			priorDown, priorUp = r.down, r.up
		}
		timer := time.NewTimer(time.Until(started.Add(time.Duration(i+1) * time.Second)))
		select {
		case <-ctx.Done():
			timer.Stop()
		case <-timer.C:
		}
	}
	report := map[string]any{"scope": "Actual BX54 page timer declarations and bounded 1Hz WAN change observation; no raw pages or rates retained", "valid_samples": valid, "timer_delays_ms": declarations, "rate_change_offsets_seconds": changes, "observation_seconds": time.Since(started).Seconds(), "raw_response_saved": false}
	if p := os.Getenv("GSO_ROUTER_LIVE_REPORT"); p != "" {
		b, _ := json.MarshalIndent(report, "", "  ")
		if os.WriteFile(p, append(b, 10), 0600) != nil {
			t.Fatal("report write failed")
		}
	}
	t.Logf("Observed %d samples, %d changes, timer delays %v", valid, len(changes), declarations)
	if valid < 30 {
		t.Fatal("too few successful samples")
	}
}
