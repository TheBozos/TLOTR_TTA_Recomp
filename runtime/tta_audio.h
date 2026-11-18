#pragma once

#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include "bozo/state.h"

#include <string>

namespace tta
{
    // EA's sound driver (AUDIOPF.IRX, with LIBSD.IRX) compiled offline from the player's disc and run by BozoEngine's
    // compiled-library host; the game's SIF commands and software registers connected to it (tta_audio.cpp).
    void installAudio(bozo::libs::Libraries &libraries, uint8_t *eeRam);
    void installAudioOverrides(bozo::port::Registry &registry);
    // Save states: the driver's host (IOP memory, tasks, mixer, libsd) and the commands waiting for the game.
    void audioState(bozo::state::Archive &a);
    std::string audioUnsaveable(); // non-empty: why a save must wait
}
