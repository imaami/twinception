#!/usr/bin/bash
# Trial 07: dstr() of a short string literal vanishes. With
# optimization, local objects initialized from short literals must fold
# into constants, even when they live across calls to outside functions:
# the object file may reference nothing but those functions. Without
# optimization, the literal must still be a compile-time initializer.

source "$DSTR_TESTS/lib.sh"

here=$DSTR_TESTS/07-vanish
lvl=07

if [[ -n ${DSTR_CFLAGS_EXTRA-} ]]; then
	dstr_skip $lvl "sanitizer instrumentation changes code generation"
	exit 0
fi
if ! command -v nm > /dev/null; then
	dstr_skip $lvl "nm not found"
	exit 0
fi

# vanished NAME SOURCE ALLOWED FLAGS...: compile SOURCE with FLAGS into
# an object and fail NAME if it references any symbol except those
# matching the regex ALLOWED
vanished ()
{
	local name=$1 src=$2 allowed=$3
	shift 3
	local obj=${name//\//_}.o
	if ! dstr_cc "$obj" -c "$@" "$src"; then
		dstr_fail "$name" "build failed" "$DSTR_BUILD/$obj.build.log"
		return
	fi
	local refs
	refs=$(nm -u "$DSTR_BUILD/$obj" | awk '{ print $NF }' |
	       grep -vE -- "$allowed")
	if [[ -n $refs ]]; then
		dstr_fail "$name" "still references: $(tr '\n' ' ' <<< "$refs")"
	else
		dstr_pass "$name"
	fi
}

for opt in -O2 -O3 -Os; do
	vanished "$lvl/fold$opt" "$here/fold.c" '^fold_sink$' "$opt"
done
vanished "$lvl/constant-O0" "$here/constant.c" '^$' -O0
