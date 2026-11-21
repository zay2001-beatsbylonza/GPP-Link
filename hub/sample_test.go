package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// sampleSetup makes a Beats folder, a pretend /Volumes with a "MPC SAMPLE"
// card and a USB stick, and a hub serving the MPC SAMPLE GPP routes.
func sampleSetup(t *testing.T) (*hub, *httptest.Server, string, string) {
	t.Helper()
	tmp := t.TempDir()
	beats := filepath.Join(tmp, "Beats")
	vols := filepath.Join(tmp, "Volumes")
	card := filepath.Join(vols, "MPC SAMPLE")
	for _, d := range []string{beats, card, filepath.Join(vols, "USB STICK"), filepath.Join(card, "samples"), filepath.Join(card, "user", "export")} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	os.WriteFile(filepath.Join(beats, "Trap Beat_140bpm.mp3"), []byte("beat-bytes"), 0o644)
	os.WriteFile(filepath.Join(beats, "notes.txt"), []byte("no"), 0o644)
	os.WriteFile(filepath.Join(card, "samples", "Factory Kick.wav"), []byte("factory"), 0o644)
	os.WriteFile(filepath.Join(card, "user", "export", "Chop 1.wav"), []byte("chop-bytes"), 0o644)
	os.WriteFile(filepath.Join(card, "user", "Pattern.mid"), []byte("midi"), 0o644)
	os.WriteFile(filepath.Join(card, "user", "export", "user/export/._Chop 1.wav"), []byte("mac junk"), 0o644)
	os.Symlink("/", filepath.Join(vols, "Macintosh HD")) // the Mac's own disk

	oldRoots, oldEject, oldFree := sampleVolumeRoots, sampleEject, sampleFreeSpace
	sampleVolumeRoots = func() []string { return []string{vols} }
	sampleFreeSpace = func(string) int64 { return 8 << 30 }
	t.Cleanup(func() { sampleVolumeRoots, sampleEject, sampleFreeSpace = oldRoots, oldEject, oldFree })

	root, _ := filepath.EvalSymlinks(beats)
	h := &hub{root: root, name: "test", port: 47800, devices: map[string]time.Time{}, queue: map[string][]string{},
		cue: map[string]string{}, sample: newSampleManager(filepath.Join(tmp, "state"))}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/sample", h.sampleStatus)
	mux.HandleFunc("POST /api/sample/pick", h.samplePick)
	mux.HandleFunc("POST /api/sample/folder", h.sampleFolder)
	mux.HandleFunc("POST /api/sample/send", h.sampleSend)
	mux.HandleFunc("GET /api/sample/folders", h.sampleFolders)
	mux.HandleFunc("POST /api/sample/sendfolder", h.sampleSendFolder)
	mux.HandleFunc("POST /api/sample/pull", h.samplePull)
	mux.HandleFunc("POST /api/sample/eject", h.sampleEjectRoute)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return h, srv, root, card
}

func samplePost(t *testing.T, srv *httptest.Server, route string, body any, header bool) (int, string) {
	t.Helper()
	b, _ := json.Marshal(body)
	req, _ := http.NewRequest("POST", srv.URL+route, bytes.NewReader(b))
	req.Header.Set("Content-Type", "application/json")
	if header {
		req.Header.Set("X-GPP-Link", "1")
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var buf bytes.Buffer
	buf.ReadFrom(resp.Body)
	return resp.StatusCode, strings.TrimSpace(buf.String())
}

func sampleGet(t *testing.T, srv *httptest.Server) sampleStatus {
	t.Helper()
	resp, err := http.Get(srv.URL + "/api/sample")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var s sampleStatus
	if err := json.NewDecoder(resp.Body).Decode(&s); err != nil {
		t.Fatal(err)
	}
	return s
}

func TestMPCSampleGPP(t *testing.T) {
	h, srv, root, card := sampleSetup(t)

	// nothing picked yet: the two drives are offered, not the Mac's own disk
	s := sampleGet(t, srv)
	if s.Connected || len(s.Candidates) != 2 || s.Candidates[0].Name != "MPC SAMPLE" || s.Candidates[1].Name != "USB STICK" {
		t.Fatalf("before picking: %+v", s)
	}
	// sending before it's picked
	if code, _ := samplePost(t, srv, "/api/sample/send", map[string]string{"path": "Trap Beat_140bpm.mp3"}, true); code != 409 {
		t.Fatalf("send with no drive: %d", code)
	}
	// a drive that isn't plugged in, and the Mac's own disk, can't be picked
	for _, d := range []string{"NOPE", "Macintosh HD", "../x"} {
		if code, _ := samplePost(t, srv, "/api/sample/pick", map[string]string{"drive": d}, true); code != 404 {
			t.Fatalf("picking %q: %d", d, code)
		}
	}
	// picking needs the header
	if code, _ := samplePost(t, srv, "/api/sample/pick", map[string]string{"drive": "MPC SAMPLE"}, false); code != 403 {
		t.Fatalf("pick without header: %d", code)
	}
	if code, body := samplePost(t, srv, "/api/sample/pick", map[string]string{"drive": "MPC SAMPLE"}, true); code != 200 {
		t.Fatalf("pick: %d %s", code, body)
	}

	// connected: the user folder opens by itself; its beats are listed, not the
	// factory samples or Mac junk files
	s = sampleGet(t, srv)
	if !s.Connected || s.Drive != "MPC SAMPLE" || s.Folder != "user" || len(s.Files) != 2 {
		t.Fatalf("after picking: %+v", s)
	}
	for _, f := range s.Files {
		if strings.HasPrefix(f.Name, ".") {
			t.Fatalf("hidden file listed: %s", f.Path)
		}
	}

	// remembered across restarts
	if again := newSampleManager(filepath.Dir(h.sample.cfgPath)); again.cfg.Drive != "MPC SAMPLE" {
		t.Fatalf("not remembered: %+v", again.cfg)
	}

	// Send to MPC Sample: into user/, then a second copy gets (2)
	for _, want := range []string{"Trap Beat_140bpm.mp3", "Trap Beat_140bpm (2).mp3"} {
		code, body := samplePost(t, srv, "/api/sample/send", map[string]string{"path": "Trap Beat_140bpm.mp3"}, true)
		if code != 200 || !strings.Contains(body, want) {
			t.Fatalf("send: %d %s", code, body)
		}
		if b, _ := os.ReadFile(filepath.Join(card, "user", want)); string(b) != "beat-bytes" {
			t.Fatalf("card copy %s: %q", want, b)
		}
	}
	// only shared file types, only from the Beats folder
	for _, p := range []string{"notes.txt", "../Volumes/MPC SAMPLE/user/Pattern.mid", "/etc/passwd"} {
		if code, _ := samplePost(t, srv, "/api/sample/send", map[string]string{"path": p}, true); code == 200 {
			t.Fatalf("sent %q", p)
		}
	}

	// a folder on the card
	if code, _ := samplePost(t, srv, "/api/sample/folder", map[string]string{"folder": "../escape"}, true); code != 400 {
		t.Fatalf("bad folder accepted: %d", code)
	}
	if code, body := samplePost(t, srv, "/api/sample/folder", map[string]string{"folder": "/Beats From Mac/"}, true); code != 200 {
		t.Fatalf("folder: %d %s", code, body)
	}
	if code, body := samplePost(t, srv, "/api/sample/send", map[string]string{"path": "Trap Beat_140bpm.mp3"}, true); code != 200 {
		t.Fatalf("send to folder: %d %s", code, body)
	}
	if _, err := os.Stat(filepath.Join(card, "Beats From Mac", "Trap Beat_140bpm.mp3")); err != nil {
		t.Fatal(err)
	}

	// Send to Folder: lands in From MPC/MPC Sample/
	if code, body := samplePost(t, srv, "/api/sample/pull", map[string]string{"path": "user/export/Chop 1.wav", "dest": "folder"}, true); code != 200 {
		t.Fatalf("pull to folder: %d %s", code, body)
	}
	if b, _ := os.ReadFile(filepath.Join(root, "From MPC", "MPC Sample", "Chop 1.wav")); string(b) != "chop-bytes" {
		t.Fatalf("pulled: %q", b)
	}
	// Send to GPP: lands in Get Producers Paid/; MIDI can't go there
	if code, body := samplePost(t, srv, "/api/sample/pull", map[string]string{"path": "user/export/Chop 1.wav", "dest": "gpp"}, true); code != 200 {
		t.Fatalf("pull to gpp: %d %s", code, body)
	}
	if _, err := os.Stat(filepath.Join(root, "Get Producers Paid", "Chop 1.wav")); err != nil {
		t.Fatal(err)
	}
	if code, _ := samplePost(t, srv, "/api/sample/pull", map[string]string{"path": "user/Pattern.mid", "dest": "gpp"}, true); code != 415 {
		t.Fatalf("midi to gpp: %d", code)
	}
	// nothing outside the card, no hidden files, no unknown destinations
	for _, req := range []map[string]string{
		{"path": "../USB STICK/x.wav", "dest": "folder"},
		{"path": "user/export/._Chop 1.wav", "dest": "folder"},
		{"path": "user/export/Chop 1.wav", "dest": "elsewhere"},
	} {
		if code, _ := samplePost(t, srv, "/api/sample/pull", req, true); code == 200 {
			t.Fatalf("pulled %v", req)
		}
	}
	// copies need the header (another web site can't trigger them)
	if code, _ := samplePost(t, srv, "/api/sample/pull", map[string]string{"path": "user/export/Chop 1.wav", "dest": "folder"}, false); code != 403 {
		t.Fatalf("pull without header: %d", code)
	}

	// Send Folder to MPC Sample: a sample pack with a folder inside
	pack := filepath.Join(root, "Packs", "Soul Chops")
	os.MkdirAll(filepath.Join(pack, "Drums"), 0o755)
	os.WriteFile(filepath.Join(pack, "Chop A.wav"), []byte("a"), 0o644)
	os.WriteFile(filepath.Join(pack, "Drums", "Kick.wav"), []byte("k"), 0o644)
	os.WriteFile(filepath.Join(pack, "readme.txt"), []byte("skip me"), 0o644)
	resp, err := http.Get(srv.URL + "/api/sample/folders")
	if err != nil {
		t.Fatal(err)
	}
	var folders []sampleFolderInfo
	json.NewDecoder(resp.Body).Decode(&folders)
	resp.Body.Close()
	var got *sampleFolderInfo
	for i := range folders {
		if folders[i].Path == "Packs/Soul Chops" {
			got = &folders[i]
		}
		if strings.HasPrefix(folders[i].Path, "From MPC") || strings.HasPrefix(folders[i].Path, "Get Producers Paid") {
			t.Fatalf("listed %s", folders[i].Path)
		}
	}
	if got == nil || got.Files != 2 {
		t.Fatalf("folders: %+v", folders)
	}
	h.sample.mu.Lock()
	h.sample.cfg.Folder = "user"
	h.sample.mu.Unlock()
	for _, want := range []string{"Soul Chops", "Soul Chops (2)"} {
		code, body := samplePost(t, srv, "/api/sample/sendfolder", map[string]string{"path": "Packs/Soul Chops"}, true)
		if code != 200 || !strings.Contains(body, `"files":2`) {
			t.Fatalf("send folder: %d %s", code, body)
		}
		if b, _ := os.ReadFile(filepath.Join(card, "user", want, "Drums", "Kick.wav")); string(b) != "k" {
			t.Fatalf("%s: kick %q", want, b)
		}
		if _, err := os.Stat(filepath.Join(card, "user", want, "readme.txt")); err == nil {
			t.Fatal("non-audio file copied")
		}
	}
	for _, p := range []string{"../Volumes", "Packs/Nope", "Trap Beat_140bpm.mp3"} {
		if code, _ := samplePost(t, srv, "/api/sample/sendfolder", map[string]string{"path": p}, true); code == 200 {
			t.Fatalf("sent folder %q", p)
		}
	}

	// too big for the card: nothing is written
	sampleFreeSpace = func(string) int64 { return 100 << 10 } // 100 KB, less than 2 files' room
	code, body := samplePost(t, srv, "/api/sample/sendfolder", map[string]string{"path": "Packs/Soul Chops"}, true)
	if code != 507 || !strings.Contains(body, "free") {
		t.Fatalf("too-big folder: %d %s", code, body)
	}
	if _, err := os.Stat(filepath.Join(card, "user", "Soul Chops (3)")); err == nil {
		t.Fatal("partial folder written")
	}
	sampleFreeSpace = func(string) int64 { return 1 } // 1 byte
	if code, _ := samplePost(t, srv, "/api/sample/send", map[string]string{"path": "Trap Beat_140bpm.mp3"}, true); code != 507 {
		t.Fatalf("too-big beat: %d", code)
	}
	// space unknown, and the copy fails partway: the partial folder is removed
	sampleFreeSpace = func(string) int64 { return -1 }
	os.Chmod(filepath.Join(pack, "Drums", "Kick.wav"), 0o000) // can't be read
	if os.Getuid() != 0 && runtime.GOOS != "windows" {
		code, body = samplePost(t, srv, "/api/sample/sendfolder", map[string]string{"path": "Packs/Soul Chops"}, true)
		if code != 500 || !strings.Contains(body, "removed") {
			t.Fatalf("failed folder: %d %s", code, body)
		}
		if _, err := os.Stat(filepath.Join(card, "user", "Soul Chops (3)")); err == nil {
			t.Fatal("partial folder left on the card")
		}
	}
	os.Chmod(filepath.Join(pack, "Drums", "Kick.wav"), 0o644)
	sampleFreeSpace = func(string) int64 { return 8 << 30 }
	if s := sampleGet(t, srv); s.Free != 8<<30 {
		t.Fatalf("free space: %d", s.Free)
	}

	// eject
	ejected := ""
	sampleEject = func(m string) error { ejected = m; return nil }
	if code, body := samplePost(t, srv, "/api/sample/eject", nil, true); code != 200 || ejected != card {
		t.Fatalf("eject: %d %s (%q)", code, body, ejected)
	}
	sampleEject = func(string) error { return errors.New("couldn't eject: busy") }
	if code, _ := samplePost(t, srv, "/api/sample/eject", nil, true); code != 500 {
		t.Fatalf("failed eject: %d", code)
	}

	// unplugged: waiting for it, sends refused
	os.RemoveAll(card)
	s = sampleGet(t, srv)
	if s.Connected || s.Drive != "MPC SAMPLE" {
		t.Fatalf("after unplugging: %+v", s)
	}
	if code, _ := samplePost(t, srv, "/api/sample/send", map[string]string{"path": "Trap Beat_140bpm.mp3"}, true); code != 409 {
		t.Fatalf("send while unplugged: %d", code)
	}
}
