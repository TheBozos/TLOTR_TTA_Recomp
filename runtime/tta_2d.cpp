// The Third Age's 2D on BozoEngine's renderer (render map 3.2): the immediate primitives of LLPrims 0x218910 (legal
// and loading screens, title, menus, effects), drawn natively from the game's own data.
//
// Nothing is read from the packets the game builds (they are built and never sent, tta_render.cpp):
// - the vertices are 0x218910's arguments (positions, colours, texture coordinates as floats), taken to the screen
//   with the matrix the game chose, as its VU0 code does;
// - the material (ALPHA, TEST, PRMODE, TEX0) is the game's draw state, its LLDisSt object DS = *(0x36f850), read only;
// - the texture is the game's texture record, found by where the game placed its group (0x219ca8), and its pixels
//   come from the offline conversion of the player's disc (tools/tta_textures.py), keyed by the record's 64-bit id.
#include "tta_port.h"

#include "tta_functions.h"

#include "bozo/render/renderer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

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

        uint64_t read64(const uint8_t *rdram, uint32_t address)
        {
            uint64_t v;
            std::memcpy(&v, rdram + (address & kMask), 8);
            return v;
        }

        uint16_t read16(const uint8_t *rdram, uint32_t address)
        {
            uint16_t v;
            std::memcpy(&v, rdram + (address & kMask), 2);
            return v;
        }

        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float v;
            std::memcpy(&v, rdram + (address & kMask), 4);
            return v;
        }

        // TTA_2D_TRACE=1: per frame, the primitives drawn and skipped; 2: each primitive.
        int traceLevel()
        {
            static const int level = []
            {
                const char *e = std::getenv("TTA_2D_TRACE");
                return e && *e ? std::atoi(e) : 0;
            }();
            return level;
        }

        uint32_t frame(const uint8_t *rdram) { return read32(rdram, 0x3b5720); }

        // ---- Textures ------------------------------------------------------------------------------------------------
        std::string textureDir()
        {
            const char *e = std::getenv("TTA_TEXTURE_DIR");
            return e && *e ? e : TTA_TEXTURE_DIR;
        }

        // The converted pack's index (tools/tta_textures.py): each texture by its id (the first of its variants) and by
        // where it is on the disc, (resource type, index, the group's offset in the resource, record). The UI's groups
        // (apxf resources) reuse small ids (0x1c in 582 images), so their textures are found by place; the texture
        // groups' (txf*) ids are unique.
        struct Index
        {
            std::unordered_map<uint64_t, std::string> byId;
            std::map<std::tuple<std::string, uint32_t, uint32_t, uint32_t>, std::string> byPlace;
        };

        const Index &index()
        {
            static const Index idx = []
            {
                Index i;
                FILE *f = std::fopen((textureDir() + "/index.txt").c_str(), "r");
                if (!f)
                {
                    std::fprintf(stderr, "[tta:2d] no texture pack at %s (tools/tta_textures.py)\n", textureDir().c_str());
                    return i;
                }
                char line[512];
                while (std::fgets(line, sizeof(line), f))
                {
                    if (line[0] == '#')
                        continue;
                    // id  file/(type,index)  offset  record  psm  w  h  tbp0  tbw  clut  outfile
                    char id[32], where[256], offset[32], record[16], rest[256];
                    if (std::sscanf(line, "%31s %255s %31s %15s %255[^\n]", id, where, offset, record, rest) != 5)
                        continue;
                    const char *last = std::strrchr(rest, ' ');
                    const std::string file = last ? last + 1 : rest;
                    const char *open = std::strrchr(where, '(');
                    char type[8] = {};
                    unsigned resourceIndex = 0;
                    if (!open || std::sscanf(open, "(%7[^,],%u)", type, &resourceIndex) != 2)
                        continue;
                    i.byId.emplace(std::strtoull(id, nullptr, 16), file); // the first of its variants
                    i.byPlace.emplace(std::make_tuple(std::string(type), static_cast<uint32_t>(resourceIndex),
                                                      static_cast<uint32_t>(std::strtoul(offset, nullptr, 16)),
                                                      static_cast<uint32_t>(std::atoi(record))),
                                      file);
                }
                std::fclose(f);
                return i;
            }();
            return idx;
        }

        // A converted texture by its file in the pack ("TTAX", u16 width, u16 height, u32 flags, u32 0, RGBA with the
        // GS's alpha); null when it cannot be read.
        std::shared_ptr<const bozo::render::Texture> texture(const std::string &file)
        {
            static std::unordered_map<std::string, std::shared_ptr<const bozo::render::Texture>> cache;
            if (auto it = cache.find(file); it != cache.end())
                return it->second;
            std::shared_ptr<bozo::render::Texture> t;
            if (FILE *f = std::fopen((textureDir() + "/" + file).c_str(), "rb"))
            {
                uint8_t header[16];
                if (std::fread(header, 1, 16, f) == 16 && std::memcmp(header, "TTAX", 4) == 0)
                {
                    t = std::make_shared<bozo::render::Texture>();
                    t->width = header[4] | header[5] << 8;
                    t->height = header[6] | header[7] << 8;
                    t->rgba.resize(static_cast<size_t>(t->width) * t->height);
                    if (std::fread(t->rgba.data(), 4, t->rgba.size(), f) != t->rgba.size())
                        t.reset();
                    else
                    {
                        t->key = std::hash<std::string>{}(file) | 1u;
                        t->version = 1;
                    }
                }
                std::fclose(f);
            }
            cache[file] = t;
            return t;
        }

        // The resources the game has loaded (0x1babe8 hands each to its type's loader; the object: +0 payload, +0x1c
        // type, +0x20 index, +0x24 size): their payload's range, type and index, to name the resource a texture group
        // lies in.
        struct Resource
        {
            uint32_t end;
            std::string type;
            uint32_t index;
        };
        std::map<uint32_t, Resource> g_resources; // by payload start

        void loadResource(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], res = ctx->r[4].uw[0];
            const uint32_t payload = read32(rdram, res), size = read32(rdram, res + 0x24), type = read32(rdram, res + 0x1c);
            const char name[5] = {static_cast<char>(type >> 24), static_cast<char>(type >> 16), static_cast<char>(type >> 8),
                                  static_cast<char>(type), 0};
            // A new resource replaces those whose memory it takes.
            auto it = g_resources.lower_bound(payload);
            if (it != g_resources.begin() && std::prev(it)->second.end > payload)
                --it;
            while (it != g_resources.end() && it->first < payload + size)
                it = g_resources.erase(it);
            g_resources[payload] = {payload + size, name, read32(rdram, res + 0x20)};
            if (traceLevel() >= 4)
                std::printf("[tta:res] %s %u at %x, %x bytes\n", name, read32(rdram, res + 0x20), payload, size);
            func_0x1babe8(rdram, ctx, rt);
            ctx->pc = ra;
        }

        // Where the game placed its textures: a group's records carry their TBP0 relocated to the group's VRAM base when
        // the group is uploaded (0x219ca8); (TBP0, PSM) -> the record's id and its converted file ("": none).
        struct Placed
        {
            uint64_t id;
            std::string file;
            uint32_t logWidth, logHeight; // the record's size (+0x3f, +0x40)
        };
        std::map<std::pair<uint32_t, uint32_t>, Placed> g_placed;

        // 0x219ca8(group): sends a texture group to GS memory for the draws that follow (the game re-sends a group
        // before the objects that use it). Natively nothing is sent: the group's records say where each texture now is.
        void uploadGroup(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], group = ctx->r[4].uw[0];
            func_0x219ca8(rdram, ctx, rt);
            const uint32_t records = read32(rdram, group + 0x20), count = read16(rdram, group + 0x12);
            // The resource holding the group (the loading screen's is in the executable: none).
            const Resource *resource = nullptr;
            uint32_t offset = 0;
            if (auto it = g_resources.upper_bound(group); it != g_resources.begin())
            {
                --it;
                if (group < it->second.end)
                {
                    resource = &it->second;
                    offset = group - it->first;
                }
            }
            const Index &idx = index();
            for (uint32_t i = 0; i < count && i < 4096; ++i)
            {
                const uint32_t r = records + i * 0x50;
                Placed placed{read64(rdram, r), {}, rdram[(r + 0x3f) & kMask], rdram[(r + 0x40) & kMask]};
                if (resource)
                    if (auto at = idx.byPlace.find({resource->type, resource->index, offset, i}); at != idx.byPlace.end())
                        placed.file = at->second;
                if (placed.file.empty())
                    if (auto at = idx.byId.find(placed.id); at != idx.byId.end())
                        placed.file = at->second;
                if (traceLevel() >= 3)
                    std::printf("[tta:2d] group %x (%s %u +%x) record %u at TBP0 %x PSM %x: id %016llx -> %s\n", group,
                                resource ? resource->type.c_str() : "-", resource ? resource->index : 0, offset, i,
                                read16(rdram, r + 0x10), rdram[(r + 0x3c) & kMask],
                                static_cast<unsigned long long>(placed.id), placed.file.empty() ? "none" : placed.file.c_str());
                g_placed[{read16(rdram, r + 0x10), rdram[(r + 0x3c) & kMask]}] = std::move(placed);
            }
            ctx->pc = ra;
        }

        // ---- Primitives ----------------------------------------------------------------------------------------------
        uint32_t g_frameSeen = 0, g_drawn = 0, g_skipped = 0;

        void count(const uint8_t *rdram, bool drawn)
        {
            if (traceLevel() < 1)
                return;
            if (frame(rdram) != g_frameSeen)
            {
                if (g_drawn || g_skipped)
                    std::printf("[tta:2d] frame %u: %u drawn, %u skipped\n", g_frameSeen, g_drawn, g_skipped);
                g_frameSeen = frame(rdram);
                g_drawn = g_skipped = 0;
            }
            ++(drawn ? g_drawn : g_skipped);
        }

        // The matrix 0x218910 takes its points with (P+0x90): 0 the object's own (P+0x40), 2 the render context's
        // screen matrix (RC+0x2e0), else its view matrix (RC+0x60); RC = **(0x36f350). Four columns of four floats.
        uint32_t matrixAddress(const uint8_t *rdram, uint32_t p)
        {
            const uint8_t mode = rdram[(p + 0x90) & kMask];
            if (mode == 0)
                return p + 0x40;
            const uint32_t rc = read32(rdram, read32(rdram, 0x36f350));
            return rc + (mode == 2 ? 0x2e0 : 0x60);
        }

        // 0x218910 LLPrims_Draw(prim, xyz, rgba, st, n): n points (float4 each) with optional colours (float4, 1.0 =
        // 255; none: P+0xc0) and texture coordinates (float4: s, t), as primitive `prim` (the GS's types: 3 triangles,
        // 4 strip, 5 fan, 6 sprites). The game's own code still runs (its buffers stay as it keeps them).
        void primitive(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            const uint32_t type = ctx->r[4].uw[0] & 7u, xyz = ctx->r[5].uw[0], rgba = ctx->r[6].uw[0],
                           st = ctx->r[7].uw[0], n = ctx->r[8].uw[0];
            const uint32_t p = read32(rdram, 0x36f870);
            float m[16];
            const uint32_t at = matrixAddress(rdram, p);
            for (int i = 0; i < 16; ++i)
                m[i] = readFloat(rdram, at + i * 4);
            func_0x218910(rdram, ctx, rt);
            ctx->pc = ra;
            if (n == 0 || n > 4096 || type < 3 || type > 6)
                return count(rdram, false);

            // The draw state (context 1 block at DS+0x100; PRMODE at DS+0x6b0: ABE 0x40, TME 0x10).
            const uint32_t ds = read32(rdram, 0x36f850);
            const uint64_t prmode = read64(rdram, ds + 0x6b0), alpha = read64(rdram, ds + 0x100),
                           test = read64(rdram, ds + 0x140), frameReg = read64(rdram, ds + 0x150),
                           tex0 = read64(rdram, ds + 0x1a0);
            const bool textured = (prmode & 0x10u) != 0, blended = (prmode & 0x40u) != 0;
            // Only the displayed frame, double-buffered at FBP 0 and 0x70 (512 x 448 x 4 bytes = 0x70 pages): the
            // off-screen targets (FBP 0x1c0: the menus' blurred background) are not drawn yet.
            const uint32_t fbp = frameReg & 0x1ffu;
            if (fbp != 0 && fbp != 0x70)
            {
                if (traceLevel() >= 2)
                    std::printf("[tta:2d] frame %u: target FBP %x (FBW %u PSM %x) type %u n %u from %x tex %d TBP0 %x\n",
                                frame(rdram), static_cast<uint32_t>(frameReg & 0x1ffu),
                                static_cast<uint32_t>((frameReg >> 16) & 0x3fu), static_cast<uint32_t>((frameReg >> 24) & 0x3fu),
                                type, n, ra, textured ? 1 : 0, static_cast<uint32_t>(tex0 & 0x3fffu));
                return count(rdram, false);
            }
            // The movie's sprites (0x22b050) draw the picture the backdrop already shows.
            if (ra == 0x22b5ec)
                return count(rdram, false);

            std::vector<bozo::render::Vertex> points(n);
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t a = xyz + i * 16;
                const float v[4] = {readFloat(rdram, a), readFloat(rdram, a + 4), readFloat(rdram, a + 8),
                                    readFloat(rdram, a + 12)};
                float r[4];
                for (int c = 0; c < 4; ++c)
                    r[c] = m[c] * v[0] + m[4 + c] * v[1] + m[8 + c] * v[2] + m[12 + c] * v[3];
                if (r[3] != 1.0f && r[3] != 0.0f)
                    for (int c = 0; c < 3; ++c)
                        r[c] /= r[3];
                bozo::render::Vertex &out = points[i];
                out.x = r[0];
                out.y = r[1];
                out.z = 0.0f;
                if (st)
                {
                    out.u = readFloat(rdram, st + i * 16);
                    out.v = readFloat(rdram, st + i * 16 + 4);
                }
                const uint32_t c = rgba ? rgba + i * 16 : p + 0xc0;
                uint32_t packed = 0;
                for (int k = 0; k < 4; ++k)
                {
                    const float f = readFloat(rdram, c + k * 4) * 255.0f;
                    const int32_t b = static_cast<int32_t>(f); // the game's VFTOI0: truncated
                    packed |= static_cast<uint32_t>(b < 0 ? 0 : b > 255 ? 255 : b) << (k * 8);
                }
                out.rgba = packed;
            }

            // An opaque untextured sprite over the whole screen is the frame's clear (0x211de4 at each frame's start):
            // the window's clear colour, not a draw over the movie's backdrop.
            if (type == 6 && !textured && !blended && n == 2)
            {
                const float x0 = std::fmin(points[0].x, points[1].x), x1 = std::fmax(points[0].x, points[1].x);
                const float y0 = std::fmin(points[0].y, points[1].y), y1 = std::fmax(points[0].y, points[1].y);
                if (x0 <= 1792.0f + 1.0f && y0 <= 1824.0f + 1.0f && x1 >= 2304.0f - 1.0f && y1 >= 2272.0f - 1.0f)
                {
                    bozo::render::Renderer::instance().setClearColour(points[1].rgba & 0xffffffu);
                    return count(rdram, false);
                }
            }

            bozo::render::Material material;
            material.depthTest = false;
            material.depthWrite = false;
            material.blend = blended ? bozo::render::blendOf(alpha) : bozo::render::Material::Blend::Opaque;
            if (test & 1u) // ATE: ATST in bits 1-3, AREF in 4-11
            {
                material.alphaTest = true;
                material.alphaFunc = static_cast<bozo::render::Material::AlphaFunc>((test >> 1) & 7u);
                material.alphaRef = static_cast<uint8_t>((test >> 4) & 0xffu);
            }
            std::shared_ptr<const bozo::render::Texture> picture;
            if (textured)
            {
                const uint32_t tbp0 = tex0 & 0x3fffu, psm = (tex0 >> 20) & 0x3fu;
                const auto placed = g_placed.find({tbp0, psm});
                // The draw's window must be the record's texture: the UI renderer (0x305330) also reads a group's memory
                // as one larger 8-bit image (TW/TH 9 over 32 x 32 records), which no record describes (not drawn yet).
                const uint32_t tw = (tex0 >> 26) & 0xfu, th = (tex0 >> 30) & 0xfu;
                if (placed != g_placed.end() && !placed->second.file.empty() && placed->second.logWidth == tw &&
                    placed->second.logHeight == th)
                    picture = texture(placed->second.file);
                if (!picture)
                {
                    if (traceLevel() >= 2)
                        std::printf("[tta:2d] frame %u: no texture for TBP0 %x PSM %x (%s %016llx) from %x\n", frame(rdram),
                                    tbp0, psm, placed == g_placed.end() ? "not placed" : "not converted",
                                    placed == g_placed.end() ? 0ull : static_cast<unsigned long long>(placed->second.id), ra);
                    return count(rdram, false);
                }
                material.textured = true;
                material.tex0 = picture->key;
                material.textureAlpha = ((tex0 >> 34) & 1u) != 0;
                const uint32_t tfx = (tex0 >> 35) & 3u;
                material.textureFunction = static_cast<bozo::render::Material::TextureFunction>(tfx);
                // The game's s, t are 0..1 across the texture's 2^TW x 2^TH; the converted image is that size.
            }
            else
                material.textured = false;

            // The primitive as triangles.
            std::vector<bozo::render::Vertex> triangles;
            if (type == 6) // sprites: two corners each
                for (uint32_t i = 0; i + 1 < n; i += 2)
                {
                    const bozo::render::Vertex &a = points[i], &b = points[i + 1];
                    bozo::render::Vertex c[4] = {a, a, a, b};
                    c[1].x = b.x;
                    c[1].u = b.u;
                    c[2].y = b.y;
                    c[2].v = b.v;
                    for (auto &k : c)
                        k.rgba = b.rgba; // a sprite takes its colour from the second corner (flat)
                    triangles.insert(triangles.end(), {c[0], c[1], c[2], c[1], c[3], c[2]});
                }
            else if (type == 3)
                triangles.assign(points.begin(), points.begin() + (n / 3) * 3);
            else if (type == 4)
                for (uint32_t i = 2; i < n; ++i)
                    triangles.insert(triangles.end(), {points[i - 2], points[i - 1], points[i]});
            else
                for (uint32_t i = 2; i < n; ++i)
                    triangles.insert(triangles.end(), {points[0], points[i - 1], points[i]});
            if (triangles.empty())
                return count(rdram, false);
            bozo::render::Renderer::instance().drawOverlay(material, triangles.data(),
                                                           static_cast<uint32_t>(triangles.size()), picture);
            if (traceLevel() >= 2)
                std::printf("[tta:2d] frame %u: type %u n %u from %x tex %s (TEX0 %016llx: TBW %u TW %u TH %u CBP %x) blend %d "
                            "(%.1f,%.1f)-(%.1f,%.1f) st (%.3f,%.3f)-(%.3f,%.3f)\n",
                            frame(rdram), type, n, ra,
                            picture ? g_placed[{static_cast<uint32_t>(tex0 & 0x3fffu), static_cast<uint32_t>((tex0 >> 20) & 0x3fu)}].file.c_str() : "-",
                            static_cast<unsigned long long>(tex0), static_cast<uint32_t>((tex0 >> 14) & 0x3fu),
                            static_cast<uint32_t>((tex0 >> 26) & 0xfu), static_cast<uint32_t>((tex0 >> 30) & 0xfu),
                            static_cast<uint32_t>((tex0 >> 37) & 0x3fffu), static_cast<int>(material.blend), points[0].x,
                            points[0].y, points[n - 1].x, points[n - 1].y, points[0].u, points[0].v, points[n - 1].u,
                            points[n - 1].v);
            count(rdram, true);
        }
    }

    void installTwoD(bozo::port::Registry &registry)
    {
        registry.registerFunction(0x1babe8, loadResource);
        registry.registerFunction(0x219ca8, uploadGroup);
        registry.registerFunction(0x218910, primitive);
    }
}
