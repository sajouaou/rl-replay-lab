// What the plugin draws over the game: the scoreboard, players' names, titles and captions for videos, its own lines
// of help and status, and during a simulation where the changed cars really went.
#include "ReplayLab.h"
#include "LabMath.h"
#include "Draw.h"
#include "Rules.h"

using namespace lab;

void ReplayLab::DrawHud(CanvasWrapper canvas) {
    if (!gameWrapper->IsInFreeplay() && !gameWrapper->IsInReplay()) return;
    std::lock_guard<std::recursive_mutex> lock(mtx);
    const bool scene = replay && mode != MODE_IDLE && gameWrapper->IsInFreeplay();
    if (scene && *opt.noms && *opt.nomsPlugin) DrawNameTags(canvas);
    if (scene) DrawMatch(canvas);   // under the scoreboard: the screen going dark leaves it
    if (scene && *opt.tableau) DrawScoreboard(canvas);
    DrawCaptions(canvas);   // over the rest (the flash too)
    if (*opt.cinema && gameWrapper->IsInFreeplay()) return;   // filming: nothing else of ours
    std::vector<std::string> lines;
    if (gameWrapper->IsInReplay()) {
        lines.push_back("Replay Lab   F5/F10 simuler depuis ce moment");
    } else {
        lines.push_back("Replay Lab   F3 fenetre   F4 vue   F9 lecture/pause   F5/F10 simuler d'ici   F11 recommencer   F7 camera");
        if (*opt.manette) {
            lines.push_back("Manette (appui long: 2e action)  croix haut: lecture (branche suivante)   gauche: camera (cinema)   "
                            "droite: joueur (ralenti)   bas: recommencer (sauvegarder)");
            lines.push_back("          Back: simuler d'ici (statut du joueur)   accelerer / freiner: avance / retour rapide   "
                            "A: passer le replay d'un but");
        }
        if (replay && mode == MODE_PLAYBACK)
            lines.push_back((replay->scenario ? replay->label + "   " : "") + (goalReplay.on ? "REPLAY du but " : playing ? "Lecture " : "Pause ") +
                            ClockAt(ShownT()) + (playSpeed != 1.f ? "  x" + std::to_string(playSpeed).substr(0, 4) : "") + "   " + CamText());
        if (replay && mode == MODE_SIM) {
            char buf[200];
            std::snprintf(buf, sizeof buf, "Simulation +%.1f s depuis %s   %s   %s", simT, ClockAt(forkT).c_str(), BallText().c_str(),
                          CamText().c_str());
            lines.push_back(buf);
        }
    }
    if (std::chrono::steady_clock::now() < statusUntil) lines.push_back(status);
    else if (loading) lines.push_back("Lecture du replay...");
    float width = 820.f;   // as wide as its longest line
    for (const std::string& line : lines) width = std::max(width, canvas.GetStringSize(line, 1.1f, 1.1f).X + 24.f);
    Color(canvas, 10, 14, 24, 170);
    canvas.SetPosition(Vector2{20, 180});
    canvas.FillBox(Vector2F{width, 12.f + 24.f * static_cast<float>(lines.size())});
    for (size_t i = 0; i < lines.size(); i++) {
        if (i == 0) Color(canvas, 255, 255, 255, 255);
        else Color(canvas, 255, 180, 235, 255);
        canvas.SetPosition(Vector2{30, 188 + 24 * static_cast<int>(i)});
        canvas.DrawString(lines[i], i == 0 ? 1.1f : 1.f, i == 0 ? 1.1f : 1.f, true);
    }
    if (replay && mode != MODE_IDLE && gameWrapper->IsInFreeplay()) DrawWorld(canvas);
}

// Like the game's: each team's score and the clock at the top of the screen (kept in cinema mode, as in a real match).
void ReplayLab::DrawScoreboard(CanvasWrapper& canvas) {
    int score[2];
    ScoreAt(ShownT(), score);   // during a goal's replay: the match as it stands after the goal
    const bool sim = mode == MODE_SIM;
    const std::string clock = sim ? SimClockText() : ClockAt(ShownT());
    const int cx = canvas.GetSize().X / 2, top = 16, side = 84, mid = 132, h = 50;
    auto box = [&](int x, int w, int r, int g, int b) {
        Color(canvas, r, g, b, 225);
        canvas.SetPosition(Vector2{x, top});
        canvas.FillBox(Vector2{w, h});
    };
    auto text = [&](const std::string& s, int x, int w, int y, float scale) {
        const Vector2F size = canvas.GetStringSize(s, scale, scale);
        canvas.SetPosition(Vector2F{x + (w - size.X) / 2.f, static_cast<float>(y)});
        canvas.DrawString(s, scale, scale, true);
    };
    box(cx - mid / 2 - side, side, 25, 85, 215);
    box(cx - mid / 2, mid, 14, 17, 26);
    box(cx + mid / 2, side, 235, 115, 20);
    Color(canvas, 255, 255, 255, 255);
    text(std::to_string(score[0]), cx - mid / 2 - side, side, top + 6, 3.f);
    text(clock, cx - mid / 2, mid, top + 6, 3.f);
    text(std::to_string(score[1]), cx + mid / 2, side, top + 6, 3.f);
    std::string under;
    if (sim && match == MATCH_ZERO) under = "0 SECONDE : le ballon doit toucher le sol";
    else if (sim && match == MATCH_OVER) under = "FIN DU MATCH";
    else if (sim && inOvertime) under = "PROLONGATION";
    else if (!sim && OvertimeAt(ShownT())) under = "PROLONGATION";
    if (sim && !inOvertime && match == MATCH_PLAYING && SimClock() < 10.f) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1f s", SimClock());
        under = buf;
    }
    if (!under.empty()) {
        Color(canvas, 255, 220, 60, 255);
        text(under, cx - 200, 400, top + h + 4, 1.4f);
    }
    if (sim && match == MATCH_OVER) {   // like the game's end of match banner
        const int y = canvas.GetSize().Y / 4, w = 900;
        Color(canvas, 10, 12, 20, 200);
        canvas.SetPosition(Vector2{cx - w / 2, y});
        canvas.FillBox(Vector2{w, 104});
        Color(canvas, 255, 255, 255, 255);
        text("FIN DU MATCH", cx - w / 2, w, y + 8, 4.f);
        Color(canvas, 255, 220, 60, 255);
        text(matchResult, cx - w / 2, w, y + 70, 1.6f);
    }
}

// Like the game's name plates (which stay white on a bot): the player's name on his team's colour, smaller farther
// away; not on the car the camera follows. Outside cinema mode it also says who drives a changed car.
void ReplayLab::DrawNameTags(CanvasWrapper& canvas) {
    CameraWrapper cam = gameWrapper->GetCamera();
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (cam.IsNull() || sw.IsNull()) return;
    const Vector camLoc = cam.GetLocation();
    const Axes camAx = AxesOf(cam.GetRotation());
    const bool cinema = *opt.cinema;
    const uintptr_t followed = Followed().memory_address;
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (car.IsNull() || car.GetbHidden() || car.memory_address == followed) continue;
        const int k = BotIndex(car);
        if (k < 0 || car.GetLocation().Z < -100.f) continue;   // not the cars waiting under the field
        const Vector top = Add(car.GetLocation(), Vector(0, 0, 110));
        const float ahead = Dot(Sub(top, camLoc), camAx.f);
        if (ahead < 50.f) continue;
        std::string text = PlayerText(k);
        if (!cinema && mode == MODE_SIM && RoleOf(k) == ROLE_BOT)
            text += k < static_cast<int>(demoed.size()) && demoed[k]   ? " (demoli, bot)"
                    : k < static_cast<int>(bumped.size()) && bumped[k] ? " (touche, bot)"
                                                                       : " (bot)";
        if (!cinema && mode == MODE_SIM && RoleOf(k) == ROLE_FREE) text += " (libre)";
        if (!cinema && mode == MODE_SIM && RoleOf(k) == ROLE_NEXTO) text += " (Nexto)";
        const float scale = std::min(1.6f, std::max(0.8f, 1800.f / Len(Sub(top, camLoc))));
        const Vector2F size = canvas.GetStringSize(text, scale, scale);
        const Vector2 p = canvas.Project(top);
        const float pad = 6.f * scale, x = p.X - size.X / 2.f - pad, y = p.Y - size.Y - 2.f * pad;
        if (GameTeam(k) == 0) Color(canvas, 25, 90, 225, 215);
        else Color(canvas, 235, 115, 20, 215);
        canvas.SetPosition(Vector2F{x, y});
        canvas.FillBox(Vector2F{size.X + 2.f * pad, size.Y + 2.f * pad});
        Color(canvas, 255, 255, 255, 255);
        canvas.SetPosition(Vector2F{x + pad, y + pad});
        canvas.DrawString(text, scale, scale, false);
    }
}

void ReplayLab::DrawCaptions(CanvasWrapper& canvas) {
    const Vector2 screen = canvas.GetSize();
    if (std::chrono::steady_clock::now() < flashUntil) {
        Color(canvas, 255, 0, 255, 255);
        canvas.SetPosition(Vector2{0, 0});
        canvas.FillBox(screen);
        return;
    }
    auto centered = [&](const std::string& s, float scale, float y) {
        const Vector2F size = canvas.GetStringSize(s, scale, scale);
        canvas.SetPosition(Vector2F{(screen.X - size.X) / 2.f, y});
        canvas.DrawString(s, scale, scale, true);
    };
    if (!title[0].empty()) {   // a band across the screen
        const float y = screen.Y * 0.36f;
        Color(canvas, 8, 10, 18, 185);
        canvas.SetPosition(Vector2F{0.f, y});
        canvas.FillBox(Vector2F{static_cast<float>(screen.X), title[1].empty() ? 120.f : 175.f});
        Color(canvas, 255, 255, 255, 255);
        centered(title[0], 6.f, y + 14.f);
        Color(canvas, 255, 200, 90, 255);
        if (!title[1].empty()) centered(title[1], 2.f, y + 122.f);
    }
    if (!caption[0].empty()) {   // a lower third
        const float big = 2.2f, small = 1.5f;
        const Vector2F a = canvas.GetStringSize(caption[0], big, big);
        const Vector2F b = caption[1].empty() ? Vector2F{0.f, 0.f} : canvas.GetStringSize(caption[1], small, small);
        const float w = std::max(a.X, b.X) + 44.f, h = a.Y + (caption[1].empty() ? 0.f : b.Y + 6.f) + 26.f;
        const float x = 60.f, y = screen.Y - 170.f - h;
        Color(canvas, 8, 10, 18, 200);
        canvas.SetPosition(Vector2F{x, y});
        canvas.FillBox(Vector2F{w, h});
        Color(canvas, 255, 120, 200, 255);   // Replay Lab's pink, as the window's
        canvas.SetPosition(Vector2F{x, y});
        canvas.FillBox(Vector2F{8.f, h});
        Color(canvas, 255, 255, 255, 255);
        canvas.SetPosition(Vector2F{x + 26.f, y + 12.f});
        canvas.DrawString(caption[0], big, big, true);
        if (!caption[1].empty()) {
            Color(canvas, 210, 215, 230, 255);
            canvas.SetPosition(Vector2F{x + 26.f, y + 18.f + a.Y});
            canvas.DrawString(caption[1], small, small, true);
        }
    }
}

// Real player names above the cars, and during a simulation where the changed cars really went.
void ReplayLab::DrawWorld(CanvasWrapper& canvas) {
    CameraWrapper cam = gameWrapper->GetCamera();
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (cam.IsNull() || sw.IsNull()) return;
    const Vector camLoc = cam.GetLocation();
    const Axes camAx = AxesOf(cam.GetRotation());
    auto visible = [&](const Vector& p) { return Dot(Sub(p, camLoc), camAx.f) > 50.f; };
    auto line = [&](const Vector& a, const Vector& b, float width) {
        if (visible(a) && visible(b)) canvas.DrawLine(canvas.Project(a), canvas.Project(b), width);
    };
    auto teamColor = [&](int k) {
        const int team = mirrored ? 1 - replay->teams[k] : replay->teams[k];
        if (team == 0) Color(canvas, 110, 165, 255, 255);
        else Color(canvas, 255, 165, 70, 255);
    };
    auto label = [&](const Vector& at, const std::string& text) {
        const Vector top = Add(at, Vector(0, 0, 90));
        if (!visible(top)) return;
        const Vector2 p = canvas.Project(top);
        canvas.SetPosition(Vector2{p.X - 4 * static_cast<int>(text.size()), p.Y - 14});
        canvas.DrawString(text, 1.f, 1.f, true);
    };

    if (mode != MODE_SIM || !*opt.ghosts) return;

    const float t = forkT + simT;
    // what the ball really did over the next 6 seconds
    Color(canvas, 255, 255, 255, 140);
    State prev = StateAt(-1, forkT);
    for (float dt = 0.1f; dt <= 6.f; dt += 0.1f) {
        const State cur = StateAt(-1, forkT + dt);
        if (prev.valid && cur.valid) line(prev.pos, cur.pos, 2.f);
        prev = cur;
    }
    const State ghostBall = StateAt(-1, t);
    if (!ballLocked && ghostBall.valid && visible(ghostBall.pos)) {
        const Vector2 c = canvas.Project(ghostBall.pos);
        const Vector2 e = canvas.Project(Add(ghostBall.pos, Mul(camAx.r, 92.f)));
        const int r = std::max(4, std::abs(e.X - c.X));
        Color(canvas, 255, 255, 255, 220);
        canvas.SetPosition(Vector2{c.X - r, c.Y - r});
        canvas.DrawBox(Vector2{2 * r, 2 * r});
    }
    // where the changed cars really went (ghost-role cars are already on the pitch for real)
    for (int k = 0; k < replay->Players(); k++) {
        if (RoleOf(k) == ROLE_GHOST) continue;
        const State car = StateAt(k, t);
        if (!car.valid) continue;
        if (roles[k] == ROLE_TOI) Color(canvas, 255, 255, 255, 200);
        else teamColor(k);
        const Axes ax = AxesOf(car.rot);
        const Vector center = Add(car.pos, Add(Mul(ax.f, OFF_F), Mul(ax.u, OFF_U)));
        Vector corners[8];
        for (int i = 0; i < 8; i++)
            corners[i] = Add(center, Add(Mul(ax.f, (i & 1) ? HALF_L : -HALF_L),
                                         Add(Mul(ax.r, (i & 2) ? HALF_W : -HALF_W), Mul(ax.u, (i & 4) ? HALF_H : -HALF_H))));
        for (int i = 0; i < 8; i++)
            for (int bit = 1; bit < 8; bit <<= 1)
                if (!(i & bit)) line(corners[i], corners[i | bit], 2.f);
        label(car.pos, replay->names[k] + " (vrai)");
    }
}
