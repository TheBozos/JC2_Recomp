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
#include <set>
#include <span>
#include <stdexcept>
#include <string>

namespace jc2
{
    void notifyFrame();

    // The frame wait. JC2's flip (0x15A150) counts its frames (0x6D6190) and waits in 0x15B400(target, &late) until the
    // vblank handler's counter (0x6D61AC) reaches the frame count; a late frame sets the counter back to target - 1 and
    // counts itself (0x6D61B0). It returns the CSR FIELD bit (no GS here: the field parity of the vblanks started),
    // which picks the half-line offset of the next field. The game steps one fixed frame per call: the frame ends here.
    constexpr uint32_t kFrameWait = 0x0015B400u;
    constexpr uint32_t kVBlankCounter = 0x006D61ACu;

    void frameWait(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        const uint32_t target = ctx->r[4].uw[0], late = ctx->r[5].uw[0];
        const uint32_t onTime = 0u;
        std::memcpy(ram + (late & 0x1FFFFFCu), &onTime, 4);
        uint32_t counter;
        std::memcpy(&counter, ram + kVBlankCounter, 4);
        if (static_cast<int32_t>(counter - target) < 0)
            std::memcpy(ram + kVBlankCounter, &target, 4);
        bozo::port::frameEnd(rt, ctx);
        notifyFrame();
        ctx->r[2].ud[0] = bozo::port::vblankCount(rt) & 1u;
        ctx->pc = ctx->r[31].uw[0];
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
        bozo::port::libraries(rt).sif.eeServers[service] =
            [ram, rt, function, buffer, gp, stack, service](uint32_t command, std::span<const uint8_t> in,
                                                             std::span<uint8_t> out)
        {
            static int depth = 0;
            static unsigned calls = 0;
            if (std::getenv("JC2_TRACE_SIF") && calls++ < 64)
                std::printf("[jc2:sif] EE 0x%x command 0x%x, %zu bytes in, %zu out (depth %d)\n", service, command,
                            in.size(), out.size(), depth);
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
            handler.cycleLimit = ~0ull; // no stop: it runs to its end
            rt->functions.generated(function)(ram, &handler, rt);
            const uint32_t reply = handler.r[2].uw[0];
            if (!out.empty())
            {
                if (!reply)
                    throw std::runtime_error("JC2 EE RPC handler gave no reply for service " + std::to_string(service));
                std::memcpy(out.data(), ram + (reply & 0x1FFFFFFu), out.size());
            }
        };
    }

    void install(Runtime &, bozo::port::Registry &r)
    {
        r.registerFunction(kSifRegisterRpc, sifRegisterRpc);
        r.registerFunction(kSifBindRpc, sifBindRpc);
        r.registerFunction(kFrameWait, frameWait);
        r.registerFunction(kIpuInit, ipuInit);
    }
} // namespace jc2
