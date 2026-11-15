// MPC Link - move beats between this MPC, the Beats folder on your Mac, and
// the Get Producers Paid app. By BeatsbyLonza. Built on Akai's MPC Plugin SDK (MIT).
//
// The screen:
//   A cassette deck on top: PLAY plays the beat picked in "Computer Files" through
//   this track (a preview, nothing is saved), the reels turn, and the beat's
//   blue waveform scrolls right to left past a red playhead, like tape moving:
//   bright blue behind the playhead (played), darker blue ahead. Pressing the
//   same button again stops and rewinds to the front; the end rewinds too.
//   Four blocks under it, each a picker (turn a Q-Link) over a big button:
//   SEND TO MPC        pick a beat on the computer ("Computer Files"); it lands in
//                      "<drive>/From Computer/" on this MPC.
//   SEND TO FOLDER     pick a file from "<drive>/To Computer/" ("From MPC"); it
//                      lands in "Beats/From MPC/<this MPC>/" on the computer.
//   SEND TO GET PRODUCERS PAID  the same "From MPC" pick; it lands in
//                      "Beats/Get Producers Paid/" on the computer, waiting to go
//                      to the Get Producers Paid app as a draft.
//   SEND STEMS TO GET PRODUCERS PAID  pick a stems folder in the Beats folder
//                      ("Computer Stems"); the computer copies it to
//                      "Beats/Get Producers Paid/Stems/".
// Nothing leaves the MPC unless a button is pressed. Files sent from the phone
// page arrive in "From Computer" by themselves, a beat cued on the phone page
// becomes the "Computer Files" pick, and the lists refresh on their own.
//
// Audio passes straight through (plus the cassette preview when playing), so the
// plugin can sit on any track. Network and disk work happen on a background
// thread; the audio thread only reads a ready-decoded buffer.
// It finds the MPC Link hub by UDP broadcast, or reads the Mac's address from
// "<drive>/MPC Link hub.txt" when broadcasts are blocked.
// <drive> is the drive this plugin is installed on (the one holding
// "VST2 Plugins"), so it works on every MPC model without guessing paths.
//
// Parameter order is FROZEN once released (saved projects store it).
// Append new parameters at the end only.

#include "mpc_plugin.h"
#include "mpc_resources.h"
#include "../../plugin-core/mpclink.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

enum ParamIndex
{
    kStatus = 0, // readout: connection and last action. Param 0 is nudged by the host on load:
                 // harmless here (the value is only a change counter; the text is the display).
    kMacBeat,    // index into the Mac's file list
    kMpcBeat,    // index into "To Computer" (for SEND TO FOLDER and SEND TO GET PRODUCERS PAID)
    kToMpc,      // button: SEND TO MPC
    kToFolder,   // button: SEND TO FOLDER
    kToGpp,      // button: SEND TO GPP
    kStems,      // index into the Beats folder's stems folders
    kToStems,    // button: SEND STEMS TO GPP
    kPlay,       // cassette PLAY / STOP
    kReel,       // cassette reels: animation frame (screen only)
    kWave0,      // first of kWaveBars waveform bars (screen only)
    kNumParams = kWave0 + 32
};

constexpr int kWaveBars   = 32;
constexpr int kPlayhead   = 16;      // bars left of this one have played
constexpr int kSliceFrames = 11025;  // each bar is a quarter second of the beat
constexpr int kWaveLevels = 17;   // bar heights 0..16; +17 = the "played" colour
constexpr int kReelFrames = 12;
constexpr size_t kMaxPreviewBytes = 128u << 20;   // ~12 minutes of 44.1 kHz stereo

constexpr int kMaxFiles = 999;   // the pickers' range

using Clock = std::chrono::steady_clock;

bool allowedExt(const std::string& name)
{
    static const char* const exts[] = {
        ".wav", ".aif", ".aiff", ".mp3", ".flac", ".ogg", ".m4a", ".mid", ".midi",
        ".xpj", ".xpm", ".xal", ".xpn", ".xmd", ".xfx", ".xpl", ".xpb" };
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string e = name.substr(dot);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    for (const char* x : exts) if (e == x) return true;
    return false;
}

std::string baseName(const std::string& p)
{
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string parentOf(const std::string& p)
{
    const size_t s = p.rfind('/');
    return (s == std::string::npos || s == 0) ? std::string() : p.substr(0, s);
}

// The MPC shows about 30 characters: keep the start and the extension.
std::string fitText(const std::string& s, size_t max = 30)
{
    if (s.size() <= max) return s;
    const size_t dot = s.rfind('.');
    const size_t tail = (dot != std::string::npos && s.size() - dot <= 6) ? s.size() - dot : 0;
    return s.substr(0, max - 3 - tail) + "..." + s.substr(s.size() - tail);
}

bool makeDirs(const std::string& dir)
{
    std::string cur;
    size_t i = 0;
    while (i <= dir.size())
    {
        const size_t j = dir.find('/', i);
        cur = dir.substr(0, j == std::string::npos ? dir.size() : j);
        if (!cur.empty()) mkdir(cur.c_str(), 0755);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    struct stat st;
    return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct LocalFile { std::string rel; long long mtime; };

// Every sendable file under root (skipping hidden names and the Sent folder), newest first.
void scanLocal(const std::string& root, const std::string& rel, std::vector<LocalFile>& out, int depth = 0)
{
    if (depth > 6 || (int)out.size() > 5000) return;
    const std::string dirPath = rel.empty() ? root : root + "/" + rel;
    DIR* d = opendir(dirPath.c_str());
    if (!d) return;
    while (dirent* e = readdir(d))
    {
        const std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        if (depth == 0 && n == "Sent") continue;
        const std::string childRel = rel.empty() ? n : rel + "/" + n;
        struct stat st;
        if (lstat((root + "/" + childRel).c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scanLocal(root, childRel, out, depth + 1);
        else if (S_ISREG(st.st_mode) && allowedExt(n)) out.push_back({ childRel, (long long)st.st_mtime });
    }
    closedir(d);
    if (depth == 0)
        std::sort(out.begin(), out.end(), [](const LocalFile& a, const LocalFile& b) { return a.mtime > b.mtime; });
}

// This MPC's name for the Mac: its hostname, cleaned to what the hub accepts.
std::string deviceName()
{
    char host[128] = {0};
    gethostname(host, sizeof host - 1);
    std::string out;
    for (const char* p = host; *p && out.size() < 40; ++p)
    {
        const char c = *p;
        if (std::isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '.' || c == '-') out += c;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    return out.empty() ? std::string("MPC") : out;
}

std::string readFirstLine(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return std::string();
    char buf[128] = {0};
    if (!std::fgets(buf, sizeof buf, f)) buf[0] = 0;
    std::fclose(f);
    std::string s = buf;
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(0, 1);
    return s;
}

// A beat decoded for the cassette: interleaved 16-bit stereo at 44.1 kHz.
struct Tape { std::vector<int16_t> pcm; size_t frames = 0; };

} // namespace

class MPCLink : public mpc::Plugin
{
public:
    MPCLink() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::intParam ("status",   "Status",         0, 1000, 0));
        addParam(mpc::intParam ("macbeat",  "Computer Files", 0, kMaxFiles, 0));
        addParam(mpc::intParam ("mpcbeat",  "From MPC",       0, kMaxFiles, 0));
        addParam(mpc::boolParam("tompc",    "Send to MPC",    false));
        addParam(mpc::boolParam("tofolder", "Send to Folder", false));
        addParam(mpc::boolParam("togpp",    "Send to GPP",    false));
        addParam(mpc::intParam ("stems",    "Computer Stems", 0, kMaxFiles, 0));
        addParam(mpc::boolParam("tostems",  "Send Stems",     false));
        addParam(mpc::boolParam("play",     "Play",           false));
        mpc::Param reel = mpc::intParam("reel", "Reel", 0, kReelFrames - 1, 0);
        reel.automatable = false;
        addParam(reel);
        for (int b = 0; b < kWaveBars; ++b)
        {
            char id[16], name[16];
            std::snprintf(id, sizeof id, "wave%02d", b);
            std::snprintf(name, sizeof name, "Wave %02d", b + 1);
            mpc::Param w = mpc::intParam(id, name, 0, 2 * kWaveLevels - 1, 0);
            w.automatable = false;
            addParam(w);
        }

        status_ = "Starting...";
        // The worker starts in prepare(): by then the host has connected to us,
        // so its notifications reach the MPC screen.
    }

    ~MPCLink() override
    {
        {
            std::lock_guard<std::mutex> lk(wakeMu_);
            stop_ = true;
        }
        ctx_.cancel = 1;   // a transfer in progress stops within one 16 KB chunk
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        playing_ = false;
        delete active_.exchange(nullptr);   // the host has stopped calling process() by now
    }

    void prepare(double sampleRate, int) override
    {
        // the preview is 44.1 kHz: step through it at the right speed for the host's rate
        const double sr = sampleRate > 1000.0 ? sampleRate : 44100.0;
        step_.store((uint64_t)(44100.0 / sr * 65536.0 + 0.5));
        if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
    }
    // Audio passes straight through, plus the cassette preview while it plays.
    // Real-time safe: no locks, no allocation, no I/O; the tape is read-only here.
    void process(mpc::ProcessContext& ctx) override
    {
        const int nCh = mpc::clamp(ctx.numOutputs, 0, 2);
        for (int ch = 0; ch < nCh; ++ch)
        {
            const float* in = (ctx.inputs && ch < ctx.numInputs) ? ctx.inputs[ch] : nullptr;
            float* out = ctx.outputs[ch];
            if (!in) { for (int i = 0; i < ctx.numFrames; ++i) out[i] = 0.0f; }
            else if (in != out) { for (int i = 0; i < ctx.numFrames; ++i) out[i] = in[i]; }
        }
        const Tape* t = active_.load(std::memory_order_acquire);
        if (t && playing_.load(std::memory_order_relaxed) && nCh > 0)
        {
            uint64_t pos = pos_.load(std::memory_order_relaxed);   // 16.16 fixed point, in tape frames
            const uint64_t step = step_.load(std::memory_order_relaxed);
            const uint64_t end = (uint64_t)t->frames << 16;
            const float k = 0.8f / 32768.0f;
            for (int i = 0; i < ctx.numFrames; ++i)
            {
                if (pos >= end) { playing_.store(false); break; }
                const size_t f = (size_t)(pos >> 16);
                ctx.outputs[0][i] += t->pcm[2 * f] * k;
                if (nCh > 1) ctx.outputs[1][i] += t->pcm[2 * f + 1] * k;
                pos += step;
            }
            pos_.store(pos, std::memory_order_relaxed);
        }
        blocks_.fetch_add(1, std::memory_order_release);
    }

    // Buttons only raise a flag; the worker does the work. (May run on any thread: keep it trivial.)
    void parameterChanged(int index, float v) override
    {
        if (selfSet_) return;   // our own screen updates
        if (index == kPlay)   // any tap flips play/stop; a duplicate tap just re-shows the real state
        {
            if (!press(playReq_, playAt_)) playSync_ = true;
            return;
        }
        if (v < 0.5f) return;
        switch (index)
        {
            case kToMpc:    press(toMpcReq_, toMpcAt_); break;
            case kToFolder: press(toFolderReq_, toFolderAt_); break;
            case kToGpp:    press(toGppReq_, toGppAt_); break;
            case kToStems:  press(toStemsReq_, toStemsAt_); break;
            default: break;
        }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        std::lock_guard<std::mutex> lk(textMu_);
        const int i = (int)std::lround(plain);
        switch (index)
        {
            case kStatus: out = fitText(status_); return true;
            case kMacBeat:
                if (macFiles_.empty()) out = connected_ ? "(Beats folder empty)" : "(not connected)";
                else out = fitText(baseName(macFiles_[(size_t)std::min(i, (int)macFiles_.size() - 1)]));
                return true;
            case kMpcBeat:
                if (mpcFiles_.empty()) out = "(To Computer is empty)";
                else out = fitText(baseName(mpcFiles_[(size_t)std::min(i, (int)mpcFiles_.size() - 1)]));
                return true;
            case kStems:
                if (stems_.empty()) out = connected_ ? "(no stems folders)" : "(not connected)";
                else
                {
                    const StemsFolder& f = stems_[(size_t)std::min(i, (int)stems_.size() - 1)];
                    const std::string count = " (" + std::to_string(f.files) + ")";
                    out = fitText(baseName(f.path), 30 - count.size()) + count;
                }
                return true;
            default: return false;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "MPC Link";
        info.vendor       = "BeatsbyLonza";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);
        info.version      = 1000;
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 1;
        return info;
    }
    static constexpr char kUid[5] = "BgQ0";

    // A tap on the MPC can arrive twice ~130 ms apart: one action per 400 ms.
    static bool press(std::atomic<bool>& req, std::atomic<long long>& at)
    {
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count();
        if (now - at.load() < 400) return false;
        at.store(now);
        req.store(true);
        return true;
    }

    // ---------------- worker thread ----------------

    void run()
    {
        // Find the drive this plugin is installed on: <drive>/VST2 Plugins/<this folder>.
        // (computePluginFolder, not pluginFolder(): no function-local static to destroy at unload.)
        const std::string folder = mpc::detail::computePluginFolder();
        std::string drive = parentOf(folder);
        if (baseName(drive) == "VST2 Plugins" || baseName(drive) == "Synths") drive = parentOf(drive);
        if (drive.empty()) drive = "/tmp";
        inboxDir_  = drive + "/From Computer";
        outboxDir_ = drive + "/To Computer";
        hubFile_   = drive + "/MPC Link hub.txt";
        makeDirs(inboxDir_);
        makeDirs(outboxDir_);

        mpclink_init(&ctx_, deviceName().c_str());
        ctx_.timeout_ms = 4000;
        rescanLocal();

        Clock::time_point lastTry{}, lastList{}, lastInbox{}, lastCue{}, lastLocal = Clock::now();
        while (!sleepFor(150))
        {
            const Clock::time_point now = Clock::now();

            if (!connected_)
            {
                if (now - lastTry < std::chrono::seconds(5)) { dropRequests(); continue; }
                lastTry = now;
                connect();
                if (connected_) { refreshMacList(); refreshStems(); lastList = now; }
                continue;
            }

            if (toMpcReq_.exchange(false))    { doGet();             release(kToMpc); }
            if (toFolderReq_.exchange(false)) { doSend(kMpcBeat, false); release(kToFolder); }
            if (toGppReq_.exchange(false))    { doSend(kMpcBeat, true);  release(kToGpp); }
            if (toStemsReq_.exchange(false))  { doSendStems();       release(kToStems); }
            if (playReq_.exchange(false))     togglePlay();
            tickTape();

            if (now - lastCue > std::chrono::seconds(1))     // a beat cued on the phone page
            {
                lastCue = now;
                takeCue();
            }
            if (now - lastInbox > std::chrono::seconds(3))   // files sent from the phone page
            {
                lastInbox = now;
                const int n = mpclink_poll_inbox(&ctx_, inboxDir_.c_str());
                if (n > 0) setActivity("Got " + std::to_string(n) + " from phone");
                else if (n < 0) lost(n);
            }
            if (connected_ && now - lastList > std::chrono::seconds(10)) { refreshMacList(); refreshStems(); lastList = now; }
            if (now - lastLocal > std::chrono::seconds(10)) { rescanLocal(); lastLocal = now; }
        }
    }

    // true when it's time to stop
    bool sleepFor(int ms)
    {
        std::unique_lock<std::mutex> lk(wakeMu_);
        wake_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stop_.load(); });
        return stop_.load();
    }

    void connect()
    {
        setStatus("Looking for Mac...");
        int r = MPCLINK_ERR_NOT_FOUND;
        const std::string manual = readFirstLine(hubFile_);
        if (!manual.empty())
        {
            std::string host = manual;
            int port = MPCLINK_PORT;
            const size_t colon = manual.find(':');
            if (colon != std::string::npos) { host = manual.substr(0, colon); port = std::atoi(manual.c_str() + colon + 1); }
            mpclink_set_hub(&ctx_, host.c_str(), port);
            r = MPCLINK_OK;
        }
        else
        {
            r = mpclink_discover(&ctx_, 1500);
        }
        if (r == MPCLINK_OK) r = mpclink_ping(&ctx_);
        if (r == MPCLINK_OK)
        {
            connected_ = true;
            setStatus(std::string("On ") + (ctx_.hub_name[0] ? ctx_.hub_name : ctx_.host));
        }
        else
        {
            setStatus(manual.empty() ? "No Mac found. Hub running?" : "Can't reach " + manual);
        }
    }

    void lost(int err)
    {
        if (err == MPCLINK_ERR_NETWORK) { connected_ = false; setStatus("Lost the Mac. Retrying..."); }
    }

    // The phone page picked a beat: make it the Computer Files pick.
    void takeCue()
    {
        char path[1024];
        const int r = mpclink_take_cue(&ctx_, path, sizeof path);
        if (r != MPCLINK_OK) { lost(r); return; }
        if (!path[0]) return;
        int idx = findMacFile(path);
        if (idx < 0) { refreshMacList(); idx = findMacFile(path); }   // a brand-new beat
        if (idx < 0) { setActivity("Cued beat not found"); return; }
        if (playing_) stopTape("Stopped");
        setSelf(kMacBeat, (float)idx);
        setActivity("Cued from phone: " + baseName(path));
    }

    int findMacFile(const std::string& path)
    {
        std::lock_guard<std::mutex> lk(textMu_);
        for (size_t i = 0; i < macFiles_.size() && (int)i <= kMaxFiles; ++i)
            if (macFiles_[i] == path) return (int)i;
        return -1;
    }

    void refreshStems()
    {
        std::vector<StemsFolder> found;
        const int r = mpclink_list_stems(&ctx_, [](const mpclink_entry* e, void* u) {
            auto* v = static_cast<std::vector<StemsFolder>*>(u);
            v->push_back({ e->path, (int)e->size });
            return (int)v->size() >= kMaxFiles + 1 ? 1 : 0;
        }, &found);
        if (r != MPCLINK_OK) { lost(r); return; }
        bool changed;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            changed = found.size() != stems_.size()
                || !std::equal(found.begin(), found.end(), stems_.begin(),
                               [](const StemsFolder& a, const StemsFolder& b) { return a.path == b.path && a.files == b.files; });
            stems_.swap(found);
        }
        if (changed) requestDisplayUpdate();
    }

    void doSendStems()
    {
        std::string folder;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            if (!stems_.empty()) folder = stems_[(size_t)std::min(getInt(kStems), (int)stems_.size() - 1)].path;
        }
        if (folder.empty()) { setActivity("No stems folders"); return; }
        setActivity("Sending stems " + baseName(folder) + "...");
        int n = 0;
        const int r = mpclink_send_stems(&ctx_, folder.c_str(), &n);
        if (r == MPCLINK_OK) setActivity("Sent " + std::to_string(n) + " stems to GPP");
        else { setActivity(std::string(mpclink_strerror(r))); lost(r); }
    }

    // ---------------- cassette ----------------

    void togglePlay()
    {
        if (playing_) { stopTape("Stopped"); return; }
        std::string path;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            if (!macFiles_.empty()) path = macFiles_[(size_t)std::min(getInt(kMacBeat), (int)macFiles_.size() - 1)];
        }
        if (path.empty()) { setSelf(kPlay, 0.0f); setActivity(connected_ ? "Pick a computer file first" : "Not connected yet"); return; }
        if (path != tapePath_)
        {
            setActivity("Loading " + baseName(path) + "...");
            if (!loadTape(path)) { setSelf(kPlay, 0.0f); return; }
        }
        tapeIndex_ = getInt(kMacBeat);
        playing_ = true;
        setSelf(kPlay, 1.0f);
        lastShownSec_ = -1;
    }

    // Fetch the beat as 16-bit stereo 44.1 kHz WAV (the Mac converts it) and swap it in.
    bool loadTape(const std::string& path)
    {
        unsigned char* buf = nullptr;
        size_t len = 0;
        const int r = mpclink_fetch_preview(&ctx_, path.c_str(), &buf, &len, kMaxPreviewBytes);
        if (r != MPCLINK_OK)
        {
            setActivity(r == MPCLINK_ERR_HTTP ? "Can't play this file" : mpclink_strerror(r));
            lost(r);
            return false;
        }
        if (len < 44 || std::memcmp(buf, "RIFF", 4) != 0 || std::memcmp(buf + 36, "data", 4) != 0)
        {
            std::free(buf);
            setActivity("Can't play this file");
            return false;
        }
        Tape* t = new Tape;
        t->frames = (len - 44) / 4;
        t->pcm.resize(t->frames * 2);
        for (size_t i = 0; i < t->frames * 2; ++i)
            t->pcm[i] = (int16_t)(buf[44 + 2 * i] | (buf[45 + 2 * i] << 8));
        std::free(buf);

        // waveform: the loudest sample in each quarter second, scaled to the loudest one
        std::vector<int> peaks((t->frames + kSliceFrames - 1) / kSliceFrames, 0);
        int loudest = 1;
        for (size_t f = 0; f < t->frames; ++f)
        {
            int& pk = peaks[f / kSliceFrames];
            pk = std::max({ pk, std::abs((int)t->pcm[2 * f]), std::abs((int)t->pcm[2 * f + 1]) });
        }
        for (int pk : peaks) loudest = std::max(loudest, pk);
        slices_.assign(peaks.size(), 0);
        for (size_t i = 0; i < peaks.size(); ++i)
            slices_[i] = (uint8_t)(peaks[i] ? std::max(1, (int)std::lround(16.0 * peaks[i] / loudest)) : 0);

        swapTape(t);
        tapePath_ = path;
        tapeFrames_ = t->frames;
        drawWave(0);
        return true;
    }

    // Swap in a new tape. The audio thread reads the pointer once per block, so the
    // old tape is freed only after two more blocks (or at once if audio is stopped).
    void swapTape(Tape* t)
    {
        playing_ = false;
        pos_.store(0);
        const uint64_t seen = blocks_.load(std::memory_order_acquire);
        Tape* old = active_.exchange(t, std::memory_order_acq_rel);
        if (!old) return;
        for (int w = 0; w < 30 && blocks_.load(std::memory_order_acquire) < seen + 2; ++w)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        delete old;
    }

    // Stop and rewind to the front, like a tape deck's stop + rewind in one.
    void stopTape(const char* why)
    {
        playing_ = false;
        shownPlaying_ = false;   // a stop, not the end of the tape
        rewind();
        setSelf(kPlay, 0.0f);
        setActivity(why);
    }

    void rewind()
    {
        pos_.store(0);
        drawWave(0);
    }

    // While playing: turn the reels, light the bars, show the time. Runs every worker tick.
    void tickTape()
    {
        if (playSync_.exchange(false)) setSelf(kPlay, playing_ ? 1.0f : 0.0f);
        const bool was = shownPlaying_;
        shownPlaying_ = playing_;
        if (!playing_)
        {
            if (was)   // the tape just reached its end: rewind for next time
            {
                rewind();
                setSelf(kPlay, 0.0f);
                setActivity("End of tape");
            }
            return;
        }
        if (getInt(kMacBeat) != tapeIndex_) { stopTape("Stopped"); return; }   // a new beat was picked
        setSelf(kReel, (float)((getInt(kReel) + 1) % kReelFrames));
        const double frame = (double)(pos_.load() >> 16);
        drawWave((int)(frame / kSliceFrames));
        const int sec = (int)(frame / 44100.0);
        if (sec != lastShownSec_)
        {
            lastShownSec_ = sec;
            const int total = (int)(tapeFrames_ / 44100);
            char t[32];
            std::snprintf(t, sizeof t, " %d:%02d / %d:%02d", sec / 60, sec % 60, total / 60, total % 60);
            const std::string tail = t;
            setActivity(fitText(baseName(tapePath_), 30 - tail.size()) + tail);
        }
    }

    // The waveform window: slice `at` sits under the playhead, earlier slices to its
    // left (bright while playing), later ones to its right (dim). Only changed bars
    // are sent to the screen.
    void drawWave(int at)
    {
        for (int b = 0; b < kWaveBars; ++b)
        {
            const int slice = at + b - kPlayhead;
            const int h = (slice >= 0 && slice < (int)slices_.size()) ? slices_[(size_t)slice] : 0;
            const int v = h + (playing_ && b < kPlayhead ? kWaveLevels : 0);
            if (getInt(kWave0 + b) != v) setSelf(kWave0 + b, (float)v);
        }
    }

    void refreshMacList()
    {
        std::vector<std::string> files;
        const int r = mpclink_list(&ctx_, [](const mpclink_entry* e, void* u) {
            auto* v = static_cast<std::vector<std::string>*>(u);
            v->push_back(e->path);
            return (int)v->size() >= kMaxFiles + 1 ? 1 : 0;
        }, &files);
        if (r != MPCLINK_OK) { lost(r); return; }
        bool changed;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            changed = files != macFiles_;
            macFiles_.swap(files);
        }
        if (changed) requestDisplayUpdate();
    }

    void rescanLocal()
    {
        std::vector<LocalFile> found;
        scanLocal(outboxDir_, "", found);
        std::vector<std::string> files;
        for (const LocalFile& f : found) { if ((int)files.size() > kMaxFiles) break; files.push_back(f.rel); }
        bool changed;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            changed = files != mpcFiles_;
            mpcFiles_.swap(files);
        }
        if (changed) requestDisplayUpdate();
    }

    void doGet()
    {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            if (macFiles_.empty()) { }
            else path = macFiles_[(size_t)std::min(getInt(kMacBeat), (int)macFiles_.size() - 1)];
        }
        if (path.empty()) { setActivity("Nothing to get"); return; }
        setActivity("Getting " + baseName(path) + "...");
        char saved[1100] = {0};
        const int r = mpclink_download(&ctx_, path.c_str(), inboxDir_.c_str(), saved, sizeof saved);
        if (r == MPCLINK_OK) setActivity("Got " + baseName(saved));
        else { setActivity(std::string(mpclink_strerror(r))); lost(r); }
    }

    // Send the file picked with `picker` to the Mac: its "From MPC" folder, or
    // with toGpp the "Get Producers Paid" drafts folder. The file stays on the MPC.
    void doSend(int picker, bool toGpp)
    {
        std::string rel;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            if (!mpcFiles_.empty())
                rel = mpcFiles_[(size_t)std::min(getInt(picker), (int)mpcFiles_.size() - 1)];
        }
        if (rel.empty()) { setActivity("To Computer is empty"); return; }
        setActivity("Sending " + baseName(rel) + "...");
        const std::string local = outboxDir_ + "/" + rel;
        const int r = mpclink_upload_to(&ctx_, local.c_str(), rel.c_str(), toGpp ? "gpp" : nullptr);
        if (r == MPCLINK_OK) setActivity(toGpp ? "Sent to GPP: " + baseName(rel) : "Sent " + baseName(rel));
        else { setActivity(std::string(mpclink_strerror(r))); lost(r); }
    }

    // Buttons pressed while not connected are dropped with a message.
    void dropRequests()
    {
        const bool any = toMpcReq_.exchange(false) | toFolderReq_.exchange(false) | toGppReq_.exchange(false)
                       | toStemsReq_.exchange(false);
        if (any) { release(kToMpc); release(kToFolder); release(kToGpp); release(kToStems); setActivity("Not connected yet"); }
        if (playReq_.exchange(false))
        {
            if (playing_) stopTape("Stopped");   // a tape already loaded can play without the Mac
            else if (active_.load() && tapeIndex_ == getInt(kMacBeat)) togglePlay();
            else { setSelf(kPlay, 0.0f); setActivity("Not connected yet"); }
        }
        tickTape();
    }

    // Pop a button back up and tell the screen.
    void release(int index)
    {
        if (getNormalised(index) > 0.0f) setSelf(index, 0.0f);
    }

    // Change one of our own values (a readout, a light, the reels) and tell the screen,
    // without it counting as a tap.
    void setSelf(int index, float plain)
    {
        selfSet_ = true;
        set(index, plain);
        selfSet_ = false;
        notifyHostParameterChanged(index);
    }

    void setStatus(const std::string& s)
    {
        { std::lock_guard<std::mutex> lk(textMu_); if (status_ == s) return; status_ = s; }
        bump(kStatus);
    }
    // One status line: connection state, replaced by the last action until the next change.
    void setActivity(const std::string& s)
    {
        { std::lock_guard<std::mutex> lk(textMu_); status_ = s; }
        bump(kStatus);
    }
    // The MPC only re-reads a readout's text when its value changes, so step a counter.
    void bump(int index)
    {
        setSelf(index, (float)((getInt(index) + 1) % 1000));
    }

    std::thread worker_;
    std::mutex wakeMu_;
    std::condition_variable wake_;
    std::atomic<bool> stop_{false};

    std::atomic<bool> toMpcReq_{false}, toFolderReq_{false}, toGppReq_{false}, toStemsReq_{false}, playReq_{false};
    std::atomic<long long> toMpcAt_{0}, toFolderAt_{0}, toGppAt_{0}, toStemsAt_{0}, playAt_{0};
    std::atomic<bool> selfSet_{false}, playSync_{false};

    // cassette: the audio thread reads active_/pos_/playing_; everything else is the worker's
    std::atomic<Tape*> active_{nullptr};
    std::atomic<uint64_t> pos_{0}, step_{65536}, blocks_{0};
    std::atomic<bool> playing_{false};
    bool shownPlaying_ = false;
    std::string tapePath_;
    size_t tapeFrames_ = 0;
    int tapeIndex_ = -1, lastShownSec_ = -1;
    std::vector<uint8_t> slices_;   // waveform heights 0..16, one per quarter second

    mpclink_ctx ctx_{};
    std::atomic<bool> connected_{false};
    std::string inboxDir_, outboxDir_, hubFile_;

    std::mutex textMu_;   // guards everything below (read by formatValue on the UI thread)
    std::string status_;
    std::vector<std::string> macFiles_, mpcFiles_;
    struct StemsFolder { std::string path; int files; };
    std::vector<StemsFolder> stems_;
};

MPC_PLUGIN_ENTRY(MPCLink)
