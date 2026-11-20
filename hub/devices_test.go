package main

import (
	"encoding/json"
	"net/http/httptest"
	"testing"
	"time"
)

func TestDevicesOnlineAndForgotten(t *testing.T) {
	h := &hub{devices: map[string]time.Time{}, queue: map[string][]string{}, cue: map[string]string{}}
	now := time.Now()
	h.devices["Live III"] = now                      // plugin open
	h.devices["MPC One"] = now.Add(-2 * time.Minute) // plugin closed a bit ago
	h.devices["Old XL"] = now.Add(-2 * time.Hour)    // long gone
	h.queue["Old XL"] = []string{"beat.wav"}
	h.cue["Old XL"] = "beat.wav"

	rec := httptest.NewRecorder()
	h.listDevices(rec, httptest.NewRequest("GET", "/api/devices", nil))
	var got []struct {
		Name   string `json:"name"`
		Online bool   `json:"online"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	want := map[string]bool{"Live III": true, "MPC One": false}
	if len(got) != len(want) {
		t.Fatalf("got %+v, want %v", got, want)
	}
	for _, d := range got {
		if on, ok := want[d.Name]; !ok || on != d.Online {
			t.Fatalf("got %+v, want %v", got, want)
		}
	}
	if _, ok := h.devices["Old XL"]; ok || h.queue["Old XL"] != nil || h.cue["Old XL"] != "" {
		t.Fatal("long-gone MPC was not forgotten")
	}
}
