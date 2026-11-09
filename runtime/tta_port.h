#pragma once

#include "bozo/port.h"

namespace tta
{
    // The Third Age's overrides on BozoEngine (tta_port.cpp).
    void install(Runtime &runtime, bozo::port::Registry &registry);
}
