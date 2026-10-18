package main

// One bounded, authenticated subscription for configured entities only. No service calls.
import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/coder/websocket"
	"gso-status/contracts"
)

const haStreamLimit = 65536

var errHAStreamAuth = errors.New("ha_ws_auth_failed")
var errHAStreamUnsupported = errors.New("ha_ws_subscription_unsupported")
var errHAStreamProtocol = errors.New("ha_ws_protocol_invalid")

func (u *Upstreams) haBindings() map[string][]string {
	bindings := map[string][]string{}
	for role, id := range u.config.HA.Entities {
		bindings[id] = append(bindings[id], role)
	}
	if car := u.config.HA.Car; car != nil {
		for role, id := range car.Entities {
			bindings[id] = append(bindings[id], "car:"+role)
		}
	}
	for _, binding := range u.config.HA.Home {
		for _, id := range bindingIDs(binding) {
			bindings[id] = append(bindings[id], "home:"+id)
		}
		for _, id := range []string{binding.StatusEntity, binding.RemainingEntity} {
			if id != "" {
				bindings[id] = append(bindings[id], "home-extra:"+id)
			}
		}
	}
	return bindings
}

// Caller holds u.mu. An uninterrupted subscription plus a verified pong confirms that
// the cached state is current. Its entity LastUpdated is never changed by a heartbeat.
func (u *Upstreams) liveObservation(key string, now time.Time) observation {
	o := u.items[key]
	if u.streamReady && now.Sub(u.streamVerified) <= 40*time.Second && u.streamKnown[key] && o.code == "" && u.streamVerified.After(o.at) {
		o.at = u.streamVerified
	}
	return o
}
func (u *Upstreams) streamHealthy() bool {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.streamReady && time.Since(u.streamVerified) <= 40*time.Second
}
func (u *Upstreams) stopStream() {
	u.mu.Lock()
	defer u.mu.Unlock()
	for key, known := range u.streamKnown {
		if known {
			o := u.items[key]
			if o.code == "" && u.streamVerified.After(o.at) {
				o.at = u.streamVerified
				u.items[key] = o
			}
		}
	}
	u.streamReady = false
	u.streamKnown = nil
}

func haTimestamp(raw any) (string, bool) {
	seconds, ok := numHA(raw)
	if !ok || seconds <= 0 || seconds > 253402300799 {
		return "", false
	}
	// HA timestamps have microsecond precision. Round before formatting to avoid
	// floating-point nanosecond regressions against REST's ISO8601 representation.
	stamp := time.UnixMicro(int64(math.Round(seconds * 1e6))).UTC()
	return stamp.Format(time.RFC3339Nano), true
}
func numHA(value any) (float64, bool) {
	switch n := value.(type) {
	case json.Number:
		f, e := n.Float64()
		return f, e == nil && !math.IsNaN(f) && !math.IsInf(f, 0)
	case float64:
		return n, !math.IsNaN(n) && !math.IsInf(n, 0)
	}
	return 0, false
}

var haAttributeKeys = map[string]bool{
	"humidity": true, "pressure": true, "pressure_unit": true, "wind_speed": true, "wind_speed_unit": true, "wind_bearing": true,
	"friendly_name": true, "unit_of_measurement": true, "updated_at": true, "temperature": true, "temperature_unit": true,
	"supported_features": true, "current_temperature": true, "min_temp": true, "max_temp": true, "target_temp_step": true,
	"next_rising": true, "next_setting": true,
	"current_position": true, "remaining_minutes": true,
	"windows_open": true, "trunk_open": true, "charging_voltage": true, "charging_current": true, "alarm": true,
	"hvac_modes": true, "supported_color_modes": true, "brightness": true, "color_temp_kelvin": true, "min_color_temp_kelvin": true, "max_color_temp_kelvin": true, "color_temp": true, "min_mireds": true, "max_mireds": true, "percentage": true, "percentage_step": true, "preset_modes": true, "preset_mode": true, "available_modes": true, "mode": true, "min_humidity": true, "max_humidity": true,
}

func haAttributes(raw any) (map[string]any, bool) {
	object, ok := raw.(map[string]any)
	if !ok {
		return nil, false
	}
	result := map[string]any{}
	for key, value := range object {
		if !haAttributeKeys[key] {
			continue
		}
		switch v := value.(type) {
		case []any:
			if len(v) > 8 {
				return nil, false
			}
			list := []any{}
			for _, item := range v {
				text, ok := item.(string)
				if !ok || len(text) > 64 || strings.ContainsAny(text, "\r\n") {
					return nil, false
				}
				list = append(list, text)
			}
			result[key] = list
		case nil:
			result[key] = nil
		case string:
			if utf8.RuneCountInString(v) > 160 {
				return nil, false
			}
			result[key] = v
		case bool:
			result[key] = v
		case json.Number:
			n, ok := numHA(v)
			if !ok {
				return nil, false
			}
			result[key] = n
		case float64:
			if math.IsNaN(v) || math.IsInf(v, 0) {
				return nil, false
			}
			result[key] = v
		default:
			return nil, false
		}
	}
	return result, true
}
func haAddedState(raw any) (entityState, bool) {
	object, ok := raw.(map[string]any)
	if !ok {
		return entityState{}, false
	}
	state, ok := object["s"].(string)
	if !ok || state == "" || utf8.RuneCountInString(state) > 160 {
		return entityState{}, false
	}
	attributes, ok := haAttributes(object["a"])
	if !ok {
		return entityState{}, false
	}
	stamp, ok := haTimestamp(object["lc"])
	if !ok {
		return entityState{}, false
	}
	if value, exists := object["lu"]; exists {
		stamp, ok = haTimestamp(value)
		if !ok {
			return entityState{}, false
		}
	}
	return entityState{State: state, Attributes: attributes, LastUpdated: stamp}, true
}
func haChangedState(old entityState, raw any) (entityState, bool) {
	diff, ok := raw.(map[string]any)
	if !ok {
		return entityState{}, false
	}
	attributes := map[string]any{}
	for k, v := range old.Attributes {
		attributes[k] = v
	}
	old.Attributes = attributes
	if raw, exists := diff["+"]; exists {
		plus, ok := raw.(map[string]any)
		if !ok {
			return entityState{}, false
		}
		if value, exists := plus["s"]; exists {
			state, ok := value.(string)
			if !ok || state == "" || utf8.RuneCountInString(state) > 160 {
				return entityState{}, false
			}
			old.State = state
		}
		if raw, exists := plus["a"]; exists {
			added, ok := haAttributes(raw)
			if !ok {
				return entityState{}, false
			}
			for k, v := range added {
				attributes[k] = v
			}
		}
		if value, exists := plus["lc"]; exists {
			stamp, ok := haTimestamp(value)
			if !ok {
				return entityState{}, false
			}
			old.LastUpdated = stamp
		}
		if value, exists := plus["lu"]; exists {
			stamp, ok := haTimestamp(value)
			if !ok {
				return entityState{}, false
			}
			old.LastUpdated = stamp
		}
	}
	if raw, exists := diff["-"]; exists {
		minus, ok := raw.(map[string]any)
		if !ok {
			return entityState{}, false
		}
		if raw, exists := minus["a"]; exists {
			keys, ok := raw.([]any)
			if !ok || len(keys) > 256 {
				return entityState{}, false
			}
			for _, item := range keys {
				key, ok := item.(string)
				if !ok {
					return entityState{}, false
				}
				delete(attributes, key)
			}
		}
	}
	return old, true
}

func haStreamEvent(event map[string]any, cache map[string]entityState, bindings map[string][]string, initial bool) (map[string]bool, error) {
	changed := map[string]bool{}
	if initial {
		if _, ok := event["a"].(map[string]any); !ok {
			return nil, errHAStreamProtocol
		}
		for id := range bindings {
			changed[id] = false
		}
	}
	if raw, exists := event["a"]; exists {
		added, ok := raw.(map[string]any)
		if !ok {
			return nil, errHAStreamProtocol
		}
		for id, raw := range added {
			if _, wanted := bindings[id]; !wanted {
				continue
			}
			state, ok := haAddedState(raw)
			if !ok {
				return nil, errHAStreamProtocol
			}
			cache[id] = state
			changed[id] = true
		}
	}
	if raw, exists := event["r"]; exists {
		removed, ok := raw.([]any)
		if !ok {
			return nil, errHAStreamProtocol
		}
		for _, raw := range removed {
			id, ok := raw.(string)
			if !ok {
				return nil, errHAStreamProtocol
			}
			if _, wanted := bindings[id]; wanted {
				delete(cache, id)
				changed[id] = false
			}
		}
	}
	if raw, exists := event["c"]; exists {
		diffs, ok := raw.(map[string]any)
		if !ok {
			return nil, errHAStreamProtocol
		}
		for id, raw := range diffs {
			if _, wanted := bindings[id]; !wanted {
				continue
			}
			old, known := cache[id]
			if !known {
				return nil, errHAStreamProtocol
			}
			state, ok := haChangedState(old, raw)
			if !ok {
				return nil, errHAStreamProtocol
			}
			cache[id] = state
			changed[id] = true
		}
	}
	return changed, nil
}
func (u *Upstreams) applyStream(cache map[string]entityState, changed map[string]bool, bindings map[string][]string, now time.Time) {
	u.mu.Lock()
	defer u.mu.Unlock()
	if u.streamKnown == nil {
		u.streamKnown = map[string]bool{}
	}
	for id, known := range changed {
		for _, key := range bindings[id] {
			o := u.items[key]
			u.streamKnown[key] = known
			if !known {
				o.code = "ha_entity_missing"
				u.items[key] = o
				continue
			}
			state := cache[id]
			if strings.HasPrefix(key, "home:") && !knownHomeState(id, state.State) {
				o.code = "entity_state_invalid"
				u.items[key] = o
				continue
			}
			oldStamp, _ := time.Parse(time.RFC3339Nano, o.entity.LastUpdated)
			nextStamp, _ := time.Parse(time.RFC3339Nano, state.LastUpdated)
			if !nextStamp.Before(oldStamp) {
				o.entity = state
				o.at = now
			}
			o.code = ""
			u.items[key] = o
		}
	}
	u.streamReady = true
	u.streamVerified = now
}
func readHAStream(ctx context.Context, connection *websocket.Conn) (contracts.Object, error) {
	kind, raw, e := connection.Read(ctx)
	if e != nil {
		return nil, e
	}
	if kind != websocket.MessageText {
		return nil, errHAStreamProtocol
	}
	object, e := contracts.Decode(raw)
	if e != nil {
		return nil, errHAStreamProtocol
	}
	return object, nil
}
func writeHAStream(ctx context.Context, connection *websocket.Conn, object any) error {
	raw, e := json.Marshal(object)
	if e != nil || len(raw) > 8192 {
		return errHAStreamProtocol
	}
	deadline, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	return connection.Write(deadline, websocket.MessageText, raw)
}

func (u *Upstreams) haStreamSession(ctx context.Context, bindings map[string][]string) error {
	session, cancel := context.WithCancel(ctx)
	defer cancel()
	defer u.stopStream()
	startup := time.AfterFunc(2*time.Second, cancel)
	defer startup.Stop()
	address, e := url.Parse(strings.TrimRight(u.config.HA.BaseURL, "/") + "/api/websocket")
	if e != nil {
		return errHAStreamProtocol
	}
	if address.Scheme == "https" {
		address.Scheme = "wss"
	} else {
		address.Scheme = "ws"
	}
	client := &http.Client{Transport: u.client.Transport, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	connection, response, e := websocket.Dial(session, address.String(), &websocket.DialOptions{HTTPClient: client, CompressionMode: websocket.CompressionDisabled})
	if e != nil {
		if response != nil && response.Body != nil {
			response.Body.Close()
		}
		return e
	}
	defer connection.CloseNow()
	connection.SetReadLimit(haStreamLimit)
	message, e := readHAStream(session, connection)
	if e != nil {
		return e
	}
	if message["type"] != "auth_required" {
		return errHAStreamProtocol
	}
	if e = writeHAStream(session, connection, map[string]any{"type": "auth", "access_token": u.config.HA.Token}); e != nil {
		return e
	}
	message, e = readHAStream(session, connection)
	if e != nil {
		return e
	}
	if message["type"] == "auth_invalid" {
		return errHAStreamAuth
	}
	if message["type"] != "auth_ok" {
		return errHAStreamProtocol
	}
	ids := make([]string, 0, len(bindings))
	for id := range bindings {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	if e = writeHAStream(session, connection, map[string]any{"id": 1, "type": "subscribe_entities", "entity_ids": ids}); e != nil {
		return e
	}
	message, e = readHAStream(session, connection)
	if e != nil {
		return e
	}
	if message["type"] != "result" || number(message["id"]) != 1 {
		return errHAStreamProtocol
	}
	if message["success"] != true {
		return errHAStreamUnsupported
	}
	message, e = readHAStream(session, connection)
	if e != nil {
		return e
	}
	if message["type"] != "event" || number(message["id"]) != 1 {
		return errHAStreamProtocol
	}
	initial, ok := message["event"].(map[string]any)
	if !ok {
		return errHAStreamProtocol
	}
	cache := map[string]entityState{}
	changed, e := haStreamEvent(initial, cache, bindings, true)
	if e != nil {
		return e
	}
	u.applyStream(cache, changed, bindings, time.Now())
	startup.Stop()
	pingEvery := u.wsPingEvery
	if pingEvery <= 0 {
		pingEvery = 15 * time.Second
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		ticker := time.NewTicker(pingEvery)
		defer ticker.Stop()
		for {
			select {
			case <-session.Done():
				return
			case <-ticker.C:
				deadline, stop := context.WithTimeout(session, 2*time.Second)
				e := connection.Ping(deadline)
				stop()
				if e != nil {
					cancel()
					return
				}
				u.mu.Lock()
				u.streamVerified = time.Now()
				u.mu.Unlock()
			}
		}
	}()
	defer func() { cancel(); connection.CloseNow(); <-done }()
	for {
		message, e = readHAStream(session, connection)
		if e != nil {
			return e
		}
		if message["type"] != "event" || number(message["id"]) != 1 {
			return errHAStreamProtocol
		}
		event, ok := message["event"].(map[string]any)
		if !ok {
			return errHAStreamProtocol
		}
		changed, e = haStreamEvent(event, cache, bindings, false)
		if e != nil {
			return e
		}
		u.applyStream(cache, changed, bindings, time.Now())
	}
}
func (u *Upstreams) runHAStream(ctx context.Context) {
	bindings := u.haBindings()
	if u.config.HA.BaseURL == "" || u.config.HA.Token == "" || len(bindings) == 0 || (u.config.HA.WebSocket != nil && !*u.config.HA.WebSocket) {
		return
	}
	retry := 2 * time.Second
	for ctx.Err() == nil {
		e := u.haStreamSession(ctx, bindings)
		if ctx.Err() != nil {
			return
		}
		wait := retry
		if errors.Is(e, errHAStreamAuth) || errors.Is(e, errHAStreamUnsupported) {
			wait = 5 * time.Minute
		}
		timer := time.NewTimer(wait)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
		if retry < time.Minute {
			retry *= 2
			if retry > time.Minute {
				retry = time.Minute
			}
		}
	}
}
