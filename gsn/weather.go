package main

import (
	"bytes"
	"context"
	"encoding/json"
	"gso-status/contracts"
	"io"
	"math"
	"net/http"
	"strings"
	"time"
)

func weatherNumber(o observation, key string, min, max, scale float64) contracts.Object {
	n, ok := o.entity.Attributes[key].(float64)
	n *= scale
	if o.code != "" || !ok || scale <= 0 || math.IsNaN(n) || math.IsInf(n, 0) || n < min || n > max {
		return missing("weather_metric_unavailable")
	}
	return value(n)
}

// Forecast retrieval is a read-only HA weather action, never a household control.
func (u *Upstreams) pollForecast(ctx context.Context) bool { return u.pollForecastKind(ctx, "daily") }
func (u *Upstreams) pollForecastKind(ctx context.Context, kind string) bool {
	id := u.config.HA.Entities["weather"]
	if id == "" {
		return false
	}
	body, _ := json.Marshal(map[string]any{"entity_id": id, "type": kind})
	req, e := http.NewRequestWithContext(ctx, "POST", strings.TrimRight(u.config.HA.BaseURL, "/")+"/api/services/weather/get_forecasts?return_response", bytes.NewReader(body))
	if e != nil {
		return false
	}
	req.Header.Set("Authorization", "Bearer "+u.config.HA.Token)
	req.Header.Set("Content-Type", "application/json")
	resp, e := u.client.Do(req)
	if e != nil {
		return false
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return false
	}
	raw, e := io.ReadAll(io.LimitReader(resp.Body, 32769))
	if e != nil || len(raw) > 32768 {
		return false
	}
	var data struct {
		ServiceResponse map[string]struct {
			Forecast []struct {
				Datetime    string
				Condition   string
				Temperature *float64
				Templow     *float64
			}
		} `json:"service_response"`
	}
	if json.Unmarshal(raw, &data) != nil {
		return false
	}
	days := []any{}
	u.mu.Lock()
	unit, _ := u.items["weather"].entity.Attributes["temperature_unit"].(string)
	u.mu.Unlock()
	if unit == "" {
		unit = u.config.HA.TemperatureUnit
	}
	for _, day := range data.ServiceResponse[id].Forecast {
		limit := 6
		if kind == "hourly" {
			limit = 12
		}
		if len(days) >= limit {
			break
		}
		at, e := time.Parse(time.RFC3339Nano, day.Datetime)
		if e != nil || len(day.Condition) > 32 {
			continue
		}
		temp := func(n *float64) contracts.Object {
			if n == nil {
				return missing("forecast_temperature_unavailable")
			}
			v, ok := toC(*n, unit)
			if !ok || v < -100 || v > 200 {
				return missing("forecast_temperature_invalid")
			}
			return value(v)
		}
		if kind == "hourly" {
			days = append(days, contracts.Object{"datetime": at.UTC().Format(time.RFC3339Nano), "condition": day.Condition, "temperature_c": temp(day.Temperature)})
		} else {
			days = append(days, contracts.Object{"datetime": at.UTC().Format(time.RFC3339Nano), "condition": day.Condition, "high_c": temp(day.Temperature), "low_c": temp(day.Templow)})
		}
	}
	if len(days) == 0 {
		return false
	}
	u.mu.Lock()
	if kind == "hourly" {
		u.hourly = days
		u.hourlyAt = time.Now()
	} else {
		u.forecast = days
		u.forecastAt = time.Now()
	}
	u.mu.Unlock()
	return true
}
func (u *Upstreams) runHourlyForecast(ctx context.Context) {
	if u.config.HA.Entities["weather"] == "" {
		return
	}
	for {
		delay := 30 * time.Minute
		if !u.pollForecastKind(ctx, "hourly") {
			delay = 5 * time.Minute
		}
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}
func (u *Upstreams) runWeatherForecast(ctx context.Context) {
	go u.runHourlyForecast(ctx)
	if u.config.HA.Entities["weather"] == "" {
		return
	}
	for {
		delay := 30 * time.Minute
		if !u.pollForecast(ctx) {
			delay = 5 * time.Minute
		}
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}
