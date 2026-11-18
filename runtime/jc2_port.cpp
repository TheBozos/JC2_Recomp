// Jade Cocoon 2's native port boundary on BozoEngine: the game's own functions answered natively, by guest address.
// No hardware execution and no interpreter fallback: what is not answered here or by BozoEngine's libraries stops the run.
#include "bozo/ee/dispatcher.h"
#include "bozo/ee/kernel.h"
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include "jc2_functions.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>

namespace jc2
{
    void notifyFrame();
    void drawFrameEnd();

    // The frame wait. JC2's flip (0x15A150) counts its frames (0x6D6190) and waits in 0x15B400(target, &late) until the
    // vblank handler's counter (0x6D61AC) reaches the frame count: it sets the wake flag (0x6D61A4) and sleeps
    // (SleepThread); the vblank handler (0x15B360) wakes it. A late frame sets the counter back to target - 1 and counts
    // itself (0x6D61B0). While the main thread sleeps, the game's lower threads run: the movie player (0x1533C0), the
    // loader. So the game's own wait runs; the frame ends at its entry (published, paced, the modules' frame listeners).
    // The wait returns the CSR FIELD bit, read at 0x15B4B0: answered natively (fieldRead), no GS.
    constexpr uint32_t kFrameWait = 0x0015B400u;
    constexpr uint32_t kFieldRead = 0x0015B4B0u; // native site: ld v0, 0x12001000

    void frameWait(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        drawFrameEnd();
        bozo::port::frameEnd(rt, ctx);
        notifyFrame();
        jc2_func_0x15b400(ram, ctx, rt);
    }

    // The game's reads of hardware state with no hardware behind them, answered inline.
    bool nativeInstruction(uint8_t *, R5900Context *ctx, Runtime *rt, uint32_t pc)
    {
        if (pc == kFieldRead)
        {
            // GS CSR as the wait reads it: only FIELD (bit 13), the parity of the vblanks started.
            ctx->r[2].ud[0] = (bozo::port::vblankCount(rt) & 1u) << 13;
            return true;
        }
        return false;
    }

    // libipu's sceIpuInit (0x119868: stops the IPU's DMA, resets the IPU and waits for it): the game calls it once at
    // start (0x2B03F8), before its movies. There is no IPU: libmpeg is native (BozoEngine's FFmpeg decoder), so there
    // is nothing to reset.
    constexpr uint32_t kIpuInit = 0x00119868u;

    void ipuInit(uint8_t *, R5900Context *ctx, Runtime *)
    {
        ctx->pc = ctx->r[31].uw[0];
    }

    // sceSifBindRpc, answered by BozoEngine's SIF library; with JC2_TRACE_SIF=1 each service id the game binds is
    // logged once, with whether a native server answers it.
    constexpr uint32_t kSifBindRpc = 0x0011CDF8u;

    void sifBindRpc(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        static const bool trace = std::getenv("JC2_TRACE_SIF") != nullptr;
        const uint32_t client = ctx->r[4].uw[0], service = ctx->r[5].uw[0];
        bozo::port::callLibrary(rt, ctx, kSifBindRpc);
        if (trace)
        {
            static std::set<uint32_t> seen;
            uint32_t server;
            std::memcpy(&server, ram + ((client + 0x24) & 0x1FFFFFCu), 4);
            if (seen.insert(service).second)
                std::printf("[jc2:sif] bind 0x%08x from ra 0x%x: %s\n", service, ctx->r[31].uw[0],
                            server ? "bound" : "no native server");
        }
        ctx->pc = ctx->r[31].uw[0];
    }

    // The game's EE RPC servers. Genki's IOP drivers call back to the EE: M2SOUND to service 0x10001 (stream state and
    // data requests, handler 0x234A60) and M2IMODE to 0x10003 (handler 0x484740). The game registers them with libsif's
    // sceSifRegisterRpc (0x11D290) from a thread that then waits in sceSifRpcLoop for SIF commands, which do not exist
    // here. So each registration also becomes a native EE service (sif.eeServers) the compiled modules call: the request
    // goes into the server's buffer and the game's own handler runs at once, as sceSifRpcLoop would have run it,
    // handler(command, buffer, size), its result the reply's address. Both handlers only update the game's state and
    // start SIF transfers (no wait), so they run to their end on a context of their own: the registering thread's gp,
    // and a stack at the bottom of that thread's stack (its top is in sceSifRpcLoop, which never returns).
    constexpr uint32_t kSifRegisterRpc = 0x0011D290u;

    // Each registration, as data: a loaded save state registers them again (the game made them long before).
    struct EeServer
    {
        uint32_t service = 0, function = 0, buffer = 0, gp = 0, stack = 0;
    };
    std::map<uint32_t, EeServer> eeServers;

    void registerEeServer(Runtime *rt, const EeServer &server)
    {
        eeServers[server.service] = server;
        uint8_t *ram = rt->memory.ram();
        const uint32_t service = server.service, function = server.function, buffer = server.buffer, gp = server.gp,
                       stack = server.stack;
        bozo::port::libraries(rt).sif.eeServers[service] =
            [ram, rt, function, buffer, gp, stack, service](uint32_t command, std::span<const uint8_t> in,
                                                             std::span<uint8_t> out)
        {
            static int depth = 0;
            static unsigned calls = 0;
            static unsigned nested = 0;
            if (std::getenv("JC2_TRACE_SIF") &&
                (calls++ < 64 || calls % 2000 == 0 || (depth > 0 && nested++ < 200)))
                std::printf("[jc2:sif] EE 0x%x command 0x%x, %zu bytes in, %zu out (depth %d, call %u, vblank %llu)\n",
                            service, command, in.size(), out.size(), depth, calls,
                            (unsigned long long)bozo::port::vblankCount(rt));
            if (depth > 4)
                throw std::runtime_error("JC2 EE RPC handlers nested too deep");
            struct Depth { Depth() { ++depth; } ~Depth() { --depth; } } guard;
            std::memcpy(ram + (buffer & 0x1FFFFFFu), in.data(), in.size());
            R5900Context handler{};
            handler.r[4].ud[0] = command;
            handler.r[5].ud[0] = buffer;
            handler.r[6].ud[0] = in.size();
            handler.r[28].ud[0] = gp;
            handler.r[29].ud[0] = stack + 0x800u;
            handler.r[31].ud[0] = 0u;
            // Its clock starts at the EE's now (the dispatcher reads guest time from the running context's cycles).
            handler.cycles = bozo::port::now(rt);
            // A native out-call on a context of its own (kernel.md section 3b): nothing stops it, not even its jumps
            // through registers (0x234A60's command table), which outside an out-call go back to the dispatcher and
            // drop every frame below: the EE thread's call into the IOP, the IOP task asking, this handler.
            auto &dispatcher = dynamic_cast<bozo::ee::Dispatcher &>(*rt->services);
            dispatcher.beginOutcall(&handler);
            struct End
            {
                bozo::ee::Dispatcher &d;
                R5900Context *c;
                ~End() { d.endOutcall(c); }
            } end{dispatcher, &handler};
            if (!dispatcher.ownContext(&handler))
                throw std::runtime_error("JC2 EE RPC handler called outside guest code");
            handler.cycleLimit = ~0ull; // no stop: it runs to its end
            rt->functions.generated(function)(ram, &handler, rt);
            const uint32_t reply = handler.r[2].uw[0];
            if (std::getenv("JC2_TRACE_SIF") && calls < 64)
                std::printf("[jc2:sif] EE 0x%x command 0x%x done: reply 0x%x, pc 0x%x (vblank %llu)\n", service, command,
                            reply, handler.pc, (unsigned long long)bozo::port::vblankCount(rt));
            if (!out.empty())
            {
                if (!reply)
                    throw std::runtime_error("JC2 EE RPC handler gave no reply for service " + std::to_string(service));
                std::memcpy(out.data(), ram + (reply & 0x1FFFFFFu), out.size());
            }
        };
    }

    void sifRegisterRpc(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        static const bool trace = std::getenv("JC2_TRACE_SIF") != nullptr;
        const uint32_t service = ctx->r[5].uw[0], function = ctx->r[6].uw[0], buffer = ctx->r[7].uw[0];
        const uint32_t gp = ctx->r[28].uw[0];
        uint32_t stack = 0;
        if (const auto *thread = bozo::port::kernel(rt).thread(bozo::port::currentThread(rt)))
            stack = thread->stack;
        if (trace)
            std::printf("[jc2:sif] EE server 0x%x: function 0x%x buffer 0x%x, stack bottom 0x%x (ra 0x%x)\n", service,
                        function, buffer, stack, ctx->r[31].uw[0]);
        if (!stack || !rt->functions.generated(function))
            throw std::runtime_error("JC2 EE RPC server without a thread stack or a compiled handler");
        jc2_func_0x11d290(ram, ctx, rt);
        registerEeServer(rt, {service, function, buffer, gp, stack});
    }

    // Bring-up probe (JC2_TRACE_MPEG=1): the movie player's slot check (0x155380, queue in a0): its count, capacity and fields.
    void probeMovieQueue(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        static unsigned n = 0;
        static const bool trace = std::getenv("JC2_TRACE_MPEG") != nullptr;
        const uint32_t q = ctx->r[4].uw[0];
        if (trace && n++ < 4)
        {
            uint32_t w[12];
            std::memcpy(w, ram + (q & 0x1FFFFFFu), sizeof(w));
            std::printf("[jc2:movie] queue 0x%x:", q);
            for (uint32_t v : w)
                std::printf(" %08x", v);
            std::printf("\n");
        }
        rt->functions.generated(0x155380u)(ram, ctx, rt);
    }

    // Bring-up probe (JC2_TRACE_MPEG=1): a library stub's calls (the first few), answered by BozoEngine as before.
    template <uint32_t Address> void traceLibrary(uint8_t *, R5900Context *ctx, Runtime *rt)
    {
        static unsigned n = 0;
        static const bool trace = std::getenv("JC2_TRACE_MPEG") != nullptr;
        const uint32_t a0 = ctx->r[4].uw[0], a1 = ctx->r[5].uw[0], a2 = ctx->r[6].uw[0], ra = ctx->r[31].uw[0];
        bozo::port::callLibrary(rt, ctx, Address);
        if (trace && n++ < 6)
            std::printf("[jc2:lib] 0x%x(0x%x, 0x%x, 0x%x) from 0x%x -> 0x%x (vblank %llu)\n", Address, a0, a1, a2, ra,
                        ctx->r[2].uw[0], (unsigned long long)bozo::port::vblankCount(rt));
        ctx->pc = ra;
    }

    // Save loading (JC2_TRACE_SAVE=1): the CRC check 0x166390(length, data, ...): CRC32 of `length` bytes against the
    // word after them, 1 when it matches; and the card poll 0x165640(&command, &result): 0 busy, 1 done, 2 error.
    const bool traceSave = std::getenv("JC2_TRACE_SAVE") != nullptr;
    void saveCrc(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        const uint32_t length = ctx->r[4].uw[0], data = ctx->r[5].uw[0], a2 = ctx->r[6].uw[0], ra = ctx->r[31].uw[0];
        rt->functions.generated(0x166390u)(ram, ctx, rt);
        uint32_t stored = 0, first = 0;
        if (data + length + 4 <= 0x2000000u)
            std::memcpy(&stored, ram + data + length, 4), std::memcpy(&first, ram + data, 4);
        std::printf("[jc2:save] crc(0x%x, 0x%x, 0x%x) from 0x%x: stored 0x%08x, first word 0x%08x -> %u\n", length, data, a2,
                    ra, stored, first, ctx->r[2].uw[0]);
    }
    void cardPoll(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        const uint32_t command = ctx->r[4].uw[0], result = ctx->r[5].uw[0], ra = ctx->r[31].uw[0];
        rt->functions.generated(0x165640u)(ram, ctx, rt);
        if (ctx->r[2].uw[0] != 0)
        {
            uint32_t c = 0, r = 0;
            std::memcpy(&c, ram + (command & 0x1FFFFFFu), 4), std::memcpy(&r, ram + (result & 0x1FFFFFFu), 4);
            std::printf("[jc2:save] poll from 0x%x -> %u (command %d, result %d)\n", ra, ctx->r[2].uw[0], int32_t(c), int32_t(r));
        }
    }

    // The game's pool: 0x152160(pool, size) allocates, 0x152240(pool, block) frees (blocks around the save buffer).
    bool poolTraced(uint32_t a) { return a >= 0x1B00000u && a < 0x1B60000u; }
    void poolAlloc(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        const uint32_t pool = ctx->r[4].uw[0], size = ctx->r[5].uw[0], ra = ctx->r[31].uw[0];
        rt->functions.generated(0x152160u)(ram, ctx, rt);
        if (poolTraced(ctx->r[2].uw[0]))
            std::printf("[jc2:pool] alloc(0x%x, 0x%x) = 0x%x from 0x%x (vblank %llu)\n", pool, size, ctx->r[2].uw[0], ra,
                        (unsigned long long)bozo::port::vblankCount(rt));
    }
    void poolFree(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        if (poolTraced(ctx->r[5].uw[0]))
            std::printf("[jc2:pool] free(0x%x, 0x%x) from 0x%x (vblank %llu)\n", ctx->r[4].uw[0], ctx->r[5].uw[0],
                        ctx->r[31].uw[0], (unsigned long long)bozo::port::vblankCount(rt));
        rt->functions.generated(0x152240u)(ram, ctx, rt);
    }

    // Bring-up probe (JC2_TRACE_STREAM=1): a function's calls with their arguments and result (the sound stream's EE
    // side: the M2SOUND server's helpers).
    const bool traceStream = std::getenv("JC2_TRACE_STREAM") != nullptr;
    template <uint32_t Address> void traceCall(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        static unsigned n = 0;
        const uint32_t a0 = ctx->r[4].uw[0], a1 = ctx->r[5].uw[0], a2 = ctx->r[6].uw[0], a3 = ctx->r[7].uw[0],
                       ra = ctx->r[31].uw[0];
        if (bozo::port::vblankCount(rt) > 3000 && n < 400)
            std::printf("[jc2:stream] enter 0x%x(0x%x, 0x%x) from 0x%x, pc 0x%x\n", Address, a0, a1, ra, ctx->pc);
        if (auto f = rt->functions.generated(Address))
            f(ram, ctx, rt);
        else
        {
            bozo::port::callLibrary(rt, ctx, Address);
            ctx->pc = ra;
        }
        if (bozo::port::vblankCount(rt) > 3000 && n++ < 400)
            std::printf("[jc2:stream] 0x%x(0x%x, 0x%x, 0x%x, 0x%x) from 0x%x -> 0x%x (vblank %llu)\n", Address, a0, a1, a2,
                        a3, ra, ctx->r[2].uw[0], (unsigned long long)bozo::port::vblankCount(rt));
    }

    void install(Runtime &, bozo::port::Registry &r)
    {
        if (traceStream)
        {
            r.registerFunction(0x234a60u, traceCall<0x234a60u>); // the M2SOUND EE server's handler
            r.registerFunction(0x486920u, traceCall<0x486920u>);
            r.registerFunction(0x4869c0u, traceCall<0x4869c0u>);
            r.registerFunction(0x2360f0u, traceCall<0x2360f0u>);
        }
        if (traceSave)
        {
            r.registerFunction(0x152160u, poolAlloc);
            r.registerFunction(0x152240u, poolFree);
            r.registerFunction(0x166390u, saveCrc);
            r.registerFunction(0x165640u, cardPoll);
        }
        r.registerFunction(0x117cd0u, traceLibrary<0x117cd0u>); // sceMpegInit
        r.registerFunction(0x117d70u, traceLibrary<0x117d70u>); // sceMpegCreate
        r.registerFunction(0x1181d0u, traceLibrary<0x1181d0u>); // sceMpegAddCallback
        r.registerFunction(0x117490u, traceLibrary<0x117490u>); // sceMpegAddStrCallback
        r.registerFunction(0x117180u, traceLibrary<0x117180u>); // sceMpegDemuxPssRing
        r.registerFunction(0x117ff0u, traceLibrary<0x117ff0u>); // sceMpegGetPicture
        r.registerFunction(0x118108u, traceLibrary<0x118108u>); // sceMpegIsEnd
        r.registerFunction(0x155380u, probeMovieQueue);
        r.registerFunction(kSifRegisterRpc, sifRegisterRpc);
        r.registerFunction(kSifBindRpc, sifBindRpc);
        r.registerFunction(kFrameWait, frameWait);
        // Native graphics: every retired GIF/VIF/GS register access stops the run. Opt-in (JC2_NATIVE_GRAPHICS=1) until
        // the game's register sites (docs/RECOMP_NOTES.md, "Graphics register sites") are all replaced by native draws;
        // meanwhile BozoEngine's I/O library completes them without any GS behind them.
        if (const char *v = std::getenv("JC2_NATIVE_GRAPHICS"); v && v[0] == '1')
            r.nativeGraphics(&nativeInstruction);
        r.registerFunction(kIpuInit, ipuInit);
    }

    // Save states: the game's EE RPC servers, registered again on load (those of the run being replaced dropped).
    void portState(bozo::state::Archive &a, Runtime &rt)
    {
        if (a.loading())
            for (const auto &[service, server] : eeServers)
                bozo::port::libraries(&rt).sif.eeServers.erase(service);
        uint64_t n = bozo::state::count(a, eeServers.size());
        std::map<uint32_t, EeServer> loaded;
        if (!a.loading())
            for (auto &[service, e] : eeServers)
                bozo::state::fields(a, e.service, e.function, e.buffer, e.gp, e.stack);
        else
            for (uint64_t i = 0; i < n; ++i)
            {
                EeServer e;
                bozo::state::fields(a, e.service, e.function, e.buffer, e.gp, e.stack);
                loaded[e.service] = e;
            }
        if (a.loading())
        {
            eeServers.clear();
            for (const auto &[service, e] : loaded)
                registerEeServer(&rt, e);
        }
    }
} // namespace jc2
