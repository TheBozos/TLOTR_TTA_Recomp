// The Third Age's 3D on BozoEngine's renderer (render map 3.3): the shader renders of LLObj nodes (terrain type 21
// 0x221178, characters type 17 0x225fb0, ...), drawn from meshes converted offline from the player's disc
// (tools/tta_meshes.py), keyed by the node's record in its resource, with the game's matrices.
#include "tta_port.h"

#include "tta_functions.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

namespace tta
{
    namespace
    {
        constexpr uint32_t kMask = 0x1ffffffu;

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & kMask), 4);
            return v;
        }

        // TTA_3D_TRACE=1: per frame, the nodes each shader render drew, by their key.
        bool trace()
        {
            static const bool on = std::getenv("TTA_3D_TRACE") != nullptr;
            return on;
        }

        uint32_t frame(const uint8_t *rdram) { return read32(rdram, 0x3b5720); }

        uint32_t g_frameSeen = 0;
        std::map<std::tuple<std::string, std::string, uint32_t, uint32_t>, uint32_t> g_seen;

        // A shader render (instance = node + 0x28; the node's first word is its record in the resource).
        void note(const uint8_t *rdram, const char *kind, uint32_t instance)
        {
            if (!trace())
                return;
            if (frame(rdram) != g_frameSeen)
            {
                if (frame(rdram) % 60 == 0)
                    for (const auto &[key, count] : g_seen)
                        std::printf("[tta:3d] frame %u: %s %s %u +%x x%u\n", g_frameSeen, std::get<0>(key).c_str(),
                                    std::get<1>(key).c_str(), std::get<2>(key), std::get<3>(key), count);
                g_seen.clear();
                g_frameSeen = frame(rdram);
            }
            const uint32_t record = read32(rdram, instance - 0x28);
            std::string type = "-";
            uint32_t index = 0, offset = 0;
            resourceAt(record, type, index, offset);
            ++g_seen[{kind, type, index, offset}];
        }

        void terrain(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            note(rdram, "terrain", ctx->r[4].uw[0]);
            func_0x221178(rdram, ctx, rt);
            ctx->pc = ra;
        }

        void skin(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            note(rdram, "skin", ctx->r[4].uw[0]);
            func_0x225fb0(rdram, ctx, rt);
            ctx->pc = ra;
        }
    }

    void installThreeD(bozo::port::Registry &registry)
    {
        registry.registerFunction(0x221178, terrain);
        registry.registerFunction(0x225fb0, skin);
    }
}
