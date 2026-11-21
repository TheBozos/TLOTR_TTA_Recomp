// The Third Age's frames on BozoEngine's renderer: what the game sends to the GS through its one VIF1 chain is taken
// natively at the game functions that build it, and the chain itself is never sent (docs/RECOMP_NOTES.md "Rendering").
//
// Frame (render map 1.3-1.4): the frame swap 0x212478 is the frame's end (it called sceGsSyncV, the frame boundary), so
// it publishes what the renderer was given and starts the next frame; the chain kick 0x216470 only keeps the game's
// bookkeeping (the frame's chain is "done" at once). Movies (3.1): the per-macroblock GS uploads 0x22af40 are not made;
// the picture BozoEngine's libmpeg decoded is the frame's backdrop instead.
#include "tta_port.h"

#include "bozo/render/movie.h"
#include "bozo/render/renderer.h"
#include "bozo/runtime_api.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

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

        void write32(uint8_t *rdram, uint32_t address, uint32_t v) { std::memcpy(rdram + (address & kMask), &v, 4); }

        void ret(R5900Context *ctx) { ctx->pc = ctx->r[31].uw[0]; }

        // The game's screen: 512 x 448 (0x3b5726 / 0x3b5728), drawn at the GS XYOFFSET origin (2048 - 256, 2048 - 224).
        bozo::render::Screen screen()
        {
            bozo::render::Screen s;
            s.width = 512.0f;
            s.height = 448.0f;
            s.originX = 2048.0f - 256.0f;
            s.originY = 2048.0f - 224.0f;
            return s;
        }

        // TTA_RENDER_TRACE=1: one line per frame swap.
        bool trace()
        {
            static const bool on = [] { const char *e = env("TTA_RENDER_TRACE"); return e && *e == '1'; }();
            return on;
        }

        // ---- Movies ----------------------------------------------------------------------------------------------
        // The latest picture sceMpegGetPicture handed the game (libmpeg's hook, on the game thread), and whether the
        // game drew a movie this frame (0x22af40 called).
        std::mutex g_movieMutex;
        std::shared_ptr<const bozo::render::Texture> g_moviePicture;
        uint64_t g_movieVersion = 0;
        bool g_movieDrawn = false;

        void onMoviePicture(uint32_t, uint32_t width, uint32_t height, const uint8_t *rgba)
        {
            auto t = std::make_shared<bozo::render::Texture>();
            t->width = width;
            t->height = height;
            t->rgba.resize(static_cast<size_t>(width) * height);
            std::memcpy(t->rgba.data(), rgba, t->rgba.size() * 4);
            for (uint32_t &p : t->rgba)
                p |= 0xff000000u; // the movie is opaque
            t->key = 0x7474614d6f766965ull; // "ttaMovie"
            std::lock_guard<std::mutex> lock(g_movieMutex);
            t->version = ++g_movieVersion;
            g_moviePicture = std::move(t);
        }

        // 0x22af40(video draw, row, column?, ...): uploads the decoded picture (RGB32 at W+0x2c, 16 x 16 macroblocks
        // column-major) to GS memory, one image transfer per macroblock (0x22ab68), and returns how many it made; the
        // movie's sprites (0x218910, textured from that memory) follow. Natively the picture is the frame's backdrop:
        // nothing is uploaded, the count is the game's.
        void movieUpload(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            const uint32_t self = ctx->r[4].uw[0];
            const uint32_t rows = read32(rdram, self + 0x2b0 + ctx->r[6].uw[0] * 4);
            const int32_t height = static_cast<int32_t>(read32(rdram, read32(rdram, self + 0x36c) + 0x1040));
            const int32_t columns = (height + 15 >= 0 ? height + 15 : height + 30) >> 4;
            const int32_t r = static_cast<int32_t>(rows);
            ctx->r[2].sd[0] = r > 0 && columns > 0 ? static_cast<int64_t>(r) * columns : 0;
            g_movieDrawn = true;
            ret(ctx);
        }

        // ---- The frame ---------------------------------------------------------------------------------------------
        bool g_frameOpen = false;

        void beginFrame()
        {
            flushDraws();
            bozo::render::Renderer::instance().beginFrame(screen());
            g_frameOpen = true;
        }

        // 0x212478 frame swap: frame counter 0x3b5720 += 1; unless *(0x36f35c): sceGsSyncV (its field, inverted, to
        // 0x3b5724), FlushCache, sceGsSwapDBuffDc, sceGsSyncPath, the display registers (0x2120f8). Natively: the frame
        // drawn is published and presented, the next begins; the GS's double buffer and display registers are not
        // kept (the window shows the frame). It is the frame boundary (sceGsSyncV was): port::frameEnd defers a stop, so
        // it comes last. Callers: the frame end 0x1c3588 and the loading screens' 0x1c0120.
        void frameSwap(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            write32(rdram, 0x3b5720, read32(rdram, 0x3b5720) + 1);
            if (rdram[0x36f35c] == 0)
            {
                flushDraws(); // the frame's queued 3D draws (tta_3d.cpp) before it is published
                auto &r = bozo::render::Renderer::instance();
                if (!g_frameOpen)
                    beginFrame();
                if (g_movieDrawn)
                {
                    std::shared_ptr<const bozo::render::Texture> picture;
                    {
                        std::lock_guard<std::mutex> lock(g_movieMutex);
                        picture = g_moviePicture;
                    }
                    if (picture)
                        r.setBackdrop(picture, screen(), wideX(2048.0f - 256.0f), 2048.0f - 224.0f,
                                      widescreen() ? 512.0f * kWideSqueeze : 512.0f, 448.0f); // widescreen: pillarboxed
                }
                else
                    r.clearBackdrop();
                if (trace())
                    std::printf("[tta:render] swap frame=%u movie=%d pictures=%llu\n", read32(rdram, 0x3b5720),
                                g_movieDrawn ? 1 : 0, (unsigned long long)g_movieVersion);
                g_movieDrawn = false;
                r.publishOverlay(screen());
                r.endFrame();
                g_frameOpen = false;
                // sceGsSyncV's field (0: even): the vblanks so far; the frame boundary.
                const uint64_t field = bozo::port::vblankCount(rt) & 1u;
                const uint16_t v = field == 0 ? 1 : 0;
                std::memcpy(rdram + 0x3b5724, &v, 2);
                beginFrame();
            }
            ret(ctx);
            if (rdram[0x36f35c] == 0)
                bozo::port::frameEnd(rt, ctx);
        }

        // 0x216470: ends the frame's VIF1 chain (an END tag at G+4, G = 0x3b5be8), FlushCache, 0x3b5c08 = {Count, busy
        // 1}, then starts DMA channel 1 on it. Nothing is sent natively: the tag is written as the game reads its own
        // chain, and the transfer is done as soon as it starts (the completion handler 0x215e48 would clear busy).
        void kickChain(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t at = read32(rdram, 0x3b5be8 + 4);
            write32(rdram, at, 0x70000000u);
            write32(rdram, at + 4, 0);
            write32(rdram, at + 8, 0);
            write32(rdram, at + 12, 0);
            const uint32_t count = bozo_cop0_read(ctx, rt, 9);
            write32(rdram, 0x3b5c08, count);
            write32(rdram, 0x3b5c08 + 4, count);
            write32(rdram, 0x3b5c08 + 8, 0);
            write32(rdram, 0x3b5c08 + 0xc, 0);
            ret(ctx);
        }

        // 0x216530(dst, src, bytes): the game's quadword memcpy (128/64/32-bit paths by alignment), returns dst. Its one
        // source outside RAM is the frame end's copy of VU1 data memory 0x1100c840 (0x1c3718: the VU1 programs'
        // statistics, render map 1.3): there is no VU1, the statistics are zeros. The frame end then clears those four
        // VU1 quadwords (sq at 0x1c3740..0x1c375c) before its own words: called from there (the copy returning to
        // 0x1c3734), the copy goes on at the game's clear of its seven words (0x1c3770, v0 = the last, a0 = 6), past
        // the VU1 stores.
        void copyQwords(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const bool frameEndCopy = ctx->r[31].uw[0] == 0x1c3734u;
            const uint32_t dst = ctx->r[4].uw[0], src = ctx->r[5].uw[0], bytes = ctx->r[6].uw[0];
            uint8_t *out = (dst & 0xf0000000u) == 0x70000000u ? bozo::port::scratchpad(rt) + (dst & 0x3fffu)
                                                             : rdram + (dst & kMask);
            if ((src & 0xffff0000u) == 0x11000000u)
                std::memset(out, 0, bytes);
            else
            {
                const uint8_t *in = (src & 0xf0000000u) == 0x70000000u ? bozo::port::scratchpad(rt) + (src & 0x3fffu)
                                                                      : rdram + (src & kMask);
                std::memmove(out, in, bytes);
            }
            ctx->r[2].sd[0] = static_cast<int32_t>(dst);
            if (frameEndCopy)
            {
                ctx->r[2].sd[0] = static_cast<int32_t>(read32(rdram, 0x3779b4) + 0x58);
                ctx->r[4].sd[0] = 6;
                ctx->r[31].ud[0] = 0x1c3770u;
            }
            ret(ctx);
        }

    }

    namespace
    {
        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float v;
            std::memcpy(&v, rdram + (address & kMask), 4);
            return v;
        }

        // 0x228118(i): the lens flares' occlusion test (render map: the Flare component 0x232bd0 -> 0x1d3f28). Flare i's
        // record R = *(0x36b228) + i * 0x100 + 0x3c0 holds the light's place on screen (+0xc8 / +0xcc, 0..1 of the
        // viewport) and GS Z (+0xd0). 0x2281e8 copies the Z buffer's 8 x 5 pixels around it (x - 4 .. x + 3, y - 2 ..
        // y + 2, moved inside the screen; +0xa4 / +0xa8 / +0xac / +0xb0 the part that is the light's window) to a small
        // place in GS memory, and this function reads them back (sceGsSetDefStoreImage PSMZ32, sceGsExecStoreImage) into
        // R +0..+0x9f, rows of 8 words. Next frame 0x228040 sums the weights (0x397c78, by pixel) of the pixels whose
        // Z & 0xffffff < the light's Z + 16 (nothing in front of it): the flare's visibility. Natively the block is the
        // window's depth (BozoEngine's depthAt: GS Z of the frame presented last; TTA's GS Z is the matrices' z / w); a
        // pixel not read back yet (headless, the first frames) counts as clear, as before. Off the PC's clock (headless,
        // TTA_DETERMINISTIC) every pixel counts as clear: the engine reads depth back asynchronously, from whichever frame
        // the window presented last, so the flare's strength depended on timing and no two captures of the Scream were
        // alike. TTA_FLARE_NO_DEPTH=1 leaves the block alone (always visible).
        void readBack(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            ctx->pc = ctx->r[31].uw[0];
            static const bool off = env("TTA_FLARE_NO_DEPTH") != nullptr;
            if (off)
                return;
            const uint32_t i = ctx->r[4].uw[0], rec = read32(rdram, 0x36b228) + i * 0x100u + 0x3c0u;
            const uint32_t rc = read32(rdram, 0x36f350), vp = read32(rdram, read32(rdram, rc) + 0x14),
                           dims = read32(rdram, read32(rdram, rc) + 0x18);
            const float w = readFloat(rdram, vp + 8) * readFloat(rdram, dims + 8),
                        h = readFloat(rdram, vp + 12) * readFloat(rdram, dims + 12);
            int x0 = static_cast<int>(readFloat(rdram, rec + 0xc8) * w) - 4;
            int y0 = static_cast<int>(readFloat(rdram, rec + 0xcc) * h) - 2;
            if (x0 < 0)
                x0 = 0;
            if (y0 < 0)
                y0 = 0;
            if (static_cast<float>(x0 + 8) > w)
                x0 = static_cast<int>(w) - 8;
            if (static_cast<float>(y0 + 5) > h)
                y0 = static_cast<int>(h) - 5;
            x0 += static_cast<int>(readFloat(rdram, vp) * readFloat(rdram, dims + 8));
            y0 += static_cast<int>(readFloat(rdram, vp + 4) * readFloat(rdram, dims + 12));
            auto &renderer = bozo::render::Renderer::instance();
            const bool live = realTime();
            if (live)
                renderer.wantDepth();
            const bozo::render::Screen s = screen();
            uint32_t seen = 0, front = 0;
            for (int r = 0; r < 5; ++r)
                for (int c = 0; c < 8; ++c)
                {
                    uint32_t z = 0;
                    if (live && renderer.depthAt(wideX(s.originX + static_cast<float>(x0 + c) + 0.5f), // where the 3D is drawn
                                                 s.originY + static_cast<float>(y0 + r) + 0.5f, z))
                        ++seen;
                    else
                        z = 0;
                    z = std::min<uint32_t>(z, 0xffffffu);
                    write32(rdram, rec + static_cast<uint32_t>(r * 32 + c * 4), z);
                    front += z >= static_cast<uint32_t>(static_cast<int32_t>(readFloat(rdram, rec + 0xd0))) + 16u ? 1u : 0u;
                }
            if (trace())
                std::printf("[tta:render] flare %u: block at %d,%d, %u of 40 read, %u in front of the light (Z %.0f)\n", i, x0,
                            y0, seen, front, readFloat(rdram, rec + 0xd0));
        }
    }

    void installRender(bozo::port::Registry &registry)
    {
        registry.registerFunction(0x228118, readBack);
        // The port's frame is the whole picture: the window shows what is presented (its frame, the movie, the 2D).
        bozo::render::Renderer &renderer = bozo::render::Renderer::instance();
        renderer.setEnabled(true);
        renderer.setPresentsAlone(true);
        bozo::render::setMoviePictureHook(onMoviePicture);
        registry.registerFunction(0x212478, frameSwap);
        registry.registerFunction(0x216470, kickChain);
        registry.registerFunction(0x216530, copyQwords);
        registry.registerFunction(0x22af40, movieUpload);
    }
}
