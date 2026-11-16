// Jade Cocoon 2 (SLUS-20309) on BozoEngine: the game's recompiled code over native services. jc2 opens the window
// (SDL3, OpenGL 3.3: BozoEngine's platform layer); jc2-headless runs without one (checks, scripted runs).
#include "bozo/audio/wav.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include "jc2_functions.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef JC2_WITH_WINDOW
#include "bozo/app/app.h"
#include "bozo/app/audio_device.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/frame_pacer.h"
#include "bozo/app/video_settings.h"
#include "bozo/app/window.h"
#include <filesystem>
#endif

namespace jc2
{
    void install(Runtime &, bozo::port::Registry &);
    void installAudio(bozo::libs::Libraries &, bool trace);
    void installDraw(Runtime &, bozo::port::Registry &);
} // namespace jc2

static int run(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // nothing lost when a run ends abruptly
    bozo::host::FreeOptions options;
    options.framePeriodCycles = 294912000 / 60;
    options.elf = JC2_DEFAULT_ELF;
    options.symbolMap = JC2_SYMBOL_MAP;
    options.iso = JC2_DEFAULT_ISO;
    bool window = false;
#ifdef JC2_WITH_WINDOW
    window = true;
    bozo::app::setIdentity({"Jade Cocoon 2", "JC2_Recomp", "jc2-recomp", "SLUS_203.09"});
    bozo::app::WindowOptions windowOptions;
    windowOptions.title = "Jade Cocoon 2 - BozoEngine";
    windowOptions.width = 960;
    windowOptions.height = 720;
#endif
    options.frames = window ? 0 : 600;
    options.printGameOutput = true;
    std::string wav;     // the mixer's output recorded (16-bit stereo, 48 kHz)
    std::string dumpRam; // EE RAM at the end of the run (a fault, the frame limit): the boundary to analyse offline
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)
            options.frames = std::stoul(argv[++i]);
        else if (a == "--pad-script" && i + 1 < argc)
            options.padScript = argv[++i];
        else if (a == "--card-dir" && i + 1 < argc)
            options.cardDir = argv[++i];
        else if (a == "--elf" && i + 1 < argc)
            options.elf = argv[++i];
        else if (a == "--iso" && i + 1 < argc)
            options.iso = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            options.out = argv[++i];
        else if (a == "--wav" && i + 1 < argc)
            wav = argv[++i];
        else if (a == "--dump-ram" && i + 1 < argc)
            dumpRam = argv[++i];
        else if (a == "--profile-io")
            options.profileIo = true;
        else if (a == "--headless")
            window = false;
#ifdef JC2_WITH_WINDOW
        else if (a == "--no-vsync")
            windowOptions.vsync = false;
        else if (a == "--fullscreen")
            windowOptions.fullscreen = true;
        else if (a == "--capture-dir" && i + 1 < argc)
            windowOptions.captureDir = argv[++i];
        else if (a == "--capture-frame" && i + 1 < argc)
            windowOptions.captureFrames.push_back(std::stoull(argv[++i]));
        else if (a == "--quit-after" && i + 1 < argc)
            windowOptions.quitAfterFrames = std::stoull(argv[++i]);
#endif
        else
        {
            std::fprintf(stderr, "Usage: jc2-headless [--frames N] [--pad-script FILE] [--card-dir DIR] [--elf FILE]\n"
                                 "                    [--iso FILE] [--out FILE] [--dump-ram FILE] [--profile-io]\n");
            return a == "--help" ? 0 : 2;
        }
    }
#ifdef JC2_WITH_WINDOW
    // A played run keeps its saves: the user folder's memory cards unless --card-dir names others.
    if (window && options.cardDir.empty())
        options.cardDir = bozo::app::userDirectory().string();
#endif
    options.realTime = window;
    bozo::host::FreeHost host(options);
    bozo::ee::Dispatcher d(host);
    d.setFunctions(jc2_function_table, jc2_function_count, jc2_resume_table, jc2_resume_count);
    bozo::port::Registry registry;
    jc2::install(d.runtime(), registry);
    jc2::installDraw(d.runtime(), registry);
    // JC2_TRACE_SIF=1: the IOP modules the game asks for and the RPC services it binds.
    const bool traceSif = std::getenv("JC2_TRACE_SIF") != nullptr;
    jc2::installAudio(host.libraries(), traceSif);
    std::string error;
    if (!host.setUp(d, jc2_external_table, jc2_external_count, &registry, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    bozo::audio::WavWriter recording;
    if (!wav.empty() && !recording.open(wav))
    {
        std::fprintf(stderr, "cannot open WAV %s\n", wav.c_str());
        return 2;
    }
#ifdef JC2_WITH_WINDOW
    bozo::app::FramePacer pacer;
    pacer.setPaced(window);
    bozo::app::DevicePad devices;
    bozo::app::AudioDevice audio;
    if (window && options.padScript.empty())
        host.libraries().pad.input = &devices;
#endif
    host.onAudio = [&](uint64_t, const int16_t *pcm, size_t count)
    {
        if (recording.isOpen())
            recording.write(pcm, count);
#ifdef JC2_WITH_WINDOW
        if (window)
            audio.push(pcm, count);
#endif
    };
    host.onFrame = [&](uint64_t f, const R5900Context *ctx)
    {
#ifdef JC2_WITH_WINDOW
        pacer.frameEnd();
#endif
        if (f % 60 == 0)
            std::printf("[jc2] frame=%llu pc=%x cycles=%llu\n", (unsigned long long)f, ctx->pc,
                        (unsigned long long)d.now());
    };
    auto play = [&]() -> int
    {
#ifdef JC2_WITH_WINDOW
        if (window)
            audio.open();
#endif
        uint64_t frames = 0;
        try
        {
            frames = host.run();
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[jc2] native failure: %s\n", e.what());
            return 1;
        }
        if (!dumpRam.empty())
            if (FILE *f = std::fopen(dumpRam.c_str(), "wb"))
            {
                std::fwrite(d.runtime().memory.ram(), 1, 0x2000000u, f);
                std::fclose(f);
            }
        std::printf("[jc2] frames=%llu end=%s\n", (unsigned long long)frames, host.endReason().c_str());
        const auto &stats = host.stats();
        std::printf("[jc2] compatibility: %llu register accesses, %llu unknown; %llu native VU0 calls\n",
                    (unsigned long long)stats.ioAccesses, (unsigned long long)stats.ioUnknown,
                    (unsigned long long)stats.vu0Calls);
        std::printf("[jc2] calls: %llu kernel, %llu library, %llu port overrides\n", (unsigned long long)stats.kernelCalls,
                    (unsigned long long)stats.libraryCalls, (unsigned long long)stats.overrideCalls);
        // JC2_CALL_STATS=1: the native library calls by function, most first.
        if (std::getenv("JC2_CALL_STATS"))
        {
            std::vector<std::pair<uint64_t, std::string>> byName;
            for (const auto &[name, count] : stats.libraryByName)
                byName.emplace_back(count, name);
            std::sort(byName.rbegin(), byName.rend());
            for (const auto &[count, name] : byName)
                std::printf("[jc2:lib] %10llu %s\n", (unsigned long long)count, name.c_str());
        }
        for (const auto &[name, count] : stats.unanswered)
            std::printf("[jc2] unanswered: %s x%llu\n", name.c_str(), (unsigned long long)count);
        for (const auto &[site, counts] : stats.ioSites)
            std::printf("[jc2:io] pc=%08x address=%08x reads=%llu writes=%llu %s\n", site.first, site.second,
                        (unsigned long long)counts[0], (unsigned long long)counts[1], host.functionAt(site.first).c_str());
        return host.endReason().rfind("FAULT:", 0) == 0 ? 1 : 0;
    };
#ifdef JC2_WITH_WINDOW
    if (window)
    {
        bozo::app::Game game;
        game.run = play;
        game.stop = [&] { host.requestStop(); };
        game.framesEnded = [&] { return host.framesEnded(); };
        const int result = bozo::app::runWindow(windowOptions, game, &devices);
        recording.close();
        return result;
    }
#endif
    const int result = play();
    recording.close();
    return result;
}

int main(int argc, char **argv)
{
    try
    {
        return run(argc, argv);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "[jc2] setup failure: %s\n", e.what());
        return 1;
    }
}
