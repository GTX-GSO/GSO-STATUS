package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"net/url"
	"os"
	"strings"
)

type Config struct {
	Listen           string          `json:"listen"`
	AgentID          string          `json:"agent_id"`
	WriteToken       string          `json:"write_token"`
	ReadToken        string          `json:"read_token"`
	HomeControlToken string          `json:"home_control_token,omitempty"`
	RouterURL        string          `json:"router_url"`
	HA               HAConfig        `json:"ha"`
	Mihomo           MihomoConfig    `json:"mihomo"`
	Docker           DockerConfig    `json:"docker,omitempty"`
	Storage          []StorageVolume `json:"storage,omitempty"`
	NAS              *NasConfig      `json:"nas,omitempty"`
	Printer          PrinterConfig   `json:"printer,omitempty"`
}

func loadConfig(path string) (Config, error) {
	var c Config
	f, e := os.Open(path)
	if e != nil {
		return c, e
	}
	defer f.Close()
	raw, readErr := io.ReadAll(io.LimitReader(f, 8193))
	if readErr != nil {
		return c, readErr
	}
	if len(raw) > 8192 {
		return c, errors.New("configuration exceeds 8192 bytes")
	}
	raw = bytes.TrimPrefix(raw, []byte{0xef, 0xbb, 0xbf})
	d := json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	if e = d.Decode(&c); e != nil {
		return c, e
	}
	var extra any
	if d.Decode(&extra) != io.EOF {
		return c, errors.New("configuration must contain one JSON object")
	}
	if c.Listen == "" {
		c.Listen = "127.0.0.1:17890"
	}
	if !agentPattern.MatchString(c.AgentID) {
		return c, errors.New("invalid agent_id")
	}
	if len(c.WriteToken) < 32 || len(c.ReadToken) < 32 || c.WriteToken == c.ReadToken {
		return c, errors.New("two distinct tokens of at least 32 characters are required")
	}
	if len(c.WriteToken) > 256 || len(c.ReadToken) > 256 || strings.ContainsAny(c.WriteToken+c.ReadToken, "\r\n") || strings.HasPrefix(c.WriteToken, "REPLACE_") || strings.HasPrefix(c.ReadToken, "REPLACE_") {
		return c, errors.New("invalid tokens")
	}
	if token := c.HomeControlToken; token != "" && (len(token) < 32 || len(token) > 256 || strings.ContainsAny(token, "\r\n") || strings.HasPrefix(token, "REPLACE_") || token == c.ReadToken || token == c.WriteToken || token == c.HA.Token) {
		return c, errors.New("HOME requires a distinct control token of 32-256 characters")
	}
	if c.RouterURL != "" {
		u, e := url.Parse(c.RouterURL)
		if e != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") || u.User != nil {
			return c, errors.New("invalid router_url")
		}
	}
	if err := validateUpstreams(c); err != nil {
		return c, err
	}
	if err := validateDocker(c.Docker); err != nil {
		return c, err
	}
	if err := validateStorage(c.Storage); err != nil {
		return c, err
	}
	if err := validateNas(c.NAS); err != nil {
		return c, err
	}
	if err := validatePrinter(c.Printer); err != nil {
		return c, err
	}
	return c, nil
}
