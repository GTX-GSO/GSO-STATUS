package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func TestHAStreamProjection(t *testing.T) {
	bindings := map[string][]string{"climate.test": {"home:climate.test"}, "light.missing": {"home:light.missing"}}
	cache := map[string]entityState{}
	initial := map[string]any{"a": map[string]any{"climate.test": map[string]any{"s": "off", "lc": float64(1792100000), "a": map[string]any{"temperature": float64(26), "current_temperature": nil, "supported_features": float64(393), "secret_unneeded": "discard"}}, "sensor.other": false}}
	changed, err := haStreamEvent(initial, cache, bindings, true)
	if err != nil || !changed["climate.test"] || changed["light.missing"] {
		t.Fatalf("initial: %v %v", changed, err)
	}
	if _, ok := cache["climate.test"].Attributes["secret_unneeded"]; ok {
		t.Fatal("unneeded attribute retained")
	}
	original := cache["climate.test"]
	delta := map[string]any{"c": map[string]any{"climate.test": map[string]any{"+": map[string]any{"s": "cool", "lu": float64(1792100001), "a": map[string]any{"temperature": float64(27)}}, "-": map[string]any{"a": []any{"supported_features"}}}}}
	if _, err = haStreamEvent(delta, cache, bindings, false); err != nil {
		t.Fatal(err)
	}
	next := cache["climate.test"]
	if next.State != "cool" || next.Attributes["temperature"] != float64(27) || original.Attributes["temperature"] != float64(26) {
		t.Fatal("delta mutated old observation or lost state")
	}
	if _, ok := next.Attributes["supported_features"]; ok {
		t.Fatal("removed attribute retained")
	}
	if next.Attributes["current_temperature"] != nil {
		t.Fatal("missing room temperature synthesized")
	}
	if _, err = haStreamEvent(map[string]any{"c": map[string]any{"light.missing": map[string]any{}}}, cache, bindings, false); err == nil {
		t.Fatal("unknown delta base accepted")
	}
	if _, err = haStreamEvent(map[string]any{}, cache, bindings, true); err == nil {
		t.Fatal("missing initial state accepted")
	}
}

func TestHAStreamSessionAndFreshness(t *testing.T) {
	var rest atomic.Int32
	handshake := make(chan struct{}, 1)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/websocket" {
			rest.Add(1)
			w.WriteHeader(500)
			return
		}
		conn, err := websocket.Accept(w, r, nil)
		if err != nil {
			return
		}
		defer conn.CloseNow()
		ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
		defer cancel()
		send := func(v any) bool { return writeHAStream(ctx, conn, v) == nil }
		if !send(map[string]any{"type": "auth_required"}) {
			return
		}
		auth, err := readHAStream(ctx, conn)
		if err != nil || auth["access_token"] != "test-token" {
			t.Error("auth failed")
			return
		}
		if !send(map[string]any{"type": "auth_ok"}) {
			return
		}
		sub, err := readHAStream(ctx, conn)
		if err != nil {
			t.Error(err)
			return
		}
		ids, ok := sub["entity_ids"].([]any)
		if !ok || len(ids) != 1 || ids[0] != "light.test" || sub["type"] != "subscribe_entities" {
			t.Errorf("unexpected subscription: %v", sub)
			return
		}
		send(map[string]any{"id": 1, "type": "result", "success": true})
		send(map[string]any{"id": 1, "type": "event", "event": map[string]any{"a": map[string]any{"light.test": map[string]any{"s": "off", "lc": 1792100000, "a": map[string]any{"supported_features": 0}}}}})
		handshake <- struct{}{}
		// Read pumps protocol pings/pongs without sending any entity update.
		conn.Read(ctx)
	}))
	defer server.Close()
	u := NewUpstreams(Config{HA: HAConfig{BaseURL: server.URL, Token: "test-token", Home: []HomeBinding{{EntityID: "light.test"}}}})
	u.wsPingEvery = 10 * time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- u.haStreamSession(ctx, u.haBindings()) }()
	select {
	case <-handshake:
	case <-time.After(time.Second):
		t.Fatal("handshake timed out")
	}
	deadline := time.Now().Add(time.Second)
	for !u.streamHealthy() && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if !u.streamHealthy() {
		t.Fatal("stream never became ready")
	}
	u.mu.Lock()
	first := u.items["home:light.test"]
	u.mu.Unlock()
	time.Sleep(70 * time.Millisecond)
	u.mu.Lock()
	live := u.liveObservation("home:light.test", time.Now())
	u.mu.Unlock()
	if !live.at.After(first.at) || live.entity.LastUpdated != first.entity.LastUpdated {
		t.Fatal("pong freshness or source timestamp wrong")
	}
	u.poll(ctx)
	if rest.Load() != 0 {
		t.Fatal("healthy stream made redundant HA poll")
	}
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("shutdown did not terminate")
	}
	if u.streamHealthy() {
		t.Fatal("disconnected stream remains healthy")
	}
	u.mu.Lock()
	disconnected := u.liveObservation("home:light.test", time.Now())
	later := u.liveObservation("home:light.test", time.Now().Add(time.Minute))
	u.mu.Unlock()
	if !disconnected.at.Equal(later.at) {
		t.Fatal("disconnected state freshness advanced")
	}
}

func TestHAStreamRejectsUntrustedFrames(t *testing.T) {
	for _, mode := range []string{"auth", "binary", "duplicate", "oversized", "unsupported"} {
		t.Run(mode, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				c, e := websocket.Accept(w, r, nil)
				if e != nil {
					return
				}
				defer c.CloseNow()
				ctx, cancel := context.WithTimeout(r.Context(), time.Second)
				defer cancel()
				if mode == "binary" {
					c.Write(ctx, websocket.MessageBinary, []byte(`{}`))
					return
				}
				if mode == "duplicate" {
					c.Write(ctx, websocket.MessageText, []byte(`{"type":"auth_required","type":"auth_ok"}`))
					return
				}
				if mode == "oversized" {
					c.Write(ctx, websocket.MessageText, []byte(`{"type":"`+strings.Repeat("x", haStreamLimit)+`"}`))
					return
				}
				writeHAStream(ctx, c, map[string]any{"type": "auth_required"})
				readHAStream(ctx, c)
				if mode == "auth" {
					writeHAStream(ctx, c, map[string]any{"type": "auth_invalid"})
					return
				}
				writeHAStream(ctx, c, map[string]any{"type": "auth_ok"})
				readHAStream(ctx, c)
				writeHAStream(ctx, c, map[string]any{"id": 1, "type": "result", "success": false})
			}))
			defer server.Close()
			u := NewUpstreams(Config{HA: HAConfig{BaseURL: server.URL, Token: "test-token", Entities: map[string]string{"proxy": "sensor.test"}}})
			if err := u.haStreamSession(context.Background(), u.haBindings()); err == nil {
				t.Fatal("invalid frame accepted")
			}
			if u.streamHealthy() {
				t.Fatal("invalid stream enabled freshness")
			}
		})
	}
}

func TestHAStreamNoRedirectCredentialLeak(t *testing.T) {
	var calls atomic.Int32
	other := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { calls.Add(1) }))
	defer other.Close()
	origin := httptest.NewServer(http.RedirectHandler(other.URL, http.StatusFound))
	defer origin.Close()
	u := NewUpstreams(Config{HA: HAConfig{BaseURL: origin.URL, Token: "test-token", Entities: map[string]string{"proxy": "sensor.test"}}})
	if err := u.haStreamSession(context.Background(), u.haBindings()); err == nil {
		t.Fatal("redirect accepted")
	}
	if calls.Load() != 0 {
		t.Fatal("redirect destination reached")
	}
}

func TestHACompressedTimestampPrecision(t *testing.T) {
	var number json.Number = "1792100000.123456"
	stamp, ok := haTimestamp(number)
	if !ok || stamp != "2026-10-15T21:33:20.123456Z" {
		t.Fatalf("timestamp: %s %v", stamp, ok)
	}
}

func TestHAStreamRESTFallbackAndStateRegression(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		json.NewEncoder(w).Encode(entityState{State: "on", LastUpdated: "2026-10-15T22:00:00Z", Attributes: map[string]any{}})
	}))
	defer server.Close()
	u := NewUpstreams(Config{HA: HAConfig{BaseURL: server.URL, Token: "test-token", Entities: map[string]string{"proxy": "sensor.test"}}})
	u.poll(context.Background())
	if calls.Load() != 1 || u.items["proxy"].entity.State != "on" {
		t.Fatal("initial REST fallback failed")
	}
	u.applyStream(map[string]entityState{"sensor.test": {State: "off", LastUpdated: "2026-10-15T23:00:00Z", Attributes: map[string]any{}}}, map[string]bool{"sensor.test": true}, u.haBindings(), time.Now())
	u.poll(context.Background())
	if calls.Load() != 1 {
		t.Fatal("healthy stream did not suppress REST")
	}
	u.stopStream()
	u.poll(context.Background())
	if calls.Load() != 2 || u.items["proxy"].entity.State != "off" || u.items["proxy"].code != "entity_state_regressed" {
		t.Fatal("fallback rolled state backward")
	}
}
