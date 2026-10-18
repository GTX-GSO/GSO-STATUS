package contracts

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestFixtures(t *testing.T) {
	paths, e := filepath.Glob("v1/*.json")
	if e != nil {
		t.Fatal(e)
	}
	for _, p := range paths {
		if strings.Contains(p, ".schema.") {
			continue
		}
		b, e := os.ReadFile(p)
		if e != nil {
			t.Fatal(e)
		}
		o, e := Decode(b)
		if e != nil {
			t.Fatal(e)
		}
		name := "snapshot"
		if strings.HasPrefix(filepath.Base(p), "home-command-") {
			name = "home-command"
		} else if strings.HasPrefix(filepath.Base(p), "home-result-") {
			name = "home-result"
		} else if strings.HasPrefix(filepath.Base(p), "telemetry") {
			name = "telemetry"
		}
		if e = Load(name).Validate(o); e != nil {
			t.Fatalf("%s: %v", p, e)
		}
	}
}
func TestEmpty(t *testing.T) {
	s := Load("snapshot")
	if e := s.Validate(s.Empty()); e != nil {
		t.Fatal(e)
	}
}

func TestHomeTemperatureStepLimits(t *testing.T) {
	b, e := os.ReadFile("v1/snapshot-home.json")
	if e != nil {
		t.Fatal(e)
	}
	for _, step := range []float64{0, -1, 101} {
		model, e := Decode(b)
		if e != nil {
			t.Fatal(e)
		}
		entity := model["home"].(Object)["entities"].([]any)[1].(Object)
		entity["temperature_step_c"].(Object)["value"] = step
		if Load("snapshot").Validate(model) == nil {
			t.Fatalf("invalid temperature step accepted: %v", step)
		}
	}
}
