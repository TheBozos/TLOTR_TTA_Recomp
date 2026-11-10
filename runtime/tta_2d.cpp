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
                const char *e = env("TTA_2D_TRACE");
                return e && *e ? std::atoi(e) : 0;
            }();
            return level;
        }

        uint32_t frame(const uint8_t *rdram) { return read32(rdram, 0x3b5720); }

        // ---- Textures ------------------------------------------------------------------------------------------------
        std::string textureDir()
        {
            const char *e = env("TTA_TEXTURE_DIR");
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
            if (traceLevel() >= 4 || env("TTA_RES_TRACE"))
                std::printf("[tta:res] frame %u: %s %u at %x, %x bytes\n", frame(rdram), name, read32(rdram, res + 0x20),
                            payload, size);
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
            std::shared_ptr<const bozo::render::Texture> live; // an image the game builds in EE memory (the text canvas)
        };
        std::map<std::pair<uint32_t, uint32_t>, Placed> g_placed;

        // 0x2c80e8: sends the UI's text canvas (EA Apt's text manager TM = *(0x37c490), canvas S = TM+0x18: +0 width,
        // +4 height, +8 TBP, +0x20 pitch, +0x2c the pixels) and its palette, CBP = TBP - 0x10 (ui_spec 1). The canvas is
        // plain 8-bit linear memory the game's font rasteriser (0x2fb040) fills, its palette (i << 24) | 0xbfbfbf: each
        // byte is the text's coverage over grey. Natively it becomes an RGBA texture placed where the game put it.
        void uploadCanvas(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            func_0x2c80e8(rdram, ctx, rt);
            ctx->pc = ra;
            const uint32_t s = read32(rdram, 0x37c490) + 0x18;
            const uint32_t width = read32(rdram, s), height = read32(rdram, s + 4), tbp = read32(rdram, s + 8) & 0x3fffu;
            const uint32_t pitch = read32(rdram, s + 0x20);
            uint32_t pixels = read32(rdram, s + 0x2c);
            if (width == 0 || height == 0 || width > 1024 || height > 1024 || pitch < width)
                return;
            if (pixels < 0x100000u || pixels >= 0x2000000u) // inline rather than a pointer
                pixels = s + 0x2c;
            static uint64_t version = 0;
            auto t = std::make_shared<bozo::render::Texture>();
            t->width = width;
            t->height = height;
            t->rgba.resize(static_cast<size_t>(width) * height);
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint8_t *row = rdram + ((pixels + y * pitch) & kMask);
                for (uint32_t x = 0; x < width; ++x)
                    t->rgba[static_cast<size_t>(y) * width + x] = (static_cast<uint32_t>(row[x]) << 24) | 0xbfbfbfu;
            }
            t->key = 0x7474615465787443ull; // "ttaTextC"
            t->version = ++version;
            uint32_t lw = 0, lh = 0;
            while ((1u << lw) < width)
                ++lw;
            while ((1u << lh) < height)
                ++lh;
            g_placed[{tbp, 0x13u}] = Placed{0, {}, lw, lh, std::move(t)};
        }

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
                Placed placed{read64(rdram, r), {}, rdram[(r + 0x3f) & kMask], rdram[(r + 0x40) & kMask], nullptr};
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
        void drawImmediate(uint8_t *rdram, uint32_t ra, uint32_t type, uint32_t xyz, uint32_t rgba, uint32_t st,
                           uint32_t n, uint32_t p, const float m[16], uint32_t adcMask = 0);

        // A vertex between two (x, y, u, v and each colour byte interpolated).
        bozo::render::Vertex lerp(const bozo::render::Vertex &a, const bozo::render::Vertex &b, float t)
        {
            bozo::render::Vertex r = a;
            r.x = a.x + (b.x - a.x) * t;
            r.y = a.y + (b.y - a.y) * t;
            r.z = a.z + (b.z - a.z) * t;
            r.u = a.u + (b.u - a.u) * t;
            r.v = a.v + (b.v - a.v) * t;
            r.q = a.q + (b.q - a.q) * t;
            uint32_t c = 0;
            for (int k = 0; k < 4; ++k)
            {
                const float ca = static_cast<float>((a.rgba >> (k * 8)) & 0xffu), cb = static_cast<float>((b.rgba >> (k * 8)) & 0xffu);
                c |= static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f) << (k * 8);
            }
            r.rgba = c;
            return r;
        }

        // EA Apt's masks: shapes drawn with every colour channel masked (FBMSK 0xffffffff) write only depth; the masked
        // content follows with ZTST GEQUAL / GREATER and lands only inside them. A full-screen mask draw starts a new
        // mask (it resets the depth). Natively: the mask's triangles, and the content cut to their union.
        std::vector<bozo::render::Vertex> g_mask;
        uint32_t g_maskFrame = 0;

        // The part of triangle (a, b, c) inside the convex triangle m.
        void clipToTriangle(const bozo::render::Vertex *tri, const bozo::render::Vertex *m, std::vector<bozo::render::Vertex> &out)
        {
            std::vector<bozo::render::Vertex> poly(tri, tri + 3), next;
            const float area = (m[1].x - m[0].x) * (m[2].y - m[0].y) - (m[1].y - m[0].y) * (m[2].x - m[0].x);
            if (area == 0.0f)
                return;
            const float sign = area > 0 ? 1.0f : -1.0f;
            for (int e = 0; e < 3 && !poly.empty(); ++e)
            {
                const auto &p0 = m[e], &p1 = m[(e + 1) % 3];
                const auto side = [&](const bozo::render::Vertex &v)
                { return sign * ((p1.x - p0.x) * (v.y - p0.y) - (p1.y - p0.y) * (v.x - p0.x)); };
                next.clear();
                for (size_t i = 0; i < poly.size(); ++i)
                {
                    const auto &a = poly[i], &b = poly[(i + 1) % poly.size()];
                    const float da = side(a), db = side(b);
                    if (da >= 0)
                    {
                        next.push_back(a);
                        if (db < 0)
                            next.push_back(lerp(a, b, da / (da - db)));
                    }
                    else if (db >= 0)
                        next.push_back(lerp(a, b, da / (da - db)));
                }
                poly.swap(next);
            }
            for (size_t i = 2; i < poly.size(); ++i)
                out.insert(out.end(), {poly[0], poly[i - 1], poly[i]});
        }

        std::vector<bozo::render::Vertex> clipToMask(const std::vector<bozo::render::Vertex> &in)
        {
            std::vector<bozo::render::Vertex> out;
            for (size_t t = 0; t + 2 < in.size(); t += 3)
                for (size_t m = 0; m + 2 < g_mask.size(); m += 3)
                    clipToTriangle(&in[t], &g_mask[m], out);
            return out;
        }

        // Triangles cut to the rectangle [x0, x1) x [y0, y1) (Sutherland-Hodgman per triangle, then fanned again).
        std::vector<bozo::render::Vertex> clipTriangles(const std::vector<bozo::render::Vertex> &in, float x0, float y0, float x1,
                                                        float y1)
        {
            std::vector<bozo::render::Vertex> out, poly, next;
            for (size_t t = 0; t + 2 < in.size(); t += 3)
            {
                poly.assign(in.begin() + static_cast<std::ptrdiff_t>(t), in.begin() + static_cast<std::ptrdiff_t>(t) + 3);
                for (int edge = 0; edge < 4 && !poly.empty(); ++edge)
                {
                    const auto inside = [&](const bozo::render::Vertex &v)
                    { return edge == 0 ? v.x >= x0 : edge == 1 ? v.x <= x1 : edge == 2 ? v.y >= y0 : v.y <= y1; };
                    const auto cut = [&](const bozo::render::Vertex &a, const bozo::render::Vertex &b)
                    {
                        const float da = edge == 0 ? a.x - x0 : edge == 1 ? x1 - a.x : edge == 2 ? a.y - y0 : y1 - a.y;
                        const float db = edge == 0 ? b.x - x0 : edge == 1 ? x1 - b.x : edge == 2 ? b.y - y0 : y1 - b.y;
                        return lerp(a, b, da / (da - db));
                    };
                    next.clear();
                    for (size_t i = 0; i < poly.size(); ++i)
                    {
                        const auto &a = poly[i], &b = poly[(i + 1) % poly.size()];
                        if (inside(a))
                        {
                            next.push_back(a);
                            if (!inside(b))
                                next.push_back(cut(a, b));
                        }
                        else if (inside(b))
                            next.push_back(cut(a, b));
                    }
                    poly.swap(next);
                }
                for (size_t i = 2; i < poly.size(); ++i)
                    out.insert(out.end(), {poly[0], poly[i - 1], poly[i]});
            }
            return out;
        }

        void primitive(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            const uint32_t type = ctx->r[4].uw[0] & 0xffu, xyz = ctx->r[5].uw[0], rgba = ctx->r[6].uw[0],
                           st = ctx->r[7].uw[0], n = ctx->r[8].uw[0];
            const uint32_t p = read32(rdram, 0x36f870);
            float m[16];
            const uint32_t at = matrixAddress(rdram, p);
            for (int i = 0; i < 16; ++i)
                m[i] = readFloat(rdram, at + i * 4);
            func_0x218910(rdram, ctx, rt);
            ctx->pc = ra;
            drawImmediate(rdram, ra, type, xyz, rgba, st, n, p, m);
        }

        // 0x218fb8(prim, xyz, adc mask, rgba, st, n): the same immediate primitive for EA Apt's shapes and bitmaps (0x2c9a08:
        // the title, menus, HUD), its arrays at 0x418610 (xyz) and 0x418810 (st), the same matrix choice.
        void strip(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            const uint32_t type = ctx->r[4].uw[0] & 0xffu, xyz = ctx->r[5].uw[0], rgba = ctx->r[7].uw[0],
                           st = ctx->r[8].uw[0], n = ctx->r[9].uw[0], adcMask = ctx->r[6].uw[0]; // bit i: vertex i closes no triangle
            const uint32_t p = read32(rdram, 0x36f870);
            float m[16];
            const uint32_t at = matrixAddress(rdram, p);
            for (int i = 0; i < 16; ++i)
                m[i] = readFloat(rdram, at + i * 4);
            func_0x218fb8(rdram, ctx, rt);
            ctx->pc = ra;
            drawImmediate(rdram, ra, type, xyz, rgba, st, n, p, m, adcMask);
        }

        void drawImmediate(uint8_t *rdram, uint32_t ra, uint32_t type, uint32_t xyz, uint32_t rgba, uint32_t st,
                           uint32_t n, uint32_t p, const float m[16], uint32_t adcMask)
        {
            if (traceLevel() >= 6)
            {
                const uint32_t ds = read32(rdram, 0x36f850);
                std::printf("[tta:2d] prim %x from %x: DS+0x6a0 %llx +0x6b0 %llx +0x6c0 %llx\n", type, ra,
                            static_cast<unsigned long long>(read64(rdram, ds + 0x6a0)),
                            static_cast<unsigned long long>(read64(rdram, ds + 0x6b0)),
                            static_cast<unsigned long long>(read64(rdram, ds + 0x6c0)));
            }
            // prim: the GS PRIM bits the caller passes (type in 0-2; IIP 8, TME 0x10, FGE 0x20, ABE 0x40).
            const uint32_t prim = type;
            type = prim & 7u;
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
            // FRAME's FBMSK (its upper word): a draw that writes no colour channel only writes the alpha the game masks
            // with (EA Apt's masks): nothing to show.
            const bool alphaOnly = ((frameReg >> 32) & 0xffffffu) == 0xffffffu;

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

            if (traceLevel() >= 8)
            {
                float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                for (const auto &q : points)
                {
                    bx0 = std::fmin(bx0, q.x);
                    by0 = std::fmin(by0, q.y);
                    bx1 = std::fmax(bx1, q.x);
                    by1 = std::fmax(by1, q.y);
                }
                std::printf("[tta:mask] frame %u from %x type %u n %u FBMSK %08x TEST %llx ALPHA %llx PRMODE %llx tex %d "
                            "(%.0f,%.0f)-(%.0f,%.0f) rgba %08x\n",
                            frame(rdram), ra, type, n, static_cast<uint32_t>(frameReg >> 32),
                            static_cast<unsigned long long>(test), static_cast<unsigned long long>(alpha),
                            static_cast<unsigned long long>(prmode), textured ? 1 : 0, bx0, by0, bx1, by1, points[0].rgba);
            }
            const bool fullScreenMask = alphaOnly && [&]
            {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const auto &q : points)
                {
                    x0 = std::fmin(x0, q.x);
                    y0 = std::fmin(y0, q.y);
                    x1 = std::fmax(x1, q.x);
                    y1 = std::fmax(y1, q.y);
                }
                return x0 <= 1793.0f && y0 <= 1825.0f && x1 >= 2303.0f && y1 >= 2271.0f;
            }();
            if (frame(rdram) != g_maskFrame)
            {
                g_mask.clear();
                g_maskFrame = frame(rdram);
            }
            if (fullScreenMask)
                g_mask.clear();
            const uint32_t ztest = (test >> 16) & 7u; // ZTE bit 16, ZTST bits 17-18
            const bool masked = !alphaOnly && (ztest & 1u) && ((ztest >> 1) == 2u || (ztest >> 1) == 3u) && !g_mask.empty();

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
                // The draw's window must be what was placed there (a record, or the UI's text canvas).
                const uint32_t tw = (tex0 >> 26) & 0xfu, th = (tex0 >> 30) & 0xfu;
                if (placed != g_placed.end() && placed->second.logWidth == tw && placed->second.logHeight == th)
                    picture = placed->second.live ? placed->second.live
                              : placed->second.file.empty() ? nullptr
                                                            : texture(placed->second.file);
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
                {
                    if (i < 32 && (adcMask >> i) & 1u) // the GS's ADC: this vertex closes no triangle
                        continue;
                    triangles.insert(triangles.end(), {points[i - 2], points[i - 1], points[i]});
                }
            else
                for (uint32_t i = 2; i < n; ++i)
                    triangles.insert(triangles.end(), {points[0], points[i - 1], points[i]});
            if (alphaOnly)
            {
                if (!fullScreenMask)
                    g_mask.insert(g_mask.end(), triangles.begin(), triangles.end());
                return count(rdram, false);
            }
            if (masked)
                triangles = clipToMask(triangles);
            // The GS scissor (SCISSOR_1, DS+0x110: x0 bits 0-10, x1 16-26, y0 32-42, y1 48-58, in the frame's pixels):
            // EA Apt clips its minimap and panels with it. The overlay has no clip of its own: the triangles are cut.
            {
                const uint64_t scissor = read64(rdram, read32(rdram, 0x36f850) + 0x110);
                const float sx0 = 1792.0f + static_cast<float>(scissor & 0x7ffu),
                            sx1 = 1792.0f + static_cast<float>((scissor >> 16) & 0x7ffu) + 1.0f,
                            sy0 = 1824.0f + static_cast<float>((scissor >> 32) & 0x7ffu),
                            sy1 = 1824.0f + static_cast<float>((scissor >> 48) & 0x7ffu) + 1.0f;
                if (traceLevel() >= 7 && (points[0].rgba & 0xffffffu) == 0x31492bu)
                    std::printf("[tta:2d] scissor %llx -> (%.0f,%.0f)-(%.0f,%.0f), DS+0x1f0 %llx frame %llx\n",
                                static_cast<unsigned long long>(scissor), sx0, sy0, sx1, sy1,
                                static_cast<unsigned long long>(test), static_cast<unsigned long long>(alpha),
                                static_cast<unsigned long long>(frameReg));
                if (sx0 > 1792.0f || sy0 > 1824.0f || sx1 < 2304.0f || sy1 < 2272.0f)
                    triangles = clipTriangles(triangles, sx0, sy0, sx1, sy1);
            }
            if (triangles.empty())
                return count(rdram, false);
            bozo::render::Renderer::instance().drawOverlay(material, triangles.data(),
                                                           static_cast<uint32_t>(triangles.size()), picture);
            if (traceLevel() >= 2)
                std::printf("[tta:2d] frame %u: type %u n %u from %x tex %s (TEX0 %016llx: TBW %u TW %u TH %u CBP %x) blend %d "
                            "(%.1f,%.1f)-(%.1f,%.1f) st (%.3f,%.3f)-(%.3f,%.3f) rgba %08x rgba-arg %x\n",
                            frame(rdram), type, n, ra,
                            picture ? g_placed[{static_cast<uint32_t>(tex0 & 0x3fffu), static_cast<uint32_t>((tex0 >> 20) & 0x3fu)}].file.c_str() : "-",
                            static_cast<unsigned long long>(tex0), static_cast<uint32_t>((tex0 >> 14) & 0x3fu),
                            static_cast<uint32_t>((tex0 >> 26) & 0xfu), static_cast<uint32_t>((tex0 >> 30) & 0xfu),
                            static_cast<uint32_t>((tex0 >> 37) & 0x3fffu), static_cast<int>(material.blend), points[0].x,
                            points[0].y, points[n - 1].x, points[n - 1].y, points[0].u, points[0].v, points[n - 1].u,
                            points[n - 1].v, points[0].rgba, rgba);
            count(rdram, true);
        }
    }

    namespace
    {
        std::string guestString(const uint8_t *rdram, uint32_t at, size_t max = 120)
        {
            std::string s;
            for (size_t i = 0; i < max; ++i)
            {
                const uint8_t c = rdram[(at + i) & kMask];
                if (!c)
                    break;
                s += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '.';
            }
            return s;
        }

        // 0x1bbea0(font, text state, string): lays out one queued string (render map 3.2.4).
        void layoutString(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], a0 = ctx->r[4].uw[0], a1 = ctx->r[5].uw[0], a2 = ctx->r[6].uw[0];
            if (traceLevel() >= 5 || (env("TTA_TEXT_TRACE") && frame(rdram) % 30 == 0))
                std::printf("[tta:text] frame %u: %x %x %x \"%s\" at (%.1f, %.1f)\n", frame(rdram), a0, a1, a2,
                            guestString(rdram, a2).c_str(), readFloat(rdram, a1 + 0x70), readFloat(rdram, a1 + 0x74));
            func_0x1bbea0(rdram, ctx, rt);
            ctx->pc = ra;
        }
    }

    bool resourceAt(uint32_t address, std::string &type, uint32_t &index, uint32_t &offset)
    {
        uint32_t size = 0;
        return resourceAt(address, type, index, offset, size);
    }

    bool resourceAt(uint32_t address, std::string &type, uint32_t &index, uint32_t &offset, uint32_t &size)
    {
        auto it = g_resources.upper_bound(address);
        if (it == g_resources.begin())
            return false;
        --it;
        if (address >= it->second.end)
            return false;
        type = it->second.type;
        index = it->second.index;
        offset = address - it->first;
        size = it->second.end - it->first;
        return true;
    }

    std::shared_ptr<const bozo::render::Texture> textureById(uint64_t id)
    {
        const Index &idx = index();
        auto it = idx.byId.find(id);
        return it == idx.byId.end() ? nullptr : texture(it->second);
    }

    void installTwoD(bozo::port::Registry &registry)
    {
        registry.registerFunction(0x1bbea0, layoutString);
        registry.registerFunction(0x2c80e8, uploadCanvas);
        registry.registerFunction(0x1babe8, loadResource);
        registry.registerFunction(0x219ca8, uploadGroup);
        registry.registerFunction(0x218910, primitive);
        registry.registerFunction(0x218fb8, strip);
    }
}
