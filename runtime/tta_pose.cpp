// The Third Age's skeleton pose builder made native (build/bozo/skin_spec.md section 8): 0x133298(E, pose, flip) with its
// inverse-bind step 0x133148. The game's version runs VU0 microprograms (0xc28 quaternion -> rows, 0xb00/0xa80 the
// products) and reads the inverse binds through a scratchpad DMA; this one reads the same inputs from EE memory and
// writes the same N bone matrices at *(E+0xe0). Matrices: four rows of four floats, row vectors (v' = v M), translation
// in row 3; A.B = A applied first.
#include "tta_port.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace tta
{
    namespace
    {
        constexpr uint32_t kMask = 0x1ffffffu;

        uint32_t read32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & kMask), 4);
            return v;
        }

        int16_t read16s(const uint8_t *rdram, uint32_t a)
        {
            int16_t v;
            std::memcpy(&v, rdram + (a & kMask), 2);
            return v;
        }

        float readFloat(const uint8_t *rdram, uint32_t a)
        {
            float v;
            std::memcpy(&v, rdram + (a & kMask), 4);
            return v;
        }

        struct M4
        {
            float m[4][4];
        };

        M4 identity()
        {
            M4 r{};
            for (int i = 0; i < 4; ++i)
                r.m[i][i] = 1.0f;
            return r;
        }

        // A.B: A applied first (row vectors): the ordinary product of the row-major matrices.
        M4 mul(const M4 &a, const M4 &b)
        {
            M4 r{};
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
            return r;
        }

        M4 load(const uint8_t *rdram, uint32_t a)
        {
            M4 r;
            std::memcpy(r.m, rdram + (a & kMask), 64);
            return r;
        }

        // The rows VU0's 0xc28 builds from a quaternion (x, y, z, w; not normalised) and a translation.
        M4 local(const float q[4], const float t[3])
        {
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            M4 r{};
            r.m[0][0] = 1 - 2 * y * y - 2 * z * z;
            r.m[0][1] = 2 * x * y + 2 * w * z;
            r.m[0][2] = 2 * x * z - 2 * w * y;
            r.m[1][0] = 2 * x * y - 2 * w * z;
            r.m[1][1] = 1 - 2 * x * x - 2 * z * z;
            r.m[1][2] = 2 * y * z + 2 * w * x;
            r.m[2][0] = 2 * x * z + 2 * w * y;
            r.m[2][1] = 2 * y * z - 2 * w * x;
            r.m[2][2] = 1 - 2 * x * x - 2 * y * y;
            r.m[3][0] = t[0];
            r.m[3][1] = t[1];
            r.m[3][2] = t[2];
            r.m[3][3] = 1;
            return r;
        }

        uint32_t g_badQuaternions = 0, g_poses = 0;

        void buildPose(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            const uint32_t e = ctx->r[4].uw[0], pose = ctx->r[5].uw[0], flip = ctx->r[6].uw[0];
            ctx->pc = ra;
            if (!e || !read32(rdram, e + 0xe4))
                return;
            const uint32_t skeletons = read32(rdram, e + 4);
            if (!skeletons)
                return;
            const uint32_t table = read32(rdram, skeletons + 0x1c);
            const int16_t which = read16s(rdram, e + 8);
            if (!table || which < 0)
                return;
            const uint32_t rec = table + static_cast<uint32_t>(which) * 0x18;
            const int32_t n = static_cast<int32_t>(read32(rdram, rec));
            if (n <= 0 || n > 512)
                return;
            const uint32_t parents = read32(rdram, rec + 4), inverseBinds = read32(rdram, rec + 8);
            // The double buffer: a new frame id moves *(E+0xe0) to the other half.
            if (flip)
            {
                const int16_t id = read16s(rdram, 0x35fc94);
                if (read16s(rdram, e + 0xe8) != id)
                {
                    std::memcpy(rdram + ((e + 0xe8) & kMask), &id, 2);
                    const uint32_t base = read32(rdram, e + 0xe4), current = read32(rdram, e + 0xe0);
                    const uint32_t next = current == base ? base + static_cast<uint32_t>(n) * 64 : base;
                    std::memcpy(rdram + ((e + 0xe0) & kMask), &next, 4);
                }
            }
            const uint32_t out = read32(rdram, e + 0xe0);
            if (!out)
                return;
            // Bone controllers: (s16)E+0x1c4 entries of 12 bytes at E+0x164: {override*, bone, flags}.
            const int16_t controllers = read16s(rdram, e + 0x1c4);
            const auto controller = [&](int32_t bone, uint32_t &ovr, uint32_t &flags) -> bool
            {
                for (int16_t c = 0; c < controllers && c < 32; ++c)
                {
                    const uint32_t entry = e + 0x164 + static_cast<uint32_t>(c) * 12;
                    if (static_cast<int32_t>(read32(rdram, entry + 4)) == bone)
                    {
                        ovr = read32(rdram, entry);
                        flags = read32(rdram, entry + 8);
                        return ovr != 0;
                    }
                }
                return false;
            };
            ++g_poses;
            std::vector<M4> b(static_cast<size_t>(n));
            const float s = readFloat(rdram, e + 0x474);
            b[0] = identity();
            for (int i = 0; i < 3; ++i)
                b[0].m[i][i] = s;
            for (int32_t i = 1; i < n; ++i)
            {
                const uint32_t entry = pose + static_cast<uint32_t>(i) * 0x20;
                float q[4], t[3];
                for (int k = 0; k < 4; ++k)
                    q[k] = readFloat(rdram, entry + k * 4);
                for (int k = 0; k < 3; ++k)
                    t[k] = readFloat(rdram, entry + 0x10 + k * 4);
                const float len = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
                if (!std::getenv("TTA_POSE_KEEP_W") && std::fabs(len - 1.0f) > 0.05f)
                {
                    // Interim: the sampled/blended pose comes with x, y, z right and w wrong (|q|^2 up to 60): w rebuilt
                    // from xyz, its sign kept. The blend's VU0 microprogram 0xdb0 (tools/tta_vu0_gen.py) is the suspect.
                    const float xyz = q[0] * q[0] + q[1] * q[1] + q[2] * q[2];
                    const float w = xyz < 1.0f ? std::sqrt(1.0f - xyz) : 0.0f;
                    q[3] = q[3] < 0 ? -w : w;
                }
                if (std::fabs(len - 1.0f) > 0.05f)
                {
                    ++g_badQuaternions;
                    static int shown = 0;
                    if (std::getenv("TTA_POSE_TRACE") && shown++ < 12)
                        std::printf("[tta:pose] E %x N %d bone %d q (%.4g %.4g %.4g %.4g) |q|^2 %.4g t (%.4g %.4g %.4g) pose %x\n", e, n, i,
                                    q[0], q[1], q[2], q[3], len, t[0], t[1], t[2], pose);
                }
                int32_t p = static_cast<int32_t>(read32(rdram, parents + static_cast<uint32_t>(i) * 8));
                if (p < 0 || p >= i)
                    p = 0;
                uint32_t ovr = 0, flags = 0;
                if (controller(i, ovr, flags))
                {
                    float oq[4], ot[3];
                    for (int k = 0; k < 4; ++k)
                        oq[k] = readFloat(rdram, ovr + k * 4);
                    for (int k = 0; k < 3; ++k)
                        ot[k] = readFloat(rdram, ovr + 0x10 + k * 4);
                    if (flags & 1)
                    {
                        b[i] = mul(local(q, t), b[p]);
                        if (flags & 4)
                        {
                            const float zero[3] = {0, 0, 0};
                            b[i] = mul(local(oq, (flags & 8) ? ot : zero), b[i]);
                        }
                        else if (flags & 8)
                        {
                            M4 row = identity();
                            for (int k = 0; k < 3; ++k)
                                row.m[3][k] = ot[k];
                            const M4 moved = mul(row, b[i]);
                            for (int k = 0; k < 4; ++k)
                                b[i].m[3][k] = moved.m[3][k];
                        }
                    }
                    else
                    {
                        M4 parent = b[p];
                        if (flags & 4)
                        {
                            const float keep[4] = {parent.m[3][0], parent.m[3][1], parent.m[3][2], parent.m[3][3]};
                            parent = identity();
                            for (int k = 0; k < 4; ++k)
                                parent.m[3][k] = keep[k];
                        }
                        if (flags & 8)
                        {
                            parent.m[3][0] = parent.m[3][1] = parent.m[3][2] = 0;
                            parent.m[3][3] = 1;
                        }
                        b[i] = mul(local((flags & 4) ? oq : q, (flags & 8) ? ot : t), parent);
                    }
                }
                else
                    b[i] = mul(local(q, t), b[p]);
            }
            for (int32_t i = 0; i < n; ++i)
            {
                const M4 m = inverseBinds ? mul(load(rdram, inverseBinds + static_cast<uint32_t>(i) * 64), b[i]) : b[i];
                std::memcpy(rdram + ((out + static_cast<uint32_t>(i) * 64) & kMask), m.m, 64);
            }
            if (std::getenv("TTA_POSE_TRACE") && g_poses % 2000 == 1)
                std::printf("[tta:pose] %u poses built natively, %u quaternions off unit length\n", g_poses, g_badQuaternions);
        }
    }

    void installPose(bozo::port::Registry &registry)
    {
        if (!std::getenv("TTA_GAME_POSE")) // TTA_GAME_POSE=1: the game's own builder (VU0, scratchpad), for comparison
            registry.registerFunction(0x133298, buildPose);
    }
}
