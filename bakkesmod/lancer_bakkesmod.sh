#!/usr/bin/env bash
# Lance l'injecteur BakkesMod dans le préfixe Proton de Rocket League.
#  1. Lance Rocket League depuis Steam et choisis « sans Easy Anti-Cheat » à la question de Steam
#     (avec l'anti-triche, les mods sont bloqués ; sans, tu ne peux pas jouer en ligne).
#  2. Une fois au menu du jeu, lance ce script. La fenêtre BakkesMod s'ouvre, se met à jour
#     au premier lancement, puis injecte le mod (F2 ouvre le menu BakkesMod en jeu).
PREFIX="${RL_PREFIX:-/mnt/data/SteamLibrary/steamapps/compatdata/252950/pfx}"   # Rocket League's Proton prefix
EXE="$PREFIX/drive_c/users/steamuser/AppData/Roaming/bakkesmod/BakkesMod.exe"
if ! pgrep -f -i "RocketLeague.exe" > /dev/null; then
    echo "Rocket League ne tourne pas : lance-le d'abord depuis Steam (sans anti-triche)."
    exit 1
fi
exec protontricks-launch --appid 252950 "$EXE"
