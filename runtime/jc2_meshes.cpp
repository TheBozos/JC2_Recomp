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
        const bool traceEffects = std::getenv("JC2_TRACE_FX") != nullptr;
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

        // A record held by a resident bank slot (found by its address).
        const TextureView *residentRecord(const uint8_t *ram, uint32_t record)
        {
            for (uint32_t bank = 0; bank < 4; ++bank)
            {
                const uint32_t entry = kTextureBanks + bank * 52u, mask = r32(ram, entry + 0x18);
                for (uint32_t slot = 0; slot < 4; ++slot)
                {
                    if (!(mask & (1u << slot)))
                        continue;
                    const uint32_t at = entry + 0x1C + slot * 12u, count = r32(ram, at + 4), records = r32(ram, at + 8);
                    if (record < records || record >= records + count * 0x60u || (record - records) % 0x60u)
                        continue;
                    uint8_t material[0x10] = {};
                    material[0xC] = static_cast<uint8_t>(bank), material[0xD] = static_cast<uint8_t>(slot);
                    const uint32_t index = (record - records) / 0x60u;
                    material[0xE] = static_cast<uint8_t>(index), material[0xF] = static_cast<uint8_t>(index >> 8);
                    return materialTexture(ram, material);
                }
            }
            return nullptr;
        }

        // ---- The room's projected texture (VU1 program B flag 0x800, second pass at 0x4BC / 0x508) ----
        // 0x195EA0(room) draws a room's nodes; it keeps the pass's texture record at room +0x46C (set up as context 2 by
        // 0x14A800: TEX0 from the record with TFX modulate, CLAMP 5, ALPHA 0x46 = (0 - Cd) As + Cd, TEST: depth GEQUAL,
        // no depth or alpha write). The builder sets the program's flag 0x800 when the material has it, camera +0x440 is
        // 0 and the node's box is inside the projector (context +0x50: camera +0x3F0 x W). Per vertex the program draws
        // the batch again with colour 128 and s, t = P (position + W's translation), P = VU[34..37] = camera +0x3B0.
        constexpr uint32_t kRoomDraw = 0x00195EA0u;
        uint32_t g_projectedRecord = 0, g_projectedTim2 = 0; // room +0x46C, +0x470 (its TIM2: picture 0)
        std::map<uint64_t, TextureView> g_projectedViews;
        const TextureView *projectedTexture(const uint8_t *ram)
        {
            const uint64_t key = 0x3C00000000000000ull | uint64_t(g_projectedTim2) << 24 ^ g_projectedRecord;
            auto found = g_projectedViews.find(key);
            if (found != g_projectedViews.end())
                return found->second.texture ? &found->second : nullptr;
            TextureView &view = g_projectedViews[key];
            const std::vector<uint32_t> pictures = tim2Pictures(ram, g_projectedTim2);
            if (pictures.empty())
                return nullptr;
            view.key = key;
            view.texture = decodeTim2Picture(at(ram, pictures[0]), 0, key);
            if (!view.texture)
                return nullptr;
            const uint64_t tex0 = r64(ram, g_projectedRecord + 0x20);
            const float pw = float(1u << ((tex0 >> 26) & 15u)), ph = float(1u << ((tex0 >> 30) & 15u));
            const uint32_t place = r32(ram, g_projectedRecord + 0x10);
            const float w = float(view.texture->width), h = float(view.texture->height);
            view.su = pw / w, view.ou = -float(place & 0xFFFFu) / w;
            view.sv = ph / h, view.ov = -float(place >> 16) / h;
            return &view;
        }
        const bool projectedOn = [] {
            const char *v = std::getenv("JC2_PROJECTED");
            return !(v && v[0] == '0');
        }();
        uint64_t g_projectedBatches = 0;
        void roomDraw(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            g_projectedRecord = r32(ram, ctx->r[4].uw[0] + 0x46Cu);
            const uint32_t tim2 = r32(ram, ctx->r[4].uw[0] + 0x470u);
            if (tim2 != g_projectedTim2)
                g_projectedViews.clear(); // another room (its picture may be at the same place next time: decode again)
            g_projectedTim2 = tim2;
            if (traceEffects)
            {
                static int shown = 0;
                if (shown++ < 3)
                    std::printf("[jc2:fx] room draw %#x: record %#x, +0 %#x\n", ctx->r[4].uw[0], g_projectedRecord,
                                r32(ram, ctx->r[4].uw[0]));
            }
            rt->functions.generated(kRoomDraw)(ram, ctx, rt);
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

        // Program B's colour (VU 0x57D / 0x479, checked against the microcode run on a captured frame):
        //   d = max(0, L . n)  with n the object normal taken through W as is (not renormalised)
        //   colour = min(1, (C0 d0 + C1 d1 + C2 d2 + ambient) x base) x scale, truncated
        // base is the material colour, or the vertex colour / 127 when the material has bit 1; scale is VU[1] (255)
        // halved to 128.01 for textured materials (bit 5 without bit 8). Unlit (program bit 0): base x scale.
        uint32_t staticColour(const Lights &l, const MaterialView &m, const float wn[3], const uint8_t *vc, bool unlit)
        {
            const bool halve = (m.flags & 0x20u) && !(m.flags & 0x100u);
            const float scale = halve ? 255.0f * 0.502f : 255.0f;
            float base[4] = {m.colour[0], m.colour[1], m.colour[2], m.colour[3]};
            if (vc && (m.flags & 2u))
                for (int c = 0; c < 3; ++c)
                    base[c] = vc[c] / 127.0f;
            float col[4];
            if (unlit)
                for (int c = 0; c < 4; ++c)
                    col[c] = base[c] * (c == 3 ? 128.0f : scale);
            else
            {
                float d[3];
                for (int k = 0; k < 3; ++k)
                    d[k] = std::max(0.0f, l.directions.m[k] * wn[0] + l.directions.m[4 + k] * wn[1] + l.directions.m[8 + k] * wn[2]);
                for (int c = 0; c < 4; ++c)
                {
                    const float lit = l.colours.m[c] * d[0] + l.colours.m[4 + c] * d[1] + l.colours.m[8 + c] * d[2] + l.colours.m[12 + c];
                    col[c] = std::min(1.0f, lit * base[c]) * (c == 3 ? 128.0f : scale);
                }
            }
            uint32_t rgba = 0;
            for (int c = 0; c < 4; ++c)
                rgba |= static_cast<uint32_t>(std::clamp(col[c], 0.0f, 255.0f)) << (8 * c);
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
            batch.setsZbuf = true; // the models write depth (ZBUF.ZMSK off): the submitter keeps the last batch's otherwise
            batch.depthMasked = false;
            batch.alpha = material.alpha;
            batch.test = material.test;

            // The projected second pass (static only).
            const TextureView *projectedView = nullptr;
            Mat projector{};
            float lift[3] = {0, 0, 0};
            if (traceEffects && !skinned && (material.flags & 0x800u))
            {
                static int shown = 0;
                if (shown++ < 6)
                    std::printf("[jc2:fx] 0x800 material: camera +0x440 %#x record %#x view %d\n", r32(ram, camera + 0x440),
                                g_projectedRecord, g_projectedTim2 ? projectedTexture(ram) != nullptr : -1);
            }
            if (!skinned && (material.flags & 0x800u) && !r32(ram, camera + 0x440) && g_projectedRecord && projectedOn)
            {
                projectedView = g_projectedTim2 ? projectedTexture(ram) : nullptr;
                projector = readMat(ram, camera + 0x3B0); // VU[34..37]: world to texture (camera +0x3F0 is the box test's)
                lift[0] = rf(ram, g_context + 0x40), lift[1] = rf(ram, g_context + 0x44), lift[2] = rf(ram, g_context + 0x48);
                if (traceEffects && projectedView && g_projectedBatches < 4)
                    std::printf("[jc2:fx] projected record %#x P %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f "
                                "%.4f / %.4f %.4f %.4f %.4f lift %.3f %.3f %.3f\n",
                                g_projectedRecord, projector.m[0], projector.m[1], projector.m[2], projector.m[3],
                                projector.m[4], projector.m[5], projector.m[6], projector.m[7], projector.m[8],
                                projector.m[9], projector.m[10], projector.m[11], projector.m[12], projector.m[13],
                                projector.m[14], projector.m[15], lift[0], lift[1], lift[2]);
            }
            bozo::render::MeshBatch projected;
            float projectedMin[2] = {1e30f, 1e30f}, projectedMax[2] = {-1e30f, -1e30f};
            if (projectedView)
            {
                projected.textured = true;
                projected.texture = projectedView->texture;
                projected.tex0 = projectedView->key;
                projected.setsTex0 = true;
                projected.blended = true;
                projected.setsAlpha = projected.setsTest = projected.setsZbuf = true;
                projected.alpha = 0x46;      // (0 - Cd) As + Cd: drawn black, its alpha darkens
                projected.test = 0x50000;    // depth GEQUAL, no alpha test
                projected.depthMasked = true; // no depth write
                projected.clampU = projected.clampV = true;
            }
            auto projectedVertex = [&](const bozo::render::Vertex &v) {
                bozo::render::Vertex out = v;
                const float p[4] = {v.x + lift[0], v.y + lift[1], v.z + lift[2], 1.0f};
                float st[4];
                apply(projector, p, st);
                projectedMin[0] = std::min(projectedMin[0], st[0]), projectedMax[0] = std::max(projectedMax[0], st[0]);
                projectedMin[1] = std::min(projectedMin[1], st[1]), projectedMax[1] = std::max(projectedMax[1], st[1]);
                out.u = st[0] * projectedView->su + projectedView->ou;
                out.v = st[1] * projectedView->sv + projectedView->ov;
                out.rgba = 0x80000000u;
                return out;
            };

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
                        const float n4[4] = {n[0], n[1], n[2], 0.0f};
                        float wn[4];
                        apply(world, n4, wn);
                        v.rgba = staticColour(lights, material, wn, hasColour ? sv.rgba : nullptr, unlit || !hasNrm);
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
                            if (projectedView)
                                for (uint32_t k = i - 2; k <= i; ++k)
                                    projected.vertices.push_back(projectedVertex(strip[k]));
                        }
                    }
                    wind *= windX;
                }
            }
            if (batch.vertices.empty())
                return;
            draw.batches.push_back(std::move(batch));
            // The builder's box test (0x13F800 against the projector), as the texture coordinates' range: a part wholly
            // past the texture's edge is not drawn again.
            if (projectedView && !projected.vertices.empty() && projectedMax[0] >= 0.0f && projectedMin[0] <= 1.0f &&
                projectedMax[1] >= 0.0f && projectedMin[1] <= 1.0f)
            {
                draw.batches.push_back(std::move(projected));
                ++g_projectedBatches;
            }
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

        // ---- The screen fade: 0x22CF10(corner colour, centre colour) ----
        // A full-screen fan in buffer 1: the centre (2048, 2048) in one colour, the four corners of the field
        // (1728..2368, 1936..2160) in the other; colours are four ints (r, g, b, alpha 0x80 = 1.0); PRIM 0x4D (fan,
        // Gouraud, blended), ALPHA 0x44 ((Cs - Cd) As + Cd), Z test ALWAYS. Drawn after the frame's 3D, before the 2D.
        constexpr uint32_t kScreenFade = 0x0022CF10u;
        struct ScreenFade
        {
            uint32_t corner, centre; // RGBA bytes, alpha 0x80 = 1.0
        };
        std::vector<ScreenFade> g_fades;

        uint32_t fadeColour(const uint8_t *ram, uint32_t at4)
        {
            uint32_t c = 0;
            for (int k = 0; k < 4; ++k)
                c |= std::min<uint32_t>(r32(ram, at4 + 4u * k), 255u) << (8 * k);
            return c;
        }
        void screenFade(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            static const bool fadeOff = [] {
                const char *v = std::getenv("JC2_FADE");
                return v && v[0] == '0';
            }();
            const ScreenFade f{fadeColour(ram, ctx->r[4].uw[0]), fadeColour(ram, ctx->r[5].uw[0])};
            static uint64_t calls = 0;
            if (traceEffects && calls++ % 300 == 0)
                std::printf("[jc2:fx] fade corner %08x centre %08x (raw %08x %08x %08x %08x)\n", f.corner, f.centre,
                            r32(ram, ctx->r[4].uw[0]), r32(ram, ctx->r[4].uw[0] + 4), r32(ram, ctx->r[4].uw[0] + 8),
                            r32(ram, ctx->r[4].uw[0] + 12));
            if (!meshesOff && !fadeOff)
                g_fades.push_back(f);
            rt->functions.generated(kScreenFade)(ram, ctx, rt);
        }
        void drawFades()
        {
            static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            for (const ScreenFade &f : g_fades)
            {
                if ((f.corner >> 24) == 0 && (f.centre >> 24) == 0)
                    continue;
                bozo::render::Material m;
                m.textured = false;
                m.directColour = true;
                m.blend = bozo::render::Material::Blend::Alpha;
                m.depthTest = false;
                m.depthWrite = false;
                auto vtx = [](float x, float y, uint32_t rgba) {
                    bozo::render::Vertex v;
                    v.x = x;
                    v.y = 2.0f * y - 2048.0f; // the field's line to the frame's
                    v.z = 0;
                    v.rgba = rgba;
                    return v;
                };
                const bozo::render::Vertex c = vtx(2048, 2048, f.centre), a = vtx(1728, 1936, f.corner),
                                           b = vtx(2368, 1936, f.corner), d = vtx(2368, 2160, f.corner),
                                           e = vtx(1728, 2160, f.corner);
                const bozo::render::Vertex tris[12] = {c, a, b, c, b, d, c, d, e, c, e, a};
                bozo::render::Renderer::instance().draw(m, identity, tris, 12);
            }
            g_fades.clear();
        }

        // ---- Effects: the primitive drawer 0x146990(obj, texture record, TIM2, camera, pass, picture) ----
        // VU1 program C, by the obj's flags (+0): 8 the shadows (above, from their own call), 4 camera-facing sprites
        // (particles, model emitters), 0x10 2D parallelograms (overlay glows), 0x400 screen sprites (the depth of field,
        // below, from its own call); no other path is used. obj: +4 blend (ALPHA: 0 0x48 additive, 1 0x42 subtractive,
        // 2 0x44 alpha, 4 0x54 by destination alpha, 5 0x40_00000046), +8 textured, +0x30 records (3 qwords each), +0x34
        // their count, +0x40 W. The pass (t0) is PRIM.ABE. TEST: flags 0x10000 alpha >= 0x20 and Z written, 0x8000 no Z
        // test, else Z GEQUAL without writing. Texture: picture t1 of the TIM2 a2, else the record a1's (its bank slot's
        // TIM2); the record's rectangle is the whole picture.
        constexpr uint32_t kPrimitiveDrawer = 0x00146990u;
        struct ScreenDraw
        {
            bozo::render::Material material;
            std::shared_ptr<const bozo::render::Texture> texture;
            std::vector<bozo::render::Vertex> vertices;
        };
        std::vector<ScreenDraw> g_effects;
        uint64_t g_effectDraws = 0;
        std::map<uint64_t, std::shared_ptr<const bozo::render::Texture>> g_effectTextures;

        std::shared_ptr<const bozo::render::Texture> recordTexture(const uint8_t *ram, uint32_t record, uint32_t tim2,
                                                                  int32_t picture)
        {
            if (tim2 && picture >= 0)
            {
                const uint64_t key = 0x3F00000000000000ull | uint64_t(tim2) << 12 | uint32_t(picture & 0xFFF);
                auto found = g_effectTextures.find(key);
                if (found != g_effectTextures.end())
                    return found->second;
                const std::vector<uint32_t> pictures = tim2Pictures(ram, tim2);
                auto texture = size_t(picture) < pictures.size() ? decodeTim2Picture(at(ram, pictures[picture]), 0, key) : nullptr;
                g_effectTextures[key] = texture;
                return texture;
            }
            // A resident record: the bank slot holding it.
            const TextureView *view = residentRecord(ram, record);
            return view ? view->texture : nullptr;
        }

        bozo::render::Material effectMaterial(uint32_t flags, uint32_t blend, bool textured, bool pass)
        {
            static const uint64_t kAlphas[6] = {0x48, 0x42, 0x44, 0x44, 0x54, 0x4000000046ull};
            const uint64_t alpha = kAlphas[blend < 6 ? blend : 2];
            bozo::render::Material m;
            m.textured = textured;
            m.directColour = !textured;
            m.clampU = m.clampV = true;
            if (pass)
            {
                m.gsBlend = true;
                m.blendA = alpha & 3u, m.blendB = (alpha >> 2) & 3u, m.blendC = (alpha >> 4) & 3u, m.blendD = (alpha >> 6) & 3u;
                m.blendFix = static_cast<uint8_t>(alpha >> 32);
            }
            else
                m.blend = bozo::render::Material::Blend::Opaque;
            if (flags & 0x10000u)
            {
                m.alphaTest = true;
                m.alphaFunc = bozo::render::Material::AlphaFunc::GreaterEqual;
                m.alphaRef = 0x20;
                m.depthWrite = true;
            }
            else if (flags & 0x8000u)
                m.depthTest = false, m.depthWrite = false;
            else
                m.depthWrite = false;
            return m;
        }

        // A GS point of program C's projection (P V W, field y) to the frame's (y' = 2 y - 2048).
        bool project(const Mat &pvw, const float p[3], float out[3], float &w)
        {
            const float v[4] = {p[0], p[1], p[2], 1.0f};
            float g[4];
            apply(pvw, v, g);
            w = g[3];
            if (w <= 1e-6f)
                return false;
            out[0] = g[0] / w, out[1] = 2.0f * (g[1] / w) - 2048.0f, out[2] = g[2] / w;
            return true;
        }
        bool insideClip(const Mat &cw, const float p[3])
        {
            const float v[4] = {p[0], p[1], p[2], 1.0f};
            float c[4];
            apply(cw, v, c);
            const float aw = std::fabs(c[3]);
            return std::fabs(c[0]) <= aw && std::fabs(c[1]) <= aw && std::fabs(c[2]) <= aw;
        }

        void primitiveDrawer(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t obj = ctx->r[4].uw[0];
            const uint32_t flags = r32(ram, obj);
            const uint32_t path = flags & 0x1C1Fu & ~8u; // the program C path bits other than the shadow's
            if (!meshesOff && (flags & 8u) == 0 && (flags & 0x400u) == 0 && (path & (4u | 0x10u)))
            {
                const uint32_t camera = r32(ram, 0x006CC5F4u + 4u * (ctx->r[7].uw[0] & 3u));
                const uint32_t records = r32(ram, obj + 0x30), count = std::min<uint32_t>(r32(ram, obj + 0x34), 500u);
                const bool textured = r32(ram, obj + 8) != 0, pass = ctx->r[8].uw[0] != 0;
                std::shared_ptr<const bozo::render::Texture> texture;
                if (textured)
                    texture = recordTexture(ram, ctx->r[5].uw[0], ctx->r[6].uw[0], static_cast<int32_t>(ctx->r[9].uw[0]));
                ScreenDraw draw;
                draw.material = effectMaterial(flags, ram[obj + 4], textured && texture, pass);
                draw.texture = texture;
                if (draw.material.textured)
                    draw.material.tex0 = texture->key;
                auto vtx = [](float x, float y, float z, float u, float v, uint32_t rgba) {
                    bozo::render::Vertex out;
                    out.x = x, out.y = y, out.z = z, out.u = u, out.v = v, out.rgba = rgba;
                    return out;
                };
                auto colour = [&](uint32_t q) {
                    uint32_t c = 0;
                    for (int k = 0; k < 4; ++k)
                        c |= static_cast<uint32_t>(std::clamp(rf(ram, q + 4u * k), 0.0f, 255.0f)) << (8 * k);
                    return c;
                };
                if ((path & 4u) && camera)
                {
                    // Camera-facing sprites: q0 (p, s), q1 colour. Corners A = p - s/2 (r0 + r1), B = p + s/2 (r0 + r1),
                    // r0, r1 the first two rows of V W's 3 x 3; a screen rectangle from A (UV 1, 1) to B (UV 0, 0) at
                    // B's depth; dropped when A or B is outside the clip volume.
                    const Mat world = readMat(ram, obj + 0x40), view = readMat(ram, camera + 0x130);
                    const Mat vw = mul(view, world), pvw = mul(readMat(ram, camera + 0x170), vw);
                    const Mat cw = mul(readMat(ram, camera + 0x230), world);
                    const float d[3] = {vw.m[0] + vw.m[1], vw.m[4] + vw.m[5], vw.m[8] + vw.m[9]}; // rows 0 + 1
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const uint32_t q = records + 48u * i;
                        const float s = rf(ram, q + 12) * 0.5f;
                        const float p[3] = {rf(ram, q), rf(ram, q + 4), rf(ram, q + 8)};
                        const float a[3] = {p[0] - s * d[0], p[1] - s * d[1], p[2] - s * d[2]};
                        const float b[3] = {p[0] + s * d[0], p[1] + s * d[1], p[2] + s * d[2]};
                        if (!insideClip(cw, a) || !insideClip(cw, b))
                            continue;
                        float sa[3], sb[3], wa, wb;
                        if (!project(pvw, a, sa, wa) || !project(pvw, b, sb, wb))
                            continue;
                        const uint32_t c = colour(q + 16);
                        const float z = sb[2];
                        const auto v0 = vtx(sa[0], sa[1], z, 1, 1, c), v1 = vtx(sb[0], sa[1], z, 0, 1, c),
                                   v2 = vtx(sa[0], sb[1], z, 1, 0, c), v3 = vtx(sb[0], sb[1], z, 0, 0, c);
                        draw.vertices.insert(draw.vertices.end(), {v0, v1, v2, v1, v3, v2});
                    }
                }
                else if (path & 0x10u)
                {
                    // 2D parallelograms in GS coordinates: q0 origin, q1 edges a (x, y) and b (z, w), q2 (fu, fv, -,
                    // alpha): o, o + a, o + b, o + a + b with UV (0, 0) (fu, 0) (0, fv) (fu, fv); colour 128 grey.
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const uint32_t q = records + 48u * i;
                        const float ox = rf(ram, q), oy = rf(ram, q + 4), oz = rf(ram, q + 8);
                        const float ax = rf(ram, q + 16), ay = rf(ram, q + 20), bx = rf(ram, q + 24), by = rf(ram, q + 28);
                        const float fu = rf(ram, q + 32), fv = rf(ram, q + 36);
                        const uint32_t c = 0x00808080u | std::min<uint32_t>(static_cast<uint32_t>(std::max(0.0f, rf(ram, q + 44))), 255u) << 24;
                        auto f = [](float y) { return 2.0f * y - 2048.0f; };
                        const auto v0 = vtx(ox, f(oy), oz, 0, 0, c), v1 = vtx(ox + ax, f(oy + ay), oz, fu, 0, c),
                                   v2 = vtx(ox + bx, f(oy + by), oz, 0, fv, c),
                                   v3 = vtx(ox + ax + bx, f(oy + ay + by), oz, fu, fv, c);
                        draw.vertices.insert(draw.vertices.end(), {v0, v1, v2, v1, v3, v2});
                    }
                }
                if (!draw.vertices.empty())
                {
                    ++g_effectDraws;
                    g_effects.push_back(std::move(draw));
                }
            }
            rt->functions.generated(kPrimitiveDrawer)(ram, ctx, rt);
        }

        void drawEffects()
        {
            static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            for (const ScreenDraw &d : g_effects)
                bozo::render::Renderer::instance().draw(d.material, identity, d.vertices.data(),
                                                        static_cast<uint32_t>(d.vertices.size()), d.texture);
            g_effects.clear();
        }

        // ---- Depth of field: 0x494200(table, f12 strength) ----
        // The DOF task (0x492DF0, mode != 0) gives a table of four {alpha - 1 (byte), GS Z} at a0: (1e7, base), (Z
        // near, in focus), (Z far, base), (0, base), the band around the focused actor. The game writes them as
        // full-screen sprites into the frame's destination alpha (Z GEQUAL: a sprite marks the pixels it is not behind,
        // the last one wins), then blurs the frame by six passes over itself, each C += (shift(C) - C) Ad, the shifts
        // (-+sqrt(3) S, -+S/2 field lines), (0, +-S/2), S = 1.5 strength. Here the mask is aux image 1, drawn by a
        // depth lookup over the frame's depth (bucket weights from the same sprite rule), and each pass samples the
        // frame drawn so far with its alpha from the mask.
        constexpr uint32_t kDepthOfField = 0x00494200u;
        struct DepthOfField
        {
            bool on = false;
            uint32_t z[4] = {}, alpha[4] = {};
            float strength = 0;
        } g_dof;
        const bool dofOff = [] {
            const char *v = std::getenv("JC2_DOF");
            return v && v[0] == '0';
        }();

        void depthOfField(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t table = ctx->r[4].uw[0];
            if (!meshesOff && !dofOff && r32(ram, 0x006CC5F4u))
            {
                g_dof.on = true;
                for (int i = 0; i < 4; ++i)
                {
                    g_dof.alpha[i] = (r32(ram, table + 8u * i) & 0xFFu) + 1u;
                    g_dof.z[i] = r32(ram, table + 8u * i + 4u);
                }
                g_dof.strength = ctx->f[12];
                static uint64_t calls = 0;
                if (traceEffects && calls++ % 300 == 0)
                    std::printf("[jc2:fx] dof strength %g: (%u, %u) (%u, %u) (%u, %u) (%u, %u)\n", g_dof.strength, g_dof.z[0],
                                g_dof.alpha[0], g_dof.z[1], g_dof.alpha[1], g_dof.z[2], g_dof.alpha[2], g_dof.z[3], g_dof.alpha[3]);
            }
            rt->functions.generated(kDepthOfField)(ram, ctx, rt);
        }

        void drawDepthOfField()
        {
            if (!g_dof.on)
                return;
            g_dof.on = false;
            auto &renderer = bozo::render::Renderer::instance();
            static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            // The mask: bucket i of the depth lookup covers GS Z [i, i + 1) x zUnit; Z near on bucket 200.
            const float depthMax = 16777215.0f;
            const float zNear = std::max<float>(1.0f, static_cast<float>(g_dof.z[1]));
            auto processing = std::make_shared<bozo::render::Material::ImageProcessing>();
            processing->depthLookup = true;
            processing->depthScale = 200.0f * depthMax / zNear;
            const float zUnit = zNear / 200.0f;
            for (int i = 0; i < 256; ++i)
            {
                const float z = (i + 0.5f) * zUnit;
                uint32_t alpha = 32; // the first sprite's (1e7)
                for (int k = 0; k < 4; ++k)
                    if (static_cast<float>(g_dof.z[k]) >= z)
                        alpha = g_dof.alpha[k];
                processing->depthWeights[i] = std::min(1.0f, alpha / 128.0f);
            }
            auto vtx = [](float x, float y, float u, float v) {
                bozo::render::Vertex out;
                out.x = x, out.y = y, out.z = 0, out.u = u, out.v = v, out.rgba = 0x80808080u;
                return out;
            };
            const float x0 = 1728, y0 = 1824, x1 = 2368, y1 = 2272;
            {
                bozo::render::Material m;
                m.target = 1;
                m.textured = false;
                m.blend = bozo::render::Material::Blend::Opaque;
                m.depthTest = false, m.depthWrite = false;
                m.imageProcessing = processing;
                const bozo::render::Vertex q[6] = {vtx(x0, y0, 0, 0), vtx(x1, y0, 0, 0), vtx(x0, y1, 0, 0),
                                                   vtx(x1, y0, 0, 0), vtx(x1, y1, 0, 0), vtx(x0, y1, 0, 0)};
                renderer.draw(m, identity, q, 6);
            }
            auto alphaFromMask = std::make_shared<bozo::render::Material::ImageProcessing>();
            alphaFromMask->alphaSource = 1;
            alphaFromMask->alphaScale = 1.0f;
            const float s = 1.5f * g_dof.strength, sx = 1.7320508f * s, sy = s; // half a field line = one frame line
            const float offsets[6][2] = {{-sx, -sy}, {sx, -sy}, {0, sy}, {-sx, sy}, {sx, sy}, {0, -sy}};
            for (const auto &o : offsets)
            {
                bozo::render::Material m;
                m.sampleTarget = true;
                m.textured = true;
                m.textureAlpha = false;
                m.blend = bozo::render::Material::Blend::Alpha;
                m.colourMask = 0x7u;
                m.depthTest = false, m.depthWrite = false;
                m.imageProcessing = alphaFromMask;
                const bozo::render::Vertex q[6] = {vtx(x0, y0, o[0], o[1]), vtx(x1, y0, 640 + o[0], o[1]),
                                                   vtx(x0, y1, o[0], 448 + o[1]), vtx(x1, y0, 640 + o[0], o[1]),
                                                   vtx(x1, y1, 640 + o[0], 448 + o[1]), vtx(x0, y1, o[0], 448 + o[1])};
                renderer.draw(m, identity, q, 6);
            }
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
        drawEffects();
        drawDepthOfField();
        drawFades();
        if (++g_frames % 600 == 0 && (g_drawn || g_missing))
            std::printf("[jc2:meshes] frames %llu: %llu parts drawn, %llu not found, %llu shadows (%llu calls, %llu without the bit, %llu culled, "
                        "%llu without a texture), %llu projected passes\n",
                        (unsigned long long)g_frames, (unsigned long long)g_drawn, (unsigned long long)g_missing,
                        (unsigned long long)g_shadows, (unsigned long long)g_shadowCalls, (unsigned long long)g_shadowNoBit,
                        (unsigned long long)g_shadowCulled, (unsigned long long)g_shadowNoTexture,
                        (unsigned long long)g_projectedBatches);
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
        r.registerFunction(kScreenFade, screenFade);
        r.registerFunction(kPrimitiveDrawer, primitiveDrawer);
        r.registerFunction(kDepthOfField, depthOfField);
        r.registerFunction(kRoomDraw, roomDraw);
    }
} // namespace jc2
