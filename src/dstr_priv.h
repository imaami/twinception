/**
 * @file dstr_priv.h
 * @brief Dynamic string with Short String Optimization for C
 *
 * This header must not be included by files which aren't part of the
 * `dstr` implementation. Specifically, only `dstr.c` and `dstr.h`
 * are allowed to include this header. Every other file that uses the
 * `dstr` API must instead include the public API header `dstr.h`.
 *
 * The above is necessary in order for the helper macros to be properly
 * undefined and not leak into the global namespace of API users, and
 * conversely it keeps them defined for `dstr` implementation code.
 */
#ifndef TWINCEPTION_SRC_DSTR_PRIV_H_
#define TWINCEPTION_SRC_DSTR_PRIV_H_

#undef dstr_printf_like
#undef dstr_useless
#undef dstr_const_inline
#undef dstr_inline
#undef dstr_ptr_space_left
#undef dstr_ptr_space_left_
#undef dstr_ptr_block_count
#undef dstr_ptr_block_count_
#undef dstr_uptr_block_count_
#undef dstr_ptr_len
#undef dstr_ptr_len_
#undef dstr_sso_len
#undef dstr_sso_len_
#undef dstr_sso_space_left
#undef dstr_sso_space_left_
#undef dstr_is_ptr
#undef dstr_is_ptr_
#undef dstr_is_sso
#undef dstr_is_sso_
#undef dstr_last_uptr
#undef dstr_last_uptr_
#undef dstr_last_uptr_const_
#undef dstr_last_byte
#undef dstr_last_byte_

/* Don't move this include statement or reorder lines in relation to it! */
#include "dstr.h"

#endif /* TWINCEPTION_SRC_DSTR_PRIV_H_ */
