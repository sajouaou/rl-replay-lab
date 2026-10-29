// The F3 window (ImGui): the replay list, the timeline, cameras, roles, branches, saves, options; and the F2 tab.
#include "ReplayLab.h"
#include "LabMath.h"
#include "Rules.h"
#include "imgui/imgui.h"

using namespace lab;

void ReplayLab::Render() {
    bool keepOpen = true;   // false once the window's close button is clicked (F3 goes through BakkesMod itself)
    if (!ImGui::Begin(GetMenuTitle().c_str(), &keepOpen, ImGuiWindowFlags_None)) {
        ImGui::End();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mtx);

    // ---- replay list
    static int selected = -1;
    const char* preview = selected >= 0 && selected < static_cast<int>(library.size()) ? library[selected].label.c_str()
                                                                                       : (listing ? "lecture du dossier..." : "choisir un replay");
    ImGui::SetNextItemWidth(460);
    if (ImGui::BeginCombo("##replays", preview)) {
        for (int i = 0; i < static_cast<int>(library.size()); i++)
            if (ImGui::Selectable(library[i].label.c_str(), i == selected)) selected = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Ouvrir") && selected >= 0 && selected < static_cast<int>(library.size())) {
        const std::string id = library[selected].id;
        gameWrapper->Execute([this, id](GameWrapper*) { OpenReplay(id, 0, false, ""); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Actualiser")) gameWrapper->Execute([this](GameWrapper*) { RefreshList(); });
    if (std::chrono::steady_clock::now() < statusUntil) ImGui::TextWrapped("%s", status.c_str());

    if (!replay) {
        ImGui::TextWrapped(loading ? "Lecture du replay..."
                                   : "Choisis un replay et Ouvrir. Ou, dans le visionneur de replays du jeu, appuie sur F5 "
                                     "au moment voulu: le replay s'ouvre ici et la simulation demarre de ce moment.");
        ImGui::End();
        if (!keepOpen) gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("togglemenu " + GetMenuName()); });
        return;
    }
    ImGui::Separator();

    // ---- transport
    int score[2];
    ScoreAt(ShownT(), score);
    ImGui::Text("%s   %s %s   bleu %d - %d orange   %s   vue: %s", replay->date.substr(0, 10).c_str(), replay->map.c_str(),
                GameKind() == KIND_SOCCAR ? "" : KIND_LABELS[GameKind()], score[0], score[1],
                (mode == MODE_SIM ? SimClockText() : ClockAt(ShownT())).c_str(), CamText().c_str());
    if (mode == MODE_SIM) {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.85f, 1.f), "SIMULATION  +%.1f s depuis %s  (%s)", simT, ClockAt(forkT).c_str(),
                           BallText().c_str());
        if (ImGui::Button("Recommencer (F11)")) gameWrapper->Execute([this](GameWrapper*) { Retry(); });
        ImGui::SameLine();
        if (ImGui::Button("Retour au replay (F9)")) gameWrapper->Execute([this](GameWrapper*) { BackToReplay(); });
        ImGui::SameLine();
        if (ImGui::Button("Revoir cette simulation")) gameWrapper->Execute([this](GameWrapper*) { WatchBranch(-1); });
    } else {
        if (ImGui::Button("-5 s")) gameWrapper->Execute([this](GameWrapper*) { Seek(rt - 5.f); });
        ImGui::SameLine();
        if (ImGui::Button(playing ? "Pause (F9)" : "Lecture (F9)")) gameWrapper->Execute([this](GameWrapper*) { TogglePlay(); });
        ImGui::SameLine();
        if (ImGui::Button("+5 s")) gameWrapper->Execute([this](GameWrapper*) { Seek(rt + 5.f); });
        ImGui::SameLine();
        int sp = 2;
        for (int i = 0; i < 4; i++) if (std::fabs(SPEEDS[i] - playSpeed) < 1e-3f) sp = i;
        ImGui::SetNextItemWidth(80);
        if (ImGui::Combo("##speed", &sp, SPEED_LABELS, 4)) playSpeed = SPEEDS[sp];
        ImGui::SameLine();
        if (ImGui::Button("Simuler d'ici (F5)")) gameWrapper->Execute([this](GameWrapper*) { Fork(); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Vue suivante (F4)")) gameWrapper->Execute([this](GameWrapper*) { NextView(); });
    float pos = (mode == MODE_SIM ? forkT : rt) - Start();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##time", &pos, 0.f, End() - Start(), "%.1f s")) {
        const float target = Start() + pos;
        gameWrapper->Execute([this, target](GameWrapper*) { Seek(target); });
    }
    {   // goals (team colour) and saves (white) on the slider
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float span = std::max(End() - Start(), 1e-3f);
        auto x = [&](int frame) { return a.x + (b.x - a.x) * (replay->time[frame] - Start()) / span; };
        for (const auto& g : replay->goals)
            dl->AddLine(ImVec2(x(g.first), a.y), ImVec2(x(g.first), b.y), g.second == 0 ? IM_COL32(80, 140, 255, 255) : IM_COL32(255, 150, 50, 255), 3.f);
        for (const auto& s : replay->saves)
            dl->AddLine(ImVec2(x(s.first), b.y - 6), ImVec2(x(s.first), b.y), IM_COL32(255, 255, 255, 255), 3.f);
    }
    ImGui::TextDisabled("Barres bleues/orange: buts.  Traits blancs: arrets.");
    DrawBranches();
    ImGui::Separator();
    DrawCamera();
    ImGui::Separator();

    DrawRoles();
    ImGui::Separator();

    // ---- saves
    ImGui::Text("Arrets (%d)", static_cast<int>(replay->saves.size()));
    for (size_t i = 0; i < replay->saves.size(); i++) {
        const int frame = replay->saves[i].first, k = replay->saves[i].second;
        if (k < 0 || k >= replay->Players()) continue;
        ImGui::PushID(2000 + static_cast<int>(i));
        const ImVec4 col = replay->teams[k] == 0 ? ImVec4(0.4f, 0.6f, 1.f, 1.f) : ImVec4(1.f, 0.62f, 0.25f, 1.f);
        ImGui::TextColored(col, "%s  %s", ClockAt(replay->time[frame]).c_str(), replay->names[k].c_str());
        ImGui::SameLine(230);
        const float t = replay->time[frame];
        if (ImGui::Button("Voir")) gameWrapper->Execute([this, t](GameWrapper*) { PlayFrom(t - 2.5f, true); });
        ImGui::SameLine();
        if (ImGui::Button("Prendre sa place")) {
            for (int& r : roles) r = ROLE_GHOST;
            roles[k] = ROLE_TOI;
            camMode = CAM_GAME;
            gameWrapper->Execute([this, t](GameWrapper*) { SetupCars([this, t] { StartPlayback(t - 1.5f, false); Fork(); }, true); });
        }
        ImGui::SameLine();
        if (ImGui::Button("Sans lui")) {
            for (int& r : roles) r = ROLE_GHOST;
            roles[k] = ROLE_REMOVED;
            // watch from a player of the other team (the shooter's side)
            for (int c = 0; c < replay->Players(); c++)
                if (replay->teams[c] != replay->teams[k]) { camTarget = c; break; }
            gameWrapper->Execute([this, t](GameWrapper*) { SetupCars([this, t] { StartPlayback(t - 1.5f, false); Fork(); }, true); });
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    DrawOptions();
    ImGui::End();
    if (!keepOpen) gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("togglemenu " + GetMenuName()); });
}

void ReplayLab::RenderSettings() {
    ImGui::TextWrapped("Replay Lab rejoue tes replays dans l'entrainement libre: a tout moment, F5 et tu reprends la main "
                       "(ou une autre voiture devient un bot, ou disparait), avec la vraie physique du jeu.");
    ImGui::TextUnformatted("F3 fenetre   F4 vue   F9 lecture/pause   F5/F10 simuler d'ici   F11 recommencer");
    bool pad = *opt.manette;
    if (ImGui::Checkbox("Manette: croix haut pause, gauche camera, droite joueur, bas recommencer (longs: branche, cinema, ralenti, sauvegarder); Back: simuler (long: statut)", &pad))
        cvarManager->getCvar("rlab_manette").setValue(pad ? 1 : 0);
    // opening a window while BakkesMod is drawing its own menu crashes it: queue it on the game thread
    if (ImGui::Button("Ouvrir la fenetre Replay Lab (F3)"))
        gameWrapper->Execute([this](GameWrapper*) { cvarManager->executeCommand("togglemenu " + GetMenuName()); });
    ImGui::Separator();
    DrawOptions();
}

void ReplayLab::SetImGuiContext(uintptr_t ctx) { ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx)); }

// Only the mouse over the window is kept from the game. Not the keyboard: once clicked, the window keeps the keyboard
// focus (you drive with a controller and never click elsewhere), and blocking it then swallowed every shortcut.
bool ReplayLab::ShouldBlockInput() {
    return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantTextInput;
}

// Returns true when your own car now stands for another player (the cars must be set up again).
bool ReplayLab::SetRole(int k, int role) {
    const bool carChanged = role == ROLE_TOI || roles[k] == ROLE_TOI;
    if (role == ROLE_TOI)
        for (int& r : roles) if (r == ROLE_TOI) r = ROLE_GHOST;
    roles[k] = role;
    if (role == ROLE_REMOVED && k == camTarget) {   // the camera was on him: on to the next player
        for (int step = 1; step < replay->Players(); step++) {
            const int c = (k + step) % replay->Players();
            if (roles[c] != ROLE_REMOVED) {
                camTarget = c;
                break;
            }
        }
    }
    if (role == ROLE_TOI) {   // behind the car you drive
        camMode = CAM_GAME;
        camTarget = k;
    }
    return carChanged;
}

void ReplayLab::DrawRoles() {
    ImGui::TextUnformatted("Role de chaque voiture (s'applique a la prochaine simulation / F11):");
    bool carChanged = false;
    for (int k = 0; k < replay->Players(); k++) {
        ImGui::PushID(k);
        const ImVec4 col = replay->teams[k] == 0 ? ImVec4(0.4f, 0.6f, 1.f, 1.f) : ImVec4(1.f, 0.62f, 0.25f, 1.f);
        ImGui::TextColored(col, "%s%s", replay->names[k].c_str(), k == ToiIndex() ? "  [tu conduis]" : (k == camTarget ? "  [camera]" : ""));
        ImGui::SameLine(230);
        ImGui::SetNextItemWidth(260);
        int role = roles[k];
        if (ImGui::Combo("##role", &role, ROLE_LABELS, ROLE_COUNT) && SetRole(k, role)) carChanged = true;

        ImGui::PopID();
    }
    if (ImGui::Button("Tous en fantome")) for (int& r : roles) if (r != ROLE_TOI) r = ROLE_GHOST;
    ImGui::SameLine();
    const int myTeam = replay->teams[ToiIndex() >= 0 ? ToiIndex() : std::min(camTarget, replay->Players() - 1)];
    if (ImGui::Button("Adversaires en bots Psyonix"))
        for (int k = 0; k < replay->Players(); k++) if (roles[k] != ROLE_TOI && replay->teams[k] != myTeam) roles[k] = ROLE_BOT;
    ImGui::SameLine();
    if (ImGui::Button("Adversaires en Nexto"))
        for (int k = 0; k < replay->Players(); k++) if (roles[k] != ROLE_TOI && replay->teams[k] != myTeam) roles[k] = ROLE_NEXTO;
    ImGui::SameLine();
    if (ImGui::Button("Personne pour moi (je regarde)")) {
        for (int& r : roles) if (r == ROLE_TOI) r = ROLE_GHOST;
        carChanged = true;
    }
    if (ToiIndex() < 0)
        ImGui::TextDisabled("Personne n'est 'Toi': tu regardes, chaque joueur dans sa vraie voiture (F4: joueur suivant).");
    if (carChanged) {
        const bool sim = mode == MODE_SIM;
        gameWrapper->Execute([this, sim](GameWrapper*) {
            SetupCars([this, sim] { if (!sim) StartPlayback(rt, playing); }, sim);
        });
    }
}

void ReplayLab::DrawCamera() {
    static const char* MODES[CAM_COUNT] = {"Jeu", "Joueur", "Libre", "Automatique"};
    ImGui::TextUnformatted("Camera (F7):");
    for (int m = 0; m < CAM_COUNT; m++) {
        ImGui::SameLine();
        if (ImGui::RadioButton(MODES[m], camMode == m) && camMode != m) {
            camMode = (m + CAM_COUNT - 1) % CAM_COUNT;
            gameWrapper->Execute([this](GameWrapper*) { NextCamMode(); });
        }
    }
    if (camMode == CAM_PLAYER) {
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("##camtarget", replay->names[std::min(camTarget, replay->Players() - 1)].c_str())) {
            for (int k = 0; k < replay->Players(); k++)
                if (ImGui::Selectable(replay->names[k].c_str(), k == camTarget)) camTarget = k;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("F4: joueur suivant.");
    } else if (camMode == CAM_FREE) {
        ImGui::TextDisabled("Ta manette quand elle ne conduit pas: gachettes avancer, stick tourner/monter, boost vite. En revoyant un replay ou un scenario.");
    } else if (camMode == CAM_AUTO) {
        ImGui::TextDisabled("Suit le joueur le plus proche du ballon, camera ballon.");
    }
    if (ImGui::Button("Camera ballon (F8)")) gameWrapper->Execute([this](GameWrapper*) { ToggleBallCam(); });
    ImGui::SameLine();
    bool his = *opt.camJoueur;
    if (ImGui::Checkbox("Vue joueur: ses reglages camera et sa camera ballon (comme le replay)", &his))
        cvarManager->getCvar("rlab_cam_joueur").setValue(his ? 1 : 0);
    bool cinema = *opt.cinema;
    if (ImGui::Checkbox("Mode cinema: rien de Replay Lab a l'ecran (pour filmer; F3 pour cette fenetre)", &cinema))
        cvarManager->getCvar("rlab_cinema").setValue(cinema ? 1 : 0);
}

void ReplayLab::DrawOptions() {
    auto check = [this](const char* cvar, const char* label) {
        bool on = cvarManager->getCvar(cvar).getBoolValue();
        if (ImGui::Checkbox(label, &on)) cvarManager->getCvar(cvar).setValue(on ? 1 : 0);
    };
    static const char* STARTS[] = {"tout de suite", "quand tu bouges ta manette (comme l'entrainement)", "compte a rebours 3-2-1"};
    int start = std::min(std::max(*opt.attendre, 0), 2);
    ImGui::SetNextItemWidth(330);
    if (ImGui::Combo("Depart d'une simulation (au coup d'envoi: toujours 3-2-1)", &start, STARTS, 3))
        cvarManager->getCvar("rlab_attendre").setValue(start);
    check("rlab_explosion", "Buts: le ballon explose avec l'explosion de but du buteur");
    check("rlab_revoir_buts", "Replay: chaque but revu comme le jeu (REPLAY, buteur, km/h) avant le coup d'envoi");
    check("rlab_revoir_simulation", "Simulation: un but revu juste apres (decoche pour t'entrainer vite, ex. un arret)");
    bool sync = *opt.sync;
    if (ImGui::Checkbox("Le ballon suit le replay tant qu'aucune voiture changee ne s'approche", &sync))
        cvarManager->getCvar("rlab_sync").setValue(sync ? 1 : 0);
    bool ghosts = *opt.ghosts;
    if (ImGui::Checkbox("Dessiner le vrai replay en fil de fer pendant la simulation", &ghosts))
        cvarManager->getCvar("rlab_ghosts").setValue(ghosts ? 1 : 0);
    bool mirror = *opt.mirror;
    if (ImGui::Checkbox("Si ta voiture est orange, tourner la scene (tu defends le but du bas)", &mirror))
        cvarManager->getCvar("rlab_mirror").setValue(mirror ? 1 : 0);
    float speed = *opt.speed;
    if (ImGui::SliderFloat("Vitesse du jeu en simulation", &speed, 0.1f, 1.f, "%.2f")) cvarManager->getCvar("rlab_speed").setValue(speed);
}

// Each simulation is a branch of the replay: watched again like a replay, filmed, simulated from again (a branch of it).
void ReplayLab::DrawBranches() {
    const ImVec4 pink(1.f, 0.45f, 0.85f, 1.f);
    if (replay->scenario) {
        ImGui::TextColored(pink, "%s: tu la revois comme un replay. F5: resimuler d'un moment, F11: recommencer.",
                           replay->label.c_str());
        if (ImGui::Button("Revenir au replay d'origine")) gameWrapper->Execute([this](GameWrapper*) { BackToOriginal(); });
    }
    // the last try: kept only if you save it
    if (scenario && scenario->Frames() >= 30 && std::find(branches.begin(), branches.end(), scenario) == branches.end()) {
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.9f, 1.f), "Dernier essai %s  (%.0f s), pas sauvegarde", scenario->label.c_str(),
                           scenario->time.back() - scenario->time.front());
        ImGui::SameLine();
        if (ImGui::Button("Sauvegarder (croix bas longue)")) gameWrapper->Execute([this](GameWrapper*) { SaveBranch(); });
        ImGui::SameLine();
        if (ImGui::Button("Revoir##essai")) gameWrapper->Execute([this](GameWrapper*) { WatchBranch(-1); });
    }
    if (branches.empty()) return;
    ImGui::Text("Branches sauvegardees (a revoir et filmer; croix haut longue: la suivante)");
    for (int i = static_cast<int>(branches.size()) - 1; i >= 0; i--) {
        ImGui::PushID(3000 + i);
        const bool shown = branches[i] == replay;
        ImGui::TextColored(shown ? pink : ImVec4(0.85f, 0.85f, 0.9f, 1.f), "%s  (%.0f s)%s", branches[i]->label.c_str(),
                           branches[i]->Frames() ? branches[i]->time.back() - branches[i]->time.front() : 0.f, shown ? "  [a l'ecran]" : "");
        ImGui::SameLine();
        if (ImGui::Button("Revoir")) gameWrapper->Execute([this, i](GameWrapper*) { WatchBranch(i); });
        ImGui::PopID();
    }
}
