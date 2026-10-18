package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"errors"
	"io"
	"net"
	"strconv"
	"strings"
	"time"
)

// A minimal implicit-FTPS client (TLS from the first byte, port 990) for reading a few files from the printer. The printer
// requires the data connection to resume the control connection's TLS session, so one tls.Config with a session cache
// serves both. Every read opens its own connection: nothing is kept open between reads, and a transfer that is cut short
// (only part of a file is wanted) needs no ABOR handling.
type ftpsClient struct {
	host string
	ctrl net.Conn
	r    *bufio.Reader
	tls  *tls.Config
}

const ftpsStep = 10 * time.Second

func dialFTPS(ctx context.Context, address, user, password string) (*ftpsClient, error) {
	host, _, err := net.SplitHostPort(address)
	if err != nil {
		return nil, err
	}
	cfg := &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS12, MaxVersion: tls.VersionTLS12, ClientSessionCache: tls.NewLRUClientSessionCache(4), ServerName: host}
	d := tls.Dialer{NetDialer: &net.Dialer{Timeout: 5 * time.Second}, Config: cfg}
	conn, err := d.DialContext(ctx, "tcp", address)
	if err != nil {
		return nil, err
	}
	c := &ftpsClient{host: host, ctrl: conn, r: bufio.NewReader(conn), tls: cfg}
	if code, _, err := c.reply(); err != nil || code != 220 {
		conn.Close()
		return nil, errors.New("ftps_no_greeting")
	}
	for _, step := range []struct {
		command string
		want    int
	}{{"USER " + user, 331}, {"PASS " + password, 230}, {"PBSZ 0", 200}, {"PROT P", 200}, {"TYPE I", 200}} {
		code, _, err := c.command(step.command)
		if err != nil || code != step.want {
			conn.Close()
			if step.want == 230 {
				return nil, errors.New("ftps_auth_failed")
			}
			return nil, errors.New("ftps_setup_failed")
		}
	}
	return c, nil
}

func (c *ftpsClient) Close() { c.ctrl.Close() }

// reply reads one (possibly multi-line) response.
func (c *ftpsClient) reply() (int, string, error) {
	c.ctrl.SetReadDeadline(time.Now().Add(ftpsStep))
	var text strings.Builder
	for {
		line, err := c.r.ReadString('\n')
		if err != nil {
			return 0, "", err
		}
		line = strings.TrimRight(line, "\r\n")
		text.WriteString(line + "\n")
		if len(line) >= 4 && line[3] == ' ' {
			code, err := strconv.Atoi(line[:3])
			return code, text.String(), err
		}
		if text.Len() > 8192 {
			return 0, "", errors.New("ftps_reply_too_long")
		}
	}
}

func (c *ftpsClient) command(line string) (int, string, error) {
	c.ctrl.SetWriteDeadline(time.Now().Add(ftpsStep))
	if _, err := io.WriteString(c.ctrl, line+"\r\n"); err != nil {
		return 0, "", err
	}
	return c.reply()
}

// passive opens the data connection announced by PASV (to the control connection's host, whatever address is announced).
func (c *ftpsClient) passive(ctx context.Context) (net.Conn, error) {
	code, text, err := c.command("PASV")
	if err != nil || code != 227 {
		return nil, errors.New("ftps_pasv_failed")
	}
	open, closing := strings.Index(text, "("), strings.Index(text, ")")
	if open < 0 || closing < open {
		return nil, errors.New("ftps_pasv_failed")
	}
	parts := strings.Split(text[open+1:closing], ",")
	if len(parts) != 6 {
		return nil, errors.New("ftps_pasv_failed")
	}
	high, e1 := strconv.Atoi(strings.TrimSpace(parts[4]))
	low, e2 := strconv.Atoi(strings.TrimSpace(parts[5]))
	if e1 != nil || e2 != nil || high < 0 || high > 255 || low < 0 || low > 255 {
		return nil, errors.New("ftps_pasv_failed")
	}
	d := tls.Dialer{NetDialer: &net.Dialer{Timeout: 5 * time.Second}, Config: c.tls}
	return d.DialContext(ctx, "tcp", net.JoinHostPort(c.host, strconv.Itoa(high*256+low)))
}

// readPrefix returns the first limit bytes of a file (fewer if it is shorter). The printer's FTP server has no REST, so a
// range can only start at the beginning; the data connection is dropped once enough has arrived.
func (c *ftpsClient) readPrefix(ctx context.Context, path string, limit int64) ([]byte, error) {
	data, err := c.passive(ctx)
	if err != nil {
		return nil, err
	}
	defer data.Close()
	if code, _, err := c.command("RETR " + path); err != nil || (code != 150 && code != 125) {
		return nil, errors.New("ftps_retr_failed")
	}
	data.SetReadDeadline(time.Now().Add(30 * time.Second))
	body, err := io.ReadAll(io.LimitReader(data, limit))
	if err != nil && len(body) == 0 {
		return nil, err
	}
	return body, nil
}
