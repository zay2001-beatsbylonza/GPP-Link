//go:build !darwin && !linux && !windows

package main

func diskFree(string) int64 { return -1 }
