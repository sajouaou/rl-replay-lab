// The freeplay scene: the replay's stadium in its mode, one bot per replay player dressed as him (body, paint, goal
// explosion...), your car, the cars set aside under the field. Bots are reused: freeplay crashes when one is removed.
#include "ReplayLab.h"
#include "LabMath.h"
#include "Rules.h"
#include "Draw.h"
#include "bakkesmod/core/bot_loadout_structs.h"
#include "Palettes.h"

using namespace lab;

int ReplayLab::GameKind() const { return replay ? KindOfBall(replay->ballArch) : KIND_SOCCAR; }

// the stadium the game is in is the replay's (same map file: day, dusk, rain... are different ones), in its mode
bool ReplayLab::InReplayStadium() const {
    auto lower = [](std::string x) {
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return x;
    };
    if (!replay || lower(gameWrapper->GetCurrentMap()) != lower(replay->map)) return false;
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    BallWrapper ball = sw.IsNull() ? BallWrapper(0) : sw.GetBall();
    return ball.IsNull() || KindOfRadius(ball.GetRadius()) == GameKind();
}

// The replay's stadium in freeplay. Bots cannot be taken out of a freeplay (that crashed the game): the ones of other
// replays are reused, and once too many have piled up a new freeplay takes them all away.
void ReplayLab::EnterFreeplay(std::function<void()> then) {
    int bots = 0;
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (!sw.IsNull()) {
        ArrayWrapper<PriWrapper> pris = sw.GetPRIs();
        for (int i = 0; i < pris.Count(); i++)
            if (!pris.Get(i).IsNull() && pris.Get(i).GetbBot()) bots++;
    }
    if (gameWrapper->IsInFreeplay() && !gameWrapper->GetLocalCar().IsNull() && InReplayStadium() &&
        bots <= replay->Players() + MAX_SPARE_BOTS) {
        then();
        return;
    }
    if (bots > replay->Players() + MAX_SPARE_BOTS) cvarManager->log("Replay Lab: " + std::to_string(bots) + " bots: nouvel entrainement libre");
    ballGone = ballBlown = ballRenew = false;   // a new ball comes with the new freeplay
    if (gameWrapper->IsInOnlineGame() && !gameWrapper->IsInReplay()) {
        Status("Pas pendant une partie en ligne.");
        return;
    }
    // the freeplay of the replay's stadium: the bots and cars of the old one go with it
    carsReady = false;
    botSlot.clear();
    dressed.clear();
    paintedAt.clear();
    lastValid.clear();
    dollyAsked = false;
    localStashed = false;
    cvarManager->log("Replay Lab: stade actuel " + gameWrapper->GetCurrentMap() + ", celui du replay " + replay->map + " (" +
                     KIND_LABELS[GameKind()] + ", " + replay->ballArch + ")");
    Status("Chargement du stade du replay (" + replay->map + ", " + KIND_LABELS[GameKind()] + ")...", 20.f);
    gameWrapper->ExecuteUnrealCommand("start " + replay->map + "?Game=" + KIND_CLASS[GameKind()] + "?GameTags=Freeplay");
    WaitForFreeplay(0, then);
}

void ReplayLab::WaitForFreeplay(int tries, std::function<void()> then) {
    gameWrapper->SetTimeout([this, tries, then](GameWrapper* gw) {
        // in the new stadium (not still the old one; a map named otherwise is accepted after 20 s)
        if (gw->IsInFreeplay() && !gw->GetLocalCar().IsNull() && (InReplayStadium() || tries > 40)) {
            gw->SetTimeout([then](GameWrapper*) { then(); }, 1.f);   // let the spawn settle
            return;
        }
        if (tries == 30 && replay)   // nothing after 15 s: asked again (a default freeplay would be the wrong stadium)
            gw->ExecuteUnrealCommand("start " + replay->map + "?Game=" + KIND_CLASS[GameKind()] + "?GameTags=Freeplay");
        if (tries == 30) Status("Lance l'entrainement libre toi-meme: Replay Lab continuera tout seul.", 60.f);
        if (tries < 240) WaitForFreeplay(tries + 1, then);
    }, 0.5f);
}

int ReplayLab::ToiIndex() const {
    for (size_t k = 0; k < roles.size(); k++)
        if (roles[k] == ROLE_TOI) return static_cast<int>(k);
    return -1;
}

// You drive the player's own car, his bot (his body and hitbox, his look): your controller's input goes to it and your
// car waits out of the way, as when you watch. The game builds your car from your own preset every time, so it cannot
// take his look. With rlab_voiture_joueur 0 your own car stands for the player you drive instead ("Toi").
int ReplayLab::CarPlayer() const { return ownCar ? ToiIndex() : -1; }

bool ReplayLab::Driving() const { return mode == MODE_SIM && !ownCar && ToiIndex() >= 0; }

// the scene turned around (rlab_mirror): only for an orange player you drive
bool ReplayLab::Turned() const {
    return *opt.mirror && ToiIndex() >= 0 && replay->teams[ToiIndex()] == 1;
}

int ReplayLab::BotIndex(CarWrapper car) const {
    auto it = botSlot.find(car.memory_address);
    return it == botSlot.end() ? -1 : it->second;
}

// Everyone but your own car's player has a bot; while simulating, removed players have none.
bool ReplayLab::NeedsBot(int k, bool forSim) const {
    return k != CarPlayer() && !(forSim && roles[k] == ROLE_REMOVED);
}

// Spawn the bots that are missing, take away the ones that must not be there, then run `then`.
void ReplayLab::SetupCars(std::function<void()> then, bool forSim) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (!replay || sw.IsNull() || !gameWrapper->IsInFreeplay()) return;
    ownCar = !*opt.voitureJoueur;   // settled here: which cars need a bot depends on it
    // Bots are never taken out of the game (that leaves controllers behind that crash the next SpawnBot). A bot keeps the
    // body it was made with: each player gets a bot of his body, with his name (the others, LabR..., wait under the field),
    // and a bot that wore another look gets this one when it comes back from a demolition (the game paints a car only when
    // it builds it). Only when no bot has the body is one made, dressed while the game builds it (OnSpawnCar).
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    std::vector<CarWrapper> labs;
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (!car.GetbHidden() && OurBot(car)) labs.push_back(car);
    }
    auto taken = [&](const std::string& name) {
        return std::any_of(labs.begin(), labs.end(), [&](CarWrapper c) { return NameOf(c) == name; });
    };
    auto rename = [&](CarWrapper car, const std::string& name) {
        dressed.erase(NameOf(car));
        car.GetPRI().eventSetPlayerName(name);
    };
    std::vector<bool> served(replay->Players(), false);
    std::vector<bool> used(labs.size(), false);
    // first the bots that already have the right name and body
    for (size_t i = 0; i < labs.size(); i++) {
        for (int k = 0; k < replay->Players(); k++) {
            if (served[k] || NameOf(labs[i]) != BotName(k) || labs[i].GetLoadoutBody() != replay->looks[k].Body()) continue;
            served[k] = used[i] = true;
            break;
        }
    }
    // then any bot of ours with the right body, renamed; a bot in the way of a name is set aside
    int spareNo = 0;
    for (int k = 0; k < replay->Players(); k++) {
        if (served[k] || !NeedsBot(k, forSim)) continue;
        for (size_t i = 0; i < labs.size(); i++) {
            if (used[i] || labs[i].GetLoadoutBody() != replay->looks[k].Body()) continue;
            for (size_t j = 0; j < labs.size(); j++)
                if (!used[j] && j != i && NameOf(labs[j]) == BotName(k)) {
                    std::string spare;
                    do spare = "LabR" + std::to_string(++spareNo);
                    while (taken(spare));
                    rename(labs[j], spare);
                }
            rename(labs[i], BotName(k));
            served[k] = used[i] = true;
            break;
        }
    }
    // the bots kept that wore another look: this one, from their next car
    for (size_t i = 0; i < labs.size(); i++) {
        if (!used[i]) continue;
        for (int k = 0; k < replay->Players(); k++)
            if (NameOf(labs[i]) == BotName(k) && dressed[BotName(k)] != replay->id + "/" + std::to_string(k)) Redress(labs[i], k);
    }
    sw.SetbRandomizedBotLoadouts(0);   // our bots wear the players' looks, not random ones
    std::vector<int> missing;
    for (int k = 0; k < replay->Players(); k++)
        if (NeedsBot(k, forSim) && !served[k]) missing.push_back(k);
    if (!missing.empty()) Status("Ajout de " + std::to_string(missing.size()) + " voiture(s)...", 8.f);
    // cars blown up to take their look are away for 3 s: a simulation starting without them would take them for
    // demolished (and play them as bots)
    auto ready = [this, then, forSim] {
        const float wait = std::chrono::duration<float>(redressedUntil - std::chrono::steady_clock::now()).count();
        if (wait <= 0.f) {
            then();
            return;
        }
        gameWrapper->SetTimeout([this, then, forSim](GameWrapper*) {
            std::lock_guard<std::recursive_mutex> lock(mtx);
            if (!replay) return;
            MapBots(forSim);
            then();
        }, wait);
    };
    SpawnBots(missing, 0, ready, forSim);
}

// The look and team of player k on a bot's player info, then its car blown up: it comes back built with them (3 s).
void ReplayLab::Redress(CarWrapper car, int k) {
    PriWrapper pri = car.GetPRI();
    if (pri.IsNull()) return;
    Dress(pri, k);
    car.Demolish();
    redressedUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(3400);
    Status("Les voitures prennent l'apparence des joueurs (3 s)...", 4.f);
}

// a bot's player info takes the look of player k (as the game builds its next car from it) and his team
void ReplayLab::Dress(PriWrapper pri, int k) {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay) return;
    const Look& look = replay->looks[k];
    if (look.known) {
        BotLoadoutData data = LoadoutOf(k);
        gameWrapper->SetBotLoadout(pri, data);
    }
    // his team too: a car moved to another team afterwards takes the default colours
    ArrayWrapper<TeamWrapper> teams = sw.GetTeams();
    if (GameTeam(k) < teams.Count() && !teams.Get(GameTeam(k)).IsNull()) pri.SetPlayerTeam(teams.Get(GameTeam(k)));
    dressed[BotName(k)] = replay->id + "/" + std::to_string(k);
    cvarManager->log("Replay Lab: bot a l'image de " + replay->names[k] + " (couleurs " + std::to_string(look.teamColor) + "/" +
                     std::to_string(look.accentColor) + ", " + std::to_string(look.slots.size()) + " objets, equipe " + std::to_string(GameTeam(k)) +
                     ")");
}

BotLoadoutData ReplayLab::LoadoutOf(int k) const {
    const Look& look = replay->looks[k];
    BotLoadoutData data;
    for (const auto& [slot, product, paint] : look.slots) {
        data.products[slot] = product;
        if (paint >= 0) data.product_attributes[slot].push_back({BotLoadoutData::Attribute::Type::PAINT, paint});
    }
    if (look.teamFinish > 0) data.products[7] = look.teamFinish;      // paint finish
    if (look.accentFinish > 0) data.products[12] = look.accentFinish; // accent finish
    data.team = static_cast<unsigned char>(GameTeam(k));
    data.team_color_id = static_cast<unsigned char>(look.teamColor);
    data.custom_color_id = static_cast<unsigned char>(look.accentColor);
    data.team_finish_id = look.teamFinish;
    data.custom_finish_id = look.accentFinish;
    return data;
}

// One bot at a time, each waited for: several SpawnBot in the same frame lose every other bot.
void ReplayLab::SpawnBots(std::vector<int> missing, int tries, std::function<void()> then, bool forSim) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay) return;
    auto there = [&](const std::string& name) {   // a bot's car (yours may have the same name: you played in the replay)
        ArrayWrapper<CarWrapper> cars = sw.GetCars();
        for (int i = 0; i < cars.Count(); i++)
            if (!cars.Get(i).GetbHidden() && OurBot(cars.Get(i)) && NameOf(cars.Get(i)) == name) return true;
        return false;
    };
    auto exists = [&](const std::string& name) {   // its player info, car or not (coming back from a demolition)
        ArrayWrapper<PriWrapper> pris = sw.GetPRIs();
        for (int i = 0; i < pris.Count(); i++)
            if (!pris.Get(i).IsNull() && pris.Get(i).GetbBot() && pris.Get(i).GetPlayerName().ToString() == name) return true;
        return false;
    };
    while (!missing.empty() && there(BotName(missing.front()))) {
        missing.erase(missing.begin());
        tries = 0;
    }
    if (missing.empty()) {
        MapBots(forSim);
        then();
        return;
    }
    const int k = missing.front();
    if (tries == 40) {   // 4 s without it: on to the next
        Status(replay->names[k] + " n'a pas pu etre ajoute.", 10.f);
        missing.erase(missing.begin());
        tries = 0;
    } else if (tries % 10 == 0 && !exists(BotName(k))) {   // asked, then again every second until it is there
        spawning = k;   // OnSpawnCar dresses it
        sw.SpawnBot(replay->looks[k].Body(), BotName(k));
        spawning = -1;
    }
    gameWrapper->SetTimeout([this, missing, tries, then, forSim](GameWrapper*) { SpawnBots(missing, tries + 1, then, forSim); }, 0.1f);
}

// Give each needed player one of our bots; every other one (a removed player, the player your
// own car stands for, a duplicate from a late spawn, a leftover from another replay) is set aside.
// Bots are never taken out of the game: RemovePlayer crashes BakkesMod in freeplay.
void ReplayLab::MapBots(bool forSim) {
    botSlot.clear();
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay) return;
    std::vector<bool> taken(replay->Players(), false);
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        if (car.IsNull()) continue;
        if (!OurBot(car)) continue;
        const std::string name = NameOf(car);
        if (car.GetbHidden() || car.GetbDeleteMe()) {   // demolished: its player comes back in a new car
            botSlot[car.memory_address] = DEAD;
            continue;
        }
        int slot = PARKED;
        for (int k = 0; k < replay->Players(); k++)
            if (name == BotName(k) && NeedsBot(k, forSim) && !taken[k]) { slot = k; taken[k] = true; }
        botSlot[car.memory_address] = slot;
    }
    for (auto it = paintedAt.begin(); it != paintedAt.end();)   // cars gone (each demolition or reset makes new ones)
        it = botSlot.count(it->first) ? std::next(it) : paintedAt.erase(it);
    carsReady = true;
    DressCars();
}

// Plain characters only (the game may change a name with others, and the bot would no longer be found by it); a
// second player of the same name gets a number: each bot has its own name.
std::string ReplayLab::BotName(int k) const {
    auto plain = [](const std::string& name) {
        std::string out;
        for (unsigned char c : name)
            if (c >= 32 && c < 127 && out.size() < 24) out += static_cast<char>(c);
        return out;
    };
    std::string name = plain(replay->names[k]);
    if (name.empty() || name == "LabCam" || name.compare(0, 4, "LabR") == 0) name = "Joueur " + std::to_string(k + 1);
    int same = 0;
    for (int j = 0; j < k; j++)
        if (plain(replay->names[j]) == plain(replay->names[k])) same++;
    return same ? name + " (" + std::to_string(same + 1) + ")" : name;
}

bool ReplayLab::OurBot(CarWrapper car) const {
    return !car.IsNull() && !car.GetPRI().IsNull() && car.GetPRI().GetbBot() && NameOf(car) != "LabCam";
}

int ReplayLab::GameTeam(int k) const {
    return replay->teams[k] ^ (Turned() ? 1 : 0);
}

// Each bot wears the look of the player it plays (body, decal, wheels, boost, antenna, topper, paints, team colours)
// and joins his team; your own car (rlab_voiture_joueur 0) joins the team of the player it stands for.
void ReplayLab::DressCars() {
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull() || !replay) return;
    ArrayWrapper<TeamWrapper> teams = sw.GetTeams();
    // a player joins a team: its info first, then the car takes the team's colours
    auto join = [&](CarWrapper car, PriWrapper pri, int team) {
        if (pri.IsNull() || team >= teams.Count() || teams.Get(team).IsNull()) return;
        if (!car.IsNull() && car.GetTeamNum2() == team) return;
        const std::string who = pri.GetPlayerName().ToString();
        pri.SetPlayerTeam(teams.Get(team));
        if (!car.IsNull()) {
            car.HandleTeamChanged(pri);
            car.InitTeamPaint();
        }
        cvarManager->log("Replay Lab: " + who + " passe dans l'equipe " + std::to_string(team));
    };
    PlayerControllerWrapper pc = gameWrapper->GetPlayerController();
    if (!pc.IsNull() && CarPlayer() >= 0) join(gameWrapper->GetLocalCar(), pc.GetPRI(), GameTeam(CarPlayer()));
    ArrayWrapper<CarWrapper> cars = sw.GetCars();
    for (int i = 0; i < cars.Count(); i++) {
        CarWrapper car = cars.Get(i);
        const int k = car.IsNull() || car.GetbHidden() ? -1 : BotIndex(car);
        if (k >= 0) join(car, car.GetPRI(), GameTeam(k));
    }
}

// Inside SpawnBot the game makes the bot's player info, then (GameEvent_TA.SpawnCar) its car, and paints the car from
// the bot's look (HandleLoadoutSelected, InitTeamPaint, SetLoadout). Just before the car: the bot we are making gets the
// look of its player and his team (a car moved to another team afterwards takes the default colours). It has no name of
// ours yet: it is the bot without a car.
void ReplayLab::OnSpawnCar() {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (spawning < 0 || !replay) return;
    ServerWrapper sw = gameWrapper->GetGameEventAsServer();
    if (sw.IsNull()) return;
    ArrayWrapper<PriWrapper> pris = sw.GetPRIs();
    PriWrapper pri(0);
    for (int i = 0; i < pris.Count(); i++)
        if (!pris.Get(i).IsNull() && pris.Get(i).GetbBot() && pris.Get(i).GetCar().IsNull()) pri = pris.Get(i);   // the newest
    if (pri.IsNull()) return;
    Dress(pri, spawning);
    spawning = -1;   // one car per bot
}

// The game paints our bots white (their colours do not come through SetBotLoadout): each gets its player's team colour
// and accent colour from the game's palettes.
void ReplayLab::PaintCar(CarWrapper car, int k) {
    const Look& look = replay->looks[k];
    if (!look.known || look.teamColor >= 70 || look.accentColor >= 105) return;
    if (GameTeam(k) == 1) {
        // SetCarColor does nothing on orange cars: the game paints them itself from their player info, once the car is there
        PriWrapper pri = car.GetPRI();
        if (pri.IsNull()) return;
        Dress(pri, k);
        car.InitTeamPaint();
        return;
    }
    const unsigned char* main = palette::BLUE[look.teamColor];
    const float gamma = *opt.gamma;
    car.SetCarColor(Paint(main, gamma), Paint(palette::ACCENT[look.accentColor], gamma));
}

// stays where it is (a player demolished at that moment of the replay)
void ReplayLab::Freeze(CarWrapper car) {
    car.SetVelocity(Vector(0, 0, 0));
    car.SetAngularVelocity(Vector(0, 0, 0), false);
}

// While you watch, your car is hidden where it is and goes through everything (under the field the game would take it
// away); when you drive it again it is back.
void ReplayLab::Stash(CarWrapper car, bool away) {
    if (away) {
        if (!car.GetbHidden()) car.SetHidden2(1);
        if (car.GetbCollideActors()) car.SetbCollideActors(0);
        if (car.GetbBlockActors()) car.SetbBlockActors(0);
        car.SetLocation(Vector(0.f, 0.f, 1900.f));   // high in the arena, out of the way
        Freeze(car);
        localStashed = true;
    } else if (localStashed) {
        car.SetHidden2(0);
        car.SetbCollideActors(1);
        car.SetbBlockActors(1);
        localStashed = false;
    }
}

// a bot nobody plays right now waits under the field, out of the way of the ball and the other cars
void ReplayLab::Park(CarWrapper car) {
    car.SetLocation(Vector(0.f, 0.f, PARK_Z));
    car.SetRotation(Rotator{0, 0, 0});
    Freeze(car);
}

void ReplayLab::SetGoals(bool enabled) {
    if (goalsOn == enabled) return;
    goalsOn = enabled;
    cvarManager->executeCommand("sv_freeplay_enablegoal_bakkesmod 1", false);
    cvarManager->executeCommand(std::string("sv_freeplay_enablegoal ") + (enabled ? "1" : "0"), false);
}

// While the replay plays: no demolitions (cars following it overlap where real players bumped), boost set by the replay.
// In a simulation it is a real match: demolitions, and a car that drives itself has limited boost (freeplay's is unlimited).
void ReplayLab::CarRules(CarWrapper car, bool drivesItself) {
    const int target = mode == MODE_SIM ? normalDemo : noDemo;
    if (target >= 0 && car.GetDemolishTarget() != target) car.SetDemolishTarget(static_cast<unsigned char>(target));
    BoostWrapper boost = car.GetBoostComponent();
    if (boost.IsNull()) return;
    const int unlimited = boost.GetUnlimitedBoostRefCount();
    if (mode == MODE_SIM && drivesItself) {
        if (unlimited > 0) {
            freeplayBoost = unlimited;
            boost.SetUnlimitedBoostRefCount(0);
        }
    } else if (unlimited == 0 && freeplayBoost > 0) {
        boost.SetUnlimitedBoostRefCount(freeplayBoost);
    }
}
