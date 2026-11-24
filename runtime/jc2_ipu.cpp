// Jade Cocoon 2's IPU animations, decoded natively (BozoEngine's IpuStream: FFmpeg's "ipu" decoder). No IPU, no DMA.
//
// Besides its PSS movies (libmpeg, jc2_draw.cpp) the game plays ".ipu" streams itself as animated textures (scene D001's
// 64 x 64 water, 298 frames; the battle code too: 0x415xxx, 0x432xxx), driving the IPU directly (mapped from
// SLUS_203.09, docs/RECOMP_NOTES.md "IPU animations"):
// - a group (the active one at gp-0x727C): +0 flags (1 on, 2 decoding), +4 its count of objects, +8 the objects (0x70
//   bytes each, 0x2B02F0), +0xC the one being decoded;
// - an object: +0 flags (1 on, 2 playing, 4 finished, 8 looping, 0x10 a frame in the IPU, 0x20 held), +4 the IPU's
//   saved DMA state (sceIpuStopDMA / RestartDMA), +0x28 the stream (its "ipum" header), +0x38 the frames decoded this
//   pass, +0x3C the stream's frames, +0x40 the passes looped, +0x44 / +0x48 its size in 16 x 16 macroblocks, +0x54 its
//   ring of frames at +0x6C (each one macroblocks x 1 KB, RGB32 as the IPU's CSC writes it), +0x58 the next slot to
//   write, +0x5C the frames waiting in the ring (the texture side takes them);
// - the game's kick 0x2B1990(group) marks the group decoding and calls 0x2B1AC0(group), which walks the objects and
//   starts one frame on the IPU with 0x2B0DA0(object) (reset, the stream by to-IPU DMA, FDEC the frame's flags, IDEC to
//   RGB32 into the ring slot by from-IPU DMA); the from-IPU DMA's handler 0x2B1780 ends the frame (0x2B10F0: the
//   counters) and starts the next until every ring is full, then the group stops.
// Native: 0x2B0DA0 decodes the object's next frame into its ring slot at once and does 0x2B10F0's bookkeeping; 0x2B1AC0
// fills every object's ring that way and reports nothing in flight, so the group stops and the game's next kick tops
// the rings up. JC2_TRACE_IPU=1 lists the streams opened.
#include "bozo/libs/ipu_stream.h"
#include "bozo/port.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace jc2
{
    namespace
    {
        constexpr uint32_t kIpuDecode = 0x002B0DA0u; // the object's next frame (0: none started)
        constexpr uint32_t kIpuNext = 0x002B1AC0u;   // the group's next object to decode (0: none in flight)
        constexpr uint32_t kObjectBytes = 0x70u;
        const bool traceIpu = std::getenv("JC2_TRACE_IPU") != nullptr;
        unsigned g_decodeCalls = 0, g_nextCalls = 0;

        uint32_t r32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1FFFFFFu), 4);
            return v;
        }
        void w32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & 0x1FFFFFFu), &v, 4); }

        struct Opened
        {
            uint32_t bytes = 0;
            std::unique_ptr<bozo::libs::IpuStream> stream;
        };
        std::map<uint32_t, Opened> g_streams; // by the stream's address (its bytes checked: the memory is reused)

        bozo::libs::IpuStream *streamAt(const uint8_t *ram, uint32_t address)
        {
            address &= 0x1FFFFFFu;
            if (address < 0x100000u || address + 16u > 0x2000000u || std::memcmp(ram + address, "ipum", 4) != 0)
                return nullptr;
            const uint32_t bytes = r32(ram, address + 4);
            if (bytes < 16u || address + bytes > 0x2000000u)
                return nullptr;
            Opened &o = g_streams[address];
            if (o.stream && o.bytes == bytes)
                return o.stream.get();
            o.bytes = bytes;
            o.stream = std::make_unique<bozo::libs::IpuStream>();
            const bool opened = o.stream->open(ram + address, bytes);
            if (traceIpu || !opened)
                std::fprintf(stderr, "[jc2:ipu] stream at %#x (%u bytes): %s, %d x %d, %d frames\n", address, bytes,
                             opened ? "opened" : "not decodable", o.stream->width(), o.stream->height(),
                             o.stream->frames());
            if (!opened)
                o.stream.reset();
            return o.stream.get();
        }

        // 0x2B0DA0's conditions, the frame decoded into the ring, then the from-IPU DMA end's bookkeeping (0x2B10F0).
        bool decodeFrame(uint8_t *ram, uint32_t object)
        {
            uint32_t flags = r32(ram, object);
            if (!(flags & 1u) || (flags & 0x30u))
                return false;
            const uint32_t ring = r32(ram, object + 0x54), waiting = r32(ram, object + 0x5C);
            if (!ring || waiting >= ring)
                return false;
            uint32_t done = r32(ram, object + 0x38);
            const uint32_t total = r32(ram, object + 0x3C);
            if (!done || done >= total)
            {
                // A pass starts: a looping object finished before starts again, a finished one stays so.
                if (flags & 8u)
                    flags &= ~4u;
                if (flags & 4u)
                {
                    w32(ram, object, flags);
                    return false;
                }
                flags |= 2u;
                done = 0;
            }
            const uint32_t mbw = r32(ram, object + 0x44), mbh = r32(ram, object + 0x48), frameBytes = mbw * mbh * 1024u;
            const uint32_t slot = r32(ram, object + 0x58) % ring;
            const uint32_t dest = (r32(ram, object + 0x6C) & 0x1FFFFFFu) + slot * frameBytes;
            bozo::libs::IpuStream *stream = streamAt(ram, r32(ram, object + 0x28));
            if (stream && stream->width() == int(mbw * 16u) && stream->height() == int(mbh * 16u) && frameBytes &&
                dest >= 0x100000u && dest + frameBytes <= 0x2000000u && stream->frames() > 0)
                stream->decodeMacroblocks(int(done % uint32_t(stream->frames())), ram + dest);
            // The frame's end, as the from-IPU DMA's handler counts it.
            ++done;
            w32(ram, object + 0x58, r32(ram, object + 0x58) + 1);
            w32(ram, object + 0x5C, waiting + 1);
            if (done >= total)
            {
                flags = (flags & ~2u) | 4u;
                done = 0;
                if (flags & 8u)
                    w32(ram, object + 0x40, r32(ram, object + 0x40) + 1);
            }
            w32(ram, object + 0x38, done);
            w32(ram, object, flags);
            return true;
        }

        void ipuDecode(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            ++g_decodeCalls;
            const uint32_t object = ctx->r[4].uw[0];
            const bool decoded = decodeFrame(ram, object);
            if (traceIpu)
            {
                static int traced = 0;
                if (traced++ < 8)
                    std::fprintf(stderr, "[jc2:ipu] decode %#x: %s (flags %#x, frames %u/%u, waiting %u/%u)\n", object,
                                 decoded ? "a frame" : "none", r32(ram, object), r32(ram, object + 0x38),
                                 r32(ram, object + 0x3C), r32(ram, object + 0x5C), r32(ram, object + 0x54));
            }
            ctx->r[2].ud[0] = decoded ? 1u : 0u;
            ctx->pc = ctx->r[31].uw[0];
        }

        void ipuNext(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            ++g_nextCalls;
            const uint32_t group = ctx->r[4].uw[0];
            const uint32_t count = r32(ram, group + 4), objects = r32(ram, group + 8);
            if (traceIpu)
            {
                static int traced = 0;
                if (traced < 8)
                {
                    ++traced;
                    std::fprintf(stderr, "[jc2:ipu] group %#x: %u objects at %#x", group, count, objects);
                    for (uint32_t i = 0; i < count && i < 4u; ++i)
                    {
                        const uint32_t o = objects + i * kObjectBytes;
                        std::fprintf(stderr, " [flags %#x stream %#x done %u/%u ring %u waiting %u]", r32(ram, o),
                                     r32(ram, o + 0x28), r32(ram, o + 0x38), r32(ram, o + 0x3C), r32(ram, o + 0x54),
                                     r32(ram, o + 0x5C));
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            for (uint32_t i = 0; i < count && i < 64u; ++i)
            {
                const uint32_t object = objects + i * kObjectBytes;
                for (int n = 0; n < 64 && decodeFrame(ram, object); ++n)
                {
                }
            }
            w32(ram, group + 0xC, 0);
            ctx->r[2].ud[0] = 0; // nothing in flight: the kick stops the group, the next one tops the rings up
            ctx->pc = ctx->r[31].uw[0];
        }
    }

    void installIpu(bozo::port::Registry &r)
    {
        r.registerFunction(kIpuDecode, ipuDecode);
        r.registerFunction(kIpuNext, ipuNext);
        if (traceIpu)
        {
            std::fprintf(stderr, "[jc2:ipu] native player installed\n");
            std::atexit([] { std::fprintf(stderr, "[jc2:ipu] calls: %u decode, %u next\n", g_decodeCalls, g_nextCalls); });
        }
    }
}
