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
// then the map's own opening (its films, its start position). Indices 6 (e99c01), 8 (e02c04) and 13 (e03c04) have no
// files on the disc; 0 and 1 are the developers' test maps (e98c03 goes back to the title, e98c02 played Eregion's
// films). The closing films after the Eye of Sauron are the Pelennor script's own, before it loads e98c03.
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
            {"e02c/e02c04", nullptr}, // not on the disc (E02C holds 01, 03, 05): a warp there stayed black
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

        // ---- Cheats ----
        // The party list 0x3a6e50 (GetPartyMemberByName's, 0x1941f8): its count at +8, member k's actor at +0xba4 + k *
        // 0x20. An actor (a combatant when +0x98 bit 27 is set, as 0x17ebf0 / 0x183f28 test) keeps its HP at +0x9c and
        // its AP as a halfword at +0xa5e; its maximums, as 0x18cef0 last worked them out, at +0xd98 and +0xd9c (0x183f28,
        // 0x1840a0). The game's own full heal (0x1249c4) writes the maximums back the same way.
        constexpr uint32_t kParty = 0x3a6e50u;
        std::atomic<bool> g_infiniteHp{false}, g_infiniteAp{false}, g_oneHitKills{false}, g_winBattle{false}, g_autoWin{false};
        // The battle's combatants: the turn queue manager 0x3a5578's list, count 0x3a5a2c, actors from 0x3a5a30.
        constexpr uint32_t kCombatantCount = 0x3a5a2cu, kCombatants = 0x3a5a30u;

        uint16_t read16(const uint8_t *rdram, uint32_t address)
        {
            uint16_t v = 0;
            std::memcpy(&v, rdram + (address & 0x1ffffffu), 2);
            return v;
        }
        void write16(uint8_t *rdram, uint32_t address, uint16_t v) { std::memcpy(rdram + (address & 0x1ffffffu), &v, 2); }

        // The party's members' actors (combatants only).
        std::vector<uint32_t> partyActors(const uint8_t *rdram)
        {
            std::vector<uint32_t> actors;
            const uint32_t count = std::min<uint32_t>(read32(rdram, kParty + 8u), 8u);
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t actor = read32(rdram, kParty + 0xba4u + k * 0x20u);
                if (actor >= 0x100000u && actor < 0x2000000u && ((read32(rdram, actor + 0x98u) >> 27) & 1u))
                    actors.push_back(actor);
            }
            return actors;
        }

        // Each game frame: the members' HP and AP back to their maximums. A member knocked out (HP 0; for the party that
        // alone is down: 0x17eb20's alive test is HP > 0, the state bit 0x1d it also reads is set for another kind of
        // actor only) is revived with full HP half a second later, once the game has dealt with the fall.
        void applyCheats(uint8_t *rdram, uint32_t frame)
        {
            const bool hp = g_infiniteHp.load(std::memory_order_relaxed), ap = g_infiniteAp.load(std::memory_order_relaxed);
            // One-hit kills: every enemy in the combatant list (flags +0x98 with bit 27, a combatant, and bit 30, an enemy:
            // 0x48000004 for the first battle's Nazgul and the forest's wargs, 0x08000004 for the party) held at 1 HP, so
            // any blow that lands fells it through the game's own damage and death. The game state word is no guide (6,
            // exploring, through a debug-started battle), and the list outlives its battle, so an actor counts only while
            // it still looks like one: both bits, its HP within a sane maximum.
            // Win the battle (the tab's button, once; TTA_CHEATS=win for every battle): every enemy at 0 HP, so the
            // game's own winner check (0x161630, after the next action) finds their side down.
            const bool autoWin = g_autoWin.load(std::memory_order_relaxed);
            if (autoWin || g_winBattle.load(std::memory_order_relaxed))
            {
                const uint32_t count = std::min<uint32_t>(read32(rdram, kCombatantCount), 16u);
                uint32_t felled = 0;
                for (uint32_t k = 0; k < count; ++k)
                {
                    const uint32_t actor = read32(rdram, kCombatants + 4u * k);
                    if (actor < 0x100000u || actor >= 0x2000000u || (read32(rdram, actor + 0x98u) & 0x48000000u) != 0x48000000u)
                        continue;
                    const int32_t health = static_cast<int32_t>(read32(rdram, actor + 0x9cu));
                    const int32_t most = static_cast<int32_t>(read32(rdram, actor + 0xd98u));
                    if (health > 0 && most >= health && most < 1000000)
                    {
                        write32(rdram, actor + 0x9cu, 0u);
                        ++felled;
                    }
                }
                g_winBattle = false;
                if (felled)
                {
                    std::printf("[tta:debug] cheat: %u enemies felled at game frame %u%c", felled, frame, 10);
                }
            }
            if (g_oneHitKills.load(std::memory_order_relaxed))
            {
                const uint32_t count = std::min<uint32_t>(read32(rdram, kCombatantCount), 16u);
                for (uint32_t k = 0; k < count; ++k)
                {
                    const uint32_t actor = read32(rdram, kCombatants + 4u * k);
                    if (actor < 0x100000u || actor >= 0x2000000u || (read32(rdram, actor + 0x98u) & 0x48000000u) != 0x48000000u)
                        continue;
                    const int32_t health = static_cast<int32_t>(read32(rdram, actor + 0x9cu));
                    const int32_t most = static_cast<int32_t>(read32(rdram, actor + 0xd98u));
                    if (health > 1 && most >= health && most < 1000000)
                        write32(rdram, actor + 0x9cu, 1u);
                }
            }
            static std::vector<std::pair<uint32_t, uint32_t>> fallen; // actor, the frame it was seen down
            if (!hp)
                fallen.clear();
            if (!hp && !ap)
                return;
            for (const uint32_t actor : partyActors(rdram))
            {
                const int32_t health = static_cast<int32_t>(read32(rdram, actor + 0x9cu));
                const int32_t mostHp = static_cast<int32_t>(read32(rdram, actor + 0xd98u));
                if (health <= 0)
                {
                    if (!hp || mostHp <= 0 || mostHp >= 100000)
                        continue;
                    auto it = std::find_if(fallen.begin(), fallen.end(), [&](const auto &f) { return f.first == actor; });
                    if (it == fallen.end())
                        fallen.push_back({actor, frame});
                    else if (frame - it->second >= 30u)
                    {
                        write32(rdram, actor + 0x9cu, static_cast<uint32_t>(mostHp));
                        fallen.erase(it);
                        std::printf("[tta:debug] cheat: revived %x at game frame %u%c", actor, frame, 10);
                    }
                    continue;
                }
                fallen.erase(std::remove_if(fallen.begin(), fallen.end(), [&](const auto &f) { return f.first == actor; }), fallen.end());
                if (hp && mostHp > health && mostHp < 100000)
                    write32(rdram, actor + 0x9cu, static_cast<uint32_t>(mostHp));
                if (const int32_t most = static_cast<int32_t>(read32(rdram, actor + 0xd9cu));
                    ap && most > 0 && most < 65536 && read16(rdram, actor + 0xa5eu) < most)
                    write16(rdram, actor + 0xa5eu, static_cast<uint16_t>(most));
            }
        }

        // Battle damage: 0x18bcd8(actor, change, ..) is the battle code's HP change (17 callers in it; a blow is a negative
        // change, which the game itself zeroes when 0x17a990 says so). With infinite HP a party member's damage is zeroed
        // here, so a blow that strikes the whole party at once (the Eye of Sauron's Wrath of Sauron, ~4450 each) cannot
        // knock everyone down in one frame, which ends the battle before the refill or the revive can act.
        void battleHpChange(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            if (g_infiniteHp.load(std::memory_order_relaxed) && ctx->r[5].sw[0] < 0)
            {
                const std::vector<uint32_t> party = partyActors(rdram);
                if (std::find(party.begin(), party.end(), ctx->r[4].uw[0]) != party.end())
                    ctx->r[5].ud[0] = 0;
            }
            if (std::getenv("TTA_DAMAGE_TRACE"))
                std::printf("[tta:dmg] frame %u 18bcd8 actor %x change %d hp %d from %x%c", gameFrame(rdram), ctx->r[4].uw[0], ctx->r[5].sw[0],
                            static_cast<int32_t>(read32(rdram, ctx->r[4].uw[0] + 0x9cu)), ctx->r[31].uw[0], 10);
            func_0x18bcd8(rdram, ctx, rt);
        }

        // TTA_DAMAGE_TRACE=1: the HP changes by path (0x18bcd8 the battle code's, 0x11b288 the generic apply, 0x11b368
        // TakeDamage's dispatch), with the actor, the amount and the caller.
        template <uint32_t Address, void (*Original)(uint8_t *, R5900Context *, Runtime *)>
        void damageTrace(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t actor = ctx->r[4].uw[0];
            const int32_t before = static_cast<int32_t>(read32(rdram, actor + 0x9cu));
            std::printf("[tta:dmg] frame %u %x actor %x amount %d hp %d from %x%c", gameFrame(rdram), Address, actor, ctx->r[5].sw[0], before,
                        ctx->r[31].uw[0], 10);
            Original(rdram, ctx, rt);
        }

        // TTA_STATE_TRACE=1 also: the chapter messages (category 0x7d0, handler 0x18f420: {0x7d0, ?, number, arguments}).
        void chapterMessageTrace(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t m = ctx->r[4].uw[0];
            if (read32(rdram, m) == 0x7d0u)
                std::printf("[tta:state] game frame %u: chapter message %u (%x %x %x %x) from %x%c", gameFrame(rdram), read32(rdram, m + 8u),
                            read32(rdram, m + 4u), read32(rdram, m + 12u), read32(rdram, m + 16u), read32(rdram, m + 20u),
                            ctx->r[31].uw[0], 10);
            func_0x18f420(rdram, ctx, rt);
        }

        // Instant kills: the battle's action (0x110e38, from the battle state at 0x16317c) marks the targets an
        // instant-death effect fells (the Eye of Sauron's Wrath of Sauron carries one) in the action context 0x3a1900
        // (0x110438): the targets from +0x210, their count at +0x204 + 0x7e, one result byte each from +0x278, 1 for
        // felled; it may also set their HP straight to 0 (0x111654-0x11167c), past 0x18bcd8. The winner check later
        // in the same frame (0x161630 -> 0x17caa0, each side counted through the alive filter 0x17da40) leaves out a
        // fighter so marked whatever its HP, so the Eye's side won with the whole party at full HP. With infinite HP
        // the party's marks are cleared and its HP put back, after the action and again before the winner check.
        constexpr uint32_t kAction = 0x3a1900u;

        void keepPartyStanding(uint8_t *rdram)
        {
            if (!g_infiniteHp.load(std::memory_order_relaxed))
                return;
            const std::vector<uint32_t> party = partyActors(rdram);
            const int count = std::clamp<int>(static_cast<int8_t>(rdram[kAction + 0x204u + 0x7eu]), 0, 26);
            for (int k = 0; k < count; ++k)
                if (std::find(party.begin(), party.end(), read32(rdram, kAction + 0x210u + 4u * k)) != party.end() &&
                    rdram[kAction + 0x278u + k] == 1u)
                {
                    rdram[kAction + 0x278u + k] = 0u;
                    std::printf("[tta:debug] cheat: %x not felled at game frame %u%c", read32(rdram, kAction + 0x210u + 4u * k),
                                gameFrame(rdram), 10);
                }
            for (const uint32_t actor : party)
            {
                const int32_t most = static_cast<int32_t>(read32(rdram, actor + 0xd98u));
                if (static_cast<int32_t>(read32(rdram, actor + 0x9cu)) <= 0 && most > 0 && most < 100000)
                    write32(rdram, actor + 0x9cu, static_cast<uint32_t>(most));
            }
        }

        void battleAction(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            func_0x110e38(rdram, ctx, rt);
            keepPartyStanding(rdram);
        }

        void battleWinner(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            keepPartyStanding(rdram);
            func_0x161630(rdram, ctx, rt);
        }

        // XP: 0x18c7d0(actor, amount) adds experience (+0xa54, at most 50,000,000; a float tally at +0xa80); the battles'
        // rewards call it (0x111a5c, 0x1127ec, 0x112838). The XP multiplier scales the amount, except when the game sets
        // a joining member's level (0x16ab20: the amount that member is short of it, from 0x16ab74).
        std::atomic<int> g_xpPercent{100};

        void addExperience(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const int percent = g_xpPercent.load(std::memory_order_relaxed);
            const int64_t amount = ctx->r[5].sw[0];
            if (percent != 100 && amount > 0 && ctx->r[31].uw[0] != 0x16ab7cu)
            {
                const int64_t scaled = std::clamp<int64_t>(amount * percent / 100, 1, 50000000);
                ctx->r[5].ud[0] = static_cast<uint64_t>(scaled);
            }
            func_0x18c7d0(rdram, ctx, rt);
        }

        // Items. The entity database *(0x35b750) (0x164808's binary search: count at +8, sorted four-character ids at
        // +0x28, records of +0x10 bytes from +0x2c) holds the items as records of kind 0x04400440 (+8); the party's bag is
        // the party manager 0x3a5bd0's first 0x200 slots {record, count}. 0x194388(party, id, notify, 0) adds one (a new
        // slot or one more, then the bag sorted), as the game's item messages do (0x18f8d8). The names are the strings
        // inv_<id>_nm (things to use) and itm_<id>_nm (things to wear) of the game's string table, read from RAM.
        constexpr uint32_t kDatabase = 0x35b750u, kItemKind = 0x04400440u, kBag = 0x3a5bd0u;
        struct Item
        {
            uint32_t id;
            std::string code; // the id's four characters
            std::string name;
            bool equipment; // itm_ (worn) rather than inv_ (used)
        };
        std::vector<Item> g_items;               // under g_mutex: empty until listed
        std::vector<std::pair<uint32_t, uint32_t>> g_bag; // under g_mutex: id, count (refreshed every half second)
        std::atomic<bool> g_itemsWanted{false};  // the tab is open: list the items and keep the bag fresh
        std::deque<std::pair<uint32_t, int>> g_itemAdds; // under g_mutex: id, amount

        void listItems(const uint8_t *rdram, std::vector<Item> &items)
        {
            items.clear();
            const uint32_t db = read32(rdram, kDatabase);
            if (db < 0x100000u || db >= 0x2000000u)
                return;
            const uint32_t count = std::min<uint32_t>(read32(rdram, db + 8u), 20000u), ids = read32(rdram, db + 0x28u),
                           size = read32(rdram, db + 0x10u), records = read32(rdram, db + 0x2cu);
            if (!count || ids >= 0x2000000u || records >= 0x2000000u || size == 0 || size > 0x1000u)
                return;
            // The names: every "inv_XXXX_nm\0<name>\0" / "itm_XXXX_nm\0<name>\0" in RAM.
            std::vector<std::pair<uint32_t, std::pair<std::string, bool>>> names;
            const char *ram = reinterpret_cast<const char *>(rdram);
            constexpr size_t kRam = 32u << 20;
            for (size_t at = 0x100000u; at + 16 < kRam;)
            {
                const void *found = std::memchr(ram + at, '_', kRam - 16 - at);
                if (!found)
                    break;
                const size_t p = static_cast<size_t>(static_cast<const char *>(found) - ram);
                at = p + 1;
                if (p < 3 || std::memcmp(ram + p + 5, "_nm", 4) != 0) // "_XXXX_nm\0"
                    continue;
                const bool inv = std::memcmp(ram + p - 3, "inv", 3) == 0, itm = std::memcmp(ram + p - 3, "itm", 3) == 0;
                if (!inv && !itm)
                    continue;
                const char *value = ram + p + 9;
                const size_t length = strnlen(value, 80);
                if (length == 0 || length == 80)
                    continue;
                uint32_t id = 0;
                std::memcpy(&id, ram + p + 1, 4);
                names.push_back({id, {std::string(value, length), itm}});
            }
            std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t id = read32(rdram, ids + 4u * i);
                if (read32(rdram, records + size * i + 8u) != kItemKind)
                    continue;
                const auto it = std::lower_bound(names.begin(), names.end(), id, [](const auto &n, uint32_t v) { return n.first < v; });
                if (it == names.end() || it->first != id)
                    continue;
                std::string code(reinterpret_cast<const char *>(&id), 4);
                items.push_back({id, code, it->second.first, it->second.second});
            }
            std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.name < b.name; });
        }

        // `amount` of the item `id` into the bag, as 0x194388 does it (one more on its slot, else the first free slot), up to
        // 99 a slot; the game's own add, called from the tick, never came back (its sort's comparison runs through the
        // C library). The bag stays in its order until the game next adds and sorts. Returns how many went in.
        int addItem(uint8_t *rdram, uint32_t id, int amount)
        {
            const uint32_t db = read32(rdram, kDatabase);
            const uint32_t count = std::min<uint32_t>(read32(rdram, db + 8u), 20000u), ids = read32(rdram, db + 0x28u),
                           size = read32(rdram, db + 0x10u), records = read32(rdram, db + 0x2cu);
            uint32_t record = 0;
            for (uint32_t lo = 0, hi = count; lo < hi;) // the ids are sorted as signed words (0x164808's slt)
            {
                const uint32_t mid = (lo + hi) / 2;
                const int32_t at = static_cast<int32_t>(read32(rdram, ids + 4u * mid));
                if (at == static_cast<int32_t>(id))
                {
                    record = records + size * mid;
                    break;
                }
                if (at < static_cast<int32_t>(id))
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (!record || read32(rdram, record + 8u) != kItemKind)
                return 0;
            uint32_t slot = 0x200u;
            for (uint32_t k = 0; k < 0x200u; ++k)
            {
                const uint32_t held = read32(rdram, kBag + k * 8u);
                if (held == record)
                {
                    slot = k;
                    break;
                }
                if (held == 0 && slot == 0x200u)
                    slot = k;
            }
            if (slot == 0x200u)
                return 0;
            const uint32_t had = read32(rdram, kBag + slot * 8u) == record ? read32(rdram, kBag + slot * 8u + 4u) : 0u;
            const uint32_t now = std::min<uint32_t>(had + static_cast<uint32_t>(amount), 99u);
            write32(rdram, kBag + slot * 8u, record);
            write32(rdram, kBag + slot * 8u + 4u, now);
            return static_cast<int>(now - had);
        }

        // The bag as {id, count}: each slot's record's id (+4).
        void readBag(const uint8_t *rdram, std::vector<std::pair<uint32_t, uint32_t>> &bag)
        {
            bag.clear();
            for (uint32_t k = 0; k < 0x200u; ++k)
            {
                const uint32_t record = read32(rdram, kBag + k * 8u);
                if (record >= 0x100000u && record < 0x2000000u)
                    bag.push_back({read32(rdram, record + 4u), read32(rdram, kBag + k * 8u + 4u)});
            }
        }

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
            std::string names; // the map's encounters, for a scripted run that asked for another
            {
                std::lock_guard lock(g_mutex);
                count = g_encounters.size();
                for (const Encounter &e : g_encounters)
                {
                    listed = listed || (e.id == id && objectIs(rdram, e, kEncounterType));
                    names += (names.empty() ? "" : ", ") + std::to_string(e.id) + " " + e.name;
                }
            }
            if (!listed)
            {
                note("no encounter " + std::to_string(id) + " on this map (" + std::to_string(count) + " listed: " + names +
                     "): not started");
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
            applyCheats(rdram, frame);
            // TTA_STATE_TRACE=1: each change of the game's state word (3 a battle, 4 its end, 5 a film, 6 exploring).
            if (static const bool trace = std::getenv("TTA_STATE_TRACE") != nullptr; trace)
            {
                static uint32_t last = ~0u;
                if (const uint32_t state = read32(rdram, kGameState); state != last)
                {
                    std::printf("[tta:state] game frame %u: %u -> %u%c", frame, last, state, 10);
                    last = state;
                }
                // The top-level mode (0x3ba700, the machine 0x1c29c8 over the table 0x35fc98).
                static int lastMode = 1000;
                if (const int mode = static_cast<int8_t>(rdram[0x3ba700u]); mode != lastMode)
                {
                    std::printf("[tta:state] game frame %u: mode %d -> %d%c", frame, lastMode, mode, 10);
                    lastMode = mode;
                }
                // The map index (kMapIndex): which region is loaded.
                static uint32_t lastMap = ~0u;
                if (const uint32_t map = read32(rdram, kMapIndex); map != lastMap)
                {
                    std::printf("[tta:state] game frame %u: map %u (%s)%c", frame, map,
                                map < 14u ? kMaps[map].path : "?", 10);
                    lastMap = map;
                }
            }
            // Items the Cheats tab asked for, once the game's data is loaded.
            {
                std::deque<std::pair<uint32_t, int>> adds;
                const uint32_t db = read32(rdram, kDatabase);
                if (db >= 0x100000u && db < 0x2000000u && read32(rdram, db + 8u) != 0u && frame > 60u)
                {
                    std::lock_guard lock(g_mutex);
                    adds.swap(g_itemAdds);
                }
                for (const auto &[id, amount] : adds)
                {
                    const int added = addItem(rdram, id, amount);
                    note("added " + std::to_string(added) + " x " + std::string(reinterpret_cast<const char *>(&id), 4) +
                         (added == 0 ? " (no such item, or the bag is full)" : ""));
                }
            }
            if (g_itemsWanted.load(std::memory_order_relaxed))
            {
                static uint32_t bagFrame = 0;
                bool empty;
                {
                    std::lock_guard lock(g_mutex);
                    empty = g_items.empty();
                }
                static uint32_t listFrame = 0;
                if (empty && frame - listFrame >= 120u) // at most every two seconds while nothing is found (a load)
                {
                    listFrame = frame;
                    std::vector<Item> items;
                    listItems(rdram, items);
                    std::lock_guard lock(g_mutex);
                    g_items.swap(items);
                }
                if (frame - bagFrame >= 30u)
                {
                    bagFrame = frame;
                    std::vector<std::pair<uint32_t, uint32_t>> bag;
                    readBag(rdram, bag);
                    std::lock_guard lock(g_mutex);
                    g_bag.swap(bag);
                }
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

        void drawCheatsTab()
        {
            ImGui::TextDisabled("For the party (everyone who fights). They apply at once and last until the game closes.");
            bool hp = g_infiniteHp.load(), ap = g_infiniteAp.load();
            if (ImGui::Checkbox("Infinite HP", &hp))
                g_infiniteHp = hp;
            ImGui::TextDisabled("Battle damage to the party ignored, health kept full; anyone knocked out anyway gets up again.");
            if (ImGui::Checkbox("Infinite AP", &ap))
                g_infiniteAp = ap;
            ImGui::TextDisabled("Skills and spells cost nothing: AP back to full every frame.");
            bool kill = g_oneHitKills.load();
            if (ImGui::Checkbox("One-hit kills", &kill))
                g_oneHitKills = kill;
            ImGui::TextDisabled("Enemies held at 1 HP in battle: any blow that lands fells them (a miss still misses).");
            if (ImGui::Button("Win this battle"))
                g_winBattle = true;
            ImGui::TextDisabled("Every enemy down at once; the battle ends after the next action.");

            static const int kXp[] = {100, 200, 300, 500, 1000};
            static const char *kXpNames[] = {"1x (normal)", "2x", "3x", "5x", "10x"};
            int xpIndex = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kXp); ++i)
                if (g_xpPercent.load() == kXp[i])
                    xpIndex = i;
            ImGui::SetNextItemWidth(160.0f * ImGui::GetStyle().FontScaleDpi);
            if (ImGui::Combo("Experience", &xpIndex, kXpNames, IM_ARRAYSIZE(kXpNames)))
                g_xpPercent = kXp[xpIndex];
            ImGui::TextDisabled("Every battle's experience multiplied (levels come faster).");

            ImGui::SeparatorText("Items");
            g_itemsWanted = true;
            std::vector<Item> items;
            std::vector<std::pair<uint32_t, uint32_t>> bag;
            {
                std::lock_guard lock(g_mutex);
                items = g_items;
                bag = g_bag;
            }
            if (items.empty())
            {
                ImGui::TextDisabled("The item list comes once a game is loaded.");
                return;
            }
            static char search[64] = "";
            static int amount = 1;
            ImGui::SetNextItemWidth(260.0f * ImGui::GetStyle().FontScaleDpi);
            ImGui::InputTextWithHint("##itemsearch", "search (athelas, mithril, Pot4...)", search, sizeof(search));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f * ImGui::GetStyle().FontScaleDpi);
            if (ImGui::InputInt("Amount", &amount))
                amount = std::clamp(amount, 1, 99);
            std::string want = search;
            for (char &c : want)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const float height = ImGui::GetStyle().FontScaleDpi * 260.0f;
            if (ImGui::BeginTable("items", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter,
                                  ImVec2(0.0f, height)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("In the bag", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                for (const Item &item : items)
                {
                    if (!want.empty())
                    {
                        std::string text = item.name + " " + item.code;
                        for (char &c : text)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (text.find(want) == std::string::npos)
                            continue;
                    }
                    uint32_t owned = 0;
                    for (const auto &[id, count] : bag)
                        if (id == item.id)
                            owned += count;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(item.id));
                    if (ImGui::SmallButton("Add"))
                    {
                        std::lock_guard lock(g_mutex);
                        g_itemAdds.push_back({item.id, amount});
                    }
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(item.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s %s", item.equipment ? "gear" : "use", item.code.c_str());
                    ImGui::TableNextColumn();
                    if (owned)
                        ImGui::Text("%u", owned);
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Added to the party's bag at once; equipment then shows in the Equip menu.");
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
        // TTA_CHEATS=hp,ap,kill,xp=<percent>,item=<id>x<amount>: the Cheats tab for a scripted run (an item's id is its four
        // characters, given when the game first ticks).
        if (const char *c = std::getenv("TTA_CHEATS"))
        {
            std::istringstream in(c);
            for (std::string spec; std::getline(in, spec, ',');)
                if (spec == "hp")
                    g_infiniteHp = true;
                else if (spec == "kill")
                    g_oneHitKills = true;
                else if (spec == "win")
                    g_autoWin = true;
                else if (spec == "ap")
                    g_infiniteAp = true;
                else if (spec.rfind("xp=", 0) == 0)
                    g_xpPercent = std::clamp(std::atoi(spec.c_str() + 3), 0, 10000);
                else if (spec.rfind("item=", 0) == 0 && spec.size() >= 9)
                {
                    uint32_t id = 0;
                    std::memcpy(&id, spec.data() + 5, 4);
                    const int amount = spec.size() > 10 && spec[9] == 'x' ? std::atoi(spec.c_str() + 10) : 1;
                    g_itemAdds.push_back({id, std::clamp(amount, 1, 99)});
                }
        }
        registry.registerFunction(0x140670, parseArguments);
        registry.registerFunction(0x216118, mainLoopTick);
        registry.registerFunction(0x18c7d0, addExperience);
        if (std::getenv("TTA_DAMAGE_TRACE"))
        {
            registry.registerFunction(0x11b288, damageTrace<0x11b288u, func_0x11b288>);
            registry.registerFunction(0x11b368, damageTrace<0x11b368u, func_0x11b368>);
        }
        registry.registerFunction(0x18bcd8, battleHpChange);
        registry.registerFunction(0x110e38, battleAction);
        registry.registerFunction(0x161630, battleWinner);
        if (std::getenv("TTA_STATE_TRACE"))
            registry.registerFunction(0x18f420, chapterMessageTrace);
        if (std::getenv("TTA_ENCOUNTER_TRACE"))
            registry.registerFunction(0x15b6c8, encounterTrace);
        if (std::getenv("TTA_ZONE_TRACE"))
        {
            registry.registerFunction(0x178f38, zoneTrace);
            registry.registerFunction(0x17bc98, zoneMessageTrace);
        }
#ifdef BOZO_SETTINGS_UI
        bozo::app::ui::registerTab("Debug", drawTab);
        bozo::app::ui::registerTab("Cheats", drawCheatsTab);
#endif
    }
}
