#pragma once

#include "bozo/port.h"
#include "bozo/render/renderer.h"
#include "bozo/render/texture.h"
#include "bozo/state.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>

#include <cstdint>
#include <string>

namespace tta
{
    // An environment switch, read once (the draw paths ask per vertex; getenv takes a lock on Windows). Keyed by the
    // name's address: the callers pass string literals, so no string is built per call (that showed in a profile).
    // One cache a thread: the draw workers (tta_3d.cpp's DrawQueue) ask too, and a shared map inserted into from two
    // threads at once is a data race. The environment is not changed while the game runs, so every copy agrees.
    inline const char *env(const char *name)
    {
        thread_local std::unordered_map<const char *, const char *> cache;
        auto [at, added] = cache.try_emplace(name, nullptr);
        if (added)
            at->second = std::getenv(name);
        return at->second;
    }

    // The Third Age's overrides on BozoEngine (tta_port.cpp).
    void install(Runtime &runtime, bozo::port::Registry &registry);

    // The loaded resource an address lies in (tta_2d.cpp tracks the loads at 0x1babe8): its type 4cc ("ter ", "apxf"),
    // index, and the address's offset in its payload. False outside every resource.
    bool resourceAt(uint32_t address, std::string &type, uint32_t &index, uint32_t &offset);
    // The same with the resource's payload size (the unpacked size its header gives).
    bool resourceAt(uint32_t address, std::string &type, uint32_t &index, uint32_t &offset, uint32_t &size);

    // A converted texture by its 64-bit id (tta_2d.cpp: the offline pack, tools/tta_textures.py); null if none.
    std::shared_ptr<const bozo::render::Texture> textureById(uint64_t id);
    // The converted texture the game uploaded at (TBP0, PSM) (tta_2d.cpp tracks the uploads at 0x219ca8); null if none.
    std::shared_ptr<const bozo::render::Texture> textureAtPlace(uint32_t tbp0, uint32_t psm);
    // The frame buffer the scene is being drawn into (FBP 0 or 0x70), as the 3D draws see it: tta_2d.cpp's screen
    // filters tell the frame being built from the other half of the double buffer by it.
    void noteSceneFbp(uint32_t fbp);
    // A one-frame report (F10 in the window build): the next game frame logs every character draw's mode, fade, sections
    // and lights, with the 0x20000 shine layer's inputs, as [tta:report] lines (tta_3d.cpp).
    void requestFrameReport();
    // The game on the host's clock (the window build playing): the frame's time step held to the host's (tta_port.cpp).
    void setRealTime(bool on);
    bool realTime(); // the window build on the PC's clock (not headless, not TTA_DETERMINISTIC)
    // Widescreen (--widescreen / TTA_WIDESCREEN=1, the window build): the picture is 16:9 and the GS frame's 512 columns
    // are spread over it, so what keeps its shape is squeezed by 3/4 around the screen's centre (x = 2048): the 3D draws
    // (their matrices: the game's 4:3 view, a third wider on screen; the game does not cull at its 4:3 edges), the 2D
    // overlay (the HUD and menus, pillarboxed in the middle) and the movies. Full-screen passes (the screen filters,
    // the frame's clear) keep the whole width. Set before the game runs (tta_main.cpp).
    void setWidescreen(bool on);
    bool widescreen();
    // The opening logos (EA, THX, New Line: one movie, unskippable on the console) ended by START, as the developers'
    // option skipallmovies does (tta_port.cpp, 0x20d528). Read by the game thread at each movie's start.
    void setSkippableLogos(bool on);
    bool skippableLogos();
    constexpr float kWideSqueeze = 0.75f;
    inline float wideX(float x) { return widescreen() ? 2048.0f + (x - 2048.0f) * kWideSqueeze : x; }
    // The 3D draws queued so far (their vertex work run on the draw workers) given to the renderer in order (tta_3d.cpp).
    // Called before any other use of the renderer: the 2D, the screen filters, the frame's begin and end.
    void flushDraws();
    // Debug (tta_debug.cpp): the developers' command-line options given to the game ("encounters_only"), as if from its
    // argv; a warp to a map (an index, a path such as e02c05 or a region such as "Helm's Deep") and a battle by the
    // current map's encounter id, each optionally "@<game frame>". Before or after install.
    void addGameOption(const std::string &spec);
    bool requestWarp(const std::string &spec);
    void requestEncounter(const std::string &spec);
    // A battle against any of the current map's enemy groups (an "_ET" record id), "@<game frame>" optional.
    void requestGroup(const std::string &spec);
    void installDebug(bozo::port::Registry &registry);
    // Save states: the resources the game loaded and where it placed its textures (tta_2d.cpp).
    void twoDState(bozo::state::Archive &a);
    // True during the reported frame (the 2D passes add their [tta:report] lines then).
    bool frameReportActive(const uint8_t *rdram);

    // The highlight mask (the coloured glow around a highlighted or buffed character in some battles). On the console
    // the scene leaves the frame's alpha alone; 0x1c1d34 clears it, each character's highlight pass (skin mode 0x80000,
    // FBMSK 0x00ffffff) writes its silhouette into it alone, and the screen filter 0x1cf1fc / 0x1cfce8 reads it back (the
    // frame as CT32 with TCC). BozoEngine keeps the frame's alpha as coverage, so the mask is an image of its own,
    // auxiliary image kHighlightMask: the silhouettes drawn into its alpha against the scene's depth, the filter's two
    // reads taking their alpha from it. TTA_NO_HIGHLIGHT_MASK=1 for the old drawing (no glow).
    constexpr uint8_t kHighlightMask = 15;
    inline bool highlightMaskWanted() { return !env("TTA_NO_HIGHLIGHT_MASK"); }
    inline bozo::render::Material highlightMaskMaterial(bozo::render::Material m)
    {
        m.target = kHighlightMask;
        m.frameDepth = true;
        m.frameWidth = 512u;
        m.frameHeight = 448u;
        m.blend = bozo::render::Material::Blend::AlphaOnly;
        m.gsBlend = false;
        m.colourMask = 0xfu;
        m.textured = false;
        m.depthWrite = false;
        return m;
    }
}
