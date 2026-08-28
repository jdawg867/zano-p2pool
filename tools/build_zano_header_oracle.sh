#!/usr/bin/env bash
set -euo pipefail

ZANO_SOURCE="${1:-${ZANO_SOURCE:-/home/jdawg/zano}}"
ZANO_BUILD="${2:-${ZANO_BUILD:-$ZANO_SOURCE/build}}"
P2POOL_SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$P2POOL_SOURCE/build-zano-oracle"

FLAGS_FILE="$(find "$ZANO_BUILD" -path '*/CMakeFiles/connectivity_tool.dir/flags.make' -print -quit 2>/dev/null || true)"
LINK_FILE="$(find "$ZANO_BUILD" -path '*/CMakeFiles/connectivity_tool.dir/link.txt' -print -quit 2>/dev/null || true)"

if [[ -z "$FLAGS_FILE" || -z "$LINK_FILE" ]]; then
    echo "error: could not find connectivity_tool build metadata under: $ZANO_BUILD" >&2
    echo "Build Zano first, or pass the correct Zano build directory as argument 2." >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

# flags.make is Make syntax (for example: CXX_DEFINES = ...), not a shell
# script. Extract the generated values without sourcing/executing the file.
make_value() {
    local name="$1"
    sed -n "s/^${name} = //p" "$FLAGS_FILE" | head -n 1
}

CXX_DEFINES="$(make_value CXX_DEFINES)"
CXX_INCLUDES="$(make_value CXX_INCLUDES)"
CXX_FLAGS="$(make_value CXX_FLAGS)"

LINK_LINE="$(cat "$LINK_FILE")"

# Parse CMake-generated command fragments with Python's shell lexer. This
# preserves quoted definitions and escaped values correctly.
mapfile -d '' -t COMPILE_ARGS < <(
    python3 - "$CXX_DEFINES" "$CXX_INCLUDES" "$CXX_FLAGS" <<'PY'
import shlex
import sys

for text in sys.argv[1:]:
    for arg in shlex.split(text):
        sys.stdout.buffer.write(arg.encode() + b"\0")
PY
)

mapfile -d '' -t LINK_ARGS < <(
    python3 - "$LINK_LINE" <<'PY'
import shlex
import sys

for arg in shlex.split(sys.argv[1]):
    sys.stdout.buffer.write(arg.encode() + b"\0")
PY
)

if (( ${#LINK_ARGS[@]} < 3 )); then
    echo "error: unexpected empty Zano connectivity_tool link command" >&2
    exit 1
fi

CXX_BIN="${LINK_ARGS[0]}"

# Locate the output marker in the original link command. Everything after the
# connectivity_tool output is Zano's already-resolved library/linker tail.
OUTPUT_INDEX=-1
for ((i = 1; i + 1 < ${#LINK_ARGS[@]}; ++i)); do
    if [[ "${LINK_ARGS[i]}" == "-o" && "${LINK_ARGS[i + 1]}" == *"connectivity_tool" ]]; then
        OUTPUT_INDEX=$i
        break
    fi
done

if (( OUTPUT_INDEX < 0 )); then
    echo "error: could not locate connectivity_tool output in Zano link command" >&2
    printf '%s\n' "$LINK_LINE" >&2
    exit 1
fi

LINK_TAIL=("${LINK_ARGS[@]:OUTPUT_INDEX+2}")

SRC="$P2POOL_SOURCE/tools/zano_header_oracle.cpp"
OBJ="$OUT_DIR/zano_header_oracle.o"
BIN="$OUT_DIR/zano-header-oracle"

"$CXX_BIN" "${COMPILE_ARGS[@]}" \
    -I"$ZANO_SOURCE/src" \
    -c "$SRC" -o "$OBJ"

"$CXX_BIN" "$OBJ" -o "$BIN" "${LINK_TAIL[@]}"

printf 'Built: %s\n' "$BIN"
