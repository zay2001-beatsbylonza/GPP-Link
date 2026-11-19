//go:build darwin

package main

// macOS Keychain through Apple's own `security` tool. The token is passed on
// security's standard input (interactive mode), never on a command line where
// other programs could see it.

import (
	"bytes"
	"errors"
	"os/exec"
	"strings"
)

type keychainSecret struct{ service string }

func systemSecretStore() secretStore {
	if _, err := exec.LookPath("security"); err != nil {
		return nil
	}
	return keychainSecret{service: secretService}
}

func (k keychainSecret) Get() (string, error) {
	out, err := exec.Command("security", "find-generic-password", "-a", secretAccount, "-s", k.service, "-w").Output()
	if err != nil {
		var ee *exec.ExitError
		if errors.As(err, &ee) && ee.ExitCode() == 44 { // errSecItemNotFound
			return "", errNoSecret
		}
		return "", errors.New("could not read the token from the Keychain")
	}
	t := strings.TrimSpace(string(out))
	if t == "" {
		return "", errNoSecret
	}
	return t, nil
}

func (k keychainSecret) Set(token string) error {
	if !tokenRe.MatchString(token) {
		return errors.New("the server sent a token in an unexpected format")
	}
	cmd := exec.Command("security", "-i")
	cmd.Stdin = strings.NewReader("add-generic-password -U -a " + secretAccount + " -s " + k.service +
		" -l GPP-Link-Get-Producers-Paid -w " + token + "\n")
	var out bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &out
	if err := cmd.Run(); err != nil {
		return errors.New("could not save the token in the Keychain")
	}
	// interactive mode exits 0 even when a command fails, so read it back
	if got, err := k.Get(); err != nil || got != token {
		return errors.New("could not save the token in the Keychain")
	}
	return nil
}

func (k keychainSecret) Delete() error {
	err := exec.Command("security", "delete-generic-password", "-a", secretAccount, "-s", k.service).Run()
	var ee *exec.ExitError
	if err != nil && !(errors.As(err, &ee) && ee.ExitCode() == 44) {
		return errors.New("could not remove the token from the Keychain")
	}
	return nil
}

func (keychainSecret) Where() string { return "the macOS Keychain" }

func testSystemStore() secretStore { return keychainSecret{service: secretService + ".test"} }
