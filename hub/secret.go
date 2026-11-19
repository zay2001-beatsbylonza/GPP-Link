package main

// Where the Get Producers Paid token is kept: the macOS Keychain, the Windows
// Credential Manager, or (anywhere else, and in tests) a private file.

import (
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

const (
	secretService = "com.beatsbylonza.gpp-link" // Keychain service / Credential Manager target
	secretAccount = "get-producers-paid"
)

// tokens look like gpph_<id>_<secret>; refuse anything else before it reaches
// a command line or the system store
var tokenRe = regexp.MustCompile(`^gpph_[A-Za-z0-9_-]{8,200}$`)

var errNoSecret = errors.New("no token stored")

type secretStore interface {
	Get() (string, error) // errNoSecret when nothing is stored
	Set(token string) error
	Delete() error
	Where() string // for the hub window: "macOS Keychain", ...
}

// newSecretStore picks the system store. GPP_LINK_TOKEN_FILE forces a file
// (tests, and machines without a keychain).
func newSecretStore(stateDir string) secretStore {
	if f := os.Getenv("GPP_LINK_TOKEN_FILE"); f != "" {
		return fileSecret{path: f}
	}
	if s := systemSecretStore(); s != nil {
		return s
	}
	return fileSecret{path: filepath.Join(stateDir, "gpp-token")}
}

type fileSecret struct{ path string }

func (f fileSecret) Get() (string, error) {
	b, err := os.ReadFile(f.path)
	if errors.Is(err, os.ErrNotExist) {
		return "", errNoSecret
	}
	if err != nil {
		return "", err
	}
	t := strings.TrimSpace(string(b))
	if t == "" {
		return "", errNoSecret
	}
	return t, nil
}

func (f fileSecret) Set(token string) error {
	if !tokenRe.MatchString(token) {
		return errors.New("the server sent a token in an unexpected format")
	}
	if err := os.MkdirAll(filepath.Dir(f.path), 0o700); err != nil {
		return err
	}
	tmp := f.path + ".tmp"
	if err := os.WriteFile(tmp, []byte(token+"\n"), 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, f.path)
}

func (f fileSecret) Delete() error {
	err := os.Remove(f.path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	return err
}

func (f fileSecret) Where() string { return "a private file (" + f.path + ")" }
