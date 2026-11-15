package main

// Preview audio for the plugin's cassette player: any shared audio file,
// returned as plain 16-bit stereo 44.1 kHz WAV, so the MPC never needs an
// MP3/AIFF/FLAC decoder. WAV is converted here; other formats go through
// macOS's built-in afconvert first.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

const (
	previewRate   = 44100
	previewMaxSec = 10 * 60 // longer files are cut to 10 minutes
)

var audioExt = map[string]bool{".wav": true, ".aif": true, ".aiff": true, ".mp3": true, ".flac": true, ".ogg": true, ".m4a": true}

func (h *hub) preview(w http.ResponseWriter, r *http.Request) {
	h.touch(r)
	p, err := h.safePath(r.URL.Query().Get("path"))
	if err != nil {
		httpErr(w, 400, err)
		return
	}
	if !audioExt[strings.ToLower(filepath.Ext(p))] {
		httpErr(w, 415, errors.New("not an audio file"))
		return
	}
	data, err := previewPCM(p)
	if err != nil {
		httpErr(w, 415, err)
		return
	}
	w.Header().Set("Content-Type", "audio/wav")
	w.Header().Set("Content-Length", fmt.Sprint(44+len(data)))
	w.Write(wavHeader(len(data)))
	w.Write(data)
}

// previewPCM returns interleaved little-endian int16 stereo at 44.1 kHz.
func previewPCM(path string) ([]byte, error) {
	if strings.EqualFold(filepath.Ext(path), ".wav") {
		if b, err := os.ReadFile(path); err == nil {
			if pcm, err := wavToPCM(b); err == nil {
				return pcm, nil
			}
		}
	}
	// Not a WAV we can read: let macOS convert it.
	conv, err := exec.LookPath("afconvert")
	if err != nil {
		return nil, errors.New("this file type can only be previewed on a Mac")
	}
	tmp, err := os.CreateTemp("", "mpclink-preview-*.wav")
	if err != nil {
		return nil, err
	}
	tmp.Close()
	defer os.Remove(tmp.Name())
	cmd := exec.Command(conv, "-f", "WAVE", "-d", fmt.Sprintf("LEI16@%d", previewRate), "-c", "2", path, tmp.Name())
	if out, err := cmd.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("could not convert for preview: %s", strings.TrimSpace(string(out)))
	}
	b, err := os.ReadFile(tmp.Name())
	if err != nil {
		return nil, err
	}
	return wavToPCM(b)
}

// wavToPCM reads PCM (8/16/24/32-bit) or float (32/64-bit) WAV, any rate and
// channel count, and returns 16-bit stereo at 44.1 kHz.
func wavToPCM(b []byte) ([]byte, error) {
	if len(b) < 12 || string(b[0:4]) != "RIFF" || string(b[8:12]) != "WAVE" {
		return nil, errors.New("not a WAV file")
	}
	var format, channels, bits uint16
	var rate uint32
	var data []byte
	for off := 12; off+8 <= len(b); {
		id := string(b[off : off+4])
		size := int(binary.LittleEndian.Uint32(b[off+4 : off+8]))
		body := off + 8
		end := body + size
		if end > len(b) {
			end = len(b) // tolerate a truncated last chunk
		}
		switch id {
		case "fmt ":
			if end-body < 16 {
				return nil, errors.New("bad WAV format chunk")
			}
			format = binary.LittleEndian.Uint16(b[body:])
			channels = binary.LittleEndian.Uint16(b[body+2:])
			rate = binary.LittleEndian.Uint32(b[body+4:])
			bits = binary.LittleEndian.Uint16(b[body+14:])
			if format == 0xFFFE && end-body >= 26 { // WAVE_FORMAT_EXTENSIBLE: real format is in the sub-format GUID
				format = binary.LittleEndian.Uint16(b[body+24:])
			}
		case "data":
			data = b[body:end]
		}
		off = body + size + size%2
	}
	if channels == 0 || rate == 0 || data == nil {
		return nil, errors.New("WAV has no audio")
	}
	bps := int(bits) / 8
	if bps == 0 || !(format == 1 && bps >= 1 && bps <= 4) && !(format == 3 && (bps == 4 || bps == 8)) {
		return nil, fmt.Errorf("unsupported WAV encoding (format %d, %d-bit)", format, bits)
	}
	frameBytes := bps * int(channels)
	frames := len(data) / frameBytes
	if max := int(rate) * previewMaxSec; frames > max {
		frames = max
	}
	sample := func(i, ch int) float64 {
		o := i*frameBytes + ch*bps
		s := data[o : o+bps]
		if format == 3 {
			if bps == 4 {
				return float64(math.Float32frombits(binary.LittleEndian.Uint32(s)))
			}
			return math.Float64frombits(binary.LittleEndian.Uint64(s))
		}
		switch bps {
		case 1:
			return (float64(s[0]) - 128) / 128
		case 2:
			return float64(int16(binary.LittleEndian.Uint16(s))) / 32768
		case 3:
			v := int32(uint32(s[0])<<8|uint32(s[1])<<16|uint32(s[2])<<24) >> 8
			return float64(v) / 8388608
		default:
			return float64(int32(binary.LittleEndian.Uint32(s))) / 2147483648
		}
	}
	right := 0
	if channels > 1 {
		right = 1
	}
	outFrames := int(int64(frames) * previewRate / int64(rate))
	var buf bytes.Buffer
	buf.Grow(outFrames * 4)
	step := float64(rate) / previewRate
	put := func(v float64) {
		if v > 1 {
			v = 1
		} else if v < -1 {
			v = -1
		}
		var t [2]byte
		binary.LittleEndian.PutUint16(t[:], uint16(int16(math.Round(v*32767))))
		buf.Write(t[:])
	}
	for i := 0; i < outFrames; i++ {
		pos := float64(i) * step // linear interpolation between source frames
		a := int(pos)
		f := pos - float64(a)
		b2 := a + 1
		if b2 >= frames {
			b2 = frames - 1
		}
		if a >= frames {
			a = frames - 1
		}
		put(sample(a, 0)*(1-f) + sample(b2, 0)*f)
		put(sample(a, right)*(1-f) + sample(b2, right)*f)
	}
	return buf.Bytes(), nil
}

func wavHeader(dataLen int) []byte {
	h := make([]byte, 44)
	copy(h[0:], "RIFF")
	binary.LittleEndian.PutUint32(h[4:], uint32(36+dataLen))
	copy(h[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(h[16:], 16)
	binary.LittleEndian.PutUint16(h[20:], 1)
	binary.LittleEndian.PutUint16(h[22:], 2)
	binary.LittleEndian.PutUint32(h[24:], previewRate)
	binary.LittleEndian.PutUint32(h[28:], previewRate*4)
	binary.LittleEndian.PutUint16(h[32:], 4)
	binary.LittleEndian.PutUint16(h[34:], 16)
	copy(h[36:], "data")
	binary.LittleEndian.PutUint32(h[40:], uint32(dataLen))
	return h
}
