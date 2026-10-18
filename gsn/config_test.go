package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestConfigCredentials(t *testing.T) {
	for _, test := range []struct {
		name   string
		change func(*Config)
		valid  bool
	}{
		{"valid", func(*Config) {}, true},
		{"same token", func(c *Config) { c.ReadToken = c.WriteToken }, false},
		{"placeholder", func(c *Config) { c.ReadToken = "REPLACE_WITH_32_OR_MORE_RANDOM_CHARACTERS_READ" }, false},
		{"short", func(c *Config) { c.WriteToken = "short" }, false},
		{"home token reuses read", func(c *Config) { c.HomeControlToken = c.ReadToken }, false},
		{"home token short", func(c *Config) { c.HomeControlToken = "short" }, false},
		{"foreign home domain", func(c *Config) {
			c.HA = HAConfig{BaseURL: "http://127.0.0.1:1", Token: "secret", Home: []HomeBinding{{EntityID: "lock.front_door"}}}
		}, false},
		{"duplicate home entity", func(c *Config) {
			c.HA = HAConfig{BaseURL: "http://127.0.0.1:1", Token: "secret", Home: []HomeBinding{{EntityID: "light.test"}, {EntityID: "light.test"}}}
		}, false},
		{"header injection", func(c *Config) { c.ReadToken = strings.Repeat("a", 32) + "\r\n" }, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			c := config()
			test.change(&c)
			b, _ := json.Marshal(c)
			path := filepath.Join(t.TempDir(), "gsn.json")
			if e := os.WriteFile(path, b, 0600); e != nil {
				t.Fatal(e)
			}
			_, e := loadConfig(path)
			if (e == nil) != test.valid {
				t.Fatalf("config valid=%v err=%v", test.valid, e)
			}
		})
	}
}
func TestConfigBOM(t *testing.T) {
	c := config()
	body, _ := json.Marshal(c)
	path := filepath.Join(t.TempDir(), "gsn.json")
	if e := os.WriteFile(path, append([]byte{0xef, 0xbb, 0xbf}, body...), 0600); e != nil {
		t.Fatal(e)
	}
	if _, e := loadConfig(path); e != nil {
		t.Fatal(e)
	}
}
