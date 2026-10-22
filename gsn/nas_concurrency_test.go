package main

import (
	"encoding/json"
	"sync"
	"testing"
	"time"
)

// A slow device must leave the last completed sample readable, including its age,
// while both refresh and telemetry continue publishing cached HTTP snapshots.
func TestNasSlowDeviceDoesNotBlockHTTP(t *testing.T) {
	for _, mode := range []string{"power", "smart"} {
		t.Run(mode, func(t *testing.T) {
			c := config()
			server := NewServer(c)
			nas := fakeHost(t, "cpu 100 0 50 800 50 0 0 0\n")
			nas.cfg = &NasConfig{Disk: "/dev/sda", DiskIntervalSeconds: 600}
			nas.power = func(string) string { return diskActive }
			nas.smartFull = func(string) (smartReading, error) {
				return smartReading{celsius: 35, hasTemp: true}, nil
			}
			nas.Sample(time.Now().Add(-601 * time.Second))
			server.nas = nas
			server.refresh(time.Now())
			started, release, sampled := make(chan struct{}), make(chan struct{}), make(chan struct{})
			var once sync.Once
			defer func() { once.Do(func() { close(release) }); <-sampled }()
			block := func() { close(started); <-release }
			nas.power = func(string) string {
				if mode == "power" {
					block()
				}
				return diskActive
			}
			nas.smartFull = func(string) (smartReading, error) {
				if mode == "smart" {
					block()
				}
				return smartReading{celsius: 44, hasTemp: true}, nil
			}
			go func() { defer close(sampled); nas.Sample(time.Now()) }()
			<-started
			results := make(chan string, 3)
			go func() {
				server.mu.Lock()
				server.refresh(time.Now())
				server.mu.Unlock()
				results <- "refresh"
			}()
			go func() {
				if w := request(server, "GET", "/api/v1/snapshot", c.ReadToken, nil); w.Code != 200 {
					results <- "GET failed"
				} else {
					results <- "GET"
				}
			}()
			body, err := json.Marshal(fixture(t))
			if err != nil {
				t.Fatal(err)
			}
			go func() {
				if w := request(server, "POST", "/api/v1/agents/desktop/telemetry", c.WriteToken, body); w.Code != 200 {
					results <- "POST failed"
				} else {
					results <- "POST"
				}
			}()
			deadline := time.NewTimer(500 * time.Millisecond)
			defer deadline.Stop()
			for i := 0; i < 3; i++ {
				select {
				case result := <-results:
					if result == "GET failed" || result == "POST failed" {
						t.Fatal(result)
					}
				case <-deadline.C:
					t.Fatal("HTTP publication waited for slow device")
				}
			}
			before := nas.Snapshot(time.Now())
			if nasValue(before, "disk_temperature_c") != 35.0 || object(before, "source")["quality"] != "offline" {
				t.Fatal("slow query changed or refreshed the last completed sample")
			}
			once.Do(func() { close(release) })
			<-sampled
			if nasValue(nas.Snapshot(time.Now()), "disk_temperature_c") != 44.0 {
				t.Fatal("completed sample did not publish")
			}
		})
	}
}
