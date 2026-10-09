#!/usr/bin/bash
# Run the dstr test trials (tests/dstr/NN-name/run.sh) and summarize.

self=${0##*/}
here=$(cd "${0%/*}" && pwd)
root=$(cd "$here/../.." && pwd)

ncpu=$(nproc 2> /dev/null)
[[ $ncpu =~ ^[1-9][0-9]*$ ]] || ncpu=1
jobs_default=$(( ncpu < 4 ? ncpu : 4 ))

usage ()
{
	cat <<EOF
Usage: $self [OPTION]...
Run the dstr test trials in order and print one line per result.

  -l, --level PATTERN    run only trials whose name starts with PATTERN
                         (repeatable; default: every trial in tests/dstr/)
  -t, --toolchain CC     C compiler to test with, such as clang, gcc or
                         gcc-16 (repeatable; default: clang and gcc, if
                         found)
  -L, --lib DIR          library sources to test (default: $root/src)
  -b, --build DIR        build directory
                         (default: $root/build/tests/dstr)
  -s, --scale N          iteration multiplier for randomized tests
                         (default: 1)
  -j, --jobs N           parallel builds and runs within a trial
                         (default: $jobs_default)
  -m, --memory-max SIZE  memory limit for each trial, enforced with
                         systemd-run --user --scope when available;
                         0 disables it (default: 4G)
  -S, --sanitize LIST    build with -fsanitize=LIST, e.g.
                         address,undefined (default: none)
  -V, --valgrind         run the test programs under valgrind memcheck
                         (default: off)
  -h, --help             show this help and exit

Exit status: 0 if no test failed, 1 if any failed, 2 on usage errors.
EOF
}

opts=$(getopt -o l:t:L:b:s:j:m:S:Vh \
              -l level:,toolchain:,lib:,build:,scale:,jobs:,memory-max:,sanitize:,valgrind,help \
              -n "$self" -- "$@")
if (( $? != 0 )); then
	usage >&2
	exit 2
fi
eval set -- "$opts"

patterns=()
toolchains=()
lib=$root/src
build=$root/build/tests/dstr
scale=1
jobs=$jobs_default
memmax=4G
sanitize=
valgrind=0
while true; do
	case $1 in
	-l|--level)      patterns+=("$2"); shift 2 ;;
	-t|--toolchain)  toolchains+=("$2"); shift 2 ;;
	-L|--lib)        lib=$2; shift 2 ;;
	-b|--build)      build=$2; shift 2 ;;
	-s|--scale)      scale=$2; shift 2 ;;
	-j|--jobs)       jobs=$2; shift 2 ;;
	-m|--memory-max) memmax=$2; shift 2 ;;
	-S|--sanitize)   sanitize=$2; shift 2 ;;
	-V|--valgrind)   valgrind=1; shift ;;
	-h|--help)       usage; exit 0 ;;
	--)              shift; break ;;
	esac
done

if (( $# )); then
	printf '%s: unexpected argument: %s\n' "$self" "$1" >&2
	exit 2
fi
if [[ ! $scale =~ ^[1-9][0-9]*$ ]]; then
	printf '%s: --scale must be a positive integer\n' "$self" >&2
	exit 2
fi
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then
	printf '%s: --jobs must be a positive integer\n' "$self" >&2
	exit 2
fi
if [[ ! $memmax =~ ^(0|[1-9][0-9]*[KMGT]?)$ ]]; then
	printf '%s: --memory-max must be 0 or a size like 512M or 4G\n' \
	       "$self" >&2
	exit 2
fi
if [[ ! -f $lib/dstr.c || ! -f $lib/dstr.h ]]; then
	printf '%s: no dstr sources in %s\n' "$self" "$lib" >&2
	exit 2
fi
lib=$(cd "$lib" && pwd)

if (( ${#toolchains[@]} == 0 )); then
	command -v clang > /dev/null && toolchains+=(clang)
	command -v gcc > /dev/null && toolchains+=(gcc)
fi

levels=()
for dir in "$here"/[0-9][0-9]-*/; do
	name=${dir%/}
	name=${name##*/}
	[[ -f $dir/run.sh ]] || continue
	if (( ${#patterns[@]} )); then
		match=0
		for p in "${patterns[@]}"; do
			[[ $name == "$p"* ]] && match=1
		done
		(( match )) || continue
	fi
	levels+=("$name")
done

if (( ${#levels[@]} == 0 )); then
	printf '%s: no levels to run\n' "$self" >&2
	exit 2
fi

# Each trial runs in its own memory-limited scope, so that a runaway
# test is killed instead of the rest of the desktop
cap=()
if [[ $memmax != 0 ]]; then
	if systemd-run --user --scope -q -p MemoryMax=16M true \
	   > /dev/null 2>&1; then
		cap=(systemd-run --user --scope -q -p "MemoryMax=$memmax"
		     -p MemorySwapMax=0)
	else
		printf '%s: systemd-run --user --scope is unavailable;' "$self" >&2
		printf ' running without a memory limit\n' >&2
	fi
fi

cflags_extra=
ldflags_extra=
run_prefix=
if [[ -n $sanitize ]]; then
	cflags_extra="-fsanitize=$sanitize -fno-sanitize-recover=all"
	ldflags_extra="-fsanitize=$sanitize"
	# Huge requests must fail with NULL, not abort the program
	export ASAN_OPTIONS=${ASAN_OPTIONS:-allocator_may_return_null=1}
fi
if (( valgrind )); then
	run_prefix="valgrind -q --error-exitcode=99 --leak-check=full"
	run_prefix+=" --errors-for-leak-kinds=definite"
fi

total_pass=0
total_fail=0
total_skip=0
for cc in "${toolchains[@]}"; do
	if ! command -v "$cc" > /dev/null; then
		printf '%s: no such compiler: %s\n' "$self" "$cc" >&2
		exit 2
	fi
	tcname=${cc//[^A-Za-z0-9._+-]/_}
	for level in "${levels[@]}"; do
		dir=$build/$tcname/$level
		rm -rf "$dir"
		if ! mkdir -p "$dir"; then
			printf '%s: cannot create %s\n' "$self" "$dir" >&2
			exit 2
		fi
		printf '== %s [%s] ==\n' "$level" "$cc"
		out=$dir/results.txt
		DSTR_TESTS=$here DSTR_LIB=$lib DSTR_BUILD=$dir DSTR_LEVEL=$level \
		DSTR_TOOLCHAIN=$tcname DSTR_CC=$cc DSTR_SCALE=$scale \
		DSTR_JOBS=$jobs DSTR_CFLAGS_EXTRA=$cflags_extra \
		DSTR_LDFLAGS_EXTRA=$ldflags_extra DSTR_RUN_PREFIX=$run_prefix \
			"${cap[@]}" bash "$here/$level/run.sh" | tee "$out"
		rc=${PIPESTATUS[0]}
		pass=$(grep -c '^PASS ' "$out")
		fail=$(grep -c '^FAIL ' "$out")
		skip=$(grep -c '^SKIP ' "$out")
		if (( rc != 0 && fail == 0 )); then
			printf 'FAIL %s: trial script exited with %d\n' "$level" "$rc"
			fail=1
		fi
		printf -- '-- %s [%s]: %d passed, %d failed, %d skipped\n' \
		       "$level" "$cc" "$pass" "$fail" "$skip"
		(( total_pass += pass, total_fail += fail, total_skip += skip ))
	done
done

printf '== total: %d passed, %d failed, %d skipped\n' \
       "$total_pass" "$total_fail" "$total_skip"
(( total_fail == 0 ))
