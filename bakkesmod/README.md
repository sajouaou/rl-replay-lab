# Plugin BakkesMod « Replay Lab » (v3.4, autonome)

Rejoue tes replays **dans l'entraînement libre** et reprends la main à n'importe quel moment,
avec la vraie physique du jeu. Aucun site ni serveur : le plugin lit lui-même les fichiers
`.replay` de ton dossier `Demos`.

## Lancer

1. Rocket League depuis Steam, **sans Easy Anti-Cheat**.
2. Au menu : `./lancer_bakkesmod.sh`.
3. Après une mise à jour du plugin, dans la console BakkesMod (F6) :
   `plugin unload replaylab` puis `plugin load replaylab` (ou relance le jeu).

## Utiliser

- **F3** ouvre la fenêtre Replay Lab : choisis un replay, « Ouvrir ». L'entraînement libre se
  charge **dans le stade et le mode du replay**, chaque joueur devient une voiture et le replay se
  rejoue dedans. Le mode est reconnu au ballon enregistré : Hoops (ballon de basket, buts dans le
  panier), Snow Day (palet), Dropshot (son ballon), sinon soccar. Rumble, Heatseeker et les ballons de
  mutateurs se jouent comme du soccar.
- Barre de temps : lecture/pause, ±5 s, vitesse ×0,25 à ×2, curseur (barres bleues/orange = buts,
  traits blancs = arrêts).
- Par défaut tu es **spectateur** : chaque joueur rejoue dans sa vraie voiture (sa carrosserie, son
  apparence, son équipe, son nom) et ta voiture attend sous le terrain ; **F4** fait suivre le joueur suivant par
  la caméra. Pour conduire, mets « Toi » sur un joueur (fenêtre F3) : ta voiture prend alors sa place.
- **F5 (ou F10) « Simuler d'ici »** : à l'instant même, chaque voiture prend son rôle et la vraie physique
  prend le relais. Rôles (fenêtre F3, ou console `rlab_role <joueur 1..N> <0 toi, 1 fantôme,
  2 retirée, 3 bot, 4 libre>`) : Toi (tu conduis), Fantôme (refait le vrai trajet et peut
  toucher le ballon), Retirée, Bot Psyonix, Libre (sans pilote). Le ballon suit le replay tant
  qu'aucune voiture changée ne s'en approche. Un but dans la simulation s'affiche (« BUT pour les
  bleus ! ») sans explosion : le ballon reste dans le filet.
- Quand tu conduis (« Toi »), la simulation attend comme en entraînement personnalisé : tout est figé
  au moment choisi jusqu'à ce que tu touches la manette, puis tout part avec sa vraie vitesse
  (option « Comme l'entraînement » dans la fenêtre F3 ; F11 refige la scène).
- Chaque voiture part avec **son saut et son flip** du replay : un joueur qui a déjà fait son flip
  (ou son double saut) en l'air ne l'a plus, celui qui vient de sauter l'a encore pendant le reste de ses
  1,25 s, celui qui est tombé sans sauter l'a. Un flip rendu par le ballon (flip reset) est reconnu : les
  roues contre le ballon, ou un deuxième flip avant de retomber.
- **Horloge et règle du 0 seconde** : la simulation reprend l'horloge du replay (en haut de l'écran, avec
  le score ; option `rlab_tableau`) et elle tourne avec le jeu. Un but l'arrête. À 0:00 le match se
  termine dès que le ballon touche le sol, et un but marqué avant compte encore (« BUT À 0 SECONDE »).
  Au coup de sifflet final, comme dans le jeu, plus personne ne pilote, le ballon s'arrête et « FIN DU
  MATCH » s'affiche avec le résultat, jusqu'à F11 / reset. Une égalité à ce moment-là part en prolongation
  (le bandeau le dit : elle commence par un coup d'envoi que le replay ne donne pas) ; si tu simules
  depuis une prolongation, le prochain but gagne. Pour tester un « et si » des dernières
  secondes : place-toi juste avant 0:00 (ou pendant le 0 seconde) et simule.
- En simulation c'est un vrai match : boost limité (il part de la quantité du replay et se
  consomme), démolitions actives. Un adversaire démoli ne suit plus le replay : il réapparaît et
  joue en bot Psyonix. Pendant la lecture du replay, ni démolition ni boost libre.
- **Nexto** : rôle « Nexto (IA RLGym) » (ou bouton « Adversaires en Nexto »). Nexto tourne dans le
  plugin même, sans Python : `nexto/nexto.cpp` refait son observation et son réseau, avec les poids
  exportés dans `nexto/nexto.bin` (`nexto/export_nexto.py`). `nexto/test_nexto.cpp` vérifie qu'il
  choisit les mêmes actions que le modèle d'origine (80/80 sur des moments de replay, 0,3 ms par
  décision). Nexto est de Rolv-Arild et Soren (RLBotPack, licence CC BY-NC-SA 4.0 : usage personnel
  non commercial).
- **F4** en simulation quand tu conduis : prendre la voiture suivante, au même moment.
- **F11** recommence la simulation au même moment. **F9** : lecture/pause, et pendant une
  simulation, retour au replay.
- Arrêts : « Voir », « Prendre sa place », « Sans lui » (la simulation démarre 1,5 s avant).
- **Depuis le visionneur de replays du jeu** : F5 au moment voulu ouvre ce replay dans Replay Lab
  et lance la simulation à ce moment-là, en prenant la voiture suivie par la caméra.

Noms : chaque bot porte le nom de son joueur (caractères simples seulement : le jeu modifierait les
autres). Au-dessus de chaque voiture, une étiquette à son nom et à la couleur de son équipe remplace celle du
jeu, qui reste blanche sur un bot ; elle reste en mode cinéma (`rlab_noms 0` : celles du jeu). Hors mode
cinéma, elle dit aussi qui pilote une voiture changée (bot, Nexto, libre).

Voitures : chaque bot est fabriqué à l'image du joueur qu'il rejoue (carrosserie, sticker, roues, boost,
antenne, chapeau, finitions), lue dans le replay : le jeu ne peint une voiture qu'en la construisant, alors
le plugin donne l'apparence et l'équipe au bot juste avant que le jeu construise sa voiture (pendant
SpawnBot). Les couleurs (équipe et accent) sont posées par le plugin à partir des palettes du jeu
(`ReplayLab/Palettes.h`, valeurs du GUI de RLBot). Un bot qui porte une autre apparence est remplacé et
quitte la partie. Quand un joueur est démoli dans le replay (ou quand les voitures sont remises en place
après un but), sa voiture explose aussi dans le jeu et revient.

Caméras (F7, ou fenêtre F3) : c'est toujours la vraie caméra du jeu, qu'on fait suivre une autre
voiture — BakkesMod ne permet pas de changer de mode de caméra en entraînement libre. Comme dans le
visionneur de replays, la caméra prend les **réglages caméra du joueur suivi** (FOV, hauteur, angle,
distance, rigidité, pivot, transition, lus dans le replay) et **sa caméra ballon**, allumée et éteinte
quand il l'a fait (option « Vue joueur : ses réglages caméra » ; F8 la reprend à ta main pour ce
joueur). **Jeu** (ta voiture), **Joueur** (n'importe quelle voiture, F4 joueur suivant),
**Automatique** (le joueur le plus proche du ballon, caméra ballon, plans d'au moins 2,5 s) et **Libre**
(une voiture invisible et sans collision pilotée avec ta manette, la caméra derrière elle : gâchettes
avancer/reculer, stick tourner et monter/descendre, boost vite ; disponible en revoyant un replay ou un
scénario). **F8** : caméra ballon.

Scénarios : chaque simulation est enregistrée. « Revoir cette simulation » (fenêtre F3, ou
`rlab_revoir`) la rejoue comme un replay : lecture/pause, ralenti, barre de temps, caméras, et F5 pour
resimuler depuis n'importe quel moment. « Revenir au replay d'origine » (`rlab_origine`). Pour filmer :
coche « Mode cinéma » (rien de Replay Lab à l'écran) et enregistre avec OBS ou l'enregistreur d'écran.

Fluidité : un replay n'enregistre chaque voiture que toutes les 2-3 images, et à des instants
irréguliers ; le plugin recalcule au chargement des trajets lisses qui suivent les vitesses enregistrées
(à une dizaine d'unités près), et place tout à chaque pas de la physique du jeu (120 par seconde de jeu).
La pause du jeu (Start) arrête aussi le replay.

Pendant une simulation, le vrai replay est dessiné en fil de fer (trajet du ballon, voitures
changées). Options : ralenti, fils de fer, ballon synchronisé, scène retournée si tu prends une
voiture orange.

Raccourcis : le plugin lit lui-même F3, F4, F5, F7, F8, F9, F10 et F11 (quand la fenêtre du jeu est
devant), sans passer par les liaisons BakkesMod : sous Proton, après un Alt+Tab, le jeu peut ne plus voir
le clavier (ou croire que Alt est encore enfoncé).

Manette (option « Manette » de la fenêtre F3, `rlab_manette`) :

| Bouton | Action |
| --- | --- |
| Croix haut | lecture/pause (F9 ; en simulation : retour au replay) |
| Croix gauche | caméra suivante (F7) |
| Croix droite | joueur suivant (F4) |
| Croix bas | statut suivant du joueur à l'écran : Fantôme, Toi, Bot, Nexto, Libre, Retirée (appliqué 1,2 s après le dernier appui) |
| Back / View (Share sur PlayStation) | simuler d'ici (F5) ; un autre bouton avec `rlab_manette_simuler <touche>`, ex. `XboxTypeS_LeftThumbstick` |
| Reset de l'entraînement (ton bouton, RB par défaut) | recommencer (F11) |

Par défaut, le jeu donne aussi la croix aux messages rapides : désactive-les dans les commandes du jeu si
tu ne veux pas qu'ils s'affichent.

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
  compilé en `rlab_replay.dll` (installé dans `plugins/replaylab/`).
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
