// A pretend MPC: loads the real MPC Link plugin (.so), runs audio on one thread
// like the MPC does, presses its buttons, and reads its screen text, while an
// MPC Link hub runs on this machine.
//
// Build: c++ -std=c++17 -I<sdk>/framework tests/plugin_host_test.cpp -ldl -lpthread -o plugin_host_test
// Run:   MPC_PLUGIN_FOLDER="<drive>/VST2 Plugins/BeatsbyLonza - VST - MPC Link" ./plugin_host_test <plugin.so> <drive>
#include "mpc_vst2_abi.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

enum { kStatus, kMacBeat, kMpcBeat, kToMpc, kToFolder, kToGpp, kStems, kToStems, kPlay, kReel, kWave0 };

static std::atomic<int> automates{0}, updates{0};
static std::atomic<bool> running{true};
static std::atomic<float> loudest{0.0f};   // how far the output strays from the 0.25 test input
static int failures = 0;

static VstIntPtr host(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float)
{
    if (opcode == audioMasterVersion) return 2400;
    if (opcode == audioMasterAutomate) automates++;
    if (opcode == audioMasterUpdateDisplay) updates++;
    return 0;
}

static std::string display(AEffect* e, int i)
{
    char buf[256] = {0};
    e->dispatcher(e, effGetParamDisplay, i, 0, buf, 0.0f);
    return buf;
}

static bool waitFor(AEffect* e, int param, const char* needle, int seconds)
{
    for (int t = 0; t < seconds * 20; ++t)
    {
        if (display(e, param).find(needle) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// wait first, then read the screen (function arguments have no fixed evaluation order)
static void expectText(AEffect* e, int param, const char* needle, int seconds, const char* what);
static bool exists(const std::string& p);
static void expectFile(const std::string& path, int seconds, const char* what);

static void check(bool ok, const char* what, const std::string& seen)
{
    std::printf("%s  %s   [%s]\n", ok ? "PASS" : "FAIL", what, seen.c_str());
    if (!ok) failures++;
}

static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

static void expectText(AEffect* e, int param, const char* needle, int seconds, const char* what)
{
    const bool ok = waitFor(e, param, needle, seconds);
    check(ok, what, display(e, param));
}

static void expectFile(const std::string& path, int seconds, const char* what)
{
    bool ok = false;
    for (int t = 0; t < seconds * 20 && !(ok = exists(path)); ++t)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(ok, what, path);
}

// a button press as the MPC sends it: value up, then back down
static void press(AEffect* e, int i)
{
    e->setParameter(e, i, 1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(130));
    e->setParameter(e, i, 0.0f);   // the duplicate "second event" the MPC sends; must not fire twice
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s plugin.so drive\n", argv[0]); return 2; }
    const std::string drive = argv[2];
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto entry = (AEffect* (*)(audioMasterCallback))dlsym(lib, "VSTPluginMain");
    AEffect* e = entry(host);
    if (!e || e->magic != kEffectMagic) { std::fprintf(stderr, "no effect\n"); return 1; }

    e->dispatcher(e, effOpen, 0, 0, nullptr, 0.0f);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    e->dispatcher(e, effSetBlockSize, 0, 128, nullptr, 0.0f);
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0.0f);

    // audio thread, like the MPC's
    std::thread audio([e] {
        std::vector<float> l(128, 0.25f), r(128, 0.25f), ol(128), orr(128);
        float* in[2] = { l.data(), r.data() };
        float* out[2] = { ol.data(), orr.data() };
        while (running)
        {
            e->processReplacing(e, in, out, 128);
            for (int i = 0; i < 128; ++i)
            {
                const float dev = std::fabs(ol[i] - 0.25f);
                if (dev > loudest.load()) loudest.store(dev);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(2900));
        }
    });

    expectText(e, kStatus, "On ", 15, "finds the Mac");
    expectText(e, kMacBeat, "Trap Beat 140", 15, "lists the Mac's newest beat");

    press(e, kToMpc);
    expectText(e, kStatus, "Got Trap Beat 140", 15, "SEND TO MPC downloads");
    check(exists(drive + "/From Computer/Trap Beat 140.wav"), "file is in From Computer", "");
    check(!exists(drive + "/From Computer/Trap Beat 140 (2).wav"), "one press = one download", "");
    {
        const bool up = e->getParameter(e, kToMpc) < 0.5f;
        check(up, "SEND TO MPC button pops back up", display(e, kToMpc));
    }

    expectText(e, kMpcBeat, "snare", 15, "From MPC lists To Computer, newest first");
    press(e, kToFolder);
    expectText(e, kStatus, "Sent snare.wav", 20, "SEND TO FOLDER uploads");
    check(exists(drive + "/To Computer/Chops/snare.wav"), "the MPC keeps its copy", "");

    // pick the second file, then send it to Get Producers Paid (the picker's range is 0..999)
    e->setParameter(e, kMpcBeat, 1.0f / 999.0f);
    expectText(e, kMpcBeat, "my chop", 10, "turning From MPC picks the next file");
    press(e, kToGpp);
    expectText(e, kStatus, "Sent to GPP: my chop", 20, "SEND TO GPP uploads");

    // a beat cued on the phone page becomes the Mac Beat: act as the phone and cue hat.wav
    check(std::system("sh -c 'D=$(curl -s localhost:47800/api/devices | sed -n \"s/.*\\\"name\\\":\\\"\\([^\\\"]*\\)\\\".*/\\1/p\"); "
                      "curl -sf -X POST localhost:47800/api/cue -d \"{\\\"device\\\":\\\"$D\\\",\\\"path\\\":\\\"hat.wav\\\"}\"'") == 0,
          "the phone page cues a beat", "");
    expectText(e, kStatus, "Cued from phone: hat.wav", 30, "a cue from the phone shows");
    expectText(e, kMacBeat, "hat.wav", 5, "the cued beat becomes the Mac Beat");
    e->setParameter(e, kMacBeat, 0.0f);   // back to the newest beat for the cassette tests
    expectText(e, kMacBeat, "Trap Beat 140", 5, "turning Mac Beat still works after a cue");

    // stems: the Mac copies a Beats folder to Get Producers Paid/Stems
    expectText(e, kStems, "Song B Stems (2)", 15, "Computer Stems lists the Beats folder's stems folders");
    press(e, kToStems);
    expectText(e, kStatus, "Sent 2 stems to GPP", 20, "SEND STEMS TO GPP copies the folder");

    // cassette: Mac Beat is still the 3-second test tone
    check(loudest.load() < 0.001f, "audio passes through untouched while stopped", std::to_string(loudest.load()));
    press(e, kPlay);
    expectText(e, kStatus, "/ 0:03", 15, "PLAY loads the beat and shows the time");
    {
        bool lit = false;   // the button settles within a worker tick after the duplicate tap event
        for (int t = 0; t < 20 && !(lit = e->getParameter(e, kPlay) > 0.5f); ++t)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(lit, "PLAY lights while playing", display(e, kPlay));
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        check(loudest.load() > 0.02f, "the beat plays through the track", std::to_string(loudest.load()));
        float wave = 0.0f;
        for (int b = 0; b < 32; ++b) wave += e->getParameter(e, kWave0 + b);
        check(wave > 0.0f, "waveform bars are drawn", std::to_string(wave));
        const float r1 = e->getParameter(e, kReel);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        check(e->getParameter(e, kReel) != r1, "reels turn", "");
        const float bright = e->getParameter(e, kWave0 + 15);   // just behind the playhead
        check(bright > 0.5f, "played bars turn bright", std::to_string(bright));
        const float before = e->getParameter(e, kWave0 + 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        check(e->getParameter(e, kWave0 + 20) != before, "the waveform scrolls",
              std::to_string(before) + " -> " + std::to_string(e->getParameter(e, kWave0 + 20)));
    }
    expectText(e, kStatus, "End of tape", 10, "stops at the end of the beat");
    {
        const float first = e->getParameter(e, kWave0);
        check(first < 0.5f, "the end rewinds: bars back to dim", std::to_string(first));
    }
    {
        const bool off = e->getParameter(e, kPlay) < 0.5f;
        check(off, "PLAY pops back up at the end", display(e, kPlay));
    }
    press(e, kPlay);
    expectText(e, kStatus, "/ 0:03", 10, "PLAY again rewinds and plays");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // a real second tap, not a duplicate
    press(e, kPlay);
    {
        bool off = false;   // the status line may already show something newer, so watch the button
        for (int t = 0; t < 40 && !(off = e->getParameter(e, kPlay) < 0.5f); ++t)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(off, "a second tap stops it", display(e, kPlay));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        float lit = 0.0f;
        for (int b = 0; b < 32; ++b) lit = std::max(lit, e->getParameter(e, kWave0 + b));
        check(lit < 0.5f, "stopping rewinds: no bars lit", std::to_string(lit));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    press(e, kPlay);
    expectText(e, kStatus, "0:00 /", 10, "PLAY after STOP starts from the front");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    press(e, kPlay);

    // phone queue: the test script queues hat.wav for this device once it appears
    expectFile(drive + "/From Computer/hat.wav", 30, "phone queue delivers");

    std::printf("INFO  host saw %d automate and %d display-update calls\n", automates.load(), updates.load());
    check(automates.load() > 0, "screen updates reach the host", "");

    // unload while idle must be quick
    running = false;
    audio.join();
    const auto t0 = std::chrono::steady_clock::now();
    e->dispatcher(e, effMainsChanged, 0, 0, nullptr, 0.0f);
    e->dispatcher(e, effClose, 0, 0, nullptr, 0.0f);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(secs < 5.0, "unloads promptly", std::to_string(secs) + " s");
    dlclose(lib);

    std::printf("\nResult: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
