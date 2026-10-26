#!/usr/bin/env bash
# Lance le labo replays : http://127.0.0.1:8765
#
# Le serveur tourne comme service systemd utilisateur plafonné (3 Go, pas de swap) :
# l'analyse d'un replay prend 600-800 Mo pendant quelques secondes, et si la mémoire
# déborde, seul le labo s'arrête, pas VS Code ni le bureau. Il continue de tourner
# après la fermeture du terminal ; ./arreter_lab.sh l'arrête.
#   ./lancer_lab.sh                 dossier de replays par défaut (Steam / Proton)
#   ./lancer_lab.sh /autre/dossier  autre dossier de .replay
#   NO_BROWSER=1 ./lancer_lab.sh    sans ouvrir le navigateur
#   journalctl --user -u rl-replay-lab    journal du serveur
cd "$(dirname "$0")" || exit 1
if systemctl --user is-active --quiet rl-replay-lab.service; then
    echo "Le labo tourne déjà : http://127.0.0.1:8765"
else
    systemctl --user reset-failed rl-replay-lab.service 2>/dev/null
    systemd-run --user --quiet --collect --unit=rl-replay-lab \
        -p MemoryMax=3G -p MemorySwapMax=0 -p WorkingDirectory="$PWD" \
        "$PWD/.venv/bin/python" -m lab.server "$@" || exit 1
    for _ in $(seq 40); do
        curl -s -o /dev/null http://127.0.0.1:8765/ && break
        sleep 0.5
    done
    echo "Labo lancé : http://127.0.0.1:8765"
fi
[ -z "$NO_BROWSER" ] && xdg-open http://127.0.0.1:8765 >/dev/null 2>&1 &
exit 0
