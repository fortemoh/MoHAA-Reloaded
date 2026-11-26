#!/bin/sh
# Renders the launchers' PNG icons from the SVGs beside this script. The PNGs are committed, so building a launcher
# needs no SVG renderer; run this again after an SVG changes. Needs rsvg-convert (librsvg).
#   game-aa.svg, game-sh.svg, game-bt.svg: the games' icons (fortemoh/moh-icons 0e8f1cb8 icons/mohaa-transparent.svg,
#   mohaa-spearhead.svg and mohaa-breakthrough.svg: Allied Assault's without its black disc, transparent as the app
#   icon is)
#   app-icon.svg: the launcher's icon, the game's gold star (moh-icons icons/mohaa-transparent.svg)
# The apps' icons (misc/macos/mohreloaded.icns, launcher/macos/Resources/AppIcon.icns) are the same star,
# misc/macos/mohreloaded.svg, through port/makeicon.swift and iconutil.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
for game in aa sh bt; do
	for size in 32 64; do
		rsvg-convert -w "$size" -h "$size" -o "$here/game-$game-$size.png" "$here/game-$game.svg"
	done
done
rsvg-convert -w 128 -h 128 -o "$here/app-icon-128.png" "$here/app-icon.svg"
