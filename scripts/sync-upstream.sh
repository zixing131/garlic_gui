#!/bin/sh
# Pull upstream/main and fold it into fork/overlay.
#
# src/ stays identical to upstream. Local changes to those files live in
# fork/overlay/<same path>. This script merges upstream into the branch, then
# three-way-merges each overlay against the upstream revision recorded in
# fork/UPSTREAM_BASE. A clean run updates that record. Conflict markers stay
# inside fork/overlay and must be resolved before the next sync.

set -eu
cd "$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"

base_file=fork/UPSTREAM_BASE
if [ ! -f "$base_file" ]; then
    echo "missing $base_file" >&2
    exit 1
fi
if [ -n "$(git status --porcelain)" ]; then
    echo "working tree is not clean; commit or stash before syncing" >&2
    exit 1
fi

old=$(tr -d '[:space:]' < "$base_file")
git fetch upstream
git merge upstream/main --no-edit
new=$(git rev-parse upstream/main)

if [ "$old" = "$new" ]; then
    echo "upstream/main is already $new"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failed=0

find fork/overlay -type f ! -name 'UPSTREAM_BASE' | sort | while IFS= read -r overlay; do
    rel=${overlay#fork/overlay/}
    base="$tmp/base"
    theirs="$tmp/theirs"
    ours="$tmp/ours"
    if ! git cat-file -e "$old:$rel" 2>/dev/null; then
        echo "keep $rel (not in the previous upstream)"
        continue
    fi
    if ! git cat-file -e "$new:$rel" 2>/dev/null; then
        echo "CONFLICT $rel was removed upstream"
        failed=1
        continue
    fi
    git show "$old:$rel" > "$base"
    git show "$new:$rel" > "$theirs"
    cp "$overlay" "$ours"
    if git merge-file -L overlay -L upstream-base -L upstream "$ours" "$base" "$theirs"; then
        cp "$ours" "$overlay"
        echo "merged $rel"
    else
        cp "$ours" "$overlay"
        echo "CONFLICT $rel"
        failed=1
    fi
done

# The while loop above runs in a subshell because of the pipe. Recompute.
failed=0
find fork/overlay -type f ! -name 'UPSTREAM_BASE' | sort > "$tmp/list"
while IFS= read -r overlay; do
    if grep -q '^<<<<<<<' "$overlay"; then
        echo "unresolved $overlay"
        failed=1
    fi
done < "$tmp/list"

refresh_overlay_links() {
    find fork/overlay -type d | while IFS= read -r dir; do
        rel=${dir#fork/overlay/}
        [ -d "$rel" ] || continue
        for src in "$rel"/*; do
            [ -f "$src" ] || continue
            name=$(basename "$src")
            dest="$dir/$name"
            if [ ! -e "$dest" ] && [ ! -L "$dest" ]; then
                ln -s "$(python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' "$src" "$dir")" "$dest"
            fi
        done
    done
}

refresh_overlay_links

if [ "$failed" -ne 0 ]; then
    echo "upstream is merged, but fork/overlay still has conflicts." >&2
    echo "Resolve those files, then write $new to $base_file." >&2
    exit 1
fi

printf '%s\n' "$new" > "$base_file"
echo "synced fork/overlay to upstream $new"
