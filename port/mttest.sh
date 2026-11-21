#!/bin/bash
# The mouse's turn, the turn distance kept (m_turn) and CS Mode (m_csmode): code/client/cl_mouseturn_logic.h against
# the unit test code/client/tests/test_mouseturn.cpp (the turns measured in the game), the Mac's stand-in for the Vulkan
# side's ctest (cmake/tests/mouseturn.cmake; cmake/tests is gone here, Phase 7a). Built into build.noindex/mttest/ and
# run, ~1 s.
#   port/mttest.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"

OUT="$OMM_BUILD/mttest"
mkdir -p "$OUT"
clang++ -std=gnu++17 -O2 -g -Wall -Wextra "$OMM_ROOT/code/client/tests/test_mouseturn.cpp" -o "$OUT/test_mouseturn"
"$OUT/test_mouseturn"
