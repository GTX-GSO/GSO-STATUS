package main

import (
	"gso-status/contracts"
	"testing"
	"time"
)

func TestNetDetailValuesAndFreshness(t *testing.T) {
	c := config()
	c.HA.Entities = map[string]string{"vpn_updated": "sensor.subscription", "antiad_rules_updated": "sensor.rules_updated", "antiad_rule_count": "sensor.rules", "dhcp_active_leases": "sensor.leases"}
	u := NewUpstreams(c)
	now := time.Now()
	states := map[string]string{"vpn_updated": "2026-10-16T02:49:01+00:00", "antiad_rules_updated": "2026-10-15T03:28:22Z", "antiad_rule_count": "102535", "dhcp_active_leases": "4"}
	for key, state := range states {
		u.items[key] = observation{entity: entityState{State: state}, at: now}
	}
	net := object(u.Snapshot(now), "net")
	if net["subscription_updated_at"] != states["vpn_updated"] || net["antiad_rules_updated_at"] != states["antiad_rules_updated"] || number(object(net, "antiad_rule_count")["value"]) != 102535 || number(object(net, "dhcp_active_leases")["value"]) != 4 {
		t.Fatal("HA detail values lost")
	}
	if contracts.Load("snapshot").Validate(u.Snapshot(now)) != nil {
		t.Fatal("invalid snapshot")
	}
	for _, state := range []string{"unknown", "unavailable", "-1", "1.5", "NaN", "9007199254740992"} {
		u.items["dhcp_active_leases"] = observation{entity: entityState{State: state}, at: now}
		if object(object(u.Snapshot(now), "net"), "dhcp_active_leases")["value"] != nil {
			t.Fatalf("invalid count accepted: %s", state)
		}
	}
	u.items["dhcp_active_leases"] = observation{entity: entityState{State: "0"}, at: now}
	if object(object(u.Snapshot(now), "net"), "dhcp_active_leases")["value"] != float64(0) {
		t.Fatal("real zero lost")
	}
	net = object(u.Snapshot(now.Add(121*time.Second)), "net")
	if net["subscription_updated_at"] != nil || net["antiad_rules_updated_at"] != nil || object(net, "antiad_rule_count")["value"] != nil || object(net, "dhcp_active_leases")["value"] != nil {
		t.Fatal("expired detail remained current")
	}
	u.items["vpn_updated"] = observation{entity: entityState{State: "not a date"}, at: now}
	if object(u.Snapshot(now), "net")["subscription_updated_at"] != nil {
		t.Fatal("invalid timestamp accepted")
	}
}
