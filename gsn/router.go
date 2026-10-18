package main

import (
	"context"
	"errors"
	"gso-status/contracts"
	"io"
	"log"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

const routerPollInterval = 5 * time.Second

var wanPattern = regexp.MustCompile(`\bvar\s+wan_rate\s*=\s*(?:"([0-9]+);([0-9]+)"|'([0-9]+);([0-9]+)')`)

type Router struct {
	url                 string
	client              *http.Client
	mu                  sync.Mutex
	last                time.Time
	down, up            uint64
	errorCode           string
	readBytes           int
	credentialsPath     string
	lastAuth            time.Time
	badCredentialHash   [32]byte
	credentialsRejected bool
	lastAuthError       string
}

func routerLogin(path string) bool {
	path = strings.ToLower(path)
	return strings.Contains(path, "login") || strings.Contains(path, "signin") || strings.HasSuffix(path, "/router_password_mobile.asp")
}
func NewRouter(address string) *Router {
	origin, _ := url.Parse(address)
	jar, _ := cookiejar.New(nil)
	return &Router{url: address, client: &http.Client{Jar: jar, Timeout: 800 * time.Millisecond, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		// Never forward the request to another origin or consume an authentication page.
		if origin == nil || len(via) > 2 || req.URL.Scheme != origin.Scheme || !strings.EqualFold(req.URL.Host, origin.Host) || req.URL.User != nil || routerLogin(req.URL.Path) {
			return http.ErrUseLastResponse
		}
		return nil
	}, Transport: &http.Transport{DisableCompression: true, MaxResponseHeaderBytes: 8192, MaxIdleConns: 1, MaxIdleConnsPerHost: 1}}, errorCode: "waiting_for_sample"}
}
func readWAN(reader io.Reader) (uint64, uint64, int, error) {
	var prefix [8192]byte
	used := 0
	for used < len(prefix) {
		end := used + 1024
		if used < 2048 && end > 2048 {
			end = 2048
		}
		if end > len(prefix) {
			end = len(prefix)
		}
		n, e := reader.Read(prefix[used:end])
		used += n
		if match := wanPattern.FindSubmatch(prefix[:used]); match != nil {
			first, second := match[1], match[2]
			if len(first) == 0 {
				first, second = match[3], match[4]
			}
			up, a := strconv.ParseUint(string(first), 10, 53)
			down, b := strconv.ParseUint(string(second), 10, 53)
			if a != nil || b != nil {
				return 0, 0, used, errors.New("wan_rate_invalid")
			}
			return down, up, used, nil
		}
		if e != nil {
			return 0, 0, used, errors.New("wan_rate_missing")
		}
		if n == 0 {
			return 0, 0, used, io.ErrNoProgress
		}
	}
	return 0, 0, used, errors.New("wan_prefix_limit")
}
func (r *Router) pollOnce(ctx context.Context) bool {
	req, e := http.NewRequestWithContext(ctx, "GET", r.url, nil)
	if e != nil {
		return false
	}
	response, e := r.client.Do(req)
	code := "router_unreachable"
	var down, up uint64
	var n int
	if e == nil {
		defer response.Body.Close()
		if routerLogin(response.Request.URL.Path) {
			e = errors.New("router_login_required")
			code = e.Error()
		} else if response.StatusCode == 200 {
			down, up, n, e = readWAN(response.Body)
			if e != nil {
				code = e.Error()
			}
		} else {
			e = errors.New("router_http_" + strconv.Itoa(response.StatusCode))
			code = e.Error()
			if response.StatusCode >= 300 && response.StatusCode < 400 {
				if target, err := response.Location(); err == nil {
					if routerLogin(target.Path) {
						code += "_login_required"
					}

				}
			}
		}

	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.readBytes = n
	if e != nil {
		if r.errorCode != code {
			log.Printf("router collection failed: %s", code)
		}
		r.errorCode = code
		return false
	}
	r.down = down
	r.up = up
	r.last = time.Now()
	r.errorCode = ""
	return true
}
func (r *Router) Run(ctx context.Context) {
	delay := routerPollInterval
	for {
		start := time.Now()
		if r.poll(ctx) {
			delay = routerPollInterval
		}
		wait := delay - time.Since(start)
		if wait < 0 {
			wait = 0
		}
		timer := time.NewTimer(wait)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
		r.mu.Lock()
		failed := r.errorCode != ""
		r.mu.Unlock()
		if failed && delay < 40*time.Second {
			delay *= 2
		}
	}
}
func (r *Router) Snapshot(now time.Time) contracts.Object {
	r.mu.Lock()
	defer r.mu.Unlock()
	source := contracts.Object{"quality": "unconfigured", "observed_at": nil, "source_updated_at": nil, "age_ms": nil, "last_error_code": r.errorCode}
	missing := func() contracts.Object { return contracts.Object{"value": nil, "reason": r.errorCode} }
	down, up := missing(), missing()
	if !r.last.IsZero() {
		source["quality"] = "ok"
		source["observed_at"] = r.last.UTC().Format(time.RFC3339Nano)
		source["age_ms"] = float64(now.Sub(r.last).Milliseconds())
		ageSourceTTL(source, 0, 15000, 45000)
		down = contracts.Object{"value": r.down, "reason": nil}
		up = contracts.Object{"value": r.up, "reason": nil}
	} else if r.errorCode != "waiting_for_sample" {
		source["quality"] = "error"
	}
	if r.errorCode == "" {
		source["last_error_code"] = nil
	}
	return contracts.Object{"source": source, "download_bytes_per_second": down, "upload_bytes_per_second": up}
}
