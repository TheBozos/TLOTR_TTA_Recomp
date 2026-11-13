// TTA_VU0_DUMP=<entry hex>: records the VU0 register state before and after the first 64 calls of one microprogram
// (the native translation from tools/tta_vu0_gen.py) to build/vu0_<entry>.bin, for checking it against the microcode
// offline (tools/tta_vu0_check.py). Each record: u32 entry, then the state before and after: vf[32] (16 bytes each),
// acc (16), vi[16] (u32), q, i, p, r, status, mac, clip, cmsar0 (u32 each).
#include "tta_port.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tta
{
    namespace
    {
        bozo::port::Function g_original = nullptr;
        uint32_t g_entry = 0, g_calls = 0;
        FILE *g_file = nullptr;

        void writeState(const R5900Context *ctx)
        {
            std::fwrite(ctx->vu0_vf, 16, 32, g_file);
            std::fwrite(&ctx->vu0_acc, 16, 1, g_file);
            std::fwrite(ctx->vi, 4, 16, g_file);
            const uint32_t words[8] = {ctx->vu0_q_bits, ctx->vu0_i_bits, ctx->vu0_p_bits, ctx->vu0_r,
                                       ctx->vu0_status, ctx->vu0_mac, ctx->vu0_clip_flags, ctx->vu0_cmsar0};
            std::fwrite(words, 4, 8, g_file);
        }

        void dumped(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const bool record = g_file && g_calls < 64;
            if (record)
            {
                std::fwrite(&g_entry, 4, 1, g_file);
                writeState(ctx);
            }
            g_original(rdram, ctx, rt);
            if (record)
            {
                writeState(ctx);
                if (++g_calls == 64)
                    std::fclose(g_file), g_file = nullptr;
            }
        }
    }

    void installVu0Check(bozo::port::Registry &registry)
    {
        const char *e = std::getenv("TTA_VU0_DUMP");
        if (!e || !*e)
            return;
        g_entry = static_cast<uint32_t>(std::strtoul(e, nullptr, 16));
        for (const auto &p : registry.vu0Programs())
            if (p.address == g_entry)
                g_original = p.function;
        if (!g_original)
            return;
        char path[96];
        std::snprintf(path, sizeof(path), "D:/TLOTR_TTA_Recomp/build/vu0_%x.bin", g_entry);
        g_file = std::fopen(path, "wb");
        registry.vu0Program(g_entry, dumped);
    }
}
