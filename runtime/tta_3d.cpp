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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
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
                const char *e = std::getenv("TTA_3D_TRACE");
                return e && *e ? std::atoi(e) : 0;
            }();
            return level;
        }

        uint32_t frame(const uint8_t *rdram) { return read32(rdram, 0x3b5720); }

        std::string meshDir()
        {
            const char *e = std::getenv("TTA_MESH_DIR");
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
        };

        struct Node
        {
            int8_t shader = 0;
            uint8_t kind = 0; // 0 own batches, 1 a type-21 instance of `master`'s, 2 not converted
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
                    std::printf("[tta:3d] frame %u: %u nodes drawn, %u without a mesh, %u not converted\n", g_frameSeen,
                                g_drawn, g_missing, g_unconverted);
                g_frameSeen = frame(rdram);
                g_drawn = g_missing = g_unconverted = g_offscreen = 0;
            }
            ++which;
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
            // The game's projection gives w < 0 in front of the camera (its VU programs divide by it all the same); the
            // PC clips w < 0 away. -M gives the same x / w, y / w, z / w with w > 0.
            for (float &f : matrix)
                f = -f;
            auto &renderer = bozo::render::Renderer::instance();
            std::vector<bozo::render::Vertex> triangles;
            for (const Batch &batch : source->batches)
            {
                const uint32_t type = batch.prim & 7u;
                if (type < 3 || type > 5 || batch.vertices.size() < 3)
                    continue;
                std::vector<bozo::render::Vertex> v = batch.vertices;
                for (size_t i = 0; i < v.size(); ++i)
                {
                    const uint32_t c = batch.colours[i];
                    v[i].rgba = batch.colourMode == 1 ? (node.colours.empty() ? 0x80808080u : node.colours[c & 0xffu]) : c;
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
                material.depthWrite = (batch.prim & 0x40u) == 0;
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
                material.textured = texture != nullptr;
                if (texture)
                    material.tex0 = texture->key;
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
                else if (it->second.kind == 2)
                    count(rdram, g_unconverted);
                else
                {
                    // RC+0x2e0: object (RC+0x20) -> GS screen, the 2048 centre and Z range in it (= RC+0x1a0, world ->
                    // screen, while RC+0x20 is the identity: terrain). A type-21 node's instance matrix comes on top.
                    const uint32_t rc = read32(rdram, read32(rdram, 0x36f350));
                    float screen[16];
                    for (int i = 0; i < 16; ++i)
                        screen[i] = readFloat(rdram, rc + 0x2e0 + i * 4);
                    drawNode(rdram, *m, it->second, screen);
                    count(rdram, g_drawn);
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
            if (rdram[(node + 0x58) & kMask] != 0 && rdram[(read32(rdram, node) + 0x88) & kMask] != 21)
                drawFor(rdram, node);
            func_0x1bdbe8(rdram, ctx, rt);
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

    void installThreeD(bozo::port::Registry &registry)
    {
        registry.registerFunction(0x2204f0, staticRender<0x2204f0, func_0x2204f0>);
        registry.registerFunction(0x21fd30, staticRender<0x21fd30, func_0x21fd30>);
        registry.registerFunction(0x2201a8, staticRender<0x2201a8, func_0x2201a8>);
        registry.registerFunction(0x221630, staticRender<0x221630, func_0x221630>);
        registry.registerFunction(0x2207a8, staticRender<0x2207a8, func_0x2207a8>);
        registry.registerFunction(0x21ffe8, staticRender<0x21ffe8, func_0x21ffe8>);
        registry.registerFunction(0x2205c0, staticRender<0x2205c0, func_0x2205c0>);
        registry.registerFunction(0x21fe00, staticRender<0x21fe00, func_0x21fe00>);
        registry.registerFunction(0x220330, staticRender<0x220330, func_0x220330>);
        registry.registerFunction(0x1bdbe8, render);
        registry.registerFunction(0x221178, terrain);
    }
}
