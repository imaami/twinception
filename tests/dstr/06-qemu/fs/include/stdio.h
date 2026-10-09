/* Freestanding <stdio.h> for the qemu trial (tests/dstr/06-qemu) */
#ifndef TWINCEPTION_TESTS_DSTR_FS_STDIO_H_
#define TWINCEPTION_TESTS_DSTR_FS_STDIO_H_

#include <stdarg.h>
#include <stddef.h>

/** An output stream: a file descriptor */
struct fs_file;
typedef struct fs_file FILE;

extern FILE *const stdout;
extern FILE *const stderr;

extern int
fprintf (FILE       *f,
         char const *fmt,
         ...) __attribute__((format(printf, 2, 3)));

extern int
printf (char const *fmt,
        ...) __attribute__((format(printf, 1, 2)));

extern int
snprintf (char       *buf,
          size_t      size,
          char const *fmt,
          ...) __attribute__((format(printf, 3, 4)));

extern int
vsnprintf (char       *buf,
           size_t      size,
           char const *fmt,
           va_list     ap) __attribute__((format(printf, 3, 0)));

extern int
fputs (char const *s,
       FILE       *f);

extern int
puts (char const *s);

#endif /* TWINCEPTION_TESTS_DSTR_FS_STDIO_H_ */
