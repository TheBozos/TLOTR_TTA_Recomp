// The Third Age on BozoEngine's free host. tta-headless runs the recompiled game without a window for --frames game
// frames (0: until it ends) and reports how it went: frames reached, the end reason, and what is not native yet (unknown
// register accesses by site, library and kernel calls by name). tta (TTA_WITH_WINDOW) runs it in a window, in real
// time, with the PC's pads and sound; --headless there behaves as tta-headless.
#include "bozo/app/frame_pacer.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"
#include "tta_audio.h"
#include "tta_functions.h"
#include "tta_host_profile.h"
#include "tta_port.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef TTA_WITH_WINDOW
#include "bozo/app/app.h"
#include "bozo/app/audio_device.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/window.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#endif

int main(int argc, char **argv)
{
    bozo::host::FreeOptions options;
    options.framePeriodCycles = 294912000 / 60;
    options.elf = TTA_DEFAULT_ELF;
    options.symbolMap = TTA_SYMBOL_MAP;
    options.iso = TTA_DEFAULT_ISO;
    options.printGameOutput = true;
    // The game waits for EA's sound driver (compiled, tta_audio.cpp) to answer commands before its first frame.
    options.advanceLibrariesWhileStalled = true;
    bool window = false;
#ifdef TTA_WITH_WINDOW
    window = true;
    bozo::app::setIdentity({"The Lord of the Rings: The Third Age", "TLOTR_TTA_Recomp", "tlotr-tta-recomp", "SLUS_210.27"});
    bozo::app::WindowOptions windowOptions;
    windowOptions.title = "The Lord of the Rings: The Third Age - BozoEngine";
    windowOptions.width = 960;
    windowOptions.height = 720;
#endif
    options.frames = window ? 0 : 600;
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
        else if (a == "--symbols" && i + 1 < argc)
            options.symbolMap = argv[++i];
        else if (a == "--headless")
            window = false;
#ifdef TTA_WITH_WINDOW
        else if (a == "--capture-dir" && i + 1 < argc)
            windowOptions.captureDir = argv[++i];
        else if (a == "--capture-frame" && i + 1 < argc)
            windowOptions.captureFrames.push_back(std::stoull(argv[++i]));
        else if (a == "--fullscreen")
            windowOptions.fullscreen = true;
#endif
        else
        {
            std::fprintf(stderr, "Usage: tta [--frames N] [--headless] [--elf FILE] [--iso FILE] [--symbols FILE]\n"
                                 "           [--card-dir DIR] [--pad-script FILE] [--capture-dir DIR] [--capture-frame N]\n"
                                 "           [--fullscreen]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    if (const char *profile = std::getenv("TTA_HOST_PROFILE"); profile && *profile)
        tta::startHostProfile(profile);

    // TTA_DETERMINISTIC=1: the window build on the game's clock as tta-headless (reproducible captures, not paced).
    const bool deterministic = std::getenv("TTA_DETERMINISTIC") != nullptr;
    options.realTime = window && !deterministic;
#ifdef TTA_WITH_WINDOW
    // A played game keeps its memory cards on disk, in the user's application data (%APPDATA%\TheBozos\The Third
    // Age\cards: mc0, mc1), so a save outlives the session without --card-dir. Headless and scripted runs keep fresh
    // cards in memory unless given --card-dir, so a test never touches the player's saves. TTA_MEMORY_CARDS=1: in
    // memory for a played game too.
    if (window && options.padScript.empty() && options.cardDir.empty() && !std::getenv("TTA_MEMORY_CARDS"))
        if (char *pref = SDL_GetPrefPath("TheBozos", "The Third Age"))
        {
            options.cardDir = std::string(pref) + "cards";
            SDL_free(pref);
            std::printf("[tta] memory cards in %s\n", options.cardDir.c_str());
        }
#endif
    bozo::host::FreeHost host(options);
    bozo::ee::Dispatcher d(host);
    tta::installAudio(host.libraries(), d.runtime().memory.ram());
    // The game seeds its RNG once at boot (0x1c2e30: seconds + minutes + hours + 10 of sceCdReadClock, BCD) for
    // everything random in it (a battle's opening turn among them): the PC's clock when played, as a PS2's real-time
    // clock is; the fixed 12:00:00 for reproducible runs (TTA_DETERMINISTIC). TTA_CLOCK=host / fixed overrides.
    {
        const char *clock = std::getenv("TTA_CLOCK");
        host.libraries().cd.hostClock = clock ? std::string(clock) == "host" : options.realTime;
    }
    // TTA plays its movies the SDK's way: raw video through sceMpegAddBs from its Nodata callback, its own pacing.
    host.libraries().mpegOptions = {.referenceLayout = false, .paced = false, .sdkNodata = true};
    d.setFunctions(tta_function_table, tta_function_count, tta_resume_table, tta_resume_count);
    bozo::port::Registry registry;
    tta::install(d.runtime(), registry);
    std::string error;
    if (!host.setUp(d, tta_external_table, tta_external_count, &registry, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    bozo::app::FramePacer pacer;
    pacer.setPaced(window && !deterministic);
    host.onFrame = [&](uint64_t f, const R5900Context *ctx)
    {
        pacer.frameEnd();
        // TTA_RAM_DUMP=<game frame>[,<game frame>...]: the EE's 32 MB at the end of those game frames, to
        // build/ram_<frame>.bin (compared with a PCSX2 save state's eeMemory.bin).
        if (static const char *dumps = std::getenv("TTA_RAM_DUMP"); dumps && *dumps)
        {
            static uint32_t last = 0;
            uint32_t game = 0;
            std::memcpy(&game, d.runtime().memory.ram() + 0x3b5720, 4);
            if (game != last)
            {
                last = game;
                const std::string list = std::string(",") + dumps + ",";
                if (list.find("," + std::to_string(game) + ",") != std::string::npos)
                    if (FILE *out = std::fopen(("build/ram_" + std::to_string(game) + ".bin").c_str(), "wb"))
                    {
                        std::fwrite(d.runtime().memory.ram(), 1, 32u << 20, out);
                        std::fclose(out);
                        std::printf("[tta] RAM of game frame %u written\n", game);
                    }
            }
        }
        if (f % 60 == 0)
        {
            // The game's own frame counter (0x3b5720) beside the host's: the TTA_*_DUMP probes key on the game's.
            uint32_t game = 0;
            std::memcpy(&game, d.runtime().memory.ram() + 0x3b5720, 4);
            std::printf("[tta] frame=%llu game=%u pc=%x cycles=%llu\n", (unsigned long long)f, game, ctx->pc,
                        (unsigned long long)d.now());
        }
    };
#ifdef TTA_WITH_WINDOW
    bozo::app::DevicePad devices;
    bozo::app::AudioDevice audio;
    if (window && options.padScript.empty())
        host.libraries().pad.input = &devices;
    // A scripted run (a test, a capture) never takes the focus from what the player is doing on the same PC, and stays
    // silent: the window shows without activating, the sound is not opened.
    const bool scripted = window && !options.padScript.empty();
    if (scripted)
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    host.onAudio = [&](uint64_t, const int16_t *pcm, size_t count)
    {
        if (window && !scripted)
            audio.push(pcm, count);
    };
#endif

    auto run = [&]() -> int
    {
#ifdef TTA_WITH_WINDOW
        if (window && !scripted)
            audio.open();
#endif
        uint64_t frames = 0;
        try
        {
            frames = host.run();
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[tta] native failure: %s\n", e.what());
            return 1;
        }
        std::printf("[tta] frames=%llu end=%s\n", (unsigned long long)frames, host.endReason().c_str());
        const auto &stats = host.stats();
        std::printf("[tta] compatibility: %llu register accesses, %llu unknown; %llu native VU0 calls\n",
                    (unsigned long long)stats.ioAccesses, (unsigned long long)stats.ioUnknown,
                    (unsigned long long)stats.vu0Calls);
        std::printf("[tta] calls: %llu kernel, %llu library, %llu port overrides\n", (unsigned long long)stats.kernelCalls,
                    (unsigned long long)stats.libraryCalls, (unsigned long long)stats.overrideCalls);
        std::vector<std::pair<uint64_t, std::string>> byName;
        for (const auto &[name, count] : stats.libraryByName)
            byName.emplace_back(count, name);
        std::sort(byName.rbegin(), byName.rend());
        for (const auto &[count, name] : byName)
            std::printf("[tta:lib] %10llu %s\n", (unsigned long long)count, name.c_str());
        for (const auto &[site, counts] : stats.ioSites)
            std::printf("[tta:io] pc=%08x address=%08x reads=%llu writes=%llu %s\n", site.first, site.second,
                        (unsigned long long)counts[0], (unsigned long long)counts[1], host.functionAt(site.first).c_str());
        return host.endReason().rfind("FAULT:", 0) == 0 ? 1 : 0;
    };
#ifdef TTA_WITH_WINDOW
    if (window)
    {
        bozo::app::Game game;
        game.run = run;
        game.stop = [&] { host.requestStop(); };
        game.framesEnded = [&] { return host.framesEnded(); };
        return bozo::app::runWindow(windowOptions, game, &devices);
    }
#endif
    return run();
}
