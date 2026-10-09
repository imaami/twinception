/* Freestanding <stdlib.h> for the qemu trial (tests/dstr/06-qemu) */
#ifndef TWINCEPTION_TESTS_DSTR_FS_STDLIB_H_
#define TWINCEPTION_TESTS_DSTR_FS_STDLIB_H_

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

extern void *
malloc (size_t size);

extern void *
calloc (size_t nmemb,
        size_t size);

extern void *
realloc (void   *ptr,
         size_t  size);

extern void
free (void *ptr);

extern char *
getenv (char const *name);

extern unsigned long
strtoul (char const  *s,
         char       **end,
         int          base);

extern unsigned long long
strtoull (char const  *s,
          char       **end,
          int          base);

extern void
abort (void) __attribute__((noreturn));

extern void
exit (int status) __attribute__((noreturn));

#endif /* TWINCEPTION_TESTS_DSTR_FS_STDLIB_H_ */
