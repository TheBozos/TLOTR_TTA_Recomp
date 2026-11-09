// The Third Age's sound: EA's driver (AUDIOPF.IRX) and Sony's LIBSD.IRX, statically compiled from the player's disc
// (tools/tta_bozo_audio.py) and run by BozoEngine's compiled-library host, LIBSD's exports answered by the engine's
// native libsd and mixer. The game and the driver talk through SIF commands and software registers only:
//  - set-sreg (system command 0x80000001): each side writes the other's register with the address of its buffers
//    (the game registers 0x1e and 0x1f, then polls its own), and waits for the other to do the same;
//  - user commands: the game's go to the driver's handlers (submitCommand), the driver's to the handlers the game
//    registered in libsif's table (sceSifSetCmdBuffer / sceSifAddCmdHandler), run one at a time as SIF interrupts.
#include "tta_audio.h"

#include "bozo/ee/kernel.h"
#include "bozo/native/compiled_libraries.h"
#include "tta_audio_modules.h"

#include <cctype>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace tta
{
    namespace
    {
        // libsif's command data in SLUS_210.27 (sceSifInitCmd 0x32a278): the packet buffer the SIF interrupt hands to
        // a handler, and the user handler table's address and size (sceSifSetCmdBuffer 0x32a530 writes them).
        constexpr uint32_t kSifCommandData = 0x41b018;
        constexpr uint32_t kPacketBuffer = 0x41af40;
        constexpr uint32_t kSetSreg = 0x80000001u;

        std::unique_ptr<bozo::native::irx::CompiledLibraries> sound;
        bozo::libs::Libraries *libs = nullptr;
        uint8_t *ram = nullptr; // EE RAM, for the first packet of a delivery (later ones go through the handler's end)
        uint32_t gameGp = 0;

        struct Packet
        {
            uint32_t command;
            std::vector<uint8_t> data;
        };
        std::deque<Packet> pending; // the driver's commands to the game, delivered one at a time
        bool delivering = false;

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & 0x1ffffffu), 4);
            return v;
        }

        // Hands the game one of the driver's commands: the packet into libsif's buffer, then the handler the game
        // registered for it as a SIF interrupt; the next one waits for its end.
        void deliver(bozo::libs::Libraries &l, Packet packet, const std::function<void(const std::vector<uint8_t> &)> &writePacket,
                     const std::function<uint32_t(uint32_t)> &read)
        {
            const uint32_t table = read(kSifCommandData + 0x14), count = read(kSifCommandData + 0x18);
            if (!table || packet.command >= count)
                throw std::runtime_error("AUDIOPF sent a user command the game has no handler table entry for");
            const uint32_t handler = read(table + packet.command * 8), argument = read(table + packet.command * 8 + 4);
            if (!handler)
                throw std::runtime_error("AUDIOPF sent a user command the game registered no handler for");
            writePacket(packet.data);
            bozo::ee::QueuedHandler h;
            h.kind = bozo::ee::kInvokeSifCommand;
            h.pc = handler;
            h.a[0] = kPacketBuffer;
            h.a[1] = argument;
            h.gp = gameGp;
            delivering = true;
            l.queueHandler(h, [](bozo::libs::Libraries &l, bozo::libs::Guest &g)
            {
                delivering = false;
                if (pending.empty())
                    return;
                Packet next = std::move(pending.front());
                pending.pop_front();
                deliver(l, std::move(next),
                        [&](const std::vector<uint8_t> &d) { g.write(kPacketBuffer, d.data(), static_cast<uint32_t>(d.size())); },
                        [&](uint32_t a) { return g.read32(a); });
            });
        }

        // sceSifSendCmd(command, packet, size, extra source, extra destination, extra size) and its interrupt form.
        void sendCommand(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            const uint32_t command = ctx->r[4].uw[0], packet = ctx->r[5].uw[0], size = ctx->r[6].uw[0];
            const uint32_t extraSource = ctx->r[7].uw[0], extraDestination = ctx->r[8].uw[0], extraSize = ctx->r[9].uw[0];
            gameGp = ctx->r[28].uw[0];
            if (size < 16 || size > 112)
                throw std::runtime_error("sceSifSendCmd with a packet size outside 16..112");
            uint32_t result = 0;
            if (command == kSetSreg)
            {
                libs->sif.iopSreg[read32(rdram, packet + 0x10) & 31u] = read32(rdram, packet + 0x14);
                result = 1;
            }
            else if (command & 0x80000000u)
                throw std::runtime_error("sceSifSendCmd: a system command the port does not answer");
            else
            {
                if (extraSize)
                    sound->writeBuffer(extraDestination, rdram + (extraSource & 0x1ffffffu), extraSize);
                std::vector<uint8_t> data(rdram + (packet & 0x1ffffffu), rdram + (packet & 0x1ffffffu) + size);
                result = sound->submitCommand(command, data);
            }
            ctx->r[2].uw[0] = result;
            ctx->r[2].uw[1] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }
    }

    void installAudio(bozo::libs::Libraries &libraries, uint8_t *eeRam)
    {
        libs = &libraries;
        ram = eeRam;
        sound = std::make_unique<bozo::native::irx::CompiledLibraries>(libraries, 0x80000);
        addTtaAudioImages(*sound);
        sound->useNativeDiscFiles(); // AUDIOPF streams its music and speech from the disc (cdvdman)
        // AUDIOPF starts a libsd transfer and polls the flag its completion callback sets (0x4b4b8).
        sound->useReadyAudioCompletionsAfterImports();
        // AUDIOPF's start clears its buffers with its own byte-wise memset, more than the default root call allows.
        sound->setRootCallBudget(50000000);
        libraries.audioProvider = [](uint64_t cycle) { return sound->advanceTo(cycle); };
        libraries.sif.transfer = [](uint32_t address, const uint8_t *bytes, uint32_t count)
        { sound->writeBuffer(address, bytes, count); };
        libraries.sif.allocateBuffer = [](uint32_t bytes) { return sound->allocateBuffer(bytes); };
        libraries.sif.freeBuffer = [](uint32_t address) { return sound->freeBuffer(address); };
        libraries.sif.resetProvider = []
        {
            sound->reset();
            pending.clear();
            delivering = false;
        };
        libraries.sif.receiveCommand = [](uint32_t command, std::span<const uint8_t> data, uint32_t,
                                          std::span<const uint8_t> extra) -> uint32_t
        {
            if (!extra.empty())
                throw std::runtime_error("AUDIOPF sent a user command with extra data (not wired)");
            Packet packet{command, {data.begin(), data.end()}};
            if (delivering)
                pending.push_back(std::move(packet));
            else
                deliver(*libs, std::move(packet),
                        [](const std::vector<uint8_t> &d) { std::memcpy(ram + (kPacketBuffer & 0x1ffffffu), d.data(), d.size()); },
                        [](uint32_t a) { return read32(ram, a); });
            static uint32_t jobs = 0;
            return ++jobs;
        };
        libraries.sif.moduleLoader = [](const std::string &path)
        {
            auto name = path.substr(path.find_last_of("/\\:") + 1);
            if (const auto version = name.find(';'); version != std::string::npos)
                name.resize(version);
            for (char &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            // Pad and memory card: answered at their EE APIs by BozoEngine's native libraries.
            if (name == "SIO2MAN.IRX" || name == "SIO2D.IRX" || name == "DS2U_D.IRX" || name == "DBCMAN.IRX" ||
                name == "MCMAN.IRX" || name == "MCSERV.IRX")
                return 1;
            return sound->startImage(name + ";1");
        };
    }

    void installAudioOverrides(bozo::port::Registry &registry)
    {
        registry.registerFunction(0x32a748, sendCommand); // sceSifSendCmd
        registry.registerFunction(0x32a788, sendCommand); // isceSifSendCmd
    }
}
