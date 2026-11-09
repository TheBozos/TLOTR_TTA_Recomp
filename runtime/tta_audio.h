#pragma once

#include "bozo/libs/libraries.h"
#include "bozo/port.h"

namespace tta
{
    // EA's sound driver (AUDIOPF.IRX, with LIBSD.IRX) compiled offline from the player's disc and run by BozoEngine's
    // compiled-library host; the game's SIF commands and software registers connected to it (tta_audio.cpp).
    void installAudio(bozo::libs::Libraries &libraries, uint8_t *eeRam);
    void installAudioOverrides(bozo::port::Registry &registry);
}
