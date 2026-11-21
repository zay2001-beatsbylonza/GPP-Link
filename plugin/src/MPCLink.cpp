// GPP Link - move beats between this MPC, the Beats folder on your Mac, and
// the Get Producers Paid app. By BeatsbyLonza. Built on Akai's MPC Plugin SDK (MIT).
//
// The screen:
//   A cassette deck on top: PLAY plays the beat picked in "Computer Files" through
//   this track (a preview, nothing is saved), the reels turn, and 32 bars on the
//   label bounce to the sound like a stereo's visualizer: bass on the left, highs
//   on the right, mirrored around the centre line, the loudest tips in red.
//   Turning a picker while it plays keeps the beat playing (browse while you
//   listen); PLAY on another beat switches to it, PLAY on the same beat stops and
//   rewinds to the front; the end rewinds too.
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
// A second tab, RECEIVED, has the same cassette and a "Received" picker: the files
// in "<drive>/From Computer/" (what this MPC got), newest first. The cassette plays
// whichever picker was turned last, Computer Files or Received; a received file is
// sent to the computer to be turned into the cassette's audio (nothing is kept there).
// Nothing leaves the MPC unless a button is pressed. Files sent from the phone
// page arrive in "From Computer" by themselves, a beat cued on the phone page
// becomes the "Computer Files" pick, and the lists refresh on their own.
//
// Audio passes straight through (plus the cassette preview when playing), so the
// plugin can sit on any track. Network and disk work happen on a background
// thread; the audio thread only reads a ready-decoded buffer.
// It finds the GPP Link hub by UDP broadcast, or reads the Mac's address from
// "<drive>/GPP Link hub.txt" when broadcasts are blocked.
// <drive> is the MPC's internal drive ("MPC User"), even when the plugin itself
// is installed on an SD card or USB stick, so beats stay in one place on each
// MPC. If no internal drive is found, it is the drive the plugin is installed
// on (the one holding "VST2 Plugins").
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
    kRecv = kWave0 + 32, // index into "From Computer" (what this MPC received), for the cassette
    kMacPos,     // readouts: "3 / 48", where each picker is in its list (screen only)
    kMpcPos,
    kStemsPos,
    kRecvPos,
    kRecvCount,  // readout: how many files this MPC has received (screen only)
    kRecvToFolder, // button (RECEIVED tab): SEND TO FOLDER with the Received pick
    kRecvToGpp,    // button (RECEIVED tab): SEND TO GPP with the Received pick
    kNumParams
};

constexpr int kWaveBars   = 32;
constexpr int kWaveLevels = 17;   // bar heights 0..16; +17 = the same height with a red (loud) tip
constexpr int kFftSize    = 1024; // the visualizer looks at ~23 ms of sound around the play position
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
        addParam(mpc::intParam ("recv",     "Received",       0, kMaxFiles, 0));
        const char* const readouts[][2] = { { "macpos", "Files Pos" }, { "mpcpos", "MPC Pos" }, { "stemspos", "Stems Pos" },
                                            { "recvpos", "Received Pos" }, { "recvcount", "Received Count" } };
        for (const auto& r : readouts)
        {
            mpc::Param q = mpc::intParam(r[0], r[1], 0, 1000, 0);
            q.automatable = false;
            addParam(q);
        }
        addParam(mpc::boolParam("recvtofolder", "Received to Folder", false));
        addParam(mpc::boolParam("recvtogpp",    "Received to GPP",    false));

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
        if (index == kMacBeat) { tapeSrc_.store(0); posDirty_ = true; return; }   // the cassette follows the last picker turned
        if (index == kRecv)    { tapeSrc_.store(1); posDirty_ = true; return; }
        if (index == kMpcBeat || index == kStems) { posDirty_ = true; return; }
        if (v < 0.5f) return;
        switch (index)
        {
            case kToMpc:    press(toMpcReq_, toMpcAt_); break;
            case kToFolder: press(toFolderReq_, toFolderAt_); break;
            case kToGpp:    press(toGppReq_, toGppAt_); break;
            case kToStems:  press(toStemsReq_, toStemsAt_); break;
            case kRecvToFolder: press(recvFolderReq_, recvFolderAt_); break;
            case kRecvToGpp:    press(recvGppReq_, recvGppAt_); break;
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
            case kMacPos:   out = posText(kMacBeat, macFiles_.size()); return true;
            case kMpcPos:   out = posText(kMpcBeat, mpcFiles_.size()); return true;
            case kStemsPos: out = posText(kStems, stems_.size()); return true;
            case kRecvPos:  out = posText(kRecv, recvFiles_.size()); return true;
            case kRecvCount: out = std::to_string(recvFiles_.size()); return true;
            case kRecv:
                if (recvFiles_.empty()) out = "(nothing received yet)";
                else out = fitText(baseName(recvFiles_[(size_t)std::min(i, (int)recvFiles_.size() - 1)]));
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
    // "3 / 48": the picker's place in its list (textMu_ held)
    std::string posText(int picker, size_t n)
    {
        if (n == 0) return "0 / 0";
        const int i = std::min(getInt(picker), (int)n - 1);
        return std::to_string(i + 1) + " / " + std::to_string(n);
    }

    void bumpCounters()
    {
        for (int p : { kMacPos, kMpcPos, kStemsPos, kRecvPos, kRecvCount }) bump(p);
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "GPP Link";
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
        // The drive this plugin is installed on: <drive>/VST2 Plugins/<this folder>.
        // (computePluginFolder, not pluginFolder(): no function-local static to destroy at unload.)
        const std::string folder = mpc::detail::computePluginFolder();
        std::string pluginDrive = parentOf(folder);
        if (baseName(pluginDrive) == "VST2 Plugins" || baseName(pluginDrive) == "Synths") pluginDrive = parentOf(pluginDrive);
        if (pluginDrive.empty()) pluginDrive = "/tmp";
        // Beats live on the internal drive (MPC User) when there is one, wherever the plugin is installed.
        const char *user = mpclink_user_drive();
        const std::string drive = user ? std::string(user) : pluginDrive;
        inboxDir_  = drive + "/From Computer";
        outboxDir_ = drive + "/To Computer";
        hubFile_   = drive + "/GPP Link hub.txt";
        hubFile2_  = pluginDrive == drive ? std::string() : pluginDrive + "/GPP Link hub.txt";
        makeDirs(inboxDir_);
        makeDirs(outboxDir_);

        mpclink_init(&ctx_, deviceName().c_str());
        ctx_.timeout_ms = 4000;
        rescanLocal();

        Clock::time_point lastTry{}, lastList{}, lastInbox{}, lastCue{}, lastLocal = Clock::now();
        while (!sleepFor(playing_ ? 60 : 150))   // the visualizer moves ~16 times a second while playing
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
            if (recvFolderReq_.exchange(false)) { doSendRecv(false); release(kRecvToFolder); }
            if (recvGppReq_.exchange(false))    { doSendRecv(true);  release(kRecvToGpp); }
            if (playReq_.exchange(false))     togglePlay();
            tickTape();
            showSource();
            if (posDirty_.exchange(false)) bumpCounters();

            if (now - lastCue > std::chrono::seconds(1))     // a beat cued on the phone page
            {
                lastCue = now;
                takeCue();
            }
            if (now - lastInbox > std::chrono::seconds(3))   // files sent from the phone page
            {
                lastInbox = now;
                const int n = mpclink_poll_inbox(&ctx_, inboxDir_.c_str());
                if (n > 0) { setActivity("Got " + std::to_string(n) + " from phone"); rescanLocal(); }
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
        std::string manual = readFirstLine(hubFile_);
        if (manual.empty() && !hubFile2_.empty()) manual = readFirstLine(hubFile2_);
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
        tapeSrc_.store(0);
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
        if (changed) { requestDisplayUpdate(); posDirty_ = true; }
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

    // The cassette's pick: the last picker turned (Computer Files or Received).
    // key names the tape ("mac:<path>" or "recv:<path>"); empty if that list is empty.
    void tapePick(int& src, int& index, std::string& key, std::string& path)
    {
        src = tapeSrc_.load();
        index = getInt(src ? kRecv : kMacBeat);
        std::lock_guard<std::mutex> lk(textMu_);
        const std::vector<std::string>& list = src ? recvFiles_ : macFiles_;
        if (list.empty()) { key.clear(); path.clear(); return; }
        const std::string& rel = list[(size_t)std::min(index, (int)list.size() - 1)];
        key = (src ? "recv:" : "mac:") + rel;
        path = src ? inboxDir_ + "/" + rel : rel;
    }

    // PLAY: start the pick; on the beat that's playing, stop; on another beat while one
    // plays, switch to it. Turning a picker never stops the tape, so you can browse
    // while you listen.
    void togglePlay()
    {
        int src, index;
        std::string key, path;
        tapePick(src, index, key, path);
        if (playing_)
        {
            if (key.empty() || key == tapePath_) { stopTape("Stopped"); return; }
            playing_ = false;   // switch: stop this one quietly, load the new pick below
            shownPlaying_ = false;
            pos_.store(0);
        }
        if (key.empty())
        {
            setSelf(kPlay, 0.0f);
            setActivity(src ? "Nothing received yet" : connected_ ? "Pick a computer file first" : "Not connected yet");
            return;
        }
        if (key != tapePath_)
        {
            if (!connected_) { setSelf(kPlay, 0.0f); setActivity("Not connected yet"); return; }
            setActivity("Loading " + baseName(path) + "...");
            if (!loadTape(path, src == 1)) { setSelf(kPlay, 0.0f); return; }
            tapePath_ = key;
            tapeName_ = baseName(path);
        }
        tapeIndex_ = index;
        tapeSrcPlaying_ = src;
        playing_ = true;
        setSelf(kPlay, 1.0f);
        lastShownSec_ = -1;
    }

    // Say which list the cassette plays when it changes (worker thread).
    void showSource()
    {
        const int src = tapeSrc_.load();
        if (src == shownSrc_) return;
        shownSrc_ = src;
        if (playing_) return;   // tickTape stops the tape and says so
        setActivity(src ? "Cassette: Received" : "Cassette: Computer Files");
    }

    // Fetch the beat as 16-bit stereo 44.1 kHz WAV (the Mac converts it) and swap it in.
    // local: a file on this MPC (Received), sent to the computer to be converted.
    bool loadTape(const std::string& path, bool local)
    {
        unsigned char* buf = nullptr;
        size_t len = 0;
        const int r = local ? mpclink_preview_file(&ctx_, path.c_str(), &buf, &len, kMaxPreviewBytes)
                            : mpclink_fetch_preview(&ctx_, path.c_str(), &buf, &len, kMaxPreviewBytes);
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

        swapTape(t);
        tapePath_.clear();   // the caller names the new tape
        tapeFrames_ = t->frames;
        drawIdle();
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
        drawIdle();
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
        setSelf(kReel, (float)((getInt(kReel) + 1) % kReelFrames));
        const double frame = (double)(pos_.load() >> 16);
        drawLive((size_t)frame);
        const int sec = (int)(frame / 44100.0);
        if (sec != lastShownSec_)
        {
            lastShownSec_ = sec;
            const int total = (int)(tapeFrames_ / 44100);
            char t[32];
            std::snprintf(t, sizeof t, " %d:%02d / %d:%02d", sec / 60, sec % 60, total / 60, total % 60);
            const std::string tail = t;
            setActivity(fitText(tapeName_, 30 - tail.size()) + tail);
        }
    }

    // The visualizer, resting: a flat line.
    void drawIdle()
    {
        std::fill(level_, level_ + kWaveBars, 0.0f);
        for (int b = 0; b < kWaveBars; ++b)
            if (getInt(kWave0 + b) != 1) setSelf(kWave0 + b, 1.0f);
    }

    // The visualizer, playing: the sound around `frame` split into 32 bands (bass
    // left, highs right), each bar jumping up with its band and falling back
    // smoothly. Only changed bars are sent to the screen.
    void drawLive(size_t frame)
    {
        const Tape* t = active_.load(std::memory_order_acquire);   // the worker owns tape swaps: safe to read
        if (!t || t->frames < (size_t)kFftSize) return;
        if (frame + kFftSize > t->frames) frame = t->frames - kFftSize;
        float* re = fftRe_;   // per instance: two GPP Links on two tracks each have a worker
        float* im = fftIm_;
        for (int i = 0; i < kFftSize; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos(6.2831853f * i / (kFftSize - 1));   // Hann window
            re[i] = w * (t->pcm[2 * (frame + i)] + t->pcm[2 * (frame + i) + 1]) * (0.5f / 32768.0f);
            im[i] = 0.0f;
        }
        fft(re, im);
        // 32 bands spaced evenly in pitch from ~40 Hz to ~16 kHz (bin width 43 Hz)
        for (int b = 0; b < kWaveBars; ++b)
        {
            const double f0 = 40.0 * std::pow(400.0, (double)b / kWaveBars);
            const double f1 = 40.0 * std::pow(400.0, (double)(b + 1) / kWaveBars);
            int k0 = std::max(1, (int)(f0 / (44100.0 / kFftSize)));
            int k1 = std::max(k0 + 1, (int)(f1 / (44100.0 / kFftSize)) + 1);
            k1 = std::min(k1, kFftSize / 2);
            float e = 0.0f;
            for (int k = k0; k < k1; ++k) e = std::max(e, re[k] * re[k] + im[k] * im[k]);
            // dB above a -60 dB floor, highs lifted a little so they show (music is bass-heavy)
            const float db = 10.0f * std::log10(e + 1e-12f) + 6.0f * (float)b / kWaveBars;
            const float target = mpc::clamp((db + 18.0f) / 40.0f, 0.0f, 1.0f) * 16.0f;
            level_[b] = target > level_[b] ? target : std::max(target, level_[b] - 2.2f);   // jump up, fall back
        }
        for (int b = 0; b < kWaveBars; ++b)
        {
            const int h = std::max(1, (int)std::lround(level_[b]));
            const int v = h + (h >= 14 ? kWaveLevels : 0);   // red tip when it's loud
            if (getInt(kWave0 + b) != v) setSelf(kWave0 + b, (float)v);
        }
    }

    // In-place radix-2 FFT of kFftSize points.
    static void fft(float* re, float* im)
    {
        const int n = kFftSize;
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const float ang = -6.2831853f / len;
            const float wr = std::cos(ang), wi = std::sin(ang);
            for (int i = 0; i < n; i += len)
            {
                float cr = 1.0f, ci = 0.0f;
                for (int k = 0; k < len / 2; ++k)
                {
                    const int a = i + k, b = a + len / 2;
                    const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                    const float nr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr; cr = nr;
                }
            }
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
        int keep;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            changed = files != macFiles_;
            keep = newIndexOf(kMacBeat, macFiles_, files);
            macFiles_.swap(files);
        }
        if (keep >= 0)
        {
            setSelf(kMacBeat, (float)keep);
            if (tapeSrcPlaying_ == 0 && tapeIndex_ >= 0) tapeIndex_ = keep;   // the same beat keeps playing
        }
        if (changed) { requestDisplayUpdate(); posDirty_ = true; }
    }

    void rescanLocal()
    {
        std::vector<std::string> files = localList(outboxDir_), recv = localList(inboxDir_);
        int keepMpc, keepRecv;
        bool changed;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            changed = files != mpcFiles_ || recv != recvFiles_;
            keepMpc = newIndexOf(kMpcBeat, mpcFiles_, files);
            keepRecv = newIndexOf(kRecv, recvFiles_, recv);
            mpcFiles_.swap(files);
            recvFiles_.swap(recv);
        }
        if (keepMpc >= 0) setSelf(kMpcBeat, (float)keepMpc);
        if (keepRecv >= 0)
        {
            setSelf(kRecv, (float)keepRecv);
            if (tapeSrcPlaying_ == 1 && tapeIndex_ >= 0) tapeIndex_ = keepRecv;   // the same beat keeps playing
        }
        if (changed) { requestDisplayUpdate(); posDirty_ = true; }
    }

    // When a list changes (a new file arrives, newest first), keep the same file
    // picked: its index in the new list, or -1 if nothing needs to move (textMu_ held).
    int newIndexOf(int picker, const std::vector<std::string>& before, const std::vector<std::string>& after)
    {
        if (before.empty() || before == after) return -1;
        const int i = std::min(getInt(picker), (int)before.size() - 1);
        const auto it = std::find(after.begin(), after.end(), before[(size_t)i]);
        if (it == after.end()) return -1;
        const int j = (int)(it - after.begin());
        return j == getInt(picker) ? -1 : j;
    }

    static std::vector<std::string> localList(const std::string& dir)
    {
        std::vector<LocalFile> found;
        scanLocal(dir, "", found);
        std::vector<std::string> files;
        for (const LocalFile& f : found) { if ((int)files.size() > kMaxFiles) break; files.push_back(f.rel); }
        return files;
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
        if (r == MPCLINK_OK) { setActivity("Got " + baseName(saved)); rescanLocal(); }
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

    // RECEIVED tab: send the Received pick (a file in From Computer) to the Mac's
    // From MPC folder, or to Get Producers Paid. The file stays on the MPC.
    void doSendRecv(bool toGpp)
    {
        std::string rel;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            if (!recvFiles_.empty())
                rel = recvFiles_[(size_t)std::min(getInt(kRecv), (int)recvFiles_.size() - 1)];
        }
        if (rel.empty()) { setActivity("Nothing received yet"); return; }
        setActivity("Sending " + baseName(rel) + "...");
        const std::string local = inboxDir_ + "/" + rel;
        const int r = mpclink_upload_to(&ctx_, local.c_str(), rel.c_str(), toGpp ? "gpp" : nullptr);
        if (r == MPCLINK_OK) setActivity(toGpp ? "Sent to GPP: " + baseName(rel) : "Sent " + baseName(rel));
        else { setActivity(std::string(mpclink_strerror(r))); lost(r); }
    }

    // Buttons pressed while not connected are dropped with a message.
    void dropRequests()
    {
        const bool any = toMpcReq_.exchange(false) | toFolderReq_.exchange(false) | toGppReq_.exchange(false)
                       | toStemsReq_.exchange(false) | recvFolderReq_.exchange(false) | recvGppReq_.exchange(false);
        if (any)
        {
            for (int b : { kToMpc, kToFolder, kToGpp, kToStems, kRecvToFolder, kRecvToGpp }) release(b);
            setActivity("Not connected yet");
        }
        if (posDirty_.exchange(false)) bumpCounters();
        if (playReq_.exchange(false))
        {
            int src, index;   // without the Mac, only the tape already loaded can play
            std::string key, path;
            tapePick(src, index, key, path);
            if (playing_ && (key.empty() || key == tapePath_)) stopTape("Stopped");
            else if (active_.load() && key == tapePath_) togglePlay();
            else { if (playing_) stopTape("Stopped"); setSelf(kPlay, 0.0f); setActivity("Not connected yet"); }
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

    std::atomic<bool> toMpcReq_{false}, toFolderReq_{false}, toGppReq_{false}, toStemsReq_{false}, playReq_{false},
                      recvFolderReq_{false}, recvGppReq_{false};
    std::atomic<long long> toMpcAt_{0}, toFolderAt_{0}, toGppAt_{0}, toStemsAt_{0}, playAt_{0}, recvFolderAt_{0}, recvGppAt_{0};
    std::atomic<bool> selfSet_{false}, playSync_{false}, posDirty_{true};

    // cassette: the audio thread reads active_/pos_/playing_; everything else is the worker's
    std::atomic<Tape*> active_{nullptr};
    std::atomic<uint64_t> pos_{0}, step_{65536}, blocks_{0};
    std::atomic<bool> playing_{false};
    bool shownPlaying_ = false;
    std::string tapePath_, tapeName_;   // the loaded tape ("mac:<path>" / "recv:<path>") and its file name
    size_t tapeFrames_ = 0;
    int tapeIndex_ = -1, lastShownSec_ = -1;
    std::atomic<int> tapeSrc_{0};        // the cassette's list: 0 = Computer Files, 1 = Received (last turned)
    int tapeSrcPlaying_ = 0, shownSrc_ = 0;
    float level_[kWaveBars] = {};   // the visualizer bars' current heights (0..16)
    float fftRe_[kFftSize] = {}, fftIm_[kFftSize] = {};

    mpclink_ctx ctx_{};
    std::atomic<bool> connected_{false};
    std::string inboxDir_, outboxDir_, hubFile_, hubFile2_;

    std::mutex textMu_;   // guards everything below (read by formatValue on the UI thread)
    std::string status_;
    std::vector<std::string> macFiles_, mpcFiles_, recvFiles_;
    struct StemsFolder { std::string path; int files; };
    std::vector<StemsFolder> stems_;
};

MPC_PLUGIN_ENTRY(MPCLink)
