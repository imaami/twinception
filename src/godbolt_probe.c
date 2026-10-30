/*
 * The probe of godbolt_targets and the parser of its output
 * (src/godbolt_probe.h).  This file ports src/probe.c of godbolt-c from
 * Jansson to fixed arrays.  The rules that derive the facts are the same.
 *
 * Copyright (c) 2026 godbolt-c contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "godbolt_probe.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
static char const godbolt_probe_text[] =
#include "godbolt_probe.inc"
;
#pragma GCC diagnostic pop

char const *
godbolt_probe_source (size_t *len)
{
	if (len)
		*len = sizeof godbolt_probe_text - 1;
	return godbolt_probe_text;
}

/* Returns whether @p c is ASCII white space. */
static bool
godbolt_probe_space (char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* Returns the value of the hexadecimal digit @p c, or 16 if @p c is not
 * one.
 */
static unsigned
godbolt_probe_hex (char c)
{
	return c >= '0' && c <= '9' ? (unsigned)(c - '0')
	     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a') + 10
	     : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A') + 10 : 16;
}

/* Stores in @p *result the value of the integer expansion @p text of a
 * macro.  The expansion can have parentheses around the number and C
 * suffixes after it.  Returns whether @p text is such a number.
 */
static bool
godbolt_probe_number (char const *text,
                      uint64_t   *result)
{
	if (!text)
		return false;
	size_t open = 0;
	while (godbolt_probe_space(*text) || *text == '(')
		open += *text++ == '(';
	if (*text == '-')
		return false;
	char *end;
	/* strtoull() reports a range error only through errno. */
	errno = 0;
	unsigned long long const value = strtoull(text, &end, 0);
	if (errno || end == text)
		return false;
	unsigned u = 0;
	unsigned l = 0;
	for (; *end == 'u' || *end == 'U' || *end == 'l' || *end == 'L'; ++end) {
		u += *end == 'u' || *end == 'U';
		l += *end == 'l' || *end == 'L';
	}
	while (godbolt_probe_space(*end) || (*end == ')' && open))
		open -= *end++ == ')';
	if (*end || open || u > 1 || l > 2)
		return false;
	*result = (uint64_t)value;
	return true;
}

char const *
godbolt_probe_macro (struct godbolt_probe const *p,
                     char const                 *name)
{
	for (uint32_t i = 0; i < p->n_macros; ++i)
		if (!strcmp(p->macros[i].name, name))
			return p->macros[i].value;
	return nullptr;
}

/* Returns whether the macro @p name is defined, and its expansion is not
 * the number 0.
 */
static bool
godbolt_probe_on (struct godbolt_probe const *p,
                  char const                 *name)
{
	char const *text = godbolt_probe_macro(p, name);
	uint64_t value;
	return text && (!godbolt_probe_number(text, &value) || value);
}

/* Stores in @p *value the number that the macro @p name expands to.
 * Returns whether the macro is defined as a number.
 */
static bool
godbolt_probe_integer (struct godbolt_probe const *p,
                       char const                 *name,
                       uint64_t                   *value)
{
	return godbolt_probe_number(godbolt_probe_macro(p, name), value);
}

/* Adds the macro name @p name to the evidence of the fact @p fact, unless
 * the evidence has it or is full.
 */
static void
godbolt_probe_cite (struct godbolt_probe    *p,
                    enum godbolt_probe_fact  fact,
                    char const              *name)
{
	struct godbolt_probe_claim *c = &p->facts[fact];
	for (uint32_t i = 0; i < c->n; ++i)
		if (!strcmp(c->evidence[i], name))
			return;
	if (c->n < GODBOLT_PROBE_EVIDENCE)
		c->evidence[c->n++] = name;
}

/* Records @p value for the fact @p fact, with the macro @p name as evidence.
 * If the fact has another value already, the fact has a conflict.
 */
static void
godbolt_probe_add_claim (struct godbolt_probe    *p,
                         enum godbolt_probe_fact  fact,
                         char const              *value,
                         char const              *name)
{
	struct godbolt_probe_claim *c = &p->facts[fact];
	c->conflict |= c->value && strcmp(c->value, value);
	c->value = value;
	godbolt_probe_cite(p, fact, name);
}

/* Records @p value for the fact @p fact, with each macro of @p names that is
 * on as evidence.  The list @p names ends with nullptr.  Returns whether a
 * macro of @p names is on.
 */
static bool
godbolt_probe_any (struct godbolt_probe    *p,
                   enum godbolt_probe_fact  fact,
                   char const              *value,
                   char const *const       *names)
{
	bool any = false;
	for (; *names; ++names) {
		bool const on = godbolt_probe_on(p, *names);
		if (on)
			godbolt_probe_add_claim(p, fact, value, *names);
		any |= on;
	}
	return any;
}

#define GODBOLT_PROBE_ANY(fact, value, ...)                                 \
        godbolt_probe_any(p, GODBOLT_PROBE_##fact, value,                   \
                          (char const *const[]){ __VA_ARGS__, nullptr })

/* Derives the architecture.  The function checks the macros of a wider
 * variant of an instruction set before the macros of its family.
 */
static void
godbolt_probe_architecture (struct godbolt_probe *p)
{
	uint64_t width = 0;
	if (!godbolt_probe_on(p, "_M_ARM64EC"))
		GODBOLT_PROBE_ANY(ARCHITECTURE, "x86_64", "__x86_64__", "__amd64__", "_M_X64",
		                  "_M_AMD64");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "i386", "__i386__", "_M_IX86");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "aarch64", "__aarch64__", "_M_ARM64", "_M_ARM64EC");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "arm", "__arm__", "_M_ARM");
	if (godbolt_probe_on(p, "__riscv")) {
		bool const known = godbolt_probe_integer(p, "__riscv_xlen", &width);
		godbolt_probe_add_claim(p, GODBOLT_PROBE_ARCHITECTURE,
		                        known && width == 64 ? "riscv64"
		                        : known && width == 32 ? "riscv32" : "riscv", "__riscv");
		if (godbolt_probe_macro(p, "__riscv_xlen"))
			godbolt_probe_cite(p, GODBOLT_PROBE_ARCHITECTURE, "__riscv_xlen");
	}
	if (!GODBOLT_PROBE_ANY(ARCHITECTURE, "powerpc64", "__powerpc64__", "__ppc64__",
	                       "_ARCH_PPC64"))
		GODBOLT_PROBE_ANY(ARCHITECTURE, "powerpc", "__powerpc__", "__ppc__", "_M_PPC");
	bool wasm = GODBOLT_PROBE_ANY(ARCHITECTURE, "wasm64", "__wasm64__");
	wasm |= GODBOLT_PROBE_ANY(ARCHITECTURE, "wasm32", "__wasm32__");
	if (!wasm)
		GODBOLT_PROBE_ANY(ARCHITECTURE, "wasm", "__wasm__");
	if (!GODBOLT_PROBE_ANY(ARCHITECTURE, "mips64", "__mips64", "__mips64__"))
		GODBOLT_PROBE_ANY(ARCHITECTURE, "mips", "__mips__", "__mips");
	bool const l64 = godbolt_probe_on(p, "__loongarch64");
	if (l64 || (godbolt_probe_integer(p, "__loongarch_grlen", &width) && width == 64))
		godbolt_probe_add_claim(p, GODBOLT_PROBE_ARCHITECTURE, "loongarch64",
		                        l64 ? "__loongarch64" : "__loongarch_grlen");
	else
		GODBOLT_PROBE_ANY(ARCHITECTURE, "loongarch", "__loongarch__");
	if (!GODBOLT_PROBE_ANY(ARCHITECTURE, "s390x", "__s390x__"))
		GODBOLT_PROBE_ANY(ARCHITECTURE, "s390", "__s390__");
	if (!GODBOLT_PROBE_ANY(ARCHITECTURE, "sparc64", "__sparc_v9__", "__sparcv9"))
		GODBOLT_PROBE_ANY(ARCHITECTURE, "sparc", "__sparc__");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "ia64", "__ia64__", "_M_IA64");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "avr", "__AVR__");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "msp430", "__MSP430__");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "m68k", "__m68k__");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "xtensa", "__XTENSA__");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "hexagon", "__hexagon__");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "bpf", "__bpf__", "__BPF__");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "arc", "__arc__");
	GODBOLT_PROBE_ANY(ARCHITECTURE, "sh", "__sh__");
}

/* Records the operating system @p os and its family @p family, with the
 * macro @p name as evidence.
 */
static void
godbolt_probe_os (struct godbolt_probe *p,
                  char const           *os,
                  char const           *family,
                  char const           *name)
{
	godbolt_probe_add_claim(p, GODBOLT_PROBE_OS, os, name);
	godbolt_probe_add_claim(p, GODBOLT_PROBE_OS_FAMILY, family, name);
}

/* Derives the operating system of an Apple target.  TARGET_OS_IPHONE is
 * also on for several more specific platforms, so it counts only if none
 * of them is on.
 */
static void
godbolt_probe_darwin (struct godbolt_probe *p)
{
	static struct {
		char const *name;
		char const *condition;
		char const *version;
	} const variants[] = {
		{ "maccatalyst", "TARGET_OS_MACCATALYST", nullptr },
		{ "visionos", "TARGET_OS_VISION", "__ENVIRONMENT_VISION_OS_VERSION_MIN_REQUIRED__" },
		{ "watchos", "TARGET_OS_WATCH", "__ENVIRONMENT_WATCH_OS_VERSION_MIN_REQUIRED__" },
		{ "tvos", "TARGET_OS_TV", "__ENVIRONMENT_TV_OS_VERSION_MIN_REQUIRED__" },
	};
	bool specific = false;
	for (size_t i = 0; i < array_size(variants); ++i) {
		bool const condition = godbolt_probe_on(p, variants[i].condition);
		bool const version = variants[i].version && godbolt_probe_on(p, variants[i].version);
		if (condition)
			godbolt_probe_os(p, variants[i].name, "darwin", variants[i].condition);
		if (version)
			godbolt_probe_os(p, variants[i].name, "darwin", variants[i].version);
		specific |= condition || version;
	}
	static char const *const iphone[] = {
		"TARGET_OS_IPHONE", "__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__"
	};
	for (size_t i = 0; !specific && i < array_size(iphone); ++i)
		if (godbolt_probe_on(p, iphone[i]))
			godbolt_probe_os(p, "ios", "darwin", iphone[i]);
	if (godbolt_probe_on(p, "TARGET_OS_OSX"))
		godbolt_probe_os(p, "macos", "darwin", "TARGET_OS_OSX");
	if (godbolt_probe_on(p, "__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__"))
		godbolt_probe_os(p, "macos", "darwin", "__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__");
	if (!p->facts[GODBOLT_PROBE_OS].value)
		godbolt_probe_os(p, "darwin", "darwin", "__APPLE__");
	godbolt_probe_cite(p, GODBOLT_PROBE_OS_FAMILY, "__APPLE__");
	godbolt_probe_cite(p, GODBOLT_PROBE_OS_FAMILY, "__MACH__");
}

/* Derives the operating system from the macros of the target, never from
 * the name of the compiler.
 */
static void
godbolt_probe_system (struct godbolt_probe *p)
{
	static struct {
		char const *macro;
		char const *os;
	} const names[] = {
		{ "__FreeBSD__", "freebsd" }, { "__NetBSD__", "netbsd" },
		{ "__OpenBSD__", "openbsd" }, { "__DragonFly__", "dragonfly" },
		{ "_AIX", "aix" }, { "__HAIKU__", "haiku" }, { "__QNX__", "qnx" },
		{ "__QNXNTO__", "qnx" }, { "__wasi__", "wasi" },
		{ "__EMSCRIPTEN__", "emscripten" },
	};
	if (godbolt_probe_on(p, "__APPLE__") && godbolt_probe_on(p, "__MACH__"))
		godbolt_probe_darwin(p);
	if (godbolt_probe_on(p, "__ANDROID__"))
		godbolt_probe_os(p, "android", "linux", "__ANDROID__");
	else if (godbolt_probe_on(p, "__linux__"))
		godbolt_probe_os(p, "linux", "linux", "__linux__");
	if (godbolt_probe_on(p, "__CYGWIN__"))
		godbolt_probe_os(p, "cygwin", "windows", "__CYGWIN__");
	else if (godbolt_probe_on(p, "_WIN64"))
		godbolt_probe_os(p, "windows", "windows", "_WIN64");
	else if (godbolt_probe_on(p, "_WIN32"))
		godbolt_probe_os(p, "windows", "windows", "_WIN32");
	for (size_t i = 0; i < array_size(names); ++i)
		if (godbolt_probe_on(p, names[i].macro))
			godbolt_probe_os(p, names[i].os, names[i].os, names[i].macro);
	if (godbolt_probe_on(p, "__sun") && godbolt_probe_on(p, "__SVR4")) {
		godbolt_probe_os(p, "solaris", "solaris", "__sun");
		godbolt_probe_cite(p, GODBOLT_PROBE_OS, "__SVR4");
	}
}

/* Records the width @p bits of a pointer, with the macro @p name as
 * evidence.  The function ignores a width out of range, and a second width
 * that differs is a conflict.
 */
static void
godbolt_probe_pointer (struct godbolt_probe *p,
                       uint64_t              bits,
                       char const           *name)
{
	if (!bits || bits > 65536)
		return;
	p->facts[GODBOLT_PROBE_POINTER_BITS].conflict |= p->pointer_bits &&
	                                                  p->pointer_bits != bits;
	p->pointer_bits = bits;
	godbolt_probe_cite(p, GODBOLT_PROBE_POINTER_BITS, name);
}

/* Adds the macros of the width of a byte that the block has to the
 * evidence of @p fact.
 */
static void
godbolt_probe_char_bit (struct godbolt_probe    *p,
                        enum godbolt_probe_fact  fact)
{
	if (godbolt_probe_macro(p, "__CHAR_BIT__"))
		godbolt_probe_cite(p, fact, "__CHAR_BIT__");
	if (godbolt_probe_macro(p, "CHAR_BIT"))
		godbolt_probe_cite(p, fact, "CHAR_BIT");
}

/* Derives the data model from the sizes of int, long and pointers, if a
 * byte has 8 bits.
 */
static void
godbolt_probe_sizes (struct godbolt_probe *p,
                     uint64_t              byte)
{
	uint64_t i = 0;
	uint64_t l = 0;
	uint64_t ptr = 0;
	if (byte != 8 || !godbolt_probe_integer(p, "__SIZEOF_INT__", &i) || i != 4 ||
	    !godbolt_probe_integer(p, "__SIZEOF_LONG__", &l) ||
	    !godbolt_probe_integer(p, "__SIZEOF_POINTER__", &ptr))
		return;
	char const *model = ptr == 8 && l == 8 ? "lp64"
	                  : ptr == 8 && l == 4 ? "llp64"
	                  : ptr == 4 && l == 4 ? "ilp32" : nullptr;
	if (!model)
		return;
	godbolt_probe_add_claim(p, GODBOLT_PROBE_DATA_MODEL, model, "__SIZEOF_INT__");
	godbolt_probe_cite(p, GODBOLT_PROBE_DATA_MODEL, "__SIZEOF_LONG__");
	godbolt_probe_cite(p, GODBOLT_PROBE_DATA_MODEL, "__SIZEOF_POINTER__");
	godbolt_probe_char_bit(p, GODBOLT_PROBE_DATA_MODEL);
}

/* Checks the data model against the width of a pointer and the sizes of
 * int and long.  A difference is a conflict of the data model.
 */
static void
godbolt_probe_check_model (struct godbolt_probe *p,
                           uint64_t              byte)
{
	struct godbolt_probe_claim *model = &p->facts[GODBOLT_PROBE_DATA_MODEL];
	struct godbolt_probe_claim const *pointer = &p->facts[GODBOLT_PROBE_POINTER_BITS];
	if (!model->value)
		return;
	uint64_t const bits = strcmp(model->value, "ilp32") ? 64 : 32;
	bool const differs = p->pointer_bits && (bits != p->pointer_bits || pointer->conflict);
	model->conflict |= differs;
	for (uint32_t i = 0; differs && i < pointer->n; ++i)
		godbolt_probe_cite(p, GODBOLT_PROBE_DATA_MODEL, pointer->evidence[i]);
	static char const *const keys[] = { "__SIZEOF_INT__", "__SIZEOF_LONG__" };
	for (size_t i = 0; byte && i < array_size(keys); ++i) {
		uint64_t const want = i && !strcmp(model->value, "lp64") ? 64 : 32;
		uint64_t size = 0;
		if (!godbolt_probe_integer(p, keys[i], &size) ||
		    (size <= 65536 / byte && size * byte == want))
			continue;
		model->conflict = 1;
		godbolt_probe_cite(p, GODBOLT_PROBE_DATA_MODEL, keys[i]);
	}
}

/* Derives the width of a pointer in bits and the data model. */
static void
godbolt_probe_layout (struct godbolt_probe *p)
{
	uint64_t byte = 0;
	uint64_t bits = 0;
	if (godbolt_probe_integer(p, "__CHAR_BIT__", &bits) && bits && bits <= 1024)
		byte = bits;
	if (godbolt_probe_integer(p, "CHAR_BIT", &bits) && bits && bits <= 1024) {
		if (byte && byte != bits) {
			p->facts[GODBOLT_PROBE_POINTER_BITS].conflict = 1;
			godbolt_probe_cite(p, GODBOLT_PROBE_POINTER_BITS, "__CHAR_BIT__");
			godbolt_probe_cite(p, GODBOLT_PROBE_POINTER_BITS, "CHAR_BIT");
		}
		byte = bits;
	}
	uint64_t bytes = 0;
	if (byte && godbolt_probe_integer(p, "__SIZEOF_POINTER__", &bytes) && bytes &&
	    bytes <= 65536 / byte) {
		godbolt_probe_pointer(p, bytes * byte, "__SIZEOF_POINTER__");
		godbolt_probe_char_bit(p, GODBOLT_PROBE_POINTER_BITS);
	}
	if (godbolt_probe_on(p, "_WIN64")) {
		godbolt_probe_pointer(p, 64, "_WIN64");
		godbolt_probe_add_claim(p, GODBOLT_PROBE_DATA_MODEL, "llp64", "_WIN64");
	} else if (godbolt_probe_on(p, "_WIN32")) {
		godbolt_probe_pointer(p, 32, "_WIN32");
		godbolt_probe_add_claim(p, GODBOLT_PROBE_DATA_MODEL, "ilp32", "_WIN32");
	}
	GODBOLT_PROBE_ANY(DATA_MODEL, "lp64", "__LP64__", "_LP64");
	GODBOLT_PROBE_ANY(DATA_MODEL, "ilp32", "__ILP32__", "_ILP32");
	godbolt_probe_sizes(p, byte);
	godbolt_probe_check_model(p, byte);
}

/* Derives the byte order.  Only macros that define the byte order count. */
static void
godbolt_probe_endianness (struct godbolt_probe *p)
{
	static struct {
		char const *key;
		char const *value;
	} const orders[] = {
		{ "__ORDER_LITTLE_ENDIAN__", "little" },
		{ "__ORDER_BIG_ENDIAN__", "big" },
		{ "__ORDER_PDP_ENDIAN__", "pdp" },
	};
	uint64_t order = 0;
	bool const known = godbolt_probe_integer(p, "__BYTE_ORDER__", &order);
	for (size_t i = 0; known && i < array_size(orders); ++i) {
		uint64_t value = 0;
		if (!godbolt_probe_integer(p, orders[i].key, &value) || value != order)
			continue;
		godbolt_probe_add_claim(p, GODBOLT_PROBE_ENDIANNESS, orders[i].value, "__BYTE_ORDER__");
		godbolt_probe_cite(p, GODBOLT_PROBE_ENDIANNESS, orders[i].key);
	}
	GODBOLT_PROBE_ANY(ENDIANNESS, "little", "__LITTLE_ENDIAN__", "__ARMEL__",
	                  "__AARCH64EL__", "__MIPSEL__");
	GODBOLT_PROBE_ANY(ENDIANNESS, "big", "__BIG_ENDIAN__", "__ARMEB__", "__AARCH64EB__",
	                  "__MIPSEB__");
}

/* Derives the C library.  Only libraries that define a macro of their own
 * count.
 */
static void
godbolt_probe_libc (struct godbolt_probe *p)
{
	GODBOLT_PROBE_ANY(LIBC, "glibc", "__GLIBC__");
	GODBOLT_PROBE_ANY(LIBC, "bionic", "__BIONIC__");
	GODBOLT_PROBE_ANY(LIBC, "uclibc", "__UCLIBC__");
	GODBOLT_PROBE_ANY(LIBC, "newlib", "__NEWLIB__");
	GODBOLT_PROBE_ANY(LIBC, "ucrt", "_UCRT", "__MINGW_USE_UCRT");
}

#undef GODBOLT_PROBE_ANY

/* Skips white space at *@p c, up to @p end. */
static void
godbolt_probe_skip (char const **c,
                    char const  *end)
{
	while (*c < end && godbolt_probe_space(**c))
		++*c;
}

/* Decodes the hexadecimal digits of a universal character name at *@p c
 * into UTF-8 at *@p out, and advances both.  @p digits is 4 or 8.  Returns
 * 0, or EPROTO if the digits are missing or name no character.
 */
static int
godbolt_probe_ucn (char const **c,
                   char const  *end,
                   char       **out,
                   unsigned     digits)
{
	uint32_t scalar = 0;
	for (unsigned i = 0; i < digits; ++i) {
		unsigned const h = *c < end ? godbolt_probe_hex(**c) : 16;
		if (h > 15)
			return EPROTO;
		scalar = scalar << 4 | h;
		++*c;
	}
	if (!scalar || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
		return EPROTO;
	static unsigned char const lead[] = { 0, 0, 0xc0, 0xe0, 0xf0 };
	unsigned const n = scalar < 0x80 ? 1 : scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
	for (unsigned i = n - 1; i; --i, scalar >>= 6)
		(*out)[i] = (char)(0x80 | (scalar & 0x3f));
	(*out)[0] = (char)(lead[n] | scalar);
	*out += n;
	return 0;
}

/* Decodes the escape sequence at *@p c, after its backslash, into *@p out,
 * and advances both.  Returns 0, or EPROTO if the sequence is not valid or
 * decodes to a NUL byte.
 */
static int
godbolt_probe_escape (char const **c,
                      char const  *end,
                      char       **out)
{
	static char const simple[0x80] = {
		['a'] = '\a', ['b'] = '\b', ['f'] = '\f', ['n'] = '\n', ['r'] = '\r',
		['t'] = '\t', ['v'] = '\v', ['\\'] = '\\', ['\''] = '\'', ['"'] = '"',
		['?'] = '?'
	};
	if (*c == end)
		return EPROTO;
	unsigned char const e = (unsigned char)*(*c)++;
	if (e < sizeof simple && simple[e]) {
		*(*out)++ = simple[e];
		return 0;
	}
	if (e == 'u' || e == 'U')
		return godbolt_probe_ucn(c, end, out, e == 'u' ? 4 : 8);
	unsigned value = 0;
	unsigned digits = 0;
	if (e == 'x') {
		for (; *c < end && godbolt_probe_hex(**c) < 16 && value <= UCHAR_MAX; ++digits)
			value = value << 4 | godbolt_probe_hex(*(*c)++);
	} else if (e >= '0' && e <= '7') {
		value = e - '0';
		for (digits = 1; digits < 3 && *c < end && **c >= '0' && **c <= '7'; ++digits)
			value = value << 3 | (unsigned)(*(*c)++ - '0');
	}
	if (!digits || !value || value > UCHAR_MAX)
		return EPROTO;
	*(*out)++ = (char)value;
	return 0;
}

/* Decodes the C string literal at *@p c, after its opening quote, into
 * *@p out, and advances both past it.  Returns 0, or EPROTO if the literal
 * has no closing quote, a NUL byte or an escape sequence that is not
 * valid.
 */
static int
godbolt_probe_string (char const **c,
                      char const  *end,
                      char       **out)
{
	while (*c < end) {
		char const b = *(*c)++;
		if (b == '"')
			return 0;
		if (!b)
			return EPROTO;
		if (b != '\\') {
			*(*out)++ = b;
			continue;
		}
		int const e = godbolt_probe_escape(c, end, out);
		if (e)
			return e;
	}
	return EPROTO;
}

/* Decodes one or more adjacent C string literals at *@p c into *@p out, with
 * a NUL after them, and advances both.  Returns the decoded text, or
 * nullptr if *@p c is not at such literals.
 */
static char const *
godbolt_probe_literal (char const **c,
                       char const  *end,
                       char       **out)
{
	char const *text = *out;
	godbolt_probe_skip(c, end);
	if (*c == end || **c != '"')
		return nullptr;
	while (*c < end && **c == '"') {
		++*c;
		if (godbolt_probe_string(c, end, out))
			return nullptr;
		godbolt_probe_skip(c, end);
	}
	*(*out)++ = '\0';
	return text;
}

/* Parses the rest of a record line, ("NAME", "VALUE"), from @p c to @p end,
 * into a record of @p list, which has *@p n records and room for @p max.
 * Returns 0, or EPROTO with the reason in p->why.
 */
static int
godbolt_probe_parse_record (struct godbolt_probe         *p,
                            struct godbolt_probe_record  *list,
                            uint32_t                     *n,
                            uint32_t                      max,
                            char const                   *c,
                            char const                   *end,
                            char                        **out)
{
	godbolt_probe_skip(&c, end);
	char const *name = c < end && *c++ == '(' ? godbolt_probe_literal(&c, end, out) : nullptr;
	char const *value = name && *name && c < end && *c++ == ','
	                  ? godbolt_probe_literal(&c, end, out) : nullptr;
	bool const closed = value && c < end && *c++ == ')';
	if (closed)
		godbolt_probe_skip(&c, end);
	if (!closed || c != end) {
		p->why = "a record of the block is not valid";
		return EPROTO;
	}
	if ((size_t)(value - name) > GODBOLT_PROBE_TEXT_MAX + 1 ||
	    (size_t)(*out - value) > GODBOLT_PROBE_TEXT_MAX + 1) {
		p->why = "a name or a value of a record has more than 256 bytes";
		return EPROTO;
	}
	for (uint32_t i = 0; i < *n; ++i) {
		if (!strcmp(list[i].name, name)) {
			p->why = "the block has a record twice";
			return EPROTO;
		}
	}
	if (*n == max) {
		p->why = "the block has too many records";
		return EPROTO;
	}
	list[(*n)++] = (struct godbolt_probe_record){ name, value };
	return 0;
}

static_assert(GODBOLT_PROBE_TEXT_MAX == 256, "the reason says 256 bytes");

/* Returns whether the @p n bytes at @p line are @p word. */
static bool
godbolt_probe_is (char const *line,
                  size_t      n,
                  char const *word,
                  size_t      len)
{
	return n == len && !memcmp(line, word, n);
}

/* Returns whether the @p n bytes at @p line begin with @p word. */
static bool
godbolt_probe_begins (char const *line,
                      size_t      n,
                      char const *word,
                      size_t      len)
{
	return n >= len && !memcmp(line, word, len);
}

#define GODBOLT_PROBE_IS(word) godbolt_probe_is(line, n, word, sizeof word - 1)
#define GODBOLT_PROBE_BEGINS(word) godbolt_probe_begins(line, n, word, sizeof word - 1)

/* Parses the line of @p n bytes at @p line, without white space at its
 * ends.  *@p state is 0 before the block, 1 in it and 2 after it.  The line
 * GBPROBE_BEGIN_1 is valid only before the block, and every other line of
 * the block only in it.  Returns 0, or an error number with the reason in
 * p->why.
 */
static int
godbolt_probe_line (struct godbolt_probe  *p,
                    char const            *line,
                    size_t                 n,
                    unsigned              *state,
                    char                 **out)
{
	unsigned const mark = GODBOLT_PROBE_IS("GBPROBE_BEGIN_1") ? 1
	                    : GODBOLT_PROBE_IS("GBPROBE_END_1") ? 2 : 0;
	bool const macro = GODBOLT_PROBE_BEGINS("GBPROBE_MACRO");
	bool const header = GODBOLT_PROBE_BEGINS("GBPROBE_HEADER");
	if (GODBOLT_PROBE_IS("[Truncated]")) {
		p->why = "Compiler Explorer cut the output";
		return EOVERFLOW;
	}
	if ((mark && mark != *state + 1) || ((macro || header) && *state != 1)) {
		p->why = "the output has a line of the block outside of one block";
		return EPROTO;
	}
	if (mark)
		*state = mark;
	if (macro)
		return godbolt_probe_parse_record(p, p->macros, &p->n_macros, GODBOLT_PROBE_MACROS,
		                                  line + sizeof "GBPROBE_MACRO" - 1, line + n, out);
	if (header)
		return godbolt_probe_parse_record(p, p->headers, &p->n_headers,
		                                  GODBOLT_PROBE_HEADERS,
		                                  line + sizeof "GBPROBE_HEADER" - 1, line + n, out);
	if (!mark && *state == 1 && n && *line != '#') {
		p->why = "the block has a line that is not a record";
		return EPROTO;
	}
	return 0;
}

#undef GODBOLT_PROBE_BEGINS
#undef GODBOLT_PROBE_IS

/* Parses the @p len bytes at @p text line by line.  Returns 0, or an error
 * number with the reason in p->why.
 */
static int
godbolt_probe_parse (struct godbolt_probe *p,
                     char const           *text,
                     size_t                len)
{
	char *out = p->store;
	unsigned state = 0;
	char const *const finish = text + len;
	for (char const *line = text; line < finish; ) {
		char const *nl = memchr(line, '\n', (size_t)(finish - line));
		char const *end = nl ? nl : finish;
		char const *const next = nl ? nl + 1 : finish;
		godbolt_probe_skip(&line, end);
		while (end > line && godbolt_probe_space(end[-1]))
			--end;
		int const e = godbolt_probe_line(p, line, (size_t)(end - line), &state, &out);
		if (e)
			return e;
		line = next;
	}
	if (state == 2)
		return 0;
	p->why = "the output has no complete block";
	return EPROTO;
}

int
godbolt_probe_init (struct godbolt_probe *dest,
                    char const           *text,
                    size_t                len)
{
	if (!dest)
		return EINVAL;
	*dest = (struct godbolt_probe){};
	if (!text && len) {
		dest->why = "the output is missing";
		return EINVAL;
	}
	dest->store = malloc(len + 1);
	if (!dest->store) {
		dest->why = "the parser ran out of memory";
		return ENOMEM;
	}
	int const e = godbolt_probe_parse(dest, text ? text : "", len);
	if (e) {
		godbolt_probe_fini(dest);
		return e;
	}
	godbolt_probe_architecture(dest);
	godbolt_probe_system(dest);
	godbolt_probe_layout(dest);
	godbolt_probe_endianness(dest);
	godbolt_probe_libc(dest);
	return 0;
}

void
godbolt_probe_fini (struct godbolt_probe *probe)
{
	if (!probe)
		return;
	free(probe->store);
	probe->store = nullptr;
	probe->n_macros = 0;
	probe->n_headers = 0;
}
