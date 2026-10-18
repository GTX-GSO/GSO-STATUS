package main

import (
	"encoding/json"
	"io"
	"mime"
	"net/http"
	"strings"
	"time"
)

func (s *Server) serveHome(w http.ResponseWriter, r *http.Request) {
	const route = "/api/v1/home/commands"
	if r.URL.Path == route && r.Method == "POST" {
		if s.config.HomeControlToken == "" {
			fail(w, 503, "home_controls_unconfigured")
			return
		}
		if !auth(r, s.config.HomeControlToken) {
			fail(w, 401, "unauthorized")
			return
		}
		media, _, e := mime.ParseMediaType(r.Header.Get("Content-Type"))
		if e != nil || media != "application/json" {
			fail(w, 415, "unsupported_media_type")
			return
		}
		r.Body = http.MaxBytesReader(w, r.Body, 2048)
		body, e := io.ReadAll(r.Body)
		if e != nil {
			fail(w, 413, "payload_too_large")
			return
		}
		result, status, code := s.home.Submit(body, time.Now())
		if code != "" {
			fail(w, status, code)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(status)
		json.NewEncoder(w).Encode(result)
		return
	}
	if strings.HasPrefix(r.URL.Path, route+"/") && r.Method == "GET" {
		if !auth(r, s.config.ReadToken) {
			fail(w, 401, "unauthorized")
			return
		}
		id := strings.TrimPrefix(r.URL.Path, route+"/")
		if len(id) != 36 || strings.Contains(id, "/") {
			fail(w, 404, "command_not_found")
			return
		}
		result, ok := s.home.Get(id)
		if !ok {
			fail(w, 404, "command_not_found")
			return
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(result)
		return
	}
	fail(w, 404, "not_found")
}
