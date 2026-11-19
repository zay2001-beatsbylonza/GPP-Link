//go:build windows

package main

// Windows Credential Manager (advapi32 CredWriteW / CredReadW / CredDeleteW),
// standard library only.

import (
	"errors"
	"syscall"
	"unsafe"
)

const (
	credTypeGeneric         = 1
	credPersistLocalMachine = 2
	errorNotFound           = 1168
)

var (
	advapi32       = syscall.NewLazyDLL("advapi32.dll")
	procCredWriteW = advapi32.NewProc("CredWriteW")
	procCredReadW  = advapi32.NewProc("CredReadW")
	procCredDelete = advapi32.NewProc("CredDeleteW")
	procCredFree   = advapi32.NewProc("CredFree")
)

// CREDENTIALW
type winCredential struct {
	Flags              uint32
	Type               uint32
	TargetName         *uint16
	Comment            *uint16
	LastWritten        syscall.Filetime
	CredentialBlobSize uint32
	CredentialBlob     *byte
	Persist            uint32
	AttributeCount     uint32
	Attributes         uintptr
	TargetAlias        *uint16
	UserName           *uint16
}

type credManSecret struct{ target string }

func systemSecretStore() secretStore {
	if procCredWriteW.Find() != nil {
		return nil
	}
	return credManSecret{target: secretService}
}

func (c credManSecret) Get() (string, error) {
	target, _ := syscall.UTF16PtrFromString(c.target)
	var p *winCredential
	r, _, err := procCredReadW.Call(uintptr(unsafe.Pointer(target)), credTypeGeneric, 0, uintptr(unsafe.Pointer(&p)))
	if r == 0 {
		if errno, ok := err.(syscall.Errno); ok && errno == errorNotFound {
			return "", errNoSecret
		}
		return "", errors.New("could not read the token from Credential Manager")
	}
	defer procCredFree.Call(uintptr(unsafe.Pointer(p)))
	if p.CredentialBlobSize == 0 || p.CredentialBlob == nil {
		return "", errNoSecret
	}
	return string(unsafe.Slice(p.CredentialBlob, p.CredentialBlobSize)), nil
}

func (c credManSecret) Set(token string) error {
	if !tokenRe.MatchString(token) {
		return errors.New("the server sent a token in an unexpected format")
	}
	target, _ := syscall.UTF16PtrFromString(c.target)
	user, _ := syscall.UTF16PtrFromString(secretAccount)
	comment, _ := syscall.UTF16PtrFromString("GPP Link: Get Producers Paid connection")
	blob := []byte(token)
	cred := winCredential{
		Type:               credTypeGeneric,
		TargetName:         target,
		Comment:            comment,
		CredentialBlobSize: uint32(len(blob)),
		CredentialBlob:     &blob[0],
		Persist:            credPersistLocalMachine,
		UserName:           user,
	}
	if r, _, _ := procCredWriteW.Call(uintptr(unsafe.Pointer(&cred)), 0); r == 0 {
		return errors.New("could not save the token in Credential Manager")
	}
	return nil
}

func (c credManSecret) Delete() error {
	target, _ := syscall.UTF16PtrFromString(c.target)
	r, _, err := procCredDelete.Call(uintptr(unsafe.Pointer(target)), credTypeGeneric, 0)
	if r == 0 {
		if errno, ok := err.(syscall.Errno); ok && errno == errorNotFound {
			return nil
		}
		return errors.New("could not remove the token from Credential Manager")
	}
	return nil
}

func (credManSecret) Where() string { return "Windows Credential Manager" }

func testSystemStore() secretStore { return credManSecret{target: secretService + ".test"} }
