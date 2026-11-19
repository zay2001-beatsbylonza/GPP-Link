package main

// Get Producers Paid: sends every beat in <Beats>/Get Producers Paid/ (and its
// Stems/ sets) to the producer's drafts in the Get Producers Paid app.
//
// Pairing: the producer gets a one-time code in the app (Producer → Profile →
// Connect GPP Link) and types it into the hub page on this computer. The hub
// trades it for a token that can only add drafts, and keeps the token in the
// macOS Keychain / Windows Credential Manager.
//
// Sending a file: POST /drafts (announce) → PUT to the signed URL → POST
// /drafts/<id>/complete. Sent files are remembered, so nothing goes twice.

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

const defaultGPPServer = "https://app.beatsbylonza.com/api/mpclink"

// tuning (tests shorten these)
var (
	gppScanEvery    = 10 * time.Second // look in the folder this often
	gppStableFor    = 3 * time.Second  // a file must sit unchanged this long before it goes
	gppCompleteWait = 3 * time.Second  // wait between confirm retries on 409
)

// the audio types the server takes as drafts
var gppAudioExt = map[string]bool{".wav": true, ".aif": true, ".aiff": true, ".mp3": true, ".flac": true, ".ogg": true, ".m4a": true}

var pairCodeRe = regexp.MustCompile(`^[A-Z0-9]{4}-?[A-Z0-9]{4}$`)

// ---------- the server API ----------

// gppError is an error answer from the server; Msg is written for the producer.
type gppError struct {
	Status int
	Msg    string
}

func (e *gppError) Error() string { return e.Msg }

type gppClient struct {
	base string
	hc   *http.Client
}

func newGPPClient(base string) (*gppClient, error) {
	base = strings.TrimRight(strings.TrimSpace(base), "/")
	if err := checkHTTPS(base); err != nil {
		return nil, fmt.Errorf("Get Producers Paid server address: %v", err)
	}
	return &gppClient{base: base, hc: &http.Client{Timeout: 30 * time.Second}}, nil
}

// checkHTTPS allows only https, except plain http to this computer (tests).
func checkHTTPS(raw string) error {
	u, err := url.Parse(raw)
	if err != nil || u.Host == "" {
		return errors.New("not a web address")
	}
	if u.Scheme == "https" {
		return nil
	}
	if u.Scheme == "http" {
		if ip := net.ParseIP(u.Hostname()); (ip != nil && ip.IsLoopback()) || u.Hostname() == "localhost" {
			return nil
		}
	}
	return errors.New("must start with https://")
}

func (c *gppClient) call(method, route, token string, body, out any) error {
	var rd io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	}
	req, err := http.NewRequest(method, c.base+route, rd)
	if err != nil {
		return err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	req.Header.Set("User-Agent", "GPP-Link-hub/"+version)
	resp, err := c.hc.Do(req)
	if err != nil {
		return fmt.Errorf("can't reach Get Producers Paid (%v)", shortNetErr(err))
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != http.StatusOK {
		var e struct {
			Error string `json:"error"`
		}
		msg := ""
		if json.Unmarshal(data, &e) == nil {
			msg = strings.TrimSpace(e.Error)
		}
		if msg == "" {
			msg = fmt.Sprintf("Get Producers Paid answered %d.", resp.StatusCode)
		}
		return &gppError{Status: resp.StatusCode, Msg: msg}
	}
	if out != nil {
		if err := json.Unmarshal(data, out); err != nil {
			return errors.New("Get Producers Paid sent an answer the hub doesn't understand")
		}
	}
	return nil
}

func shortNetErr(err error) string {
	var ue *url.Error
	if errors.As(err, &ue) {
		err = ue.Err
	}
	s := err.Error()
	if len(s) > 120 {
		s = s[:120]
	}
	return s
}

type gppPairResult struct {
	Token      string `json:"token"`
	ProducerID string `json:"producerId"`
	StoreName  string `json:"storeName"`
	Scope      string `json:"scope"`
}

type gppMe struct {
	ProducerID string `json:"producerId"`
	StoreName  string `json:"storeName"`
	Scope      string `json:"scope"`
	HubName    string `json:"hubName"`
}

type gppDraftStart struct {
	Duplicate bool              `json:"duplicate"`
	DraftID   string            `json:"draftId"`
	Status    string            `json:"status"`
	UploadURL string            `json:"uploadUrl"`
	Method    string            `json:"method"`
	Headers   map[string]string `json:"headers"`
}

func (c *gppClient) pair(code, hubName string) (gppPairResult, error) {
	var r gppPairResult
	err := c.call("POST", "/pair", "", map[string]string{"code": code, "hubName": hubName}, &r)
	if err == nil && r.Token == "" {
		err = errors.New("Get Producers Paid didn't send a token back. Get a new code and try again.")
	}
	return r, err
}

func (c *gppClient) me(token string) (gppMe, error) {
	var r gppMe
	return r, c.call("GET", "/me", token, nil, &r)
}

func (c *gppClient) startDraft(token, fileName string, size int64, clientKey string) (gppDraftStart, error) {
	var r gppDraftStart
	err := c.call("POST", "/drafts", token, map[string]any{"fileName": fileName, "size": size, "clientKey": clientKey}, &r)
	if err == nil && !r.Duplicate && (r.DraftID == "" || r.UploadURL == "") {
		err = errors.New("Get Producers Paid didn't send an upload link")
	}
	return r, err
}

// putFile sends the file's bytes to the signed storage URL, with exactly the
// headers the server gave and no Authorization header.
func (c *gppClient) putFile(d gppDraftStart, f io.Reader, size int64) error {
	if err := checkHTTPS(d.UploadURL); err != nil {
		return fmt.Errorf("upload link: %v", err)
	}
	method := strings.ToUpper(d.Method)
	if method == "" {
		method = "PUT"
	}
	if method != "PUT" {
		return fmt.Errorf("upload link asks for %s; the hub only uploads with PUT", method)
	}
	req, err := http.NewRequest("PUT", d.UploadURL, f)
	if err != nil {
		return err
	}
	req.ContentLength = size
	for k, v := range d.Headers {
		if strings.EqualFold(k, "Authorization") || strings.EqualFold(k, "Host") || strings.EqualFold(k, "Content-Length") {
			continue
		}
		req.Header.Set(k, v)
	}
	hc := &http.Client{Timeout: 30 * time.Minute} // big wavs on slow upload links
	resp, err := hc.Do(req)
	if err != nil {
		return fmt.Errorf("upload failed (%v)", shortNetErr(err))
	}
	io.Copy(io.Discard, io.LimitReader(resp.Body, 1<<20))
	resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return &gppError{Status: 0, Msg: fmt.Sprintf("the file upload was refused (HTTP %d)", resp.StatusCode)}
	}
	return nil
}

// complete confirms the upload; 409 means storage hasn't seen the file yet, so retry a few times.
func (c *gppClient) complete(token, draftID string) error {
	route := "/drafts/" + url.PathEscape(draftID) + "/complete"
	var err error
	for i := 0; i < 5; i++ {
		err = c.call("POST", route, token, nil, nil)
		var ge *gppError
		if !(errors.As(err, &ge) && ge.Status == 409) {
			return err
		}
		time.Sleep(gppCompleteWait)
	}
	return err
}

// ---------- the uploader ----------

type gppSent struct {
	Name    string `json:"name"`
	DraftID string `json:"draftId,omitempty"`
	At      int64  `json:"at"`
	Skipped string `json:"skipped,omitempty"` // why it will never be sent (too big, wrong type)
}

type gppFile struct {
	abs, rel, name, key string
	size                int64
	mtime               time.Time
}

type gppObs struct {
	size  int64
	mtime time.Time
}

// GPPStatus is what the hub page shows.
type GPPStatus struct {
	Connected  bool     `json:"connected"`
	StoreName  string   `json:"storeName,omitempty"`
	State      string   `json:"state"` // off, checking, ready, sending, paused, offline, needs-pair
	Message    string   `json:"message,omitempty"`
	Current    string   `json:"current,omitempty"`
	LastSent   string   `json:"lastSent,omitempty"`
	LastSentAt int64    `json:"lastSentAt,omitempty"`
	Waiting    int      `json:"waiting"`
	Skipped    []string `json:"skipped,omitempty"` // files that can't be drafts, and why (latest first)
	SentCount  int      `json:"sentCount"`
	Server     string   `json:"server"`
	CanManage  bool     `json:"canManage"`
	KeptIn     string   `json:"keptIn,omitempty"`
}

type gppManager struct {
	root      string // the Beats folder
	hubName   string
	client    *gppClient
	store     secretStore
	statePath string

	mu         sync.Mutex
	token      string
	producerID string
	storeName  string
	state      string
	message    string
	current    string
	lastSent   string
	lastSentAt time.Time
	waiting    int
	skipped    []string
	sent       map[string]map[string]gppSent // producerId -> clientKey -> record
	seen       map[string]gppObs             // abs path -> last size/mtime seen
	redo       map[string]int                // clientKey -> times the server rejected the upload (422)
	retryAt    time.Time
	failures   int

	sendMu sync.Mutex // one sending pass at a time
	kick   chan struct{}
}

func newGPPManager(root, hubName, server, stateDir string) (*gppManager, error) {
	c, err := newGPPClient(server)
	if err != nil {
		return nil, err
	}
	m := &gppManager{
		root: root, hubName: hubName, client: c,
		store:     newSecretStore(stateDir),
		statePath: filepath.Join(stateDir, "gpp-sent.json"),
		state:     "off",
		sent:      map[string]map[string]gppSent{},
		seen:      map[string]gppObs{},
		redo:      map[string]int{},
		kick:      make(chan struct{}, 1),
	}
	m.loadSent()
	if t, err := m.store.Get(); err == nil {
		m.token, m.state = t, "checking"
	} else if !errors.Is(err, errNoSecret) {
		m.message = err.Error()
	}
	return m, nil
}

func gppStateDir() string {
	if d := os.Getenv("GPP_LINK_STATE_DIR"); d != "" {
		return d
	}
	if d, err := os.UserConfigDir(); err == nil {
		return filepath.Join(d, "GPP Link")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".gpp-link")
}

func (m *gppManager) loadSent() {
	b, err := os.ReadFile(m.statePath)
	if err != nil {
		return
	}
	var s struct {
		Sent map[string]map[string]gppSent `json:"sent"`
	}
	if json.Unmarshal(b, &s) == nil && s.Sent != nil {
		m.sent = s.Sent
	}
}

// saveSent writes the record of sent files (m.mu held).
func (m *gppManager) saveSent() {
	b, _ := json.MarshalIndent(map[string]any{"server": m.client.base, "sent": m.sent}, "", " ")
	if err := os.MkdirAll(filepath.Dir(m.statePath), 0o700); err != nil {
		log.Printf("Get Producers Paid: can't save sent list: %v", err)
		return
	}
	tmp := m.statePath + ".tmp"
	err := os.WriteFile(tmp, b, 0o600)
	if err == nil {
		err = os.Rename(tmp, m.statePath)
	}
	if err != nil {
		log.Printf("Get Producers Paid: can't save sent list: %v", err)
	}
}

// Kick makes the uploader look at the folder now (after the MPC drops a beat in).
func (m *gppManager) Kick() {
	select {
	case m.kick <- struct{}{}:
	default:
	}
}

func (m *gppManager) run() {
	m.checkConnection()
	again := m.pass()
	t := time.NewTicker(gppScanEvery)
	defer t.Stop()
	for {
		var soon <-chan time.Time
		if again { // a file was still being written: look again once it has settled
			soon = time.After(gppStableFor + 500*time.Millisecond)
		}
		select {
		case <-t.C:
		case <-m.kick:
		case <-soon:
		}
		again = m.pass()
	}
}

// checkConnection asks the server who this token belongs to.
func (m *gppManager) checkConnection() {
	m.mu.Lock()
	token := m.token
	m.mu.Unlock()
	if token == "" {
		return
	}
	me, err := m.client.me(token)
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.token != token { // re-paired meanwhile
		return
	}
	if err != nil {
		m.failLocked(err, nil)
		return
	}
	m.producerID, m.storeName = me.ProducerID, me.StoreName
	m.state, m.message = "ready", ""
	m.failures = 0
}

// Pair trades a one-time code from the app for a token.
func (m *gppManager) Pair(code string) (string, error) {
	code = strings.ToUpper(strings.Join(strings.Fields(code), ""))
	if !pairCodeRe.MatchString(code) {
		return "", errors.New("Enter the 8-character code shown in the app, like K7Q4-MZP2.")
	}
	if !strings.Contains(code, "-") {
		code = code[:4] + "-" + code[4:]
	}
	r, err := m.client.pair(code, m.hubName)
	if err != nil {
		return "", err
	}
	note := ""
	if err := m.store.Set(r.Token); err != nil {
		note = " (" + err.Error() + ": it will work until the hub closes, then you'll need to pair again)"
		log.Printf("Get Producers Paid: %v", err)
	}
	m.mu.Lock()
	m.token, m.producerID, m.storeName = r.Token, r.ProducerID, r.StoreName
	m.state, m.message, m.failures, m.retryAt = "ready", "", 0, time.Time{}
	m.mu.Unlock()
	if r.ProducerID == "" { // older server: ask who we are
		m.checkConnection()
	}
	log.Printf("Get Producers Paid: connected to %s", r.StoreName)
	m.Kick()
	return "Connected to " + r.StoreName + "." + note, nil
}

// Disconnect forgets the token on this computer.
func (m *gppManager) Disconnect() error {
	err := m.store.Delete()
	m.mu.Lock()
	m.token, m.producerID, m.storeName = "", "", ""
	m.state, m.message, m.current = "off", "Disconnected on this computer. To fully remove it, also tap Disconnect in the app.", ""
	m.mu.Unlock()
	return err
}

func (m *gppManager) Status() GPPStatus {
	m.mu.Lock()
	defer m.mu.Unlock()
	s := GPPStatus{
		Connected: m.token != "", StoreName: m.storeName, State: m.state, Message: m.message,
		Current: m.current, LastSent: m.lastSent, Waiting: m.waiting,
		SentCount: len(m.sent[m.producerID]), Server: m.client.base, Skipped: append([]string(nil), m.skipped...),
	}
	if !m.lastSentAt.IsZero() {
		s.LastSentAt = m.lastSentAt.Unix()
	}
	if m.token != "" {
		s.KeptIn = m.store.Where()
	}
	return s
}

// failLocked records an error from the server or network (m.mu held). It
// returns true when sending should stop for now; f is the file being sent, if any.
func (m *gppManager) failLocked(err error, f *gppFile) (stop bool) {
	var ge *gppError
	now := time.Now()
	if errors.As(err, &ge) {
		switch ge.Status {
		case 401: // disconnected in the app, or a bad token: forget it
			m.store.Delete()
			m.token, m.producerID, m.storeName, m.current = "", "", "", ""
			m.state, m.message = "needs-pair", ge.Msg
			log.Printf("Get Producers Paid: %s", ge.Msg)
			return true
		case 403:
			m.state, m.message, m.retryAt = "paused", ge.Msg, now.Add(10*time.Minute)
			return true
		case 429:
			m.state, m.message, m.retryAt = "paused", ge.Msg, now.Add(time.Hour)
			return true
		case 404:
			m.state, m.message, m.retryAt = "paused", ge.Msg+" (check the Get Producers Paid server address)", now.Add(10*time.Minute)
			return true
		case 400, 413, 415:
			if f != nil { // this file can never go: remember why, carry on with the rest
				m.recordLocked(f, "", ge.Msg)
				m.noteSkipLocked(f.name + ": " + ge.Msg)
				log.Printf("Get Producers Paid: skipped %s: %s", f.rel, ge.Msg)
				return false
			}
			m.message = ge.Msg
			return true
		case 422:
			if f != nil {
				m.redo[f.key]++
				if m.redo[f.key] >= 3 {
					m.recordLocked(f, "", ge.Msg)
					m.noteSkipLocked(f.name + ": " + ge.Msg)
				}
			}
			return false
		}
	}
	// server trouble or no internet: back off and try again
	m.failures++
	wait := 30 * time.Second << min(m.failures-1, 5)
	if wait > 10*time.Minute {
		wait = 10 * time.Minute
	}
	m.retryAt = now.Add(wait)
	m.state, m.message = "offline", err.Error()+" Trying again soon."
	log.Printf("Get Producers Paid: %v (retrying in %s)", err, wait.Round(time.Second))
	return true
}

func (m *gppManager) noteSkipLocked(s string) {
	m.skipped = append([]string{s}, m.skipped...)
	if len(m.skipped) > 5 {
		m.skipped = m.skipped[:5]
	}
}

func (m *gppManager) recordLocked(f *gppFile, draftID, skipped string) {
	if m.sent[m.producerID] == nil {
		m.sent[m.producerID] = map[string]gppSent{}
	}
	m.sent[m.producerID][f.key] = gppSent{Name: f.rel, DraftID: draftID, At: time.Now().Unix(), Skipped: skipped}
	m.saveSent()
}

// scan lists the files in Get Producers Paid/ and Get Producers Paid/Stems/<set>/
// that have stopped changing, and counts the ones still being written.
func (m *gppManager) scan() (ready []gppFile, busy int) {
	base := filepath.Join(m.root, gppDirName)
	var out []gppFile
	now := time.Now()
	seen := map[string]gppObs{}
	add := func(abs, rel, name string) {
		info, err := os.Lstat(abs)
		if err != nil || !info.Mode().IsRegular() || info.Size() == 0 {
			return
		}
		o := gppObs{size: info.Size(), mtime: info.ModTime()}
		seen[abs] = o
		prev, had := m.seen[abs]
		if now.Sub(o.mtime) < gppStableFor || (had && prev != o) {
			busy++ // still being written
			return
		}
		out = append(out, gppFile{abs: abs, rel: rel, name: name, size: o.size, mtime: o.mtime,
			key: fmt.Sprintf("%s|%d|%d", rel, o.size, o.mtime.Unix())})
	}
	ok := func(n string) bool {
		return !strings.HasPrefix(n, ".") && gppAudioExt[strings.ToLower(filepath.Ext(n))]
	}
	entries, _ := os.ReadDir(base)
	for _, e := range entries {
		if e.Type().IsRegular() && ok(e.Name()) {
			add(filepath.Join(base, e.Name()), path.Join(gppDirName, e.Name()), e.Name())
		}
	}
	sets, _ := os.ReadDir(filepath.Join(base, "Stems"))
	for _, s := range sets {
		if !s.IsDir() || strings.HasPrefix(s.Name(), ".") {
			continue
		}
		stems, _ := os.ReadDir(filepath.Join(base, "Stems", s.Name()))
		for _, e := range stems {
			if e.Type().IsRegular() && ok(e.Name()) {
				// the app shows one draft per file, so name each stem after its set
				add(filepath.Join(base, "Stems", s.Name(), e.Name()),
					path.Join(gppDirName, "Stems", s.Name(), e.Name()), s.Name()+" - "+e.Name())
			}
		}
	}
	m.mu.Lock()
	m.seen = seen
	m.mu.Unlock()
	sort.Slice(out, func(i, j int) bool { return out[i].mtime.Before(out[j].mtime) }) // oldest first
	return out, busy
}

// pass sends whatever is waiting, one file at a time. It returns true when a
// file was still being written, so run can look again as soon as it settles.
func (m *gppManager) pass() (again bool) {
	m.sendMu.Lock()
	defer m.sendMu.Unlock()
	files, busy := m.scan()

	m.mu.Lock()
	if m.token != "" && m.producerID == "" && time.Now().After(m.retryAt) {
		m.mu.Unlock()
		m.checkConnection()
		m.mu.Lock()
	}
	todo := files[:0]
	for _, f := range files {
		if _, done := m.sent[m.producerID][f.key]; !done || m.producerID == "" {
			todo = append(todo, f)
		}
	}
	m.waiting = len(todo) + busy
	token := m.token
	ready := token != "" && m.producerID != "" && time.Now().After(m.retryAt)
	m.mu.Unlock()
	if !ready {
		return busy > 0
	}

	for i := range todo {
		f := todo[i]
		m.mu.Lock()
		if m.token != token { // disconnected or re-paired meanwhile
			m.mu.Unlock()
			return busy > 0
		}
		m.state, m.current = "sending", f.name
		m.mu.Unlock()

		err := m.sendOne(token, &f)

		m.mu.Lock()
		m.current = ""
		if err != nil {
			stop := m.failLocked(err, &f)
			if stop {
				m.mu.Unlock()
				return false
			}
		} else {
			m.failures = 0
			m.lastSent, m.lastSentAt = f.name, time.Now()
			m.message = ""
		}
		m.waiting = max(0, m.waiting-1)
		m.state = "ready"
		m.mu.Unlock()
	}
	m.mu.Lock()
	if m.state == "sending" {
		m.state = "ready"
	}
	m.mu.Unlock()
	return busy > 0
}

func (m *gppManager) sendOne(token string, f *gppFile) error {
	d, err := m.client.startDraft(token, f.name, f.size, f.key)
	if err != nil {
		return err
	}
	if !d.Duplicate {
		fh, err := os.Open(f.abs)
		if err != nil {
			return err
		}
		info, err := fh.Stat()
		if err != nil || info.Size() != f.size {
			fh.Close()
			return errors.New(f.name + " changed while sending.")
		}
		err = m.client.putFile(d, fh, f.size)
		fh.Close()
		if err != nil {
			return err
		}
		if err := m.client.complete(token, d.DraftID); err != nil {
			return err
		}
		log.Printf("Get Producers Paid: sent %s as a draft", f.rel)
	} else {
		log.Printf("Get Producers Paid: %s was already a draft", f.rel)
	}
	m.mu.Lock()
	m.recordLocked(f, d.DraftID, "")
	delete(m.redo, f.key)
	m.mu.Unlock()
	return nil
}

// ---------- hub page routes ----------

// Pairing and disconnecting only from the computer the hub runs on (opened as
// localhost or as its own Wi-Fi address), never from another device on the
// Wi-Fi, and never from a web page on another site: the X-GPP-Link header
// forces a CORS preflight, which the hub doesn't answer.

// thisComputer reports whether ip is one of this computer's own addresses.
func thisComputer(ip net.IP) bool {
	if ip == nil {
		return false
	}
	if ip.IsLoopback() {
		return true
	}
	addrs, _ := net.InterfaceAddrs()
	for _, a := range addrs {
		if n, ok := a.(*net.IPNet); ok && n.IP.Equal(ip) {
			return true
		}
	}
	return false
}

func (h *hub) isLocalRequest(r *http.Request) bool {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return false
	}
	return thisComputer(net.ParseIP(host))
}

func (h *hub) gppManageAllowed(r *http.Request) error {
	if !h.isLocalRequest(r) {
		return errors.New("Connect Get Producers Paid on the computer the hub runs on: open http://localhost:" + fmt.Sprint(h.port) + " there.")
	}
	if r.Header.Get("X-GPP-Link") != "1" {
		return errors.New("refused")
	}
	if o := r.Header.Get("Origin"); o != "" {
		u, err := url.Parse(o)
		if err != nil || u.Scheme != "http" || u.Port() != fmt.Sprint(h.port) {
			return errors.New("refused")
		}
		if u.Hostname() != "localhost" && !thisComputer(net.ParseIP(u.Hostname())) {
			return errors.New("refused")
		}
	}
	return nil
}

func (h *hub) gppStatus(w http.ResponseWriter, r *http.Request) {
	if h.gpp == nil {
		writeJSON(w, GPPStatus{State: "off", Message: "Get Producers Paid isn't set up on this hub."})
		return
	}
	s := h.gpp.Status()
	s.CanManage = h.isLocalRequest(r)
	if !s.CanManage {
		s.KeptIn = ""
	}
	writeJSON(w, s)
}

func (h *hub) gppPair(w http.ResponseWriter, r *http.Request) {
	if h.gpp == nil {
		httpErr(w, 503, errors.New("Get Producers Paid isn't set up on this hub"))
		return
	}
	if err := h.gppManageAllowed(r); err != nil {
		httpErr(w, 403, err)
		return
	}
	var body struct {
		Code string `json:"code"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&body); err != nil {
		httpErr(w, 400, errors.New("send {\"code\": \"K7Q4-MZP2\"}"))
		return
	}
	msg, err := h.gpp.Pair(body.Code)
	if err != nil {
		code := 400
		var ge *gppError
		if errors.As(err, &ge) && ge.Status >= 500 {
			code = 502
		}
		httpErr(w, code, err)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "message": msg})
}

func (h *hub) gppDisconnect(w http.ResponseWriter, r *http.Request) {
	if h.gpp == nil {
		httpErr(w, 503, errors.New("Get Producers Paid isn't set up on this hub"))
		return
	}
	if err := h.gppManageAllowed(r); err != nil {
		httpErr(w, 403, err)
		return
	}
	if err := h.gpp.Disconnect(); err != nil {
		httpErr(w, 500, err)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}
