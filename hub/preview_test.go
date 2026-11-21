package main

import (
	"bytes"
	"encoding/binary"
	"io"
	"math"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func makeWav(format, channels uint16, rate uint32, bits uint16, frames int, val func(i, ch int) float64) []byte {
	bps := int(bits) / 8
	data := make([]byte, frames*int(channels)*bps)
	for i := 0; i < frames; i++ {
		for ch := 0; ch < int(channels); ch++ {
			o := (i*int(channels) + ch) * bps
			v := val(i, ch)
			switch {
			case format == 3:
				binary.LittleEndian.PutUint32(data[o:], math.Float32bits(float32(v)))
			case bps == 2:
				binary.LittleEndian.PutUint16(data[o:], uint16(int16(v*32767)))
			case bps == 3:
				x := int32(v * 8388607)
				data[o], data[o+1], data[o+2] = byte(x), byte(x>>8), byte(x>>16)
			}
		}
	}
	h := wavHeader(len(data))
	binary.LittleEndian.PutUint16(h[20:], format)
	binary.LittleEndian.PutUint16(h[22:], channels)
	binary.LittleEndian.PutUint32(h[24:], rate)
	binary.LittleEndian.PutUint16(h[34:], bits)
	return append(h, data...)
}

func TestWavToPCM(t *testing.T) {
	cases := []struct {
		name             string
		format, channels uint16
		rate             uint32
		bits             uint16
	}{
		{"16-bit stereo 44.1k", 1, 2, 44100, 16},
		{"24-bit stereo 48k", 1, 2, 48000, 24},
		{"float mono 96k", 3, 1, 96000, 32},
	}
	for _, c := range cases {
		frames := int(c.rate) // one second
		w := makeWav(c.format, c.channels, c.rate, c.bits, frames, func(i, ch int) float64 {
			return 0.5 * math.Sin(2*math.Pi*441*float64(i)/float64(c.rate))
		})
		pcm, err := wavToPCM(w)
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if got := len(pcm) / 4; got < 44000 || got > 44200 {
			t.Errorf("%s: %d output frames, want about 44100", c.name, got)
		}
		peak := 0
		for i := 0; i+1 < len(pcm); i += 2 {
			v := int(int16(binary.LittleEndian.Uint16(pcm[i:])))
			if v < 0 {
				v = -v
			}
			if v > peak {
				peak = v
			}
		}
		if peak < 15000 || peak > 17500 {
			t.Errorf("%s: peak %d, want about 16384 (half scale)", c.name, peak)
		}
	}
	if _, err := wavToPCM([]byte("not a wav at all")); err == nil {
		t.Error("garbage accepted")
	}
}

func TestPreviewWithFFmpeg(t *testing.T) {
	ff := findFFmpeg()
	if ff == "" {
		t.Skip("ffmpeg not installed")
	}
	dir := t.TempDir()
	src := filepath.Join(dir, "beat.wav")
	if err := os.WriteFile(src, makeWav(1, 2, 48000, 16, 48000, func(i, ch int) float64 { return 0.3 * math.Sin(float64(i)/7) }), 0o644); err != nil {
		t.Fatal(err)
	}
	mp3 := filepath.Join(dir, "beat.mp3")
	if out, err := exec.Command(ff, "-v", "error", "-y", "-i", src, mp3).CombinedOutput(); err != nil {
		t.Skipf("ffmpeg can't make an mp3 here: %s", out)
	}
	if _, err := exec.LookPath("afconvert"); err == nil {
		t.Skip("afconvert is used instead on this machine")
	}
	pcm, err := previewPCM(mp3)
	if err != nil {
		t.Fatal(err)
	}
	if got := len(pcm) / 4; got < 43000 || got > 46000 {
		t.Errorf("%d frames from a 1 s mp3, want about 44100", got)
	}
}

// The cassette can play a file the MPC received: it sends the file, gets WAV back.
func TestPreviewFile(t *testing.T) {
	h := &hub{root: t.TempDir(), devices: map[string]time.Time{}}
	srv := httptest.NewServer(http.HandlerFunc(h.previewFile))
	defer srv.Close()
	wav := makeWav(1, 1, 22050, 16, 22050, func(i, ch int) float64 { return 0.5 })
	put := func(name string, body []byte, dev string) (*http.Response, []byte) {
		req, _ := http.NewRequest("PUT", srv.URL+"/api/preview-file?name="+url.QueryEscape(name), bytes.NewReader(body))
		if dev != "" {
			req.Header.Set("X-MPC-Device", dev)
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		return resp, b
	}
	resp, b := put("Received Beat.wav", wav, "Live III")
	if resp.StatusCode != 200 || len(b) != 44+44100*4 || string(b[:4]) != "RIFF" {
		t.Fatalf("preview: %d, %d bytes", resp.StatusCode, len(b))
	}
	if resp, _ := put("notes.txt", []byte("x"), "Live III"); resp.StatusCode != 415 {
		t.Fatalf("text file: %d", resp.StatusCode)
	}
	if resp, _ := put("a.wav", wav, ""); resp.StatusCode != 400 {
		t.Fatalf("no device: %d", resp.StatusCode)
	}
}
