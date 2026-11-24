# Jade Cocoon 2 (NTSC-U, SLUS-20309) recompilation notes

## The disc

| File | Notes |
|---|---|
| `SLUS_203.09` | SHA-1 `fa778a8dcb5a16e8146976fe4ab444856f04c47b`, stripped (empty `.symtab`), one 6 MB `WAX` segment at `0x00100000`, entry `0x00100008` |
| `CDDATA.000` / `CDDATA.LOC` | 1.3 GB data archive and its index |
| `IRX/` | `IOPRP23.IMG`, Sony modules (`LIBSD`, `MCMAN/MCSERV`, `PADMAN`, `MTAPMAN`, `SIO2MAN`), HDD stack (`ATAD`, `DEV9`, `HDD`, `PFS`), i.mode (`IMODEDRV`, `IMODEMAN`, `M2IMODE`), and Genki's own `M2IOPSYS`, `M2SOUND`, `MODHSYN`, `MODMIDI`, `UAMAN` |
| `MOV/`, `VOICE/` | movies and voice streams |

`SYSTEM.CNF` sets `HDDUNITPOWER = NICHDD`, so the game may probe the HDD.

## Pipeline (BozoEngine, since 2026-11-15)

The port is built on BozoEngine (TheBozos/BozoEngine, the `BozoEngine` submodule). Everything is made at build time
from the player's own files in `game/`, into `build/`:

1. `tools/jc2_bozo_prepare.py`: `SLUS_203.09` with its code spans described as sections (`0x100000-0x4D8800`,
   `0x4E3768-0x4E3E80`, `0x6B2B00-0x6B9310`; the VU1 microcode and the data left out), and the Ghidra function map as
   explicit entries (`recomp/ghidra/jc2_functions.csv`).
2. `bozo-externals` with `recomp/jc2_symbols.txt` (what BozoEngine answers) and the port's registrations
   (`tools/jc2_bozo_overrides.py` reads them from `runtime/jc2_*.cpp`).
3. `bozo-recomp`: 75,380 functions, 0 untranslated instructions.
4. `tools/jc2_bozo_audio.py`: the IOP modules the game talks to, compiled offline to native C++ (BozoEngine's
   `tools/native_irx`): `LIBSD`, `MODMIDI`, `MODHSYN`, `UAMAN`, `IMODEMAN`, `M2IOPSYS`, `M2SOUND`, `M2IMODE`.
5. `jc2-headless` (no window yet): `cmake -S . -B build/bozo_game -G Ninja && cmake --build build/bozo_game`.

The log's first weeks describe the earlier build, on an outside recompiler and its runtime; that build and its
scripts were removed from the repository on 2026-11-23.

## Log

### Day 0 (2026-10-27)

- The earlier recompiler at `c0b08a0`; it already has `58b5c8b`, `02398e0`, `9dc213a`, `ebc558a` and `281f7d9`.
- `ps2_analyzer`: 244 SDK functions to stub, 324 recognised without a runtime handler, 40 jump tables.
- Ghidra headless: 72,636 entries. The first recompile had 69,751 unhandled instructions, all in data
  (0x500000 and up) that shares the code's segment. `tools/filter_ghidra_map.py` drops the entries in the data spans
  (`ghidra.sh` runs it); the recompile is then clean: 72,296 functions, 252 stubs, 0 errors, 6,044 unresolved
  JR/JALR handled by fallback entries. `ps2_vu_aot`: 9 VU1 microcode fragments.
- Code ranges: `0x00100000-0x004E3E80` (ends with a kernel-mode handler at `0x4E3DE8`, `lui k0,0x8007` / `eret`,
  probably copied into kernel memory) and `0x006B2C10-0x006B9310`.
- **Overlays:** the ELF carries ~1,390 zero-size LOAD segments at `0x00940000` (memsz 0x800-0x16800), so the game
  loads code there at runtime. That code is not in the ELF and is not recompiled yet.
- Runner build: 16 min at -j16 (2,468 steps), debug info split off.

## Boot

1. `SetupHeap base=0x97e800`, then at once `[guest-branch:missing-target] JALR 0x10dc04 -> 0x6b2b00`: crt0 runs the
   static constructors (table `0x6B9310-0x6B9764`, 277 pointers) and the first one, `0x6B2B00`, was in a range the
   map filter had dropped as data. The late code block starts at `0x6B2B00` (a pointer table and zero padding before
   it), not at `0x6B2C10`; every other constructor was already in the code ranges. Filter fixed; `ghidra.sh` now keeps
   the unfiltered export in `build/ghidra/functions_unfiltered.csv`.
2. The constructors run; `SifInitRpc`, thread 2 (entry `0x150A60`, priority 4), two GS clear packets and display
   uploads. Then the main thread spins at `0x1249B0`: `FUN_00124910` binds cdvdfsv RPC `0x8000059A`
   (`CD_SERVER_DISKREADY`) and retries while `client->server` stays 0. Nothing serves it: the game binds it before
   any IOP reset or module load, so it expects the boot ROM's CDVDFSV, and the analyzer did not recognise this
   function. It is libcdvd's `sceCdDiskReady` (mode in a0, `sceSifCallRpc` at 0x11CFC8, returns the reply word), now
   stubbed to the runtime's handler. The other cdvdfsv binders: `0x80000592` in `sceCdInit` and `0x80000597` in
   `sceCdSearchFile` (both stubbed); `0x80000595` (0x1241A8) and `0x80000593` (0x1244C0) are only called from stubbed
   functions.
3. Past the disc check: `sceCdSearchFile("\CDDATA.LOC;1")` (lsn 0x100000, 0x14814 bytes) and `sceCdRead` of its 42
   sectors to `0x97F1E0` from the loader thread (2, `0x150A60`). The main thread then waits in `WaitSema`
   (`0x119F20`, called from `0x485BD0` on the semaphore its argument points to) for the rest of the run.
   Inlined-stub scan (FFOP's generic recompiler bug: a stub address turned into a goto label): 0 in JC2.
4. Why it waits (boots 4-5, `PS2X_STATE_DUMP` and `JC2_TRACE_SYNC=1`): both EE threads wait on semaphore 4, and
   the IOP has run no instruction (JC2 loads no module yet). Semaphore 4 (`0x6C4764`) is the loader's mutex and
   semaphore 5 (`0x6C4768`) its request event; the helpers are wait `0x485BD0(&id)` and signal `0x485C30(&id, isr)`.
   The loader (thread 2) takes the mutex and issues `sceCdRead`; the HLE completes it at once and queued the
   `sceCdCallback` function `0x1509E0` as an interrupt, which ran on top of the loader (thread=2, ra=0) before
   `sceCdRead` returned. Its first step is to take the mutex the loader holds: deadlock, and the main thread queues
   behind it. On hardware `sceCdInitEeCB` gives libcdvd a callback thread, which just waits for the release; the
   runtime's `sceCdInitEeCB` did nothing. Fork fix on `jc2/cd-callback-thread`: `sceCdInitEeCB(priority, stack,
   size)` creates that thread, and read completions start the callback on it (one at a time).
5. Boot 6, on the fix: the CD callback now runs on its own thread (3), and the first overlay image is captured
   (`build/overlays/ovl_fa0b7fafeef6b3f2.bin`, 0x40000 bytes at 0x940000). Still stuck, differently: the loader
   (2) waits on semaphore 5 while it holds mutex 4, and the callback, which signals 5, waits for 4 (main too).
   Just before, the loader's `SignalSema(4)` (`0x485C30` from `0x15036C`) returned -1, a signal refused as over
   the maximum: if the runtime has the wrong maximum for 4, a release is lost there. Boot 7 traces `CreateSema`.
6. Boot 7 (`CreateSema` traced): semaphores 1-4 are all `init 1, max 1` (4 from `0x485B10`, the mutex). The
   runtime reads them right; JC2 just signals over the maximum as a matter of course (the first `SignalSema`, on
   3, already returns -420 `KE_SEMA_OVF`). With the maximum enforced, the loader's release-before-take of mutex 4
   is dropped, it then holds the only token while it waits on 5, and the callback, which signals 5, can never
   take 4: a deadlock that does not depend on timing. The retail game runs, so the EE kernel most likely does not
   cap the count. Fork branch `jc2/ee-sema-overflow`: EE `SignalSema` counts past `max_count` (IOP untouched).
   Merged to main as 5b67c2b. Boot 12 still logs it for semaphores 3 and 4.
7. Boots 8-10: M2IOPSYS binds the EE servers 0x10001/0x10003 (IOP -> EE RPC, merged) and needs the EE -> IOP
   handoff (`jc2/ee-iop-handoff`, local, tests in, not pushed). Then EE thread 1 ends with pc 0, ra 0.
8. Boots 11-14, why main ends. Not the scene loop: its state (`0x6C47B0`) stays 0, and main never calls `0x15C140`.
   Main's first call `0x15C0E0` saves ra (0x141D28) at 0x1FFFFE0 (its sp, main's stack is 0x1F00000-0x2000000),
   runs `0x15B780`, `0x15C210` and `0x165390` to the end, then returns to 0: the slot was zeroed meanwhile, by thread
   1 itself (boot 13, `JC2_WATCH`). Boot 14 (`JC2_GUARD`, a guard page on that host page) names the writer:
   `ps2_syscalls::ReferThreadStatus` (Thread.cpp:375, `*status = {}`), once per frame (every ~4.92M EE cycles), with
   its status struct at about 0x1FFFFD0. That is a handler's frame on an invocation stack: interrupt, alarm and
   callback invocations queued with sp 0 get `EeScheduler::invocationStackTop()`, which takes stacks from
   `PS2Runtime::reserveAsyncCallbackStack`, and those start at `m_asyncCallbackStackTop = PS2_RAM_SIZE`.
   `configureReservedMemory`, which would lower that top and set a floor, has no caller. So the first
   invocation stack is 0x1FFC000-0x1FFFFF0: the top of main's stack. JC2's per-frame handler (the vblank
   interrupt, or its alarm) writes over main's frames. Fix to make in the fork: keep the invocation stacks out of
   the main thread's stack, either by calling `configureReservedMemory` with a region below the main stack (the
   ELF's `SetupThread` stack/size give it), or by reserving them from the guest heap. Any port whose main stack is
   at the top of RAM (SetupThread stack -1) can be hit the same way.

## Overlays (plan)

- The ELF has no code for `0x940000`: no `lui 0x94`, no `jal` into it, and the data words that point there look like
  asset data. The overlays come from `CDDATA.000` at run time and are reached through registers.
- The fork recompiles overlays as modules (README "Overlay modules"): wrap each image into an ELF at its load
  address, map it with Ghidra, recompile with `module_name` / `module_load` / `module_guard_size`; at run time an
  address with no main-ELF function resolves in the module whose guard bytes match guest memory.
- To do once boot reaches them: capture each image as the game loads it (SCTPT2 does an overlay capture too), then
  one module per image.

## For later (room findings)

- Below the field rate: TMB's vblank hold (fork `346ccc9`, `EeScheduler::setVBlankHold`/`setVBlankHoldLimit`): a game
  override turns it on when the frame's work starts after its vblank wait and off when processing ends, so a frame
  just over 16.7 ms does not wait a whole extra field. Hook points to find in JC2 once it renders.
- Risky HLE (FFOP): `sceVif1PkOpenDirectCode`/`CloseGifTag`/`AddGsAD` broke packets in FFOP; JC2 binds none of them.
- Heap corruption suspect (room, FFOP): if the runtime switches EE threads where hardware never would, two threads
  can carve the same newlib chunk (`__malloc_lock` is a no-op in many PS2 builds). JC2 has a second thread from
  boot (entry `0x150A60`): suspect it first if JC2 shows overlapping allocations.
- To pick up: fork main `0a62202b..61788601` (VU1 AOT about 2.8x faster on the exact path; SC3 40 -> 60 fps). Update
  the submodule, rerun `scripts/recomp.sh` (ps2_vu_aot), full rebuild. Batched with the next runtime change.
- If JC2 dies with "EE invocation stack space exhausted" (VP2S): `EeScheduler::invocationStackTop` keeps one 16 KiB
  stack per (thread, nesting depth) forever, from the reserved range past its HLE pools; enlarge the range.
- Possible generic VU1 race (FFOP, after main's vu1/aot-speed): an intermittent endless microprogram with garbage
  lower ops, as if a fragment was not refreshed after an upload. If JC2 hits a VU1 stall, dump VU1 code memory.
- With `jc2/ee-sema-overflow` (sc3-port): a vblank or interrupt handler that `iSignalSema`s a max-1 semaphore every
  field now accumulates signals while its waiter is late, so the waiter catches up back to back. Doubled game
  ticks after a hitch would point here.
- Sound, expected later (FFOP, room): LIBSD `sceSdVoiceTrans`/`sceSdBlockTrans` stuck busy (-201) and a sound thread
  waiting forever on a semaphore, as if the SPU2 DMA completion interrupt (IOP DMA 4/7) or the handler set with
  `sceSdSetTransIntrHandler` never fires. JC2 loads Sony LIBSD (and Genki's M2SOUND/MODHSYN/MODMIDI on top).
- Renderer (BaldHuman, 2026-10-27): paraLLEl-GS is to be dropped from every port, JC2 included, once sc3-port's GPU
  renderer (gs/gpu-renderer) is done. Do not make JC2 depend on paraLLEl-GS; check JC2 on the CPU renderer and the
  new GPU renderer.
- Known main regression (room, 2026-10-27): merge `6dd26a0` (`sctpt2/ipu-in-dma`) gives VP2S a black screen; fix on
  `sctpt2/ipu-release`. JC2's local integration tree is on `6dd26a0`: a frozen screen around movies points here first.
- Room regression harness (sc3-port, `tools/port_regress.sh` on `tools/port-regress`): each port provides
  `scripts/regress.sh <build-dir> <out-dir>` writing `<out-dir>/summary.txt` (key=value: port, recompiler, route_reached,
  exit ok|hang|crash|timeout, duration_s, frames_presented, fps_mean, fps_p10, frames_over_25ms,
  gs_frames_per_5s_median/min, spu2_underruns, error_lines, warning_lines; NA when unknown), `last_frame.png` and
  `run.log`. To add for JC2 once it reaches a scripted scene.
- `ffop/iop-module-start-thread` (FFOP, coming): IRX start routines run on a loader thread, so waits inside them (LIBSD's)
  block instead of returning -418. Also covers the IOP -> EE RPC WAIT call made outside any IOP thread, which
  `jc2/iop-to-ee-rpc` can only log. Pick it up when it lands.

## Missed functions to look for (VP2S, 2026-10-28)

Each one hangs only when the game first calls it: the runtime logs `[guest-branch:missing-target]` and the thread
goes dormant. (1) C++ this-adjusting thunks (`j method` + `addiu a0,a0,-N` + 2 nops, 16-aligned) after .text,
before the vtables; (2) one-instruction virtual methods (`jr ra` + delay slot, padded) in the same place;
(3) branch targets that cross into another function (needs its own entry); (4) overlay modules not on the disc
in a form found statically: hook the game's module init and save them at run time (JC2: 0x940000); (5) a CMake
`file(GLOB)` of module dirs without `CONFIGURE_DEPENDS` leaves new modules out of the link. Scans:
VP2S_Recomp `tools/find_entry_points.py`, `tools/wrap_elf.py`.

## Display and movies (boots 16-29, jc2/native)

- Zoomed/cropped intro and menu: JC2 calls `sceGsSetDefDispEnv(640x224)` before `sceGsResetGraph(interlace,
  frame)`. libgraph builds the display from its parameters, whose defaults (`0x4E2AF0`: 1, 2, 1) are already
  interlaced NTSC frame mode, so DH is 447; the runtime stub read SMODE2 (still 0) and wrote DH 223, and the
  presenter showed the top 112 lines doubled. Fork fix cfbdc01 (stub takes the mode from `g_gparam`).
- The intro story, the title and the long dark part after it are all one movie, `MOV/ADV0.PSS` (one decoder
  instance, 2700 pictures, ~90 s at 30 fps). Each picture is uploaded as 1120 16x16 IMAGE transfers (CT32, TBP
  0x2520) and drawn as one sprite over the 640x224 frame. The dark part's pictures are dark as decoded (GS trace
  `build/gs/dark28.gsr`, rebuilt uploads); not checked against a real console yet. START at the title opens the
  main menu; idling on the menu starts the 3D demo.
- Tools: `PS2X_GS_RECORD` + `tools/gs_trace_dump.py` (decode a trace, rebuild uploads), `ps2_gs_replay --present`
  (picture as the window shows it), `PS2X_MPEG_TRACE_LIMIT`, `PS2X_PAD_SCRIPT` (START opens the menu, not CROSS).

## Sound

- Default SPU2 path: white noise (24k zero crossings/s, RMS 18900 flat, clipping). `PS2X_SPU2_WALLCLOCK=1`
  (now the default in `scripts/run.sh`): music, but ~277 AutoDMA underruns per 75 s (`iop_spu2.cpp`
  `fillInputHalf`): the IOP driver restarts core0's AutoDMA late because the IOP only advances with the EE. The
  fix is the IOP on the PC clock (native layer 2, not written yet).

## Native path, boots 30-51 (2026-10-29 night)

- Native IOP: all 13 IRX recompiled (`scripts/iop_recomp.sh`, fork `tools/iop_recomp.py` from
  `jc2/iop-recomp-scan`: pointer-reached functions, prologues after a function end), `PS2X_IOP_NATIVE=1` by
  default; boot 33 compared them with the interpreter by behaviour: identical SPU2 counters (key-ons, AutoDMA
  bytes, underruns, peak) and identical sound loudness. `PS2X_IOP_NATIVE_CHECK=1` reported nothing, but it only
  checks TMB's voice search (VP2S, 2026-10-29), so it says nothing for JC2's modules. Export the variables: a bare `VAR=1 cmd` inside another `bash -c` did not reach the runner (boot 31).
- Sound: `PS2X_SPU2_WALLCLOCK=1` + `PS2X_IOP_WALLCLOCK=1` by default; the 230 "AutoDMA underruns" per 75 s leave
  no silent gap in the output (boots 33, 37).
- Native GS: `PS2X_GS_RENDERER=native` by default. Fork fix 732b622: the backend wrote depth through
  `gl_Position.z = z*2-1`, which collapses small Z; JC2's movie sprite (Z 1 over a Z 0 clear, ZTST GREATER) was
  dropped. Captures from the native backend are 1280 wide with a white right half (capture buffer, not the screen).
- Movies: libmpeg's `sceMpegIsEnd` (0x118108) was missed by the analyzer; left native it read libmpeg state the HLE
  never sets, so the opening movie never ended (manual stub in the TOML).
- Code overlays: 1383 CodeWarrior `MWo3` modules in CDDATA.000 (`tools/find_overlays.py`; slots 0x940000 (559),
  0x963000 (347), 0x967000, 0x96A800, 0x96B000, 0x972000, 0x976800). `scripts/overlays.sh`: VP2S_Recomp's
  `wrap_elf.py`, `tools/overlay_functions.py` (function map without Ghidra), ps2_recomp module mode (guard: the 64-byte
  header with the module's name) into `generated/overlays/`; one 23-minute build. 1368 build; 15 `SC_*` modules
  (non-empty constructor table) still fail in wrap_elf/ps2_recomp.
- Main ELF: 99 functions reached only through pointers or from overlays added to `entry_points`
  (`tools/overlay_main_targets.py`; boot 46 stopped at 0x194FB0, a leaf after `jr ra; addiu sp` + padding).
- `scripts/recomp.sh` keeps `generated/iop` and `generated/overlays` (`tools/sync_tree.py --keep`).
- Now (boot 48-51): New game -> opening movie -> first in-game 3D scene (the hero with his portrait, no text box),
  which never moves on: no button changes it (boot 50), the main thread runs its scene loop, the loader idles, the
  sound driver (IOP RPC 0x10000, fno 0x102, server 0xAE3E0 in M2SOUND) is polled every frame and the EE keeps
  sending audio data over SIF DMA. Hypothesis: the scene waits for a voice stream to end. Not confirmed.
- First in-game scene (boots 52-56): overlay m2esA001's script (0x940710, run every frame) is a list of command
  calls (0x1EB5D0 for most, interpreter with a jump table at 0x6944A0; also 0x1F4B20, 0x1F3390), each returning
  non-zero while running (the script then yields). It stops at command 0xCB (handler 0x1F0DC4): done when the global
  0x879FAC == 2 (a three-way cursor kept by 0x48BD20, Left/Right; that routine never runs in this scene) or when
  0x477C50(0) returns 0 (0x41DA80: status of one of four slots in a table at 0x8B9860, probably memory-card
  ports/slots). Meanwhile the game finds no save (`GetDir '/BASLUS-20309MAYU2'` -> 0 entries) and polls the card
  (GetInfo + Sync, ~216 times). Hypothesis: a memory-card prompt after "no save found" that is neither drawn nor
  driven. JC2_TRACE_SCRIPT=1 logs the script's commands, JC2_WATCH=879fac the cursor.
- Solved (boots 57-62): the slots at 0x8B9860 are file-load requests (state +0x10, handle +0xC; 0x41D3F0 steps them).
  Slot 0 waited in state 9 for the stream record of its handle (0x89DD40 + handle*64 + 0x280: chunk 0x2000 bytes,
  EE source +0x290, IOP buffer +0x2A4, bytes in flight +0x2B0) to come back to 0 bytes in flight. The EE sends each
  chunk to the IOP with sceSifSetDma (0x486920), and the IOP answers through an EE RPC server (sid 0x10001,
  handler 0x234A60, codes 0x400-0x405; 0x403 = chunk done). The runtime refused that call ("[sifrpc] IOP -> EE
  call sid=0x10001 fno=1027: no EE server queue, refused"): SifRegisterRpc read cfunc, cbuf and qd from the stack,
  while the EE EABI passes them in t0-t2, so the server's queue was 0. Fixed on jc2/native c3ae3e1. Boot 62: the
  scene goes on (the hero walks the room with a "!" bubble, the next shots and portraits follow).
  JC2_WATCH=<addr>,<bytes> logs a whole range when any word of it changes (needs JC2_TRACE_SCENE=1).
- Dialogue text box (boots 62-66): the scene ran, but the text box was never drawn (GS record: no draw at all, on the
  native and the CPU GS alike). One `[guest-branch:missing-target]` gave it away: 0x10DC04 (the static constructor
  walk run when an overlay loads) called 0x964080, SC_A005's static initializer. CodeWarrior keeps the initializers
  at the end of the data, just before the constructor table (header +0x18..+0x1C), and the table follows text + data
  on the disc. `tools/find_overlays.py` cut each module at text + data, so the table was lost and
  `tools/overlay_functions.py` never listed the initializers: 174 modules (SC_* scenes, BTLEF_* battle effects).
  Now the extract keeps the table, the function map lists the initializers, and `tools/wrap_overlay.py` runs VP2S's
  `wrap_elf.py` with JC2's text end (text size + 0x40) so its .init/.ctors sections do not overlap .text. Boot 66:
  the dialogue box and its text show, and the conversation moves on (Levant appears).

## Frame rate and the native frame loop (boots 67-76, 2026-10-29)

- On the vblank grid (the default) JC2 holds 60 fps: `JC2_FPS=1` samples the flip's frame count (0x6D6190) and its
  late frames (0x6D61B0) every 5 s; boot 75: 59.8-60.0 through the intro, menus and the first scene, one dip to 31-49
  during a scene load (143 late frames).
- The frame wait is 0x15B400(target, &late): the vblank handler 0x15B360 counts vblanks (0x6D61AC) and wakes the main
  thread (0x6D61A4 set before it sleeps); a late frame sets the counter back and counts itself; the wait returns the
  CSR FIELD bit (half-line offset of the next field). All flips go through 0x15A150 (called from 0x15BC80).
- `JC2_NATIVE=1` replaces the wait with a FramePacer frame end (as VP2S/TMB): a steady 59.6 fps, the game goes on,
  but every frame is black, with the native or CPU GS, presented at the frame end, after the flip, newest buffer or
  on the vblanks (`JC2_NATIVE_FRAMES=0`, boot 76). Cause: the main thread never sleeps, so the lower-priority threads
  that run while it waits for the vblank starve. A JC2 native loop has to let them run (a real sleep to the next host
  frame) before it can replace the grid. Wrapping the flip 0x15A150 broke the game (boot 74): it sleeps inside.
- VU1: `PS2X_VU1_STATS=1` (jc2/native 1453fde) counts the microcode pairs run compiled and interpreted. Boot 77
  (intro, menus, first scene): 509M pairs compiled by `ps2_vu_aot`, 0 interpreted.
- Movie sound: with sc3's AutoDMA wall-clock change (sc3/native-spu2-adma 4fc53883) JC2's movie soundtracks (PCM
  through core 0's AutoDMA sound data input) came out as white noise (boot 77: 24000 zero crossings/s, ~180
  clipped samples/s); the same build without it is clean (boot 78: ~1000 crossings/s, nothing clipped). Reverted on
  jc2/native (3465dff) and reported; sc3 is fixing it.
- Native frame loop, working (boot 82, default since): the frame end blocks the main thread with
  `EeScheduler::waitExternal`; a host thread sleeps until the frame deadline (FramePacer cap, 1/cap after the previous
  deadline, or now when late) and posts a task that charges the real EE cycles, presents the frame and completes the
  wait. JC2's lower-priority threads run while the main thread waits. 57-60 fps through the intro, menus, opening
  movie and first scene with its dialogue; the scene-load dip is 57 fps (31 on the vblank grid).
- Open (intermittent): boot 84 (native loop, CROSS every 1 s from 200 s) stayed black from the end of the opening
  movie (~220 s) to the end of the run, the game running at 60 fps; boot 85, the same input, played the scene.
  A race; candidate: the IOP's instant CD completions (TTA's tlotr/iop-cd-timing, PS2X_IOP_CD_SECTORS_PER_SECOND).
- Frame cost (boot 90, `JC2_FPS=1` logs the frame work at the cap): menus and movies 2-3 ms mean, the first in-game
  scene 7.5-15.7 ms mean and 18-26 ms worst (60 fps leaves 16.7 ms). Profile of that scene (boot 91,
  `PS2X_PROFILE=50 PS2X_PROFILE_DELAY=230`, symbolized with MSYS2 `addr2line` against `ps2EntryRunner.debug`): the EE
  thread is busy 48%, about half in the recompiled game code; then the VIF unpack (4.7%), PSMT8 texture uploads into
  the CPU GS memory mirror (`GSMem::WriteP8` + `PixelStorageTraits<19>`, ~5.5%), the IOP clock (2.7%) and the VU1
  dispatcher around the AOT microcode (~6%). The GS worker is busy 35%, over half of it on one address of
  `GS::workerLoop` (a spin-wait). Shared runtime layers (TMB's VIF1/adapter work), not JC2 code.
- Throne room exit (boots 80-94, open): free control in the Jade Throne Room starts ~360-480 s with the CROSS script.
  Walking, collisions, talking to Levant (his hint: through the Kumari Gate to the Forest) all work. Tried without a
  room change: the bottom edge (centre and right), the left wall and the pink-lit doorway on it (straight, diagonal,
  CROSS there), both pillars. Next needs game knowledge of the way out (or an event to trigger first).
- Sound: sc3's 1895289 (SPU2 within 512 samples of the IOP clock) does not cure JC2's movie noise (boot 93: intro
  movie 24000 zero crossings/s, ~190 clipped/s; state dump late=2250 gap=2213 drop=256); jc2/native stays on the
  path without 4fc53883.
- Movie sound on the native loop, fixed (boots 95-100): the movie soundtracks came out as white noise (24000 zero
  crossings/s) with the native frame loop, also without sc3's 4fc53883, clean on the vblank grid. Deterministic, and
  byte-swapping the output brought it back near music (3685 crossings/s): the soundtrack with its bytes in the wrong
  order. Cause: the native frame end returned the frame count's parity as the field, where the original wait returns
  the CSR FIELD bit read at its vblank; JC2's movie audio path depends on that bit. Returning CSR FIELD at the
  wake-up (the vblanks still run on EE time): clean, 870-1230 crossings/s, nothing clipped (boot 100).
  `JC2_NATIVE_FIELD=parity` and `JC2_NATIVE_COUNTER=1` keep the old behaviours for A/B.
- sc3/native-spu2-adma (1083741) back on jc2/native: with the field fix, movies stay clean on it (boot 101: 866-1769
  zero crossings/s, nothing clipped) and the AutoDMA underruns go from 89-151 to 0 at the same point of the intro.
- Throne room exit (from the owner): stand on the yin-yang disc on the left and press X. Boot 103: the Wormhole
  Forest loads and Levant's tutorial plays (BeastAmulet, Divine Beasts' nests). Pad script: CROSS every 2 s to
  470 s, then LEFT 3 s and CROSS presses around the disc from 480 s.
- The New-game movie's sound is noise on the vblank grid too (boot 104: 7600-15000 zero crossings/s, clipping), and
  not byte-swapped (boot 102); the intro movie is clean in the same builds. A second, movie-specific fault.
- New-game movie noise, findings so far (boots 102-106): the same on the vblank grid, with and without sc3's SPU2
  change (old path, boot 106: 5900-14500 zero crossings/s), and with the MPEG lead capped at 6 pictures (boot 105;
  kept, as TMB). Block by block the output alternates between correct and byte-swapped runs (4 correct blocks of
  256 samples, then a variable swapped run). All movies have the same audio (SShd type 1, 16-bit PCM, 48 kHz stereo,
  interleave 0x200). JC2's PCM stream callback (0x153880, registered as stream type 2 = sceMpegStrPCM) skips the
  4-byte sub-header and copies the big-endian PCM into the game's ring (0x1590E0) without swapping; the swap happens
  later, on the way to the SPU2 (JC2's stream to the IOP, record at 0x89DD40, chunks by sceSifSetDma 0x486920,
  acknowledged through EE RPC sid 0x10001). Next: where the swap is done and why some chunks are played unswapped.
- Correction (same night): the movie PCM is little-endian (decoded from W01.PSS: smooth as LE, 40 zero crossings per
  256 samples as BE), so nothing swaps it. The "swapped" runs are the audio one byte off. Every audio packet of every
  movie has an odd payload (ff a0 00 00 + an odd number of PCM bytes), so samples straddle packets and any stage
  that is not byte-exact flips the alignment. The game's copy into its ring (0x1590E0 -> memcpy 0x152550, byte to
  quadword loops) is byte-exact. Still to find: the stage that drops or adds a byte (asked TMB, who overrides its
  movie PCM callback).
- Located (boots 107-108): a dump of the game's movie PCM ring (0x18A0000, `JC2_DUMP=18a0000,0x20000,150,<file>`)
  during the New-game movie (W01.PSS) holds the soundtrack in order except 0x1000-byte blocks holding the stream from
  23 bytes (0x17 = PES header 19 + sub-header 4) too early, the following block restarting at the right offset, and
  0xFDB bytes of the stream missing before each such block. `JC2_TRACE_PCM=<n>` shows the libmpeg HLE hands the PCM
  callback (0x153880) correct data: 9315 calls, all starting ff a0 00 00, lengths 4077 or 4063. So the bytes go wrong
  inside the game's ring code: 0x155950 (reserve), 0x1590E0 (copy, memcpy 0x152550), 0x155AE0 (commit). 4073 + 0x17
  = 0x1000: a reservation or commit that rounds a 4073-byte write to 0x1000 fits. Next: trace those three around a
  bad block (write position, size before and after) and compare with the guest code's intent.
- New-game movie noise, continued (boots 109-113): the libmpeg HLE ignores stream callbacks' results (no back-pressure:
  libmpeg would hand a refused packet again), so a host FIFO for what the PCM ring cannot take was tried
  (`JC2_PCM_QUEUE=1`, since removed). Queueing whole packets left the ring short of full and the movie silent;
  queueing only the bytes that do not fit fills it to the brim, but the ring then stays exactly full for the whole
  movie (used 0x18000 of 0x18000, never lowered) while the AutoDMA stream to the IOP runs on, and the noise is
  unchanged. So the consumer that feeds the IOP does not free this ring as assumed (another read position, or another
  buffer). Next: find the reader of the ring at 0x18A4B00 (the SIF DMA sources 0x18A....-0x18B.... of 0x486920 calls)
  and which counter it moves.
- The ring's reader (same night): 0x155E20 (called from 0x20E870) is the movie audio pump. It queries the sound
  driver with 0x1588F0(1, 9, 0) (command 9; the result minus the ring object's +0x44 is the playback position),
  computes from it how much of the destination (the play buffer on the IOP side) has been played (0x156130), and
  moves data from the PCM ring (0x1586D0, used -= moved) only when both the ring holds and the destination has room
  for at least 0x400 bytes. So the refill is paced by the playback position the IOP reports. Boot 113: the ring stays
  full the whole movie, i.e. the pump moves nothing. Next: trace 0x1588F0(1, 9) results during the movie (do they
  advance at 48 kHz, in which units) and what M2SOUND reads to answer (an SPU2 register our SPU2 may not update).
- Found and fixed (boots 114-118, 2026-10-30): the pump does work (boot 114, `JC2_TRACE_PUMP`: the play position
  advances and the pump writes 0x1000 behind it), and an AutoDMA input dump (`PS2X_SPU2_ADMA_DUMP=<file>`, local
  bring-up aid in the fork's SPU2) lined up against W01.PSS's PCM showed the SPU2 gets the right movie PCM, but in
  0x1000-0x2000-byte pieces repeated, skipped and off the 0x200 left/right grid. Boot 117 (`JC2_TRACE_PCM` and
  `JC2_TRACE_PUMP` together) gave the cause: every audio packet reached the PCM callback three times, same data and
  PTS. The libmpeg HLE keeps stream callbacks per sceMpeg address and drops them only in sceMpegDelete, which JC2
  does not route to the HLE (its own code runs), so each movie's sceMpegAddStrCallback added one more: the intro
  (first movie) played clean, the New-game movie (third) got each packet three times, the ring overflowed with
  repeats and the callback's partial copies broke the byte alignment. Fix in the HLE (jc2/native): a stream callback
  registered again on the same sceMpeg (same type, id, function, data) replaces itself. (Clearing the callbacks in
  sceMpegCreate fixed it too, boot 118, but a fork test expects callbacks to outlive a re-create.) Boot 118: one call per
  packet, New-game movie 1600-4300 zero crossings/s, RMS 1600-3300, nothing clipped (was 6000-10600/s, RMS 11000,
  1700 clipped per 5 s). The PCM FIFO experiment (`JC2_PCM_QUEUE`) is gone.
- native/gs-backend 19edc88 merged into jc2/native (af3cc76; the partial-FBMSK conflict resolved to gs-backend's
  uColorKeep). Boot 121, same route to the Wormhole Forest: 59.2-60 fps, frame work 7-14 ms mean and 12-27 ms worst
  in the throne room and the forest, 2-3.5 ms in menus: about as before the merge (7.5-15.7 / 18-26 ms), so JC2's
  in-game cost is not the CLUT read-backs it removed. Still the VIF unpack, GSMem::WriteP8 and the GS worker spin.

## Play sessions and saves (2026-10-30)

- `scripts/play.sh [minutes]`: the owner plays (one bench slot, a screenshot every 10 s and the log in
  build/play/<time>/; `JC2_TRACE_SCRIPT=1` passes through). Closing the window now ends the process (fork): the
  game thread never stops on its own, so the runtime exits 3 s after the window closes.
- Saving works through the libmc HLE (game/mc0/BASLUS-20309MAYU2: icon.sys, three icons, the save file and two
  slot files of 141940 bytes). Loading showed every slot "Unregistered" (boots 125-126): the header read is right
  and its CRC (0x166390/0x166AC0, CRC32 of the first 0x4C bytes, at 0x4C) passes, but the slot list (0x230980) then
  wants sceMcGetDir entry slot+6 to be 141940 bytes long: a card lists a directory in creation order (., ..,
  icon.sys, the icons, then the slots), and the HLE listed names alphabetically. Fix (fork): sceMcGetDir lists by
  the host file's creation time (Windows), then name. Boot 127: "Load Successful", back in the forest.
  `JC2_TRACE_SAVE=1` logs the CRC checks; the HLE logs each sceMcOpen (path, flags) and sceMcRead (first word).
- Open from the owner's first session: the throne-room scene with Mahbu after the forest stops (game alive, 60 fps,
  a script waiting); the picture shakes up and down by about a line (an interlace half-line offset every other
  frame, the CPU GS's stabiliser has no native equivalent yet).

## BozoEngine bring-up (2026-11-15)

Advice asked of TTA and VP2S (both answered): boot headless first, zero unknown device accesses, then the frame, the
sound, then draws one family at a time; dump EE RAM at the failing boundary (`--dump-ram`).

- **Symbol map** (`recomp/jc2_symbols.txt`): `bozo_symbolmap.py match` against DC's and TMB's unstripped
  executables names 663 functions by evidence; 45 more are reviewed (`recomp/jc2_symbols_reviewed.txt`): the earlier
  analyzer's names the game ran under for weeks, plus ones read here. TTA's warning held: `sceMcMkdir` (0x125748) is
  `sceMcOpen(..., 0x40)`; named as mkdir. `InitAlarm` (0x11AB90) installs the old SDK's alarm patch (syscall 116):
  named, BozoEngine answers it; `SetAlarm`/`iSetAlarm` are syscalls 0xFC/0xFD at 0x119C60/0x119C70.
  `sceSifLoadModule` is 0x121960 and `sceSifLoadStartModule` 0x121980 (the game calls the latter with a result).
- **VU1 microcode in the code span**: 0x4D8800-0x4E3768 is VU1 code and data. Left in, it gave 1,015 untranslated
  words; described as data, none.
- **`sceIpuInit`** (0x119868, called at 0x2B03F8) resets the IPU: answered as done (libmpeg is native).
- **IOP modules**: SIO2MAN, MTAPMAN, PADMAN, MCMAN, MCSERV are answered at their EE APIs by BozoEngine (id 1, nothing
  started). The others are compiled (bases as on the console: LIBSD 0x4F500, MODMIDI 0x54200, MODHSYN 0x57F00, UAMAN
  0x66E00, IMODEMAN 0x6A100, M2IOPSYS 0xAC900, M2SOUND 0xAE300, M2IMODE 0xBC800; heap from 0xC1000). The game binds
  M2SOUND's server 0x10000 (`guid 65536`) and M2IMODE's 0x10002. M2IMODE cannot be skipped: when the game's i-mode
  setup (0x484220, from 0x498FB0) fails, the system init at 0x15B9F0 returns early, before it registers the packet
  buffers (table 0x6CC5F0), and the main loop then spins at 0x13E070 on an empty buffer.
- **The game's EE RPC servers**: Genki's drivers call back to the EE: M2SOUND to 0x10001 (handler 0x234A60, commands
  0x400-0x405: stream state, data requests through `sceSifSetDma`) and M2IMODE to 0x10003 (handler 0x484740). The game
  registers them with libsif's own `sceSifRegisterRpc` (0x11D290), then waits in `sceSifRpcLoop` for SIF commands
  that do not exist here. `runtime/jc2_port.cpp` turns each registration into a `sif.eeServers` entry that runs the
  game's handler at once on a context of its own (the thread's gp, a stack at the bottom of the server thread's
  stack, no stops): both handlers only touch game state and start SIF transfers.
- **BozoEngine changes** (branch `jc2/native-irx-host`): vblank 4/6 waits in the IRX host (IMODEMAN polls
  `WaitVblankStart`); `recompile.py` leaves out a pointer guess that is data (IMODEMAN 0x84C10) instead of failing; the
  "no blocking progress" error names the stuck task.
- **Native stack**: the game's code nests deep enough to overflow the default 2 MB thread stack (0xC00000FD after the
  first sound bank upload); `jc2-headless` links with a 256 MB reserve.
- **Result** (boot 20): 600 frames headless, 0 unknown device accesses, 605 port overrides. Still device accesses
  (11,722): the game's DMA kicks (VIF1, GIF, IPU-to) and GS `EXTWRITE`, to be replaced by native draws.


## Window and the first draw steps (2026-11-16)

- **Window**: `-DJC2_WINDOW=ON` builds `jc2` (BozoEngine's SDL3 platform layer; SDL3 3.4.20 and Dear ImGui 1.92.7 from
  `FETCHCONTENT_SOURCE_DIR_SDL3` / `FETCHCONTENT_SOURCE_DIR_IMGUI` when a local copy exists). It runs at 60 frames/s;
  a played run keeps its saves in the user folder unless `--card-dir` names another. Captures: `--capture-dir`,
  `--capture-frame N`, `--quit-after N`. `runtime/jc2_draw.cpp` publishes a frame at each frame wait.
- **The frame wait runs as the game wrote it**: answering 0x15B400 at once kept the main thread from ever sleeping,
  so the game's lower threads starved: the movie player (thread entry 0x1533C0, created at frame ~550) never ran. The
  override now ends the native frame at the wait's entry, then runs the game's own wait (sleep until the vblank
  handler 0x15B360 wakes it). Its GS CSR read (FIELD, 0x15B4B0) is a native site answered from the vblank parity.
- **Movies**: the intro story, the title and the long dark part are one movie (`MOV/ADV0.PSS`), so movies come first.
  The player: `sceMpegCreate` at 0x152C04, six nodata callbacks 0x1535D0 (`sceMpegAddCallback`), video and PCM stream
  callbacks, the CD feeder 0x1531A0 calling `sceMpegDemuxPssRing`, and the picture loop 0x153410 (slot queue at the
  player's +0x58: 0x155250 full, 0x155380 next slot; `sceMpegGetPicture` at 0x15348C; `sceMpegIsEnd`). The display
  side 0x20F030 takes a picture only on frames whose FIELD bit (0x6D618C) is 1 and no GIF transfer is in flight
  (+0x48, cleared by the GIF end), then kicks a GIF chain of the picture's image transfer.
  BozoEngine's libmpeg is written around TMB (its `sceMpegCreate` writes TMB addresses, 0x1717BC and 0x171800, into
  guest memory), and its "not decodable yet" path queues the game's nodata callback: JC2's (0x154020) is the IPU
  feeder of the console's libmpeg, which waits for data (+0x44) that never comes and spins (frame ~530 headless).
  Next: a JC2 movie owner (as VP2S did), pictures to the renderer's backdrop, PCM to the port stream.
- **Graphics register sites** (survey over 530 frames with `JC2_NATIVE_GRAPHICS=0`, the default for now): all must go
  before native graphics is on (`JC2_NATIVE_GRAPHICS=1` faults at the first). VIF1: the frame's packet kick
  0x13E0DC/0x13E104/0x13E124 (poll STR, TADR/QWC, CHCR), 0x14F538/0x14F980, 0x150FC4, 0x15C60C. GIF: 0x13E1D8/0x13E220,
  the flip's display packet 0x15A04C/0x15A234/0x15A9A8, the movie picture 0x20F0C8-0x20F110 and 0x1556F8. GS:
  `EXTWRITE` 0x15A270, CSR 0x15B4B0 (now native). Not graphics (kept): T0 0x13DF00/0x13E24C/0x13E5C0, the IPU-to
  channel 0x153B38/0x153D04.

## Movies, native (2026-11-16)

JC2 plays its PSS with Sony's **ezmovie sample** wrapped by a game state machine (mapped from the disassembly):
- player object (create 0x152BE0): flags +0 (0x100 thread started, 0x200 first picture, 0x400 abort, 0x800 done),
  sceMpeg at +8, stream ring +0x50, ViBuf +0x54, picture queue +0x58, audio decoder +0x5C;
- the game's movie wrapper (create 0x20DDA0, current movie at gp-0x7390): state +0x1C, GIF in flight +0x48, presenting
  +0x4C; start 0x20E5C0(movie, id into the 28-entry `\MOV\*.PSS` table at 0x5CD780), per-frame update 0x20E870, skip
  (START after 11 pictures, ids < 3) 0x492A00 -> 0x20EB90 -> abort flag;
- CD: a 3 x 0x18000 ring filled by the loader thread (sceCdRead); the feeder 0x1531A0 calls `sceMpegDemuxPssRing`;
- picture thread 0x1533C0 (priority caller + 1): GetPicture into one of 3 640 x 448 RGB32 slots, pushed to the queue;
- display 0x20F030: on field-1 frames the oldest picture goes to the GS by a GIF chain; the GIF end handler 0x20F450
  pops the queue and clears +0x48; 0x20F140 draws the uploaded picture as a sprite while flag 0x200 is set;
- sound: the PCM stream callback 0x153880 fills the game's audio decoder (96 KB EE ring, 48 KB IOP buffer), pumped
  by 0x155E20 with `sceSifSetDma` and LIBSD block transfers through the game's sound-driver RPC.

Native owner (`runtime/jc2_draw.cpp`): BozoEngine's libmpeg decodes (FFmpeg) from its own demux, so the IPU feeder
is idle: `viBufAddDMA` 0x154020 (the nodata callback that spun), StopDMA 0x154330, RestartDMA 0x1544F0 return, and the
video stream callback 0x1536C0 reports its packet taken. The display side 0x20F030 is native: on field-1 frames the
oldest slot (RGBA32 in 16 x 16 macroblocks, column-major) becomes the renderer's backdrop and the slot is released at
once, as the GIF end would; 0x20F140 keeps it on screen while flag 0x200 is set. No GIF packet is built or read. The
sound is the game's own path over the compiled M2SOUND/LIBSD: the intro's soundtrack plays (music: ~1,300-2,900 zero
crossings a second, 0.02 % of samples at full scale).

Result: the intro story (`ADV0.PSS`) plays in the window at 60 frames/s with its music; `--wav FILE` records the mixer.
Still to check: the title part and START, the other movies (`ADV1`, `ADV2`, the beast movies), the skip.
BozoEngine note: its `sceMpegCreate` writes TMB addresses (0x1717BC, 0x171800: JC2 code bytes, harmless here); to
be made port-neutral upstream.

## After the intro: what is drawn (2026-11-16)

- `--pad-script` (`frame:BUTTON/frames`): `700:START/4` skips the intro (allowed after 11 pictures, ids < 3).
- **Draw census** (`JC2_DRAW_CENSUS=1`, `runtime/jc2_draw.cpp`): the game's builders write seven per-frame packet
  buffers (table 0x6CC5F0: entry +0x84 base, +0x88 size, +0x8C fill). Pointer 0x13DD50(buffer) (0x13DC90: buffer 0),
  base 0x13DD90(buffer), commit 0x13DE10(buffer, qwc); buffers 0-5 go out as one VIF1 chain (0x13E0D4), buffer 6 as
  GIF (0x13E1C0); 0x13E2E0 links the frame's chain. The census counts callers of each per 300 frames.
- After the skip (frames 1500-1800): the only content is **0x161D50(object)**, 3 a frame: JC2's model renderer. It
  loops over the object's parts (matrices from +0x08/+0x0C), sends each to VU1 through 0x1622A0 into buffer 1, then
  0x13DE70 adds its GIF part to buffer 6 (4 a frame in all). The screen polls the pads and the memory card (no game
  printf). So the menus already need the 3D path: the model format, the VU1 programs (0x4D8800-0x4E3768) redone on the
  CPU, meshes and textures converted offline from the disc, drawn with `Renderer::draw`.

## 2D pictures, native (2026-11-16)

The screen after the intro is not 3D: 0x161D50(object) is the game's picture drawer (title, menu frames, cursor,
logos). Mapped from the disassembly and a RAM dump (`runtime/jc2_2d.cpp` header has the layouts): resource nodes in a
list at 0x6C47D4, parts of tiles, each tile a TIM2 picture kept in EE RAM (uploaded once to GS memory by 0x161380 on
the console), GIF strips in buffer 1 linked by layer into buffer 6. Drawn natively: the tile's TIM2 picture decoded
from EE RAM (8-bit with a CSM1 CT16/CT32 palette, 4-bit CT32; a CLUT may hold several palettes, the object's +0x20
picks one), quads at the object's x/y (+ the tile's offset, scaled to +0x10/+0x14), modulated by +0x18 (its alpha the
fade), alpha-blended, far layers first. The tile records' U/V are GS memory positions (pictures packed side by side):
each tile samples its own picture from its origin. A resource not uploaded yet goes once through the game's drawer
(its upload fills the records), as a tail call.
Result: the Genki logo, the title (PRESS START BUTTON) and the main menu's frames and cursor draw in the window. Not
yet: the menu's labels (another routine), rotation (+0x2C), mip shift (+0x1C).
- **Menu text and box fills** (same day): text 0x1636E0 (a grid bitmap font: slots at 0x86A900, charset strings,
  colour codes 0xB1-0xB9 and the 9-entry colour table at 0x4E7FA0, alignment, fonts 0x625-0x627 read from the disc's
  CDDATA since the game frees them after its GS upload) and box fills 0x167270 (four-corner gradient, style insets at
  0x4E80D0). The main menu (New game / Continue / Battle Arena) draws complete. Font slot 3 (digits) and the boxes'
  animated size are not done yet.

## Overlays, New game, the first scene (2026-11-16)

- **Overlays**: `tools/jc2_bozo_overlays.py` extracts the 1,383 MWo3 modules from CDDATA.000 (`find_overlays.py`), wraps
  each as an ELF (`wrap_overlay.py`), maps its functions (`overlay_functions.py`) and compiles it with bozo-recomp as an
  image of its own: all 1,383, 0 untranslated instructions (the 15 `SC_*` that the earlier recompiler could not take included). They
  build in 32 batches (each module in its own namespace) and `jc2_main.cpp` registers them with
  `FunctionTable::addModule` by their 64-byte header. BozoEngine: `moduleAt` got a span check and a last-module cache.
- **New game**: the opening movie (not skippable) plays natively; the picture thread's slot wait spins on a yield
  (0x153170), now charged 2000 EE cycles a turn (it had dropped the game to ~33 frames/s). The EE RPC bridge's handler
  context now starts at the EE's clock (it started at 0, which the watchdog read as a 600-field stall). M2SOUND's
  `WaitNonVblank` (vblank 7) returns at once (BozoEngine).
- **First scene** (frame ~10,500 of a New-game run): the dialogue box and its text draw natively. The 3D is mapped
  (subagent analysis in `build/bozo/analysis3`, an offline proof that renders the scene from a RAM dump):
  - static models: builder 0x149350 (draw lists from 0x148EA0), VU1 program B (0x4D8810), per part a 0x60-byte material
    (flags, texture bank/slot/index, colours, PRIM, ALPHA, CLAMP, TEST) then a prebuilt VIF chain in the model data;
  - characters: 0x14ADD0/0x14B230, VU1 program A (0x4DC6E0), 4-bone skinned batches (bones from the pose in RAM),
    textures uploaded per part from TIM2 data in RAM;
  - shadows (guess) 0x146B70 and a screen tint 0x22CF10, VU1 program C (0x4DD9B0);
  - projection X = 1126.1 x/z + 2048, Y = 525.5 y/z + 2048 (a 640 x 224 field), lighting: 3 directional lights +
    ambient + a second set, material colour, fog 0.51 (1000 - w).
  The vertex data exist only as prebuilt VIF packets in the model files: per the room's rule they will be converted
  offline from the disc into a mesh pack keyed by the game's own model keys, and drawn from a hook at the builders
  with the game's matrices and lights (program A/B maths on the CPU).

## 3D models (2026-11-16)

Mapped from a RAM dump of the first scene and the disc (subagent analysis, `build/bozo/analysis4`); the parser
(`tools/jc2_models.py`) rebuilds every triangle of that scene from the disc's bytes alone (13,828 of 13,828, 0.0002 px).

- **Resources**: CDDATA.LOC entries at `*0x6C47BC + 16 id` (+0 sector, +4 bytes). Every read goes through
  0x15C390(desc): +0 flags, +4 resource id, +8 destination. No id -> address table is kept, so the port records it there.
  A model resource is an archive (u32 count, then the sub-files' offsets); a character's holds a stub, the model
  ('0IVX'), a TIM2 bank and its animation; a room's (e.g. 2858) several models and TIM2 banks. Loaded unchanged, except
  the skinned chains' REF tags (patched with palette + bone x 0x80), material flags (bit 0x4000 each frame) and node +8
  bit 31.
- **Model sub-file**: '0IVX' '00.1'/'00.2', 'LDMX', 'SKIN'/'NORM' (byte-reversed). v1 (characters, a few static):
  counts at +0x20, then groups (32 bytes), nodes (0x50), meshes (0x30 + {material, submesh} pairs), materials (0x60),
  submeshes. v2 (static): each node followed by its mesh and, per item, a 0x60 material then the submesh.
- **Material** (0x60): +0 flags (2 prelit, 8 back faces culled (the program gets the flags as they are; read as "double-sided,
  flags ^ 8" until the Room of Life's wall showed otherwise), 0x20 textured, 0x40 environment
  map, 0x100 two-pass), +0xC bank, +0xD slot, +0xE index (texture record), +0x10 colour, +0x20 second colour, +0x30
  PRIM, +0x38 ALPHA_1, +0x40 CLAMP_1, +0x48 TEST_1, +0x50/+0x58 MIPTBP.
- **Submesh**: {vertices, batches, qwc}, bounding box, then the chain at +0x30 (the DMA CALL target). A batch: (skinned)
  2 REF tags a bone slot, a CNT tag (its address word: the 4 slots' palette indices), V4-32 header and GIF tags (the
  last one's w: the starting winding), then per vertex position V3-32, s/t V2-32, normal V4-16 (1.15), weights /
  colour V4-8; MSCNT. One triangle strip a batch.
- **Builders**: static 0x148EA0 -> 0x149350(context) per node -> per item 0x149CE0(packet, material) and
  0x14AA30(packet, submesh); skinned 0x14ADD0 -> 0x14B230(instance, context) -> 0x14BC00(instance, packet, material
  copy) and 0x14C450(instance, packet, submesh record +0x30 chain). Context: +0 camera, +4 light set, +0x10 the node's
  world matrix. Camera: +0x130 V, +0x170 P, +0x2F0 clip, +0x380 (winding -1, 1, fog 1000, 0.51). Light set: +0xB0
  colours + ambient, +0xF0 directions. Bone palettes: actor (0x1C5ECE0 + 0x600 k, +0 the instance) +0x1E0 set A,
  +0x1E4 set B (by `*0x6CC670 & 1`), 0x80 bytes a bone in world space (+0x30.w: 1/scale for the normals).
- **Textures**: bank table 0x6CC5F0, 52 bytes a bank (+0x18 slot mask, +0x1C + 12 slot {TIM2, count, records}); a
  record is 0x60 bytes (+0x20 TEX0). 0x13DAE0(bank, slot, count, records, tim2) registers a slot.
- **The port** (`tools/jc2_mesh_pack.py`, `runtime/jc2_meshes.cpp`): one pack file per distinct model sub-file (1,888
  on the disc, 1,378 distinct, 30,322 submeshes, 7.5 M vertices: a 368 MB pack), plus a 'RESOURCES' index (resource id ->
  sub-files). The hooks note the builder's context and the material, the mesh call looks the chain up by (resource,
  offset) and draws it with the programs' maths on the CPU.
- **Room of Life** (2026-11-19): its resources (3309-3313, 3324-3329) keep their models in an archive inside the
  resource's first sub-file (the pack tool walks nested archives); the player's model (2283) is copied by the game
  to the room's heap, not read from the disc, so a chain in no recorded load is matched from the copy itself (the
  '0IVX00.1' header before it and its archive's sub-file layout against the pack's index); a skinned triangle with
  a vertex outside the camera's clip volume (+0x2F0, |x|, |y| or |z| > |w|) is dropped as program A does (2026-11-23:
  only A; program B cuts at the volume, dropping its triangles holed the battle arena's floor); and
  material bit 3 culls back faces (it is not "double-sided": the room's near wall is culled).
- **Textures** (2026-11-16): a material's (bank, slot, index) gives its record and its slot's TIM2 picture; the UVs
  are in the record's GS page (TEX0 TW/TH) and remapped to the picture by its place (record +0x10). A slot registered
  without its TIM2 (a room's) is matched to a loaded resource's TIM2 sub-file with the same pictures. Palettes are
  24-bit (TIM2 CLUT type 2, CSM1 order); a room's pictures (picture format 0x30) keep their 8-bit pixels in the GS's
  PSMT8 order (unswizzled when decoded); the characters' are linear.
- **Shadows**: 0x177CF0(actor) draws one blob quad on the floor (through the generic primitive drawer 0x146990 ->
  0x146B70, VU1 program C, flags 8). Native: centre actor +0x190 + +0x410 (y - 0.01), size +0x1B0, half sizes
  0.45/0.35 (mode word 0x8788D2 == 10, else 0.65/0.45), turned by +0x1A4, culled by camera +0x270; texture picture 0
  of the TIM2 at G +0x470 (G = *(**0x520C2C + 0x24): resource 0xB69, a 32-bit picture, black with a radial alpha);
  ALPHA (0 - Cd) As + Cd, depth test without write. Analysis: `build/bozo/analysis5` (program C emulated, the
  EE-side rebuild matches it exactly). 0x146990's other callers (effects) and the screen passes (0x22CF10 fade, a
  depth mask and a blur in program C screen-space mode) are not ported.
- **Effects** (2026-11-16, analysis `build/bozo/analysis6`): the primitive drawer 0x146990 (VU1 program C) is only used
  with flags 8 (shadows), 4 (camera-facing sprites: particles attached to actors, emitters in models), 0x10 (2D
  parallelograms: overlay glows) and 0x400 (screen sprites: the depth of field); every program C path was checked
  against the microcode (vu1.py) on random inputs. Native: flags 4 and 0x10 at 0x146990 itself (blend from obj +4 as
  the GS's ALPHA, depth test from the flags, texture from the record's bank slot or the call's TIM2 picture); not yet
  seen on screen (the first scene has none).
- **Screen passes**: 0x22CF10(corner, centre) is a vignette (a full-screen fan, colours as four ints; the first scene's
  is brown at alpha 0x28 in the corners, clear in the centre). 0x494200(table, f12 strength) is the depth of field
  (task 0x492DF0): a band of GS Z around the focused actor stays sharp, the rest blurred by six passes over the frame
  (offsets +-sqrt(3) S, +-S/2 field lines; S = 1.5 strength), each mixing by the mask's alpha. Native: aux image 1 = the
  mask by a depth lookup on the frame's depth (the game's sprite rule per bucket), six frame-sampling draws with their
  alpha from it. JC2_DOF=0 / JC2_FADE=0 turn them off; JC2_TRACE_FX=1 logs their parameters.
- **Static lighting** (program B, analysis `build/bozo/analysis7`: the frame's whole VIF chain run through the real
  microcode with `vu1b.py`, which adds the MAC flags): the lights are taken into object space once per object (VU[76..78]
  = L x W), so a vertex gets d = max(0, L . n W) with the normal not renormalised; colour = min(1, (C0 d0 + C1 d1 +
  C2 d2 + ambient) x base) x scale, truncated. base = the material colour, or the vertex colour / 127 when the material
  has bit 1 (the room: its colours are a bake that the lights still multiply); scale = VU[1] (255), times 0.502
  for textured materials (bit 5 without bit 8); alpha 128. Unlit (program bit 0): base x scale. The same maths in
  both paths (0x57D per vertex, 0x479 for the room's flag-0x4000 batches).
- **Projected texture** (program B flag 0x800, second pass at 0x4BC): the batch again in GS context 2, colour 128,
  s, t = P (position + W's translation), P = VU[34..37] = camera +0x3B0 (world to texture; camera +0x3F0 is the
  box test's), perspective-correct. The room function 0x195EA0(room) keeps the texture: record at room +0x46C, TIM2 at
  +0x470 (picture 0); 0x14A800 sets context 2 from the record: TFX modulate, CLAMP 5, ALPHA 0x46 = (0 - Cd) As + Cd,
  TEST depth GEQUAL without depth or alpha write. The builder sets the flag when the material has 0x800, camera +0x440
  is 0 and the node's box is inside camera +0x3F0 x W. The first scene's is a black picture with a soft round alpha,
  a dark spot on the floor at the warp circle (x within 0.75, z within 0.6 of the room's origin). Native: a second
  batch in the part's draw (black vertices, the texture's alpha darkens; BozoEngine's MeshBatch got clampU/V for it),
  skipped when its texture coordinates miss 0..1 (the box test). JC2_PROJECTED=0 turns it off.
- The model batches set the depth write themselves (the mesh submitter keeps the last batch's ZBUF: after the first
  shadow, nothing wrote depth and the shop counter was drawn over).

## Scenes, teleport and battles (2026-11-23)

Mapped from SLUS_203.09 (`runtime/jc2_debug.cpp`), for testing without playing up to a place:
- **Scenes**: 1208 entries at 0x5AD4F0, 32 bytes: +0 the name's address, +4..+0x10 routines, +0x14 the event script
  overlay (CDDATA resource, -1 for the forests), +0x18 a data resource, +0x1C the overlay's kind. 0-9 are the forests
  (WIND, AQUA, FIRE, TERA, DWIND, DAQUA, DFIRE, DTERA, DLLTH, TUTO), then the m2es* scripts (38 m2esA001 = New game,
  44 the Temple, 45 the Throne Room, 49 the Room of Life). Halfwords 0x87860C next (-1 none), 0x87860E current,
  0x878610 previous. The scene task (0x1A1050) moves next into current and loads it (0x1A0D50 loads the overlay with
  0x233AF0(resource, slot address)) between scenes; New game asks for 38 (0x1E58E0). A running event scene leaves
  through its step 0x1E7B90(scene): scene +6 is 1 while the script runs, 3 waits for the scene's loads (0x151320)
  and returns mode 2 if "next" is set; the caller keeps it in scene +0x1314 and the exit (0x1E7500) loads "next".
  The teleport overrides that step: "next" and state 3 while the script runs.
- **Modes**: bytes S+0 current, S+1 asked for (0xFF none), S = *(*0x6C4890 + 0x24) (gp 0x6CBC70 - 0x73E0; 0x1FAD00
  asks). The title menu's table (0x4E8480, {mode, label}): NEW GAME 2, LOAD GAME 7, VERSUS 5, BATTLE 3, ARENA 0xD,
  NETTRADE 0x15 (the retail menu shows three). Seen in states: 0 the title, 2 the game (forests and event
  scenes alike), 3 a battle, 9 (the hatching menu) and 0xA (between scenes).
- **Hunter battles** (an event's, 0x221B14): opponent records at 0x5F4B80 + 0x94 i (+0 name, +0x30 party): 0 Kahu, 4-18
  the developers' TEST1-3 parties, 19-150 the hunters (Garon, Bazu, Hydra...). Setup at 0x8963EC (0x2918A0 returns
  it): +8 the party, +4 -1, +0x38 and +0x3C 0; kind byte 0x89B071 = 3 (4 another event path, 0x1E and 8 other battles:
  0x47D200, 0x2AC0C0); then 0x430E10 and mode 3.
- **Use**: `--scene N [--scene-at F]`, `--battle N [--battle-at F]`; in the window F2 > Teleport and F2 > Battle.
- Developer modes are still in the program (0x15C140, table 0x4E6FA0: MODESEL, CHARTEST, MAPTEST, BTLEFTEST,
  BTLCAMTEST...); several read host0: files, so they are not used.
- Build note: the externals list (`tools/jc2_bozo_overrides.py` over runtime/jc2_*.cpp, then bozo-externals) was
  rewritten on any runtime edit, which recompiled every overlay batch (~30 min at -j 4). It is now copied only when it
  changes (copy_if_different + Ninja restat); a new override still recompiles everything, as it must.

## Battles drawn (2026-11-23)

- **Battle stage**: the arena's models are single sub-files the game copies out of loaded resources (1737's pads to
  0x1B906C0 and on, 1732's 62 KB arena to 0x1B9A000) and relocates (14-15 pointer words, header and node tables).
  `discoverModelCopy` (runtime/jc2_meshes.cpp) matches such a copy against the loaded resources' model sub-files (at
  most one word in 64 may differ) and keeps it in `g_copies` (found again from memory after a state load). A chain in
  a resource's footprint but none of its models (the game reused the memory) is rediscovered too.
- **Overlapping overlays** (BozoEngine): the Arena's lesson scripts (m2esA3xx at 0x96A800, to 0x96D6C0) span the
  battle-effect slot 0x96B000; with an effect loaded there both headers match and the run ended ("ambiguous native
  module identity"). The module starting higher now wins over the other's tail (FunctionTable::moduleAt; each module
  keeps the lowest other start inside its span as its fast-path limit).
- **Getting a real battle**: the Arena's tutorial (hub A015 = scene 50, menu Arena, talk to the lady): states
  s23945_hub_menu, s24300_arena, s26200_arena_lesson. The battle's controls: Left/Right the command tables, L1 the four
  colour tables, Triangle Items/Escape. A battle started with `--battle` from before the hatching (no minion) waits
  forever; from s21900_battle_tamatoch it shows the hunter's minions but not the hunter (not yet compared with the
  Arena's setup).
- **Effect textures**: a battle effect's particles (the sleep rings, frame 27150 of the lesson) get their texture record
  from 0x13DB60(bank, slot, index) at the effect's set-up and keep a copy (0x1DD3D08 is the battle bank's record 12,
  bank 1 slot 0 at 0xDD6CF0): not in any bank when drawn, they came out as flat yellow squares. `residentCopy` finds the
  bank record with the same TEX0, place and size. JC2 prints "[jc2:fx] textured effect without a texture" with the
  banks when one is still missing.
- Open: one static mesh near resource 1595 (0x1B9DDB0) once in the lesson, not found.
- **`--battle` (2026-11-23, not finished)**: an event script starts a battle with command 0x1C of the executor
  0x1EB5D0 (third argument 1: the battle case at 0x1EC280; the kind, 0x89B071, from its fifth: 7 for the Arena lesson;
  the group from its fourth, `0x1A4CC0(n)` = 0x5857D0 + 0x30 n, 2000 groups, +0 the battle script's name such as
  "esAsg_Battle_Tutorial3"; the hunters' records +0x30 have the same layout, "eM2ARENA_BATTLE_LV2_1" for Garon). The
  scene then leaves with mode 3: the exit (0x1E7500) prepares the battle and installs 0x1E6520 for it and the way
  back. The launcher now does that from the scene step's override (step returns 3, the battle block filled; the intro
  camera plays), but the battle state 0x89B010-0x89B030 is the running script's context (copied from scene +0x10..+0x28
  by 0x1E5100, reset to "no script" by 0x1E51B0 from the battle handler): with no script behind it, the group is
  cleared, the battle shows four default bugs and waits for ever. To finish: start a real battle script (the group's
  name) rather than fill the block. Until then the Arena lesson (s26200_arena_lesson) is the battle to test with.

## IPU animations (2026-11-23)

- Besides the PSS movies, the game plays ".ipu" streams itself as animated textures ("ipum" header: size, width,
  height, frames; then intra-only IPU frames), driving the IPU directly. Scene D001's water is one (64 x 64, 298
  frames, at 0x1947B80 in that scene); the battle code uses the same player (0x415xxx, 0x432xxx). Before this the run
  ended on the first IPU register write ("unhandled device write at 0x10002000", pc 0x2B0F08).
- The player (0x2AF530-0x2B2260): a group (active one at gp-0x727C: +0 flags, +4 count, +8 objects of 0x70 bytes,
  +0xC the current one) of objects (+0 flags 1 on, 2 playing, 4 finished, 8 looping, 0x10 in the IPU, 0x20 held;
  +0x28 the stream; +0x38 / +0x3C frames done / total; +0x40 loops; +0x44 x +0x48 macroblocks; a ring of +0x54
  frames at +0x6C, +0x58 the next slot, +0x5C the frames waiting). The kick 0x2B1990 -> 0x2B1AC0 -> 0x2B0DA0 starts a
  frame (IPU reset, to-IPU DMA of the stream, FDEC the frame's flags, IDEC to RGB32 by from-IPU DMA); the from-IPU
  handler 0x2B1780 ends it (0x2B10F0) and starts the next until the rings are full.
- Native (`runtime/jc2_ipu.cpp`, BozoEngine `bozo/libs/ipu_stream.h`: FFmpeg's "ipu" parser and decoder): 0x2B0DA0
  decodes the next frame into its ring slot as the CSC lays it out (16 x 16 RGB32 macroblocks) with 0x2B10F0's
  bookkeeping; 0x2B1AC0 fills every ring and reports nothing in flight. `ffmpeg -f ipu` decodes D001's stream (297 of
  298 frames; the last one is short). JC2_TRACE_IPU=1 lists the streams opened, the group's state and the calls.
- Checked (2026-11-24, engine master 36991be and later, s24300_arena with `--scene 581`): D001 no longer stops. Its one
  object (0x1947AF0, looping) opens, frames are decoded into its two-slot ring and the game takes them (waiting 2, 1,
  2...), about one frame every two to three. Everything goes through 0x2B1AC0 (0 direct 0x2B0DA0 calls): with one
  object the DMA handler never asks for the next. Not seen on screen yet: in the arrival view nothing but the hero
  changes between frames, so the water is off camera or its uploads are not drawn by the native renderer; to find.
- Where the water goes (RAM dumps in D001, 2026-11-24): the game uploads each ring frame to GS page 0x3800 (TBW 1,
  CT32, 64 x 64) as 16 x 16 macroblocks (0x2AFA3C: the oldest frame from 0x2B1370, the transfer built by 0x2B04C0,
  the slot freed by 0x2B01D0); its own texture record (not in a bank) is at 0x1947A70 (+0x14 size 64 x 64, +0x20
  TEX0 0x198007800). It is drawn with a VU1 program the port does not translate yet, "program D" (microcode
  0x4DFF60, its DMA packet 0x4DFF50; 0x1FB330(packet, kind) picks it for kind 4, 0x4DFBE0 for kind 5), seven times
  a frame: per draw a context (0x1FB290: 0x31 qwords, camera and light matrices), a 9-qword block at VU 0x33, A+D
  TEX0/TEX1/ALPHA, then 20 batches of 84 qwords from 0x19CAF90 (a 21 x 20 grid, 3.6 units square: per vertex
  (S, T, 1, 0) with S, T 0..2, then (x, y, z, 1) with y the animated wave height). The submitter is 0x4314E0's
  kind-4 path (0x431700), a draw-list kick with 20 callers (the frame dispatcher 0x4333B0's bit 1 routine 0x206850
  among them). To show it: a native draw for program D's lists (the grid, its matrix, the IPU frame as texture).
- Native water (2026-11-24, `runtime/jc2_meshes.cpp` waterDraw): 0x206850 is the water routine (the frame
  dispatcher 0x4333B0's bit 1). The water system at 0x89D3E0 queues tiles (+0x14 count, +0x20 positions; D001: 7
  tiles of 3.6 x 3.6 from (5.4, 0, 1.8) to (9, 0, 16.2)), double-buffers its wave grid (+0x200 the one drawn; 0x206420
  moves the waves, a height field) and keeps the texture record at +0x260. The override runs the routine, then draws
  each tile's grid (20 x 20 quads) with world = the tile's translation, the frame's camera (0x6CC5F4[0]), the GS
  blend (Cs - Cd) 0x3F + Cd, depth test GEQUAL without write, and the picture the game took last from the IPU ring.
  Program D's microcode (dumped by walking its MPG packet, disassembled with TMB's tools/vu_disasm.py): init at 0
  multiplies the context's matrices (VU 0x49 = P V W, 0x55 = clip W), the water's path (VU 53.x = 2) at 0x5B0
  projects (x, y, z, 1) by P V W and clips by clip W: the same maths, no height offset.
- Where it shows: in D001's arrival view the room covers it. The world is y down: the hero's feet and the ground
  by him are at y -0.6, and the ground chunks (res 2690, flags 0x482A) line the channel there at about -0.3, above
  the surface at y ~0 (+- 0.05 waves); with the depth test off the water fills the channel's shape. The console
  agrees: program B places the room's vertices with P V W alone (its bit 0x2 only takes the colours from the
  vertices), and no draw of the frame masks depth (every ZBUF is Z32 with ZMSK clear). Walking up from the arrival
  point (pad UP for 150 frames from 24700) the pool beyond the plants shows the water, animated, between its banks.
- Program D in general (2026-11-24): the water is one user of a generic submitter. The frame's effects task
  (0x4333B0) calls one routine per bit of the area's effect flags, set per forest and sub-area by 0x433AF0 from the
  table at 0x64E2F0 (forests 0x19801 = bits 0x1 water, 0x800, 0x1000, 0x8000, 0x10000; area 1 adds 0x400 or 0x100
  by sub-area; area 4 to 7 0xB800 / 0xB880). Bits 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x80, 0x400, 0x800, 0x1000, 0x8000
  draw through 0x4314E0(object, position, texture animation, GS parameters, t0, camera index; f12 scale); 0x40 and
  0x100 select a fifth program (kind 5, 0x4DFBE0) directly. The object's +0x30 is program D's path (VU 53.x) and
  sets the batches (0x432170: per mode the vertices a batch and qwords a vertex). The GS parameters are three A+D
  pairs: ALPHA (FIX in its high word), TEXA, TEST. The port now overrides 0x4314E0 itself and draws mode 2 (the
  water's strips), 0x40 (path 0x1630: (x, y, z, size) + colour, a quad flat in x z, corners at +-size/2 on x and z)
  and 0x400 (path 0x21D8: (x, y, half width, half height) in GS coordinates + colour, a screen quad at Z 0xFFFF)
  with ALPHA, the alpha test and the depth test from the parameters; other modes are listed once.
- Effect textures are the open part: the records are the effects' own (not in the banks). The water's is found by
  its IPU object; the Wind Forest's light spots (mode 0x40, record built by 0x22D7D8) sample page 0x3800, the water
  animation's page, with no IPU playing there; its sparkles (mode 0x400) use picture 0 of a TIM2 the effect keeps
  (0x89DD00: record, TIM2), but matching a record to a loaded TIM2 by format and size is ambiguous (five candidates
  in that forest), so it is used only when unique. Both stay undrawn for now. The way out: follow the game's own
  uploads (which picture went to which page and CLUT) instead of guessing from the record.
- Uploads followed (2026-11-24): the game starts its VIF1 and GIF transfers itself (its packet lists' kick
  0x13E0D8 / 0x13E1D4 and others write CHCR; it never calls sceDmaSend for them), so BozoEngine gained
  Libraries::dmaStarted (called as a channel starts, before the I/O library completes it). JC2 walks the chain
  (DMA tags; with TTE each tag's whole qword reaches VIF1, its lower half ignored: kept as padding, or DIRECT's data
  lands half a qword off), VIF1's codes (DIRECT carries GIF data; UNPACK, MPG, STMASK, STROW / STCOL skipped by
  size) and the GIF stream, and notes each image transfer (BITBLTBUF, TRXPOS, TRXREG, then IMAGE: the RAM its pixels
  came from). An effect record outside the banks is then assembled from the transfers into its page (CT32 / 24 /
  16, PSMT8 / 4 with the CLUT uploaded at CBP, TEXA from the effect's parameters) and read from that RAM, as TIM2
  pictures are; no GS memory. The Wind Forest's whole frame chain (about 2 MB, 25,000 GIF qwords) walks through.
  Its dust motes (mode 0x400, a 64 x 64 4-bit grain cloud) now draw from their upload when the forest is entered
  in the run (a state saved before the upload map existed has none: the effects started before it stay undrawn).
  The light spots' page 0x3800 gets no upload in the Wind Forest: still undrawn. JC2_TRACE_UPLOADS=1 lists the
  chains and transfers, JC2_DUMP_EFFECT_TEXTURES=dir writes each effect texture once.
- Forest survey (Throne Room state, --scene 0..9, 440 frames each): Wind 0x400 (dust) and 0x40 (light spots, no
  texture); Water and Fire 0x2 (water); Lilith's Nest 0x20 and 0x80; Earth, the dark forests and the tutorial none
  in that time. Translated next: path 0x1240 (mode 0x20: (x, y, z, size), colour, (cos, sin); a camera-facing quad
  on VU 28 = the view's y row negated and VU 27 x it (VU 27 its z row), turned on screen about its centre with y
  stretched by 2.14393 for the turn) and path 0x1C30 (mode 0x80: colour, a direction t, (x, y, z, width); a ribbon,
  p +- (VU 27 x t) width, one strip a batch). Lilith's Nest's fireflies: spinning sparks (0x20) with ribbon trails
  (0x80), additive (ALPHA 0x48: Cs As + Cd), fading in after arrival.
- Towns and battles (same survey; Arena route for D001, the two battle states): mode 0x10 in the Room of Life (49)
  and the Jade Throne Room (50) from 0x422A9C, mode 0x1000 in the Training Forest (44) from 0x47C118, none in the
  battles. Translated: path 0xC50 (mode 0x10: (x, y, z, size), colour; path 0x1240's camera-facing quad, not turned)
  and path 0x18F8 (mode 0x1000: (x, y, z, size), colour, (cos, sin); path 0x1630's flat quad turned about y,
  corners p - u, p + w, p - w, p + u with u = (h sin, 0, -h cos), w = (h cos, 0, h sin); 25 a batch, 3 qwords). Both
  small (8 elements), depth-tested, additive; not yet told apart on screen from the scenes' own lights.
- Engine master since this merge answers the IPU's registers as an idle decoder (libs/io.cpp), so without the native
  player the game would no longer stop there: it would run on with frames never written.
- Testing gotcha: `timeout` (msys) and `scripts/bench_run.sh` lose variables set in front of them (JC2_TRACE_*): put
  `env VAR=1` in the command after bench_run.sh.
- The JC2 save states (before 2026-11-24) needed the engine to read four older section layouts: pad (libpad2 sockets),
  mpeg (`inputEnded`), sif (the IOP's software registers) and the 989SND section (absent).
