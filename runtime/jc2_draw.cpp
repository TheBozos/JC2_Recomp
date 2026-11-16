// Jade Cocoon 2's pictures drawn natively (BozoEngine's renderer): no GS, no GIF or VIF packet read. Each family of the
// game's draws is taken from its own state at the routine that would build its packets.
//
// Movies. JC2 plays its PSS files with Sony's ezmovie sample (docs/RECOMP_NOTES.md, "Movies"): libmpeg decodes into a
// three-slot picture queue (player +0x58); the game's display side (0x20F030) uploads the oldest picture with a GIF
// image transfer and its GIF end handler (0x20F450) releases the slot; 0x20F140 then draws the uploaded picture as a
// sprite. Here libmpeg is BozoEngine's (FFmpeg): it takes the video from its own demux, so the game's IPU feeder (the
// ViBuf: its nodata, StopDMA and RestartDMA callbacks, and the video stream callback that fills it) has nothing to do.
// The display side is native: the picture is read from the game's slot (RGBA32 in 16 x 16 macroblocks, column-major,
// as libmpeg wrote it) and shown as the renderer's backdrop; the slot is released at once, as the GIF end would. The
// sound stays the game's own: its PCM callback, its audio decoder and the compiled M2SOUND/LIBSD driver.
#include "bozo/port.h"
#include "bozo/render/renderer.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>
#include <cstring>
#include <memory>

namespace jc2
{
    void flush2D();
    void install2D(bozo::port::Registry &);

    namespace
    {
        // JC2's display: 640 x 448 (NTSC, a frame of two fields), centred on the GS's 2048 origin.
        bozo::render::Screen screen()
        {
            bozo::render::Screen s;
            s.originX = 2048.0f - 320.0f;
            s.originY = 2048.0f - 224.0f;
            s.width = 640.0f;
            s.height = 448.0f;
            return s;
        }

        uint32_t read32(const uint8_t *ram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, ram + (address & 0x1FFFFFCu), 4);
            return v;
        }
        void write32(uint8_t *ram, uint32_t address, uint32_t value) { std::memcpy(ram + (address & 0x1FFFFFCu), &value, 4); }

        // ---- Movies ----
        constexpr uint32_t kMovieDisplay = 0x0020F030u;   // the display side: one picture to the GS a field-1 frame
        constexpr uint32_t kMovieSprite = 0x0020F140u;    // the uploaded picture drawn over the frame (flag 0x200)
        constexpr uint32_t kViBufAddDma = 0x00154020u;    // nodata: more bitstream to the IPU
        constexpr uint32_t kViBufStopDma = 0x00154330u;   // callback 2: IPU input stopped
        constexpr uint32_t kViBufRestartDma = 0x001544F0u; // callback 3: IPU input restarted
        constexpr uint32_t kVideoStream = 0x001536C0u;    // the video stream callback: bitstream into the ViBuf
        constexpr uint32_t kQueuePop = 0x00155340u;       // the picture queue's pop (releases the oldest slot)
        constexpr uint32_t kFieldWord = 0x006D618Cu;      // the flip's FIELD bit (pictures change on field-1 frames)

        std::shared_ptr<const bozo::render::Texture> moviePicture; // the picture on screen (game thread only)
        bool movieShown = false;                                   // 0x20F140 drew it this frame
        uint64_t movieVersion = 0;

        // The queue's oldest slot as a picture: RGBA32 in 16 x 16 macroblocks, macroblock (x, y) at x * rows + y.
        std::shared_ptr<const bozo::render::Texture> slotPicture(const uint8_t *ram, uint32_t queue)
        {
            const uint32_t base = read32(ram, queue) & 0x1FFFFFFu, stride = read32(ram, queue + 4);
            const uint32_t head = read32(ram, queue + 0x14), width = read32(ram, queue + 0x20),
                           height = read32(ram, queue + 0x24);
            if (!width || !height || width > 1024 || height > 1024 || (width | height) & 15u)
                return nullptr;
            const uint8_t *pixels = ram + ((base + head * stride) & 0x1FFFFFFu);
            auto texture = std::make_shared<bozo::render::Texture>();
            texture->width = width;
            texture->height = height;
            texture->rgba.resize(size_t(width) * height);
            const uint32_t rows = height / 16;
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    uint32_t p;
                    std::memcpy(&p, pixels + ((size_t(x / 16) * rows + y / 16) * 256 + (y % 16) * 16 + x % 16) * 4, 4);
                    texture->rgba[size_t(y) * width + x] = p | 0xFF000000u; // a movie picture is opaque
                }
            texture->version = ++movieVersion;
            return texture;
        }

        // 0x20F030(movie): the oldest decoded picture shown on a field-1 frame (natively), its slot released; an aborted
        // movie's pictures released unshown. 1, as the game's.
        void movieDisplay(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t movie = ctx->r[4].uw[0];
            const uint32_t player = read32(ram, movie + 0x14);
            const uint32_t queue = read32(ram, player + 0x58);
            if (read32(ram, queue + 0x18) != 0 && read32(ram, movie + 0x48) == 0)
            {
                const bool aborted = (read32(ram, player) & 0x400u) != 0;
                if (aborted || read32(ram, kFieldWord) == 1u)
                {
                    if (!aborted)
                        if (auto picture = slotPicture(ram, queue))
                            moviePicture = std::move(picture);
                    const uint32_t ra = ctx->r[31].uw[0];
                    ctx->r[4].ud[0] = queue;
                    rt->functions.generated(kQueuePop)(ram, ctx, rt);
                    ctx->r[31].ud[0] = ra;
                }
            }
            ctx->r[2].ud[0] = 1;
            ctx->pc = ctx->r[31].uw[0];
        }

        // 0x20F140(movie): while the player has decoded its first picture (+0x18 bit 0x200), the picture is on screen.
        void movieSprite(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            if (read32(ram, ctx->r[4].uw[0] + 0x18) & 0x200u)
                movieShown = true;
            ctx->pc = ctx->r[31].uw[0];
        }

        // The IPU feeder's work: none (the decoder has the bitstream from the demux). nodata returns, as viBufAddDMA does
        // after queueing its transfers; the video stream callback reports its packet taken.
        void ipuFeederIdle(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }
        void videoStreamTaken(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->r[2].ud[0] = 1;
            ctx->pc = ctx->r[31].uw[0];
        }
        // ---- Bring-up: the draw census (JC2_DRAW_CENSUS=1) ----
        // The game's packet builders take their write pointer in one of seven per-frame packet buffers (table 0x6CC5F0)
        // with 0x13DD50(buffer) (0x13DC90 for buffer 0). Every 300 frames the census lists which callers did, per buffer:
        // the builders of what is on screen, to port one family at a time.
        constexpr uint32_t kBufferPointer = 0x0013DD50u;
        constexpr uint32_t kBufferPointer0 = 0x0013DC90u;
        const bool census = std::getenv("JC2_DRAW_CENSUS") != nullptr;
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> censusCounts; // (buffer, caller) -> calls
        uint64_t censusFrames = 0;

        void countCaller(uint32_t buffer, uint32_t ra)
        {
            if (census)
                ++censusCounts[{buffer, ra}];
        }
        void bufferPointer(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            countCaller(ctx->r[4].uw[0], ctx->r[31].uw[0]);
            rt->functions.generated(kBufferPointer)(ram, ctx, rt);
        }
        void bufferPointer0(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            countCaller(0, ctx->r[31].uw[0]);
            rt->functions.generated(kBufferPointer0)(ram, ctx, rt);
        }
        constexpr uint32_t kBufferBase = 0x0013DD90u;   // buffer a0's pointer (another form)
        constexpr uint32_t kBufferCommit = 0x0013DE10u; // buffer a0 advanced by a1 quadwords
        void bufferBase(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            countCaller(0x100u | ctx->r[4].uw[0], ctx->r[31].uw[0]);
            rt->functions.generated(kBufferBase)(ram, ctx, rt);
        }
        void bufferCommit(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            countCaller(0x200u | ctx->r[4].uw[0], ctx->r[31].uw[0]);
            rt->functions.generated(kBufferCommit)(ram, ctx, rt);
        }
        void censusFrameEnd()
        {
            if (!census || ++censusFrames % 300 != 0)
                return;
            std::printf("[jc2:census] frames %llu-%llu: buffer caller calls\n", (unsigned long long)(censusFrames - 299),
                        (unsigned long long)censusFrames);
            for (const auto &[key, calls] : censusCounts)
                std::printf("[jc2:census]   %u 0x%06x %llu\n", key.first, key.second, (unsigned long long)calls);
            censusCounts.clear();
        }
    } // namespace

    // The game's frame ended (the frame wait): what was drawn for it is published, the next frame begins.
    void drawFrameEnd()
    {
        auto &renderer = bozo::render::Renderer::instance();
        const bozo::render::Screen s = screen();
        if (movieShown && moviePicture)
            renderer.setBackdrop(moviePicture, s, s.originX, s.originY, s.width, s.height, true);
        else
            renderer.clearBackdrop();
        movieShown = false;
        censusFrameEnd();
        flush2D();
        renderer.publishOverlay();
        renderer.endFrame();
        renderer.beginFrame(s);
    }

    void installDraw(Runtime &, bozo::port::Registry &r)
    {
        install2D(r);
        auto &renderer = bozo::render::Renderer::instance();
        renderer.setEnabled(true);
        renderer.setStreamDraws(false);
        renderer.setPresentsAlone(true);
        renderer.beginFrame(screen());
        r.registerFunction(kMovieDisplay, movieDisplay);
        r.registerFunction(kMovieSprite, movieSprite);
        r.registerFunction(kViBufAddDma, ipuFeederIdle);
        r.registerFunction(kViBufStopDma, ipuFeederIdle);
        r.registerFunction(kViBufRestartDma, ipuFeederIdle);
        r.registerFunction(kVideoStream, videoStreamTaken);
        r.registerFunction(kBufferPointer, bufferPointer);
        r.registerFunction(kBufferPointer0, bufferPointer0);
        r.registerFunction(kBufferBase, bufferBase);
        r.registerFunction(kBufferCommit, bufferCommit);
    }
} // namespace jc2
