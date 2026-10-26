#!/usr/bin/env bash
# Copie ReplayLab.dll dans les plugins BakkesMod du préfixe Proton et l'ajoute au chargement automatique.
set -euo pipefail
cd "$(dirname "$0")"
# RL_PREFIX: Rocket League's Proton prefix (Steam: …/steamapps/compatdata/252950/pfx)
PREFIX="${RL_PREFIX:-/mnt/data/SteamLibrary/steamapps/compatdata/252950/pfx}"
BM="$PREFIX/drive_c/users/steamuser/AppData/Roaming/bakkesmod/bakkesmod"
[ -f build/ReplayLab.dll ] && [ -f build/rlab_replay.dll ] || { echo "build/ manquant : lance ./build.sh"; exit 1; }
# copy then rename: a running game keeps its loaded copy intact (overwriting in place could crash it)
cp build/ReplayLab.dll "$BM/plugins/ReplayLab.dll.new"
mv -f "$BM/plugins/ReplayLab.dll.new" "$BM/plugins/ReplayLab.dll"
mkdir -p "$BM/plugins/replaylab"   # the replay reader, loaded by the plugin itself
cp build/rlab_replay.dll "$BM/plugins/replaylab/rlab_replay.dll.new"
mv -f "$BM/plugins/replaylab/rlab_replay.dll.new" "$BM/plugins/replaylab/rlab_replay.dll"
cp nexto/nexto.bin "$BM/plugins/replaylab/nexto.bin.new"   # Nexto's network (export_nexto.py)
mv -f "$BM/plugins/replaylab/nexto.bin.new" "$BM/plugins/replaylab/nexto.bin"
CFG="$BM/cfg/plugins.cfg"
if ! grep -q "^plugin load replaylab" "$CFG"; then   # the file has Windows line ends
    if grep -qx "writeplugins" "$CFG"; then sed -i 's/^writeplugins$/plugin load replaylab\nwriteplugins/' "$CFG"
    else echo "plugin load replaylab" >> "$CFG"; fi
fi
echo "ReplayLab installé dans $BM/plugins (chargé au démarrage de BakkesMod)."
