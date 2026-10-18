package main

import (
	"context"
	"errors"
	"regexp"
	"strings"
	"sync"
	"time"

	"gso-status/contracts"
)

// Optional NAS volume capacity for the NET detail page. The container only sees the paths that are mounted into
// it; any directory on a volume reports that volume's filesystem capacity, so mounting a directory of each volume is
// enough. Only totals are read, never file names or contents.
type StorageVolume struct {
	Label string `json:"label"`
	Path  string `json:"path"`
}

const (
	storageEvery   = 30 * time.Second
	storageStale   = 90 * time.Second
	storageOffline = 180 * time.Second
)

var storageLabel = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9 _.-]{0,23}$`)

func validateStorage(volumes []StorageVolume) error {
	if len(volumes) > 2 {
		return errors.New("at most two storage volumes")
	}
	seen := map[string]bool{}
	for _, v := range volumes {
		key := strings.ToLower(v.Label)
		if !storageLabel.MatchString(v.Label) || seen[key] || v.Path == "" || len(v.Path) > 240 || strings.ContainsAny(v.Path, "\x00\r\n\t") {
			return errors.New("invalid or duplicate storage volume")
		}
		seen[key] = true
	}
	return nil
}

type volumeReading struct {
	total, free uint64
	ok          bool
}

type StorageSource struct {
	volumes []StorageVolume
	read    func(path string) (total, free uint64, err error)
	mu      sync.Mutex
	last    []volumeReading
	at      time.Time
	failed  bool
}

func NewStorageSource(volumes []StorageVolume) *StorageSource {
	return &StorageSource{volumes: volumes, read: diskUsage}
}

func (s *StorageSource) Sample(now time.Time) {
	readings := make([]volumeReading, len(s.volumes))
	any := false
	for i, v := range s.volumes {
		total, free, err := s.read(v.Path)
		if err == nil && total > 0 && free <= total && total <= 9007199254740991 {
			readings[i] = volumeReading{total: total, free: free, ok: true}
			any = true
		}
	}
	s.mu.Lock()
	s.last, s.failed = readings, !any
	if any {
		s.at = now
	}
	s.mu.Unlock()
}

func (s *StorageSource) Run(ctx context.Context) {
	if len(s.volumes) == 0 {
		return
	}
	s.Sample(time.Now())
	t := time.NewTicker(storageEvery)
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

func (s *StorageSource) Snapshot(now time.Time) contracts.Object {
	source := contracts.Object{"quality": "unconfigured", "observed_at": nil, "source_updated_at": nil, "age_ms": nil, "last_error_code": "not_configured"}
	volumes := []any{}
	if len(s.volumes) == 0 {
		return contracts.Object{"source": source, "volumes": volumes}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	missing := func(reason string) contracts.Object { return contracts.Object{"value": nil, "reason": reason} }
	for i, v := range s.volumes {
		total, free := missing("source_unavailable"), missing("source_unavailable")
		if i < len(s.last) && s.last[i].ok {
			total = contracts.Object{"value": float64(s.last[i].total), "reason": nil}
			free = contracts.Object{"value": float64(s.last[i].free), "reason": nil}
		} else if i < len(s.last) {
			total, free = missing("volume_unavailable"), missing("volume_unavailable")
		}
		volumes = append(volumes, contracts.Object{"id": strings.ToLower(v.Label), "label": v.Label, "total_bytes": total, "free_bytes": free})
	}
	switch {
	case s.at.IsZero():
		source["quality"], source["last_error_code"] = "error", "storage_unavailable"
		if len(s.last) == 0 {
			source["quality"], source["last_error_code"] = "unconfigured", "waiting_for_first_sample"
		}
	default:
		age := now.Sub(s.at)
		source = contracts.Object{"quality": "ok", "observed_at": s.at.UTC().Format(time.RFC3339Nano), "source_updated_at": nil, "age_ms": float64(age.Milliseconds()), "last_error_code": nil}
		if age > storageOffline {
			source["quality"] = "offline"
		} else if age > storageStale {
			source["quality"] = "stale"
		}
		if s.failed {
			source["last_error_code"] = "storage_unavailable"
		}
	}
	return contracts.Object{"source": source, "volumes": volumes}
}
