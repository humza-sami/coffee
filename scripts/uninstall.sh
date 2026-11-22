#!/bin/sh
# Remove a Coffee installation by following its install manifest.
#   uninstall.sh --prefix DIR [--dry-run]
# Removes only regular files recorded in DIR/share/coffee/install-manifest.txt whose SHA-256 still matches; edited files are
# kept. All directories, including empty user directories, are kept. A symlinked directory below the prefix on the way to a manifest entry is
# never followed: the whole run is refused before anything is deleted. State directories (coffee init --state), Bean work
# directories, results and artifacts are never touched. The prefix itself may be a symlink (it is resolved once and used as anchor).
set -eu
prefix=""
dry=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) [ $# -ge 2 ] || { echo "uninstall: --prefix needs a value" >&2; exit 2; }; prefix=$2; shift 2 ;;
    --dry-run) dry=1; shift ;;
    -h|--help) echo "usage: uninstall.sh --prefix DIR [--dry-run]"; exit 0 ;;
    *) echo "uninstall: unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$prefix" ] || { echo "uninstall: --prefix DIR is required" >&2; exit 2; }
prefix=$(cd -- "$prefix" 2>/dev/null && pwd -P) || { echo "uninstall: prefix is not an accessible directory" >&2; exit 1; }
manifest="$prefix/share/coffee/install-manifest.txt"

# Succeeds only if no directory component of "$1" (relative to the prefix, excluding the final name) is a symlink.
no_symlink_parents() {
  rest=$1; cur=$prefix
  while :; do
    case $rest in
      */*) c=${rest%%/*}; rest=${rest#*/} ;;
      *) return 0 ;;
    esac
    cur=$cur/$c
    if [ -L "$cur" ]; then echo "uninstall: '$cur' is a symbolic link; refusing to follow it" >&2; return 1; fi
  done
}

[ ! -L "$manifest" ] || { echo "uninstall: manifest is a symbolic link; nothing removed" >&2; exit 1; }
no_symlink_parents "share/coffee/install-manifest.txt" || { echo "uninstall: unsafe prefix layout; nothing removed" >&2; exit 1; }
[ -f "$manifest" ] || { echo "uninstall: no manifest at $manifest; nothing removed" >&2; exit 1; }
[ "$(sed -n 1p "$manifest")" = "# coffee install manifest v1" ] || { echo "uninstall: unrecognised manifest format; nothing removed" >&2; exit 1; }

# Validate EVERY entry (format, path shape, symlinked ancestors) before touching anything.
bad=0; entries=0
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in ''|'#'*) continue ;; esac
  case "$line" in
    *"  "*) ;;
    *) echo "uninstall: malformed manifest line: $line; nothing removed" >&2; exit 1 ;;
  esac
  rel=${line#*  }
  digest=${line%%  *}
  case "$rel" in
    ''|/*|..|../*|*/..|*/../*|.|./*|*/.|*/./*|*//*) echo "uninstall: unsafe path in manifest: $rel" >&2; bad=1; continue ;;
  esac
  case "$digest" in *[!0-9a-f]*) echo "uninstall: malformed digest in manifest line: $line" >&2; bad=1 ;; esac
  [ ${#digest} -eq 64 ] || { echo "uninstall: malformed digest in manifest line: $line" >&2; bad=1; }
  entries=$((entries+1))
  no_symlink_parents "$rel" || { echo "uninstall: nothing removed" >&2; exit 1; }
done < "$manifest"
for d in share/coffee share/doc/coffee; do
  no_symlink_parents "$d/x" || { echo "uninstall: nothing removed" >&2; exit 1; }
  if [ -L "$prefix/$d" ]; then echo "uninstall: '$prefix/$d' is a symbolic link; refusing to follow it; nothing removed" >&2; exit 1; fi
done
[ "$entries" -gt 0 ] || bad=1
[ "$bad" -eq 0 ] || { echo "uninstall: manifest or prefix layout rejected; nothing removed" >&2; exit 1; }

removed=0; kept=0; missing=0
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in ''|'#'*) continue ;; esac
  rel=${line#*  }
  digest=${line%%  *}
  f="$prefix/$rel"
  if [ -L "$f" ]; then echo "kept (symlink, not installed by Coffee): $rel"; kept=$((kept+1)); continue; fi
  if [ ! -e "$f" ]; then missing=$((missing+1)); continue; fi
  have=$(sha256sum "$f" | cut -d' ' -f1)
  if [ "$have" != "$digest" ]; then echo "kept (modified since install): $rel"; kept=$((kept+1)); continue; fi
  if [ "$dry" -eq 1 ]; then echo "would remove: $rel"; else rm -f -- "$f"; echo "removed: $rel"; fi
  removed=$((removed+1))
done < "$manifest"

if [ "$dry" -eq 0 ]; then
  rm -f -- "$manifest"
  # The manifest records files only; no directory ownership is established. Leave all directories intact.
fi
echo "uninstall summary: removed=$removed kept=$kept already-missing=$missing dry-run=$dry"
echo "State directories, Bean work directories and result/artifact files were not touched."
