package main

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestBX54PasswordTransform(t *testing.T) {
	if bx54Password("0123456789") != "TUbc12nopq" || bx54Password("!_-") != "!_-" {
		t.Fatal("BX54 script substitution mismatch")
	}
	if len(bx54Password("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")) != 62 {
		t.Fatal("substitution table length")
	}
}
func TestRouterAutomaticLoginAndExpiry(t *testing.T) {
	posts := 0
	session := "first"
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		switch req.URL.Path {
		case "/mobile.asp":
			cookie, err := req.Cookie("USERLOGINIDFLAG")
			if err != nil || cookie.Value != session {
				http.Redirect(w, req, "/router_password_mobile.asp", 302)
				return
			}
			io.WriteString(w, `var wan_rate="12;34"`)
		case "/router_password_mobile.asp":
			if req.Method == "GET" {
				io.WriteString(w, `<form name=router_password_set><input name=login_passwd></form>`)
				return
			}
			posts++
			req.ParseForm()
			flag, _ := req.Cookie("PSWMOBILEFLAG")
			if req.Form.Get("psd") != "TUb" || req.Form.Get("login_passwd") != "TUb" || flag == nil || flag.Value != "true" || req.Referer() == "" {
				t.Error("form did not reproduce observed browser login")
			}
			io.WriteString(w, `var resultInfo="true;0";var sessionid="`+session+`";`)
		}
	}))
	defer server.Close()
	router := NewRouter(server.URL + "/mobile.asp")
	router.credentialsPath = filepath.Join(t.TempDir(), "router-login.private.json")
	os.WriteFile(router.credentialsPath, []byte(`{"username":"","password":"012"}`), 0600)
	if !router.poll(context.Background()) || router.down != 34 || router.up != 12 || posts != 1 {
		t.Fatalf("automatic login failed: %s", router.errorCode)
	}
	if !router.poll(context.Background()) || posts != 1 {
		t.Fatal("active session was not reused")
	}
	session = "second"
	router.lastAuth = time.Now().Add(-2 * time.Minute)
	if !router.poll(context.Background()) || posts != 2 {
		t.Fatalf("expired session was not refreshed: %s", router.errorCode)
	}
}
func TestRouterBadPasswordStopsAutomaticRetries(t *testing.T) {
	posts := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/mobile.asp" {
			http.Redirect(w, r, "/router_password_mobile.asp", 302)
			return
		}
		if r.Method == "POST" {
			posts++
			io.WriteString(w, `var resultInfo="false;0";var sessionid="0";`)
			return
		}
		io.WriteString(w, `<form name=router_password_set><input name=login_passwd></form>`)
	}))
	defer server.Close()
	router := NewRouter(server.URL + "/mobile.asp")
	router.credentialsPath = filepath.Join(t.TempDir(), "router-login.private.json")
	os.WriteFile(router.credentialsPath, []byte(`{"password":"wrong"}`), 0600)
	for range 4 {
		router.lastAuth = time.Now().Add(-2 * time.Minute)
		if router.poll(context.Background()) {
			t.Fatal("invalid credentials accepted")
		}
	}
	if posts != 1 || router.errorCode != "router_auth_rejected_update_credentials" {
		t.Fatal("wrong password was retried")
	}
	os.WriteFile(router.credentialsPath, []byte(`{"password":"updated"}`), 0600)
	router.poll(context.Background())
	if posts != 2 {
		t.Fatal("updated credential was not read")
	}
}
func TestRouterLoginGlobalDeadline(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/mobile.asp" {
			http.Redirect(w, r, "/router_password_mobile.asp", 302)
			return
		}
		<-r.Context().Done()
	}))
	defer server.Close()
	router := NewRouter(server.URL + "/mobile.asp")
	router.credentialsPath = filepath.Join(t.TempDir(), "router-login.private.json")
	os.WriteFile(router.credentialsPath, []byte(`{"password":"example"}`), 0600)
	start := time.Now()
	if router.poll(context.Background()) || time.Since(start) > time.Second {
		t.Fatal("login exceeded total deadline")
	}
}
