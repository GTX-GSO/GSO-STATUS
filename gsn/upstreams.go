package main

// Read-only source adapters; HOME service writes are isolated in HomeCommands.
import (
	"context"
	"encoding/json"
	"fmt"
	"gso-status/contracts"
	"io"
	"math"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

var entityPattern = regexp.MustCompile(`^[a-z][a-z0-9_]*\.[a-z0-9_]+$`)

type HAConfig struct {
	WebSocket       *bool             `json:"websocket,omitempty"`
	Home            []HomeBinding     `json:"home,omitempty"`
	Car             *CarConfig        `json:"car,omitempty"`
	TemperatureUnit string            `json:"temperature_unit,omitempty"`
	BaseURL         string            `json:"base_url"`
	Token           string            `json:"token"`
	Location        string            `json:"location"`
	Entities        map[string]string `json:"entities"`
	VPNTotalBytes   *uint64           `json:"vpn_total_bytes"`
	VPNTotalOrigin  string            `json:"vpn_total_origin"`
}
type MihomoConfig struct {
	BaseURL string `json:"base_url"`
	Token   string `json:"token"`
}
type entityState struct {
	State       string         `json:"state"`
	LastUpdated string         `json:"last_updated"`
	Attributes  map[string]any `json:"attributes"`
}
type observation struct {
	entity entityState
	at     time.Time
	code   string
}
type Upstreams struct {
	config         Config
	mu             sync.Mutex
	items          map[string]observation
	client         *http.Client
	schema         *contracts.Schema
	streamReady    bool
	streamVerified time.Time
	streamKnown    map[string]bool
	wsPingEvery    time.Duration
	forecast       []any
	forecastAt     time.Time
	hourly         []any
	hourlyAt       time.Time
}

func NewUpstreams(c Config) *Upstreams {
	return &Upstreams{config: c, items: map[string]observation{}, client: &http.Client{Timeout: 800 * time.Millisecond, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }, Transport: &http.Transport{Proxy: nil, MaxIdleConns: 4, MaxIdleConnsPerHost: 2}}, schema: contracts.Load("snapshot")}
}
func (u *Upstreams) fetch(ctx context.Context, address, token string, target any) string {
	req, e := http.NewRequestWithContext(ctx, "GET", address, nil)
	if e != nil {
		return "source_url_invalid"
	}
	req.Header.Set("Authorization", "Bearer "+token)
	response, e := u.client.Do(req)
	if e != nil {
		return "source_unreachable"
	}
	defer response.Body.Close()
	if response.StatusCode == 401 || response.StatusCode == 403 {
		return "source_auth_failed"
	}
	if response.StatusCode != 200 {
		return "source_http_error"
	}
	b, e := io.ReadAll(io.LimitReader(response.Body, 32769))
	if e != nil {
		return "source_read_error"
	}
	if len(b) > 32768 {
		return "source_payload_too_large"
	}
	if json.Unmarshal(b, target) != nil {
		return "source_json_invalid"
	}
	return ""
}
func (u *Upstreams) poll(ctx context.Context) {
	for role, id := range u.config.HA.Entities {
		if role == "weather" {
			u.mu.Lock()
			last := u.items[role].at
			u.mu.Unlock()
			if !last.IsZero() && time.Since(last) < 5*time.Minute {
				continue
			}
		}
		if u.streamHealthy() {
			break
		}
		var entity entityState
		code := u.fetch(ctx, strings.TrimRight(u.config.HA.BaseURL, "/")+"/api/states/"+url.PathEscape(id), u.config.HA.Token, &entity)
		if code == "" {
			if _, e := time.Parse(time.RFC3339Nano, entity.LastUpdated); e != nil || entity.State == "" || len(entity.State) > 160 {
				code = "entity_state_invalid"
			}
		}
		u.mu.Lock()
		old := u.items[role]
		// An in-flight REST fallback may finish after the subscription has caught up.
		if u.streamReady && u.streamKnown[role] && time.Since(u.streamVerified) <= 40*time.Second {
			u.mu.Unlock()
			continue
		}
		if code == "" && old.entity.LastUpdated != "" {
			previous, e1 := time.Parse(time.RFC3339Nano, old.entity.LastUpdated)
			next, e2 := time.Parse(time.RFC3339Nano, entity.LastUpdated)
			if e1 == nil && e2 == nil && next.Before(previous) {
				code = "entity_state_regressed"
			}
		}
		if code == "" {
			old.entity = entity
			old.at = time.Now()
		}
		old.code = code
		u.items[role] = old
		u.mu.Unlock()
	}
	if c := u.config.Mihomo; c.BaseURL != "" && u.config.HA.Entities["clash"] == "" {
		var version struct {
			Version string `json:"version"`
		}
		code := u.fetch(ctx, strings.TrimRight(c.BaseURL, "/")+"/version", c.Token, &version)
		if code == "" && version.Version == "" {
			code = "mihomo_version_missing"
		}
		u.mu.Lock()
		old := u.items["clash"]
		if code == "" {
			old.at = time.Now()
			old.entity = entityState{State: "running"}
		}
		old.code = code
		u.items["clash"] = old
		u.mu.Unlock()
	}
	u.pollCar(ctx)
}
func (u *Upstreams) Run(ctx context.Context) {
	go u.runHome(ctx)
	go u.runHAStream(ctx)
	go u.runWeatherForecast(ctx)
	for {
		u.poll(ctx)
		timer := time.NewTimer(15 * time.Second)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}
func sourceFrom(o observation, configured bool, now time.Time) contracts.Object {
	source := contracts.Object{"quality": "unconfigured", "observed_at": nil, "source_updated_at": nil, "age_ms": nil, "last_error_code": "not_configured"}
	if !configured {
		return source
	}
	source["quality"] = "error"
	source["last_error_code"] = "waiting_for_sample"
	if o.code != "" {
		source["last_error_code"] = o.code
	}
	if !o.at.IsZero() {
		age := math.Max(0, float64(now.Sub(o.at).Milliseconds()))
		source["quality"] = "ok"
		source["observed_at"] = o.at.UTC().Format(time.RFC3339Nano)
		source["age_ms"] = age
		source["last_error_code"] = nil
		if o.entity.LastUpdated != "" {
			source["source_updated_at"] = o.entity.LastUpdated
		}
		if age > 45000 {
			source["quality"] = "stale"
		}
		if age > 120000 {
			source["quality"] = "offline"
		}
		if o.code != "" {
			source["quality"] = "error"
			source["last_error_code"] = o.code
		}
		if o.entity.State == "unavailable" || o.entity.State == "unknown" {
			source["quality"] = "offline"
			source["last_error_code"] = "entity_unavailable"
		}
	}
	return source
}
func missing(reason string) contracts.Object { return contracts.Object{"value": nil, "reason": reason} }
func value(n float64) contracts.Object {
	if math.IsNaN(n) || math.IsInf(n, 0) {
		return missing("invalid_number")
	}
	return contracts.Object{"value": n, "reason": nil}
}
func numeric(o observation) contracts.Object {
	if o.at.IsZero() {
		return missing("source_unavailable")
	}
	n, e := strconv.ParseFloat(o.entity.State, 64)
	if e != nil || n < 0 {
		return missing("state_not_numeric")
	}
	return value(n)
}
func (u *Upstreams) Snapshot(now time.Time) contracts.Object {
	u.mu.Lock()
	defer u.mu.Unlock()
	model := u.schema.Empty()
	net := object(model, "net")
	services := object(net, "services")
	configured := func(role string) bool {
		if role == "clash" && u.config.HA.Entities[role] == "" {
			return u.config.Mihomo.BaseURL != ""
		}
		return u.config.HA.Entities[role] != ""
	}
	for _, role := range []string{"proxy", "dhcp", "antiad", "clash"} {
		o := u.liveObservation(role, now)
		state := contracts.Object{"source": sourceFrom(o, configured(role), now), "raw_state": nil}
		if !o.at.IsZero() {
			state["raw_state"] = o.entity.State
		}
		services[role] = state
	}
	vpn := object(net, "vpn")
	if u.config.HA.VPNTotalOrigin != "" {
		vpn["total_origin"] = u.config.HA.VPNTotalOrigin
	}
	o := u.liveObservation("vpn_remaining", now)
	vpn["source"] = sourceFrom(o, configured("vpn_remaining"), now)
	for _, candidate := range []any{u.liveObservation("vpn_updated", now).entity.State, o.entity.Attributes["updated_at"]} {
		if stamp, ok := candidate.(string); ok {
			if _, err := time.Parse(time.RFC3339Nano, stamp); err == nil {
				object(vpn, "source")["source_updated_at"] = stamp
				break
			}
		}
	}
	vpn["remaining_bytes"] = missing("source_unavailable")
	if metric := numeric(o); metric["value"] != nil {
		scale := map[string]float64{"B": 1, "MB": 1e6, "GB": 1e9, "TB": 1e12, "MiB": 1048576, "GiB": 1073741824, "TiB": 1099511627776}
		unit, _ := o.entity.Attributes["unit_of_measurement"].(string)
		if factor, ok := scale[unit]; ok {
			n := math.Round(number(metric["value"]) * factor)
			if n <= 9007199254740991 {
				vpn["remaining_bytes"] = value(n)
			}
		} else {
			vpn["remaining_bytes"] = missing("unit_unsupported")
		}
	}
	if total := u.config.HA.VPNTotalBytes; total != nil {
		vpn["total_bytes"] = value(float64(*total))
		vpn["total_origin"] = u.config.HA.VPNTotalOrigin
	}
	detailSources := object(net, "detail_sources")
	for field, role := range map[string]string{"subscription_updated": "vpn_updated", "antiad_rules_updated": "antiad_rules_updated", "antiad_rule_count": "antiad_rule_count", "dhcp_active_leases": "dhcp_active_leases"} {
		detailSources[field] = sourceFrom(u.liveObservation(role, now), configured(role), now)
	}
	counter := func(role string) contracts.Object {
		if object(detailSources, role)["quality"] != "ok" {
			return missing("source_unavailable")
		}
		metric := numeric(u.liveObservation(role, now))
		if n := metric["value"]; n != nil && (math.Trunc(number(n)) != number(n) || number(n) > 9007199254740991) {
			return missing("count_invalid")
		}
		return metric
	}
	net["antiad_rule_count"] = counter("antiad_rule_count")
	net["dhcp_active_leases"] = counter("dhcp_active_leases")
	for field, role := range map[string]string{"subscription_updated_at": "vpn_updated", "antiad_rules_updated_at": "antiad_rules_updated"} {
		sourceKey := role
		if role == "vpn_updated" {
			sourceKey = "subscription_updated"
		}
		if object(detailSources, sourceKey)["quality"] == "ok" {
			if stamp := u.liveObservation(role, now).entity.State; stamp != "" {
				if _, e := time.Parse(time.RFC3339Nano, stamp); e == nil {
					net[field] = stamp
				}
			}
		}
	}
	weather := object(model, "weather")
	o = u.liveObservation("weather", now)
	weather["source"] = sourceFrom(o, configured("weather"), now)
	if !o.at.IsZero() && o.code == "" {
		object(weather, "source")["quality"] = "ok"
		ageSourceTTL(object(weather, "source"), 0, 600000, 1800000)
	}
	if !o.at.IsZero() {
		weather["condition"] = o.entity.State
	}
	if u.config.HA.Location != "" {
		weather["location"] = u.config.HA.Location
	} else if name, ok := o.entity.Attributes["friendly_name"].(string); ok {
		weather["location"] = cleanName(name)
	}
	if temp, ok := o.entity.Attributes["temperature"].(float64); ok && temp >= -100 && temp <= 200 {
		unit, _ := o.entity.Attributes["temperature_unit"].(string)
		if unit == "°C" || unit == "C" {
			weather["temperature_c"] = value(temp)
		}
	}
	weather["humidity_percent"] = weatherNumber(o, "humidity", 0, 100, 1)
	pressureScale := map[string]float64{"hPa": 1, "mbar": 1, "Pa": 0.01, "inHg": 33.8638866667, "mmHg": 1.333223874}
	pressureUnit, _ := o.entity.Attributes["pressure_unit"].(string)
	weather["pressure_hpa"] = weatherNumber(o, "pressure", 0, 2000, pressureScale[pressureUnit])
	speedScale := map[string]float64{"km/h": 1, "m/s": 3.6, "mph": 1.609344, "kn": 1.852}
	speedUnit, _ := o.entity.Attributes["wind_speed_unit"].(string)
	weather["wind_speed_kmh"] = weatherNumber(o, "wind_speed", 0, 1000, speedScale[speedUnit])
	weather["wind_bearing_degrees"] = weatherNumber(o, "wind_bearing", 0, 360, 1)
	weather["hourly_forecast"] = []any{}
	if u.hourly != nil && now.Sub(u.hourlyAt) < 2*time.Hour {
		weather["hourly_forecast"] = u.hourly
	}
	weather["forecast"] = []any{}
	if u.forecast != nil && now.Sub(u.forecastAt) < 2*time.Hour {
		weather["forecast"] = u.forecast
	}
	sun := object(model, "sun")
	o = u.liveObservation("sun", now)
	sun["source"] = sourceFrom(o, configured("sun"), now)
	for out, attribute := range map[string]string{"sunrise_at": "next_rising", "sunset_at": "next_setting"} {
		if stamp, ok := o.entity.Attributes[attribute].(string); ok {
			if _, e := time.Parse(time.RFC3339Nano, stamp); e == nil {
				sun[out] = stamp
			}
		}
	}
	ambient := object(model, "ambient")
	o = u.liveObservation("ambient_light", now)
	ambient["source"] = sourceFrom(o, configured("ambient_light"), now)
	ambient["illuminance_lux"] = missing("source_unavailable")
	if object(ambient, "source")["quality"] == "ok" {
		n, err := strconv.ParseFloat(o.entity.State, 64)
		if err == nil && !math.IsNaN(n) && !math.IsInf(n, 0) && n >= 0 && n <= 200000 && o.entity.Attributes["unit_of_measurement"] == "lx" {
			ambient["illuminance_lux"] = value(n)
		} else {
			object(ambient, "source")["quality"] = "error"
			object(ambient, "source")["last_error_code"] = "illuminance_invalid"
		}
	}
	model["home"] = u.homeSnapshot(now)
	model["car"] = u.carSnapshot(now)
	return model
}
func validURL(address string) bool {
	u, e := url.Parse(address)
	return e == nil && u.Host != "" && (u.Scheme == "http" || u.Scheme == "https") && u.User == nil && u.RawQuery == "" && u.Fragment == ""
}
func validateUpstreams(c Config) error {
	if c.HA.BaseURL != "" && (!validURL(c.HA.BaseURL) || c.HA.Token == "" || len(c.HA.Location) > 160) {
		return fmt.Errorf("invalid HA configuration")
	}
	if len(c.HA.Home) > 16 {
		return fmt.Errorf("at most 16 HOME bindings")
	}
	if unit := c.HA.TemperatureUnit; unit != "" && unit != "°C" && unit != "C" && unit != "°F" && unit != "F" {
		return fmt.Errorf("invalid HA temperature unit")
	}
	seenHome := map[string]bool{}
	for _, b := range c.HA.Home {
		domain := homeDomain(b.EntityID)
		if c.HA.BaseURL == "" || !entityPattern.MatchString(b.EntityID) || len(b.EntityID) > 160 || seenHome[b.EntityID] || (domain != "light" && domain != "climate" && domain != "cover" && domain != "fan" && domain != "humidifier" && domain != "switch") || len([]rune(b.Name)) > 160 {
			return fmt.Errorf("invalid HOME entity binding")
		}
		if len(b.Members) > 0 && (domain != "cover" || len(b.Members) < 2 || len(b.Members) > 4) {
			return fmt.Errorf("invalid HOME group")
		}
		for _, id := range b.Members {
			if !entityPattern.MatchString(id) || len(id) > 160 || homeDomain(id) != "cover" || id == b.EntityID || seenHome[id] {
				return fmt.Errorf("invalid HOME group member")
			}
			seenHome[id] = true
		}
		for _, id := range []string{b.StatusEntity, b.RemainingEntity} {
			if id != "" && (!entityPattern.MatchString(id) || homeDomain(id) != "sensor" || len(id) > 160) {
				return fmt.Errorf("invalid HOME status sensor")
			}
		}
		if !validDisabledHomeActions(b.DisabledActions) {
			return fmt.Errorf("invalid disabled HOME action")
		}
		seenHome[b.EntityID] = true
	}
	if len(c.HA.Entities) > 12 {
		return fmt.Errorf("at most 12 HA entity bindings")
	}
	allowed := map[string]bool{"clash": true, "proxy": true, "dhcp": true, "antiad": true, "vpn_remaining": true, "vpn_updated": true, "antiad_rule_count": true, "antiad_rules_updated": true, "dhcp_active_leases": true, "weather": true, "sun": true, "ambient_light": true}
	for role, id := range c.HA.Entities {
		if !allowed[role] || !entityPattern.MatchString(id) || c.HA.BaseURL == "" {
			return fmt.Errorf("invalid HA entity binding")
		}
	}
	if c.Mihomo.BaseURL != "" && (!validURL(c.Mihomo.BaseURL) || c.Mihomo.Token == "") {
		return fmt.Errorf("invalid mihomo configuration")
	}
	if err := validateCar(c); err != nil {
		return err
	}
	if c.HA.VPNTotalBytes != nil && (*c.HA.VPNTotalBytes > 9007199254740991 || c.HA.VPNTotalOrigin == "" || len(c.HA.VPNTotalOrigin) > 160) {
		return fmt.Errorf("VPN total requires valid bytes and origin")
	}
	return nil
}
