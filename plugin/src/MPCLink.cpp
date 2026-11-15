// MPC Link - send files between this MPC and the Beats folder on your Mac.
// By BeatsbyLonza. Built on Akai's MPC Plugin SDK (MIT).
//
// How it works
//   * The plugin is an insert effect that passes audio straight through, so it
//     can sit on any track. All the work happens on a background thread.
//   * It finds the MPC Link hub on your Wi-Fi (UDP broadcast), or reads the
//     Mac's address from "<drive>/MPC Link hub.txt" if broadcasts are blocked.
//   * GET:  pick a file from the Mac with the "Mac File" control, press Get.
//           It lands in "<drive>/From Computer/".
//   * SEND: put files in "<drive>/To Computer/" (save or export them there from
//           the MPC browser), pick one with "MPC File", press Send, or press
//           Send All. Sent files move to "<drive>/To Computer/Sent/".
//           On the Mac they land in "Beats/From MPC/<this MPC's name>/".
//   * Phone: with "Phone Q" on, files you send from the phone page arrive in
//            "<drive>/From Computer/" by themselves.
//   <drive> is the drive this plugin is installed on (the one holding
//   "VST2 Plugins"), so it works on every MPC model without guessing paths.
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
    kLevel = 0,  // output level, dB. Param 0 is nudged by the host on load: harmless here.
    kStatus,     // readout: connection status (text only; the value is a change counter)
    kMacFile,    // index into the Mac's file list
    kGet,        // button: download the selected Mac file
    kRefresh,    // button: reconnect and re-read both lists
    kPhoneQ,     // switch: accept files sent from the phone page
    kMpcFile,    // index into "To Computer" on this MPC
    kSend,       // button: send the selected MPC file
    kSendAll,    // button: send everything in "To Computer"
    kActivity,   // readout: what happened last
    kNumParams
};

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

} // namespace

class MPCLink : public mpc::Plugin
{
public:
    MPCLink() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("level",    "Level",     -24.0f, 6.0f, 0.0f, "dB", 1));
        addParam(mpc::intParam  ("status",   "Mac",       0, 1000, 0));
        addParam(mpc::intParam  ("macfile",  "Mac File",  0, kMaxFiles, 0));
        addParam(mpc::boolParam ("get",      "Get",       false));
        addParam(mpc::boolParam ("refresh",  "Refresh",   false));
        addParam(mpc::boolParam ("phoneq",   "Phone Q",   true));
        addParam(mpc::intParam  ("mpcfile",  "MPC File",  0, kMaxFiles, 0));
        addParam(mpc::boolParam ("send",     "Send",      false));
        addParam(mpc::boolParam ("sendall",  "Send All",  false));
        addParam(mpc::intParam  ("activity", "Activity",  0, 1000, 0));

        status_ = "Starting...";
        activity_ = "Ready";
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
    }

    void prepare(double, int) override
    {
        if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
        reset();
    }
    void reset() override { gain_ = targetGain(); }

    // Audio passes straight through (with the Level control). Nothing else happens here.
    void process(mpc::ProcessContext& ctx) override
    {
        const float target = targetGain();
        const int nCh = mpc::clamp(ctx.numOutputs, 0, 2);
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            gain_ += 0.002f * (target - gain_);
            for (int ch = 0; ch < nCh; ++ch)
            {
                const float in = (ctx.inputs && ch < ctx.numInputs && ctx.inputs[ch]) ? ctx.inputs[ch][i] : 0.0f;
                ctx.outputs[ch][i] = in * gain_;
            }
        }
    }

    // Buttons only raise a flag; the worker does the work. (May run on any thread: keep it trivial.)
    void parameterChanged(int index, float v) override
    {
        if (v < 0.5f) return;
        switch (index)
        {
            case kGet:     press(getReq_, getAt_); break;
            case kRefresh: press(refreshReq_, refreshAt_); break;
            case kSend:    press(sendReq_, sendAt_); break;
            case kSendAll: press(sendAllReq_, sendAllAt_); break;
            default: break;
        }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        std::lock_guard<std::mutex> lk(textMu_);
        const int i = (int)std::lround(plain);
        switch (index)
        {
            case kStatus:   out = fitText(status_); return true;
            case kActivity: out = fitText(activity_); return true;
            case kMacFile:
                if (macFiles_.empty()) out = connected_ ? "(Beats folder empty)" : "(not connected)";
                else out = fitText(baseName(macFiles_[(size_t)std::min(i, (int)macFiles_.size() - 1)]));
                return true;
            case kMpcFile:
                if (mpcFiles_.empty()) out = "(To Computer is empty)";
                else out = fitText(baseName(mpcFiles_[(size_t)std::min(i, (int)mpcFiles_.size() - 1)]));
                return true;
            case kGet: case kRefresh: case kSend: case kSendAll:
                out = plain >= 0.5f ? "Working" : "Press";
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

    float targetGain() const { return std::pow(10.0f, get(kLevel) / 20.0f); }

    // A tap on the MPC can arrive twice ~130 ms apart: one action per 400 ms.
    static void press(std::atomic<bool>& req, std::atomic<long long>& at)
    {
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count();
        if (now - at.load() < 400) return;
        at.store(now);
        req.store(true);
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
        makeDirs(outboxDir_ + "/Sent");

        mpclink_init(&ctx_, deviceName().c_str());
        ctx_.timeout_ms = 4000;
        rescanLocal();

        Clock::time_point lastTry{}, lastList{}, lastInbox{}, lastLocal = Clock::now();
        while (!sleepFor(150))
        {
            const Clock::time_point now = Clock::now();

            if (refreshReq_.exchange(false))
            {
                release(kRefresh);
                connected_ = false;
                lastTry = Clock::time_point{};
                rescanLocal();
            }

            if (!connected_)
            {
                if (now - lastTry < std::chrono::seconds(5)) { dropRequests(); continue; }
                lastTry = now;
                connect();
                if (connected_) { refreshMacList(); lastList = now; }
                continue;
            }

            if (getReq_.exchange(false))     { doGet();     release(kGet); }
            if (sendReq_.exchange(false))    { doSend(false); release(kSend); }
            if (sendAllReq_.exchange(false)) { doSend(true);  release(kSendAll); }

            if (getBool(kPhoneQ) && now - lastInbox > std::chrono::seconds(3))
            {
                lastInbox = now;
                const int n = mpclink_poll_inbox(&ctx_, inboxDir_.c_str());
                if (n > 0) setActivity("Got " + std::to_string(n) + " from phone");
                else if (n < 0) lost(n);
            }
            if (connected_ && now - lastList > std::chrono::seconds(10)) { refreshMacList(); lastList = now; }
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
            if (macFiles_.empty()) { activity_ = "Nothing to get"; }
            else path = macFiles_[(size_t)std::min(getInt(kMacFile), (int)macFiles_.size() - 1)];
        }
        if (path.empty()) { bump(kActivity); return; }
        setActivity("Getting " + baseName(path) + "...");
        char saved[1100] = {0};
        const int r = mpclink_download(&ctx_, path.c_str(), inboxDir_.c_str(), saved, sizeof saved);
        if (r == MPCLINK_OK) setActivity("Got " + baseName(saved));
        else { setActivity(std::string(mpclink_strerror(r))); lost(r); }
    }

    void doSend(bool all)
    {
        std::vector<std::string> todo;
        {
            std::lock_guard<std::mutex> lk(textMu_);
            if (all) todo = mpcFiles_;
            else if (!mpcFiles_.empty())
                todo.push_back(mpcFiles_[(size_t)std::min(getInt(kMpcFile), (int)mpcFiles_.size() - 1)]);
        }
        if (todo.empty()) { setActivity("To Computer is empty"); return; }
        int sent = 0;
        for (const std::string& rel : todo)
        {
            if (stop_) break;
            setActivity("Sending " + baseName(rel) + "...");
            const std::string local = outboxDir_ + "/" + rel;
            const int r = mpclink_upload(&ctx_, local.c_str(), rel.c_str());
            if (r != MPCLINK_OK) { setActivity(std::string(mpclink_strerror(r))); lost(r); break; }
            ++sent;
            // Move it to Sent/ so Send All doesn't send it twice.
            const std::string dest = outboxDir_ + "/Sent/" + rel;
            makeDirs(parentOf(dest));
            struct stat st;
            if (stat(dest.c_str(), &st) == 0) unlink(dest.c_str());
            std::rename(local.c_str(), dest.c_str());
        }
        if (sent == (int)todo.size())
            setActivity(sent == 1 ? "Sent " + baseName(todo[0]) : "Sent " + std::to_string(sent) + " files");
        rescanLocal();
    }

    // Buttons pressed while not connected are dropped with a message.
    void dropRequests()
    {
        const bool any = getReq_.exchange(false) | sendReq_.exchange(false) | sendAllReq_.exchange(false);
        if (any) { release(kGet); release(kSend); release(kSendAll); setActivity("Not connected yet"); }
    }

    // Pop a button back up and tell the screen.
    void release(int index)
    {
        if (getNormalised(index) > 0.0f) { setNormalised(index, 0.0f); notifyHostParameterChanged(index); }
    }

    void setStatus(const std::string& s)
    {
        { std::lock_guard<std::mutex> lk(textMu_); if (status_ == s) return; status_ = s; }
        bump(kStatus);
    }
    void setActivity(const std::string& s)
    {
        { std::lock_guard<std::mutex> lk(textMu_); activity_ = s; }
        bump(kActivity);
    }
    // The MPC only re-reads a readout's text when its value changes, so step a counter.
    void bump(int index)
    {
        const int next = (getInt(index) + 1) % 1000;
        set(index, (float)next);
        notifyHostParameterChanged(index);
    }

    float gain_ = 1.0f;

    std::thread worker_;
    std::mutex wakeMu_;
    std::condition_variable wake_;
    std::atomic<bool> stop_{false};

    std::atomic<bool> getReq_{false}, refreshReq_{false}, sendReq_{false}, sendAllReq_{false};
    std::atomic<long long> getAt_{0}, refreshAt_{0}, sendAt_{0}, sendAllAt_{0};

    mpclink_ctx ctx_{};
    std::atomic<bool> connected_{false};
    std::string inboxDir_, outboxDir_, hubFile_;

    std::mutex textMu_;   // guards everything below (read by formatValue on the UI thread)
    std::string status_, activity_;
    std::vector<std::string> macFiles_, mpcFiles_;
};

MPC_PLUGIN_ENTRY(MPCLink)
