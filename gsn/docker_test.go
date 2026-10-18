package main

import (
	"context"
	"encoding/json"
	"gso-status/contracts"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

type dockerTransport func(*http.Request) (*http.Response, error)

func (f dockerTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }
func dockerClient(t *testing.T, body string, status int, calls *int) *http.Client {
	return &http.Client{Transport: dockerTransport(func(r *http.Request) (*http.Response, error) {
		t.Helper()
		*calls++
		if r.Method != "GET" || r.URL.String() != "http://docker/v1.41/containers/ha/json" {
			t.Fatalf("unexpected daemon operation: %s %s", r.Method, r.URL)
		}
		return &http.Response{StatusCode: status, Body: io.NopCloser(strings.NewReader(body)), Header: make(http.Header)}, nil
	})}
}
func TestDockerHealthAndFailureIsolation(t *testing.T) {
	for _, test := range []struct {
		name, body, want string
		status           int
	}{
		{"healthy", `{"State":{"Status":"running","Running":true,"Health":{"Status":"healthy","Log":[{"Output":"private-health-log"}]}},"Config":{"Env":["PRIVATE=secret"]}}`, "healthy", 200},
		{"no healthcheck", `{"State":{"Status":"running","Running":true}}`, "none", 200},
		{"exited", `{"State":{"Status":"exited","Running":false}}`, "none", 200},
		{"unhealthy", `{"State":{"Status":"running","Running":true,"Health":{"Status":"unhealthy"}}}`, "unhealthy", 200},
		{"starting", `{"State":{"Status":"running","Running":true,"Health":{"Status":"starting"}}}`, "starting", 200},
		{"missing", "", "docker_container_missing", 404},
		{"authorization", "", "docker_http_error", 403},
		{"invalid", "{}", "docker_payload_invalid", 200},
		{"contradiction", `{"State":{"Status":"running","Running":false}}`, "docker_state_invalid", 200},
		{"bad health", `{"State":{"Status":"running","Running":true,"Health":{"Status":"invented"}}}`, "docker_health_invalid", 200},
		{"large", strings.Repeat("x", 65537), "docker_payload_too_large", 200},
	} {
		t.Run(test.name, func(t *testing.T) {
			d := NewDockerSource(DockerConfig{Socket: "/var/run/docker.sock", Containers: []string{"ha"}})
			calls := 0
			d.client = dockerClient(t, test.body, test.status, &calls)
			d.poll(context.Background())
			snapshot := d.Snapshot(time.Now())
			item := snapshot.Containers[0]
			if strings.HasPrefix(test.want, "docker_") {
				if item.Error == nil || *item.Error != test.want || item.State != nil || item.Running != nil || item.ObservedAt != nil {
					t.Fatalf("failure was presented as data: %+v", item)
				}
			} else if item.Health == nil || *item.Health != test.want || item.Error != nil || item.ObservedAt == nil {
				t.Fatalf("health mismatch: %+v", item)
			}
			raw, _ := json.Marshal(snapshot)
			model, err := contracts.Decode(raw)
			if err != nil || contracts.Load("docker").Validate(model) != nil {
				t.Fatalf("invalid Docker contract: %s", raw)
			}
			if strings.Contains(string(raw), "secret") || strings.Contains(string(raw), "private-health-log") {
				t.Fatal("private daemon fields leaked")
			}
			for i := 0; i < 20; i++ {
				d.Snapshot(time.Now())
			}
			if calls != 1 {
				t.Fatal("snapshot reads caused upstream requests")
			}
			if item.ObservedAt != nil {
				stale := d.Snapshot(time.Now().Add(time.Minute)).Containers[0]
				if stale.Error == nil || *stale.Error != "docker_observation_stale" || stale.Running != nil {
					t.Fatal("stale data remains current")
				}
			}
		})
	}
}
func TestDockerOptionalEndpointAndConfig(t *testing.T) {
	for _, c := range []DockerConfig{{Socket: "http://localhost:2375", Containers: []string{"ha"}}, {Socket: "/var/run/docker.sock", Containers: []string{"../ha"}}, {Socket: "/var/run/docker.sock", Containers: []string{"ha", "ha"}}, {Socket: "/var/run/../docker.sock", Containers: []string{"ha"}}, {Containers: []string{"ha"}}} {
		if validateDocker(c) == nil {
			t.Fatal("invalid config accepted", c)
		}
	}
	if validateDocker(DockerConfig{}) != nil || validateDocker(DockerConfig{Socket: "/var/run/docker.sock", Containers: []string{"ha"}}) != nil {
		t.Fatal("valid optional configuration rejected")
	}
	s := NewServer(config())
	for _, test := range []struct {
		method, token string
		want          int
	}{{"GET", "", 401}, {"GET", config().ReadToken, 200}, {"POST", config().ReadToken, 405}, {"GET", config().WriteToken, 401}} {
		request := httptest.NewRequest(test.method, "/api/v1/docker", nil)
		request.Header.Set("Authorization", "Bearer "+test.token)
		response := httptest.NewRecorder()
		s.ServeHTTP(response, request)
		if response.Code != test.want {
			t.Fatalf("%s: %d", test.method, response.Code)
		}
		if response.Code == 200 {
			var value DockerSnapshot
			if json.Unmarshal(response.Body.Bytes(), &value) != nil || value.Enabled || len(value.Containers) != 0 {
				t.Fatal("unconfigured Docker must be disabled")
			}
		}
	}
}

func TestDockerDeadlineAndRecovery(t *testing.T) {
	d := NewDockerSource(DockerConfig{Socket: "/var/run/docker.sock", Containers: []string{"ha"}})
	d.client = &http.Client{Timeout: 20 * time.Millisecond, Transport: dockerTransport(func(r *http.Request) (*http.Response, error) { <-r.Context().Done(); return nil, r.Context().Err() })}
	started := time.Now()
	d.poll(context.Background())
	if time.Since(started) > time.Second {
		t.Fatal("Docker deadline blocked collection")
	}
	if item := d.Snapshot(time.Now()).Containers[0]; item.Error == nil || *item.Error != "docker_unreachable" {
		t.Fatal("timeout was not explicit")
	}
	calls := 0
	d.client = dockerClient(t, `{"State":{"Status":"running","Running":true}}`, 200, &calls)
	d.poll(context.Background())
	item := d.Snapshot(time.Now()).Containers[0]
	if item.Error != nil || item.Running == nil || !*item.Running {
		t.Fatal("source failed to recover")
	}
	cancelled, cancel := context.WithCancel(context.Background())
	cancel()
	d.poll(cancelled)
	if calls != 1 {
		t.Fatal("cancelled poll queried Docker")
	}
}
