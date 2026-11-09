// The Third Age's port: guest functions answered natively by address.
#include "tta_port.h"

#include "tta_audio.h"

namespace tta
{
    void install(Runtime &, bozo::port::Registry &registry)
    {
        installAudioOverrides(registry);
    }
}
