#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. The text compiled here is
#                 what `txtest --dump-shaders` writes: the exact strings the
#                 plugin hands the driver.
#   optimal       the row programme's cost equals an exhaustive search,
#                 exactly. No GL: this is the encoder's claim on its own.
#   checks        every picture check, at TWO rasters: 320x180, which is what
#                 CI renders at, and 1280x720. A check that holds at one
#                 raster was fitted to it. Each is measured out of the picture:
#                   --gap        one background cell per colour change, two
#                                per background change
#                   --palette    eight colours, every sixel uniform
#                   --separated  the gutter where the SAA5050 puts it
#                   --carriage   row r shows the frame of the field that
#                                carried it, through a resize
#                   --parity     one bit blanks, two bits change, none moves
#                   --negative   every one of those FAILS on a perturbed model
#   pipe          the fleet's --pipe frame format: whole frames only, an
#                 unknown cue refused, a closed stdout is exit 1 not SIGPIPE
#   sweep         does every control change the picture
#   bench         the render cost, for the record. Not pass/fail.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

TXTEST="$BUILD/txtest"

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails. No shaders at all is a FAILURE: it means the dump broke.
#---------------------------------------------------------------------------
step "shaders"
if ! command -v glslc >/dev/null 2>&1; then
	printf '   skipped: glslc not installed (brew install shaderc)\n'
else
	dir="$( mktemp -d )"
	"$TXTEST" --dump-shaders "$dir" >/dev/null
	n=0; bad=0
	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done
	rm -rf "$dir"
	if [ "$n" -ne 3 ]; then
		fail "$n shaders were dumped, expected 3 -- the dump has gone stale"
	elif [ "$bad" -eq 0 ]; then
		pass "all $n shaders compile"
	else
		fail "$bad of $n shaders do not compile"
	fi
fi

step "optimal (no GL)"
if out=$("$TXTEST" --optimal 2>&1); then
	pass "txtest --optimal: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
else
	fail "txtest --optimal"
	printf '%s\n' "$out" | tail -5 | sed 's/^/      /'
fi

for size in 320x180 1280x720; do
	step "checks at $size"
	for check in gap palette separated carriage parity negative; do
		if out=$("$TXTEST" --$check --size $size 2>&1); then
			pass "txtest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "txtest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format. Two and a half frames in must be exactly
# two frames out and a clean exit -- a partial frame is the end of the stream,
# never a frame -- a cue naming no parameter must be refused rather than
# silently doing nothing to a take, and a reader that hangs up early must end
# the run with exit 1, not SIGPIPE's silent 141.
#---------------------------------------------------------------------------
step "pipe"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); cues=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
got=$( "$TXTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever txtest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$TXTEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi
head -c $(( frame * 20 )) /dev/zero > "$raw"
"$TXTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout ends the run with exit 1, not SIGPIPE"
else
	fail "a closed stdout gave exit $status, not 1"
fi
rm -f "$raw" "$cues"

step "sweep"
for size in 320x180 480x270; do
	if out=$(python3 tools/sweep.py --binary "$TXTEST" --size $size 2>/dev/null); then
		pass "$size: $( printf '%s\n' "$out" | tail -1 )"
	else
		fail "tools/sweep.py reports a dead control at $size"
		printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
	fi
done

step "bench (for the record)"
"$TXTEST" --bench --frames 60 2>&1 | sed -n '3,7p' | sed 's/^/   /'

BUNDLE="$BUILD/Teletext.bundle"
BIN="$BUNDLE/Contents/MacOS/Teletext"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.teletext" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Teletext.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Teletext" "id:          TX01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
