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
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include "bozo/render/mesh_draw.h"
#include "bozo/render/mesh_pack.h"
#include "bozo/render/renderer.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define PSAPI_VERSION 2 // GetProcessMemoryInfo from kernel32
#include <psapi.h>
#include "bozo/render/texture.h"
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef JC2_MESH_PACK
#define JC2_MESH_PACK "jc2_meshes.pack"
#endif

namespace jc2
{
    std::shared_ptr<const bozo::render::Texture> decodeTim2Picture(const uint8_t *header, uint32_t palette, uint64_t key);
    size_t pictureCacheSize();

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
        std::set<uint32_t> g_unknown;        // chains in no resource (discoverCopy), until the next load
        // Single models the game copied out of a loaded resource (the battle stage: resource 1737's sub-files copied
        // to 0x1B906C0 and on, their header's pointers relocated): copy address -> the pack file. Found again from
        // memory after a state load, so not saved.
        struct Copy
        {
            uint32_t size, file;
        };
        std::map<uint32_t, Copy> g_copies;

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
            g_unknown.clear();
        }

        // A resource the game copied in memory rather than loaded (the player's model, 2283, moved to the Room of
        // Life's heap): no disc read tells where it went. Found from the copy itself: the model header ('0IVX00.1')
        // before the chain, and the archive around it (u32 count, then the sub-files' offsets), whose model sub-file
        // has the place and size the pack's index gives a resource. Addresses tried in vain are kept until the next
        // load, so a chain in no resource costs one search.
        // The model at `copy` (its "0IVX00.1") as one of a loaded resource's model sub-files copied there: the same
        // bytes except the pointers the game relocates (the header's, the node tables': 14 words in resource 1737's
        // stage pads, 15 in 1732's 62 KB arena), so at most one word in 64 may differ.
        bool sameModel(const uint8_t *ram, uint32_t source, uint32_t copy, uint32_t size)
        {
            uint32_t allowed = size / 256u + 4u, differ = 0;
            for (uint32_t k = 8; k + 4 <= size; k += 4)
                if (r32(ram, source + k) != r32(ram, copy + k) && ++differ > allowed)
                    return false;
            return true;
        }

        bool discoverModelCopy(const uint8_t *ram, uint32_t copy)
        {
            for (const auto &[base, loaded] : g_loaded)
            {
                auto found = g_resources.find(loaded.id);
                if (found == g_resources.end())
                    continue;
                for (const SubFile &sf : found->second)
                {
                    const uint32_t source = base + sf.offset;
                    if (sf.size < 0x100u || source == copy || copy + sf.size > 0x2000000u ||
                        source + sf.size > 0x2000000u || std::memcmp(at(ram, source), "0IVX00.1", 8) != 0 ||
                        !sameModel(ram, source, copy, sf.size))
                        continue;
                    auto it = g_copies.lower_bound(copy);
                    if (it != g_copies.begin() && std::prev(it)->first + std::prev(it)->second.size > copy)
                        it = std::prev(it);
                    while (it != g_copies.end() && it->first < copy + sf.size)
                        it = g_copies.erase(it);
                    g_copies[copy] = {sf.size, sf.file};
                    if (traceLoad)
                        std::printf("[jc2:loaded] resource %u's model at +%#x found copied alone at %#x\n", loaded.id,
                                    sf.offset, copy);
                    return true;
                }
            }
            return false;
        }

        bool discoverCopy(const uint8_t *ram, uint32_t chain)
        {
            if (!g_unknown.insert(chain).second)
                return false;
            constexpr uint32_t kSearch = 0x80000u;
            for (uint32_t p = chain & ~0xFu; p + kSearch > chain && p >= 0x10u; p -= 0x10u)
            {
                if (std::memcmp(at(ram, p), "0IVX00.1", 8) != 0)
                    continue;
                for (const auto &[id, subs] : g_resources)
                    for (const SubFile &sf : subs)
                    {
                        if (sf.offset > p || chain - p >= sf.size)
                            continue;
                        const uint32_t base = p - sf.offset;
                        const uint32_t count = r32(ram, base);
                        if (count == 0 || count > 255u)
                            continue;
                        bool listed = false;
                        for (uint32_t k = 0; k < count && !listed; ++k)
                            listed = r32(ram, base + 4u + 4u * k) == sf.offset;
                        // The sub-file's size from the archive (the next offset), as the pack has it.
                        if (!listed)
                            continue;
                        uint32_t next = 0;
                        for (uint32_t k = 0; k < count; ++k)
                            if (r32(ram, base + 4u + 4u * k) == sf.offset)
                                next = k + 1 < count ? r32(ram, base + 8u + 4u * k) : 0u;
                        if (next && next - sf.offset != sf.size)
                            continue;
                        noteLoaded(ram, id, base);
                        if (traceLoad)
                            std::printf("[jc2:loaded] id %u found copied at %#x (chain %#x)\n", id, base, chain);
                        return true;
                    }
                return discoverModelCopy(ram, p); // no archive has this model there: a model copied alone?
            }
            return false;
        }

        // A chain address (a submesh + 0x30) to its pack object.
        const packs::MeshObject *lookup(const uint8_t *ram, uint32_t chain, uint32_t &resource)
        {
            chain &= 0x1FFFFFFu;
            if (auto c = g_copies.upper_bound(chain); c != g_copies.begin())
            {
                --c;
                if (chain - c->first < c->second.size)
                {
                    if (std::memcmp(at(ram, c->first), "0IVX00.1", 8) == 0 && c->second.file < g_pack.files().size())
                        return g_pack.files()[c->second.file].object(chain - c->first);
                    g_copies.erase(c); // the memory went to something else
                }
            }
            auto it = g_loaded.upper_bound(chain);
            if (it == g_loaded.begin() || chain - std::prev(it)->first >= std::prev(it)->second.size)
            {
                return discoverCopy(ram, chain) ? lookup(ram, chain, resource) : nullptr;
            }
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
                {
                    // The memory may have gone to something else since (the game frees and copies models).
                    if (std::memcmp(at(ram, it->first + s.offset), "0IVX00.1", 8) != 0)
                    {
                        g_loaded.erase(it);
                        g_unknown.clear();
                        return discoverCopy(ram, chain) ? lookup(ram, chain, resource) : nullptr;
                    }
                    return g_pack.files()[s.file].object(offset - s.offset);
                }
            // In the resource's footprint but none of its models: the game put something else there since (a model
            // copied over its pictures).
            return discoverCopy(ram, chain) ? lookup(ram, chain, resource) : nullptr;
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
            const uint32_t where = r32(ram, desc + 0x18);
            const uint32_t before = where ? r32(ram, where) : 0;
            rt->functions.generated(kResourceRequest)(ram, ctx, rt);
            if (traceLoad && where)
                std::printf("[jc2:load]   id %u: *%#x %#x -> %#x at return\n", id, where, before, r32(ram, where));
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
            // By record only: the forest registers a slot every frame, and a key with the registration's generation in
            // it decoded every texture again under a new key each frame, the picture cache keeping them all (4 GB a
            // minute). A picture whose bytes changed at the same record is decoded again by that cache's check.
            view.key = 0x3D00000000000000ull | record;
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

        const TextureView *residentRecord(const uint8_t *ram, uint32_t record);

        // A copy of a resident record (a battle effect's particles keep copies of the battle bank's records): the bank
        // record with the same TEX0, place and size, so the same picture of the same TIM2.
        const TextureView *residentCopy(const uint8_t *ram, uint32_t record)
        {
            const uint64_t tex0 = r64(ram, record + 0x20);
            const uint32_t place = r32(ram, record + 0x10), size = r32(ram, record + 0x14);
            for (uint32_t bank = 0; bank < 4; ++bank)
            {
                const uint32_t entry = kTextureBanks + bank * 52u, mask = r32(ram, entry + 0x18);
                for (uint32_t slot = 0; slot < 4; ++slot)
                {
                    if (!(mask & (1u << slot)))
                        continue;
                    const uint32_t at = entry + 0x1C + slot * 12u, count = std::min(r32(ram, at + 4), 1024u),
                                   records = r32(ram, at + 8);
                    if (records < 0x100000u || records + count * 0x60u > 0x2000000u)
                        continue;
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const uint32_t candidate = records + i * 0x60u;
                        if (r64(ram, candidate + 0x20) == tex0 && r32(ram, candidate + 0x10) == place &&
                            r32(ram, candidate + 0x14) == size)
                            return residentRecord(ram, candidate);
                    }
                }
            }
            return nullptr;
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
            const packs::MeshObject *object = lookup(ram, chain, resource);
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
            // VU 53.x: bit 0 unlit, bit 3 cull back faces, as the material has them. (Read as "double-sided, the
            // program gets flags ^ 8" until the Room of Life: its round wall, between the camera and the room, is
            // culled on the console; the forest, the Temple and the throne room look the same either way.)
            const uint32_t programFlags = material.flags;
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
            // Per strip vertex: outside the camera's clip volume (+0x2F0). Program A (skinned) drops a triangle with any
            // such vertex (CLIP against |w|, checked against the microcode: build/bozo/analysis3/emu.py). Program B
            // (static) does not: its CLIP groups (0x681, 0x778...) cut triangles at the volume, so its triangles go to
            // the renderer whole and are clipped there (dropping them left holes in the battle arena's floor and cut
            // the summoning circle short under a close camera).
            std::vector<uint8_t> outside;
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
                outside.clear();
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
                    float c4[4];
                    apply(clipObject, o4, c4);
                    const float cw = std::fabs(c4[3]);
                    outside.push_back(std::fabs(c4[0]) > cw || std::fabs(c4[1]) > cw || std::fabs(c4[2]) > cw);
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
                        bool keep = !skinned || !(outside[i - 2] || outside[i - 1] || outside[i]);
                        if (keep && cull)
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
            // JC2_TRACE_DRAWS=n: every part drawn in the n-th frame since the run started (resource, sub-file offset,
            // material, the area its vertices cover on screen).
            static const uint64_t traceDrawsAt = [] {
                const char *v = std::getenv("JC2_TRACE_DRAWS");
                return v ? std::strtoull(v, nullptr, 10) : 0ull;
            }();
            if (traceDrawsAt && g_frames == traceDrawsAt)
            {
                float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                unsigned behind = 0;
                for (const bozo::render::Vertex &v : draw.batches[0].vertices)
                {
                    float s4[4];
                    const float o4[4] = {v.x, v.y, v.z, 1.0f};
                    apply(screenM, o4, s4);
                    if (s4[3] <= 0)
                        ++behind;
                    else
                    {
                        const float x = s4[0] / s4[3], y = s4[1] / s4[3];
                        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                    }
                }
                std::printf("[jc2:draw] %s res %u chain %#x flags %#x prim %#llx alpha %#llx colour a %.2f tris %zu "
                            "screen %.0f..%.0f x %.0f..%.0f behind %u%s; texture %s %ux%u, bank %u slot %u index %u, "
                            "uv %.2f..%.2f x %.2f..%.2f\n",
                            skinned ? "skinned" : "static", resource, chain, material.flags,
                            (unsigned long long)material.prim, (unsigned long long)material.alpha, material.colour[3],
                            draw.batches[0].vertices.size() / 3, x0, x1, y0, y1, behind, cull ? " cull" : "",
                            draw.batches[0].texture ? "yes" : "no",
                            draw.batches[0].texture ? draw.batches[0].texture->width : 0u,
                            draw.batches[0].texture ? draw.batches[0].texture->height : 0u, material.bytes[0xC],
                            material.bytes[0xD], unsigned(material.bytes[0xE] | material.bytes[0xF] << 8),
                            [&] {
                                float m = 1e30f;
                                for (const auto &v : draw.batches[0].vertices) m = std::min(m, v.u);
                                return m;
                            }(),
                            [&] {
                                float m = -1e30f;
                                for (const auto &v : draw.batches[0].vertices) m = std::max(m, v.u);
                                return m;
                            }(),
                            [&] {
                                float m = 1e30f;
                                for (const auto &v : draw.batches[0].vertices) m = std::min(m, v.v);
                                return m;
                            }(),
                            [&] {
                                float m = -1e30f;
                                for (const auto &v : draw.batches[0].vertices) m = std::max(m, v.v);
                                return m;
                            }());
                // JC2_TRACE_AREA=x0,z0,x1,z1: the world heights of this part's vertices over that area (static only).
                static const char *area = std::getenv("JC2_TRACE_AREA");
                float ax0, az0, ax1, az1;
                if (skinned && area)
                {
                    float sy0 = 1e30f, sy1 = -1e30f, sx = 0, sz = 0;
                    for (const bozo::render::Vertex &v : draw.batches[0].vertices)
                        sy0 = std::min(sy0, v.y), sy1 = std::max(sy1, v.y), sx += v.x, sz += v.z;
                    const float n = float(std::max<size_t>(1, draw.batches[0].vertices.size()));
                    std::printf("[jc2:area] skinned res %u chain %#x: y %.3f..%.3f, centre x %.2f z %.2f\n", resource, chain,
                                sy0, sy1, sx / n, sz / n);
                }
                if (!skinned && area && std::sscanf(area, "%f,%f,%f,%f", &ax0, &az0, &ax1, &az1) == 4)
                {
                    float hy0 = 1e30f, hy1 = -1e30f;
                    unsigned n = 0;
                    for (const bozo::render::Vertex &v : draw.batches[0].vertices)
                    {
                        const float o4[4] = {v.x, v.y, v.z, 1.0f};
                        float w4[4];
                        apply(world, o4, w4);
                        if (w4[0] >= ax0 && w4[0] <= ax1 && w4[2] >= az0 && w4[2] <= az1)
                            hy0 = std::min(hy0, w4[1]), hy1 = std::max(hy1, w4[1]), ++n;
                    }
                    if (n)
                        std::printf("[jc2:area] res %u chain %#x: %u vertices in the area, y %.3f..%.3f\n", resource, chain,
                                    n, hy0, hy1);
                    // JC2_TRACE_AREA_FILE: every triangle of the part in world space (x y z, nine floats a line).
                    static FILE *triangles = [] {
                        const char *path = std::getenv("JC2_TRACE_AREA_FILE");
                        return path ? std::fopen(path, "w") : nullptr;
                    }();
                    if (triangles && n)
                    {
                        const auto &vs = draw.batches[0].vertices;
                        for (size_t t = 0; t + 2 < vs.size(); t += 3)
                        {
                            for (int k = 0; k < 3; ++k)
                            {
                                const float o4[4] = {vs[t + k].x, vs[t + k].y, vs[t + k].z, 1.0f};
                                float w4[4];
                                apply(world, o4, w4);
                                std::fprintf(triangles, "%.3f %.3f %.3f ", w4[0], w4[1], w4[2]);
                            }
                            std::fprintf(triangles, "%u\n", chain);
                        }
                        std::fflush(triangles);
                    }
                }
            }
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
            // A resident record: the bank slot holding it, or the bank record it is a copy of.
            const TextureView *view = residentRecord(ram, record);
            if (!view && record >= 0x100000u && record + 0x60u <= 0x2000000u)
                view = residentCopy(ram, record);
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
            if (traceEffects)
            {
                static std::set<uint64_t> seen;
                const uint64_t sig = uint64_t(flags) << 32 | uint32_t(ram[obj + 4]) << 8 | (r32(ram, obj + 8) ? 1u : 0u) |
                                     (ctx->r[8].uw[0] ? 2u : 0u);
                if (seen.size() < 64 && seen.insert(sig).second)
                    std::printf("[jc2:fx] drawer flags 0x%x blend %u textured %u pass %u count %u record 0x%x tim2 0x%x "
                                "picture %d caller 0x%x\n",
                                flags, ram[obj + 4], r32(ram, obj + 8), ctx->r[8].uw[0], r32(ram, obj + 0x34),
                                ctx->r[5].uw[0], ctx->r[6].uw[0], static_cast<int32_t>(ctx->r[9].uw[0]), ctx->r[31].uw[0]);
            }
            if (!meshesOff && (flags & 8u) == 0 && (flags & 0x400u) == 0 && (path & (4u | 0x10u)))
            {
                const uint32_t camera = r32(ram, 0x006CC5F4u + 4u * (ctx->r[7].uw[0] & 3u));
                const uint32_t records = r32(ram, obj + 0x30), count = std::min<uint32_t>(r32(ram, obj + 0x34), 500u);
                const bool textured = r32(ram, obj + 8) != 0, pass = ctx->r[8].uw[0] != 0;
                std::shared_ptr<const bozo::render::Texture> texture;
                if (textured)
                    texture = recordTexture(ram, ctx->r[5].uw[0], ctx->r[6].uw[0], static_cast<int32_t>(ctx->r[9].uw[0]));
                if (textured && !texture)
                {
                    static std::set<uint32_t> missing;
                    if (missing.size() < 16 && missing.insert(ctx->r[5].uw[0]).second)
                    {
                        std::fprintf(stderr, "[jc2:fx] textured effect without a texture: record %#x tim2 %#x picture %d;",
                                     ctx->r[5].uw[0], ctx->r[6].uw[0], static_cast<int32_t>(ctx->r[9].uw[0]));
                        for (uint32_t bank = 0; bank < 4; ++bank)
                        {
                            const uint32_t entry = kTextureBanks + bank * 52u;
                            std::fprintf(stderr, " bank %u mask %#x", bank, r32(ram, entry + 0x18));
                            for (uint32_t slot = 0; slot < 4; ++slot)
                                std::fprintf(stderr, " [%#x %u %#x]", r32(ram, entry + 0x1C + slot * 12u),
                                             r32(ram, entry + 0x20 + slot * 12u), r32(ram, entry + 0x24 + slot * 12u));
                        }
                        std::fputc('\n', stderr);
                    }
                }
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

        // ---- VU1 program D (microcode 0x4DFF60): the forests' effects and the water ----
        // The frame's effects task (0x4333B0) calls one routine per bit of the area's effect flags (0x433AF0 sets them
        // per forest and sub-area from the table at 0x64E2F0). Most of them (bits 0x1 water, 0x2, 0x4, 0x8, 0x10, 0x20,
        // 0x80, 0x400, 0x800, 0x1000, 0x8000) draw through 0x4314E0(object, position, texture animation, GS parameters,
        // t0, camera index; f12 scale): world = translation to the position times the scale, the camera 0x6CC5F4[i].
        // The object: +0 its vertex buffer, +0x10 / +0x20 its bounds, +0x30 the mode (VU 53.x: program D's path),
        // +0x34 its vertex count, +0x38 / +0x3C kinds, +0x40 1: box-tested. The animation: +0 the picture, +0x10 its
        // texture records (0x60 bytes each) when +0x14 is 0. GS parameters: +0 ALPHA's A B C D, +4 its FIX. Each batch
        // of the buffer (programDShape) is a strip. Mode 2 (the water): (S, T, 1, 0) then (x, y, z, 1) a vertex; the
        // program (path at 0x5B0; init at 0: VU 0x49 = P V W, 0x55 = clip W) projects by P V W and clips by clip W, as
        // program C's projection here; depth test GEQUAL, no depth write. The water's texture is the picture the game
        // took last from its IPU animation (the object 0x80 after the record: its ring in RAM, the CSC's macroblocks).
        // The other modes are listed once ("not drawn yet"). JC2_TRACE_PROGRAM_D=1 traces the draws
        // (JC2_WATER_NODEPTH=1: without the depth test; JC2_WATER_DY: a height offset), JC2_TRACE_AREA /
        // JC2_TRACE_AREA_FILE the room's heights over an area.
        constexpr uint32_t kProgramD = 0x004314E0u; // the program D submitter: one object a call
        struct WaterTexture
        {
            std::shared_ptr<bozo::render::Texture> texture;
            uint64_t hash = 0;
        };
        std::map<uint32_t, WaterTexture> g_waterTextures; // by texture record
        // GS page 0x3800: the water effect's init (0x2063E0 -> 0x433CE0) plays its IPU animation there, and other
        // effects sample the page (the forests' light spots, 0x22D7D8's record): the last picture taken from it.
        constexpr uint32_t kAnimationPage = 0x3800u;
        std::shared_ptr<const bozo::render::Texture> g_animationPage;

        std::shared_ptr<const bozo::render::Texture> waterTexture(const uint8_t *ram, uint32_t record)
        {
            if (record < 0x100000u || record + 0x100u > 0x2000000u)
                return nullptr;
            const uint32_t size = r32(ram, record + 0x14), w = size & 0xFFFFu, h = size >> 16;
            const uint32_t object = record + 0x80u, stream = r32(ram, object + 0x28) & 0x1FFFFFFu;
            const uint32_t mbw = r32(ram, object + 0x44), mbh = r32(ram, object + 0x48), ring = r32(ram, object + 0x54);
            if (!w || !h || w > 512u || h > 512u || mbw * 16u != w || mbh * 16u != h || !ring || ring > 16u ||
                stream < 0x100000u || stream + 4u > 0x2000000u || std::memcmp(ram + stream, "ipum", 4) != 0)
                return nullptr;
            // The picture the game took last: taking slot s leaves s = next - waiting - 1, and decoding moves both.
            const uint32_t slot = (r32(ram, object + 0x58) - r32(ram, object + 0x5C) - 1u) % ring;
            const uint32_t frameBytes = mbw * mbh * 1024u, base = (r32(ram, object + 0x6C) & 0x1FFFFFFu) + slot * frameBytes;
            if (base < 0x100000u || base + frameBytes > 0x2000000u)
                return nullptr;
            const uint8_t *src = ram + base;
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t i = 0; i < frameBytes; i += 64)
                hash = (hash ^ (uint64_t(src[i]) | uint64_t(src[i + 1]) << 8 | uint64_t(src[i + 2]) << 16)) * 1099511628211ull;
            WaterTexture &t = g_waterTextures[record];
            const bool onPage = (r64(ram, record + 0x20) & 0x3FFFu) == kAnimationPage;
            if (t.texture && t.hash == hash)
            {
                if (onPage)
                    g_animationPage = t.texture;
                return t.texture;
            }
            auto texture = std::make_shared<bozo::render::Texture>();
            texture->width = w, texture->height = h;
            texture->rgba.resize(size_t(w) * h);
            texture->key = 0x3E00000000000000ull | record;
            texture->version = t.texture ? t.texture->version + 1 : 1;
            // Macroblocks in raster order, each 16 rows of 16 RGBA pixels; alpha 0x80 is opaque.
            for (uint32_t my = 0; my < mbh; ++my)
                for (uint32_t mx = 0; mx < mbw; ++mx)
                {
                    const uint8_t *mb = src + (size_t(my) * mbw + mx) * 1024u;
                    for (uint32_t y = 0; y < 16; ++y)
                        for (uint32_t x = 0; x < 16; ++x)
                        {
                            const uint8_t *p = mb + (y * 16u + x) * 4u;
                            const uint32_t a = std::min<uint32_t>(255u, uint32_t(p[3]) * 255u / 128u);
                            texture->rgba[size_t(my * 16u + y) * w + mx * 16u + x] =
                                uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | a << 24;
                        }
                }
            t.texture = texture, t.hash = hash;
            if (onPage)
                g_animationPage = texture;
            return texture;
        }

        // ---- GS uploads: what the game sent where ----
        // The game starts VIF1 and GIF transfers itself (its packet lists, 0x13E0D8 / 0x13E1D4, and others), which
        // BozoEngine's I/O library completes at once: no DMA, no GIF. Their image transfers (BITBLTBUF, TRXPOS, TRXREG
        // by A+D, then IMAGE data, in a GIF chain or VIF1's DIRECT) say which RAM went to which GS page: noted as the
        // channel starts, an effect's texture record (not in the banks) is decoded from that RAM as the TIM2 pictures
        // are (no GS memory: the transfers' sources, read where they are). JC2_TRACE_UPLOADS=1 lists them.
        struct Upload
        {
            uint32_t dbp = 0, dbw = 0, psm = 0, x = 0, y = 0, w = 0, h = 0;
            std::vector<std::pair<uint32_t, uint32_t>> spans; // (RAM address, bytes) of the pixels, in order
            uint64_t serial = 0;
        };
        std::vector<Upload> g_uploads; // oldest first
        uint64_t g_uploadSerial = 0;
        const bool traceUploads = std::getenv("JC2_TRACE_UPLOADS") != nullptr;

        uint32_t bitsPerPixel(uint32_t psm)
        {
            switch (psm)
            {
            case 0x00: return 32; // CT32
            case 0x01: return 24; // CT24
            case 0x02: case 0x0A: return 16; // CT16, CT16S
            case 0x13: return 8;
            case 0x14: return 4;
            default: return 0;
            }
        }

        void noteUpload(Upload u)
        {
            if (!u.w || !u.h || !bitsPerPixel(u.psm))
                return;
            u.serial = ++g_uploadSerial;
            // The same place again replaces it; the list keeps the latest 2048.
            for (auto it = g_uploads.begin(); it != g_uploads.end(); ++it)
                if (it->dbp == u.dbp && it->psm == u.psm && it->x == u.x && it->y == u.y && it->w == u.w && it->h == u.h)
                {
                    g_uploads.erase(it);
                    break;
                }
            if (traceUploads && g_uploadSerial <= 200)
                std::fprintf(stderr, "[jc2:upload] page %#x width %u psm %#x at %u,%u %u x %u from %#x\n", u.dbp, u.dbw,
                             u.psm, u.x, u.y, u.w, u.h, u.spans.empty() ? 0u : u.spans[0].first);
            g_uploads.push_back(std::move(u));
            if (g_uploads.size() > 2048u)
                g_uploads.erase(g_uploads.begin());
        }

        // A GIF stream: A+D registers set the transfer, IMAGE data carries its pixels.
        struct GifReader
        {
            uint64_t qwords = 0;
            uint32_t vifStop = 0;
            uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
            uint32_t left = 0, nreg = 0, flg = 0, reg = 0;
            uint64_t regs = 0;
            Upload pending;
            uint64_t pendingBytes = 0, wantBytes = 0;

            void startImage()
            {
                pending = Upload{};
                pending.dbp = uint32_t(bitbltbuf >> 32) & 0x3FFFu, pending.dbw = uint32_t(bitbltbuf >> 48) & 0x3Fu;
                pending.psm = uint32_t(bitbltbuf >> 56) & 0x3Fu;
                pending.x = uint32_t(trxpos >> 32) & 0x7FFu, pending.y = uint32_t(trxpos >> 48) & 0x7FFu;
                pending.w = uint32_t(trxreg) & 0xFFFu, pending.h = uint32_t(trxreg >> 32) & 0xFFFu;
                pendingBytes = 0;
                wantBytes = uint64_t(pending.w) * pending.h * bitsPerPixel(pending.psm) / 8u;
            }
            // One qword of the stream, read at `address`.
            void qword(const uint8_t *ram, uint32_t address)
            {
                ++qwords;
                uint64_t lo, hi;
                std::memcpy(&lo, at(ram, address), 8);
                std::memcpy(&hi, at(ram, address + 8), 8);
                if (!left)
                {
                    left = uint32_t(lo & 0x7FFFu), flg = uint32_t(lo >> 58) & 3u, nreg = uint32_t(lo >> 60) & 15u;
                    if (!nreg)
                        nreg = 16;
                    regs = hi, reg = 0;
                    if (flg == 0 || flg == 1)
                        left *= flg == 0 ? nreg : (nreg + 1) / 2;
                    if (flg >= 2 && left && !wantBytes)
                        startImage();
                    return;
                }
                --left;
                if (flg == 0)
                {
                    const uint32_t r = uint32_t(regs >> (4 * reg)) & 15u;
                    reg = (reg + 1) % nreg;
                    if (r == 0xEu)
                    {
                        const uint32_t target = uint32_t(hi) & 0xFFu;
                        if (target == 0x50u)
                            bitbltbuf = lo;
                        else if (target == 0x51u)
                            trxpos = lo;
                        else if (target == 0x52u)
                            trxreg = lo;
                        else if (target == 0x53u && (lo & 3u) == 0u)
                            wantBytes = 0; // TRXDIR host to local: the IMAGE data that follows
                    }
                }
                else if (flg >= 2 && wantBytes)
                {
                    if (!pending.spans.empty() && pending.spans.back().first + pending.spans.back().second == address)
                        pending.spans.back().second += 16;
                    else
                        pending.spans.push_back({address, 16u});
                    pendingBytes += 16;
                    if (pendingBytes >= wantBytes)
                    {
                        noteUpload(std::move(pending));
                        pending = Upload{};
                        wantBytes = 0;
                    }
                }
            }
        };

        // The data of a source chain (DMA tags: refe, cnt, next, ref, refs, call, ret, end) in order, as (address,
        // bytes); with TTE (VIF channels) each tag's qword comes first, its lower half as padding (kPadding).
        constexpr uint32_t kPadding = 0xFFFFFFF0u;
        void chainSegments(const uint8_t *ram, uint32_t tag, bool tte, std::vector<std::pair<uint32_t, uint32_t>> &out)
        {
            uint32_t stack[2] = {0, 0}, depth = 0;
            for (int n = 0; n < 65536 && tag; ++n)
            {
                const uint32_t t = tag & ~15u;
                if (((t >> 28) != 7u) && (t < 0x1000u || t + 16u > 0x2000000u))
                    return;
                uint64_t word;
                std::memcpy(&word, at(ram, t), 8);
                const uint32_t qwc = uint32_t(word) & 0xFFFFu, id = uint32_t(word >> 28) & 7u;
                const uint32_t addr = uint32_t(word >> 32) & 0x7FFFFFF0u, spr = uint32_t(word >> 63);
                const uint32_t dataAddr = spr ? (0x70000000u | (addr & 0x3FF0u)) : addr;
                if (tte)
                {
                    // The tag's whole qword reaches VIF1, which ignores its lower half: kept as padding so that DIRECT's
                    // data after it stays on a qword boundary.
                    out.push_back({kPadding, 8u});
                    out.push_back({t + 8, 8u});
                }
                switch (id)
                {
                case 0: // refe
                    out.push_back({dataAddr, qwc * 16u});
                    return;
                case 1: // cnt
                    out.push_back({t + 16, qwc * 16u});
                    tag = t + 16 + qwc * 16u;
                    break;
                case 2: // next
                    out.push_back({t + 16, qwc * 16u});
                    tag = addr;
                    break;
                case 3: // ref
                case 4: // refs
                    out.push_back({dataAddr, qwc * 16u});
                    tag = t + 16;
                    break;
                case 5: // call
                    out.push_back({t + 16, qwc * 16u});
                    if (depth >= 2)
                        return;
                    stack[depth++] = t + 16 + qwc * 16u;
                    tag = addr;
                    break;
                case 6: // ret
                    out.push_back({t + 16, qwc * 16u});
                    if (!depth)
                        return;
                    tag = stack[--depth];
                    break;
                default: // end
                    out.push_back({t + 16, qwc * 16u});
                    return;
                }
            }
        }

        // A stream of segments read by words, with each word's address.
        struct Stream
        {
            const std::vector<std::pair<uint32_t, uint32_t>> &segs;
            size_t seg = 0;
            uint32_t offset = 0;
            uint64_t position = 0; // bytes read so far
            bool more() const { return seg < segs.size(); }
            uint32_t address() const { return segs[seg].first + offset; }
            void skip(uint32_t bytes)
            {
                while (bytes && more())
                {
                    const uint32_t take = std::min(bytes, segs[seg].second - offset);
                    offset += take, bytes -= take, position += take;
                    if (offset >= segs[seg].second)
                        ++seg, offset = 0;
                }
                while (more() && offset >= segs[seg].second)
                    ++seg, offset = 0;
            }
            void align(uint32_t to)
            {
                if (position % to)
                    skip(uint32_t(to - position % to));
            }
        };

        // VIF1's codes: DIRECT / DIRECTHL carry GIF data; the others' sizes are skipped.
        void readVif(const uint8_t *ram, const std::vector<std::pair<uint32_t, uint32_t>> &segs, GifReader &gif)
        {
            Stream s{segs};
            s.skip(0);
            for (int n = 0; n < 1 << 22 && s.more(); ++n)
            {
                if (s.segs[s.seg].first == kPadding || s.segs[s.seg].second - s.offset < 4u)
                {
                    s.skip(s.segs[s.seg].second - s.offset);
                    continue;
                }
                uint32_t code;
                std::memcpy(&code, at(ram, s.address()), 4);
                s.skip(4);
                const uint32_t cmd = (code >> 24) & 0x7Fu, imm = code & 0xFFFFu, num = (code >> 16) & 0xFFu;
                if (cmd == 0x50u || cmd == 0x51u)
                {
                    s.align(16);
                    const uint32_t qwords = imm ? imm : 65536u;
                    for (uint32_t q = 0; q < qwords && s.more(); ++q)
                    {
                        if (s.segs[s.seg].second - s.offset >= 16u)
                            gif.qword(ram, s.address());
                        s.skip(16);
                    }
                }
                else if (cmd == 0x4Au)
                {
                    s.align(8);
                    s.skip((num ? num : 256u) * 8u);
                }
                else if (cmd >= 0x60u)
                {
                    const uint32_t vl = cmd & 3u, vn = (cmd >> 2) & 3u, elements = num ? num : 256u;
                    const uint32_t bits = (32u >> vl) * (vn + 1u) * elements;
                    s.skip((bits + 31u) / 32u * 4u);
                }
                else if (cmd == 0x20u)
                    s.skip(4);
                else if (cmd == 0x30u || cmd == 0x31u)
                    s.skip(16);
                else if (cmd > 0x17u)
                {
                    gif.vifStop = code ? code : 1u;
                    return; // not a VIF code: stop rather than misread
                }
            }
        }

        void observeSend(const uint8_t *ram, bool vif1, uint32_t data, uint32_t qwc, bool chain, bool tte)
        {
            std::vector<std::pair<uint32_t, uint32_t>> segs;
            if (chain)
                chainSegments(ram, data, tte, segs);
            else
                segs.push_back({data & ~15u, qwc * 16u});
            GifReader reader;
            if (vif1)
                readVif(ram, segs, reader);
            static int traced = 0;
            if (traceUploads && traced < 40)
            {
                ++traced;
                uint64_t bytes = 0;
                for (const auto &sg : segs)
                    bytes += sg.second;
                std::fprintf(stderr, "[jc2:upload] %s %s from %#x: %zu segments, %llu bytes, %llu GIF qwords, VIF stop %#x\n",
                             vif1 ? "VIF1" : "GIF", chain ? "chain" : "normal", data, segs.size(),
                             (unsigned long long)bytes, (unsigned long long)reader.qwords, reader.vifStop);
            }
            else
            {
                Stream s{segs};
                for (int n = 0; n < 1 << 20 && s.more(); ++n)
                {
                    if (s.segs[s.seg].second - s.offset >= 16u)
                        reader.qword(ram, s.address());
                    s.skip(16);
                }
            }
        }

        // A started VIF1 (1) or GIF (2) channel (BozoEngine's Libraries::dmaStarted, before the I/O library completes
        // it): CHCR's MOD 1 is a source chain from TADR (TTE: the tags' upper halves go first), else QWC from MADR.
        uint8_t *g_uploadRam = nullptr;
        void dmaStarted(uint32_t channel, const bozo::libs::IoState::Channel &c)
        {
            if (!g_uploadRam || (channel != 1u && channel != 2u))
                return;
            const bool chain = ((c.chcr >> 2) & 3u) == 1u, tte = (c.chcr & 0x40u) != 0;
            if (!chain && c.qwc)
                observeSend(g_uploadRam, channel == 1u, c.madr, c.qwc, false, false);
            else if (chain)
                observeSend(g_uploadRam, channel == 1u, c.tadr, 0, true, tte);
        }

        // A record's picture from the uploads into its page: the record's place and size in TBP's buffer (TBW wide),
        // the newest transfers over it last; indexed formats through the CLUT uploaded at CBP.
        struct GsPicture
        {
            TextureView view;
            std::vector<uint64_t> serials;
        };
        std::map<uint32_t, GsPicture> g_gsPictures; // by record
        const TextureView *gsPicture(const uint8_t *ram, uint32_t record, uint64_t texa)
        {
            if (record < 0x100000u || record + 0x60u > 0x2000000u || g_uploads.empty())
                return nullptr;
            const uint64_t tex0 = r64(ram, record + 0x20);
            const uint32_t tbp = uint32_t(tex0) & 0x3FFFu, psm = uint32_t(tex0 >> 20) & 0x3Fu;
            const uint32_t cbp = uint32_t(tex0 >> 37) & 0x3FFFu, cpsm = uint32_t(tex0 >> 51) & 15u;
            const uint32_t csa = uint32_t(tex0 >> 56) & 31u;
            const uint32_t place = r32(ram, record + 0x10), size = r32(ram, record + 0x14);
            const uint32_t px = place & 0xFFFFu, py = place >> 16, w = size & 0xFFFFu, h = size >> 16;
            const uint32_t bpp = bitsPerPixel(psm);
            if (!bpp || !w || !h || w > 1024u || h > 1024u)
                return nullptr;
            // The transfers that cover the picture, and the CLUT's.
            std::vector<const Upload *> pixels, clut;
            for (const Upload &u : g_uploads)
            {
                if (u.dbp == tbp && u.psm == psm && u.x < px + w && u.x + u.w > px && u.y < py + h && u.y + u.h > py)
                    pixels.push_back(&u);
                if ((psm == 0x13u || psm == 0x14u) && u.dbp == cbp && (u.psm == 0u || u.psm == 2u || u.psm == 0x0Au))
                    clut.push_back(&u);
            }
            if (pixels.empty() || ((psm == 0x13u || psm == 0x14u) && clut.empty()))
                return nullptr;
            std::vector<uint64_t> serials;
            for (const Upload *u : pixels)
                serials.push_back(u->serial);
            for (const Upload *u : clut)
                serials.push_back(u->serial);
            GsPicture &g = g_gsPictures[record];
            if (g.view.texture && g.serials == serials && g.view.key == (0x3A00000000000000ull | record))
                return &g.view;

            // A transfer's pixel n (row-major in its rectangle), read from its spans.
            auto byteAt = [&](const Upload &u, uint64_t offset) -> const uint8_t * {
                for (const auto &[a, n] : u.spans)
                {
                    if (offset < n)
                        return at(ram, a + uint32_t(offset));
                    offset -= n;
                }
                return nullptr;
            };
            auto rawAt = [&](const Upload &u, uint32_t x, uint32_t y) -> uint32_t {
                const uint64_t n = uint64_t(y - u.y) * u.w + (x - u.x);
                const uint32_t b = bitsPerPixel(u.psm);
                const uint8_t *p = byteAt(u, n * b / 8u);
                if (!p)
                    return 0;
                switch (b)
                {
                case 4: return (n & 1u) ? (p[0] >> 4) : (p[0] & 15u);
                case 8: return p[0];
                case 16: return uint32_t(p[0]) | uint32_t(p[1]) << 8;
                case 24: return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
                default: return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
                }
            };
            const uint32_t ta0 = uint32_t(texa) & 0xFFu, ta1 = uint32_t(texa >> 32) & 0xFFu, aem = uint32_t(texa >> 15) & 1u;
            auto colour = [&](uint32_t raw, uint32_t format) -> uint32_t { // to RGBA, alpha 0x80 opaque
                if (format == 0u)
                    return raw;
                if (format == 1u)
                    return raw | ((aem && !(raw & 0xFFFFFFu)) ? 0u : ta0) << 24;
                const uint32_t r = (raw & 31u) << 3, gg = ((raw >> 5) & 31u) << 3, b = ((raw >> 10) & 31u) << 3;
                const uint32_t a = (raw & 0x8000u) ? ta1 : ((aem && !(raw & 0x7FFFu)) ? 0u : ta0);
                return r | gg << 8 | b << 16 | a << 24;
            };
            // The CLUT: 16 entries (8 x 2) or 256 (16 x 16, CSM1's order); CSA picks a 16-entry block.
            std::vector<uint32_t> palette;
            if (psm == 0x13u || psm == 0x14u)
            {
                const uint32_t entries = psm == 0x14u ? 16u : 256u;
                palette.assign(entries, 0u);
                for (uint32_t i = 0; i < entries; ++i)
                {
                    const uint32_t e = psm == 0x14u ? csa * 16u + i : i;
                    uint32_t cx, cy;
                    bozo::render::csm1Position(e, cx, cy);
                    if (psm == 0x14u && clut.back()->w == 8u)
                        cx = i & 7u, cy = i >> 3; // an 8 x 2 CLUT of its own
                    for (const Upload *u : clut)
                        if (cx >= u->x && cx < u->x + u->w && cy >= u->y && cy < u->y + u->h)
                            palette[i] = colour(rawAt(*u, cx, cy), u->psm == 0u ? 0u : 2u);
                }
            }
            auto texture = std::make_shared<bozo::render::Texture>();
            texture->width = w, texture->height = h;
            texture->rgba.assign(size_t(w) * h, 0u);
            texture->key = 0x3A00000000000000ull | record;
            texture->version = g.view.texture ? g.view.texture->version + 1 : 1;
            for (const Upload *u : pixels)
                for (uint32_t y = std::max(py, u->y); y < std::min(py + h, u->y + u->h); ++y)
                    for (uint32_t x = std::max(px, u->x); x < std::min(px + w, u->x + u->w); ++x)
                    {
                        const uint32_t raw = rawAt(*u, x, y);
                        uint32_t c = (psm == 0x13u || psm == 0x14u) ? (raw < palette.size() ? palette[raw] : 0u)
                                                                    : colour(raw, psm == 0u ? 0u : psm == 1u ? 1u : 2u);
                        const uint32_t a = std::min<uint32_t>(255u, (c >> 24) * 255u / 128u);
                        texture->rgba[size_t(y - py) * w + (x - px)] = (c & 0xFFFFFFu) | a << 24;
                    }
            g.serials = std::move(serials);
            g.view = TextureView{};
            g.view.texture = texture;
            g.view.key = texture->key;
            // The record's place is in its page; the picture fills it (su, sv: page size over picture size).
            const float pw = float(1u << ((tex0 >> 26) & 15u)), ph = float(1u << ((tex0 >> 30) & 15u));
            g.view.su = pw / float(w), g.view.ou = -float(px) / float(w);
            g.view.sv = ph / float(h), g.view.ov = -float(py) / float(h);
            return &g.view;
        }

        // A record outside the banks (an effect's own, made when it uploads a picture): the picture of a loaded
        // resource's TIM2 with the same size and the same TEX0 but for where it went (TBP, TBW, CBP, CSA: a picture's
        // header keeps its file's addresses). The forests' screen sparkles (0x22E1C8) use one.
        std::map<uint32_t, TextureView> g_uploadedViews; // by record; cleared with the loaded resources' changes
        size_t g_uploadedLoaded = 0;
        const TextureView *uploadedPicture(const uint8_t *ram, uint32_t record)
        {
            if (record < 0x100000u || record + 0x60u > 0x2000000u)
                return nullptr;
            if (g_uploadedLoaded != g_loaded.size())
                g_uploadedViews.clear(), g_uploadedLoaded = g_loaded.size();
            auto found = g_uploadedViews.find(record);
            if (found != g_uploadedViews.end())
                return found->second.texture ? &found->second : nullptr;
            TextureView &view = g_uploadedViews[record];
            const uint64_t kWhere = 0x3FFFull | 0x3Full << 14 | 0x3FFFull << 37 | 0x1Full << 56; // TBP TBW CBP CSA
            const uint64_t tex0 = r64(ram, record + 0x20) & ~kWhere;
            const uint32_t size = r32(ram, record + 0x14);
            // Only a single candidate: pictures of the same format and size are common (the Wind Forest has five for
            // its sparkles' record, with different palettes), and the wrong one is worse than none.
            uint32_t match = 0, matches = 0;
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
                    for (const uint32_t picture : tim2Pictures(ram, sub))
                        if (r32(ram, picture + 20) == size && (r64(ram, picture + 0x18) & ~kWhere) == tex0)
                            match = picture, ++matches;
                }
            }
            if (matches != 1)
                return nullptr;
            view.key = 0x3B00000000000000ull | match;
            view.texture = decodeTim2Picture(at(ram, match), 0, view.key);
            if (!view.texture)
                return nullptr;
            // The record's place is in its page; the picture fills it (su, sv: page size over picture size).
            const float pw = float(1u << ((r64(ram, record + 0x20) >> 26) & 15u));
            const float ph = float(1u << ((r64(ram, record + 0x20) >> 30) & 15u));
            const uint32_t place = r32(ram, record + 0x10);
            const float w = float(view.texture->width), h = float(view.texture->height);
            view.su = pw / w, view.ou = -float(place & 0xFFFFu) / w;
            view.sv = ph / h, view.ov = -float(place >> 16) / h;
            return &view;
        }

        // ---- Program D into a texture page ----
        // 0x4314E0's t0, when set, is a render target (0x4326D0 writes it as GS state): +0 FBP, +4 FBW, +8 PSM, +0xC /
        // +0x10 XYOFFSET (12.4), +0x14 / +0x18 / +0x1C / +0x20 SCISSOR x0, y0, x1, y1, +0x30..+0x3C the clear colour (a
        // sprite over the target before the draw). The Wind Forest draws its dust motes into page 0x3800 (64 x 64)
        // that way and its light spots sample the page. Native: the target is a small picture made on the CPU from
        // the draw's quads (no GS memory), kept as the page's texture for the records that sample it.
        struct RenderedPage
        {
            uint32_t width = 0, height = 0;
            std::vector<uint8_t> rgba; // GS values (alpha 0x80 opaque)
            std::shared_ptr<bozo::render::Texture> texture;
        };
        std::map<uint32_t, RenderedPage> g_renderedPages; // by TBP (FBP x 32)
        std::map<uint32_t, TextureView> g_renderedViews;  // by record

        const TextureView *renderedPicture(const uint8_t *ram, uint32_t record)
        {
            const uint64_t tex0 = r64(ram, record + 0x20);
            auto page = g_renderedPages.find(uint32_t(tex0) & 0x3FFFu);
            if (page == g_renderedPages.end() || !page->second.texture)
                return nullptr;
            TextureView &view = g_renderedViews[record];
            view.texture = page->second.texture;
            view.key = page->second.texture->key;
            // The record's place in the page; the picture is the whole target.
            const float pw = float(1u << ((tex0 >> 26) & 15u)), ph = float(1u << ((tex0 >> 30) & 15u));
            const float w = float(page->second.width), h = float(page->second.height);
            view.su = pw / w, view.ou = 0, view.sv = ph / h, view.ov = 0;
            return &view;
        }

        // A triangle into a target: ST interpolated (Q 1), the texture nearest and clamped, modulated by the vertex
        // colour (0x80 = 1), the alpha test, then ALPHA's ((A - B) C >> 7) + D; the written alpha is the source's.
        struct TargetVertex
        {
            float x, y, u, v;
            uint32_t rgba;
        };
        void rasterTarget(RenderedPage &t, const uint32_t scissor[4], const TargetVertex tri[3],
                          const bozo::render::Texture *texture, const bozo::render::Material &m)
        {
            const float area = (tri[1].x - tri[0].x) * (tri[2].y - tri[0].y) - (tri[2].x - tri[0].x) * (tri[1].y - tri[0].y);
            if (std::fabs(area) < 1e-6f)
                return;
            const int x0 = std::max<int>(int(scissor[0]), int(std::floor(std::min({tri[0].x, tri[1].x, tri[2].x}))));
            const int x1 = std::min<int>(int(scissor[2]), int(std::ceil(std::max({tri[0].x, tri[1].x, tri[2].x}))));
            const int y0 = std::max<int>(int(scissor[1]), int(std::floor(std::min({tri[0].y, tri[1].y, tri[2].y}))));
            const int y1 = std::min<int>(int(scissor[3]), int(std::ceil(std::max({tri[0].y, tri[1].y, tri[2].y}))));
            for (int y = y0; y <= y1 && y < int(t.height); ++y)
                for (int x = x0; x <= x1 && x < int(t.width); ++x)
                {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w0 = ((tri[1].x - px) * (tri[2].y - py) - (tri[2].x - px) * (tri[1].y - py)) / area;
                    const float w1 = ((tri[2].x - px) * (tri[0].y - py) - (tri[0].x - px) * (tri[2].y - py)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0)
                        continue;
                    uint32_t src[4];
                    for (int c = 0; c < 4; ++c)
                        src[c] = (tri[0].rgba >> (8 * c)) & 0xFFu; // flat: the strip's last vertex colours it
                    if (texture && texture->width && texture->height)
                    {
                        const float u = w0 * tri[0].u + w1 * tri[1].u + w2 * tri[2].u;
                        const float v = w0 * tri[0].v + w1 * tri[1].v + w2 * tri[2].v;
                        const int tx = std::clamp(int(u * texture->width), 0, int(texture->width) - 1);
                        const int ty = std::clamp(int(v * texture->height), 0, int(texture->height) - 1);
                        const uint32_t texel = texture->rgba[size_t(ty) * texture->width + tx];
                        for (int c = 0; c < 3; ++c)
                            src[c] = std::min<uint32_t>(255u, ((texel >> (8 * c)) & 0xFFu) * src[c] >> 7);
                        const uint32_t ta = ((texel >> 24) * 128u + 127u) / 255u; // back to the GS's 0..0x80
                        src[3] = ta * src[3] >> 7;
                    }
                    if (m.alphaTest && src[3] < m.alphaRef)
                        continue;
                    uint8_t *dst = &t.rgba[(size_t(y) * t.width + x) * 4];
                    const uint32_t as = src[3];
                    auto pick = [&](uint32_t which, int c) -> int {
                        return which == 0 ? int(src[c]) : which == 1 ? int(dst[c]) : 0;
                    };
                    const int coef = m.blendC == 0 ? int(as) : m.blendC == 1 ? int(dst[3]) : int(m.blendFix);
                    for (int c = 0; c < 3; ++c)
                    {
                        const int value = (((pick(m.blendA, c) - pick(m.blendB, c)) * coef) >> 7) + pick(m.blendD, c);
                        dst[c] = static_cast<uint8_t>(std::clamp(value, 0, 255));
                    }
                    dst[3] = static_cast<uint8_t>(std::min<uint32_t>(as, 255u));
                }
        }

        // The batches program D's path takes per mode (0x432170: VU 53.x = the object's +0x30): vertices per batch
        // (each batch its own strip) and qwords per vertex.
        bool programDShape(uint32_t mode, const uint8_t *ram, uint32_t object, uint32_t &batch, uint32_t &qwords)
        {
            static const struct
            {
                uint32_t bit, batch, qwords;
            } kShapes[] = {{0x1, 42, 2},   {0x2, 42, 2},   {0x4, 34, 2},   {0x8, 34, 2},   {0x10, 25, 2},
                           {0x2000, 25, 2}, {0x20, 25, 3},  {0x40, 25, 2},  {0x80, 20, 3},  {0x100, 40, 2},
                           {0x200, 0, 2},   {0x400, 25, 2}, {0x800, 20, 3}, {0x1000, 25, 3}};
            for (const auto &s : kShapes)
                if (mode & s.bit)
                {
                    batch = s.bit == 0x200 ? r32(ram, object + 0x34) : s.batch; // 0x200: the whole buffer at once
                    qwords = s.qwords;
                    return true;
                }
            return false;
        }

        void programD(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            // 0x4314E0(object, position, texture animation, GS parameters, t0, camera index; f12 scale). Read before the
            // routine runs: it advances the animation and builds the game's own packets.
            const uint32_t object = ctx->r[4].uw[0], position = ctx->r[5].uw[0], anim = ctx->r[6].uw[0];
            const uint32_t gs = ctx->r[7].uw[0], cameraIndex = ctx->r[9].uw[0] & 3u, caller = ctx->r[31].uw[0];
            // t0: a render target, or 0. A frame-sized one (FBW over 4: the 640-wide display buffers) is the screen
            // given back explicitly (the Wind Forest's light spots pass it after drawing into page 0x3800).
            uint32_t target = ctx->r[8].uw[0];
            if (target && (target < 0x100000u || target + 0x40u > 0x2000000u || (r32(ram, target + 4) & 0x3Fu) > 4u))
                target = 0;
            const float scale = ctx->f[12];
            const bool valid = object >= 0x100000u && object + 0x44u <= 0x2000000u && position >= 0x100000u &&
                               position + 16u <= 0x2000000u;
            uint32_t mode = 0, count = 0, buffer = 0, record = 0;
            float pos[3] = {0, 0, 0};
            uint32_t gsWords[3] = {0, 0, 0};
            if (valid)
            {
                mode = r32(ram, object + 0x30), count = r32(ram, object + 0x34), buffer = r32(ram, object);
                for (int k = 0; k < 3; ++k)
                    pos[k] = rf(ram, position + 4u * k);
                if (anim >= 0x100000u && anim + 0x18u <= 0x2000000u && r32(ram, anim + 0x14) == 0)
                    record = r32(ram, anim + 0x10) + 0x60u * r32(ram, anim);
                if (gs >= 0x100000u && gs + 12u <= 0x2000000u)
                    for (int k = 0; k < 3; ++k)
                        gsWords[k] = r32(ram, gs + 4u * k);
            }
            rt->functions.generated(kProgramD)(ram, ctx, rt);
            const uint32_t camera = r32(ram, 0x006CC5F4u + 4u * cameraIndex);
            if (meshesOff || !valid || !camera || !count || buffer < 0x100000u)
                return;

            static const bool traceProgramD = std::getenv("JC2_TRACE_PROGRAM_D") != nullptr;
            uint32_t batchSize = 0, qwords = 0;
            const bool known = programDShape(mode, ram, object, batchSize, qwords);
            // Native: modes 2 (strips: the water), 0x40 (world quads) and 0x400 (screen quads). Others are listed once.
            const bool native =
                known && batchSize &&
                (mode == 2u || mode == 0x10u || mode == 0x20u || mode == 0x40u || mode == 0x80u || mode == 0x400u ||
                 mode == 0x1000u);
            auto notDrawn = [&](const char *why) {
                static std::set<uint64_t> seen;
                if (seen.size() < 64 && seen.insert(uint64_t(mode) << 32 | caller).second)
                    std::fprintf(stderr, "[jc2:programD] not drawn (%s): mode %#x from %#x, %u vertices at %#x (%u a batch, "
                                         "%u qwords), record %#x, GS %#x %#x %#x\n",
                                 why, mode, caller, count, buffer, batchSize, qwords, record, gsWords[0], gsWords[1],
                                 gsWords[2]);
            };
            if (!native)
            {
                notDrawn("mode not translated yet");
                return;
            }
            if (target && mode != 0x400u)
            {
                notDrawn("render target not translated for this mode");
                return;
            }
            if (buffer + size_t(count) * qwords * 16u > 0x2000000u)
                return;

            // The texture: the record's IPU animation, a bank record (or a copy of one), or the animation page.
            ScreenDraw draw;
            const TextureView *view = nullptr;
            const char *source = "none";
            if (record)
            {
                draw.texture = waterTexture(ram, record);
                source = "IPU";
                if (!draw.texture)
                {
                    view = renderedPicture(ram, record), source = "rendered";
                    if (!view)
                        view = residentRecord(ram, record), source = "bank";
                    if (!view && record >= 0x100000u && record + 0x60u <= 0x2000000u)
                        view = residentCopy(ram, record), source = "bank copy";
                    if (!view)
                    {
                        const uint64_t texa = gs && r32(ram, gs + 0x18) == 0x3Bu ? r64(ram, gs + 0x10) : 0x80ull;
                        view = gsPicture(ram, record, texa), source = "upload";
                    }
                    if (!view)
                        view = uploadedPicture(ram, record), source = "TIM2";
                    if (view)
                        draw.texture = view->texture;
                }
                // A page drawn by the game (the Wind Forest's light spots sample its dust motes' target): the result is
                // a haze over the whole view, not yet checked against the console's; JC2_RENDERED_EFFECTS=1 draws it.
                static const bool renderedEffects = [] {
                    const char *v = std::getenv("JC2_RENDERED_EFFECTS");
                    return v && v[0] == '1';
                }();
                if (view && !std::strcmp(source, "rendered") && !renderedEffects)
                {
                    notDrawn("samples a rendered page (JC2_RENDERED_EFFECTS=1 to draw)");
                    return;
                }
                if (!draw.texture && (r64(ram, record + 0x20) & 0x3FFFu) == kAnimationPage)
                    draw.texture = g_animationPage;
                if (!draw.texture)
                {
                    notDrawn("no texture");
                    return;
                }
            }
            // The record's picture in its page, as the program gets it (0x431C90: VU 55 = place / page size, VU 54 =
            // (place + size) / page size); a bank view then maps page coordinates to its picture.
            float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
            if (record)
            {
                const uint64_t tex0 = r64(ram, record + 0x20);
                const float pw = float(1u << ((tex0 >> 26) & 15u)), ph = float(1u << ((tex0 >> 30) & 15u));
                const uint32_t place = r32(ram, record + 0x10), size = r32(ram, record + 0x14);
                u0 = float(place & 0xFFFFu) / pw, v0 = float(place >> 16) / ph;
                u1 = u0 + float(size & 0xFFFFu) / pw, v1 = v0 + float(size >> 16) / ph;
            }
            auto uv = [&](float u, float t, bozo::render::Vertex &out) {
                if (view)
                    u = u * view->su + view->ou, t = t * view->sv + view->ov;
                out.u = u, out.v = t;
            };

            // GS state from the parameters: ALPHA at +0 (FIX in its high word), TEST at +0x20 (A+D pairs).
            bozo::render::Material &m = draw.material;
            m.textured = draw.texture != nullptr;
            m.directColour = !m.textured;
            m.clampU = m.clampV = mode != 2u;
            m.gsBlend = true;
            const uint32_t abcd = gsWords[0] & 0xFFu;
            m.blendA = abcd & 3u, m.blendB = (abcd >> 2) & 3u, m.blendC = (abcd >> 4) & 3u, m.blendD = (abcd >> 6) & 3u;
            m.blendFix = static_cast<uint8_t>(gsWords[1]);
            m.depthTest = true, m.depthWrite = false;
            if (gs && r32(ram, gs + 0x28) == 0x47u)
            {
                const uint32_t test = r32(ram, gs + 0x20);
                const uint32_t zte = (test >> 16) & 1u, ztst = (test >> 17) & 3u;
                m.depthTest = zte && ztst != 1u; // ALWAYS: no test
                const uint32_t ate = test & 1u, atst = (test >> 1) & 7u, aref = (test >> 4) & 0xFFu;
                if (ate && (atst == 5u || atst == 6u))
                {
                    m.alphaTest = true;
                    m.alphaFunc = bozo::render::Material::AlphaFunc::GreaterEqual;
                    m.alphaRef = static_cast<uint8_t>(std::min<uint32_t>(255u, aref + (atst == 6u ? 1u : 0u)));
                }
            }
            if (m.textured)
                m.tex0 = draw.texture->key;

            Mat world{};
            world.m[0] = world.m[5] = world.m[10] = scale, world.m[15] = 1.0f;
            world.m[12] = pos[0], world.m[13] = pos[1], world.m[14] = pos[2];
            const Mat view4 = readMat(ram, camera + 0x130), projection = readMat(ram, camera + 0x170);
            const Mat pvw = mul(projection, mul(view4, world)), cw = mul(readMat(ram, camera + 0x230), world);
            auto colourAt = [&](uint32_t q) {
                uint32_t c = 0;
                for (int k = 0; k < 4; ++k)
                    c |= static_cast<uint32_t>(std::clamp(rf(ram, q + 4u * k), 0.0f, 255.0f)) << (8 * k);
                return c;
            };

            if (mode == 2u)
            {
                // JC2_WATER_DY: a test offset of the surface's height (y down), to compare with the room's.
                static const float yOffset = [] {
                    const char *v = std::getenv("JC2_WATER_DY");
                    return v ? std::strtof(v, nullptr) : 0.0f;
                }();
                for (uint32_t first = 0; first < count; first += batchSize)
                {
                    const uint32_t n = std::min(batchSize, count - first);
                    bozo::render::Vertex v[2];
                    bool ok[2] = {false, false};
                    for (uint32_t k = 0; k < n; ++k)
                    {
                        const uint32_t q = buffer + (first + k) * qwords * 16u;
                        const float p[3] = {rf(ram, q + 16), rf(ram, q + 20) + yOffset, rf(ram, q + 24)};
                        float s[3], wq;
                        bozo::render::Vertex out;
                        const bool inside = insideClip(cw, p) && project(pvw, p, s, wq);
                        if (inside)
                        {
                            out.x = s[0], out.y = s[1], out.z = s[2], out.rgba = 0x80808080u;
                            uv(rf(ram, q), rf(ram, q + 4), out);
                        }
                        // A strip: each vertex closes a triangle with the two before it (dropped if one is outside).
                        if (k >= 2 && inside && ok[0] && ok[1])
                            draw.vertices.insert(draw.vertices.end(), {v[0], v[1], out});
                        v[0] = v[1], ok[0] = ok[1];
                        v[1] = out, ok[1] = inside;
                    }
                }
            }
            else if (mode == 0x20u || mode == 0x10u)
            {
                // Path 0x1240: per element (x, y, z, size), colour, (cos, sin); a camera-facing quad, p -+ a s -+ b s
                // with a = VU 28 (the view's y row, negated), b = VU 27 x a (VU 27: its z row), turned on screen about
                // its centre by the angle (in the field: y stretched by 2.14393 for the turn, then back); ST as path
                // 0x1630; dropped whole when a corner is outside the clip volume. Path 0xC50 (mode 0x10): the same
                // quad from (x, y, z, size), colour, not turned.
                const bool turned = mode == 0x20u;
                const uint32_t stride = turned ? 48u : 32u;
                const float az[3] = {view4.m[2], view4.m[6], view4.m[10]};
                const float a[3] = {-view4.m[1], -view4.m[5], -view4.m[9]};
                const float b[3] = {az[1] * a[2] - az[2] * a[1], az[2] * a[0] - az[0] * a[2], az[0] * a[1] - az[1] * a[0]};
                const float st[4][2] = {{u1, v1}, {u1, v0}, {u0, v1}, {u0, v0}};
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t q = buffer + i * stride;
                    const float p[3] = {rf(ram, q), rf(ram, q + 4), rf(ram, q + 8)}, s = rf(ram, q + 12);
                    const float c = turned ? rf(ram, q + 32) : 1.0f, sn = turned ? rf(ram, q + 36) : 0.0f;
                    const uint32_t col = colourAt(q + 16);
                    // Corners 15, 16, 19, 20: -a -b, +a -b, -a +b, +a +b.
                    const float sa[4] = {-1, 1, -1, 1}, sb[4] = {-1, -1, 1, 1};
                    float scr[4][3];
                    bool ok = true;
                    for (int k = 0; k < 4 && ok; ++k)
                    {
                        float w4[3];
                        for (int d = 0; d < 3; ++d)
                            w4[d] = p[d] + sa[k] * a[d] * s + sb[k] * b[d] * s;
                        float wq = 0;
                        ok = insideClip(cw, w4) && project(pvw, w4, scr[k], wq);
                        if (ok)
                            scr[k][1] = (scr[k][1] + 2048.0f) * 0.5f; // the frame's y back to the field's
                    }
                    if (!ok)
                        continue;
                    const float cx = (scr[1][0] + scr[2][0]) * 0.5f, cy = (scr[1][1] + scr[2][1]) * 0.5f;
                    auto turn = [&](const float *v, float out[2]) {
                        const float x = v[0] - cx, y = (v[1] - cy) * 2.14393f;
                        out[0] = x * sn - y * c, out[1] = (x * c + y * sn) * 0.466434f;
                    };
                    float r0[2], r1[2];
                    turn(scr[0], r0), turn(scr[1], r1);
                    float xy[4][2] = {{cx + r0[0], cy + r0[1]}, {cx + r1[0], cy + r1[1]}, {cx - r1[0], cy - r1[1]},
                                      {cx - r0[0], cy - r0[1]}};
                    if (!turned)
                        for (int k = 0; k < 4; ++k)
                            xy[k][0] = scr[k][0], xy[k][1] = scr[k][1];
                    bozo::render::Vertex out[4];
                    for (int k = 0; k < 4; ++k)
                    {
                        out[k].x = xy[k][0], out[k].y = 2.0f * xy[k][1] - 2048.0f, out[k].z = scr[k][2], out[k].rgba = col;
                        uv(st[k][0], st[k][1], out[k]);
                    }
                    draw.vertices.insert(draw.vertices.end(), {out[0], out[1], out[2], out[1], out[2], out[3]});
                }
            }
            else if (mode == 0x80u)
            {
                // Path 0x1C30: per element colour, a direction t, (x, y, z, width); a ribbon, two vertices an element:
                // p + d with ST (u0, v0), p - d with (u1, v0), d = (VU 27 x t) width (VU 27: the view's z row); one
                // strip a batch, a vertex outside the clip volume closing no triangle.
                const float az[3] = {view4.m[2], view4.m[6], view4.m[10]};
                for (uint32_t first = 0; first < count; first += batchSize)
                {
                    const uint32_t n = std::min(batchSize, count - first);
                    bozo::render::Vertex v[2];
                    bool ok[2] = {false, false};
                    uint32_t k = 0;
                    for (uint32_t e = 0; e < n; ++e)
                    {
                        const uint32_t q = buffer + (first + e) * 48u;
                        const uint32_t col = colourAt(q);
                        const float t[3] = {rf(ram, q + 16), rf(ram, q + 20), rf(ram, q + 24)};
                        const float p[3] = {rf(ram, q + 32), rf(ram, q + 36), rf(ram, q + 40)}, w = rf(ram, q + 44);
                        const float d[3] = {(az[1] * t[2] - az[2] * t[1]) * w, (az[2] * t[0] - az[0] * t[2]) * w,
                                            (az[0] * t[1] - az[1] * t[0]) * w};
                        for (int side = 0; side < 2; ++side, ++k)
                        {
                            const float sgn = side ? -1.0f : 1.0f;
                            const float pt[3] = {p[0] + sgn * d[0], p[1] + sgn * d[1], p[2] + sgn * d[2]};
                            float s[3] = {0, 0, 0}, wq = 0;
                            bozo::render::Vertex out;
                            const bool inside = insideClip(cw, pt) && project(pvw, pt, s, wq);
                            if (inside)
                            {
                                out.x = s[0], out.y = s[1], out.z = s[2], out.rgba = col;
                                uv(side ? u1 : u0, v0, out);
                            }
                            if (k >= 2 && inside && ok[0] && ok[1])
                                draw.vertices.insert(draw.vertices.end(), {v[0], v[1], out});
                            v[0] = v[1], ok[0] = ok[1];
                            v[1] = out, ok[1] = inside;
                        }
                    }
                }
            }
            else if (mode == 0x1000u)
            {
                // Path 0x18F8: per element (x, y, z, size), colour, (cos, sin); the flat x z quad of path 0x1630 turned
                // about y: p - u, p + w, p - w, p + u with u = (h sin, 0, -h cos), w = (h cos, 0, h sin), h = size / 2;
                // ST as path 0x1630; dropped whole when a corner is outside the clip volume.
                const float st[4][2] = {{u1, v1}, {u1, v0}, {u0, v1}, {u0, v0}};
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t q = buffer + i * 48u;
                    const float x = rf(ram, q), y = rf(ram, q + 4), z = rf(ram, q + 8), h = rf(ram, q + 12) * 0.5f;
                    const float c = rf(ram, q + 32), sn = rf(ram, q + 36);
                    const float u[3] = {h * sn, 0, -h * c}, w[3] = {h * c, 0, h * sn};
                    const float corners[4][3] = {{x - u[0], y, z - u[2]}, {x + w[0], y, z + w[2]}, {x - w[0], y, z - w[2]},
                                                 {x + u[0], y, z + u[2]}};
                    const uint32_t col = colourAt(q + 16);
                    bozo::render::Vertex out[4];
                    bool ok = true;
                    for (int k = 0; k < 4 && ok; ++k)
                    {
                        float s[3] = {0, 0, 0}, wq = 0;
                        ok = insideClip(cw, corners[k]) && project(pvw, corners[k], s, wq);
                        out[k].x = s[0], out[k].y = s[1], out[k].z = s[2], out[k].rgba = col;
                        uv(st[k][0], st[k][1], out[k]);
                    }
                    if (ok)
                        draw.vertices.insert(draw.vertices.end(), {out[0], out[1], out[2], out[1], out[2], out[3]});
                }
            }
            else if (mode == 0x40u)
            {
                // Path 0x1630: per element (x, y, z, size), colour; a quad flat in x z: p - (s/2, 0, 0), p + (0, 0, s/2),
                // p - (0, 0, s/2), p + (s/2, 0, 0) with ST (u1, v1), (u1, v0), (u0, v1), (u0, v0); dropped whole when a
                // corner is outside the clip volume.
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t q = buffer + i * 32u;
                    const float x = rf(ram, q), y = rf(ram, q + 4), z = rf(ram, q + 8), h = rf(ram, q + 12) * 0.5f;
                    const float corners[4][3] = {{x - h, y, z}, {x, y, z + h}, {x, y, z - h}, {x + h, y, z}};
                    const float st[4][2] = {{u1, v1}, {u1, v0}, {u0, v1}, {u0, v0}};
                    const uint32_t c = colourAt(q + 16);
                    bozo::render::Vertex out[4];
                    bool ok = true;
                    for (int k = 0; k < 4 && ok; ++k)
                    {
                        float s[3] = {0, 0, 0}, wq = 0;
                        ok = insideClip(cw, corners[k]) && project(pvw, corners[k], s, wq);
                        out[k].x = s[0], out[k].y = s[1], out[k].z = s[2], out[k].rgba = c;
                        uv(st[k][0], st[k][1], out[k]);
                    }
                    if (ok)
                        draw.vertices.insert(draw.vertices.end(), {out[0], out[1], out[2], out[1], out[2], out[3]});
                }
            }
            else
            {
                // Path 0x21D8: per element (x, y, half width, half height) in GS coordinates, colour; a screen quad at Z
                // 0xFFFF: (x - w, y - h) (x + w, y - h) (x - w, y + h) (x + w, y + h) with ST (u0, v0), (u0, v1), (u1, v0),
                // (u1, v1); the field's y to the frame's. With a render target the quads go to the target's picture.
                if (target >= 0x100000u && target + 0x40u <= 0x2000000u)
                {
                    const uint32_t fbp = r32(ram, target) & 0x1FFu, fbw = r32(ram, target + 4) & 0x3Fu;
                    const float offX = float(r32(ram, target + 0xC)) / 16.0f, offY = float(r32(ram, target + 0x10)) / 16.0f;
                    const uint32_t scissor[4] = {r32(ram, target + 0x14), r32(ram, target + 0x18), r32(ram, target + 0x1C),
                                                 r32(ram, target + 0x20)};
                    if (!fbw || scissor[2] >= 1024u || scissor[3] >= 1024u)
                        return;
                    RenderedPage &page = g_renderedPages[fbp * 32u];
                    page.width = fbw * 64u, page.height = scissor[3] + 1u;
                    page.rgba.assign(size_t(page.width) * page.height * 4u, 0);
                    // The clear: the target's colour over its scissor.
                    for (uint32_t y = scissor[1]; y <= scissor[3] && y < page.height; ++y)
                        for (uint32_t x = scissor[0]; x <= scissor[2] && x < page.width; ++x)
                            for (int c = 0; c < 4; ++c)
                                page.rgba[(size_t(y) * page.width + x) * 4 + c] =
                                    static_cast<uint8_t>(r32(ram, target + 0x30 + 4u * c));
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const uint32_t q = buffer + i * 32u;
                        const float x = rf(ram, q) - offX, y = rf(ram, q + 4) - offY, hw = rf(ram, q + 8),
                                    hh = rf(ram, q + 12);
                        const uint32_t c = colourAt(q + 16);
                        const float corners[4][2] = {{x - hw, y - hh}, {x + hw, y - hh}, {x - hw, y + hh}, {x + hw, y + hh}};
                        const float st[4][2] = {{u0, v0}, {u0, v1}, {u1, v0}, {u1, v1}};
                        TargetVertex tv[4];
                        for (int k = 0; k < 4; ++k)
                        {
                            bozo::render::Vertex mapped;
                            uv(st[k][0], st[k][1], mapped);
                            tv[k] = {corners[k][0], corners[k][1], mapped.u, mapped.v, c};
                        }
                        const TargetVertex a[3] = {tv[0], tv[1], tv[2]}, b[3] = {tv[1], tv[2], tv[3]};
                        rasterTarget(page, scissor, a, draw.texture.get(), m);
                        rasterTarget(page, scissor, b, draw.texture.get(), m);
                    }
                    auto texture = std::make_shared<bozo::render::Texture>();
                    texture->width = page.width, texture->height = page.height;
                    texture->rgba.resize(size_t(page.width) * page.height);
                    for (size_t p = 0; p < texture->rgba.size(); ++p)
                    {
                        const uint8_t *s = &page.rgba[p * 4];
                        const uint32_t alpha = std::min<uint32_t>(255u, uint32_t(s[3]) * 255u / 128u);
                        texture->rgba[p] = uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | alpha << 24;
                    }
                    texture->key = 0x3900000000000000ull | fbp * 32u;
                    texture->version = page.texture ? page.texture->version + 1 : 1;
                    page.texture = texture;
                    return;
                }
                auto f = [](float y) { return 2.0f * y - 2048.0f; };
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t q = buffer + i * 32u;
                    const float x = rf(ram, q), y = rf(ram, q + 4), hw = rf(ram, q + 8), hh = rf(ram, q + 12);
                    const uint32_t c = colourAt(q + 16);
                    const float corners[4][2] = {{x - hw, y - hh}, {x + hw, y - hh}, {x - hw, y + hh}, {x + hw, y + hh}};
                    const float st[4][2] = {{u0, v0}, {u0, v1}, {u1, v0}, {u1, v1}};
                    bozo::render::Vertex out[4];
                    for (int k = 0; k < 4; ++k)
                    {
                        out[k].x = corners[k][0], out[k].y = f(corners[k][1]), out[k].z = 65535.0f, out[k].rgba = c;
                        uv(st[k][0], st[k][1], out[k]);
                    }
                    draw.vertices.insert(draw.vertices.end(), {out[0], out[1], out[2], out[1], out[2], out[3]});
                }
            }
            if (traceProgramD)
            {
                static std::set<uint32_t> traced;
                if (traced.size() < 16 && traced.insert(mode << 8 ^ caller).second)
                {
                    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
                    uint32_t a0 = 255, a1 = 0, ta0 = 255, ta1 = 0;
                    for (const auto &v : draw.vertices)
                        x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y),
                        a0 = std::min(a0, v.rgba >> 24), a1 = std::max(a1, v.rgba >> 24);
                    if (draw.texture)
                        for (uint32_t t : draw.texture->rgba)
                            ta0 = std::min(ta0, t >> 24), ta1 = std::max(ta1, t >> 24);
                    std::fprintf(stderr, "[jc2:programD] mode %#x from %#x: %u elements, record %#x, texture %s, %zu "
                                         "vertices drawn, depth %d, alpha test %d (%u); x %.0f..%.0f y %.0f..%.0f, vertex "
                                         "alpha %u..%u, texture alpha %u..%u (%s), uv %.3f..%.3f; position %.2f %.2f %.2f "
                                         "scale %.3f, target %#x\n",
                                 mode, caller, count, record, draw.texture ? "yes" : "no", draw.vertices.size(),
                                 m.depthTest, m.alphaTest, m.alphaRef, x0, x1, y0, y1, a0, a1, ta0, ta1, source, u0, u1, pos[0], pos[1],
                                 pos[2], scale, ctx->r[8].uw[0]);
                }
                // JC2_DUMP_EFFECT_TEXTURES=dir: each effect texture once, as raw RGBA (record_wxh.rgba).
                static const char *dumpDir = std::getenv("JC2_DUMP_EFFECT_TEXTURES");
                static std::set<uint32_t> dumped;
                if (dumpDir && draw.texture && dumped.insert(record).second)
                {
                    char name[512];
                    std::snprintf(name, sizeof name, "%s/%08x_%ux%u.rgba", dumpDir, record, draw.texture->width,
                                  draw.texture->height);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(draw.texture->rgba.data(), 4, draw.texture->rgba.size(), f);
                        std::fclose(f);
                    }
                }
                if (std::getenv("JC2_WATER_NODEPTH"))
                    m.depthTest = false;
            }
            if (!draw.vertices.empty())
            {
                ++g_effectDraws;
                g_effects.push_back(std::move(draw));
            }
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
                        "%llu without a texture), %llu projected passes, %llu effect draws\n",
                        (unsigned long long)g_frames, (unsigned long long)g_drawn, (unsigned long long)g_missing,
                        (unsigned long long)g_shadows, (unsigned long long)g_shadowCalls, (unsigned long long)g_shadowNoBit,
                        (unsigned long long)g_shadowCulled, (unsigned long long)g_shadowNoTexture,
                        (unsigned long long)g_projectedBatches, (unsigned long long)g_effectDraws);
        if (g_frames % 600 == 0 && std::getenv("JC2_TRACE_MEMORY"))
        {
            PROCESS_MEMORY_COUNTERS_EX m{};
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m));
            std::printf("[jc2:memory] frame %llu: %llu MB private; caches: %zu pictures, %zu textures, %zu projected, %zu "
                        "effect textures; %zu opaque, %zu blended draws\n",
                        (unsigned long long)g_frames, (unsigned long long)(m.PrivateUsage >> 20), pictureCacheSize(),
                        g_textures.size(), g_projectedViews.size(), g_effectTextures.size(), g_opaque.size(),
                        g_blended.size());
        }
        g_opaque.clear();
        g_blended.clear();
    }

    // The GS uploads, noted as the game starts its VIF1 and GIF transfers.
    void installUploads(bozo::libs::Libraries &libraries, uint8_t *ram)
    {
        g_uploadRam = ram;
        libraries.dmaStarted = dmaStarted;
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
        r.registerFunction(kProgramD, programD);
        r.registerFunction(kDepthOfField, depthOfField);
        r.registerFunction(kRoomDraw, roomDraw);
    }
} // namespace jc2

namespace jc2
{
    // Save states (BozoEngine's bozo/state.h): where the game's model resources sit (noted as it loaded them, which a
    // loaded state does not repeat) and the room's projected texture record. Everything decoded from guest memory is
    // dropped on load.
    void meshesState(bozo::state::Archive &a)
    {
        uint64_t n = bozo::state::count(a, g_loaded.size());
        if (!a.loading())
            for (auto &[base, l] : g_loaded)
            {
                uint32_t b = base;
                bozo::state::fields(a, b, l.size, l.id);
            }
        else
        {
            g_loaded.clear();
            for (uint64_t i = 0; i < n; ++i)
            {
                uint32_t base = 0;
                Loaded l{};
                bozo::state::fields(a, base, l.size, l.id);
                g_loaded[base] = l;
            }
        }
        bozo::state::fields(a, g_projectedRecord, g_projectedTim2);
        if (std::getenv("JC2_TRACE_STATE"))
        {
            std::printf("[jc2:state] %s: %zu loaded resources\n", a.loading() ? "loaded" : "saved", g_loaded.size());
            for (const auto &[base, l] : g_loaded)
                std::printf("[jc2:state]   %#x..%#x resource %u\n", base, base + l.size, l.id);
        }
        if (a.loading())
        {
            g_copies.clear();
            g_textures.clear();
            g_projectedViews.clear();
            g_effectTextures.clear();
            g_bySector.clear();
            g_unknown.clear();
            g_opaque.clear();
            g_blended.clear();
            g_fades.clear();
            g_effects.clear();
            g_gsPictures.clear();
            g_uploadedViews.clear();
            g_renderedPages.clear();
            g_renderedViews.clear();
        }
    }

    // The uploads noted (GS pages and where their pixels are in RAM): a state saved before they were saved has
    // none, and the effects started before it find their textures again when the game uploads them.
    void uploadsState(bozo::state::Archive &a)
    {
        if (a.loading() && a.sectionLeft() == 0)
        {
            g_uploads.clear();
            return;
        }
        uint64_t n = bozo::state::count(a, g_uploads.size());
        if (a.loading())
            g_uploads.assign(size_t(n), Upload{});
        for (Upload &u : g_uploads)
        {
            bozo::state::fields(a, u.dbp, u.dbw, u.psm, u.x, u.y, u.w, u.h, u.serial);
            uint64_t spans = bozo::state::count(a, u.spans.size());
            if (a.loading())
                u.spans.assign(size_t(spans), {0u, 0u});
            for (auto &[address, bytes] : u.spans)
                bozo::state::fields(a, address, bytes);
        }
        bozo::state::fields(a, g_uploadSerial);
    }
}
