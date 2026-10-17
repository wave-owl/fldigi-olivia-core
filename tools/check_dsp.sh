#!/bin/sh
# Pin gate for the fldigi-dsp-core submodule dsp/ (wave-owl R-920.3, INV-920).
#
#   sh tools/check_dsp.sh [dsp.lock] [dsp dir]
#
# Refuses (exit 1) when dsp/ is missing, reports another version than dsp.lock, is
# checked out at another commit (when dsp/ is a git checkout), or when the content
# checksum of the linked files differs from dsp.lock. The checksum is the sha256 of the
# sorted "<sha256>  <path>" listing of the files named in dsp.lock, the same value that
# `make checksum` prints in fldigi-dsp-core.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
lock=${1:-dsp.lock}
dir=${2:-dsp}
fail() { echo "check_dsp: $*" >&2; exit 1; }
[ -f "$lock" ] || fail "$lock not found"
get() { sed -n "s/^$1=//p" "$lock" | head -n 1; }
want_version=$(get version)
want_commit=$(get commit)
want_sum=$(get checksum)
files=$(get files)
[ -n "$want_version" ] && [ -n "$want_sum" ] && [ -n "$files" ] || fail "$lock is incomplete"
[ -f "$dir/include/fldigi_dsp.h" ] || \
    fail "$dir/ is missing: run 'git submodule update --init --recursive'"
have_version=$(sed -n 's/^#define FDSP_VERSION_STRING "\(.*\)"/\1/p' "$dir/include/fldigi_dsp.h")
[ "$have_version" = "$want_version" ] || \
    fail "$dir/ is fldigi-dsp-core $have_version, $lock pins $want_version"
if [ -e "$dir/.git" ] && command -v git >/dev/null 2>&1; then
    have_commit=$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)
    if [ -n "$have_commit" ] && [ -n "$want_commit" ] && [ "$have_commit" != "$want_commit" ]; then
        fail "$dir/ is at commit $have_commit, $lock pins $want_commit"
    fi
fi
if command -v sha256sum >/dev/null 2>&1; then
    sha() { sha256sum "$@"; }
else
    sha() { shasum -a 256 "$@"; }
fi
have_sum=$(cd "$dir" && for f in $files; do echo "$f"; done | LC_ALL=C sort | \
    while read -r f; do sha "$f"; done | sha | cut -d' ' -f1)
[ "$have_sum" = "$want_sum" ] || \
    fail "$dir/ content checksum $have_sum differs from $lock ($want_sum)"
echo "check_dsp: fldigi-dsp-core $have_version ok (${have_commit:-no git metadata}, $have_sum)"
