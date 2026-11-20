#!/bin/bash
# Remote prediction's model (code/cgame/remote_predict.cpp, cg_remotePrediction) against the game's own movement
# (code/fgame/bg_pmove.cpp): the unit test code/cgame/tests/test_remote_predict.cpp, the Mac's stand-in for the
# Vulkan side's ctest (cmake/tests/remote_predict.cmake; cmake/tests is gone here, Phase 7a). Built twice into
# build.noindex/rptest/: with the cgame's Release flags (-O3 -DNDEBUG and appleclang.cmake's -mllvm flag: the physics
# compared as the game computes it), and under AddressSanitizer and UBSan.
#   port/rptest.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"

OUT="$OMM_BUILD/rptest"
mkdir -p "$OUT"
C="$OMM_ROOT/code"
SRC=("$C/cgame/tests/test_remote_predict.cpp" "$C/cgame/remote_predict.cpp" "$C/fgame/bg_pmove.cpp"
	"$C/fgame/bg_slidemove.cpp" "$C/qcommon/q_math.c" "$C/qcommon/q_shared.c" "$C/qcommon/common_light.c")
INC=(-I"$C" -I"$C/fgame" -I"$C/qcommon")

build() {	# name, flags
	local name=$1 f o objs=()
	shift
	if [ -x "$OUT/$name" ] && [ -z "$(find "${SRC[@]}" "$C/cgame/remote_predict.h" "$C/fgame/bg_public.h" \
		"$C/qcommon/q_shared.h" "$0" -newer "$OUT/$name")" ]; then
		return
	fi
	mkdir -p "$OUT/$name.o"
	for f in "${SRC[@]}"; do
		o="$OUT/$name.o/$(basename "$f").o"
		case "$f" in
			*.c) clang -std=gnu17 -w "${INC[@]}" "$@" -c "$f" -o "$o" ;;
			*) clang++ -std=gnu++17 -w -mllvm -simplifycfg-merge-compatible-invokes=false "${INC[@]}" "$@" -c "$f" \
				-o "$o" ;;
		esac
		objs+=("$o")
	done
	clang++ "$@" "${objs[@]}" -o "$OUT/$name"
}

build test_remote_predict -O3 -DNDEBUG -g
build test_remote_predict-asan -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
"$OUT/test_remote_predict"
"$OUT/test_remote_predict-asan"
