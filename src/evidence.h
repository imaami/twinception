/** @file evidence.h
 *
 * The evidence of the findings (src/finding.h). A change of a finding
 * cites evidence with references (evidence_ref_parse()):
 *
 * - cN is the call N of the ask call, as the stamp [askai: call cN] of its
 *   result says: the arguments and the result, exactly as askai sent them.
 * - cN:A-B,C is lines of the file that the git read or the code describe
 *   call N showed whole.
 * - X#n/m is the evidence item m of the finding X#n, and X#n/m:A-B lines
 *   of a lines item. A finding cites such an item again with its
 *   identifiers and its archive files (evidence_recite()).
 *
 * askai makes an evidence item from a reference to a call of the ring of
 * the ask call (src/seen.h), with the identifiers that reproduce the
 * material (evidence_cite()). The material, the arguments and the window
 * of the result as the agent got them, or the lines of the result, is
 * stored in the same commit as the finding, as the file archive/XX/YYY
 * (evidence_path()), where XX are the first 2 and YYY the other 62
 * hexadecimal digits of the SHA-256 of the bytes. The action expand of
 * the findings tool prints the stored material (evidence_expand()).
 * Material outside askai is an external text with an exact hash, which
 * askai does not store (evidence_external()).
 */
#ifndef ASKAI_SRC_EVIDENCE_H_
#define ASKAI_SRC_EVIDENCE_H_

#include <stddef.h>
#include <stdint.h>

#include "cstr_view.h"
#include "dstr.h"
#include "finding_id.h"
#include "hexid.h"
#include "range.h"
#include "seen.h"
#include "util.h"

struct json_object;

enum {
	EVIDENCE_REFS     = 16,                /**< references of a change, at
	                                            most */
	EVIDENCE_EXTERNAL = 8,                 /**< external items of a change,
	                                            at most */
	EVIDENCE_RANGES   = 16,                /**< line ranges of a reference,
	                                            at most */
	EVIDENCE_TEXT     = 1024,              /**< bytes of an external item, at
	                                            most */
	EVIDENCE_PATH     = sizeof "archive/xx/" + 62,  /**< bytes of the path of
	                                                     an archive file, NUL
	                                                     included */
	EVIDENCE_SHA256   = 32,                /**< bytes of a SHA-256 */
};

/** The kinds of evidence items: X(ID, name), the constant EVIDENCE_ID of
 *  enum evidence_kind and the name in the file of a finding.
 */
#define EVIDENCE_KIND_LIST(X) \
        X(LINES, "lines")     \
        X(CALL,  "call")

/** The kinds of evidence items. lines is lines of a file that a git read
 *  or a code describe showed, and call is any other call. A call item
 *  stores the arguments of the call (request) and the result. A lines item
 *  stores only the lines (result).
 */
enum evidence_kind : uint8_t {
#define EVIDENCE_KIND_ENUM(id, ...) EVIDENCE_##id,
	EVIDENCE_KIND_LIST(EVIDENCE_KIND_ENUM)
#undef EVIDENCE_KIND_ENUM
	EVIDENCE_KINDS
};

/** The forms of a reference (struct evidence_ref). The type is 16 bits
 *  wide so that struct evidence_ref ends without padding.
 */
enum evidence_form : uint16_t {
	EVIDENCE_REF_CALL,        /**< cN */
	EVIDENCE_REF_LINES,       /**< cN:A-B,C */
	EVIDENCE_REF_ITEM,        /**< X#n/m */
	EVIDENCE_REF_ITEM_LINES,  /**< X#n/m:A-B,C */
	EVIDENCE_REF_FINDING,     /**< X#n, a finding, which is no evidence
	                               item */
};

/** A reference of the evidence argument of a findings call. */
struct evidence_ref {
	struct range       ranges[EVIDENCE_RANGES];  /**< the lines of
	                                                  EVIDENCE_REF_LINES and
	                                                  EVIDENCE_REF_ITEM_LINES,
	                                                  numbered from 1 */
	struct finding_id  id;                       /**< X#n or X#n/m, or no ID
	                                                  for a call */
	uint32_t           call;                     /**< N of cN, or 0 */
	uint16_t           n_ranges;                 /**< ranges in use */
	enum evidence_form form;
};

/** The size and the SHA-256 of stored material. */
struct evidence_blob {
	uint64_t      bytes;
	unsigned char sha256[EVIDENCE_SHA256];
};

/** The flags of an evidence item. */
enum evidence_flag : uint8_t {
	EVIDENCE_FAILED = 1U << 0,  /**< the call failed */
};

/** An evidence item: the record of stored material and the identifiers
 *  that reproduce it. Each member has a defined value from evidence()
 *  on.
 */
struct evidence {
	struct dstr           ids;      /**< the identifiers that the tool
	                                     reported, a JSON object text, or
	                                     empty */
	struct dstr           tool;     /**< the name of the tool of the call */
	struct dstr           call_id;  /**< the ID of the call from the
	                                     provider, or call_T_K */
	UTIL_ARRAY(struct range) ranges;  /**< the lines of EVIDENCE_LINES,
	                                       numbered from 1 */
	struct evidence_blob  request;  /**< the arguments of the call of an
	                                     EVIDENCE_CALL item, or zeroed */
	struct evidence_blob  result;   /**< the material that expand prints:
	                                     the result or the lines */
	uint64_t              output;   /**< bytes of the whole output of the
	                                     call */
	uint32_t              skip;     /**< offset of the window in the output,
	                                     at most TOOL_OUTPUT_MAX */
	uint32_t              item;     /**< m of AUTHOR#n/m, or 0 before the
	                                     change numbers the item */
	uint32_t              call;     /**< N of the call cN */
	uint32_t              turn;     /**< the turn whose answer made the call */
	struct finding_id     from;     /**< the ID of the item that the item
	                                     cites again, such as cuh#2/1, or no
	                                     ID */
	uint16_t              flags;    /**< enum evidence_flag. The member is
	                                     16 bits wide so that the struct
	                                     ends without padding. */
	enum evidence_kind    kind;
	struct hexid          ask;      /**< the ask call of the call, or no id
	                                     for a job resumed from a journal
	                                     state of version 2 */
};

/** Returns the name of the tool with the registry ID @p tool, or nullptr
 *  for an unknown ID. @p owner is the owner of struct evidence_cite.
 */
typedef char const *evidence_name_fn (void const *owner,
                                      uint32_t    tool);

/** The material of an evidence item that evidence_cite() made, for the
 *  archive files of the item. The views are views of the ring or of the
 *  lines that the caller passed.
 */
struct evidence_material {
	struct cstr_view request;  /**< the arguments of the call of an
	                                EVIDENCE_CALL item, else empty */
	struct cstr_view result;   /**< the result, or the lines */
};

/** What evidence_cite() needs from the findings call that cites. */
struct evidence_cite {
	struct seen const  *seen;   /**< the ring of the ask call, or nullptr */
	struct hexid const *ask;    /**< the ask call */
	evidence_name_fn   *name;   /**< the names of the tools */
	void const         *owner;  /**< passed to name */
	uint32_t            call;   /**< N of the findings call that cites */
	uint32_t            turn;   /**< the turn of that call */
};

/** Returns an evidence item without material, of the kind EVIDENCE_CALL. */
extern struct evidence
evidence (void);

/** Frees the texts and the ranges of @p e and leaves @p e as evidence()
 *  returns it.
 */
extern void
evidence_fini (struct evidence *e);

/**
 * Reads the reference @p text into @p dest: cN, cN:A-B,C, X#n, X#n/m or
 * X#n/m:A-B,C, where N is a number from 1 without a leading zero, X#n and
 * X#n/m are IDs (finding_id_parse()), and A-B,C is at most
 * EVIDENCE_RANGES ranges, each a line A or lines A to B with 1 <= A <= B.
 * The ranges of @p dest ascend, and ranges that overlap or touch become
 * one range (range_merge()). Returns 0, or EINVAL for any other text or a
 * null @p dest.
 */
extern int
evidence_ref_parse (struct evidence_ref *dest,
                    struct cstr_view     text);

/**
 * Makes the evidence item @p dest from the reference @p ref to a call of
 * the ring cite->seen, for the findings call cite->call in the turn
 * cite->turn. With @p strict, the call must be of an earlier turn, as
 * verified and disproved need. dest->item is 0. A call of a git read or a
 * code describe whose result showed lines whole makes an item of the kind
 * EVIDENCE_LINES, with all its lines for cN, and the material is the
 * lines, which the function writes to @p lines. Any other call makes an
 * item of the kind EVIDENCE_CALL, and the material is the arguments and
 * the result. The function stores the material in @p *material and its
 * sizes and SHA-256 in dest->request and dest->result. The caller stores
 * the material at evidence_path() before the ring or @p lines change.
 *
 * Returns 0. Returns EINVAL with the refusal for the turn in the @p len
 * bytes at @p why if the call is not in the ring, cannot be evidence, is
 * of the current turn with @p strict, or does not show what the form of
 * @p ref names, and for X#n, a finding. Returns ENOMEM, or EINVAL without
 * a refusal for a null argument and for X#n/m and X#n/m:A-B, which
 * evidence_recite() takes. On failure @p dest can contain a part of the
 * item.
 */
extern int
evidence_cite (struct evidence            *dest,
               struct evidence_material   *material,
               struct dstr                *lines,
               struct evidence_ref const  *ref,
               struct evidence_cite const *cite,
               bool                        strict,
               char                       *why,
               size_t                      len);

/**
 * Makes the evidence item @p dest that cites again the item that @p ref
 * names, X#n/m or X#n/m:A-B,C, from @p record, the JSON object of the item
 * in the file of its finding (evidence_json()). The kind, the identifiers
 * and the material of @p dest are those of that item, dest->item is 0,
 * and dest->from is the ID X#n/m. For X#n/m, the material of @p dest is
 * the material of the item, and the caller stores the archive files of the
 * item again. For X#n/m:A-B,C, the item must be of the kind
 * EVIDENCE_LINES, with each line of the ranges of @p ref in a range of the
 * item, and @p material must be the stored lines of the item. The function
 * writes the lines of the ranges of @p ref to @p lines, the material of
 * @p dest, and stores their size and SHA-256 in dest->result.
 *
 * Returns 0, EINVAL with the refusal for the turn in the @p len bytes at
 * @p why for a record that is not an item, an item of another kind, lines
 * that the item does not contain, or @p material that does not match the
 * recorded SHA-256, ENOMEM, or EINVAL without a refusal for a null
 * argument or another form of @p ref. On failure @p dest can contain a
 * part of the item.
 */
extern int
evidence_recite (struct evidence           *dest,
                 struct dstr               *lines,
                 struct evidence_ref const *ref,
                 struct cstr_view           record,
                 struct cstr_view           material,
                 char                      *why,
                 size_t                     len);

/**
 * Appends to @p d the evidence item @p e with the ID @p id, such as
 * cuh#2/1, as the action expand prints it. id->author is the author of
 * the finding. The first line is a notice with the ID of the item and what
 * the item is. A lines item then prints the lines, and the other kinds the
 * arguments of the call, a notice with the size of the result, and the
 * result, each with a line feed at its end. @p material is the stored
 * material, the arguments of the call and the result or the lines, with
 * a view without data for material that is missing. A notice after the
 * material says if material is missing or does not match the recorded
 * SHA-256. Returns 0, EFAULT for a null argument, or ENOMEM.
 */
extern int
evidence_expand (struct evidence const          *e,
                 struct finding_id const        *id,
                 struct evidence_material const *material,
                 struct dstr                    *d);

/** Checks the external item @p text, the item @p index of the argument
 *  external: 1 to EVIDENCE_TEXT bytes of UTF-8 text without control
 *  characters, with a run of exactly 40 or 64 hexadecimal digits. Returns
 *  0, or EINVAL with the refusal in the @p len bytes at @p why.
 */
extern int
evidence_external (struct cstr_view  text,
                   size_t            index,
                   char             *why,
                   size_t            len);

/** Writes to @p path the path of the archive file of material with the
 *  SHA-256 @p sha256: archive/XX/YYY.
 */
extern void
evidence_path (unsigned char const sha256[static EVIDENCE_SHA256],
               char                path[static EVIDENCE_PATH]);

/** Returns @p e as a JSON object for the file of a finding, or nullptr
 *  with ENOMEM in @p *err. A null @p e stores EFAULT in @p *err.
 */
extern struct json_object *
evidence_json (struct evidence const *e,
               int                   *err);

/** Replaces @p dest with the evidence item of the JSON object @p obj that
 *  evidence_json() made. Returns 0, EINVAL for an object that is not such
 *  an item, or ENOMEM. An item of an unknown kind, a call item without
 *  the member request and a lines item with the member request are not
 *  such items. On failure @p dest is as evidence() returns it.
 */
extern int
evidence_parse (struct evidence          *dest,
                struct json_object const *obj);

/** Replaces @p dest with the evidence item of @p json, the text of a JSON
 *  object that evidence_json() made, as evidence_parse() does. Returns 0,
 *  EINVAL for a text that is not such an object, or ENOMEM.
 */
extern int
evidence_parse_text (struct evidence  *dest,
                     struct cstr_view  json);

/** Appends to @p d the @p n line ranges at @p r, with @p sep between two
 *  ranges and @p to between the first and the last line of a range: such
 *  as 120-140,150 with "," and "-", or 120 to 140, 150 with ", " and
 *  " to ". Returns 0 or ENOMEM.
 */
extern int
evidence_ranges_put (struct range const *r,
                     size_t              n,
                     char const         *sep,
                     char const         *to,
                     struct dstr        *d);

/** Appends to @p d the reference @p ref as evidence_ref_parse() reads it:
 *  cN, cN:A-B,C, X#n/m or X#n/m:A-B,C. Returns 0 or ENOMEM.
 */
extern int
evidence_ref_put (struct evidence_ref const *ref,
                  struct dstr               *d);

/** Appends to @p d a summary of @p e for show, such as "call c9, git,
 *  3120 bytes" or "lines 161 to 171 of src/git.c in repository askai at
 *  commit d02cc2338c4f", without a line feed. Returns 0 or ENOMEM.
 */
extern int
evidence_summary (struct evidence const *e,
                  struct dstr           *d);

#endif /* ASKAI_SRC_EVIDENCE_H_ */
