/* Freestanding <inttypes.h> for the qemu trial (tests/dstr/06-qemu) */
#ifndef TWINCEPTION_TESTS_DSTR_FS_INTTYPES_H_
#define TWINCEPTION_TESTS_DSTR_FS_INTTYPES_H_

#include <stdint.h>

#define PRIu64  __UINT64_FMTu__
#define PRIx64  __UINT64_FMTx__
#define PRIuMAX __UINTMAX_FMTu__

#endif /* TWINCEPTION_TESTS_DSTR_FS_INTTYPES_H_ */
