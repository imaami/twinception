#!/usr/bin/bash
# Trial 06: run trials 01-04 and an encoding check on big-endian and
# 32-bit machines under qemu-user. Each program is built freestanding
# with clang for each target against the minimal libc in fs/. Targets
# whose clang backend, lld or qemu is missing are skipped, so the trial
# is harmless in CI; it only runs with the clang toolchain.

source "$DSTR_TESTS/lib.sh"

here=$DSTR_TESTS/06-qemu
lvl=06

if [[ -n ${DSTR_CFLAGS_EXTRA-}${DSTR_LDFLAGS_EXTRA-}${DSTR_RUN_PREFIX-} ]]; then
	dstr_skip $lvl "freestanding builds can't use sanitizers or valgrind"
	exit 0
fi
if [[ $DSTR_TOOLCHAIN != clang ]]; then
	dstr_skip $lvl "cross builds use clang; run with the clang toolchain"
	exit 0
fi
if ! command -v ld.lld > /dev/null; then
	dstr_skip $lvl "ld.lld not found"
	exit 0
fi

# name|clang target|runner (- for none)|extra flags
targets=(
	'x86_64|x86_64-linux-gnu|-|'
	'i386|i386-linux-gnu|qemu-i386|'
	'aarch64_be|aarch64_be-linux-gnu|qemu-aarch64_be|'
	'ppc64|powerpc64-linux-gnu|qemu-ppc64|-mabi=elfv2'
	's390x|s390x-linux-gnu|qemu-s390x|'
	'ppc|powerpc-linux-gnu|qemu-ppc|'
	'armeb|armeb-linux-gnueabi|qemu-armeb|-march=armv7ve'
	'arm|arm-linux-gnueabi|qemu-arm|-march=armv7ve'
	'riscv64|riscv64-linux-gnu|qemu-riscv64|-mno-relax'
	'riscv32|riscv32-linux-gnu|qemu-riscv32|-mno-relax'
)
programs=(api alloc sizes model encode)

# target NAME TRIPLE RUNNER FLAGS: build and run every program for one
# target; result lines go to $DSTR_BUILD/NAME.results
target ()
{
	local name=$1 triple=$2 runner=$3 extra=$4
	local dir=$DSTR_BUILD/$name out=$DSTR_BUILD/$name.results
	mkdir -p "$dir"
	: > "$out"

	local cc=(clang --target="$triple" $extra -std=gnu23 -O2 -g
	          -ffreestanding -nostdlibinc -isystem "$here/fs/include"
	          -fno-stack-protector -fno-pic -DNDEBUG=1
	          -Wall -Wextra -Wpedantic -Werror
	          -I"$DSTR_LIB" -I"$DSTR_TESTS")
	local ld=(clang --target="$triple" $extra -nostdlib -static
	          -fuse-ld=lld)
	local run=()
	[[ $runner != - ]] && run=("$runner")

	# compile OUT ARGS...: compile into $dir/OUT, log to $dir/OUT.log
	compile ()
	{
		local o=$1
		shift
		"${cc[@]}" -c "$@" -o "$dir/$o" > "$dir/$o.log" 2>&1
	}

	# A trivial program must build and run, or the target is skipped
	if [[ $runner != - ]] && ! command -v "$runner" > /dev/null; then
		printf 'SKIP %s/%s: %s not found\n' $lvl "$name" "$runner" >> "$out"
		return
	fi
	printf 'int main (void) { return 0; }\n' > "$dir/probe.c"
	if ! compile libc.o "$here/fs/libc.c" ||
	   ! compile probe.o "$dir/probe.c" ||
	   ! "${ld[@]}" "$dir/probe.o" "$dir/libc.o" -o "$dir/probe" \
	     > "$dir/probe.log" 2>&1 ||
	   ! timeout 60 "${run[@]}" "$dir/probe" >> "$dir/probe.log" 2>&1; then
		printf 'SKIP %s/%s: cannot build or run a trivial program (%s)\n' \
		       $lvl "$name" "$dir/probe.log" >> "$out"
		return
	fi

	local sizes
	if ! mapfile -t sizes < <(dstr_sizes clang --target="$triple"); then
		printf 'FAIL %s/%s: cannot tell the pointer size\n' $lvl "$name" \
		       >> "$out"
		return
	fi

	local ok=1
	compile lib.o "$DSTR_LIB/dstr.c" || ok=0

	local prog s objs src defs suite
	for prog in "${programs[@]}"; do
		objs=()
		defs=()
		suite=1
		case $prog in
		api)    src=$DSTR_TESTS/01-api/api.c; suite=0 ;;
		alloc)  src=$DSTR_TESTS/04-alloc/alloc.c; suite=0 ;;
		sizes)  src=$DSTR_TESTS/02-sizes/sizes.c ;;
		model)  src=$DSTR_TESTS/03-model/model.c
		        defs=(-DMODEL_OPS=3000U) ;;
		encode) src=$here/encode.c ;;
		esac

		if (( !suite )); then
			compile "$prog.o" "$src" && objs=("$dir/$prog.o")
		else
			mkdir -p "$dir/$prog-inc"
			: > "$dir/$prog-inc/suites.inc"
			dstr_gen_literals "$dir/$prog-inc/literals.inc"
			for s in "${sizes[@]}"; do
				printf 'SUITE(%s_%s)\n' "$prog" "$s" \
				       >> "$dir/$prog-inc/suites.inc"
				compile "$prog-$s.o" -DDSTR_OBJ_SIZE="$s" \
				        -DSUITE="${prog}_$s" -I"$dir/$prog-inc" \
				        "${defs[@]}" "$src" &&
					objs+=("$dir/$prog-$s.o")
			done
			if (( ${#objs[@]} != ${#sizes[@]} )) ||
			   ! compile "$prog-main.o" -I"$dir/$prog-inc" \
			     "$DSTR_TESTS/suites_main.c"; then
				objs=()
			else
				objs+=("$dir/$prog-main.o")
			fi
		fi

		local res=$lvl/$name/$prog
		if (( !ok || ${#objs[@]} == 0 )) ||
		   ! "${ld[@]}" "${objs[@]}" "$dir/lib.o" "$dir/libc.o" \
		     -o "$dir/$prog" > "$dir/$prog.link.log" 2>&1; then
			printf 'FAIL %s: build failed (logs in %s)\n' "$res" "$dir" \
			       >> "$out"
			continue
		fi
		timeout "$DSTR_TIMEOUT" "${run[@]}" "$dir/$prog" \
		        > "$dir/$prog.run.log" 2>&1
		local rc=$?
		if (( rc == 0 )); then
			printf 'PASS %s\n' "$res" >> "$out"
		else
			printf 'FAIL %s: exit status %d (%s)\n' "$res" "$rc" \
			       "$dir/$prog.run.log" >> "$out"
		fi
	done
}

for t in "${targets[@]}"; do
	IFS='|' read -r name triple runner extra <<< "$t"
	dstr_spawn target "$name" "$triple" "$runner" "$extra"
done
wait

for t in "${targets[@]}"; do
	name=${t%%|*}
	cat "$DSTR_BUILD/$name.results"
done
