package main

// MPC SAMPLE GPP: the MPC Sample has no Wi-Fi and can't run plugins, so it
// can't use GPP Link the way the other MPCs do. Instead, the producer puts it
// in SD Card Mode and plugs it into this computer with USB-C. Its card shows up
// as a drive, and the hub page gets an MPC SAMPLE GPP box:
//
//   - Send to MPC Sample: copies a beat from the Beats folder onto the card
//   - Send to Folder:     copies a beat off the card into <Beats>/From MPC/MPC Sample/
//   - Send to GPP:        copies a beat off the card into <Beats>/Get Producers Paid/,
//                         which the hub uploads to the app as a draft
//   - Eject:              so the card is safe to unplug
//
// Everything is sent by hand; nothing syncs by itself. Nothing is overwritten:
// a second kick.wav becomes kick (2).wav, on the card and on the computer.
//
// The first time, the producer picks which drive is the MPC Sample; the hub
// remembers it (sample.json next to the Get Producers Paid state).

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
)

const (
	sampleDeviceName = "MPC Sample" // its folder under From MPC/
	sampleMaxFiles   = 2000         // card listing cap
	sampleMaxDepth   = 6
)

// sampleVolumeRoots lists the folders where removable drives appear (tests replace it).
var sampleVolumeRoots = func() []string {
	switch runtime.GOOS {
	case "darwin":
		return []string{"/Volumes"}
	case "windows":
		return nil // drive letters, handled in sampleCandidates
	default:
		var roots []string
		if u := os.Getenv("USER"); u != "" {
			roots = append(roots, filepath.Join("/media", u), filepath.Join("/run/media", u))
		}
		return append(roots, "/media")
	}
}

// sampleEject unmounts the drive (tests replace it).
var sampleEject = func(mount string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		cmd = exec.Command("diskutil", "eject", mount)
	case "linux":
		cmd = exec.Command("umount", mount)
	default:
		return errors.New("eject the MPC Sample's drive from your computer before unplugging it")
	}
	out, err := cmd.CombinedOutput()
	if err != nil {
		msg := strings.TrimSpace(string(out))
		if msg == "" {
			msg = err.Error()
		}
		return fmt.Errorf("couldn't eject: %s", msg)
	}
	return nil
}

// sampleFreeSpace reports the bytes free on the card, or -1 if unknown (tests replace it).
var sampleFreeSpace = diskFree

// sampleRoom is extra room kept per file: memory cards store files in blocks
// (often 32 KB), so a small file takes a whole block.
const sampleRoom = 64 << 10

func humanSize(n int64) string {
	switch {
	case n >= 1<<30:
		return fmt.Sprintf("%.1f GB", float64(n)/(1<<30))
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MB", float64(n)/(1<<20))
	default:
		return fmt.Sprintf("%d KB", max(1, n>>10))
	}
}

// sampleFits refuses a copy the card has no room for, before anything is written.
func sampleFits(mount, what string, bytes int64, files int) error {
	free := sampleFreeSpace(mount)
	if free < 0 {
		return nil // unknown: try, and clean up if it runs out
	}
	need := bytes + int64(files)*sampleRoom
	if need > free {
		return fmt.Errorf("%s needs %s, the MPC Sample has %s free", what, humanSize(need), humanSize(free))
	}
	return nil
}

type sampleConfig struct {
	Drive     string `json:"drive"`     // the drive's name, e.g. "MPC SAMPLE"
	SendTo    string `json:"sendTo"`    // where beats go on the card, if the producer changed it
	SendToSet bool   `json:"sendToSet"` // false: use sampleDefaultSendTo
}

type sampleManager struct {
	mu      sync.Mutex
	cfg     sampleConfig
	cfgPath string
	busy    string // what's being copied right now
}

func newSampleManager(stateDir string) *sampleManager {
	s := &sampleManager{cfgPath: filepath.Join(stateDir, "sample.json")}
	if b, err := os.ReadFile(s.cfgPath); err == nil {
		json.Unmarshal(b, &s.cfg)
	}
	return s
}

func (s *sampleManager) saveLocked() error {
	if err := os.MkdirAll(filepath.Dir(s.cfgPath), 0o700); err != nil {
		return err
	}
	b, _ := json.MarshalIndent(s.cfg, "", " ")
	tmp := s.cfgPath + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, s.cfgPath)
}

type sampleDrive struct {
	Name  string `json:"name"`
	mount string
}

// sampleCandidates lists the removable drives plugged into this computer.
func sampleCandidates() []sampleDrive {
	var out []sampleDrive
	roots := sampleVolumeRoots()
	if runtime.GOOS == "windows" && roots == nil {
		sys := strings.ToUpper(strings.TrimSuffix(os.Getenv("SystemDrive"), ":"))
		for c := 'D'; c <= 'Z'; c++ {
			if string(c) == sys {
				continue
			}
			m := string(c) + `:\`
			if info, err := os.Stat(m); err == nil && info.IsDir() {
				out = append(out, sampleDrive{Name: string(c) + ":", mount: m})
			}
		}
		return out
	}
	seen := map[string]bool{}
	for _, root := range roots {
		entries, err := os.ReadDir(root)
		if err != nil {
			continue
		}
		for _, e := range entries {
			n := e.Name()
			if strings.HasPrefix(n, ".") || seen[n] || strings.HasPrefix(n, "com.apple.") {
				continue
			}
			m := filepath.Join(root, n)
			// the Mac's own disk shows up in /Volumes as a link to "/"
			if e.Type()&fs.ModeSymlink != 0 {
				continue
			}
			if info, err := os.Stat(m); err != nil || !info.IsDir() {
				continue
			}
			if r, err := filepath.EvalSymlinks(m); err != nil || r == "/" {
				continue
			}
			seen[n] = true
			out = append(out, sampleDrive{Name: n, mount: m})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}

// sampleUserDir finds the MPC Sample's user folder on the card (where its
// exports live), relative to the card: "user" at the top, or one or two levels
// down (e.g. "MPC-Sample/user"). A user folder holding "export" wins. It
// returns "" if the card has none.
func sampleUserDir(mount string) string {
	type cand struct {
		rel    string
		export bool
	}
	var found []cand
	level := []string{""}
	for depth := 0; depth < 3 && len(level) > 0; depth++ {
		var next []string
		for _, rel := range level {
			entries, _ := os.ReadDir(filepath.Join(mount, filepath.FromSlash(rel)))
			for _, e := range entries {
				n := e.Name()
				if !e.IsDir() || strings.HasPrefix(n, ".") || strings.HasPrefix(n, "$") || n == "System Volume Information" {
					continue
				}
				r := path.Join(rel, n)
				if strings.EqualFold(n, "user") {
					hasExport := false
					sub, _ := os.ReadDir(filepath.Join(mount, filepath.FromSlash(r)))
					for _, x := range sub {
						if x.IsDir() && strings.EqualFold(x.Name(), "export") {
							hasExport = true
						}
					}
					found = append(found, cand{r, hasExport})
					continue
				}
				if !strings.EqualFold(n, "samples") { // the factory kits can't hold the user folder
					next = append(next, r)
				}
			}
		}
		for _, c := range found {
			if c.export {
				return c.rel
			}
		}
		if len(found) > 0 {
			return found[0].rel
		}
		sort.Strings(next)
		level = next
	}
	return ""
}

const sampleSendDirName = "GPP Link" // beats sent from the computer land in a folder of their own

// sampleDefaultSendTo is where beats go unless the producer picks another
// folder: straight into the MPC Sample's user folder (it doesn't look inside
// extra folders there), or a "GPP Link" folder on a card without one.
func sampleDefaultSendTo(mount string) string {
	if u := sampleUserDir(mount); u != "" {
		return u
	}
	return sampleSendDirName
}

// sendTo returns where beats go on the card mounted at m.
func (c sampleConfig) sendTo(m string) string {
	if c.SendToSet {
		return c.SendTo
	}
	return sampleDefaultSendTo(m)
}

// sampleDest resolves the send-to folder on the card (it's created on the first send).
func sampleDest(m string, cfg sampleConfig) (string, error) {
	f := cfg.sendTo(m)
	if f == "" {
		return m, nil
	}
	return sampleSafe(m, f)
}

// mount returns where the chosen drive is mounted, or "" if it isn't plugged in.
func (s *sampleManager) mount() (string, sampleConfig) {
	s.mu.Lock()
	cfg := s.cfg
	s.mu.Unlock()
	if cfg.Drive == "" {
		return "", cfg
	}
	for _, d := range sampleCandidates() {
		if d.Name == cfg.Drive {
			return d.mount, cfg
		}
	}
	return "", cfg
}

// sampleSafe joins rel under root and makes sure the result stays inside root.
func sampleSafe(root, rel string) (string, error) {
	rel = strings.ReplaceAll(rel, "\\", "/")
	if rel == "" || strings.HasPrefix(rel, "/") || strings.ContainsRune(rel, 0) {
		return "", errBadPath
	}
	for _, seg := range strings.Split(rel, "/") {
		if seg == "" || seg == "." || seg == ".." || strings.HasPrefix(seg, ".") {
			return "", errBadPath
		}
		if runtime.GOOS == "windows" && windowsUnsafe(seg) {
			return "", errBadPath
		}
	}
	realRoot, err := filepath.EvalSymlinks(root)
	if err != nil {
		return "", err
	}
	full := filepath.Join(root, filepath.FromSlash(rel))
	check := full
	for {
		if r, err := filepath.EvalSymlinks(check); err == nil {
			if !inside(r, realRoot) {
				return "", errBadPath
			}
			break
		}
		parent := filepath.Dir(check)
		if parent == check {
			return "", errBadPath
		}
		check = parent
	}
	return full, nil
}

// copyNoOverwrite copies src into dir/name (renaming to "name (2)" if taken),
// flushing it to disk so the card is safe to eject. It returns the final path.
func copyNoOverwrite(src, dir, name string) (string, error) {
	in, err := os.Open(src)
	if err != nil {
		return "", err
	}
	defer in.Close()
	if info, err := in.Stat(); err != nil || !info.Mode().IsRegular() {
		return "", errors.New("not a file")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	tmp, err := os.CreateTemp(dir, ".gpplink-*.part")
	if err != nil {
		return "", err
	}
	_, err = io.Copy(tmp, in)
	if err == nil {
		err = tmp.Sync()
	}
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp.Name())
		return "", err
	}
	final := freeName(filepath.Join(dir, name))
	if err := os.Rename(tmp.Name(), final); err != nil {
		os.Remove(tmp.Name())
		return "", err
	}
	return final, nil
}

// ---------- hub page routes ----------

type sampleStatus struct {
	Connected  bool          `json:"connected"`
	Drive      string        `json:"drive,omitempty"`  // the remembered drive
	Folder     string        `json:"folder"`           // where beats go on the card (relative to it)
	Candidates []sampleDrive `json:"candidates"`       // drives to pick from
	Files      []entry       `json:"files"`            // beats on the card
	More       bool          `json:"more,omitempty"`   // the card has more than we listed
	Busy       string        `json:"busy,omitempty"`   // copying right now
	CanManage  bool          `json:"canManage"`        // pick the drive, change the folder, eject
	Free       int64         `json:"free"`             // bytes free on the card (-1 unknown)
	NoUser     bool          `json:"noUser,omitempty"` // no MPC Sample user folder found on the card
}

func (h *hub) sampleStatus(w http.ResponseWriter, r *http.Request) {
	s := h.sample
	m, cfg := s.mount()
	st := sampleStatus{Drive: cfg.Drive, Candidates: sampleCandidates(), Files: []entry{},
		CanManage: h.isLocalRequest(r)}
	if st.Candidates == nil {
		st.Candidates = []sampleDrive{}
	}
	s.mu.Lock()
	st.Busy = s.busy
	s.mu.Unlock()
	if m == "" {
		writeJSON(w, st)
		return
	}
	st.Connected = true
	st.Free = sampleFreeSpace(m)
	st.Folder = cfg.sendTo(m)
	// list the producer's own folder (the card's user folder, with exports and
	// the GPP Link folder), not the factory kits; paths stay relative to the card
	top := m
	if u := sampleUserDir(m); u != "" {
		top = filepath.Join(m, filepath.FromSlash(u))
	} else {
		st.NoUser = true
	}
	n := 0
	filepath.WalkDir(top, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if p == top {
			return nil
		}
		rel, _ := filepath.Rel(m, p)
		if strings.HasPrefix(d.Name(), ".") || strings.HasPrefix(d.Name(), "$") || d.Name() == "System Volume Information" {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() {
			if strings.Count(rel, string(os.PathSeparator)) >= sampleMaxDepth {
				return filepath.SkipDir
			}
			return nil
		}
		if d.Type()&fs.ModeSymlink != 0 || !allowedExt[strings.ToLower(filepath.Ext(p))] {
			return nil
		}
		if n >= sampleMaxFiles {
			st.More = true
			return filepath.SkipAll
		}
		info, err := d.Info()
		if err != nil {
			return nil
		}
		n++
		st.Files = append(st.Files, entry{Path: filepath.ToSlash(rel), Name: d.Name(), Size: info.Size(), Mtime: info.ModTime().Unix()})
		return nil
	})
	sort.Slice(st.Files, func(i, j int) bool { return st.Files[i].Mtime > st.Files[j].Mtime })
	writeJSON(w, st)
}

// sampleAllowed: changes need the X-GPP-Link header (so another web site can't
// trigger them); picking the drive, the folder and ejecting only from this computer.
func (h *hub) sampleAllowed(r *http.Request, manage bool) error {
	if manage {
		return h.gppManageAllowed(r)
	}
	if r.Header.Get("X-GPP-Link") != "1" {
		return errors.New("refused")
	}
	return nil
}

func readJSON(r *http.Request, v any) error {
	return json.NewDecoder(io.LimitReader(r.Body, 8192)).Decode(v)
}

func (h *hub) samplePick(w http.ResponseWriter, r *http.Request) {
	if err := h.sampleAllowed(r, true); err != nil {
		httpErr(w, 403, err)
		return
	}
	var req struct {
		Drive string `json:"drive"`
	}
	if err := readJSON(r, &req); err != nil {
		httpErr(w, 400, err)
		return
	}
	found := req.Drive == "" // "" forgets the drive
	for _, d := range sampleCandidates() {
		if d.Name == req.Drive {
			found = true
		}
	}
	if !found {
		httpErr(w, 404, errors.New("that drive isn't plugged in"))
		return
	}
	s := h.sample
	s.mu.Lock()
	if s.cfg.Drive != req.Drive {
		s.cfg.SendTo, s.cfg.SendToSet = "", false
	}
	s.cfg.Drive = req.Drive
	err := s.saveLocked()
	s.mu.Unlock()
	if err != nil {
		httpErr(w, 500, err)
		return
	}
	log.Printf("MPC Sample drive: %q", req.Drive)
	writeJSON(w, map[string]any{"ok": true})
}

func (h *hub) sampleFolder(w http.ResponseWriter, r *http.Request) {
	if err := h.sampleAllowed(r, true); err != nil {
		httpErr(w, 403, err)
		return
	}
	var req struct {
		Folder string `json:"folder"`
	}
	if err := readJSON(r, &req); err != nil {
		httpErr(w, 400, err)
		return
	}
	f := strings.Trim(strings.TrimSpace(strings.ReplaceAll(req.Folder, "\\", "/")), "/")
	if f != "" {
		if m, _ := h.sample.mount(); m != "" {
			if _, err := sampleSafe(m, f); err != nil {
				httpErr(w, 400, errors.New("that folder name won't work"))
				return
			}
		} else if _, err := h.validRel(f); err != nil {
			httpErr(w, 400, errors.New("that folder name won't work"))
			return
		}
	}
	s := h.sample
	s.mu.Lock()
	s.cfg.SendTo, s.cfg.SendToSet = f, f != "" // blank: back to the GPP Link folder
	err := s.saveLocked()
	s.mu.Unlock()
	if err != nil {
		httpErr(w, 500, err)
		return
	}
	m, cfg := h.sample.mount()
	writeJSON(w, map[string]any{"ok": true, "folder": cfg.sendTo(m)})
}

func (h *hub) setSampleBusy(what string) {
	h.sample.mu.Lock()
	h.sample.busy = what
	h.sample.mu.Unlock()
}

// sampleSend copies a beat from the Beats folder onto the card.
func (h *hub) sampleSend(w http.ResponseWriter, r *http.Request) {
	if err := h.sampleAllowed(r, false); err != nil {
		httpErr(w, 403, err)
		return
	}
	var req struct {
		Path string `json:"path"`
	}
	if err := readJSON(r, &req); err != nil {
		httpErr(w, 400, err)
		return
	}
	src, err := h.safePath(req.Path)
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	if info, err := os.Stat(src); err != nil || !info.Mode().IsRegular() || !allowedExt[strings.ToLower(filepath.Ext(src))] {
		httpErr(w, 404, errors.New("not found"))
		return
	}
	m, cfg := h.sample.mount()
	if m == "" {
		httpErr(w, 409, errors.New("the MPC Sample isn't plugged in (put it in SD Card Mode)"))
		return
	}
	dir, err := sampleDest(m, cfg)
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	name := filepath.Base(src)
	if info, err := os.Stat(src); err == nil {
		if err := sampleFits(m, name, info.Size(), 1); err != nil {
			httpErr(w, 507, err)
			return
		}
	}
	h.setSampleBusy(name)
	defer h.setSampleBusy("")
	final, err := copyNoOverwrite(src, dir, name)
	if err != nil {
		httpErr(w, 500, fmt.Errorf("couldn't copy to the MPC Sample: %v", err))
		return
	}
	rel, _ := filepath.Rel(m, final)
	log.Printf("MPC Sample: sent %s -> card %s", req.Path, filepath.ToSlash(rel))
	writeJSON(w, map[string]any{"saved": filepath.ToSlash(rel)})
}

// samplePull copies a beat off the card into From MPC/MPC Sample/ (dest "folder")
// or Get Producers Paid/ (dest "gpp").
func (h *hub) samplePull(w http.ResponseWriter, r *http.Request) {
	if err := h.sampleAllowed(r, false); err != nil {
		httpErr(w, 403, err)
		return
	}
	var req struct {
		Path string `json:"path"`
		Dest string `json:"dest"`
	}
	if err := readJSON(r, &req); err != nil {
		httpErr(w, 400, err)
		return
	}
	m, _ := h.sample.mount()
	if m == "" {
		httpErr(w, 409, errors.New("the MPC Sample isn't plugged in (put it in SD Card Mode)"))
		return
	}
	src, err := sampleSafe(m, req.Path)
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	ext := strings.ToLower(filepath.Ext(src))
	if info, err := os.Lstat(src); err != nil || !info.Mode().IsRegular() || !allowedExt[ext] {
		httpErr(w, 404, errors.New("not found on the card"))
		return
	}
	var base string
	switch req.Dest {
	case "folder":
		base = path.Join(inboxDirName, sampleDeviceName)
	case "gpp":
		if !gppAudioExt[ext] {
			httpErr(w, 415, errors.New("only audio files can go to Get Producers Paid"))
			return
		}
		base = gppDirName
	default:
		httpErr(w, 400, errors.New("unknown destination"))
		return
	}
	dir, err := h.safePath(base)
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	name := filepath.Base(src)
	h.setSampleBusy(name)
	defer h.setSampleBusy("")
	final, err := copyNoOverwrite(src, dir, name)
	if err != nil {
		httpErr(w, 500, fmt.Errorf("couldn't copy from the MPC Sample: %v", err))
		return
	}
	saved, _ := filepath.Rel(h.root, final)
	log.Printf("MPC Sample: card %s -> %s", req.Path, filepath.ToSlash(saved))
	if req.Dest == "gpp" && h.gpp != nil {
		h.gpp.Kick()
	}
	writeJSON(w, map[string]any{"saved": filepath.ToSlash(saved)})
}

func (h *hub) sampleEjectRoute(w http.ResponseWriter, r *http.Request) {
	if err := h.sampleAllowed(r, true); err != nil {
		httpErr(w, 403, err)
		return
	}
	m, _ := h.sample.mount()
	if m == "" {
		writeJSON(w, map[string]any{"ok": true, "message": "Already unplugged."})
		return
	}
	h.sample.mu.Lock()
	busy := h.sample.busy
	h.sample.mu.Unlock()
	if busy != "" {
		httpErr(w, 409, fmt.Errorf("still copying %s; eject when it's done", busy))
		return
	}
	if err := sampleEject(m); err != nil {
		httpErr(w, 500, err)
		return
	}
	log.Printf("MPC Sample: ejected %s", m)
	writeJSON(w, map[string]any{"ok": true, "message": "Ejected. You can unplug the MPC Sample and leave SD Card Mode."})
}

// ---------- Send Folder to MPC Sample ----------

const sampleMaxFolderFiles = 5000

type sampleFolderInfo struct {
	Path  string `json:"path"`
	Name  string `json:"name"`
	Files int    `json:"files"`
	Bytes int64  `json:"bytes"`
}

// sampleFolderFiles lists the shareable files under dir (any depth), relative to dir.
func sampleFolderFiles(dir string) (files []string, bytes int64) {
	filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil || p == dir {
			return nil
		}
		if strings.HasPrefix(d.Name(), ".") {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() || d.Type()&fs.ModeSymlink != 0 || !allowedExt[strings.ToLower(filepath.Ext(p))] {
			return nil
		}
		if len(files) >= sampleMaxFolderFiles {
			return filepath.SkipAll
		}
		if info, err := d.Info(); err == nil {
			bytes += info.Size()
		}
		rel, _ := filepath.Rel(dir, p)
		files = append(files, rel)
		return nil
	})
	return files, bytes
}

// sampleFolders lists the folders in the Beats folder that hold samples or
// beats (sample packs, kits, stems), so one can go to the MPC Sample in one go.
func (h *hub) sampleFolders(w http.ResponseWriter, r *http.Request) {
	skip := map[string]bool{
		filepath.Join(h.root, gppDirName):   true,
		filepath.Join(h.root, inboxDirName): true,
	}
	out := []sampleFolderInfo{}
	filepath.WalkDir(h.root, func(p string, d fs.DirEntry, err error) error {
		if err != nil || !d.IsDir() || p == h.root {
			return nil
		}
		if strings.HasPrefix(d.Name(), ".") || skip[p] {
			return filepath.SkipDir
		}
		rel, _ := filepath.Rel(h.root, p)
		if strings.Count(rel, string(os.PathSeparator)) >= 3 {
			return filepath.SkipDir
		}
		if files, bytes := sampleFolderFiles(p); len(files) > 0 {
			out = append(out, sampleFolderInfo{Path: filepath.ToSlash(rel), Name: d.Name(), Files: len(files), Bytes: bytes})
		}
		return nil
	})
	sort.Slice(out, func(i, j int) bool { return strings.ToLower(out[i].Path) < strings.ToLower(out[j].Path) })
	writeJSON(w, out)
}

// freeDirName returns dir, or "dir (2)", "dir (3)" ... if it's taken.
func freeDirName(p string) string {
	if _, err := os.Lstat(p); errors.Is(err, os.ErrNotExist) {
		return p
	}
	for i := 2; ; i++ {
		c := fmt.Sprintf("%s (%d)", p, i)
		if _, err := os.Lstat(c); errors.Is(err, os.ErrNotExist) {
			return c
		}
	}
}

// sampleSendFolder copies a whole folder from the Beats folder onto the card,
// keeping its name and the folders inside it. An existing folder on the card
// is left alone: the copy becomes "Pack (2)".
func (h *hub) sampleSendFolder(w http.ResponseWriter, r *http.Request) {
	if err := h.sampleAllowed(r, false); err != nil {
		httpErr(w, 403, err)
		return
	}
	var req struct {
		Path string `json:"path"`
	}
	if err := readJSON(r, &req); err != nil {
		httpErr(w, 400, err)
		return
	}
	src, err := h.safePath(req.Path)
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	if info, err := os.Stat(src); err != nil || !info.IsDir() {
		httpErr(w, 404, errors.New("folder not found"))
		return
	}
	files, total := sampleFolderFiles(src)
	if len(files) == 0 {
		httpErr(w, 404, errors.New("no samples or beats in that folder"))
		return
	}
	m, cfg := h.sample.mount()
	if m == "" {
		httpErr(w, 409, errors.New("the MPC Sample isn't plugged in (put it in SD Card Mode)"))
		return
	}
	dir, err := sampleDest(m, cfg)
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	if err := sampleFits(m, filepath.Base(src), total, len(files)); err != nil {
		httpErr(w, 507, err)
		return
	}
	// dest is a new folder (an existing one is never reused), so if the copy
	// can't finish, removing it takes away only what this copy wrote
	dest := freeDirName(filepath.Join(dir, filepath.Base(src)))
	name := filepath.Base(dest)
	defer h.setSampleBusy("")
	for i, rel := range files {
		h.setSampleBusy(fmt.Sprintf("%s (%d of %d)", name, i+1, len(files)))
		if _, err := copyNoOverwrite(filepath.Join(src, rel), filepath.Join(dest, filepath.Dir(rel)), filepath.Base(rel)); err != nil {
			os.RemoveAll(dest)
			log.Printf("MPC Sample: folder %s stopped after %d of %d files (%v); removed the partial copy", req.Path, i, len(files), err)
			httpErr(w, 500, fmt.Errorf("couldn't finish copying %s (%v), so the partial folder was removed from the card", filepath.Base(src), err))
			return
		}
	}
	saved, _ := filepath.Rel(m, dest)
	log.Printf("MPC Sample: sent folder %s -> card %s (%d files)", req.Path, filepath.ToSlash(saved), len(files))
	writeJSON(w, map[string]any{"saved": filepath.ToSlash(saved), "files": len(files)})
}
