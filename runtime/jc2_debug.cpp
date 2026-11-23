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
        constexpr int kModeTitle = 0, kModeGame = 2, kModeBattle = 3;

        const uint8_t *g_ram = nullptr;
        std::atomic<int> g_battle{-1}; // the opponent to fight from the next frame wait
        std::atomic<uint64_t> g_battleAt{0};
        std::atomic<uint64_t> g_frame{0};
        std::atomic<int> g_request{-1};   // the scene to ask for at the next frame's end
        std::atomic<uint64_t> g_requestAt{0}; // not before this frame
        std::function<void(const std::string &)> g_message;

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

    void requestBattle(int opponent, uint64_t atFrame)
    {
        g_battleAt = atFrame;
        g_battle = opponent;
    }

    // In the frame wait (the main thread, before the frame ends): a battle asked for starts as an event starts one.
    // Only in mode 2 (a forest or an event scene) with no mode change pending.
    void debugFrameWait(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        const int opponent = g_battle.load();
        if (opponent < 0 || g_frame.load() < g_battleAt.load())
            return;
        const uint32_t s = modeBytes(ram);
        if (!s || ram[s] != kModeGame || ram[s + 1] != 0xFF)
            return; // the title, a battle, a menu, a mode change pending: wait
        g_battle = -1;
        w32(ram, kBattleSetup + 8, kOpponents + 0x94u * uint32_t(opponent) + 0x30u);
        w32(ram, kBattleSetup + 4, 0xFFFFFFFFu);
        ram[kBattleSetup + 0x38] = 0;
        w32(ram, kBattleSetup + 0x3C, 0);
        ram[kBattleKind] = 3;
        R5900Context call = *ctx; // the game's own step, on the waiting thread's stack below its frame
        call.r[31].uw[0] = ctx->pc;
        jc2_func_0x430e10(ram, &call, rt);
        ram[s + 1] = kModeBattle;
        const std::string message = "Battle: against " + std::to_string(opponent) + " " + opponentName(opponent);
        std::printf("[jc2:debug] frame %llu: %s\n", (unsigned long long)g_frame.load(), message.c_str());
        if (g_message)
            g_message(message);
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

    // At each game frame's end (the EE thread, the game waiting for its frame).
    void debugFrame(uint8_t *ram, uint64_t frame)
    {
        g_ram = ram;
        g_frame = frame;
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
            const int16_t next = int16_t(target), leaving = 3;
            std::memcpy(ram + kSceneNext, &next, 2);
            std::memcpy(ram + scene + 6, &leaving, 2);
            const std::string message = "Teleport: " + sceneLabel(r16(ram, kSceneCurrent)) + " -> " + sceneLabel(target);
            std::printf("[jc2:debug] frame %llu: %s\n", (unsigned long long)g_frame.load(), message.c_str());
            if (g_message)
                g_message(message);
        }
        rt->functions.generated(kSceneStep)(ram, ctx, rt);
    }

    void installDebugOverrides(bozo::port::Registry &r)
    {
        r.registerFunction(kSceneStep, sceneStep);
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
                requestBattle(selected, 0);
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
                        requestBattle(i, 0);
                }
                ImGui::EndListBox();
            }
            ImGui::TextDisabled("Starts as an event starts a hunter battle; it ends back where it started.");
        });
#endif
    }
}
