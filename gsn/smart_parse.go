package main

import "errors"

// smartCount is one SMART raw value that the drive may not report at all.
type smartCount struct {
	n  uint64
	ok bool
}

// smartHealth holds the few SMART figures that say whether a hard disk is wearing out, read from the same 512-byte
// attribute table as the temperature (no extra command reaches the drive for them).
type smartHealth struct {
	failing     bool // SMART RETURN STATUS says a threshold was exceeded
	statusKnown bool
	hours       smartCount // power-on hours (9)
	reallocated smartCount // sectors the drive replaced with spares (5)
	pending     smartCount // sectors waiting to be replaced or unreadable (197 + 198)
	crc         smartCount // interface transfer errors (199)
	powerCycles smartCount // power-on count (12)
	loadCycles  smartCount // head load/unload count (193)
}

type smartReading struct {
	celsius float64
	hasTemp bool
	health  smartHealth
}

const (
	healthGood    = "good"
	healthWatch   = "watch"
	healthBad     = "bad"
	healthUnknown = "unknown"
)

// verdict is the one-word summary: any failing SMART status, ten or more waiting sectors or a hundred replaced ones is bad,
// any replaced or waiting sector is worth watching, and a drive that reports neither counter cannot be judged.
func (h smartHealth) verdict() string {
	switch {
	case h.failing, h.pending.n >= 10, h.reallocated.n >= 100:
		return healthBad
	case h.pending.n > 0, h.reallocated.n > 0:
		return healthWatch
	case !h.reallocated.ok && !h.pending.ok && !h.statusKnown:
		return healthUnknown
	}
	return healthGood
}

// parseSmart reads the SMART attribute table (SMART READ DATA): twelve-byte entries from offset 2 with the id, flags, the
// normalised value, the worst value and six raw bytes (little endian). Counters that some makers pack with other data in
// the upper bytes are read from the low bytes only.
func parseSmart(data []byte) (smartReading, error) {
	var r smartReading
	if len(data) != 512 {
		return r, errors.New("smart data size")
	}
	var sum byte
	for _, b := range data {
		sum += b
	}
	if sum != 0 {
		return r, errors.New("smart checksum mismatch")
	}
	raw := func(i, bytes int) uint64 {
		var n uint64
		for k := bytes - 1; k >= 0; k-- {
			n = n<<8 | uint64(data[i+5+k])
		}
		return n
	}
	temp := -1
	for i := 2; i+12 <= 362; i += 12 {
		switch id := data[i]; id {
		case 194:
			temp = int(data[i+5])
		case 190:
			if temp < 0 {
				temp = int(data[i+5])
			}
		case 5:
			r.health.reallocated = smartCount{raw(i, 2), true}
		case 9:
			r.health.hours = smartCount{raw(i, 4), true}
		case 12:
			r.health.powerCycles = smartCount{raw(i, 4), true}
		case 193:
			r.health.loadCycles = smartCount{raw(i, 4), true}
		case 197, 198:
			r.health.pending = smartCount{r.health.pending.n + raw(i, 2), true}
		case 199:
			r.health.crc = smartCount{raw(i, 4), true}
		}
	}
	if temp >= 0 && temp <= 100 {
		r.celsius, r.hasTemp = float64(temp), true
	}
	return r, nil
}
