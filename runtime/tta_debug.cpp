// Debug tools built on what the game itself has: start any of the current map's battles, warp to any map, and give the
// game its developers' command-line options. In the window build they are the F2 window's "Debug" tab; scripted runs
// use --warp / --encounter / --game-option (or the TTA_WARP / TTA_ENCOUNTER / TTA_GAME_OPTIONS variables).
//
// Battles. The level scripts' Lua function StartEncounter (C side 0x268668) calls 0x17f040(id) -> 0x15b6c8(id), which
// hands the id to the game mode's state machine (*(0x3a4950)). The ids are the current map's entity records: *(0x3dff84)
// is the map's record table ("manC", count at +0xc, records of 0x20 bytes from +0x10: the id, then the name). Its
// encounters are the records named Enc<n>[a-z]_<Name> (Enc01_Nazgul = 2109, Enc11_WargCamp01 = 2118 in Eregion);
// the random battles' _EP (placements) and _ET (enemy groups) records alone start an empty battle (not listed).
// A battle asked for while the game plays one of its in-engine films (game state 0x3a5294 == 5) waits for the film to
// end (two minutes at most): started inside Helm's Deep's opening films it kept the film's riders, cameras and zones.
//
// Maps. The map index (0x35bd0c, 0..13) chooses among the paths at 0x35bc88 (e98c/e98c03 ... e03c/e03c04); 0x190130(i)
// sets it, and the chapter message 0 (category 0x7d0, handler 0x18f420: {0x7d0, 0, number, arguments}) loads the map
// at that index through 0x18f318 (SetLevel 0x190040, Data/Game/<path>.scp into 0x35bcc8): the game's loading screen,
// then the map's own opening (its films, its start position). Indices 6 (e99c01) and 13 (e03c04) have no files on the
// disc; 0 and 1 are the developers' test maps (e98c03 went back to the title, e98c02 played Eregion's films).
//
// Both run from the main loop's wait on the graphics paths (0x216118, once a frame, see tta_port.cpp's busyPaths): a warp
// as calls into the game with the caller's registers kept, a battle as the loop's next call (callStartEncounter).
//
// Command-line options: 0x140670 parses the game's argv (argc at 0x37a728, argv at 0x37a72c) into an option table:
// every "-name" with the next argument as its value when that does not start with '-' (else ""). The table is
// 0x3d70b0: a 0x2000-byte string pool from +1 (used bytes at +0x2004), up to 64 entries {name without '-', value} from
// +0x2008, their count at +0x2208. 0x1a0820(name, default) reads it (the name's leading '-' skipped). Options the code
// asks for: "level" and "file" (0x191360, only when no map is set, which never happens), "encounters_only" (0x160b88,
// 0x1c3af4), "encounters_only_noheal". With any option given the game skips its opening films.
#include "tta_port.h"

#include "tta_functions.h"

#include <algorithm>
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

        // An encounter (type 0x1775: id at +0, the enemy group's id at +0x28) or an enemy group (type 0x1770, an "_ET"
        // record: the placement's id, an "_EP" record, at +0x24) as the map's objects hold them.
        struct Encounter
        {
            int32_t id;
            std::string name;
            uint32_t address; // the object in RAM
            int32_t link;     // an encounter's group, a group's placement
        };
        constexpr uint32_t kRecordTable = 0x3dff84u, kEncounterType = 0x1775u, kGroupType = 0x1770u;
        constexpr uint32_t kGameState = 0x3a5294u, kStateFilm = 5u; // the game's state: 5 a film, 6 exploring, 3 a battle

        // The current map's records by id (*(0x3dff84): "manC", count at +0xc, {id, name[28]} from +0x10).
        std::vector<std::pair<int32_t, std::string>> readRecords(const uint8_t *rdram)
        {
            std::vector<std::pair<int32_t, std::string>> list;
            const uint32_t table = read32(rdram, kRecordTable);
            if (!table || table >= 0x2000000u || std::memcmp(rdram + table, "manC", 4) != 0)
                return list;
            const uint32_t count = read32(rdram, table + 0xcu);
            for (uint32_t k = 0; k < count && k < 0x4000u; ++k)
            {
                const uint32_t at = table + 0x10u + k * 0x20u;
                char name[29] = {};
                std::memcpy(name, rdram + ((at + 4u) & 0x1ffffffu), 28);
                list.emplace_back(static_cast<int32_t>(read32(rdram, at)), name);
            }
            return list;
        }

        // The map's encounter and group objects: RAM scanned for {id, type} with the id one of the map's records (the
        // record names them). Encounters without a group (scripted cut-ins) are left out.
        void scanObjects(const uint8_t *rdram, std::vector<Encounter> &encounters, std::vector<Encounter> &groups)
        {
            encounters.clear();
            groups.clear();
            const auto records = readRecords(rdram);
            if (records.empty())
                return;
            int32_t lo = records.front().first, hi = lo;
            for (const auto &r : records)
            {
                lo = std::min(lo, r.first);
                hi = std::max(hi, r.first);
            }
            std::vector<const std::string *> byId(static_cast<size_t>(hi - lo + 1), nullptr);
            for (const auto &r : records)
                byId[static_cast<size_t>(r.first - lo)] = &r.second;
            for (uint32_t a = 0x100000u; a + 0x30u < 0x2000000u; a += 4u)
            {
                const uint32_t type = read32(rdram, a + 4u);
                if (type != kEncounterType && type != kGroupType)
                    continue;
                const int32_t id = static_cast<int32_t>(read32(rdram, a));
                if (id < lo || id > hi || !byId[static_cast<size_t>(id - lo)])
                    continue;
                const int32_t link = static_cast<int32_t>(read32(rdram, a + (type == kEncounterType ? 0x28u : 0x24u)));
                if (link < lo || link > hi || !byId[static_cast<size_t>(link - lo)])
                    continue;
                // The links name what they are: an encounter's group is an enemy template, a group's placement a
                // placement, and a group is itself a template. Anything else is another object whose words happen to
                // match. Eregion names them "..._ET" / "..._EP"; Rohan and the later maps "..._Encounter_Template" /
                // "..._EncounterPlacement" and the like, cut at the record's 27 characters ("sBridge02_Encounter_Templat").
                const auto endsWith = [](const std::string &n, const char *tail)
                {
                    const size_t k = std::strlen(tail);
                    return n.size() >= k && n.compare(n.size() - k, k, tail) == 0;
                };
                const auto has = [](const std::string &n, const char *part) { return n.find(part) != std::string::npos; };
                const auto isTemplate = [&](const std::string &n)
                { return endsWith(n, "_ET") || (has(n, "Encounter") && has(n, "Templ")); };
                const auto isPlacement = [&](const std::string &n)
                { return endsWith(n, "_EP") || (has(n, "Encounter") && has(n, "Placem")); };
                const std::string &name = *byId[static_cast<size_t>(id - lo)];
                const std::string &linked = *byId[static_cast<size_t>(link - lo)];
                if (type == kEncounterType ? !isTemplate(linked) : (!isTemplate(name) || !isPlacement(linked)))
                    continue;
                auto &into = type == kEncounterType ? encounters : groups;
                if (std::none_of(into.begin(), into.end(), [&](const Encounter &e) { return e.id == id; }))
                    into.push_back({id, name, a, link});
            }
            const auto byIdOrder = [](const Encounter &x, const Encounter &y) { return x.id < y.id; };
            std::sort(encounters.begin(), encounters.end(), byIdOrder);
            std::sort(groups.begin(), groups.end(), byIdOrder);
        }

        // ---- Requests, served on the game's thread ----

        struct Request
        {
            enum Kind
            {
                kEncounter,
                kWarp,
                kGroup
            } kind;
            int32_t value;
            uint32_t atFrame;
        };
        std::mutex g_mutex;
        std::deque<Request> g_requests;
        std::vector<Encounter> g_encounters, g_groups; // the current map's, refreshed by the game thread
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

        // StartEncounter 0x17f040(id), as the main loop's own next call: the hook 0x216118 hands its caller's call over to
        // it, returning to the loop's jal 0x216118 (ra - 8), so the loop asks the hook again when it is done. A call made
        // from inside the hook (callGame) can switch threads in the game (Rohan's encounters do)
        // and then never comes back to the hook, which left the main loop broken and the frame stopped.
        bool g_tailCall = false;

        void callStartEncounter(R5900Context *ctx, int32_t id)
        {
            ctx->r[4].ud[0] = static_cast<uint64_t>(static_cast<int64_t>(id));
            ctx->r[31].ud[0] = ctx->r[31].ud[0] - 8u;
            ctx->pc = 0x17f040u;
            g_tailCall = true;
        }

        bool objectIs(const uint8_t *rdram, const Encounter &e, uint32_t type);

        // TTA_DEBUG_OBJDUMP=1: an encounter's object, its group's and every object carrying its placement's id, as words
        // and floats (what the battle reads to place itself).
        void dumpWords(const uint8_t *rdram, const char *what, uint32_t address, uint32_t words)
        {
            std::printf("[tta:debug] %s at %06x:\n", what, address);
            for (uint32_t k = 0; k < words; k += 4)
            {
                std::printf("[tta:debug]   +%03x", k * 4);
                for (uint32_t j = 0; j < 4; ++j)
                {
                    const uint32_t v = read32(rdram, address + (k + j) * 4u);
                    float f;
                    std::memcpy(&f, &v, 4);
                    std::printf("  %08x (%g)", v, f);
                }
                std::printf("\n");
            }
        }
        void dumpEncounterObjects(const uint8_t *rdram, int32_t id)
        {
            std::vector<Encounter> encounters, groups;
            {
                std::lock_guard lock(g_mutex);
                encounters = g_encounters;
                groups = g_groups;
            }
            for (const Encounter &e : encounters)
            {
                if (e.id != id)
                    continue;
                dumpWords(rdram, ("encounter " + e.name).c_str(), e.address, 0x20);
                for (const Encounter &g : groups)
                {
                    if (g.id != e.link)
                        continue;
                    dumpWords(rdram, ("group " + g.name).c_str(), g.address, 0x20);
                    for (uint32_t a = 0x100000u; a + 0x100u < 0x2000000u; a += 4u)
                        if (static_cast<int32_t>(read32(rdram, a)) == g.link && read32(rdram, a + 4u) >= 0x1000u &&
                            read32(rdram, a + 4u) < 0x2000u)
                            dumpWords(rdram, ("placement " + std::to_string(g.link) + " type " +
                                              std::to_string(read32(rdram, a + 4u))).c_str(),
                                      a, 0x30);
                }
            }
            std::fflush(stdout);
        }

        void startEncounter(uint8_t *rdram, R5900Context *ctx, Runtime *rt, int32_t id)
        {
            // Only one of the current map's encounters: StartEncounter with an id the map does not have never comes back
            // (the game's frame stops; seen with an Eregion id right after a warp to Rohan, its list still empty).
            bool listed = false;
            size_t count = 0;
            {
                std::lock_guard lock(g_mutex);
                count = g_encounters.size();
                for (const Encounter &e : g_encounters)
                    listed = listed || (e.id == id && objectIs(rdram, e, kEncounterType));
            }
            if (!listed)
            {
                note("no encounter " + std::to_string(id) + " on this map (" + std::to_string(count) + " listed): not started");
                return;
            }
            // Not inside one of the game's in-engine films (0x3a5294: 5 in a film, 6 exploring, 3 in battle): Helm's
            // Deep's opening films (the ride to the gate, Theoden) run until about frame 19500 after the warp, and a
            // battle started in them was fought with the film still in place: its five riders on their horses, its
            // camera for half the shots, and only the zones round the film's valley, so the battle (near x 2700) had
            // no ground. The request waits a second and asks again, for two minutes at most: state 5 is also the
            // game's own prompts and movies (the Gullies' "Epic Scene, view it now?" and the scene it plays), where a
            // battle starts well, and a scripted run must not wait for ever.
            static int32_t waitingFor = 0;
            static uint32_t waitingSince = 0;
            const uint32_t now = gameFrame(rdram);
            if (read32(rdram, kGameState) == kStateFilm && !(waitingFor == id && now - waitingSince >= 7200u))
            {
                if (waitingFor != id)
                {
                    note("encounter " + std::to_string(id) + " waits for the film to end");
                    waitingFor = id;
                    waitingSince = now;
                }
                std::lock_guard lock(g_mutex);
                g_requests.push_back({Request::kEncounter, id, now + 60u});
                return;
            }
            if (waitingFor == id && read32(rdram, kGameState) == kStateFilm)
                note("encounter " + std::to_string(id) + ": still in a film or prompt after two minutes, started anyway");
            waitingFor = 0;
            note("StartEncounter(" + std::to_string(id) + ") at game frame " + std::to_string(gameFrame(rdram)));
            if (std::getenv("TTA_DEBUG_OBJDUMP"))
                dumpEncounterObjects(rdram, id);
            callStartEncounter(ctx, id);
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

        // A battle against any enemy group of the map: a host encounter (the map's first random encounter, "Rnd..", else
        // its first) has its group (+0x28) pointed at the chosen one, is started, and gets its own group back 10 seconds
        // later (the battle has read it by then). The group brings its placement (+0x24): the random groups have theirs
        // (e1c1_Rnd_Easy_01_ET: the two-enemy layout e1c1_Rnd_2enemy_EP).
        struct Restore
        {
            uint32_t address = 0, frame = 0;
            int32_t id = 0, group = 0;
        } g_restore;

        bool objectIs(const uint8_t *rdram, const Encounter &e, uint32_t type)
        {
            return read32(rdram, e.address) == static_cast<uint32_t>(e.id) && read32(rdram, e.address + 4u) == type;
        }

        void startGroup(uint8_t *rdram, R5900Context *ctx, Runtime *rt, int32_t groupId)
        {
            std::vector<Encounter> encounters, groups;
            {
                std::lock_guard lock(g_mutex);
                encounters = g_encounters;
                groups = g_groups;
            }
            const auto group = std::find_if(groups.begin(), groups.end(), [&](const Encounter &g) { return g.id == groupId; });
            if (group == groups.end() || !objectIs(rdram, *group, kGroupType))
            {
                note("no enemy group " + std::to_string(groupId) + " on this map");
                return;
            }
            const Encounter *host = nullptr;
            for (const Encounter &e : encounters)
                if (objectIs(rdram, e, kEncounterType) && (!host || (e.name.rfind("Rnd", 0) == 0 && host->name.rfind("Rnd", 0) != 0)))
                    host = &e;
            if (!host)
            {
                note("no encounter on this map to start the group with");
                return;
            }
            if (g_restore.address)
                write32(rdram, g_restore.address + 0x28u, static_cast<uint32_t>(g_restore.group));
            g_restore = {host->address, gameFrame(rdram) + 600u, host->id, static_cast<int32_t>(read32(rdram, host->address + 0x28u))};
            write32(rdram, host->address + 0x28u, static_cast<uint32_t>(groupId));
            note("enemy group " + std::to_string(groupId) + " (" + group->name + ") through " + host->name);
            callStartEncounter(ctx, host->id);
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
            g_tailCall = false;
            for (size_t k = 0; k < due.size(); ++k)
            {
                const Request &r = due[k];
                if (g_tailCall)
                {
                    // One battle a tick: the rest wait for the next one.
                    std::lock_guard lock(g_mutex);
                    g_requests.push_back(r);
                    continue;
                }
                if (r.kind == Request::kEncounter)
                    startEncounter(rdram, ctx, rt, r.value);
                else if (r.kind == Request::kGroup)
                    startGroup(rdram, ctx, rt, r.value);
                else
                    warp(rdram, ctx, rt, r.value);
            }
            if (g_restore.address && frame >= g_restore.frame)
            {
                if (read32(rdram, g_restore.address) == static_cast<uint32_t>(g_restore.id))
                    write32(rdram, g_restore.address + 0x28u, static_cast<uint32_t>(g_restore.group));
                g_restore = {};
            }
            // The current map's encounters and groups: a RAM scan when the map's record table moves (a map loaded),
            // then again 1, 5, 15 and 60 seconds later (the objects come after the table, while the map streams in).
            static uint32_t lastTable = 0, tableFrame = 0, rescans = 0;
            static constexpr uint32_t kRescanAt[] = {60u, 300u, 900u, 3600u};
            const uint32_t table = read32(rdram, kRecordTable);
            g_currentMap.store(static_cast<int>(read32(rdram, kMapIndex)));
            const bool moved = table != lastTable;
            if (moved)
            {
                lastTable = table;
                tableFrame = frame;
                rescans = 0;
            }
            if (moved || (rescans < 4u && frame - tableFrame >= kRescanAt[rescans]))
            {
                if (!moved)
                    ++rescans;
                std::vector<Encounter> encounters, groups;
                scanObjects(rdram, encounters, groups);
                std::printf("[tta:debug] map objects at game frame %u: %zu encounters, %zu enemy groups%c", frame, encounters.size(),
                            groups.size(), 10);
                // TTA_DEBUG_LIST=1: the ids and names, for --encounter / --group in scripted runs.
                if (std::getenv("TTA_DEBUG_LIST"))
                {
                    for (const Encounter &e : encounters)
                        std::printf("[tta:debug]   encounter %d %s (group %d)%c", e.id, e.name.c_str(), e.link, 10);
                    for (const Encounter &g : groups)
                        std::printf("[tta:debug]   group %d %s%c", g.id, g.name.c_str(), 10);
                }
                std::lock_guard lock(g_mutex);
                g_encounters.swap(encounters);
                g_groups.swap(groups);
            }
            ctx->r[2].ud[0] = 0;
            ctx->pc = g_tailCall ? 0x17f040u : ctx->r[31].uw[0]; // StartEncounter next (callStartEncounter), else back
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

        // TTA_ZONE_TRACE=1: the random-encounter zones' updates (0x178f38(zone, step)): each zone once with its countdown
        // and encounter list.
        void zoneTrace(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t zone = ctx->r[4].uw[0];
            static std::vector<uint32_t> seen;
            static uint32_t lastPrint = 0;
            const bool fresh = std::find(seen.begin(), seen.end(), zone) == seen.end();
            if (fresh || gameFrame(rdram) - lastPrint > 600u)
            {
                if (fresh)
                    seen.push_back(zone);
                lastPrint = gameFrame(rdram);
                float countdown, step;
                std::memcpy(&countdown, rdram + zone + 0x50u, 4);
                std::memcpy(&step, &ctx->f[12], 4);
                const uint32_t total = read32(rdram, zone + 0x54u), left = read32(rdram, zone + 0x5cu);
                std::printf("[tta:debug] zone %x game frame %u: countdown %.2f step %.3f, %u encounters (%u left):", zone,
                            gameFrame(rdram), countdown, step, total, left);
                for (uint32_t k = 0; k < total && k < 32u; ++k)
                    std::printf(" %u", read32(rdram, zone + 0x84u + k * 4u));
                std::printf("  words:");
                for (uint32_t k = 0; k < 0x84u; k += 4)
                    std::printf(" %x", read32(rdram, zone + k));
                std::printf("\n");
                std::fflush(stdout);
            }
            func_0x178f38(rdram, ctx, rt);
        }

        // With TTA_ZONE_TRACE: the encounter manager's messages (0x17bc98: 0xc8 a zone entered, 0xc9 left; +4 the id).
        void zoneMessageTrace(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t msg = ctx->r[5].uw[0];
            if (msg)
            {
                const uint32_t number = read32(rdram, msg + 8u);
                if (number == 0xc8u || number == 0xc9u)
                {
                    std::printf("[tta:debug] zone message %x id %u, game frame %u%c", number, read32(rdram, msg + 4u), gameFrame(rdram), 10);
                    std::fflush(stdout);
                }
            }
            func_0x17bc98(rdram, ctx, rt);
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
        // A filtered, scrolling list of encounters or groups with a button each.
        void drawList(const char *id, const std::vector<Encounter> &list, const char *filter, const char *button,
                      Request::Kind kind, float height)
        {
            std::string want = filter;
            for (char &c : want)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ImGui::BeginChild(id, ImVec2(0, height * ImGui::GetStyle().FontScaleDpi), ImGuiChildFlags_Borders))
            {
                for (const Encounter &e : list)
                {
                    std::string lower = e.name;
                    for (char &c : lower)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (!want.empty() && lower.find(want) == std::string::npos)
                        continue;
                    ImGui::PushID(e.id);
                    if (ImGui::Button(button))
                        request(kind, e.id);
                    ImGui::SameLine();
                    ImGui::Text("%d  %s", e.id, e.name.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }

        void drawTab()
        {
            std::vector<Encounter> encounters, groups;
            std::string last;
            {
                std::lock_guard lock(g_mutex);
                encounters = g_encounters;
                groups = g_groups;
                last = g_last;
            }
            const int current = g_currentMap.load();
            ImGui::TextDisabled("Warp loads a map from its start (its films, then its start position). Battles start on the");
            ImGui::TextDisabled("current map: use them while walking freely, not during a film or another battle.");
            if (ImGui::CollapsingHeader("Maps", ImGuiTreeNodeFlags_DefaultOpen))
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
            static char filter[64] = "";
            ImGui::SetNextItemWidth(220.0f * ImGui::GetStyle().FontScaleDpi);
            ImGui::InputTextWithHint("##filter", "filter both lists (rnd, warg, orc...)", filter, sizeof(filter));
            if (encounters.empty() && groups.empty())
                ImGui::TextDisabled("Nothing listed yet (the map's objects are read once it has loaded).");
            if (ImGui::CollapsingHeader("Encounters (the map's own battles, Rnd.. its random ones)", ImGuiTreeNodeFlags_DefaultOpen))
                drawList("encounters", encounters, filter, "Start", Request::kEncounter, 200.0f);
            if (ImGui::CollapsingHeader("Enemy groups (any group in a battle: Rnd_Easy / Med / Hard are the random battles')",
                                        ImGuiTreeNodeFlags_DefaultOpen))
                drawList("groups", groups, filter, "Fight", Request::kGroup, 200.0f);
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

    void requestGroup(const std::string &spec)
    {
        const auto [what, frame] = withFrame(spec);
        request(Request::kGroup, std::atoi(what.c_str()), frame);
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
        if (const char *g = std::getenv("TTA_GROUP"))
            requestGroup(g);
        registry.registerFunction(0x140670, parseArguments);
        registry.registerFunction(0x216118, mainLoopTick);
        if (std::getenv("TTA_ENCOUNTER_TRACE"))
            registry.registerFunction(0x15b6c8, encounterTrace);
        if (std::getenv("TTA_ZONE_TRACE"))
        {
            registry.registerFunction(0x178f38, zoneTrace);
            registry.registerFunction(0x17bc98, zoneMessageTrace);
        }
#ifdef BOZO_SETTINGS_UI
        bozo::app::ui::registerTab("Debug", drawTab);
#endif
    }
}
