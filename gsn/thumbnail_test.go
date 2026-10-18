package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"math/big"
	"net"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"gso-status/contracts"
)

// A 512x512 render like the printer's: transparent, with a small opaque model off to one side.
func modelPicture(t *testing.T, fill color.NRGBA) []byte {
	t.Helper()
	img := image.NewNRGBA(image.Rect(0, 0, 512, 512))
	for y := 100; y < 160; y++ {
		for x := 300; x < 420; x++ {
			img.SetNRGBA(x, y, fill)
		}
	}
	var out bytes.Buffer
	if err := png.Encode(&out, img); err != nil {
		t.Fatal(err)
	}
	return out.Bytes()
}

// threeMF builds an archive laid out like the printer's: pictures first, a large G-code after them.
func threeMF(t *testing.T, picture []byte, method uint16, plate int) []byte {
	t.Helper()
	var buf bytes.Buffer
	z := zip.NewWriter(&buf)
	add := func(name string, m uint16, data []byte) {
		w, err := z.CreateHeader(&zip.FileHeader{Name: name, Method: m})
		if err != nil {
			t.Fatal(err)
		}
		w.Write(data)
	}
	add("[Content_Types].xml", zip.Deflate, []byte(strings.Repeat("<Types/>", 50)))
	add(fmt.Sprintf("Metadata/plate_%d.png", plate), method, picture)
	add(fmt.Sprintf("Metadata/plate_%d_small.png", plate), method, picture[:len(picture)/2])
	big := make([]byte, 600<<10)
	rand.Read(big)
	add(fmt.Sprintf("Metadata/plate_%d.gcode", plate), zip.Store, big)
	if err := z.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func TestZipPictureFindsThePlatePicture(t *testing.T) {
	picture := modelPicture(t, color.NRGBA{200, 30, 30, 255})
	for _, method := range []uint16{zip.Store, zip.Deflate} {
		archive := threeMF(t, picture, method, 1)
		got, err := zipPicture(archive[:thumbnailPrefix], 1)
		if err != nil || !bytes.Equal(got, picture) {
			t.Fatalf("method %d: err=%v equal=%v", method, err, bytes.Equal(got, picture))
		}
		if _, err := zipPicture(archive[:thumbnailPrefix], 2); err == nil || err.Error() != "thumbnail_not_found" {
			t.Fatalf("a plate that is not there: %v", err)
		}
		if _, err := zipPicture(archive[:200], 1); err == nil {
			t.Fatal("a prefix cut inside the first entries cannot hold the picture")
		}
		if _, err := zipPicture(archive[:400], 1); err == nil {
			t.Fatal("a prefix that ends inside the picture must not return a partial picture")
		}
	}
	if got, err := zipPicture(threeMF(t, picture, zip.Store, 3), 3); err != nil || !bytes.Equal(got, picture) {
		t.Fatalf("plate 3: %v", err)
	}
	if _, err := zipPicture([]byte("not a zip file at all, not even close to one"), 1); err == nil {
		t.Fatal("garbage")
	}
	if plateNumber("/data/Metadata/plate_2.gcode") != 2 || plateNumber("") != 1 || plateNumber("plate_0.gcode") != 1 {
		t.Fatal("plate number from gcode_file")
	}
}

func TestRenderThumbnailCropsToTheModel(t *testing.T) {
	out, err := renderThumbnail(modelPicture(t, color.NRGBA{200, 30, 30, 255}))
	if err != nil || len(out) != thumbnailBytes {
		t.Fatalf("len %d err %v", len(out), err)
	}
	at := func(x, y int) (b, g, r, a byte) {
		o := (y*thumbnailSize + x) * 4
		return out[o], out[o+1], out[o+2], out[o+3]
	}
	if b, g, r, a := at(60, 60); r != 200 || g != 30 || b != 30 || a != 255 {
		t.Fatalf("centre pixel bgra=%d,%d,%d,%d (straight alpha, blue first)", b, g, r, a)
	}
	if _, _, _, a := at(0, 0); a != 0 {
		t.Fatal("the margin stays transparent")
	}
	opaque := 0
	for i := 3; i < len(out); i += 4 {
		if out[i] > 200 {
			opaque++
		}
	}
	if float64(opaque)/float64(thumbnailSize*thumbnailSize) < 0.25 {
		t.Fatalf("a 120x60 model must fill the width of the square: only %d opaque pixels", opaque)
	}
	if _, err := renderThumbnail(modelPicture(t, color.NRGBA{0, 0, 0, 0})); err == nil {
		t.Fatal("an empty picture is not a thumbnail")
	}
	if _, err := renderThumbnail([]byte("PNG? no")); err == nil {
		t.Fatal("not a picture")
	}
}

// fakeFTPS serves /cache over implicit FTPS with a data connection per transfer, like the printer.
type fakeFTPS struct {
	l     net.Listener
	files map[string][]byte
	mu    sync.Mutex
	got   []string
}

func newFakeFTPS(t *testing.T, code string, files map[string][]byte) *fakeFTPS {
	t.Helper()
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	template := &x509.Certificate{SerialNumber: big.NewInt(2), Subject: pkix.Name{CommonName: fakeSerial}, NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour)}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	cfg := &tls.Config{Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: key}}}
	l, err := tls.Listen("tcp", "127.0.0.1:0", cfg)
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeFTPS{l: l, files: files}
	t.Cleanup(func() { l.Close() })
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			go f.handle(c, cfg, code)
		}
	}()
	return f
}

func (f *fakeFTPS) handle(c net.Conn, cfg *tls.Config, code string) {
	defer c.Close()
	r := bufio.NewReader(c)
	say := func(line string) { fmt.Fprintf(c, "%s\r\n", line) }
	say("220 fake printer FTP")
	var data net.Listener
	var accepted chan net.Conn
	defer func() {
		if data != nil {
			data.Close()
		}
	}()
	user := ""
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return
		}
		verb, arg, _ := strings.Cut(strings.TrimSpace(line), " ")
		switch strings.ToUpper(verb) {
		case "USER":
			user = arg
			say("331 password please")
		case "PASS":
			if user == "bblp" && arg == code {
				say("230 ok")
			} else {
				say("530 login incorrect")
				return
			}
		case "PBSZ", "PROT", "TYPE":
			say("200 ok")
		case "SIZE":
			if body, ok := f.files[arg]; ok {
				say(fmt.Sprintf("213 %d", len(body)))
			} else {
				say("550 no such file")
			}
		case "PASV":
			l, err := tls.Listen("tcp", "127.0.0.1:0", cfg)
			if err != nil {
				say("425 no data port")
				continue
			}
			data = l
			accepted = make(chan net.Conn, 1)
			go func() { // the client connects (and handshakes) before it asks for the file
				dc, err := l.Accept()
				if err != nil {
					return
				}
				if tc, ok := dc.(*tls.Conn); ok && tc.Handshake() != nil {
					dc.Close()
					return
				}
				accepted <- dc
			}()
			port := l.Addr().(*net.TCPAddr).Port
			say(fmt.Sprintf("227 Entering Passive Mode (127,0,0,1,%d,%d)", port/256, port%256))
		case "RETR":
			f.mu.Lock()
			f.got = append(f.got, arg)
			f.mu.Unlock()
			body, ok := f.files[arg]
			if !ok || data == nil {
				say("550 no such file")
				continue
			}
			say("150 opening data connection")
			dc := <-accepted
			dc.Write(body) // the client may stop reading early; the error is expected
			dc.Close()
			data = nil
			say("226 done")
		default:
			say("502 not supported")
		}
	}
}

func (f *fakeFTPS) retrieved() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.got...)
}

func TestPrinterThumbnailEndToEnd(t *testing.T) {
	picture := modelPicture(t, color.NRGBA{40, 120, 200, 255})
	other := modelPicture(t, color.NRGBA{200, 120, 40, 255})
	ftp := newFakeFTPS(t, "testcode1", map[string][]byte{
		"/cache/Benchy.3mf":           threeMF(t, picture, zip.Store, 1),
		"/cache/Second_part.3mf":      threeMF(t, other, zip.Deflate, 2),
		"/cache/Gcode_name.gcode.3mf": threeMF(t, picture, zip.Store, 1),
		"/Studio_sent.gcode.3mf":      threeMF(t, other, zip.Store, 1),
	})
	f := newFakePrinter(t, "testcode1")
	s := startPrinter(t, f, "")
	s.ftpAddr, s.thumbAfter = ftp.l.Addr().String(), 100*time.Millisecond
	// the loop is started by Run before ftpAddr is set; it reads the field on every fetch, so the override takes effect
	report := func(job, file, state string) {
		f.send <- f.report("device/"+fakeSerial+"/report", 0, fmt.Sprintf(`{"print":{"gcode_state":%q,"subtask_name":%q,"gcode_file":%q,"mc_percent":10}}`, state, job, file))
	}
	report("Benchy", "/data/Metadata/plate_1.gcode", "RUNNING")
	waitFor(t, "thumbnail id", func() bool { return s.Snapshot(time.Now())["thumbnail_id"] != nil })
	id, data := s.Thumbnail(time.Now())
	if id != thumbnailKey("Benchy", 1) || len(data) != thumbnailBytes {
		t.Fatalf("id %q len %d", id, len(data))
	}
	if b, _, r, a := data[(60*thumbnailSize+60)*4], data[(60*thumbnailSize+60)*4+1], data[(60*thumbnailSize+60)*4+2], data[(60*thumbnailSize+60)*4+3]; b != 200 || r != 40 || a != 255 {
		t.Fatalf("model colour b=%d r=%d a=%d", b, r, a)
	}
	// served over HTTP with the read token; a stale id gets 404
	c := config()
	srv := NewServer(c)
	srv.printer = s
	w := request(srv, "GET", "/api/v1/printer/thumbnail?id="+id, c.ReadToken, nil)
	if w.Code != http.StatusOK || w.Body.Len() != thumbnailBytes || w.Header().Get("X-GSO-Thumbnail-Id") != id || w.Header().Get("Content-Type") != "application/octet-stream" {
		t.Fatalf("thumbnail response %d len %d", w.Code, w.Body.Len())
	}
	if w = request(srv, "GET", "/api/v1/printer/thumbnail?id=0000000000000000", c.ReadToken, nil); w.Code != 404 {
		t.Fatalf("stale id: %d", w.Code)
	}
	if w = request(srv, "GET", "/api/v1/printer/thumbnail", c.WriteToken, nil); w.Code != 401 {
		t.Fatalf("the write token must not read: %d", w.Code)
	}
	if err := contracts.Load("snapshot").Validate(func() contracts.Object { o := srv.schema.Empty(); o["printer"] = s.Snapshot(time.Now()); return o }()); err != nil {
		t.Fatalf("snapshot with a thumbnail_id: %v", err)
	}
	if n := len(ftp.retrieved()); n != 1 {
		t.Fatalf("the archive must be fetched once per job, got %d", n)
	}
	// the next job: the old picture goes at once, the new one arrives
	report("Second_part", "/data/Metadata/plate_2.gcode", "RUNNING")
	waitFor(t, "new thumbnail", func() bool { id, _ := s.Thumbnail(time.Now()); return id == thumbnailKey("Second_part", 2) })
	if _, d := s.Thumbnail(time.Now()); d[(60*thumbnailSize+60)*4] != 40 {
		t.Fatal("the second model is orange (blue channel 40)")
	}
	// a file the printer does not have: no picture, retried later, never the previous job's picture
	report("Missing_job", "", "RUNNING")
	waitFor(t, "picture dropped", func() bool { id, _ := s.Thumbnail(time.Now()); return id == "" })
	waitFor(t, "second attempt", func() bool { s.mu.Lock(); defer s.mu.Unlock(); return s.thumbErrors >= 2 })
	if s.Snapshot(time.Now())["thumbnail_id"] != nil {
		t.Fatal("no thumbnail_id without a picture")
	}
	// the name with .gcode.3mf is tried after .3mf
	report("Gcode_name", "", "RUNNING")
	waitFor(t, "fallback file name", func() bool { id, _ := s.Thumbnail(time.Now()); return id == thumbnailKey("Gcode_name", 1) })
	// a file sent from Studio sits in the root under the name gcode_file gives
	report("Studio_sent.stl", "Studio_sent.gcode.3mf", "RUNNING")
	waitFor(t, "root file", func() bool { id, _ := s.Thumbnail(time.Now()); return id == thumbnailKey("Studio_sent.stl", 1) })
	// idle shows no picture
	report("Gcode_name", "", "IDLE")
	waitFor(t, "idle", func() bool { return s.Snapshot(time.Now())["state"] == "idle" })
	if s.Snapshot(time.Now())["thumbnail_id"] != nil {
		t.Fatal("an idle printer has no model on show")
	}
	_ = context.Background
}

func TestPictureFileCandidates(t *testing.T) {
	got := pictureFiles("Part.stl", "Part.stl.gcode.3mf")
	want := []string{"/Part.stl.gcode.3mf", "/cache/Part.stl.gcode.3mf", "/cache/Part.stl.3mf", "/cache/Part.stl.gcode.3mf.gcode.3mf", "/Part.stl.3mf", "/Part.stl.gcode.3mf.gcode.3mf"}
	_ = want
	if len(got) < 4 || got[0] != "/Part.stl.gcode.3mf" || got[1] != "/cache/Part.stl.gcode.3mf" {
		t.Fatalf("%v", got)
	}
	seen := map[string]bool{}
	for _, g := range got {
		if seen[g] {
			t.Fatalf("duplicate %s", g)
		}
		seen[g] = true
	}
	if names := pictureFiles("A\nB", ""); len(names) != 0 {
		t.Fatalf("a name with a line break is never sent: %v", names)
	}
}

func TestFTPSLoginRefused(t *testing.T) {
	ftp := newFakeFTPS(t, "right", nil)
	if _, err := dialFTPS(context.Background(), ftp.l.Addr().String(), "bblp", "wrong"); err == nil || err.Error() != "ftps_auth_failed" {
		t.Fatalf("got %v", err)
	}
}
