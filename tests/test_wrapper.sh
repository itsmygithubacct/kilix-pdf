#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
wrapper=$root/kilix-pdf-viewer
temporary=$(mktemp -d "${TMPDIR:-/tmp}/kilix-pdf-wrapper.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

make_program() {
    path=$1
    status=$2
    marker=$3
    {
        # shellcheck disable=SC2016
        printf '%s\n' '#!/bin/sh'
        # shellcheck disable=SC2016
        printf 'printf "%%s\\n" %s >> "$KILIX_PDF_TEST_TRACE"\n' "$marker"
        # shellcheck disable=SC2016
        printf 'printf "%%s\\n" "$@" >> "$KILIX_PDF_TEST_TRACE"\n'
        printf 'exit %s\n' "$status"
    } >"$path"
    chmod 700 "$path"
}

trace=$temporary/trace
core=$temporary/core
evince=$temporary/evince
export KILIX_PDF_TEST_TRACE="$trace"
export KILIX_PDF_VIEWER_CORE="$core"
export KILIX_PDF_VIEWER_EVINCE="$evince"

make_program "$core" 0 core-ok
make_program "$evince" 7 evince
: >"$trace"
"$wrapper" document.pdf
test "$(sed -n '1p' "$trace")" = core-ok
test "$(sed -n '2p' "$trace")" = document.pdf
test "$(wc -l <"$trace")" -eq 2

make_program "$core" 65 core-data-error
: >"$trace"
set +e
"$wrapper" broken.pdf
status=$?
set -e
test "$status" -eq 65
test "$(sed -n '1p' "$trace")" = core-data-error
test "$(wc -l <"$trace")" -eq 2

make_program "$core" 69 core-unavailable
: >"$trace"
set +e
"$wrapper" fallback.pdf
status=$?
set -e
test "$status" -eq 7
test "$(sed -n '1p' "$trace")" = core-unavailable
test "$(sed -n '3p' "$trace")" = evince
test "$(sed -n '4p' "$trace")" = fallback.pdf

: >"$trace"
set +e
"$wrapper" --no-fallback no-gui.pdf
status=$?
set -e
test "$status" -eq 69
test "$(sed -n '1p' "$trace")" = core-unavailable
test "$(wc -l <"$trace")" -eq 2

export KILIX_PDF_VIEWER_CORE="$temporary/missing"
: >"$trace"
set +e
"$wrapper" missing-core.pdf
status=$?
set -e
test "$status" -eq 7
test "$(sed -n '1p' "$trace")" = evince
test "$(sed -n '2p' "$trace")" = missing-core.pdf

printf '%s\n' '5 wrapper tests passed'
