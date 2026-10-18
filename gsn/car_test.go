package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"gso-status/contracts"
)

func carTestConfig(base string) Config {
	c := config()
	c.HomeControlToken = strings.Repeat("c", 32)
	c.HA = HAConfig{BaseURL: base, Token: "test-ha-token", Car: &CarConfig{Name: "Test car", Actions: []string{"lock", "unlock", "press"}, Entities: map[string]string{
		"state": "sensor.car_state", "battery": "sensor.car_battery", "range": "sensor.car_range", "charge": "sensor.car_charge", "updated": "sensor.car_updated",
		"cabin": "sensor.car_cabin", "odometer": "sensor.car_odometer", "week": "sensor.car_week", "consumption": "sensor.car_consumption", "tyre_fl": "sensor.car_tyre_fl", "tyre_fr": "sensor.car_tyre_fr", "tyre_rl": "sensor.car_tyre_rl", "tyre_rr": "sensor.car_tyre_rr", "lock": "lock.car_door", "refresh": "button.car_refresh"}}}
	return c
}
func seedCar(u *Upstreams, role, state string, attrs map[string]any) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.items["car:"+role] = observation{entity: entityState{State: state, LastUpdated: time.Now().UTC().Format(time.RFC3339Nano), Attributes: attrs}, at: time.Now()}
}
func seedCarAll(u *Upstreams, lock, charge string) {
	seedCar(u, "state", "parked", map[string]any{"windows_open": false, "trunk_open": false})
	seedCar(u, "battery", "76", map[string]any{"charging_voltage": float64(400), "charging_current": float64(17.5)})
	seedCar(u, "tyre_fl", "2.33", map[string]any{"alarm": false})
	seedCar(u, "tyre_fr", "235", map[string]any{"unit_of_measurement": "kPa", "alarm": false})
	seedCar(u, "tyre_rl", "2.31", nil)
	seedCar(u, "tyre_rr", "2.34", map[string]any{"alarm": false})
	seedCar(u, "range", "403.5", nil)
	seedCar(u, "charge", charge, map[string]any{"remaining_minutes": float64(95)})
	seedCar(u, "updated", "2026-10-17T13:20:00+00:00", nil)
	seedCar(u, "cabin", "28", map[string]any{"unit_of_measurement": "°C"})
	seedCar(u, "odometer", "20610", nil)
	seedCar(u, "week", "132.4", nil)
	seedCar(u, "consumption", "14.8", nil)
	seedCar(u, "lock", lock, nil)
	seedCar(u, "refresh", "unknown", nil)
}
func TestCarSnapshotValuesAndActions(t *testing.T) {
	c := carTestConfig("http://127.0.0.1:1")
	u := NewUpstreams(c)
	if car := object(u.Snapshot(time.Now()), "car"); car["lock_state"] != "unknown" || car["name"] != "Test car" {
		t.Fatalf("unseeded car: %#v", car)
	}
	seedCarAll(u, "locked", "charging")
	m := u.Snapshot(time.Now())
	if e := contracts.Load("snapshot").Validate(m); e != nil {
		t.Fatal(e)
	}
	car := object(m, "car")
	if car["vehicle_state"] != "parked" || car["charge_state"] != "charging" || car["lock_state"] != "locked" || object(car, "source")["quality"] != "ok" {
		t.Fatalf("states: %#v", car)
	}
	if number(object(car, "battery_percent")["value"]) != 76 || number(object(car, "range_km")["value"]) != 403.5 || number(object(car, "remaining_charge_minutes")["value"]) != 95 || number(object(car, "cabin_temperature_c")["value"]) != 28 || number(object(car, "odometer_km")["value"]) != 20610 || number(object(car, "week_distance_km")["value"]) != 132.4 || number(object(car, "energy_kwh_per_100km")["value"]) != 14.8 {
		t.Fatalf("metrics: %#v", car)
	}
	if number(object(car, "tyre_fl_bar")["value"]) != 2.33 || number(object(car, "tyre_fr_bar")["value"]) != 2.35 || number(object(car, "tyre_rl_bar")["value"]) != 2.31 || number(object(car, "tyre_rr_bar")["value"]) != 2.34 || car["tyre_alarm"] != "ok" {
		t.Fatalf("tyres: %#v", car)
	}
	if car["windows_state"] != "closed" || car["trunk_state"] != "closed" || number(object(car, "charging_power_kw")["value"]) != 7 {
		t.Fatalf("body or charging power: %#v", car)
	}
	if car["updated_at"] != "2026-10-17T13:20:00+00:00" || car["lock_entity_id"] != "lock.car_door" || car["refresh_entity_id"] != "button.car_refresh" {
		t.Fatalf("identity: %#v", car)
	}
	if actions := car["actions"].([]any); len(actions) != 2 || actions[0] != "unlock" || actions[1] != "press" {
		t.Fatalf("a locked car must offer unlock and refresh, got %#v", actions)
	}
	seedCar(u, "state", "parked", map[string]any{"windows_open": true, "trunk_open": true})
	seedCar(u, "tyre_rr", "2.0", map[string]any{"alarm": true})
	seedCar(u, "battery", "76", map[string]any{"charging_voltage": float64(400), "charging_current": float64(-10)})
	car = object(u.Snapshot(time.Now()), "car")
	if car["windows_state"] != "open" || car["trunk_state"] != "open" || car["tyre_alarm"] != "alarm" || number(object(car, "charging_power_kw")["value"]) != 4 {
		t.Fatalf("open state, tyre alarm or negative current: %#v", car)
	}
	seedCar(u, "state", "parked", nil)
	seedCar(u, "tyre_fl", "9000", nil)
	seedCar(u, "battery", "76", nil)
	car = object(u.Snapshot(time.Now()), "car")
	if car["windows_state"] != "unknown" || object(car, "tyre_fl_bar")["value"] != nil || object(car, "charging_power_kw")["value"] != nil || object(car, "charging_power_kw")["reason"] != "power_unavailable" {
		t.Fatalf("missing attributes must stay unknown: %#v", car)
	}
	seedCarAll(u, "unlocked", "unplugged")
	car = object(u.Snapshot(time.Now()), "car")
	if object(car, "charging_power_kw")["reason"] != "not_charging" {
		t.Fatalf("charging power shown while not charging: %#v", car)
	}
	if actions := car["actions"].([]any); len(actions) != 2 || actions[0] != "lock" {
		t.Fatalf("an unlocked car must offer lock, got %#v", actions)
	}
	if object(car, "remaining_charge_minutes")["value"] != nil {
		t.Fatal("remaining charge time shown while not charging")
	}
	seedCarAll(u, "jammed", "plugged")
	if actions := object(u.Snapshot(time.Now()), "car")["actions"].([]any); len(actions) != 1 || actions[0] != "press" {
		t.Fatalf("a jammed lock must not be commanded, got %#v", actions)
	}
	seedCar(u, "battery", "unavailable", nil)
	seedCar(u, "cabin", "82.4", map[string]any{"unit_of_measurement": "°F"})
	car = object(u.Snapshot(time.Now()), "car")
	if object(car, "battery_percent")["value"] != nil || object(car, "source")["quality"] == "ok" {
		t.Fatal("unavailable battery reported as live")
	}
	if got := number(object(car, "cabin_temperature_c")["value"]); got < 27.9 || got > 28.1 {
		t.Fatalf("Fahrenheit not converted: %v", got)
	}
	seedCar(u, "cabin", "-5", nil)
	if number(object(object(u.Snapshot(time.Now()), "car"), "cabin_temperature_c")["value"]) != -5 {
		t.Fatal("negative cabin temperature lost")
	}
	c.HomeControlToken = ""
	u = NewUpstreams(c)
	seedCarAll(u, "locked", "plugged")
	car = object(u.Snapshot(time.Now()), "car")
	if len(car["actions"].([]any)) != 0 || car["lock_entity_id"] != nil {
		t.Fatal("car controls exposed without a control token")
	}
	c = carTestConfig("")
	c.HA.Car = nil
	if object(NewUpstreams(c).Snapshot(time.Now()), "car")["name"] != nil {
		t.Fatal("unconfigured car invented a name")
	}
}
func TestCarConfigValidation(t *testing.T) {
	good := carTestConfig("http://127.0.0.1:1")
	if validateCar(good) != nil {
		t.Fatal("valid car configuration rejected")
	}
	for name, mutate := range map[string]func(*CarConfig){
		"unknown role":    func(c *CarConfig) { c.Entities["gps"] = "sensor.car_gps" },
		"wrong domain":    func(c *CarConfig) { c.Entities["lock"] = "switch.car_door" },
		"bad entity id":   func(c *CarConfig) { c.Entities["battery"] = "Sensor Battery" },
		"empty":           func(c *CarConfig) { c.Entities = map[string]string{} },
		"unknown action":  func(c *CarConfig) { c.Actions = []string{"trunk"} },
		"duplicate":       func(c *CarConfig) { c.Actions = []string{"lock", "lock"} },
		"action w/o role": func(c *CarConfig) { delete(c.Entities, "refresh") },
	} {
		c := carTestConfig("http://127.0.0.1:1")
		mutate(c.HA.Car)
		if validateCar(c) == nil {
			t.Fatalf("%s accepted", name)
		}
	}
	noHA := carTestConfig("")
	if validateCar(noHA) == nil {
		t.Fatal("car without a Home Assistant address accepted")
	}
}
func TestCarCommandsAreConfirmedByObservedState(t *testing.T) {
	var mu sync.Mutex
	lockState := "locked"
	pressed := "unknown"
	var posts []string
	mock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		if r.Method == "POST" {
			posts = append(posts, r.URL.Path)
			switch r.URL.Path {
			case "/api/services/lock/unlock":
				lockState = "unlocked"
			case "/api/services/button/press":
				pressed = time.Now().UTC().Format(time.RFC3339Nano)
			}
			w.Write([]byte("[]"))
			return
		}
		state := lockState
		if strings.HasSuffix(r.URL.Path, "car_refresh") {
			state = pressed
		}
		json.NewEncoder(w).Encode(map[string]any{"state": state, "last_updated": time.Now().UTC().Format(time.RFC3339Nano), "attributes": map[string]any{}})
	}))
	defer mock.Close()
	c := carTestConfig(mock.URL)
	u := NewUpstreams(c)
	seedCarAll(u, "locked", "plugged")
	h := NewHomeCommands(c, u)
	run := func(n int, entity, action string) string {
		t.Helper()
		_, _, code := h.Submit(homeBody(h, n, entity, action, time.Now().Add(3*time.Second), nil), time.Now())
		if code != "" {
			return code
		}
		id := <-h.queue
		h.execute(context.Background(), id)
		v, _ := h.Get(id)
		if e := contracts.Load("home-result").Validate(v); e != nil {
			t.Fatal(e)
		}
		return v["status"].(string)
	}
	if code := run(1, "lock.car_door", "lock"); code != "action_not_supported_or_source_stale" {
		t.Fatalf("locking a locked car: %s", code)
	}
	if status := run(2, "lock.car_door", "unlock"); status != "confirmed" {
		t.Fatalf("unlock: %s", status)
	}
	if status := run(3, "button.car_refresh", "press"); status != "confirmed" {
		t.Fatalf("refresh: %s", status)
	}
	mu.Lock()
	if len(posts) != 2 || posts[0] != "/api/services/lock/unlock" || posts[1] != "/api/services/button/press" {
		t.Fatalf("service calls: %v", posts)
	}
	mu.Unlock()
	temp := 20.0
	seedCarAll(u, "locked", "plugged")
	if _, _, code := h.Submit(homeBody(h, 4, "lock.car_door", "unlock", time.Now().Add(3*time.Second), &temp), time.Now()); code != "unexpected_or_missing_parameter" {
		t.Fatalf("parameter on a lock command: %s", code)
	}
	if code := run(5, "lock.car_door", "turn_on"); code == "confirmed" || code == "queued" {
		t.Fatalf("home action accepted for a car lock: %s", code)
	}
	// A lock that never reports the requested state must time out, not confirm.
	mu.Lock()
	lockState = "locked"
	mu.Unlock()
	c.HA.Car.Actions = []string{"lock", "unlock", "press"}
	seedCarAll(u, "locked", "plugged")
	mock2 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "POST" {
			w.Write([]byte("[]"))
			return
		}
		json.NewEncoder(w).Encode(map[string]any{"state": "locked", "last_updated": time.Now().UTC().Format(time.RFC3339Nano), "attributes": map[string]any{}})
	}))
	defer mock2.Close()
	c2 := carTestConfig(mock2.URL)
	u2 := NewUpstreams(c2)
	seedCarAll(u2, "locked", "plugged")
	h = NewHomeCommands(c2, u2)
	_, _, code := h.Submit(homeBody(h, 6, "lock.car_door", "unlock", time.Now().Add(300*time.Millisecond), nil), time.Now())
	if code != "" {
		t.Fatal(code)
	}
	id := <-h.queue
	h.execute(context.Background(), id)
	homeStatus(t, h, id, "timed_out")
}
func TestCarServedInSnapshotAndUsesSubscriptionKeys(t *testing.T) {
	c := carTestConfig("http://127.0.0.1:1")
	u := NewUpstreams(c)
	bindings := u.haBindings()
	if len(bindings["lock.car_door"]) != 1 || bindings["lock.car_door"][0] != "car:lock" || bindings["sensor.car_charge"][0] != "car:charge" {
		t.Fatalf("subscription bindings: %v", bindings)
	}
	if _, ok := haAttributes(map[string]any{"remaining_minutes": float64(12), "vin": "secret"}); !ok {
		t.Fatal("charge attribute rejected")
	}
	attributes, _ := haAttributes(map[string]any{"remaining_minutes": float64(12), "vin": "secret", "windows_open": true})
	if attributes["remaining_minutes"] != float64(12) || attributes["windows_open"] != true || attributes["vin"] != nil {
		t.Fatalf("only the charge time may pass the attribute filter: %v", attributes)
	}
	s := NewServer(c)
	seedCarAll(s.upstreams, "locked", "charging")
	s.mu.Lock()
	s.refresh(time.Now())
	payload := string(s.payload)
	s.mu.Unlock()
	if !strings.Contains(payload, `"lock_state":"locked"`) || !strings.Contains(payload, `"remaining_charge_minutes":{"reason":null,"value":95}`) {
		t.Fatalf("car missing from the served snapshot: %.400s", payload)
	}
}

func TestCarExampleConfigurationIsValid(t *testing.T) {
	raw, err := os.ReadFile("../config/gsn-ha.example.json")
	if err != nil {
		t.Fatal(err)
	}
	var c Config
	if err := json.Unmarshal(raw, &c); err != nil {
		t.Fatal(err)
	}
	if c.HA.Car == nil || validateCar(c) != nil || len(c.HA.Car.Actions) != 0 {
		t.Fatal("example car block must be valid and ship with actions disabled")
	}
	compact, _ := json.Marshal(c.HA.Car)
	if len(compact) > 900 {
		t.Fatalf("car block of %d bytes leaves too little of the 8192-byte configuration", len(compact))
	}
}
