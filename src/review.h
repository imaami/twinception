/** @file review.h
 *
 * The review record of an ask call. It consists of two parts:
 *
 * - The read log. It records which lines of committed files the results of
 *   git read calls and code describe calls returned to the turns of the
 *   call.
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
 * A record is charged at most TOOL_TEXT_MAX bytes, pending lines included:
 * REVIEW_ITEM for each file, line range, finding and change, and the bytes
 * of their texts. Nothing depends on the wall clock, so a findings call
 * repeated with the same record, arguments and turn returns the same
 * result. A function with an int *e parameter returns nullptr if e is
 * nullptr.
 *
 * With --notes, an ask call of a thread saves the findings of its record
 * if an add or update changed them, and the next ask call of the thread
 * starts with those findings (review_carry_json(), review_carry()). The
 * read log is not saved.
 */
#ifndef ASKAI_SRC_REVIEW_H_
#define ASKAI_SRC_REVIEW_H_

#include <stddef.h>
#include <stdint.h>

#include "cstr_view.h"
#include "seen.h"
#include "tool.h"

struct dstr;
struct json_object;
struct oid;

enum {
	REVIEW_ITEM     = 64,       /**< the bytes charged for each file, line
	                                 range, finding and change */
};

/** The flags of review_create(). */
enum review_flag : uint16_t {
	REVIEW_READ_LOG = 1U << 0U,  /**< the turns of the ask call can call the
	                                  git tool or the code tool, so the
	                                  record contains a read log */
	REVIEW_GIT      = 1U << 1U,  /**< they can call the git tool, whose
	                                  read a refusal suggests */
};

/** The states of a finding: X(ID, name), the constant REVIEW_ID of enum
 *  review_state and the name in the findings tool. Candidate and verified
 *  findings are open. Disproved and retracted findings are closed.
 */
#define REVIEW_STATE_LIST(X)      \
        X(CANDIDATE, "candidate") \
        X(VERIFIED,  "verified")  \
        X(DISPROVED, "disproved") \
        X(RETRACTED, "retracted")

/** The actions of the findings tool: X(ID, name), the constant REVIEW_ID of
 *  enum review_action and the name.
 */
#define REVIEW_ACTION_LIST(X) \
        X(ADD,    "add")      \
        X(UPDATE, "update")   \
        X(SHOW,   "show")

#define REVIEW_ENUM(id, ...) REVIEW_##id,
enum review_state : uint8_t {
	REVIEW_STATE_LIST(REVIEW_ENUM)
	REVIEW_STATES
};

enum review_action : uint8_t {
	REVIEW_ACTION_LIST(REVIEW_ENUM)
	REVIEW_ACTIONS
};
#undef REVIEW_ENUM

/** The names of the states in the findings tool. Each is NUL-terminated. */
extern struct cstr_view const review_state_names[REVIEW_STATES];

/** The names of the actions of the findings tool. Each is NUL-terminated. */
extern struct cstr_view const review_action_names[REVIEW_ACTIONS];


/** A review record. */
struct review;


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
 * Adds the lines @p lines of the file of @p read, which a result in turn
 * @p turn shows whole (seen_lines()), to the pending lines of @p r. If the
 * record would be charged more than TOOL_TEXT_MAX bytes, the lines are
 * not added, and the record stores the first turn whose lines it did not
 * add.
 *
 * @retval 0 on success, also for no line (lines.first 0).
 * @retval EFAULT if @p r or @p read is nullptr.
 * @retval EINVAL if @p lines are not within the numbered lines of @p read.
 * @retval ENOMEM if out of memory.
 */
extern int
review_pending_add (struct review          *r,
                    struct seen_read const *read,
                    struct seen_lines       lines,
                    uint32_t                turn);

/**
 * Moves the pending lines of @p r into the read log (see the file
 * comment). The lines replace the lines that the log contains for them, so
 * the commit of a location is the commit of the latest read that showed
 * its lines. Lines that would make the record exceed TOOL_TEXT_MAX are
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
 * @p r is nullptr. The text starts with two header lines, then lists the
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
 * unchanged. The function also restores the commit that review_carry()
 * stored and the value that review_changed() returns.
 *
 * @retval 0 on success.
 * @retval EFAULT if @p r or @p obj is nullptr.
 * @retval EINVAL if @p obj is not such a record, or if its charge exceeds
 *         TOOL_TEXT_MAX.
 * @retval ENOMEM if out of memory.
 */
extern int
review_restore (struct review            *r,
                struct json_object const *obj);

/** Returns the findings of @p r as the JSON object {"findings": [...]},
 *  and writes the commit that review_carry() stored to @p base, or no name.
 *  Returns nullptr with EFAULT (for a nullptr @p r or @p base) or ENOMEM in
 *  @p *e.
 */
extern struct json_object *
review_carry_json (struct review const *r,
                   struct oid          *base,
                   int                 *e);

/** Replaces the contents of @p r with the findings of @p obj, a JSON object
 *  from review_carry_json() or review_json(), and an empty read log, and
 *  stores the commit @p base. Other members of @p obj are ignored. The flags
 *  of @p r do not change, and review_changed() returns false. Returns 0,
 *  EFAULT for a nullptr argument, EINVAL as review_restore(), or ENOMEM. On
 *  error @p r is unchanged.
 */
extern int
review_carry (struct review            *r,
              struct json_object const *obj,
              struct oid const         *base);

/** Returns true if an add or update changed the findings of @p r after
 *  review_create() or review_carry(), and false for a nullptr @p r.
 */
extern bool
review_changed (struct review const *r);

#endif /* ASKAI_SRC_REVIEW_H_ */
