# Labo replays Rocket League

Rejouer ses replays Rocket League et refaire les actions autrement, de deux façons :

- **dans le jeu** : le plugin BakkesMod **Replay Lab** ([`bakkesmod/`](bakkesmod/README.md)) rejoue le replay en
  entraînement libre, dans son stade et son mode (Hoops, Snow Day, Dropshot), chaque joueur dans sa voiture ;
  on simule depuis n'importe quel moment avec la vraie physique du jeu, en prenant une voiture, en mettant
  Nexto ou un bot à la place d'un joueur, avec l'horloge et la règle du 0 seconde ; chaque simulation se
  revoit comme un replay, avec les caméras du jeu, pour la filmer ;
- **dans le navigateur** : le labo web (ci-dessous), avec le moteur RocketSim.

## Récupérer le projet

    git clone --recursive git@github.com:sajouaou/rl-replay-lab.git

Le SDK BakkesMod est un sous-module (`bakkesmod/sdk`). Labo web : `python3 -m venv .venv` puis
`.venv/bin/pip install -r requirements.txt`. Plugin : voir « Recompiler » dans [`bakkesmod/README.md`](bakkesmod/README.md).

## Labo web

Revoir ses replays en 3D, s'arrêter à n'importe quel moment et rejouer la suite autrement
avec le moteur physique RocketSim : retirer une voiture, la remplacer par un bot, la conduire
soi-même, déplacer ou relancer le ballon. Le vrai replay continue en transparence pour comparer.

## Lancer / arrêter

    ./lancer_lab.sh          # ouvre http://127.0.0.1:8765
    ./arreter_lab.sh

Le serveur tourne comme service systemd utilisateur plafonné à 3 Go sans swap (même principe
que `lancer_comfyui.sh`) : si la mémoire déborde, seul le labo s'arrête. Il continue de tourner
après la fermeture du terminal. Journal : `journalctl --user -u rl-replay-lab`.
Dossier de replays par défaut : `…/compatdata/252950/…/TAGame/Demos` (Steam/Proton).
Autre dossier : `./lancer_lab.sh /chemin/vers/Demos`. Ton pseudo : `RL_PLAYER=Anxoide` (par défaut).

## Utilisation

1. Choisis un replay en haut à gauche. La première ouverture prend 1 à 3 s (analyse mise en cache dans `cache/`).
2. Lis le replay (Espace) ou déplace-toi sur la ligne de temps : traits bleus/orange = buts, traits blancs = tes touches.
3. Dans « Et si… », choisis le rôle de chaque voiture :
   - **Fantôme** : refait exactement le vrai trajet (peut toucher le ballon, mais ne réagit pas).
   - **Retirée** : n'existe plus.
   - **Libre** : physique réelle, personne aux commandes.
   - **Nexto (IA)** : le bot RLGym Nexto joue à sa place (réagit comme un très bon joueur).
     Chargé au premier usage (environ 4 s, environ 250 Mo). Raccourcis : « Adversaires en Nexto »,
     « Tous en Nexto », et « Nexto à sa place » sur chaque arrêt.
   - **Bot simple** : fonce sur le ballon (réagit, mais joue mal).
   - **Je conduis** : clavier (Z/Q/S/D, Espace, Maj boost, Ctrl dérapage, A/E air roll) ou manette.
4. « Simuler à partir d'ici ». Changer un rôle pendant la simulation la relance au même instant.
5. Panneau « Ballon » : modifier position et vitesse, « Placer au clic » puis clic sur le terrain.

## Limites

- Le terrain de simulation est reconstruit à partir des cotes officielles (pas les fichiers du jeu) :
  très fidèle en l'air et au sol, approximatif sur les rampes courbes et dans les coins.
- Les fantômes ne réagissent pas au nouveau ballon : au-delà de 3-4 s, la comparaison perd son sens.
- Les touches du replay sont attribuées à la voiture la plus proche du ballon.

## Contenu

- `lab/server.py` : serveur web + simulation en direct (WebSocket, 120 Hz).
- `lab/session.py` : une simulation « et si » (fantômes, bot, pilotage).
- `lab/parse_replay.py` : lecture des replays dans un sous-processus (libère sa mémoire en sortant).
- `lab/arena_mesh.py` : terrain approximatif pour RocketSim et pour l'affichage.
- `lab/nexto_brain.py` : fait jouer Nexto dans RocketSim (reconstruit l'état de jeu au format RLGym).
- `vendor/nexto/` : fichiers originaux de Nexto, non modifiés, tirés de RLBotPack (`RLBotPack/Necto/Nexto`).
  Nexto est l'œuvre de Rolv-Arild et Soren, sous licence CC BY-NC-SA 4.0 : usage personnel,
  non commercial, en citant les auteurs.
- `static/` : interface 3D (three.js 0.160, copié localement dans `static/vendor/`).

## Licences

- Nexto (`vendor/nexto/`, et ses poids convertis `bakkesmod/nexto/nexto.bin`) : Rolv-Arild et Soren, CC BY-NC-SA 4.0
  (voir `vendor/nexto/LICENSE`) : usage non commercial, en citant les auteurs, sous la même licence.
- three.js (`static/vendor/`) : MIT.
- SDK BakkesMod : sous-module, dépôt officiel `bakkesmodorg/BakkesModSDK`.
- Les en-têtes et bibliothèques Microsoft nécessaires à la compilation ne sont pas fournis (xwin les télécharge).
