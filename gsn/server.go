package main

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"io"
	"mime"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"gso-status/contracts"
)

const maxPayload = 32768

var agentPattern = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)

type Server struct {
	config            Config
	mu                sync.Mutex
	schema, telemetry *contracts.Schema
	snapshot          contracts.Object
	payload           []byte
	boot              string
	seq               uint64
	retired           map[string]bool
	received          time.Time
	tokens            chan struct{}
	lastPost          time.Time
	router            *Router
	upstreams         *Upstreams
	home              *HomeCommands
	docker            *DockerSource
	storage           *StorageSource
	nas               *NasSource
	printer           *PrinterSource
}

func NewServer(c Config) *Server {
	s := &Server{config: c, schema: contracts.Load("snapshot"), telemetry: contracts.Load("telemetry"), retired: map[string]bool{}, tokens: make(chan struct{}, 8)}
	s.snapshot = s.schema.Empty()
	s.upstreams = NewUpstreams(c)
	s.home = NewHomeCommands(c, s.upstreams)
	s.docker = NewDockerSource(c.Docker)
	s.storage = NewStorageSource(c.Storage)
	s.nas = NewNasSource(c.NAS)
	s.printer = NewPrinterSource(c.Printer)
	if c.RouterURL != "" {
		s.router = NewRouter(c.RouterURL)
	}
	s.refresh(time.Now())
	return s
}
func (s *Server) Run(ctx context.Context) {
	go s.upstreams.Run(ctx)
	go s.home.Run(ctx)
	go s.docker.Run(ctx)
	go s.storage.Run(ctx)
	go s.nas.Run(ctx)
	go s.printer.Run(ctx)
	if s.router != nil {
		go s.router.Run(ctx)
	}
	t := time.NewTicker(time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-t.C:
			s.mu.Lock()
			s.refresh(now)
			s.mu.Unlock()
		}
	}
}
func object(o contracts.Object, k string) contracts.Object { return o[k].(contracts.Object) }
func number(v any) float64 {
	switch n := v.(type) {
	case json.Number:
		f, _ := n.Float64()
		return f
	case float64:
		return n
	case int:
		return float64(n)
	}
	return 0
}
func ageSource(source contracts.Object, elapsed float64) {
	ageSourceTTL(source, elapsed, 3000, 10000)
}
func ageSourceTTL(source contracts.Object, elapsed, stale, offline float64) {
	if source["age_ms"] == nil {
		return
	}
	age := number(source["age_ms"]) + elapsed
	source["age_ms"] = age
	q := source["quality"]
	if q == "ok" || q == "stale" || q == "offline" {
		if age > offline {
			source["quality"] = "offline"
		} else if age > stale && q == "ok" {
			source["quality"] = "stale"
		}
	}
}
func (s *Server) refresh(now time.Time) {
	// Work on a copy: source age must always derive from receive time, not accumulate each tick.
	b, _ := json.Marshal(s.snapshot)
	var model contracts.Object
	json.Unmarshal(b, &model)
	upstream := s.upstreams.Snapshot(now)
	for _, key := range []string{"net", "weather", "sun", "home", "ambient", "car"} {
		model[key] = upstream[key]
	}
	model["storage"] = s.storage.Snapshot(now)
	model["nas"] = s.nas.Snapshot(now)
	model["printer"] = s.printer.Snapshot(now)
	if !s.received.IsZero() {
		elapsed := float64(now.Sub(s.received).Milliseconds())
		for _, key := range []string{"windows", "game", "codex"} {
			src := object(object(model, key), "source")
			if key == "codex" {
				ageSourceTTL(src, elapsed, 180000, 600000)
			} else {
				ageSource(src, elapsed)
			}
		}
	}
	if s.router != nil {
		model["net"].(contracts.Object)["wan"] = s.router.Snapshot(now)
	}
	if s.config.HomeControlToken != "" {
		object(model, "home")["control_epoch"] = s.home.epoch
	}
	model["generated_at"] = now.UTC().Format(time.RFC3339Nano)
	if s.schema.Validate(model) != nil {
		return
	}
	b, err := json.Marshal(model)
	if err == nil && len(b) <= maxPayload {
		s.payload = b
	}
}
func fail(w http.ResponseWriter, status int, code string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(map[string]any{"error": map[string]string{"code": code, "message": code}})
}
func auth(r *http.Request, token string) bool {
	return token != "" && subtle.ConstantTimeCompare([]byte(r.Header.Get("Authorization")), []byte("Bearer "+token)) == 1
}
func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-GSO-Session-Metadata", "1")
	w.Header().Set("X-GSO-Hardware-Clocks", "1")
	w.Header().Set("X-GSO-Game-Icon", "1")
	w.Header().Set("X-GSO-Vibe-Clients", "1")
	w.Header().Set("X-GSO-Disk-Activity", "1")
	w.Header().Set("X-GSO-Disk-Throughput", "1")
	select {
	case s.tokens <- struct{}{}:
		defer func() { <-s.tokens }()
	default:
		fail(w, 429, "too_many_requests")
		return
	}
	if r.URL.Path == "/healthz" && r.Method == "GET" {
		w.Write([]byte("ok\n"))
		return
	}
	if r.URL.Path == "/readyz" && r.Method == "GET" {
		if !auth(r, s.config.ReadToken) {
			fail(w, 401, "unauthorized")
			return
		}
		s.mu.Lock()
		ready := len(s.payload) > 0
		s.mu.Unlock()
		if !ready {
			fail(w, 503, "not_ready")
		} else {
			w.Write([]byte("ready\n"))
		}
		return
	}
	if r.URL.Path == "/api/v1/docker" {
		if !auth(r, s.config.ReadToken) {
			fail(w, 401, "unauthorized")
			return
		}
		if r.Method != "GET" {
			w.Header().Set("Allow", "GET")
			fail(w, 405, "method_not_allowed")
			return
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(s.docker.Snapshot(time.Now()))
		return
	}
	if r.URL.Path == "/api/v1/printer/thumbnail" && r.Method == "GET" {
		if !auth(r, s.config.ReadToken) {
			fail(w, 401, "unauthorized")
			return
		}
		id, picture := s.printer.Thumbnail(time.Now())
		if picture == nil || (r.URL.Query().Get("id") != "" && r.URL.Query().Get("id") != id) {
			fail(w, 404, "no_thumbnail")
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("X-GSO-Thumbnail-Id", id)
		w.Header().Set("Content-Length", strconv.Itoa(len(picture)))
		w.Write(picture)
		return
	}
	if r.URL.Path == "/api/v1/snapshot" && r.Method == "GET" {
		if !auth(r, s.config.ReadToken) {
			fail(w, 401, "unauthorized")
			return
		}
		s.mu.Lock()
		b := s.payload
		s.mu.Unlock()
		if len(b) == 0 {
			fail(w, 503, "not_ready")
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write(b)
		return
	}
	if strings.HasPrefix(r.URL.Path, "/api/v1/home/commands") {
		s.serveHome(w, r)
		return
	}
	if strings.HasPrefix(r.URL.Path, "/api/v1/agents/") && r.Method == "POST" {
		if !auth(r, s.config.WriteToken) {
			fail(w, 401, "unauthorized")
			return
		}
		if r.URL.Path != "/api/v1/agents/"+s.config.AgentID+"/telemetry" {
			fail(w, 403, "agent_forbidden")
			return
		}
		content, _, e := mime.ParseMediaType(r.Header.Get("Content-Type"))
		if e != nil || content != "application/json" {
			fail(w, 415, "unsupported_media_type")
			return
		}
		r.Body = http.MaxBytesReader(w, r.Body, maxPayload)
		b, e := io.ReadAll(r.Body)
		if e != nil {
			fail(w, 413, "payload_too_large")
			return
		}
		model, e := contracts.Decode(b)
		if e != nil || s.telemetry.Validate(model) != nil || !crossValid(model) {
			fail(w, 400, "invalid_payload")
			return
		}
		sampled, e := time.Parse(time.RFC3339Nano, model["sampled_at"].(string))
		if e != nil || time.Since(sampled) > 30*time.Second || time.Until(sampled) > 30*time.Second {
			fail(w, 409, "sample_clock_skew")
			return
		}
		seq := uint64(number(model["seq"]))
		boot := model["boot_id"].(string)
		s.mu.Lock()
		defer s.mu.Unlock()
		if s.retired[boot] || (s.boot == boot && seq <= s.seq) {
			fail(w, 409, "old_sequence")
			return
		}
		if s.boot != "" && s.boot != boot && len(s.retired) >= 256 {
			fail(w, 409, "boot_history_full")
			return
		}
		if time.Since(s.lastPost) < 100*time.Millisecond {
			fail(w, 429, "report_rate_exceeded")
			return
		}
		// At most one snapshot: clients never queue historical telemetry.
		candidate := s.schema.Empty()
		upstream := s.upstreams.Snapshot(time.Now())
		for _, key := range []string{"net", "weather", "sun", "home", "ambient", "car"} {
			candidate[key] = upstream[key]
		}
		candidate["storage"] = s.storage.Snapshot(time.Now())
		candidate["nas"] = s.nas.Snapshot(time.Now())
		candidate["printer"] = s.printer.Snapshot(time.Now())
		for _, key := range []string{"windows", "game", "codex"} {
			candidate[key] = model[key]
		}
		if s.router != nil {
			object(candidate, "net")["wan"] = s.router.Snapshot(time.Now())
		}
		encoded, _ := json.Marshal(candidate)
		if len(encoded) > maxPayload-128 {
			fail(w, 413, "snapshot_too_large")
			return
		}
		if s.boot != "" && s.boot != boot {
			s.retired[s.boot] = true
		}
		s.boot = boot
		s.seq = seq
		s.received = time.Now()
		s.lastPost = s.received
		for _, key := range []string{"windows", "game", "codex"} {
			s.snapshot[key] = model[key]
		}
		s.refresh(s.received)
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"accepted":true}`))
		return
	}
	fail(w, 404, "not_found")
}
func crossValid(model contracts.Object) bool {
	win := object(model, "windows")
	for _, key := range []string{"cpu", "gpu"} {
		h := object(win, key)
		used, total := object(h, "memory_used_bytes")["value"], object(h, "memory_total_bytes")["value"]
		if used != nil && total != nil && number(used) > number(total) {
			return false
		}
	}
	ids := map[string]bool{}
	for _, entry := range win["disks"].([]any) {
		d := entry.(contracts.Object)
		id := d["id"].(string)
		if ids[id] {
			return false
		}
		ids[id] = true
		free, total := object(d, "free_bytes")["value"], object(d, "total_bytes")["value"]
		if free != nil && total != nil && number(free) > number(total) {
			return false
		}
	}
	quotas := map[string]bool{}
	for _, entry := range object(model, "codex")["quotas"].([]any) {
		q := entry.(contracts.Object)
		b, _ := json.Marshal([]any{q["bucket"], q["window_duration_mins"]})
		id := string(b)
		if quotas[id] {
			return false
		}
		quotas[id] = true
	}
	return true
}
