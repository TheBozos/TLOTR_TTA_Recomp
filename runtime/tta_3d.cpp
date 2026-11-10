// The Third Age's 3D on BozoEngine's renderer (render map 3.3): LLObj nodes (terrain, trees, static objects) drawn
// from meshes converted offline from the player's disc (tools/tta_meshes.py), keyed by the node's record in its
// resource, with the game's matrices. No VIF/GIF packet is read while the game runs.
//
// LLObj_Render 0x1bdbe8(node) runs each node's shader render (node+0x58 set: (*(node+0x54))[+8](node+0x28)) then its
// children; the port draws the node's converted batches there. Node: +0 its record in the resource (record +0x88 the
// shader type). Matrices: RC = **(0x36f350), RC+0x2e0 world (or object) -> GS screen; a type-21 node (instanced
// terrain, trees) carries its own instance matrix in its geometry (in the converted mesh).
#include "tta_port.h"

#include "tta_functions.h"

#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <set>
#include <vector>

namespace tta
{
    namespace
    {
        constexpr uint32_t kMask = 0x1ffffffu;

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & kMask), 4);
            return v;
        }

        uint64_t read64(const uint8_t *rdram, uint32_t address)
        {
            uint64_t v;
            std::memcpy(&v, rdram + (address & kMask), 8);
            return v;
        }

        uint16_t read16(const uint8_t *rdram, uint32_t address)
        {
            uint16_t v;
            std::memcpy(&v, rdram + (address & kMask), 2);
            return v;
        }

        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float v;
            std::memcpy(&v, rdram + (address & kMask), 4);
            return v;
        }

        // TTA_3D_TRACE=1: per second, the nodes drawn and missing by kind.
        int traceLevel()
        {
            static const int level = []
            {
                const char *e = env("TTA_3D_TRACE");
                return e && *e ? std::atoi(e) : 0;
            }();
            return level;
        }

        uint32_t frame(const uint8_t *rdram) { return read32(rdram, 0x3b5720); }

        std::string meshDir()
        {
            const char *e = env("TTA_MESH_DIR");
            return e && *e ? e : TTA_MESH_DIR;
        }

        // ---- The converted meshes (tools/tta_meshes.py, format in its docstring) -----------------------------------
        struct Batch
        {
            uint64_t texture = 0;
            uint16_t prim = 0;
            uint8_t colourMode = 0; // 0 RGBA, 1 index into the node's colour table
            std::vector<std::pair<uint8_t, uint64_t>> registers;
            std::vector<bozo::render::Vertex> vertices;
            std::vector<uint8_t> adc;
            std::vector<uint32_t> colours; // as stored (RGBA or an index)
            // Skinned (kind 3): the batch's section (its bit in the visibility mask G+0x280), per vertex the normal and
            // four skeleton bones with their weights; positions in the model space the bones blend.
            uint32_t section = 0;
            std::vector<std::array<float, 3>> normals;
            std::vector<std::array<uint8_t, 4>> bones;
            std::vector<std::array<float, 4>> weights;
        };

        struct Node
        {
            int8_t shader = 0;
            uint8_t kind = 0; // 0 own batches, 1 a type-21 instance of `master`'s, 2 not converted, 3 skinned
            uint16_t number = 0;
            uint32_t master = 0xffffffffu;
            bool hasMatrix = false;
            float matrix[16] = {};
            std::vector<uint32_t> colours;
            std::vector<Batch> batches;
        };

        struct Mesh
        {
            uint32_t payloadSize = 0;
            std::unordered_map<uint32_t, Node> byOffset; // record offset -> node
            std::unordered_map<uint32_t, uint32_t> offsetByNumber;
        };

        template <typename T> bool get(FILE *f, T &v) { return std::fread(&v, sizeof(T), 1, f) == 1; }

        std::shared_ptr<Mesh> loadMesh(const std::string &path)
        {
            FILE *f = std::fopen(path.c_str(), "rb");
            if (!f)
                return nullptr;
            auto mesh = std::make_shared<Mesh>();
            char magic[4], type[4];
            uint32_t version = 0, nodes = 0, index = 0;
            bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "TTAM", 4) == 0 && get(f, version) &&
                      get(f, nodes) && std::fread(type, 1, 4, f) == 4 && get(f, index) && get(f, mesh->payloadSize);
            for (uint32_t n = 0; ok && n < nodes; ++n)
            {
                uint32_t offset = 0, batchCount = 0, vertexCount = 0;
                Node node;
                ok = get(f, offset) && get(f, node.shader) && get(f, node.kind) && get(f, node.number) &&
                     get(f, batchCount) && get(f, vertexCount) && get(f, node.master);
                if (ok && node.shader == 21)
                {
                    uint16_t instance = 0, group = 0;
                    uint32_t word = 0;
                    node.hasMatrix = true;
                    ok = std::fread(node.matrix, 4, 16, f) == 16 && get(f, instance) && get(f, group) && get(f, word);
                    node.colours.resize(256);
                    ok = ok && std::fread(node.colours.data(), 4, 256, f) == 256;
                }
                if (ok && node.kind == 3)
                {
                    // Skinned: sections of batches, 64-byte vertices (f32 xyz, f32 normal, f32 st, u32 rgba, u8 bone[4],
                    // f32 weight[4], u8 adc, 7 pad).
                    uint32_t sections = 0;
                    ok = get(f, sections);
                    for (uint32_t sct = 0; ok && sct < sections; ++sct)
                    {
                        uint32_t sectionIndex = 0, count = 0;
                        ok = get(f, sectionIndex) && get(f, count);
                        for (uint32_t b = 0; ok && b < count; ++b)
                        {
                            Batch batch;
                            batch.section = sectionIndex;
                            uint8_t registerCount = 0;
                            uint32_t vertices = 0;
                            ok = get(f, batch.texture) && get(f, batch.prim) && get(f, batch.colourMode) &&
                                 get(f, registerCount) && get(f, vertices);
                            for (uint32_t r = 0; ok && r < registerCount; ++r)
                            {
                                uint8_t reg = 0, pad[7];
                                uint64_t value = 0;
                                ok = get(f, reg) && std::fread(pad, 1, 7, f) == 7 && get(f, value);
                                batch.registers.emplace_back(reg, value);
                            }
                            batch.vertices.resize(vertices);
                            batch.adc.resize(vertices);
                            batch.colours.resize(vertices);
                            batch.normals.resize(vertices);
                            batch.bones.resize(vertices);
                            batch.weights.resize(vertices);
                            for (uint32_t v = 0; ok && v < vertices; ++v)
                            {
                                float p[8];
                                uint8_t pad[7];
                                ok = std::fread(p, 4, 8, f) == 8 && get(f, batch.colours[v]) &&
                                     std::fread(batch.bones[v].data(), 1, 4, f) == 4 &&
                                     std::fread(batch.weights[v].data(), 4, 4, f) == 4 && get(f, batch.adc[v]) &&
                                     std::fread(pad, 1, 7, f) == 7;
                                bozo::render::Vertex &out = batch.vertices[v];
                                out.x = p[0];
                                out.y = p[1];
                                out.z = p[2];
                                batch.normals[v] = {p[3], p[4], p[5]};
                                out.u = p[6];
                                out.v = p[7];
                            }
                            node.batches.push_back(std::move(batch));
                        }
                    }
                    batchCount = 0; // read above
                }
                for (uint32_t b = 0; ok && b < batchCount; ++b)
                {
                    Batch batch;
                    uint8_t registerCount = 0;
                    uint32_t count = 0;
                    ok = get(f, batch.texture) && get(f, batch.prim) && get(f, batch.colourMode) && get(f, registerCount) &&
                         get(f, count);
                    for (uint32_t r = 0; ok && r < registerCount; ++r)
                    {
                        uint8_t reg = 0, pad[7];
                        uint64_t value = 0;
                        ok = get(f, reg) && std::fread(pad, 1, 7, f) == 7 && get(f, value);
                        batch.registers.emplace_back(reg, value);
                    }
                    batch.vertices.resize(count);
                    batch.adc.resize(count);
                    batch.colours.resize(count);
                    for (uint32_t v = 0; ok && v < count; ++v)
                    {
                        float xyzst[5];
                        uint32_t colour = 0;
                        uint8_t adc = 0, pad[3];
                        ok = std::fread(xyzst, 4, 5, f) == 5 && get(f, colour) && get(f, adc) && std::fread(pad, 1, 3, f) == 3;
                        bozo::render::Vertex &out = batch.vertices[v];
                        out.x = xyzst[0];
                        out.y = xyzst[1];
                        out.z = xyzst[2];
                        out.u = xyzst[3];
                        out.v = xyzst[4];
                        batch.colours[v] = colour;
                        batch.adc[v] = adc;
                    }
                    node.batches.push_back(std::move(batch));
                }
                if (ok)
                {
                    mesh->offsetByNumber[node.number] = offset;
                    mesh->byOffset.emplace(offset, std::move(node));
                }
            }
            std::fclose(f);
            if (!ok)
                std::fprintf(stderr, "[tta:3d] %s: truncated or malformed\n", path.c_str());
            return ok ? mesh : nullptr;
        }

        // The mesh of a loaded resource: <type>_<index>[_vN].mesh, the variant whose payload size is the resource's.
        std::shared_ptr<Mesh> meshFor(const std::string &type, uint32_t index, uint32_t size)
        {
            static std::map<std::tuple<std::string, uint32_t, uint32_t>, std::shared_ptr<Mesh>> cache;
            const auto key = std::make_tuple(type, index, size);
            if (auto it = cache.find(key); it != cache.end())
                return it->second;
            std::string name = type;
            while (!name.empty() && name.back() == ' ')
                name.pop_back();
            std::shared_ptr<Mesh> found;
            for (int v = 0; v < 64 && !found; ++v)
            {
                const std::string path = meshDir() + "/" + name + "_" + std::to_string(index) +
                                         (v ? "_v" + std::to_string(v) : std::string()) + ".mesh";
                std::shared_ptr<Mesh> m = loadMesh(path);
                if (!m)
                {
                    if (v > 0)
                        break;
                    continue;
                }
                if (m->payloadSize == size)
                    found = m;
            }
            cache[key] = found;
            return found;
        }

        // ---- Drawing -----------------------------------------------------------------------------------------------
        // a * b (column-major 4x4: column c is m[4c..4c+3], p' = M p).
        void multiply(const float a[16], const float b[16], float out[16])
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
        }

        uint32_t g_frameSeen = 0, g_drawn = 0, g_missing = 0, g_unconverted = 0, g_offscreen = 0;

        void count(const uint8_t *rdram, uint32_t &which)
        {
            if (traceLevel() >= 1 && frame(rdram) != g_frameSeen)
            {
                if (g_frameSeen % 60 == 0 && (g_drawn || g_missing || g_unconverted))
                    std::printf("[tta:3d] frame %u: %u nodes drawn, %u without a mesh, %u not converted or skinned, %u off-screen\n",
                                g_frameSeen, g_drawn, g_missing, g_unconverted, g_offscreen);
                g_frameSeen = frame(rdram);
                g_drawn = g_missing = g_unconverted = g_offscreen = 0;
            }
            ++which;
        }

        // The VU programs only send triangles that land inside the GS's guard band and Z range (ADC otherwise; the
        // nodes that reach the camera go through the clipping programs). The renderer divides z by w per vertex and
        // wraps GS Z past its range as the GS does, so a triangle reaching behind the camera or past the near Z gets a
        // wrong depth and disappears. Cut here instead, in clip space where the planes are linear: w > 0, the GS Z
        // (z / w) >= 0 and <= the 24-bit range. TTA_NO_NEAR_CLIP turns it off.
        void clipToDepthRange(std::vector<bozo::render::Vertex> &triangles, const float m[16])
        {
            if (env("TTA_NO_NEAR_CLIP"))
                return;
            constexpr float kDepthMax = 16777215.0f, kWMin = 1.0e-3f;
            const auto distance = [m](const bozo::render::Vertex &v, int plane)
            {
                const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                if (plane == 0)
                    return w - kWMin;
                const float z = m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14];
                return plane == 1 ? z : kDepthMax * w - z;
            };
            bool change = false;
            for (size_t i = 0; i < triangles.size() && !change; ++i)
                for (int p = 0; p < 3 && !change; ++p)
                    change = !(distance(triangles[i], p) >= 0.0f);
            if (!change)
                return;
            const auto lerp = [](const bozo::render::Vertex &a, const bozo::render::Vertex &b, float t)
            {
                bozo::render::Vertex o = a;
                o.x = a.x + t * (b.x - a.x), o.y = a.y + t * (b.y - a.y), o.z = a.z + t * (b.z - a.z);
                o.u = a.u + t * (b.u - a.u), o.v = a.v + t * (b.v - a.v), o.fog = a.fog + t * (b.fog - a.fog);
                o.q = a.q + t * (b.q - a.q);
                uint32_t rgba = 0;
                for (int ch = 0; ch < 32; ch += 8)
                {
                    const float ca = static_cast<float>((a.rgba >> ch) & 0xffu), cb = static_cast<float>((b.rgba >> ch) & 0xffu);
                    rgba |= static_cast<uint32_t>(std::lround(ca + t * (cb - ca))) << ch;
                }
                o.rgba = rgba;
                return o;
            };
            std::vector<bozo::render::Vertex> out;
            out.reserve(triangles.size());
            for (size_t t = 0; t + 2 < triangles.size(); t += 3)
            {
                bozo::render::Vertex poly[12], next[12];
                int n = 3;
                for (int k = 0; k < 3; ++k)
                    poly[k] = triangles[t + static_cast<size_t>(k)];
                for (int p = 0; p < 3 && n >= 3; ++p)
                {
                    float d[12];
                    bool allIn = true;
                    for (int k = 0; k < n; ++k)
                        allIn = (d[k] = distance(poly[k], p)) >= 0.0f && allIn;
                    if (allIn)
                        continue;
                    int n2 = 0;
                    for (int k = 0; k < n && n2 < 11; ++k)
                    {
                        const int k1 = (k + 1) % n;
                        if (d[k] >= 0.0f)
                            next[n2++] = poly[k];
                        if ((d[k] >= 0.0f) != (d[k1] >= 0.0f))
                            next[n2++] = lerp(poly[k], poly[k1], d[k] / (d[k] - d[k1]));
                    }
                    for (int k = 0; k < n2; ++k)
                        poly[k] = next[k];
                    n = n2;
                }
                for (int f = 1; f + 1 < n; ++f)
                    out.insert(out.end(), {poly[0], poly[f], poly[f + 1]});
            }
            triangles.swap(out);
        }

        void drawNode(const uint8_t *rdram, const Mesh &mesh, const Node &node, const float screen[16])
        {
            const Node *source = &node;
            if (node.kind == 1)
            {
                auto at = mesh.offsetByNumber.find(node.master);
                if (at == mesh.offsetByNumber.end())
                    return;
                source = &mesh.byOffset.at(at->second);
            }
            float matrix[16];
            if (node.hasMatrix)
                multiply(screen, node.matrix, matrix);
            else
                std::memcpy(matrix, screen, sizeof(matrix));
            auto &renderer = bozo::render::Renderer::instance();
            // The scene's fog (fog_spec 1): K at DS+0x54c, FOGCOL at DS+0x720 (R, G, B bytes).
            const uint32_t ds = read32(rdram, 0x36f850);
            const float fogK = readFloat(rdram, ds + 0x54c);
            const uint32_t fogColour = read32(rdram, ds + 0x720) & 0xffffffu;
            if (traceLevel() >= 2)
            {
                static uint32_t lastFrame = 0, fogBatches = 0, batches = 0;
                if (frame(rdram) / 120 != lastFrame / 120)
                {
                    std::printf("[tta:fog] frame %u: K %g colour %06x, %u of %u batches fogged\n", frame(rdram), fogK, fogColour,
                                fogBatches, batches);
                    fogBatches = batches = 0;
                }
                lastFrame = frame(rdram);
                for (const Batch &batch : source->batches)
                {
                    ++batches;
                    fogBatches += (batch.prim & 0x20u) ? 1u : 0u;
                }
            }
            std::vector<bozo::render::Vertex> triangles;
            for (const Batch &batch : source->batches)
            {
                const uint32_t type = batch.prim & 7u;
                if (type < 3 || type > 5 || batch.vertices.size() < 3)
                    continue;
                std::vector<bozo::render::Vertex> v = batch.vertices;
                const bool fogged = (batch.prim & 0x20u) != 0;
                for (size_t i = 0; i < v.size(); ++i)
                {
                    const uint32_t c = batch.colours[i];
                    v[i].rgba = batch.colourMode == 1 ? (node.colours.empty() ? 0x80808080u : node.colours[c & 0xffu]) : c;
                    // The VU programs' fog (fog_spec 1): F = min(K / clip.w, 255), K = DS+0x54c; GS F 255 = no fog.
                    if (fogged)
                    {
                        const float w = matrix[3] * v[i].x + matrix[7] * v[i].y + matrix[11] * v[i].z + matrix[15];
                        v[i].fog = w > 0.0f ? std::fmin(fogK / w, 255.0f) / 255.0f : 1.0f;
                    }
                }
                triangles.clear();
                for (size_t i = 2; i < v.size(); ++i)
                {
                    if (batch.adc[i])
                        continue;
                    if (type == 3)
                    {
                        if (i % 3 == 2)
                            triangles.insert(triangles.end(), {v[i - 2], v[i - 1], v[i]});
                    }
                    else if (type == 4)
                        triangles.insert(triangles.end(), {v[i - 2], v[i - 1], v[i]});
                    else
                        triangles.insert(triangles.end(), {v[0], v[i - 1], v[i]});
                }
                if (triangles.empty())
                    continue;
                bozo::render::Material material;
                material.depthTest = true;
                // Z writes follow ZBUF_1's ZMSK (bit 32; DS+0x120, on for the scene, 0x1a8600), not PRIM.ABE: the
                // whole scene (sky, ground, trees) is drawn with ABE and opaque texels (alpha 0x80), and a cut-out texel
                // fails the alpha test, which writes nothing on the GS either.
                material.depthWrite = ((read64(rdram, ds + 0x120) >> 32) & 1u) == 0;
                material.blend = (batch.prim & 0x40u) ? bozo::render::Material::Blend::Alpha
                                                     : bozo::render::Material::Blend::Opaque;
                for (const auto &[reg, value] : batch.registers)
                {
                    if (reg == 0x42 || reg == 0x43) // ALPHA_1/2
                        material.blend = (batch.prim & 0x40u) ? bozo::render::blendOf(value) : material.blend;
                    else if (reg == 0x47 || reg == 0x48) // TEST_1/2
                    {
                        if (value & 1u)
                        {
                            material.alphaTest = true;
                            material.alphaFunc = static_cast<bozo::render::Material::AlphaFunc>((value >> 1) & 7u);
                            material.alphaRef = static_cast<uint8_t>((value >> 4) & 0xffu);
                        }
                    }
                    else if (reg == 0x08 || reg == 0x09) // CLAMP_1/2
                    {
                        material.clampU = (value & 3u) == 1u;
                        material.clampV = ((value >> 2) & 3u) == 1u;
                    }
                }
                std::shared_ptr<const bozo::render::Texture> texture;
                if ((batch.prim & 0x10u) && batch.texture)
                    texture = textureById(batch.texture);
                material.fog = fogged;
                material.fogColour = fogColour;
                material.textured = texture != nullptr;
                if (texture)
                {
                    material.tex0 = texture->key;
                    // Without a TEST of its own the batch keeps the shader's cut-out test (leaves, grass, branches: their
                    // texels are 0 or 0x80 in alpha).
                    if (!material.alphaTest && !env("TTA_NO_CUTOUT"))
                    {
                        material.alphaTest = true;
                        material.alphaFunc = bozo::render::Material::AlphaFunc::Greater;
                        material.alphaRef = 0; // the scene's TEST (0x1a8600): alpha > 0
                    }
                }
                clipToDepthRange(triangles, matrix);
                if (!triangles.empty())
                    renderer.draw(material, matrix, triangles.data(), static_cast<uint32_t>(triangles.size()), texture);
            }
        }

        // A node's converted batches, drawn with the game's current screen matrix.


        void drawFor(const uint8_t *rdram, uint32_t node)
        {
            // Only the passes into the displayed frame (FBP 0 / 0x70, the draw state's FRAME_1 at DS+0x150): an
            // off-screen pass has its own camera and target (not drawn yet).
            const uint32_t ds = read32(rdram, 0x36f850), fbp = read32(rdram, ds + 0x150) & 0x1ffu;
            if (fbp != 0 && fbp != 0x70)
            {
                count(rdram, g_offscreen);
                return;
            }
            noteSceneFbp(fbp);
            {
                const uint32_t record = read32(rdram, node);
                std::string type;
                uint32_t index = 0, offset = 0, size = 0;
                const std::shared_ptr<Mesh> mesh = resourceAt(record, type, index, offset, size)
                                                       ? meshFor(type, index, size)
                                                       : nullptr;
                const Mesh *m = mesh.get();
                const auto it = m ? m->byOffset.find(offset) : decltype(m->byOffset)::const_iterator{};
                if (!m || it == m->byOffset.end())
                    count(rdram, g_missing);
                else if (it->second.kind == 2 || it->second.kind == 3) // not converted, or skinned (drawSkin)
                {
                    count(rdram, g_unconverted);
                    if (traceLevel() >= 2 && it->second.kind == 2)
                    {
                        static std::set<std::string> seen;
                        char key[96];
                        std::snprintf(key, sizeof(key), "%s %u +%x shader %d", type.c_str(), index, offset, it->second.shader);
                        if (seen.insert(key).second)
                            std::printf("[tta:3d] not converted: %s (frame %u)\n", key, frame(rdram));
                    }
                }
                else
                {
                    // RC+0x2e0: object (RC+0x20) -> GS screen, the 2048 centre and Z range in it (= RC+0x1a0, world ->
                    // screen, while RC+0x20 is the identity: terrain). A type-21 node's instance matrix comes on top.
                    const uint32_t rc = read32(rdram, read32(rdram, 0x36f350));
                    float screen[16];
                    for (int i = 0; i < 16; ++i)
                        screen[i] = readFloat(rdram, rc + 0x2e0 + i * 4);
                    // TTA_3D_ONLY=<hex offset>[,...]: draw only these record offsets (debugging).
                    if (const char *only = env("TTA_3D_ONLY"))
                    {
                        char want[16];
                        std::snprintf(want, sizeof(want), "%x", offset);
                        bool listed = false;
                        for (const char *p = only; *p && !listed;)
                        {
                            const char *e = std::strchr(p, ',');
                            const size_t len = e ? static_cast<size_t>(e - p) : std::strlen(p);
                            listed = len == std::strlen(want) && std::strncmp(p, want, len) == 0;
                            p = e ? e + 1 : p + len;
                        }
                        if (!listed)
                            return;
                    }
                    drawNode(rdram, *m, it->second, screen);
                    count(rdram, g_drawn);
                    // TTA_3D_DUMP_FRAME=<n>: every node drawn in frame n, with its screen bounds (GS pixels, 2048 centre).
                    if (const char *d = env("TTA_3D_DUMP_FRAME"); d && frame(rdram) == static_cast<uint32_t>(std::atoi(d)))
                    {
                        const Node &n = it->second.kind == 1 ? m->byOffset.at(m->offsetByNumber.at(it->second.master)) : it->second;
                        float mat[16];
                        if (it->second.hasMatrix)
                            multiply(screen, it->second.matrix, mat);
                        else
                            std::memcpy(mat, screen, sizeof(mat));
                        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, w0 = 1e9f, w1 = -1e9f;
                        size_t verts = 0;
                        for (const Batch &b : n.batches)
                            for (const auto &v : b.vertices)
                            {
                                const float w = mat[3] * v.x + mat[7] * v.y + mat[11] * v.z + mat[15];
                                ++verts;
                                w0 = std::fmin(w0, w);
                                w1 = std::fmax(w1, w);
                                if (w <= 0.0f)
                                    continue;
                                const float x = (mat[0] * v.x + mat[4] * v.y + mat[8] * v.z + mat[12]) / w;
                                const float y = (mat[1] * v.x + mat[5] * v.y + mat[9] * v.z + mat[13]) / w;
                                x0 = std::fmin(x0, x); x1 = std::fmax(x1, x); y0 = std::fmin(y0, y); y1 = std::fmax(y1, y);
                            }
                        std::printf("[tta:dump] %s %u +%x kind %d shader %d: %zu batches %zu verts, x %.0f..%.0f y %.0f..%.0f w %.1f..%.1f\n",
                                    type.c_str(), index, offset, it->second.kind, it->second.shader, n.batches.size(), verts,
                                    x0, x1, y0, y1, w0, w1);
                    }
                }
            }
        }

        void dumpShaderTable(const uint8_t *rdram)
        {
            static bool done = false;
            if (done || traceLevel() < 2 || frame(rdram) < 3000)
                return;
            done = true;
            for (uint32_t i = 0; i < 22; ++i)
            {
                std::printf("[tta:3d] shader type %u:", i);
                for (uint32_t w = 0; w < 0x44; w += 4)
                    std::printf(" %x", read32(rdram, 0x35f690 + i * 0x44 + w));
                std::printf("\n");
            }
        }

        // 0x1bdbe8 LLObj_Render(node): the node's shader render, then its children (through this same override).
        void render(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], node = ctx->r[4].uw[0];
            dumpShaderTable(rdram);
            if (traceLevel() >= 4)
            {
                static std::map<std::pair<int, int>, uint32_t> seen; // (shader type, has shader) -> nodes
                static uint32_t last = 0;
                ++seen[{rdram[(read32(rdram, node) + 0x88) & kMask], rdram[(node + 0x58) & kMask] != 0}];
                if (frame(rdram) / 300 != last)
                {
                    last = frame(rdram) / 300;
                    for (const auto &[k, n] : seen)
                        std::printf("[tta:3d] LLObj nodes frame %u: type %d shader %d x%u\n", frame(rdram), k.first, k.second, n);
                    seen.clear();
                }
            }
            if (rdram[(node + 0x58) & kMask] != 0 && rdram[(read32(rdram, node) + 0x88) & kMask] != 21)
                drawFor(rdram, node);
            func_0x1bdbe8(rdram, ctx, rt);
            ctx->pc = ra;
        }

        // 0x2221b8: the water's (type 19) per-node render, instance = node + 0x28: it only marks the node visible this
        // frame (inst+0x1e) for the water driver 0x2246e8. Natively the node's converted surface is drawn here (at rest:
        // the driver's waves, 0x2236a0, are not reproduced yet).
        void water(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            if (traceLevel() >= 2)
            {
                static uint32_t calls = 0;
                if (++calls % 60 == 1)
                {
                    std::string type;
                    uint32_t index = 0, offset = 0, size = 0;
                    resourceAt(read32(rdram, ctx->r[4].uw[0] - 0x28), type, index, offset, size);
                    std::printf("[tta:3d] water render %u: %s %u +%x (size %u) frame %u\n", calls, type.c_str(), index, offset, size,
                                frame(rdram));
                }
            }
            if (env("TTA_MIST_AT_REST"))
                drawFor(rdram, ctx->r[4].uw[0] - 0x28);
            func_0x2221b8(rdram, ctx, rt);
            ctx->pc = ra;
        }

        // 0x221178: the type-21 render (instanced terrain, trees), instance = node + 0x28. Called by the scene directly.
        void terrain(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], node = ctx->r[4].uw[0] - 0x28;
            drawFor(rdram, node);
            func_0x221178(rdram, ctx, rt);
            ctx->pc = ra;
        }
    }

    namespace
    {
        // Discovery: which static shader renders run, and for which node types (TTA_3D_TRACE=3).
        std::map<std::pair<uint32_t, int>, uint32_t> g_staticCalls;

        template <uint32_t Address, void (*Original)(uint8_t *, R5900Context *, Runtime *)>
        void staticRender(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], a0 = ctx->r[4].uw[0];
            if (traceLevel() >= 3)
            {
                const uint32_t record = read32(rdram, a0 - 0x28);
                const int type = (record > 0x100000 && record < 0x2000000) ? rdram[(record + 0x88) & kMask] : -1;
                ++g_staticCalls[{Address, type}];
                if (frame(rdram) % 300 == 0 && g_staticCalls.size())
                {
                    for (const auto &[k, c] : g_staticCalls)
                        std::printf("[tta:3d] render %x node type %d: %u\n", k.first, k.second, c);
                    g_staticCalls.clear();
                }
            }
            // Type 0 (0x2207a8: the ground, rocks, static objects; instance = node + 0x28): drawn natively.
            if (Address == 0x2207a8)
                drawFor(rdram, a0 - 0x28);
            Original(rdram, ctx, rt);
            ctx->pc = ra;
        }
    }

    namespace
    {
        // ---- Characters: shader type 17, WeightedSkin (build/bozo/skin_spec.md) -----------------------------------
        // At 0x225fb0(instance = node + 0x28) the game's skin context is G = *(0x403a60) (VisualEntity E = G - 0x1d0):
        // the bone matrices *(G+0x290) (4x4 floats, rows, model space before W), W = *(G+0x294) (the character's
        // placement), the visible sections G+0x280 (128 bits), the fade G+0x2bc (0..0x80, 0 = not drawn) and the
        // lighting: order-2 coefficients at G+0x00..0x9f (C0, xx, yy, zz, xy, xz, yz, x, y, z; float4 RGB each, world
        // space), applied to the skinned normal: colour = vertex RGBA x max(c, 0). Screen: clip = p W RC+0x260, then
        // screen = clip / w x A + B (A = RC+0x440.., B = RC+0x450..). The vertices are skinned here, on the CPU.
        void transformPoint(const float m[16], const float in[3], float w, float out[3])
        {
            for (int r = 0; r < 3; ++r)
                out[r] = m[r] * in[0] + m[4 + r] * in[1] + m[8 + r] * in[2] + m[12 + r] * w;
        }

        uint32_t g_skinsDrawn = 0;
        bool g_flatDebug = false; // the type-18 pass under TTA_TYPE18_DEBUG: drawn flat magenta
        // The type-18 pass (0x2255d0): the shadow section's skinned triangles, in world space, and the screen matrix.
        bool g_shadowPass = false;
        std::vector<std::array<float, 3>> g_shadowWorld;
        float g_shadowScreen[16];

        void drawSkin(const uint8_t *rdram, uint32_t node)
        {
            // Only the passes into the displayed frame (FBP 0 / 0x70): a character is also drawn into off-screen targets
            // (shadows, reflections) with other cameras.
            {
                const uint32_t ds = read32(rdram, 0x36f850), fbp = read32(rdram, ds + 0x150) & 0x1ffu;
                if (fbp != 0 && fbp != 0x70)
                    return;
            }
            const uint32_t record = read32(rdram, node);
            std::string type;
            uint32_t index = 0, offset = 0, size = 0;
            if (!resourceAt(record, type, index, offset, size))
                return;
            const std::shared_ptr<Mesh> mesh = meshFor(type, index, size);
            if (!mesh)
                return;
            const auto it = mesh->byOffset.find(offset);
            if (it == mesh->byOffset.end() || it->second.kind != 3)
                return;
            const uint32_t g = read32(rdram, 0x403a60);
            const uint32_t bones = read32(rdram, g + 0x290), placement = read32(rdram, g + 0x294);
            const uint32_t fade = read32(rdram, g + 0x2bc);
            // TTA_SKIN_LAYERS=<mask>: draw only these layer passes (1 normal "01", 2 0x20000 "02", 4 0x200 "03",
            // 8 0x1000 "04"; debugging).
            if (const char *layers = env("TTA_SKIN_LAYERS"))
            {
                const uint32_t m = read32(rdram, g + 0x2b0);
                const uint32_t bit = (m & 0x80000u) ? 16u : (m & 0x200u) ? 4u : (m & 0x1000u) ? 8u : (m & 0x20000u) ? 2u : 1u;
                if (!(std::strtoul(layers, nullptr, 0) & bit))
                    return;
            }
            if (traceLevel() >= 2)
            {
                // The draw modes (G+0x2b0: 0x200 additive, 0x1000 subtractive, 0x2000 fade, 0x20000 two passes,
                // 0x40000 / 0x80000 silhouette or shadow) and the sections asked for (G+0x280).
                static std::map<std::pair<uint32_t, uint32_t>, uint32_t> modes;
                static uint32_t last = 0;
                ++modes[{read32(rdram, g + 0x2b0), index}];
                if (frame(rdram) / 300 != last)
                {
                    last = frame(rdram) / 300;
                    for (const auto &[mode, n] : modes)
                        std::printf("[tta:skinmode] frame %u: G+0x2b0 %x scbm %u x%u\n", frame(rdram), mode.first, mode.second, n);
                    modes.clear();
                }
            }
            if (!bones || !placement || fade == 0)
                return;
            uint32_t visible[4];
            for (int i = 0; i < 4; ++i)
                visible[i] = read32(rdram, g + 0x280 + i * 4);
            if (g_flatDebug)
            {
                static int n = 0;
                if (n++ < 6)
                    std::printf("[tta:t18] visible %08x %08x %08x %08x\n", visible[0], visible[1], visible[2], visible[3]);
            }
            float lighting[10][3];
            for (int k = 0; k < 10; ++k)
                for (int c = 0; c < 3; ++c)
                    lighting[k][c] = readFloat(rdram, g + k * 16 + c * 4);
            float w[16];
            for (int i = 0; i < 16; ++i)
                w[i] = readFloat(rdram, placement + i * 4);
            const uint32_t rc = read32(rdram, read32(rdram, 0x36f350));
            float clip[16], viewport[16] = {}, screen[16], matrix[16];
            for (int i = 0; i < 16; ++i)
                clip[i] = readFloat(rdram, rc + 0x260 + i * 4);
            for (int r = 0; r < 3; ++r)
            {
                viewport[r * 4 + r] = readFloat(rdram, rc + 0x440 + r * 4);
                viewport[12 + r] = readFloat(rdram, rc + 0x450 + r * 4);
            }
            viewport[15] = 1.0f;
            multiply(viewport, clip, screen);
            // G is set only by the VisualEntity draw 0x1b1b18 (through 0x225920): a skinned node drawn by another path
            // (the scene 0x1a7108, 0x1b24a8) runs with the last character's G. Fresh, RC+0x20 is the root bone
            // (bones[E+0x1c6], E = G - 0x1d0) placed by W; otherwise the node's own matrix is RC+0x20, in bind pose.
            float object[16], expected[16], root[16];
            for (int i = 0; i < 16; ++i)
                object[i] = readFloat(rdram, rc + 0x20 + i * 4);
            const uint32_t rootBone = read16(rdram, g - 0x1d0 + 0x1c6);
            for (int i = 0; i < 16; ++i)
                root[i] = readFloat(rdram, bones + rootBone * 64 + i * 4);
            multiply(w, root, expected);
            bool fresh = true;
            for (int i = 0; i < 16 && fresh; ++i)
                fresh = std::fabs(expected[i] - object[i]) <= 1e-3f * (1.0f + std::fabs(object[i]));
            if (!fresh)
                std::memcpy(w, object, sizeof(w));
            multiply(screen, w, matrix);
            std::unordered_map<uint32_t, std::array<float, 16>> boneCache;
            static const std::array<float, 16> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            const auto bone = [&](uint8_t b) -> const std::array<float, 16> &
            {
                if (!fresh)
                    return identity;
                auto [at, added] = boneCache.try_emplace(b);
                if (added)
                    for (int i = 0; i < 16; ++i)
                        at->second[i] = readFloat(rdram, bones + b * 64 + i * 4);
                return at->second;
            };
            if (env("TTA_SKIN_PROBE2") && frame(rdram) >= static_cast<uint32_t>(std::atoi(env("TTA_SKIN_PROBE2"))) && frame(rdram) < static_cast<uint32_t>(std::atoi(env("TTA_SKIN_PROBE2"))) + 2)
            {
                const uint32_t e = g - 0x1d0;
                const float sx = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), sy = std::sqrt(w[4] * w[4] + w[5] * w[5] + w[6] * w[6]);
                std::printf("[tta:skin2] %s %u +%x fresh %d E %x scale474 %.4g Wscale %.3g/%.3g Wpos %.4g %.4g %.4g bone0 %.4g %.4g %.4g t %.4g %.4g %.4g RC20pos %.4g %.4g %.4g root %u\n",
                            type.c_str(), index, offset, fresh ? 1 : 0, e, readFloat(rdram, e + 0x474), sx, sy, w[12], w[13], w[14],
                            readFloat(rdram, bones), readFloat(rdram, bones + 20), readFloat(rdram, bones + 40),
                            readFloat(rdram, bones + 48), readFloat(rdram, bones + 52), readFloat(rdram, bones + 56),
                            object[12], object[13], object[14], rootBone);
            }
            if (env("TTA_SKIN_PROBE3") && frame(rdram) >= static_cast<uint32_t>(std::atoi(env("TTA_SKIN_PROBE3"))) &&
                frame(rdram) < static_cast<uint32_t>(std::atoi(env("TTA_SKIN_PROBE3"))) + 1 && !it->second.batches.empty())
            {
                const Batch &b0 = it->second.batches[0];
                std::printf("[tta:skin3] %s %u fresh %d Wpos %.3g %.3g %.3g\n", type.c_str(), index, fresh ? 1 : 0, w[12], w[13], w[14]);
                for (size_t v = 0; v < b0.vertices.size() && v < 2; ++v)
                {
                    const uint8_t bi = b0.bones[v][0];
                    const auto &m = bone(bi);
                    const float in[3] = {b0.vertices[v].x, b0.vertices[v].y, b0.vertices[v].z};
                    float bp[3], wp[3];
                    transformPoint(m.data(), in, 1.0f, bp);
                    transformPoint(w, bp, 1.0f, wp);
                    std::printf("[tta:skin3]  v%zu (%.3g %.3g %.3g) bone %u [%.3g %.3g %.3g | %.3g %.3g %.3g | %.3g %.3g %.3g | t %.3g %.3g %.3g] -> model %.3g %.3g %.3g world %.4g %.4g %.4g\n",
                                v, in[0], in[1], in[2], bi, m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14],
                                bp[0], bp[1], bp[2], wp[0], wp[1], wp[2]);
                }
            }
            if (env("TTA_SKIN_PROBE4") && frame(rdram) == static_cast<uint32_t>(std::atoi(env("TTA_SKIN_PROBE4"))))
            {
                std::map<int, std::pair<int, float>> farBones; // bone -> (vertices, worst distance)
                size_t total = 0;
                for (const Batch &b : it->second.batches)
                    for (size_t v = 0; v < b.vertices.size(); ++v)
                    {
                        ++total;
                        const float in[3] = {b.vertices[v].x, b.vertices[v].y, b.vertices[v].z};
                        float p[3] = {};
                        for (int k = 0; k < 4; ++k)
                        {
                            if (b.weights[v][k] == 0.0f)
                                continue;
                            float bp[3];
                            transformPoint(bone(b.bones[v][k]).data(), in, 1.0f, bp);
                            for (int c = 0; c < 3; ++c)
                                p[c] += b.weights[v][k] * bp[c];
                        }
                        const float d = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                        if (d > 3.0f)
                        {
                            auto &e = farBones[b.bones[v][0]];
                            ++e.first;
                            e.second = std::fmax(e.second, d);
                        }
                    }
                std::printf("[tta:skin4] %s %u: %zu vertices, bones used far (model space, > 3 units):", type.c_str(), index, total);
                for (const auto &[bi, e] : farBones)
                {
                    const auto &m = bone(static_cast<uint8_t>(bi));
                    std::printf(" b%d x%d max %.3g [t %.3g %.3g %.3g s %.3g]", bi, e.first, e.second, m[12], m[13], m[14], m[0]);
                }
                std::printf("\n");
            }
            if (env("TTA_SKIN_PROBE5") && frame(rdram) == static_cast<uint32_t>(std::atoi(env("TTA_SKIN_PROBE5"))))
            {
                const uint32_t e = g - 0x1d0;
                const uint32_t cur = read32(rdram, e + 0xe0), base = read32(rdram, e + 0xe4), stamp = read32(rdram, e + 0xe8);
                std::printf("[tta:skin5] %s %u E %x G+0x290 %x E+0xe0 %x E+0xe4 %x E+0xe8 %x frameid %x\n", type.c_str(), index, e, bones,
                            cur, base, stamp, read32(rdram, 0x35fc94));
                const uint32_t ptrs[3] = {cur, base, base + 64 * 64};
                for (uint32_t p : ptrs)
                {
                    std::printf("[tta:skin5]   at %x:", p);
                    for (int b = 0; b < 6; ++b)
                        std::printf(" b%d[s %.3g t %.3g %.3g %.3g]", b, readFloat(rdram, p + b * 64), readFloat(rdram, p + b * 64 + 48),
                                    readFloat(rdram, p + b * 64 + 52), readFloat(rdram, p + b * 64 + 56));
                    std::printf("\n");
                }
            }
            // TTA_BONE_DUMP=<frame>: each character drawn that frame: its W and bone matrices to build/bones_<scbm>.bin
            // (u32 index, u32 bone count, 16 f32 W, then the bones, 16 f32 each), for checking the skin offline.
            if (env("TTA_BONE_DUMP") && frame(rdram) == static_cast<uint32_t>(std::atoi(env("TTA_BONE_DUMP"))))
            {
                const uint32_t e = g - 0x1d0, e4 = read32(rdram, e + 4);
                int16_t which = 0;
                std::memcpy(&which, rdram + ((e + 8) & kMask), 2);
                const uint32_t rec = e4 ? read32(rdram, e4 + 0x1c) + static_cast<uint32_t>(which) * 0x18 : 0;
                const uint32_t count = rec ? read32(rdram, rec) : 0;
                char path[96];
                std::snprintf(path, sizeof(path), "D:/TLOTR_TTA_Recomp/build/bones_%u.bin", index);
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fwrite(&index, 4, 1, f);
                    std::fwrite(&count, 4, 1, f);
                    std::fwrite(w, 4, 16, f);
                    std::fwrite(rdram + (bones & kMask), 64, count, f);
                    std::fclose(f);
                }
            }
            static int skinProbes = 0;
            if (env("TTA_SKIN_PROBE") && skinProbes < 4 && !it->second.batches.empty() &&
                !it->second.batches[0].vertices.empty())
            {
                ++skinProbes;
                const Batch &b0 = it->second.batches[0];
                std::printf("[tta:skin] %s %u +%x G %x bones %x W %x fade %u\n", type.c_str(), index, offset, g, bones,
                            placement, fade, fresh ? 1 : 0);
                std::printf("[tta:skin] W:");
                for (float f : w)
                    std::printf(" %.4g", f);
                std::printf("\n");
                for (int v = 0; v < 3 && v < static_cast<int>(b0.vertices.size()); ++v)
                {
                    std::printf("[tta:skin] v%d %.4g %.4g %.4g bones %u %u %u %u w %.3f %.3f %.3f %.3f\n", v, b0.vertices[v].x,
                                b0.vertices[v].y, b0.vertices[v].z, b0.bones[v][0], b0.bones[v][1], b0.bones[v][2],
                                b0.bones[v][3], b0.weights[v][0], b0.weights[v][1], b0.weights[v][2], b0.weights[v][3]);
                    const auto &m = bone(b0.bones[v][0]);
                    std::printf("[tta:skin]   bone %u:", b0.bones[v][0]);
                    for (float f : m)
                        std::printf(" %.4g", f);
                    std::printf("\n");
                }
                const auto &m = bone(b0.bones[0][0]);
                const float in[3] = {b0.vertices[0].x, b0.vertices[0].y, b0.vertices[0].z};
                float bp[3];
                transformPoint(m.data(), in, 1.0f, bp);
                float wp[4];
                for (int r = 0; r < 4; ++r)
                    wp[r] = w[r] * bp[0] + w[4 + r] * bp[1] + w[8 + r] * bp[2] + w[12 + r];
                float sp[4];
                for (int r = 0; r < 4; ++r)
                    sp[r] = matrix[r] * bp[0] + matrix[4 + r] * bp[1] + matrix[8 + r] * bp[2] + matrix[12 + r];
                std::printf("[tta:skin] model %.4g %.4g %.4g world %.4g %.4g %.4g screen %.4g %.4g (w %.4g)\n", bp[0], bp[1],
                            bp[2], wp[0], wp[1], wp[2], sp[0] / sp[3], sp[1] / sp[3], sp[3]);
            }
            auto &renderer = bozo::render::Renderer::instance();
            std::vector<bozo::render::Vertex> skinned, triangles;
            float extentMin[3] = {1e30f, 1e30f, 1e30f}, extentMax[3] = {-1e30f, -1e30f, -1e30f};
            for (const Batch &batch : it->second.batches)
            {
                if (batch.section < 128 && !(visible[batch.section / 32] & (1u << (batch.section % 32))))
                    continue;
                const uint32_t type = batch.prim & 7u;
                if (type < 3 || type > 5 || batch.vertices.size() < 3)
                    continue;
                skinned = batch.vertices;
                for (size_t v = 0; v < skinned.size(); ++v)
                {
                    float p[3] = {}, n[3] = {};
                    const float in[3] = {batch.vertices[v].x, batch.vertices[v].y, batch.vertices[v].z};
                    for (int k = 0; k < 4; ++k)
                    {
                        const float weight = batch.weights[v][k];
                        if (weight == 0.0f)
                            continue;
                        const auto &m = bone(batch.bones[v][k]);
                        float bp[3], bn[3];
                        transformPoint(m.data(), in, 1.0f, bp);
                        transformPoint(m.data(), batch.normals[v].data(), 0.0f, bn);
                        for (int c = 0; c < 3; ++c)
                        {
                            p[c] += weight * bp[c];
                            n[c] += weight * bn[c];
                        }
                    }
                    for (int c = 0; c < 3; ++c)
                    {
                        extentMin[c] = std::fmin(extentMin[c], p[c]);
                        extentMax[c] = std::fmax(extentMax[c], p[c]);
                    }
                    skinned[v].x = p[0];
                    skinned[v].y = p[1];
                    skinned[v].z = p[2];
                    // The lighting, on the world-space normal (W's rotation), normalised.
                    float wn[3];
                    transformPoint(w, n, 0.0f, wn);
                    const float len = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                    if (len > 0.0f)
                        for (float &c : wn)
                            c /= len;
                    const float terms[10] = {1.0f, wn[0] * wn[0], wn[1] * wn[1], wn[2] * wn[2], wn[0] * wn[1],
                                             wn[0] * wn[2], wn[1] * wn[2], wn[0], wn[1], wn[2]};
                    uint32_t rgba = 0;
                    for (int c = 0; c < 3; ++c)
                    {
                        float light = 0.0f;
                        for (int k = 0; k < 10; ++k)
                            light += lighting[k][c] * terms[k];
                        const float value = static_cast<float>((batch.colours[v] >> (c * 8)) & 0xffu) * std::fmax(light, 0.0f);
                        rgba |= static_cast<uint32_t>(std::fmin(value, 255.0f)) << (c * 8);
                    }
                    if (env("TTA_SKIN_NOLIGHT"))
                        rgba = batch.colours[v] & 0xffffffu;
                    const uint32_t alpha = ((batch.colours[v] >> 24) & 0xffu) * fade / 0x80u;
                    skinned[v].rgba = rgba | std::min<uint32_t>(alpha, 0xffu) << 24;
                }
                // A shadow-volume mesh (prim 0x8003, the part layout of set 5's program 0x178): never a surface; in the
                // type-18 pass its triangles go to the volume, in world space (W).
                if (batch.prim & 0x8000u)
                {
                    if (g_shadowPass && type == 3)
                    {
                        std::memcpy(g_shadowScreen, screen, sizeof(g_shadowScreen));
                        for (size_t v = 0; v + 2 < skinned.size(); v += 3)
                            for (size_t k = 0; k < 3; ++k)
                            {
                                const float in[3] = {skinned[v + k].x, skinned[v + k].y, skinned[v + k].z};
                                std::array<float, 3> out;
                                transformPoint(w, in, 1.0f, out.data());
                                g_shadowWorld.push_back(out);
                            }
                    }
                    continue;
                }
                triangles.clear();
                for (size_t i = 2; i < skinned.size(); ++i)
                {
                    if (batch.adc[i])
                        continue;
                    if (type == 3)
                    {
                        if (i % 3 == 2)
                            triangles.insert(triangles.end(), {skinned[i - 2], skinned[i - 1], skinned[i]});
                    }
                    else if (type == 4)
                        triangles.insert(triangles.end(), {skinned[i - 2], skinned[i - 1], skinned[i]});
                    else
                        triangles.insert(triangles.end(), {skinned[0], skinned[i - 1], skinned[i]});
                }
                if (triangles.empty())
                    continue;
                bozo::render::Material material;
                material.depthTest = true;
                material.depthWrite = fade >= 0x80;
                material.blend = fade < 0x80 ? bozo::render::Material::Blend::Alpha : bozo::render::Material::Blend::Opaque;
                std::shared_ptr<const bozo::render::Texture> texture = batch.texture ? textureById(batch.texture) : nullptr;
                material.textured = texture != nullptr;
                if (g_flatDebug)
                {
                    material.textured = false;
                    for (auto &v : triangles)
                        v.rgba = 0x80ff00ffu;
                }
                if (texture)
                    material.tex0 = texture->key;
                clipToDepthRange(triangles, matrix);
                if (triangles.empty())
                    continue;
                // The default material template (0x377aa0): ALPHA_1 0x44 = (Cs - Cd) As + Cd and TEST_1 0x5180b = alpha
                // >= 0x80, a failing texel still written in colour but not in Z (AFAIL FB_ONLY), Z GEQUAL. Hair and
                // cloak edges blend over what is behind them, the opaque texels own the depth. The renderer has no
                // AFAIL: two passes, the passing texels with Z writes, then the others blended without.
                // The layer passes (0x1b1b18 draws a character's layers "01".."04" one after the other, G+0x2b0 set per
                // layer): 0x200 (layer 03) ALPHA 0x68 = Cs FIX + Cd, 0x1000 (layer 04) 0x62 = Cd - Cs FIX, FIX = the
                // fade; both without Z writes or alpha test (templates 0x377bf0 / 0x377c60).
                const uint32_t mode = read32(rdram, g + 0x2b0);
                if (mode & 0x1200u)
                {
                    material.gsBlend = true;
                    material.blendA = (mode & 0x200u) ? 0 : 2;
                    material.blendB = (mode & 0x200u) ? 2 : 0;
                    material.blendC = 2;
                    material.blendD = 1;
                    material.blendFix = static_cast<uint8_t>(std::min<uint32_t>(fade, 0x80u));
                    material.depthWrite = false;
                }
                else if (texture && fade >= 0x80 && !env("TTA_SKIN_NO_AFAIL"))
                {
                    material.blend = bozo::render::Material::Blend::Alpha;
                    material.alphaTest = true;
                    material.alphaFunc = bozo::render::Material::AlphaFunc::GreaterEqual;
                    material.alphaRef = 0x80;
                    renderer.draw(material, matrix, triangles.data(), static_cast<uint32_t>(triangles.size()), texture);
                    material.alphaFunc = bozo::render::Material::AlphaFunc::Less;
                    material.depthWrite = false;
                }
                renderer.draw(material, matrix, triangles.data(), static_cast<uint32_t>(triangles.size()), texture);
            }
            ++g_skinsDrawn;
            if (env("TTA_SKIN_EXTENT") && frame(rdram) == static_cast<uint32_t>(std::atoi(env("TTA_SKIN_EXTENT"))))
                std::printf("[tta:extent] %s %u: model x %.3g..%.3g y %.3g..%.3g z %.3g..%.3g; W pos %.4g %.4g %.4g scale %.3g\n",
                            type.c_str(), index, extentMin[0], extentMax[0], extentMin[1], extentMax[1], extentMin[2], extentMax[2],
                            w[12], w[13], w[14], std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]));
        }

        // The VisualEntity draw 0x1b1b18(E, W, ...): the only one that sets G (0x225920) for its skinned nodes.
        int g_inVisualEntity = 0;
        uint32_t g_skinInside = 0, g_skinOutside = 0;

        void visualEntity(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            ++g_inVisualEntity;
            func_0x1b1b18(rdram, ctx, rt);
            --g_inVisualEntity;
            ctx->pc = ra;
        }

        std::map<uint32_t, uint32_t> g_setters;

        uint32_t g_samplerCalls = 0, g_samplerZero = 0, g_deferred = 0;

        // Discovery: the per-frame animation update 0x137800(E, f12 = dt) (skin_spec 7).
        void animate(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], e = ctx->r[4].uw[0];
            const float dt = ctx->f[12];
            static uint32_t calls = 0, lastFrame = 0, printed = 0;
            ++calls;
            const char *probe = env("TTA_ANIM_PROBE");
            if (probe && frame(rdram) >= static_cast<uint32_t>(std::atoi(probe)) && printed < 12)
            {
                ++printed;
                int16_t e8;
                std::memcpy(&e8, rdram + ((e + 8) & kMask), 2);
                const uint32_t e4 = read32(rdram, e + 4);
                std::printf("[tta:anim] frame %u from %x E %x *E %x E+4 %x (+0x1c %x) E+8 %d E+0xc %x dt %.4g calls %u\n", frame(rdram), ra,
                            e, read32(rdram, e), e4, e4 ? read32(rdram, e4 + 0x1c) : 0, e8, read32(rdram, e + 0xc), dt, calls);
            }
            if (frame(rdram) != lastFrame)
            {
                if (probe && frame(rdram) % 600 == 0)
                    std::printf("[tta:anim] frame %u: %u updates, %u samplings (%u gave 0), %u stopped inside\n", frame(rdram), calls, g_samplerCalls, g_samplerZero, g_deferred);
                lastFrame = frame(rdram);
            }
            func_0x137800(rdram, ctx, rt);
            if (rt->stopDeferred || ctx->pc != ra)
                ++g_deferred;
            ctx->pc = ra;
        }

        // Discovery: the animation sampler 0x13ea30(E): the pose buffer, 0 when nothing samples (skin_spec 7).
        void sample(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], e = ctx->r[4].uw[0];
            func_0x13ea30(rdram, ctx, rt);
            ctx->pc = ra;
            ++g_samplerCalls;
            if (ctx->r[2].uw[0] == 0)
                ++g_samplerZero;
            static uint32_t printed = 0;
            const char *probe = env("TTA_ANIM_PROBE");
            if (probe && frame(rdram) >= static_cast<uint32_t>(std::atoi(probe)) && printed < 8)
            {
                ++printed;
                const uint32_t anim = read32(rdram, e + 0xc);
                std::printf("[tta:anim] sampler(E %x) from %x -> %x; anim %x: %x %x %x %x %x %x %x %x\n", e, ra, ctx->r[2].uw[0], anim,
                            read32(rdram, anim), read32(rdram, anim + 4), read32(rdram, anim + 8), read32(rdram, anim + 12),
                            read32(rdram, anim + 16), read32(rdram, anim + 20), read32(rdram, anim + 24), read32(rdram, anim + 28));
            }
        }

        // Discovery: the skeleton record 0x13a260(E) (skin_spec 7).
        void skeletonRecord(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], e = ctx->r[4].uw[0];
            func_0x13a260(rdram, ctx, rt);
            ctx->pc = ra;
            static uint32_t printed = 0;
            const char *probe = env("TTA_ANIM_PROBE");
            if (probe && frame(rdram) >= static_cast<uint32_t>(std::atoi(probe)) && printed++ < 8)
            {
                const uint32_t e4 = read32(rdram, e + 4), rec = ctx->r[2].uw[0];
                std::printf("[tta:anim] skeleton(E %x) from %x -> %x (N %u); E+4 %x, *(E+4)+0x1c %x, *that %x\n", e, ra, rec,
                            rec ? read32(rdram, rec) : 0, e4, e4 ? read32(rdram, e4 + 0x1c) : 0,
                            e4 ? read32(rdram, read32(rdram, e4 + 0x1c)) : 0);
            }
        }

        // Discovery: the pose builder 0x133298(E, localPose, 1) (skin_spec 6).
        uint32_t g_poseCalls = 0;
        void buildPose(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0], e = ctx->r[4].uw[0];
            func_0x133298(rdram, ctx, rt);
            ctx->pc = ra;
            ++g_poseCalls;
            static uint32_t posePrinted = 0;
            if (env("TTA_POSE_PROBE") && frame(rdram) >= static_cast<uint32_t>(std::atoi(env("TTA_POSE_PROBE"))) && posePrinted++ < 6)
            {
                const uint32_t cur = read32(rdram, e + 0xe0);
                std::printf("[tta:pose] E %x (calls so far %u) from %x: E+0xe0 %x E+0x474 %.3g:", e, g_poseCalls, ra, cur,
                            readFloat(rdram, e + 0x474));
                for (int b = 0; b < 5; ++b)
                    std::printf(" b%d[%.3g %.3g %.3g | t %.3g %.3g %.3g]", b, readFloat(rdram, cur + b * 64),
                                readFloat(rdram, cur + b * 64 + 20), readFloat(rdram, cur + b * 64 + 40),
                                readFloat(rdram, cur + b * 64 + 48), readFloat(rdram, cur + b * 64 + 52), readFloat(rdram, cur + b * 64 + 56));
                std::printf("\n");
            }
        }

        // TTA_CHAR_PROBE=<frame>: each character a G setter installs that frame: its pose stamp (E+0xe8) against the
        // frame id, its pose buffer, its animation and skeleton.
        int g_lastSetter = 0; // 1: 0x225920 (VisualEntity), 2: 0x2251b0 (ChrCntl)

        void noteCharacter(const uint8_t *rdram, uint32_t e, uint32_t from, const char *setter)
        {
            g_lastSetter = setter[7] == '9' ? 1 : 2;
            const char *probe = env("TTA_CHAR_PROBE");
            if (!probe || frame(rdram) != static_cast<uint32_t>(std::atoi(probe)))
                return;
            int16_t stamp, id, which;
            std::memcpy(&stamp, rdram + ((e + 0xe8) & kMask), 2);
            std::memcpy(&id, rdram + 0x35fc94, 2);
            std::memcpy(&which, rdram + ((e + 8) & kMask), 2);
            std::printf("[tta:char] %s from %x: E %x *E %x stamp %d (frame id %d) E+0xe0 %x E+0xe4 %x E+4 %x E+8 %d E+0xc %x "
                        "E+0x474 %.3g E+0x460 %x\n",
                        setter, from, e, read32(rdram, e), stamp, id, read32(rdram, e + 0xe0), read32(rdram, e + 0xe4),
                        read32(rdram, e + 4), which, read32(rdram, e + 0xc), readFloat(rdram, e + 0x474), read32(rdram, e + 0x460));
        }

        void setSkinContext(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            if (traceLevel() >= 1)
            {
                ++g_setters[ra];
                static bool printed = false;
                if (frame(rdram) >= 5000 && !printed && (printed = true))
                {
                    for (const auto &[at, n] : g_setters)
                        std::printf("[tta:3d] G set from %x: %u\n", at, n);
                    g_setters.clear();
                }
            }
            noteCharacter(rdram, ctx->r[4].uw[0], ra, "0x225920");
            func_0x225920(rdram, ctx, rt);
            ctx->pc = ra;
        }

        // 0x2251b0(G): the ChrCntl character draw's G setter (0x11ab40); G = E + 0x1d0.
        void setSkinContextChr(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            noteCharacter(rdram, ctx->r[4].uw[0] - 0x1d0, ra, "0x2251b0");
            func_0x2251b0(rdram, ctx, rt);
            ctx->pc = ra;
        }

        // TTA_SHADOW_PROBE=<game frame>: the 14-qword VU block 0x21d438 uploads (VU 0..13) in that frame, by caller.
        void vuBlock(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            if (const char *f = env("TTA_SHADOW_PROBE"); f && frame(rdram) == static_cast<uint32_t>(std::atoi(f)))
            {
                const uint32_t block = ctx->r[5].uw[0], g = read32(rdram, 0x403a60);
                std::printf("[tta:vublock] from %x G %x G+298 %x G+2b4 %x:\n", ra, g, read32(rdram, g + 0x298), read32(rdram, g + 0x2b4));
                for (int q = 0; q < 14; ++q)
                    std::printf("  %2d: %g %g %g %g\n", q, readFloat(rdram, block + q * 16), readFloat(rdram, block + q * 16 + 4),
                                readFloat(rdram, block + q * 16 + 8), readFloat(rdram, block + q * 16 + 12));
            }
            func_0x21d438(rdram, ctx, rt);
            ctx->pc = ra;
        }

        // The shadow volume: each triangle of the shadow mesh swept along the light (G+0x298: the light's direction, the
        // default 0x377990 = (2, -2, 0); y is down) into a closed prism, its faces counted into the stencil (z-pass: up for
        // one screen winding, down for the other; a pixel whose surface lies inside the swept body keeps a count), then the
        // frame darkened where the count is not 0. The game does the same with set 5's program 0x178 (the silhouette edges
        // and caps, +/-) and its full-screen pass (0x21ae10: the frame x (1 - TA0/128) where the count is not 0; checked in a PCSX2 GS dump).
        // TTA_NO_SHADOWS=1 turns it off. TTA_SHADOW_REACH (x the mesh's size)
        // and TTA_SHADOW_ALPHA (0..0x80) tune it.
        void drawShadowVolume(const uint8_t *rdram)
        {
            if (g_shadowWorld.size() < 3)
                return;
            const uint32_t g = read32(rdram, 0x403a60), lightAt = read32(rdram, g + 0x298);
            const uint32_t light = lightAt ? lightAt : 0x377990u;
            float d[3] = {readFloat(rdram, light), readFloat(rdram, light + 4), readFloat(rdram, light + 8)};
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (!(dl > 0.0f))
                return;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (const auto &p : g_shadowWorld)
                for (int c = 0; c < 3; ++c)
                {
                    lo[c] = std::fmin(lo[c], p[c]);
                    hi[c] = std::fmax(hi[c], p[c]);
                }
            const float size = std::fmax(hi[0] - lo[0], std::fmax(hi[1] - lo[1], hi[2] - lo[2]));
            if (env("TTA_SHADOW_ORDER"))
                std::printf("[tta:shadow] light %x (%.3g %.3g %.3g) world x %.4g..%.4g y %.4g..%.4g z %.4g..%.4g, %zu triangles\n", light,
                            d[0], d[1], d[2], lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], g_shadowWorld.size() / 3);
            const float reach = size * static_cast<float>(env("TTA_SHADOW_REACH") ? std::atof(env("TTA_SHADOW_REACH")) : 3.0);
            for (float &c : d)
                c *= reach / dl;
            const float *m = g_shadowScreen;
            const auto vertex = [](const std::array<float, 3> &p)
            {
                bozo::render::Vertex v;
                v.x = p[0];
                v.y = p[1];
                v.z = p[2];
                v.rgba = 0x80808080u;
                return v;
            };
            // Reused between frames (a character's volume is a few thousand faces each frame).
            static std::vector<bozo::render::Vertex> up, down, face;
            up.clear();
            down.clear();
            up.reserve(g_shadowWorld.size() * 8);
            down.reserve(g_shadowWorld.size() * 8);
            constexpr float kDepthMax = 16777215.0f, kWMin = 1.0e-3f;
            const auto addFace = [&](std::array<float, 3> a, std::array<float, 3> b, std::array<float, 3> c, const float centre[3])
            {
                // Outward: the face's normal away from the prism's centre.
                const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, t[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const float n[3] = {u[1] * t[2] - u[2] * t[1], u[2] * t[0] - u[0] * t[2], u[0] * t[1] - u[1] * t[0]};
                const float out = n[0] * (a[0] - centre[0]) + n[1] * (a[1] - centre[1]) + n[2] * (a[2] - centre[2]);
                if (out == 0.0f)
                    return;
                if (out < 0.0f)
                    std::swap(b, c);
                // The common case: all three in front of the camera and inside the Z range: no clipping, the screen
                // winding from the projected points directly.
                const std::array<float, 3> *p[3] = {&a, &b, &c};
                float sx[3], sy[3];
                bool inside = true;
                for (int k = 0; k < 3 && inside; ++k)
                {
                    const auto &q = *p[k];
                    const float w = m[3] * q[0] + m[7] * q[1] + m[11] * q[2] + m[15];
                    const float z = m[2] * q[0] + m[6] * q[1] + m[10] * q[2] + m[14];
                    inside = w > kWMin && z >= 0.0f && z <= kDepthMax * w;
                    if (inside)
                    {
                        sx[k] = (m[0] * q[0] + m[4] * q[1] + m[8] * q[2] + m[12]) / w;
                        sy[k] = (m[1] * q[0] + m[5] * q[1] + m[9] * q[2] + m[13]) / w;
                    }
                }
                if (inside)
                {
                    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
                    auto &into = area > 0.0f ? up : down;
                    into.push_back(vertex(a));
                    into.push_back(vertex(b));
                    into.push_back(vertex(c));
                    return;
                }
                face = {vertex(a), vertex(b), vertex(c)};
                clipToDepthRange(face, m);
                for (size_t i = 0; i + 2 < face.size(); i += 3)
                {
                    float sx[3], sy[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const auto &v = face[i + k];
                        const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                        sx[k] = (m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12]) / w;
                        sy[k] = (m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13]) / w;
                    }
                    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
                    auto &into = area > 0.0f ? up : down;
                    into.insert(into.end(), face.begin() + static_cast<std::ptrdiff_t>(i),
                                face.begin() + static_cast<std::ptrdiff_t>(i) + 3);
                }
            };
            for (size_t i = 0; i + 2 < g_shadowWorld.size(); i += 3)
            {
                const auto &a = g_shadowWorld[i], &b = g_shadowWorld[i + 1], &c = g_shadowWorld[i + 2];
                std::array<float, 3> a2, b2, c2;
                for (int k = 0; k < 3; ++k)
                {
                    a2[k] = a[k] + d[k];
                    b2[k] = b[k] + d[k];
                    c2[k] = c[k] + d[k];
                }
                // A triangle edge-on to the light sweeps no volume.
                const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, t[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const float n[3] = {u[1] * t[2] - u[2] * t[1], u[2] * t[0] - u[0] * t[2], u[0] * t[1] - u[1] * t[0]};
                const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (!(nl > 0.0f) || std::fabs(n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) < 1e-4f * nl * reach)
                    continue;
                float centre[3];
                for (int k = 0; k < 3; ++k)
                    centre[k] = (a[k] + b[k] + c[k]) / 3.0f + d[k] * 0.5f;
                addFace(a, b, c, centre);
                addFace(a2, b2, c2, centre);
                addFace(a, b, b2, centre);
                addFace(a, b2, a2, centre);
                addFace(b, c, c2, centre);
                addFace(b, c2, b2, centre);
                addFace(c, a, a2, centre);
                addFace(c, a2, c2, centre);
            }
            auto &renderer = bozo::render::Renderer::instance();
            // TTA_SHADOW_SHOW=1: the volume's faces drawn in colour instead (red one winding, blue the other).
            if (env("TTA_SHADOW_SHOW"))
            {
                bozo::render::Material show;
                show.textured = false;
                show.blend = bozo::render::Material::Blend::Alpha;
                show.depthTest = true;
                show.depthWrite = false;
                for (auto &v : up)
                    v.rgba = 0x300000ffu;
                for (auto &v : down)
                    v.rgba = 0x30ff0000u;
                if (!up.empty())
                    renderer.draw(show, m, up.data(), static_cast<uint32_t>(up.size()));
                if (!down.empty())
                    renderer.draw(show, m, down.data(), static_cast<uint32_t>(down.size()));
                return;
            }
            static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            // The whole GS window (TTA draws to 1792..2304 x 1824..2272).
            const auto wholeScreen = [&](const bozo::render::Material &material, uint32_t rgba)
            {
                bozo::render::Vertex q[6];
                const float corners[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
                for (int k = 0; k < 6; ++k)
                {
                    q[k].x = 1792.0f + corners[k][0] * 512.0f;
                    q[k].y = 1824.0f + corners[k][1] * 448.0f;
                    q[k].z = 0.0f;
                    q[k].rgba = rgba;
                }
                renderer.draw(material, identity, q, 6u);
            };
            bozo::render::Material volume;
            volume.textured = false;
            volume.blend = bozo::render::Material::Blend::Opaque;
            volume.depthTest = false;
            volume.depthWrite = false;
            volume.colourMask = 0u;
            volume.stencilClear = true;
            wholeScreen(volume, 0u);
            volume.stencilClear = false;
            volume.depthTest = true;
            volume.stencilOp = bozo::render::Material::StencilOp::IncrementWrap;
            if (!up.empty())
                renderer.draw(volume, m, up.data(), static_cast<uint32_t>(up.size()));
            volume.stencilOp = bozo::render::Material::StencilOp::DecrementWrap;
            if (!down.empty() && !env("TTA_SHADOW_UP_ONLY"))
                renderer.draw(volume, m, down.data(), static_cast<uint32_t>(down.size()));
            bozo::render::Material darken;
            darken.textured = false;
            darken.blend = bozo::render::Material::Blend::Alpha;
            darken.depthTest = false;
            darken.depthWrite = false;
            darken.stencilNotZero = !env("TTA_SHADOW_NO_STENCIL");
            darken.colourMask = 7u;
            const char *alpha = env("TTA_SHADOW_ALPHA");
            // The game's strength (0x21ae10's TEXA.TA0 = *(0x3779a0), set by 0x225218 from G+0x2b4 when the character has
            // its own light at G+0x298): the frame times 1 - TA0 / 128 where a volume counted (0x13..0x20 seen: -15..25%).
            const uint32_t gameStrength = (lightAt ? read32(rdram, g + 0x2b4) : read32(rdram, 0x3779a0)) & 0xffu;
            const uint32_t strength = alpha ? static_cast<uint32_t>(std::strtoul(alpha, nullptr, 0)) : gameStrength;
            wholeScreen(darken, std::min<uint32_t>(strength, 0x80u) << 24);
        }

        // TTA_POST_DUMP=<game frame>: a discovery probe (not a render path): the packet bytes the post passes build in
        // that frame (the packet pointer *(0x3b5bf8) before and after each call) to build/post_<function>_<n>.bin, with the
        // draw state (FRAME_1, TEST, ALPHA, ZBUF) printed, for decoding offline.
        template <uint32_t Address, void (*Original)(uint8_t *, R5900Context *, Runtime *)>
        void postProbe(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            const char *dump = env("TTA_POST_DUMP");
            const bool on = dump && frame(rdram) >= static_cast<uint32_t>(std::atoi(dump)) &&
                            frame(rdram) < static_cast<uint32_t>(std::atoi(dump)) + 2;
            const uint32_t a0 = ctx->r[4].uw[0], before = read32(rdram, 0x3b5bf8);
            Original(rdram, ctx, rt);
            if (on)
            {
                static int n = 0;
                const uint32_t after = read32(rdram, 0x3b5bf8);
                char path[128];
                std::snprintf(path, sizeof(path), "D:/TLOTR_TTA_Recomp/build/post_%x_%d.bin", Address, n++);
                if (after > before && after - before < 0x100000)
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fwrite(rdram + (before & kMask), 1, after - before, f);
                        std::fclose(f);
                    }
                const uint32_t ds = read32(rdram, 0x36f850);
                std::printf("[tta:post] frame %u %x(a0 %x): %u bytes at %x -> %s; FRAME_1 %llx TEST %llx ALPHA %llx ZBUF %llx\n",
                            frame(rdram), Address, a0, after - before, before, path,
                            static_cast<unsigned long long>(read64(rdram, ds + 0x150)),
                            static_cast<unsigned long long>(read64(rdram, ds + 0x140)),
                            static_cast<unsigned long long>(read64(rdram, ds + 0x100)),
                            static_cast<unsigned long long>(read64(rdram, ds + 0x120)));
            }
            ctx->pc = ra;
        }

        // TTA_GLOW_DUMP=<game frame>: a discovery probe (not a render path): while a glow renders (0x21f980), each DMA tag it
        // chains (0x2163c8(address, qwords, .., tag)) and the qwords it points at, to build/glow_<n>_<k>.bin.
        bool g_inGlow = false;
        int g_glowIndex = -1, g_glowTag = 0;

        void glowRender(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            const char *dump = env("TTA_GLOW_DUMP");
            const bool on = dump && frame(rdram) == static_cast<uint32_t>(std::atoi(dump));
            if (on)
            {
                static int n = 0;
                g_glowIndex = n++;
                g_glowTag = 0;
                const uint32_t s = read32(rdram, ctx->r[4].uw[0] + 4);
                std::printf("[tta:glow] #%d instance %x S %x: +88 %g +8c %x +a0 %x +a4 %g +c8 %x +134 %x +138 %x\n", g_glowIndex,
                            ctx->r[4].uw[0], s, readFloat(rdram, s + 0x88), read32(rdram, s + 0x8c), read32(rdram, s + 0xa0),
                            readFloat(rdram, s + 0xa4), read32(rdram, s + 0xc8), read32(rdram, s + 0x134), read32(rdram, s + 0x138));
                char path[128];
                std::snprintf(path, sizeof(path), "D:/TLOTR_TTA_Recomp/build/glowS_%d.bin", g_glowIndex);
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fwrite(rdram + (s & kMask), 1, 0x300, f);
                    std::fclose(f);
                }
                const uint32_t parts[3] = {read32(rdram, s + 0xc8), read32(rdram, s + 0xcc), read32(rdram, s + 0x134)};
                for (int k = 0; k < 3; ++k)
                {
                    std::snprintf(path, sizeof(path), "D:/TLOTR_TTA_Recomp/build/glowR_%d_%d.bin", g_glowIndex, k);
                    if (parts[k])
                        if (FILE *f = std::fopen(path, "wb"))
                        {
                            std::fwrite(rdram + (parts[k] & kMask), 1, 0x200, f);
                            std::fclose(f);
                        }
                }
            }
            g_inGlow = on;
            func_0x21f980(rdram, ctx, rt);
            g_inGlow = false;
            ctx->pc = ra;
        }

        void dmaTag(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            if (g_inGlow)
            {
                const uint32_t address = ctx->r[4].uw[0], qwords = ctx->r[5].uw[0], tag = ctx->r[7].uw[0];
                std::printf("[tta:glow]   tag %x: %u qwords at %x (a2 %x)\n", tag, qwords, address, ctx->r[6].uw[0]);
                char path[128];
                std::snprintf(path, sizeof(path), "D:/TLOTR_TTA_Recomp/build/glow_%d_%d.bin", g_glowIndex, g_glowTag++);
                if (qwords && qwords < 0x10000)
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fwrite(rdram + (address & kMask), 16, qwords, f);
                        std::fclose(f);
                    }
            }
            func_0x2163c8(rdram, ctx, rt);
            ctx->pc = ra;
        }

        // ---- Mist banks: the type-19 module's animator 0x2236a0 (the forest's haze) ---------------------------------------
        // 0x2236a0(output buffer a0, object a1 (the geometry below), time a2, mask a3, interpolated ST t0, .., second layer t2, ..; f12 / f13 the
        // texture's scroll, f14 a scale), called by the driver 0x2246e8 per layer once it has set the layer's texture
        // (TEX0_1 at DS+0x1a0, uploaded by 0x219ca8). Geometry: +8 u8 ?, +9 u8 subdivisions n, +0xe u16 wave amplitude
        // (x 0.001), +0x14 u16 edge count, +0x18 the edges, +0x1c a record (+0x30 / +0x32: the world-mapped ST's offsets).
        // An edge is two 60-byte vertices {x y z, s t, R G B A bytes, dx dy dz, ds dt, dR dG dB dA}: n + 1 rows from the
        // vertex to vertex + d, each a pair of the strip, everything interpolated; y moves by amplitude x 0.001 x a sine
        // of the 32-entry quarter-wave table 0x403788 at ((x + y + z) x 16 + time) mod 124; ST scrolls by f12 / f13. The
        // mask pass (a3 != 0: flat colours, alpha 0x80) is not drawn. TTA_NO_MIST=1 turns the native mist off.
        float mistSine(const uint8_t *rdram, float x, float y, float z, uint32_t time)
        {
            const uint32_t at = (static_cast<uint32_t>(static_cast<int32_t>((x + y + z) * 16.0f)) + time) % 124u;
            uint32_t i = at;
            float sign = 1.0f;
            if (at < 62u)
                i = at < 31u ? at : 62u - at;
            else
            {
                sign = -1.0f;
                i = at - 62u;
                if (at >= 93u)
                    i = 124u - at;
            }
            return sign * readFloat(rdram, 0x403788u + i * 4u);
        }

        void drawMist(const uint8_t *rdram, uint32_t geometry, uint32_t time, bool interpolatedSt, float scrollS, float scrollT,
                      float fade)
        {
            const uint32_t subdivisions = rdram[(geometry + 9) & kMask];
            const uint32_t edges = read32(rdram, geometry + 0x18), count = read16(rdram, geometry + 0x14);
            const uint32_t record = read32(rdram, geometry + 0x1c);
            const float amplitude = static_cast<float>(read16(rdram, geometry + 0xe)) * 0.001f;
            if (!subdivisions || !count || count > 4096 || !edges)
                return;
            const uint32_t ds = read32(rdram, 0x36f850);
            const uint64_t tex0 = read64(rdram, ds + 0x1a0);
            const auto texture = textureAtPlace(static_cast<uint32_t>(tex0 & 0x3fffu), static_cast<uint32_t>((tex0 >> 20) & 0x3fu));
            const uint32_t rc = read32(rdram, read32(rdram, 0x36f350));
            float matrix[16];
            for (int i = 0; i < 16; ++i)
                matrix[i] = readFloat(rdram, rc + 0x2e0 + i * 4);
            const float step = 1.0f / static_cast<float>(subdivisions);
            const float offsetS = record ? static_cast<float>(static_cast<int16_t>(read16(rdram, record + 0x30))) : 0.0f;
            const float offsetT = record ? static_cast<float>(static_cast<int16_t>(read16(rdram, record + 0x32))) : 0.0f;
            std::vector<bozo::render::Vertex> strip, triangles;
            for (uint32_t e = 0; e < count; ++e)
            {
                const uint32_t edge = edges + e * 0x78u;
                strip.clear();
                // Rows run along the edge, A (+0) to B (+0x3c) in n steps; each row pairs the base point with the extruded one
                // (the vertex + its d), both lines interpolated A -> B (0x223ac0..0x223d28, 0x223f28).
                const auto f = [&](uint32_t side, uint32_t o) { return readFloat(rdram, edge + side * 0x3cu + o); };
                const auto byte = [&](uint32_t side, int c)
                { return static_cast<float>(rdram[(edge + side * 0x3cu + 0x14u + static_cast<uint32_t>(c)) & kMask]); };
                for (uint32_t row = 0; row <= subdivisions; ++row)
                {
                    const float t = static_cast<float>(row) * step;
                    for (uint32_t extruded = 0; extruded < 2; ++extruded)
                    {
                        const auto at = [&](uint32_t o, uint32_t d)
                        {
                            const float a = f(0, o) + (extruded ? f(0, d) : 0.0f), b = f(1, o) + (extruded ? f(1, d) : 0.0f);
                            return a + (b - a) * t;
                        };
                        const float x = at(0x00, 0x18), y = at(0x04, 0x1c), z = at(0x08, 0x20);
                        bozo::render::Vertex out;
                        out.x = x;
                        out.y = y + amplitude * mistSine(rdram, x, y, z, time);
                        out.z = z;
                        if (interpolatedSt)
                        {
                            out.u = at(0x0c, 0x24) + scrollS;
                            out.v = at(0x10, 0x28) + scrollT;
                        }
                        else
                        {
                            out.u = x * 0.1f - offsetS;
                            out.v = z * 0.1f - offsetT;
                        }
                        uint32_t rgba = 0;
                        for (int c = 0; c < 4; ++c)
                        {
                            const float a = byte(0, c) + (extruded ? f(0, 0x2cu + static_cast<uint32_t>(c) * 4u) : 0.0f);
                            const float b = byte(1, c) + (extruded ? f(1, 0x2cu + static_cast<uint32_t>(c) * 4u) : 0.0f);
                            float value = a + (b - a) * t;
                            if (c == 3)
                                value *= fade;
                            rgba |= static_cast<uint32_t>(std::clamp(value, 0.0f, 255.0f)) << (c * 8);
                        }
                        out.rgba = rgba;
                        strip.push_back(out);
                    }
                }
                for (size_t i = 2; i < strip.size(); ++i)
                    triangles.insert(triangles.end(), {strip[i - 2], strip[i - 1], strip[i]});
            }
            if (triangles.empty())
                return;
            bozo::render::Material material;
            material.depthTest = true;
            material.depthWrite = ((read64(rdram, ds + 0x120) >> 32) & 1u) == 0;
            material.blend = bozo::render::Material::Blend::Alpha;
            material.alphaTest = true;
            material.alphaFunc = bozo::render::Material::AlphaFunc::Greater;
            material.alphaRef = 0;
            material.textured = texture != nullptr;
            if (texture)
                material.tex0 = texture->key;
            if (env("TTA_MIST_NODEPTH"))
                material.depthTest = material.depthWrite = false;
            if (env("TTA_MIST_TRACE") && frame(rdram) % 120 == 0)
            {
                uint32_t lo = 255, hi = 0;
                for (const auto &v : triangles)
                {
                    lo = std::min(lo, v.rgba >> 24);
                    hi = std::max(hi, v.rgba >> 24);
                }
                std::printf("[tta:mist] frame %u: alpha %u..%u, first rgba %08x uv %g %g, texture %s %ux%u\n", frame(rdram), lo, hi,
                            triangles.empty() ? 0u : triangles[0].rgba, triangles.empty() ? 0.0f : triangles[0].u,
                            triangles.empty() ? 0.0f : triangles[0].v, texture ? "found" : "missing", texture ? texture->width : 0u,
                            texture ? texture->height : 0u);
            }
            if (env("TTA_MIST_SHOW")) // debugging: the sheets in flat magenta over everything
            {
                material.textured = false;
                material.depthTest = false;
                for (auto &v : triangles)
                    v.rgba = 0x80ff00ffu;
            }
            clipToDepthRange(triangles, matrix);
            if (!triangles.empty())
                bozo::render::Renderer::instance().draw(material, matrix, triangles.data(), static_cast<uint32_t>(triangles.size()),
                                                        material.textured ? texture : nullptr);
            if (traceLevel() >= 2)
            {
                static uint32_t last = 0;
                if (frame(rdram) / 60 != last / 60)
                    std::printf("[tta:mist] frame %u: geometry %x, %u edges x %u rows, texture %s, %zu triangles\n", frame(rdram),
                                geometry, count, subdivisions + 1, texture ? "found" : "missing", triangles.size() / 3);
                last = frame(rdram);
            }
        }

        void mistAnimator(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            // a0 is the layer's output buffer; the mist's geometry is on the object (a1).
            const uint32_t geometry = ctx->r[5].uw[0], time = ctx->r[6].uw[0], mask = ctx->r[7].uw[0];
            const bool interpolatedSt = ctx->r[8].uw[0] != 0;
            const float scrollS = ctx->f[12], scrollT = ctx->f[13], scale = ctx->f[14];
            if (traceLevel() >= 2)
            {
                static int calls = 0;
                if (calls++ < 12)
                    std::printf("[tta:mist] animator frame %u: geometry %x object %x time %u mask %u st %d layer2 %u scroll %g %g scale %g"
                                " (n %u, %u edges)\n",
                                frame(rdram), geometry, ctx->r[5].uw[0], time, mask, interpolatedSt ? 1 : 0, ctx->r[10].uw[0], scrollS,
                                scrollT, scale, geometry ? rdram[(geometry + 9) & kMask] : 0u, geometry ? read16(rdram, geometry + 0x14) : 0u);
            }
            if (!env("TTA_NO_MIST") && mask == 0 && geometry)
            {
                const uint32_t ds = read32(rdram, 0x36f850), fbp = read32(rdram, ds + 0x150) & 0x1ffu;
                if (fbp == 0 || fbp == 0x70)
                    drawMist(rdram, geometry, time, interpolatedSt, scrollS, scrollT, scale > 0.0f ? std::fmin(scale, 1.0f) : 1.0f);
            }
            func_0x2236a0(rdram, ctx, rt);
            ctx->pc = ra;
        }

        // 0x2255d0: the type-18 render (PRMODE 0x40 flat untextured, template 0x377a10) on a skinned node.
        void type18(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            if (env("TTA_TYPE18_DEBUG"))
            {
                static uint32_t n = 0;
                if (n++ < 40)
                {
                    const uint32_t ds = read32(rdram, 0x36f850);
                    {
                        std::string type;
                        uint32_t index = 0, offset = 0, size = 0;
                        const uint32_t rec = read32(rdram, ctx->r[4].uw[0] - 0x28);
                        const bool ok = resourceAt(rec, type, index, offset, size);
                        const auto mesh = ok ? meshFor(type, index, size) : nullptr;
                        const auto it = mesh ? mesh->byOffset.find(offset) : decltype(mesh->byOffset)::const_iterator{};
                        const uint32_t g = read32(rdram, 0x403a60);
                        std::printf("[tta:t18] node %s %u +%x mesh %d kind %d G %x fade %x bones %x W %x\n", type.c_str(), index, offset,
                                    mesh != nullptr, mesh && it != mesh->byOffset.end() ? it->second.kind : -1, g,
                                    read32(rdram, g + 0x2bc), read32(rdram, g + 0x290), read32(rdram, g + 0x294));
                    }
                    std::printf("[tta:t18] frame %u FRAME_1 %llx ZBUF_1 %llx SCISSOR %llx XYOFF %llx\n", frame(rdram),
                                static_cast<unsigned long long>(read64(rdram, ds + 0x150)),
                                static_cast<unsigned long long>(read64(rdram, ds + 0x120)),
                                static_cast<unsigned long long>(read64(rdram, ds + 0x110)),
                                static_cast<unsigned long long>(read64(rdram, ds + 0x100)));
                }
                g_flatDebug = true;
                drawSkin(rdram, ctx->r[4].uw[0] - 0x28);
                g_flatDebug = false;
            }
            if (env("TTA_SHADOW_ORDER"))
                std::printf("[tta:shadow] frame %u: type-18 pass after %u nodes drawn\n", frame(rdram), g_drawn);
            if (!env("TTA_NO_SHADOWS"))
            {
                g_shadowWorld.clear();
                g_shadowPass = true;
                drawSkin(rdram, ctx->r[4].uw[0] - 0x28);
                g_shadowPass = false;
                drawShadowVolume(rdram);
            }
            func_0x2255d0(rdram, ctx, rt);
            ctx->pc = ra;
        }

        void skin(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            ++(g_inVisualEntity ? g_skinInside : g_skinOutside);
            if (traceLevel() >= 1 && frame(rdram) % 120 == 0 && (g_skinInside + g_skinOutside) % 50 == 1)
                std::printf("[tta:3d] skins drawn inside VisualEntity %u, outside %u\n", g_skinInside, g_skinOutside);
            const uint32_t ra = ctx->r[31].uw[0];
            const char *only = env("TTA_SKIN_ONLY");
            if (!env("TTA_NO_SKIN") && (!only || std::atoi(only) == g_lastSetter))
                drawSkin(rdram, ctx->r[4].uw[0] - 0x28);
            func_0x225fb0(rdram, ctx, rt);
            ctx->pc = ra;
        }
    }

    void installThreeD(bozo::port::Registry &registry)
    {
        registry.registerFunction(0x225fb0, skin);
        registry.registerFunction(0x21d438, vuBlock);
        registry.registerFunction(0x1b1b18, visualEntity);
        registry.registerFunction(0x225920, setSkinContext);
        registry.registerFunction(0x2251b0, setSkinContextChr);
        registry.registerFunction(0x137800, animate);
        registry.registerFunction(0x13ea30, sample);
        registry.registerFunction(0x13a260, skeletonRecord);
        registry.registerFunction(0x2204f0, staticRender<0x2204f0, func_0x2204f0>);
        registry.registerFunction(0x21fd30, staticRender<0x21fd30, func_0x21fd30>);
        registry.registerFunction(0x2201a8, staticRender<0x2201a8, func_0x2201a8>);
        registry.registerFunction(0x221630, staticRender<0x221630, func_0x221630>);
        registry.registerFunction(0x221760, staticRender<0x221760, func_0x221760>);
        registry.registerFunction(0x21f010, postProbe<0x21f010, func_0x21f010>);
        registry.registerFunction(0x21f088, staticRender<0x21f088, func_0x21f088>);
        registry.registerFunction(0x21f980, glowRender);
        registry.registerFunction(0x2163c8, dmaTag);
        registry.registerFunction(0x21fce8, staticRender<0x21fce8, func_0x21fce8>);
        registry.registerFunction(0x21c098, postProbe<0x21c098, func_0x21c098>);
        registry.registerFunction(0x21c420, postProbe<0x21c420, func_0x21c420>);
        registry.registerFunction(0x2207a8, staticRender<0x2207a8, func_0x2207a8>);
        registry.registerFunction(0x21ffe8, staticRender<0x21ffe8, func_0x21ffe8>);
        registry.registerFunction(0x2205c0, staticRender<0x2205c0, func_0x2205c0>);
        registry.registerFunction(0x21fe00, staticRender<0x21fe00, func_0x21fe00>);
        registry.registerFunction(0x220330, staticRender<0x220330, func_0x220330>);
        registry.registerFunction(0x21ea08, staticRender<0x21ea08, func_0x21ea08>);
        registry.registerFunction(0x21ea10, staticRender<0x21ea10, func_0x21ea10>);
        registry.registerFunction(0x21ea18, staticRender<0x21ea18, func_0x21ea18>);
        registry.registerFunction(0x21ea20, staticRender<0x21ea20, func_0x21ea20>);
        registry.registerFunction(0x21e5e8, staticRender<0x21e5e8, func_0x21e5e8>);
        registry.registerFunction(0x21e638, staticRender<0x21e638, func_0x21e638>);
        registry.registerFunction(0x221928, staticRender<0x221928, func_0x221928>);
        registry.registerFunction(0x221a10, staticRender<0x221a10, func_0x221a10>);
        registry.registerFunction(0x221a18, staticRender<0x221a18, func_0x221a18>);
        registry.registerFunction(0x2255d0, type18);
        registry.registerFunction(0x21a778, postProbe<0x21a778, func_0x21a778>);
        registry.registerFunction(0x21aa88, postProbe<0x21aa88, func_0x21aa88>);
        registry.registerFunction(0x21ae10, postProbe<0x21ae10, func_0x21ae10>);
        registry.registerFunction(0x21b0f8, postProbe<0x21b0f8, func_0x21b0f8>);
        registry.registerFunction(0x211b10, postProbe<0x211b10, func_0x211b10>);
        registry.registerFunction(0x2132c8, staticRender<0x2132c8, func_0x2132c8>);
        registry.registerFunction(0x21d9f8, staticRender<0x21d9f8, func_0x21d9f8>);
        registry.registerFunction(0x1bdbe8, render);
        registry.registerFunction(0x221178, terrain);
        registry.registerFunction(0x2221b8, water);
        registry.registerFunction(0x2236a0, mistAnimator);
    }
}
