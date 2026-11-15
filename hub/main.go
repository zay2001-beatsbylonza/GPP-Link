// MPC Link hub: shares one folder on your Mac with the MPCs on your Wi-Fi.
//
// MPCs send files into   <Beats>/From MPC/<MPC name>/
// MPCs pull files out of the whole Beats folder (audio, MIDI and MPC files only).
// A phone or computer can open http://<mac>:47800 to queue files for a given MPC.
//
// Standard library only. Build: go build -o mpc-link-hub .
package main

import (
	"embed"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	version       = "0.1.0"
	defaultPort   = 47800
	discoveryPort = 47801
	discoverMsg   = "MPCLINK-DISCOVER 1"
	maxUpload     = 4 << 30 // 4 GB
	inboxDirName  = "From MPC"
)

//go:embed web/index.html
var webFS embed.FS

// Files an MPC may send or receive. Everything else in the folder is invisible to it.
var allowedExt = map[string]bool{
	".wav": true, ".aif": true, ".aiff": true, ".mp3": true, ".flac": true,
	".ogg": true, ".m4a": true, ".mid": true, ".midi": true,
	".xpj": true, ".xpm": true, ".xal": true, ".xpn": true, ".xmd": true,
	".xfx": true, ".xpl": true, ".xpb": true,
}

var deviceNameRe = regexp.MustCompile(`^[A-Za-z0-9 _.\-]{1,40}$`)

type hub struct {
	root string
	name string
	port int

	mu      sync.Mutex
	devices map[string]time.Time // MPC name -> last seen
	queue   map[string][]string  // MPC name -> paths waiting for it
}

type entry struct {
	Path  string `json:"path"`
	Name  string `json:"name"`
	Size  int64  `json:"size"`
	Mtime int64  `json:"mtime"`
}

func main() {
	home, _ := os.UserHomeDir()
	defDir := filepath.Join(home, "Music", "Beats")
	dir := flag.String("dir", defDir, "the Beats folder to share")
	port := flag.Int("port", defaultPort, "HTTP port")
	name := flag.String("name", "", "name shown on the MPC (default: this computer's name)")
	flag.Parse()

	root, err := filepath.Abs(expandHome(*dir, home))
	if err != nil {
		log.Fatal(err)
	}
	if err := os.MkdirAll(root, 0o755); err != nil {
		log.Fatalf("cannot use folder %s: %v", root, err)
	}
	if r, err := filepath.EvalSymlinks(root); err == nil {
		root = r
	}
	hubName := *name
	if hubName == "" {
		hubName, _ = os.Hostname()
		hubName = strings.TrimSuffix(hubName, ".local")
	}

	h := &hub{root: root, name: hubName, port: *port,
		devices: map[string]time.Time{}, queue: map[string][]string{}}

	go h.discovery()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /", h.index)
	mux.HandleFunc("GET /api/hello", h.hello)
	mux.HandleFunc("GET /api/list", h.list)
	mux.HandleFunc("GET /api/file", h.download)
	mux.HandleFunc("PUT /api/upload", h.upload)
	mux.HandleFunc("GET /api/devices", h.listDevices)
	mux.HandleFunc("GET /api/inbox", h.inbox)
	mux.HandleFunc("POST /api/inbox/ack", h.ack)
	mux.HandleFunc("POST /api/queue", h.enqueue)

	srv := &http.Server{
		Addr:              fmt.Sprintf(":%d", *port),
		Handler:           logRequests(mux),
		ReadHeaderTimeout: 10 * time.Second,
	}
	fmt.Printf("MPC Link hub %s\n", version)
	fmt.Printf("Sharing:   %s\n", root)
	fmt.Printf("Hub name:  %s\n", hubName)
	for _, a := range lanAddrs() {
		fmt.Printf("Open:      http://%s:%d   (phone or computer on the same Wi-Fi)\n", a, *port)
	}
	fmt.Println("Leave this window open while you use the MPCs. Press Ctrl+C to stop.")
	log.Fatal(srv.ListenAndServe())
}

// ---------- discovery: MPCs broadcast a UDP hello, the hub answers ----------

func (h *hub) discovery() {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{Port: discoveryPort})
	if err != nil {
		log.Printf("discovery off (%v); MPCs will need this computer's IP address", err)
		return
	}
	buf := make([]byte, 512)
	for {
		n, from, err := conn.ReadFromUDP(buf)
		if err != nil {
			continue
		}
		if strings.TrimSpace(string(buf[:n])) != discoverMsg {
			continue
		}
		reply := fmt.Sprintf("MPCLINK-HUB 1 %d %s", h.port, h.name)
		conn.WriteToUDP([]byte(reply), from)
	}
}

// ---------- handlers ----------

func (h *hub) index(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	b, _ := webFS.ReadFile("web/index.html")
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(b)
}

func (h *hub) hello(w http.ResponseWriter, r *http.Request) {
	h.touch(r)
	writeJSON(w, map[string]any{"app": "mpc-link", "version": version, "name": h.name, "folder": filepath.Base(h.root)})
}

// list returns every allowed file under the Beats folder (or under ?dir=).
// ?format=tsv gives "size<TAB>mtime<TAB>path" lines, easy to parse on the MPC.
func (h *hub) list(w http.ResponseWriter, r *http.Request) {
	h.touch(r)
	start := h.root
	if d := r.URL.Query().Get("dir"); d != "" {
		p, err := h.safePath(d)
		if err != nil {
			httpErr(w, 400, err)
			return
		}
		start = p
	}
	var out []entry
	filepath.WalkDir(start, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if strings.HasPrefix(d.Name(), ".") && p != start {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() || d.Type()&fs.ModeSymlink != 0 || !allowedExt[strings.ToLower(filepath.Ext(p))] {
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return nil
		}
		rel, _ := filepath.Rel(h.root, p)
		out = append(out, entry{Path: filepath.ToSlash(rel), Name: d.Name(), Size: info.Size(), Mtime: info.ModTime().Unix()})
		return nil
	})
	sort.Slice(out, func(i, j int) bool { return out[i].Mtime > out[j].Mtime }) // newest first
	if r.URL.Query().Get("format") == "tsv" {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		for _, e := range out {
			fmt.Fprintf(w, "%d\t%d\t%s\n", e.Size, e.Mtime, e.Path)
		}
		return
	}
	writeJSON(w, out)
}

func (h *hub) download(w http.ResponseWriter, r *http.Request) {
	h.touch(r)
	p, err := h.safePath(r.URL.Query().Get("path"))
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	if !allowedExt[strings.ToLower(filepath.Ext(p))] {
		httpErr(w, 403, errors.New("file type not shared"))
		return
	}
	f, err := os.Open(p)
	if err != nil {
		httpErr(w, 404, errors.New("not found"))
		return
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil || info.IsDir() {
		httpErr(w, 404, errors.New("not found"))
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", filepath.Base(p)))
	http.ServeContent(w, r, filepath.Base(p), info.ModTime(), f)
}

// upload stores a file from an MPC in <Beats>/From MPC/<device>/<path>.
// ?path= may contain folders (for an MPC project's [ProjectData] folder).
// An existing file is never overwritten: "kick.wav" becomes "kick (2).wav".
func (h *hub) upload(w http.ResponseWriter, r *http.Request) {
	dev, ok := h.touch(r)
	if !ok {
		httpErr(w, 400, errors.New("send the MPC's name in the X-MPC-Device header"))
		return
	}
	rel := r.URL.Query().Get("path")
	if !allowedExt[strings.ToLower(path.Ext(rel))] {
		httpErr(w, 403, errors.New("file type not accepted"))
		return
	}
	// check the MPC's path on its own first, so path.Join below can't fold
	// a ".." out of this MPC's folder
	if _, err := h.validRel(rel); err != nil {
		httpErr(w, 400, err)
		return
	}
	target, err := h.safePath(path.Join(inboxDirName, dev, rel))
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		httpErr(w, 500, err)
		return
	}
	// re-check after creating folders, in case one of them was a symlink
	if _, err := h.safePath(path.Join(inboxDirName, dev, rel)); err != nil {
		httpErr(w, 400, err)
		return
	}
	tmp, err := os.CreateTemp(filepath.Dir(target), ".mpclink-*.part")
	if err != nil {
		httpErr(w, 500, err)
		return
	}
	n, err := io.Copy(tmp, http.MaxBytesReader(w, r.Body, maxUpload))
	tmp.Close()
	if err != nil {
		os.Remove(tmp.Name())
		httpErr(w, 400, fmt.Errorf("upload interrupted: %v", err))
		return
	}
	if r.ContentLength >= 0 && n != r.ContentLength {
		os.Remove(tmp.Name())
		httpErr(w, 400, errors.New("upload incomplete"))
		return
	}
	final := freeName(target)
	if err := os.Rename(tmp.Name(), final); err != nil {
		os.Remove(tmp.Name())
		httpErr(w, 500, err)
		return
	}
	saved, _ := filepath.Rel(h.root, final)
	log.Printf("received %s from %s (%d bytes)", saved, dev, n)
	writeJSON(w, map[string]any{"saved": filepath.ToSlash(saved), "size": n})
}

func (h *hub) listDevices(w http.ResponseWriter, r *http.Request) {
	h.mu.Lock()
	defer h.mu.Unlock()
	type dv struct {
		Name     string `json:"name"`
		LastSeen int64  `json:"lastSeen"`
		Waiting  int    `json:"waiting"`
	}
	out := []dv{}
	for n, t := range h.devices {
		out = append(out, dv{n, t.Unix(), len(h.queue[n])})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	writeJSON(w, out)
}

// inbox: files queued for this MPC (from the phone page). One path per line.
func (h *hub) inbox(w http.ResponseWriter, r *http.Request) {
	dev, ok := h.touch(r)
	if !ok {
		httpErr(w, 400, errors.New("send the MPC's name in the X-MPC-Device header"))
		return
	}
	h.mu.Lock()
	q := append([]string(nil), h.queue[dev]...)
	h.mu.Unlock()
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	for _, p := range q {
		fmt.Fprintln(w, p)
	}
}

// ack: the MPC got a queued file; take it off its list.
func (h *hub) ack(w http.ResponseWriter, r *http.Request) {
	dev, ok := h.touch(r)
	if !ok {
		httpErr(w, 400, errors.New("send the MPC's name in the X-MPC-Device header"))
		return
	}
	p := r.URL.Query().Get("path")
	h.mu.Lock()
	q := h.queue[dev]
	for i, v := range q {
		if v == p {
			h.queue[dev] = append(q[:i:i], q[i+1:]...)
			break
		}
	}
	h.mu.Unlock()
	w.WriteHeader(204)
}

// enqueue: the phone page asks for a file to go to one MPC.
func (h *hub) enqueue(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Device string `json:"device"`
		Path   string `json:"path"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 8192)).Decode(&req); err != nil {
		httpErr(w, 400, err)
		return
	}
	if !deviceNameRe.MatchString(req.Device) {
		httpErr(w, 400, errors.New("unknown MPC"))
		return
	}
	p, err := h.safePath(req.Path)
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	if info, err := os.Stat(p); err != nil || info.IsDir() || !allowedExt[strings.ToLower(filepath.Ext(p))] {
		httpErr(w, 404, errors.New("not found"))
		return
	}
	rel, _ := filepath.Rel(h.root, p)
	rel = filepath.ToSlash(rel)
	h.mu.Lock()
	defer h.mu.Unlock()
	if _, known := h.devices[req.Device]; !known {
		httpErr(w, 404, errors.New("that MPC hasn't connected yet"))
		return
	}
	for _, v := range h.queue[req.Device] {
		if v == rel {
			w.WriteHeader(204)
			return
		}
	}
	h.queue[req.Device] = append(h.queue[req.Device], rel)
	w.WriteHeader(204)
}

// ---------- helpers ----------

// touch records which MPC is talking to us (X-MPC-Device header).
func (h *hub) touch(r *http.Request) (string, bool) {
	dev := strings.TrimSpace(r.Header.Get("X-MPC-Device"))
	if dev == "" || !deviceNameRe.MatchString(dev) || strings.Trim(dev, ". ") == "" {
		return "", false
	}
	h.mu.Lock()
	h.devices[dev] = time.Now()
	h.mu.Unlock()
	return dev, true
}

var errBadPath = errors.New("bad path")

// safePath turns a client path into a real path that is guaranteed to sit
// inside the Beats folder: no "..", no absolute paths, no hidden names, and
// no symlink that leads outside.
func (h *hub) validRel(rel string) (string, error) {
	rel = strings.ReplaceAll(rel, "\\", "/")
	if rel == "" || strings.HasPrefix(rel, "/") || strings.ContainsRune(rel, 0) {
		return "", errBadPath
	}
	for _, seg := range strings.Split(rel, "/") {
		if seg == "" || seg == "." || seg == ".." || strings.HasPrefix(seg, ".") {
			return "", errBadPath
		}
	}
	return rel, nil
}

func (h *hub) safePath(rel string) (string, error) {
	rel, err := h.validRel(rel)
	if err != nil {
		return "", err
	}
	full := filepath.Join(h.root, filepath.FromSlash(rel))
	// resolve the deepest part that exists, then make sure it's still inside root
	check := full
	for {
		if r, err := filepath.EvalSymlinks(check); err == nil {
			if r != h.root && !strings.HasPrefix(r, h.root+string(os.PathSeparator)) {
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

func freeName(p string) string {
	if _, err := os.Lstat(p); errors.Is(err, os.ErrNotExist) {
		return p
	}
	ext := filepath.Ext(p)
	base := strings.TrimSuffix(p, ext)
	for i := 2; ; i++ {
		c := base + " (" + strconv.Itoa(i) + ")" + ext
		if _, err := os.Lstat(c); errors.Is(err, os.ErrNotExist) {
			return c
		}
	}
}

func expandHome(p, home string) string {
	if p == "~" {
		return home
	}
	if strings.HasPrefix(p, "~/") {
		return filepath.Join(home, p[2:])
	}
	return p
}

func lanAddrs() []string {
	var out []string
	ifs, _ := net.InterfaceAddrs()
	for _, a := range ifs {
		if ipn, ok := a.(*net.IPNet); ok && ipn.IP.To4() != nil && !ipn.IP.IsLoopback() {
			out = append(out, ipn.IP.String())
		}
	}
	if len(out) == 0 {
		out = []string{"localhost"}
	}
	return out
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func httpErr(w http.ResponseWriter, code int, err error) {
	http.Error(w, err.Error(), code)
}

func logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/inbox" && r.URL.Path != "/api/devices" { // these poll often
			log.Printf("%s %s %s", r.RemoteAddr, r.Method, r.URL.RequestURI())
		}
		next.ServeHTTP(w, r)
	})
}
