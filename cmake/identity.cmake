# The programs' names: the app mohreloaded.app, installed into <prefix>/lib/<PROJECT_NAME>; upstream's were openmohaa
# and omohaaded
set(PROJECT_NAME mohreloaded)
# MoHAA Reloaded's version, which the game shows and sends (cmake/version.cmake adds the stage, the CI run and the
# commit). A release's tag is v<this>: CI's tag builds check it (PRODUCT_RELEASE_TAG, below).
set(PROJECT_VERSION 1.0.0)

set(SERVER_NAME mohreloaded-server)
set(CLIENT_NAME mohreloaded)

set(BASEGAME)

set(CGAME_MODULE cgame)
set(GAME_MODULE game)
set(UI_MODULE ui)

# The Metal port's icon (misc/macos/mohreloaded.svg, built with port/makeicon.swift)
set(MACOS_ICON_PATH ${CMAKE_SOURCE_DIR}/misc/macos/mohreloaded.icns)
# The Metal port's own app identity (its first release, 2026-10-20): the bundle ID, kept through the rename (macOS
# keys the game's permissions and settings by it). The Info.plist's version and the release zip's are PROJECT_VERSION.
set(MACOS_BUNDLE_ID io.github.pstngh.openmohaa-metal)

set(COPYRIGHT "OpenMoHAA Copyright © 2025 OpenMoHAA Team, Inc. All rights reserved.")

set(CONTACT_EMAIL "")
