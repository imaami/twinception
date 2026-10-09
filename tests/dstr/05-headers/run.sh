#!/usr/bin/bash
# Trial 05: compile-time checks. Invalid object sizes must be rejected
# and legal ones accepted; internal helper macros must not leak out of
# the public headers; the constant macros must have the types they
# document; the headers must compile in every include order under every
# C standard with -Werror; and the library sources must compile as C23
# (C11 and C17 are not a goal for the sources yet).

source "$DSTR_TESTS/lib.sh"

here=$DSTR_TESTS/05-headers
gen=$DSTR_BUILD/gen
mkdir -p "$gen"

warn=(-Wall -Wextra -Wpedantic -Werror)
cflags=(-O2 "${warn[@]}" -I"$DSTR_LIB" -I"$here" -c -o /dev/null)
stds=(c11 c17 c23 gnu11 gnu17 gnu23)

if ! mapfile -t sizes < <(dstr_sizes "$DSTR_CC"); then
	dstr_fail 05/sizes "cannot tell the pointer size of $DSTR_CC"
	exit 0
fi

names=()
expects=()

# run_job TAG CMD...: run CMD with at most 4 GiB of address space (a
# compiler that runs away fails fast instead of eating the machine's
# memory), keep its output and exit status
run_job ()
{
	local tag=$1
	shift
	( ulimit -v 4194304 && exec "$@" ) > "$gen/$tag.log" 2>&1
	printf '%s\n' "$?" > "$gen/$tag.rc"
}

# job NAME EXPECT CMD...: queue a compile. EXPECT is "ok" if it must
# succeed, or an extended regex its diagnostics must match if it must
# fail.
job ()
{
	local name=$1 expect=$2
	shift 2
	names+=("$name")
	expects+=("$expect")
	dstr_spawn run_job "${name//\//_}" "$@"
}

# source_file NAME LINE...: write $gen/NAME.c from LINEs
source_file ()
{
	local name=$1
	shift
	printf '%s\n' "$@" > "$gen/$name.c"
}

# --- Include orders, every C standard --------------------------------

declare -A orders=(
	[dstr]='"dstr.h"'
	[twice]='"dstr.h" "dstr.h"'
	[system-around]='<stdio.h> "dstr.h" <stdlib.h> <string.h>'
	[constants-first]='"dstr_constants.h" "dstr.h"'
)
for order in $(printf '%s\n' "${!orders[@]}" | sort); do
	lines=()
	for h in ${orders[$order]}; do
		lines+=("#include $h")
	done
	lines+=('#include "use_dstr.inc"')
	source_file "order-$order" "${lines[@]}"
	for std in "${stds[@]}"; do
		job "05/order/$order/$std" ok \
		    "$DSTR_CC" -std="$std" "${cflags[@]}" "$gen/order-$order.c"
	done
done

# --- Strict user-side warnings ----------------------------------------

strict=(-Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wundef
        -Wstrict-prototypes -Wmissing-prototypes -Wcast-align
        -Wnull-dereference -Wdouble-promotion -Wformat=2)
case $("$DSTR_CC" --version 2> /dev/null) in
*clang*)
	# Everything, minus warnings about the user's own code style, and
	# -Wdisabled-macro-expansion: every use of a public constant such
	# as DSTR_IS_SSO expands the self-referential enumerator macro
	# of dstr_constants.h in the user's code, by design
	everything=(-Weverything -Wno-unsafe-buffer-usage
	            -Wno-declaration-after-statement -Wno-pre-c11-compat
	            -Wno-disabled-macro-expansion)
	;;
*)
	strict+=(-Wcast-align=strict -Wlogical-op -Wduplicated-cond
	         -Wjump-misses-init)
	everything=()
	;;
esac
job 05/strict ok "$DSTR_CC" -std=gnu23 "${cflags[@]}" "${strict[@]}" \
    "$gen/order-dstr.c"

# Some warnings only appear without optimization
for opt in -O0 -Og; do
	job "05/unoptimized$opt" ok "$DSTR_CC" -std=gnu23 "${cflags[@]}" \
	    "${strict[@]}" "$opt" "$gen/order-dstr.c"
done
if (( ${#everything[@]} )); then
	job 05/everything ok "$DSTR_CC" -std=gnu23 "${cflags[@]}" \
	    "${everything[@]}" "$gen/order-dstr.c"
fi

# --- Object sizes -------------------------------------------------------

declare -A legal=()
for s in "${sizes[@]}"; do
	legal[$s]=1
	job "05/size/$s" ok "$DSTR_CC" -std=gnu23 "${cflags[@]}" \
	    -DDSTR_OBJ_SIZE="$s" "$gen/order-dstr.c"
done
for s in 0 8 16 20 23 25 36 132 136 256 1000; do
	[[ -n ${legal[$s]} ]] && continue
	job "05/size/$s" 'is too (large|small)|is not a multiple' \
	    "$DSTR_CC" -std=gnu23 "${cflags[@]}" -DDSTR_OBJ_SIZE="$s" \
	    "$gen/order-dstr.c"
done

# --- Macro hygiene ------------------------------------------------------

# Every lowercase macro in the headers is an internal helper that the
# header must undefine, except the API macros dstr_auto and dstr and
# the macros they use at the user's site, transitively
mapfile -t helpers < <(
	awk '
	# Join continuation lines; record every macro and its replacement
	{ line = line $0 }
	/\\$/ { sub(/\\$/, " ", line); next }
	match(line, /^[[:space:]]*#[[:space:]]*define[[:space:]]+[A-Za-z_][A-Za-z0-9_]*/) {
		name = substr(line, RSTART, RLENGTH)
		sub(/.*define[[:space:]]+/, "", name)
		body[name] = body[name] " " substr(line, RSTART + RLENGTH)
	}
	{ line = "" }
	END {
		keep["dstr_auto"] = keep["dstr"] = 1
		do {
			added = 0
			for (k in keep) {
				b = body[k]
				while (match(b, /[A-Za-z_][A-Za-z0-9_]*/)) {
					id = substr(b, RSTART, RLENGTH)
					b = substr(b, RSTART + RLENGTH)
					if ((id in body) && !(id in keep)) {
						keep[id] = 1
						added = 1
					}
				}
			}
		} while (added)
		for (n in body)
			if (n ~ /^[a-z_]/ && !(n in keep))
				print n
	}' "$DSTR_LIB/dstr.h" "$DSTR_LIB/dstr_constants.h" | sort -u)
if (( ${#helpers[@]} < 10 )); then
	names+=(05/hygiene/list)
	expects+=(none)
	printf '1\n' > "$gen/05_hygiene_list.rc"
	printf 'found only %d helper macros\n' "${#helpers[@]}" \
	       > "$gen/05_hygiene_list.log"
fi
lines=('#include "dstr.h"')
for m in "${helpers[@]}"; do
	lines+=("#ifdef $m" "# error \"$m leaks out of dstr.h\"" "#endif")
done
for m in dstr_auto dstr DSTR_INIT DSTR_OBJ_SIZE DSTR_BLK_SIZE \
         DSTR_GROW_SHIFT_MAX DSTR_GROW_COUNT_MASK DSTR_IS_SSO; do
	lines+=("#ifndef $m" "# error \"$m is missing after dstr.h\"" "#endif")
done
source_file hygiene "${lines[@]}"
job 05/hygiene/dstr.h ok "$DSTR_CC" -std=gnu23 "${cflags[@]}" "$gen/hygiene.c"

# --- Constant types -----------------------------------------------------

# The macros paired with the enumerators cast them to the types they
# document: size_t, or unsigned char for the bits of the meta byte
lines=('#include "dstr.h"'
       '#define TYPE_IS(x, T) _Generic((x), T: 1, default: 0)')
for m in DSTR_BLK_SIZE DSTR_GROW_SHIFT_MAX DSTR_BLK_COUNT_POS \
         DSTR_UPTR_META_POS DSTR_MIN_SIZE DSTR_MAX_SIZE \
         DSTR_IS_NOT DSTR_IS_SSO DSTR_IS_PTR DSTR_IS_BAD; do
	lines+=("_Static_assert(TYPE_IS($m, size_t), \"$m is not size_t\");")
done
for m in DSTR_PTR_FLAG DSTR_GROW_COUNT_MASK; do
	lines+=("_Static_assert(TYPE_IS($m, unsigned char),"
	        "               \"$m is not unsigned char\");")
done
source_file types "${lines[@]}"
for std in "${stds[@]}"; do
	job "05/types/$std" ok "$DSTR_CC" -std="$std" "${cflags[@]}" "$gen/types.c"
done

# --- The library sources ------------------------------------------------

for std in c23 gnu23; do
	job "05/lib/dstr.c/$std" ok "$DSTR_CC" -std="$std" "${cflags[@]}" \
	    -DNDEBUG=1 "$DSTR_LIB/dstr.c"
	job "05/lib/dstr.c/$std/debug" ok "$DSTR_CC" -std="$std" \
	    "${cflags[@]}" -UNDEBUG "$DSTR_LIB/dstr.c"
done

wait

for i in "${!names[@]}"; do
	name=${names[i]}
	expect=${expects[i]}
	tag=${name//\//_}
	log=$gen/$tag.log
	rc=$(< "$gen/$tag.rc")
	if grep -qE 'out of memory|LLVM ERROR|internal compiler error' "$log"; then
		dstr_fail "$name" "the compiler crashed or ran out of memory" "$log"
	elif [[ $expect == ok ]]; then
		if [[ $rc == 0 ]]; then
			dstr_pass "$name"
		else
			dstr_fail "$name" "did not compile" "$log"
		fi
	elif [[ $rc == 0 ]]; then
		dstr_fail "$name" "compiled, expected an error matching /$expect/"
	elif grep -qE -- "$expect" "$log"; then
		dstr_pass "$name"
	else
		dstr_fail "$name" "failed without a diagnostic matching /$expect/" \
		          "$log"
	fi
done
