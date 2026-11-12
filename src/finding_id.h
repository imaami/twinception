/** @file finding_id.h
 *
 * The IDs of the findings (src/finding.h) and of their evidence items
 * (src/evidence.h). The ID of a finding is the name of its author, '#' and
 * the number of the finding, such as bro#5. The ID of an evidence item is
 * the ID of its finding, '/' and the number of the item, such as bro#5/2.
 * Both numbers count from 1. An ID is text only where askai reads or
 * writes it: in the arguments and the results of the findings tool, in the
 * files of the findings and in the journal (finding_id_parse(),
 * finding_id_put()).
 */
#ifndef ASKAI_SRC_FINDING_ID_H_
#define ASKAI_SRC_FINDING_ID_H_

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "compat.h"
#include "cstr_view.h"
#include "member_name.h"
#include "util.h"

enum {
	FINDING_ID_TEXT = MEMBER_NAME + sizeof "#4294967295/4294967295" - 1,
	                  /**< bytes of the text of an ID, NUL included */
};

/** The ID of a finding, or of an evidence item of a finding. */
struct finding_id {
	uint32_t number;               /**< n of X#n, from 1 */
	uint32_t item;                 /**< m of X#n/m, from 1, or 0 in the ID
	                                    of a finding */
	char     author[MEMBER_NAME];  /**< X of X#n, or "" for no ID */
};

/**
 * Reads the ID @p text into @p id: X#n, the ID of a finding, or X#n/m, the
 * ID of an evidence item. X is a member name (member_name_valid()), and n
 * and m are numbers from 1 to UINT32_MAX without a leading zero. id->item
 * is 0 for X#n. Returns 0, or EINVAL for any other text, with no ID in
 * @p id.
 */
static inline int
finding_id_parse (struct finding_id *id,
                  struct cstr_view   text)
{
	*id = (struct finding_id){};
	struct cstr_view name;
	uint32_t number;
	uint32_t item = 0;
	if (!cstr_view_cut(&text, '#', &name) || !member_name_valid(name) ||
	    util_ordinal(&text, &number))
		return EINVAL;
	/* After the number, the text ends, or '/' and the number of an item
	 * follow. */
	if (text.len) {
		bool const slash = *text.data == '/';
		text = cstr_view_slice(text, 1, SIZE_MAX);
		if (!slash || util_ordinal(&text, &item) || text.len)
			return EINVAL;
	}
	id->number = number;
	id->item = item;
	member_name_set(id->author, name);
	return 0;
}

/** Writes the text of @p id to @p text: X#n, or X#n/m if id->item is not
 *  0. Returns the view of the text in @p text.
 */
static inline struct cstr_view
finding_id_put (struct finding_id const *id,
                char                     text[static FINDING_ID_TEXT])
{
	if (id->item)
		return util_format_view(text, FINDING_ID_TEXT, "%s#%" PRIu32 "/%" PRIu32, id->author,
		                        id->number, id->item);
	return util_format_view(text, FINDING_ID_TEXT, "%s#%" PRIu32, id->author, id->number);
}

/** Returns the length of the text of @p id, the ID of a finding, as
 *  finding_id_put() writes it, such as 5 for bro#5, or 0 for no ID.
 */
static inline int
finding_id_width (struct finding_id const *id)
{
	char digits[UTIL_DECIMAL];
	size_t const name = strlen(id->author);
	return name ? (int)(name + 1 + util_decimal(digits + sizeof digits, id->number, 0).len) : 0;
}

/** Returns true if @p a and @p b are the same ID. */
static inline bool
finding_id_equal (struct finding_id const *a,
                  struct finding_id const *b)
{
	return a->number == b->number && a->item == b->item && !strcmp(a->author, b->author);
}

/** Returns the ID of the finding of @p id: @p id with 0 as the number of
 *  the item.
 */
static inline struct finding_id
finding_id_without_item (struct finding_id const *id)
{
	struct finding_id finding = *id;
	finding.item = 0;
	return finding;
}

#endif /* ASKAI_SRC_FINDING_ID_H_ */
