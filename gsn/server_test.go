package main

import (
	"bytes"
	"context"
	"encoding/json"
	"gso-status/contracts"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"
)

func config() Config {
	return Config{AgentID: "desktop", WriteToken: strings.Repeat("w", 32), ReadToken: strings.Repeat("r", 32)}
}
func fixture(t *testing.T) contracts.Object {
	t.Helper()
	b, e := os.ReadFile("../contracts/v1/telemetry-partial.json")
	if e != nil {
		t.Fatal(e)
	}
	o, e := contracts.Decode(b)
	if e != nil {
		t.Fatal(e)
	}
	o["sampled_at"] = time.Now().UTC().Format(time.RFC3339Nano)
	return o
}
func request(s *Server, method, path, token string, body []byte) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	r.Header.Set("Content-Type", "application/json")
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	return w
}
func TestContractAndSequence(t *testing.T) {
	c := config()
	s := NewServer(c)
	o := fixture(t)
	path := "/api/v1/agents/desktop/telemetry"
	b, _ := json.Marshal(o)
	for _, tc := range []struct {
		token  string
		status int
	}{{"", 401}, {c.ReadToken, 401}, {c.WriteToken, 200}, {c.WriteToken, 409}} {
		if w := request(s, "POST", path, tc.token, b); w.Code != tc.status {
			t.Fatalf("got %d want %d: %s", w.Code, tc.status, w.Body)
		}
	}
	w := request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil)
	if w.Code != 200 {
		t.Fatal(w.Code)
	}
	model, e := contracts.Decode(w.Body.Bytes())
	if e != nil || contracts.Load("snapshot").Validate(model) != nil {
		t.Fatal("invalid snapshot")
	}
	if object(object(object(model, "windows"), "cpu"), "load_percent")["value"].(json.Number) != "0" {
		t.Fatal("real zero lost")
	}
	s.lastPost = time.Time{}
	o["boot_id"] = "00000000-0000-4000-8000-000000000002"
	b, _ = json.Marshal(o)
	if request(s, "POST", path, c.WriteToken, b).Code != 200 {
		t.Fatal("new boot rejected")
	}
	s.lastPost = time.Time{}
	o["boot_id"] = "00000000-0000-4000-8000-000000000001"
	o["seq"] = 99
	b, _ = json.Marshal(o)
	if request(s, "POST", path, c.WriteToken, b).Code != 409 {
		t.Fatal("retired boot accepted")
	}
	s.mu.Lock()
	s.refresh(s.received.Add(11 * time.Second))
	s.mu.Unlock()
	w = request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil)
	model, _ = contracts.Decode(w.Body.Bytes())
	if object(object(model, "windows"), "source")["quality"] != "offline" {
		t.Fatal("TTL did not expire")
	}
}
func TestInvalidInputs(t *testing.T) {
	c := config()
	path := "/api/v1/agents/desktop/telemetry"
	cases := []struct {
		name   string
		mutate func(contracts.Object)
	}{
		{"icon oversized", func(o contracts.Object) { object(o, "game")["icon_argb8888"] = strings.Repeat("A", 3076) }},
		{"icon invalid base64", func(o contracts.Object) { object(o, "game")["icon_argb8888"] = "!" + strings.Repeat("A", 3071) }},
		{"percent", func(o contracts.Object) { object(object(object(o, "windows"), "cpu"), "load_percent")["value"] = 101 }},
		{"missing reason", func(o contracts.Object) { object(object(object(o, "windows"), "cpu"), "temperature_c")["reason"] = nil }},
		{"future", func(o contracts.Object) { o["sampled_at"] = time.Now().Add(time.Hour).Format(time.RFC3339Nano) }},
		{"invalid time", func(o contracts.Object) { o["sampled_at"] = "tomorrow" }},
		{"RAM", func(o contracts.Object) {
			object(object(object(o, "windows"), "cpu"), "memory_used_bytes")["value"] = 9e15
		}},
		{"unknown", func(o contracts.Object) { o["secret"] = true }},
		{"missing", func(o contracts.Object) { delete(o, "game") }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			s := NewServer(c)
			o := fixture(t)
			tc.mutate(o)
			b, _ := json.Marshal(o)
			if w := request(s, "POST", path, c.WriteToken, b); w.Code < 400 {
				t.Fatal("accepted invalid input")
			}
		})
	}
	for _, b := range [][]byte{[]byte(`{"seq":0,"seq":1}`), []byte(`{} {}`), {0xff}} {
		if request(NewServer(c), "POST", path, c.WriteToken, b).Code != 400 {
			t.Fatal("malformed input accepted")
		}
	}
	if request(NewServer(c), "POST", path, c.WriteToken, bytes.Repeat([]byte("x"), maxPayload+1)).Code != 413 {
		t.Fatal("body size limit")
	}
	if request(NewServer(c), "POST", "/api/v1/agents/other/telemetry", c.WriteToken, nil).Code != 403 {
		t.Fatal("identity not bound")
	}
}

type chunkReader struct {
	data  []byte
	chunk int
	read  int
}

func (r *chunkReader) Read(b []byte) (int, error) {
	if len(r.data) == 0 {
		return 0, io.EOF
	}
	n := min(len(b), r.chunk, len(r.data))
	copy(b, r.data[:n])
	r.data = r.data[n:]
	r.read += n
	return n, nil
}
func TestRouterPrefix(t *testing.T) {
	for _, chunk := range []int{1, 7, 1024} {
		r := &chunkReader{data: []byte(strings.Repeat(" ", 1020) + `var wan_rate="12;34";` + strings.Repeat("x", 20000)), chunk: chunk}
		down, up, n, e := readWAN(r)
		if e != nil || down != 34 || up != 12 || n > 2048 || r.read > 2048 {
			t.Fatalf("chunk %d: %d %d %d %v", chunk, down, up, n, e)
		}
	}
	for _, input := range []string{`var wan_rate="0;0"`, `var wan_rate='5;7'`} {
		if _, _, _, e := readWAN(strings.NewReader(input)); e != nil {
			t.Fatal(e)
		}
	}
	for _, input := range []string{`var wan_rate="1;x"`, `var wan_rate="-1;2"`, `var wan_rate="1;2`, strings.Repeat("x", 8192) + `var wan_rate="1;2"`} {
		if _, _, n, e := readWAN(strings.NewReader(input)); e == nil || n > 8192 {
			t.Fatal("invalid prefix accepted")
		}
	}
}
func TestSingleSourceFailureDoesNotBlockCache(t *testing.T) {
	c := config()
	c.RouterURL = "http://127.0.0.1:1/mobile.asp"
	s := NewServer(c)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go s.Run(ctx)
	for range 30 {
		w := request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil)
		if w.Code != 200 {
			t.Fatal(w.Code)
		}
	}
}
func BenchmarkSnapshot(b *testing.B) {
	s := NewServer(config())
	b.ReportAllocs()
	for b.Loop() {
		request(s, "GET", "/api/v1/snapshot", s.config.ReadToken, nil)
	}
}
func TestRouterLive(t *testing.T) {
	if os.Getenv("GSO_LIVE_ROUTER") != "1" {
		t.Skip("explicit opt-in read-only BX54 test")
	}
	r := NewRouter("http://192.168.124.1/mobile.asp")
	for range 2 {
		start := time.Now()
		if !r.poll(context.Background()) {
			t.Fatal(r.errorCode)
		}
		if r.readBytes > 8192 {
			t.Fatal("prefix exceeded")
		}
		t.Logf("application_read_bytes=%d elapsed_ms=%.2f", r.readBytes, float64(time.Since(start).Microseconds())/1000)
		time.Sleep(time.Second)
	}
}

func TestRouterHTTPFailureDetails(t *testing.T) {
	for _, status := range []int{302, 401, 403, 500} {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(status) }))
		router := NewRouter(server.URL)
		if router.poll(context.Background()) || router.errorCode != "router_http_"+strconv.Itoa(status) {
			t.Fatalf("status %d: %s", status, router.errorCode)
		}
		server.Close()
	}
}

func TestRouterSafeRedirects(t *testing.T) {
	loginHits := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/mobile.asp":
			http.Redirect(w, r, "/status.asp", 302)
		case "/status.asp":
			io.WriteString(w, `var wan_rate="12;34"`)
		case "/protected.asp":
			http.Redirect(w, r, "/login.asp?secret=do-not-log", 302)
		case "/login.asp":
			loginHits++
			io.WriteString(w, "login")
		case "/loop.asp":
			http.Redirect(w, r, "/loop.asp", 302)
		}
	}))
	defer server.Close()
	router := NewRouter(server.URL + "/mobile.asp")
	if !router.poll(context.Background()) || router.down != 34 || router.up != 12 {
		t.Fatalf("same origin redirect: %s", router.errorCode)
	}
	router = NewRouter(server.URL + "/protected.asp")
	if router.poll(context.Background()) || router.errorCode != "router_http_302_login_required" || loginHits != 0 {
		t.Fatalf("login redirect: %s", router.errorCode)
	}
	router = NewRouter(server.URL + "/loop.asp")
	if router.poll(context.Background()) || router.errorCode != "router_http_302" {
		t.Fatal("redirect loop accepted")
	}
	foreignHits := 0
	foreign := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { foreignHits++ }))
	defer foreign.Close()
	external := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, foreign.URL, 302) }))
	defer external.Close()
	router = NewRouter(external.URL)
	if router.poll(context.Background()) || foreignHits != 0 {
		t.Fatal("cross origin redirect followed")
	}
}

func TestSessionMetadataRoundTrip(t *testing.T) {
	c := config()
	s := NewServer(c)
	o := fixture(t)
	session := contracts.Object{"id": "active", "title": "Current work", "state": "running", "title_reason": nil, "started_at": "2026-10-16T00:00:00Z", "running_duration_ms": float64(321000), "duration_reason": nil}
	object(o, "codex")["state"] = "running"
	object(o, "codex")["sessions"] = []any{session}
	b, _ := json.Marshal(o)
	w := request(s, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, b)
	if w.Code != 200 {
		t.Fatalf("metadata rejected: %d %s", w.Code, w.Body)
	}
	if w.Header().Get("X-GSO-Session-Metadata") != "1" {
		t.Fatal("missing compatibility capability")
	}
	w = request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil)
	var result contracts.Object
	if e := json.Unmarshal(w.Body.Bytes(), &result); e != nil {
		t.Fatal(e)
	}
	actual := object(result, "codex")["sessions"].([]any)[0].(map[string]any)
	if actual["title"] != "Current work" || actual["running_duration_ms"] != float64(321000) {
		t.Fatal("session metadata lost")
	}
	session["running_duration_ms"] = float64(-1)
	o["seq"] = float64(2)
	b, _ = json.Marshal(o)
	if w = request(s, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, b); w.Code != 400 {
		t.Fatal("negative duration accepted")
	}
}

// GSW 0.1.17 adds busy_percent, read_ms and write_ms to each disk, 0.1.18 response_ms and the read/write byte rates; GSN advertises that it takes them and passes them on.
func TestDiskActivityRoundTrip(t *testing.T) {
	c := config()
	s := NewServer(c)
	o := fixture(t)
	disk := object(o, "windows")["disks"].([]any)[0].(contracts.Object)
	if number(object(disk, "busy_percent")["value"]) != 12.5 {
		t.Fatalf("fixture must carry disk activity: %v", disk)
	}
	b, _ := json.Marshal(o)
	w := request(s, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, b)
	if w.Code != 200 || w.Header().Get("X-GSO-Disk-Activity") != "1" || w.Header().Get("X-GSO-Disk-Throughput") != "1" {
		t.Fatalf("disk activity must be accepted and advertised: %d %v", w.Code, w.Header())
	}
	var model contracts.Object
	if json.Unmarshal(request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil).Body.Bytes(), &model) != nil {
		t.Fatal("snapshot is not JSON")
	}
	d := object(model, "windows")["disks"].([]any)[0].(contracts.Object)
	if number(object(d, "busy_percent")["value"]) != 12.5 || number(object(d, "read_ms")["value"]) != 0.4 || object(d, "write_ms")["reason"] != "disk_no_io" || number(object(d, "response_ms")["value"]) != 0.4 || number(object(d, "read_bytes_per_second")["value"]) != 5242880 {
		t.Fatalf("disk activity lost in the snapshot: %v", d)
	}
	object(disk, "busy_percent")["value"] = 101.0
	b, _ = json.Marshal(o)
	if w := request(s, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, b); w.Code == 200 {
		t.Fatal("accepted a busy percentage above 100")
	}
}
