package main

import (
	"path/filepath"
	"testing"
	"time"
)

// smartTable builds a SMART READ DATA block with the given attributes (id -> raw value) and a valid checksum.
func smartTable(raw map[byte]uint64) []byte {
	data := make([]byte, 512)
	i := 2
	for id, n := range raw {
		data[i], data[i+3], data[i+4] = id, 100, 100
		for k := 0; k < 6; k++ {
			data[i+5+k] = byte(n >> (8 * k))
		}
		i += 12
	}
	var sum byte
	for _, b := range data[:511] {
		sum += b
	}
	data[511] = -sum
	return data
}

func TestParseSmart(t *testing.T) {
	r, err := parseSmart(smartTable(map[byte]uint64{194: 41, 9: 11234, 12: 56, 193: 3000, 5: 0, 197: 0, 198: 0, 199: 2}))
	if err != nil || !r.hasTemp || r.celsius != 41 {
		t.Fatalf("temperature: %+v %v", r, err)
	}
	h := r.health
	if h.hours != (smartCount{11234, true}) || h.powerCycles != (smartCount{56, true}) || h.loadCycles != (smartCount{3000, true}) || h.crc != (smartCount{2, true}) ||
		h.reallocated != (smartCount{0, true}) || h.pending != (smartCount{0, true}) || h.verdict() != healthGood {
		t.Fatalf("health: %+v", h)
	}
	// Seagate-style 48-bit raw values keep the counter in the low bytes; pending and uncorrectable add up.
	r, _ = parseSmart(smartTable(map[byte]uint64{5: 0x0003_0000_0007, 197: 2, 198: 1}))
	if r.health.reallocated.n != 7 || r.health.pending.n != 3 || r.hasTemp {
		t.Fatalf("raw bytes: %+v", r)
	}
	bad := smartTable(map[byte]uint64{5: 1})
	bad[100]++
	if _, err := parseSmart(bad); err == nil {
		t.Fatal("a block with a wrong checksum is refused")
	}
	if _, err := parseSmart(make([]byte, 100)); err == nil {
		t.Fatal("a short block is refused")
	}
}

func TestSmartVerdict(t *testing.T) {
	count := func(n uint64) smartCount { return smartCount{n, true} }
	for _, c := range []struct {
		h    smartHealth
		want string
	}{
		{smartHealth{reallocated: count(0), pending: count(0)}, healthGood},
		{smartHealth{reallocated: count(3), pending: count(0)}, healthWatch},
		{smartHealth{reallocated: count(0), pending: count(1)}, healthWatch},
		{smartHealth{reallocated: count(99), pending: count(9)}, healthWatch},
		{smartHealth{reallocated: count(100)}, healthBad},
		{smartHealth{pending: count(10)}, healthBad},
		{smartHealth{reallocated: count(0), pending: count(0), failing: true, statusKnown: true}, healthBad},
		{smartHealth{}, healthUnknown},
		{smartHealth{statusKnown: true}, healthGood},
		{smartHealth{crc: count(9000), reallocated: count(0), pending: count(0)}, healthGood},
	} {
		if got := c.h.verdict(); got != c.want {
			t.Fatalf("%+v: %s, want %s", c.h, got, c.want)
		}
	}
}

func TestNasHealthInSnapshot(t *testing.T) {
	now := time.Now()
	s := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	s.cfg = &NasConfig{Disk: "/dev/sda", DiskIntervalSeconds: 60}
	s.power = func(string) string { return diskActive }
	if _, ok := s.Snapshot(now)["disk_health"]; ok {
		t.Fatal("no health before the first read")
	}
	reads := 0
	s.smartFull = func(string) (smartReading, error) {
		reads++
		return smartReading{celsius: 38, hasTemp: true, health: smartHealth{hours: smartCount{8760, true}, reallocated: smartCount{4, true}, pending: smartCount{0, true}, powerCycles: smartCount{12, true}}}, nil
	}
	stat := filepath.Join(s.sys, "block/sda/stat")
	writeFake(t, stat, "     100 1 2 3      200 4 5 6 0 7 8\n")
	s.Sample(now)
	snap := s.Snapshot(now)
	if reads != 1 || snap["disk_health"] != "watch" || nasValue(snap, "disk_power_on_hours") != 8760.0 || nasValue(snap, "disk_reallocated_sectors") != 4.0 ||
		nasValue(snap, "disk_pending_sectors") != 0.0 || nasValue(snap, "disk_power_cycles") != 12.0 || nasReason(snap, "disk_load_cycles") != "not_supported" || nasReason(snap, "disk_crc_errors") != "not_supported" {
		t.Fatalf("health fields: reads=%d %v", reads, snap)
	}
	// The counters stay on screen after the temperature has expired (the disk fell asleep).
	late := s.Snapshot(now.Add(2 * time.Hour))
	if late["disk_health"] != "watch" || nasValue(late, "disk_power_on_hours") != 8760.0 {
		t.Fatalf("health is kept: %v", late)
	}
}
