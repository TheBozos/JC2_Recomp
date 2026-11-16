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
#include <string>
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
        std::map<std::pair<uint64_t, uint32_t>, Picture> pictures; // by key (EE address, or font id) and palette
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
        uint32_t get32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
        uint16_t get16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

        // A TIM2 picture decoded from its header at `h` (in EE RAM or in a file read from the disc), cached by `key`.
        std::shared_ptr<const bozo::render::Texture> decodePicture(const uint8_t *h, uint32_t paletteIndex, uint64_t key)
        {
            const uint32_t imageBytes = get32(h + 8);
            const uint16_t header = get16(h + 12), colours = get16(h + 14);
            const uint8_t clutType = h[18], imageType = h[19];
            const uint16_t width = get16(h + 20), height = get16(h + 22);
            if (!width || !height || width > 2048 || height > 2048 || imageType < 1 || imageType > 5)
                return nullptr;
            const uint8_t *pixels = h + header, *clut = pixels + imageBytes;
            // Direct colour (no CLUT): type 1 16-bit (A1B5G5R5), 2 24-bit, 3 32-bit (alpha 0x80 = 1.0).
            if (imageType <= 3)
            {
                const uint64_t check = fnv(pixels, imageBytes);
                Picture &cached = pictures[{key, 0u}];
                if (cached.texture && cached.check == check)
                    return cached.texture;
                auto texture = std::make_shared<bozo::render::Texture>();
                texture->width = width;
                texture->height = height;
                texture->rgba.resize(size_t(width) * height);
                const uint32_t bytes = imageType == 1 ? 2u : imageType == 2 ? 3u : 4u;
                if (size_t(width) * height * bytes > imageBytes)
                    return nullptr;
                for (size_t i = 0; i < texture->rgba.size(); ++i)
                {
                    const uint8_t *p = pixels + i * bytes;
                    uint32_t c;
                    if (imageType == 1)
                    {
                        const uint16_t v = get16(p);
                        c = (v & 31) * 255 / 31 | ((v >> 5 & 31) * 255 / 31) << 8 | ((v >> 10 & 31) * 255 / 31) << 16 |
                            (v & 0x8000u ? 0xFF000000u : 0u);
                    }
                    else if (imageType == 2)
                        c = p[0] | p[1] << 8 | p[2] << 16 | 0xFF000000u;
                    else
                        c = p[0] | p[1] << 8 | p[2] << 16 | std::min(255u, uint32_t(p[3]) * 2u) << 24;
                    texture->rgba[i] = c;
                }
                texture->key = 0x4A43320000000000ull ^ key;
                texture->version = ++pictureVersion;
                cached.texture = texture;
                cached.check = check;
                return texture;
            }
            // Picture format 0x30 (a room's pictures): 8-bit pixels in the GS's own order, 16 x 16 blocks.
            const bool swizzled = (h[16] & 0x30u) == 0x30u && width % 16 == 0 && height % 16 == 0;
            const size_t clutBytes = (clutType & 0x1F) == 1 ? colours * 2u : (clutType & 0x1F) == 2 ? colours * 3u : colours * 4u;
            const uint64_t check = fnv(pixels, imageBytes, fnv(clut, clutBytes));
            Picture &cached = pictures[{key, paletteIndex}];
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
                    const uint16_t c = get16(clut + 2 * (start + i));
                    const uint32_t r = (c & 31) * 255 / 31, g = (c >> 5 & 31) * 255 / 31, b = (c >> 10 & 31) * 255 / 31;
                    palette[i] = r | g << 8 | b << 16 | (c & 0x8000u ? 0xFF000000u : 0u);
                }
                else if ((clutType & 0x1F) == 2)
                {
                    // CT24: three bytes a colour, opaque (TEXA's TA0, 0x80).
                    const uint8_t *c = clut + 3 * (start + i);
                    palette[i] = c[0] | c[1] << 8 | c[2] << 16 | 0xFF000000u;
                }
                else
                {
                    const uint32_t c = get32(clut + 4 * (start + i)), a = std::min(255u, (c >> 24) * 2u);
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
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    const size_t i = size_t(y) * width + x;
                    uint32_t index;
                    if (imageType == 5 && swizzled)
                    {
                        // The GS's PSMT8 layout (blocks of 16 x 16, columns of 16 x 4 in four 32-bit lanes), as the
                        // pictures with format 0x30 keep their pixels (uploaded as PSMCT32 words).
                        const size_t block = size_t(y & ~0xFu) * width + size_t(x & ~0xFu) * 2;
                        const uint32_t swap = (((y + 2) >> 2) & 1u) * 4u;
                        const uint32_t row = ((((y & ~3u) >> 1) + (y & 1u)) & 7u);
                        const size_t column = size_t(row) * width * 2 + ((x + swap) & 7u) * 4u;
                        const uint32_t byte = ((y >> 1) & 1u) + ((x >> 2) & 2u);
                        index = pixels[block + column + byte];
                    }
                    else
                        index = imageType == 5 ? pixels[i] : (pixels[i / 2] >> (4 * (x & 1))) & 15u;
                    texture->rgba[i] = index < palette.size() ? palette[index] : 0u;
                }
            texture->key = 0x4A43320000000000ull ^ key ^ uint64_t(paletteIndex & 0xFFu) << 40;
            texture->version = ++pictureVersion;
            texture->asset = true;
            cached = {check, texture};
            return texture;
        }

        std::shared_ptr<const bozo::render::Texture> picture(const uint8_t *ram, uint32_t p, uint32_t paletteIndex)
        {
            return decodePicture(ram + (p & 0x1FFFFFFu), paletteIndex, p);
        }

        // A quad of this frame, drawn at the frame's end in the game's layer order.
        struct Quad
        {
            uint32_t layer, serial; // layer: the game's layer x 2 (+1: a box fill, just behind its frame)
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
                q.layer = layer * 2;
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

        // ---- Text: 0x1636E0(text), a string in a grid bitmap font ----
        // text (0x60 bytes): +0 the string (ASCII, up to 0x40 bytes; 0xB1-0xB9 switch the colour), +0x40 font slot,
        // +0x44/+0x48 x/y, +0x4C layer, +0x50 alpha, +0x54 colour index, +0x58 alignment (0 left, 1 right, 2 centred, on
        // the visible characters' count x cell width), +0x5C extra advance. Font slots: 4 x 0xA0 bytes at 0x86A900:
        // +0x68/+0x6C cell width/height, +0x74 the charset string (a glyph is its character's position there; a missing
        // one is '0'), +0x7C/+0x80 columns/rows. Colours: 9 x 8 bytes at 0x4E7FA0 (+0 code, +1..3 RGB, +4 palette). The
        // game loads each font (resources 0x625-0x627, bare TIM2 files in CDDATA) into GS memory and frees it: here the
        // font is read from the disc's CDDATA.000 (CDDATA.LOC: 16-byte entries by resource id, +4 sector, +8 size).
        constexpr uint32_t kDrawText = 0x001636E0u;
        constexpr uint32_t kFontSlots = 0x0086A900u;
        constexpr uint32_t kTextColours = 0x004E7FA0u;
        constexpr uint32_t kFontResources[3] = {0x625u, 0x626u, 0x627u};

        std::map<uint32_t, std::vector<uint8_t>> fonts; // resource id -> its TIM2 file

        const std::vector<uint8_t> &diskResource(uint32_t id)
        {
            auto found = fonts.find(id);
            if (found != fonts.end())
                return found->second;
            std::vector<uint8_t> &out = fonts[id];
            const std::string dir = JC2_GAME_DIR;
            uint8_t entry[16] = {};
            if (FILE *loc = std::fopen((dir + "/CDDATA.LOC").c_str(), "rb"))
            {
                if (std::fseek(loc, static_cast<long>(id) * 16, SEEK_SET) == 0 && std::fread(entry, 1, 16, loc) == 16)
                {
                    const uint32_t sector = get32(entry + 4), bytes = get32(entry + 8);
                    if (FILE *data = std::fopen((dir + "/CDDATA.000").c_str(), "rb"))
                    {
                        if (bytes && bytes < 0x1000000u && _fseeki64(data, int64_t(sector) * 2048, SEEK_SET) == 0)
                        {
                            out.resize(bytes);
                            if (std::fread(out.data(), 1, bytes, data) != bytes)
                                out.clear();
                        }
                        std::fclose(data);
                    }
                }
                std::fclose(loc);
            }
            if (out.size() < 0x40 || std::memcmp(out.data(), "TIM2", 4) != 0)
            {
                std::fprintf(stderr, "[jc2:2d] resource 0x%x: no TIM2 read from %s/CDDATA.000\n", id, dir.c_str());
                out.clear();
            }
            return out;
        }

        void drawText(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            const uint32_t text = ctx->r[4].uw[0];
            ctx->pc = ctx->r[31].uw[0];
            const uint32_t slot = r32(ram, text + 0x40);
            if (slot >= 3)
            {
                static bool warned = false;
                if (!warned)
                {
                    std::fprintf(stderr, "[jc2:2d] text in font slot %u: not drawn yet\n", slot);
                    warned = true;
                }
                return;
            }
            const std::vector<uint8_t> &file = diskResource(kFontResources[slot]);
            if (file.empty())
                return;
            const uint32_t font = kFontSlots + slot * 0xA0u;
            const uint32_t cw = r32(ram, font + 0x68), ch = r32(ram, font + 0x6C), charset = r32(ram, font + 0x74);
            const uint32_t cols = r32(ram, font + 0x7C), rows = r32(ram, font + 0x80);
            if (!cw || !ch || !cols || !rows || cw > 64 || ch > 64)
                return;
            std::string chars;
            for (uint32_t k = 0; k < 512; ++k)
            {
                const char c = static_cast<char>(ram[(charset + k) & 0x1FFFFFFu]);
                if (!c)
                    break;
                chars += c;
            }
            std::string str;
            for (uint32_t k = 0; k < 0x40; ++k)
            {
                const char c = static_cast<char>(ram[(text + k) & 0x1FFFFFFu]);
                if (!c)
                    break;
                str += c;
            }
            auto isCode = [](uint8_t c) { return c >= 0xB1 && c <= 0xB9; };
            uint32_t visible = 0;
            for (char c : str)
                visible += isCode(static_cast<uint8_t>(c)) ? 0u : 1u;
            float x = static_cast<float>(static_cast<int32_t>(r32(ram, text + 0x44)));
            const float y = static_cast<float>(static_cast<int32_t>(r32(ram, text + 0x48)));
            const uint32_t align = r32(ram, text + 0x58);
            if (align == 1)
                x -= static_cast<float>(visible * cw);
            else if (align == 2)
                x -= static_cast<float>(visible * cw / 2);
            const float advance = static_cast<float>(static_cast<int32_t>(cw) + static_cast<int32_t>(r32(ram, text + 0x5C)));
            const uint32_t layer = r32(ram, text + 0x4C), alpha = r32(ram, text + 0x50) & 0xFFu;
            uint32_t colourEntry = std::min<uint32_t>(r32(ram, text + 0x54), 8u);
            const uint8_t *picture0 = file.data() + 0x10; // the TIM2's first picture
            for (char c : str)
            {
                const uint8_t code = static_cast<uint8_t>(c);
                if (isCode(code))
                {
                    for (uint32_t e = 0; e < 9; ++e)
                        if (ram[(kTextColours + 8 * e) & 0x1FFFFFFu] == code)
                            colourEntry = e;
                    continue;
                }
                size_t glyph = chars.find(c);
                if (glyph == std::string::npos)
                    glyph = chars.find('0');
                if (glyph == std::string::npos)
                {
                    x += advance;
                    continue;
                }
                const uint32_t perPage = cols * rows, page = static_cast<uint32_t>(glyph) / perPage;
                const uint8_t *entry = ram + ((kTextColours + 8 * colourEntry) & 0x1FFFFFFu);
                const uint32_t palette = entry[4] + page;
                auto texture = decodePicture(picture0, palette, 0x80000000ull | kFontResources[slot]);
                if (texture)
                {
                    const uint32_t cell = static_cast<uint32_t>(glyph) % perPage;
                    const float u0 = static_cast<float>((cell % cols) * cw) / texture->width;
                    const float v0 = static_cast<float>((cell / cols) * ch) / texture->height;
                    const float u1 = u0 + static_cast<float>(cw) / texture->width;
                    const float v1 = v0 + static_cast<float>(ch) / texture->height;
                    const uint32_t rgba = entry[1] | entry[2] << 8 | entry[3] << 16 | alpha << 24;
                    // The GS sprite: XYZ2 (x, y) to (x + cw - 1, y + ch - 1), the cell's whole UV range.
                    const float x0 = kOriginX + x, y0 = kOriginY + y, x1 = x0 + cw - 1, y1 = y0 + ch - 1;
                    Quad q;
                    q.layer = layer * 2;
                    q.serial = serial++;
                    q.texture = std::move(texture);
                    auto vtx = [&](float px, float py, float s, float t) {
                        bozo::render::Vertex p;
                        p.x = px;
                        p.y = py;
                        p.u = s;
                        p.v = t;
                        p.rgba = rgba;
                        return p;
                    };
                    const auto a = vtx(x0, y0, u0, v0), b = vtx(x1, y0, u1, v0), cc = vtx(x0, y1, u0, v1),
                               d = vtx(x1, y1, u1, v1);
                    q.v[0] = a, q.v[1] = b, q.v[2] = cc, q.v[3] = b, q.v[4] = d, q.v[5] = cc;
                    quads.push_back(std::move(q));
                }
                x += advance;
            }
        }

        // ---- Box fill: 0x167270(box), a framed box's gradient interior ----
        // box: +0 style, +4/+8 x/y, +0xC/+0x10 w/h, +0x18..+0x24 corner colours 0xRRGGBBAA (top left, bottom left, top
        // right, bottom right), +0x28 modulating colour, +0x2C layer, +0x30/+0x34 animation size (-1: none). Style:
        // 0xB4-byte entries at 0x4E80D0, +0xA4..+0xB0 the fill's insets l/t/r/b (-1: no fill). Each channel is
        // c x mod >> 7; the fill sits just behind its own frame (Z one less).
        constexpr uint32_t kDrawBoxFill = 0x00167270u;
        constexpr uint32_t kBoxStyles = 0x004E80D0u;

        void drawBoxFill(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            const uint32_t box = ctx->r[4].uw[0];
            ctx->pc = ctx->r[31].uw[0];
            const uint32_t style = kBoxStyles + r32(ram, box) * 0xB4u;
            const int32_t l = static_cast<int32_t>(r32(ram, style + 0xA4)), t = static_cast<int32_t>(r32(ram, style + 0xA8)),
                          r = static_cast<int32_t>(r32(ram, style + 0xAC)), b = static_cast<int32_t>(r32(ram, style + 0xB0));
            if (l < 0)
                return;
            if (static_cast<int32_t>(r32(ram, box + 0x30)) != -1)
            {
                static bool warned = false;
                if (!warned)
                {
                    std::fprintf(stderr, "[jc2:2d] animated box fill: drawn at its full size\n");
                    warned = true;
                }
            }
            const float x0 = static_cast<float>(static_cast<int32_t>(r32(ram, box + 4)) + l);
            const float y0 = static_cast<float>(static_cast<int32_t>(r32(ram, box + 8)) + t);
            const float x1 = x0 + static_cast<float>(static_cast<int32_t>(r32(ram, box + 0xC)) - l - r);
            const float y1 = y0 + static_cast<float>(static_cast<int32_t>(r32(ram, box + 0x10)) - t - b);
            const uint32_t mod = r32(ram, box + 0x28);
            auto colour = [&](uint32_t c) {
                uint32_t out = 0;
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t channel = (((c >> (24 - 8 * k)) & 0xFFu) * ((mod >> (24 - 8 * k)) & 0xFFu)) >> 7;
                    out |= std::min(channel, 255u) << (8 * k);
                }
                return out;
            };
            auto vtx = [&](float px, float py, uint32_t c) {
                bozo::render::Vertex p;
                p.x = kOriginX + px;
                p.y = kOriginY + py;
                p.rgba = colour(c);
                return p;
            };
            const auto tl = vtx(x0, y0, r32(ram, box + 0x18)), bl = vtx(x0, y1, r32(ram, box + 0x1C)),
                       tr = vtx(x1, y0, r32(ram, box + 0x20)), br = vtx(x1, y1, r32(ram, box + 0x24));
            Quad q;
            q.layer = r32(ram, box + 0x2C) * 2 + 1;
            q.serial = serial++;
            q.v[0] = tl, q.v[1] = tr, q.v[2] = bl, q.v[3] = tr, q.v[4] = br, q.v[5] = bl;
            quads.push_back(std::move(q));
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
            material.textured = q.texture != nullptr;
            material.tex0 = q.texture ? q.texture->key : 0;
            bozo::render::Renderer::instance().drawOverlay(material, q.v, 6, q.texture);
        }
        quads.clear();
        serial = 0;
    }

    // A TIM2 picture (its header in EE memory) decoded, for the 3D models' textures (runtime/jc2_meshes.cpp).
    std::shared_ptr<const bozo::render::Texture> decodeTim2Picture(const uint8_t *header, uint32_t palette, uint64_t key)
    {
        return decodePicture(header, palette, key);
    }

    void install2D(bozo::port::Registry &r)
    {
        r.registerFunction(kDrawPicture, drawPicture);
        r.registerFunction(kDrawText, drawText);
        r.registerFunction(kDrawBoxFill, drawBoxFill);
    }
} // namespace jc2
