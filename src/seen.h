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
 * read and a code describe pass a struct seen_source to mcp_slot_source()
 * for the numbered lines of a committed file in their output. The slot of
 * the call stores a copy of it, a struct seen_read (seen_read_create()).
 *
 * A findings call and a compact call show nothing that a finding can
 * cite, and the ring does not store them. A resumed ask call records the
 * calls that askai made before it restarted (seen_resume()). seen_find()
 * returns SEEN_RESTARTED for such a call and SEEN_DROPPED for a call that
 * left the ring.
 *
 * The storage of a dropped result stays for the next call in the same
 * place of the ring. The storage of dropped arguments goes back to the
 * thread's stash (src/buf.h).
 *
 * The ring also stores the evidence items of findings (src/evidence.h)
 * that askai printed to the turns: by the action expand of the findings
 * tool, or after the refusal of an item that the turns had not seen. A
 * call that prints an item names the bytes of the item in its output
 * (seen_print()). When askai writes the result of the call, the bytes of
 * the window that continue the bytes shown before count as shown
 * (seen_shown()). An item counts as seen from the turn after the one that
 * showed its last byte. The pages of an item can come from several calls,
 * in order. The ring stores the JSON of each such item and the git IDs of
 * its archive files, so that a finding can cite the item again. The journal
 * state of an ask call contains the items (seen_items_json()), and
 * seen_resume() puts them into the ring of the resumed ask call. The calls
 * are not in the journal.
 */
#ifndef ASKAI_SRC_SEEN_H_
#define ASKAI_SRC_SEEN_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "compat.h"
#include "cstr_view.h"
#include "dstr.h"
#include "finding_id.h"
#include "flag.h"
#include "oid.h"
#include "range.h"
#include "tool.h"
#include "util.h"

struct json_object;

enum {
	SEEN_CALLS    = 64,                 /**< calls in the ring of an ask
	                                         call, at most */
	SEEN_NAME_MAX = 1024,               /**< bytes of a repository name or
	                                         a path in a struct
	                                         seen_source, at most */
	SEEN_ITEMS    = 1024,               /**< evidence items in the ring of
	                                         an ask call, at most */
	SEEN_LISTED   = 64,                 /**< items that the notice of
	                                         seen_notice() names, at most */
	SEEN_BYTES    = 16U << 20,          /**< bytes of the arguments and
	                                         results of the calls in the
	                                         ring, at most */
};

/** The flags of a call in the ring. The flags of a failed call contain
 *  FLAG_FAILED, the shared bit of src/flag.h. SEEN_TOO_LARGE is a bit of
 *  this word only.
 */
enum seen_flag : uint16_t {
	SEEN_TOO_LARGE = FLAG_OWN(0),  /**< the arguments and the result were
	                                    larger than TOOL_TEXT_MAX, so the
	                                    ring stores neither */
};

/** Where seen_find() found a call. */
enum seen_where : uint8_t {
	SEEN_KEPT,       /**< the ring contains the call */
	SEEN_DROPPED,    /**< the call left the ring for newer calls */
	SEEN_RESTARTED,  /**< askai made the call before it restarted */
	SEEN_MISSING,    /**< the ring contains no call of the number: a
	                      findings or compact call, a call that did not
	                      run, or no call has the number yet */
};

/** A position in the numbered lines of an output. seen_lines() counts the
 *  lines of a window from the position of the window before.
 */
struct seen_mark {
	uint64_t line;  /**< number of the line of the byte at `at` */
	size_t   at;    /**< offset in the output, at or after
	                     src->output.at */
};

/** The numbered lines of a committed file in the output of a git read or a
 *  code describe (mcp_slot_source()). The texts are views of the caller's
 *  storage.
 */
struct seen_source {
	struct cstr_view repo;    /**< repository name */
	struct cstr_view path;    /**< path of the file in the commit */
	struct extent    output;  /**< the numbered lines in the output. There
	                               is one output line for each number in
	                               the range lines, and each output line
	                               ends with a newline, except perhaps the
	                               last. */
	uint64_t         total;   /**< line count of the file, or 0 if
	                               unknown */
	struct range     lines;   /**< numbers of the first and the last line,
	                               from 1 */
	struct oid       commit;  /**< name of the commit */
};

/** A copy of a struct seen_source in one allocation, and the position of
 *  the last window in the numbered lines (seen_lines()).
 */
struct seen_read {
	struct seen_source src;     /**< repo and path are views of text */
	struct seen_mark   mark;    /**< the position of the last window */
	char               text[];  /**< the repository name and the path */
};

/** The lines of a file that a window showed whole, and the offset of the
 *  first of them in the window.
 */
struct seen_lines {
	struct range range;  /**< the numbers of the first and the last line,
	                          or 0 and 0 for no line */
	uint32_t     at;     /**< offset of the start of the first line from
	                          the start of the window */
};

/** The stamp of a call contains the numbers of the call, of the turn and
 *  of the tool, the size of the output, and the flags of the call.
 *  seen_keep() copies the stamp of a result into the ring.
 */
struct seen_stamp {
	uint64_t output;  /**< bytes of the whole output */
	uint32_t number;  /**< N of the call cN */
	uint32_t turn;    /**< the turn whose answer made the call */
	uint32_t tool;    /**< the registry ID of the tool, or TOOL_MAX for an
	                       unknown tool */
	uint16_t index;   /**< K of the call in its turn, from 0, as in the ID
	                       call_T_K of a call without an ID from the
	                       provider */
	uint16_t flags;   /**< FLAG_FAILED and enum seen_flag */
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
	struct seen_stamp stamp;    /**< the stamp of the result. stamp.flags
	                                 contains SEEN_TOO_LARGE if the ring
	                                 stores neither request nor result. */
	uint32_t          skip;     /**< offset of the window in the output,
	                                 at most TOOL_OUTPUT_MAX */
	struct seen_lines lines;    /**< the lines that the window showed
	                                 whole. The window is at the start of
	                                 result. */
};

/** Returns a call of the ring that is not in use. The result and the
 *  identifiers are empty, the tool is unknown, and every other member is
 *  zero.
 */
static force_inline struct seen_call
seen_call (void)
{
	return (struct seen_call){ .result = DSTR_INIT, .ids = DSTR_INIT,
	                           .stamp = { .tool = TOOL_MAX } };
}

/** An evidence item that askai printed to the turns. */
struct seen_item {
	struct dstr       record;    /**< the JSON object of the item, as in
	                                  the file of its finding */
	struct extent     output;    /**< the bytes of the item in an output,
	                                  at the offset in the output of the
	                                  call `call` */
	uint32_t          shown;     /**< the bytes of the item from its start
	                                  that results showed, in order */
	uint32_t          call;      /**< N of the call cN whose result is not
	                                  written yet and prints the item, or
	                                  0 */
	uint32_t          turn;      /**< the turn whose result showed the last
	                                  byte of the item, or 0 */
	struct finding_id key;       /**< the ID of the item, such as cuh#2/1 */
	struct oid        blobs[2];  /**< git IDs of the archive files: the
	                                  arguments of the call, or no name,
	                                  and the material */
};

/** The ring of an ask call. calls[(head + i) % SEEN_CALLS] is the call i of
 *  the n calls in the ring, from the oldest. items.items[(items_head + i) %
 *  SEEN_ITEMS] is the evidence item i of the items.n items that askai
 *  printed, from the oldest. printed contains the indexes in items.items
 *  of the items that calls printed (seen_print()) whose results askai has
 *  not written yet (seen_shown()), and can contain an index more than
 *  once.
 */
struct seen {
	struct seen_call calls[SEEN_CALLS];
	UTIL_ARRAY(struct seen_item) items;
	UTIL_ARRAY(size_t) printed;
	size_t           items_head;  /**< index of the oldest item */
	size_t           bytes;       /**< bytes of the arguments and results
	                                   of the calls in the ring */
	uint32_t         resumed;     /**< the calls that askai made before it
	                                   restarted, or 0 (seen_resume()) */
	uint16_t         head;        /**< index of the oldest call */
	uint16_t         n;           /**< calls in the ring */
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
	struct seen_stamp       stamp;   /**< the stamp of the call. seen_keep()
	                                      copies only FLAG_FAILED of
	                                      stamp.flags. */
	uint64_t                at;      /**< offset of @p out in the whole
	                                      output */
	size_t                  skip;    /**< offset of the window in @p out */
};

static force_inline struct seen_result
seen_result (struct seen_result const r)
{
	return r;
}

#define seen_result(...) (seen_result)((struct seen_result){__VA_ARGS__})

/**
 * Returns a copy of @p src, whose numbered lines are in the output @p out.
 *
 * @retval nullptr with EFAULT in @p *e if @p src is nullptr, or if
 *         out.data is nullptr and out.len is not 0.
 * @retval nullptr with EINVAL in @p *e if the commit is no name
 *         (src/oid.h), if the repository name or the path is not 1 to
 *         SEEN_NAME_MAX bytes of UTF-8 text without NUL, if the line
 *         numbers do not satisfy 1 <= first <= last <= total (or total is
 *         0), if total is greater than UINT32_MAX, or if the numbered lines
 *         are empty, not within @p out, or not last - first + 1 lines.
 * @retval nullptr with ENOMEM in @p *e if out of memory.
 * @retval nullptr for a null @p e.
 */
extern struct seen_read *
seen_read_create (struct seen_source const *src,
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
 * Returns the first and the last of the numbered lines of @p read that the
 * window @p window of the output @p out shows whole, and the offset of the
 * first of those lines from window.at. A line that the window shows only
 * in part does not count. The last line may end without a newline, and it
 * counts if the window ends at the end of the numbered lines. read->mark
 * is the position of the last window. The function counts the lines from
 * that position, or from the start of the numbered lines if the position
 * is after the window, and then sets read->mark to the start of the
 * window. Returns no line if the window shows none.
 *
 * Stores 0 in @p *e, EFAULT for a null @p read or for an @p out with a
 * length and no bytes, or EINVAL if the numbered lines are empty, not
 * within @p out, or more lines than read->src.lines, or if the first line
 * that the window shows whole starts more than UINT32_MAX bytes after
 * window.at. Returns no line for a null @p e.
 */
extern struct seen_lines
seen_lines (struct seen_read *read,
            struct cstr_view  out,
            struct extent     window,
            int              *e);

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
 * lines that the window shows whole (seen_lines()).
 *
 * Returns 0, EFAULT for a null argument, a null r->parts or an r->out with
 * a length and no bytes, EINVAL if the window parts[0] is not the bytes of
 * r->out from r->skip on, if r->at + r->skip is greater than
 * TOOL_OUTPUT_MAX, or for the errors of seen_lines(), or ENOMEM. On failure
 * the ring does not contain the call, but calls may have left the ring.
 */
extern int
seen_keep (struct seen              **p_seen,
           struct buf                *request,
           struct seen_result const  *r);

/**
 * Records in the ring @p *p_seen the calls that askai made of the ask call
 * before it restarted, as a resumed ask call starts. @p calls is their
 * number in the journal state of the ask call, a JSON number from 0 to
 * UINT32_MAX, or nullptr for 0, as in a state of version 2. The items of
 * @p items, the array of seen_items_json() in the journal state, replace
 * the items of the ring. @p items is nullptr for no items. The ring is
 * allocated if @p *p_seen is nullptr. Returns 0, EFAULT for a null
 * @p p_seen, EINVAL with the ring unchanged if @p calls is not such a
 * number or @p items is not such an array, or ENOMEM with no items in the
 * ring.
 */
extern int
seen_resume (struct seen              **p_seen,
             struct json_object const  *calls,
             struct json_object const  *items);

/**
 * Finds the call @p number in @p seen. Stores the call in @p *call and
 * returns SEEN_KEPT if the ring contains it. Otherwise stores nullptr and
 * returns SEEN_RESTARTED for a number from 1 to the calls before a restart
 * (seen_resume()), SEEN_DROPPED for a later number before the oldest call
 * of the ring, and SEEN_MISSING for any other number, also for a null
 * @p seen. A findings or compact call is missing, and so is a call that
 * did not run, such as a call after compact in its turn, and a number
 * after the newest call.
 */
extern enum seen_where
seen_find (struct seen const       *seen,
           uint32_t                 number,
           struct seen_call const **call);

/** Returns the number of the oldest call among the calls of @p seen, or 0
 *  for a null @p seen or a ring without calls.
 */
extern uint32_t
seen_oldest (struct seen const *seen);

/**
 * Records in the ring @p *p_seen that the output of the call @p call prints
 * the item @p item in the bytes @p output. The ring is allocated on the
 * first call. The ring copies item->key, item->record and item->blobs for
 * an item that it does not store, as its newest item, and drops its oldest
 * item if it stores SEEN_ITEMS items. For an item that the ring stores,
 * the bytes that results showed stay shown, unless output.len is not the
 * size that the ring stores, which starts the item anew. seen_shown()
 * counts the window of the result of the call. Returns 0, EFAULT for a
 * null @p p_seen or @p item, EINVAL for a @p call of 0, an item->key that
 * is not the ID of an item, an output.len of 0, or bytes past
 * TOOL_OUTPUT_MAX, or ENOMEM.
 */
extern int
seen_print (struct seen            **p_seen,
            struct seen_item const  *item,
            uint32_t                 call,
            struct extent            output);

/**
 * Counts the window @p window of the output in the result of the call
 * @p call in the turn @p turn, for each item that the output of the call
 * prints (seen_print()). The bytes of the window that continue the bytes
 * of the item shown before count as shown. If the item is then shown whole
 * and was not before, the turn of the item becomes @p turn. Does nothing
 * for a null @p seen.
 */
extern void
seen_shown (struct seen   *seen,
            uint32_t       call,
            struct extent  window,
            uint32_t       turn);

/** Returns the items of @p seen that results showed, whole or in part, for
 *  the journal state of the ask call: a JSON array with an object for each
 *  item, with the members id, record, blobs (two git IDs or ""), bytes,
 *  shown and turn. The array is empty for a null @p seen. Returns nullptr
 *  with ENOMEM in @p *e if out of memory, and nullptr for a null @p e.
 */
extern struct json_object *
seen_items_json (struct seen const *seen,
                 int               *e);

/** Returns the item of @p seen with the ID @p key, such as cuh#2/1, or
 *  nullptr for no such item or a null @p seen or @p key.
 */
extern struct seen_item const *
seen_find_item (struct seen const       *seen,
                struct finding_id const *key);

/**
 * Appends to @p d the notices of the calls and the items that @p seen
 * stores, for the prompt after a compaction. The first is "[askai: askai
 * keeps your calls cA to cB of this ask call. Cite the calls as evidence
 * by number.]" with the oldest and the newest call of the ring that a
 * finding can cite, or "your call cA" and "the call" for one call. The
 * second is "[askai: Evidence items that you expanded in this ask call:
 * cuh#2/1 and cuh#2/2. These items count as seen.]" with the items that
 * results showed whole, at most SEEN_LISTED of them by ID and " and N
 * more" for the others. Each notice ends with a line feed. Appends nothing
 * for a null @p seen, and no notice without such calls or items. Returns
 * 0, or ENOMEM.
 */
extern int
seen_notice (struct seen const *seen,
             struct dstr       *d);

/** Drops the calls and the items of @p seen at the end of an ask call, and
 *  the calls before a restart. The storage of the results stays for the
 *  next ask call that uses @p seen. Does nothing for a null @p seen.
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
