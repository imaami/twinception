#!/usr/bin/bash
# Trial 02: boundary sweep at every legal object size, all sizes linked
# into one program (and once more with LTO).

source "$DSTR_TESTS/lib.sh"

here=$DSTR_TESTS/02-sizes

if ! mapfile -t sizes < <(dstr_sizes "$DSTR_CC"); then
	dstr_fail 02/sizes "cannot tell the pointer size of $DSTR_CC"
	exit 0
fi

: > "$DSTR_BUILD/suites.inc"
for s in "${sizes[@]}"; do
	printf 'SUITE(sizes_%s)\n' "$s" >> "$DSTR_BUILD/suites.inc"
done
dstr_gen_literals "$DSTR_BUILD/literals.inc"

# variant NAME FLAGS...: compile every size and link one program
variant ()
{
	local name=$1
	shift
	local objs=() s
	for s in "${sizes[@]}"; do
		dstr_spawn dstr_cc "$name-$s.o" -c "$@" -DDSTR_OBJ_SIZE="$s" \
		           -DSUITE="sizes_$s" -I"$DSTR_BUILD" "$here/sizes.c"
		objs+=("$DSTR_BUILD/$name-$s.o")
	done
	dstr_spawn dstr_cc "$name-lib.o" -c "$@" "$DSTR_LIB/dstr.c"
	dstr_spawn dstr_cc "$name-main.o" -c "$@" -I"$DSTR_BUILD" \
	           "$DSTR_TESTS/suites_main.c"
	wait
	dstr_cc "$name" "$@" "${objs[@]}" "$DSTR_BUILD/$name-lib.o" \
	        "$DSTR_BUILD/$name-main.o"
}

variant sizes
dstr_check 02/sizes sizes

variant sizes-lto -flto
dstr_check 02/sizes/lto sizes-lto
