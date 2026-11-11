/** @file finding.h
 *
 * The findings of the members of the session. A finding is a first-person
 * claim of one member, its author, about material that askai showed the
 * author. Its ID is the name of the author, '#', and a number that counts
 * the findings of the author from 1, such as bro#5. The ID stays the same
 * in every ask call. A finding consists of a claim, the conversation in
 * which the author added the finding, the ID of the finding that it checks
 * (about), and the changes. Each change sets the state of the finding, and
 * the state of the finding is the state of the last change
 * (finding_state()). A candidate or verified finding
 * is open, a disproved or retracted finding is closed. Only the author
 * changes a finding. Another member checks a finding with a finding of its
 * own whose about names that finding.
 *
 * The evidence of a change is a list of evidence items (src/evidence.h).
 * The items of a finding are numbered from 1 across its changes, so that
 * bro#5/2 names the item 2 of bro#5, and the numbers never change.
 *
 * A finding is the file findings/D/N.json (finding_path()), where N is its
 * number and D is N / 100, on the branch refs/heads/agent/AUTHOR/findings
 * of the session repository (member_ref(), src/store.h). The file is a
 * JSON object of version 1 (finding_render(), finding_parse()).
 */
#ifndef ASKAI_SRC_FINDING_H_
#define ASKAI_SRC_FINDING_H_

#include <stddef.h>
#include <stdint.h>

#include "cstr_view.h"
#include "dstr.h"
#include "evidence.h"
#include "hexid.h"
#include "member_name.h"
#include "util.h"

/** The directory of the files of the findings on a branch, and the end of
 *  the name of a file (finding_path()).
 */
#define FINDING_DIR "findings/"
#define FINDING_EXT ".json"

enum {
	FINDING_ID      = MEMBER_NAME + 17,  /**< bytes of an ID such as bro#5,
	                                          NUL included: a member name,
	                                          '#', at most 10 digits, and 6
	                                          spare bytes in place of the
	                                          padding of struct finding */
	FINDING_FILE    = sizeof FINDING_DIR "42949672/4294967295" FINDING_EXT,
	                                     /**< bytes of the path of the file
	                                          of a finding, NUL included */
	FINDING_CLAIM   = 200,               /**< bytes of a claim in a list, at
	                                          most */
};

/** The states of a finding: X(ID, name), the constant FINDING_ID of enum
 *  finding_state and the name in the findings tool and in the file.
 */
#define FINDING_STATE_LIST(X)      \
        X(CANDIDATE, "candidate") \
        X(VERIFIED,  "verified")  \
        X(DISPROVED, "disproved") \
        X(RETRACTED, "retracted")

enum finding_state : uint16_t {
#define FINDING_STATE_ENUM(id, ...) FINDING_##id,
	FINDING_STATE_LIST(FINDING_STATE_ENUM)
#undef FINDING_STATE_ENUM
	FINDING_STATES
};

/** The names of the states. Each is NUL-terminated. */
extern struct cstr_view const finding_state_names[FINDING_STATES];

enum {
	/** The open states, as a mask of the bits 1 << state. */
	FINDING_OPEN  = 1U << FINDING_CANDIDATE | 1U << FINDING_VERIFIED,

	/** The states that need evidence that askai showed before the turn,
	 *  as a mask of the bits 1 << state.
	 */
	FINDING_SHOWN = 1U << FINDING_VERIFIED | 1U << FINDING_DISPROVED,
};

/** A change of a finding. Each member has a defined value from
 *  finding_change() on.
 */
struct finding_change {
	struct dstr        reason;        /**< why the evidence shows the state,
	                                       or why the author retracts the
	                                       finding */
	struct dstr        claim_before;  /**< the claim that the change
	                                       corrected, or empty */
	struct dstr        askai;         /**< the version of askai that made
	                                       the change (ASKAI_VERSION), or
	                                       empty */
	UTIL_ARRAY(struct evidence) evidence;  /**< the evidence items */
	UTIL_ARRAY(struct dstr)     external;  /**< material outside askai */
	uint32_t           turn;          /**< the turn of the findings call */
	uint32_t           call;          /**< N of the findings call cN */
	uint32_t           items;         /**< the number of the last evidence
	                                       item of the finding after the
	                                       change */
	enum finding_state state;
	struct hexid       ask;           /**< the ask call, or no id for a job
	                                       resumed from a journal state of
	                                       version 2 */
	char               asked_by[MEMBER_NAME];  /**< the member that made the
	                                                ask call, or "" */
};

/** A finding. Each member has a defined value from finding() on. */
struct finding {
	struct dstr  claim;
	UTIL_ARRAY(struct finding_change) changes;
	uint32_t     number;              /**< N of AUTHOR#N */
	char         about[FINDING_ID];   /**< the ID of the finding that this
	                                       finding checks, or "" */
	struct hexid conversation;        /**< the conversation in which the
	                                       author added the finding, or no
	                                       id */
	char         author[MEMBER_NAME];
};

/** Returns the state of @p f: the state of the last change, or candidate
 *  for a finding without changes.
 */
static inline enum finding_state
finding_state (struct finding const *f)
{
	return f->changes.n ? f->changes.items[f->changes.n - 1].state : FINDING_CANDIDATE;
}

/** Returns a finding without a claim, a number and changes. */
extern struct finding
finding (void);

/** Frees the texts and arrays of @p f and leaves @p f as finding() returns
 *  it.
 */
extern void
finding_fini (struct finding *f);

/** Returns a change without texts and items, to the state candidate. */
extern struct finding_change
finding_change (void);

/** Frees the texts and arrays of @p c and leaves @p c as finding_change()
 *  returns it.
 */
extern void
finding_change_fini (struct finding_change *c);

/** Appends the external item @p text to the change @p c. Returns 0 or
 *  ENOMEM.
 */
extern int
finding_change_external (struct finding_change *c,
                         struct cstr_view       text);

/**
 * Reads the ID @p text, such as bro#5: a member name (member_name_valid()),
 * '#' and a number from 1 to UINT32_MAX without a leading zero. Stores the
 * name in @p author and the number in @p *number. Returns 0, or EINVAL for
 * any other text, with "" in @p author and 0 in @p *number.
 */
extern int
finding_id_parse (struct cstr_view  text,
                  char              author[static MEMBER_NAME],
                  uint32_t         *number);

/** Writes the ID of the finding @p number of @p author to @p id, such as
 *  bro#5.
 */
extern void
finding_id_format (char const *author,
                   uint32_t    number,
                   char        id[static FINDING_ID]);

/** Writes the ID of @p f to @p id, such as bro#5. */
extern void
finding_id (struct finding const *f,
            char                  id[static FINDING_ID]);

/** Writes to @p path the path of the file of the finding @p number:
 *  findings/D/N.json, where D is @p number / 100. Returns the view of the
 *  path in @p path.
 */
extern struct cstr_view
finding_path (uint32_t number,
              char     path[static FINDING_FILE]);

/** Returns the number of the finding whose file has the path @p path, as
 *  finding_path() writes it, or 0 for any other path.
 */
extern uint32_t
finding_path_number (struct cstr_view path);

/**
 * Replaces @p f with the finding in @p json, the text of a file of version
 * 1. Returns 0, EINVAL for a text that is not such a file, such as a file
 * of another version, an evidence item of an unknown kind, or item numbers
 * that do not grow, or ENOMEM. On failure @p f is as finding() returns it.
 */
extern int
finding_parse (struct finding   *f,
               struct cstr_view  json);

/**
 * Appends to @p d the text of the file of @p f, of version 1, and a line
 * feed. The change @p c, unless it is nullptr, comes after the changes of
 * @p f, with its evidence items numbered already. A @p claim that is not
 * empty is the claim of the file in place of the claim of @p f, and the
 * claim before of the change @p c is then the claim of @p f. Returns 0,
 * EFBIG if the text would be longer than TOOL_TEXT_MAX, or ENOMEM. On
 * failure @p d is as it was.
 */
extern int
finding_render (struct finding const        *f,
                struct finding_change const *c,
                struct cstr_view             claim,
                struct dstr                 *d);

/**
 * Checks the change @p c by the rules of its state: verified and disproved
 * need a reason and at least one evidence item, and retracted needs a
 * reason. Returns 0, or EINVAL with the refusal for the turn in the
 * @p len bytes at @p why.
 */
extern int
finding_check (struct finding_change const *c,
               char                        *why,
               size_t                       len);

/** Sorts the @p n findings at @p list: the open findings first, then the
 *  closed ones, each by the name of the author, and the findings of one
 *  author newest first.
 */
extern void
finding_sort (struct finding *list,
              size_t          n);

/** Returns the finding @p number of @p author among the @p n findings at
 *  @p list, which finding_sort() sorted, or nullptr for no such finding or
 *  a null @p list or @p author.
 */
extern struct finding const *
finding_find (struct finding const *list,
              size_t                n,
              char const           *author,
              uint32_t              number);

/** Returns the evidence item @p item of @p f, or nullptr for no such item
 *  or a null @p f.
 */
extern struct evidence const *
finding_item (struct finding const *f,
              uint32_t              item);

/** Returns the claim of @p f for a line: up to its first line feed, at
 *  most FINDING_CLAIM bytes.
 */
extern struct cstr_view
finding_claim_line (struct finding const *f);

/**
 * Appends to @p d the list of the @p n findings at @p list, which
 * finding_sort() sorted, as the action list writes it: a header that names
 * the session @p session, or the member @p author if it is not "", the
 * open findings, and the closed findings, one finding a line with its ID,
 * state, about and claim. If @p author is not "", @p list must be the
 * findings of @p author. A claim is cut at its first line feed and at
 * FINDING_CLAIM bytes. Returns 0 or ENOMEM.
 */
extern int
finding_list (struct finding const *list,
              size_t                n,
              struct cstr_view      session,
              char const           *author,
              struct dstr          *d);

/** Appends to @p d the finding @p f as the action show writes it: its ID
 *  @p id (finding_id()), author, state, claim and about, the findings of
 *  the @p n findings at @p list whose about names @p f, and each change
 *  with its reason and evidence. Returns 0 or ENOMEM.
 */
extern int
finding_show (struct finding const *f,
              char const           *id,
              struct finding const *list,
              size_t                n,
              struct dstr          *d);

/**
 * Appends to @p d the notices of the open findings of the @p n findings at
 * @p list, the findings of the agent that finding_sort() sorted, for the
 * prompt of a turn: the open findings of the conversation @p conversation,
 * newest first, one a line with its ID, state and claim, then the number
 * of open findings of other conversations, if any. The lines of the
 * findings are at most @p most bytes, and a notice counts the open
 * findings of the conversation that did not fit. Returns 0 or ENOMEM.
 */
extern int
finding_notice (struct finding const *list,
                size_t                n,
                struct hexid const   *conversation,
                size_t                most,
                struct dstr          *d);

#endif /* ASKAI_SRC_FINDING_H_ */
