// Package contracts embeds the same contracts used by the development validator.
package contracts

import (
	"bytes"
	"embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"regexp"
	"time"
	"unicode/utf8"
)

//go:embed v1/*.schema.json
var files embed.FS

type Object = map[string]any
type Schema struct {
	Root Object
	Defs Object
}

func Load(name string) *Schema {
	b, err := files.ReadFile("v1/" + name + ".schema.json")
	if err != nil {
		panic(err)
	}
	var root Object
	if err = json.Unmarshal(b, &root); err != nil {
		panic(err)
	}
	return &Schema{root, root["$defs"].(Object)}
}

// Decode rejects duplicate keys before decoding objects, as well as trailing JSON.
func Decode(b []byte) (Object, error) {
	if !utf8.Valid(b) {
		return nil, errors.New("invalid UTF-8")
	}
	d := json.NewDecoder(bytes.NewReader(b))
	d.UseNumber()
	var walk func(int) (any, error)
	walk = func(depth int) (any, error) {
		if depth > 32 {
			return nil, errors.New("JSON nesting exceeds 32")
		}
		t, e := d.Token()
		if e != nil {
			return nil, e
		}
		delim, ok := t.(json.Delim)
		if !ok {
			return t, nil
		}
		switch delim {
		case '{':
			o := Object{}
			for d.More() {
				k, e := d.Token()
				if e != nil {
					return nil, e
				}
				key, ok := k.(string)
				if !ok {
					return nil, errors.New("invalid key")
				}
				if _, ok = o[key]; ok {
					return nil, errors.New("duplicate key")
				}
				v, e := walk(depth + 1)
				if e != nil {
					return nil, e
				}
				o[key] = v
			}
			_, e = d.Token()
			return o, e
		case '[':
			a := []any{}
			for d.More() {
				v, e := walk(depth + 1)
				if e != nil {
					return nil, e
				}
				a = append(a, v)
			}
			_, e = d.Token()
			return a, e
		}
		return nil, errors.New("invalid delimiter")
	}
	value, e := walk(0)
	if e != nil {
		return nil, e
	}
	if _, e = d.Token(); e != io.EOF {
		return nil, errors.New("trailing JSON")
	}
	o, ok := value.(Object)
	if !ok {
		return nil, errors.New("root must be object")
	}
	return o, nil
}
func (s *Schema) Validate(v any) error { return s.check(s.Root, v, "$") }
func (s *Schema) resolve(rule Object) Object {
	if r, ok := rule["$ref"].(string); ok {
		return s.Defs[r[len("#/$defs/"):]].(Object)
	}
	return rule
}
func num(v any) (float64, bool) {
	switch n := v.(type) {
	case json.Number:
		f, e := n.Float64()
		return f, e == nil && !math.IsInf(f, 0)
	case float64:
		return n, !math.IsNaN(n) && !math.IsInf(n, 0)
	case int:
		return float64(n), true
	case int64:
		return float64(n), true
	case uint64:
		return float64(n), true
	}
	return 0, false
}
func eq(a, b any) bool { x, _ := json.Marshal(a); y, _ := json.Marshal(b); return bytes.Equal(x, y) }
func (s *Schema) check(rule Object, v any, path string) error {
	rule = s.resolve(rule)
	fail := func() error { return fmt.Errorf("invalid field %s", path) }
	if arr, ok := rule["allOf"].([]any); ok {
		for _, r := range arr {
			if e := s.check(r.(Object), v, path); e != nil {
				return e
			}
		}
	}
	if arr, ok := rule["oneOf"].([]any); ok {
		n := 0
		for _, r := range arr {
			if s.check(r.(Object), v, path) == nil {
				n++
			}
		}
		if n != 1 {
			return fail()
		}
	}
	if c, ok := rule["const"]; ok && !eq(c, v) {
		return fail()
	}
	if choices, ok := rule["enum"].([]any); ok {
		found := false
		for _, c := range choices {
			found = found || eq(c, v)
		}
		if !found {
			return fail()
		}
	}
	if typ, ok := rule["type"]; ok {
		accepts := func(t string) bool {
			switch t {
			case "null":
				return v == nil
			case "object":
				_, ok := v.(Object)
				return ok
			case "array":
				_, ok := v.([]any)
				return ok
			case "string":
				_, ok := v.(string)
				return ok
			case "boolean":
				_, ok := v.(bool)
				return ok
			case "number":
				_, ok := num(v)
				return ok
			case "integer":
				n, ok := num(v)
				return ok && math.Trunc(n) == n
			}
			return false
		}
		valid := false
		switch t := typ.(type) {
		case string:
			valid = accepts(t)
		case []any:
			for _, item := range t {
				valid = valid || accepts(item.(string))
			}
		}
		if !valid {
			return fail()
		}
	}
	if n, ok := num(v); ok {
		if min, ok := rule["exclusiveMinimum"].(float64); ok && n <= min {
			return fail()
		}
		if max, ok := rule["exclusiveMaximum"].(float64); ok && n >= max {
			return fail()
		}
		if min, ok := rule["minimum"].(float64); ok && n < min {
			return fail()
		}
		if max, ok := rule["maximum"].(float64); ok && n > max {
			return fail()
		}
	}
	if str, ok := v.(string); ok {
		length := len([]rune(str))
		if min, ok := rule["minLength"].(float64); ok && length < int(min) {
			return fail()
		}
		if max, ok := rule["maxLength"].(float64); ok && length > int(max) {
			return fail()
		}
		if pattern, ok := rule["pattern"].(string); ok {
			matched, err := regexp.MatchString(pattern, str)
			if err != nil || !matched {
				return fail()
			}
		}
		if format, ok := rule["format"].(string); ok {
			switch format {
			case "date-time":
				if _, e := time.Parse(time.RFC3339Nano, str); e != nil {
					return fail()
				}
			case "uuid":
				if !uuidPattern.MatchString(str) {
					return fail()
				}
			}
		}
	}
	if a, ok := v.([]any); ok {
		if max, ok := rule["maxItems"].(float64); ok && len(a) > int(max) {
			return fail()
		}
		if item, ok := rule["items"].(Object); ok {
			for i, value := range a {
				if e := s.check(item, value, fmt.Sprintf("%s[%d]", path, i)); e != nil {
					return e
				}
			}
		}
	}
	if o, ok := v.(Object); ok {
		if required, ok := rule["required"].([]any); ok {
			for _, key := range required {
				if _, exists := o[key.(string)]; !exists {
					return fail()
				}
			}
		}
		props, _ := rule["properties"].(Object)
		for key, value := range o {
			if r, exists := props[key]; exists {
				if e := s.check(r.(Object), value, path+"."+key); e != nil {
					return e
				}
			} else if rule["additionalProperties"] == false {
				return fail()
			}
		}
	}
	return nil
}

var uuidPattern = regexp.MustCompile(`^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$`)

// Empty creates an unavailable model from structure, never from a test fixture.
func (s *Schema) Empty() Object {
	var init func(Object) any
	init = func(rule Object) any {
		if r, ok := rule["$ref"].(string); ok {
			if r == "#/$defs/source" {
				return Object{"quality": "unconfigured", "observed_at": nil, "source_updated_at": nil, "age_ms": nil, "last_error_code": "not_configured"}
			}
			if r == "#/$defs/metric" || r == "#/$defs/bytes" || r == "#/$defs/percent" || r == "#/$defs/temperature" || r == "#/$defs/frequency" || r == "#/$defs/illuminance" || r == "#/$defs/printer_temperature" || r == "#/$defs/tyre_pressure" || r == "#/$defs/charging_power" {
				return Object{"value": nil, "reason": "not_configured"}
			}
			return init(s.resolve(rule))
		}
		if c, ok := rule["const"]; ok {
			return c
		}
		if types, ok := rule["type"].([]any); ok {
			for _, t := range types {
				if t == "null" {
					return nil
				}
			}
		}
		switch rule["type"] {
		case "object":
			o := Object{}
			for k, r := range rule["properties"].(Object) {
				o[k] = init(r.(Object))
			}
			return o
		case "array":
			return []any{}
		}
		if values, ok := rule["enum"].([]any); ok {
			for _, v := range values {
				if v == "unknown" {
					return v
				}
			}
			return values[0]
		}
		if rule["format"] == "date-time" {
			return time.Now().UTC().Format(time.RFC3339Nano)
		}
		return nil
	}
	return init(s.Root).(Object)
}
