//go:build darwin || windows

package main

import (
	"errors"
	"os"
	"testing"
)

// Round trip through the real Keychain / Credential Manager under a test name.
// Only runs when GPP_SECRET_STORE_TEST=1 (set in CI), so it never touches a
// producer's own saved connection.
func TestSystemSecretStore(t *testing.T) {
	if os.Getenv("GPP_SECRET_STORE_TEST") != "1" {
		t.Skip("set GPP_SECRET_STORE_TEST=1 to test the system store")
	}
	s := testSystemStore()
	if s == nil {
		t.Fatal("no system store on this machine")
	}
	s.Delete()
	if _, err := s.Get(); !errors.Is(err, errNoSecret) {
		t.Fatalf("empty store: %v", err)
	}
	if err := s.Set("not a token; rm -rf ~"); err == nil {
		t.Fatal("odd token accepted")
	}
	for _, tok := range []string{"gpph_3f9c0a1b2c3d4e5f_firstsecret", "gpph_3f9c0a1b2c3d4e5f_second-secret_2"} {
		if err := s.Set(tok); err != nil {
			t.Fatalf("set: %v", err)
		}
		if got, err := s.Get(); err != nil || got != tok {
			t.Fatalf("get: %q %v, want %q", got, err, tok)
		}
	}
	if err := s.Delete(); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Get(); !errors.Is(err, errNoSecret) {
		t.Fatalf("after delete: %v", err)
	}
	if err := s.Delete(); err != nil {
		t.Fatalf("deleting twice: %v", err)
	}
	t.Logf("tested %s", s.Where())
}
