package main

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net"
	"regexp"
	"strings"
	"sync"
	"time"

	"gso-status/contracts"
)

// Optional Bambu Lab printer status for the PRINT card. The printer's local MQTT broker (LAN mode, developer mode)
// pushes its job state over TLS on port 8883 with the user name "bblp" and the printer's access code as password.
// GSN only subscribes (to the printer's own topic; its serial is read from the TLS certificate), plus one "pushall" request per session and every quarter hour, which asks the printer to send its
// complete status; it never sends a control command. The printer uses a certificate that no public CA signed, so the
// connection is encrypted but the broker is identified only by the address and the access code.
type PrinterConfig struct {
	Host       string `json:"host"`
	AccessCode string `json:"access_code"`
	Serial     string `json:"serial,omitempty"`
}

const (
	printerPort       = "8883"
	printerStale      = 30 * time.Second
	printerOffline    = 90 * time.Second
	printerPushEvery  = 15 * time.Minute
	printerPingEvery  = 20 * time.Second
	printerReadWindow = 45 * time.Second
)

var (
	printerHostPattern   = regexp.MustCompile(`^[A-Za-z0-9]([A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$`)
	printerCodePattern   = regexp.MustCompile(`^[A-Za-z0-9]{4,32}$`)
	printerSerialPattern = regexp.MustCompile(`^[A-Za-z0-9]{6,32}$`)
)

func (c PrinterConfig) configured() bool { return c.Host != "" || c.AccessCode != "" || c.Serial != "" }

func validatePrinter(c PrinterConfig) error {
	if !c.configured() {
		return nil
	}
	if !printerHostPattern.MatchString(c.Host) || !printerCodePattern.MatchString(c.AccessCode) || (c.Serial != "" && !printerSerialPattern.MatchString(c.Serial)) {
		return errors.New("printer needs host and access_code (4-32 letters or digits); serial is optional")
	}
	return nil
}

type PrinterSource struct {
	cfg  PrinterConfig
	addr string
	dial func(ctx context.Context, addr string) (net.Conn, error)
	mu   sync.Mutex
	// latest values merged from the printer's reports (P1/A1 printers send only what changed)
	numbers  map[string]float64
	gcode    string
	jobName  string
	seen     time.Time // last packet of any kind, so a quiet idle printer still counts as alive
	lastErr  string
	serial   string
	sessions int
	retry    time.Duration // first reconnect delay; doubles up to 30 s
	// model picture of the current job (see thumbnail.go)
	gcodeFile   string
	thumbKey    string
	thumb       []byte
	thumbTried  map[string]time.Time
	thumbAfter  time.Duration // wait before trying a picture again after it failed
	ftpAddr     string
	fetch       func(ctx context.Context, job string, plate int, gcodeFile string) ([]byte, error)
	thumbErrors int
	thumbTick   time.Duration // how often the loop looks at the job
}

func NewPrinterSource(c PrinterConfig) *PrinterSource {
	s := &PrinterSource{cfg: c, serial: c.Serial, numbers: map[string]float64{}, retry: 2 * time.Second, thumbTried: map[string]time.Time{}, thumbAfter: 45 * time.Second, thumbTick: 2 * time.Second}
	if c.configured() {
		s.addr = net.JoinHostPort(c.Host, printerPort)
		s.ftpAddr = net.JoinHostPort(c.Host, "990")
	}
	s.fetch = s.fetchFromPrinter
	s.dial = func(ctx context.Context, addr string) (net.Conn, error) {
		d := tls.Dialer{NetDialer: &net.Dialer{Timeout: 5 * time.Second}, Config: &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS12}}
		return d.DialContext(ctx, "tcp", addr)
	}
	return s
}

func (s *PrinterSource) Run(ctx context.Context) {
	if !s.cfg.configured() {
		return
	}
	go s.thumbnailLoop(ctx)
	wait := s.retry
	for ctx.Err() == nil {
		started := time.Now()
		err := s.session(ctx)
		s.mu.Lock()
		switch {
		case errors.Is(err, errMQTTRefused):
			s.lastErr = "printer_auth_failed"
		case err != nil:
			s.lastErr = "printer_unreachable"
		}
		s.mu.Unlock()
		if time.Since(started) > time.Minute {
			wait = s.retry
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(wait):
		}
		if wait < 30*time.Second {
			wait *= 2
		}
	}
}

func (s *PrinterSource) touch() {
	s.mu.Lock()
	s.seen = time.Now()
	s.mu.Unlock()
}

// session runs one MQTT connection until it fails or the context ends.
func (s *PrinterSource) session(ctx context.Context) error {
	conn, err := s.dial(ctx, s.addr)
	if err != nil {
		return err
	}
	defer conn.Close()
	m := newMQTTConn(conn)
	if err = m.connect("gso-status-"+randomSuffix(), "bblp", s.cfg.AccessCode, 8*time.Second); err != nil {
		return err
	}
	// The printer's broker only accepts its own topic, and its TLS certificate is issued to the serial number, so the
	// serial needs no configuration. A broker without that certificate (or an older firmware) is asked for any device.
	serial := s.cfg.Serial
	if tc, ok := conn.(*tls.Conn); ok && serial == "" {
		if certs := tc.ConnectionState().PeerCertificates; len(certs) > 0 && printerSerialPattern.MatchString(certs[0].Subject.CommonName) {
			serial = certs[0].Subject.CommonName
		}
	}
	filter := "device/+/report"
	if serial != "" {
		filter = "device/" + serial + "/report"
		s.mu.Lock()
		s.serial = serial
		s.mu.Unlock()
	}
	if err = m.subscribe(filter, 8*time.Second); err != nil {
		return err
	}
	s.touch()
	s.mu.Lock()
	s.lastErr = ""
	s.sessions++
	s.mu.Unlock()
	done := make(chan struct{})
	defer close(done)
	requests := make(chan struct{}, 1)
	go func() { // keep-alive and the periodic full-status request
		ping, push := time.NewTicker(printerPingEvery), time.NewTicker(printerPushEvery)
		defer ping.Stop()
		defer push.Stop()
		for {
			select {
			case <-done:
				return
			case <-ping.C:
				if m.ping() != nil {
					conn.Close()
					return
				}
			case <-push.C:
				select {
				case requests <- struct{}{}:
				default:
				}
			}
		}
	}()
	go func() { // closing the connection ends a blocked read when the context is cancelled
		select {
		case <-ctx.Done():
			conn.Close()
		case <-done:
		}
	}()
	asked := false
	if serial != "" { // a known serial needs no first report to ask for the full status
		asked = true
		m.publish("device/"+serial+"/request", []byte(printerPushAll))
	}
	for {
		conn.SetReadDeadline(time.Now().Add(printerReadWindow))
		p, err := m.read()
		if err != nil {
			return err
		}
		s.touch()
		topic, payload, ok := m.message(p)
		if !ok {
			continue
		}
		if learned := printerSerialFromTopic(topic); learned != "" {
			s.mu.Lock()
			s.serial = learned
			s.mu.Unlock()
			if !asked {
				asked = true
				m.publish("device/"+learned+"/request", []byte(printerPushAll))
			}
		}
		s.merge(payload)
		select {
		case <-requests:
			s.mu.Lock()
			serial := s.serial
			s.mu.Unlock()
			if serial != "" {
				m.publish("device/"+serial+"/request", []byte(printerPushAll))
			}
		default:
		}
	}
}

func printerSerialFromTopic(topic string) string {
	parts := strings.Split(topic, "/")
	if len(parts) == 3 && parts[0] == "device" && parts[2] == "report" && printerSerialPattern.MatchString(parts[1]) {
		return parts[1]
	}
	return ""
}

func randomSuffix() string {
	b := make([]byte, 4)
	if _, err := rand.Read(b); err != nil {
		return "0000"
	}
	return hex.EncodeToString(b)
}

const printerPushAll = `{"pushing":{"sequence_id":"0","command":"pushall"}}`

var printerNumberFields = []string{"mc_percent", "mc_remaining_time", "layer_num", "total_layer_num", "nozzle_temper", "nozzle_target_temper", "bed_temper", "bed_target_temper", "print_error"}

// merge folds the fields of one report's "print" object into the remembered state; anything else is ignored.
func (s *PrinterSource) merge(payload []byte) {
	var report struct {
		Print map[string]json.RawMessage `json:"print"`
	}
	if json.Unmarshal(payload, &report) != nil || report.Print == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, key := range printerNumberFields {
		var v float64
		if raw, ok := report.Print[key]; ok && json.Unmarshal(raw, &v) == nil {
			s.numbers[key] = v
		}
	}
	var text string
	if raw, ok := report.Print["gcode_state"]; ok && json.Unmarshal(raw, &text) == nil {
		s.gcode = text
	}
	if raw, ok := report.Print["subtask_name"]; ok && json.Unmarshal(raw, &text) == nil {
		s.jobName = text
	}
	if raw, ok := report.Print["gcode_file"]; ok && json.Unmarshal(raw, &text) == nil {
		s.gcodeFile = text
	}
}

func printerState(gcode string) string {
	switch strings.ToUpper(gcode) {
	case "IDLE":
		return "idle"
	case "INIT", "SLICING", "PREPARE":
		return "preparing"
	case "RUNNING":
		return "running"
	case "PAUSE":
		return "paused"
	case "FINISH":
		return "finished"
	case "FAILED":
		return "failed"
	}
	return "unknown"
}

func (s *PrinterSource) Snapshot(now time.Time) contracts.Object {
	missing := func(reason string) contracts.Object { return contracts.Object{"value": nil, "reason": reason} }
	field := func(reason string, key string, low, high float64) contracts.Object {
		s.mu.Lock()
		v, ok := s.numbers[key]
		s.mu.Unlock()
		switch {
		case reason != "":
			return missing(reason)
		case !ok:
			return missing("not_reported")
		case v < low || v > high || v != v:
			return missing("invalid_value")
		}
		return contracts.Object{"value": v, "reason": nil}
	}
	build := func(source contracts.Object, state string, name any, reason string) contracts.Object {
		return contracts.Object{
			"source": source, "state": state, "job_name": name,
			"progress_percent":  field(reason, "mc_percent", 0, 100),
			"remaining_minutes": field(reason, "mc_remaining_time", 0, 1e6),
			"layer":             field(reason, "layer_num", 0, 1e6),
			"layer_total":       field(reason, "total_layer_num", 0, 1e6),
			"nozzle_c":          field(reason, "nozzle_temper", 0, 400),
			"nozzle_target_c":   field(reason, "nozzle_target_temper", 0, 400),
			"bed_c":             field(reason, "bed_temper", 0, 400),
			"bed_target_c":      field(reason, "bed_target_temper", 0, 400),
			"error_code":        field(reason, "print_error", 0, 4294967295),
			"thumbnail_id":      nil,
		}
	}
	source := contracts.Object{"quality": "unconfigured", "observed_at": nil, "source_updated_at": nil, "age_ms": nil, "last_error_code": "not_configured"}
	if !s.cfg.configured() {
		return build(source, "unknown", nil, "not_configured")
	}
	s.mu.Lock()
	seen, lastErr, gcode, job := s.seen, s.lastErr, s.gcode, s.jobName
	s.mu.Unlock()
	if seen.IsZero() {
		source["quality"], source["last_error_code"] = "error", "waiting_for_printer"
		if lastErr != "" {
			source["last_error_code"] = lastErr
		}
		return build(source, "unknown", nil, "printer_unavailable")
	}
	age := now.Sub(seen)
	if age < 0 {
		age = 0
	}
	source = contracts.Object{"quality": "ok", "observed_at": seen.UTC().Format(time.RFC3339Nano), "source_updated_at": nil, "age_ms": float64(age.Milliseconds()), "last_error_code": nil}
	switch {
	case age > printerOffline:
		source["quality"] = "offline"
		if lastErr != "" {
			source["last_error_code"] = lastErr
		}
		return build(source, "unknown", nil, "printer_offline")
	case age > printerStale:
		source["quality"] = "stale"
	}
	var name any
	if job = strings.TrimSpace(job); job != "" {
		runes := []rune(job)
		if len(runes) > 120 {
			job = string(runes[:120])
		}
		name = job
	}
	state := printerState(gcode)
	result := build(source, state, name, "")
	result["thumbnail_id"] = nil
	if id, _ := s.Thumbnail(now); id != "" && state != "idle" && state != "unknown" {
		result["thumbnail_id"] = id
	}
	return result
}

// Thumbnail returns the picture of the job being shown (id, raw ARGB8888), or nothing.
func (s *PrinterSource) Thumbnail(now time.Time) (string, []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.thumb == nil || now.Sub(s.seen) > printerOffline {
		return "", nil
	}
	return s.thumbKey, s.thumb
}

// wantedPicture is the job whose picture should be on show: only while a job exists and the printer is reachable.
func (s *PrinterSource) wantedPicture(now time.Time) (job string, plate int, gcodeFile string, ok bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	state := printerState(s.gcode)
	job = strings.TrimSpace(s.jobName)
	if s.seen.IsZero() || now.Sub(s.seen) > printerStale || job == "" || state == "idle" || state == "unknown" || strings.ContainsAny(job, "\r\n") {
		return "", 0, "", false
	}
	return job, plateNumber(s.gcodeFile), s.gcodeFile, true
}

// thumbnailLoop fetches the picture once per job (a failed attempt waits before it is repeated) and drops the old
// picture as soon as the job changes, so one model's picture is never shown for another.
func (s *PrinterSource) thumbnailLoop(ctx context.Context) {
	tick := time.NewTicker(s.thumbTick)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
		now := time.Now()
		job, plate, gcodeFile, ok := s.wantedPicture(now)
		s.mu.Lock()
		current := s.thumbKey
		s.mu.Unlock()
		if !ok {
			continue
		}
		key := thumbnailKey(job, plate)
		if key == current {
			continue
		}
		s.mu.Lock()
		s.thumbKey, s.thumb = "", nil
		last, tried := s.thumbTried[key]
		wait := s.thumbAfter
		s.mu.Unlock()
		if tried && now.Sub(last) < wait {
			continue
		}
		fetchCtx, cancel := context.WithTimeout(ctx, 40*time.Second)
		data, err := s.fetch(fetchCtx, job, plate, gcodeFile)
		cancel()
		s.mu.Lock()
		s.thumbTried[key] = time.Now()
		if len(s.thumbTried) > 32 {
			s.thumbTried = map[string]time.Time{key: s.thumbTried[key]}
		}
		if err == nil && len(data) == thumbnailBytes {
			s.thumbKey, s.thumb = key, data
		} else {
			s.thumbErrors++
		}
		s.mu.Unlock()
	}
}

// pictureFiles lists where a job's archive may be: files sent from Studio over the LAN sit in the root of the printer's
// storage under the name in gcode_file, others in /cache under the job name (with or without ".gcode").
func pictureFiles(job, gcodeFile string) []string {
	var names []string
	seen := map[string]bool{}
	add := func(path string) {
		if !seen[path] && !strings.ContainsAny(path, "\r\n") {
			seen[path] = true
			names = append(names, path)
		}
	}
	if strings.HasSuffix(gcodeFile, ".3mf") && !strings.Contains(gcodeFile, "/") {
		add("/" + gcodeFile)
		add("/cache/" + gcodeFile)
	}
	for _, dir := range []string{"/cache/", "/"} {
		add(dir + job + ".3mf")
		add(dir + job + ".gcode.3mf")
	}
	return names
}

// fetchFromPrinter reads the start of the job's .3mf over FTPS and renders the plate picture. A candidate that does not
// exist is answered at once and leaves the connection usable; one that does ends the search.
func (s *PrinterSource) fetchFromPrinter(ctx context.Context, job string, plate int, gcodeFile string) ([]byte, error) {
	c, err := dialFTPS(ctx, s.ftpAddr, "bblp", s.cfg.AccessCode)
	if err != nil {
		return nil, err
	}
	defer c.Close()
	for _, path := range pictureFiles(job, gcodeFile) {
		prefix, err := c.readPrefix(ctx, path, thumbnailPrefix)
		if err != nil || len(prefix) == 0 {
			continue
		}
		picture, err := zipPicture(prefix, plate)
		if err != nil {
			return nil, err
		}
		return renderThumbnail(picture)
	}
	return nil, errors.New("thumbnail_file_missing")
}
