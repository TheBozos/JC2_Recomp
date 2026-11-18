// Jade Cocoon 2's IOP modules on BozoEngine. The sound driver (Sony's LIBSD, MODMIDI, MODHSYN under Genki's M2IOPSYS and
// M2SOUND) is the game's own code, compiled offline to native C++ (tools/jc2_bozo_audio.py), over native host services:
// LIBSD's exports go to BozoEngine's mixer, no SPU2. The pad and memory-card modules are answered at their EE APIs by
// BozoEngine's native libraries, so nothing of them is started.
#include "jc2_audio_modules.h"
#include "bozo/libs/libraries.h"
#include <algorithm>
#include <cctype>
#include <span>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace jc2
{
    namespace
    {
        std::unique_ptr<bozo::native::irx::CompiledLibraries> sound;

        std::string moduleName(const std::string &path)
        {
            auto name = path.substr(path.find_last_of("/\\") + 1);
            if (const auto version = name.find(';'); version != std::string::npos)
                name.resize(version);
            for (char &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return name;
        }
    } // namespace

    // The game frame ended (the frame wait): the modules' frame listeners run.
    void notifyFrame()
    {
        if (sound)
            sound->notifyFrameCompleted();
    }

    // Save states: the IOP's state (its memory, the modules' tasks, the mixer).
    std::string audioUnsaveable() { return sound ? sound->unsaveable() : std::string(); }
    void audioState(bozo::state::Archive &a)
    {
        if (!sound)
            throw bozo::state::Error("save state: JC2's IOP modules are not set up");
        sound->serialize(a);
    }

    void installAudio(bozo::libs::Libraries &libraries, bool trace)
    {
        // The modules end at 0xC0500 (M2IMODE); the IOP heap the drivers allocate from starts above them.
        sound = std::make_unique<bozo::native::irx::CompiledLibraries>(libraries, 0xC1000);
        // M2IMODE registers a vblank handler: it runs at each native game frame's end (notifyFrame), no display.
        sound->useNativeFrameCallbacks();
        addJc2AudioImages(*sound);
        libraries.audioProvider = [](uint64_t cycle) { return sound->advanceTo(cycle); };
        libraries.sif.transfer = [](uint32_t address, const uint8_t *bytes, uint32_t count)
        { sound->writeBuffer(address, bytes, count); };
        libraries.sif.allocateBuffer = [](uint32_t bytes) { return sound->allocateBuffer(bytes); };
        libraries.sif.freeBuffer = [](uint32_t address) { return sound->freeBuffer(address); };
        libraries.sif.resetProvider = [] { sound->reset(); };
        libraries.sif.moduleLoaderWithArguments = [trace](const std::string &path, const std::vector<std::string> &args)
        {
            const std::string name = moduleName(path);
            int32_t id;
            // Answered at their EE APIs by BozoEngine's native pad, multitap and memory-card libraries.
            if (name == "SIO2MAN.IRX" || name == "MTAPMAN.IRX" || name == "PADMAN.IRX" || name == "MCMAN.IRX" ||
                name == "MCSERV.IRX")
                id = 1;
            else
                id = sound->startImage(name + ";1", args);
            if (trace)
            {
                std::string joined;
                for (const auto &a : args)
                    joined += " [" + a + "]";
                std::printf("[jc2:sif] load %s%s -> %d\n", path.c_str(), joined.c_str(), id);
            }
            return id;
        };
    }
} // namespace jc2
