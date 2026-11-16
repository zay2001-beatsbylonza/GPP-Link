package main

import "testing"

func TestValidRelRefusesWindowsTricks(t *testing.T) {
	h := &hub{}
	bad := []string{"../x.wav", "a/../../x.wav", "..\\x.wav", "C:x.wav", "C:/x.wav", "beat.wav:stream", "CON.wav",
		"nul", "Beats/COM1.wav", "lpt9.mid", "beat.wav.", "beat ", "a*b.wav", "/abs.wav", ".hidden.wav"}
	for _, r := range bad {
		if _, err := h.validRel(r); err == nil {
			t.Errorf("%q was allowed", r)
		}
	}
	good := []string{"Beats 2026/Trap Beat 140.wav", "kick (2).wav", "Console Beat.wav", "COM10.wav", "conga.wav", "Stems/Song B/bass.wav"}
	for _, r := range good {
		if _, err := h.validRel(r); err != nil {
			t.Errorf("%q was refused: %v", r, err)
		}
	}
}
