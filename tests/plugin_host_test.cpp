// A pretend MPC: loads the real MPC Link plugin (.so), runs audio on one thread
// like the MPC does, presses its buttons, and reads its screen text, while an
// MPC Link hub runs on this machine.
//
// Build: c++ -std=c++17 -I<sdk>/framework tests/plugin_host_test.cpp -ldl -lpthread -o plugin_host_test
// Run:   MPC_PLUGIN_FOLDER="<drive>/VST2 Plugins/BeatsbyLonza - VST - MPC Link" ./plugin_host_test <plugin.so> <drive>
#include "mpc_vst2_abi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

enum { kLevel, kStatus, kMacFile, kGet, kRefresh, kPhoneQ, kMpcFile, kSend, kSendAll, kActivity };

static std::atomic<int> automates{0}, updates{0};
static std::atomic<bool> running{true};
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
            std::this_thread::sleep_for(std::chrono::microseconds(2900));
        }
    });

    expectText(e, kStatus, "On ", 15, "finds the Mac");
    expectText(e, kMacFile, "Trap Beat 140", 15, "lists the Mac's newest file");

    press(e, kGet);
    expectText(e, kActivity, "Got Trap Beat 140", 15, "GET downloads");
    check(exists(drive + "/From Computer/Trap Beat 140.wav"), "file is in From Computer", "");
    check(!exists(drive + "/From Computer/Trap Beat 140 (2).wav"), "one press = one download", "");
    {
        const bool up = e->getParameter(e, kGet) < 0.5f;
        check(up, "GET button pops back up", display(e, kGet));
    }

    expectText(e, kMpcFile, "snare", 15, "lists To Computer, newest first");
    press(e, kSendAll);
    expectText(e, kActivity, "Sent 2 files", 20, "SEND ALL uploads");
    check(exists(drive + "/To Computer/Sent/my chop #1.wav"), "sent file moved to Sent/", "");
    expectText(e, kMpcFile, "(To Computer is empty)", 15, "list empties after sending");

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
