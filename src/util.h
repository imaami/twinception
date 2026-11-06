/** @file util.h
 *
 * Small compile-time, container, hashing, formatting, text and
 * file helpers.
 */
#ifndef ASKAI_SRC_UTIL_H_
#define ASKAI_SRC_UTIL_H_

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clk.h"
#include "compat.h"
#include "cstr_view.h"

/** Write the @p n bytes at @p src to @p dst as 2 @p n lowercase hexadecimal
 *  digits, without a NUL.
 */
static inline void
util_hex (char *restrict       dst,
          void const *restrict src,
          size_t               n)
{
	static char const digits[] = "0123456789abcdef";
	unsigned char const *p = src;
	for (size_t i = 0; i < n; ++i) {
		dst[2 * i] = digits[p[i] >> 4];
		dst[2 * i + 1] = digits[p[i] & 15];
	}
}

/** The most bytes that util_random() fills in one call. */
enum { UTIL_RANDOM_MAX = 256 };

/** Fill the @p n bytes at @p dst, at most UTIL_RANDOM_MAX, with random
 *  bytes from the kernel. Returns 0, or the error number.
 */
static inline int
util_random (void   *dst,
             size_t  n)
{
	if (n > UTIL_RANDOM_MAX)
		return EINVAL;
	/* For so few bytes, getrandom() returns fewer only if a signal
	 * interrupts it before it has any. */
	ssize_t got;
	while ((got = getrandom(dst, n, 0)) < 0)
		if (errno != EINTR)
			return errno;
	return (size_t)got == n ? 0 : EIO;
}

/** Write @p n random bytes, at most UTIL_RANDOM_MAX, from the kernel to
 *  @p dst as 2 @p n lowercase hexadecimal digits, without a NUL. Returns 0,
 *  or the error number.
 */
static inline int
util_random_hex (char   *dst,
                 size_t  n)
{
	unsigned char bits[UTIL_RANDOM_MAX];
	int e = util_random(bits, n);
	if (!e)
		util_hex(dst, bits, n);
	return e;
}

/** Whether the @p n bytes at @p s are lowercase hexadecimal digits. */
static inline bool
util_is_hex (char const *s,
             size_t      n)
{
	for (size_t i = 0; i < n; ++i)
		if ((s[i] < '0' || s[i] > '9') && (s[i] < 'a' || s[i] > 'f'))
			return false;
	return true;
}

/** The value of the hexadecimal digit @p c, of either case, or 16 if @p c
 *  is not one.
 */
static inline unsigned
util_hex_digit (unsigned char c)
{
	unsigned lower = c | 0x20u;
	return c >= '0' && c <= '9' ? (unsigned)(c - '0')
	     : lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : 16;
}

/** The most digits of a uint64_t in decimal. */
enum { UTIL_DECIMAL = 20 };

/** Write @p n in decimal into the bytes before @p end, at most UTIL_DECIMAL
 *  of them. Returns a pointer to the first digit.
 */
static inline char *
util_decimal (char     *end,
              uint64_t  n)
{
	do *--end = (char)('0' + n % 10);
	while (n /= 10);
	return end;
}

/** The plural ending of a noun after the count @p n: "" if @p n is 1, else
 *  "s".
 */
static inline char const *
util_s (uint64_t n)
{
	return n == 1 ? "" : "s";
}

/** Write the @p n low bytes of @p v, at most 8, to @p to, the most
 *  significant first.
 */
static inline void
util_be_put (unsigned char *to,
             uint64_t       v,
             size_t         n)
{
	while (n--) {
		to[n] = (unsigned char)v;
		v >>= 8;
	}
}

/** The number in the @p n bytes at @p from, at most 8, the most
 *  significant first.
 */
static inline uint64_t
util_be_get (unsigned char const *from,
             size_t               n)
{
	uint64_t v = 0;
	for (size_t i = 0; i < n; ++i)
		v = v << 8 | from[i];
	return v;
}

/** Parse the decimal number at the start of the @p len bytes at @p s into
 *  @p *value. Returns the number of digits. If there are no digits or the
 *  number is greater than @p max, returns 0 and sets @p *value to 0.
 */
static inline size_t
util_digits (char const *s,
             size_t      len,
             uint64_t    max,
             uint64_t   *value)
{
	*value = 0;
	size_t i = 0;
	for (; i < len && s[i] >= '0' && s[i] <= '9'; ++i) {
		unsigned digit = (unsigned)(s[i] - '0');
		if (digit > max || *value > (max - digit) / 10) {
			*value = 0;
			return 0;
		}
		*value = *value * 10 + digit;
	}
	return i;
}

/** Parse the @p len bytes at @p s, which must all be decimal digits, at
 *  least one, into @p *value. Returns 0, or EINVAL if they are not or the
 *  number is greater than @p max.
 */
static inline int
util_number (char const *s,
             size_t      len,
             uint64_t    max,
             uint64_t   *value)
{
	return len && util_digits(s, len, max, value) == len ? 0 : EINVAL;
}

/** Returns how many of the @p len bytes at @p s fit in @p max bytes
 *  without ending within a UTF-8 character.
 */
static inline size_t
util_clip (char const *s,
           size_t      len,
           size_t      max)
{
	if (len <= max)
		return len;
	while (max && ((unsigned char)s[max] & 0xc0) == 0x80)
		--max;
	return max;
}

/** Returns @p off, at most @p len, moved forward past the UTF-8
 *  continuation bytes of the @p len bytes at @p s: the start of a
 *  character, or @p len.
 */
static inline size_t
util_char_start (char const *s,
                 size_t      len,
                 size_t      off)
{
	while (off < len && ((unsigned char)s[off] & 0xc0) == 0x80)
		++off;
	return off;
}

/** The ASCII letters and digits, as a string: A to Z, a to z, then 0 to 9. */
#define UTIL_ALNUM "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

/** The most bytes of a word in askai's Git refs, such as a participant. */
enum { UTIL_WORD_MAX = 64 };

/** Whether the @p n bytes at @p s, 1 to @p max of them, are ASCII letters,
 *  digits, underscores or hyphens, as the words in askai's Git refs are.
 */
static inline bool
util_word (char const *s,
           size_t      n,
           size_t      max)
{
	if (!s || !n || n > max)
		return false;
	for (size_t i = 0; i < n; ++i)
		if (!(s[i] >= 'a' && s[i] <= 'z') && !(s[i] >= 'A' && s[i] <= 'Z') &&
		    !(s[i] >= '0' && s[i] <= '9') && s[i] != '_' && s[i] != '-')
			return false;
	return true;
}

/** Returns the length of the string @p s if util_word() accepts it as a
 *  word of 1 to @p max bytes, or 0 if not or if @p s is nullptr.
 */
static inline size_t
util_word_len (char const *s,
               size_t      max)
{
	size_t const n = s ? strnlen(s, max + 1) : 0;
	return util_word(s, n, max) ? n : 0;
}

/** The entry "NAME=value" of the process's environment whose "NAME=" is
 *  the @p len bytes at @p name, or nullptr if it has none. Nothing in
 *  askai changes its environment, so the entry lasts.
 */
static inline char *
util_env (char const *name,
          size_t      len)
{
	extern char **environ;
	for (char **e = environ; e && *e; ++e)
		if (!strncmp(*e, name, len))
			return *e;
	return nullptr;
}

/** The entry "PATH=..." of the process's environment for a child to
 *  inherit, or "PATH=/usr/bin:/bin" if it has no PATH or an empty one.
 */
static inline char *
util_env_path (void)
{
	char *p = util_env("PATH=", sizeof "PATH=" - 1U);
	return p && p[sizeof "PATH=" - 1U]
	       ? p : "PATH=/usr/bin:/bin";
}

/** Find askai's directory of state files by the XDG Base Directory
 *  specification: the value of XDG_STATE_HOME followed by "/askai", or, if
 *  that variable is not an absolute path, the value of HOME followed by
 *  "/.local/state/askai". Stores the value in @p *base and what follows it
 *  in @p *rest. Returns 0, or ENOENT if neither variable is an absolute
 *  path.
 */
static inline int
util_state_path (char const **base,
                 char const **rest)
{
	static struct { char const *name, *rest; } const homes[] = {
		{ "XDG_STATE_HOME", "/askai" }, { "HOME", "/.local/state/askai" }
	};
	for (size_t i = 0; i < array_size(homes); ++i) {
		*base = getenv(homes[i].name);
		*rest = homes[i].rest;
		if (*base && **base == '/')
			return 0;
	}
	return ENOENT;
}

/** Close the descriptor @p *fd unless it is negative, and set it to -1. */
static inline void
util_close (int *fd)
{
	if (*fd >= 0)
		(void)close(*fd);
	*fd = -1;
}

/** Take the next part of the path @p *rest, between slashes, that is not
 *  empty and not ".", into @p *part, and leave the bytes after it in
 *  @p *rest. At the end of the path, @p *part is empty. Returns 0, or
 *  EINVAL if the part is "..".
 */
static inline int
util_path_next (struct cstr_view *rest,
                struct cstr_view *part)
{
	do {
		*part = *rest;
		if (!cstr_view_cut(rest, '/', part))
			rest->len = 0;
	} while (part->len ? cstr_view_equal(*part, CSTR_VIEW(".")) : rest->len != 0);
	return cstr_view_equal(*part, CSTR_VIEW("..")) ? EINVAL : 0;
}

/** Block every signal but those that faults raise in the calling thread,
 *  whose threads to come inherit its mask, and store the mask before in
 *  @p old. Returns 0, or the error number of pthread_sigmask().
 */
static inline int
util_block_async (sigset_t *old)
{
	static int const faults[] = {
		SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGSYS, SIGTRAP
	};
	sigset_t all;
	sigfillset(&all);
	for (size_t i = 0; i < array_size(faults); ++i)
		sigdelset(&all, faults[i]);
	return pthread_sigmask(SIG_BLOCK, &all, old);
}

/** Returns the reason why the file that @p st describes is not a private
 *  file of the process's user of the type @p type, S_IFREG or S_IFDIR, or
 *  nullptr if it is one. A private file has that type, the user owns it,
 *  no other user may access it, and a regular one has one link.
 */
static inline char const *
util_private (struct stat const *st,
              mode_t             type)
{
	if ((st->st_mode & S_IFMT) != type)
		return type == S_IFDIR ? "the file is not a directory"
		                       : "the file is not a regular file";
	if (st->st_uid != geteuid())
		return "askai's user does not own the file";
	if (st->st_mode & 0077)
		return "the mode of the file lets other users access it";
	if (type == S_IFREG && st->st_nlink != 1)
		return "the file has more than one hard link";
	return nullptr;
}

/** Returns 0 if the file open as @p fd is a private file of the process's
 *  user of the type @p type, as util_private() says, EACCES if it is not,
 *  or the error number of fstat().
 */
static inline int
util_private_fd (int    fd,
                 mode_t type)
{
	struct stat st;
	return fstat(fd, &st) ? errno : util_private(&st, type) ? EACCES : 0;
}

/** Write the @p len bytes at @p buf to @p fd, again after a signal or a
 *  partial write. Returns 0, or the error number, EIO if no byte could be
 *  written.
 */
static inline int
util_write_all (int         fd,
                void const *buf,
                size_t      len)
{
	unsigned char const *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n ? errno : EIO;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/** Read up to @p len bytes from @p fd into @p buf, from the offset @p at
 *  with pread(), or from where the file stands with read() if @p at is
 *  negative, again after a signal or a partial read, until the end of the
 *  file; their count goes to @p *got. Returns 0, or the error number.
 */
static inline int
util_read_all (int     fd,
               void   *buf,
               size_t  len,
               off_t   at,
               size_t *got)
{
	unsigned char *p = buf;
	*got = 0;
	while (*got < len) {
		ssize_t n = at < 0 ? read(fd, p + *got, len - *got)
		                   : pread(fd, p + *got, len - *got, at + (off_t)*got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n ? errno : 0;
		*got += (size_t)n;
	}
	return 0;
}

/** Read the start of the file at @p path, at most @p size - 1 bytes, into
 *  @p buf as a string, which is empty if the file cannot be opened.
 *  Returns 0, or the error number.
 */
static inline int
util_read_text (char const *path,
                char       *buf,
                size_t      size)
{
	*buf = '\0';
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;
	size_t got;
	int e = util_read_all(fd, buf, size - 1, -1, &got);
	util_close(&fd);
	buf[got] = '\0';
	return e;
}

/** A hash of the @p len bytes at @p data, seeded with @p seed: a seed no
 *  client knows keeps clients from picking keys that crowd one bucket.
 */
static inline uint64_t
util_hash (uint64_t    seed,
           void const *data,
           size_t      len)
{
	unsigned char const *p = data;
	uint64_t h = seed ^ len;
	for (size_t i = 0; i < len; ++i)
		h = (h ^ p[i]) * 0x100000001b3;
	h = (h ^ h >> 33) * 0xff51afd7ed558ccd;
	return h ^ h >> 33;
}

/** Returns a seed for util_hash(). The seed is 8 random bytes from the
 *  kernel, or the wall clock if the kernel has no random bytes yet.
 */
static inline uint64_t
util_seed (void)
{
	uint64_t seed;
	if (getrandom(&seed, sizeof seed, GRND_NONBLOCK) == (ssize_t)sizeof seed)
		return seed;
	return clk_realtime_ns();
}

/** Grow @p items, an array with room for @p *cap items of @p size bytes,
 *  so that it has room for @p n items, and update @p *cap. Returns the
 *  array, which may have moved. If out of memory, returns @p items and
 *  leaves @p *cap as it is, less than @p n.
 */
static inline void *
util_room (void   *items,
           size_t *cap,
           size_t  n,
           size_t  size)
{
	if (n <= *cap)
		return items;
	size_t more = *cap ? 2 * *cap : 4;
	more = more < n || more > SIZE_MAX / size ? n : more;
	void *p = n <= SIZE_MAX / size ? realloc(items, more * size) : nullptr;
	if (!p)
		return items;
	*cap = more;
	return p;
}

/** Grow the array @p items with room for @p cap items, both lvalues, to
 *  room for @p n items with util_room(). Evaluates to 0, or to ENOMEM
 *  with @p items and @p cap unchanged.
 */
#define util_grow(items, cap, n) \
        ((items) = util_room((items), &(cap), (n), sizeof *(items)), \
         (cap) < (n) ? ENOMEM : 0)

/** Make room for @p add items at the index @p at of @p items, an array of
 *  @p n items of @p size bytes with room for @p *cap items, in place of
 *  the @p drop items from @p at on: grow the array with util_room() and
 *  move the items after the dropped ones. Returns the array, which may
 *  have moved. If out of memory, returns @p items as it is and leaves
 *  @p *cap less than n - drop + add.
 */
static inline void *
util_splice_ (void   *items,
              size_t  n,
              size_t *cap,
              size_t  at,
              size_t  drop,
              size_t  add,
              size_t  size)
{
	size_t const count = n - drop + add;
	unsigned char *p = util_room(items, cap, count, size);
	if (*cap >= count && at + drop < n)
		memmove(p + (at + add) * size, p + (at + drop) * size, (n - at - drop) * size);
	return p;
}

/** Replace the @p drop items from the index @p at of the array @p items of
 *  @p n items with room for @p cap, all three lvalues, with room for
 *  @p add items with util_splice_(), and set @p n to n - drop + add.
 *  Evaluates to 0, or to ENOMEM with @p items, @p n and @p cap unchanged.
 */
#define util_splice(items, n, cap, at, drop, add) \
        ((items) = util_splice_((items), (n), &(cap), (at), (drop), (add), sizeof *(items)), \
         (cap) < (n) - (drop) + (add) ? ENOMEM : ((n) += (add) - (drop), 0))

/** Returns the length of the scheme of @p url with its "://", 7 for http
 *  and 8 for https, if more follows it and each byte of @p url is printable
 *  ASCII and not a space; 0 otherwise.
 */
static inline size_t
util_url_scheme (struct cstr_view url)
{
	size_t const scheme = cstr_view_starts(url, CSTR_VIEW("http://")) ? sizeof "http://" - 1
	                    : cstr_view_starts(url, CSTR_VIEW("https://")) ? sizeof "https://" - 1
	                    : 0;
	if (!scheme || scheme == url.len)
		return 0;
	for (size_t i = 0; i < url.len; ++i)
		if ((unsigned char)url.data[i] <= ' ' || (unsigned char)url.data[i] > '~')
			return 0;
	return scheme;
}

/** Returns the length of the origin of @p url, an http or https URL as
 *  util_url_scheme() takes it: its scheme, "://" and authority, which ends
 *  at the first '/', '?' or '#'. Returns 0 if @p url is no such URL, if
 *  the authority is empty, or if the authority has a user name or a
 *  password, which end in '@'.
 */
static inline size_t
util_url_origin (struct cstr_view url)
{
	static char const stops[] = "/?#@";
	size_t const scheme = util_url_scheme(url);
	if (!scheme)
		return 0;
	size_t end = scheme;
	while (end < url.len && !memchr(stops, url.data[end], sizeof stops - 1))
		++end;
	return end > scheme && (end == url.len || url.data[end] != '@') ? end : 0;
}

/** The part of @p url that a log shows after its scheme: the rest of the
 *  URL after its user name and password, if it has them. Stores the length
 *  of the scheme and its "://" in @p *scheme.
 */
static inline char const *
util_url_shown (char const *restrict url,
                int        *restrict scheme)
{
	char const *host = url;
	for (size_t i = 0; url[i]; ) {
		if (url[i++] == ':' &&
		    url[ i ] == '/' &&
		    url[++i] == '/') {
			host = &url[++i];
			break;
		}
	}

	*scheme = (int)(host - url);

	for (url = host;; ) {
		switch (*url++) {
		case '\0':
		case '#':
		case '?':
		case '/':
			return host;
		case '@':
			return url;
		}
	}
}

[[gnu::format(printf, 3, 0)]]
static inline size_t
util_vformat (char       *buf,
              size_t      size,
              char const *fmt,
              va_list     ap)
{
	/* Set `len` to `SIZE_MAX` if args are valid, otherwise 0. */
	size_t len = !buf | !size;

	if (--len) {
		union { int i; unsigned u; } x = {
			vsnprintf(buf, size, fmt, ap)
		};
		bool const err = x.u & ~(~0U >> 1U);

		/* Wipe on error, otherwise assign the length. */
		len += err;
		len &= x.u;

		if (err)
			/* Terminate buffer on error just in case. */
			buf[0] = '\0';

		if (len >= size) {
			/* Truncated write. Adjust the length we intend to
			 * return, and indicate truncation by changing the
			 * last characters to "..." if there's room left.
			 * The dots also replace the rest of a UTF-8
			 * character that they would split.
			 */
			len = --size;
			if (len > 2U) {
				size_t const dots = util_clip(buf, len, len - 3U);
				memset(buf + dots, '.', len - dots);
			}
		}
	}

	return len;
}

[[gnu::format(printf, 3, 4)]]
static inline size_t
util_format (char       *buf,
             size_t      size,
             char const *fmt,
             ...)
{
	va_list ap;
	va_start(ap, fmt);
	size_t len = util_vformat(buf, size, fmt, ap);
	va_end(ap);
	return len;
}

/** Formats text into the @p size bytes at @p buf as util_format() does.
 *  Returns a view of the text in @p buf.
 */
[[gnu::format(printf, 3, 4)]]
static inline struct cstr_view
util_format_view (char       *buf,
                  size_t      size,
                  char const *fmt,
                  ...)
{
	va_list ap;
	va_start(ap, fmt);
	size_t len = util_vformat(buf, size, fmt, ap);
	va_end(ap);
	return (struct cstr_view){ buf, len };
}

/** Formats the reason of a refusal into the @p len bytes at @p why as
 *  util_format() does. Returns EINVAL.
 */
[[gnu::format(printf, 3, 4)]]
static inline int
util_refuse (char       *why,
             size_t      len,
             char const *fmt,
             ...)
{
	va_list ap;
	va_start(ap, fmt);
	(void)util_vformat(why, len, fmt, ap);
	va_end(ap);
	return EINVAL;
}

/** A bounded text: @p n bytes of storage, at most n - 1 bytes of text and
 *  a NUL, and the length of the text. A zeroed one is empty. The util_text
 *  macros take a pointer to one.
 */
#define UTIL_TEXT(n) struct { uint32_t len; char data[n]; }

/** A view of the text of @p t. */
#define util_text_view(t) ((struct cstr_view){ (t)->data, (t)->len })

/** Empty the text of @p t. */
#define util_text_clear(t) ((t)->data[0] = '\0', (void)((t)->len = 0))

/** Replace the text of @p t with @p view, cut at a character boundary if it
 *  does not fit. Evaluates to 0, or to EINVAL if it cut @p view.
 */
#define util_text_store(t, view) \
        util_text_store_(&(t)->len, (t)->data, sizeof (t)->data, (view))

/** Append the text that @p fmt formats to the text of @p t, as
 *  util_format() does, cut with "..." if it does not fit. Once the text
 *  fills its storage, nothing more is appended. Evaluates to 0, or to
 *  EINVAL if the text fills its storage.
 */
#define util_text_printf(t, ...) \
        util_text_printf_(&(t)->len, (t)->data, sizeof (t)->data, __VA_ARGS__)

/** util_text_store() on the text of @p *len bytes in the @p size bytes at
 *  @p data.
 */
static inline int
util_text_store_ (uint32_t         *len,
                  char             *data,
                  size_t            size,
                  struct cstr_view  view)
{
	size_t const n = util_clip(view.data, view.len, size - 1);
	if (n)
		memcpy(data, view.data, n);
	data[n] = '\0';
	*len = (uint32_t)n;
	return n < view.len ? EINVAL : 0;
}

/** util_text_printf() on the text of @p *len bytes in the @p size bytes at
 *  @p data.
 */
[[gnu::format(printf, 4, 5)]]
static inline int
util_text_printf_ (uint32_t   *len,
                   char       *data,
                   size_t      size,
                   char const *fmt,
                   ...)
{
	size_t const room = size - *len;
	if (room < 2)
		return EINVAL;
	va_list ap;
	va_start(ap, fmt);
	*len += (uint32_t)util_vformat(data + *len, room, fmt, ap);
	va_end(ap);
	return *len < size - 1 ? 0 : EINVAL;
}

#endif /* ASKAI_SRC_UTIL_H_ */
