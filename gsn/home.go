package main

// HOME is opt-in: configured entities, observed capabilities, and a separate control token.
// HA service success is only delivery; confirmation means a subsequent HA state report.
import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
	"unicode"
	"unicode/utf8"

	"gso-status/contracts"
)

type HomeBinding struct {
	EntityID        string   `json:"entity_id"`
	Name            string   `json:"name,omitempty"`
	Members         []string `json:"members,omitempty"`
	StatusEntity    string   `json:"status_entity,omitempty"`
	RemainingEntity string   `json:"remaining_entity,omitempty"`
	DisabledActions []string `json:"disabled_actions,omitempty"`
}

func homeDomain(id string) string { return strings.SplitN(id, ".", 2)[0] }
func cleanName(s string) string {
	r := []rune(strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return -1
		}
		return r
	}, s))
	if len(r) > 160 {
		r = r[:160]
	}
	return string(r)
}
func homeUnit(o observation, fallback string) string {
	unit, _ := o.entity.Attributes["temperature_unit"].(string)
	if unit == "" {
		unit = fallback
	}
	return unit
}
func toC(n float64, unit string) (float64, bool) {
	if math.IsNaN(n) || math.IsInf(n, 0) {
		return 0, false
	}
	switch unit {
	case "°C", "C":
		return n, true
	case "°F", "F":
		return (n - 32) * 5 / 9, true
	}
	return 0, false
}
func homeTemperature(o observation, key, fallback string) contracts.Object {
	n, ok := o.entity.Attributes[key].(float64)
	if !ok {
		return missing("temperature_unavailable")
	}
	n, ok = toC(n, homeUnit(o, fallback))
	if !ok || n < -100 || n > 200 {
		return missing("temperature_unit_or_value_invalid")
	}
	return value(n)
}
func knownHomeState(id, state string) bool {
	if state == "unknown" || state == "unavailable" {
		return true
	}
	switch homeDomain(id) {
	case "light", "fan", "humidifier", "switch":
		return state == "on" || state == "off"
	case "cover":
		return state == "open" || state == "closed" || state == "opening" || state == "closing" || state == "mixed"
	case "climate":
		switch state {
		case "off", "heat", "cool", "heat_cool", "auto", "dry", "fan_only":
			return true
		}
	}
	return false
}
func homeActions(id string, o observation, now time.Time, fallback string) []any {
	actions := []any{}
	if sourceFrom(o, true, now)["quality"] != "ok" || !knownHomeState(id, o.entity.State) {
		return actions
	}
	features, _ := o.entity.Attributes["supported_features"].(float64)
	if math.IsNaN(features) || math.IsInf(features, 0) || features < 0 || features > 2147483647 || math.Trunc(features) != features {
		return actions
	}
	bits := int64(features)
	// Official ClimateEntityFeature and CoverEntityFeature flags; see docs/HOME控制.md.
	switch homeDomain(id) {
	case "fan":
		if bits&32 != 0 {
			actions = append(actions, "turn_on")
		}
		if bits&16 != 0 {
			actions = append(actions, "turn_off")
		}
	case "light":
		actions = append(actions, "turn_on", "turn_off")
	case "climate":
		if bits&128 != 0 {
			actions = append(actions, "turn_off")
		}
		if bits&256 != 0 {
			actions = append(actions, "turn_on")
		}
		if bits&1 != 0 && !(bits&2 != 0 && (o.entity.State == "heat_cool" || o.entity.State == "auto")) {
			min := homeTemperature(o, "min_temp", fallback)
			max := homeTemperature(o, "max_temp", fallback)
			if min["value"] != nil && max["value"] != nil && number(min["value"]) <= number(max["value"]) {
				actions = append(actions, "set_temperature")
			}

		}
	case "cover":
		if bits&1 != 0 {
			actions = append(actions, "open_cover")
		}
		if bits&2 != 0 {
			actions = append(actions, "close_cover")
		}
		if bits&8 != 0 {
			actions = append(actions, "stop_cover")
		}
	}
	return append(actions, extraHomeActions(id, o)...)
}
func (u *Upstreams) homeObservation(id string) (observation, bool) {
	u.mu.Lock()
	defer u.mu.Unlock()
	for _, binding := range u.config.HA.Home {
		if binding.EntityID == id {
			return u.bindingObservation(binding, time.Now()), true
		}
	}
	return observation{}, false
}
func (u *Upstreams) fetchHome(ctx context.Context, id string) observation {
	for _, b := range u.config.HA.Home {
		if b.EntityID == id && len(b.Members) > 0 {
			for _, member := range b.Members {
				u.fetchHome(ctx, member)
			}
			o, _ := u.homeObservation(id)
			return o
		}
	}
	started := time.Now()
	var entity entityState
	code := u.fetch(ctx, strings.TrimRight(u.config.HA.BaseURL, "/")+"/api/states/"+url.PathEscape(id), u.config.HA.Token, &entity)
	if code == "" {
		if _, e := time.Parse(time.RFC3339Nano, entity.LastUpdated); e != nil || entity.State == "" || utf8.RuneCountInString(entity.State) > 160 || !knownHomeState(id, entity.State) {
			code = "entity_state_invalid"
		}
	}
	u.mu.Lock()
	defer u.mu.Unlock()
	old := u.items["home:"+id]
	if code == "" && old.entity.LastUpdated != "" {
		previous, e1 := time.Parse(time.RFC3339Nano, old.entity.LastUpdated)
		next, e2 := time.Parse(time.RFC3339Nano, entity.LastUpdated)
		if e1 == nil && e2 == nil && next.Before(previous) {
			if old.code == "" && (old.at.After(started) || (u.streamReady && u.streamKnown["home:"+id] && time.Since(u.streamVerified) <= 40*time.Second)) {
				return u.liveObservation("home:"+id, time.Now())
			}
			code = "entity_state_regressed"
		}
	}
	if code == "" {
		old.entity = entity
		old.at = time.Now()
	}
	old.code = code
	u.items["home:"+id] = old
	return old
}
func (u *Upstreams) runHome(ctx context.Context) {
	if len(u.config.HA.Home) == 0 {
		return
	}
	for {
		for _, b := range u.config.HA.Home {
			if u.streamHealthy() {
				break
			}
			if ctx.Err() != nil {
				return
			}
			u.fetchHome(ctx, b.EntityID)
			for _, extra := range []string{b.StatusEntity, b.RemainingEntity} {
				if extra != "" {
					u.fetchHomeExtra(ctx, extra)
				}
			}
		}
		timer := time.NewTimer(5 * time.Second)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}

// Caller holds u.mu. Sources and names are bounded, and missing room temperature stays null.
func (u *Upstreams) homeSnapshot(now time.Time) contracts.Object {
	entities := []any{}
	aggregate := sourceFrom(observation{}, false, now)
	for i, b := range u.config.HA.Home {
		o := u.bindingObservation(b, now)
		source := sourceFrom(o, true, now)
		if i == 0 || source["quality"] != "ok" {
			aggregate = source
		}
		name := b.Name
		if name == "" {
			name, _ = o.entity.Attributes["friendly_name"].(string)
		}
		name = cleanName(name)
		if name == "" {
			name = b.EntityID
		}
		actions := []any{}
		if u.config.HomeControlToken != "" {
			actions = bindingActions(b, o, now, u.config.HA.TemperatureUnit)
		}
		var state any
		if !o.at.IsZero() {
			state = o.entity.State
		}
		current, target := missing("not_a_climate_entity"), missing("not_a_climate_entity")
		if homeDomain(b.EntityID) == "climate" {
			current = homeTemperature(o, "current_temperature", u.config.HA.TemperatureUnit)
			target = homeTemperature(o, "temperature", u.config.HA.TemperatureUnit)
		}
		min, max, step := missing("not_a_climate_entity"), missing("not_a_climate_entity"), missing("not_a_climate_entity")
		if homeDomain(b.EntityID) == "climate" {
			min = homeTemperature(o, "min_temp", u.config.HA.TemperatureUnit)
			max = homeTemperature(o, "max_temp", u.config.HA.TemperatureUnit)
			if min["value"] != nil && max["value"] != nil && number(min["value"]) > number(max["value"]) {
				min = missing("temperature_range_invalid")
				max = missing("temperature_range_invalid")
			}
			step = missing("temperature_step_unavailable")
			if n, ok := o.entity.Attributes["target_temp_step"].(float64); ok && n > 0 && n <= 100 {
				if unit := homeUnit(o, u.config.HA.TemperatureUnit); unit == "°C" || unit == "C" {
					step = value(n)
				} else if unit == "°F" || unit == "F" {
					step = value(n * 5 / 9)
				}
			}
		}
		entry := contracts.Object{"entity_id": b.EntityID, "name": name, "source": source, "state": state, "current_temperature_c": current, "target_temperature_c": target, "actions": actions, "min_temperature_c": min, "max_temperature_c": max, "temperature_step_c": step}
		for k, v := range homeControlSnapshot(b.EntityID, o) {
			entry[k] = v
		}
		if b.StatusEntity != "" {
			extra := u.liveObservation("home-extra:"+b.StatusEntity, now)
			if sourceFrom(extra, true, now)["quality"] == "ok" {
				entry["detail_state"] = extra.entity.State
			}
		}
		if b.RemainingEntity != "" && o.entity.State != "off" {
			extra := u.liveObservation("home-extra:"+b.RemainingEntity, now)
			if sourceFrom(extra, true, now)["quality"] == "ok" {
				entry["remaining_minutes"] = numeric(extra)
			}
		}
		entities = append(entities, entry)
	}
	return contracts.Object{"source": aggregate, "entities": entities}
}

type HomeRequest struct {
	ControlEpoch string    `json:"control_epoch"`
	RequestID    string    `json:"request_id"`
	EntityID     string    `json:"entity_id"`
	Action       string    `json:"action"`
	ExpiresAt    time.Time `json:"expires_at"`
	Temperature  *float64  `json:"temperature_c,omitempty"`
	Brightness   *float64  `json:"brightness_percent,omitempty"`
	Kelvin       *float64  `json:"color_temperature_kelvin,omitempty"`
	Percentage   *float64  `json:"percentage,omitempty"`
	Humidity     *float64  `json:"humidity_percent,omitempty"`
	Mode         *string   `json:"mode,omitempty"`
}
type homeRecord struct {
	request          HomeRequest
	fingerprint      [32]byte
	status, code     string
	started          bool
	created, updated time.Time
}
type HomeCommands struct {
	epoch         string
	config        Config
	upstreams     *Upstreams
	mu            sync.Mutex
	records       map[string]*homeRecord
	queue         chan string
	priority      chan string
	confirmID     string
	confirmCancel context.CancelFunc
	schema        *contracts.Schema
}

func NewHomeCommands(c Config, u *Upstreams) *HomeCommands {
	bytes := make([]byte, 16)
	if _, e := rand.Read(bytes); e != nil {
		panic("HOME boot identity unavailable")
	}
	bytes[6] = (bytes[6] & 15) | 64
	bytes[8] = (bytes[8] & 63) | 128
	epoch := fmt.Sprintf("%x-%x-%x-%x-%x", bytes[:4], bytes[4:6], bytes[6:8], bytes[8:10], bytes[10:])
	return &HomeCommands{epoch: epoch, config: c, upstreams: u, records: map[string]*homeRecord{}, queue: make(chan string, 8), priority: make(chan string, 1), schema: contracts.Load("home-command")}
}

func commandView(r *homeRecord) contracts.Object {
	var code any
	if r.code != "" {
		code = r.code
	}
	return contracts.Object{"command_id": r.request.RequestID, "entity_id": r.request.EntityID, "action": r.request.Action, "status": r.status, "error_code": code, "created_at": r.created.UTC().Format(time.RFC3339Nano), "updated_at": r.updated.UTC().Format(time.RFC3339Nano), "expires_at": r.request.ExpiresAt.UTC().Format(time.RFC3339Nano)}
}
func (h *HomeCommands) Get(id string) (contracts.Object, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	r, ok := h.records[id]
	if !ok {
		return nil, false
	}
	return commandView(r), true
}
func (h *HomeCommands) Submit(body []byte, now time.Time) (contracts.Object, int, string) {
	if h.config.HomeControlToken == "" || h.config.HA.BaseURL == "" {
		return nil, 503, "home_controls_unconfigured"
	}
	object, e := contracts.Decode(body)
	if e != nil || h.schema.Validate(object) != nil {
		return nil, 400, "invalid_home_command"
	}
	var request HomeRequest
	if json.Unmarshal(body, &request) != nil {
		return nil, 400, "invalid_home_command"
	}
	if request.ControlEpoch != h.epoch {
		return nil, 409, "server_restarted_refresh_state"
	}
	canonical, _ := json.Marshal(request)
	fingerprint := sha256.Sum256(canonical)
	h.mu.Lock()
	defer h.mu.Unlock()
	if old, ok := h.records[request.RequestID]; ok {
		if old.fingerprint != fingerprint {
			return nil, 409, "idempotency_conflict"
		}
		return commandView(old), 200, ""
	}
	if !request.ExpiresAt.After(now) || request.ExpiresAt.Sub(now) > 30*time.Second {
		return nil, 409, "command_expired_or_too_far"
	}
	if _, code := h.plan(request, now); code != "" {
		return nil, 422, code
	}
	superseded := []*homeRecord{}
	for id, r := range h.records {
		if r.status != "queued" && r.status != "sent" && now.Sub(r.updated) > 2*time.Minute {
			delete(h.records, id)
			continue
		}
		same := r.request.EntityID == request.EntityID
		movement := r.request.Action == "open_cover" || r.request.Action == "close_cover"
		reverse := (request.Action == "open_cover" || request.Action == "close_cover") && request.Action != r.request.Action
		interrupt := same && movement && (request.Action == "stop_cover" || reverse)
		if same && (r.status == "queued" || r.status == "sent") {
			if interrupt {
				superseded = append(superseded, r)
			} else {
				return nil, 409, "entity_busy"
			}
		}
		if same && now.Sub(r.created) < 500*time.Millisecond && !interrupt {
			return nil, 429, "touch_debounced"
		}
	}
	if len(h.records) >= 64 {
		return nil, 429, "command_history_full"
	}
	r := &homeRecord{request: request, fingerprint: fingerprint, status: "queued", created: now, updated: now}
	h.records[request.RequestID] = r
	queue := h.queue
	if request.Action == "stop_cover" || len(superseded) > 0 {
		queue = h.priority
	}
	select {
	case queue <- request.RequestID:
		for _, old := range superseded {
			old.status = "superseded"
			old.code = "superseded_by_stop"
			if request.Action != "stop_cover" {
				old.code = "superseded_by_cover_reversal"
			}
			old.updated = now
			if h.confirmID == old.request.RequestID && h.confirmCancel != nil {
				h.confirmCancel()
			}
		}
		return commandView(r), 202, ""
	default:
		delete(h.records, request.RequestID)
		return nil, 429, "command_queue_full"
	}
}

func (h *HomeCommands) plan(r HomeRequest, now time.Time) (map[string]any, string) {
	if _, car := h.upstreams.carRole(r.EntityID); car {
		if r.Temperature != nil || r.Brightness != nil || r.Kelvin != nil || r.Percentage != nil || r.Humidity != nil || r.Mode != nil {
			return nil, "unexpected_or_missing_parameter"
		}
		if !h.upstreams.carActionAllowed(r.EntityID, r.Action, now) {
			return nil, "action_not_supported_or_source_stale"
		}
		return map[string]any{"entity_id": r.EntityID}, ""
	}
	o, bound := h.upstreams.homeObservation(r.EntityID)
	if !bound {
		return nil, "entity_not_bound"
	}
	allowed := false
	var binding HomeBinding
	for _, b := range h.config.HA.Home {
		if b.EntityID == r.EntityID {
			binding = b
			break
		}
	}
	for _, action := range bindingActions(binding, o, now, h.config.HA.TemperatureUnit) {
		allowed = allowed || action == r.Action
	}
	if !allowed {
		return nil, "action_not_supported_or_source_stale"
	}
	data := map[string]any{"entity_id": r.EntityID}
	for _, b := range h.config.HA.Home {
		if b.EntityID == r.EntityID && len(b.Members) > 0 {
			data["entity_id"] = b.Members
		}
	}
	if handled, code := planExtraHome(r, o, data); handled || code != "" {
		return data, code
	}
	if r.Action != "set_temperature" {
		if r.Temperature != nil {
			return nil, "unexpected_temperature"
		}
		return data, ""
	}
	if r.Temperature == nil {
		return nil, "temperature_required"
	}
	target := *r.Temperature
	unit := homeUnit(o, h.config.HA.TemperatureUnit)
	if unit == "°F" || unit == "F" {
		target = target*9/5 + 32
	}
	min, a := o.entity.Attributes["min_temp"].(float64)
	max, b := o.entity.Attributes["max_temp"].(float64)
	if !a || !b || target < min || target > max {
		return nil, "temperature_out_of_range"
	}
	if step, ok := o.entity.Attributes["target_temp_step"].(float64); ok && step > 0 {
		n := (target - min) / step
		if math.Abs(n-math.Round(n)) > 0.000001 {
			return nil, "temperature_step_invalid"
		}
		target = min + math.Round(n)*step
	}
	data["temperature"] = target
	return data, ""
}
func (h *HomeCommands) update(id, status, code string) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	if r := h.records[id]; r != nil && r.status != "superseded" {
		r.status = status
		r.code = code
		r.updated = time.Now()
		return true
	}
	return false
}
func (h *HomeCommands) Run(ctx context.Context) {
	defer func() {
		h.mu.Lock()
		defer h.mu.Unlock()
		for _, r := range h.records {
			if r.status == "queued" || r.status == "sent" {
				r.status = "rejected"
				r.code = "service_stopping"
				r.updated = time.Now()
			}
		}
	}()
	for {
		if ctx.Err() != nil {
			return
		}
		var id string
		select {
		case id = <-h.priority:
		default:
			select {
			case <-ctx.Done():
				return
			case id = <-h.priority:
			case id = <-h.queue:
			}
		}
		h.execute(ctx, id)
	}
}
func (h *HomeCommands) execute(ctx context.Context, id string) {
	h.mu.Lock()
	record := h.records[id]
	if record == nil || record.started || record.status != "queued" {
		h.mu.Unlock()
		return
	}
	record.started = true
	request := record.request
	h.mu.Unlock()
	if !request.ExpiresAt.After(time.Now()) {
		h.update(id, "timed_out", "command_expired")
		return
	}
	data, code := h.plan(request, time.Now())
	if code != "" {
		h.update(id, "rejected", code)
		return
	}
	body, _ := json.Marshal(data)
	dispatch, cancelDispatch := context.WithDeadline(ctx, request.ExpiresAt)
	defer cancelDispatch()
	req, e := http.NewRequestWithContext(dispatch, "POST", strings.TrimRight(h.config.HA.BaseURL, "/")+"/api/services/"+homeDomain(request.EntityID)+"/"+homeService(request.Action), bytes.NewReader(body))
	if e != nil {
		h.update(id, "rejected", "service_url_invalid")
		return
	}
	req.Header.Set("Authorization", "Bearer "+h.config.HA.Token)
	req.Header.Set("Content-Type", "application/json")
	if !h.update(id, "sent", "") {
		return
	}
	response, e := h.upstreams.client.Do(req)
	if e == nil {
		_, readErr := io.Copy(io.Discard, io.LimitReader(response.Body, 32769))
		response.Body.Close()
		if response.StatusCode >= 300 && response.StatusCode < 500 {
			h.update(id, "rejected", fmt.Sprintf("ha_http_%d", response.StatusCode))
			return
		}
		if response.StatusCode != 200 || readErr != nil {
			h.update(id, "sent", "delivery_unconfirmed")
		}
	} else {
		h.update(id, "sent", "delivery_unconfirmed")
	}
	// Observe only the target, with a deadline; never retry the service POST.
	deadline := request.ExpiresAt
	confirmationWait := 10 * time.Second
	if domain := homeDomain(request.EntityID); domain == "cover" || domain == "lock" {
		confirmationWait = 30 * time.Second
	}
	if max := time.Now().Add(confirmationWait); deadline.After(max) {
		deadline = max
	}
	confirmation, cancel := context.WithDeadline(ctx, deadline)
	h.mu.Lock()
	if h.records[id].status == "superseded" {
		h.mu.Unlock()
		cancel()
		return
	}
	h.confirmID = id
	h.confirmCancel = cancel
	h.mu.Unlock()
	defer func() {
		cancel()
		h.mu.Lock()
		if h.confirmID == id {
			h.confirmID = ""
			h.confirmCancel = nil
		}
		h.mu.Unlock()
	}()
	carRole, isCar := h.upstreams.carRole(request.EntityID)
	h.mu.Lock()
	created := h.records[id].created
	h.mu.Unlock()
	for confirmation.Err() == nil {
		var confirmedNow bool
		if isCar {
			confirmedNow = carConfirmed(request, h.upstreams.fetchCarRole(confirmation, carRole), created)
		} else {
			o := h.upstreams.fetchHome(confirmation, request.EntityID)
			confirmedNow = sourceFrom(o, true, time.Now())["quality"] == "ok" && h.confirmed(request, o)
		}
		if confirmedNow {
			h.update(id, "confirmed", "")
			return
		}
		timer := time.NewTimer(500 * time.Millisecond)
		select {
		case <-confirmation.Done():
			timer.Stop()
		case <-timer.C:
		}
	}
	h.update(id, "timed_out", "state_not_confirmed")
}
func (h *HomeCommands) confirmed(r HomeRequest, o observation) bool {
	if handled, ok := confirmExtraHome(r, o); handled {
		return ok
	}
	switch r.Action {
	case "turn_on":
		if homeDomain(r.EntityID) == "climate" {
			return knownHomeState(r.EntityID, o.entity.State) && o.entity.State != "off" && o.entity.State != "unknown" && o.entity.State != "unavailable"
		}
		return o.entity.State == "on"
	case "turn_off":
		return o.entity.State == "off"
	case "open_cover":
		if position, ok := o.entity.Attributes["current_position"].(float64); ok {
			return o.entity.State == "open" && position >= 100
		}
		return o.entity.State == "open"
	case "close_cover":
		if position, ok := o.entity.Attributes["current_position"].(float64); ok {
			return o.entity.State == "closed" && position <= 0
		}
		return o.entity.State == "closed"
	case "stop_cover":
		return o.entity.State == "open" || o.entity.State == "closed"
	case "set_temperature":
		actual := homeTemperature(o, "temperature", h.config.HA.TemperatureUnit)
		if actual["value"] == nil || r.Temperature == nil {
			return false
		}
		return math.Abs(number(actual["value"])-*r.Temperature) < 0.01
	}
	return false
}
