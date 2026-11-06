/** @file review.h
 *
 * The review record of an ask call. It consists of two parts:
 *
 * - The read log. It records which lines of committed files the results of
 *   git read calls showed to the turns of the call.
 * - The findings, which the turns record with the findings tool. A finding
 *   consists of a claim, an optional location (a path and a line range), a
 *   state and its changes. Each change is stored with the turn, the
 *   evidence and the commit at which the lines of the location were read.
 *   The record sets that commit from the read log: it is the commit of the
 *   latest read that showed the lines, among the commits that start with
 *   the commit prefix of the location, if the location contains one. A
 *   finding can be verified or disproved only if the read log contains the
 *   lines of its location.
 *
 * The lines that a result shows are pending until the upstream answers the
 * request that contains the result. review_pending_log() then moves them
 * into the read log, and review_pending_drop() drops them when a compaction
 * removes the result first. So the lines that a result in turn N shows are
 * in the read log from turn N + 1 on.
 *
 * A record is charged at most REVIEW_MAX bytes, pending lines included:
 * REVIEW_ITEM for each file, line range, finding and change, and the bytes
 * of their texts. Nothing depends on the wall clock, so a findings call
 * repeated with the same record, arguments and turn returns the same
 * result. A function with an int *e parameter returns nullptr if e is
 * nullptr.
 */
#ifndef TWINCEPTION_SRC_REVIEW_H_
#define TWINCEPTION_SRC_REVIEW_H_

#include <stddef.h>
#include <stdint.h>

#include "cstr_view.h"
#include "tool.h"

struct dstr;
struct json_object;

enum {
	REVIEW_MAX      = 4 << 20,  /**< the most bytes charged for a record */
	REVIEW_ITEM     = 64,       /**< the bytes charged for each file, line
	                                 range, finding and change */
	REVIEW_NAME_MAX = 1024,     /**< the most bytes of a repository name or
	                                 a path */
};

/** The flags of review_create(). */
enum review_flag : uint32_t {
	REVIEW_READ_LOG = 1U << 0U,  /**< the turns of the ask call can call the
	                                  git tool, so the record keeps a read
	                                  log */
};

/** The states of a finding. Candidate and verified findings are open.
 *  Disproved and retracted findings are closed.
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

/** The names of the states in the findings tool. Each is NUL-terminated. */
extern struct cstr_view const review_state_names[REVIEW_STATES];

/** The names of the actions of the findings tool. Each is NUL-terminated. */
extern struct cstr_view const review_action_names[REVIEW_ACTIONS];

/** The numbered lines of a committed file in the output of a git read. The
 *  texts are views of the caller's storage.
 */
struct review_source {
	struct cstr_view repo;    /**< repository name */
	struct cstr_view commit;  /**< full commit ID */
	struct cstr_view path;    /**< path of the file in the commit */
	size_t           at;      /**< offset of the numbered lines in the
	                               output */
	size_t           len;     /**< bytes of the numbered lines. There is one
	                               output line for each line from first to
	                               last, and each output line ends with a
	                               newline, except perhaps the last. */
	uint32_t         first;   /**< number of the first line, from 1 */
	uint32_t         last;    /**< number of the last line */
	uint32_t         total;   /**< line count of the file, or 0 if
	                               unknown */
};

/** A review record. */
struct review;

/** A copy of a struct review_source in one allocation. */
struct review_read;

/** Returns the number of the @p len bytes at @p text that remain after the
 *  notices at its end are removed. askai passes notice_kept(). The notices
 *  are lines that askai appends to tool results, and a model can copy them
 *  into a claim or evidence. The record also removes the spaces and line
 *  ends before the notices.
 */
typedef size_t review_kept_fn (char const *text,
                               size_t      len);

/** Returns a new empty record with the enum review_flag bits @p flags, or
 *  nullptr with ENOMEM in @p *e. The read log of a record without
 *  REVIEW_READ_LOG stays empty, and review_call() refuses the states
 *  verified and disproved for it.
 */
extern struct review *
review_create (enum review_flag  flags,
               int              *e);

/** Frees @p *p_review and sets it to nullptr. */
extern void
review_destroy (struct review **p_review);

/**
 * Returns a copy of @p src, whose numbered lines are in the output @p out.
 *
 * @retval nullptr with EFAULT in @p *e if @p src is nullptr, or if
 *         out.data is nullptr and out.len is not 0.
 * @retval nullptr with EINVAL in @p *e if the commit is not 40 or 64
 *         hexadecimal digits, if the repository name or the path is not 1
 *         to REVIEW_NAME_MAX bytes of UTF-8 text without NUL, if the line
 *         numbers do not satisfy 1 <= first <= last <= total (or total is
 *         0), or if the numbered lines are empty, not within @p out, or not
 *         last - first + 1 lines.
 * @retval nullptr with ENOMEM in @p *e if out of memory.
 */
extern struct review_read *
review_read_create (struct review_source const *src,
                    struct cstr_view            out,
                    int                        *e);

/** Frees @p *p_read and sets it to nullptr. */
extern void
review_read_destroy (struct review_read **p_read);

/** Returns @p read as a JSON object, or nullptr with EFAULT (for a nullptr
 *  @p read) or ENOMEM in @p *e.
 */
extern struct json_object *
review_read_json (struct review_read const *read,
                  int                      *e);

/** Returns a new read from the JSON object @p obj that review_read_json()
 *  made, checked against the output @p out as review_read_create() checks
 *  it. Returns nullptr with EFAULT (for a nullptr @p obj), EINVAL or ENOMEM
 *  in @p *e on failure.
 */
extern struct review_read *
review_read_create_json (struct json_object const *obj,
                         struct cstr_view          out,
                         int                      *e);

/**
 * Adds the lines of @p read that a result in turn @p turn shows whole to
 * the pending lines of @p r. The result shows the @p window bytes of the
 * output @p out from byte @p skip on. A line that the window shows only in
 * part is not added. If the record would be charged more than REVIEW_MAX
 * bytes, the lines are not added, and the record stores the first turn
 * whose lines it did not add. @p read stores the position of the window,
 * so that the next call with a later window counts the lines from there.
 *
 * @retval 0 on success.
 * @retval EFAULT if @p r or @p read is nullptr, or if out.data is nullptr
 *         and out.len is not 0.
 * @retval EINVAL if the numbered lines of @p read are not within @p out,
 *         or if they contain more lines than the range of line numbers in
 *         @p read.
 * @retval ENOMEM if out of memory.
 */
extern int
review_pending_add (struct review      *r,
                    struct review_read *read,
                    struct cstr_view    out,
                    size_t              skip,
                    size_t              window,
                    uint32_t            turn);

/**
 * Moves the pending lines of @p r into the read log (see the file
 * comment). The lines replace the lines that the log contains for them, so
 * the commit of a location is the commit of the latest read that showed
 * its lines. Lines that would make the record exceed REVIEW_MAX are
 * dropped, as in review_pending_add(). Returns 0, EFAULT if @p r is
 * nullptr, or ENOMEM. After ENOMEM the lines that were not yet moved are
 * dropped.
 */
extern int
review_pending_log (struct review *r);

/** Drops the pending lines of @p r (see the file comment). Does nothing if
 *  @p r is nullptr.
 */
extern void
review_pending_drop (struct review *r);

/**
 * Runs the findings call with the arguments @p args in turn @p turn and
 * appends its result to @p out. add records a finding, update changes one,
 * and show writes the record or, with an id, the history of one finding.
 * If @p kept is not nullptr, it trims claims and evidence. A show writes
 * at most @p room bytes of its text from @p *cursor on and moves
 * @p *cursor, so that a later call with the same arguments writes the
 * rest. The offset argument of show is for the caller: the text starts at
 * byte 0, and the caller returns the part from the offset on.
 *
 * @retval 0 on success.
 * @retval EAGAIN if the text of a show continues after @p room bytes.
 * @retval EINVAL if the call is refused. The refusal for the model is
 *         appended to @p out.
 * @retval EFAULT if @p r, @p cursor or @p out is nullptr.
 * @retval ENOMEM if out of memory.
 */
extern int
review_call (struct review      *r,
             struct json_object *args,
             uint32_t            turn,
             review_kept_fn     *kept,
             struct tool_cursor *cursor,
             size_t              room,
             struct dstr        *out);

/**
 * Appends to @p out the text of the record @p r, or of an empty record if
 * @p r is nullptr. The text starts with a header line, then lists the
 * findings, then the read log. If the text is longer than @p most bytes,
 * it is cut after its last newline within @p most bytes, and a notice with
 * the offset of the cut for the action show is appended. Returns 0, EFAULT
 * if @p out is nullptr, or ENOMEM.
 */
extern int
review_render (struct review const *r,
               struct dstr         *out,
               size_t               most);

/** Returns @p r as a JSON object, or nullptr with EFAULT (for a nullptr
 *  @p r) or ENOMEM in @p *e.
 */
extern struct json_object *
review_json (struct review const *r,
             int                 *e);

/**
 * Replaces the contents of @p r with the record in the JSON object @p obj
 * that review_json() made, pending lines included. The flags of @p r do not
 * change. If @p r was created without REVIEW_READ_LOG, the read
 * log and the pending lines of @p obj are not restored. On error @p r is
 * unchanged.
 *
 * @retval 0 on success.
 * @retval EFAULT if @p r or @p obj is nullptr.
 * @retval EINVAL if @p obj is not such a record, or if its charge exceeds
 *         REVIEW_MAX.
 * @retval ENOMEM if out of memory.
 */
extern int
review_restore (struct review            *r,
                struct json_object const *obj);

#endif /* TWINCEPTION_SRC_REVIEW_H_ */
