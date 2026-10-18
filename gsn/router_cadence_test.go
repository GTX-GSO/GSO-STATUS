package main

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestRouterFiveSecondCadence(t *testing.T) {
	calls := make(chan time.Time, 8)
	mock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls <- time.Now()
		w.Write([]byte(`var wan_rate="128;256";`))
	}))
	defer mock.Close()
	r := NewRouter(mock.URL)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { r.Run(ctx); close(done) }()
	deadline := time.NewTimer(8 * time.Second)
	defer deadline.Stop()
	var first time.Time
	for i := 0; i < 2; i++ {
		select {
		case at := <-calls:
			if i == 0 {
				first = at
			} else if at.Sub(first) < 4900*time.Millisecond {
				t.Fatal("router polled faster than measured page refresh")
			}
		case <-deadline.C:
			t.Fatal("router did not refresh")
		}
	}
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("router cancellation delayed")
	}
}
func TestRouterFreshnessMatchesRefreshCadence(t *testing.T) {
	r := NewRouter("http://127.0.0.1")
	now := time.Now()
	r.last = now
	r.errorCode = ""
	for _, test := range []struct {
		age     time.Duration
		quality string
	}{{6 * time.Second, "ok"}, {16 * time.Second, "stale"}, {46 * time.Second, "offline"}} {
		s := r.Snapshot(now.Add(test.age))
		if object(s, "source")["quality"] != test.quality {
			t.Fatal("WAN freshness not aligned with 5-second refresh", test)
		}
	}
}
