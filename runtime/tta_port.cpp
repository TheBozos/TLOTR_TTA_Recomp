// The Third Age's port: guest functions answered natively by address. Empty for the first headless boots, which show
// what the game still asks of the PS2 (unknown register accesses, unnamed library calls).
#include "tta_port.h"

namespace tta
{
    void install(Runtime &, bozo::port::Registry &)
    {
    }
}
