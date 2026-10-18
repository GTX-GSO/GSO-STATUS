package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"math/big"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	"gso-status/contracts"
)

// A stand-in for the printer's MQTT broker: TLS with a throw-away certificate, the same login, and reports pushed on demand.
type fakePrinter struct {
	l        net.Listener
	code     string
	refuse   byte
	strict   bool // close the connection on a wildcard subscription, as the real printer does
	mu       sync.Mutex
	requests []string // "topic payload" of every PUBLISH the client sent
	acks     int
	logins   int
	send     chan []byte // raw packets to push to the client
}

const fakeSerial = "01P00A123456789"

// newFakePrinter behaves like the real broker: the certificate is issued to the serial number and any subscription other
// than the printer's own topic gets the connection closed.
func newFakePrinter(t *testing.T, code string) *fakePrinter {
	t.Helper()
	f := newFakePrinterWith(t, code, fakeSerial)
	f.strict = true
	return f
}

func newFakePrinterWith(t *testing.T, code, commonName string) *fakePrinter {
	t.Helper()
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	template := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: commonName}, NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour)}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	l, err := tls.Listen("tcp", "127.0.0.1:0", &tls.Config{Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: key}}})
	if err != nil {
		t.Fatal(err)
	}
	f := &fakePrinter{l: l, code: code, send: make(chan []byte, 16)}
	go f.serve()
	t.Cleanup(func() { l.Close() })
	return f
}

func (f *fakePrinter) addr() string { return f.l.Addr().String() }

func packet(kind, flags byte, body []byte) []byte {
	return append(append([]byte{kind<<4 | flags}, mqttLength(len(body))...), body...)
}

func (f *fakePrinter) report(topic string, qos byte, payload string) []byte {
	body := mqttString(nil, topic)
	if qos > 0 {
		body = append(body, 0, 7)
	}
	return packet(mqttPublish, qos<<1, append(body, payload...))
}

func (f *fakePrinter) serve() {
	for {
		c, err := f.l.Accept()
		if err != nil {
			return
		}
		go f.handle(c)
	}
}

func (f *fakePrinter) handle(c net.Conn) {
	defer c.Close()
	m := newMQTTConn(c)
	p, err := m.read()
	if err != nil || p.kind != mqttConnect {
		return
	}
	// protocol name, level, flags, keep-alive, then client id, user, password
	rest := p.payload[10:]
	field := func() string {
		n := int(rest[0])<<8 | int(rest[1])
		v := string(rest[2 : 2+n])
		rest = rest[2+n:]
		return v
	}
	field()
	user, password := field(), field()
	f.mu.Lock()
	f.logins++
	f.mu.Unlock()
	if f.refuse != 0 || user != "bblp" || password != f.code {
		code := f.refuse
		if code == 0 {
			code = 5
		}
		c.Write(packet(mqttConnack, 0, []byte{0, code}))
		return
	}
	c.Write(packet(mqttConnack, 0, []byte{0, 0}))
	for {
		p, err := m.read()
		if err != nil {
			return
		}
		switch p.kind {
		case mqttSubscribe:
			if n := int(p.payload[2])<<8 | int(p.payload[3]); f.strict && !strings.HasSuffix(string(p.payload[4:4+n]), fakeSerial+"/report") {
				return
			}
			c.Write(packet(mqttSuback, 0, []byte{p.payload[0], p.payload[1], 0}))
			go func() { // reports are pushed only once the subscription is acknowledged
				for raw := range f.send {
					if _, err := c.Write(raw); err != nil {
						return
					}
				}
			}()
		case mqttPingreq:
			c.Write(packet(mqttPingresp, 0, nil))
		case mqttPublish:
			topic, payload, _ := m.message(p)
			f.mu.Lock()
			f.requests = append(f.requests, topic+" "+string(payload))
			f.mu.Unlock()
		case mqttPuback:
			f.mu.Lock()
			f.acks++
			f.mu.Unlock()
		}
	}
}

func (f *fakePrinter) sent() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.requests...)
}

func startPrinter(t *testing.T, f *fakePrinter, serial string) *PrinterSource {
	t.Helper()
	s := NewPrinterSource(PrinterConfig{Host: "127.0.0.1", AccessCode: f.code, Serial: serial})
	s.addr, s.retry, s.thumbTick = f.addr(), 20*time.Millisecond, 20*time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	go s.Run(ctx)
	t.Cleanup(cancel)
	return s
}

func waitFor(t *testing.T, what string, ok func() bool) {
	t.Helper()
	for i := 0; i < 300; i++ {
		if ok() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func metricValue(o contracts.Object, key string) any {
	m, _ := o[key].(contracts.Object)
	return m["value"]
}

const printerFull = `{"print":{"command":"push_status","gcode_state":"RUNNING","mc_percent":42,"mc_remaining_time":75,"layer_num":10,"total_layer_num":100,"nozzle_temper":219.6,"nozzle_target_temper":220,"bed_temper":60,"bed_target_temper":60,"subtask_name":"Benchy","print_error":0,"hms":[]}}`

func TestPrinterStatusAndPushall(t *testing.T) {
	f := newFakePrinter(t, "testcode1")
	s := startPrinter(t, f, "")
	waitFor(t, "pushall request right after subscribing", func() bool { return len(f.sent()) == 1 })
	if got := f.sent()[0]; got != `device/01P00A123456789/request {"pushing":{"sequence_id":"0","command":"pushall"}}` {
		t.Fatalf("request: %s", got)
	}
	f.send <- f.report("device/01P00A123456789/report", 0, printerFull)
	waitFor(t, "running state", func() bool { return s.Snapshot(time.Now())["state"] == "running" })
	snap := s.Snapshot(time.Now())
	if metricValue(snap, "progress_percent") != 42.0 || metricValue(snap, "remaining_minutes") != 75.0 || metricValue(snap, "layer") != 10.0 || metricValue(snap, "layer_total") != 100.0 || metricValue(snap, "nozzle_c") != 219.6 || metricValue(snap, "bed_target_c") != 60.0 || metricValue(snap, "error_code") != 0.0 || snap["job_name"] != "Benchy" {
		t.Fatalf("snapshot: %v", snap)
	}
	if q := snap["source"].(contracts.Object)["quality"]; q != "ok" {
		t.Fatalf("quality %v", q)
	}
	// P1/A1 printers send only what changed: the rest must be kept
	f.send <- f.report("device/01P00A123456789/report", 0, `{"print":{"mc_percent":43,"nozzle_temper":220.1}}`)
	waitFor(t, "delta", func() bool { return metricValue(s.Snapshot(time.Now()), "progress_percent") == 43.0 })
	snap = s.Snapshot(time.Now())
	if snap["state"] != "running" || snap["job_name"] != "Benchy" || metricValue(snap, "layer_total") != 100.0 || metricValue(snap, "nozzle_c") != 220.1 {
		t.Fatalf("delta lost state: %v", snap)
	}
	f.send <- f.report("device/01P00A123456789/report", 0, `{"print":{"gcode_state":"PAUSE"}}`)
	waitFor(t, "pause", func() bool { return s.Snapshot(time.Now())["state"] == "paused" })
	f.send <- f.report("device/01P00A123456789/report", 0, `{"print":{"gcode_state":"FINISH","mc_percent":100,"subtask_name":""}}`)
	waitFor(t, "finish", func() bool { return s.Snapshot(time.Now())["state"] == "finished" })
	if snap = s.Snapshot(time.Now()); snap["job_name"] != nil || metricValue(snap, "progress_percent") != 100.0 {
		t.Fatalf("finished: %v", snap)
	}
	if n := len(f.sent()); n != 1 {
		t.Fatalf("pushall must be requested once per session, got %d requests", n)
	}
}

// An older firmware or another broker has no serial in its certificate: ask for any device and learn the serial from the topic.
func TestPrinterLearnsSerialFromTopic(t *testing.T) {
	f := newFakePrinterWith(t, "testcode1", "printer.local")
	s := startPrinter(t, f, "")
	f.send <- f.report("device/01P00A123456789/report", 0, `{"info":{"command":"get_version"}}`)
	waitFor(t, "pushall after the first report", func() bool { return len(f.sent()) == 1 })
	if got := f.sent()[0]; got != `device/01P00A123456789/request {"pushing":{"sequence_id":"0","command":"pushall"}}` {
		t.Fatalf("request: %s", got)
	}
	f.send <- f.report("device/01P00A123456789/report", 0, printerFull)
	waitFor(t, "running state", func() bool { return s.Snapshot(time.Now())["state"] == "running" })
}

func TestPrinterLargeAndQoS1Reports(t *testing.T) {
	f := newFakePrinter(t, "testcode1")
	s := startPrinter(t, f, "01P00A123456789")
	waitFor(t, "pushall right after subscribing when the serial is configured", func() bool { return len(f.sent()) == 1 })
	big := `{"print":{"gcode_state":"RUNNING","mc_percent":7,"hms":["` + strings.Repeat("x", 300000) + `"]}}`
	f.send <- f.report("device/01P00A123456789/report", 1, big)
	waitFor(t, "large qos1 report", func() bool { return metricValue(s.Snapshot(time.Now()), "progress_percent") == 7.0 })
	waitFor(t, "puback", func() bool { f.mu.Lock(); defer f.mu.Unlock(); return f.acks == 1 })
	f.send <- f.report("device/OTHER000001/report", 0, `{"print":{"mc_percent":99}}`)
	f.send <- f.report("device/01P00A123456789/report", 0, `not json`)
	f.send <- f.report("device/01P00A123456789/report", 0, `{"print":{"mc_percent":8}}`)
	waitFor(t, "later report after garbage", func() bool { return metricValue(s.Snapshot(time.Now()), "progress_percent") == 8.0 })
}

func TestPrinterRefusedLoginAndRecovery(t *testing.T) {
	f := newFakePrinter(t, "right-code")
	s := NewPrinterSource(PrinterConfig{Host: "127.0.0.1", AccessCode: "wrongcode"})
	s.addr, s.retry = f.addr(), 20*time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go s.Run(ctx)
	waitFor(t, "refused login", func() bool {
		o := s.Snapshot(time.Now())["source"].(contracts.Object)
		return o["quality"] == "error" && o["last_error_code"] == "printer_auth_failed"
	})
	if snap := s.Snapshot(time.Now()); snap["state"] != "unknown" || snap["job_name"] != nil {
		t.Fatalf("no data yet: %v", snap)
	}
	f.mu.Lock()
	if f.logins < 2 {
		f.mu.Unlock()
		waitFor(t, "retry", func() bool { f.mu.Lock(); defer f.mu.Unlock(); return f.logins >= 2 })
	} else {
		f.mu.Unlock()
	}
}

func TestPrinterStalenessAndOffline(t *testing.T) {
	f := newFakePrinter(t, "testcode1")
	s := startPrinter(t, f, "")
	f.send <- f.report("device/01P00A123456789/report", 0, printerFull)
	waitFor(t, "data", func() bool { return s.Snapshot(time.Now())["state"] == "running" })
	now := time.Now()
	quality := func(at time.Time) any { return s.Snapshot(at)["source"].(contracts.Object)["quality"] }
	if quality(now.Add(10*time.Second)) != "ok" || quality(now.Add(45*time.Second)) != "stale" || quality(now.Add(2*time.Minute)) != "offline" {
		t.Fatal("quality does not follow the age of the last packet")
	}
	offline := s.Snapshot(now.Add(2 * time.Minute))
	if offline["state"] != "unknown" || offline["job_name"] != nil || metricValue(offline, "progress_percent") != nil {
		t.Fatalf("an offline printer must not keep showing its last job: %v", offline)
	}
	if reason := offline["progress_percent"].(contracts.Object)["reason"]; reason != "printer_offline" {
		t.Fatalf("reason %v", reason)
	}
}

func TestPrinterInServerSnapshot(t *testing.T) {
	f := newFakePrinter(t, "testcode1")
	c := config()
	c.Printer = PrinterConfig{Host: "127.0.0.1", AccessCode: f.code}
	s := NewServer(c)
	s.printer.addr, s.printer.retry = f.addr(), 20*time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go s.printer.Run(ctx)
	f.send <- f.report("device/01P00A123456789/report", 0, printerFull)
	waitFor(t, "data", func() bool { return s.printer.Snapshot(time.Now())["state"] == "running" })
	s.mu.Lock()
	s.refresh(time.Now())
	s.mu.Unlock()
	w := request(s, "GET", "/api/v1/snapshot", c.ReadToken, nil)
	if w.Code != 200 {
		t.Fatalf("snapshot status %d", w.Code)
	}
	var body map[string]any
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	printer, ok := body["printer"].(map[string]any)
	if !ok || printer["state"] != "running" || printer["job_name"] != "Benchy" {
		t.Fatalf("printer in snapshot: %v", body["printer"])
	}
	if w.Body.Len() > 16000 {
		t.Fatalf("snapshot grew to %d bytes", w.Body.Len())
	}
}

func TestUnconfiguredPrinterSnapshotValidates(t *testing.T) {
	s := NewServer(config())
	snap := s.printer.Snapshot(time.Now())
	if snap["source"].(contracts.Object)["quality"] != "unconfigured" || snap["state"] != "unknown" {
		t.Fatalf("%v", snap)
	}
	w := request(s, "GET", "/api/v1/snapshot", config().ReadToken, nil)
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"printer"`) {
		t.Fatalf("snapshot without a printer must still carry the unconfigured object: %d", w.Code)
	}
}

func TestPrinterConfigValidation(t *testing.T) {
	for _, tc := range []struct {
		c  PrinterConfig
		ok bool
	}{
		{PrinterConfig{}, true},
		{PrinterConfig{Host: "192.168.1.20", AccessCode: "testcode1"}, true},
		{PrinterConfig{Host: "a1mini.lan", AccessCode: "testcode1", Serial: "0309DA123456789"}, true},
		{PrinterConfig{Host: "192.168.1.20"}, false},
		{PrinterConfig{AccessCode: "testcode1"}, false},
		{PrinterConfig{Host: "http://192.168.1.20", AccessCode: "testcode1"}, false},
		{PrinterConfig{Host: "192.168.1.20:8883", AccessCode: "testcode1"}, false},
		{PrinterConfig{Host: "192.168.1.20", AccessCode: "bad code"}, false},
		{PrinterConfig{Host: "192.168.1.20", AccessCode: "testcode1", Serial: "a/b"}, false},
	} {
		if got := validatePrinter(tc.c) == nil; got != tc.ok {
			t.Errorf("%+v: valid=%v want %v", tc.c, got, tc.ok)
		}
	}
}
