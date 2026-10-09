/* Freestanding <errno.h> for the qemu trial (tests/dstr/06-qemu) */
#ifndef TWINCEPTION_TESTS_DSTR_FS_ERRNO_H_
#define TWINCEPTION_TESTS_DSTR_FS_ERRNO_H_

extern int errno;

#define EAGAIN    11
#define ENOMEM    12
#define EFAULT    14
#define EINVAL    22
#define EDOM      33
#define EOVERFLOW 75
#define EILSEQ    84

#endif /* TWINCEPTION_TESTS_DSTR_FS_ERRNO_H_ */
