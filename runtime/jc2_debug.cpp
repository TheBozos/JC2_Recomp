// Jade Cocoon 2 testing aids: go to any scene (a forest or an event) without playing up to it.
//
// The game's scenes (mapped from SLUS_203.09, docs/RECOMP_NOTES.md "Scenes and teleport"): a table of 1208 entries at
// 0x5AD4F0, 32 bytes each: +0 the name's address ("WIND", "A014"...), +4..+0x10 the scene's routines, +0x14 its event
// script overlay (a CDDATA resource, -1 for the forests), +0x18 its data resource, +0x1C the overlay's kind. Entries 0-9
// are the forests (WIND, AQUA, FIRE, TERA, their dark sides, Lilith's, the tutorial), the rest event scripts
// (m2esA001 = 38, the Temple 44, the Throne Room 45, the Room of Life 49...). Three halfwords hold where the game is:
// 0x87860C the scene asked for next (-1: none), 0x87860E the current one, 0x878610 the one before. The scene task
// (0x1A1050) moves "next" into "current" and loads it as soon as the game is between scenes or idle in one; New game
// asks for 38 that way (0x1E58E0). So going somewhere is asking for it: the game itself unloads, loads and starts it.
//
// Battles: the game's top-level mode is a byte, S+0 the current one and S+1 the one asked for, S = *(*(gp - 0x73E0) +
// 0x24, 0xFF: none) (0x1FAD00 asks; the title menu's entries at 0x4E8480 give NEW GAME 2, LOAD GAME 7, VERSUS 5,
// BATTLE 3, ARENA 0xD). Seen: 0 the title, 2 the game (a forest or an event scene), 3 a battle, 9 and 0xA other
// screens (the hatching menu, between scenes). An event starts a battle against a hunter (0x221B14) as this
// does: the opponent is a 148-byte record at 0x5F4B80 + 0x94 i (+0 the name, +0x30 the party; 0 is Kahu, 4-18 the
// developers' TEST1-3 parties, 19-150 the hunters), and the battle's setup at 0x8963EC takes +8 the party, +4 -1, +0x38
// and +0x3C 0; the kind (0x89B071) is 3; 0x430E10 tells the field's objects 3 and 4 to stop; then mode 3. The battle
// ends back where it started. Asked only in mode 2 with no mode change pending.
#include "bozo/port.h"
#include "jc2_functions.h"
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#ifdef JC2_WITH_WINDOW
#include "bozo/app/settings_tabs.h"
#include <imgui.h>
#endif

namespace jc2
{
    namespace
    {
        constexpr uint32_t kSceneTable = 0x005AD4F0u;
        constexpr int kSceneCount = 1208;
        constexpr uint32_t kSceneNext = 0x0087860Cu;
        constexpr uint32_t kSceneCurrent = 0x0087860Eu;
        constexpr uint32_t kScenePrevious = 0x00878610u;

        constexpr uint32_t kModeHolder = 0x006C4890u; // gp - 0x73E0: +0x24 the mode bytes
        constexpr uint32_t kOpponents = 0x005F4B80u;
        constexpr int kOpponentCount = 151;
        constexpr uint32_t kBattleSetup = 0x008963ECu;
        constexpr uint32_t kBattleKind = 0x0089B071u;
        constexpr uint32_t kGroups = 0x005857D0u; // battle groups: +0 the battle script's name ("e..."), the slots
        constexpr int kGroupCount = 2000;
        constexpr uint32_t kEnemyGroup = 0x0089B028u; // the enemies the battle reads (an event battle: 0x5857D0 + 0x30 id)
        constexpr int kModeTitle = 0, kModeGame = 2, kModeBattle = 3;

        // The story (docs/RECOMP_NOTES.md "Story state"): halfwords and flag words of the game's block at 0x8785F0.
        constexpr uint32_t kChapter = 0x008788DCu;    // 3 at New game, 4 after the hatching, 5 at the Arena lesson
        constexpr uint32_t kStoryFlags = 0x008788E0u; // 96 flags (0x1F4AE0 sets one)
        constexpr int kStoryFlagCount = 96;
        constexpr uint32_t kSceneDone = 0x008788ECu;  // a bit per scene, set as its event is played (0x1F4B20)
        constexpr uint32_t kForestStage = 0x00878612u; // a forest's stage (0x1A1050 copies it from the next one)
        constexpr uint32_t kForestStageNext = 0x00878614u;
        [[maybe_unused]] constexpr uint32_t kArea = 0x008788D2u;       // the area and the map's sub-area (the effects: area, stage)
        [[maybe_unused]] constexpr uint32_t kSubArea = 0x008788D6u;
        constexpr uint32_t kSceneAreas = 0x005A92A0u; // 28 bytes: +0 a scene, +0x16 its area (0x1EAD60); -1 ends
        constexpr uint32_t kStageSubAreas = 0x005CA340u; // per area a table of 36 words: stage -> sub-area (0x205110)
        constexpr int kStageCount = 36;

        const uint8_t *g_ram = nullptr;
        std::atomic<int> g_battle{-1}; // the opponent to fight from the next frame wait
        std::atomic<uint64_t> g_battleAt{0};
        std::atomic<bool> g_battleGroup{false}; // g_battle is a battle group (0x5857D0 + 0x30 n), not a hunter
        std::atomic<uint64_t> g_frame{0};
        std::atomic<int> g_request{-1};   // the scene to ask for at the next frame's end
        std::atomic<uint64_t> g_requestAt{0}; // not before this frame
        std::function<void(const std::string &)> g_message;

        // Story edits asked for (options, F2 > Story), made by the game's thread at a frame's end once a scene runs.
        struct StoryEdit
        {
            enum Kind { Chapter, Flag, SceneDone, Stage, SubArea } kind;
            int value;
            bool set;
        };
        std::mutex g_storyLock;
        std::vector<StoryEdit> g_story;

        uint32_t r32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + a, 4);
            return v;
        }
        int16_t r16(const uint8_t *ram, uint32_t a)
        {
            int16_t v;
            std::memcpy(&v, ram + a, 2);
            return v;
        }
    }

    namespace
    {
        void w32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + a, &v, 4); }
        uint32_t modeBytes(const uint8_t *ram)
        {
            const uint32_t holder = r32(ram, kModeHolder);
            if (holder < 0x100000u || holder >= 0x2000000u)
                return 0;
            const uint32_t s = r32(ram, holder + 0x24);
            return s >= 0x100000u && s < 0x2000000u ? s : 0;
        }
    }

    // The opponent's name (record +0); empty for the unnamed ones.
    std::string opponentName(int i)
    {
        if (!g_ram || i < 0 || i >= kOpponentCount)
            return {};
        const char *s = reinterpret_cast<const char *>(g_ram + kOpponents + 0x94u * uint32_t(i));
        std::string name(s, strnlen(s, 0x14));
        for (char c : name)
            if (c < 32 || c > 126)
                return {};
        return name;
    }

    void requestBattle(int opponent, uint64_t atFrame, bool group)
    {
        g_battleAt = atFrame;
        g_battleGroup = group;
        g_battle = opponent;
    }

    // A battle group's script name without its "e" (a group's +0 names the battle's script: "esAsg_Battle_Tutorial3").
    std::string groupName(int group)
    {
        if (!g_ram || group < 0 || group >= kGroupCount)
            return {};
        const uint32_t name = r32(g_ram, kGroups + 0x30u * uint32_t(group));
        if (name < 0x100000u || name >= 0x2000000u)
            return {};
        const char *s = reinterpret_cast<const char *>(g_ram + name);
        std::string text(s, strnlen(s, 48));
        for (char c : text)
            if (c < 32 || c > 126)
                return {};
        return text.size() > 1 && text[0] == 'e' ? text.substr(1) : text;
    }

    // The frame wait: nothing any more (battles start from the scene's step, below).
    void debugFrameWait(uint8_t *, R5900Context *, Runtime *) {}

    // A battle asked for, as an event script's battle command starts one (0x1EC2C4): it fills the battle state block
    // (0x89B070: -1, the kind, 0 (h); +8 0, +0xC the kind's flags (bit 0 for kind 3, the command's table at 0x6943B0),
    // +0x10 -1) and the enemies the battle reads (0x89B028: a party, as a group of the table 0x5857D0 or a hunter's
    // record +0x30), and the scene's step returns mode 3. The scene's exit (0x1E7500, mode 3) then prepares the battle
    // and puts 0x1E6520 in charge of it and of the way back. Taken only while the scene's script runs (scene +6 = 1).
    bool startBattle(uint8_t *ram, uint32_t scene)
    {
        const int opponent = g_battle.load();
        if (opponent < 0 || g_frame.load() < g_battleAt.load() || r16(ram, scene + 6) != 1)
            return false;
        const uint32_t s = modeBytes(ram);
        if (!s || ram[s] != kModeGame || ram[s + 1] != 0xFF)
            return false; // a mode change pending: wait
        g_battle = -1;
        const bool group = g_battleGroup.load();
        const uint32_t party = group ? kGroups + 0x30u * uint32_t(opponent) : kOpponents + 0x94u * uint32_t(opponent) + 0x30u;
        // Kind 3, a hunter's battle (the field's encounter, 0x221B14), for a group too: an event script's battle (kind
        // 7, the Arena lesson's command 0x1C) waits at its start for that script to run it (no command menu, nothing
        // moves), while a kind 3 battle runs by itself and goes back to the scene. A group has a party record's layout.
        ram[kBattleKind - 1] = 0xFF;
        ram[kBattleKind] = 3;
        ram[kBattleKind + 1] = ram[kBattleKind + 2] = 0;
        w32(ram, kEnemyGroup, party);
        w32(ram, kBattleKind - 1 + 8, 0);
        w32(ram, kBattleKind - 1 + 0xC, 1);
        w32(ram, kBattleKind - 1 + 0x10, 0xFFFFFFFFu);
        w32(ram, scene + 0x20, party); // the scene's script context, copied to 0x89B010.. by the step (0x1E5100)
        // The forest hunter encounter's setup too (0x221B14): the party, -1, 0, 0.
        w32(ram, kBattleSetup + 8, party);
        w32(ram, kBattleSetup + 4, 0xFFFFFFFFu);
        ram[kBattleSetup + 0x38] = 0;
        w32(ram, kBattleSetup + 0x3C, 0);
        const std::string message = group ? "Battle: group " + std::to_string(opponent) + " " + groupName(opponent)
                                          : "Battle: against " + std::to_string(opponent) + " " + opponentName(opponent);
        std::printf("[jc2:debug] frame %llu: %s\n", (unsigned long long)g_frame.load(), message.c_str());
        if (g_message)
            g_message(message);
        return true;
    }

    // The scene's name as the game's table gives it ("A014"); "?" before the game is in memory.
    std::string sceneName(int scene)
    {
        if (!g_ram || scene < 0 || scene >= kSceneCount)
            return "-";
        const uint32_t name = r32(g_ram, kSceneTable + 32u * uint32_t(scene));
        if (name < 0x100000u || name >= 0x2000000u)
            return "?";
        const char *s = reinterpret_cast<const char *>(g_ram + name);
        return std::string(s, strnlen(s, 16));
    }

    std::string sceneLabel(int scene)
    {
        static const char *const forests[] = {"Wind forest", "Water forest", "Fire forest", "Earth forest",
                                              "Dark Wind forest", "Dark Water forest", "Dark Fire forest",
                                              "Dark Earth forest", "Lilith's forest", "Tutorial forest"};
        std::string label = std::to_string(scene) + " " + sceneName(scene);
        if (scene >= 0 && scene < 10)
            label += std::string(" (") + forests[scene] + ")";
        return label;
    }

    void requestScene(int scene, uint64_t atFrame)
    {
        g_requestAt = atFrame;
        g_request = scene;
    }

    // --chapter, --story-flag, --scene-done, --forest-stage, --subarea and F2 > Story.
    void requestStory(int kind, int value, bool set)
    {
        std::lock_guard<std::mutex> lock(g_storyLock);
        g_story.push_back({StoryEdit::Kind(kind), value, set});
    }

    namespace
    {
        void w16(uint8_t *ram, uint32_t a, int16_t v) { std::memcpy(ram + a, &v, 2); }
        [[maybe_unused]] bool flag(const uint8_t *ram, uint32_t base, int n) { return r32(ram, base + 4u * uint32_t(n >> 5)) >> (n & 31) & 1u; }
        void setFlag(uint8_t *ram, uint32_t base, int n, bool on)
        {
            const uint32_t a = base + 4u * uint32_t(n >> 5), bit = 1u << (n & 31);
            w32(ram, a, on ? r32(ram, a) | bit : r32(ram, a) & ~bit);
        }

        // A scene's area (0x1EAD60): the forests 0-3 Water, Fire, Wind, Earth; 4-7 their dark sides; 10 the town.
        int sceneArea(const uint8_t *ram, int scene)
        {
            for (uint32_t e = kSceneAreas; e < kSceneAreas + 0x1Cu * 2000u; e += 0x1Cu)
            {
                const int s = r16(ram, e);
                if (s == -1)
                    break;
                if (s == scene)
                    return int8_t(ram[e + 0x16]);
            }
            return -1;
        }

        // The stage that gives a forest area this sub-area (the first in its table), -1 if none does.
        int stageFor(const uint8_t *ram, int area, int subArea)
        {
            if (area < 0 || area > 9)
                return -1;
            const uint32_t table = r32(ram, kStageSubAreas + 4u * uint32_t(area));
            if (table < 0x100000u || table >= 0x2000000u)
                return -1;
            for (int stage = 0; stage < kStageCount; ++stage)
                if (int(r32(ram, table + 4u * uint32_t(stage))) == subArea)
                    return stage;
            return -1;
        }

        int g_subArea = -1; // --subarea: kept for the next forest entered (the stage depends on its area)

        void say(const std::string &message)
        {
            std::printf("[jc2:debug] frame %llu: %s\n", (unsigned long long)g_frame.load(), message.c_str());
            if (g_message)
                g_message(message);
        }

        void setStage(uint8_t *ram, int value)
        {
            w16(ram, kForestStage, int16_t(value));
            w16(ram, kForestStageNext, int16_t(value));
        }

        // The story edits, once the game is in a scene (New game clears the block, 0x1E5A80, before its first one).
        void applyStory(uint8_t *ram)
        {
            const uint32_t s = modeBytes(ram);
            if (!s || ram[s] != kModeGame || r16(ram, kSceneCurrent) < 0)
                return;
            std::vector<StoryEdit> edits;
            {
                std::lock_guard<std::mutex> lock(g_storyLock);
                edits.swap(g_story);
            }
            for (const StoryEdit &e : edits)
                switch (e.kind)
                {
                case StoryEdit::Chapter:
                    say("Story: chapter " + std::to_string(r16(ram, kChapter)) + " -> " + std::to_string(e.value));
                    w16(ram, kChapter, int16_t(e.value));
                    break;
                case StoryEdit::Flag:
                    if (e.value >= 0 && e.value < kStoryFlagCount)
                    {
                        setFlag(ram, kStoryFlags, e.value, e.set);
                        say("Story: flag " + std::to_string(e.value) + (e.set ? " set" : " cleared"));
                    }
                    break;
                case StoryEdit::SceneDone:
                    if (e.value >= 0 && e.value < kSceneCount)
                    {
                        setFlag(ram, kSceneDone, e.value, e.set);
                        say("Story: scene " + sceneLabel(e.value) + (e.set ? " done" : " not done"));
                    }
                    break;
                case StoryEdit::Stage:
                    setStage(ram, e.value);
                    say("Story: forest stage " + std::to_string(e.value) + " (taken as a forest is entered)");
                    break;
                case StoryEdit::SubArea:
                    g_subArea = e.value;
                    // Not going anywhere: enter the forest the game is in again, to take it.
                    if (g_request.load() < 0 && r16(ram, kSceneCurrent) < 10)
                        requestScene(r16(ram, kSceneCurrent), 0);
                    break;
                }
        }
    }

    // At each game frame's end (the EE thread, the game waiting for its frame).
    void debugFrame(uint8_t *ram, uint64_t frame)
    {
        g_ram = ram;
        g_frame = frame;
        applyStory(ram);
    }

    // An event scene's step (0x1E7B90(scene)), each frame while it runs: scene +6 is its state (1 the script runs, 3
    // waits for the scene's loads (0x151320) and then leaves if "next" is set, returning mode 2; the caller keeps it in
    // scene +0x1314 and the scene's exit (0x1E7500) loads "next"). A teleport is that exit: "next" and state 3, taken
    // only while the script runs (state 1), so the game unloads and loads as when a script leaves.
    constexpr uint32_t kSceneStep = 0x001E7B90u;
    void sceneStep(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        const uint32_t scene = ctx->r[4].uw[0];
        const int target = g_request.load();
        if (target >= 0 && g_frame.load() >= g_requestAt.load() && scene >= 0x100000u && scene < 0x2000000u - 8 &&
            r16(ram, scene + 6) == 1)
        {
            g_request = -1;
            // A field scene only: kind 0 (the forests) or 1 (entry +0x1C), which have an entry function (+4): 574 of
            // the 1208. Kinds 2 to 5 have none (the game enters them another way); a teleport there jumped to 0.
            const uint32_t entry = kSceneTable + 32u * uint32_t(target);
            if (target >= kSceneCount || r32(ram, entry + 0x1C) > 1u || !r32(ram, entry + 4))
            {
                const std::string message = "Teleport: " + sceneLabel(target) + " is not a field scene (kind " +
                                            std::to_string(target < kSceneCount ? r32(ram, entry + 0x1C) : 0) + ")";
                std::printf("[jc2:debug] frame %llu: %s\n", (unsigned long long)g_frame.load(), message.c_str());
                if (g_message)
                    g_message(message);
                rt->functions.generated(kSceneStep)(ram, ctx, rt);
                return;
            }
            // --subarea: the stage giving it in the forest entered (the forest's entry 0x22B770 reads its sub-area
            // from its area's table at that stage, 0x205110).
            if (g_subArea >= 0 && target < 10)
            {
                const int area = sceneArea(ram, target), st = stageFor(ram, area, g_subArea);
                if (st >= 0)
                {
                    setStage(ram, st);
                    say("Story: sub-area " + std::to_string(g_subArea) + " of area " + std::to_string(area) +
                        " is forest stage " + std::to_string(st));
                }
                else
                    say("Story: area " + std::to_string(area) + " has no sub-area " + std::to_string(g_subArea));
                g_subArea = -1;
            }
            const int16_t next = int16_t(target), leaving = 3;
            std::memcpy(ram + kSceneNext, &next, 2);
            std::memcpy(ram + scene + 6, &leaving, 2);
            const std::string message = "Teleport: " + sceneLabel(r16(ram, kSceneCurrent)) + " -> " + sceneLabel(target);
            std::printf("[jc2:debug] frame %llu: %s\n", (unsigned long long)g_frame.load(), message.c_str());
            if (g_message)
                g_message(message);
        }
        const bool battle = scene >= 0x100000u && scene < 0x2000000u - 8 && startBattle(ram, scene);
        const uint32_t party = battle ? r32(ram, kEnemyGroup) : 0;
        rt->functions.generated(kSceneStep)(ram, ctx, rt);
        if (battle)
        {
            ctx->r[2].ud[0] = kModeBattle; // the scene leaves into a battle
            // The step's script runner copies the running script's context over 0x89B010..0x89B030 (0x1E5100): the
            // group written before it was 0 again by the exit (JC2_TRACE_BATTLE). Written again after it.
            w32(ram, kEnemyGroup, party);
        }
    }

    // JC2_TRACE_BATTLE=1: the way into a battle. The field task step 0x1A1670(task): its +8 (the transition: 2 or
    // 0x17 read the enemies' group) and +0xC (its state) when they change; the enemies' set-up 0x286260(group); the
    // scene exit 0x1E7500(scene) with the mode the step left at scene +0x1314.
    constexpr uint32_t kFieldTask = 0x001A1670u;
    constexpr uint32_t kEnemySetup = 0x00286260u;
    constexpr uint32_t kSceneExit = 0x001E7500u;
    const bool traceBattle = std::getenv("JC2_TRACE_BATTLE") != nullptr;
    void fieldTask(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        const uint32_t task = ctx->r[4].uw[0];
        if (traceBattle && task >= 0x100000u && task + 0x10u <= 0x2000000u)
        {
            static uint64_t last = ~0ull;
            const uint64_t now = uint64_t(r32(ram, task + 8)) << 32 | r32(ram, task + 0xC);
            if (now != last)
            {
                last = now;
                std::printf("[jc2:battle] frame %llu: field task %#x +8 %u +0xC %u\n", (unsigned long long)g_frame.load(),
                            task, r32(ram, task + 8), r32(ram, task + 0xC));
            }
        }
        rt->functions.generated(kFieldTask)(ram, ctx, rt);
    }
    void enemySetup(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        if (traceBattle)
            std::printf("[jc2:battle] frame %llu: enemies from group %#x (ra %#x)\n", (unsigned long long)g_frame.load(),
                        ctx->r[4].uw[0], ctx->r[31].uw[0]);
        rt->functions.generated(kEnemySetup)(ram, ctx, rt);
    }
    void sceneExit(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        const uint32_t scene = ctx->r[4].uw[0];
        if (traceBattle && scene >= 0x100000u && scene + 0x1318u <= 0x2000000u)
            std::printf("[jc2:battle] frame %llu: scene exit %#x mode %u, battle block %08x %08x %08x %08x, group %#x\n",
                        (unsigned long long)g_frame.load(), scene, r32(ram, scene + 0x1314), r32(ram, 0x0089B070u),
                        r32(ram, 0x0089B074u), r32(ram, 0x0089B078u), r32(ram, 0x0089B07Cu), r32(ram, 0x0089B028u));
        rt->functions.generated(kSceneExit)(ram, ctx, rt);
    }

    void installDebugOverrides(bozo::port::Registry &r)
    {
        r.registerFunction(kSceneStep, sceneStep);
        r.registerFunction(kFieldTask, fieldTask);
        r.registerFunction(kEnemySetup, enemySetup);
        r.registerFunction(kSceneExit, sceneExit);
    }

    void installDebug(uint8_t *ram, std::function<void(const std::string &)> message)
    {
        g_ram = ram;
        g_message = std::move(message);
#ifdef JC2_WITH_WINDOW
        // F2 > Teleport: where the game is, and any scene to go to.
        bozo::app::ui::registerTab("Teleport", []
        {
            if (!g_ram)
                return;
            const int current = r16(g_ram, kSceneCurrent), previous = r16(g_ram, kScenePrevious),
                      next = r16(g_ram, kSceneNext);
            ImGui::Text("Scene: %s", sceneLabel(current).c_str());
            ImGui::Text("Before: %s", sceneLabel(previous).c_str());
            if (next >= 0 || g_request >= 0)
                ImGui::TextDisabled("Going to %s", sceneLabel(next >= 0 ? next : g_request.load()).c_str());
            ImGui::Separator();
            static char filter[32] = "";
            static int selected = 0;
            ImGui::SetNextItemWidth(160);
            ImGui::InputTextWithHint("##filter", "Filter (A014, forest...)", filter, sizeof(filter));
            ImGui::SameLine();
            if (ImGui::Button("Go"))
                requestScene(selected, 0);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", sceneLabel(selected).c_str());
            if (ImGui::BeginListBox("##scenes", ImVec2(-1, 300)))
            {
                std::string needle(filter);
                for (char &c : needle)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                for (int i = 0; i < kSceneCount; ++i)
                {
                    std::string label = sceneLabel(i), low = label;
                    for (char &c : low)
                        c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (!needle.empty() && low.find(needle) == std::string::npos)
                        continue;
                    if (ImGui::Selectable(label.c_str(), selected == i))
                        selected = i;
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                        requestScene(i, 0);
                }
                ImGui::EndListBox();
            }
            ImGui::TextDisabled("Double-click or Go. The game moves there once it is between scenes or idle in one;\n"
                                "a story scene runs as if reached, with the story flags the save has.");
        });
        // F2 > Battle: fight any hunter (or the developers' test parties) from where the game is.
        bozo::app::ui::registerTab("Battle", []
        {
            if (!g_ram)
                return;
            const uint32_t s = modeBytes(g_ram);
            const int mode = s ? g_ram[s] : -1;
            ImGui::Text("Game mode: %d%s", mode,
                        mode == kModeBattle ? " (battle)" : mode == kModeGame ? " (game)"
                                                          : mode == kModeTitle ? " (title)" : "");
            static int selected = 19;
            if (ImGui::Button("Fight"))
                requestBattle(selected, 0, false);
            ImGui::SameLine();
            ImGui::TextDisabled("%d %s", selected, opponentName(selected).c_str());
            if (g_battle >= 0)
                ImGui::TextDisabled("Waiting for the game's own mode (not in a menu or a battle)...");
            if (ImGui::BeginListBox("##opponents", ImVec2(-1, 300)))
            {
                for (int i = 1; i < kOpponentCount; ++i)
                {
                    const std::string name = opponentName(i);
                    if (name.empty())
                        continue;
                    const std::string label = std::to_string(i) + " " + name;
                    if (ImGui::Selectable(label.c_str(), selected == i))
                        selected = i;
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                        requestBattle(i, 0, false);
                }
                ImGui::EndListBox();
            }
            ImGui::TextDisabled("The battle starts from a field scene (not the title) and plays as a hunter's does,\n"
                                "going back to the scene after. Event battles' scripts (dialogue, tutorials) do not run.");
        });
        // F2 > Story: the chapter, the story flags, the scenes played and the forests' stage, read and set.
        bozo::app::ui::registerTab("Story", []
        {
            if (!g_ram)
                return;
            const int current = r16(g_ram, kSceneCurrent);
            ImGui::Text("Chapter %d   forest stage %d (next %d)   area %d sub-area %d", r16(g_ram, kChapter),
                        r16(g_ram, kForestStage), r16(g_ram, kForestStageNext), r16(g_ram, kArea), r16(g_ram, kSubArea));
            static int chapter = 5, stageValue = 0, subArea = 8;
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("##chapter", &chapter);
            ImGui::SameLine();
            if (ImGui::Button("Set chapter"))
                requestStory(StoryEdit::Chapter, chapter, true);
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("##stage", &stageValue);
            ImGui::SameLine();
            if (ImGui::Button("Set forest stage"))
                requestStory(StoryEdit::Stage, stageValue, true);
            ImGui::SetNextItemWidth(100);
            ImGui::InputInt("##subarea", &subArea);
            ImGui::SameLine();
            if (ImGui::Button("Enter forest at sub-area"))
                requestStory(StoryEdit::SubArea, subArea, true);
            if (current >= 0 && current < kSceneCount)
            {
                bool done = flag(g_ram, kSceneDone, current);
                if (ImGui::Checkbox(("Scene " + sceneLabel(current) + " played").c_str(), &done))
                    requestStory(StoryEdit::SceneDone, current, done);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Story flags");
            for (int i = 0; i < kStoryFlagCount; ++i)
            {
                bool on = flag(g_ram, kStoryFlags, i);
                if (i % 12)
                    ImGui::SameLine();
                if (ImGui::Checkbox(("##f" + std::to_string(i)).c_str(), &on))
                    requestStory(StoryEdit::Flag, i, on);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("flag %d", i);
            }
            ImGui::TextDisabled("The scenes' scripts choose their events by chapter and by these flags. A forest takes\n"
                                "its stage (which picks its sub-area, its effects) as it is entered.");
        });
#endif
    }
}
