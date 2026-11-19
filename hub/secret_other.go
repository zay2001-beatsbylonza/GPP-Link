//go:build !darwin && !windows

package main

// Linux and others: no system store here, so newSecretStore falls back to a
// private file (0600) in the hub's state folder.
func systemSecretStore() secretStore { return nil }
