/** @file review.h
 *
 * The review record of an ask call has two parts. The read log holds the
 * lines of committed files that git read results showed the turns of the
 * call. The findings are what the turns record with the findings tool.
 * A finding has a claim, an optional location (a path and a line range)
 * and a state. Each change of a finding goes into its history with the
 * turn, the evidence, and the commit at which the lines of the location
 * were read. The record, not the model, chooses that commit: the commit
 * of the latest read that showed the lines. verified and disproved need
 * the lines of the location in the read log.
 *
 * The lines that a result shows are pending until the upstream answers
 * the request that holds the result. review_answered() then moves them
 * into the read log, and review_compacted() drops them if a compaction
 * removes the result first. So the reads of a turn count from the next
 * turn on.
 *
 * A record holds at most REVIEW_MAX charged bytes, pending lines included:
 * REVIEW_ITEM for each file and each line range, the struct size for each
 * finding and each change, and the bytes of their texts. Nothing depends
 * on the wall clock, so a findings call repeated with the same record,
 * arguments and turn gives the same result. Each function that has an
 * argument @p e returns nullptr if @p e is nullptr.
 */
#ifndef TWINCEPTION_SRC_REVIEW_H_
#define TWINCEPTION_SRC_REVIEW_H_

#include <stddef.h>
#include <stdint.h>

#include "tool.h"

struct dstr;
struct json_object;

enum {
	REVIEW_MAX    = 4 << 20,  /**< maximum charged bytes of a record */
	REVIEW_ITEM   = 64,       /**< bytes charged for each file and each line
	                               range */
	REVIEW_CLOSED = 200,      /**< maximum bytes of the claim of a closed
	                               finding in a render */
	REVIEW_WORD   = 1024,     /**< maximum bytes of a repository name or a
	                               path */
};

/** The flags of review_create(). */
enum review_flag : uint8_t {
	REVIEW_READS = 1U << 0U,  /**< the turns of the ask call have the git
	                               tool */
};

/** The states of a finding. Candidate and verified findings are open;
 *  disproved and retracted findings are closed.
 */
enum review_state : uint8_t {
	REVIEW_CANDIDATE,
	REVIEW_VERIFIED,
	REVIEW_DISPROVED,
	REVIEW_RETRACTED,
	REVIEW_STATES
};

/** The actions of the findings tool. */
enum review_action : uint8_t {
	REVIEW_ADD,
	REVIEW_UPDATE,
	REVIEW_SHOW,
	REVIEW_ACTIONS
};

/** The state names that the findings tool takes. Each is NUL-terminated. */
extern struct tool_text const review_state_names[REVIEW_STATES];

/** The action names that the findings tool takes. Each is NUL-terminated. */
extern struct tool_text const review_action_names[REVIEW_ACTIONS];

/** The numbered lines of a committed file in the output of a git read. The
 *  texts are borrowed.
 */
struct review_source {
	struct tool_text repo;    /**< repository name */
	struct tool_text commit;  /**< full commit ID */
	struct tool_text path;    /**< path of the file in the commit */
	size_t           at;      /**< offset of the numbered lines in the
	                               output */
	size_t           len;     /**< bytes of the numbered lines: one output
	                               line for each line from first to last.
	                               Each ends with a newline, but the last
	                               may have none. */
	uint32_t         first;   /**< number of the first line, from 1 */
	uint32_t         last;    /**< number of the last line */
	uint32_t         total;   /**< line count of the file; 0 if unknown */
};

/** A review record. */
struct review;

/** An owned copy of a struct review_source. */
struct review_read;

/** Returns how many of the @p len bytes at @p text to keep, without the
 *  notices at the end that the caller appends to tool results. A model can
 *  copy those into a claim or evidence. askai passes notice_kept(). The
 *  record also drops the spaces and line ends before the notices.
 */
typedef size_t review_kept_fn (char const *text,
                               size_t      len);

/** Returns a new empty record with the enum review_flag bits @p flags, or
 *  nullptr with ENOMEM in @p *e. Without REVIEW_READS the read log stays
 *  empty, so verified and disproved are refused and no result suggests a
 *  git read.
 */
extern struct review *
review_create (uint32_t  flags,
               int      *e);

/** Frees @p *p_review and sets it to nullptr. */
extern void
review_destroy (struct review **p_review);

/**
 * Returns a copy of @p src, whose numbered lines are in the output @p out,
 * or nullptr with the error in @p *e: EFAULT for nullptr; EINVAL if the
 * commit is not 40 or 64 hexadecimal digits, the repository name or the
 * path is not 1 to REVIEW_WORD bytes of UTF-8 text without NUL, the lines
 * do not satisfy 1 <= first <= last <= total (or total is 0), the numbered
 * lines are empty or not all within @p out, or they are not
 * last - first + 1 lines; or ENOMEM.
 */
extern struct review_read *
review_read_create (struct review_source const *src,
                    struct tool_text            out,
                    int                        *e);

/** Frees @p *p_read and sets it to nullptr. */
extern void
review_read_destroy (struct review_read **p_read);

/** Returns @p read as a JSON object, or nullptr with EFAULT or ENOMEM in
 *  @p *e.
 */
extern struct json_object *
review_read_json (struct review_read const *read,
                  int                      *e);

/** Parses @p obj, made by review_read_json(), into a new read, and checks
 *  it against the output @p out as review_read_create() does. Returns the
 *  read, or nullptr with EFAULT, EINVAL or ENOMEM in @p *e.
 */
extern struct review_read *
review_read_restore (struct json_object const *obj,
                     struct tool_text          out,
                     int                      *e);

/**
 * Adds to the pending lines of @p r the lines of @p read that a result in
 * turn @p turn shows whole. The result shows the @p window bytes of the
 * output @p out from byte @p skip on. A line that the window cuts is not
 * added. If the lines would take the record past REVIEW_MAX, they are
 * dropped, and the record notes the first turn whose lines it dropped.
 * Returns 0, EFAULT for nullptr, EINVAL if the numbered lines of @p read
 * are not within @p out or hold more lines than @p read says, or ENOMEM.
 */
extern int
review_shown (struct review            *r,
              struct review_read const *read,
              struct tool_text          out,
              size_t                    skip,
              size_t                    window,
              uint32_t                  turn);

/** Moves the pending lines of @p r into the read log (see the file
 *  comment). The lines replace what the log has for them, so the commit of
 *  a location is the commit of the latest read that showed its lines.
 *  Lines that would take the record past REVIEW_MAX are dropped, as in
 *  review_shown(). Returns 0, EFAULT for nullptr, or ENOMEM; after ENOMEM
 *  some lines are not logged.
 */
extern int
review_answered (struct review *r);

/** Drops the pending lines of @p r (see the file comment). Does nothing if
 *  @p r is nullptr.
 */
extern void
review_compacted (struct review *r);

/**
 * Runs the findings call with the arguments @p args in turn @p turn and
 * appends its result to @p out. add records a finding, update changes
 * one, and show writes the record or, with an id, the history of one
 * finding. If @p kept is not nullptr, it trims claims and evidence. A
 * show writes at most @p most bytes of its text, from byte @p from on; add
 * and update ignore both. Returns 0; EAGAIN if the text of a show goes on
 * past these bytes (call again with a larger @p from); EINVAL with a
 * refusal for the model in @p out; EFAULT for nullptr; or ENOMEM.
 */
extern int
review_call (struct review      *r,
             struct json_object *args,
             uint32_t            turn,
             review_kept_fn     *kept,
             size_t              from,
             size_t              most,
             struct dstr        *out);

/**
 * Appends to @p out the text of the record @p r, or of an empty record if
 * @p r is nullptr: a header, the findings, then the read log. A text over
 * @p most bytes is cut after its last newline within them, and a notice
 * gives that offset for findings action show. Returns 0, EFAULT for
 * nullptr @p out, or ENOMEM.
 */
extern int
review_render (struct review const *r,
               struct dstr         *out,
               size_t               most);

/** Returns @p r as a JSON object, or nullptr with EFAULT or ENOMEM in
 *  @p *e.
 */
extern struct json_object *
review_json (struct review const *r,
             int                 *e);

/** Replaces the contents of @p r with the record in @p obj, made by
 *  review_json(), pending lines included. @p r keeps its flags from
 *  review_create(). Returns 0; EFAULT for nullptr; EINVAL if @p obj is not
 *  such a record or is over REVIEW_MAX; or ENOMEM. On error @p r is
 *  unchanged.
 */
extern int
review_restore (struct review            *r,
                struct json_object const *obj);

#endif /* TWINCEPTION_SRC_REVIEW_H_ */
