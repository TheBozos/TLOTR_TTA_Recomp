// tta-headless: The Third Age on BozoEngine's free host, without a window. Runs the recompiled game for --frames game
// frames (0: until it ends) and reports how it went: frames reached, the end reason, and what is not native yet (unknown
// register accesses by site, library and kernel calls by name).
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
#include <string>
#include <utility>
#include <vector>

int main(int argc, char **argv)
{
    bozo::host::FreeOptions options;
    options.framePeriodCycles = 294912000 / 60;
    options.elf = TTA_DEFAULT_ELF;
    options.symbolMap = TTA_SYMBOL_MAP;
    options.iso = TTA_DEFAULT_ISO;
    options.frames = 600;
    options.printGameOutput = true;
    // The game waits for EA's sound driver (compiled, tta_audio.cpp) to answer commands before its first frame.
    options.advanceLibrariesWhileStalled = true;
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
        else
        {
            std::fprintf(stderr, "Usage: tta-headless [--frames N] [--elf FILE] [--iso FILE] [--symbols FILE]\n"
                                 "                    [--card-dir DIR] [--pad-script FILE]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    if (const char *profile = std::getenv("TTA_HOST_PROFILE"); profile && *profile)
        tta::startHostProfile(profile);

    bozo::host::FreeHost host(options);
    bozo::ee::Dispatcher d(host);
    tta::installAudio(host.libraries(), d.runtime().memory.ram());
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
    host.onFrame = [&](uint64_t f, const R5900Context *ctx)
    {
        if (f % 60 == 0)
            std::printf("[tta] frame=%llu pc=%x cycles=%llu\n", (unsigned long long)f, ctx->pc,
                        (unsigned long long)d.now());
    };

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
}
