# Helpers for the trial scripts in tests/dstr/*/run.sh. Source this
# file; tests/dstr/run.sh sets these variables before it runs a trial:
#
#   DSTR_TESTS      the test tree, tests/dstr
#   DSTR_LIB        directory with the library sources under test
#   DSTR_BUILD      this trial's build directory, empty at start
#   DSTR_LEVEL      trial directory name, e.g. 01-api
#   DSTR_TOOLCHAIN  toolchain name, e.g. clang or gcc
#   DSTR_CC         C compiler
#   DSTR_SCALE      iteration multiplier for randomized tests (>= 1)
#   DSTR_JOBS       how many builds or runs a trial may start at once
#
# Set by tests/dstr/run.sh for sanitizer and valgrind runs:
#
#   DSTR_CFLAGS_EXTRA   appended to every compile
#   DSTR_LDFLAGS_EXTRA  appended to every link
#   DSTR_RUN_PREFIX     command words put in front of every dstr_check
#                       run
#   DSTR_WERROR         -Werror by default; set it empty to allow
#                       warnings
#
# Result protocol: a trial prints exactly one line per test result to
# stdout, "PASS name", "FAIL name: reason" or "SKIP name: reason".
# Everything else goes to log files in $DSTR_BUILD or to stderr.

DSTR_TIMEOUT=${DSTR_TIMEOUT:-300}
DSTR_WERROR=${DSTR_WERROR--Werror}
DSTR_JOBS=${DSTR_JOBS:-1}

dstr_cflags=(-std=gnu23 -O2 -g -fno-omit-frame-pointer -DNDEBUG=1
             -Wall -Wextra -Wpedantic $DSTR_WERROR
             -I"$DSTR_LIB" -I"$DSTR_TESTS")
dstr_lib_sources=("$DSTR_LIB/dstr.c")

# The allocation shim (tests/dstr/shim.h): link its source and these
# flags
dstr_shim_sources=("$DSTR_TESTS/shim.c")
dstr_shim_ldflags=(-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free)

# Word-split the extra flags on purpose: they are flag lists
read -r -a dstr_cflags_extra <<< "${DSTR_CFLAGS_EXTRA-}"
read -r -a dstr_ldflags_extra <<< "${DSTR_LDFLAGS_EXTRA-}"
read -r -a dstr_run_prefix <<< "${DSTR_RUN_PREFIX-}"

dstr_pass () { printf 'PASS %s\n' "$1"; }
dstr_skip () { printf 'SKIP %s: %s\n' "$1" "$2"; }

# dstr_fail NAME REASON [LOG]: report a failure; the last lines of LOG,
# if given, go to stderr for context.
dstr_fail ()
{
	printf 'FAIL %s: %s\n' "$1" "$2"
	if [[ -n $3 && -f $3 ]]; then
		printf '  | %s\n' "--- $1 (tail of $3)" >&2
		tail -n 20 "$3" | sed 's/^/  | /' >&2
	fi
}

# dstr_spawn CMD...: run CMD in the background, keeping at most
# DSTR_JOBS background jobs. Collect them with a plain 'wait'.
dstr_spawn ()
{
	while (( $(jobs -rp | wc -l) >= DSTR_JOBS )); do
		wait -n
	done
	"$@" &
}

# dstr_sizes CC [FLAGS...]: print the legal DSTR_OBJ_SIZE values for
# the target of CC, from 3 * sizeof (uintptr_t) to 128 in steps of
# sizeof (uintptr_t). Returns nonzero if the compiler can't tell.
dstr_sizes ()
{
	local cc=$1
	shift
	local p
	p=$("$cc" "$@" -dM -E -x c /dev/null 2> /dev/null |
	    sed -n 's/^#define __SIZEOF_POINTER__ \([0-9]*\)$/\1/p')
	[[ $p =~ ^[1-9][0-9]*$ ]] || return 1
	local s
	for (( s = 3 * p; s <= 128; s += p )); do
		printf '%s\n' "$s"
	done
}

# dstr_gen_literals FILE: write lines LITERAL(n, "...") for n from 0 to
# 300, each with a string literal of n letters, a to z repeated
dstr_gen_literals ()
{
	local abc=abcdefghijklmnopqrstuvwxyz text= n
	for (( n = 0; n <= 300; ++n )); do
		printf 'LITERAL(%d, "%s")\n' "$n" "$text"
		text+=${abc:n % 26:1}
	done > "$1"
}

# dstr_cc OUT ARGS...: compile and link C sources into $DSTR_BUILD/OUT
# with the base flags, ARGS and the extra flags. The compiler output
# goes to $DSTR_BUILD/OUT.build.log. Returns the compiler's exit status.
dstr_cc ()
{
	local out=$1
	shift
	local log=$DSTR_BUILD/$out.build.log
	printf '%q ' "$DSTR_CC" "${dstr_cflags[@]}" "$@" "${dstr_cflags_extra[@]}" \
	       "${dstr_ldflags_extra[@]}" -o "$DSTR_BUILD/$out" > "$log"
	printf '\n' >> "$log"
	"$DSTR_CC" "${dstr_cflags[@]}" "$@" "${dstr_cflags_extra[@]}" \
	           "${dstr_ldflags_extra[@]}" -o "$DSTR_BUILD/$out" >> "$log" 2>&1
}

# dstr_check NAME BIN [ARGS...]: run $DSTR_BUILD/BIN (with
# DSTR_RUN_PREFIX in front) and report PASS on exit status 0, FAIL
# otherwise. Output goes to $DSTR_BUILD/NAME.run.log.
dstr_check ()
{
	local name=$1 bin=$2
	shift 2
	local log=$DSTR_BUILD/${name//\//_}.run.log
	if [[ ! -x $DSTR_BUILD/$bin ]]; then
		dstr_fail "$name" "build failed" "$DSTR_BUILD/$bin.build.log"
		return 1
	fi
	timeout "$DSTR_TIMEOUT" "${dstr_run_prefix[@]}" "$DSTR_BUILD/$bin" "$@" \
	        > "$log" 2>&1
	local rc=$?
	if (( rc == 0 )); then
		dstr_pass "$name"
	elif (( rc == 124 )); then
		dstr_fail "$name" "timed out after ${DSTR_TIMEOUT}s" "$log"
	else
		dstr_fail "$name" "exit status $rc" "$log"
	fi
	return $rc
}

# dstr_build_check NAME BIN ARGS...: dstr_cc BIN ARGS..., then
# dstr_check NAME BIN. A failed build is reported as a FAIL of NAME.
dstr_build_check ()
{
	local name=$1 bin=$2
	shift 2
	dstr_cc "$bin" "$@"
	dstr_check "$name" "$bin"
}

# dstr_compile_fails NAME REGEX ARGS...: compile ARGS (with -c and no
# output) and expect it to fail with a diagnostic matching the extended
# regex REGEX. Compiling successfully is a FAIL.
dstr_compile_fails ()
{
	local name=$1 regex=$2
	shift 2
	local log=$DSTR_BUILD/${name//\//_}.build.log
	if "$DSTR_CC" "${dstr_cflags[@]}" "$@" -c -o /dev/null > "$log" 2>&1; then
		dstr_fail "$name" "compiled, expected an error matching /$regex/"
	elif grep -qE -- "$regex" "$log"; then
		dstr_pass "$name"
	else
		dstr_fail "$name" "failed without a diagnostic matching /$regex/" "$log"
	fi
}
