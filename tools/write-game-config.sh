#!/bin/sh
# Writes <bin-dir>/config.json for dal_game when the game data is present and no config.json exists.
#
# usage: tools/write-game-config.sh --bin-dir <dir> --game-dir <dir> [--eboot-elf <file>]
#
# Exit codes: 0 success (including every "not written" / "left unchanged" report), 1 failure,
# 2 usage error.
set -eu
unset CDPATH

usage() {
    echo "usage: tools/write-game-config.sh --bin-dir <dir> --game-dir <dir> [--eboot-elf <file>]" >&2
}

usage_error() {
    echo "write-game-config: $1" >&2
    usage
    exit 2
}

# Lexical absolute path against the caller's directory; trailing separators are not significant.
abs_lexical() {
    case $1 in
        /*) p=$1 ;;
        *) p=$PWD/$1 ;;
    esac
    while [ "$p" != / ] && [ "${p%/}" != "$p" ]; do
        p=${p%/}
    done
    printf '%s\n' "$p"
}

# Absolute path of a directory without resolving symbolic links (as given, made absolute).
abs_dir() {
    if [ -d "$1" ]; then
        case $1 in
            /*) d=$1 ;;
            *) d=./$1 ;;
        esac
        (cd "$d" && pwd)
    else
        abs_lexical "$1"
    fi
}

# Canonical form for comparisons: symbolic links resolved when the directory exists.
comparable_dir() {
    if [ -d "$1" ]; then
        case $1 in
            /*) d=$1 ;;
            *) d=./$1 ;;
        esac
        (cd "$d" && pwd -P)
    else
        abs_lexical "$1"
    fi
}

bin_dir=
game_dir=
elf=
while [ $# -gt 0 ]; do
    case $1 in
        --bin-dir | --game-dir | --eboot-elf)
            [ $# -ge 2 ] || usage_error "$1 needs a value"
            case $1 in
                --bin-dir) bin_dir=$2 ;;
                --game-dir) game_dir=$2 ;;
                --eboot-elf) elf=$2 ;;
            esac
            shift 2
            ;;
        *) usage_error "unexpected argument: $1" ;;
    esac
done
[ -n "$bin_dir" ] || usage_error "--bin-dir is required"
[ -n "$game_dir" ] || usage_error "--game-dir is required"

bin_dir=$(abs_dir "$bin_dir")
game_dir=$(abs_dir "$game_dir")
if [ -n "$elf" ]; then
    case $elf in
        /*) ;;
        *) elf=$PWD/$elf ;;
    esac
fi

if [ ! -f "$game_dir/PS3_GAME/PARAM.SFO" ]; then
    echo "config.json not written: $game_dir has no PS3_GAME/PARAM.SFO"
    exit 0
fi

config=$bin_dir/config.json

if [ -e "$config" ] || [ -L "$config" ]; then
    # An existing file is the user's; it is never modified. The first lines say what it holds
    # (status, game_dir is a string, elf is a string, the elf option as a JSON string); the rest
    # is its game_dir.
    info=$(python3 -I - "$config" "$elf" <<'PY'
import json
import sys


def out(text):
    sys.stdout.buffer.write(text.encode("utf-8", "replace"))


path, elf = sys.argv[1], sys.argv[2]
try:
    with open(path, "rb") as stream:
        data = json.loads(stream.read().decode("utf-8-sig"))
except Exception:
    out("unparsable\n")
    sys.exit(0)
game_dir = data.get("game_dir") if isinstance(data, dict) else None
has_elf = isinstance(data, dict) and isinstance(data.get("elf"), str)
out("ok\n%d\n%d\n%s\n" % (isinstance(game_dir, str), has_elf, json.dumps(elf, ensure_ascii=False)))
out(game_dir if isinstance(game_dir, str) else "")
PY
)
    if [ "$(printf '%s\n' "$info" | sed -n 1p)" != ok ]; then
        echo "config.json left unchanged: $config cannot be parsed"
        exit 0
    fi
    game_is_string=$(printf '%s\n' "$info" | sed -n 2p)
    elf_is_string=$(printf '%s\n' "$info" | sed -n 3p)
    elf_json=$(printf '%s\n' "$info" | sed -n 4p)
    existing=$(printf '%s\n' "$info" | sed -n '5,$p')
    same=0
    if [ "$game_is_string" = 1 ] &&
        [ "$(comparable_dir "$existing")" = "$(comparable_dir "$game_dir")" ]; then
        same=1
    fi
    if [ "$same" = 0 ]; then
        echo "config.json left unchanged: game_dir in $config differs from $game_dir"
    elif [ -n "$elf" ] && [ "$elf_is_string" = 0 ]; then
        # Without elf, dal_game looks for EBOOT.elf in the game directory, not where the build put it.
        echo "config.json left unchanged: $config has no elf; add \"elf\": $elf_json"
    fi
    exit 0
fi

# sdata_cache and hdd_dir are left out: dal_game finds them next to bin/, and a written value would
# pin a path that a clean build deletes.
# The file is created exclusively (no overwrite) with LF line ends and no byte-order mark.
rc=0
python3 -I - "$config" "$game_dir" "$elf" <<'PY' || rc=$?
import json
import sys

path, game_dir, elf = sys.argv[1:4]
config = {"game_dir": game_dir}
if elf:
    config["elf"] = elf
text = json.dumps(config, indent=2, ensure_ascii=False) + "\n"
try:
    with open(path, "xb") as stream:
        stream.write(text.encode("utf-8", "surrogateescape"))
except FileExistsError:
    sys.exit(3)
except OSError as error:
    sys.stderr.write("write-game-config: %s\n" % error)
    sys.exit(1)
PY
case $rc in
    0) echo "wrote $config" ;;
    3) echo "config.json left unchanged: $config was created meanwhile" ;;
    *) exit 1 ;;
esac
