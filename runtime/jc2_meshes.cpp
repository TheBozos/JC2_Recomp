// Jade Cocoon 2's 3D models drawn natively (BozoEngine's mesh pack and renderer): no GS, no VIF or GIF packet read.
//
// The models are CDDATA resources whose '0IVX' sub-files hold the meshes as prebuilt VIF chains; tools/jc2_mesh_pack.py
// converts them offline from the player's disc into build/.../jc2_meshes.pack (docs/RECOMP_NOTES.md, "3D models").
// While the game runs:
// - Which resource is where: the loader's request (0x15C390: desc +4 the resource id, +8 its destination) and the
//   LOC table give (id, base, size). A mesh's chain address then resolves to (resource, model sub-file, offset), the
//   pack's key, without reading the chain.
// - What is drawn: the game's builders decide (culling, visibility, two-pass materials) and call, per part, a material
//   emitter (static 0x149CE0(packet, material), skinned 0x14BC00(instance, packet, material)) and then a mesh call
//   (static 0x14AA30(packet, submesh), skinned 0x14C450(instance, packet, submesh record)). Each hook notes its
//   arguments and runs the game's own routine (its packets go nowhere: there is no GS); the mesh call builds the draw.
// - The maths of VU1 programs B (static) and A (skinned) on the CPU, from EE memory: the builder's context (its
//   argument: +0 camera, +4 light set, +0x10 the node's world matrix W), the camera (+0x130 view V, +0x170 projection
//   P, +0x2F0 clip, +0x380 winding/fog), the light set (+0xB0 colours and ambient, +0xF0 directions), the material
//   (+0 flags, +0x10 colour, +0x30 PRIM, +0x38 ALPHA, +0x48 TEST) and, for characters, the actor's bone palettes
//   (actor +0x1E0 set A, +0x1E4 set B, picked by the frame's parity *0x6CC670 & 1; 0x80 bytes a bone, already in
//   world space). Screen: P V W gives the GS's field coordinates (640 x 224 around 2048); the frame is 448 high.
#include "bozo/port.h"
#include "bozo/render/mesh_draw.h"
#include "bozo/render/mesh_pack.h"
#include "bozo/render/renderer.h"
#include "bozo/render/texture.h"
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef JC2_MESH_PACK
#define JC2_MESH_PACK "jc2_meshes.pack"
#endif

namespace jc2
{
    std::shared_ptr<const bozo::render::Texture> decodeTim2Picture(const uint8_t *header, uint32_t palette, uint64_t key);

    namespace
    {
        namespace packs = bozo::render::packs;

        Runtime *g_rt = nullptr;
        const bool traceLoad = std::getenv("JC2_TRACE_LOAD") != nullptr;
        const bool meshesOff = [] {
            const char *v = std::getenv("JC2_NATIVE_MESHES");
            return v && v[0] == '0';
        }();

        const uint8_t *at(const uint8_t *ram, uint32_t address)
        {
            if ((address >> 28) == 7u)
                return bozo::port::scratchpad(g_rt) + (address & 0x3FFFu);
            return ram + (address & 0x1FFFFFFu);
        }
        uint32_t r32(const uint8_t *ram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, at(ram, address & ~3u), 4);
            return v;
        }
        uint64_t r64(const uint8_t *ram, uint32_t address)
        {
            uint64_t v;
            std::memcpy(&v, at(ram, address & ~7u), 8);
            return v;
        }
        float rf(const uint8_t *ram, uint32_t address)
        {
            float v;
            std::memcpy(&v, at(ram, address & ~3u), 4);
            return v;
        }

        // ---- 4 x 4 matrices as VU1 keeps them: four column vectors, m[4 * column + row] ----
        struct Mat
        {
            float m[16];
        };
        Mat readMat(const uint8_t *ram, uint32_t address)
        {
            Mat r;
            std::memcpy(r.m, at(ram, address), 64);
            return r;
        }
        Mat mul(const Mat &a, const Mat &b)
        {
            Mat r;
            for (int c = 0; c < 4; ++c)
                for (int row = 0; row < 4; ++row)
                {
                    float s = 0;
                    for (int k = 0; k < 4; ++k)
                        s += a.m[4 * k + row] * b.m[4 * c + k];
                    r.m[4 * c + row] = s;
                }
            return r;
        }
        void apply(const Mat &a, const float v[4], float out[4])
        {
            for (int row = 0; row < 4; ++row)
                out[row] = a.m[row] * v[0] + a.m[4 + row] * v[1] + a.m[8 + row] * v[2] + a.m[12 + row] * v[3];
        }
        // The GS's field (y around 2048, 224 high) to the frame (448 high): y' = 2 y - 2048 w.
        Mat fieldToFrame()
        {
            Mat s{};
            s.m[0] = 1, s.m[5] = 2, s.m[10] = 1, s.m[15] = 1;
            s.m[13] = -2048.0f;
            return s;
        }

        // ---- The pack and the loaded resources ----
        packs::MeshPack g_pack;
        bool g_packTried = false;
        struct SubFile
        {
            uint32_t offset, size, file;
        };
        std::map<uint32_t, std::vector<SubFile>> g_resources; // resource id -> its model sub-files
        struct Loaded
        {
            uint32_t size, id;
        };
        std::map<uint32_t, Loaded> g_loaded; // base -> resource

        bool pack()
        {
            if (g_packTried)
                return g_pack.loaded();
            g_packTried = true;
            const char *env = std::getenv("JC2_MESH_PACK");
            const std::string path = env && *env ? env : JC2_MESH_PACK;
            if (!g_pack.load(path))
            {
                std::fprintf(stderr, "[jc2:meshes] no mesh pack (%s): %s; tools/jc2_mesh_pack.py makes it\n", path.c_str(),
                             g_pack.error().c_str());
                return false;
            }
            if (const packs::MeshFile *index = g_pack.file("RESOURCES"))
                if (!index->objects.empty())
                    if (const packs::MeshSection *s = index->objects[0].section(packs::fourcc("RES ")))
                        for (uint32_t k = 0; k + 16 <= s->size; k += 16)
                        {
                            uint32_t e[4];
                            std::memcpy(e, s->data + k, 16);
                            g_resources[e[0]].push_back({e[1], e[2], e[3]});
                        }
            std::printf("[jc2:meshes] pack %s: %zu files, %zu objects, %zu model resources\n", path.c_str(),
                        g_pack.files().size(), g_pack.objectCount(), g_resources.size());
            return true;
        }

        // The game's LOC table (CDDATA.LOC as loaded; *0x6C47BC its entries: +0 sector, +4 bytes).
        uint32_t resourceSize(const uint8_t *ram, uint32_t id)
        {
            const uint32_t table = r32(ram, 0x006C47BCu);
            return table ? r32(ram, table + id * 16u + 4u) : 0u;
        }

        // A request names its resource by disc sector (+4: the LOC entry's sector + CDDATA.000's own, *0x866860); the id
        // is the LOC entry with that sector (the table's 0x1481 entries, read once the game has it).
        constexpr uint32_t kCddataSector = 0x00866860u;
        std::map<uint32_t, uint32_t> g_bySector;
        uint32_t resourceAtSector(const uint8_t *ram, uint32_t sector)
        {
            const uint32_t table = r32(ram, 0x006C47BCu);
            if (g_bySector.empty() && table)
                for (uint32_t id = 0; id < 0x1481u; ++id)
                    if (r32(ram, table + id * 16u + 4u))
                        g_bySector.emplace(r32(ram, table + id * 16u), id); // the first id of a sector (copies share)
            auto it = g_bySector.find(sector);
            return it == g_bySector.end() ? ~0u : it->second;
        }

        void noteLoaded(const uint8_t *ram, uint32_t id, uint32_t base)
        {
            base &= 0x1FFFFFFu;
            if (id >= 0x1481u)
                return;
            const uint32_t size = resourceSize(ram, id);
            if (!base || !size)
                return;
            // A new resource replaces whatever the game had where it lands.
            auto it = g_loaded.lower_bound(base);
            if (it != g_loaded.begin())
            {
                auto prev = std::prev(it);
                if (prev->first + prev->second.size > base)
                    it = prev;
            }
            while (it != g_loaded.end() && it->first < base + size)
                it = g_loaded.erase(it);
            g_loaded[base] = {size, id};
        }

        // A chain address (a submesh + 0x30) to its pack object.
        const packs::MeshObject *lookup(uint32_t chain, uint32_t &resource)
        {
            chain &= 0x1FFFFFFu;
            auto it = g_loaded.upper_bound(chain);
            if (it == g_loaded.begin())
                return nullptr;
            --it;
            const uint32_t offset = chain - it->first;
            if (offset >= it->second.size)
                return nullptr;
            resource = it->second.id;
            auto found = g_resources.find(it->second.id);
            if (found == g_resources.end())
                return nullptr;
            for (const SubFile &s : found->second)
                if (offset >= s.offset && offset < s.offset + s.size && s.file < g_pack.files().size())
                    return g_pack.files()[s.file].object(offset - s.offset);
            return nullptr;
        }

        // 0x15C390(desc): a resource read queued (desc +0 flags, +4 resource id, +8 destination).
        constexpr uint32_t kResourceRequest = 0x0015C390u;
        void resourceRequest(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t desc = ctx->r[4].uw[0];
            const uint32_t id = r32(ram, desc + 4), dest = r32(ram, desc + 8);
            if (traceLoad)
                std::printf("[jc2:load] id %u flags %#x +8 %#x +c %#x +10 %#x +14 %#x +18 %#x +1c %#x ra %#x\n", id,
                            r32(ram, desc), dest, r32(ram, desc + 0xC), r32(ram, desc + 0x10), r32(ram, desc + 0x14),
                            r32(ram, desc + 0x18), r32(ram, desc + 0x1C), ctx->r[31].uw[0]);
            (void)dest;
            rt->functions.generated(kResourceRequest)(ram, ctx, rt);
        }

        // 0x150340(request): the CD thread's request done. Its state (0x6CC770 + 0x201C: +0 the request, +0x3C bytes,
        // +0x44 the offset in the first sector, +0x48 the buffer) still says where the data went; the request: +0 flags
        // (0x80: a part of the resource, from +0x14), +0x2C the resource id, +0x30 bit 0x10 failed.
        constexpr uint32_t kRequestDone = 0x00150340u;
        constexpr uint32_t kCdState = 0x006CC770u + 0x201Cu;
        void requestDone(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t request = ctx->r[4].uw[0];
            if (request && r32(ram, kCdState) == request && !(r32(ram, request + 0x30) & 0x10u))
            {
                const uint32_t flags = r32(ram, request);
                const uint32_t id = resourceAtSector(ram, r32(ram, request + 4) - r32(ram, kCddataSector));
                const uint32_t data = r32(ram, kCdState + 0x48) + r32(ram, kCdState + 0x44);
                const uint32_t part = (flags & 0x80u) ? r32(ram, request + 0x14) : 0u;
                if (traceLoad)
                    std::printf("[jc2:loaded] id %u at %#x (%u bytes, part %#x, flags %#x)\n", id, data,
                                r32(ram, kCdState + 0x3C), part, flags);
                if (part == 0)
                    noteLoaded(ram, id, data);
            }
            rt->functions.generated(kRequestDone)(ram, ctx, rt);
        }

        // ---- Textures ----
        // A material's texture: bank +0xC, slot +0xD, index +0xE (u16). Bank table 0x6CC5F0, 52 bytes a bank: +0x18
        // the slots' mask, +0x1C + 12 slot {TIM2, count, records}; record = records + 0x60 index: +0x10 the picture's
        // place in its GS page (x low half, y high half), +0x14 its size (w low, h high), +0x20 TEX0 (the page: TW, TH).
        // The picture is the slot's TIM2's picture `index`. A slot registered without its TIM2 (a room's: uploaded at
        // load) is matched to a TIM2 sub-file of a loaded resource with the same pictures (count and first size).
        constexpr uint32_t kTextureBanks = 0x006CC5F0u;
        struct TextureView
        {
            std::shared_ptr<const bozo::render::Texture> texture;
            float su = 1, ou = 0, sv = 1, ov = 0; // picture u = s su + ou, v = t sv + ov
            uint64_t key = 0;
        };
        std::map<uint32_t, TextureView> g_textures; // by record address; cleared when a slot is registered
        uint64_t g_textureGeneration = 1;

        // The TIM2's picture headers.
        std::vector<uint32_t> tim2Pictures(const uint8_t *ram, uint32_t tim2)
        {
            std::vector<uint32_t> out;
            if (!tim2 || std::memcmp(at(ram, tim2), "TIM2", 4) != 0)
                return out;
            const uint8_t *h = at(ram, tim2);
            const uint32_t count = h[6] | h[7] << 8;
            uint32_t p = tim2 + (h[5] ? 0x80u : 0x10u);
            for (uint32_t k = 0; k < count && k < 4096u; ++k)
            {
                out.push_back(p);
                const uint32_t total = r32(ram, p);
                if (!total)
                    break;
                p += total;
            }
            return out;
        }

        // A TIM2 sub-file of a loaded resource whose pictures match a slot's records (count, first picture's size).
        uint32_t findTim2(const uint8_t *ram, uint32_t count, uint32_t records)
        {
            const uint32_t size0 = r32(ram, records + 0x14);
            for (const auto &[base, loaded] : g_loaded)
            {
                const uint32_t n = r32(ram, base);
                if (!n || n > 64u)
                    continue;
                for (uint32_t i = 0; i < n; ++i)
                {
                    const uint32_t sub = base + r32(ram, base + 4 + 4 * i);
                    if (sub >= base + loaded.size || std::memcmp(at(ram, sub), "TIM2", 4) != 0)
                        continue;
                    const uint8_t *h = at(ram, sub);
                    if ((uint32_t(h[6]) | uint32_t(h[7]) << 8) != count)
                        continue;
                    const uint32_t p0 = sub + (h[5] ? 0x80u : 0x10u);
                    const uint32_t w = r32(ram, p0 + 20) & 0xFFFFu, hh = r32(ram, p0 + 20) >> 16;
                    if ((hh << 16 | w) == size0)
                        return sub;
                }
            }
            return 0;
        }

        const TextureView *materialTexture(const uint8_t *ram, const uint8_t *material);

        // 0x13DAE0(bank, slot, count, records, tim2): a texture slot registered.
        constexpr uint32_t kTextureSlot = 0x0013DAE0u;
        void textureSlot(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            g_textures.clear();
            ++g_textureGeneration;
            if (traceLoad)
            {
                const uint32_t records = ctx->r[7].uw[0];
                std::printf("[jc2:texslot] bank %u slot %u count %u records %#x tim2 %#x ra %#x rec0 %08x %08x %08x %08x "
                            "%08x %08x tex0 %08x%08x\n",
                            ctx->r[4].uw[0], ctx->r[5].uw[0], ctx->r[6].uw[0], records, ctx->r[8].uw[0], ctx->r[31].uw[0],
                            r32(ram, records), r32(ram, records + 4), r32(ram, records + 8), r32(ram, records + 0xC),
                            r32(ram, records + 0x10), r32(ram, records + 0x14), r32(ram, records + 0x24),
                            r32(ram, records + 0x20));
            }
            rt->functions.generated(kTextureSlot)(ram, ctx, rt);
        }

        const TextureView *materialTexture(const uint8_t *ram, const uint8_t *material)
        {
            const uint32_t bank = material[0xC], slot = material[0xD], index = material[0xE] | material[0xF] << 8;
            if (bank > 3u || slot > 3u)
                return nullptr;
            const uint32_t entry = kTextureBanks + bank * 52u;
            if (!(r32(ram, entry + 0x18) & (1u << slot)))
                return nullptr;
            const uint32_t slotAt = entry + 0x1C + slot * 12u;
            uint32_t tim2 = r32(ram, slotAt);
            const uint32_t count = r32(ram, slotAt + 4), records = r32(ram, slotAt + 8);
            if (index >= count || !records)
                return nullptr;
            const uint32_t record = records + index * 0x60u;
            auto found = g_textures.find(record);
            if (found != g_textures.end())
                return found->second.texture ? &found->second : nullptr;
            TextureView &view = g_textures[record];
            if (!tim2)
                tim2 = findTim2(ram, count, records);
            const std::vector<uint32_t> pictures = tim2Pictures(ram, tim2);
            if (index >= pictures.size())
            {
                static int warned = 0;
                if (warned++ < 8)
                    std::fprintf(stderr, "[jc2:meshes] texture bank %u slot %u index %u: no TIM2 picture (tim2 %#x)\n", bank,
                                 slot, index, tim2);
                return nullptr;
            }
            view.key = 0x3D00000000000000ull | (g_textureGeneration << 32) | record;
            view.texture = decodeTim2Picture(at(ram, pictures[index]), 0, view.key);
            if (!view.texture)
                return nullptr;
            const uint64_t tex0 = r64(ram, record + 0x20);
            const float pw = float(1u << ((tex0 >> 26) & 15u)), ph = float(1u << ((tex0 >> 30) & 15u));
            const uint32_t place = r32(ram, record + 0x10);
            const float w = float(view.texture->width), h = float(view.texture->height);
            view.su = pw / w, view.ou = -float(place & 0xFFFFu) / w;
            view.sv = ph / h, view.ov = -float(place >> 16) / h;
            return &view;
        }

        // ---- The draws ----
        constexpr uint32_t kStaticBuilder = 0x00149350u;  // (context, ...): one node's parts
        constexpr uint32_t kStaticMaterial = 0x00149CE0u; // (packet, material, ...)
        constexpr uint32_t kStaticCall = 0x0014AA30u;     // (packet, submesh)
        constexpr uint32_t kSkinnedBuilder = 0x0014B230u; // (instance, context)
        constexpr uint32_t kSkinnedMaterial = 0x0014BC00u; // (instance, packet, material copy, ...)
        constexpr uint32_t kSkinnedCall = 0x0014C450u;    // (instance, packet, submesh record: +0x30 the chain)
        constexpr uint32_t kFrameParity = 0x006CC670u;    // bit 0: which bone palette set the frame uses
        constexpr uint32_t kActors = 0x01C5ECE0u;         // the actors: 0x600 bytes each, +0 the instance
        constexpr uint32_t kActorStride = 0x600u, kActorCount = 64u;

        uint32_t g_context = 0, g_material = 0;
        uint8_t g_materialCopy[0x60]; // the skinned material: the builder passes a copy on its stack
        bool g_materialIsCopy = false;
        std::vector<bozo::render::MeshDraw> g_opaque, g_blended;
        uint64_t g_drawn = 0, g_missing = 0, g_frames = 0;

        void staticBuilder(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            g_context = ctx->r[4].uw[0];
            rt->functions.generated(kStaticBuilder)(ram, ctx, rt);
        }
        void skinnedBuilder(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            g_context = ctx->r[5].uw[0];
            rt->functions.generated(kSkinnedBuilder)(ram, ctx, rt);
        }
        void staticMaterial(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            g_material = ctx->r[5].uw[0];
            g_materialIsCopy = false;
            rt->functions.generated(kStaticMaterial)(ram, ctx, rt);
        }
        void skinnedMaterial(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            std::memcpy(g_materialCopy, at(ram, ctx->r[6].uw[0]), sizeof g_materialCopy);
            g_materialIsCopy = true;
            rt->functions.generated(kSkinnedMaterial)(ram, ctx, rt);
        }

        struct MaterialView
        {
            uint32_t flags = 0;
            float colour[4] = {1, 1, 1, 1};
            uint64_t prim = 0, alpha = 0, test = 0;
            const uint8_t *bytes = nullptr;
        };
        MaterialView currentMaterial(const uint8_t *ram)
        {
            MaterialView m;
            const uint8_t *p = g_materialIsCopy ? g_materialCopy : at(ram, g_material);
            m.bytes = p;
            std::memcpy(&m.flags, p, 4);
            std::memcpy(m.colour, p + 0x10, 16);
            std::memcpy(&m.prim, p + 0x30, 8);
            std::memcpy(&m.alpha, p + 0x38, 8);
            std::memcpy(&m.test, p + 0x48, 8);
            return m;
        }

        // The light set: colours C0..C2 and ambient C3 (+0xB0), directions L (+0xF0, columns).
        struct Lights
        {
            Mat colours, directions;
        };

        // Program A's / B's colour: three directional lights and ambient, times the material colour, as 0x80 = 1.0.
        uint32_t litColour(const Lights &l, const MaterialView &m, const float n[3], bool unlit)
        {
            float col[4];
            if (unlit)
                col[0] = col[1] = col[2] = col[3] = 1.0f;
            else
            {
                float d[3];
                for (int k = 0; k < 3; ++k)
                    d[k] = std::max(0.0f, l.directions.m[k] * n[0] + l.directions.m[4 + k] * n[1] + l.directions.m[8 + k] * n[2]);
                for (int c = 0; c < 4; ++c)
                    col[c] = l.colours.m[c] * d[0] + l.colours.m[4 + c] * d[1] + l.colours.m[8 + c] * d[2] + l.colours.m[12 + c];
            }
            uint32_t rgba = 0;
            for (int c = 0; c < 4; ++c)
            {
                const float v = std::min(1.0f, std::min(1.0f, col[c]) * m.colour[c]);
                rgba |= static_cast<uint32_t>(std::clamp(v * 128.0f, 0.0f, 255.0f) + 0.5f) << (8 * c);
            }
            return rgba;
        }

        // The actor whose instance this is (its bone palettes).
        uint32_t actorOf(const uint8_t *ram, uint32_t instance)
        {
            for (uint32_t k = 0; k < kActorCount; ++k)
            {
                const uint32_t actor = kActors + k * kActorStride;
                if (r32(ram, actor) == instance)
                    return actor;
            }
            return 0;
        }

        void buildDraw(const uint8_t *ram, uint32_t chain, bool skinned, uint32_t instance)
        {
            if (meshesOff || !pack() || !g_context)
                return;
            uint32_t resource = 0;
            const packs::MeshObject *object = lookup(chain, resource);
            if (!object)
            {
                if (g_missing++ < 16)
                    std::fprintf(stderr, "[jc2:meshes] %s mesh at %#x (resource %u): not in the pack\n",
                                 skinned ? "skinned" : "static", chain, resource);
                return;
            }
            const uint32_t camera = r32(ram, g_context), lightSet = r32(ram, g_context + 4);
            if (!camera || !lightSet)
                return;
            const MaterialView material = currentMaterial(ram);
            const Mat view = readMat(ram, camera + 0x130), projection = readMat(ram, camera + 0x170);
            const Mat clipM = readMat(ram, camera + 0x2F0);
            const float windX = rf(ram, camera + 0x380), windY = rf(ram, camera + 0x384);
            Lights lights{readMat(ram, lightSet + 0xB0), readMat(ram, lightSet + 0xF0)};
            const uint32_t programFlags = material.flags ^ 8u; // VU 53.x: bit 0 unlit, bit 3 cull back faces
            const bool unlit = programFlags & 1u, cull = programFlags & 8u;

            Mat world{};
            if (skinned)
                world.m[0] = world.m[5] = world.m[10] = world.m[15] = 1.0f; // the palettes are in world space
            else
                world = readMat(ram, g_context + 0x10);
            const Mat pv = mul(projection, view);
            const Mat screenM = mul(fieldToFrame(), mul(pv, world));

            // Skinned: the bone palette the frame uses.
            uint32_t palette = 0;
            if (skinned)
            {
                const uint32_t actor = actorOf(ram, instance);
                if (!actor)
                    return;
                palette = r32(ram, actor + ((r32(ram, kFrameParity) & 1u) ? 0x1E4u : 0x1E0u));
                if (!palette)
                    return;
            }

            bozo::render::MeshDraw draw;
            std::memcpy(draw.matrix, screenM.m, sizeof draw.matrix);
            const Mat clipObject = mul(clipM, mul(pv, world));
            std::memcpy(draw.clip, clipObject.m, sizeof draw.clip);
            draw.hasClip = true;
            draw.object = resource;
            draw.offset = chain;
            bozo::render::MeshBatch batch;
            const TextureView *texture = (material.flags & 0x20u) ? materialTexture(ram, material.bytes) : nullptr;
            batch.textured = texture != nullptr;
            if (texture)
            {
                batch.texture = texture->texture;
                batch.tex0 = texture->key;
                batch.setsTex0 = true;
            }
            batch.blended = (material.prim & 0x40u) != 0;
            batch.setsAlpha = batch.setsTest = true;
            batch.alpha = material.alpha;
            batch.test = material.test;

            const packs::MeshSection *nrmSection = object->section(packs::fourcc("NRM "));
            const packs::MeshSection *uvSection = object->section(packs::fourcc("UV  "));
            const float *positions = object->positions;
            Mat slotPos[4], slotNrm[4];
            for (auto &s : slotPos)
                std::memset(s.m, 0, sizeof s.m);
            for (auto &s : slotNrm)
                std::memset(s.m, 0, sizeof s.m);

            std::vector<bozo::render::Vertex> strip;
            std::vector<float> screen; // x, y, w per strip vertex (for the back-face test)
            packs::StripCursor cursor(*object);
            packs::StripHeader header;
            const uint8_t *vertices;
            while (cursor.next(header, vertices))
            {
                const uint32_t word = header.material;
                const bool negative = word & 1u, hasNrm = word & 4u, hasColour = word & 8u, rigid = word & 16u,
                           reuse = word & 32u;
                const uint32_t slots = rigid ? 1u : ((word >> 6) & 3u) + 1u;
                uint8_t bones[4];
                std::memcpy(bones, &header.slot, 4);
                if (skinned && !reuse)
                    for (uint32_t k = 0; k < slots; ++k)
                    {
                        const uint32_t b = palette + bones[k] * 0x80u;
                        Mat src = readMat(ram, b);
                        slotPos[k] = src;
                        slotPos[k].m[15] = 1.0f;
                        const float scale = src.m[15];
                        slotNrm[k] = src;
                        for (int i = 0; i < 12; ++i)
                            slotNrm[k].m[i] *= scale;
                    }
                strip.clear();
                screen.clear();
                for (uint32_t i = 0; i < header.vertexCount; ++i)
                {
                    packs::StripVertex sv;
                    std::memcpy(&sv, vertices + i * sizeof sv, sizeof sv);
                    const float *p = positions + 4u * sv.position;
                    float pos[4] = {p[0], p[1], p[2], 1.0f};
                    float n[3] = {0, 0, 0};
                    if (hasNrm && nrmSection && (sv.position + 1u) * 8u <= nrmSection->size)
                    {
                        int16_t raw[4];
                        std::memcpy(raw, nrmSection->data + sv.position * 8u, 8);
                        n[0] = raw[0] / 32768.0f, n[1] = raw[1] / 32768.0f, n[2] = raw[2] / 32768.0f;
                    }
                    bozo::render::Vertex v;
                    if (skinned)
                    {
                        float out[4] = {0, 0, 0, 0}, nn[3] = {0, 0, 0};
                        for (uint32_t k = 0; k < slots; ++k)
                        {
                            const float w = rigid ? (k == 0 ? 1.0f : 0.0f) : sv.rgba[k] / 255.0f;
                            if (w == 0.0f)
                                continue;
                            float pk[4], nk[4];
                            apply(slotPos[k], pos, pk);
                            const float n4[4] = {n[0], n[1], n[2], 0.0f};
                            apply(slotNrm[k], n4, nk);
                            for (int c = 0; c < 4; ++c)
                                out[c] += w * pk[c];
                            for (int c = 0; c < 3; ++c)
                                nn[c] += w * nk[c];
                        }
                        v.x = out[0], v.y = out[1], v.z = out[2];
                        v.rgba = litColour(lights, material, nn, unlit);
                    }
                    else
                    {
                        v.x = pos[0], v.y = pos[1], v.z = pos[2];
                        if (hasColour)
                        {
                            uint32_t rgba = 0;
                            for (int c = 0; c < 4; ++c)
                                rgba |= static_cast<uint32_t>(std::min<uint32_t>(sv.rgba[c], 128u)) << (8 * c);
                            v.rgba = rgba;
                        }
                        else
                        {
                            // Program B's lighting is not decoded yet: the normal in world space, lit as program A.
                            const float n4[4] = {n[0], n[1], n[2], 0.0f};
                            float wn[4];
                            apply(world, n4, wn);
                            const float len = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                            if (len > 1e-6f)
                                wn[0] /= len, wn[1] /= len, wn[2] /= len;
                            v.rgba = litColour(lights, material, wn, unlit || !hasNrm);
                        }
                    }
                    if (uvSection && (sv.position + 1u) * 8u <= uvSection->size)
                    {
                        std::memcpy(&v.u, uvSection->data + sv.position * 8u, 4);
                        std::memcpy(&v.v, uvSection->data + sv.position * 8u + 4, 4);
                        if (texture)
                            v.u = v.u * texture->su + texture->ou, v.v = v.v * texture->sv + texture->ov;
                    }
                    strip.push_back(v);
                    float s4[4];
                    const float o4[4] = {v.x, v.y, v.z, 1.0f};
                    apply(screenM, o4, s4);
                    screen.push_back(s4[0]);
                    screen.push_back(s4[1]);
                    screen.push_back(s4[3]);
                }
                // Triangles of the strip; the program's back-face test: the winding sign starts at |V29.y| x the
                // strip's sign and is multiplied by V29.x at each vertex.
                float wind = std::fabs(windY) * (negative ? -1.0f : 1.0f);
                for (uint32_t i = 0; i < strip.size(); ++i)
                {
                    if (i >= 2)
                    {
                        bool keep = true;
                        if (cull)
                        {
                            const float *a = &screen[3 * (i - 2)], *b = &screen[3 * (i - 1)], *c = &screen[3 * i];
                            if (a[2] > 0 && b[2] > 0 && c[2] > 0)
                            {
                                const float ax = a[0] / a[2], ay = a[1] / a[2], bx = b[0] / b[2], by = b[1] / b[2],
                                            cx = c[0] / c[2], cy = c[1] / c[2];
                                const float cross = (cx - ax) * (by - ay) - (bx - ax) * (cy - ay);
                                keep = cross * wind >= 0;
                            }
                        }
                        if (keep)
                        {
                            batch.vertices.push_back(strip[i - 2]);
                            batch.vertices.push_back(strip[i - 1]);
                            batch.vertices.push_back(strip[i]);
                        }
                    }
                    wind *= windX;
                }
            }
            if (batch.vertices.empty())
                return;
            draw.batches.push_back(std::move(batch));
            ++g_drawn;
            (draw.batches[0].blended ? g_blended : g_opaque).push_back(std::move(draw));
        }

        void staticCall(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            buildDraw(ram, ctx->r[5].uw[0] + 0x30u, false, 0);
            rt->functions.generated(kStaticCall)(ram, ctx, rt);
        }
        void skinnedCall(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            buildDraw(ram, r32(ram, ctx->r[6].uw[0] + 0x30u), true, ctx->r[4].uw[0]);
            rt->functions.generated(kSkinnedCall)(ram, ctx, rt);
        }

        // ---- Shadows: 0x177CF0(actor), a blob on the floor under a character ----
        // The game fills one record (actor +0x280) and draws it with its primitive drawer (0x146990, VU1 program C,
        // flags 8: a flat quad). Here the quad is made from the same actor fields: centre p = actor +0x190 + actor
        // +0x410 (y - 0.01), size s = +0x1B0, half sizes 0.45 / 0.35 (0.65 / 0.45 unless the mode word 0x8788D2 is
        // 10), turned by +0x1A4 around y; corners (+,+) (+,-) (-,+) (-,-) with UV (0,0) (0,1) (1,0) (1,1). Not drawn
        // when p is outside the camera's cull volume (+0x270). Texture: picture 0 of the TIM2 the game keeps at
        // G +0x470 (resource 0xB69, black with a radial alpha; G = *(**0x520C2C + 0x24)). GS state: ALPHA (0 - Cd) As + Cd
        // (the texture's colour is black: the plain alpha blend is the same), depth test GEQUAL, no depth write.
        constexpr uint32_t kShadow = 0x00177CF0u;
        constexpr uint32_t kShadowMode = 0x008788D2u;
        constexpr uint32_t kShadowGlobals = 0x00520C2Cu;
        uint64_t g_shadows = 0, g_shadowCalls = 0, g_shadowNoBit = 0, g_shadowCulled = 0, g_shadowNoTexture = 0;

        void shadow(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t actor = ctx->r[4].uw[0];
            const uint32_t camera = r32(ram, 0x006CC5F4u);
            ++g_shadowCalls;
            if (!(r32(ram, actor + 4) & 0x400000u))
                ++g_shadowNoBit;
            if (!meshesOff && camera && (r32(ram, actor + 4) & 0x400000u))
            {
                const uint32_t g = r32(ram, r32(ram, r32(ram, kShadowGlobals)) + 0x24u);
                const uint32_t tim2 = r32(ram, g + 0x470);
                const std::vector<uint32_t> pictures = tim2Pictures(ram, tim2);
                float p[4] = {rf(ram, actor + 0x190) + rf(ram, actor + 0x410), rf(ram, actor + 0x194) + rf(ram, actor + 0x414),
                              rf(ram, actor + 0x198) + rf(ram, actor + 0x418), 1.0f};
                p[1] -= 0.01f;
                const Mat cull = readMat(ram, camera + 0x270);
                float c[4];
                apply(cull, p, c);
                const bool inside = std::fabs(c[0]) <= std::fabs(c[3]) && std::fabs(c[1]) <= std::fabs(c[3]) &&
                                    std::fabs(c[2]) <= std::fabs(c[3]);
                g_shadowCulled += inside ? 0u : 1u;
                g_shadowNoTexture += pictures.empty() ? 1u : 0u;
                if (inside && !pictures.empty())
                {
                    const uint64_t key = 0x3E00000000000000ull | tim2;
                    auto texture = decodeTim2Picture(at(ram, pictures[0]), 0, key);
                    const float s = rf(ram, actor + 0x1B0), angle = rf(ram, actor + 0x1A4);
                    const bool mode10 = static_cast<int16_t>(r32(ram, kShadowMode & ~3u) >> 16) == 10;
                    const float hx = (mode10 ? 0.45f : 0.65f) * s, hz = (mode10 ? 0.35f : 0.45f) * s;
                    const float co = std::cos(angle), si = std::sin(angle);
                    auto corner = [&](float ox, float oz, float u, float v) {
                        bozo::render::Vertex out;
                        out.x = p[0] + co * ox + si * oz;
                        out.y = p[1];
                        out.z = p[2] - si * ox + co * oz;
                        out.u = u, out.v = v;
                        out.rgba = 0x80808080u;
                        return out;
                    };
                    const auto v0 = corner(hx, hz, 0, 0), v1 = corner(hx, -hz, 0, 1), v2 = corner(-hx, hz, 1, 0),
                               v3 = corner(-hx, -hz, 1, 1);
                    const Mat view = readMat(ram, camera + 0x130), projection = readMat(ram, camera + 0x170);
                    const Mat pv = mul(projection, view);
                    bozo::render::MeshDraw draw;
                    const Mat screenM = mul(fieldToFrame(), pv), clipM = mul(readMat(ram, camera + 0x2F0), pv);
                    std::memcpy(draw.matrix, screenM.m, sizeof draw.matrix);
                    std::memcpy(draw.clip, clipM.m, sizeof draw.clip);
                    draw.hasClip = true;
                    draw.object = actor;
                    bozo::render::MeshBatch batch;
                    batch.textured = texture != nullptr;
                    batch.texture = texture;
                    batch.tex0 = key;
                    batch.setsTex0 = true;
                    batch.blended = true;
                    batch.setsAlpha = batch.setsTest = batch.setsZbuf = true;
                    batch.alpha = 0x4000000046ull;
                    batch.test = 0x51001ull;
                    batch.depthMasked = true;
                    batch.vertices = {v0, v1, v2, v1, v2, v3};
                    draw.batches.push_back(std::move(batch));
                    g_blended.push_back(std::move(draw));
                    ++g_shadows;
                }
            }
            rt->functions.generated(kShadow)(ram, ctx, rt);
        }

        bozo::render::MeshSubmitter g_submitter;
    } // namespace

    // The game's frame ended: its 3D, the opaque draws then the blended ones, in the order the game made them.
    void meshesFrameEnd()
    {
        for (const auto &d : g_opaque)
            g_submitter.submit(d);
        for (const auto &d : g_blended)
            g_submitter.submit(d);
        if (++g_frames % 600 == 0 && (g_drawn || g_missing))
            std::printf("[jc2:meshes] frames %llu: %llu parts drawn, %llu not found, %llu shadows (%llu calls, %llu without the bit, %llu culled, "
                        "%llu without a texture)\n",
                        (unsigned long long)g_frames, (unsigned long long)g_drawn, (unsigned long long)g_missing,
                        (unsigned long long)g_shadows, (unsigned long long)g_shadowCalls, (unsigned long long)g_shadowNoBit,
                        (unsigned long long)g_shadowCulled, (unsigned long long)g_shadowNoTexture);
        g_opaque.clear();
        g_blended.clear();
    }

    void installMeshes(Runtime &rt, bozo::port::Registry &r)
    {
        g_rt = &rt;
        g_submitter.tag = "jc2";
        r.registerFunction(kResourceRequest, resourceRequest);
        r.registerFunction(kTextureSlot, textureSlot);
        r.registerFunction(kRequestDone, requestDone);
        if (meshesOff)
            return;
        r.registerFunction(kStaticBuilder, staticBuilder);
        r.registerFunction(kStaticMaterial, staticMaterial);
        r.registerFunction(kStaticCall, staticCall);
        r.registerFunction(kSkinnedBuilder, skinnedBuilder);
        r.registerFunction(kSkinnedMaterial, skinnedMaterial);
        r.registerFunction(kSkinnedCall, skinnedCall);
        r.registerFunction(kShadow, shadow);
    }
} // namespace jc2
