package main

import (
	"context"
	"gso-status/contracts"
	"math"
	"net/url"
	"strings"
	"time"
	"unicode/utf8"
)

func bindingIDs(b HomeBinding) []string {
	if len(b.Members) > 0 {
		return b.Members
	}
	return []string{b.EntityID}
}

// Caller holds u.mu; aggregate from both independent observations, never one-sided success.
func (u *Upstreams) bindingObservation(b HomeBinding, now time.Time) observation {
	if len(b.Members) == 0 {
		return u.liveObservation("home:"+b.EntityID, now)
	}
	result := observation{entity: entityState{State: "closed", Attributes: map[string]any{}}, at: now}
	features := int64(15)
	position := 0.0
	positions := true
	first := ""
	mixed := false
	for _, id := range b.Members {
		o := u.liveObservation("home:"+id, now)
		if sourceFrom(o, true, now)["quality"] != "ok" {
			result.code = "group_member_unavailable"
		}
		if o.at.IsZero() || o.at.Before(result.at) {
			result.at = o.at
		}
		if o.entity.LastUpdated > result.entity.LastUpdated {
			result.entity.LastUpdated = o.entity.LastUpdated
		}
		if first == "" {
			first = o.entity.State
		} else if first != o.entity.State {
			mixed = true
		}
		f, _ := o.entity.Attributes["supported_features"].(float64)
		features &= int64(f)
		if n, ok := o.entity.Attributes["current_position"].(float64); ok {
			position += n
		} else {
			positions = false
		}
	}
	result.entity.State = first
	if mixed {
		result.entity.State = "mixed"
	}
	result.entity.Attributes["supported_features"] = float64(features)
	if positions {
		result.entity.Attributes["current_position"] = position / float64(len(b.Members))
	}
	return result
}
func (u *Upstreams) fetchHomeExtra(ctx context.Context, id string) {
	var entity entityState
	code := u.fetch(ctx, strings.TrimRight(u.config.HA.BaseURL, "/")+"/api/states/"+url.PathEscape(id), u.config.HA.Token, &entity)
	if code == "" {
		if _, e := time.Parse(time.RFC3339Nano, entity.LastUpdated); e != nil || utf8.RuneCountInString(entity.State) > 160 {
			code = "entity_state_invalid"
		}
	}
	u.mu.Lock()
	defer u.mu.Unlock()
	old := u.items["home-extra:"+id]
	if code == "" {
		old = observation{entity: entity, at: time.Now()}
	} else {
		old.code = code
	}
	u.items["home-extra:"+id] = old
}
func homeModes(id string, o observation) ([]any, string) {
	key, current := "", ""
	switch homeDomain(id) {
	case "climate":
		key = "hvac_modes"
		current = o.entity.State
	case "fan":
		key = "preset_modes"
		current, _ = o.entity.Attributes["preset_mode"].(string)
	case "humidifier":
		key = "available_modes"
		current, _ = o.entity.Attributes["mode"].(string)
	}
	list := []any{}
	if raw, ok := o.entity.Attributes[key].([]any); ok {
		for _, v := range raw {
			if s, ok := v.(string); ok && s != "" && utf8.RuneCountInString(s) <= 16 && !strings.ContainsAny(s, "\r\n") && len(list) < 8 {
				list = append(list, s)
			}
		}
	}
	return list, current
}
func hasMode(id string, o observation, mode string) bool {
	list, _ := homeModes(id, o)
	for _, item := range list {
		if item == mode {
			return true
		}
	}
	return false
}
func attributeNumber(o observation, key string, min, max float64) contracts.Object {
	if n, ok := o.entity.Attributes[key].(float64); ok && n >= min && n <= max && !math.IsNaN(n) && !math.IsInf(n, 0) {
		return value(n)
	}
	return missing("attribute_unavailable")
}
func lightColor(o observation) bool {
	if list, ok := o.entity.Attributes["supported_color_modes"].([]any); ok {
		for _, item := range list {
			if item == "color_temp" {
				return true
			}
		}
	}
	return false
}
func lightBrightness(o observation) bool {
	if list, ok := o.entity.Attributes["supported_color_modes"].([]any); ok {
		for _, item := range list {
			if s, ok := item.(string); ok && s != "onoff" && s != "unknown" {
				return true
			}
		}
	}
	return false
}
func homeControlSnapshot(id string, o observation) contracts.Object {
	m := contracts.Object{}
	list, mode := homeModes(id, o)
	m["modes"] = list
	m["mode"] = nil
	if mode != "" && utf8.RuneCountInString(mode) <= 16 {
		m["mode"] = mode
	}
	if homeDomain(id) == "light" {
		if n := attributeNumber(o, "brightness", 0, 255); n["value"] != nil {
			m["brightness_percent"] = value(number(n["value"]) * 100 / 255)
		}
		for out, key := range map[string]string{"color_temperature_kelvin": "color_temp_kelvin", "min_color_temperature_kelvin": "min_color_temp_kelvin", "max_color_temperature_kelvin": "max_color_temp_kelvin"} {
			m[out] = attributeNumber(o, key, 1000, 20000)
		}
	}
	if homeDomain(id) == "fan" {
		m["percentage"] = attributeNumber(o, "percentage", 0, 100)
		m["percentage_step"] = attributeNumber(o, "percentage_step", 0.001, 100)
	}
	if homeDomain(id) == "humidifier" {
		m["humidity_percent"] = attributeNumber(o, "humidity", 0, 100)
		m["min_humidity_percent"] = attributeNumber(o, "min_humidity", 0, 100)
		m["max_humidity_percent"] = attributeNumber(o, "max_humidity", 0, 100)
	}
	return m
}
func extraHomeActions(id string, o observation) []any {
	actions := []any{}
	list, _ := homeModes(id, o)
	f, _ := o.entity.Attributes["supported_features"].(float64)
	bits := int64(f)
	switch homeDomain(id) {
	case "switch":
		actions = append(actions, "turn_on", "turn_off")
	case "humidifier":
		actions = append(actions, "turn_on", "turn_off")
		if bits&1 != 0 && len(list) > 0 {
			actions = append(actions, "set_mode")
		}
		if a, ok := o.entity.Attributes["min_humidity"].(float64); ok {
			if b, ok := o.entity.Attributes["max_humidity"].(float64); ok && a >= 0 && b <= 100 && a < b {
				actions = append(actions, "set_humidity")
			}
		}
	case "climate":
		if len(list) > 0 {
			actions = append(actions, "set_hvac_mode")
		}
	case "fan":
		if bits&1 != 0 {
			actions = append(actions, "set_percentage")
		}
		if bits&8 != 0 && len(list) > 0 {
			actions = append(actions, "set_preset_mode")
		}
	case "light":
		if lightBrightness(o) {
			actions = append(actions, "set_brightness")
		}
		if lightColor(o) {
			a := attributeNumber(o, "min_color_temp_kelvin", 1000, 20000)
			b := attributeNumber(o, "max_color_temp_kelvin", 1000, 20000)
			if a["value"] != nil && b["value"] != nil && number(a["value"]) <= number(b["value"]) {
				actions = append(actions, "set_color_temperature")
			}
		}
	}
	return actions
}
func homeService(action string) string {
	if action == "set_brightness" || action == "set_color_temperature" {
		return "turn_on"
	}
	return action
}
func planExtraHome(r HomeRequest, o observation, data map[string]any) (bool, string) {
	// Exactly one parameter family; reject hidden extra values even on simple switches.
	count := 0
	for _, v := range []*float64{r.Temperature, r.Brightness, r.Kelvin, r.Percentage, r.Humidity} {
		if v != nil {
			count++
		}
	}
	if r.Mode != nil {
		count++
	}
	want := 0
	switch r.Action {
	case "set_temperature", "set_brightness", "set_color_temperature", "set_percentage", "set_humidity", "set_hvac_mode", "set_preset_mode", "set_mode":
		want = 1
	}
	if count != want && r.Temperature != nil && want == 0 {
		return true, "unexpected_temperature"
	}
	if count != want {
		return true, "unexpected_or_missing_parameter"
	}
	switch r.Action {
	case "set_hvac_mode", "set_preset_mode", "set_mode":
		if r.Mode == nil || !hasMode(r.EntityID, o, *r.Mode) {
			return true, "mode_not_supported"
		}
		key := "mode"
		if r.Action == "set_hvac_mode" {
			key = "hvac_mode"
		}
		if r.Action == "set_preset_mode" {
			key = "preset_mode"
		}
		data[key] = *r.Mode
		return true, ""
	case "set_brightness":
		if r.Brightness == nil || *r.Brightness < 0 || *r.Brightness > 100 {
			return true, "brightness_out_of_range"
		}
		data["brightness_pct"] = *r.Brightness
		return true, ""
	case "set_color_temperature":
		if r.Kelvin == nil {
			return true, "color_temperature_required"
		}
		a := attributeNumber(o, "min_color_temp_kelvin", 1000, 20000)
		b := attributeNumber(o, "max_color_temp_kelvin", 1000, 20000)
		if a["value"] == nil || b["value"] == nil || *r.Kelvin < number(a["value"]) || *r.Kelvin > number(b["value"]) {
			return true, "color_temperature_out_of_range"
		}
		data["color_temp_kelvin"] = math.Round(*r.Kelvin)
		return true, ""
	case "set_percentage":
		if r.Percentage == nil || *r.Percentage < 0 || *r.Percentage > 100 {
			return true, "percentage_out_of_range"
		}
		if step, ok := o.entity.Attributes["percentage_step"].(float64); ok && step > 0 {
			n := *r.Percentage / step
			if math.Abs(*r.Percentage-math.Round(n)*step) > 1.01 {
				return true, "percentage_step_invalid"
			}
		}
		data["percentage"] = math.Round(*r.Percentage)
		return true, ""
	case "set_humidity":
		if r.Humidity == nil {
			return true, "humidity_required"
		}
		a := attributeNumber(o, "min_humidity", 0, 100)
		b := attributeNumber(o, "max_humidity", 0, 100)
		if a["value"] == nil || b["value"] == nil || *r.Humidity < number(a["value"]) || *r.Humidity > number(b["value"]) || math.Trunc(*r.Humidity) != *r.Humidity {
			return true, "humidity_out_of_range"
		}
		data["humidity"] = *r.Humidity
		return true, ""
	}
	return false, ""
}
func confirmExtraHome(r HomeRequest, o observation) (bool, bool) {
	switch r.Action {
	case "set_hvac_mode":
		return true, r.Mode != nil && o.entity.State == *r.Mode
	case "set_preset_mode", "set_mode":
		key := "mode"
		if r.Action == "set_preset_mode" {
			key = "preset_mode"
		}
		return true, r.Mode != nil && o.entity.Attributes[key] == *r.Mode
	case "set_brightness":
		n, ok := o.entity.Attributes["brightness"].(float64)
		if r.Brightness != nil && *r.Brightness == 0 && o.entity.State == "off" {
			return true, true
		}
		return true, ok && r.Brightness != nil && o.entity.State == "on" && math.Abs(n-*r.Brightness*255/100) <= 1.01
	case "set_color_temperature":
		n, ok := o.entity.Attributes["color_temp_kelvin"].(float64)
		return true, ok && r.Kelvin != nil && math.Abs(n-*r.Kelvin) <= 1
	case "set_percentage":
		n, ok := o.entity.Attributes["percentage"].(float64)
		return true, ok && r.Percentage != nil && math.Abs(n-*r.Percentage) <= 1.01
	case "set_humidity":
		n, ok := o.entity.Attributes["humidity"].(float64)
		return true, ok && r.Humidity != nil && math.Abs(n-*r.Humidity) < 0.01
	}
	return false, false
}

func bindingActions(b HomeBinding, o observation, now time.Time, fallback string) []any {
	result := []any{}
	for _, action := range homeActions(b.EntityID, o, now, fallback) {
		disabled := false
		for _, deny := range b.DisabledActions {
			if action == deny {
				disabled = true
			}
		}
		if !disabled {
			result = append(result, action)
		}
	}
	return result
}
func validDisabledHomeActions(actions []string) bool {
	if len(actions) > 13 {
		return false
	}
	seen := map[string]bool{}
	for _, action := range actions {
		valid := false
		for _, known := range []string{"turn_on", "turn_off", "set_temperature", "open_cover", "close_cover", "stop_cover", "set_hvac_mode", "set_brightness", "set_color_temperature", "set_percentage", "set_preset_mode", "set_mode", "set_humidity"} {
			if action == known {
				valid = true
			}
		}
		if !valid || seen[action] {
			return false
		}
		seen[action] = true
	}
	return true
}
