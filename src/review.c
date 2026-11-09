/** @file review.c
 *
 * The review record of an ask call (src/review.h).
 */
#define _GNU_SOURCE /* memrchr(), mempcpy() */

#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "dstr.h"
#include "jb.h"
#include "oid.h"
#include "review.h"
#include "util.h"
#include "xlist.h"

struct cstr_view const review_state_names[REVIEW_STATES] = { REVIEW_STATE_LIST(XL_VIEW) };

struct cstr_view const review_action_names[REVIEW_ACTIONS] = { REVIEW_ACTION_LIST(XL_VIEW) };

enum {
	REVIEW_OID_MIN       = 7,    /* the fewest digits of a commit prefix in a
	                                location */
	REVIEW_SHORT_OID     = 12,   /* digits of a commit ID in results and
	                                refusals */
	REVIEW_CLOSED_CLAIM  = 200,  /* the most bytes of the claim of a closed
	                                finding in the record text */
	REVIEW_RANGE_NUMBERS = 3,    /* JSON numbers of a line range: first,
	                                last and turn */
};

/* The states that close a finding, and the states that require the lines
 * of the location in the read log, as bit masks of enum review_state.
 */
enum : uint8_t {
	REVIEW_CLOSED_STATES = 1U << REVIEW_DISPROVED | 1U << REVIEW_RETRACTED,
	REVIEW_LOGGED_STATES = 1U << REVIEW_VERIFIED | 1U << REVIEW_DISPROVED,
};

/** Returns true if @p state requires the lines of the location in the read
 *  log.
 */
static bool
review_state_logged (enum review_state state)
{
	return REVIEW_LOGGED_STATES >> state & 1U;
}

/** Lines first to last of a file, numbered from 1. */
struct review_range {
	uint32_t first;
	uint32_t last;
	uint32_t turn;   /**< the turn of the latest read that showed the lines */
};

/**
 * The lines read from one file at one commit of one repository. The
 * ranges are sorted and do not overlap, and the turns of two adjoining
 * ranges differ.
 */
struct review_file {
	struct dstr          repo;
	struct dstr          path;
	UTIL_ARRAY(struct review_range) ranges;
	uint32_t             total;   /**< line count of the file, or 0 if
	                                   unknown */
	struct oid           commit;
};

/** Returns an empty file. */
static struct review_file
review_file (void)
{
	return (struct review_file){ .repo = DSTR_INIT, .path = DSTR_INIT };
}

/** A list of files. The files of one repository and commit are next to
 *  each other, in the order in which they were added.
 */
struct review_files {
	UTIL_ARRAY(struct review_file);
};

/** Lines first to last of the file at path. The path is empty if there is
 *  no location.
 */
struct review_location {
	struct dstr path;
	uint32_t    first;
	uint32_t    last;
};

/** Returns a location without a path, with the lines @p first to
 *  @p last.
 */
static struct review_location
review_location (uint32_t first,
                 uint32_t last)
{
	return (struct review_location){ .path = DSTR_INIT, .first = first, .last = last };
}

/** A struct review_location whose path is a view, and the commit prefix
 *  of a location argument, or empty.
 */
struct review_location_view {
	struct cstr_view path;
	struct cstr_view prefix;
	uint32_t         first;
	uint32_t         last;
};

/**
 * One change of a finding. old_claim is the claim that the change replaced,
 * or empty. If moved is not 0, old_location is the location that the change
 * replaced, and its path is empty if the finding had no location. commit is
 * the commit at which the lines of the new location were read, or empty.
 */
struct review_change {
	struct dstr            evidence;
	struct dstr            old_claim;
	struct review_location old_location;
	uint32_t               turn;
	uint16_t               moved;
	enum review_state      state;
	struct oid             commit;
};

/** Returns a change in the turn @p turn to the state @p state, read at the
 *  commit @p commit, without texts and without an old location.
 */
static struct review_change
review_change (uint32_t          turn,
               enum review_state state,
               struct oid        commit)
{
	return (struct review_change){
		.evidence     = DSTR_INIT,
		.old_claim    = DSTR_INIT,
		.old_location = review_location(0, 0),
		.turn         = turn,
		.state        = state,
		.commit       = commit,
	};
}

/** A finding. Its state is the state of its last change. */
struct review_finding {
	struct dstr            claim;
	struct review_location location;
	UTIL_ARRAY(struct review_change) changes;
};

/** Returns a finding without a claim and without changes, at the lines
 *  @p first to @p last of no path.
 */
static struct review_finding
review_finding (uint32_t first,
                uint32_t last)
{
	return (struct review_finding){ .claim = DSTR_INIT, .location = review_location(first, last) };
}

struct review {
	struct review_files    log;           /**< the read log */
	struct review_files    pending;       /**< the pending lines (review.h) */
	UTIL_ARRAY(struct review_finding) findings;  /**< items[i] is the finding
	                                                  with the id i + 1 */
	size_t                 size;          /**< charged bytes, pending lines
	                                           included */
	uint32_t               dropped_turn;  /**< the first turn whose lines did
	                                           not fit into the record, or 0 */
	enum review_flag       flags;
	bool                   changed;       /**< an add or update changed the
	                                           findings (review_changed()) */
	struct oid             base;          /**< the commit that review_carry()
	                                           stored, or no name */
};

/** Returns the bytes charged for @p items items with @p text bytes of
 *  text.
 */
static size_t
review_charge (size_t items,
               size_t text)
{
	return items * REVIEW_ITEM + text;
}

/** Returns the bytes charged for the file @p f with @p n ranges. */
static size_t
review_file_charge (struct review_file const *f,
                    size_t                    n)
{
	return review_charge(1 + n, dstr_len(&f->repo) + dstr_len(&f->path));
}

/** Returns the bytes charged for the change @p c. */
static size_t
review_change_charge (struct review_change const *c)
{
	return review_charge(1, dstr_len(&c->evidence) + dstr_len(&c->old_claim) +
	                        dstr_len(&c->old_location.path));
}

/** Returns the bytes charged for the finding @p f and its changes. */
static size_t
review_finding_charge (struct review_finding const *f)
{
	size_t n = review_charge(1, dstr_len(&f->claim) + dstr_len(&f->location.path));
	for (size_t i = 0; i < f->changes.n; ++i)
		n += review_change_charge(&f->changes.items[i]);
	return n;
}

static void
review_file_fini (struct review_file *f)
{
	dstr_cleanup(&f->repo);
	dstr_cleanup(&f->path);
	free(f->ranges.items);
	*f = review_file();
}

/** Finalizes the files of @p list and sets its count to 0. The array of
 *  files is not freed, so that the list can be filled again.
 */
static void
review_files_clear (struct review_files *list)
{
	for (size_t i = 0; i < list->n; ++i)
		review_file_fini(&list->items[i]);
	list->n = 0;
}

static void
review_files_fini (struct review_files *list)
{
	review_files_clear(list);
	free(list->items);
	*list = (struct review_files){};
}

static void
review_change_fini (struct review_change *c)
{
	dstr_cleanup(&c->evidence);
	dstr_cleanup(&c->old_claim);
	dstr_cleanup(&c->old_location.path);
}

static void
review_finding_fini (struct review_finding *f)
{
	dstr_cleanup(&f->claim);
	dstr_cleanup(&f->location.path);
	for (size_t i = 0; i < f->changes.n; ++i)
		review_change_fini(&f->changes.items[i]);
	free(f->changes.items);
	*f = review_finding(0, 0);
}

/** Frees the members of @p r and leaves it empty. */
static void
review_fini (struct review *r)
{
	review_files_fini(&r->log);
	review_files_fini(&r->pending);
	for (size_t i = 0; i < r->findings.n; ++i)
		review_finding_fini(&r->findings.items[i]);
	free(r->findings.items);
	*r = (struct review){};
}

struct review *
review_create (enum review_flag  flags,
               int              *e)
{
	if (!e)
		return nullptr;
	struct review *r = calloc(1, sizeof *r);
	if (!r) {
		*e = ENOMEM;
		return nullptr;
	}
	r->flags = flags;
	*e = 0;
	return r;
}

void
review_destroy (struct review **p_review)
{
	if (!p_review || !*p_review)
		return;
	review_fini(*p_review);
	free(*p_review);
	*p_review = nullptr;
}

/** Returns true if @p t is 1 to SEEN_NAME_MAX bytes of UTF-8 text without
 *  NUL, as a repository name or a path must be.
 */
static bool
review_name_valid (struct cstr_view t)
{
	return t.len && t.len <= SEEN_NAME_MAX && jb_is_text(t);
}

/** Returns the repository, commit, path and total of @p f as a struct
 * call_source with views of the texts of @p f and no lines.
 */
static struct call_source
review_file_source (struct review_file const *f)
{
	return (struct call_source){
		.repo   = cstr_view_dstr(&f->repo),
		.commit = oid_view(&f->commit),
		.path   = cstr_view_dstr(&f->path),
		.total  = f->total,
	};
}

/** Returns true if the file @p f was read at the commit @p commit of the
 *  repository @p repo.
 */
static bool
review_file_commit_equal (struct review_file const *f,
                          struct cstr_view          repo,
                          struct cstr_view          commit)
{
	return cstr_view_equal(oid_view(&f->commit), commit) &&
	       cstr_view_equal(cstr_view_dstr(&f->repo), repo);
}

/**
 * Returns the index of the file of @p list with the repository, commit and
 * path of @p key, or list->n if there is no such file. Stores in @p *at the
 * index at which such a file is inserted: after the last file of the same
 * repository and commit, or list->n if there is none.
 */
static size_t
review_files_find (struct review_files const *list,
                   struct call_source const  *key,
                   size_t                    *at)
{
	*at = list->n;
	for (size_t i = 0; i < list->n; ++i) {
		struct review_file const *f = &list->items[i];
		if (!review_file_commit_equal(f, key->repo, key->commit))
			continue;
		if (cstr_view_equal(cstr_view_dstr(&f->path), key->path))
			return i;
		*at = i + 1;
	}
	return list->n;
}

/** Inserts @p f into @p list at the index @p at. On success the list owns
 *  the texts and ranges of @p f. Returns 0 or ENOMEM.
 */
static int
review_files_insert (struct review_files      *list,
                     struct review_file const *f,
                     size_t                    at)
{
	if (util_splice(list->items, list->n, list->cap, at, 0, 1))
		return ENOMEM;
	list->items[at] = *f;
	return 0;
}

/** Copies the repository name, commit ID, path and total of @p key into the
 *  empty file @p f. Returns 0, EINVAL if the commit ID is not valid, or
 *  ENOMEM.
 */
static int
review_file_set (struct review_file       *f,
                 struct call_source const *key)
{
	f->total = (uint32_t)key->total;
	int e = oid_init(&f->commit, key->commit);
	e = e ? e : cstr_view_store(key->repo, &f->repo);
	return e ? e : cstr_view_store(key->path, &f->path);
}

/** Returns true if a record of @p size bytes is within TOOL_TEXT_MAX.
 *  Otherwise sets r->dropped_turn to @p turn if it is 0, and returns false.
 */
static bool
review_within_max (struct review *r,
                   size_t         size,
                   uint32_t       turn)
{
	if (size <= TOOL_TEXT_MAX)
		return true;
	if (!r->dropped_turn)
		r->dropped_turn = turn;
	return false;
}

/**
 * Writes to @p put the ranges that replace the @p n ranges at @p p, which
 * overlap or adjoin @p lines, and returns their number, 1 to 3. The part
 * of p[0] before @p lines and the part of p[n - 1] after @p lines remain
 * ranges of their own if their turn differs from lines.turn. Otherwise
 * they become part of @p lines.
 */
static size_t
review_range_merge (struct review_range const *p,
                    size_t                     n,
                    struct review_range        lines,
                    struct review_range        put[3])
{
	struct review_range lo = n ? p[0] : lines;
	struct review_range hi = n ? p[n - 1] : lines;
	if (lo.turn == lines.turn && lo.first < lines.first)
		lines.first = lo.first;
	if (hi.turn == lines.turn && hi.last > lines.last)
		lines.last = hi.last;
	size_t m = 0;
	if (lo.first < lines.first) {
		lo.last = lines.first - 1;
		put[m++] = lo;
	}
	put[m++] = lines;
	if (hi.last > lines.last) {
		hi.first = lines.last + 1;
		put[m++] = hi;
	}
	return m;
}

/**
 * Adds the range @p lines to the file @p f of @p r. A nonzero @p total
 * replaces the total of @p f. If the charge of @p r would exceed
 * TOOL_TEXT_MAX, nothing changes (review_within_max()). Returns 0 or ENOMEM.
 */
static int
review_file_add_range (struct review       *r,
                       struct review_file  *f,
                       uint32_t             total,
                       struct review_range  lines)
{
	struct review_range const *p = f->ranges.items;
	/* The ranges before i end before line lines.first - 1, the ranges from
	 * j on start after line lines.last + 1, and the ranges from i to j - 1
	 * overlap or adjoin lines. */
	size_t i = 0;
	while (i < f->ranges.n && (uint64_t)p[i].last + 1 < lines.first)
		++i;
	size_t j = i;
	while (j < f->ranges.n && p[j].first <= (uint64_t)lines.last + 1)
		++j;
	struct review_range put[3];
	size_t const m = review_range_merge(p + i, j - i, lines, put);
	size_t const n = f->ranges.n - (j - i) + m;
	size_t const size = r->size - review_charge(f->ranges.n, 0) + review_charge(n, 0);
	if (!review_within_max(r, size, lines.turn))
		return 0;
	if (util_splice(f->ranges.items, f->ranges.n, f->ranges.cap, i, j - i, m))
		return ENOMEM;
	memcpy(&f->ranges.items[i], put, m * sizeof *put);
	if (total)
		f->total = total;
	r->size = size;
	return 0;
}

/**
 * Adds the range @p lines to the file of @p list with the repository,
 * commit and path of @p key, and adds the file if @p list does not contain
 * it. A nonzero key->total replaces the total of the file. If the charge
 * of @p r would exceed TOOL_TEXT_MAX, nothing changes (review_within_max()).
 * Returns 0 or ENOMEM.
 */
static int
review_files_add (struct review            *r,
                  struct review_files       *list,
                  struct call_source const  *key,
                  struct review_range        lines)
{
	size_t at;
	size_t const k = review_files_find(list, key, &at);
	if (k < list->n)
		return review_file_add_range(r, &list->items[k], (uint32_t)key->total, lines);
	size_t const size = r->size + review_charge(2, key->repo.len + key->path.len);
	if (!review_within_max(r, size, lines.turn))
		return 0;
	struct review_file f = review_file();
	if (util_grow(f.ranges.items, f.ranges.cap, 1))
		return ENOMEM;
	f.ranges.items[0] = lines;
	f.ranges.n = 1;
	int e = review_file_set(&f, key);
	if (!e)
		e = review_files_insert(list, &f, at);
	if (e) {
		review_file_fini(&f);
		return e;
	}
	r->size = size;
	return 0;
}

int
review_pending_add (struct review          *r,
                    struct seen_read const *read,
                    struct seen_lines       lines,
                    uint32_t                turn)
{
	if (!r || !read)
		return EFAULT;
	if (!lines.first)
		return 0;
	if (lines.first < read->src.first || lines.last < lines.first || lines.last > read->src.last)
		return EINVAL;
	return review_files_add(r, &r->pending, &read->src, (struct review_range){
		lines.first, lines.last, turn });
}

/** Subtracts the charges of the pending files of @p r from r->size. */
static void
review_pending_uncharge (struct review *r)
{
	for (size_t i = 0; i < r->pending.n; ++i)
		r->size -= review_file_charge(&r->pending.items[i], r->pending.items[i].ranges.n);
}

/**
 * Moves the pending file @p f into the read log of @p r if the log does
 * not contain the file. Otherwise adds the ranges of @p f to the logged
 * file. The charge of @p f must already be subtracted from r->size. A file
 * that would make the charge exceed TOOL_TEXT_MAX is not logged, as in
 * review_files_add(). A moved file leaves @p f empty. Returns 0 or ENOMEM.
 */
static int
review_log_file (struct review      *r,
                 struct review_file *f)
{
	struct call_source const key = review_file_source(f);
	size_t at;
	size_t const k = review_files_find(&r->log, &key, &at);
	if (k < r->log.n) {
		int e = 0;
		for (size_t i = 0; !e && i < f->ranges.n; ++i)
			e = review_file_add_range(r, &r->log.items[k], f->total, f->ranges.items[i]);
		return e;
	}
	size_t const size = r->size + review_file_charge(f, f->ranges.n);
	if (!review_within_max(r, size, f->ranges.items[0].turn))
		return 0;
	int e = review_files_insert(&r->log, f, at);
	if (e)
		return e;
	*f = review_file();
	r->size = size;
	return 0;
}

int
review_pending_log (struct review *r)
{
	if (!r)
		return EFAULT;
	review_pending_uncharge(r);
	int e = 0;
	for (size_t i = 0; !e && i < r->pending.n; ++i)
		e = review_log_file(r, &r->pending.items[i]);
	review_files_clear(&r->pending);
	return e;
}

void
review_pending_drop (struct review *r)
{
	if (!r)
		return;
	review_pending_uncharge(r);
	review_files_clear(&r->pending);
}

/** Appends the string literal @p s to @p out. Evaluates to 0, or to ENOMEM
 *  if out of memory, as the review_put functions return.
 */
#define PUT(out, s) cstr_view_cat(CSTR_VIEW(s), out)

/** Appends the line numbers @p first to @p last to @p out, as "first" or
 *  "first-last".
 */
static int
review_put_lines (struct dstr *out,
                  uint32_t     first,
                  uint32_t     last)
{
	return (first == last ? dstr_printf(out, "%" PRIu32, first)
	                      : dstr_printf(out, "%" PRIu32 "-%" PRIu32, first, last)) ? 0 : ENOMEM;
}

/** Appends the location @p path, @p first and @p last to @p out, as
 *  "path:first" or "path:first-last".
 */
static int
review_put_location (struct dstr      *out,
                     struct cstr_view  path,
                     uint32_t          first,
                     uint32_t          last)
{
	return cstr_view_cat(path, out) || PUT(out, ":") || review_put_lines(out, first, last)
	       ? ENOMEM : 0;
}

/** Appends " at " and the first REVIEW_SHORT_OID digits of @p oid to
 *  @p out.
 */
static int
review_put_oid (struct dstr      *out,
                struct oid const *oid)
{
	struct cstr_view const digits = { oid->hex, REVIEW_SHORT_OID };
	return PUT(out, " at ") || cstr_view_cat(digits, out) ? ENOMEM : 0;
}

/**
 * Appends the status of the finding @p f to @p out: the state, and the
 * location if there is one. The location is followed by the commit at
 * which its lines were read, or by "not read".
 */
static int
review_put_status (struct dstr                 *out,
                   struct review_finding const *f)
{
	struct review_change const *c = &f->changes.items[f->changes.n - 1];
	struct cstr_view const path = cstr_view_dstr(&f->location.path);
	if (cstr_view_cat(review_state_names[c->state], out))
		return ENOMEM;
	if (!path.len)
		return 0;
	return PUT(out, ", ") || review_put_location(out, path, f->location.first, f->location.last) ||
	       (oid_len(&c->commit) ? review_put_oid(out, &c->commit) : PUT(out, ", not read"))
	       ? ENOMEM : 0;
}

/** Returns true if the finding @p f is closed. */
static bool
review_finding_closed (struct review_finding const *f)
{
	return REVIEW_CLOSED_STATES >> f->changes.items[f->changes.n - 1].state & 1U;
}

/** The sections of the text of a record or of a history, in the order of
 *  the text.
 */
enum review_section : uint8_t {
	REVIEW_SECTION_HEAD,     /**< the first lines */
	REVIEW_SECTION_OPEN,     /**< the open findings */
	REVIEW_SECTION_CLOSED,   /**< the closed findings */
	REVIEW_SECTION_LOG,      /**< the files of the read log */
	REVIEW_SECTION_DROPPED,  /**< the notice about lines that were not logged */
	REVIEW_SECTION_CHANGES,  /**< the changes of the finding of a history */
	REVIEW_SECTION_END,
};

/**
 * Appends the next finding of the section at @p cursor of the text of the
 * record @p r to @p out: an open finding with its claim and latest
 * evidence, or a closed finding on one line. A line that names the closed
 * states comes before the first closed finding. Moves @p cursor past the
 * finding, or to the next section if no finding of the section is left.
 */
static int
review_put_finding (struct dstr         *out,
                    struct review const *r,
                    struct tool_cursor  *cursor)
{
	bool const closed = cursor->section == REVIEW_SECTION_CLOSED;
	size_t k = cursor->item;
	while (k < r->findings.n && review_finding_closed(&r->findings.items[k]) != closed)
		++k;
	if (k == r->findings.n) {
		*cursor = (struct tool_cursor){
			.section = closed ? REVIEW_SECTION_LOG : REVIEW_SECTION_CLOSED,
		};
		return 0;
	}
	struct review_finding const *f = &r->findings.items[k];
	struct cstr_view const claim = cstr_view_dstr(&f->claim);
	if ((closed && !cursor->item && PUT(out, "[askai: disproved and retracted findings:]\n")) ||
	    !dstr_printf(out, "%zu. ", k + 1) || review_put_status(out, f))
		return ENOMEM;
	cursor->item = k + 1;
	if (closed) {
		/* The first line of the claim, cut to REVIEW_CLOSED_CLAIM bytes. */
		struct cstr_view head = cstr_view_slice(claim, 0, REVIEW_CLOSED_CLAIM);
		if (!cstr_view_cut(&head, '\n', &head))
			head.len = util_clip(claim.data, claim.len, REVIEW_CLOSED_CLAIM);
		return PUT(out, ": ") || cstr_view_cat(head, out) ||
		       (head.len < claim.len && PUT(out, "...")) || PUT(out, "\n") ? ENOMEM : 0;
	}
	struct cstr_view evidence = {};
	for (size_t i = f->changes.n; i && !evidence.len; --i)
		evidence = cstr_view_dstr(&f->changes.items[i - 1].evidence);
	return PUT(out, "\n   claim: ") || cstr_view_cat(claim, out) ||
	       (evidence.len && (PUT(out, "\n   evidence: ") || cstr_view_cat(evidence, out))) ||
	       PUT(out, "\n") ? ENOMEM : 0;
}

/** Appends the ranges of the file @p f to @p out, separated by ", ".
 *  Adjoining ranges are written as one.
 */
static int
review_put_ranges (struct dstr              *out,
                   struct review_file const *f)
{
	int e = 0;
	for (size_t k = 0; !e && k < f->ranges.n;) {
		uint32_t const first = f->ranges.items[k].first;
		uint32_t last = f->ranges.items[k].last;
		while (++k < f->ranges.n && f->ranges.items[k].first == (uint64_t)last + 1)
			last = f->ranges.items[k].last;
		e = review_put_lines(out, first, last) || (k < f->ranges.n && PUT(out, ", ")) ? ENOMEM : 0;
	}
	return e;
}

/**
 * Appends the line of the logged file @p f to @p out: the path, the line
 * ranges, and the number of lines that they cover with the line count of
 * the file.
 */
static int
review_put_file (struct dstr              *out,
                 struct review_file const *f)
{
	uint64_t lines = 0;
	for (size_t k = 0; k < f->ranges.n; ++k)
		lines += (uint64_t)(f->ranges.items[k].last - f->ranges.items[k].first) + 1;
	if (PUT(out, "  ") || cstr_view_cat(cstr_view_dstr(&f->path), out) ||
	    !dstr_printf(out, "  line%s ", util_s(lines)) || review_put_ranges(out, f))
		return ENOMEM;
	/* The ranges are within the file and do not overlap, so they cover
	 * the whole file if lines is f->total. */
	if (lines == f->total)
		return dstr_printf(out, ": all %" PRIu64 " line%s\n", lines, util_s(lines)) ? 0 : ENOMEM;
	if (f->total)
		return dstr_printf(out, ": %" PRIu64 " of %" PRIu32 " lines\n", lines, f->total)
		       ? 0 : ENOMEM;
	return dstr_printf(out, ": %" PRIu64 " of an unknown number of lines\n", lines) ? 0 : ENOMEM;
}

/**
 * Appends the line of the file @p i of the read log of @p r to @p out. The
 * header of the log comes before the first file, and a line with the
 * repository and the commit comes before the first file of each commit.
 */
static int
review_put_logged (struct dstr         *out,
                   struct review const *r,
                   size_t               i)
{
	struct review_file const *f = &r->log.items[i];
	bool const group = !i || !review_file_commit_equal(f, cstr_view_dstr(&f[-1].repo),
	                                                    oid_view(&f[-1].commit));
	return (!i && PUT(out, "[askai: your read log of the lines that git read and code "
	                       "describe showed you in this ask call:]\n")) ||
	       (group && (cstr_view_cat(cstr_view_dstr(&f->repo), out) || PUT(out, " ") ||
	                  cstr_view_cat(oid_view(&f->commit), out) || PUT(out, "\n"))) ||
	       review_put_file(out, f) ? ENOMEM : 0;
}

/** Appends the lines of the change @p c of a history to @p out: the turn,
 *  the state and the commit, then the evidence, the old claim and the old
 *  location.
 */
static int
review_put_change (struct dstr                *out,
                   struct review_change const *c)
{
	struct cstr_view const evidence = cstr_view_dstr(&c->evidence);
	struct cstr_view const claim = cstr_view_dstr(&c->old_claim);
	struct cstr_view const path = cstr_view_dstr(&c->old_location.path);
	return !dstr_printf(out, "turn %" PRIu32 ": ", c->turn) ||
	       cstr_view_cat(review_state_names[c->state], out) ||
	       (oid_len(&c->commit) && review_put_oid(out, &c->commit)) || PUT(out, "\n") ||
	       (evidence.len && (PUT(out, "  evidence: ") || cstr_view_cat(evidence, out) ||
	                         PUT(out, "\n"))) ||
	       (claim.len && (PUT(out, "  old claim: ") || cstr_view_cat(claim, out) ||
	                      PUT(out, "\n"))) ||
	       (c->moved && (PUT(out, "  old location: ") ||
	                     (path.len ? review_put_location(out, path, c->old_location.first,
	                                                     c->old_location.last)
	                               : PUT(out, "none")) ||
	                     PUT(out, "\n"))) ? ENOMEM : 0;
}

/* Appends the line before the findings of @p r, which has some: their
 * number, and the number in each state that has any. Returns 0 or ENOMEM.
 */
static int
review_put_counts (struct dstr         *out,
                   struct review const *r)
{
	size_t n[REVIEW_STATES] = {};
	for (size_t k = 0; k < r->findings.n; ++k)
		++n[r->findings.items[k].changes.items[r->findings.items[k].changes.n - 1].state];
	if (!dstr_printf(out, "[askai: your %zu finding%s", r->findings.n, util_s(r->findings.n)))
		return ENOMEM;
	char const *sep = ": ";
	for (size_t k = 0; k < REVIEW_STATES; ++k) {
		if (!n[k])
			continue;
		if (!dstr_printf(out, "%s%zu %s", sep, n[k], review_state_names[k].data))
			return ENOMEM;
		sep = ", ";
	}
	return PUT(out, ". Call findings with action show and id for the history of a "
	                "finding.]\n") ? ENOMEM : 0;
}

/**
 * Appends the item at @p cursor to @p out and moves @p cursor to the next
 * item. The items are the lines of the text of the record @p r, or of the
 * empty record if @p r is nullptr, or of the history of the finding @p id
 * of @p r if @p id is not 0. A section without items is passed without
 * writing anything.
 */
static int
review_put_item (struct dstr         *out,
                 struct review const *r,
                 size_t               id,
                 struct tool_cursor  *cursor)
{
	struct review_finding const *f = id ? &r->findings.items[id - 1] : nullptr;
	switch (cursor->section) {
	case REVIEW_SECTION_HEAD:
		if (f) {
			*cursor = (struct tool_cursor){ .section = REVIEW_SECTION_CHANGES };
			return !dstr_printf(out, "Finding %zu: ", id) || review_put_status(out, f) ||
			       PUT(out, "\nclaim: ") || cstr_view_cat(cstr_view_dstr(&f->claim), out) ||
			       PUT(out, "\n") ? ENOMEM : 0;
		}
		if (PUT(out, "[askai: your review record. A compaction does not remove the "
		             "record.]\n"))
			return ENOMEM;
		if (!r || !r->findings.n) {
			*cursor = (struct tool_cursor){ .section = REVIEW_SECTION_LOG };
			return PUT(out, "[askai: no findings yet. Record findings with the "
			                "findings tool.]\n") ? ENOMEM : 0;
		}
		*cursor = (struct tool_cursor){ .section = REVIEW_SECTION_OPEN };
		return review_put_counts(out, r);
	case REVIEW_SECTION_OPEN:
	case REVIEW_SECTION_CLOSED:
		return review_put_finding(out, r, cursor);
	case REVIEW_SECTION_LOG:
		if (r && cursor->item < r->log.n)
			return review_put_logged(out, r, cursor->item++);
		*cursor = (struct tool_cursor){ .section = REVIEW_SECTION_DROPPED };
		return 0;
	case REVIEW_SECTION_DROPPED:
		cursor->section = REVIEW_SECTION_END;
		return r && r->dropped_turn &&
		       !dstr_printf(out, "[askai: the read log is full. Lines that you read from "
		                    "turn %" PRIu32 " on can be missing from the read log.]\n",
		                    r->dropped_turn)
		       ? ENOMEM : 0;
	case REVIEW_SECTION_CHANGES:
		if (cursor->item < f->changes.n)
			return review_put_change(out, &f->changes.items[cursor->item++]);
		cursor->section = REVIEW_SECTION_END;
		return 0;
	default:
		return 0;
	}
}

/**
 * Appends the text from @p cursor on to @p out (review_put_item()), at
 * most @p room bytes, and moves @p cursor to the end of what it appended.
 * An item that does not fit is cut, and @p cursor stores the bytes of the
 * item that are written. Every line of the text ends with a newline, and
 * the last newline is dropped. Returns 0 at the end of the text, EAGAIN
 * if the text continues, or ENOMEM.
 */
static int
review_put_text (struct dstr         *out,
                 struct review const *r,
                 size_t               id,
                 struct tool_cursor  *cursor,
                 size_t               room)
{
	size_t const start = dstr_len(out);
	while (cursor->section != REVIEW_SECTION_END) {
		struct tool_cursor const at = *cursor;
		size_t const before = dstr_len(out);
		if (review_put_item(out, r, id, cursor))
			return ENOMEM;
		/* Drop the bytes of an item that an earlier part cut and wrote. */
		dstr_erase(out, before, at.byte);
		if (dstr_len(out) - start > room) {
			dstr_erase(out, start + room, SIZE_MAX);
			*cursor = at;
			cursor->byte += start + room - before;
			return EAGAIN;
		}
		cursor->byte = 0;
	}
	dstr_erase(out, dstr_len(out) - 1, 1);
	return 0;
}

int
review_render (struct review const *r,
               struct dstr         *out,
               size_t               most)
{
	if (!out)
		return EFAULT;
	/* The last newline is dropped, so the text fits if its length is at
	 * most (most + 1). */
	size_t const start = dstr_len(out);
	struct tool_cursor cursor = {};
	int const e = review_put_text(out, r, 0, &cursor, most < SIZE_MAX ? most + 1 : most);
	if (e != EAGAIN)
		return e;
	struct cstr_view const text = cstr_view_slice(cstr_view_dstr(out), start, SIZE_MAX);
	/* The text stopped at most + 1 bytes. Cut it after its last newline
	 * within most bytes. The text is not empty, and the test of text.len
	 * only shows that to the compiler. */
	char const *nl = text.len ? memrchr(text.data, '\n', text.len - 1) : nullptr;
	size_t const keep = nl ? (size_t)(nl - text.data) + 1 : 0;
	dstr_erase(out, start + keep, SIZE_MAX);
	return dstr_printf(out, "[askai: the record continues at byte %zu. Call findings with "
	                   "action show and \"offset\": %zu.]", keep, keep)
	       ? 0 : ENOMEM;
}

/* Appends a refusal to @p out with dstr_printf(). Evaluates to EINVAL, or
 * to ENOMEM if the append fails.
 */
#define REFUSE(out, ...) (dstr_printf(out, __VA_ARGS__) ? EINVAL : ENOMEM)

/** The arguments of the findings tool: X(ID, name), the constant
 *  REVIEW_ARG_ID of enum review_arg and the name.
 */
#define REVIEW_ARG_LIST(X)      \
        X(ACTION,   "action")   \
        X(ID,       "id")       \
        X(CLAIM,    "claim")    \
        X(LOCATION, "location") \
        X(STATE,    "state")    \
        X(EVIDENCE, "evidence") \
        X(OFFSET,   "offset")

enum review_arg : uint8_t {
#define REVIEW_ARG_ENUM(id, ...) REVIEW_ARG_##id,
	REVIEW_ARG_LIST(REVIEW_ARG_ENUM)
#undef REVIEW_ARG_ENUM
	REVIEW_ARGS
};

static struct cstr_view const review_arg_names[] = { REVIEW_ARG_LIST(XL_VIEW) };
#undef REVIEW_ARG_LIST

/** The arguments of each action, as bit masks of enum review_arg. */
#define A(x) (1U << REVIEW_ARG_##x)
static uint8_t const review_action_args[REVIEW_ACTIONS] = {
	[REVIEW_ADD]    = A(ACTION) | A(CLAIM) | A(LOCATION) | A(STATE) | A(EVIDENCE),
	[REVIEW_UPDATE] = A(ACTION) | A(ID) | A(CLAIM) | A(LOCATION) | A(STATE) | A(EVIDENCE),
	[REVIEW_SHOW]   = A(ACTION) | A(ID) | A(OFFSET),
};
#undef A

/** The bits of review_args.trimmed. */
enum review_trimmed : uint8_t {
	REVIEW_TRIMMED_CLAIM    = 1U << 0U,
	REVIEW_TRIMMED_EVIDENCE = 1U << 1U,
};

/** The parsed arguments of a findings call. */
struct review_args {
	struct cstr_view            claim;     /**< empty if there is none */
	struct cstr_view            evidence;  /**< empty if there is none */
	struct review_location_view location;  /**< location.path is empty if
	                                            there is no location */
	uint32_t                    id;        /**< 0 if there is none */
	enum review_action          action;
	enum review_state           state;     /**< REVIEW_STATES if there is
	                                            none */
	uint16_t                    trimmed;   /**< enum review_trimmed bits of
	                                            the texts that @p kept
	                                            trimmed */
};

/**
 * Parses the location argument @p t, of the form [commit:]path:first[-last],
 * into @p *loc. The path is the text before the last colon, and the line
 * numbers must satisfy 1 <= first <= last. A commit is REVIEW_OID_MIN to
 * OID_SHA256 hexadecimal digits, as git grep prints it. A leading "HEAD:"
 * means no commit. Returns 0 or EINVAL.
 */
static int
review_args_location (struct review_location_view *loc,
                      struct cstr_view             t)
{
	char const *colon = memrchr(t.data, ':', t.len);
	if (!colon)
		return EINVAL;
	char const *q = colon + 1;
	char const *end = t.data + t.len;
	uint64_t first;
	q += util_digits(q, (size_t)(end - q), UINT32_MAX, &first);
	uint64_t last = first;
	if (first && q < end && *q == '-')
		q += 1 + util_digits(q + 1, (size_t)(end - q - 1), UINT32_MAX, &last);
	if (!first || last < first || q != end)
		return EINVAL;
	struct cstr_view const path = { t.data, (size_t)(colon - t.data) };
	*loc = (struct review_location_view){ .first = (uint32_t)first, .last = (uint32_t)last };
	char const *sep = memchr(path.data, ':', path.len);
	size_t const hex = sep ? (size_t)(sep - path.data) : 0;
	size_t skip = 0;
	if (hex >= REVIEW_OID_MIN && hex <= OID_SHA256 && util_is_hex(path.data, hex)) {
		loc->prefix = (struct cstr_view){ path.data, hex };
		skip = hex + 1;
	} else if (cstr_view_starts(path, CSTR_VIEW("HEAD:"))) {
		skip = sizeof "HEAD:" - 1;
	}
	loc->path = cstr_view_slice(path, skip, SIZE_MAX);
	return review_name_valid(loc->path) ? 0 : EINVAL;
}

/**
 * Stores the text of the string argument @p v, named @p name, in @p *t.
 * If @p kept is not nullptr, it trims the text, and @p bit is set in
 * a->trimmed if it does. Returns 0, or EINVAL with a refusal appended to
 * @p out, or ENOMEM if the append fails.
 */
static int
review_args_text (struct review_args *a,
                  struct json_object *v,
                  struct cstr_view    name,
                  review_kept_fn     *kept,
                  uint8_t             bit,
                  struct cstr_view   *t,
                  struct dstr        *out)
{
	struct cstr_view const s = jb_view(v);
	int const len = (int)name.len;
	if (!s.len || jb_utf8(s.data, s.len) != s.len)
		return REFUSE(out, "The %.*s argument must be a non-empty string of UTF-8 text.",
		              len, name.data);
	size_t const n = kept ? kept(s.data, s.len) : s.len;
	*t = s;
	if (n >= s.len)
		return 0;
	/* Also drop the spaces and line ends before the notices. */
	*t = cstr_view_trim_end((struct cstr_view){ s.data, n });
	if (!t->len)
		return REFUSE(out, "The %.*s argument contains only [askai: ...] lines from tool "
		              "results. Give the actual %.*s.", len, name.data, len, name.data);
	a->trimmed |= bit;
	return 0;
}

/**
 * Parses the arguments @p args of a findings call into @p a. Each argument
 * must be one that the action accepts. The id must be the id of a finding
 * of @p r, and update needs one. Returns 0, or EINVAL with a refusal
 * appended to @p out, or ENOMEM if the append fails.
 */
static int
review_args_parse (struct review_args  *a,
                   struct review const *r,
                   struct json_object  *args,
                   review_kept_fn      *kept,
                   struct dstr         *out)
{
	*a = (struct review_args){ .state = REVIEW_STATES };
	/* jb_get() returns nullptr if args is not an object, and jb_view()
	 * returns an empty view for nullptr. */
	a->action = (enum review_action)cstr_view_index(review_action_names, REVIEW_ACTIONS,
	                                                 jb_view(jb_get(args, "action")));
	if (a->action == REVIEW_ACTIONS)
		return REFUSE(out, "The action argument must be "
		              XL_JOIN(", ", " or ", REVIEW_ACTION_LIST(XL_TEXT)) ".");
	/* Refuse the first argument that the action does not accept. */
	struct json_object *of[REVIEW_ARGS] = {};
	uint64_t seen;
	struct cstr_view other;
	if (jb_args(args, review_arg_names, REVIEW_ARGS, review_action_args[a->action], of, &seen,
	            &other))
		return cstr_view_index(review_arg_names, REVIEW_ARGS, other) == REVIEW_ARGS
		       ? REFUSE(out, "Unknown argument %.*s for the findings tool.", (int)other.len,
		                other.data)
		       : REFUSE(out, "The argument %.*s is not valid with action %s.",
		                (int)other.len, other.data, review_action_names[a->action].data);
	struct json_object *id = of[REVIEW_ARG_ID];
	uint64_t const n = jb_count(id, SIZE_MAX);
	if (id ? !n : a->action == REVIEW_UPDATE)
		return REFUSE(out, id ? "The id argument must be the number of a finding, such as 3."
		                      : "update requires the id argument.");
	if (n > r->findings.n) {
		/* %.0zu writes nothing for 0. */
		static char const *const have[] = {
			"no findings yet", "only finding ", "findings 1 to ",
		};
		size_t const k = r->findings.n;
		return REFUSE(out, "There is no finding %" PRIu64 ". The record contains %s%.0zu.",
		              n, have[k < 2 ? k : 2], k);
	}
	a->id = (uint32_t)n;
	struct json_object *state = of[REVIEW_ARG_STATE];
	if (state) {
		a->state = (enum review_state)cstr_view_index(review_state_names, REVIEW_STATES,
		                                               jb_view(state));
		if (a->state == REVIEW_STATES)
			return REFUSE(out, "The state argument must be "
			              XL_JOIN(", ", " or ", REVIEW_STATE_LIST(XL_TEXT)) ".");
	}
	struct json_object *location = of[REVIEW_ARG_LOCATION];
	int e = 0;
	if (of[REVIEW_ARG_CLAIM])
		e = review_args_text(a, of[REVIEW_ARG_CLAIM], review_arg_names[REVIEW_ARG_CLAIM],
		                     kept, REVIEW_TRIMMED_CLAIM, &a->claim, out);
	if (!e && of[REVIEW_ARG_EVIDENCE])
		e = review_args_text(a, of[REVIEW_ARG_EVIDENCE], review_arg_names[REVIEW_ARG_EVIDENCE],
		                     kept, REVIEW_TRIMMED_EVIDENCE, &a->evidence, out);
	if (e || !location)
		return e;
	struct cstr_view text;
	e = review_args_text(a, location, review_arg_names[REVIEW_ARG_LOCATION], nullptr, 0,
	                     &text, out);
	if (!e && review_args_location(&a->location, text))
		e = REFUSE(out, "The location argument must be path:line or path:first-last, "
		           "such as src/mcp.c:120-140. Leave out location if no lines of a file "
		           "show the claim.");
	return e;
}

/**
 * Returns the last line of the run of adjoining ranges of @p f that starts
 * with the range that contains line @p a and ends at line @p b or before
 * the first gap, or 0 if no range of @p f contains line @p a. If the
 * result is not 0, @p *turn is set to the lowest turn of the run.
 */
static uint32_t
review_file_run_end (struct review_file const *f,
                     uint32_t                  a,
                     uint32_t                  b,
                     uint32_t                 *turn)
{
	size_t lo = 0;
	size_t hi = f->ranges.n;
	while (lo < hi) {
		size_t const mid = lo + (hi - lo) / 2;
		if (f->ranges.items[mid].first <= a)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo || f->ranges.items[lo - 1].last < a)
		return 0;
	struct review_range const *g = &f->ranges.items[lo - 1];
	struct review_range const *end = f->ranges.items + f->ranges.n;
	uint32_t last = g->last;
	*turn = g->turn;
	while (last < b && ++g < end && g->first == (uint64_t)last + 1) {
		last = g->last;
		*turn = g->turn < *turn ? g->turn : *turn;
	}
	return last;
}

/** The ranks of the logged files for a location in review_log_cover(). */
enum review_rank : uint8_t {
	REVIEW_RANK_NONE,     /**< no file */
	REVIEW_RANK_SHORTER,  /**< a run from the first line, in a file with
	                           fewer lines than the location */
	REVIEW_RANK_PART,     /**< a run from the first line, not to the last */
	REVIEW_RANK_ALL,      /**< a run over all lines of the location */
};

/** The logged files that review_log_cover() selects for a location. */
struct review_cover {
	struct review_file const *best;     /**< the best file, or nullptr */
	struct review_file const *largest;  /**< the file with the largest
	                                         total, where 0 is largest, or
	                                         nullptr */
	uint32_t                  end;      /**< the last line of the run of
	                                         best from the first line, or 0 */
	uint32_t                  turn;     /**< the lowest turn of that run */
};

/**
 * Selects the files of the read log of @p r with the path of @p loc and a
 * commit that starts with its prefix. The best file contains all lines of
 * @p loc in one run of adjoining ranges, and among those files the file
 * whose oldest read of the lines is newest is best. Otherwise the best
 * file contains the longest run from loc->first, and files with a total
 * of 0 (unknown) or at least loc->last rank before the others.
 */
static struct review_cover
review_log_cover (struct review const               *r,
                  struct review_location_view const *loc)
{
	struct review_cover c = {};
	enum review_rank rank = REVIEW_RANK_NONE;
	for (size_t i = 0; i < r->log.n; ++i) {
		struct review_file const *f = &r->log.items[i];
		if (!cstr_view_equal(cstr_view_dstr(&f->path), loc->path) ||
		    !oid_starts(&f->commit, loc->prefix))
			continue;
		if (!c.largest || (c.largest->total && (!f->total || f->total > c.largest->total)))
			c.largest = f;
		uint32_t turn = 0;
		uint32_t const end = review_file_run_end(f, loc->first, loc->last, &turn);
		if (!end)
			continue;
		enum review_rank k = REVIEW_RANK_ALL;
		if (end < loc->last)
			k = !f->total || f->total >= loc->last ? REVIEW_RANK_PART : REVIEW_RANK_SHORTER;
		if (k < rank || (k == rank && (k == REVIEW_RANK_ALL ? turn < c.turn : end < c.end)))
			continue;
		c = (struct review_cover){ f, c.largest, end, turn };
		rank = k;
	}
	return c;
}

/**
 * Resolves loc->path against the paths of the read log of @p r whose
 * commit starts with loc->prefix. If a logged path equals loc->path, stores
 * loc->path in found[0] and returns 1. Otherwise the matches are the
 * logged paths that end with '/' and loc->path. Stores up to @p max
 * distinct matches in @p found and returns their number.
 */
static size_t
review_log_resolve (struct review const               *r,
                    struct review_location_view const *loc,
                    struct cstr_view                  *found,
                    size_t                             max)
{
	size_t n = 0;
	size_t const tail = loc->path.len;
	for (size_t i = 0; i < r->log.n; ++i) {
		struct review_file const *f = &r->log.items[i];
		if (!oid_starts(&f->commit, loc->prefix))
			continue;
		struct cstr_view const path = cstr_view_dstr(&f->path);
		if (cstr_view_equal(path, loc->path)) {
			found[0] = loc->path;
			return 1;
		}
		if (path.len <= tail || path.data[path.len - tail - 1] != '/' ||
		    !cstr_view_equal(cstr_view_slice(path, path.len - tail, tail), loc->path) ||
		    n == max || cstr_view_index(found, n, path) < n)
			continue;
		found[n++] = path;
	}
	return n;
}

/**
 * Appends a refusal of the state @p state to @p out. The lines of the
 * location @p loc are not all in the read log of @p r. If the flags of
 * @p r contain REVIEW_GIT and the record is not full, the refusal also
 * contains the arguments of a git tool read that adds the lines to the
 * log. @p c is the result of review_log_cover() for @p loc, and
 * @p resolved is the number of logged paths that loc->path resolved to.
 * Returns EINVAL, or ENOMEM if the append fails.
 */
static int
review_refuse_unread (struct review const               *r,
                      struct review_location_view const *loc,
                      enum review_state                  state,
                      struct review_cover const         *c,
                      size_t                             resolved,
                      struct dstr                       *out)
{
	char const *name = review_state_names[state].data;
	int const len = (int)loc->path.len;
	char const *path = loc->path.data;
	struct review_file const *largest = c->largest;
	struct cstr_view const prefix = loc->prefix;
	if (largest && largest->total && largest->total < loc->last)
		return REFUSE(out, "At %.*s, %.*s ends at line %" PRIu32 ". Give a location within "
		              "lines 1 to %" PRIu32 ".", REVIEW_SHORT_OID, largest->commit.hex, len,
		              path, largest->total, largest->total);
	/* Select the read to suggest. If the best file can contain line
	 * loc->last, the read starts at line c->end + 1 at the commit of the
	 * best file. If the best file contains fewer lines, no file that can
	 * contain loc->last contains loc->first, so the read starts at
	 * loc->first at the commit of the largest file. Without a best file it
	 * starts at loc->first at the prefix or HEAD. */
	bool const shorter = c->best && c->best->total && c->best->total < loc->last;
	struct review_file const *to = shorter ? largest : c->best;
	bool const rest = to && !shorter;
	struct cstr_view const rev = to ? (struct cstr_view){ to->commit.hex, REVIEW_SHORT_OID }
	                           : prefix.len ? prefix : CSTR_VIEW("HEAD");
	uint32_t const end = c->best ? c->end : loc->last;
	bool failed = !dstr_printf(out, "%s requires the lines of the location in your read log. "
	                           "The read log %s %.*s line%s ", name,
	                           c->best ? "contains" : "does not contain", len, path,
	                           loc->first == end ? "" : "s") ||
	              review_put_lines(out, loc->first, end);
	if (!c->best)
		failed = failed ||
		         (prefix.len && (PUT(out, " at ") || cstr_view_cat(prefix, out))) ||
		         PUT(out, ".");
	else
		failed = failed || review_put_oid(out, &c->best->commit) ||
		         !dstr_printf(out, ", but not line%s ", end + 1 < loc->last ? "s" : "") ||
		         review_put_lines(out, end + 1, loc->last) || PUT(out, ".") ||
		         (shorter && !dstr_printf(out, " At %.*s, %.*s ends at line %" PRIu32 ".",
		                                  REVIEW_SHORT_OID, c->best->commit.hex, len, path,
		                                  c->best->total));
	/* The read is logged as a new pending file after a later turn. Its
	 * charge is estimated from the file to read, or from a file with this
	 * path and a 1-byte repository name. */
	size_t const charge = to ? review_file_charge(to, 1) : review_charge(2, 1 + loc->path.len);
	if (r->size + charge > TOOL_TEXT_MAX) {
		if (r->dropped_turn)
			failed = failed || !dstr_printf(out, " The read log has been full since turn %"
			                                PRIu32 ".", r->dropped_turn);
		else
			failed = failed || PUT(out, " The record is full.");
		return failed || PUT(out, " A new read does not enter the read log. Give a "
		                          "location with lines from your read log, or keep the "
		                          "finding a candidate.")
		       ? ENOMEM : EINVAL;
	}
	if (!(r->flags & REVIEW_GIT))
		return failed || PUT(out, " The git tool is not available in this ask call. "
		                          "Lines from the describe action of the code tool enter "
		                          "the read log in your next turn.")
		       ? ENOMEM : EINVAL;
	static char const *const hints[] = {
		" The path in revisions must be the whole path from the top of the repository.",
		"",
		" A long range can need several reads.",
	};
	return failed || !dstr_printf(out, " To read %s, call the git tool with \"command\": "
	                              "\"read\", \"revisions\": [\"%.*s:",
	                              rest ? "the missing lines" : "the lines", (int)rev.len,
	                              rev.data) ||
	       jb_escape_cat(loc->path, out) ||
	       !dstr_printf(out, "\"] and \"line\": %" PRIu32 ".%s The lines of a read enter the "
	                    "read log in your next turn. Call findings again in a later turn.",
	                    rest ? c->end + 1 : loc->first, hints[to ? 2 : resolved != 0])
	       ? ENOMEM : EINVAL;
}

/**
 * Checks the location @p loc of a finding in the state @p state against
 * the read log of @p r, and stores in @p *commit the commit of the logged
 * file that contains all lines of @p loc, or an empty one. A short path
 * that resolves to one logged path (review_log_resolve()) is replaced by
 * that path. A state that requires logged lines is refused for a record
 * without REVIEW_READ_LOG, without a location, or for lines that the log
 * does not contain. A short path that resolves to several logged paths is
 * refused if the arguments contain the location (@p in_args) or the state
 * requires logged lines. Returns 0, or EINVAL with a refusal appended to
 * @p out, or ENOMEM if the append fails.
 */
static int
review_check_location (struct review const          *r,
                       struct review_location_view  *loc,
                       enum review_state             state,
                       bool                          in_args,
                       struct oid                   *commit,
                       struct dstr                  *out)
{
	*commit = (struct oid){};
	bool const requires_log = review_state_logged(state);
	if (requires_log && !(r->flags & REVIEW_READ_LOG))
		return REFUSE(out, "verified and disproved need the git tool or the code tool. "
		              "Neither tool is available in this ask call. Keep the finding a "
		              "candidate, or retract the finding.");
	if (!loc->path.len)
		return !requires_log ? 0
		     : REFUSE(out, "%s requires a location with lines from your read log, such as "
		              "src/mcp.c:120-140.", review_state_names[state].data);
	struct cstr_view found[4];
	size_t const n = review_log_resolve(r, loc, found, array_size(found));
	if (n == 1)
		loc->path = found[0];
	if (n > 1 && (in_args || requires_log)) {
		/* List up to three paths, then "and others". */
		size_t const shown = n < array_size(found) ? n : array_size(found) - 1;
		return !dstr_printf(out, "%.*s matches ", (int)loc->path.len, loc->path.data) ||
		       cstr_view_join(found, shown, UINT64_MAX, CSTR_VIEW(", "),
		                      n == shown ? CSTR_VIEW(" and ") : CSTR_VIEW(", "), out) ||
		       (n > shown && PUT(out, " and others")) ||
		       PUT(out, " in your read log. Give the whole path in location.")
		       ? ENOMEM : EINVAL;
	}
	struct review_cover const c = review_log_cover(r, loc);
	if (c.best && c.end >= loc->last) {
		*commit = c.best->commit;
		return 0;
	}
	return requires_log ? review_refuse_unread(r, loc, state, &c, n, out) : 0;
}

/**
 * Returns 0 if a record of @p size bytes is within TOOL_TEXT_MAX. Otherwise
 * appends the refusal for the record @p r to @p out and returns EINVAL, or
 * ENOMEM if the append fails. @p fixed is the size of the record without
 * the claim and the evidence of the change.
 */
static int
review_check_size (struct review const *r,
                   size_t               size,
                   size_t               fixed,
                   struct dstr         *out)
{
	int const mib = TOOL_TEXT_MAX >> 20;
	if (size <= TOOL_TEXT_MAX)
		return 0;
	if (fixed < TOOL_TEXT_MAX)
		return REFUSE(out, "The record is limited to %d MiB. Give a claim and evidence of "
		              "at most %zu bytes together.", mib, TOOL_TEXT_MAX - fixed);
	/* The smallest change is an update with 1 byte of evidence, charged
	 * REVIEW_ITEM + 1 bytes. Pending lines can make room: a compaction
	 * drops them, and the log may charge less for them. Without pending
	 * lines no room becomes free, and a new read cannot become pending,
	 * because a read is charged at least 2 * REVIEW_ITEM + 2 bytes. */
	return REFUSE(out, "The record is limited to %d MiB. %s Keep further findings in your "
	              "notes or your summary.", mib,
	              r->pending.n || r->size + REVIEW_ITEM < TOOL_TEXT_MAX
	              ? "This change does not fit in the record."
	              : "No further change fits in the record of this ask call.");
}

/**
 * Appends the end of an add or update result for the finding @p f of @p r
 * to @p out. The end requests a read if @p f is a candidate whose lines
 * were not read and the flags of @p r contain REVIEW_READ_LOG. If
 * @p trimmed is not 0, the end lists the texts whose trailing notices were
 * removed.
 */
static int
review_put_tail (struct dstr                 *out,
                 struct review const         *r,
                 struct review_finding const *f,
                 uint16_t                     trimmed)
{
	static struct cstr_view const texts[] = {
		[REVIEW_TRIMMED_CLAIM]    = CSTR_VIEW_INIT("the claim"),
		[REVIEW_TRIMMED_EVIDENCE] = CSTR_VIEW_INIT("the evidence"),
		[REVIEW_TRIMMED_CLAIM | REVIEW_TRIMMED_EVIDENCE] =
			CSTR_VIEW_INIT("the claim and the evidence"),
	};
	struct review_change const *c = &f->changes.items[f->changes.n - 1];
	bool const unread = c->state == REVIEW_CANDIDATE && dstr_len(&f->location.path) &&
	                    !oid_len(&c->commit) && (r->flags & REVIEW_READ_LOG);
	return (unread && PUT(out, " Read the lines of the location before you verify the "
	                           "finding.")) ||
	       (trimmed && (PUT(out, " askai removed the [askai: ...] lines at the end of ") ||
	                    cstr_view_cat(texts[trimmed], out) || PUT(out, "."))) ? ENOMEM : 0;
}

/** Runs the action add with the arguments @p a in turn @p turn. */
static int
review_add (struct review            *r,
            struct review_args const *a,
            uint32_t                  turn,
            struct dstr              *out)
{
	if (!a->claim.len)
		return REFUSE(out, "add requires the claim argument.");
	enum review_state const state = a->state == REVIEW_STATES ? REVIEW_CANDIDATE : a->state;
	if (state == REVIEW_RETRACTED)
		return REFUSE(out, "The state retracted is not valid with action add. Retract a "
		              "finding with update.");
	struct review_location_view loc = a->location;
	struct oid commit;
	int e = review_check_location(r, &loc, state, true, &commit, out);
	if (e)
		return e;
	if (review_state_logged(state) && !a->evidence.len)
		return REFUSE(out, "%s requires the evidence argument. Give what you read that "
		              "shows the claim %s.", review_state_names[state].data,
		              state == REVIEW_DISPROVED ? "false" : "true");
	/* The finding and its first change are charged REVIEW_ITEM each. */
	size_t const texts = a->claim.len + a->evidence.len;
	size_t const size = r->size + review_charge(2, texts + loc.path.len);
	e = review_check_size(r, size, size - texts, out);
	if (!e)
		e = util_grow(r->findings.items, r->findings.cap, r->findings.n + 1);
	if (e)
		return e;
	struct review_finding f = review_finding(loc.first, loc.last);
	if (util_grow(f.changes.items, f.changes.cap, 1))
		return ENOMEM;
	f.changes.items[f.changes.n++] = review_change(turn, state, commit);
	e = cstr_view_store(a->claim, &f.claim);
	if (!e)
		e = cstr_view_store(loc.path, &f.location.path);
	if (!e)
		e = cstr_view_store(a->evidence, &f.changes.items[0].evidence);
	if (e) {
		review_finding_fini(&f);
		return e;
	}
	r->findings.items[r->findings.n++] = f;
	r->size = size;
	r->changed = true;
	return !dstr_printf(out, "Added finding %zu: ", r->findings.n) ||
	       review_put_status(out, &f) || PUT(out, ".") ||
	       review_put_tail(out, r, &f, a->trimmed) ? ENOMEM : 0;
}

/** Runs the action update with the arguments @p a in turn @p turn. */
static int
review_update (struct review            *r,
               struct review_args const *a,
               uint32_t                  turn,
               struct dstr              *out)
{
	if (a->state == REVIEW_STATES || !a->evidence.len)
		return REFUSE(out, "update requires the state and evidence arguments.");
	struct review_finding *f = &r->findings.items[a->id - 1];
	struct cstr_view const old_claim = cstr_view_dstr(&f->claim);
	struct cstr_view const old_path = cstr_view_dstr(&f->location.path);
	bool const in_args = a->location.path.len;
	struct review_location_view loc = in_args ? a->location : (struct review_location_view){
		.path = old_path, .first = f->location.first, .last = f->location.last,
	};
	struct oid commit;
	int e = review_check_location(r, &loc, a->state, in_args, &commit, out);
	if (e)
		return e;
	/* review_check_location() can replace a short path with a logged path.
	 * The location moves only if the arguments contain it. */
	bool const new_path = !cstr_view_equal(old_path, loc.path);
	bool const moved = in_args &&
	                   (new_path || f->location.first != loc.first || f->location.last != loc.last);
	bool const new_claim = a->claim.len && !cstr_view_equal(old_claim, a->claim);
	/* The new change is charged REVIEW_ITEM, its evidence, the old claim if
	 * the claim changes, and the old path if the location moves. The
	 * finding is charged its new claim and path instead of the old ones. */
	size_t const claim_len = new_claim ? a->claim.len : 0;
	size_t const size = r->size - old_path.len +
	                    review_charge(1, a->evidence.len + claim_len + loc.path.len +
	                                     (moved ? old_path.len : 0));
	e = review_check_size(r, size, size - a->evidence.len - claim_len, out);
	if (e)
		return e;
	if (util_grow(f->changes.items, f->changes.cap, f->changes.n + 1))
		return ENOMEM;
	struct review_change c = review_change(turn, a->state, commit);
	c.old_location = review_location(f->location.first, f->location.last);
	c.moved = moved;
	struct dstr claim = DSTR_INIT;
	struct dstr path = DSTR_INIT;
	bool const set_path = new_path || moved;
	e = cstr_view_store(a->evidence, &c.evidence);
	if (!e && new_claim)
		e = cstr_view_store(a->claim, &claim);
	if (!e && set_path)
		e = cstr_view_store(loc.path, &path);
	if (e) {
		review_change_fini(&c);
		dstr_cleanup(&claim);
		dstr_cleanup(&path);
		return e;
	}
	/* Move the replaced texts into the change. */
	if (new_claim) {
		c.old_claim = f->claim;
		f->claim = claim;
	}
	if (set_path) {
		c.old_location.path = f->location.path;
		f->location.path = path;
	}
	if (!moved)
		dstr_cleanup(&c.old_location.path);
	f->location.first = loc.first;
	f->location.last = loc.last;
	f->changes.items[f->changes.n++] = c;
	r->size = size;
	r->changed = true;
	return !dstr_printf(out, "Updated finding %" PRIu32 ": ", a->id) ||
	       review_put_status(out, f) || PUT(out, ".") ||
	       review_put_tail(out, r, f, a->trimmed) ? ENOMEM : 0;
}

#undef PUT

int
review_call (struct review      *r,
             struct json_object *args,
             uint32_t            turn,
             review_kept_fn     *kept,
             struct tool_cursor *cursor,
             size_t              room,
             struct dstr        *out)
{
	if (!r || !cursor || !out)
		return EFAULT;
	struct review_args a;
	int e = review_args_parse(&a, r, args, kept, out);
	if (e)
		return e;
	if (a.action == REVIEW_ADD)
		return review_add(r, &a, turn, out);
	if (a.action == REVIEW_UPDATE)
		return review_update(r, &a, turn, out);
	return review_put_text(out, r, a.id, cursor, room);
}

#undef REFUSE

/** Returns the file @p f as JSON, or nullptr if out of memory. */
static struct json_object *
review_file_json (struct review_file const *f)
{
	struct json_object *lines = json_object_new_array();
	for (size_t i = 0; lines && i < f->ranges.n; ++i) {
		struct review_range const *g = &f->ranges.items[i];
		lines = jb_push_or_free(lines, json_object_new_uint64(g->first));
		lines = jb_push_or_free(lines, json_object_new_uint64(g->last));
		lines = jb_push_or_free(lines, json_object_new_uint64(g->turn));
	}
	return jb_object("repo", jb_string(cstr_view_dstr(&f->repo)),
	                 "commit", jb_string(oid_view(&f->commit)),
	                 "path", jb_string(cstr_view_dstr(&f->path)),
	                 "total", json_object_new_uint64(f->total),
	                 "lines", lines,
	                 nullptr);
}

/** Returns the files of @p list as a JSON array, or nullptr if out of
 *  memory.
 */
static struct json_object *
review_files_json (struct review_files const *list)
{
	struct json_object *files = json_object_new_array();
	for (size_t i = 0; files && i < list->n; ++i)
		files = jb_push_or_free(files, review_file_json(&list->items[i]));
	return files;
}

/**
 * Sets the members first and last of @p obj to the lines of the location
 * @p loc, and the member path to its path unless the path is empty.
 * Returns @p obj, or nullptr if out of memory, and frees @p obj then.
 */
static struct json_object *
review_location_json (struct json_object           *obj,
                      struct review_location const *loc)
{
	struct cstr_view const path = cstr_view_dstr(&loc->path);
	obj = jb_set_or_free(obj, "first", json_object_new_uint64(loc->first));
	obj = jb_set_or_free(obj, "last", json_object_new_uint64(loc->last));
	return path.len ? jb_set_or_free(obj, "path", jb_string(path)) : obj;
}

/** Returns the change @p c as JSON, or nullptr if out of memory. The old
 *  location of a change that moved the finding is the object location.
 */
static struct json_object *
review_change_json (struct review_change const *c)
{
	struct cstr_view const claim = cstr_view_dstr(&c->old_claim);
	struct json_object *obj = jb_object("turn", json_object_new_uint64(c->turn),
	                                    "state", jb_string(review_state_names[c->state]),
	                                    "commit", jb_string(oid_view(&c->commit)),
	                                    "evidence", jb_string(cstr_view_dstr(&c->evidence)),
	                                    nullptr);
	if (claim.len)
		obj = jb_set_or_free(obj, "claim", jb_string(claim));
	if (c->moved)
		obj = jb_set_or_free(obj, "location", review_location_json(json_object_new_object(),
		                                                           &c->old_location));
	return obj;
}

/** Returns the finding @p f as JSON, or nullptr if out of memory. */
static struct json_object *
review_finding_json (struct review_finding const *f)
{
	struct json_object *history = json_object_new_array();
	for (size_t i = 0; history && i < f->changes.n; ++i)
		history = jb_push_or_free(history, review_change_json(&f->changes.items[i]));
	return review_location_json(jb_object("claim", jb_string(cstr_view_dstr(&f->claim)),
	                                      "history", history,
	                                      nullptr), &f->location);
}

struct json_object *
review_carry_json (struct review const *r,
                   struct oid          *base,
                   int                 *e)
{
	if (!e)
		return nullptr;
	if (!r || !base) {
		*e = EFAULT;
		return nullptr;
	}
	*base = r->base;
	struct json_object *findings = json_object_new_array();
	for (size_t i = 0; findings && i < r->findings.n; ++i)
		findings = jb_push_or_free(findings, review_finding_json(&r->findings.items[i]));
	struct json_object *obj = jb_object("findings", findings, nullptr);
	*e = obj ? 0 : ENOMEM;
	return obj;
}

struct json_object *
review_json (struct review const *r,
             int                 *e)
{
	struct oid base;
	struct json_object *obj = review_carry_json(r, &base, e);
	if (!obj)
		return nullptr;
	obj = jb_set_or_free(obj, "reads", review_files_json(&r->log));
	obj = jb_set_or_free(obj, "pending", review_files_json(&r->pending));
	obj = jb_set_or_free(obj, "full", json_object_new_uint64(r->dropped_turn));
	if (oid_len(&base))
		obj = jb_set_or_free(obj, "base", jb_string(oid_view(&base)));
	if (r->changed)
		obj = jb_set_or_free(obj, "changed", json_object_new_boolean(1));
	*e = obj ? 0 : ENOMEM;
	return obj;
}

/**
 * Parses the file @p obj of a saved list and inserts it into @p list of
 * @p r, as review_files_add() would insert it, and adds its charge to
 * r->size. The file must not be in @p list already, and its ranges must
 * fit into TOOL_TEXT_MAX. Returns 0, EINVAL or ENOMEM.
 */
static int
review_files_add_json (struct review            *r,
                       struct review_files      *list,
                       struct json_object const *obj)
{
	struct call_source key = {
		.repo   = jb_view(jb_get(obj, "repo")),
		.commit = jb_view(jb_get(obj, "commit")),
		.path   = jb_view(jb_get(obj, "path")),
	};
	uint64_t total;
	struct json_object const *lines = jb_get(obj, "lines");
	size_t const numbers = jb_len(lines);
	size_t const n = numbers / REVIEW_RANGE_NUMBERS;
	size_t at;
	if (!review_name_valid(key.repo) || !review_name_valid(key.path) ||
	    jb_uint_checked(jb_get(obj, "total"), UINT32_MAX, &total) || !n ||
	    numbers % REVIEW_RANGE_NUMBERS || n > (TOOL_TEXT_MAX - r->size) / REVIEW_ITEM ||
	    review_files_find(list, &key, &at) < list->n)
		return EINVAL;
	key.total = total;
	struct review_file f = review_file();
	int e = util_grow(f.ranges.items, f.ranges.cap, n) ? ENOMEM : review_file_set(&f, &key);
	/* The ranges must be sorted, must not overlap and must be within the
	 * file, and the turns of two adjoining ranges must differ. */
	for (uint64_t next = 1; !e && f.ranges.n < n; ++f.ranges.n) {
		uint64_t first;
		uint64_t last;
		uint64_t turn;
		size_t const k = REVIEW_RANGE_NUMBERS * f.ranges.n;
		if (jb_uint_checked(jb_at(lines, k), UINT32_MAX, &first) ||
		    jb_uint_checked(jb_at(lines, k + 1), UINT32_MAX, &last) ||
		    jb_uint_checked(jb_at(lines, k + 2), UINT32_MAX, &turn) || first < next ||
		    last < first || (key.total && last > key.total) ||
		    (f.ranges.n && first == next && turn == f.ranges.items[f.ranges.n - 1].turn)) {
			e = EINVAL;
			break;
		}
		f.ranges.items[f.ranges.n] = (struct review_range){
			(uint32_t)first, (uint32_t)last, (uint32_t)turn
		};
		next = last + 1;
	}
	if (!e)
		e = review_files_insert(list, &f, at);
	if (e) {
		review_file_fini(&f);
		return e;
	}
	r->size += review_file_charge(&f, f.ranges.n);
	return 0;
}

/**
 * Checks the pending files of @p r against its logged files. If a pending
 * file and a logged file are the same file at the same commit, their
 * totals must be equal unless one is 0, and no range of either may end
 * after a nonzero total. Returns 0 or EINVAL.
 */
static int
review_files_check_totals (struct review const *r)
{
	for (size_t i = 0; i < r->pending.n; ++i) {
		struct review_file const *p = &r->pending.items[i];
		struct call_source const key = review_file_source(p);
		size_t at;
		size_t const k = review_files_find(&r->log, &key, &at);
		if (k == r->log.n)
			continue;
		struct review_file const *f = &r->log.items[k];
		uint32_t const total = f->total > p->total ? f->total : p->total;
		if ((f->total && p->total && f->total != p->total) ||
		    (total && (f->ranges.items[f->ranges.n - 1].last > total ||
		               p->ranges.items[p->ranges.n - 1].last > total)))
			return EINVAL;
	}
	return 0;
}

/** Restores the read log, the pending lines and the first dropped turn of
 *  the saved record @p obj into @p r. Returns 0, EINVAL or ENOMEM.
 */
static int
review_restore_log (struct review            *r,
                    struct json_object const *obj)
{
	struct {
		char const          *key;
		struct review_files *list;
	} const lists[] = { { "reads", &r->log }, { "pending", &r->pending } };
	uint64_t dropped;
	int e = jb_uint_checked(jb_get(obj, "full"), UINT32_MAX, &dropped);
	if (e)
		return e;
	r->dropped_turn = (uint32_t)dropped;
	/* The restore stops once the record exceeds TOOL_TEXT_MAX. */
	for (size_t k = 0; !e && k < array_size(lists); ++k) {
		struct json_object const *a = jb_get(obj, lists[k].key);
		e = json_object_is_type(a, json_type_array) ? 0 : EINVAL;
		for (size_t i = 0, n = jb_len(a); !e && r->size <= TOOL_TEXT_MAX && i < n; ++i)
			e = review_files_add_json(r, lists[k].list, jb_at(a, i));
	}
	return e ? e : review_files_check_totals(r);
}

/** Copies the text @p s of a saved record into @p t. The text must be
 *  UTF-8, and not empty unless @p empty_ok is true. Returns 0, EINVAL or
 *  ENOMEM.
 */
static int
review_text_json (struct dstr      *t,
                  struct cstr_view  s,
                  bool              empty_ok)
{
	if (!s.data || (!empty_ok && !s.len) || jb_utf8(s.data, s.len) != s.len)
		return EINVAL;
	return cstr_view_store(s, t);
}

/**
 * Copies the location in the members path, first and last of the saved
 * object @p obj into the empty location @p loc. The lines of a location
 * without a path are 0. Returns 0, EINVAL or ENOMEM.
 */
static int
review_location_init_json (struct review_location   *loc,
                           struct json_object const *obj)
{
	struct json_object *path = jb_get(obj, "path");
	struct cstr_view const where = jb_view(path);
	uint64_t first;
	uint64_t last;
	if (jb_uint_checked(jb_get(obj, "first"), UINT32_MAX, &first) ||
	    jb_uint_checked(jb_get(obj, "last"), UINT32_MAX, &last) ||
	    !(path ? review_name_valid(where) && first && first <= last : !first && !last))
		return EINVAL;
	loc->first = (uint32_t)first;
	loc->last = (uint32_t)last;
	return cstr_view_store(where, &loc->path);
}

/**
 * Initializes the change @p c from the saved change @p obj. A change to a
 * state that requires logged lines must contain evidence and a commit, and
 * it is valid only in a finding with a location (@p located). Returns 0,
 * EINVAL or ENOMEM. On failure nothing in @p c needs to be freed.
 */
static int
review_change_init_json (struct review_change     *c,
                         struct json_object const *obj,
                         bool                      located)
{
	struct cstr_view const id = jb_view(jb_get(obj, "commit"));
	struct cstr_view const evidence = jb_view(jb_get(obj, "evidence"));
	struct json_object *claim = jb_get(obj, "claim");
	struct json_object *location = jb_get(obj, "location");
	size_t const state = cstr_view_index(review_state_names, REVIEW_STATES,
	                                     jb_view(jb_get(obj, "state")));
	struct oid commit = {};
	uint64_t turn;
	if (jb_uint_checked(jb_get(obj, "turn"), UINT32_MAX, &turn) || state == REVIEW_STATES ||
	    !id.data || (id.len && oid_init(&commit, id)) ||
	    (review_state_logged(state) && (!located || !evidence.len || !id.len)))
		return EINVAL;
	*c = review_change((uint32_t)turn, (enum review_state)state, commit);
	c->moved = location != nullptr;
	int e = review_text_json(&c->evidence, evidence, true);
	if (!e && claim)
		e = review_text_json(&c->old_claim, jb_view(claim), false);
	if (!e && location)
		e = review_location_init_json(&c->old_location, location);
	if (e)
		review_change_fini(c);
	return e;
}

/**
 * Parses the saved finding @p obj, appends it to the findings of @p r, and
 * adds its charge to r->size. Returns 0, EINVAL or ENOMEM.
 */
static int
review_findings_add_json (struct review            *r,
                          struct json_object const *obj)
{
	struct json_object const *history = jb_get(obj, "history");
	size_t const n = jb_len(history);
	if (!n || n > (TOOL_TEXT_MAX - r->size) / REVIEW_ITEM)
		return EINVAL;
	if (util_grow(r->findings.items, r->findings.cap, r->findings.n + 1))
		return ENOMEM;
	struct review_finding f = review_finding(0, 0);
	if (util_grow(f.changes.items, f.changes.cap, n))
		return ENOMEM;
	int e = review_location_init_json(&f.location, obj);
	if (!e)
		e = review_text_json(&f.claim, jb_view(jb_get(obj, "claim")), false);
	bool const located = dstr_len(&f.location.path);
	for (size_t i = 0; !e && i < n; ++i) {
		e = review_change_init_json(&f.changes.items[i], jb_at(history, i), located);
		f.changes.n += !e;
	}
	if (e) {
		review_finding_fini(&f);
		return e;
	}
	r->findings.items[r->findings.n++] = f;
	r->size += review_finding_charge(&f);
	return 0;
}

/**
 * Replaces the contents of @p r with the findings of the saved record
 * @p obj, and with its read log and pending lines if @p flags contains
 * REVIEW_READ_LOG. The function sets the base of @p r to no name and
 * changed to false. On error @p r is unchanged. Returns 0, EINVAL or
 * ENOMEM.
 */
static int
review_load (struct review            *r,
             struct json_object const *obj,
             enum review_flag          flags)
{
	struct review got = { .flags = r->flags };
	struct json_object const *findings = jb_get(obj, "findings");
	int e = json_object_is_type(findings, json_type_array) ? 0 : EINVAL;
	/* The restore stops once the record exceeds TOOL_TEXT_MAX. */
	for (size_t i = 0, n = jb_len(findings); !e && got.size <= TOOL_TEXT_MAX && i < n; ++i)
		e = review_findings_add_json(&got, jb_at(findings, i));
	if (!e && (flags & REVIEW_READ_LOG))
		e = review_restore_log(&got, obj);
	if (!e && got.size > TOOL_TEXT_MAX)
		e = EINVAL;
	if (e) {
		review_fini(&got);
		return e;
	}
	review_fini(r);
	*r = got;
	return 0;
}

int
review_restore (struct review            *r,
                struct json_object const *obj)
{
	if (!r || !obj)
		return EFAULT;
	struct json_object *saved = jb_get(obj, "base");
	struct oid base = {};
	int e = saved && oid_init(&base, jb_view(saved)) ? EINVAL : review_load(r, obj, r->flags);
	if (e)
		return e;
	r->base = base;
	r->changed = jb_true(jb_get(obj, "changed"));
	return 0;
}

int
review_carry (struct review            *r,
              struct json_object const *obj,
              struct oid const         *base)
{
	if (!r || !obj || !base)
		return EFAULT;
	int e = review_load(r, obj, 0);
	if (!e)
		r->base = *base;
	return e;
}

bool
review_changed (struct review const *r)
{
	return r && r->changed;
}
