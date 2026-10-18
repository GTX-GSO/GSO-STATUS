package main

// CAR: vehicle state from the Home Assistant Leapmotor integration, and three opt-in actions (lock, unlock, press the
// "refresh vehicle state" button). Reads share the HA subscription; actions go through HomeCommands with the HOME control
// token, and are confirmed by a later HA state report like every other command.
import (
	"context"
	"fmt"
	"math"
	"net/url"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"gso-status/contracts"
)

type CarConfig struct {
	Name     string            `json:"name,omitempty"`
	Entities map[string]string `json:"entities"`
	Actions  []string          `json:"actions,omitempty"`
}

// Role -> required HA domain.
var carDomains = map[string]string{"state": "sensor", "battery": "sensor", "range": "sensor", "charge": "sensor", "updated": "sensor", "cabin": "sensor", "odometer": "sensor", "week": "sensor", "consumption": "sensor", "tyre_fl": "sensor", "tyre_fr": "sensor", "tyre_rl": "sensor", "tyre_rr": "sensor", "lock": "lock", "refresh": "button"}

// Action -> role whose entity it operates.
var carActionRole = map[string]string{"lock": "lock", "unlock": "lock", "press": "refresh"}

func validateCar(c Config) error {
	car := c.HA.Car
	if car == nil {
		return nil
	}
	if c.HA.BaseURL == "" || len(car.Entities) == 0 || utf8.RuneCountInString(car.Name) > 160 {
		return fmt.Errorf("invalid car configuration")
	}
	for role, id := range car.Entities {
		if domain, ok := carDomains[role]; !ok || !entityPattern.MatchString(id) || len(id) > 160 || homeDomain(id) != domain {
			return fmt.Errorf("invalid car entity binding")
		}
	}
	seen := map[string]bool{}
	for _, action := range car.Actions {
		if role, ok := carActionRole[action]; !ok || seen[action] || car.Entities[role] == "" {
			return fmt.Errorf("invalid car action")
		}
		seen[action] = true
	}
	return nil
}

// carRole names the controllable role (lock or refresh) a bound entity plays.
func (u *Upstreams) carRole(id string) (string, bool) {
	if car := u.config.HA.Car; car != nil {
		for _, role := range []string{"lock", "refresh"} {
			if car.Entities[role] == id {
				return role, true
			}
		}
	}
	return "", false
}

// fetchCarRole reads one car entity over REST: the fallback while the subscription is down, and the confirmation read.
func (u *Upstreams) fetchCarRole(ctx context.Context, role string) observation {
	key := "car:" + role
	started := time.Now()
	var entity entityState
	code := u.fetch(ctx, strings.TrimRight(u.config.HA.BaseURL, "/")+"/api/states/"+url.PathEscape(u.config.HA.Car.Entities[role]), u.config.HA.Token, &entity)
	if code == "" {
		if _, e := time.Parse(time.RFC3339Nano, entity.LastUpdated); e != nil || entity.State == "" || utf8.RuneCountInString(entity.State) > 160 {
			code = "entity_state_invalid"
		}
	}
	u.mu.Lock()
	defer u.mu.Unlock()
	old := u.items[key]
	if code == "" && old.entity.LastUpdated != "" {
		previous, e1 := time.Parse(time.RFC3339Nano, old.entity.LastUpdated)
		next, e2 := time.Parse(time.RFC3339Nano, entity.LastUpdated)
		if e1 == nil && e2 == nil && next.Before(previous) {
			if old.code == "" && (old.at.After(started) || (u.streamReady && u.streamKnown[key] && time.Since(u.streamVerified) <= 40*time.Second)) {
				return u.liveObservation(key, time.Now())
			}
			code = "entity_state_regressed"
		}
	}
	if code == "" {
		old.entity = entity
		old.at = time.Now()
	}
	old.code = code
	u.items[key] = old
	return old
}

func (u *Upstreams) pollCar(ctx context.Context) {
	if u.config.HA.Car == nil {
		return
	}
	for role := range u.config.HA.Car.Entities {
		if u.streamHealthy() || ctx.Err() != nil {
			return
		}
		u.fetchCarRole(ctx, role)
	}
}

// carEnum returns the entity state when it is one of the allowed values and fresh, else "unknown".
func (u *Upstreams) carEnum(role string, now time.Time, allowed ...string) string {
	if u.config.HA.Car.Entities[role] == "" {
		return "unknown"
	}
	o := u.liveObservation("car:"+role, now)
	if sourceFrom(o, true, now)["quality"] != "ok" {
		return "unknown"
	}
	for _, state := range allowed {
		if o.entity.State == state {
			return state
		}
	}
	return "unknown"
}
func (u *Upstreams) carMetric(role string, now time.Time, min, max float64) contracts.Object {
	if u.config.HA.Car.Entities[role] == "" {
		return missing("not_configured")
	}
	o := u.liveObservation("car:"+role, now)
	if sourceFrom(o, true, now)["quality"] != "ok" {
		return missing("source_unavailable")
	}
	n, err := strconv.ParseFloat(o.entity.State, 64)
	if err != nil || math.IsNaN(n) || math.IsInf(n, 0) || n < min || n > max {
		return missing("state_not_numeric")
	}
	return value(n)
}

// Caller holds u.mu. Actions follow the observed state: a locked car offers unlock, an unlocked one offers lock.
func (u *Upstreams) carActions(now time.Time, lockState string) []any {
	actions := []any{}
	car := u.config.HA.Car
	if car == nil || u.config.HomeControlToken == "" {
		return actions
	}
	for _, action := range []string{"lock", "unlock", "press"} {
		listed := false
		for _, configured := range car.Actions {
			listed = listed || configured == action
		}
		if !listed {
			continue
		}
		switch action {
		case "lock":
			if lockState != "unlocked" {
				continue
			}
		case "unlock":
			if lockState != "locked" {
				continue
			}
		case "press":
			// A button reports "unknown" until it was pressed once; only a missing or stale report rules it out.
			o := u.liveObservation("car:refresh", now)
			if o.at.IsZero() || o.code != "" || now.Sub(o.at) > 120*time.Second || o.entity.State == "unavailable" {
				continue
			}
		}
		actions = append(actions, action)
	}
	return actions
}

// Caller holds u.mu.
func (u *Upstreams) carSnapshot(now time.Time) contracts.Object {
	out := contracts.Object{
		"source": sourceFrom(observation{}, false, now), "name": nil, "vehicle_state": "unknown", "charge_state": "unknown", "lock_state": "unknown",
		"battery_percent": missing("not_configured"), "range_km": missing("not_configured"), "remaining_charge_minutes": missing("not_configured"),
		"cabin_temperature_c": missing("not_configured"), "odometer_km": missing("not_configured"), "week_distance_km": missing("not_configured"),
		"energy_kwh_per_100km": missing("not_configured"), "tyre_fl_bar": missing("not_configured"), "tyre_fr_bar": missing("not_configured"),
		"tyre_rl_bar": missing("not_configured"), "tyre_rr_bar": missing("not_configured"), "tyre_alarm": "unknown", "windows_state": "unknown", "trunk_state": "unknown",
		"charging_power_kw": missing("not_configured"), "updated_at": nil, "lock_entity_id": nil, "refresh_entity_id": nil, "actions": []any{},
	}
	car := u.config.HA.Car
	if car == nil {
		return out
	}
	if name := cleanName(car.Name); name != "" {
		out["name"] = name
	}
	first := true
	for _, role := range []string{"state", "battery", "range", "charge"} {
		if car.Entities[role] == "" {
			continue
		}
		source := sourceFrom(u.liveObservation("car:"+role, now), true, now)
		if first || source["quality"] != "ok" {
			out["source"] = source
		}
		first = false
	}
	out["vehicle_state"] = u.carEnum("state", now, "driving", "parked")
	out["charge_state"] = u.carEnum("charge", now, "charging", "plugged", "unplugged")
	out["lock_state"] = u.carEnum("lock", now, "locked", "unlocked", "locking", "unlocking", "jammed")
	out["battery_percent"] = u.carMetric("battery", now, 0, 100)
	out["range_km"] = u.carMetric("range", now, 0, 5000)
	out["odometer_km"] = u.carMetric("odometer", now, 0, 9000000)
	out["week_distance_km"] = u.carMetric("week", now, 0, 100000)
	out["energy_kwh_per_100km"] = u.carMetric("consumption", now, 0, 1000)
	// The sensor reports its own unit; anything but Celsius or Fahrenheit is not trusted.
	out["cabin_temperature_c"] = missing("not_configured")
	if car.Entities["cabin"] != "" {
		out["cabin_temperature_c"] = missing("source_unavailable")
		o := u.liveObservation("car:cabin", now)
		if n, err := strconv.ParseFloat(o.entity.State, 64); err == nil && sourceFrom(o, true, now)["quality"] == "ok" {
			unit, _ := o.entity.Attributes["unit_of_measurement"].(string)
			if unit == "" {
				unit = "°C"
			}
			if c, ok := toC(n, unit); ok && c >= -100 && c <= 200 {
				out["cabin_temperature_c"] = value(c)
			} else {
				out["cabin_temperature_c"] = missing("temperature_unit_or_value_invalid")
			}
		}
	}
	out["remaining_charge_minutes"] = missing("not_charging")
	if out["charge_state"] == "charging" {
		o := u.liveObservation("car:charge", now)
		if n, ok := o.entity.Attributes["remaining_minutes"].(float64); ok && n >= 0 && n <= 100000 && !math.IsNaN(n) {
			out["remaining_charge_minutes"] = value(n)
		} else {
			out["remaining_charge_minutes"] = missing("remaining_unavailable")
		}
	}
	u.carTyres(car, now, out)
	u.carBody(car, now, out)
	if car.Entities["updated"] != "" {
		if o := u.liveObservation("car:updated", now); sourceFrom(o, true, now)["quality"] == "ok" {
			if _, e := time.Parse(time.RFC3339Nano, o.entity.State); e == nil {
				out["updated_at"] = o.entity.State
			}
		}
	}
	if u.config.HomeControlToken != "" {
		if id := car.Entities["lock"]; id != "" {
			out["lock_entity_id"] = id
		}
		if id := car.Entities["refresh"]; id != "" {
			out["refresh_entity_id"] = id
		}
		out["actions"] = u.carActions(now, out["lock_state"].(string))
	}
	return out
}

// carActionAllowed reports whether the action may be sent now to this entity.
func (u *Upstreams) carActionAllowed(entityID, action string, now time.Time) bool {
	u.mu.Lock()
	defer u.mu.Unlock()
	car := u.config.HA.Car
	role, ok := carActionRole[action]
	if car == nil || !ok || car.Entities[role] != entityID {
		return false
	}
	for _, listed := range u.carActions(now, u.carEnum("lock", now, "locked", "unlocked", "locking", "unlocking", "jammed")) {
		if listed == action {
			return true
		}
	}
	return false
}

// carConfirmed: lock and unlock are confirmed by the lock state; a button press by a press time after the request.
func carConfirmed(r HomeRequest, o observation, created time.Time) bool {
	switch r.Action {
	case "lock":
		return o.entity.State == "locked"
	case "unlock":
		return o.entity.State == "unlocked"
	case "press":
		pressed, e := time.Parse(time.RFC3339Nano, o.entity.State)
		return e == nil && pressed.After(created.Add(-5*time.Second))
	}
	return false
}
