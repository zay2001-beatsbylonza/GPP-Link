package main

import (
	"runtime"
	"testing"
)

func TestValidRelRefusesWindowsTricks(t *testing.T) {
	h := &hub{}
	bad := []string{"../x.wav", "a/../../x.wav", "..\\x.wav", "/abs.wav", ".hidden.wav"}
	winOnly := []string{"C:x.wav", "C:/x.wav", "beat.wav:stream", "CON.wav", "nul", "Beats/COM1.wav", "lpt9.mid", "beat.wav.", "beat ", "a*b.wav"}
	if runtime.GOOS == "windows" {
		bad = append(bad, winOnly...)
	}
	for _, r := range bad {
		if _, err := h.validRel(r); err == nil {
			t.Errorf("%q was allowed", r)
		}
	}
	good := []string{"Beats 2026/Trap Beat 140.wav", "kick (2).wav", "Console Beat.wav", "COM10.wav", "conga.wav", "Stems/Song B/bass.wav"}
	if runtime.GOOS != "windows" { // ordinary Mac names that Windows can't have
		good = append(good, "Why Me?.wav", "Beat *NEW*.wav", "Say \"Yeah\".wav", "Left|Right.wav", "Beats/Mix v2./kick.wav")
	}
	for _, r := range good {
		if _, err := h.validRel(r); err != nil {
			t.Errorf("%q was refused: %v", r, err)
		}
	}
	for _, seg := range winOnly[2:] {
		if !windowsUnsafe(seg) && seg != "Beats/COM1.wav" && seg != "C:/x.wav" {
			t.Errorf("windowsUnsafe(%q) = false", seg)
		}
	}
}
