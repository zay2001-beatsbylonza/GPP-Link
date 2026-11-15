package main

// Stems for Get Producers Paid. Stems live in the Beats folder on the Mac: any
// folder holding two or more audio files counts as a stems folder. The MPC picks
// one and the hub copies it into <Beats>/Get Producers Paid/Stems/<folder name>/.
// The files never pass through the MPC.

import (
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type stemFolder struct {
	Path  string `json:"path"`
	Name  string `json:"name"`
	Files int    `json:"files"`
	Mtime int64  `json:"mtime"`
}

// stemFolders lists folders under the Beats folder with 2+ audio files directly
// inside, newest first. The Get Producers Paid and From MPC folders are skipped.
func (h *hub) stemFolders() []stemFolder {
	byDir := map[string]*stemFolder{}
	skip := map[string]bool{
		filepath.Join(h.root, gppDirName):   true,
		filepath.Join(h.root, inboxDirName): true,
	}
	filepath.WalkDir(h.root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			if p != h.root && (strings.HasPrefix(d.Name(), ".") || skip[p]) {
				return filepath.SkipDir
			}
			return nil
		}
		if d.Type()&fs.ModeSymlink != 0 || strings.HasPrefix(d.Name(), ".") || !audioExt[strings.ToLower(filepath.Ext(p))] {
			return nil
		}
		dir := filepath.Dir(p)
		if dir == h.root {
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return nil
		}
		f := byDir[dir]
		if f == nil {
			rel, _ := filepath.Rel(h.root, dir)
			f = &stemFolder{Path: filepath.ToSlash(rel), Name: filepath.Base(dir)}
			byDir[dir] = f
		}
		f.Files++
		if t := info.ModTime().Unix(); t > f.Mtime {
			f.Mtime = t
		}
		return nil
	})
	var out []stemFolder
	for _, f := range byDir {
		if f.Files >= 2 {
			out = append(out, *f)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Mtime > out[j].Mtime })
	return out
}

// GET /api/stems?format=tsv -> "files<TAB>mtime<TAB>path" per stems folder.
func (h *hub) listStems(w http.ResponseWriter, r *http.Request) {
	h.touch(r)
	folders := h.stemFolders()
	if r.URL.Query().Get("format") == "tsv" {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		for _, f := range folders {
			fmt.Fprintf(w, "%d\t%d\t%s\n", f.Files, f.Mtime, f.Path)
		}
		return
	}
	if folders == nil {
		folders = []stemFolder{}
	}
	writeJSON(w, folders)
}

// POST /api/stems/send?path=<folder> copies its audio files to Get Producers Paid/Stems/<name>/.
func (h *hub) sendStems(w http.ResponseWriter, r *http.Request) {
	dev, _ := h.touch(r)
	src, err := h.safePath(r.URL.Query().Get("path"))
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	if info, err := os.Stat(src); err != nil || !info.IsDir() {
		httpErr(w, 404, errors.New("stems folder not found"))
		return
	}
	stemsRoot := filepath.Join(h.root, gppDirName, "Stems")
	if rel, err := filepath.Rel(filepath.Join(h.root, gppDirName), src); err == nil && !strings.HasPrefix(rel, "..") {
		httpErr(w, 400, errors.New("that folder is already in Get Producers Paid"))
		return
	}
	entries, err := os.ReadDir(src)
	if err != nil {
		httpErr(w, 500, err)
		return
	}
	var files []string
	for _, e := range entries {
		if e.Type().IsRegular() && !strings.HasPrefix(e.Name(), ".") && audioExt[strings.ToLower(filepath.Ext(e.Name()))] {
			files = append(files, e.Name())
		}
	}
	if len(files) < 2 {
		httpErr(w, 400, errors.New("not a stems folder (needs 2 or more audio files)"))
		return
	}
	if err := os.MkdirAll(stemsRoot, 0o755); err != nil {
		httpErr(w, 500, err)
		return
	}
	dest := freeName(filepath.Join(stemsRoot, filepath.Base(src))) // "Song Stems (2)" if sent before
	if err := os.Mkdir(dest, 0o755); err != nil {
		httpErr(w, 500, err)
		return
	}
	for _, name := range files {
		if err := copyFile(filepath.Join(src, name), filepath.Join(dest, name)); err != nil {
			httpErr(w, 500, fmt.Errorf("copying %s: %v", name, err))
			return
		}
	}
	saved, _ := filepath.Rel(h.root, dest)
	log.Printf("stems %s -> %s (%d files) for %s", r.URL.Query().Get("path"), saved, len(files), dev)
	writeJSON(w, map[string]any{"saved": filepath.ToSlash(saved), "files": len(files)})
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	tmp := dst + ".part"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		os.Remove(tmp)
		return err
	}
	if err := out.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, dst)
}
