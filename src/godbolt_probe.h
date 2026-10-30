/** @file godbolt_probe.h
 *
 * The probe of godbolt_targets (src/godbolt.c), and the parser of its output.
 *
 * The probe is C source that a compiler only preprocesses.  The output has a
 * block from a line GBPROBE_BEGIN_1 to a line GBPROBE_END_1.  The block has
 * a line GBPROBE_MACRO("NAME", "VALUE") for each macro of a fixed list that
 * the compiler predefines, and a line GBPROBE_HEADER("NAME", "STATUS") for
 * each of a few target headers.  godbolt_probe_init() parses the block, and
 * it derives facts about the target from the macros.  The module uses only
 * libc.
 */
#ifndef TWINCEPTION_SRC_GODBOLT_PROBE_H_
#define TWINCEPTION_SRC_GODBOLT_PROBE_H_

#include <stddef.h>
#include <stdint.h>

/** The facts about a target. */
enum godbolt_probe_fact : uint8_t {
	GODBOLT_PROBE_ARCHITECTURE,
	GODBOLT_PROBE_OS,
	GODBOLT_PROBE_OS_FAMILY,
	GODBOLT_PROBE_DATA_MODEL,    /**< the sizes of int, long and pointers */
	GODBOLT_PROBE_POINTER_BITS,  /**< the value is in pointer_bits */
	GODBOLT_PROBE_ENDIANNESS,
	GODBOLT_PROBE_LIBC,
	GODBOLT_PROBE_FACTS
};

enum {
	GODBOLT_PROBE_MACROS   = 160,  /**< the largest number of macro
	                                    records of a block */
	GODBOLT_PROBE_HEADERS  = 8,    /**< the largest number of header
	                                    records of a block */
	GODBOLT_PROBE_EVIDENCE = 24,   /**< the largest number of macro names
	                                    of a fact */
	GODBOLT_PROBE_TEXT_MAX = 256,  /**< the largest length in bytes of the
	                                    name or the value of a record */
};

/** A record of the block.  The name and the value are NUL-terminated, in the
 *  memory of the probe.
 */
struct godbolt_probe_record {
	char const *name;
	char const *value;
};

/** A fact about the target, and the names of the macros that it comes
 *  from.
 */
struct godbolt_probe_claim {
	char const *value;     /**< a constant text, or nullptr if no macro
	                            is evidence of the fact */
	char const *evidence[GODBOLT_PROBE_EVIDENCE];  /**< constant texts */
	uint32_t    n;         /**< the number of names in evidence */
	uint32_t    conflict;  /**< 1 if the macros are evidence of different
	                            values */
};

/** The parsed block of the output of the probe. */
struct godbolt_probe {
	struct godbolt_probe_claim  facts[GODBOLT_PROBE_FACTS];
	struct godbolt_probe_record macros[GODBOLT_PROBE_MACROS];
	struct godbolt_probe_record headers[GODBOLT_PROBE_HEADERS];
	char                       *store;  /**< the names and values */
	char const                 *why;    /**< the reason of an error of
	                                         godbolt_probe_init() */
	uint64_t                    pointer_bits;  /**< 0 if unknown */
	uint32_t                    n_macros;
	uint32_t                    n_headers;
};

/** Returns the source of the probe, NUL-terminated, and stores its length
 *  in @p *len.  A line "#define GODBOLT_PROBE_HEADERS 1" before the source
 *  makes the compiler include the headers of the header records.
 */
extern char const *
godbolt_probe_source (size_t *len);

/**
 * Parses the @p len bytes at @p text, the output of the probe, into
 * @p dest.  The output must have exactly one complete block, and each line
 * of the block must be a record, empty, or a line that begins with '#'.
 * The name and the value of a record have at most GODBOLT_PROBE_TEXT_MAX
 * bytes each.  Then the function derives the facts from the macros.  If
 * the macros are evidence of different values of a fact, the fact has
 * conflict 1.
 *
 * Returns 0, or an error number with a constant reason in dest->why:
 * EOVERFLOW if a line is "[Truncated]", EPROTO if the block is missing or
 * not valid, EINVAL if @p text is nullptr and @p len is not 0, or ENOMEM.
 * It returns EINVAL without a reason if @p dest is nullptr.  After an
 * error, @p dest has no memory to free.
 */
extern int
godbolt_probe_init (struct godbolt_probe *dest,
                    char const           *text,
                    size_t                len);

/** Frees the memory of @p probe. */
extern void
godbolt_probe_fini (struct godbolt_probe *probe);

/** Returns the expansion of the macro @p name, or nullptr if the block of
 *  @p probe has no record of the macro.
 */
extern char const *
godbolt_probe_macro (struct godbolt_probe const *probe,
                     char const                 *name);

#endif /* TWINCEPTION_SRC_GODBOLT_PROBE_H_ */
