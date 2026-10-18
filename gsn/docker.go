package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"path"
	"regexp"
	"strings"
	"sync"
	"time"
)

// Optional local socket source. The adapter only issues bounded inspect GETs.
type DockerConfig struct {
	Socket     string   `json:"socket"`
	Containers []string `json:"containers"`
}

var dockerName = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$`)

func validateDocker(c DockerConfig) error {
	if c.Socket == "" && len(c.Containers) == 0 {
		return nil
	}
	if len(c.Socket) > 240 || strings.ContainsAny(c.Socket, "\x00\r\n\t") || !path.IsAbs(c.Socket) || path.Clean(c.Socket) != c.Socket || len(c.Containers) == 0 || len(c.Containers) > 16 {
		return errors.New("Docker requires an absolute Unix socket path and 1-16 container names")
	}
	seen := map[string]bool{}
	for _, name := range c.Containers {
		if !dockerName.MatchString(name) || seen[name] {
			return errors.New("invalid or duplicate Docker container name")
		}
		seen[name] = true
	}
	return nil
}

type DockerItem struct {
	Name       string     `json:"name"`
	State      *string    `json:"state"`
	Running    *bool      `json:"running"`
	Health     *string    `json:"health"`
	ObservedAt *time.Time `json:"observed_at"`
	Error      *string    `json:"error"`
}
type DockerSnapshot struct {
	Enabled    bool         `json:"enabled"`
	Containers []DockerItem `json:"containers"`
}
type DockerSource struct {
	config DockerConfig
	client *http.Client
	mu     sync.Mutex
	items  []DockerItem
}

func NewDockerSource(c DockerConfig) *DockerSource {
	d := &DockerSource{config: c, items: []DockerItem{}}
	transport := &http.Transport{Proxy: nil, MaxIdleConns: 1, MaxIdleConnsPerHost: 1,
		DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
			return (&net.Dialer{}).DialContext(ctx, "unix", c.Socket)
		}}
	d.client = &http.Client{Transport: transport, Timeout: 800 * time.Millisecond, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	for _, name := range c.Containers {
		code := "docker_not_observed"
		d.items = append(d.items, DockerItem{Name: name, Error: &code})
	}
	return d
}
func dockerFailure(name, code string) DockerItem { return DockerItem{Name: name, Error: &code} }
func (d *DockerSource) inspect(ctx context.Context, name string) DockerItem {
	req, err := http.NewRequestWithContext(ctx, "GET", "http://docker/v1.41/containers/"+name+"/json", nil)
	if err != nil {
		return dockerFailure(name, "docker_request_invalid")
	}
	response, err := d.client.Do(req)
	if err != nil {
		return dockerFailure(name, "docker_unreachable")
	}
	defer response.Body.Close()
	if response.StatusCode == 404 {
		return dockerFailure(name, "docker_container_missing")
	}
	if response.StatusCode != 200 {
		return dockerFailure(name, "docker_http_error")
	}
	raw, err := io.ReadAll(io.LimitReader(response.Body, 65537))
	if err != nil {
		return dockerFailure(name, "docker_read_error")
	}
	if len(raw) > 65536 {
		return dockerFailure(name, "docker_payload_too_large")
	}
	// No environment variables, mount paths, command arguments or health logs are retained.
	var value struct {
		State *struct {
			Status  string
			Running *bool
			Health  *struct{ Status string }
		}
	}
	if json.Unmarshal(raw, &value) != nil || value.State == nil || value.State.Running == nil {
		return dockerFailure(name, "docker_payload_invalid")
	}
	state := value.State.Status
	valid := map[string]bool{"created": true, "running": true, "paused": true, "restarting": true, "removing": true, "exited": true, "dead": true}
	if !valid[state] || (*value.State.Running && state != "running" && state != "paused" && state != "restarting") || (!*value.State.Running && (state == "running" || state == "paused")) {
		return dockerFailure(name, "docker_state_invalid")
	}
	health := "none"
	if value.State.Health != nil {
		health = value.State.Health.Status
		if health != "healthy" && health != "unhealthy" && health != "starting" {
			return dockerFailure(name, "docker_health_invalid")
		}
	}
	now := time.Now().UTC()
	return DockerItem{Name: name, State: &state, Running: value.State.Running, Health: &health, ObservedAt: &now}
}
func (d *DockerSource) poll(ctx context.Context) {
	for i, name := range d.config.Containers {
		if ctx.Err() != nil {
			return
		}
		item := d.inspect(ctx, name)
		d.mu.Lock()
		d.items[i] = item
		d.mu.Unlock()
	}
}
func (d *DockerSource) Run(ctx context.Context) {
	if d.config.Socket == "" {
		return
	}
	defer d.client.CloseIdleConnections()
	for {
		d.poll(ctx)
		timer := time.NewTimer(15 * time.Second)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}
func (d *DockerSource) Snapshot(now time.Time) DockerSnapshot {
	d.mu.Lock()
	defer d.mu.Unlock()
	items := append([]DockerItem{}, d.items...)
	for i, item := range items {
		if item.ObservedAt != nil && now.Sub(*item.ObservedAt) > 45*time.Second {
			items[i] = dockerFailure(item.Name, "docker_observation_stale")
		}
	}
	return DockerSnapshot{Enabled: d.config.Socket != "", Containers: items}
}
