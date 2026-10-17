#!/bin/sh
# Installs (or removes) the staged Cell PPU language module for Ghidra.
#
# Copies the module built by `python3 tools/ps3tool.py ghidra-cell-lang build` into a place Ghidra
# loads modules from. Installing is optional: it is only needed for the Ghidra GUI and for runs
# outside this repository's tools. The script lists what it will change and asks before writing; it
# writes only inside the destination (plus the missing `Extensions` folder of the settings
# directory). Running it again with nothing to change prints `up to date` and touches nothing.
#
# Ghidra aborts at startup when a module named CellPPU exists in more than one place, so the script
# refuses to install while another copy exists.
#
# usage: tools/install-ghidra-cell.sh [--ghidra-dir <dir>] [--target user|processors]
#        [--settings-dir <dir>] [--source <dir>] [--uninstall] [--yes] [--dry-run]
#
#   --ghidra-dir <dir>    The Ghidra install directory. Defaults to $GHIDRA_INSTALL_DIR. There is no
#                         disk search.
#   --target user         (default) <settings>/Extensions/CellPPU, which survives a replacement of
#                         the install directory.
#   --target processors   <ghidra-dir>/Ghidra/Processors/CellPPU.
#   --settings-dir <dir>  Ghidra's user settings directory for this version. The default follows
#                         Ghidra's own rule: XDG_CONFIG_HOME (with `<username>-ghidra` instead of
#                         `ghidra` when it lies outside the home directory), else ~/Library on
#                         macOS, ~/.config elsewhere, then ghidra/ghidra_<version>_<release>. For
#                         the user target it must already exist (start Ghidra once).
#   --source <dir>        The staged module. Defaults to generated/ghidra/cell/CellPPU in this
#                         repository.
#   --uninstall           Removes the installed module instead of installing it.
#   --yes                 Do not ask before writing.
#   --dry-run             Print the plan and change nothing.
#
# Without --yes the script asks `proceed? [y/N]` on stdin; end of input or any answer but y/yes
# cancels.
#
# Exit codes: 0 success (including up to date, cancelled and --dry-run), 1 failure, 2 usage error.
set -eu
unset CDPATH

module_name=CellPPU
language_id=PowerPC:BE:64:CellPPU-32addr
build_hint='run python3 tools/ps3tool.py ghidra-cell-lang build'

usage() {
    cat >&2 <<'EOF'
usage: tools/install-ghidra-cell.sh [--ghidra-dir <dir>] [--target user|processors]
                                    [--settings-dir <dir>] [--source <dir>] [--uninstall]
                                    [--yes] [--dry-run]
EOF
}

usage_error() {
    echo "install-ghidra-cell: $1" >&2
    usage
    exit 2
}

die() {
    echo "install-ghidra-cell: $1" >&2
    exit 1
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

# Absolute path of a directory, symbolic links kept; a missing directory is only made absolute.
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

# True when $2 lies below directory $1.
is_inside() {
    case $2 in
        "${1%/}"/*) return 0 ;;
    esac
    return 1
}

exists() {
    [ -e "$1" ] || [ -L "$1" ]
}

# prop FILE KEY: the value of KEY in a Java properties file (last one wins; `#`/`!` comments, `=`,
# `:` or blanks as separator, value trimmed). Empty when missing.
prop() {
    awk -v key="$2" '
        {
            line = $0
            sub(/^[ \t\r]+/, "", line)
            sub(/[ \t\r]+$/, "", line)
            if (line == "" || line ~ /^[#!]/) next
            if (match(line, /^[^=: \t]+/)) {
                name = substr(line, 1, RLENGTH)
                rest = substr(line, RLENGTH + 1)
                sub(/^[ \t]*[=:]?[ \t]*/, "", rest)
                if (name == key) value = rest
            }
        }
        END { print value }
    ' "$1"
}

trim() {
    printf '%s\n' "$1" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'
}

# env_dir NAME VALUE, like Ghidra's getEnvFile: the value is trimmed, a blank one counts as unset, a
# relative one is an error. Prints nothing for an unset variable.
env_dir() {
    value=$(trim "$2")
    [ -n "$value" ] || return 0
    case $value in
        /*) abs_lexical "$value" ;;
        *) die "$1 must be an absolute path (got '$value'); fix it or pass --settings-dir" ;;
    esac
}

# Ghidra's SystemUtilities.getCleanUserName: no spaces, no leading DOMAIN\ or path part.
clean_user_name() {
    name=${USER:-}
    [ -n "$name" ] || name=$(id -un)
    printf '%s\n' "$name" | sed 's| ||g; s|^.*[\\/]||'
}

# Mirrors the environment part of Ghidra's settings directory rule.
default_settings_dir() {
    leaf=ghidra_$1_$2
    home_dir=${HOME:-}
    [ -n "$home_dir" ] || die "cannot determine the home directory; pass --settings-dir"
    home_dir=$(abs_lexical "$home_dir")
    xdg=$(env_dir XDG_CONFIG_HOME "${XDG_CONFIG_HOME:-}") || exit 1
    if [ -n "$xdg" ]; then
        if [ "$xdg" = "$home_dir" ] || is_inside "$home_dir" "$xdg"; then
            leaf_parent=ghidra
        else
            leaf_parent=$(clean_user_name)-ghidra
        fi
        printf '%s\n' "$xdg/$leaf_parent/$leaf"
    elif [ "$(uname -s)" = Darwin ]; then
        printf '%s\n' "$home_dir/Library/ghidra/$leaf"
    else
        printf '%s\n' "$home_dir/.config/ghidra/$leaf"
    fi
}

# Refuses a tree that holds a symbolic link: copying or deleting it could leave the tree.
reject_links() {
    link=$(find "$1" -type l | head -n 1)
    if [ -n "$link" ]; then
        die "refusing to use $1: $link is a symbolic link or junction"
    fi
}

# Sorted relative paths of every file below directory $1, written to file $2.
list_files() {
    (cd "$1" && find . -type f) | sed 's|^\./||' | LC_ALL=C sort >"$2"
}

count_lines() {
    awk 'END { print NR }' "$1"
}

confirm() {
    # --dry-run wins over --yes.
    if [ "$dry_run" = 1 ]; then
        return 1
    fi
    if [ "$assume_yes" = 1 ]; then
        return 0
    fi
    printf 'proceed? [y/N] '
    read -r answer || answer=
    case $(printf '%s' "$answer" | tr 'A-Z' 'a-z') in
        y | yes) return 0 ;;
    esac
    return 1
}

ghidra_dir=${GHIDRA_INSTALL_DIR:-}
target=user
settings_dir=
source_dir=
uninstall=0
assume_yes=0
dry_run=0
while [ $# -gt 0 ]; do
    case $1 in
        --uninstall)
            uninstall=1
            shift
            ;;
        --yes)
            assume_yes=1
            shift
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        --ghidra-dir | --target | --settings-dir | --source)
            [ $# -ge 2 ] || usage_error "$1 needs a value"
            case $1 in
                --ghidra-dir) ghidra_dir=$2 ;;
                --target) target=$2 ;;
                --settings-dir) settings_dir=$2 ;;
                --source) source_dir=$2 ;;
            esac
            shift 2
            ;;
        *) usage_error "unexpected argument: $1" ;;
    esac
done
target=$(printf '%s' "$target" | tr 'A-Z' 'a-z')
case $target in
    user | processors) ;;
    *) usage_error "--target must be user or processors (got '$target')" ;;
esac

work=
cleanup() {
    if [ -n "$work" ]; then
        rm -rf "$work"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

[ -n "$(trim "$ghidra_dir")" ] || die "pass --ghidra-dir or set GHIDRA_INSTALL_DIR"
ghidra_root=$(abs_dir "$ghidra_dir")
app_props_file=$ghidra_root/Ghidra/application.properties
power_pc_dir=$ghidra_root/Ghidra/Processors/PowerPC
if [ ! -f "$app_props_file" ] || [ ! -d "$power_pc_dir" ]; then
    die "not a Ghidra install: $ghidra_root"
fi
version=$(prop "$app_props_file" application.version)
release=$(prop "$app_props_file" application.release.name)
if [ -z "$version" ] || [ -z "$release" ]; then
    die "application.version or application.release.name is missing in $app_props_file"
fi

if [ -n "$settings_dir" ]; then
    settings=$(abs_lexical "$settings_dir")
else
    settings=$(default_settings_dir "$version" "$release") || exit 1
fi

user_dest=$settings/Extensions/$module_name
proc_dest=$ghidra_root/Ghidra/Processors/$module_name
ghidra_ext_dest=$ghidra_root/Ghidra/Extensions/$module_name
if [ "$target" = user ]; then
    root=$settings
    dest=$user_dest
    other_a=$proc_dest
    other_b=$ghidra_ext_dest
else
    root=$ghidra_root
    dest=$proc_dest
    other_a=$user_dest
    other_b=$ghidra_ext_dest
fi

echo "ghidra: $version $release ($ghidra_root)"
echo "target: $dest"

# Safety checks come before any write.
if [ "$target" = user ] && [ "$uninstall" = 0 ] && [ ! -d "$settings" ]; then
    die "settings directory not found: $settings; start Ghidra once or pass --settings-dir"
fi
walk=$root
relative=${dest#"${root%/}"/}
old_ifs=$IFS
IFS=/
set -f
for segment in $relative; do
    walk=${walk%/}/$segment
    exists "$walk" || break
    if [ -L "$walk" ]; then
        die "refusing to write through a symbolic link or junction: $walk"
    fi
done
set +f
IFS=$old_ifs

dest_exists=0
if exists "$dest"; then
    dest_exists=1
    dest_props=$dest/extension.properties
    if [ ! -d "$dest" ] || [ ! -f "$dest_props" ] || [ "$(prop "$dest_props" name)" != "$module_name" ]; then
        die "refusing to touch $dest: extension.properties does not name $module_name"
    fi
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/install-ghidra-cell.XXXXXX")

if [ "$uninstall" = 1 ]; then
    if [ "$dest_exists" = 0 ]; then
        echo "not installed: $dest"
        exit 0
    fi
    reject_links "$dest"
    list_files "$dest" "$work/dest"
    echo "plan: remove $(count_lines "$work/dest") files"
    if ! confirm; then
        echo "cancelled: nothing changed"
        exit 0
    fi
    rm -rf "$dest"
    echo "uninstalled: $dest"
    exit 0
fi

for other in "$other_a" "$other_b"; do
    if exists "$other"; then
        die "$module_name already exists at $other; Ghidra cannot load two copies; uninstall it first"
    fi
done

if [ -z "$source_dir" ]; then
    script_dir=$(cd "$(dirname "$0")" && pwd)
    source_dir=$(cd "$script_dir/.." && pwd)/generated/ghidra/cell/$module_name
fi
source_dir=$(abs_lexical "$source_dir")
source_props=$source_dir/extension.properties
if [ ! -f "$source_props" ]; then
    die "staged module not found or not built: $source_dir; $build_hint"
fi
if [ "$(prop "$source_props" name)" != "$module_name" ]; then
    die "$source_dir is not a $module_name module (extension.properties name); $build_hint"
fi
source_version=$(prop "$source_props" version)
if [ "$source_version" != "$version" ]; then
    die "staged module is for Ghidra '$source_version' but the target is $version; $build_hint"
fi
if [ ! -f "$source_dir/data/languages/ppc_cell.sla" ]; then
    die "staged module has no compiled ppc_cell.sla: $source_dir; $build_hint"
fi
if [ "$source_dir" = "$dest" ] || is_inside "$source_dir" "$dest" || is_inside "$dest" "$source_dir"; then
    die "the source and the destination overlap: $source_dir"
fi

reject_links "$source_dir"
list_files "$source_dir" "$work/source"
if [ "$dest_exists" = 1 ]; then
    reject_links "$dest"
    list_files "$dest" "$work/dest"
else
    : >"$work/dest"
fi
LC_ALL=C comm -23 "$work/source" "$work/dest" >"$work/added"
LC_ALL=C comm -13 "$work/source" "$work/dest" >"$work/removed"
LC_ALL=C comm -12 "$work/source" "$work/dest" >"$work/common"
: >"$work/updated"
unchanged=0
while IFS= read -r rel; do
    if cmp -s "$source_dir/$rel" "$dest/$rel"; then
        unchanged=$((unchanged + 1))
    else
        printf '%s\n' "$rel" >>"$work/updated"
    fi
done <"$work/common"

added=$(count_lines "$work/added")
updated=$(count_lines "$work/updated")
removed=$(count_lines "$work/removed")
if [ $((added + updated + removed)) -eq 0 ]; then
    echo "up to date: $dest"
    exit 0
fi

echo "plan: added $added, updated $updated, removed $removed, unchanged $unchanged"
while IFS= read -r rel; do echo "  added $rel"; done <"$work/added"
while IFS= read -r rel; do echo "  updated $rel"; done <"$work/updated"
while IFS= read -r rel; do echo "  removed $rel"; done <"$work/removed"

if ! confirm; then
    echo "cancelled: nothing changed"
    exit 0
fi

# The .sla goes last so an interrupted copy never leaves a compiled file that is newer than its
# sources.
cat "$work/added" "$work/updated" >"$work/to_copy"
{
    awk '!/\.sla$/' "$work/to_copy"
    awk '/\.sla$/' "$work/to_copy"
} >"$work/ordered"
while IFS= read -r rel; do
    mkdir -p "$(dirname "$dest/$rel")"
    cp "$source_dir/$rel" "$dest/$rel"
done <"$work/ordered"
while IFS= read -r rel; do
    rm -f "$dest/$rel"
done <"$work/removed"
find "$dest" -depth -type d | while IFS= read -r dir; do
    [ "$dir" = "$dest" ] || rmdir "$dir" 2>/dev/null || :
done

# Ghidra recompiles a .sla that is older than its sources, so make it the newest file.
python3 -I - "$dest" <<'PY'
import os
import sys
import time

dest = sys.argv[1]
newest = 0
slas = []
for directory, _, names in os.walk(dest):
    for name in names:
        path = os.path.join(directory, name)
        if name.endswith(".sla"):
            slas.append(path)
        else:
            newest = max(newest, os.stat(path).st_mtime_ns)
stamp = time.time_ns()
if stamp <= newest:
    stamp = newest + 1_000_000_000
for path in slas:
    os.utime(path, ns=(stamp, stamp))
PY

echo "installed: restart Ghidra to load $language_id"
exit 0
