#pragma once

#include "bozo/port.h"
#include "bozo/render/texture.h"

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
}
