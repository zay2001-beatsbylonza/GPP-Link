//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

var procGetDiskFreeSpaceExW = syscall.NewLazyDLL("kernel32.dll").NewProc("GetDiskFreeSpaceExW")

// diskFree returns the bytes free on the drive holding p, or -1 if unknown.
func diskFree(p string) int64 {
	ptr, err := syscall.UTF16PtrFromString(p)
	if err != nil {
		return -1
	}
	var free, total, totalFree uint64
	r, _, _ := procGetDiskFreeSpaceExW.Call(uintptr(unsafe.Pointer(ptr)),
		uintptr(unsafe.Pointer(&free)), uintptr(unsafe.Pointer(&total)), uintptr(unsafe.Pointer(&totalFree)))
	if r == 0 {
		return -1
	}
	return int64(free)
}
