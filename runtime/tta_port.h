#pragma once

#include "bozo/port.h"

#include <cstdint>
#include <string>

namespace tta
{
    // The Third Age's overrides on BozoEngine (tta_port.cpp).
    void install(Runtime &runtime, bozo::port::Registry &registry);

    // The loaded resource an address lies in (tta_2d.cpp tracks the loads at 0x1babe8): its type 4cc ("ter ", "apxf"),
    // index, and the address's offset in its payload. False outside every resource.
    bool resourceAt(uint32_t address, std::string &type, uint32_t &index, uint32_t &offset);
}
