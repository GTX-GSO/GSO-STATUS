package main

import (
	"context"
	"errors"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"gso-status/contracts"
)

// Optional NAS host status for the NAS card: CPU load, memory, SoC/GPU temperature and the temperature of one hard disk.
// Everything is read from /proc and /sys (visible to a container because they report the host) and, for the disk, from one
// read-only SMART command. The disk is never woken for it: the temperature is only asked for while the kernel's own I/O
// counters show the disk serving requests (or, unless disabled, while the drive says it is spinning).
type NasConfig struct {
	Disk string `json:"disk,omitempty"`
	// Seconds between SMART reads (60-3600, default 600). Any command resets the drive's own standby timer, so reading less
	// often lets a drive with a standby timer still go to sleep.
	DiskIntervalSeconds int `json:"disk_interval_seconds,omitempty"`
	// DiskIOOnly sends the drive no command at all except the SMART read itself, and that only right after the kernel's block
	// counters (/sys/block/sdX/stat) showed completed I/O, which proves the disk is spinning. Without it the drive is also
	// asked for its power state (ATA CHECK POWER MODE, which by specification does not wake it) once a minute.
	DiskIOOnly bool `json:"disk_io_only,omitempty"`
	// Emmc is the block device of the eMMC (/dev/mmcblkN) whose busy percentage and read/write throughput are reported.
	// Left empty, the first mmcblkN found in /sys/block is used. Only the kernel's counters are read, never the device.
	Emmc string `json:"emmc,omitempty"`
	// DiskSleepSeconds is the drive's own idle timeout (60-3600, default 300): without a precise power answer the disk is
	// taken to fall asleep that long after the last completed request.
	DiskSleepSeconds int `json:"disk_sleep_seconds,omitempty"`
	// UTCOffsetMinutes sets where "today" starts for the wake counter (default +480, Beijing time).
	UTCOffsetMinutes *int `json:"utc_offset_minutes,omitempty"`
	// DiskReadAsleep also reads the temperature while the disk sleeps (or its state is not known). That may wake the drive:
	// a read that takes two seconds or more, or a drive that answers "active" right after it, counts as a wake-up of our own
	// making in the day's wake counter, so the counter shows whether the drive can answer from its cache.
	DiskReadAsleep bool `json:"disk_read_asleep,omitempty"`
}

const (
	nasEvery       = 5 * time.Second
	nasStale       = 20 * time.Second
	nasOffline     = 45 * time.Second
	nasDiskEvery   = 600 * time.Second // default SMART interval while the drive is awake
	nasPowerEvery  = 60 * time.Second  // power-state query interval (does not wake the drive)
	nasDiskExpires = 30 * time.Minute  // how long the last temperature stays on screen while the disk is idle
)

var nasDiskPattern = regexp.MustCompile(`^/dev/sd[a-z]$`)
var nasEmmcName = regexp.MustCompile(`^mmcblk[0-9]{1,2}$`)
var nasEmmcPattern = regexp.MustCompile(`^/dev/mmcblk[0-9]{1,2}$`)

func validateNas(c *NasConfig) error {
	if c != nil && (c.Disk != "" && !nasDiskPattern.MatchString(c.Disk) || c.Emmc != "" && !nasEmmcPattern.MatchString(c.Emmc) || c.DiskSleepSeconds != 0 && (c.DiskSleepSeconds < 60 || c.DiskSleepSeconds > 3600) || c.UTCOffsetMinutes != nil && (*c.UTCOffsetMinutes < -720 || *c.UTCOffsetMinutes > 840) || c.DiskIntervalSeconds != 0 && (c.DiskIntervalSeconds < 60 || c.DiskIntervalSeconds > 3600)) {
		return errors.New("invalid nas disk")
	}
	return nil
}

// Disk power state as reported by the drive. A drive that does not answer is treated as asleep: it is never read blindly.
const (
	diskActive  = "active"
	diskStandby = "standby"
	diskUnknown = "unknown"
)

type nasReading struct {
	cpu, memory, total, soc, gpu contracts.Object
}

type NasSource struct {
	cfg        *NasConfig
	proc, sys  string
	power      func(device string) string
	smart      func(device string) (celsius float64, err error) // temperature only (tests); smartFull is used when nil
	smartFull  func(device string) (smartReading, error)
	health     *smartHealth // the last SMART table read; these counters change slowly and stay until GSN restarts
	ioTotal    uint64
	ioHave     bool
	ioSeen     time.Time
	act        blockActivity // the hard disk
	emmcAct    blockActivity
	powerState string
	mu         sync.Mutex
	prevBusy   uint64
	prevTotal  uint64
	havePrev   bool
	last       nasReading
	at         time.Time
	disk       contracts.Object
	diskUntil  time.Time
	powerTried time.Time
	powerAt    time.Time // when powerState was last learned: an I/O sample or the drive's own answer
	preciseAt  time.Time // when the drive itself last answered the power query
	precise    string
	begin      time.Time
	awake      bool // the disk is spinning (judged from the drive's answer, else from completed I/O)
	awakeKnown bool
	wakes      int // wake-ups on wakeDay
	wakeDay    int
	selfWake   time.Time // when our own SMART read woke the disk
	elapsed    func(time.Time) time.Duration
	diskRead   time.Time
	sampledAny bool
}

func NewNasSource(cfg *NasConfig) *NasSource {
	return &NasSource{cfg: cfg, proc: "/proc", sys: "/sys", power: readDiskPower, smartFull: readSmart, elapsed: time.Since}
}

// cpuTimes parses the first line of /proc/stat into busy and total jiffies (iowait counts as idle).
func cpuTimes(text string) (busy, total uint64, ok bool) {
	line := text
	if i := strings.IndexByte(text, '\n'); i >= 0 {
		line = text[:i]
	}
	fields := strings.Fields(line)
	if len(fields) < 5 || fields[0] != "cpu" {
		return 0, 0, false
	}
	var v [8]uint64
	for i := 0; i < len(v) && i+1 < len(fields); i++ {
		n, err := strconv.ParseUint(fields[i+1], 10, 64)
		if err != nil {
			return 0, 0, false
		}
		v[i] = n
		total += n
	}
	idle := v[3] + v[4]
	return total - idle, total, true
}

// memInfo returns MemTotal and MemAvailable in bytes.
func memInfo(text string) (total, available uint64, ok bool) {
	var haveT, haveA bool
	for _, line := range strings.Split(text, "\n") {
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		n, err := strconv.ParseUint(fields[1], 10, 64)
		if err != nil || n > math.MaxUint64/1024 {
			continue
		}
		switch fields[0] {
		case "MemTotal:":
			total, haveT = n*1024, true
		case "MemAvailable:":
			available, haveA = n*1024, true
		}
	}
	return total, available, haveT && haveA && total > 0 && available <= total && total <= 9007199254740991
}

func round1(n float64) float64 { return math.Round(n*10) / 10 }

func readText(path string) (string, bool) {
	b, err := os.ReadFile(path)
	if err != nil || len(b) > 1<<20 {
		return "", false
	}
	return string(b), true
}

// zoneTemperature finds the thermal zone whose type contains one of the words and returns its temperature in °C.
func (s *NasSource) zoneTemperature(words ...string) contracts.Object {
	zones, _ := filepath.Glob(filepath.Join(s.sys, "class", "thermal", "thermal_zone*"))
	for _, zone := range zones {
		kind, ok := readText(filepath.Join(zone, "type"))
		if !ok {
			continue
		}
		kind = strings.ToLower(strings.TrimSpace(kind))
		for _, w := range words {
			if !strings.Contains(kind, w) {
				continue
			}
			text, ok := readText(filepath.Join(zone, "temp"))
			milli, err := strconv.ParseInt(strings.TrimSpace(text), 10, 64)
			if !ok || err != nil || milli < -40000 || milli > 150000 {
				return missing("temperature_invalid")
			}
			return value(round1(float64(milli) / 1000))
		}
	}
	return missing("sensor_not_found")
}

func (s *NasSource) Sample(now time.Time) {
	var r nasReading
	r.cpu, r.memory, r.total = missing("source_unavailable"), missing("source_unavailable"), missing("source_unavailable")
	got := false
	s.mu.Lock()
	defer s.mu.Unlock()
	if text, ok := readText(filepath.Join(s.proc, "stat")); ok {
		if busy, total, ok := cpuTimes(text); ok {
			got = true
			r.cpu = missing("warming_up")
			if s.havePrev && total > s.prevTotal && busy >= s.prevBusy {
				r.cpu = value(round1(math.Min(100, float64(busy-s.prevBusy)*100/float64(total-s.prevTotal))))
			}
			s.prevBusy, s.prevTotal, s.havePrev = busy, total, true
		}
	}
	if text, ok := readText(filepath.Join(s.proc, "meminfo")); ok {
		if total, available, ok := memInfo(text); ok {
			got = true
			r.memory = value(round1(float64(total-available) * 100 / float64(total)))
			r.total = value(float64(total))
		}
	}
	r.soc = s.zoneTemperature("soc", "cpu")
	r.gpu = s.zoneTemperature("gpu")
	if r.soc["value"] != nil || r.gpu["value"] != nil {
		got = true
	}
	s.last = r
	if got {
		s.at = now
	}
	s.sampledAny = true
	s.trackEmmc(now)
	s.sampleDisk(now)
}

// blockStat is the part of /sys/block/sdX/stat that tells what the disk has done: completed requests, sectors moved
// (512 bytes each) and milliseconds with a request in flight.
type blockStat struct{ reads, writes, sectorsRead, sectorsWritten, busyMs uint64 }

// blockActivity turns successive block statistics into busy percentage and read/write throughput.
type blockActivity struct {
	prev              blockStat
	at                time.Time
	have              bool
	busy, read, write contracts.Object
}

// readBlockStat parses the kernel's block statistics. Reading this file does not touch the device, and SG_IO commands are
// not counted in it.
func (s *NasSource) readBlockStat(name string) (blockStat, bool) {
	text, ok := readText(filepath.Join(s.sys, "block", name, "stat"))
	fields := strings.Fields(text)
	if !ok || len(fields) < 10 {
		return blockStat{}, false
	}
	var v [10]uint64
	for i := range v {
		n, err := strconv.ParseUint(fields[i], 10, 64)
		if err != nil {
			return blockStat{}, false
		}
		v[i] = n
	}
	return blockStat{reads: v[0], writes: v[4], sectorsRead: v[2], sectorsWritten: v[6], busyMs: v[9]}, true
}

// update folds one reading (ok=false: unreadable) into the activity figures of the interval since the last one.
func (a *blockActivity) update(cur blockStat, ok bool, now time.Time) {
	if !ok {
		a.have = false
		a.busy, a.read, a.write = missing("source_unavailable"), missing("source_unavailable"), missing("source_unavailable")
		return
	}
	prev, prevAt := a.prev, a.at
	a.prev, a.at = cur, now
	elapsed := now.Sub(prevAt).Seconds()
	if !a.have || elapsed <= 0 || cur.sectorsRead < prev.sectorsRead || cur.sectorsWritten < prev.sectorsWritten || cur.busyMs < prev.busyMs {
		a.have = true
		a.busy, a.read, a.write = missing("warming_up"), missing("warming_up"), missing("warming_up")
		return
	}
	a.busy = value(round1(math.Min(100, float64(cur.busyMs-prev.busyMs)/(elapsed*1000)*100)))
	a.read = value(math.Round(float64(cur.sectorsRead-prev.sectorsRead) * 512 / elapsed))
	a.write = value(math.Round(float64(cur.sectorsWritten-prev.sectorsWritten) * 512 / elapsed))
}

// trackActivity follows the hard disk's counters: throughput for the card and the moment requests last completed.
func (s *NasSource) trackActivity(now time.Time) {
	cur, ok := s.readBlockStat(filepath.Base(s.cfg.Disk))
	if !ok {
		s.ioHave = false
	} else {
		if s.ioHave && cur.reads+cur.writes != s.ioTotal {
			s.ioSeen = now
		}
		s.ioTotal, s.ioHave = cur.reads+cur.writes, true
	}
	s.act.update(cur, ok, now)
}

// emmcDevice is the configured eMMC, else the first mmcblkN in /sys/block ("" when there is none).
func (s *NasSource) emmcDevice() string {
	if s.cfg.Emmc != "" {
		return filepath.Base(s.cfg.Emmc)
	}
	entries, _ := filepath.Glob(filepath.Join(s.sys, "block", "mmcblk*"))
	for _, e := range entries {
		if name := filepath.Base(e); nasEmmcName.MatchString(name) {
			return name
		}
	}
	return ""
}

func (s *NasSource) trackEmmc(now time.Time) {
	name := s.emmcDevice()
	if name == "" {
		s.emmcAct = blockActivity{}
		return
	}
	cur, ok := s.readBlockStat(name)
	s.emmcAct.update(cur, ok, now)
}

// Caller holds s.mu. A SMART read is only sent while the disk is certainly spinning: when requests completed since the
// last sample (a few seconds ago), or when the drive itself reports an active power state. A sleeping drive, a silent drive
// and an idle one that nobody asked anything are all left alone.
func (s *NasSource) sampleDisk(now time.Time) {
	if s.cfg == nil || s.cfg.Disk == "" {
		return
	}
	s.trackActivity(now)
	interval := nasDiskEvery
	if s.cfg.DiskIntervalSeconds > 0 {
		interval = time.Duration(s.cfg.DiskIntervalSeconds) * time.Second
	}
	busy := s.ioSeen.Equal(now)
	due := s.diskRead.IsZero() || now.Sub(s.diskRead) >= interval
	fresh := busy // the power state is from this very sample
	if busy {
		s.powerState, s.powerAt = diskActive, now
	} else if !s.cfg.DiskIOOnly && (s.powerTried.IsZero() || now.Sub(s.powerTried) >= nasPowerEvery || due) {
		s.powerTried, fresh = now, true
		s.powerState, s.powerAt = s.power(s.cfg.Disk), now
		s.precise, s.preciseAt = s.powerState, now
	}
	s.trackAwake(now)
	switch {
	case s.powerState == diskActive:
		if !due || !fresh {
			return
		}
		s.readTemperature(now, interval)
	case s.cfg.DiskReadAsleep:
		// Asleep, or nothing known: read anyway when due, and note whether that woke the drive.
		if due {
			asleep := !s.awake
			took := s.readTemperature(now, interval)
			woke := asleep && took >= 2*time.Second
			if !s.cfg.DiskIOOnly {
				s.powerTried = now
				s.powerState, s.powerAt = s.power(s.cfg.Disk), now
				s.precise, s.preciseAt = s.powerState, now
				woke = woke || asleep && s.powerState == diskActive
			}
			if woke {
				s.selfWake = now
			}
			s.trackAwake(now)
		}
	case s.powerState == diskStandby:
		s.disk, s.diskUntil, s.diskRead = missing("disk_standby"), now.Add(3*nasPowerEvery), time.Time{}
	}
	// otherwise nothing is known about the drive (or only I/O counters are used and the disk is quiet): the last reading ages
}

// readTemperature sends the SMART read and returns how long it took (a drive that has to spin up needs seconds).
func (s *NasSource) readTemperature(now time.Time, interval time.Duration) time.Duration {
	s.diskRead = now
	start := time.Now()
	var reading smartReading
	var err error
	if s.smart != nil {
		var celsius float64
		celsius, err = s.smart(s.cfg.Disk)
		reading = smartReading{celsius: celsius, hasTemp: err == nil}
	} else {
		reading, err = s.smartFull(s.cfg.Disk)
	}
	took := s.elapsed(start)
	if err == nil {
		health := reading.health
		s.health = &health
	}
	if celsius := reading.celsius; err == nil && reading.hasTemp && celsius >= -40 && celsius <= 150 {
		s.disk, s.diskUntil = value(round1(celsius)), now.Add(nasDiskExpires)
	} else {
		s.disk, s.diskUntil = missing("disk_unavailable"), now.Add(interval)
	}
	return took
}

func (s *NasSource) sleepWindow() time.Duration {
	if s.cfg.DiskSleepSeconds > 0 {
		return time.Duration(s.cfg.DiskSleepSeconds) * time.Second
	}
	return 300 * time.Second
}

// today is the date number (yyyymmdd) in the configured offset.
func (s *NasSource) today(now time.Time) int {
	offset := 480
	if s.cfg.UTCOffsetMinutes != nil {
		offset = *s.cfg.UTCOffsetMinutes
	}
	y, m, d := now.UTC().Add(time.Duration(offset) * time.Minute).Date()
	return y*10000 + int(m)*100 + d
}

// trackAwake decides whether the disk is spinning and counts the wake-ups. The drive's own answer (CHECK POWER MODE, which
// does not wake it) is used while it is fresh, but completed requests within the idle timeout always mean awake. Without a
// fresh answer the disk is taken to sleep sleepWindow after the last completed request, which is how the
// drive's own timer works; before that has been seen once the state is unknown. Only a change from asleep to awake counts.
func (s *NasSource) trackAwake(now time.Time) {
	if s.begin.IsZero() {
		s.begin = now
	}
	window := s.sleepWindow()
	awake, known := false, true
	switch {
	case !s.selfWake.IsZero() && now.Sub(s.selfWake) <= window:
		awake = true
	case !s.preciseAt.IsZero() && now.Sub(s.preciseAt) <= 3*nasPowerEvery && s.precise != diskUnknown && s.precise != "":
		// A drive that says "standby" right after it completed requests is taken as awake: the answer is not trusted over
		// the kernel's own counters, which cannot be wrong about requests that really reached the disk.
		awake = s.precise == diskActive || !s.ioSeen.IsZero() && now.Sub(s.ioSeen) <= window
	case !s.ioSeen.IsZero() && now.Sub(s.ioSeen) <= window:
		awake = true
	case !s.ioSeen.IsZero() || now.Sub(s.begin) >= window:
	default:
		known = false
	}
	if !known {
		return
	}
	if awake && s.awakeKnown && !s.awake {
		if day := s.today(now); day != s.wakeDay {
			s.wakeDay, s.wakes = day, 0
		}
		s.wakes++
	}
	s.awake, s.awakeKnown = awake, true
}

// diskState: "active" while the disk is awake, "standby" while it sleeps, "quiet" until that is known.
func (s *NasSource) diskState() string {
	switch {
	case !s.awakeKnown:
		return "quiet"
	case s.awake:
		return "active"
	}
	return "standby"
}

func (s *NasSource) wakeCount(now time.Time) float64 {
	if s.wakeDay != s.today(now) {
		return 0
	}
	return float64(s.wakes)
}

func (s *NasSource) Run(ctx context.Context) {
	if s.cfg == nil {
		return
	}
	s.Sample(time.Now())
	t := time.NewTicker(nasEvery)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-t.C:
			s.Sample(now)
		}
	}
}

func (s *NasSource) Snapshot(now time.Time) contracts.Object {
	source := contracts.Object{"quality": "unconfigured", "observed_at": nil, "source_updated_at": nil, "age_ms": nil, "last_error_code": "not_configured"}
	if s.cfg == nil {
		return contracts.Object{"source": source, "cpu_percent": missing("not_configured"), "memory_percent": missing("not_configured"), "memory_total_bytes": missing("not_configured"),
			"soc_temperature_c": missing("not_configured"), "gpu_temperature_c": missing("not_configured"), "disk_temperature_c": missing("not_configured")}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	out := contracts.Object{"source": source, "cpu_percent": missing("source_unavailable"), "memory_percent": missing("source_unavailable"), "memory_total_bytes": missing("source_unavailable"),
		"soc_temperature_c": missing("source_unavailable"), "gpu_temperature_c": missing("source_unavailable"), "disk_temperature_c": missing("not_configured")}
	switch {
	case s.at.IsZero():
		source["quality"], source["last_error_code"] = "error", "nas_unavailable"
		if !s.sampledAny {
			source["quality"], source["last_error_code"] = "unconfigured", "waiting_for_first_sample"
		}
	default:
		age := now.Sub(s.at)
		out["source"] = contracts.Object{"quality": "ok", "observed_at": s.at.UTC().Format(time.RFC3339Nano), "source_updated_at": nil, "age_ms": float64(age.Milliseconds()), "last_error_code": nil}
		if age > nasOffline {
			out["source"].(contracts.Object)["quality"] = "offline"
		} else if age > nasStale {
			out["source"].(contracts.Object)["quality"] = "stale"
		}
		out["cpu_percent"], out["memory_percent"], out["memory_total_bytes"] = s.last.cpu, s.last.memory, s.last.total
		out["soc_temperature_c"], out["gpu_temperature_c"] = s.last.soc, s.last.gpu
	}
	if s.cfg.Disk != "" {
		out["disk_state"] = s.diskState()
		out["disk_wake_count_today"] = value(s.wakeCount(now))
		out["disk_busy_percent"], out["disk_read_bytes_per_second"], out["disk_write_bytes_per_second"] = missing("warming_up"), missing("warming_up"), missing("warming_up")
		if s.act.busy != nil {
			out["disk_busy_percent"], out["disk_read_bytes_per_second"], out["disk_write_bytes_per_second"] = s.act.busy, s.act.read, s.act.write
		}
		out["disk_temperature_c"] = missing("disk_idle")
		if s.disk != nil && !now.After(s.diskUntil) {
			out["disk_temperature_c"] = s.disk
		}
	}
	if s.health != nil {
		count := func(c smartCount) contracts.Object {
			if !c.ok {
				return missing("not_supported")
			}
			return value(float64(c.n))
		}
		h := s.health
		out["disk_health"] = h.verdict()
		out["disk_power_on_hours"], out["disk_reallocated_sectors"], out["disk_pending_sectors"] = count(h.hours), count(h.reallocated), count(h.pending)
		out["disk_crc_errors"], out["disk_power_cycles"], out["disk_load_cycles"] = count(h.crc), count(h.powerCycles), count(h.loadCycles)
	}
	if s.emmcAct.busy != nil {
		out["emmc_busy_percent"], out["emmc_read_bytes_per_second"], out["emmc_write_bytes_per_second"] = s.emmcAct.busy, s.emmcAct.read, s.emmcAct.write
	}
	return out
}
