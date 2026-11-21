//go:build darwin || linux

package main

import "syscall"

// diskFree returns the bytes free on the drive holding p, or -1 if unknown.
func diskFree(p string) int64 {
	var st syscall.Statfs_t
	if err := syscall.Statfs(p, &st); err != nil {
		return -1
	}
	return int64(st.Bavail) * int64(st.Bsize)
}
