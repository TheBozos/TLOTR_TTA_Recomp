#pragma once

#include "bozo/port.h"
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
    inline const char *env(const char *name)
    {
        static std::unordered_map<const char *, const char *> cache;
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
}
