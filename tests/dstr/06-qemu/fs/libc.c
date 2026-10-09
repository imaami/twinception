/**
 * @file libc.c
 * @brief Minimal freestanding C library for the qemu trial
 *
 * Enough of libc for the test programs and the library: raw write and
 * exit_group system calls and _start for each supported architecture,
 * the string functions, an allocator over a static arena that also
 * implements tests/dstr/shim.h, and a printf without division (32-bit
 * targets have no runtime library for 64-bit division here).
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim.h"

int errno;

/* --- System calls -------------------------------------------------- */

#if defined __aarch64__
# define FS_SYS_WRITE      64
# define FS_SYS_EXIT_GROUP 94
static long
fs_syscall3 (long n,
             long a,
             long b,
             long c)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
	                  : "memory");
	return x0;
}
#elif defined __arm__
# define FS_SYS_WRITE      4
# define FS_SYS_EXIT_GROUP 248
static long
fs_syscall3 (long n,
             long a,
             long b,
             long c)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	__asm__ volatile ("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2)
	                  : "memory");
	return r0;
}
#elif defined __powerpc__
# define FS_SYS_WRITE      4
# define FS_SYS_EXIT_GROUP 234
static long
fs_syscall3 (long n,
             long a,
             long b,
             long c)
{
	register long r0 __asm__("r0") = n;
	register long r3 __asm__("r3") = a;
	register long r4 __asm__("r4") = b;
	register long r5 __asm__("r5") = c;
	__asm__ volatile ("sc" : "+r"(r0), "+r"(r3), "+r"(r4), "+r"(r5)
	                  : : "memory", "cr0", "r6", "r7", "r8", "r9", "r10",
	                      "r11", "r12", "ctr", "xer");
	return r3;
}
#elif defined __s390x__
# define FS_SYS_WRITE      4
# define FS_SYS_EXIT_GROUP 248
static long
fs_syscall3 (long n,
             long a,
             long b,
             long c)
{
	register long r1 __asm__("r1") = n;
	register long r2 __asm__("r2") = a;
	register long r3 __asm__("r3") = b;
	register long r4 __asm__("r4") = c;
	__asm__ volatile ("svc 0" : "+r"(r2) : "r"(r1), "r"(r3), "r"(r4)
	                  : "memory");
	return r2;
}
#elif defined __riscv
# define FS_SYS_WRITE      64
# define FS_SYS_EXIT_GROUP 94
static long
fs_syscall3 (long n,
             long a,
             long b,
             long c)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	__asm__ volatile ("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2)
	                  : "memory");
	return a0;
}
#elif defined __x86_64__
# define FS_SYS_WRITE      1
# define FS_SYS_EXIT_GROUP 231
static long
fs_syscall3 (long n,
             long a,
             long b,
             long c)
{
	long ret;
	__asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c)
	                  : "rcx", "r11", "memory");
	return ret;
}
#elif defined __i386__
# define FS_SYS_WRITE      4
# define FS_SYS_EXIT_GROUP 252
static long
fs_syscall3 (long n,
             long a,
             long b,
             long c)
{
	long ret;
	__asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c)
	                  : "memory");
	return ret;
}
#else
# error "no system calls for this architecture"
#endif

/**
 * @brief Write all of `n` bytes to file descriptor `fd`
 */
static void
fs_write (int         fd,
          char const *buf,
          size_t      n)
{
	while (n) {
		long const w = fs_syscall3(FS_SYS_WRITE, fd, (long)buf, (long)n);
		if (w <= 0)
			return;
		buf += w;
		n -= (size_t)w;
	}
}

void
exit (int status)
{
	(void)fs_syscall3(FS_SYS_EXIT_GROUP, status, 0, 0);
	for (;;) {
	}
}

void
abort (void)
{
	exit(134);
}

extern int
main (void);

/** Program entry: run main() and exit with its status */
#if defined __i386__ || defined __x86_64__
__attribute__((force_align_arg_pointer))
#endif
__attribute__((noreturn, used)) void
_start (void);

#if defined __i386__ || defined __x86_64__
__attribute__((force_align_arg_pointer))
#endif
__attribute__((noreturn, used)) void
_start (void)
{
	exit(main());
}

/* --- Strings ------------------------------------------------------- */

void *
memcpy (void       *dest,
        void const *src,
        size_t      n)
{
	unsigned char *d = dest;
	unsigned char const *s = src;
	while (n--)
		*d++ = *s++;
	return dest;
}

void *
memmove (void       *dest,
         void const *src,
         size_t      n)
{
	unsigned char *d = dest;
	unsigned char const *s = src;
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		while (n--)
			d[n] = s[n];
	}
	return dest;
}

void *
memset (void   *dest,
        int     c,
        size_t  n)
{
	unsigned char *d = dest;
	while (n--)
		*d++ = (unsigned char)c;
	return dest;
}

int
memcmp (void const *a,
        void const *b,
        size_t      n)
{
	unsigned char const *x = a, *y = b;
	for (size_t i = 0; i < n; ++i) {
		if (x[i] != y[i])
			return x[i] < y[i] ? -1 : 1;
	}
	return 0;
}

size_t
strlen (char const *s)
{
	size_t n = 0;
	while (s[n])
		++n;
	return n;
}

int
strncmp (char const *a,
         char const *b,
         size_t      n)
{
	for (size_t i = 0; i < n; ++i) {
		unsigned char const x = (unsigned char)a[i];
		unsigned char const y = (unsigned char)b[i];
		if (x != y)
			return x < y ? -1 : 1;
		if (!x)
			break;
	}
	return 0;
}

int
strcmp (char const *a,
        char const *b)
{
	return strncmp(a, b, SIZE_MAX);
}

char *
strerror (int err)
{
	(void)err;
	return (char *)"error";
}

char *
getenv (char const *name)
{
	(void)name;
	return NULL;
}

unsigned long long
strtoull (char const  *s,
          char       **end,
          int          base)
{
	unsigned long long v = 0;
	if (!base) {
		base = 10;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			s += 2;
		}
	}
	for (;; ++s) {
		unsigned d;
		if (*s >= '0' && *s <= '9')
			d = (unsigned)(*s - '0');
		else if (*s >= 'a' && *s <= 'f')
			d = (unsigned)(*s - 'a' + 10);
		else if (*s >= 'A' && *s <= 'F')
			d = (unsigned)(*s - 'A' + 10);
		else
			break;
		if (d >= (unsigned)base)
			break;
		v = v * (unsigned)base + d;
	}
	if (end)
		*end = (char *)s;
	return v;
}

unsigned long
strtoul (char const  *s,
         char       **end,
         int          base)
{
	return (unsigned long)strtoull(s, end, base);
}

/* --- Allocator and shim.h ----------------------------------------- */

/** Arena size */
#define FS_ARENA ((size_t)96 << 20)

/** Header in front of every block (16 bytes keeps 16-byte alignment) */
struct fs_head {
	uint32_t magic;  /**< FS_LIVE or FS_FREE */
	uint32_t cls;    /**< Size class: capacity 1 << cls */
	uint64_t pad;
};

#define FS_LIVE UINT32_C(0x11ec11ec)
#define FS_FREE UINT32_C(0xf7eef7ee)
#define FS_MIN_CLS 4U
#define FS_MAX_CLS 26U

/** Allocator and shim state */
static struct {
	struct fs_head *free_list[FS_MAX_CLS + 1U];
	size_t          top;
	size_t          live;
	size_t          calls;
	size_t          bad;
	size_t          fail_at;
	int             fail_err;
} fs;

static unsigned char fs_arena[FS_ARENA] __attribute__((aligned(16)));

/**
 * @brief Allocate without counting; NULL if impossible
 */
static void *
fs_alloc (size_t size)
{
	uint32_t cls = FS_MIN_CLS;
	while (cls <= FS_MAX_CLS && ((size_t)1 << cls) < size)
		++cls;
	if (cls > FS_MAX_CLS)
		return NULL;

	struct fs_head *h = fs.free_list[cls];
	if (h) {
		fs.free_list[cls] = *(struct fs_head **)(h + 1);
	} else {
		size_t const need = sizeof *h + ((size_t)1 << cls);
		if (need > FS_ARENA - fs.top)
			return NULL;
		h = (struct fs_head *)(void *)&fs_arena[fs.top];
		fs.top += need;
		h->cls = cls;
	}
	h->magic = FS_LIVE;
	++fs.live;
	return h + 1;
}

/**
 * @brief Header of a live block, or NULL (counted as bad)
 */
static struct fs_head *
fs_head_of (void *ptr)
{
	unsigned char *const p = ptr;
	if (p < fs_arena + sizeof (struct fs_head) ||
	    p >= fs_arena + FS_ARENA) {
		++fs.bad;
		return NULL;
	}
	struct fs_head *const h = (struct fs_head *)ptr - 1;
	if (h->magic != FS_LIVE) {
		++fs.bad;
		return NULL;
	}
	return h;
}

/**
 * @brief Release a live block without counting
 */
static void
fs_release (struct fs_head *h)
{
	h->magic = FS_FREE;
	*(struct fs_head **)(h + 1) = fs.free_list[h->cls];
	fs.free_list[h->cls] = h;
	--fs.live;
}

/**
 * @brief Count an allocation call and say whether it must fail
 */
static bool
fs_fails (void)
{
	if (++fs.calls != fs.fail_at)
		return false;
	fs.fail_at = 0;
	if (fs.fail_err)
		errno = fs.fail_err;
	return true;
}

void
shim_reset (void)
{
	fs.calls = 0;
	fs.fail_at = 0;
	fs.fail_err = 0;
}

void
shim_fail_at (size_t n,
              int    err)
{
	fs.fail_at = n ? fs.calls + n : 0U;
	fs.fail_err = err;
}

size_t
shim_calls (void)
{
	return fs.calls;
}

size_t
shim_live (void)
{
	return fs.live;
}

size_t
shim_bad (void)
{
	return fs.bad;
}

void *
malloc (size_t size)
{
	if (fs_fails())
		return NULL;
	void *const p = fs_alloc(size);
	if (!p)
		errno = ENOMEM;
	return p;
}

void *
calloc (size_t nmemb,
        size_t size)
{
	if (fs_fails())
		return NULL;
	if (size && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		return NULL;
	}
	void *const p = fs_alloc(nmemb * size);
	if (!p)
		errno = ENOMEM;
	else
		memset(p, 0, nmemb * size);
	return p;
}

void *
realloc (void   *ptr,
         size_t  size)
{
	if (fs_fails())
		return NULL;
	if (!ptr) {
		void *const p = fs_alloc(size);
		if (!p)
			errno = ENOMEM;
		return p;
	}
	struct fs_head *const h = fs_head_of(ptr);
	if (!h)
		return NULL;

	/* Always move, so that stale pointers read freed memory */
	void *const p = fs_alloc(size);
	if (!p) {
		errno = ENOMEM;
		return NULL;
	}
	size_t const old = (size_t)1 << h->cls;
	memcpy(p, ptr, old < size ? old : size);
	memset(ptr, 0xa5, old);
	fs_release(h);
	return p;
}

void
free (void *ptr)
{
	if (!ptr)
		return;
	struct fs_head *const h = fs_head_of(ptr);
	if (!h)
		return;
	memset(ptr, 0xa5, (size_t)1 << h->cls);
	fs_release(h);
}

/* --- Formatted output ---------------------------------------------- */

/** Output buffer of vsnprintf() */
struct fs_out {
	char   *buf;
	size_t  size;
	size_t  len;
};

/**
 * @brief Append one character, counting it even if it doesn't fit
 */
static void
fs_put (struct fs_out *o,
        char           c)
{
	if (o->len + 1U < o->size)
		o->buf[o->len] = c;
	++o->len;
}

/**
 * @brief Append a number in decimal, without division
 */
static void
fs_put_dec (struct fs_out *o,
            uintmax_t      v)
{
	static uint64_t const pow10[] = {
		UINT64_C(10000000000000000000), UINT64_C(1000000000000000000),
		UINT64_C(100000000000000000), UINT64_C(10000000000000000),
		UINT64_C(1000000000000000), UINT64_C(100000000000000),
		UINT64_C(10000000000000), UINT64_C(1000000000000),
		UINT64_C(100000000000), UINT64_C(10000000000),
		UINT64_C(1000000000), UINT64_C(100000000), UINT64_C(10000000),
		UINT64_C(1000000), UINT64_C(100000), UINT64_C(10000),
		UINT64_C(1000), UINT64_C(100), UINT64_C(10), UINT64_C(1),
	};
	bool started = false;
	for (size_t i = 0; i < sizeof pow10 / sizeof pow10[0]; ++i) {
		char d = '0';
		while (v >= pow10[i]) {
			v -= pow10[i];
			++d;
		}
		if (d != '0' || started || i + 1U == sizeof pow10 / sizeof pow10[0]) {
			fs_put(o, d);
			started = true;
		}
	}
}

/**
 * @brief Append a number in hexadecimal
 */
static void
fs_put_hex (struct fs_out *o,
            uintmax_t      v)
{
	bool started = false;
	for (int shift = 60; shift >= 0; shift -= 4) {
		unsigned const d = (unsigned)(v >> shift) & 15U;
		if (d || started || !shift) {
			fs_put(o, "0123456789abcdef"[d]);
			started = true;
		}
	}
}

int
vsnprintf (char       *buf,
           size_t      size,
           char const *fmt,
           va_list     ap)
{
	struct fs_out o = { buf, size, 0 };

	for (char const *f = fmt; *f; ++f) {
		if (*f != '%') {
			fs_put(&o, *f);
			continue;
		}
		++f;
		size_t prec = SIZE_MAX;
		if (*f == '.') {
			prec = 0;
			for (++f; *f >= '0' && *f <= '9'; ++f)
				prec = prec * 10U + (size_t)(*f - '0');
		}
		/* Length: 0 int, 1 long, 2 long long, 3 size_t, 4 intmax_t */
		unsigned len = 0;
		for (;; ++f) {
			if (*f == 'l')
				++len;
			else if (*f == 'z')
				len = 3;
			else if (*f == 'j')
				len = 4;
			else
				break;
		}
		switch (*f) {
		case '%':
			fs_put(&o, '%');
			break;
		case 'c':
			fs_put(&o, (char)va_arg(ap, int));
			break;
		case 's': {
			char const *s = va_arg(ap, char const *);
			if (!s)
				s = "(null)";
			for (size_t i = 0; s[i] && i < prec; ++i)
				fs_put(&o, s[i]);
			break;
		}
		case 'p':
			fs_put(&o, '0');
			fs_put(&o, 'x');
			fs_put_hex(&o, (uintptr_t)va_arg(ap, void *));
			break;
		case 'd':
		case 'i': {
			intmax_t v = len == 0 ? va_arg(ap, int)
			           : len == 1 ? va_arg(ap, long)
			           : len == 2 ? va_arg(ap, long long)
			           : len == 3 ? (intmax_t)va_arg(ap, size_t)
			           : va_arg(ap, intmax_t);
			if (v < 0) {
				fs_put(&o, '-');
				fs_put_dec(&o, -(uintmax_t)v);
			} else {
				fs_put_dec(&o, (uintmax_t)v);
			}
			break;
		}
		case 'u':
		case 'x': {
			uintmax_t const v = len == 0 ? va_arg(ap, unsigned)
			                  : len == 1 ? va_arg(ap, unsigned long)
			                  : len == 2 ? va_arg(ap, unsigned long long)
			                  : len == 3 ? va_arg(ap, size_t)
			                  : va_arg(ap, uintmax_t);
			if (*f == 'u')
				fs_put_dec(&o, v);
			else
				fs_put_hex(&o, v);
			break;
		}
		default:
			fs_put(&o, '?');
			if (!*f)
				--f;
			break;
		}
	}
	if (size)
		buf[o.len < size ? o.len : size - 1U] = '\0';
	return (int)o.len;
}

int
snprintf (char       *buf,
          size_t      size,
          char const *fmt,
          ...)
{
	va_list ap;
	va_start(ap, fmt);
	int const n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

/** A file is just its descriptor */
struct fs_file {
	int fd;
};

static struct fs_file fs_stdout = { 1 };
static struct fs_file fs_stderr = { 2 };
FILE *const stdout = &fs_stdout;
FILE *const stderr = &fs_stderr;

/**
 * @brief Format into a buffer and write it to `f`
 */
static int
fs_vfprintf (FILE       *f,
             char const *fmt,
             va_list     ap)
{
	char buf[1024];
	int const n = vsnprintf(buf, sizeof buf, fmt, ap);
	size_t const len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1U;
	fs_write(f->fd, buf, len);
	return n;
}

int
fprintf (FILE       *f,
         char const *fmt,
         ...)
{
	va_list ap;
	va_start(ap, fmt);
	int const n = fs_vfprintf(f, fmt, ap);
	va_end(ap);
	return n;
}

int
printf (char const *fmt,
        ...)
{
	va_list ap;
	va_start(ap, fmt);
	int const n = fs_vfprintf(stdout, fmt, ap);
	va_end(ap);
	return n;
}

int
fputs (char const *s,
       FILE       *f)
{
	fs_write(f->fd, s, strlen(s));
	return 0;
}

int
puts (char const *s)
{
	fs_write(1, s, strlen(s));
	fs_write(1, "\n", 1);
	return 0;
}
