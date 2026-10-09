#!/usr/bin/bash
# Trial 03: seeded random operations checked against a reference model
# after every step, at every legal object size. HELL_SEED replays a run.

source "$DSTR_TESTS/lib.sh"

here=$DSTR_TESTS/03-model

if ! mapfile -t sizes < <(dstr_sizes "$DSTR_CC"); then
	dstr_fail 03/model "cannot tell the pointer size of $DSTR_CC"
	exit 0
fi

: > "$DSTR_BUILD/suites.inc"
objs=()
for s in "${sizes[@]}"; do
	printf 'SUITE(model_%s)\n' "$s" >> "$DSTR_BUILD/suites.inc"
	dstr_spawn dstr_cc "model-$s.o" -c -DDSTR_OBJ_SIZE="$s" \
	           -DSUITE="model_$s" "$here/model.c"
	objs+=("$DSTR_BUILD/model-$s.o")
done
dstr_spawn dstr_cc lib.o -c "$DSTR_LIB/dstr.c"
dstr_spawn dstr_cc main.o -c -I"$DSTR_BUILD" "$DSTR_TESTS/suites_main.c"
wait

dstr_cc model "${objs[@]}" "$DSTR_BUILD/lib.o" "$DSTR_BUILD/main.o"
dstr_check 03/model model
HELL_SEED=20261008 dstr_check 03/model/seed2 model
