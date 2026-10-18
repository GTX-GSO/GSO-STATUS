package main

// CAR read-only extras: tyre pressures, window/trunk state and charging power. Everything comes from the entities already
// bound for the card; nothing here can send a command.
import (
	"math"
	"strconv"
	"time"

	"gso-status/contracts"
)

var carTyreRoles = [4][2]string{{"tyre_fl", "tyre_fl_bar"}, {"tyre_fr", "tyre_fr_bar"}, {"tyre_rl", "tyre_rl_bar"}, {"tyre_rr", "tyre_rr_bar"}}

// tyreBar converts a tyre pressure sensor reading to bar. A missing unit means bar (what the integration reports).
func tyreBar(n float64, unit string) (float64, bool) {
	switch unit {
	case "", "bar":
	case "kPa", "kpa":
		n /= 100
	case "MPa", "mpa":
		n *= 10
	case "psi", "PSI":
		n /= 14.5038
	default:
		return 0, false
	}
	return n, !math.IsNaN(n) && !math.IsInf(n, 0) && n >= 0 && n <= 10
}

// Caller holds u.mu. tyre_alarm is "alarm" when any fresh tyre reports an alarm, "ok" when at least one reports none and
// no tyre alarms, otherwise "unknown" (the alarm attribute is optional in the integration).
func (u *Upstreams) carTyres(car *CarConfig, now time.Time, out contracts.Object) {
	known, alarm := false, false
	for _, tyre := range carTyreRoles {
		if car.Entities[tyre[0]] == "" {
			continue
		}
		out[tyre[1]] = missing("source_unavailable")
		o := u.liveObservation("car:"+tyre[0], now)
		if sourceFrom(o, true, now)["quality"] != "ok" {
			continue
		}
		n, err := strconv.ParseFloat(o.entity.State, 64)
		unit, _ := o.entity.Attributes["unit_of_measurement"].(string)
		bar, ok := tyreBar(n, unit)
		if err != nil || !ok {
			out[tyre[1]] = missing("state_not_numeric")
			continue
		}
		out[tyre[1]] = value(math.Round(bar*100) / 100)
		if flag, ok := o.entity.Attributes["alarm"].(bool); ok {
			known = true
			alarm = alarm || flag
		}
	}
	switch {
	case alarm:
		out["tyre_alarm"] = "alarm"
	case known:
		out["tyre_alarm"] = "ok"
	}
}

// carOpenState maps a boolean attribute of the vehicle state sensor to closed/open.
func carOpenState(o observation, now time.Time, key string) string {
	if sourceFrom(o, true, now)["quality"] != "ok" {
		return "unknown"
	}
	open, ok := o.entity.Attributes[key].(bool)
	switch {
	case !ok:
		return "unknown"
	case open:
		return "open"
	}
	return "closed"
}

// Caller holds u.mu. Charging power is the battery sensor's charging voltage times charging current, only while charging.
func (u *Upstreams) carBody(car *CarConfig, now time.Time, out contracts.Object) {
	if car.Entities["state"] != "" {
		o := u.liveObservation("car:state", now)
		out["windows_state"] = carOpenState(o, now, "windows_open")
		out["trunk_state"] = carOpenState(o, now, "trunk_open")
	}
	if car.Entities["battery"] == "" {
		return
	}
	out["charging_power_kw"] = missing("not_charging")
	if out["charge_state"] != "charging" {
		return
	}
	o := u.liveObservation("car:battery", now)
	volts, okV := o.entity.Attributes["charging_voltage"].(float64)
	amps, okA := o.entity.Attributes["charging_current"].(float64)
	if sourceFrom(o, true, now)["quality"] != "ok" || !okV || !okA || math.IsNaN(volts) || math.IsNaN(amps) || math.IsInf(volts, 0) || math.IsInf(amps, 0) {
		out["charging_power_kw"] = missing("power_unavailable")
		return
	}
	// The current's sign is not specified by the integration, so only its magnitude counts.
	kw := volts * math.Abs(amps) / 1000
	if volts < 0 || kw > 500 {
		out["charging_power_kw"] = missing("power_out_of_range")
		return
	}
	out["charging_power_kw"] = value(math.Round(kw*10) / 10)
}
