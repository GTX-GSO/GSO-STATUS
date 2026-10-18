package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"

	"gso-status/contracts"
)

func writeFake(t *testing.T, path, text string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

// fakeHost lays out the parts of /proc and /sys the source reads.
func fakeHost(t *testing.T, stat string) *NasSource {
	root := t.TempDir()
	s := NewNasSource(&NasConfig{})
	s.proc, s.sys = filepath.Join(root, "proc"), filepath.Join(root, "sys")
	writeFake(t, filepath.Join(s.proc, "stat"), stat)
	writeFake(t, filepath.Join(s.proc, "meminfo"), "MemTotal:        2007892 kB\nMemFree:          228020 kB\nMemAvailable:     558740 kB\n")
	writeFake(t, filepath.Join(s.sys, "class/thermal/thermal_zone0/type"), "soc-thermal\n")
	writeFake(t, filepath.Join(s.sys, "class/thermal/thermal_zone0/temp"), "44375\n")
	writeFake(t, filepath.Join(s.sys, "class/thermal/thermal_zone1/type"), "gpu-thermal\n")
	writeFake(t, filepath.Join(s.sys, "class/thermal/thermal_zone1/temp"), "43750\n")
	return s
}

func nasValue(o contracts.Object, key string) any  { return o[key].(contracts.Object)["value"] }
func nasReason(o contracts.Object, key string) any { return o[key].(contracts.Object)["reason"] }

func TestNasParsers(t *testing.T) {
	busy, total, ok := cpuTimes("cpu  100 0 50 800 50 0 0 0 0 0\ncpu0 1 2 3 4 5 6 7 8\n")
	if !ok || busy != 150 || total != 1000 {
		t.Fatalf("cpu jiffies: %d %d %v", busy, total, ok)
	}
	for _, bad := range []string{"", "cpu0 1 2 3 4 5", "cpu 1 2 x 4 5", "cpu 1 2 3"} {
		if _, _, ok := cpuTimes(bad); ok {
			t.Fatalf("accepted %q", bad)
		}
	}
	if total, avail, ok := memInfo("MemTotal: 2000 kB\nMemAvailable: 500 kB\n"); !ok || total != 2000*1024 || avail != 500*1024 {
		t.Fatal("meminfo")
	}
	for _, bad := range []string{"MemTotal: 2000 kB\n", "MemTotal: 100 kB\nMemAvailable: 200 kB\n", "MemTotal: 0 kB\nMemAvailable: 0 kB\n", "MemTotal: x kB\nMemAvailable: 1 kB\n"} {
		if _, _, ok := memInfo(bad); ok {
			t.Fatalf("accepted %q", bad)
		}
	}
}

func TestNasSnapshot(t *testing.T) {
	now := time.Now()
	un := NewNasSource(nil).Snapshot(now)
	if object(un, "source")["quality"] != "unconfigured" || nasReason(un, "cpu_percent") != "not_configured" {
		t.Fatalf("unconfigured: %v", un)
	}
	s := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	if q := object(s.Snapshot(now), "source")["quality"]; q != "unconfigured" {
		t.Fatalf("before the first sample the source is waiting, got %v", q)
	}
	s.Sample(now)
	first := s.Snapshot(now)
	if nasReason(first, "cpu_percent") != "warming_up" || nasValue(first, "cpu_percent") != nil {
		t.Fatalf("cpu needs two samples: %v", first)
	}
	if nasValue(first, "memory_percent") != 72.2 || nasValue(first, "memory_total_bytes") != float64(2007892*1024) || nasValue(first, "soc_temperature_c") != 44.4 || nasValue(first, "gpu_temperature_c") != 43.8 {
		t.Fatalf("host readings: %v", first)
	}
	if nasReason(first, "disk_temperature_c") != "not_configured" {
		t.Fatalf("no disk configured: %v", first)
	}
	if _, has := first["disk_state"]; has {
		t.Fatalf("no disk configured, no state: %v", first)
	}
	writeFake(t, filepath.Join(s.proc, "stat"), "cpu  200 0 100 1250 50 0 0 0\n")
	s.Sample(now.Add(5 * time.Second))
	if v := nasValue(s.Snapshot(now.Add(5*time.Second)), "cpu_percent"); v != 25.0 {
		t.Fatalf("150 busy of 600 jiffies is 25%%, got %v", v)
	}
	writeFake(t, filepath.Join(s.sys, "class/thermal/thermal_zone0/temp"), "999999\n")
	s.Sample(now.Add(10 * time.Second))
	if bad := s.Snapshot(now.Add(10 * time.Second)); nasValue(bad, "soc_temperature_c") != nil || nasReason(bad, "soc_temperature_c") != "temperature_invalid" || nasValue(bad, "gpu_temperature_c") != 43.8 {
		t.Fatalf("an absurd temperature is rejected on its own: %v", bad)
	}
	if q := object(s.Snapshot(now.Add(40*time.Second)), "source")["quality"]; q != "stale" {
		t.Fatalf("20 s without a sample is stale, got %v", q)
	}
	if q := object(s.Snapshot(now.Add(70*time.Second)), "source")["quality"]; q != "offline" {
		t.Fatalf("45 s without a sample is offline, got %v", q)
	}
	os.RemoveAll(s.proc)
	os.RemoveAll(s.sys)
	s.Sample(now.Add(80 * time.Second))
	if source := object(s.Snapshot(now.Add(80*time.Second)), "source"); source["quality"] != "offline" {
		t.Fatalf("a failed sample keeps the last good time: %v", source)
	}
}

func TestNasDisk(t *testing.T) {
	now := time.Now()
	s := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	s.cfg = &NasConfig{Disk: "/dev/sda", DiskIntervalSeconds: 120}
	reads, asks := 0, 0
	state := diskActive
	var failure error
	s.power = func(device string) string {
		asks++
		if device != "/dev/sda" {
			t.Fatalf("unexpected device %s", device)
		}
		return state
	}
	s.smart = func(device string) (float64, error) {
		reads++
		return 41.04, failure
	}
	s.Sample(now)
	s.Sample(now.Add(5 * time.Second))
	if asks != 1 || reads != 1 || nasValue(s.Snapshot(now.Add(5*time.Second)), "disk_temperature_c") != 41.0 {
		t.Fatalf("an awake disk is read once: asks=%d reads=%d %v", asks, reads, s.Snapshot(now))
	}
	if st := s.Snapshot(now.Add(5 * time.Second))["disk_state"]; st != "active" {
		t.Fatalf("a disk the drive reports as active is awake, got %v", st)
	}
	s.Sample(now.Add(61 * time.Second))
	if asks != 2 || reads != 1 {
		t.Fatalf("the power state is asked every minute, the temperature only every disk interval: asks=%d reads=%d", asks, reads)
	}
	state = diskStandby
	s.Sample(now.Add(122 * time.Second))
	if snap := s.Snapshot(now.Add(122 * time.Second)); reads != 1 || nasValue(snap, "disk_temperature_c") != nil || nasReason(snap, "disk_temperature_c") != "disk_standby" {
		t.Fatalf("a sleeping disk is reported, not read: reads=%d %v", reads, snap)
	}
	if st := s.Snapshot(now.Add(122 * time.Second))["disk_state"]; st != "standby" {
		t.Fatalf("a sleeping disk is standby, got %v", st)
	}
	if n := nasValue(s.Snapshot(now.Add(122*time.Second)), "disk_wake_count_today"); n != 0.0 {
		t.Fatalf("falling asleep is not a wake-up: %v", n)
	}
	state = diskUnknown
	s.Sample(now.Add(183 * time.Second))
	if snap := s.Snapshot(now.Add(183 * time.Second)); reads != 1 || nasValue(snap, "disk_temperature_c") != nil {
		t.Fatalf("a disk that cannot report its power state is never read: reads=%d %v", reads, snap)
	}
	state = diskActive
	s.Sample(now.Add(244 * time.Second))
	if reads != 2 || nasValue(s.Snapshot(now.Add(244*time.Second)), "disk_temperature_c") != 41.0 {
		t.Fatalf("a disk that woke up is read at once: reads=%d", reads)
	}
	if n := nasValue(s.Snapshot(now.Add(244*time.Second)), "disk_wake_count_today"); n != 1.0 {
		t.Fatalf("one wake-up so far: %v", n)
	}
	failure = errors.New("io")
	s.Sample(now.Add(366 * time.Second))
	if snap := s.Snapshot(now.Add(366 * time.Second)); reads != 3 || nasValue(snap, "disk_temperature_c") != nil || nasReason(snap, "disk_temperature_c") != "disk_unavailable" {
		t.Fatalf("a failed read is unavailable: reads=%d %v", reads, snap)
	}
	if v := nasValue(s.Snapshot(now.Add(366*time.Second+time.Hour)), "disk_temperature_c"); v != nil {
		t.Fatalf("an old disk reading expires: %v", v)
	}
}

func TestNasDiskIOOnly(t *testing.T) {
	now := time.Now()
	s := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	s.cfg = &NasConfig{Disk: "/dev/sda", DiskIntervalSeconds: 60, DiskIOOnly: true}
	reads := 0
	s.power = func(string) string { t.Fatal("io-only mode never asks the drive anything"); return diskUnknown }
	s.smart = func(string) (float64, error) { reads++; return 39.6, nil }
	stat := filepath.Join(s.sys, "block/sda/stat")
	counters := func(r, w int) {
		writeFake(t, stat, fmt.Sprintf("%8d 1 2 3 %8d 4 5 6 0 7 8\n", r, w))
	}
	counters(100, 200)
	for i := 0; i < 4; i++ {
		s.Sample(now.Add(time.Duration(i*5) * time.Second))
	}
	if snap := s.Snapshot(now.Add(20 * time.Second)); reads != 0 || nasReason(snap, "disk_temperature_c") != "disk_idle" {
		t.Fatalf("a quiet disk is not read: reads=%d %v", reads, snap)
	}
	if st := s.Snapshot(now.Add(20 * time.Second))["disk_state"]; st != "quiet" {
		t.Fatalf("without any I/O or power answer the state is quiet, got %v", st)
	}
	counters(101, 200)
	s.Sample(now.Add(25 * time.Second))
	if st := s.Snapshot(now.Add(25 * time.Second))["disk_state"]; st != "active" {
		t.Fatalf("a disk that served requests is active, got %v", st)
	}
	if snap := s.Snapshot(now.Add(25 * time.Second)); reads != 1 || nasValue(snap, "disk_temperature_c") != 39.6 {
		t.Fatalf("a disk that just served requests is read: reads=%d %v", reads, snap)
	}
	counters(150, 230)
	counters(150, 230)
	s.Sample(now.Add(30 * time.Second))
	if reads != 1 {
		t.Fatalf("the interval is respected while the disk is busy: reads=%d", reads)
	}
	s.Sample(now.Add(35 * time.Second))
	if st := s.Snapshot(now.Add(45 * time.Second))["disk_state"]; st != "active" {
		t.Fatalf("without a drive answer the disk counts as awake until its idle timeout has passed, got %v", st)
	}
	s.Sample(now.Add(400 * time.Second))
	if st := s.Snapshot(now.Add(400 * time.Second))["disk_state"]; st != "standby" {
		t.Fatalf("five minutes after the last request the disk is taken to sleep, got %v", st)
	}
	if snap := s.Snapshot(now.Add(36 * time.Second)); reads != 1 || nasValue(snap, "disk_temperature_c") != 39.6 {
		t.Fatalf("the last reading stays while the disk is quiet: reads=%d %v", reads, snap)
	}
	counters(151, 230)
	s.Sample(now.Add(90 * time.Second))
	if reads != 2 {
		t.Fatalf("a busy disk is read again after the interval: reads=%d", reads)
	}
	if v := nasValue(s.Snapshot(now.Add(90*time.Second+time.Hour)), "disk_temperature_c"); v != nil {
		t.Fatalf("an old reading expires: %v", v)
	}
}

func TestNasWakeCount(t *testing.T) {
	// 2026-10-18 23:00 UTC is 07:00 on the 19th in Beijing.
	now := time.Date(2026, 10, 18, 23, 0, 0, 0, time.UTC)
	s := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	s.cfg = &NasConfig{Disk: "/dev/sda", DiskIOOnly: true, DiskSleepSeconds: 60}
	s.power = func(string) string { t.Fatal("io-only never asks the drive"); return diskUnknown }
	s.smart = func(string) (float64, error) { return 40, nil }
	stat := filepath.Join(s.sys, "block/sda/stat")
	counters := func(r int) { writeFake(t, stat, fmt.Sprintf("%d 1 2 3 5 4 5 6 0 7 8\n", r)) }
	at := func(seconds int, reads int) {
		counters(reads)
		s.Sample(now.Add(time.Duration(seconds) * time.Second))
	}
	state := func(seconds int) (string, any) {
		snap := s.Snapshot(now.Add(time.Duration(seconds) * time.Second))
		return snap["disk_state"].(string), nasValue(snap, "disk_wake_count_today")
	}
	at(0, 10)
	at(5, 11) // requests soon after start: awake, but a wake-up was not seen
	if st, n := state(5); st != "active" || n != 0.0 {
		t.Fatalf("first requests: %s %v", st, n)
	}
	at(30, 11)
	at(66, 11)
	if st, _ := state(66); st != "standby" {
		t.Fatalf("a minute after the last request the disk sleeps: %s", st)
	}
	at(100, 12)
	if st, n := state(100); st != "active" || n != 1.0 {
		t.Fatalf("a request after sleeping is a wake-up: %s %v", st, n)
	}
	at(105, 13)
	at(110, 14)
	if _, n := state(110); n != 1.0 {
		t.Fatalf("requests while awake are not wake-ups: %v", n)
	}
	at(200, 14)
	at(300, 15)
	if st, n := state(300); st != "active" || n != 2.0 {
		t.Fatalf("second wake-up: %s %v", st, n)
	}
	if _, n := state(300 + 3600*17); n != 0.0 {
		t.Fatalf("the count restarts at midnight (UTC+8): %v", n)
	}
	for _, bad := range []int{59, 3601} {
		if validateNas(&NasConfig{DiskSleepSeconds: bad}) == nil {
			t.Fatalf("accepted sleep %d", bad)
		}
	}
}

func TestNasPreciseAnswerDoesNotOverrideIO(t *testing.T) {
	now := time.Now()
	s := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	s.cfg = &NasConfig{Disk: "/dev/sda"}
	s.power = func(string) string { return diskStandby }
	s.smart = func(string) (float64, error) { return 40, nil }
	stat := filepath.Join(s.sys, "block/sda/stat")
	writeFake(t, stat, "10 1 2 3 5 4 5 6 0 7 8\n")
	s.Sample(now)
	writeFake(t, stat, "11 1 2 3 5 4 5 6 0 7 8\n")
	s.Sample(now.Add(5 * time.Second))
	if st := s.Snapshot(now.Add(5 * time.Second))["disk_state"]; st != "active" {
		t.Fatalf("requests just completed, so a standby answer is not believed: %v", st)
	}
	s.Sample(now.Add(400 * time.Second))
	if st := s.Snapshot(now.Add(400 * time.Second))["disk_state"]; st != "standby" {
		t.Fatalf("long after the last request the standby answer holds: %v", st)
	}
}

func TestNasReadAsleep(t *testing.T) {
	now := time.Now()
	s := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	s.cfg = &NasConfig{Disk: "/dev/sda", DiskIntervalSeconds: 60, DiskReadAsleep: true}
	state := diskStandby
	reads := 0
	took := 20 * time.Millisecond
	s.power = func(string) string { return state }
	s.smart = func(string) (float64, error) { reads++; return 37.4, nil }
	s.elapsed = func(time.Time) time.Duration { return took }
	s.Sample(now)
	snap := s.Snapshot(now)
	if reads != 1 || nasValue(snap, "disk_temperature_c") != 37.4 || snap["disk_state"] != "standby" || nasValue(snap, "disk_wake_count_today") != 0.0 {
		t.Fatalf("a quick answer from a sleeping drive is no wake-up: reads=%d %v", reads, snap)
	}
	s.Sample(now.Add(65 * time.Second))
	if snap := s.Snapshot(now.Add(65 * time.Second)); reads != 2 || snap["disk_state"] != "standby" || nasValue(snap, "disk_wake_count_today") != 0.0 {
		t.Fatalf("still asleep after a second read: reads=%d %v", reads, snap)
	}
	took = 6 * time.Second
	s.Sample(now.Add(130 * time.Second))
	if snap := s.Snapshot(now.Add(130 * time.Second)); reads != 3 || snap["disk_state"] != "active" || nasValue(snap, "disk_wake_count_today") != 1.0 {
		t.Fatalf("a read that took seconds woke the drive: reads=%d %v", reads, snap)
	}
	took = 20 * time.Millisecond
	state = diskActive
	s.Sample(now.Add(200 * time.Second))
	if snap := s.Snapshot(now.Add(200 * time.Second)); snap["disk_state"] != "active" || nasValue(snap, "disk_wake_count_today") != 1.0 {
		t.Fatalf("one wake-up, not two: %v", snap)
	}
	// Drive that answers active right after the read although the read was quick.
	s2 := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	s2.cfg = &NasConfig{Disk: "/dev/sda", DiskIntervalSeconds: 60, DiskReadAsleep: true}
	answers := []string{diskStandby, diskActive}
	s2.power = func(string) string {
		a := answers[0]
		if len(answers) > 1 {
			answers = answers[1:]
		}
		return a
	}
	s2.smart = func(string) (float64, error) { return 38, nil }
	s2.elapsed = func(time.Time) time.Duration { return time.Millisecond }
	s2.Sample(now)
	s2.Sample(now.Add(65 * time.Second))
	if snap := s2.Snapshot(now.Add(65 * time.Second)); nasValue(snap, "disk_wake_count_today") != 1.0 {
		t.Fatalf("a drive that is active right after our read was woken by it: %v", snap)
	}
}

func TestNasDiskActivity(t *testing.T) {
	now := time.Now()
	s := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	s.cfg = &NasConfig{Disk: "/dev/sda", DiskIOOnly: true}
	s.smart = func(string) (float64, error) { return 40, nil }
	stat := func(reads, writes, sectorsRead, sectorsWritten, ms int) {
		writeFake(t, filepath.Join(s.sys, "block/sda/stat"), fmt.Sprintf("%d 1 %d 3 %d 4 %d 6 0 %d 8\n", reads, sectorsRead, writes, sectorsWritten, ms))
	}
	stat(100, 200, 1000, 2000, 1000)
	s.Sample(now)
	if snap := s.Snapshot(now); nasReason(snap, "disk_busy_percent") != "warming_up" || nasReason(snap, "disk_read_bytes_per_second") != "warming_up" {
		t.Fatalf("one sample is not enough: %v", snap)
	}
	stat(110, 210, 1000+2048, 2000+4096, 1000+2500)
	s.Sample(now.Add(5 * time.Second))
	snap := s.Snapshot(now.Add(5 * time.Second))
	if nasValue(snap, "disk_busy_percent") != 50.0 || nasValue(snap, "disk_read_bytes_per_second") != 209715.0 || nasValue(snap, "disk_write_bytes_per_second") != 419430.0 {
		t.Fatalf("2500 ms busy of 5 s, 1 MiB read and 2 MiB written: %v", snap)
	}
	stat(110, 210, 1000+2048, 2000+4096, 1000+2500)
	s.Sample(now.Add(10 * time.Second))
	if snap := s.Snapshot(now.Add(10 * time.Second)); nasValue(snap, "disk_busy_percent") != 0.0 || nasValue(snap, "disk_read_bytes_per_second") != 0.0 {
		t.Fatalf("a quiet disk reports zero, not a missing value: %v", snap)
	}
	stat(1, 1, 1, 1, 1)
	s.Sample(now.Add(15 * time.Second))
	if snap := s.Snapshot(now.Add(15 * time.Second)); nasReason(snap, "disk_busy_percent") != "warming_up" {
		t.Fatalf("counters that went backwards restart the measurement: %v", snap)
	}
	os.Remove(filepath.Join(s.sys, "block/sda/stat"))
	s.Sample(now.Add(20 * time.Second))
	if snap := s.Snapshot(now.Add(20 * time.Second)); nasReason(snap, "disk_busy_percent") != "source_unavailable" {
		t.Fatalf("an unreadable statistics file is unavailable: %v", snap)
	}
}

func TestNasEmmcActivity(t *testing.T) {
	now := time.Now()
	s := fakeHost(t, "cpu  100 0 50 800 50 0 0 0\n")
	s.cfg = &NasConfig{}
	if snap := s.Snapshot(now); snap["emmc_busy_percent"] != nil {
		t.Fatalf("no eMMC, no figures: %v", snap)
	}
	stat := func(name string, sectorsRead, sectorsWritten, ms int) {
		writeFake(t, filepath.Join(s.sys, "block", name, "stat"), fmt.Sprintf("1 1 %d 3 1 4 %d 6 0 %d 8\n", sectorsRead, sectorsWritten, ms))
	}
	stat("mmcblk0boot0", 1, 1, 1) // boot partitions are not the eMMC itself
	s.Sample(now)
	if snap := s.Snapshot(now); snap["emmc_busy_percent"] != nil {
		t.Fatalf("boot partitions are ignored: %v", snap)
	}
	stat("mmcblk0", 1000, 2000, 0)
	s.Sample(now.Add(5 * time.Second))
	if snap := s.Snapshot(now.Add(5 * time.Second)); nasReason(snap, "emmc_busy_percent") != "warming_up" {
		t.Fatalf("first reading warms up: %v", snap)
	}
	stat("mmcblk0", 1000+2048, 2000+10240, 1000)
	s.Sample(now.Add(10 * time.Second))
	snap := s.Snapshot(now.Add(10 * time.Second))
	if nasValue(snap, "emmc_busy_percent") != 20.0 || nasValue(snap, "emmc_read_bytes_per_second") != 209715.0 || nasValue(snap, "emmc_write_bytes_per_second") != 1048576.0 {
		t.Fatalf("1000 ms busy of 5 s, 1 MiB read, 5 MiB written: %v", snap)
	}
	for _, bad := range []string{"mmcblk0", "/dev/mmcblk0p1", "/dev/sda", "/dev/mmcblk0\n"} {
		if validateNas(&NasConfig{Emmc: bad}) == nil {
			t.Fatalf("accepted %q", bad)
		}
	}
	if validateNas(&NasConfig{Emmc: "/dev/mmcblk1"}) != nil {
		t.Fatal("rejected a valid eMMC device")
	}
}

func TestNasConfigAndServer(t *testing.T) {
	for _, bad := range []string{"sda", "/dev/sda1", "/dev/../etc/passwd", "/dev/nvme0n1", "/dev/sda\n"} {
		if validateNas(&NasConfig{Disk: bad}) == nil {
			t.Fatalf("accepted %q", bad)
		}
	}
	for _, bad := range []int{-1, 59, 3601} {
		if validateNas(&NasConfig{DiskIntervalSeconds: bad}) == nil {
			t.Fatalf("accepted interval %d", bad)
		}
	}
	if validateNas(nil) != nil || validateNas(&NasConfig{}) != nil || validateNas(&NasConfig{Disk: "/dev/sdb"}) != nil {
		t.Fatal("no NAS section, no disk and a plain disk are valid")
	}
	c := config()
	c.NAS = &NasConfig{}
	s := NewServer(c)
	s.nas.proc, s.nas.sys = t.TempDir(), t.TempDir()
	writeFake(t, filepath.Join(s.nas.proc, "stat"), "cpu  100 0 50 800 50 0 0 0\n")
	writeFake(t, filepath.Join(s.nas.proc, "meminfo"), "MemTotal: 2000 kB\nMemAvailable: 500 kB\n")
	s.nas.Sample(time.Now())
	b, _ := json.Marshal(fixture(t))
	if w := request(s, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, b); w.Code != 200 {
		t.Fatalf("telemetry: %d", w.Code)
	}
	snap := request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil)
	var model contracts.Object
	if json.Unmarshal(snap.Body.Bytes(), &model) != nil {
		t.Fatal("snapshot is not JSON")
	}
	nas := object(model, "nas")
	if object(nas, "source")["quality"] != "ok" || nasValue(nas, "memory_percent") != 75.0 || nasReason(nas, "soc_temperature_c") != "sensor_not_found" {
		t.Fatalf("snapshot nas: %v", nas)
	}
}
