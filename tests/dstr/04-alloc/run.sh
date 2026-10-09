#!/usr/bin/bash
# Trial 04: every allocation the library makes fails in turn, through
# the allocation shim (tests/dstr/shim.h), and so does its vsnprintf(),
# through a wrapper in alloc.c. Fortified, the library would call
# __vsnprintf_chk() past the wrapper, so _FORTIFY_SOURCE is off, also
# where the compiler defines it by default.

source "$DSTR_TESTS/lib.sh"

here=$DSTR_TESTS/04-alloc

dstr_build_check 04/alloc alloc "$here/alloc.c" "${dstr_lib_sources[@]}" \
                 "${dstr_shim_sources[@]}" "${dstr_shim_ldflags[@]}" \
                 -U_FORTIFY_SOURCE -Wl,--wrap=vsnprintf
