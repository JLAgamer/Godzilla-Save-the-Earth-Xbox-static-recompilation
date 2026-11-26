#!/bin/sh
# Godzilla: Save the Earth recomp, Linux / Steam Deck.
# The first time, give it your disc image (.iso / .xiso.iso) or extracted game
# folder:   ./play.sh "/path/to/Godzilla - Save the Earth (USA, Europe).iso"
# It is remembered in game_path.txt, so afterwards just run ./play.sh.
# The first start unpacks the disc next to this file (about 800 MB).
cd "$(dirname "$0")" || exit 1
if [ -n "$1" ]; then
    printf '%s\n' "$1" > game_path.txt
fi
if [ ! -f game_path.txt ]; then
    echo "Run ./play.sh <your Godzilla: Save the Earth disc image or game folder> once."
    exit 1
fi
GAME=$(head -n 1 game_path.txt)
exec ./godzilla_ste_recomp --game "$GAME" --log "$PWD/logs/godzilla.log"
