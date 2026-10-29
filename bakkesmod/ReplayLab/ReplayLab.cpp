// The plugin's entry: its settings and console commands, the game functions it hooks, loading the replay reader.
#include "ReplayLab.h"
#include "LabMath.h"
#include "Draw.h"
#include "Rules.h"
#include "bakkesmod/wrappers/Engine/EnumWrapper.h"
#include "bakkesmod/wrappers/GameObject/CameraSettingsActorWrapper.h"

#include <filesystem>
#include <fstream>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace lab;

BAKKESMOD_PLUGIN(ReplayLab, "Replay Lab - rejouer un replay comme le match et reprendre la main", "4.0", PLUGINTYPE_FREEPLAY | PLUGINTYPE_REPLAY)

void ReplayLab::onLoad() {
    cvarManager->registerCvar("rlab_player", "Anxoide", "Ton pseudo: ta voiture par defaut dans les replays");
    cvarManager->registerCvar("rlab_speed", "1.0", "Vitesse du jeu pendant la simulation (0.5 = ralenti)", true, true, 0.1f, true, 1.f);
    cvarManager->registerCvar("rlab_ghosts", "1", "Dessiner le vrai replay en fil de fer pendant la simulation", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_mirror", "0", "Voiture orange: tourner la scene pour defendre le but du bas", true, true, 0.f, true, 1.f);
    // version 3.3: each car plays in its real team, so the scene no longer needs turning around by default
    CVarWrapper teamsV33 = cvarManager->registerCvar("rlab_teams_v33", "0", "", false);
    if (!teamsV33.getBoolValue()) {
        cvarManager->getCvar("rlab_mirror").setValue(0);
        teamsV33.setValue(1);
    }
    cvarManager->registerCvar("rlab_sync", "1", "Le ballon suit le replay tant qu'aucune voiture changee ne s'approche", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_debug", "0", "Ecrire dans la console ou chaque voiture devrait etre et ou elle est (2: chaque pas)", true, true, 0.f, true, 2.f, false);

    // the same command names as before, so existing key binds keep working
    cvarManager->registerNotifier("rlab_capture", [this](std::vector<std::string>) { TogglePlay(); },
                                  "Lecture / pause du replay (en simulation: retour au replay)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_play", [this](std::vector<std::string>) { OnSimulateKey(); },
                                  "Simuler d'ici", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_retry", [this](std::vector<std::string>) { Retry(); }, "Recommencer la simulation", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_camera", [this](std::vector<std::string>) { NextView(); },
                                  "Regarder depuis la voiture du joueur suivant", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_open", [this](std::vector<std::string> args) {
        if (args.size() < 2) return;
        const int frame = args.size() > 2 ? std::atoi(args[2].c_str()) : 0;
        const bool sim = args.size() > 3 && args[3] == "1";
        OpenReplay(HexId(args[1]), frame, sim, "");
    }, "rlab_open <id du replay> [image] [1 = simuler tout de suite]", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_role", [this](std::vector<std::string> args) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (!replay || args.size() < 3) return;
        const int k = std::atoi(args[1].c_str()) - 1, role = std::atoi(args[2].c_str());
        if (k < 0 || k >= replay->Players() || role < 0 || role >= ROLE_COUNT) return;
        const bool sim = mode == MODE_SIM;
        if (SetRole(k, role)) SetupCars([this, sim] { if (!sim) StartPlayback(rt, playing); }, sim);
        Status(replay->names[k] + ": " + ROLE_LABELS[role] + " (prochaine simulation / F11)");
    }, "rlab_role <joueur 1..N> <0 toi, 1 fantome, 2 retiree, 3 bot Psyonix, 4 libre, 5 Nexto>", PERMISSION_ALL);
    // a title and a caption over the game, kept in cinema mode: to make a video ("first line | second line", nothing: off)
    auto text = [](const std::vector<std::string>& args, std::string out[2]) {
        std::string all;
        for (size_t i = 1; i < args.size(); i++) all += (i > 1 ? " " : "") + args[i];
        all = Ascii(all);
        const size_t bar = all.find(" | ");
        out[0] = all.substr(0, bar);
        out[1] = bar == std::string::npos ? "" : all.substr(bar + 3);
    };
    cvarManager->registerNotifier("rlab_vitesse", [this](std::vector<std::string> args) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (args.size() > 1) playSpeed = std::min(std::max(static_cast<float>(std::atof(args[1].c_str())), 0.1f), 4.f);
    }, "rlab_vitesse <0.25 .. 2>: vitesse de lecture du replay (ralenti)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_hud", [this](std::vector<std::string>) {
        gameWrapper->Execute([](GameWrapper* gw) { gw->ExecuteUnrealCommand("ShowHUD"); });
    }, "rlab_hud: afficher / cacher l'interface du jeu (jauge de boost, aides), pour filmer", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_flash", [this](std::vector<std::string>) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        flashUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    }, "rlab_flash: l'ecran en magenta 0,4 s (repere pour caler le montage d'une video)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_titre", [this, text](std::vector<std::string> args) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        text(args, title);
    }, "rlab_titre <titre | sous-titre>: grand titre a l'ecran (sans texte: l'enlever)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_legende", [this, text](std::vector<std::string> args) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        text(args, caption);
    }, "rlab_legende <texte | detail>: legende en bas de l'ecran (sans texte: l'enlever)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_cam_mode", [this](std::vector<std::string> args) {
        if (args.size() > 1) camMode = (std::atoi(args[1].c_str()) + CAM_COUNT - 1) % CAM_COUNT;   // that one: the one before, then next
        NextCamMode();
    },
                                  "Camera suivante: jeu, joueur, libre, automatique (F7)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_ballcam", [this](std::vector<std::string>) { ToggleBallCam(); },
                                  "Camera ballon (F8)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_revoir", [this](std::vector<std::string> args) {
        WatchBranch(args.size() > 1 ? std::atoi(args[1].c_str()) - 1 : -1);
    }, "rlab_revoir [n]: revoir une branche sauvegardee comme un replay; sans numero la derniere simulation", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_sauver", [this](std::vector<std::string>) { SaveBranch(); },
                                  "Sauvegarder la derniere simulation comme branche (sinon le prochain essai la remplace)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_branche", [this](std::vector<std::string>) { NextBranch(); },
                                  "Branche suivante: le replay d'origine, puis chaque simulation de la derniere a la premiere", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_origine", [this](std::vector<std::string>) { BackToOriginal(); },
                                  "Revenir au replay d'origine", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_cinema_bascule", [this](std::vector<std::string>) {
        CVarWrapper cinema = cvarManager->getCvar("rlab_cinema");
        cinema.setValue(cinema.getBoolValue() ? 0 : 1);
        if (!cinema.getBoolValue()) Status("Mode cinema desactive", 3.f);
    }, "Mode cinema oui/non (rien de Replay Lab a l'ecran, pour filmer)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_ralenti", [this](std::vector<std::string>) {
        // the replay's speed (x1, x0.5, x0.25); in a simulation the game's
        std::lock_guard<std::recursive_mutex> lock(mtx);
        if (mode == MODE_SIM) {
            CVarWrapper speed = cvarManager->getCvar("rlab_speed");
            const float next = speed.getFloatValue() > 0.75f ? 0.5f : speed.getFloatValue() > 0.375f ? 0.25f : 1.f;
            speed.setValue(next);
            cvarManager->executeCommand("sv_soccar_gamespeed " + std::to_string(next), false);
            Status("Vitesse du jeu x" + std::to_string(next).substr(0, 4), 3.f);
            return;
        }
        playSpeed = playSpeed > 0.75f ? 0.5f : playSpeed > 0.375f ? 0.25f : 1.f;
        Status("Lecture x" + std::to_string(playSpeed).substr(0, 4), 3.f);
    }, "Ralenti: x1, x0.5, x0.25 (le replay; en simulation le jeu)", PERMISSION_ALL);
    cvarManager->registerCvar("rlab_gamma", "1", "Couleurs des voitures: 1 telles quelles, 2.2 lineaires", true, true, 0.3f, true, 3.f);
    cvarManager->registerCvar("rlab_attendre", "1", "Depart d'une simulation: 0 tout de suite, 1 quand tu bouges ta manette (comme l'entrainement), "
                              "2 compte a rebours 3-2-1 (toujours au coup d'envoi)", true, true, 0.f, true, 2.f);
    cvarManager->registerCvar("rlab_explosion", "1", "Buts: le ballon explose avec l'explosion de but du buteur et revient au coup d'envoi "
                              "(0: il reste dans le filet)", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_voiture_joueur", "1", "Tu conduis la voiture du joueur (son bot: sa carrosserie, sa hitbox, son apparence; 0: ta propre voiture)",
                              true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_revoir_buts", "1", "Replay: chaque but revu comme le jeu (REPLAY, buteur, km/h), puis le coup d'envoi "
                              "(0: tout de suite le coup d'envoi)", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_revoir_simulation", "1", "Simulation: un but revu comme le jeu juste apres (0: non, pour s'entrainer vite)",
                              true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_cam_joueur", "1", "Vue d'un joueur: ses reglages de camera et sa camera ballon, comme dans le replay", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_noms", "1", "Nom de chaque joueur au-dessus de sa voiture (ceux du jeu; reste en mode cinema)",
                              true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_noms_plugin", "0", "Noms: 1 la copie du plugin a la couleur de l'equipe, avec le role (0: ceux du jeu)",
                              true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_tableau", "1", "Score et horloge en haut de l'ecran, comme en match (reste en mode cinema)", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_cinema", "0", "Mode cinema: aucun texte ni dessin de Replay Lab a l'ecran (pour filmer)", true, true, 0.f, true, 1.f);
    cvarManager->registerNotifier("rlab_photo", [this](std::vector<std::string>) { gameWrapper->ExecuteUnrealCommand("shot"); },
                                  "Capture d'ecran du jeu (dossier Screenshots du jeu)", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_etat", [this](std::vector<std::string>) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        static const char* MODES[] = {"aucun replay", "lecture", "simulation"};
        char buf[220];
        std::snprintf(buf, sizeof buf, "Replay Lab etat: %s, t=%.2f, %s, fenetre=%d, jeu en pause=%d, ta voiture=%s, entrainement libre=%d, plots connus=%d",
                      MODES[mode], replay ? Now() - Start() : 0.f, playing ? "lecture" : "arret", windowOpen ? 1 : 0, gameWrapper->IsPaused() ? 1 : 0,
                      gameWrapper->GetLocalCar().IsNull() ? "absente" : "presente", gameWrapper->IsInFreeplay() ? 1 : 0,
                      static_cast<int>(pads.size()));
        cvarManager->log(buf);
        ServerWrapper srv = gameWrapper->GetGameEventAsServer();
        if (!srv.IsNull()) {   // what is in the game: player infos, cars, controllers
            ArrayWrapper<PriWrapper> pris = srv.GetPRIs();
            ArrayWrapper<CarWrapper> cars = srv.GetCars();
            ArrayWrapper<ControllerWrapper> players = srv.GetPlayers();
            std::string who;
            for (int i = 0; i < pris.Count(); i++)
                if (!pris.Get(i).IsNull())
                    who += " " + pris.Get(i).GetPlayerName().ToString() + (pris.Get(i).GetbBot() ? "" : "(toi)") +
                           (pris.Get(i).GetCar().IsNull() ? "[sans voiture]" : "");
            cvarManager->log("Replay Lab etat: " + std::to_string(pris.Count()) + " joueurs, " + std::to_string(cars.Count()) + " voitures, " +
                             std::to_string(players.Count()) + " controleurs:" + who);
            std::string list = "Replay Lab etat: voitures:";
            for (int i = 0; i < cars.Count(); i++) {
                CarWrapper car = cars.Get(i);
                if (car.IsNull()) continue;
                char one[120];
                std::snprintf(one, sizeof one, " [%s z=%.0f%s%s]", NameOf(car).c_str(), car.GetLocation().Z, car.GetbHidden() ? " cachee" : "",
                              car.GetPRI().IsNull() ? " sans-joueur" : "");
                list += one;
            }
            cvarManager->log(list);
            BallWrapper ball = srv.GetBall();
            char what[260];
            if (!ball.IsNull()) {
                std::snprintf(what, sizeof what, "Replay Lab etat: ballon rayon=%.1f visuel=%.1f taille=%.2f rebond=%.2f gravite=%.2f, %d ballon(s), stade %s",
                              ball.GetRadius(), ball.GetVisualRadius(), ball.GetReplicatedBallScale(), ball.GetReplicatedWorldBounceScale(),
                              ball.GetReplicatedBallGravityScale(), srv.GetGameBalls().Count(), gameWrapper->GetCurrentMap().c_str());
                cvarManager->log(what);
            }
            ArrayWrapper<GoalWrapper> goals = srv.GetGoals();
            for (int i = 0; i < goals.Count(); i++) {
                GoalWrapper g = goals.Get(i);
                if (g.IsNull()) continue;
                const Vector c = g.GetWorldCenter(), e = g.GetWorldExtent(), l = g.GetLocation(), d = g.GetDirection();
                std::snprintf(what, sizeof what, "Replay Lab etat: but equipe %d centre (%.0f,%.0f,%.0f) demi-taille (%.0f,%.0f,%.0f) lieu (%.0f,%.0f,%.0f) dir (%.2f,%.2f,%.2f)",
                              g.GetTeamNum(), c.X, c.Y, c.Z, e.X, e.Y, e.Z, l.X, l.Y, l.Z, d.X, d.Y, d.Z);
                cvarManager->log(what);
            }
        }
        if (mode == MODE_SIM) {
            int sc[2];
            ScoreAt(Now(), sc);
            cvarManager->log("Replay Lab etat: horloge " + SimClockText() + " (" + std::to_string(SimClock()) + " s), match " + std::to_string(match) +
                             ", score " + std::to_string(sc[0]) + "-" + std::to_string(sc[1]));
        }
        cvarManager->log("Replay Lab etat: croix " + std::to_string(padKey[0]) + "/" + std::to_string(padKey[1]) + "/" +
                         std::to_string(padKey[2]) + "/" + std::to_string(padKey[3]) + ", simuler " + padSimulate + "=" +
                         std::to_string(padKey[4]) + ", camera ballon octet " + std::to_string(ballCamByte) +
                         " bit " + std::to_string(ballCamBit) + ", suivie " + (BallCamOn() ? "oui" : "non") + ", ecrite " + std::to_string(ballCamWrites) + " fois");
        std::string binds = "Replay Lab etat: liaisons BakkesMod restantes:";
        for (const char* key : {"F3", "F4", "F5", "F9", "F11"}) binds += std::string(" ") + key + "=[" + cvarManager->getBindStringForKey(key) + "]";
        cvarManager->log(binds);
        CameraWrapper cam = gameWrapper->GetCamera();
        if (!cam.IsNull() && replay) {
            const ViewTarget vt = cam.GetViewTarget();
            ActorWrapper followed(reinterpret_cast<uintptr_t>(vt.Target));
            CarWrapper dolly = Dolly();
            char view[260];
            std::snprintf(view, sizeof view, "Replay Lab etat: %s%s, %s, camera en (%.0f,%.0f,%.0f), a %.0f de ce qu'elle suit (%s)%s",
                          replay->scenario ? "SCENARIO " : "", CamText().c_str(), cam.GetCameraState().c_str(),
                          cam.GetLocation().X, cam.GetLocation().Y, cam.GetLocation().Z, followed.IsNull() ? -1.f : Len(Sub(followed.GetLocation(), cam.GetLocation())),
                          followed.IsNull() ? "rien" : (CarWrapper(followed.memory_address).GetPRI().IsNull() ? "?" : NameOf(CarWrapper(followed.memory_address)).c_str()),
                          dolly.IsNull() ? "" : (std::string(", voiture-camera cachee=") + std::to_string(dolly.GetbHidden()) + " collisions=" +
                                                 std::to_string(dolly.GetbCollideActors())).c_str());
            cvarManager->log(view);
            std::snprintf(view, sizeof view, "Replay Lab etat: mode %s (ballon du replay %s)", KIND_LABELS[GameKind()], replay->ballArch.c_str());
            cvarManager->log(view);
            auto settingsText = [](const ProfileCameraSettings& s) {
                char b[100];
                std::snprintf(b, sizeof b, "fov %.0f h %.0f d %.0f pivot %.1f", s.FOV, s.Height, s.Distance, s.SwivelSpeed);
                return std::string(b);
            };
            CameraSettingsActorWrapper csa = cam.GetCameraSettingsActor();
            std::string more = "Replay Lab etat: fov reel " + std::to_string(cam.GetFOV()) + ", acteur reglages camera: " +
                               (csa.IsNull() ? "aucun" : (csa.GetPri().IsNull() ? "?" : csa.GetPri().GetPlayerName().ToString()) + " " + settingsText(csa.GetProfileSettings()));
            if (!followed.IsNull() && !srv.IsNull() && !srv.GetBall().IsNull()) {
                const Vector toBall = Sub(srv.GetBall().GetLocation(), cam.GetLocation());
                char yaw[160];
                std::snprintf(yaw, sizeof yaw, ", lacet camera %.0f voiture %.0f vers le ballon %.0f",
                              cam.GetRotation().Yaw / 182.044f, followed.GetRotation().Yaw / 182.044f,
                              std::atan2(toBall.Y, toBall.X) * 57.2958f);
                more += yaw;
            }
            if (!followed.IsNull()) {
                PriWrapper fp = CarWrapper(followed.memory_address).GetPRI();
                if (!fp.IsNull())
                    more += ", joueur suivi " + fp.GetPlayerName().ToString() + ": " + settingsText(fp.GetCameraSettings()) + " ballcam " +
                            std::to_string(fp.GetbUsingSecondaryCamera()) + " acteur " + (fp.GetPersistentCamera().IsNull() ? "aucun" : "oui");
            }
            CarWrapper me = gameWrapper->GetLocalCar();
            if (!me.IsNull())
                more += ", ta voiture: sol " + std::to_string(me.GetbOnGround()) + " saute " + std::to_string(me.GetbJumped()) + " double " +
                        std::to_string(me.GetbDoubleJumped()) + " flip " + std::to_string(me.HasFlip());
            if (!me.IsNull() && !me.GetPRI().IsNull())
                more += ", toi: " + settingsText(me.GetPRI().GetCameraSettings()) + " ballcam " + std::to_string(me.GetPRI().GetbUsingSecondaryCamera()) +
                        " acteur " + (me.GetPRI().GetPersistentCamera().IsNull() ? "aucun" : "oui");
            cvarManager->log(more);
        }
    }, "rlab_etat: ecrire l'etat de Replay Lab dans la console", PERMISSION_ALL);
    cvarManager->registerCvar("rlab_manette", "1", "Manette: croix haut lecture/pause, gauche camera, droite joueur, bas recommencer "
                              "(appui long: branche suivante, cinema, ralenti, sauvegarder); Back: simuler d'ici (long: statut)", true, true, 0.f, true, 1.f);
    cvarManager->registerCvar("rlab_manette_simuler", "XboxTypeS_Back",
                              "Bouton de manette qui lance la simulation (nom de touche du jeu, ex. XboxTypeS_LeftThumbstick)");
    cvarManager->registerNotifier("rlab_statut", [this](std::vector<std::string>) { NextRole(); },
                                  "Statut suivant (fantome, toi, bot...) du joueur que la camera montre", PERMISSION_ALL);
    cvarManager->registerNotifier("rlab_demolir", [this](std::vector<std::string> args) {
        if (args.size() >= 2) DemolishPlayer(std::atoi(args[1].c_str()) - 1);
    }, "rlab_demolir <joueur 1..N>: pendant une simulation, demolir ce joueur maintenant", PERMISSION_ALL);

    BindSettings();

    // the plugin reads its keys itself (PollKeys): BakkesMod binds to its commands would fire them twice
    for (const char* key : {"F3", "F4", "F5", "F7", "F8", "F9", "F10", "F11"}) {
        const std::string bound = cvarManager->getBindStringForKey(key);
        if (bound.find("rlab_") != std::string::npos || bound.find("togglemenu replaylab") != std::string::npos)
            cvarManager->removeBind(key);
    }

    // freeplay goals are switched off while the ball follows the replay; keep the player's settings to restore them
    CVarWrapper goal = cvarManager->getCvar("sv_freeplay_enablegoal");
    CVarWrapper goalBm = cvarManager->getCvar("sv_freeplay_enablegoal_bakkesmod");
    origGoal = goal.IsNull() ? "" : goal.getStringValue();
    origGoalBm = goalBm.IsNull() ? "" : goalBm.getStringValue();

    // cars driven by the replay overlap where real players touched: no demolitions while the replay plays,
    // the normal rule (the other team) in a simulation
    EnumWrapper targets = EnumWrapper::GetDemolishTargets();
    if (!targets.IsNull()) {
        std::string known;
        for (const auto& [value, label] : targets.GetPossibleValues()) {
            known += " " + label + "=" + std::to_string(value);
            std::string lower = label;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("none") != std::string::npos) noDemo = value;
            if (lower.find("otherteam") != std::string::npos) normalDemo = value;
        }
        cvarManager->log("Replay Lab: demolitions" + known + " -> replay " + std::to_string(noDemo) + ", simulation " +
                         std::to_string(normalDemo));
    }

    gameWrapper->HookEvent("Function TAGame.GameEvent_TA.SpawnCar", [this](std::string) { OnSpawnCar(); });
    gameWrapper->HookEvent("Function TAGame.GameEvent_TA.PlayerResetTraining", [this](std::string) { OnGameReset(); });
    gameWrapper->HookEvent("Function Engine.GameViewportClient.Tick", [this](std::string) {
        PollKeys();
        TakeOpened();
        OnFrame();
    });
    gameWrapper->HookEventWithCaller<CarWrapper>("Function TAGame.Car_TA.SetVehicleInput",
        [this](CarWrapper car, void* params, std::string) { OnCarInput(car, params); });
    for (const char* demolished : {"Function TAGame.Car_TA.Demolish", "Function TAGame.Car_TA.EventDemolished"})
        gameWrapper->HookEventWithCaller<CarWrapper>(demolished, [this](CarWrapper car, void*, std::string) { OnDemolished(car); });
    gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) { DrawHud(canvas); });
    // the game's camera follows the car of the camera mode: its target, changed just before it works out its view
    gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.CameraState_Car_TA.UpdatePOV",
        [this](ActorWrapper, void*, std::string) { AimCamera(); });

    std::string nextoError;
    if (!nextoModel.Load(std::filesystem::path(gameWrapper->GetBakkesModPathW()) / L"plugins" / L"replaylab" / L"nexto.bin", nextoError))
        cvarManager->log("Replay Lab: Nexto indisponible (" + nextoError + "), ses voitures seront des bots Psyonix");
    gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.VehiclePickup_TA.OnPickUp",
        [this](ActorWrapper pad, void*, std::string) { OnPadPickedUp(pad); });
    gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.VehiclePickup_TA.Respawn",   // pads put back: each of them
        [this](ActorWrapper pad, void*, std::string) {
            std::lock_guard<std::recursive_mutex> lock(mtx);
            NotePad(pad);
            RecordPad(pad, false);
        });
    gameWrapper->HookEventWithCaller<CarWrapper>("Function TAGame.Car_TA.OnHitBall",
        [this](CarWrapper car, void*, std::string) { OnHitBall(car); });
    if (LoadReader()) {
        RefreshList();
        cvarManager->log("Replay Lab 4.0 charge: F3 fenetre, F4 vue, F9 lecture/pause, F5/F10 simuler d'ici, F11 recommencer; manette: "
                         "croix haut pause, gauche camera, droite joueur, bas recommencer (appui long: branche suivante, cinema, ralenti, "
                         "sauvegarder), Back simuler d'ici (long: statut), reset recommencer");
    }
}

void ReplayLab::BindSettings() {
    cvarManager->getCvar("rlab_manette").bindTo(opt.manette);
    cvarManager->getCvar("rlab_tableau").bindTo(opt.tableau);
    cvarManager->getCvar("rlab_cinema").bindTo(opt.cinema);
    cvarManager->getCvar("rlab_cam_joueur").bindTo(opt.camJoueur);
    cvarManager->getCvar("rlab_sync").bindTo(opt.sync);
    cvarManager->getCvar("rlab_noms").bindTo(opt.noms);
    cvarManager->getCvar("rlab_noms_plugin").bindTo(opt.nomsPlugin);
    cvarManager->getCvar("rlab_mirror").bindTo(opt.mirror);
    cvarManager->getCvar("rlab_ghosts").bindTo(opt.ghosts);
    cvarManager->getCvar("rlab_explosion").bindTo(opt.explosion);
    cvarManager->getCvar("rlab_voiture_joueur").bindTo(opt.voitureJoueur);
    cvarManager->getCvar("rlab_revoir_simulation").bindTo(opt.revoirSimulation);
    cvarManager->getCvar("rlab_revoir_buts").bindTo(opt.revoirButs);
    cvarManager->getCvar("rlab_attendre").bindTo(opt.attendre);
    cvarManager->getCvar("rlab_debug").bindTo(opt.debug);
    cvarManager->getCvar("rlab_speed").bindTo(opt.speed);
    cvarManager->getCvar("rlab_gamma").bindTo(opt.gamma);
    cvarManager->getCvar("rlab_player").bindTo(opt.player);
    cvarManager->getCvar("rlab_manette_simuler").bindTo(opt.manetteSimuler);
}

void ReplayLab::onUnload() {
    for (std::thread* worker : {&listWorker, &openWorker})   // not reading anything any more
        if (worker->joinable()) worker->join();
    gameWrapper->UnhookEvent("Function Engine.GameViewportClient.Tick");
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.SetVehicleInput");
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.Demolish");
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.EventDemolished");
    gameWrapper->UnhookEvent("Function TAGame.VehiclePickup_TA.OnPickUp");
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.OnHitBall");
    gameWrapper->UnhookEvent("Function TAGame.VehiclePickup_TA.Respawn");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_TA.SpawnCar");
    gameWrapper->UnhookEvent("Function TAGame.GameEvent_TA.PlayerResetTraining");
    if (gameWrapper->IsInFreeplay()) {   // the ball and the nets as freeplay had them
        BallBack(gameWrapper->GetGameEventAsServer());
        GoalTeamBack(gameWrapper->GetGameEventAsServer());
        PadDelaysBack();
    }   // not left hidden by a match's end
    gameWrapper->UnhookEvent("Function TAGame.CameraState_Car_TA.UpdatePOV");
    if (reader) {   // the next Replay Lab loads the installed reader (no replay is being read: joined above)
        FreeLibrary(static_cast<HMODULE>(reader));
        reader = nullptr;
    }
    if (!origGoalBm.empty()) cvarManager->executeCommand("sv_freeplay_enablegoal_bakkesmod " + origGoalBm, false);
    if (!origGoal.empty()) cvarManager->executeCommand("sv_freeplay_enablegoal " + origGoal, false);
}

void ReplayLab::Status(const std::string& text, float seconds) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    status = text;
    statusUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    cvarManager->log(text);
}

bool ReplayLab::LoadReader() {
    const std::filesystem::path dll = std::filesystem::path(gameWrapper->GetBakkesModPathW()) / L"plugins" / L"replaylab" / L"rlab_replay.dll";
    // a copy an earlier Replay Lab left loaded would be handed back instead of the installed one
    for (int i = 0; i < 8; i++) {
        HMODULE old = GetModuleHandleW(dll.wstring().c_str());
        if (!old) break;
        FreeLibrary(old);
    }
    HMODULE h = LoadLibraryW(dll.wstring().c_str());
    if (h) {
        parseReplay = reinterpret_cast<ParseFn>(GetProcAddress(h, "rlab_parse_replay"));
        replayHeader = reinterpret_cast<ParseFn>(GetProcAddress(h, "rlab_replay_header"));
        freeBlob = reinterpret_cast<FreeFn>(GetProcAddress(h, "rlab_free"));
        reader = h;
    }
    if (!parseReplay || !replayHeader || !freeBlob) {
        Status("Replay Lab: rlab_replay.dll introuvable dans plugins/replaylab (relance install_plugin.sh).", 30.f);
        return false;
    }
    return true;
}

std::wstring ReplayLab::DemosFolder() const {
    wchar_t buf[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Documents\\My Games\\Rocket League\\TAGame\\Demos";
}
