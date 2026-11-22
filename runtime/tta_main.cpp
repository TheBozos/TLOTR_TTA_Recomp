// The Third Age on BozoEngine's free host. tta-headless runs the recompiled game without a window for --frames game
// frames (0: until it ends) and reports how it went: frames reached, the end reason, and what is not native yet (unknown
// register accesses by site, library and kernel calls by name). tta (TTA_WITH_WINDOW) runs it in a window, in real
// time, with the PC's pads and sound; --headless there behaves as tta-headless.
#include "bozo/app/frame_pacer.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"
#include "bozo/render/renderer.h"
#include "tta_audio.h"
#include "tta_functions.h"
#include "tta_host_profile.h"
#include "tta_port.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifdef TTA_WITH_WINDOW
#include "bozo/app/app.h"
#include "bozo/app/audio_device.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/video_settings.h"
#include "bozo/app/window.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_messagebox.h>
#include <fstream>
#ifdef BOZO_SETTINGS_UI
#include "bozo/app/settings_tabs.h"
#include <imgui.h>
#endif
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
    std::string dumpRam; // --dump-ram FILE: the EE's 32 MB at the end of the run (save states are checked with it)
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
        else if (a == "--load-state" && i + 1 < argc)
            options.loadState = argv[++i];
        else if (a == "--load-state-at" && i + 1 < argc)
            options.loadStateFrame = std::stoul(argv[++i]);
        else if (a == "--save-state" && i + 1 < argc)
            options.saveState = argv[++i];
        else if (a == "--save-state-at" && i + 1 < argc)
            options.saveStateFrame = std::stoul(argv[++i]);
        else if (a == "--dump-ram" && i + 1 < argc)
            dumpRam = argv[++i];
        else if (a == "--headless")
            window = false;
        // Debug (tta_debug.cpp): --warp MAP[@FRAME], --encounter ID[@FRAME], --group ID[@FRAME], --game-option NAME[=VALUE].
        else if (a == "--warp" && i + 1 < argc)
        {
            if (!tta::requestWarp(argv[++i]))
                return 2;
        }
        else if (a == "--encounter" && i + 1 < argc)
            tta::requestEncounter(argv[++i]);
        else if (a == "--group" && i + 1 < argc)
            tta::requestGroup(argv[++i]);
        else if (a == "--game-option" && i + 1 < argc)
            tta::addGameOption(argv[++i]);
#ifdef TTA_WITH_WINDOW
        else if (a == "--capture-dir" && i + 1 < argc)
            windowOptions.captureDir = argv[++i];
        else if (a == "--capture-frame" && i + 1 < argc)
            windowOptions.captureFrames.push_back(std::stoull(argv[++i]));
        else if (a == "--fullscreen")
            windowOptions.fullscreen = true;
        else if (a == "--widescreen")
            tta::setWidescreen(true);
#endif
        else
        {
            std::fprintf(stderr, "Usage: tta [--frames N] [--headless] [--elf FILE] [--iso FILE] [--symbols FILE]\n"
                                 "           [--card-dir DIR] [--pad-script FILE] [--capture-dir DIR] [--capture-frame N]\n"
                                 "           [--fullscreen] [--widescreen] [--dump-ram FILE]\n"
                                 "           [--load-state FILE [--load-state-at FRAME]] [--save-state FILE --save-state-at FRAME]\n"
                                 "           [--warp MAP[@FRAME]] [--encounter ID[@FRAME]] [--group ID[@FRAME]] [--game-option NAME[=VALUE]]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    if (const char *profile = std::getenv("TTA_HOST_PROFILE"); profile && *profile)
        tta::startHostProfile(profile);
    // TTA_SKIP_LOGOS=1: the opening logos skippable in any run (the Game tab's setting, read from tta.ini when played).
    if (const char *logos = std::getenv("TTA_SKIP_LOGOS"); logos && *logos && *logos != '0')
        tta::setSkippableLogos(true);
#ifdef TTA_WITH_WINDOW
    // Widescreen (tta_port.h): a 16:9 picture, the game's 4:3 view a third wider; the window build only.
    if (const char *wide = std::getenv("TTA_WIDESCREEN"); wide && *wide && *wide != '0')
        tta::setWidescreen(true);
    if (!window)
        tta::setWidescreen(false);
    // A played game takes the player's choices from tta.ini in the user folder (the F2 window's Game tab writes it):
    // widescreen (unless --widescreen or TTA_WIDESCREEN asked already) and the skippable logos. Scripted and
    // TTA_DETERMINISTIC runs do not read it.
    const std::filesystem::path portSettings = bozo::app::userDirectory() / "tta.ini";
    const bool played = window && options.padScript.empty() && !std::getenv("TTA_DETERMINISTIC");
    if (played)
    {
        std::ifstream file(portSettings);
        std::string line;
        const auto on = [&line](const char *key)
        { return line.rfind(key, 0) == 0 && line.find("on", std::strlen(key)) != std::string::npos; };
        while (std::getline(file, line))
        {
            if (on("widescreen") && !std::getenv("TTA_WIDESCREEN"))
                tta::setWidescreen(true);
            if (on("skip_logos"))
                tta::setSkippableLogos(true);
        }
    }
    [[maybe_unused]] const auto savePortSettings = [portSettings, played]
    {
        if (!played)
            return;
        std::ofstream file(portSettings, std::ios::trunc);
        file << "# The Third Age's own settings (the F2 window's Game tab writes this file).\n"
                "# widescreen: on (16:9, the 3D a third wider) or off (4:3, as the console; --widescreen and "
                "TTA_WIDESCREEN=1 also turn it on).\n"
                "# skip_logos: on (START ends the EA, THX and New Line logos at boot) or off (as the console).\n"
             << "widescreen = " << (tta::widescreen() ? "on" : "off") << "\n"
             << "skip_logos = " << (tta::skippableLogos() ? "on" : "off") << "\n";
    };
    if (tta::widescreen())
    {
        windowOptions.aspect = 16.0f / 9.0f;
        windowOptions.width = 1280;
    }
    // A played game (not scripted, not TTA_DETERMINISTIC: their captures stay reproducible) takes the player's video
    // settings: BozoEngine's video.ini in the user folder, edited in the Video tab of the settings window (F2): the
    // window's size, windowed / full screen / borderless (F11 or Alt+Enter), the render scale, texture upscaling.
    // --fullscreen asks borderless full screen for this run. TTA_NO_VIDEO_SETTINGS=1: the fixed window as before.
    if (window && options.padScript.empty() && !std::getenv("TTA_DETERMINISTIC") && !std::getenv("TTA_NO_VIDEO_SETTINGS"))
    {
        bozo::app::video::loadAtStartup(windowOptions.width, windowOptions.height);
        if (windowOptions.fullscreen)
            bozo::app::video::edited().mode = bozo::app::video::WindowMode::Borderless;
        windowOptions.videoSettings = true;
    }
    // TTA_FRAME_RATE=<n> (or monitor): the window's frame rate in any window run, scripted ones included (the video
    // settings' frame rate otherwise); above the game's 60 the frames between are drawn interpolated (tta_3d.cpp keys
    // the 3D draws). Not with TTA_DETERMINISTIC's lockstep.
    if (const char *rate = std::getenv("TTA_FRAME_RATE"); rate && *rate && window)
    {
        int parsed = 0;
        if (bozo::app::video::parseFrameRate(rate, parsed))
            bozo::render::Renderer::instance().setFrameRate(parsed);
    }
#ifdef BOZO_SETTINGS_UI
    // The F2 window's Game tab, kept in tta.ini for the next played game. Widescreen, live (the window thread presents
    // with the aspect it sets; the draws read tta::widescreen each frame); the skippable logos (read at a movie's start).
    bozo::app::ui::registerTab("Game",
                               [&windowOptions, savePortSettings]
                               {
                                   bool wide = tta::widescreen();
                                   if (ImGui::Checkbox("Widescreen (16:9)", &wide))
                                   {
                                       tta::setWidescreen(wide);
                                       windowOptions.aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
                                       savePortSettings();
                                   }
                                   ImGui::TextDisabled("The 3D a third wider than the console's view; the HUD, menus and");
                                   ImGui::TextDisabled("movies keep their shape in the middle. Applies at once.");
                                   ImGui::Spacing();
                                   bool logos = tta::skippableLogos();
                                   if (ImGui::Checkbox("Skippable logos", &logos))
                                   {
                                       tta::setSkippableLogos(logos);
                                       savePortSettings();
                                   }
                                   ImGui::TextDisabled("START ends the EA, THX and New Line logos at boot (unskippable on");
                                   ImGui::TextDisabled("the console). From the next start.");
                               });
#endif
    // PS2X_TEXTURE_SMOOTHING (off, trilinear, 2x .. 16x) for the runs without the video settings too (scripted ones).
    if (const char *smoothing = std::getenv("PS2X_TEXTURE_SMOOTHING"); smoothing && *smoothing && window && !windowOptions.videoSettings)
    {
        int level = 0;
        if (bozo::app::video::parseTextureSmoothing(smoothing, level))
            bozo::render::Renderer::instance().setTextureSmoothing(level);
    }
#endif

    // What stops the game before it starts, on stderr and, for a played game, in a message box (started from the
    // desktop there is no console to read).
    const auto fail = [&](const std::string &message)
    {
        std::fprintf(stderr, "%s\n", message.c_str());
#ifdef TTA_WITH_WINDOW
        if (window && options.padScript.empty())
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "The Third Age", message.c_str(), nullptr);
#endif
        return 2;
    };
    // The disc and its executable, checked first: without the disc image the game started and stalled at its first
    // read, a black window and no word of why.
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(options.iso, ec))
            return fail("The disc image was not found:\n  " + options.iso +
                        "\n\nGive the path of your own copy of the game (SLUS-21027) with --iso FILE, or configure the "
                        "build with -DTTA_ISO=FILE.");
        if (!std::filesystem::is_regular_file(options.elf, ec))
            return fail("The game's executable was not found:\n  " + options.elf +
                        "\n\nExtract the disc first (scripts/extract_iso.sh <iso>), or give it with --elf FILE.");
    }

    // TTA_DETERMINISTIC=1: the window build on the game's clock as tta-headless (reproducible captures, not paced).
    const bool deterministic = std::getenv("TTA_DETERMINISTIC") != nullptr;
    options.realTime = window && !deterministic;
    tta::setRealTime(options.realTime);
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
    options.gameTag = "SLUS-21027";
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
    // Save states: TTA's own part (the engine saves its runtime, libraries and host): the sound driver's host and its
    // waiting commands, the resources the game loaded and where it placed its textures.
    host.stateSections()->add({"tta",
                               [&](bozo::state::Archive &a)
                               {
                                   tta::audioState(a);
                                   tta::twoDState(a);
                               },
                               [] { return tta::audioUnsaveable(); }});
    std::string error;
    if (!host.setUp(d, tta_external_table, tta_external_count, &registry, error))
        return fail(error);
    bozo::app::FramePacer pacer;
    pacer.setPaced(window && !deterministic);
    // Exact captures (TTA_DETERMINISTIC with --capture-frame): the game thread holds at the end of each capture frame
    // until the window has captured that picture. Unpaced, the window otherwise presented whenever it came round, and
    // a capture landed a frame or two late, by chance (frame_005452 for 5451): no two runs captured the same frame.
    // And every frame in lockstep (BozoEngine's WindowOptions::lockstep): the game thread holds after each frame until
    // the window has presented what it published, and the window presents each frame once. The Nazgul Scream's
    // ghosting keeps the previous picture: with frames skipped or shown twice by the window's timing, runs differed by
    // up to 0.9 levels at its capture. Vsync off: the presents follow the game, not the monitor.
    std::atomic<uint64_t> captured{0}, presented{0};
    std::vector<uint64_t> holdAt;
    bool lockstep = false;
#ifdef TTA_WITH_WINDOW
    if (window && deterministic)
    {
        holdAt = windowOptions.captureFrames;
        std::sort(holdAt.begin(), holdAt.end());
        windowOptions.onCaptured = [&captured](uint64_t frames) { captured.store(frames, std::memory_order_release); };
        lockstep = !std::getenv("TTA_NO_LOCKSTEP");
        windowOptions.lockstep = lockstep;
        windowOptions.vsync = !lockstep;
        windowOptions.onPresented = [&presented](uint64_t publishes) { presented.store(publishes, std::memory_order_release); };
    }
#endif
    host.onFrame = [&](uint64_t f, const R5900Context *ctx)
    {
        pacer.frameEnd();
        const auto hold = [](const std::atomic<uint64_t> &counter, uint64_t until)
        {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (counter.load(std::memory_order_acquire) < until && std::chrono::steady_clock::now() < limit)
                std::this_thread::yield();
        };
        if (lockstep)
            hold(presented, bozo::render::Renderer::instance().publishes());
        // f is the frame that just ended, counted from 0: the window's frame count is now f + 1.
        if (std::binary_search(holdAt.begin(), holdAt.end(), f + 1))
            hold(captured, f + 1);
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
        // TTA_STALL_EVERY=<host frames>: a 90 ms stall that often (a slow picture or a load), to test the pacing.
        if (static const char *stall = std::getenv("TTA_STALL_EVERY"); stall && *stall && f % std::strtoull(stall, nullptr, 10) == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
        // TTA_REPORT_AT=<game frame>: F10's frame report (below) from a script.
        if (static const char *at = std::getenv("TTA_REPORT_AT"); at && *at)
        {
            static bool done = false;
            uint32_t game = 0;
            std::memcpy(&game, d.runtime().memory.ram() + 0x3b5720, 4);
            if (!done && game >= static_cast<uint32_t>(std::atoi(at)))
            {
                done = true;
                tta::requestFrameReport();
            }
        }
#ifdef TTA_WITH_WINDOW
        // F10 (F7 loads a save state): a report of this moment for a bug seen while playing: the EE's RAM to build/ram_key_<game frame>.bin
        // and one game frame of [tta:report] lines (every character draw's mode, lights and blend state).
        if (window)
        {
            static bool held = false;
            const bool *keys = SDL_GetKeyboardState(nullptr);
            const bool down = keys && keys[SDL_SCANCODE_F10];
            if (down && !held)
            {
                uint32_t game = 0;
                std::memcpy(&game, d.runtime().memory.ram() + 0x3b5720, 4);
                const std::string path = "build/ram_key_" + std::to_string(game) + ".bin";
                if (FILE *out = std::fopen(path.c_str(), "wb"))
                {
                    std::fwrite(d.runtime().memory.ram(), 1, 32u << 20, out);
                    std::fclose(out);
                }
                std::printf("[tta:report] F10 at game frame %u: RAM in %s\n", game, path.c_str());
                tta::requestFrameReport();
            }
            held = down;
        }
#endif
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
    // TTA_AUDIO_STATS=1: the sound queue's level, latency and underruns every 2 s (stderr). TTA_SCRIPTED_SOUND=1: a
    // scripted run opens the sound too (with SDL_AUDIO_DRIVER=dummy, a device that plays nothing at the real rate: the
    // underruns of a test under load, silently).
    bozo::app::AudioOptions audioOptions;
    // A 64 ms queue (the engine's default 32): a turn-based game loses nothing to the extra latency, and the sound
    // rides through the game thread's dips when other programs load the PC (no underrun in a test with two other
    // copies of the game running flat out; at 32 ms, 138 in a minute).
    audioOptions.latencyMs = 64.0;
    audioOptions.maxQueueMs = 160.0;
    audioOptions.stats = std::getenv("TTA_AUDIO_STATS") != nullptr;
    // TTA_AUDIO_LATENCY_MS / TTA_AUDIO_MAX_QUEUE_MS: the queue's target and its cap (testing).
    if (const char *l = std::getenv("TTA_AUDIO_LATENCY_MS"))
        audioOptions.latencyMs = std::atof(l);
    if (const char *m = std::getenv("TTA_AUDIO_MAX_QUEUE_MS"))
        audioOptions.maxQueueMs = std::atof(m);
    bozo::app::AudioDevice audio(audioOptions);
    if (window && options.padScript.empty())
        host.libraries().pad.input = &devices;
    // A scripted run (a test, a capture) never takes the focus from what the player is doing on the same PC, and stays
    // silent: the window shows without activating, the sound is not opened.
    const bool scripted = window && !options.padScript.empty();
    if (scripted)
    {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        // Nothing on the player's screen either: the window stays hidden and captures read the renderer's own picture
        // (TTA_SHOW_WINDOW=1 shows it, captures then from the window as before).
        if (!std::getenv("TTA_SHOW_WINDOW"))
        {
            windowOptions.hidden = true;
            windowOptions.captureScene = true;
        }
    }
    const bool sound = window && (!scripted || std::getenv("TTA_SCRIPTED_SOUND"));
    host.onAudio = [&](uint64_t frame, const int16_t *pcm, size_t count)
    {
        // TTA_AUDIO_RATE=1: the sound made, every 60 game frames: frames, samples, wall time (probe).
        if (static const bool rate = std::getenv("TTA_AUDIO_RATE") != nullptr; rate)
        {
            static uint64_t firstFrame = frame, samples = 0, calls = 0, biggest = 0;
            static auto start = std::chrono::steady_clock::now();
            samples += count;
            ++calls;
            biggest = std::max<uint64_t>(biggest, count);
            if (frame - firstFrame >= 60)
            {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                std::fprintf(stderr, "[audio-rate] frames %llu..%llu: %llu samples in %llu calls (largest %llu), %.0f ms wall: %.0f per game second, %.0f per wall second%c",
                             (unsigned long long)firstFrame, (unsigned long long)frame, (unsigned long long)samples,
                             (unsigned long long)calls, (unsigned long long)biggest, ms,
                             samples * 60.0 / double(frame - firstFrame), samples * 1000.0 / ms, 10);
                firstFrame = frame;
                samples = calls = biggest = 0;
                start = std::chrono::steady_clock::now();
            }
        }
        if (sound)
            audio.push(pcm, count);
    };
#endif

    auto run = [&]() -> int
    {
#ifdef TTA_WITH_WINDOW
        if (sound)
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
        if (!dumpRam.empty())
            if (FILE *out = std::fopen(dumpRam.c_str(), "wb"))
            {
                std::fwrite(d.runtime().memory.ram(), 1, 32u << 20, out);
                std::fclose(out);
            }
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
        // F5 saves the slot, F7 loads it, F6 / Shift+F6 pick it (0-9); the "Save states" tab in F2.
        game.saveState = [&](int slot) { host.requestSave(bozo::app::stateSlotPath(slot)); };
        game.loadState = [&](int slot) { host.requestLoad(bozo::app::stateSlotPath(slot)); };
        host.onStateMessage = [](bool, const std::string &message)
        {
            std::fprintf(stderr, "[state] %s\n", message.c_str());
            bozo::app::notify(message);
        };
        return bozo::app::runWindow(windowOptions, game, &devices);
    }
#endif
    return run();
}
