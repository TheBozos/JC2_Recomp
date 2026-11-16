// Jade Cocoon 2's 2D pictures (title, menus, backgrounds), drawn natively from the game's own resources.
//
// The game draws a picture with 0x161D50(object): object +0 resource id, +4 part, +8/+0xC screen x/y, +0x10/+0x14 target
// size (-1: the part's own; 0: nothing), +0x18 colour 0xRRGGBBAA (0x80 = 1.0, the alpha is the fade), +0x1C mip shift,
// +0x20 palette index, +0x24 layer (smaller is nearer: Z 0xF42400 + (0x1F - layer) * 4), +0x28 flips (bit 0 x, bit 1 y),
// +0x2C rotation (degrees about the part's centre). The resource is a node of the list whose head is at 0x6C47D4
// (*(gp - 0x749C)): +0 id, +4 parts, +8 part descriptors (0x1C bytes), +0xC the loaded file (a TIM2 0x20 bytes in),
// +0x10 the pictures' 0x60-byte GS records, +0x20 bit 0 uploaded, +0x24 next. A part: +0 first picture, +4 sub-quads,
// +8 16-byte records (x, y, w, h in the part), +0xC 0x60-byte records (+0x10 u16 U, V, W, H texels), +0x10/+0x14 size.
// On the console the pictures are uploaded once to GS memory (0x161380) and each sub-quad is a GIF strip in packet
// buffer 1, linked by layer into the GIF ordering table (0x13DE70). Here the TIM2 picture is decoded from EE RAM and the
// quad drawn by the renderer: no GS upload, no packet. The node is still marked uploaded (+0x20 bit 0) by the game's own
// 0x161380 (it fills the 0x60-byte records the quads read), until that routine's GS transfer is replaced as well.
#include "bozo/port.h"
#include "bozo/render/renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace jc2
{
    namespace
    {
        constexpr uint32_t kDrawPicture = 0x00161D50u;
        constexpr uint32_t kResourceList = 0x006C47D4u;
        constexpr float kOriginX = 2048.0f - 320.0f, kOriginY = 2048.0f - 224.0f;

        uint32_t r32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1FFFFFFu), 4);
            return v;
        }
        uint16_t r16(const uint8_t *ram, uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, ram + (a & 0x1FFFFFFu), 2);
            return v;
        }

        // One TIM2 picture decoded (RGBA, alpha to 255), kept while its bytes are unchanged.
        struct Picture
        {
            uint64_t check = 0;
            std::shared_ptr<const bozo::render::Texture> texture;
        };
        std::map<std::pair<uint32_t, uint32_t>, Picture> pictures; // by the picture's EE address and palette
        uint64_t pictureVersion = 0;

        uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ull)
        {
            for (size_t i = 0; i < n; ++i)
                h = (h ^ p[i]) * 1099511628211ull;
            return h;
        }

        // The TIM2 picture at EE address p (its header: total, CLUT and image sizes, header size, colours, CLUT type,
        // image type, width, height). Image type 5: 8-bit indices; 4: 4-bit (low nibble first). CLUT type & 0x1F: 1
        // CT16 (A1B5G5R5, alpha from bit 15 as TEXA's TA1 0x80), else CT32 (alpha 0x80 = 1.0); a 256-colour CLUT without
        // bit 7 is in CSM1 order (index bits 3 and 4 swapped). A CLUT may hold several palettes (16 colours each for 4-bit
        // pictures, 256 for 8-bit): the object's palette index (+0x20, an offset of the GS CLUT address) picks one.
        std::shared_ptr<const bozo::render::Texture> picture(const uint8_t *ram, uint32_t p, uint32_t paletteIndex)
        {
            const uint32_t imageBytes = r32(ram, p + 8);
            const uint16_t header = r16(ram, p + 12), colours = r16(ram, p + 14);
            const uint8_t clutType = ram[(p + 18) & 0x1FFFFFFu], imageType = ram[(p + 19) & 0x1FFFFFFu];
            const uint16_t width = r16(ram, p + 20), height = r16(ram, p + 22);
            if (!width || !height || width > 2048 || height > 2048 || (imageType != 4 && imageType != 5))
                return nullptr;
            const uint32_t image = p + header, clut = image + imageBytes;
            const size_t clutBytes = (clutType & 0x1F) == 1 ? colours * 2u : colours * 4u;
            const uint64_t check = fnv(ram + (image & 0x1FFFFFFu), imageBytes, fnv(ram + (clut & 0x1FFFFFFu), clutBytes));
            Picture &cached = pictures[{p, paletteIndex}];
            if (cached.texture && cached.check == check)
                return cached.texture;
            const uint32_t size = imageType == 5 ? 256u : 16u;
            const uint32_t start = paletteIndex * size < colours ? paletteIndex * size : 0u;
            const uint32_t used = std::min<uint32_t>(size, colours - start);
            std::vector<uint32_t> palette(size, 0);
            for (uint32_t i = 0; i < used; ++i)
            {
                if ((clutType & 0x1F) == 1)
                {
                    const uint16_t c = r16(ram, clut + 2 * (start + i));
                    const uint32_t r = (c & 31) * 255 / 31, g = (c >> 5 & 31) * 255 / 31, b = (c >> 10 & 31) * 255 / 31;
                    palette[i] = r | g << 8 | b << 16 | (c & 0x8000u ? 0xFF000000u : 0u);
                }
                else
                {
                    const uint32_t c = r32(ram, clut + 4 * (start + i)), a = std::min(255u, (c >> 24) * 2u);
                    palette[i] = (c & 0xFFFFFFu) | a << 24;
                }
            }
            if (size == 256 && !(clutType & 0x80))
            {
                std::vector<uint32_t> linear(256);
                for (uint32_t i = 0; i < 256; ++i)
                    linear[i] = palette[(i & 0xE7u) | ((i & 8u) << 1) | ((i & 0x10u) >> 1)];
                palette.swap(linear);
            }
            auto texture = std::make_shared<bozo::render::Texture>();
            texture->width = width;
            texture->height = height;
            texture->rgba.resize(size_t(width) * height);
            const uint8_t *pixels = ram + (image & 0x1FFFFFFu);
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    const size_t i = size_t(y) * width + x;
                    const uint32_t index = imageType == 5 ? pixels[i] : (pixels[i / 2] >> (4 * (x & 1))) & 15u;
                    texture->rgba[i] = index < palette.size() ? palette[index] : 0u;
                }
            texture->key = 0x4A433200000000ull | p | uint64_t(paletteIndex & 0xFFu) << 32;
            texture->version = ++pictureVersion;
            texture->asset = true;
            cached = {check, texture};
            return texture;
        }

        // A quad of this frame, drawn at the frame's end in the game's layer order.
        struct Quad
        {
            uint32_t layer, serial;
            bozo::render::Vertex v[6];
            std::shared_ptr<const bozo::render::Texture> texture;
        };
        std::vector<Quad> quads;
        uint32_t serial = 0;

        // 0x161D50(object), natively.
        void drawPicture(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t object = ctx->r[4].uw[0], ra = ctx->r[31].uw[0];
            const uint32_t id = r32(ram, object), part = r32(ram, object + 4);
            uint32_t node = r32(ram, kResourceList);
            for (unsigned guard = 0; node && r32(ram, node) != id && guard < 4096; ++guard)
                node = r32(ram, node + 0x24);
            const int32_t targetW = static_cast<int32_t>(r32(ram, object + 0x10)),
                          targetH = static_cast<int32_t>(r32(ram, object + 0x14));
            if (!node || !r32(ram, node + 0xC) || part >= r32(ram, node + 4) || !targetW || !targetH)
            {
                ctx->pc = ra;
                return;
            }
            if (!(r32(ram, node + 0x20) & 1u))
            {
                // Not uploaded yet: the game's own drawer this once, as a tail call (its upload fills the GS records the
                // quads read, marks the node and may wait: a stop there resumes inside the game's code). Drawn natively
                // from the next frame on.
                rt->functions.generated(kDrawPicture)(ram, ctx, rt);
                return;
            }
            static bool warned = false;
            if (!warned && (r32(ram, object + 0x2C) || r32(ram, object + 0x1C)))
            {
                warned = true;
                std::fprintf(stderr, "[jc2:2d] picture 0x%x part %u with palette %u, rotation %d, mip %u: not drawn yet\n",
                             id, part, r32(ram, object + 0x20), static_cast<int32_t>(r32(ram, object + 0x2C)),
                             r32(ram, object + 0x1C));
            }
            const uint32_t descriptor = r32(ram, node + 8) + part * 0x1Cu;
            const uint32_t first = r32(ram, descriptor), count = r32(ram, descriptor + 4);
            const uint32_t tiles = r32(ram, descriptor + 8), records = r32(ram, descriptor + 0xC);
            const float partW = static_cast<float>(r32(ram, descriptor + 0x10)),
                        partH = static_cast<float>(r32(ram, descriptor + 0x14));
            const float sx = targetW < 0 || partW <= 0 ? 1.0f : targetW / partW;
            const float sy = targetH < 0 || partH <= 0 ? 1.0f : targetH / partH;
            const float ox = static_cast<float>(static_cast<int32_t>(r32(ram, object + 8)));
            const float oy = static_cast<float>(static_cast<int32_t>(r32(ram, object + 0xC)));
            const uint32_t colour = r32(ram, object + 0x18), flips = r32(ram, object + 0x28);
            const uint32_t rgba = (colour >> 24) | ((colour >> 16) & 0xFFu) << 8 | ((colour >> 8) & 0xFFu) << 16 |
                                  (colour & 0xFFu) << 24;
            const uint32_t layer = r32(ram, object + 0x24);
            // The TIM2 file: 0x20 bytes into the loaded file; its pictures follow its 16-byte header, each its total size.
            const uint32_t tim = r32(ram, node + 0xC) + 0x20;
            const uint16_t total = r16(ram, tim + 6);
            uint32_t at = tim + 0x10;
            std::vector<uint32_t> starts;
            for (uint16_t k = 0; k < total && k < 1024; ++k)
            {
                starts.push_back(at);
                at += r32(ram, at);
            }
            for (uint32_t j = 0; j < count; ++j)
            {
                const uint32_t index = first + j;
                if (index >= starts.size())
                    break;
                auto texture = picture(ram, starts[index], r32(ram, object + 0x20));
                if (!texture)
                    continue;
                const uint32_t tile = tiles + 16 * j, record = records + 0x60 * j;
                const float qx = static_cast<float>(static_cast<int32_t>(r32(ram, tile))),
                            qy = static_cast<float>(static_cast<int32_t>(r32(ram, tile + 4)));
                // U, V place the picture in GS memory (pictures packed side by side); here each tile samples its own
                // picture from its origin.
                const float w = r16(ram, record + 0x14), h = r16(ram, record + 0x16);
                float x0 = ox + qx * sx, y0 = oy + qy * sy, x1 = x0 + w * sx, y1 = y0 + h * sy;
                if (flips & 1u)
                {
                    x0 = ox + (partW - qx - w) * sx;
                    x1 = x0 + w * sx;
                }
                if (flips & 2u)
                {
                    y0 = oy + (partH - qy - h) * sy;
                    y1 = y0 + h * sy;
                }
                float u0 = 0.0f, u1 = w / texture->width;
                float v0 = 0.0f, v1 = h / texture->height;
                if (flips & 1u)
                    std::swap(u0, u1);
                if (flips & 2u)
                    std::swap(v0, v1);
                Quad q;
                q.layer = layer;
                q.serial = serial++;
                q.texture = std::move(texture);
                auto vertex = [&](float x, float y, float s, float t) {
                    bozo::render::Vertex p;
                    p.x = kOriginX + x;
                    p.y = kOriginY + y;
                    p.u = s;
                    p.v = t;
                    p.rgba = rgba;
                    return p;
                };
                const bozo::render::Vertex a = vertex(x0, y0, u0, v0), b = vertex(x1, y0, u1, v0),
                                           c = vertex(x0, y1, u0, v1), d = vertex(x1, y1, u1, v1);
                q.v[0] = a, q.v[1] = b, q.v[2] = c, q.v[3] = b, q.v[4] = d, q.v[5] = c;
                quads.push_back(std::move(q));
            }
            ctx->pc = ra;
        }
    } // namespace

    // The frame's pictures to the renderer: far layers first (a larger layer is farther), then in submission order.
    void flush2D()
    {
        std::stable_sort(quads.begin(), quads.end(), [](const Quad &a, const Quad &b) {
            return std::tie(b.layer, a.serial) < std::tie(a.layer, b.serial);
        });
        bozo::render::Material material;
        material.blend = bozo::render::Material::Blend::Alpha;
        material.depthTest = false;
        material.depthWrite = false;
        material.clampU = material.clampV = true;
        for (const Quad &q : quads)
        {
            material.tex0 = q.texture->key;
            bozo::render::Renderer::instance().drawOverlay(material, q.v, 6, q.texture);
        }
        quads.clear();
        serial = 0;
    }

    void install2D(bozo::port::Registry &r) { r.registerFunction(kDrawPicture, drawPicture); }
} // namespace jc2
