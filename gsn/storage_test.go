package main

import (
	"encoding/json"
	"errors"
	"testing"
	"time"

	"gso-status/contracts"
)

func volumeValue(t *testing.T, snapshot contracts.Object, i int, field string) any {
	t.Helper()
	return snapshot["volumes"].([]any)[i].(contracts.Object)[field].(contracts.Object)["value"]
}

func TestStorageSnapshot(t *testing.T) {
	now := time.Now()
	empty := NewStorageSource(nil).Snapshot(now)
	if object(empty, "source")["quality"] != "unconfigured" || len(empty["volumes"].([]any)) != 0 {
		t.Fatalf("unconfigured storage must be empty: %v", empty)
	}
	s := NewStorageSource([]StorageVolume{{Label: "hdd0", Path: "/a"}, {Label: "usb3", Path: "/b"}})
	s.read = func(path string) (uint64, uint64, error) {
		if path == "/b" {
			return 0, 0, errors.New("not mounted")
		}
		return 4000, 1500, nil
	}
	if q := object(s.Snapshot(now), "source")["quality"]; q != "unconfigured" {
		t.Fatalf("before the first sample the source is waiting, got %v", q)
	}
	s.Sample(now)
	snap := s.Snapshot(now.Add(time.Second))
	if object(snap, "source")["quality"] != "ok" || volumeValue(t, snap, 0, "total_bytes") != float64(4000) || volumeValue(t, snap, 0, "free_bytes") != float64(1500) {
		t.Fatalf("sampled volume: %v", snap)
	}
	if volumeValue(t, snap, 1, "total_bytes") != nil {
		t.Fatalf("a volume that cannot be read must not invent capacity: %v", snap)
	}
	if object(snap, "source")["last_error_code"] != nil {
		t.Fatalf("one readable volume keeps the source healthy: %v", snap)
	}
	if q := object(s.Snapshot(now.Add(2*time.Minute)), "source")["quality"]; q != "stale" {
		t.Fatalf("90 s without a sample is stale, got %v", q)
	}
	if q := object(s.Snapshot(now.Add(4*time.Minute)), "source")["quality"]; q != "offline" {
		t.Fatalf("180 s without a sample is offline, got %v", q)
	}
	s.read = func(string) (uint64, uint64, error) { return 100, 200, nil }
	s.Sample(now.Add(time.Minute))
	if source := object(s.Snapshot(now.Add(time.Minute)), "source"); source["quality"] != "ok" || source["last_error_code"] != "storage_unavailable" {
		t.Fatalf("a failed sample keeps the last good time but reports the error: %v", source)
	}
	if volumeValue(t, s.Snapshot(now.Add(time.Minute)), 0, "total_bytes") != nil {
		t.Fatal("free larger than total is rejected, not displayed")
	}
}

func TestStorageConfigAndServer(t *testing.T) {
	for _, bad := range [][]StorageVolume{
		{{Label: "", Path: "/a"}},
		{{Label: "a", Path: ""}},
		{{Label: "a", Path: "/a"}, {Label: "A", Path: "/b"}},
		{{Label: "bad/label", Path: "/a"}},
		{{Label: "a", Path: "/a"}, {Label: "b", Path: "/b"}, {Label: "c", Path: "/c"}},
	} {
		if validateStorage(bad) == nil {
			t.Fatalf("accepted %v", bad)
		}
	}
	if validateStorage([]StorageVolume{{Label: "hdd0", Path: "/app/config"}, {Label: "usb3", Path: "/app/usb3"}}) != nil {
		t.Fatal("two NAS volumes must be accepted")
	}
	c := config()
	c.Storage = []StorageVolume{{Label: "hdd0", Path: t.TempDir()}}
	s := NewServer(c)
	s.storage.Sample(time.Now())
	o := fixture(t)
	o["codex"].(contracts.Object)["clients"] = []any{
		contracts.Object{"id": "codex", "online": false, "tokens_today": nil},
		contracts.Object{"id": "claude", "online": true, "tokens_today": float64(1234567)},
	}
	b, _ := json.Marshal(o)
	w := request(s, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, b)
	if w.Code != 200 || w.Header().Get("X-GSO-Vibe-Clients") != "1" {
		t.Fatalf("client facts must be accepted and advertised: %d %v", w.Code, w.Header())
	}
	snap := request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil)
	var model contracts.Object
	if json.Unmarshal(snap.Body.Bytes(), &model) != nil {
		t.Fatal("snapshot is not JSON")
	}
	volumes := object(model, "storage")["volumes"].([]any)
	if len(volumes) != 1 || volumes[0].(contracts.Object)["label"] != "hdd0" || object(model, "storage")["source"].(contracts.Object)["quality"] != "ok" {
		t.Fatalf("snapshot storage: %v", model["storage"])
	}
	if clients := object(model, "codex")["clients"].([]any); len(clients) != 2 || clients[1].(contracts.Object)["tokens_today"] != float64(1234567) {
		t.Fatalf("snapshot clients: %v", object(model, "codex")["clients"])
	}
}
