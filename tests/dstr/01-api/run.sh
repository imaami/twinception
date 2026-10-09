#!/usr/bin/bash
# Trial 01: the documented contract of every public function, with the
# allocation shim catching leaks and bad frees.

source "$DSTR_TESTS/lib.sh"

here=$DSTR_TESTS/01-api

dstr_build_check 01/api api "$here/api.c" "${dstr_lib_sources[@]}" \
                 "${dstr_shim_sources[@]}" "${dstr_shim_ldflags[@]}"
