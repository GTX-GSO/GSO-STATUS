package main

import (
	"encoding/json"
	"gso-status/contracts"
	"os"
	"testing"
	"time"
)

// Benchmarks of the snapshot path (refresh once a second, telemetry POST, GET of the cached payload):
// go test -run '^$' -bench 'Refresh|Validate|Serve|Post' -benchmem ./gsn
func benchTelemetry(b *testing.B) contracts.Object {
	b.Helper()
	raw, err := os.ReadFile("../contracts/v1/telemetry-partial.json")
	if err != nil {
		b.Fatal(err)
	}
	o, err := contracts.Decode(raw)
	if err != nil {
		b.Fatal(err)
	}
	return o
}
func benchServer(b *testing.B) *Server {
	b.Helper()
	c := config()
	s := NewServer(c)
	o := benchTelemetry(b)
	o["sampled_at"] = time.Now().UTC().Format(time.RFC3339Nano)
	payload, _ := json.Marshal(o)
	if w := request(s, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, payload); w.Code != 200 {
		b.Fatalf("seed telemetry rejected: %d %s", w.Code, w.Body)
	}
	return s
}
func BenchmarkRefresh(b *testing.B) {
	s := benchServer(b)
	now := time.Now()
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		s.mu.Lock()
		s.refresh(now.Add(time.Duration(i) * time.Second))
		s.mu.Unlock()
	}
}
func BenchmarkValidateSnapshot(b *testing.B) {
	s := benchServer(b)
	model, err := contracts.Decode(s.payload)
	if err != nil {
		b.Fatal(err)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if err := s.schema.Validate(model); err != nil {
			b.Fatal(err)
		}
	}
}
func BenchmarkServeSnapshot(b *testing.B) {
	s := benchServer(b)
	c := config()
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if w := request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil); w.Code != 200 {
			b.Fatal(w.Code)
		}
	}
}
func BenchmarkTelemetryPost(b *testing.B) {
	s := benchServer(b)
	c := config()
	o := benchTelemetry(b)
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		o["seq"] = float64(1000 + i)
		o["sampled_at"] = time.Now().UTC().Format(time.RFC3339Nano)
		payload, _ := json.Marshal(o)
		s.mu.Lock()
		s.lastPost = time.Time{}
		s.mu.Unlock()
		if w := request(s, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, payload); w.Code != 200 {
			b.Fatalf("%d %s", w.Code, w.Body)
		}
	}
}
