/* Freestanding <string.h> for the qemu trial (tests/dstr/06-qemu) */
#ifndef TWINCEPTION_TESTS_DSTR_FS_STRING_H_
#define TWINCEPTION_TESTS_DSTR_FS_STRING_H_

#include <stddef.h>

extern void *
memcpy (void       *dest,
        void const *src,
        size_t      n);

extern void *
memmove (void       *dest,
         void const *src,
         size_t      n);

extern void *
memset (void   *dest,
        int     c,
        size_t  n);

extern int
memcmp (void const *a,
        void const *b,
        size_t      n);

extern size_t
strlen (char const *s);

extern int
strcmp (char const *a,
        char const *b);

extern int
strncmp (char const *a,
         char const *b,
         size_t      n);

extern char *
strerror (int err);

#endif /* TWINCEPTION_TESTS_DSTR_FS_STRING_H_ */
