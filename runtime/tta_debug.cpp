// Debug tools built on what the game itself has: start any of the current map's battles, warp to any map, and give the
// game its developers' command-line options. In the window build they are the F2 window's "Debug" tab; scripted runs
// use --warp / --encounter / --game-option (or the TTA_WARP / TTA_ENCOUNTER / TTA_GAME_OPTIONS variables).
//
// Battles. The level scripts' Lua function StartEncounter (C side 0x268668) calls 0x17f040(id) -> 0x15b6c8(id), which
// hands the id to the game mode's state machine (*(0x3a4950)). The ids are the current map's entity records: *(0x3dff84)
// is the map's record table ("manC", count at +0xc, records of 0x20 bytes from +0x10: the id, then the name). Its
// encounters are the records named Enc<n>[a-z]_<Name> (Enc01_Nazgul = 2109, Enc11_WargCamp01 = 2118 in Eregion);
// the random battles' _EP (placements) and _ET (enemy groups) records alone start an empty battle (not listed).
//
// Maps. The map index (0x35bd0c, 0..13) chooses among the paths at 0x35bc88 (e98c/e98c03 ... e03c/e03c04); 0x190130(i)
// sets it, and the chapter message 0 (category 0x7d0, handler 0x18f420: {0x7d0, 0, number, arguments}) loads the map
// at that index through 0x18f318 (SetLevel 0x190040, Data/Game/<path>.scp into 0x35bcc8): the game's loading screen,
// then the map's own opening (its films, its start position). Indices 6 (e99c01) and 13 (e03c04) have no files on the
// disc; 0 and 1 are the developers' test maps (e98c03 went back to the title, e98c02 played Eregion's films).
//
// Both run from the main loop's wait on the graphics paths (0x216118, once a frame, see tta_port.cpp's busyPaths), as
// calls into the game with the caller's registers kept.
//
// Command-line options: 0x140670 parses the game's argv (argc at 0x37a728, argv at 0x37a72c) into an option table:
// every "-name" with the next argument as its value when that does not start with '-' (else ""). The table is
// 0x3d70b0: a 0x2000-byte string pool from +1 (used bytes at +0x2004), up to 64 entries {name without '-', value} from
// +0x2008, their count at +0x2208. 0x1a0820(name, default) reads it (the name's leading '-' skipped). Options the code
// asks for: "level" and "file" (0x191360, only when no map is set, which never happens), "encounters_only" (0x160b88,
// 0x1c3af4), "encounters_only_noheal". With any option given the game skips its opening films.
#include "tta_port.h"

#include "tta_functions.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef BOZO_SETTINGS_UI
#include "bozo/app/settings_tabs.h"
#include <imgui.h>
#endif

namespace tta
{
    namespace
    {
        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & 0x1ffffffu), 4);
            return v;
        }
        void write32(uint8_t *rdram, uint32_t address, uint32_t v) { std::memcpy(rdram + (address & 0x1ffffffu), &v, 4); }
        uint32_t gameFrame(const uint8_t *rdram) { return read32(rdram, 0x3b5720); }

        // ---- The maps ----

        struct MapInfo
        {
            const char *path, *region; // region: the game's map_e<n>c<m>_<name> label, spelled out
        };
        constexpr MapInfo kMaps[14] = {
            {"e98c/e98c03", "Test map (back to the title)"},
            {"e98c/e98c02", "Test map"},
            {"e01c/e01c01", "Eregion"},
            {"e01c/e01c02", "Western Moria"},
            {"e01c/e01c04", "Eastern Moria"},
            {"e02c/e02c01", "Eastemnet Gullies"},
            {"e99c/e99c01", nullptr}, // not on the disc
            {"e02c/e02c03", "Rohan"},
            {"e02c/e02c04", "(e02c04)"},
            {"e02c/e02c05", "Helm's Deep"},
            {"e03c/e03c01", "Osgiliath"},
            {"e03c/e03c02", "Minas Tirith"},
            {"e03c/e03c03", "Pelennor Fields"},
            {"e03c/e03c04", nullptr}, // not on the disc
        };
        constexpr uint32_t kMapIndex = 0x35bd0cu;

        // A map by index, path ("e02c05", "e02c/e02c05") or region name (any case, "helms deep" or "helm's deep").
        int mapByName(const std::string &text)
        {
            const auto squash = [](const std::string &s)
            {
                std::string out;
                for (char c : s)
                    if (std::isalnum(static_cast<unsigned char>(c)))
                        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return out;
            };
            if (!text.empty() && std::isdigit(static_cast<unsigned char>(text[0])))
                return std::atoi(text.c_str());
            const std::string want = squash(text);
            for (int i = 0; i < 14; ++i)
            {
                const std::string path = kMaps[i].path;
                if (!kMaps[i].region)
                    continue;
                if (want == squash(path) || want == squash(path.substr(path.find('/') + 1)) || want == squash(kMaps[i].region))
                    return i;
            }
            return -1;
        }

        // ---- The current map's encounters ----

        struct Encounter
        {
            int32_t id;
            std::string name;
        };
        constexpr uint32_t kRecordTable = 0x3dff84u;

        bool isEncounterName(const std::string &n)
        {
            // Enc<digits>[letter]_<Capital>...
            size_t i = 3;
            if (n.compare(0, 3, "Enc") != 0)
                return false;
            const size_t digits = i;
            while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i])))
                ++i;
            if (i == digits)
                return false;
            if (i < n.size() && std::islower(static_cast<unsigned char>(n[i])))
                ++i;
            if (i + 1 >= n.size() || n[i] != '_' || !std::isupper(static_cast<unsigned char>(n[i + 1])))
                return false;
            for (const char *skip : {"Place", "EncounterParty", "Trigger", "Override", "Marker", "Path", "Volume", "Point"})
                if (n.find(skip) != std::string::npos)
                    return false;
            const size_t cam = n.rfind("_Cam");
            return !(cam != std::string::npos && (cam + 4 == n.size() || std::isdigit(static_cast<unsigned char>(n[cam + 4])) ||
                                                  n[cam + 4] == '_'));
        }

        std::vector<Encounter> readEncounters(const uint8_t *rdram)
        {
            std::vector<Encounter> list;
            const uint32_t table = read32(rdram, kRecordTable);
            if (!table || table >= 0x2000000u || std::memcmp(rdram + table, "manC", 4) != 0)
                return list;
            const uint32_t count = read32(rdram, table + 0xcu);
            for (uint32_t k = 0; k < count && k < 0x4000u; ++k)
            {
                const uint32_t at = table + 0x10u + k * 0x20u;
                char name[29] = {};
                std::memcpy(name, rdram + ((at + 4u) & 0x1ffffffu), 28);
                if (isEncounterName(name))
                    list.push_back({static_cast<int32_t>(read32(rdram, at)), name});
            }
            return list;
        }

        // ---- Requests, served on the game's thread ----

        struct Request
        {
            enum Kind
            {
                kEncounter,
                kWarp
            } kind;
            int32_t value;
            uint32_t atFrame;
        };
        std::mutex g_mutex;
        std::deque<Request> g_requests;
        std::vector<Encounter> g_encounters; // the current map's, refreshed by the game thread
        std::atomic<int> g_currentMap{-1};
        std::string g_last; // the last action's result, for the tab

        void request(Request::Kind kind, int32_t value, uint32_t atFrame = 0)
        {
            std::lock_guard lock(g_mutex);
            g_requests.push_back({kind, value, atFrame});
        }

        // A game function called from a hook: arguments in a0..a3, the caller's registers kept when it returns.
        bool callGame(uint8_t *rdram, R5900Context *ctx, Runtime *rt, void (*fn)(uint8_t *, R5900Context *, Runtime *),
                      std::initializer_list<uint32_t> args, uint32_t stackBelow = 0, uint32_t *result = nullptr)
        {
            const R5900Context saved = *ctx;
            constexpr uint32_t kReturn = 0x216118u; // the hook's own address: only compared on return
            int r = 4;
            for (uint32_t a : args)
                ctx->r[r++].ud[0] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)));
            if (stackBelow)
                ctx->r[29].ud[0] = stackBelow;
            ctx->r[31].ud[0] = kReturn;
            fn(rdram, ctx, rt);
            const bool returned = ctx->pc == kReturn && !ctx->leaving;
            if (result)
                *result = ctx->r[2].uw[0];
            if (returned)
                *ctx = saved;
            return returned;
        }

        void note(const std::string &text)
        {
            std::printf("[tta:debug] %s\n", text.c_str());
            std::fflush(stdout);
            std::lock_guard lock(g_mutex);
            g_last = text;
        }

        void startEncounter(uint8_t *rdram, R5900Context *ctx, Runtime *rt, int32_t id)
        {
            const bool ok = callGame(rdram, ctx, rt, func_0x17f040, {static_cast<uint32_t>(id)});
            note("StartEncounter(" + std::to_string(id) + ") at game frame " + std::to_string(gameFrame(rdram)) +
                 (ok ? "" : " (did not return)"));
        }

        void warp(uint8_t *rdram, R5900Context *ctx, Runtime *rt, int32_t index)
        {
            if (index < 0 || index >= 14 || !kMaps[index].region)
            {
                note("no map " + std::to_string(index));
                return;
            }
            callGame(rdram, ctx, rt, func_0x190130, {static_cast<uint32_t>(index)});
            // Chapter message 0 on the guest stack, below the caller's frame.
            const uint32_t msg = (ctx->r[29].uw[0] - 0x200u) & ~0xfu, arguments = msg + 0x40u;
            write32(rdram, msg, 0x7d0u);
            write32(rdram, msg + 4u, 0u);
            write32(rdram, msg + 8u, 0u);
            write32(rdram, msg + 12u, arguments);
            std::memset(rdram + arguments, 0, 0x40);
            uint32_t handled = 0;
            const bool ok = callGame(rdram, ctx, rt, func_0x18f420, {msg}, msg - 0x100u, &handled);
            note(std::string("warp to ") + kMaps[index].region + " (" + kMaps[index].path + ")" +
                 (ok && handled ? "" : " (not handled)"));
        }

        // 0x216118: the graphics paths still busy (the main loop waits on it once a frame). There are none: 0. The
        // requests are served here.
        void mainLoopTick(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t frame = gameFrame(rdram);
            std::vector<Request> due;
            {
                std::lock_guard lock(g_mutex);
                for (auto it = g_requests.begin(); it != g_requests.end();)
                    if (frame >= it->atFrame)
                    {
                        due.push_back(*it);
                        it = g_requests.erase(it);
                    }
                    else
                        ++it;
            }
            for (const Request &r : due)
                if (r.kind == Request::kEncounter)
                    startEncounter(rdram, ctx, rt, r.value);
                else
                    warp(rdram, ctx, rt, r.value);
            // The current map's encounters, twice a second (the table moves when a map loads).
            static uint32_t lastTable = 0, lastFrame = 0;
            const uint32_t table = read32(rdram, kRecordTable);
            if (table != lastTable || frame - lastFrame >= 30u)
            {
                lastTable = table;
                lastFrame = frame;
                std::vector<Encounter> list = readEncounters(rdram);
                g_currentMap.store(static_cast<int>(read32(rdram, kMapIndex)));
                std::lock_guard lock(g_mutex);
                g_encounters.swap(list);
            }
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }

        // ---- Command-line options ----

        constexpr uint32_t kTable = 0x3d70b0u, kPoolUsed = kTable + 0x2004u, kEntries = kTable + 0x2008u,
                           kCount = kTable + 0x2208u, kPoolSize = 0x2000u, kMaxEntries = 64u;

        std::vector<std::pair<std::string, std::string>> &options()
        {
            static std::vector<std::pair<std::string, std::string>> list;
            return list;
        }

        // The pool's copy of text (0x1a0560's way: at pool + used, used += length + 1), or 0 when full.
        uint32_t poolString(uint8_t *rdram, const std::string &text)
        {
            const uint32_t used = read32(rdram, kPoolUsed);
            if (used + text.size() + 1 >= kPoolSize)
                return 0;
            const uint32_t at = kTable + 1u + used;
            std::memcpy(rdram + at, text.c_str(), text.size() + 1);
            write32(rdram, kPoolUsed, used + static_cast<uint32_t>(text.size()) + 1u);
            return at;
        }

        void setOption(uint8_t *rdram, const std::string &name, const std::string &value)
        {
            const uint32_t count = read32(rdram, kCount);
            uint32_t entry = 0;
            // An option the game's argv already gave is replaced (0x1a06e0 does the same through 0x1a0628).
            for (uint32_t i = 0; i < count && i < kMaxEntries; ++i)
            {
                const uint32_t at = kEntries + i * 8u, n = read32(rdram, at);
                if (n && std::strcmp(reinterpret_cast<const char *>(rdram + (n & 0x1ffffffu)), name.c_str()) == 0)
                    entry = at;
            }
            if (!entry)
            {
                if (count >= kMaxEntries)
                    return;
                entry = kEntries + count * 8u;
                write32(rdram, kCount, count + 1u);
            }
            const uint32_t n = poolString(rdram, name), v = poolString(rdram, value);
            if (!n || !v)
                return;
            write32(rdram, entry, n);
            write32(rdram, entry + 4u, v);
            std::printf("[tta:debug] game option -%s %s\n", name.c_str(), value.c_str());
        }

        // 0x140670, the argv parse (from 0x140760 at boot, after 0x1a0548 cleared the table).
        void parseArguments(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            func_0x140670(rdram, ctx, rt);
            if (ctx->pc != ra)
                return;
            for (const auto &[name, value] : options())
                setOption(rdram, name, value);
            ctx->pc = ra;
        }

        // TTA_ENCOUNTER_TRACE=1: every encounter start (0x15b6c8(id)) with its id and caller.
        void encounterTrace(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            std::printf("[tta:debug] encounter start id %d from %x, game frame %u\n", ctx->r[4].sw[0], ctx->r[31].uw[0],
                        gameFrame(rdram));
            std::fflush(stdout);
            func_0x15b6c8(rdram, ctx, rt);
        }

        // "<value>[@<game frame>]"
        std::pair<std::string, uint32_t> withFrame(const std::string &spec)
        {
            const size_t at = spec.rfind('@');
            if (at == std::string::npos)
                return {spec, 0u};
            return {spec.substr(0, at), static_cast<uint32_t>(std::strtoul(spec.c_str() + at + 1, nullptr, 0))};
        }

#ifdef BOZO_SETTINGS_UI
        void drawTab()
        {
            std::vector<Encounter> encounters;
            std::string last;
            {
                std::lock_guard lock(g_mutex);
                encounters = g_encounters;
                last = g_last;
            }
            const int current = g_currentMap.load();
            ImGui::TextDisabled("Warp loads a map from its start (its films, then its start position). A battle starts on the");
            ImGui::TextDisabled("current map: use it while walking freely, not during a film or another battle.");
            ImGui::SeparatorText("Maps");
            for (int i = 2; i < 14; ++i)
            {
                if (!kMaps[i].region)
                    continue;
                ImGui::PushID(i);
                if (ImGui::Button("Warp"))
                    request(Request::kWarp, i);
                ImGui::SameLine();
                ImGui::Text("%s%s  (%s)", kMaps[i].region, i == current ? "  <- current" : "", kMaps[i].path);
                ImGui::PopID();
            }
            ImGui::SeparatorText("Battles on this map");
            static char filter[64] = "";
            ImGui::SetNextItemWidth(200.0f * ImGui::GetStyle().FontScaleDpi);
            ImGui::InputTextWithHint("##filter", "filter", filter, sizeof(filter));
            ImGui::SameLine();
            static int custom = 0;
            ImGui::SetNextItemWidth(120.0f * ImGui::GetStyle().FontScaleDpi);
            ImGui::InputInt("##id", &custom, 0);
            ImGui::SameLine();
            if (ImGui::Button("Start id"))
                request(Request::kEncounter, custom);
            if (encounters.empty())
                ImGui::TextDisabled("No encounters listed (no map loaded yet).");
            if (ImGui::BeginChild("encounters", ImVec2(0, 260.0f * ImGui::GetStyle().FontScaleDpi), ImGuiChildFlags_Borders))
            {
                std::string want = filter;
                for (char &c : want)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (const Encounter &e : encounters)
                {
                    std::string lower = e.name;
                    for (char &c : lower)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (!want.empty() && lower.find(want) == std::string::npos)
                        continue;
                    ImGui::PushID(e.id);
                    if (ImGui::Button("Start"))
                        request(Request::kEncounter, e.id);
                    ImGui::SameLine();
                    ImGui::Text("%d  %s", e.id, e.name.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            if (!last.empty())
                ImGui::TextDisabled("Last: %s", last.c_str());
        }
#endif
    }

    void addGameOption(const std::string &spec)
    {
        std::string name = spec, value;
        if (const size_t eq = spec.find('='); eq != std::string::npos)
        {
            name = spec.substr(0, eq);
            value = spec.substr(eq + 1);
        }
        while (!name.empty() && name.front() == '-')
            name.erase(name.begin());
        if (!name.empty())
            options().emplace_back(name, value);
    }

    bool requestWarp(const std::string &spec)
    {
        const auto [what, frame] = withFrame(spec);
        const int index = mapByName(what);
        if (index < 0 || index >= 14 || !kMaps[index].region)
        {
            std::fprintf(stderr, "[tta:debug] no map '%s'; maps:", what.c_str());
            for (const MapInfo &m : kMaps)
                if (m.region)
                    std::fprintf(stderr, " %s (%s);", m.region, m.path);
            std::fprintf(stderr, "\n");
            return false;
        }
        request(Request::kWarp, index, frame);
        return true;
    }

    void requestEncounter(const std::string &spec)
    {
        const auto [what, frame] = withFrame(spec);
        request(Request::kEncounter, std::atoi(what.c_str()), frame);
    }

    void installDebug(bozo::port::Registry &registry)
    {
        if (const char *list = std::getenv("TTA_GAME_OPTIONS"))
        {
            std::istringstream in(list);
            for (std::string spec; in >> spec;)
                addGameOption(spec);
        }
        if (const char *w = std::getenv("TTA_WARP"))
            requestWarp(w);
        if (const char *e = std::getenv("TTA_ENCOUNTER"))
            requestEncounter(e);
        registry.registerFunction(0x140670, parseArguments);
        registry.registerFunction(0x216118, mainLoopTick);
        if (std::getenv("TTA_ENCOUNTER_TRACE"))
            registry.registerFunction(0x15b6c8, encounterTrace);
#ifdef BOZO_SETTINGS_UI
        bozo::app::ui::registerTab("Debug", drawTab);
#endif
    }
}
