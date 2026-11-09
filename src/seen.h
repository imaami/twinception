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
 * the lines of a file that its result showed whole (seen_lines()). A git
 * read and a code describe name the numbered lines of a committed file in
 * their output with a struct call_source (src/mcp.h), and the slot of the
 * call stores a copy of it, a struct seen_read (seen_read_create()).
 *
 * A findings call and a compact call show nothing that a finding can
 * cite. Such a call takes no place in the ring. The ring stores only its
 * number and its tool, apart from the calls, for the last SEEN_NUMBERS
 * such calls (seen_keep_number()), so that the refusal of the call names
 * why the call is not evidence. A resumed ask call records the calls that
 * askai made before it restarted (seen_resume()). seen_find() returns
 * SEEN_RESTARTED for such a call and SEEN_DROPPED for a call that left the
 * ring.
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
#include "mcp.h"
#include "util.h"

struct json_object;

enum {
	SEEN_CALLS    = 64,    /**< calls in the ring of an ask call, at
	                            most */
	SEEN_NAME_MAX = 1024,  /**< bytes of a repository name or a path in
	                            a struct call_source, at most */
	SEEN_NUMBERS  = 1024,  /**< findings and compact calls whose numbers
	                            the ring of an ask call stores, at most */
};

/** The bytes of the arguments and results of the calls in the ring, at
 *  most.
 */
static constexpr size_t SEEN_BYTES = 16U << 20;

/** The flags of a call in the ring. */
enum seen_flag : uint16_t {
	SEEN_FAILED    = 1U << 0,  /**< the call failed */
	SEEN_TOO_LARGE = 1U << 1,  /**< the arguments and the result were
	                                larger than TOOL_TEXT_MAX, so the ring
	                                stores neither */
};

/** Where seen_find() found a call. */
enum seen_where : uint8_t {
	SEEN_KEPT,       /**< the ring contains the call */
	SEEN_NUMBER,     /**< a findings or compact call, whose number and
	                      tool only the ring stores (seen_keep_number()) */
	SEEN_DROPPED,    /**< the call left the ring for newer calls */
	SEEN_RESTARTED,  /**< askai made the call before it restarted */
	SEEN_MISSING,    /**< the ring contains no call of the number: the
	                      call did not run, or no call has the number
	                      yet */
};

/** A position in the numbered lines of an output. seen_lines() counts the
 *  lines of a window from the position of the window before.
 */
struct seen_mark {
	uint64_t line;  /**< number of the line of the byte at `at` */
	size_t   at;    /**< offset in the output, at or after src->at */
};

/** A copy of a struct call_source in one allocation, and the position of
 *  the last window in the numbered lines (seen_lines()).
 */
struct seen_read {
	struct call_source src;     /**< views of text */
	struct seen_mark   mark;    /**< the position of the last window */
	char               text[];  /**< the repository name, the commit ID
	                                 and the path */
};

/** The first and the last line of a file that a window showed whole, or 0
 *  and 0 for no line, and the offset of the first of them in the window.
 */
struct seen_lines {
	uint32_t first;
	uint32_t last;
	uint32_t at;     /**< offset of the start of line first from the start
	                      of the window */
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
	uint32_t          skip;     /**< offset of the window in the output,
	                                 at most TOOL_OUTPUT_MAX */
	struct seen_lines lines;    /**< the lines that the window showed
	                                 whole. The window is at the start of
	                                 result. */
	uint32_t          number;   /**< N of the stamp cN */
	uint32_t          turn;     /**< the turn whose answer made the call */
	uint32_t          tool;     /**< the registry ID of the tool, or
	                                 TOOL_MAX for an unknown tool */
	uint16_t          index;    /**< K of the call in its turn, from 0,
	                                 as in the ID call_T_K of a call
	                                 without an ID from the provider */
	uint16_t          flags;    /**< enum seen_flag */
};

/** A findings call or a compact call, of which the ring stores only the
 *  number and the tool.
 */
struct seen_number {
	uint32_t number;  /**< N of the stamp cN */
	uint32_t tool;    /**< the registry ID of the tool */
};

/** The ring of an ask call. calls[(head + i) % SEEN_CALLS] is the call i of
 *  the n calls in the ring, from the oldest. numbers are the findings and
 *  compact calls, from the oldest.
 */
struct seen {
	struct seen_call calls[SEEN_CALLS];
	UTIL_ARRAY(struct seen_number) numbers;
	size_t           bytes;    /**< bytes of the arguments and results of
	                                the calls in the ring */
	uint32_t         resumed;  /**< the calls that askai made before it
	                                restarted, or 0 (seen_resume()) */
	uint16_t         head;     /**< index of the oldest call */
	uint16_t         n;        /**< calls in the ring */
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
	struct seen_read       *read;    /**< the numbered lines of a file in
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
 * Returns a copy of @p src, whose numbered lines are in the output @p out.
 *
 * @retval nullptr with EFAULT in @p *e if @p src is nullptr, or if
 *         out.data is nullptr and out.len is not 0.
 * @retval nullptr with EINVAL in @p *e if the commit is not 40 or 64
 *         hexadecimal digits, if the repository name or the path is not 1
 *         to SEEN_NAME_MAX bytes of UTF-8 text without NUL, if the line
 *         numbers do not satisfy 1 <= first <= last <= total (or total is
 *         0), if total is greater than UINT32_MAX, or if the numbered lines
 *         are empty, not within @p out, or not last - first + 1 lines.
 * @retval nullptr with ENOMEM in @p *e if out of memory.
 * @retval nullptr for a null @p e.
 */
extern struct seen_read *
seen_read_create (struct call_source const *src,
                  struct cstr_view          out,
                  int                      *e);

/** Frees @p *p_read and sets it to nullptr. */
extern void
seen_read_destroy (struct seen_read **p_read);

/** Returns @p read as a JSON object, or nullptr with EFAULT (for a nullptr
 *  @p read) or ENOMEM in @p *e.
 */
extern struct json_object *
seen_read_json (struct seen_read const *read,
                int                    *e);

/** Returns a new read from the JSON object @p obj that seen_read_json()
 *  made, checked against the output @p out as seen_read_create() checks
 *  it. Returns nullptr with EFAULT (for a nullptr @p obj), EINVAL or ENOMEM
 *  in @p *e on failure.
 */
extern struct seen_read *
seen_read_create_json (struct json_object const *obj,
                       struct cstr_view          out,
                       int                      *e);

/**
 * Returns the first and the last of the numbered lines @p src that the
 * window of @p window bytes of the output @p out from @p skip on shows
 * whole, and the offset of the first of them from @p skip. A line that
 * the window shows only in part does not count. The
 * last line may end without a newline, and it counts if the window ends at
 * the end of the numbered lines. @p mark is the position of the last
 * window. A window counts its lines from that position, or from the start
 * of the numbered lines if the position is after the window, and then
 * moves @p mark to the start of the window. Returns no line if the window
 * shows none.
 *
 * Stores 0 in @p *e, EFAULT for a null @p mark or @p src, or for an @p out
 * with a length and no bytes, or EINVAL if the numbered lines are empty,
 * not within @p out, or more lines than src->first to src->last, or if the
 * first line that the window shows whole starts more than UINT32_MAX bytes
 * after @p skip. Returns no line for a null @p e.
 */
extern struct seen_lines
seen_lines (struct seen_mark         *mark,
            struct call_source const *src,
            struct cstr_view          out,
            size_t                    skip,
            size_t                    window,
            int                      *e);

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
 * whole output, which is at most TOOL_OUTPUT_MAX.
 *
 * With r->read, the ring entry stores the identifiers of the file and the
 * lines that the window shows whole (seen_lines()). The function stores
 * these lines in @p *lines too, if @p lines is not nullptr, and no line
 * for a call without r->read or on failure.
 *
 * Returns 0, EFAULT for a null argument other than @p lines, a null
 * r->parts or an r->out with a length and no bytes, EINVAL if the window
 * parts[0] is not the bytes of r->out from r->skip on, if r->at + r->skip
 * is greater than TOOL_OUTPUT_MAX, or for the errors of seen_lines(), or
 * ENOMEM. On failure the ring does not contain the call,
 * but calls may have left the ring.
 */
extern int
seen_keep (struct seen              **p_seen,
           struct buf                *request,
           struct seen_result const  *r,
           struct seen_lines         *lines);

/** Records in the ring @p *p_seen the call @p number of the tool @p tool,
 *  a findings call or a compact call, as its newest number. The call takes
 *  no place among the calls of the ring. The ring stores no arguments and
 *  no result of the call. The ring is allocated on the first call, and the
 *  oldest number goes if the ring stores SEEN_NUMBERS numbers. Returns 0,
 *  EFAULT for a null @p p_seen, or ENOMEM.
 */
extern int
seen_keep_number (struct seen **p_seen,
                  uint32_t      number,
                  uint32_t      tool);

/** Records in the ring @p *p_seen that askai made @p calls calls of the
 *  ask call before it restarted, as a resumed ask call starts. The ring is
 *  allocated if @p *p_seen is nullptr. Returns 0, EFAULT for a null
 *  @p p_seen, or ENOMEM.
 */
extern int
seen_resume (struct seen **p_seen,
             uint32_t      calls);

/**
 * Finds the call @p number in @p seen. Stores the call in @p *call and
 * returns SEEN_KEPT if the ring contains it. Otherwise stores nullptr. For
 * a findings or compact call whose number the ring stores, the function
 * stores the registry ID of its tool in @p *tool and returns SEEN_NUMBER.
 * Otherwise it stores TOOL_MAX in @p *tool and returns SEEN_RESTARTED for a
 * number from 1 to the calls before a restart (seen_resume()),
 * SEEN_DROPPED for a later number before the oldest call of the ring, and
 * SEEN_MISSING for any other number, also for a null @p seen. A call that
 * did not run, such as a call after compact in its turn, is missing, and
 * so is a number after the newest call.
 */
extern enum seen_where
seen_find (struct seen const       *seen,
           uint32_t                 number,
           struct seen_call const **call,
           uint32_t                *tool);

/** Returns the number of the oldest call among the calls of @p seen, or 0
 *  for a null @p seen or a ring without calls.
 */
extern uint32_t
seen_oldest (struct seen const *seen);

/** Appends to @p d the notice of the calls that @p seen stores, for the
 *  prompt after a compaction: "[askai: askai keeps your calls cA to cB of
 *  this ask call. Cite the calls as evidence by number.]" and a line feed,
 *  with the oldest and the newest call of the ring that a finding can
 *  cite. Appends nothing if @p seen is nullptr or stores no such call.
 *  Returns 0, or ENOMEM.
 */
extern int
seen_notice (struct seen const *seen,
             struct dstr       *d);

/** Drops the calls of @p seen at the end of an ask call, and the calls
 *  before a restart. The storage of the results stays for the next ask
 *  call that uses @p seen. Does nothing for a null @p seen.
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
