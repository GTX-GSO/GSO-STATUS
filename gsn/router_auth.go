package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strings"
	"time"
)

var routerResultPattern = regexp.MustCompile(`\bvar\s+resultInfo\s*=\s*["']([^"'\r\n]{1,100})["']`)
var routerSessionPattern = regexp.MustCompile(`\bvar\s+sessionid\s*=\s*["']([^"'\r\n]{1,256})["']`)
var routerWaitPattern = regexp.MustCompile(`\bvar\s+LoginErrOutWait\s*=\s*["']errout;`)

// Implements only the form and substitution observed in this BX54's own scripts.
func bx54Password(password string) string {
	const source = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
	const target = "TUbc12nopq345VWX89aPQRSeABxyzGHIJYZ67CDEFfghijklmd0rstuvwKLMNO"
	return strings.Map(func(ch rune) rune {
		if i := strings.IndexRune(source, ch); i >= 0 {
			return rune(target[i])
		}
		return ch
	}, password)
}
func (r *Router) authenticate(ctx context.Context) string {
	if r.credentialsPath == "" {
		return "router_auth_not_configured"
	}
	f, err := os.Open(r.credentialsPath)
	if err != nil {
		return "router_auth_credentials_unavailable"
	}
	raw, err := io.ReadAll(io.LimitReader(f, 4097))
	f.Close()
	if err != nil || len(raw) > 4096 {
		return "router_auth_credentials_invalid"
	}
	var credentials struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	decoder := json.NewDecoder(bytes.NewReader(bytes.TrimPrefix(raw, []byte{0xef, 0xbb, 0xbf})))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&credentials) != nil || decoder.Decode(new(any)) != io.EOF || len(credentials.Password) == 0 || len(credentials.Password) > 256 {
		return "router_auth_credentials_invalid"
	}
	fingerprint := sha256.Sum256([]byte(credentials.Password))
	if r.credentialsRejected && fingerprint == r.badCredentialHash {
		return "router_auth_rejected_update_credentials"
	}
	if r.credentialsRejected {
		r.credentialsRejected = false
		r.lastAuth = time.Time{}
	}
	if time.Since(r.lastAuth) < time.Minute {
		return "router_auth_cooldown"
	}
	r.lastAuth = time.Now()
	origin, err := url.Parse(r.url)
	if err != nil {
		return "router_auth_url_invalid"
	}
	login := origin.ResolveReference(&url.URL{Path: "/router_password_mobile.asp"})
	// Separate redirect policy for the known login flow; cookie jar is shared with polling.
	client := &http.Client{Transport: r.client.Transport, Jar: r.client.Jar, Timeout: 800 * time.Millisecond, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if len(via) > 2 || req.URL.Scheme != origin.Scheme || !strings.EqualFold(req.URL.Host, origin.Host) || req.URL.User != nil {
			return http.ErrUseLastResponse
		}
		return nil
	}}
	read := func(req *http.Request) ([]byte, *http.Response, error) {
		req.Header.Set("Referer", login.String())
		req.Header.Set("User-Agent", "GSO-STATUS-traffic-reader/0.1")
		response, e := client.Do(req)
		if e != nil {
			return nil, nil, e
		}
		defer response.Body.Close()
		body, e := io.ReadAll(io.LimitReader(response.Body, 131073))
		if len(body) > 131072 {
			return nil, response, io.ErrShortBuffer
		}
		return body, response, e
	}
	get, _ := http.NewRequestWithContext(ctx, "GET", login.String(), nil)
	page, response, err := read(get)
	if err != nil || response.StatusCode != 200 {
		return "router_auth_page_unavailable"
	}
	if strings.HasSuffix(response.Request.URL.Path, "/noauth_password_mobile.asp") {
		return ""
	}
	if routerWaitPattern.Match(page) {
		return "router_auth_locked"
	}
	if response.Request.URL.Path != login.Path || !bytes.Contains(page, []byte("router_password_set")) || !bytes.Contains(page, []byte("login_passwd")) {
		return "router_auth_form_unsupported"
	}
	r.client.Jar.SetCookies(login, []*http.Cookie{{Name: "PSWMOBILEFLAG", Value: "true", Path: "/"}, {Name: "LOGIN_PSD_REM_FLAG", Value: "0", Path: "/"}})
	encoded := bx54Password(credentials.Password)
	form := url.Values{"psd": {encoded}, "login_passwd": {encoded}}
	post, _ := http.NewRequestWithContext(ctx, "POST", login.String(), strings.NewReader(form.Encode()))
	post.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	post.Header.Set("Origin", origin.Scheme+"://"+origin.Host)
	result, response, err := read(post)
	if err != nil {
		return "router_auth_request_failed"
	}
	if response.StatusCode != 200 {
		return "router_auth_http_error"
	}
	// An HTTP redirect directly to the status page is also valid; polling verifies WAN.
	if response.Request.URL.Path == origin.Path {
		return ""
	}
	match := routerResultPattern.FindSubmatch(result)
	if match == nil {
		return "router_auth_result_missing"
	}
	state := strings.SplitN(string(match[1]), ";", 2)[0]
	if state == "errout" {
		return "router_auth_locked"
	}
	if state != "true" {
		r.credentialsRejected = true
		r.badCredentialHash = fingerprint
		return "router_auth_rejected_update_credentials"
	}
	session := routerSessionPattern.FindSubmatch(result)
	if session == nil {
		return "router_auth_session_missing"
	}
	r.client.Jar.SetCookies(login, []*http.Cookie{{Name: "USERLOGINIDFLAG", Value: string(session[1]), Path: "/"}})
	return ""
}
func (r *Router) poll(ctx context.Context) bool {
	ctx, cancel := context.WithTimeout(ctx, 800*time.Millisecond)
	defer cancel()
	if r.pollOnce(ctx) {
		return true
	}
	r.mu.Lock()
	code := r.errorCode
	r.mu.Unlock()
	if !strings.Contains(code, "login_required") || r.credentialsPath == "" {
		return false
	}
	if authError := r.authenticate(ctx); authError != "" {
		if authError != r.lastAuthError {
			log.Printf("router automatic login: %s", authError)
			r.lastAuthError = authError
		}
		r.mu.Lock()
		r.errorCode = authError
		r.mu.Unlock()
		return false
	}
	r.lastAuthError = ""
	return r.pollOnce(ctx)
}
