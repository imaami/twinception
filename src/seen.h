/** @file seen.h
 *
 * The calls whose results askai showed to the turns of an ask call. The
 * note of each result of a turn's call contains the stamp [askai: call cN]
 * (NOTICE_CALL), after the notebook notice and before the context notices.
 * N counts the calls of the ask call in call order, from 1, whatever order
 * the calls end in.
 *
 * The ring of an ask call stores the last SEEN_CALLS calls, with at most
 * SEEN_BYTES bytes of arguments and results, and drops the oldest call
 * first. The newest call always stays. For each call, the ring stores the
 * arguments as the upstream sent them and the result as the turn got it:
 * the window of the output, the note and the notices, before the escapes
 * of the upstream's API. A call whose arguments and result are together
 * larger than TOOL_TEXT_MAX stays as a number only (SEEN_TOO_LARGE). The
 * ring also stores the identifiers of the material that a call showed, and
 * the lines of a file that its result showed whole (seen_lines()).
 *
 * The storage of a dropped result stays for the next call in the same
 * place of the ring. The storage of dropped arguments goes back to the
 * thread's stash (src/buf.h).
 */
#ifndef ASKAI_SRC_SEEN_H_
#define ASKAI_SRC_SEEN_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "cstr_view.h"
#include "dstr.h"

struct review_read;
struct review_source;

/** The calls in the ring of an ask call, at most. */
enum { SEEN_CALLS = 64 };

/** The bytes of the arguments and results of the calls in the ring, at
 *  most.
 */
static constexpr size_t SEEN_BYTES = 16U << 20;

/** The flags of a call in the ring. */
enum seen_flag : uint16_t {
	SEEN_FAILED    = 1U << 0,  /**< the call failed */
	SEEN_TOO_LARGE = 1U << 1,  /**< the arguments and the result were larger
	                                than TOOL_TEXT_MAX, so the ring stores
	                                neither */
};

/** A position in the numbered lines of an output. seen_lines() counts the
 *  lines of a window from the position of the window before.
 */
struct seen_mark {
	uint64_t line;  /**< number of the line of the byte at `at` */
	size_t   at;    /**< offset in the output, at or after src->at */
};

/** The first and the last line of a file that a window showed whole, or 0
 *  and 0 for no line.
 */
struct seen_lines {
	uint32_t first;
	uint32_t last;
};

/** A call in the ring. */
struct seen_call {
	struct buf        request;  /**< the struct job_call of the call and
	                                 the parts after it, as the upstream
	                                 sent them */
	struct dstr       result;   /**< the window, the note and the notices,
	                                 as the turn got them */
	struct dstr       ids;      /**< the identifiers of the material as a
	                                 JSON object, or empty */
	uint64_t          output;   /**< bytes of the whole output */
	uint64_t          skip;     /**< offset of the window in the output */
	struct seen_lines lines;    /**< the lines that the window showed whole */
	uint32_t          number;   /**< N of the stamp cN */
	uint32_t          turn;     /**< the turn whose answer made the call */
	uint32_t          tool;     /**< the registry ID of the tool, or
	                                 TOOL_MAX for an unknown tool */
	uint16_t          index;    /**< K of the call in its turn, from 0,
	                                 as in the ID call_T_K of a call
	                                 without an ID from the provider */
	uint16_t          flags;    /**< enum seen_flag */
};

/** The ring of an ask call. calls[(head + i) % SEEN_CALLS] is the call i of
 *  the n calls in the ring, from the oldest.
 */
struct seen {
	struct seen_call calls[SEEN_CALLS];
	size_t           bytes;  /**< bytes of the arguments and results of
	                              the calls in the ring */
	uint32_t         head;   /**< index of the oldest call */
	uint32_t         n;      /**< calls in the ring */
};

/** The parts of the result of a turn's call, in order: the window of the
 *  output, the note after the window, a blank line if the notices of the
 *  agenda follow or else nothing, and the notices of the agenda.
 */
enum { SEEN_PARTS = 4 };

/** The result of a turn's call for seen_keep(). The views stay the
 *  caller's.
 */
struct seen_result {
	struct cstr_view const *parts;   /**< SEEN_PARTS views, the result as
	                                      the turn got it. parts[0] is the
	                                      window: the bytes of @p out from
	                                      @p skip on. */
	struct cstr_view        out;     /**< the output, or the part of the
	                                      output that the journal recorded
	                                      for a replayed result */
	struct review_read     *read;    /**< the numbered lines of a file in
	                                      @p out, or nullptr */
	uint64_t                output;  /**< bytes of the whole output */
	uint64_t                at;      /**< offset of @p out in the whole
	                                      output */
	size_t                  skip;    /**< offset of the window in @p out */
	uint32_t                number;  /**< N of the stamp cN */
	uint32_t                turn;    /**< the turn whose answer made the
	                                      call */
	uint32_t                tool;    /**< the registry ID of the tool */
	uint16_t                index;   /**< K of the call in its turn */
	uint16_t                flags;   /**< enum seen_flag: SEEN_FAILED */
};

/**
 * Returns the first and the last of the numbered lines @p src that the
 * window of @p window bytes of the output @p out from @p skip on shows
 * whole. A line that the window shows only in part does not count. The
 * last line may end without a newline, and it counts if the window ends at
 * the end of the numbered lines. @p mark is the position of the last
 * window. A window counts its lines from that position, or from the start
 * of the numbered lines if the position is after the window, and then
 * moves @p mark to the start of the window. Returns no line if the window
 * shows none.
 *
 * Stores 0 in @p *e, EFAULT for a null @p mark or @p src, or for an @p out
 * with a length and no bytes, or EINVAL if the numbered lines are empty,
 * not within @p out, or more lines than src->first to src->last. Returns
 * no line for a null @p e.
 */
extern struct seen_lines
seen_lines (struct seen_mark           *mark,
            struct review_source const *src,
            struct cstr_view            out,
            size_t                      skip,
            size_t                      window,
            int                        *e);

/**
 * Adds the call @p r with the arguments in @p request to the ring
 * @p *p_seen as its newest call. The ring is allocated on the first call,
 * and @p *p_seen then points to it. The oldest calls leave the ring until
 * the call fits: at most SEEN_CALLS calls with at most SEEN_BYTES bytes.
 * The storage of @p request moves into the ring, and @p request is left
 * empty. The ring stores a copy of the parts of the result, r->parts. If
 * the arguments and the result are larger than TOOL_TEXT_MAX together, the
 * ring stores neither, and the flags of the call contain SEEN_TOO_LARGE.
 * The ring entry stores r->at + r->skip as the offset of the window in the
 * whole output.
 *
 * With r->read, the ring entry stores the identifiers of the file and the
 * lines that the window shows whole (seen_lines()). The function stores
 * these lines in @p *lines too, if @p lines is not nullptr, and no line
 * for a call without r->read or on failure.
 *
 * Returns 0, EFAULT for a null argument other than @p lines, a null
 * r->parts or an r->out with a length and no bytes, EINVAL if the window
 * parts[0] is not the bytes of r->out from r->skip on or for the errors of
 * seen_lines(), or ENOMEM. On failure the ring does not contain the call,
 * but calls may have left the ring.
 */
extern int
seen_keep (struct seen              **p_seen,
           struct buf                *request,
           struct seen_result const  *r,
           struct seen_lines         *lines);

/** Drops the calls of @p seen at the end of an ask call. The storage of
 *  the results stays for the next ask call that uses @p seen. Does
 *  nothing for a null @p seen.
 */
extern void
seen_clear (struct seen *seen);

/** Returns the bytes of memory of @p seen and its storage, or 0 for a null
 *  @p seen.
 */
extern size_t
seen_kept (struct seen const *seen);

/** Frees @p *p_seen and its storage and sets @p *p_seen to nullptr. Does
 *  nothing for a null @p p_seen.
 */
extern void
seen_destroy (struct seen **p_seen);

#endif /* ASKAI_SRC_SEEN_H_ */
