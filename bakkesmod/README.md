# Plugin BakkesMod « Replay Lab » (v4.0, autonome)

Rejoue tes replays **dans l'entraînement libre, comme le vrai match** (coups d'envoi, buts qui explosent,
replays des buts, horloge, score), et reprends la main à n'importe quel moment avec la vraie physique du jeu.
Chaque simulation devient une **branche** que tu peux revoir, filmer et resimuler. Aucun site ni serveur : le
plugin lit lui-même les fichiers `.replay` de ton dossier `Demos`.

Pour comprendre le projet (concepts, architecture, format des données) : `docs/` à la racine du dépôt.

## Lancer

1. Rocket League depuis Steam, **sans Easy Anti-Cheat**.
2. Au menu : `./lancer_bakkesmod.sh`.
3. Après une mise à jour du plugin, dans la console BakkesMod (F6) :
   `plugin unload replaylab` puis `plugin load replaylab` (ou relance le jeu).

## Regarder un replay comme le vrai match

- **F3** ouvre la fenêtre Replay Lab : choisis un replay (le nom que tu lui as donné dans le jeu s'affiche en
  premier, entre guillemets), « Ouvrir ». L'entraînement libre se charge **dans le stade et le mode du replay**,
  chaque joueur devient une voiture et le replay se rejoue dedans. Le mode est reconnu au ballon enregistré :
  Hoops (ballon de basket, buts dans le panier), Snow Day (palet), Dropshot (son ballon), sinon soccar.
  Rumble, Heatseeker et les ballons de mutateurs se jouent comme du soccar.
- Comme dans le jeu :
  - **Coup d'envoi** : les voitures sont à leur place, le **compte à rebours du jeu lui-même** (3, 2, 1, GO !,
    avec ses sons) au moment exact du replay.
  - **But** : le ballon **explose avec l'explosion de but du joueur qui a marqué** (son bot porte son
    explosion), un bandeau aux couleurs de l'équipe dit **qui a marqué, la passe décisive et la vitesse du
    ballon (km/h)**, les voitures jouent encore 3 secondes, puis le **replay du but** (bandes noires, « REPLAY » en bas à
    gauche, buteur, passe et km/h en bas, caméra sur le buteur, caméra ballon) et l'écran passe au noir pour le coup d'envoi suivant. Le fichier ne contient pas le
    replay que le jeu montrait : le plugin le refait avec les vraies images. F9 (ou A, ou croix haut) le passe.
    Option « Replay : chaque but revu » décochée : directement le coup d'envoi.
  - **Démolition** : la voiture explose au moment exact du replay et revient quand elle revient dans le replay.
  - **Score et horloge** en haut de l'écran, ceux du replay (même un match rejoint en cours de route, qui
    commence par exemple à 3-0) ; prolongation affichée « +0:12 ». À la fin du replay, « FIN DU MATCH ».
  - Les **roues** tournent et braquent comme celles du joueur (son volant et son accélérateur sont dans le
    replay), les flammes du boost et les dérapages aussi.
- Barre de temps : lecture/pause, ±5 s, vitesse ×0,25 à ×2, curseur (barres bleues/orange = buts,
  traits blancs = arrêts). À la manette : accélérer = avance rapide, freiner = retour rapide.
- Par défaut tu es **spectateur** : chaque joueur rejoue dans sa vraie voiture (sa carrosserie, son
  apparence, son équipe, son nom) et ta voiture attend hors du terrain ; **F4** fait suivre le joueur suivant
  par la caméra. Pour conduire, mets « Toi » sur un joueur (fenêtre F3) : en simulation tu conduis **sa
  voiture à lui** (son bot : sa carrosserie et sa hitbox, son apparence, son explosion de but) avec ta
  manette, la caméra derrière elle (ton bouton caméra balle, ou F8, l'active ou la coupe comme en match) ; ta
  propre voiture attend hors du terrain. Le compteur de boost du jeu montre le sien, et sa caméra prend tes réglages. `rlab_voiture_joueur 0` : l'ancien mode, ta propre
  voiture prend sa place (ta carrosserie, ton apparence).

## Simuler : reprendre la main

- **F5 (ou F10, ou Back à la manette) « Simuler d'ici »** : à l'instant même, chaque voiture prend son rôle et la
  vraie physique prend le relais. Rôles (fenêtre F3, ou console `rlab_role <joueur 1..N> <0 toi, 1 fantôme,
  2 retirée, 3 bot, 4 libre, 5 Nexto>`) : Toi (tu conduis), Fantôme (refait le vrai trajet et peut
  toucher le ballon), Retirée, Bot Psyonix, Libre (sans pilote), Nexto. Le ballon suit le replay tant
  qu'aucune voiture changée ne le touche (ni toi, ni un bot, ni dans le replay le joueur que tu remplaces).
- **Départ** (option « Départ d'une simulation », `rlab_attendre`) : tout de suite (0), quand tu bouges ta
  manette comme l'entraînement personnalisé (1, par défaut), ou après un compte à rebours 3-2-1 (2).
- **Coup d'envoi** : simuler pendant un coup d'envoi ou après un but démarre au coup d'envoi suivant, **avec son
  compte à rebours 3, 2, 1, GO !** : tout le monde est à sa place et personne ne bouge avant GO, comme en match.
  Croix bas, F11 ou reset rejoue le même coup d'envoi, autant de fois que tu veux.
- **But simulé** : le ballon explose avec l'explosion du **dernier joueur de l'équipe qui a touché le ballon**
  (si personne ne l'a touché depuis le début de la simulation : celui qui a marqué ce but dans le replay), le
  bandeau du buteur s'affiche (km/h), puis, 2,5 s après, le **replay du but** de ta simulation. Pour s'entraîner
  vite (par exemple refaire un arrêt en boucle), décoche « Simulation : un but revu juste après »
  (`rlab_revoir_simulation 0`) : F11 / reset relance tout de suite.
- Quand tu conduis, chaque voiture part avec **son saut et son flip** du replay : un joueur qui a déjà fait son
  flip (ou son double saut) en l'air ne l'a plus, celui qui vient de sauter l'a encore pendant le reste de ses
  1,25 s, celui qui est tombé sans sauter l'a. Un flip rendu par le ballon (flip reset) est reconnu : les roues
  contre le ballon, ou un deuxième flip avant de retomber.
- **Horloge et règle du 0 seconde** : la simulation reprend l'horloge du replay (en haut de l'écran, avec le
  score ; option `rlab_tableau`) et elle tourne avec le jeu. Un but l'arrête. À 0:00 le match se termine dès
  que le ballon touche le sol, et un but marqué avant compte encore (« BUT À 0 SECONDE »). Au coup de sifflet
  final, plus personne ne pilote et « FIN DU MATCH » s'affiche avec le résultat, jusqu'à F11 / reset. Une
  égalité à ce moment-là part en prolongation ; si tu simules depuis une prolongation, le prochain but gagne.
- En simulation c'est un vrai match : boost limité (il part de la quantité du replay et se consomme),
  démolitions actives. Les **plots de boost** sont ceux du replay à ce moment-là (pris ou non, et ils réapparaissent
  quand ils réapparaissaient), à chaque départ et à chaque recommencement. Pendant la lecture aussi, les plots
  suivent le replay (en avant, en arrière, au ralenti) et restent figés quand le replay est en pause. Chaque essai
  enregistre aussi ses plots (revoir un essai ou resimuler depuis lui les remet comme ils étaient).
- **Contacts** : un fantôme est ramené sur sa trajectoire à chaque instant, donc il serait comme un mur. Dès
  qu'une voiture qui roule seule (toi, un bot) arrive contre lui (coup d'envoi, 50/50), il est lâché juste avant
  le contact : la collision est celle du jeu, puis le bot Psyonix le joue (comme après une démolition).
  De même, si le replay marque à un moment où ta simulation ne marque pas, les fantômes arrêtent de suivre
  le replay (sinon ils seraient projetés par une explosion qui n'a pas lieu) et jouent en bot Psyonix. Un adversaire démoli ne suit plus le replay : il réapparaît et joue en bot Psyonix.
  Pendant la lecture du replay, ni démolition ni boost libre.
- **Nexto** : rôle « Nexto (IA RLGym) » (ou bouton « Adversaires en Nexto »). Nexto tourne dans le plugin
  même, sans Python : `nexto/nexto.cpp` refait son observation et son réseau, avec les poids exportés dans
  `nexto/nexto.bin` (`nexto/export_nexto.py`). `nexto/test_nexto.cpp` vérifie qu'il choisit les mêmes actions
  que le modèle d'origine (80/80 sur des moments de replay, 0,3 ms par décision). Nexto est de Rolv-Arild et
  Soren (RLBotPack, licence CC BY-NC-SA 4.0 : usage personnel non commercial).
- **F4** en simulation quand tu conduis : prendre la voiture suivante, au même moment.
- **F11** (ou le reset de la manette) recommence la dernière simulation au même moment. **F9** : lecture/pause,
  et pendant une simulation, retour au replay.
- Arrêts : « Voir », « Prendre sa place », « Sans lui » (la simulation démarre 1,5 s avant).
- **Depuis le visionneur de replays du jeu** : F5 (ou Back) au moment voulu ouvre ce replay dans Replay Lab et
  lance la simulation à ce moment-là, en prenant la voiture suivie par la caméra.

## Branches : explorer et filmer des « et si »

Les essais ne sont **pas** gardés : recommence autant que tu veux (**croix bas**, F11 ou reset), chaque essai
remplace le précédent. Quand un essai te plaît, **sauvegarde-le** (**croix bas longue**, bouton « Sauvegarder » de la
fenêtre F3, ou `rlab_sauver`) : il devient une **branche** du replay (les 8 dernières sauvegardées sont gardées,
5 minutes chacune au plus).

- **Revoir** : le dernier essai, sauvegardé ou non (fenêtre F3, « Revoir cette simulation », `rlab_revoir`) ; une
  branche (liste « Branches », `rlab_revoir n`, ou à la manette **croix haut longue** : le replay d'origine, puis
  chaque branche de la dernière à la première, et on recommence). Une branche se revoit comme un replay : lecture/pause, ralenti, caméras (libre, automatique,
  joueur), mode cinéma, ses buts explosent et sont revus.
- **Brancher d'une branche** : F5 (Back) à n'importe quel moment d'une branche lance une nouvelle simulation
  depuis là (« Branche 4 depuis 2:41 de la Branche 2 »).
- **Revenir au replay d'origine** : bouton de la fenêtre, `rlab_origine`, ou **croix haut longue** (le cycle des
  branches passe par lui).
- Pour filmer : **mode cinéma** (rien de Replay Lab à l'écran sauf ce que le jeu montrerait : score, horloge,
  compte à rebours, buteur ; **croix gauche longue**), **ralenti** ×1 / ×0,5 / ×0,25 (**croix droite longue**,
  en simulation c'est le jeu entier qui ralentit), caméra libre (F7) ; enregistre avec OBS ou l'enregistreur
  d'écran.

## Caméras

C'est toujours la vraie caméra du jeu, qu'on fait suivre une autre voiture (F7, ou fenêtre F3) — BakkesMod ne
permet pas de changer de mode de caméra en entraînement libre. Comme dans le visionneur de replays, la caméra
prend les **réglages caméra du joueur suivi** (FOV, hauteur, angle, distance, rigidité, pivot, transition, lus
dans le replay) et **sa caméra ballon**, allumée et éteinte quand il l'a fait (option « Vue joueur : ses
réglages caméra » ; F8 la reprend à ta main pour ce joueur). **Jeu** (ta voiture), **Joueur** (n'importe quelle
voiture, F4 joueur suivant), **Automatique** (le joueur le plus proche du ballon, caméra ballon, plans d'au moins
2,5 s) et **Libre** (une voiture invisible et sans collision pilotée avec ta manette, la caméra derrière elle :
gâchettes avancer/reculer, stick tourner et monter/descendre, boost vite ; disponible en revoyant un replay ou
une branche). **F8** : caméra ballon. Pendant le replay d'un but, la caméra suit le buteur (sauf en caméra
libre), puis revient à la tienne.

## Noms et voitures

Chaque bot porte le nom de son joueur (caractères simples seulement : le jeu modifierait les autres). Au-dessus
de chaque voiture, une étiquette à son nom et à la couleur de son équipe remplace celle du jeu, qui reste
blanche sur un bot ; elle reste en mode cinéma (`rlab_noms 0` : celles du jeu). Hors mode cinéma, elle dit aussi
qui pilote une voiture changée (bot, Nexto, libre).

Chaque bot est fabriqué à l'image du joueur qu'il rejoue (carrosserie, sticker, roues, boost, antenne, chapeau,
finitions, **explosion de but**), lue dans le replay : le jeu ne peint une voiture qu'en la construisant, alors
le plugin donne l'apparence et l'équipe au bot juste avant que le jeu construise sa voiture (pendant SpawnBot).
Les couleurs (équipe et accent) sont posées par le plugin à partir des palettes du jeu (`ReplayLab/Palettes.h`,
valeurs du GUI de RLBot). Un bot qui porte une autre apparence est remplacé et quitte la partie.

**Mémoire** : le jeu plante si on retire un bot d'un entraînement libre, alors les bots sont réutilisés d'un
replay à l'autre (même carrosserie). Quand trop de bots se sont accumulés (plus de 5 en trop), l'ouverture du
replay suivant recharge l'entraînement libre, ce qui les retire tous.

## Fluidité

Un replay n'enregistre chaque voiture que toutes les 2-3 images, et à des instants irréguliers ; le plugin
recalcule au chargement des trajets lisses qui suivent les vitesses enregistrées (à une dizaine d'unités près), et
place tout à chaque pas de la physique du jeu (120 par seconde de jeu). La pause du jeu (Start) arrête aussi le
replay. Les trous du fichier (le replay de but que le jeu montrait) sont sautés, l'écran passe au noir pendant
le saut ; les voitures et le ballon du coup d'envoi apparaissent directement à leur place.

Pendant une simulation, le vrai replay est dessiné en fil de fer (trajet du ballon, voitures changées).
Options : ralenti, fils de fer, ballon synchronisé, scène retournée si tu prends une voiture orange.

## Raccourcis

Le plugin lit lui-même F3, F4, F5, F7, F8, F9, F10 et F11 (quand la fenêtre du jeu est devant), sans passer par
les liaisons BakkesMod : sous Proton, après un Alt+Tab, le jeu peut ne plus voir le clavier (ou croire que Alt
est encore enfoncé).

| Touche | Action |
| --- | --- |
| F3 | fenêtre Replay Lab |
| F4 | joueur suivant (en simulation quand tu conduis : prendre sa voiture) |
| F5 / F10 | simuler d'ici (dans le visionneur de replays du jeu : ouvrir ce replay et simuler) |
| F7 | caméra suivante : jeu, joueur, libre, automatique |
| F8 | caméra ballon |
| F9 | lecture/pause ; pendant le replay d'un but : le passer ; en simulation : retour au replay |
| F11 | recommencer la dernière simulation (l'essai n'est gardé que si tu le sauvegardes : `rlab_sauver`) |

## Manette

Option « Manette » de la fenêtre F3 (`rlab_manette`). Un **appui long** (0,6 s) sur la croix ou sur Back donne la
deuxième action ; l'action normale part quand on relâche avant.

| Bouton | Appui court | Appui long |
| --- | --- | --- |
| Croix haut | lecture/pause (en simulation : retour au replay ; replay d'un but : le passer) | branche suivante (replay d'origine, puis les branches sauvegardées) |
| Croix gauche | caméra suivante | mode cinéma oui/non |
| Croix droite | joueur suivant | ralenti ×1 / ×0,5 / ×0,25 |
| Croix bas | **recommencer** la simulation (F11) ; sans simulation encore : simuler d'ici | **sauvegarder** la dernière simulation comme branche |
| Back / View (Share sur PlayStation) | simuler d'ici (F5) ; un autre bouton avec `rlab_manette_simuler <touche>`, ex. `XboxTypeS_LeftThumbstick` | statut suivant du joueur à l'écran : Fantôme, Toi, Bot, Nexto, Libre, Retirée (appliqué 1,2 s après le dernier appui) |
| Reset de l'entraînement (ton bouton, RB par défaut) | recommencer (F11) | |
| A (saut) | passer le replay d'un but | |
| Accélérer / freiner (quand tu regardes) | avance rapide ×3 / retour rapide ×3 (en caméra libre : la caméra avance) | |

Par défaut, le jeu donne aussi la croix aux messages rapides : désactive-les dans les commandes du jeu si tu ne
veux pas qu'ils s'affichent.

## Options (fenêtre F3, ou console)

| Variable | Défaut | Effet |
| --- | --- | --- |
| `rlab_explosion` | 1 | le ballon explose aux buts avec l'explosion du buteur (0 : il reste dans le filet) |
| `rlab_noms_plugin` | 0 | noms au-dessus des voitures : 0 ceux du jeu, 1 la copie du plugin à la couleur de l'équipe, avec le rôle |
| `rlab_voiture_joueur` | 1 | tu conduis la voiture du joueur, son bot (0 : ta propre voiture prend sa place) |
| `rlab_revoir_buts` | 1 | replay de chaque but du replay avant le coup d'envoi |
| `rlab_revoir_simulation` | 1 | replay d'un but de simulation (0 : pour s'entraîner vite) |
| `rlab_attendre` | 1 | départ d'une simulation : 0 tout de suite, 1 quand tu bouges, 2 compte à rebours |
| `rlab_tableau` | 1 | score, horloge, compte à rebours et buteur à l'écran |
| `rlab_noms` | 1 | étiquettes aux noms des joueurs |
| `rlab_cinema` | 0 | mode cinéma |
| `rlab_sync` | 1 | le ballon suit le replay tant qu'aucune voiture changée ne le touche |
| `rlab_speed` | 1 | vitesse du jeu en simulation |

## Recompiler

    ./build.sh && ./install_plugin.sh

Depuis un clone (Linux), la première fois :

1. Outils : clang (clang-cl, lld-link), Rust avec la cible Windows (`rustup target add x86_64-pc-windows-msvc`),
   xwin (`cargo install xwin`).
2. En-têtes et bibliothèques Microsoft (leur licence est à accepter) :
   `xwin --accept-license splat --output toolchain/msvc`.
3. Noms de fichiers du SDK BakkesMod sous Linux : `python3 fix_sdk_case.py sdk/include ReplayLab`.
4. `RL_PREFIX` : le préfixe Proton de Rocket League, si ce n'est pas
   `/mnt/data/SteamLibrary/steamapps/compatdata/252950/pfx` (`install_plugin.sh`, `lancer_bakkesmod.sh`).

- `rlab_replay/` : lecteur de replays en Rust (subtr-actor 1.5.1, le même que le labo web),
  compilé en `rlab_replay.dll` (installé dans `plugins/replaylab/`). `cargo run --release --example flow --
  <replay>` montre comment un replay déroule le match (états, compte à rebours, buts, démolitions, trous).
- `ReplayLab/` : le plugin (C++), compilé avec clang-cl, les en-têtes MSVC et le SDK Windows
  téléchargés par xwin dans `toolchain/msvc` (licence Microsoft acceptée le 25/10/2026), le SDK
  BakkesMod (`sdk/`) et son ImGui (`imgui_src/`). Rust est installé dans `~/.cargo` et `~/.rustup`.
- Les compilations tournent dans un scope systemd plafonné (1,5 Go, un seul cœur pour Rust).

## Fichiers

- `dist/` (non versionné) : BakkesMod 2.0.78 (GitHub officiel bakkesmodorg).
- BakkesMod est installé dans le préfixe Proton :
  `…/compatdata/252950/pfx/drive_c/users/steamuser/AppData/Roaming/bakkesmod/`.

## Vidéo de présentation (en cours)

- `video/tourner.py` enchaîne les scènes d'une présentation (titres et légendes : `rlab_titre`, `rlab_legende` ;
  interface du jeu : `rlab_hud`) en filmant l'écran, et ne filme que le jeu (au premier plan).
- `video/ecran.py` enregistre l'écran sous Wayland par le portail de partage d'écran (PipeWire, NVENC).
- `video/monter.py` monte la vidéo à partir des scènes filmées (`rushes/scenes.json`).
