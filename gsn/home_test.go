package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"gso-status/contracts"
)

func homeTestConfig(base string) Config {
	c := config()
	c.HomeControlToken = strings.Repeat("c", 32)
	c.HA = HAConfig{BaseURL: base, Token: "test-ha-token", Home: []HomeBinding{{EntityID: "light.test", Name: "Test light"}, {EntityID: "climate.test"}, {EntityID: "cover.test"}}}
	return c
}
func seedHome(u *Upstreams, id, state string, attrs map[string]any) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.items["home:"+id] = observation{entity: entityState{State: state, LastUpdated: time.Now().UTC().Format(time.RFC3339Nano), Attributes: attrs}, at: time.Now()}
}
func homeBody(h *HomeCommands, n int, entity, action string, expiry time.Time, temp *float64) []byte {
	b, _ := json.Marshal(HomeRequest{ControlEpoch: h.epoch, RequestID: fmt.Sprintf("00000000-0000-4000-8000-%012d", n), EntityID: entity, Action: action, ExpiresAt: expiry, Temperature: temp})
	return b
}
func homeStatus(t *testing.T, h *HomeCommands, id, status string) {
	t.Helper()
	v, ok := h.Get(id)
	if ok {
		if e := contracts.Load("home-result").Validate(v); e != nil {
			t.Fatal(e)
		}
	}
	if !ok || v["status"] != status {
		t.Fatalf("wanted %s, got %#v", status, v)
	}
}
func TestHomeCapabilitiesAndUnits(t *testing.T) {
	c := homeTestConfig("http://127.0.0.1:1")
	u := NewUpstreams(c)
	seedHome(u, "light.test", "off", nil)
	seedHome(u, "climate.test", "cool", map[string]any{"supported_features": float64(385), "temperature_unit": "°F", "temperature": float64(77), "current_temperature": nil, "min_temp": float64(60), "max_temp": float64(90), "target_temp_step": float64(1)})
	seedHome(u, "cover.test", "open", map[string]any{"supported_features": float64(3)})
	m := u.Snapshot(time.Now())
	if e := contracts.Load("snapshot").Validate(m); e != nil {
		t.Fatal(e)
	}
	entities := object(m, "home")["entities"].([]any)
	climate := entities[1].(contracts.Object)
	if object(climate, "current_temperature_c")["value"] != nil {
		t.Fatal("target temperature fabricated as room temperature")
	}
	if number(object(climate, "target_temperature_c")["value"]) != 25 {
		t.Fatal("Fahrenheit not converted")
	}
	if len(climate["actions"].([]any)) != 3 || len(entities[2].(contracts.Object)["actions"].([]any)) != 2 {
		t.Fatal("capabilities not respected")
	}
	seedHome(u, "cover.test", "unavailable", map[string]any{"supported_features": float64(11)})
	if actions := homeActions("cover.test", u.items["home:cover.test"], time.Now(), ""); len(actions) != 0 {
		t.Fatal("offline entity exposes controls")
	}
	c.HomeControlToken = ""
	u = NewUpstreams(c)
	seedHome(u, "light.test", "on", nil)
	if len(object(u.Snapshot(time.Now()), "home")["entities"].([]any)[0].(contracts.Object)["actions"].([]any)) != 0 {
		t.Fatal("unconfigured control token exposed actions")
	}
}
func TestHomeHTTPAuthIdempotencyAndEpoch(t *testing.T) {
	c := homeTestConfig("http://127.0.0.1:1")
	s := NewServer(c)
	seedHome(s.upstreams, "light.test", "off", nil)
	body := homeBody(s.home, 1, "light.test", "turn_on", time.Now().Add(5*time.Second), nil)
	for _, token := range []string{"", c.ReadToken, c.WriteToken} {
		if w := request(s, "POST", "/api/v1/home/commands", token, body); w.Code != 401 {
			t.Fatal("non-control token accepted")
		}
	}
	if len(s.home.queue) != 0 {
		t.Fatal("unauthorized command queued")
	}
	if w := request(s, "POST", "/api/v1/home/commands", c.HomeControlToken, body); w.Code != 202 {
		t.Fatalf("submit: %d %s", w.Code, w.Body)
	}
	if w := request(s, "POST", "/api/v1/home/commands", c.HomeControlToken, body); w.Code != 200 {
		t.Fatal("duplicate not idempotent")
	}
	if len(s.home.queue) != 1 {
		t.Fatal("duplicate queued twice")
	}
	var changed map[string]any
	json.Unmarshal(body, &changed)
	changed["action"] = "turn_off"
	different, _ := json.Marshal(changed)
	if w := request(s, "POST", "/api/v1/home/commands", c.HomeControlToken, different); w.Code != 409 {
		t.Fatal("idempotency conflict accepted")
	}
	if w := request(s, "GET", "/api/v1/home/commands/00000000-0000-4000-8000-000000000001", c.ReadToken, nil); w.Code != 200 {
		t.Fatal("readback inaccessible")
	}
	restarted := NewServer(c)
	seedHome(restarted.upstreams, "light.test", "off", nil)
	if w := request(restarted, "POST", "/api/v1/home/commands", c.HomeControlToken, body); w.Code != 409 {
		t.Fatal("old command replayed after service restart")
	}
}
func TestHomeRejectsUnboundExpiredAndInvalidTemperature(t *testing.T) {
	c := homeTestConfig("http://127.0.0.1:1")
	u := NewUpstreams(c)
	h := NewHomeCommands(c, u)
	seedHome(u, "light.test", "off", nil)
	seedHome(u, "climate.test", "cool", map[string]any{"supported_features": float64(1), "temperature_unit": "°C", "min_temp": float64(16), "max_temp": float64(30), "target_temp_step": float64(1)})
	temp := 15.0
	half := 22.5
	for _, tc := range []struct {
		entity, action string
		expires        time.Time
		temp           *float64
		code           string
	}{
		{"light.other", "turn_on", time.Now().Add(time.Second), nil, "entity_not_bound"},
		{"light.test", "turn_on", time.Now().Add(-time.Second), nil, "command_expired_or_too_far"},
		{"light.test", "turn_on", time.Now().Add(time.Minute), nil, "command_expired_or_too_far"},
		{"climate.test", "set_temperature", time.Now().Add(time.Second), &temp, "temperature_out_of_range"},
		{"climate.test", "set_temperature", time.Now().Add(time.Second), &half, "temperature_step_invalid"},
		{"light.test", "turn_on", time.Now().Add(time.Second), &temp, "unexpected_temperature"},
		{"climate.test", "turn_off", time.Now().Add(time.Second), nil, "action_not_supported_or_source_stale"},
	} {
		_, _, code := h.Submit(homeBody(h, 1, tc.entity, tc.action, tc.expires, tc.temp), time.Now())
		if code != tc.code {
			t.Fatalf("wanted %s got %s", tc.code, code)
		}
	}
	if len(h.queue) != 0 {
		t.Fatal("invalid command queued")
	}
}
func TestHomeCommandDeliveryAndStateConfirmation(t *testing.T) {
	var calls atomic.Int32
	var mu sync.Mutex
	state := "off"
	change := true
	mock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer test-ha-token" {
			t.Error("missing HA authorization")
			w.WriteHeader(401)
			return
		}
		mu.Lock()
		defer mu.Unlock()
		if r.Method == "POST" {
			calls.Add(1)
			if r.URL.Path != "/api/services/light/turn_on" {
				t.Error("wrong HA service")
			}
			if change {
				state = "on"
			}
			w.Write([]byte("[]"))
			return
		}
		json.NewEncoder(w).Encode(map[string]any{"state": state, "last_updated": time.Now().UTC().Format(time.RFC3339Nano), "attributes": map[string]any{}})
	}))
	defer mock.Close()
	c := homeTestConfig(mock.URL)
	u := NewUpstreams(c)
	seedHome(u, "light.test", "off", nil)
	h := NewHomeCommands(c, u)
	body := homeBody(h, 1, "light.test", "turn_on", time.Now().Add(2*time.Second), nil)
	result, _, code := h.Submit(body, time.Now())
	if code != "" {
		t.Fatal(code)
	}
	id := <-h.queue
	h.execute(context.Background(), id)
	homeStatus(t, h, id, "confirmed")
	if calls.Load() != 1 {
		t.Fatal("service POST repeated")
	}
	if _, status, _ := h.Submit(body, time.Now()); status != 200 {
		t.Fatal("completed duplicate not idempotent")
	}
	if calls.Load() != 1 || result["status"] != "queued" {
		t.Fatal("delivery stages not isolated")
	}
	// A successful service HTTP response without the requested state must time out.
	mu.Lock()
	state = "off"
	change = false
	mu.Unlock()
	seedHome(u, "light.test", "off", nil)
	h = NewHomeCommands(c, u)
	_, _, code = h.Submit(homeBody(h, 2, "light.test", "turn_on", time.Now().Add(150*time.Millisecond), nil), time.Now())
	if code != "" {
		t.Fatal(code)
	}
	id = <-h.queue
	h.execute(context.Background(), id)
	homeStatus(t, h, id, "timed_out")
	if calls.Load() != 2 {
		t.Fatal("unconfirmed action was retried")
	}
}
func TestHomeExpiredQueueAndRedirectNeverWriteAgain(t *testing.T) {
	var posts, leaks atomic.Int32
	mock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/leak" {
			leaks.Add(1)
		}
		if r.Method == "POST" {
			posts.Add(1)
			w.Header().Set("Location", "/leak")
			w.WriteHeader(302)
			return
		}
		w.WriteHeader(404)
	}))
	defer mock.Close()
	c := homeTestConfig(mock.URL)
	u := NewUpstreams(c)
	seedHome(u, "light.test", "off", nil)
	h := NewHomeCommands(c, u)
	_, _, code := h.Submit(homeBody(h, 1, "light.test", "turn_on", time.Now().Add(20*time.Millisecond), nil), time.Now())
	if code != "" {
		t.Fatal(code)
	}
	time.Sleep(30 * time.Millisecond)
	id := <-h.queue
	h.execute(context.Background(), id)
	homeStatus(t, h, id, "timed_out")
	if posts.Load() != 0 {
		t.Fatal("expired queue wrote to HA")
	}
	h = NewHomeCommands(c, u)
	h.Submit(homeBody(h, 2, "light.test", "turn_on", time.Now().Add(time.Second), nil), time.Now())
	id = <-h.queue
	h.execute(context.Background(), id)
	homeStatus(t, h, id, "rejected")
	if posts.Load() != 1 || leaks.Load() != 0 {
		t.Fatal("redirect followed or service retried")
	}
}
func TestHomeQueueBoundAndSourceExpiry(t *testing.T) {
	c := homeTestConfig("http://127.0.0.1:1")
	c.HA.Home = nil
	for n := 0; n < 9; n++ {
		c.HA.Home = append(c.HA.Home, HomeBinding{EntityID: fmt.Sprintf("light.test%d", n)})
	}
	u := NewUpstreams(c)
	h := NewHomeCommands(c, u)
	for n, b := range c.HA.Home {
		seedHome(u, b.EntityID, "off", nil)
		_, status, code := h.Submit(homeBody(h, n, b.EntityID, "turn_on", time.Now().Add(time.Second), nil), time.Now())
		if n < 8 && status != 202 {
			t.Fatal(code)
		}
		if n == 8 && code != "command_queue_full" {
			t.Fatal("unbounded command queue")
		}
	}
	if len(h.records) != 8 {
		t.Fatal("queue rejection leaked records")
	}
	u.mu.Lock()
	o := u.items["home:light.test8"]
	o.at = time.Now().Add(-time.Minute)
	u.items["home:light.test8"] = o
	u.mu.Unlock()
	_, _, code := h.Submit(homeBody(h, 10, "light.test8", "turn_on", time.Now().Add(time.Second), nil), time.Now())
	if code != "action_not_supported_or_source_stale" {
		t.Fatal("stale source allowed command")
	}
}

func TestHomeTemperatureNativeDispatchAndMotionConfirmation(t *testing.T) {
	var sentTemperature float64
	mock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "POST" {
			var data map[string]any
			json.NewDecoder(r.Body).Decode(&data)
			sentTemperature = data["temperature"].(float64)
			if r.URL.Path != "/api/services/climate/set_temperature" {
				t.Error("incorrect temperature service")
			}
			w.Write([]byte("[]"))
			return
		}
		json.NewEncoder(w).Encode(map[string]any{"state": "cool", "last_updated": time.Now().UTC().Format(time.RFC3339Nano), "attributes": map[string]any{"temperature": 77, "temperature_unit": "°F"}})
	}))
	defer mock.Close()
	c := homeTestConfig(mock.URL)
	u := NewUpstreams(c)
	seedHome(u, "climate.test", "cool", map[string]any{"supported_features": float64(1), "temperature_unit": "°F", "min_temp": float64(60), "max_temp": float64(90), "target_temp_step": float64(1)})
	h := NewHomeCommands(c, u)
	target := 25.0
	_, _, code := h.Submit(homeBody(h, 1, "climate.test", "set_temperature", time.Now().Add(time.Second), &target), time.Now())
	if code != "" {
		t.Fatal(code)
	}
	id := <-h.queue
	h.execute(context.Background(), id)
	homeStatus(t, h, id, "confirmed")
	if sentTemperature != 77 {
		t.Fatal("Celsius command sent as Fahrenheit without conversion")
	}
	if h.confirmed(HomeRequest{EntityID: "cover.test", Action: "open_cover"}, observation{entity: entityState{State: "open", Attributes: map[string]any{"current_position": float64(50)}}}) {
		t.Fatal("partial opening reported as fully open")
	}
}

func TestHomeStopInterruptsPendingMovement(t *testing.T) {
	var mu sync.Mutex
	state := "closed"
	position := 0.0
	calls := []string{}
	mock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		if r.Method == "POST" {
			action := strings.TrimPrefix(r.URL.Path, "/api/services/cover/")
			calls = append(calls, action)
			if action == "open_cover" {
				state = "opening"
				position = 40
			}
			if action == "stop_cover" {
				state = "open"
				position = 40
			}
			w.Write([]byte("[]"))
			return
		}
		json.NewEncoder(w).Encode(map[string]any{"state": state, "last_updated": time.Now().UTC().Format(time.RFC3339Nano), "attributes": map[string]any{"supported_features": 11, "current_position": position}})
	}))
	defer mock.Close()
	c := homeTestConfig(mock.URL)
	u := NewUpstreams(c)
	seedHome(u, "cover.test", "closed", map[string]any{"supported_features": float64(11), "current_position": float64(0)})
	h := NewHomeCommands(c, u)
	h.Submit(homeBody(h, 1, "cover.test", "open_cover", time.Now().Add(5*time.Second), nil), time.Now())
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { h.Run(ctx); close(done) }()
	defer func() { cancel(); <-done }()
	until := time.Now().Add(time.Second)
	for time.Now().Before(until) {
		mu.Lock()
		sent := len(calls) > 0
		mu.Unlock()
		if sent {
			break
		}
		time.Sleep(time.Millisecond)
	}
	_, status, code := h.Submit(homeBody(h, 2, "cover.test", "stop_cover", time.Now().Add(time.Second), nil), time.Now())
	if status != 202 {
		t.Fatalf("stop blocked by movement: %d %s", status, code)
	}
	until = time.Now().Add(time.Second)
	confirmed := false
	for time.Now().Before(until) {
		if result, _ := h.Get("00000000-0000-4000-8000-000000000002"); result["status"] == "confirmed" {
			confirmed = true
			break
		}
		time.Sleep(time.Millisecond)
	}
	if !confirmed {
		t.Fatal("stop waited for movement confirmation timeout")
	}
	homeStatus(t, h, "00000000-0000-4000-8000-000000000001", "superseded")
	mu.Lock()
	defer mu.Unlock()
	if len(calls) != 2 || calls[0] != "open_cover" || calls[1] != "stop_cover" {
		t.Fatalf("unexpected service ordering: %v", calls)
	}
}

func TestFanCapabilities(t *testing.T) {
	c := homeTestConfig("http://127.0.0.1:1")
	c.HA.Home = []HomeBinding{{EntityID: "fan.test", Name: "Fan"}}
	if validateUpstreams(c) != nil {
		t.Fatal("fan binding rejected")
	}
	u := NewUpstreams(c)
	seedHome(u, "fan.test", "off", map[string]any{"supported_features": float64(59)})
	a := homeActions("fan.test", u.items["home:fan.test"], time.Now(), "")
	if len(a) != 3 || a[0] != "turn_on" || a[1] != "turn_off" || a[2] != "set_percentage" {
		t.Fatal("fan power capabilities mismatch", a)
	}
	if contracts.Load("snapshot").Validate(u.Snapshot(time.Now())) != nil {
		t.Fatal("fan snapshot contract invalid")
	}
	seedHome(u, "fan.test", "off", map[string]any{"supported_features": float64(1)})
	if actions := homeActions("fan.test", u.items["home:fan.test"], time.Now(), ""); len(actions) != 1 || actions[0] != "set_percentage" {
		t.Fatal("unsupported fan power action enabled")
	}
}
