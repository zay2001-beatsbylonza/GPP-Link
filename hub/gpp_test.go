package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeGPP is a pretend Get Producers Paid server that follows MPC_LINK_API.md.
type fakeGPP struct {
	t   *testing.T
	srv *httptest.Server

	mu            sync.Mutex
	codes         map[string]bool // unused pairing codes
	tokens        map[string]bool // token -> revoked
	drafts        map[string]*fakeDraft
	byKey         map[string]string // clientKey -> draftId
	starts        int
	completes     int
	conflictsLeft int // answer 409 to this many complete calls first
	maxBytes      int64
	hubNames      []string
}

type fakeDraft struct {
	name, contentType, status string
	size                      int64
	data                      []byte
}

func newFakeGPP(t *testing.T) *fakeGPP {
	f := &fakeGPP{t: t, codes: map[string]bool{"K7Q4-MZP2": true}, tokens: map[string]bool{},
		drafts: map[string]*fakeDraft{}, byKey: map[string]string{}, maxBytes: 300 << 20}
	f.srv = httptest.NewServer(http.HandlerFunc(f.serve))
	t.Cleanup(f.srv.Close)
	return f
}

func (f *fakeGPP) fail(w http.ResponseWriter, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func (f *fakeGPP) ok(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func (f *fakeGPP) auth(w http.ResponseWriter, r *http.Request) bool {
	tok := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	revoked, known := f.tokens[tok]
	if !known {
		f.fail(w, 401, "This hub token isn't valid. Pair MPC Link again from the app.")
		return false
	}
	if revoked {
		f.fail(w, 401, "This hub was disconnected in the app. Pair it again to keep sending beats.")
		return false
	}
	return true
}

func (f *fakeGPP) serve(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	p := r.URL.Path
	switch {
	case r.Method == "POST" && p == "/api/mpclink/pair":
		var b struct{ Code, HubName string }
		json.NewDecoder(r.Body).Decode(&b)
		if !f.codes[b.Code] {
			f.fail(w, 400, "That code isn't valid or was already used. Get a new code in the app.")
			return
		}
		f.codes[b.Code] = false
		tok := fmt.Sprintf("gpph_%016x_secretsecret", len(f.tokens)+1)
		f.tokens[tok] = false
		f.hubNames = append(f.hubNames, b.HubName)
		f.ok(w, map[string]string{"token": tok, "producerId": "p_lonza", "storeName": "BeatsbyLonza", "scope": "drafts:add"})
	case r.Method == "GET" && p == "/api/mpclink/me":
		if f.auth(w, r) {
			f.ok(w, map[string]string{"producerId": "p_lonza", "storeName": "BeatsbyLonza", "scope": "drafts:add", "hubName": "Test"})
		}
	case r.Method == "POST" && p == "/api/mpclink/drafts":
		if !f.auth(w, r) {
			return
		}
		f.starts++
		var b struct {
			FileName  string `json:"fileName"`
			Size      int64  `json:"size"`
			ClientKey string `json:"clientKey"`
		}
		json.NewDecoder(r.Body).Decode(&b)
		if id, dup := f.byKey[b.ClientKey]; dup && b.ClientKey != "" {
			f.ok(w, map[string]any{"duplicate": true, "draftId": id, "status": "draft"})
			return
		}
		if b.Size > f.maxBytes {
			f.fail(w, 413, "Files can be up to 300 MB.")
			return
		}
		id := fmt.Sprintf("d%d", len(f.drafts)+1)
		f.drafts[id] = &fakeDraft{name: b.FileName, size: b.Size, status: "uploading", contentType: "audio/wav"}
		f.byKey[b.ClientKey] = id
		f.ok(w, map[string]any{"draftId": id, "uploadUrl": f.srv.URL + "/storage/" + id, "method": "PUT",
			"headers": map[string]string{"Content-Type": "audio/wav", "x-goog-content-length-range": "0,314572800"}, "expiresAt": 0})
	case r.Method == "PUT" && strings.HasPrefix(p, "/storage/"):
		d := f.drafts[strings.TrimPrefix(p, "/storage/")]
		if d == nil || r.Header.Get("Authorization") != "" ||
			r.Header.Get("Content-Type") != "audio/wav" || r.Header.Get("x-goog-content-length-range") != "0,314572800" {
			w.WriteHeader(403)
			return
		}
		d.data, _ = io.ReadAll(r.Body)
		w.WriteHeader(200)
	case r.Method == "POST" && strings.HasPrefix(p, "/api/mpclink/drafts/") && strings.HasSuffix(p, "/complete"):
		if !f.auth(w, r) {
			return
		}
		f.completes++
		if f.conflictsLeft > 0 {
			f.conflictsLeft--
			f.fail(w, 409, "The file isn't uploaded yet.")
			return
		}
		id := strings.TrimSuffix(strings.TrimPrefix(p, "/api/mpclink/drafts/"), "/complete")
		d := f.drafts[id]
		if d == nil || int64(len(d.data)) != d.size {
			f.fail(w, 422, "The uploaded file didn't match. Send it again.")
			return
		}
		d.status = "draft"
		f.ok(w, map[string]string{"draftId": id, "status": "draft"})
	default:
		f.fail(w, 404, "Unknown route")
	}
}

func (f *fakeGPP) finished() map[string][]byte {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := map[string][]byte{}
	for _, d := range f.drafts {
		if d.status == "draft" {
			out[d.name] = d.data
		}
	}
	return out
}

func writeOld(t *testing.T, p string, data []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, data, 0o644); err != nil {
		t.Fatal(err)
	}
	old := time.Now().Add(-time.Minute)
	os.Chtimes(p, old, old)
}

func TestGetProducersPaidUploads(t *testing.T) {
	gppStableFor, gppCompleteWait = 2*time.Second, 5*time.Millisecond
	fake := newFakeGPP(t)
	root := t.TempDir()
	state := t.TempDir()
	t.Setenv("GPP_LINK_TOKEN_FILE", filepath.Join(state, "token"))
	gp := filepath.Join(root, gppDirName)
	writeOld(t, filepath.Join(gp, "Night Drive_140bpm_F#min.wav"), []byte("RIFF night drive"))
	writeOld(t, filepath.Join(gp, "Stems", "Song A", "bass.wav"), []byte("bass"))
	writeOld(t, filepath.Join(gp, "Stems", "Song A", "drums.wav"), []byte("drums!"))
	writeOld(t, filepath.Join(gp, "notes.txt"), []byte("not audio"))
	writeOld(t, filepath.Join(gp, ".hidden.wav"), []byte("hidden"))
	writeOld(t, filepath.Join(gp, "Sent", "old.wav"), []byte("in another folder"))
	os.WriteFile(filepath.Join(gp, "exporting.wav"), []byte("still being written"), 0o644) // brand new

	m, err := newGPPManager(root, "Lonza's Mac Studio", fake.srv.URL+"/api/mpclink", state)
	if err != nil {
		t.Fatal(err)
	}
	// not connected yet: counts what's waiting, sends nothing
	m.pass()
	if s := m.Status(); s.Connected || s.Waiting != 4 || fake.starts != 0 { // 3 ready + 1 still being written
		t.Fatalf("before pairing: %+v starts=%d", s, fake.starts)
	}

	// pairing: bad format, wrong code, then the real one
	if _, err := m.Pair("hello"); err == nil || !strings.Contains(err.Error(), "8-character") {
		t.Fatalf("bad format: %v", err)
	}
	if _, err := m.Pair("AAAA-BBBB"); err == nil || !strings.Contains(err.Error(), "isn't valid") {
		t.Fatalf("wrong code should show the server's message, got %v", err)
	}
	msg, err := m.Pair(" k7q4mzp2 ") // any case, with or without the dash
	if err != nil || !strings.Contains(msg, "BeatsbyLonza") {
		t.Fatalf("pair: %q %v", msg, err)
	}
	if fake.hubNames[0] != "Lonza's Mac Studio" {
		t.Fatalf("hub name sent: %q", fake.hubNames[0])
	}
	if b, _ := os.ReadFile(filepath.Join(state, "token")); !strings.HasPrefix(string(b), "gpph_") {
		t.Fatalf("token not stored: %q", b)
	}

	fake.conflictsLeft = 2 // storage is slow to see the first file
	m.pass()
	got := fake.finished()
	want := map[string]string{
		"Night Drive_140bpm_F#min.wav": "RIFF night drive",
		"Song A - bass.wav":            "bass",
		"Song A - drums.wav":           "drums!",
	}
	if len(got) != len(want) {
		t.Fatalf("drafts: %v", keys(got))
	}
	for name, data := range want {
		if string(got[name]) != data {
			t.Errorf("draft %q = %q, want %q", name, got[name], data)
		}
	}
	if s := m.Status(); s.SentCount != 3 || s.Waiting != 1 || s.State != "ready" || s.LastSent == "" { // 1 = still being written
		t.Fatalf("after sending: %+v", s)
	}

	// the file that was still being written goes once it settles
	old := time.Now().Add(-time.Minute)
	os.Chtimes(filepath.Join(gp, "exporting.wav"), old, old)
	m.pass() // it just changed since the last look: wait one more pass
	if _, ok := fake.finished()["exporting.wav"]; ok {
		t.Fatal("file sent on the same pass it changed")
	}
	m.pass()
	if _, ok := fake.finished()["exporting.wav"]; !ok {
		t.Fatal("settled file wasn't sent")
	}

	// nothing goes twice
	starts := fake.starts
	m.pass()
	if fake.starts != starts {
		t.Fatalf("sent again: %d -> %d", starts, fake.starts)
	}

	// restart: token and sent list come back, still nothing twice
	m2, _ := newGPPManager(root, "Lonza's Mac Studio", fake.srv.URL+"/api/mpclink", state)
	m2.checkConnection()
	m2.pass()
	if s := m2.Status(); !s.Connected || s.StoreName != "BeatsbyLonza" || fake.starts != starts {
		t.Fatalf("after restart: %+v starts %d->%d", s, starts, fake.starts)
	}

	// sent list lost: the server says "duplicate", no new drafts
	os.Remove(filepath.Join(state, "gpp-sent.json"))
	m3, _ := newGPPManager(root, "Lonza's Mac Studio", fake.srv.URL+"/api/mpclink", state)
	m3.checkConnection()
	n := len(fake.drafts)
	m3.pass()
	if len(fake.drafts) != n || m3.Status().SentCount != 4 {
		t.Fatalf("duplicates made new drafts: %d -> %d, %+v", n, len(fake.drafts), m3.Status())
	}

	// too big: skipped with the server's message, the rest still goes
	fake.maxBytes = 10
	writeOld(t, filepath.Join(gp, "huge.wav"), []byte("this one is far too big"))
	writeOld(t, filepath.Join(gp, "small.wav"), []byte("tiny"))
	m3.pass()
	if _, ok := fake.finished()["small.wav"]; !ok {
		t.Fatal("small file after the skipped one wasn't sent")
	}
	if s := m3.Status(); len(s.Skipped) != 1 || !strings.Contains(s.Skipped[0], "huge.wav: Files can be up to 300 MB") {
		t.Fatalf("skip message: %+v", s)
	}
	starts = fake.starts
	m3.pass()
	if fake.starts != starts {
		t.Fatal("skipped file was tried again")
	}

	// disconnected in the app: show the message, forget the token, stop
	for tok := range fake.tokens {
		fake.tokens[tok] = true
	}
	writeOld(t, filepath.Join(gp, "after.wav"), []byte("after"))
	m3.pass()
	s := m3.Status()
	if s.Connected || s.State != "needs-pair" || !strings.Contains(s.Message, "disconnected in the app") {
		t.Fatalf("after revoke: %+v", s)
	}
	if _, err := os.Stat(filepath.Join(state, "token")); !os.IsNotExist(err) {
		t.Fatal("revoked token still stored")
	}
	m3.pass()                  // stays stopped
	if fake.starts != starts { // the 401 is answered before a draft starts
		t.Fatalf("kept trying after 401: %d", fake.starts-starts)
	}
}

func TestGetProducersPaidServerAddress(t *testing.T) {
	for _, bad := range []string{"http://app.beatsbylonza.com/api/mpclink", "ftp://x", "app.beatsbylonza.com"} {
		if _, err := newGPPClient(bad); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
	for _, good := range []string{defaultGPPServer, "http://127.0.0.1:9999/api", "https://us-east1-x.cloudfunctions.net/mpcLink/"} {
		if _, err := newGPPClient(good); err != nil {
			t.Errorf("%q refused: %v", good, err)
		}
	}
	// an upload link that isn't https is refused before any bytes go
	c, _ := newGPPClient(defaultGPPServer)
	if err := c.putFile(gppDraftStart{UploadURL: "http://evil.example/x"}, bytes.NewReader(nil), 0); err == nil {
		t.Error("http upload link accepted")
	}
}

func TestGetProducersPaidPairingOnlyFromThisComputer(t *testing.T) {
	fake := newFakeGPP(t)
	state := t.TempDir()
	t.Setenv("GPP_LINK_TOKEN_FILE", filepath.Join(state, "token"))
	m, _ := newGPPManager(t.TempDir(), "Test", fake.srv.URL+"/api/mpclink", state)
	h := &hub{port: 47800, gpp: m}
	try := func(remote, origin string, header bool) int {
		r := httptest.NewRequest("POST", "/api/gpp/pair", strings.NewReader(`{"code":"K7Q4-MZP2"}`))
		r.RemoteAddr = remote
		if origin != "" {
			r.Header.Set("Origin", origin)
		}
		if header {
			r.Header.Set("X-GPP-Link", "1")
		}
		w := httptest.NewRecorder()
		h.gppPair(w, r)
		return w.Code
	}
	if c := try("192.168.1.20:5000", "", true); c != 403 {
		t.Errorf("phone on the Wi-Fi could pair: %d", c)
	}
	if c := try("127.0.0.1:5000", "", false); c != 403 {
		t.Errorf("no X-GPP-Link header could pair: %d", c)
	}
	if c := try("127.0.0.1:5000", "https://evil.example", true); c != 403 {
		t.Errorf("another website could pair: %d", c)
	}
	if c := try("127.0.0.1:5000", "http://localhost:47800", true); c != 200 {
		t.Errorf("hub page on this computer couldn't pair: %d", c)
	}
	// the phone sees the status, without where the token is kept
	r := httptest.NewRequest("GET", "/api/gpp", nil)
	r.RemoteAddr = "192.168.1.20:5000"
	w := httptest.NewRecorder()
	h.gppStatus(w, r)
	var s GPPStatus
	json.Unmarshal(w.Body.Bytes(), &s)
	if !s.Connected || s.CanManage || s.KeptIn != "" || strings.Contains(w.Body.String(), "gpph_") {
		t.Errorf("phone status: %s", w.Body.String())
	}
}

func keys(m map[string][]byte) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	return out
}
