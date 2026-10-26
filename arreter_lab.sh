#!/usr/bin/env bash
# Arrête le labo replays.
systemctl --user stop rl-replay-lab.service 2>/dev/null && echo "Labo arrêté." || echo "Le labo ne tournait pas."
