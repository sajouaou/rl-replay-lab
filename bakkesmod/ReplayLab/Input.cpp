// Keys and controller. The plugin reads the F keys itself (under Proton the game can stop seeing the keyboard after an
// Alt+Tab) and the controller's d-pad, simulate button and A as the game sees them; a d-pad direction held 0.6 s has a
// second action.
#include "ReplayLab.h"
#include "LabMath.h"
#include "Rules.h"

#include <fstream>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace lab;

// The F keys, read straight from Windows (Wine) while the game window is in front, not through the game's own input:
// after an Alt+Tab under Proton the game can stop seeing the keyboard (or believe Alt is still held).
void ReplayLab::PollKeys() {
    static const struct {
        int vk;
        const char* command;
    } KEYS[] = {{VK_F3, "togglemenu replaylab"}, {VK_F4, "rlab_camera"}, {VK_F5, "rlab_play"},   {VK_F7, "rlab_cam_mode"},
                {VK_F8, "rlab_ballcam"},         {VK_F9, "rlab_capture"}, {VK_F10, "rlab_play"}, {VK_F11, "rlab_retry"}};
    static_assert(sizeof(KEYS) / sizeof(KEYS[0]) == sizeof(keyDown) / sizeof(keyDown[0]), "one state per key");
    DWORD pid = 0;
    HWND front = GetForegroundWindow();
    if (front) GetWindowThreadProcessId(front, &pid);
    const bool ours = front && pid == GetCurrentProcessId();
    if (ours != gameInFront) {
        gameInFront = ours;
        cvarManager->log(ours ? "Replay Lab: jeu au premier plan" : "Replay Lab: jeu en arriere-plan");
    }
    // twice a second, the game in front or not, and when (a script filming the screen films only the game: a file not
    // written for a while means the game no longer runs its frames, minimized)
    const auto now = std::chrono::steady_clock::now();
    if (now - focusWritten > std::chrono::milliseconds(500)) {
        focusWritten = now;
        std::ofstream(gameWrapper->GetDataFolder() / "replaylab_focus.txt", std::ios::trunc)
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
            << " " << (ours ? 1 : 0) << "\n";
    }
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        const bool down = ours && (GetAsyncKeyState(KEYS[i].vk) & 0x8000) != 0;
        if (down && !keyDown[i]) cvarManager->executeCommand(KEYS[i].command);
        keyDown[i] = down;
    }
    // The controller's d-pad, simulate button and A, as the game sees them (any controller it knows goes by the Xbox
    // names). A d-pad direction or the simulate button has a second action when held 0.6 s: its first one comes when it
    // is let go before.
    // A (jump) only skips a goal's replay, like the game's.
    static const char* PAD[] = {"XboxTypeS_DPad_Up", "XboxTypeS_DPad_Left", "XboxTypeS_DPad_Right", "XboxTypeS_DPad_Down", nullptr, "XboxTypeS_A"};
    static const char* PAD_SHORT[] = {"rlab_capture", "rlab_cam_mode", "rlab_camera", "rlab_retry", "rlab_play", "rlab_capture"};
    static const char* PAD_LONG[] = {"rlab_branche", "rlab_cinema_bascule", "rlab_ralenti", "rlab_sauver", "rlab_statut", nullptr};
    const std::string simulate = *opt.manetteSimuler;
    if (simulate != padSimulate) {
        padSimulate = simulate;
        padKey[4] = simulate.empty() ? -1 : gameWrapper->GetFNameIndexByString(simulate);
    }
    const bool pad = *opt.manette;
    for (int i = 0; i < 6; i++) {
        if (PAD[i] && padKey[i] < 0) padKey[i] = gameWrapper->GetFNameIndexByString(PAD[i]);
        // simulate also works in the game's replay viewer (opens this replay there, like F5)
        bool on = pad && ((replay && mode != MODE_IDLE) || (i == 4 && gameWrapper->IsInReplay()));
        if (i == 5) on = on && goalReplay.on;
        const bool down = on && padKey[i] >= 0 && gameWrapper->IsKeyPressed(padKey[i]);
        if (down && !padDown[i]) {
            padSince[i] = now;
            padLong[i] = false;
            if (!PAD_LONG[i]) cvarManager->executeCommand(PAD_SHORT[i]);
        }
        if (down && PAD_LONG[i] && !padLong[i] && now - padSince[i] >= std::chrono::milliseconds(600)) {
            padLong[i] = true;
            cvarManager->executeCommand(PAD_LONG[i]);
        }
        if (!down && padDown[i] && PAD_LONG[i] && !padLong[i]) cvarManager->executeCommand(PAD_SHORT[i]);
        padDown[i] = down;
    }
    if (roleNext >= 0 && std::chrono::steady_clock::now() >= roleAt) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        const int k = rolePlayer, role = roleNext;
        roleNext = -1;
        if (replay && k < replay->Players()) {
            const bool sim = mode == MODE_SIM;
            if (SetRole(k, role)) SetupCars([this, sim] { if (!sim) StartPlayback(rt, playing); }, sim);
            Status(replay->names[k] + ": " + ROLE_LABELS[role] + (role == ROLE_TOI ? "" : " (a la prochaine simulation: bouton reset)"), 5.f);
        }
    }
}

// D-pad down: the next role for the player the camera shows (you, if it shows your car), applied once you stop
// pressing, so going through the roles does not set up the cars at each one
void ReplayLab::NextRole() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (!replay || mode == MODE_IDLE) return;
    static const int ORDER[] = {ROLE_GHOST, ROLE_TOI, ROLE_BOT, ROLE_NEXTO, ROLE_FREE, ROLE_REMOVED};
    const int k = roleNext >= 0 ? rolePlayer : (ShownMode() == CAM_GAME && CarPlayer() >= 0 ? CarPlayer() : camTarget);
    if (k < 0 || k >= replay->Players()) return;
    const int from = roleNext >= 0 ? roleNext : roles[k];
    int at = 0;
    while (at < 6 && ORDER[at] != from) at++;
    rolePlayer = k;
    roleNext = ORDER[(at + 1) % 6];
    roleAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    Status("Statut de " + replay->names[k] + ": " + ROLE_LABELS[roleNext] + "   (Back longue: suivant)", 3.f);
}

// your controller's reset button (the game's training reset, whatever button it is on): restart, like F11
void ReplayLab::OnGameReset() {
    const auto now = std::chrono::steady_clock::now();
    if (!replay || mode == MODE_IDLE || !*opt.manette ||
        std::chrono::duration<float>(now - lastReset).count() < 0.5f)
        return;
    lastReset = now;
    gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("rlab_retry"); });
}

ControllerInput ReplayLab::Pad() const {
    if (std::chrono::steady_clock::now() - camInputAt > std::chrono::milliseconds(150)) return ControllerInput{};
    return camInput;
}
