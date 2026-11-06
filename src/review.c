/** @file review.c
 *
 * The review record of an ask call (src/review.h).
 */
#define _GNU_SOURCE /* memrchr() */

#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "dstr.h"
#include "jb.h"
#include "review.h"
#include "util.h"

struct cstr_view const review_state_names[REVIEW_STATES] = {
	[REVIEW_CANDIDATE] = CSTR_VIEW_INIT("candidate"),
	[REVIEW_VERIFIED]  = CSTR_VIEW_INIT("verified"),
	[REVIEW_DISPROVED] = CSTR_VIEW_INIT("disproved"),
	[REVIEW_RETRACTED] = CSTR_VIEW_INIT("retracted"),
};

struct cstr_view const review_action_names[REVIEW_ACTIONS] = {
	[REVIEW_ADD]    = CSTR_VIEW_INIT("add"),
	[REVIEW_UPDATE] = CSTR_VIEW_INIT("update"),
	[REVIEW_SHOW]   = CSTR_VIEW_INIT("show"),
};

enum {
	REVIEW_OID_MAX       = 64,   /* digits of a SHA-256 commit ID */
	REVIEW_OID_MIN       = 7,    /* the fewest digits of a commit prefix in a
	                                location */
	REVIEW_SHORT_OID     = 12,   /* digits of a commit ID in results and
	                                refusals */
	REVIEW_CLOSED_CLAIM  = 200,  /* the most bytes of the claim of a closed
	                                finding in the record text */
	REVIEW_RANGE_NUMBERS = 3,    /* JSON numbers of a line range: first,
	                                last and turn */
	REVIEW_KEY_SHOWN     = 64,   /* the most bytes of an unknown argument
	                                name in a refusal */
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

/** A full commit ID of 40 or 64 hexadecimal digits, or no commit ID. */
struct review_oid {
	char    hex[REVIEW_OID_MAX];
	uint8_t len;  /**< 0, 40 or 64 */
};

/** Returns the commit ID @p hex, which must be empty or a valid argument
 *  of util_is_oid().
 */
static struct review_oid
review_oid (struct cstr_view hex)
{
	struct review_oid oid = { .len = (uint8_t)hex.len };
	if (hex.len)
		memcpy(oid.hex, hex.data, hex.len);
	return oid;
}

/** Returns a view of the digits of @p oid. */
static struct cstr_view
review_oid_view (struct review_oid const *oid)
{
	return (struct cstr_view){ oid->hex, oid->len };
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
	struct review_range *ranges;
	size_t               n;
	size_t               cap;
	uint32_t             total;   /**< line count of the file, or 0 if
	                                   unknown */
	struct review_oid    commit;
};

/** The initializers of the texts of a struct review_file. */
#define REVIEW_FILE_TEXTS .repo = DSTR_INIT, .path = DSTR_INIT

/** A list of files. The files of one repository and commit are next to
 *  each other, in the order in which they were added.
 */
struct review_files {
	struct review_file *files;
	size_t              n;
	size_t              cap;
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

/** A struct review_location whose path is a view. */
struct review_location_view {
	struct cstr_view path;
	uint32_t         first;
	uint32_t         last;
};

/**
 * One change of a finding. old_claim is the claim that the change replaced,
 * or empty. If moved is true, old_location is the location that the change
 * replaced, and its path is empty if the finding had no location. commit is
 * the commit at which the lines of the new location were read, or empty.
 */
struct review_change {
	struct dstr            evidence;
	struct dstr            old_claim;
	struct review_location old_location;
	uint32_t               turn;
	enum review_state      state;
	bool                   moved;
	struct review_oid      commit;
};

/** A finding. Its state is the state of its last change. */
struct review_finding {
	struct dstr            claim;
	struct review_location location;
	struct review_change  *changes;
	size_t                 n;
	size_t                 cap;
};

struct review_read {
	struct review_source src;        /**< views of text */
	size_t               mark;       /**< an offset in the output, from
	                                      src.at on */
	uint64_t             mark_line;  /**< the number of the line that
	                                      contains the byte at mark */
	char                 text[];     /**< the repository name, the commit ID
	                                      and the path */
};

struct review {
	struct review_files    log;           /**< the read log */
	struct review_files    pending;       /**< the pending lines (review.h) */
	struct review_finding *findings;      /**< findings[i] is the finding with
	                                           the id i + 1 */
	size_t                 n_findings;
	size_t                 cap_findings;
	size_t                 size;          /**< charged bytes, pending lines
	                                           included */
	uint32_t               dropped_turn;  /**< the first turn whose lines did
	                                           not fit into the record, or 0 */
	enum review_flag       flags;
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
	for (size_t i = 0; i < f->n; ++i)
		n += review_change_charge(&f->changes[i]);
	return n;
}

static void
review_file_fini (struct review_file *f)
{
	dstr_cleanup(&f->repo);
	dstr_cleanup(&f->path);
	free(f->ranges);
	*f = (struct review_file){ REVIEW_FILE_TEXTS };
}

/** Finalizes the files of @p list and sets its count to 0. The array of
 *  files is not freed, so that the list can be filled again.
 */
static void
review_files_clear (struct review_files *list)
{
	for (size_t i = 0; i < list->n; ++i)
		review_file_fini(&list->files[i]);
	list->n = 0;
}

static void
review_files_fini (struct review_files *list)
{
	review_files_clear(list);
	free(list->files);
	*list = (struct review_files){};
}

static void
review_change_fini (struct review_change *c)
{
	dstr_cleanup(&c->evidence);
	dstr_cleanup(&c->old_claim);
	dstr_cleanup(&c->old_location.path);
	*c = (struct review_change){
		.evidence = DSTR_INIT, .old_claim = DSTR_INIT, .old_location = review_location(0, 0)
	};
}

static void
review_finding_fini (struct review_finding *f)
{
	dstr_cleanup(&f->claim);
	dstr_cleanup(&f->location.path);
	for (size_t i = 0; i < f->n; ++i)
		review_change_fini(&f->changes[i]);
	free(f->changes);
	*f = (struct review_finding){ .claim = DSTR_INIT, .location = review_location(0, 0) };
}

/** Frees the members of @p r and leaves it empty. */
static void
review_fini (struct review *r)
{
	review_files_fini(&r->log);
	review_files_fini(&r->pending);
	for (size_t i = 0; i < r->n_findings; ++i)
		review_finding_fini(&r->findings[i]);
	free(r->findings);
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

/** Returns true if @p t is 1 to REVIEW_NAME_MAX bytes of UTF-8 text without
 *  NUL, as a repository name or a path must be.
 */
static bool
review_name_valid (struct cstr_view t)
{
	return t.data && t.len && t.len <= REVIEW_NAME_MAX && jb_utf8(t.data, t.len) == t.len &&
	       !memchr(t.data, '\0', t.len);
}

/** Returns the number of newlines in @p t, whose data must not be
 *  nullptr.
 */
static size_t
review_newlines (struct cstr_view t)
{
	size_t k = 0;
	char const *p = t.data;
	char const *end = p + t.len;
	while (p < end && (p = memchr(p, '\n', (size_t)(end - p)))) {
		++k;
		++p;
	}
	return k;
}

/** Returns true if the numbered lines of @p src are not empty and are
 *  within @p out.
 */
static bool
review_source_within (struct review_source const *src,
                      struct cstr_view            out)
{
	return src->len && src->len <= out.len && src->at <= out.len - src->len;
}

struct review_read *
review_read_create (struct review_source const *src,
                    struct cstr_view            out,
                    int                        *e)
{
	if (!e)
		return nullptr;
	if (!src || (!out.data && out.len)) {
		*e = EFAULT;
		return nullptr;
	}
	*e = EINVAL;
	if (!src->commit.data || !util_is_oid(src->commit.data, src->commit.len) ||
	    !review_name_valid(src->repo) || !review_name_valid(src->path) || !src->first ||
	    src->last < src->first || (src->total && src->total < src->last) ||
	    !review_source_within(src, out))
		return nullptr;
	/* Each numbered line ends with a newline, except perhaps the last. */
	struct cstr_view const lines = { out.data + src->at, src->len };
	if (review_newlines(lines) + (lines.data[lines.len - 1] != '\n') !=
	    (uint64_t)src->last - src->first + 1)
		return nullptr;
	struct review_read *read = malloc(sizeof *read + src->repo.len + src->commit.len +
	                                  src->path.len);
	if (!read) {
		*e = ENOMEM;
		return nullptr;
	}
	*read = (struct review_read){ .src = *src, .mark = src->at, .mark_line = src->first };
	read->src.repo.data = read->text;
	read->src.commit.data = mempcpy(read->text, src->repo.data, src->repo.len);
	read->src.path.data = mempcpy((char *)read->src.commit.data, src->commit.data,
	                              src->commit.len);
	memcpy((char *)read->src.path.data, src->path.data, src->path.len);
	*e = 0;
	return read;
}

void
review_read_destroy (struct review_read **p_read)
{
	if (!p_read)
		return;
	free(*p_read);
	*p_read = nullptr;
}

struct json_object *
review_read_json (struct review_read const *read,
                  int                      *e)
{
	if (!e)
		return nullptr;
	if (!read) {
		*e = EFAULT;
		return nullptr;
	}
	struct review_source const *src = &read->src;
	struct json_object *obj = jb_object("repo", jb_string(src->repo),
	                                    "commit", jb_string(src->commit),
	                                    "path", jb_string(src->path),
	                                    "at", json_object_new_uint64(src->at),
	                                    "len", json_object_new_uint64(src->len),
	                                    "first", json_object_new_uint64(src->first),
	                                    "last", json_object_new_uint64(src->last),
	                                    "total", json_object_new_uint64(src->total),
	                                    nullptr);
	*e = obj ? 0 : ENOMEM;
	return obj;
}

struct review_read *
review_read_create_json (struct json_object const *obj,
                         struct cstr_view          out,
                         int                      *e)
{
	if (!e)
		return nullptr;
	if (!obj) {
		*e = EFAULT;
		return nullptr;
	}
	uint64_t at;
	uint64_t len;
	uint64_t first;
	uint64_t last;
	uint64_t total;
	if (jb_uint_checked(jb_get(obj, "at"), SIZE_MAX, &at) ||
	    jb_uint_checked(jb_get(obj, "len"), SIZE_MAX, &len) ||
	    jb_uint_checked(jb_get(obj, "first"), UINT32_MAX, &first) ||
	    jb_uint_checked(jb_get(obj, "last"), UINT32_MAX, &last) ||
	    jb_uint_checked(jb_get(obj, "total"), UINT32_MAX, &total)) {
		*e = EINVAL;
		return nullptr;
	}
	/* review_read_create() checks the texts. */
	struct review_source const src = {
		.repo   = jb_view(jb_get(obj, "repo")),
		.commit = jb_view(jb_get(obj, "commit")),
		.path   = jb_view(jb_get(obj, "path")),
		.at     = (size_t)at,
		.len    = (size_t)len,
		.first  = (uint32_t)first,
		.last   = (uint32_t)last,
		.total  = (uint32_t)total,
	};
	return review_read_create(&src, out, e);
}

/** Returns the repository, commit, path and total of @p f as a struct
 *  review_source with views of the texts of @p f and no lines.
 */
static struct review_source
review_file_source (struct review_file const *f)
{
	return (struct review_source){
		.repo   = cstr_view_dstr(&f->repo),
		.commit = review_oid_view(&f->commit),
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
	return cstr_view_equal(review_oid_view(&f->commit), commit) &&
	       cstr_view_equal(cstr_view_dstr(&f->repo), repo);
}

/**
 * Returns the index of the file of @p list with the repository, commit and
 * path of @p key, or list->n if there is no such file. Stores in @p *at the
 * index at which such a file is inserted: after the last file of the same
 * repository and commit, or list->n if there is none.
 */
static size_t
review_files_find (struct review_files const  *list,
                   struct review_source const *key,
                   size_t                     *at)
{
	*at = list->n;
	for (size_t i = 0; i < list->n; ++i) {
		struct review_file const *f = &list->files[i];
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
	struct review_file *files = util_room(list->files, &list->cap, list->n + 1, sizeof *files);
	if (!files)
		return ENOMEM;
	list->files = files;
	memmove(&files[at + 1], &files[at], (list->n - at) * sizeof *files);
	files[at] = *f;
	++list->n;
	return 0;
}

/** Copies the repository name, commit ID, path and total of @p key into the
 *  empty file @p f. Returns 0 or ENOMEM.
 */
static int
review_file_set (struct review_file         *f,
                 struct review_source const *key)
{
	f->commit = review_oid(key->commit);
	f->total = key->total;
	int e = cstr_view_store(key->repo, &f->repo);
	return e ? e : cstr_view_store(key->path, &f->path);
}

/** Returns true if a record of @p size bytes is within REVIEW_MAX.
 *  Otherwise sets r->dropped_turn to @p turn if it is 0, and returns false.
 */
static bool
review_within_max (struct review *r,
                   size_t         size,
                   uint32_t       turn)
{
	if (size <= REVIEW_MAX)
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
 * REVIEW_MAX, nothing changes (review_within_max()). Returns 0 or ENOMEM.
 */
static int
review_file_add_range (struct review       *r,
                       struct review_file  *f,
                       uint32_t             total,
                       struct review_range  lines)
{
	struct review_range const *p = f->ranges;
	/* The ranges before i end before line lines.first - 1, the ranges from
	 * j on start after line lines.last + 1, and the ranges from i to j - 1
	 * overlap or adjoin lines. */
	size_t i = 0;
	while (i < f->n && (uint64_t)p[i].last + 1 < lines.first)
		++i;
	size_t j = i;
	while (j < f->n && p[j].first <= (uint64_t)lines.last + 1)
		++j;
	struct review_range put[3];
	size_t const m = review_range_merge(p + i, j - i, lines, put);
	size_t const n = f->n - (j - i) + m;
	size_t const size = r->size - review_charge(f->n, 0) + review_charge(n, 0);
	if (!review_within_max(r, size, lines.turn))
		return 0;
	struct review_range *ranges = util_room(f->ranges, &f->cap, n, sizeof *ranges);
	if (!ranges)
		return ENOMEM;
	f->ranges = ranges;
	memmove(&ranges[i + m], &ranges[j], (f->n - j) * sizeof *ranges);
	memcpy(&ranges[i], put, m * sizeof *ranges);
	f->n = n;
	if (total)
		f->total = total;
	r->size = size;
	return 0;
}

/**
 * Adds the range @p lines to the file of @p list with the repository,
 * commit and path of @p key, and adds the file if @p list does not contain
 * it. A nonzero key->total replaces the total of the file. If the charge
 * of @p r would exceed REVIEW_MAX, nothing changes (review_within_max()).
 * Returns 0 or ENOMEM.
 */
static int
review_files_add (struct review              *r,
                  struct review_files        *list,
                  struct review_source const *key,
                  struct review_range         lines)
{
	size_t at;
	size_t const k = review_files_find(list, key, &at);
	if (k < list->n)
		return review_file_add_range(r, &list->files[k], key->total, lines);
	size_t const size = r->size + review_charge(2, key->repo.len + key->path.len);
	if (!review_within_max(r, size, lines.turn))
		return 0;
	struct review_file f = { REVIEW_FILE_TEXTS };
	f.ranges = util_room(nullptr, &f.cap, 1, sizeof *f.ranges);
	if (!f.ranges)
		return ENOMEM;
	f.ranges[0] = lines;
	f.n = 1;
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
review_pending_add (struct review      *r,
                    struct review_read *read,
                    struct cstr_view    out,
                    size_t              skip,
                    size_t              window,
                    uint32_t            turn)
{
	if (!r || !read || (!out.data && out.len))
		return EFAULT;
	struct review_source const *src = &read->src;
	if (!review_source_within(src, out))
		return EINVAL;
	size_t const end = src->at + src->len;
	if (skip >= end)
		return 0;
	size_t lo = skip > src->at ? skip : src->at;
	size_t const hi = window < end - skip ? skip + window : end;
	if (lo >= hi)
		return 0;
	char const *p = out.data;
	/* Count the lines from the mark of the last window, if it is before
	 * this one. */
	if (read->mark > lo) {
		read->mark = src->at;
		read->mark_line = src->first;
	}
	read->mark_line += review_newlines((struct cstr_view){ p + read->mark, lo - read->mark });
	read->mark = lo;
	uint64_t line = read->mark_line;
	/* Skip a line whose start is before the window. */
	if (lo > src->at && p[lo - 1] != '\n') {
		char const *nl = memchr(p + lo, '\n', hi - lo);
		if (!nl)
			return 0;
		lo = (size_t)(nl - p) + 1;
		++line;
	}
	/* Count the lines that end within the window. The last line of the
	 * read may end without a newline, and it counts if the window ends at
	 * the end of the read. */
	uint64_t const n = review_newlines((struct cstr_view){ p + lo, hi - lo }) +
	                   (hi == end && hi > lo && p[hi - 1] != '\n');
	if (!n)
		return 0;
	if (line + n - 1 > src->last)
		return EINVAL;
	return review_files_add(r, &r->pending, src, (struct review_range){
		(uint32_t)line, (uint32_t)(line + n - 1), turn });
}

/** Subtracts the charges of the pending files of @p r from r->size. */
static void
review_pending_uncharge (struct review *r)
{
	for (size_t i = 0; i < r->pending.n; ++i)
		r->size -= review_file_charge(&r->pending.files[i], r->pending.files[i].n);
}

/**
 * Moves the pending file @p f into the read log of @p r if the log does
 * not contain the file. Otherwise adds the ranges of @p f to the logged
 * file. The charge of @p f must already be subtracted from r->size. A file
 * that would make the charge exceed REVIEW_MAX is not logged, as in
 * review_files_add(). A moved file leaves @p f empty. Returns 0 or ENOMEM.
 */
static int
review_log_file (struct review      *r,
                 struct review_file *f)
{
	struct review_source const key = review_file_source(f);
	size_t at;
	size_t const k = review_files_find(&r->log, &key, &at);
	if (k < r->log.n) {
		int e = 0;
		for (size_t i = 0; !e && i < f->n; ++i)
			e = review_file_add_range(r, &r->log.files[k], f->total, f->ranges[i]);
		return e;
	}
	size_t const size = r->size + review_file_charge(f, f->n);
	if (!review_within_max(r, size, f->ranges[0].turn))
		return 0;
	int e = review_files_insert(&r->log, f, at);
	if (e)
		return e;
	*f = (struct review_file){ REVIEW_FILE_TEXTS };
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
		e = review_log_file(r, &r->pending.files[i]);
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

/** Appends @p t to @p out. Returns false if out of memory. */
static bool
review_put (struct dstr      *out,
            struct cstr_view  t)
{
	return dstr_cat_n(out, t.data, t.len);
}

/** Appends the string literal @p s to @p out. */
#define PUT(out, s) review_put(out, CSTR_VIEW(s))

/** Appends the line numbers @p first to @p last to @p out, as "first" or
 *  "first-last".
 */
static bool
review_put_lines (struct dstr *out,
                  uint32_t     first,
                  uint32_t     last)
{
	return dstr_printf(out, "%" PRIu32, first) &&
	       (first == last || dstr_printf(out, "-%" PRIu32, last));
}

/** Appends the location @p path, @p first and @p last to @p out, as
 *  "path:first" or "path:first-last".
 */
static bool
review_put_location (struct dstr      *out,
                     struct cstr_view  path,
                     uint32_t          first,
                     uint32_t          last)
{
	return review_put(out, path) && PUT(out, ":") && review_put_lines(out, first, last);
}

/** Appends " at " and the first REVIEW_SHORT_OID digits of @p oid to
 *  @p out.
 */
static bool
review_put_oid (struct dstr             *out,
                struct review_oid const *oid)
{
	return PUT(out, " at ") && review_put(out, (struct cstr_view){ oid->hex, REVIEW_SHORT_OID });
}

/**
 * Appends the status of the finding @p f to @p out: the state, and the
 * location if there is one. The location is followed by the commit at
 * which its lines were read, or by "not read".
 */
static bool
review_put_status (struct dstr                 *out,
                   struct review_finding const *f)
{
	struct review_change const *c = &f->changes[f->n - 1];
	struct cstr_view const path = cstr_view_dstr(&f->location.path);
	if (!review_put(out, review_state_names[c->state]))
		return false;
	if (!path.len)
		return true;
	return PUT(out, ", ") && review_put_location(out, path, f->location.first, f->location.last) &&
	       (c->commit.len ? review_put_oid(out, &c->commit) : PUT(out, ", not read"));
}

/** Returns true if the finding @p f is closed. */
static bool
review_finding_closed (struct review_finding const *f)
{
	return REVIEW_CLOSED_STATES >> f->changes[f->n - 1].state & 1U;
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
 * evidence, or a closed finding on one line. Moves @p cursor past the
 * finding, or to the next section if no finding of the section is left.
 */
static bool
review_put_finding (struct dstr          *out,
                    struct review const  *r,
                    struct review_cursor *cursor)
{
	bool const closed = cursor->section == REVIEW_SECTION_CLOSED;
	size_t k = cursor->item;
	while (k < r->n_findings && review_finding_closed(&r->findings[k]) != closed)
		++k;
	if (k == r->n_findings) {
		*cursor = (struct review_cursor){
			.section = closed ? REVIEW_SECTION_LOG : REVIEW_SECTION_CLOSED,
		};
		return true;
	}
	cursor->item = k + 1;
	struct review_finding const *f = &r->findings[k];
	struct cstr_view const claim = cstr_view_dstr(&f->claim);
	if (!dstr_printf(out, "%zu. ", k + 1) || !review_put_status(out, f))
		return false;
	if (closed) {
		/* The first line of the claim, cut to REVIEW_CLOSED_CLAIM bytes. */
		struct cstr_view const head = cstr_view_slice(claim, 0, REVIEW_CLOSED_CLAIM);
		char const *nl = memchr(head.data, '\n', head.len);
		size_t const n = nl ? (size_t)(nl - head.data)
		                    : util_clip(claim.data, claim.len, REVIEW_CLOSED_CLAIM);
		return PUT(out, ": ") && review_put(out, (struct cstr_view){ claim.data, n }) &&
		       (n == claim.len || PUT(out, "...")) && PUT(out, "\n");
	}
	struct cstr_view evidence = {};
	for (size_t i = f->n; i && !evidence.len; --i)
		evidence = cstr_view_dstr(&f->changes[i - 1].evidence);
	return PUT(out, "\n   claim: ") && review_put(out, claim) &&
	       (!evidence.len || (PUT(out, "\n   evidence: ") && review_put(out, evidence))) &&
	       PUT(out, "\n");
}

/** Appends the ranges of the file @p f to @p out, separated by ", ".
 *  Adjoining ranges are written as one.
 */
static bool
review_put_ranges (struct dstr              *out,
                   struct review_file const *f)
{
	bool ok = true;
	for (size_t k = 0; ok && k < f->n;) {
		uint32_t const first = f->ranges[k].first;
		uint32_t last = f->ranges[k].last;
		while (++k < f->n && f->ranges[k].first == (uint64_t)last + 1)
			last = f->ranges[k].last;
		ok = review_put_lines(out, first, last) && (k == f->n || PUT(out, ", "));
	}
	return ok;
}

/**
 * Appends the line of the logged file @p f to @p out: the path, the line
 * ranges, and the number of lines that they cover with the line count of
 * the file.
 */
static bool
review_put_file (struct dstr              *out,
                 struct review_file const *f)
{
	uint64_t lines = 0;
	for (size_t k = 0; k < f->n; ++k)
		lines += (uint64_t)(f->ranges[k].last - f->ranges[k].first) + 1;
	char const *const word = lines == 1 ? "line" : "lines";
	if (!PUT(out, "  ") || !review_put(out, cstr_view_dstr(&f->path)) ||
	    !dstr_printf(out, "  %s ", word) || !review_put_ranges(out, f))
		return false;
	/* The ranges are within the file and do not overlap, so they cover
	 * the whole file if lines is f->total. */
	if (lines == f->total)
		return dstr_printf(out, ": all %" PRIu64 " %s\n", lines, word);
	if (f->total)
		return dstr_printf(out, ": %" PRIu64 " of %" PRIu32 " lines\n", lines, f->total);
	return dstr_printf(out, ": %" PRIu64 " of an unknown number of lines\n", lines);
}

/**
 * Appends the line of the file @p i of the read log of @p r to @p out. The
 * header of the log comes before the first file, and a line with the
 * repository and the commit comes before the first file of each commit.
 */
static bool
review_put_logged (struct dstr         *out,
                   struct review const *r,
                   size_t               i)
{
	struct review_file const *f = &r->log.files[i];
	bool const group = !i || !review_file_commit_equal(f, cstr_view_dstr(&f[-1].repo),
	                                                    review_oid_view(&f[-1].commit));
	return (i || PUT(out, "[askai: what you read with git read in this call:]\n")) &&
	       (!group || (review_put(out, cstr_view_dstr(&f->repo)) && PUT(out, " ") &&
	                   review_put(out, review_oid_view(&f->commit)) && PUT(out, "\n"))) &&
	       review_put_file(out, f);
}

/** Appends the lines of the change @p c of a history to @p out: the turn,
 *  the state and the commit, then the evidence, the old claim and the old
 *  location.
 */
static bool
review_put_change (struct dstr                *out,
                   struct review_change const *c)
{
	struct cstr_view const evidence = cstr_view_dstr(&c->evidence);
	struct cstr_view const claim = cstr_view_dstr(&c->old_claim);
	struct cstr_view const path = cstr_view_dstr(&c->old_location.path);
	return dstr_printf(out, "turn %" PRIu32 ": ", c->turn) &&
	       review_put(out, review_state_names[c->state]) &&
	       (!c->commit.len || review_put_oid(out, &c->commit)) && PUT(out, "\n") &&
	       (!evidence.len || (PUT(out, "  evidence: ") && review_put(out, evidence) &&
	                          PUT(out, "\n"))) &&
	       (!claim.len || (PUT(out, "  the claim was: ") && review_put(out, claim) &&
	                       PUT(out, "\n"))) &&
	       (!c->moved || (PUT(out, "  the location was: ") &&
	                      (path.len ? review_put_location(out, path, c->old_location.first,
	                                                      c->old_location.last)
	                                : PUT(out, "none")) &&
	                      PUT(out, "\n")));
}

/**
 * Appends the item at @p cursor to @p out and moves @p cursor to the next
 * item. The items are the lines of the text of the record @p r, or of the
 * empty record if @p r is nullptr, or of the history of the finding @p id
 * of @p r if @p id is not 0. A section without items is passed without
 * writing anything.
 */
static bool
review_put_item (struct dstr          *out,
                 struct review const  *r,
                 size_t                id,
                 struct review_cursor *cursor)
{
	static struct cstr_view const heads[] = {
		CSTR_VIEW_INIT("[askai: your review record, which askai keeps through every "
		               "compaction of this ask call.]\n[askai: you have recorded no findings; "
		               "the findings tool records them.]\n"),
		CSTR_VIEW_INIT("[askai: your review record, which askai keeps through every "
		               "compaction of this ask call.]\n[askai: your findings; findings action "
		               "show with an id gives one's history:]\n"),
	};
	struct review_finding const *f = id ? &r->findings[id - 1] : nullptr;
	switch (cursor->section) {
	case REVIEW_SECTION_HEAD:
		if (f) {
			*cursor = (struct review_cursor){ .section = REVIEW_SECTION_CHANGES };
			return dstr_printf(out, "Finding %zu: ", id) && review_put_status(out, f) &&
			       PUT(out, "\nclaim: ") && review_put(out, cstr_view_dstr(&f->claim)) &&
			       PUT(out, "\n");
		}
		bool const any = r && r->n_findings;
		*cursor = (struct review_cursor){
			.section = any ? REVIEW_SECTION_OPEN : REVIEW_SECTION_LOG,
		};
		return review_put(out, heads[any]);
	case REVIEW_SECTION_OPEN:
	case REVIEW_SECTION_CLOSED:
		return review_put_finding(out, r, cursor);
	case REVIEW_SECTION_LOG:
		if (r && cursor->item < r->log.n)
			return review_put_logged(out, r, cursor->item++);
		*cursor = (struct review_cursor){ .section = REVIEW_SECTION_DROPPED };
		return true;
	case REVIEW_SECTION_DROPPED:
		cursor->section = REVIEW_SECTION_END;
		return !r || !r->dropped_turn ||
		       dstr_printf(out, "[askai: the read log is full, so what you read from turn %"
		                   PRIu32 " on may be missing from it.]\n", r->dropped_turn);
	case REVIEW_SECTION_CHANGES:
		if (cursor->item < f->n)
			return review_put_change(out, &f->changes[cursor->item++]);
		cursor->section = REVIEW_SECTION_END;
		return true;
	default:
		return true;
	}
}

/**
 * Appends the text from @p cursor on to @p out (review_put_item()), at
 * most @p room bytes, and moves @p cursor to the end of what it appended.
 * An item that does not fit is cut, and @p cursor stores the bytes of the
 * item that are written. Returns 0 at the end of the text, EAGAIN if the
 * text continues, or ENOMEM.
 */
static int
review_put_text (struct dstr          *out,
                 struct review const  *r,
                 size_t                id,
                 struct review_cursor *cursor,
                 size_t                room)
{
	size_t const start = dstr_len(out);
	dstr_auto item = DSTR_INIT;
	while (cursor->section != REVIEW_SECTION_END) {
		struct review_cursor const at = *cursor;
		size_t const before = dstr_len(out);
		/* An item that an earlier part cut is written again into item, and
		 * only its rest is appended. */
		bool ok;
		if (at.skip) {
			dstr_clear(&item);
			ok = review_put_item(&item, r, id, cursor) &&
			     review_put(out, cstr_view_slice(cstr_view_dstr(&item), at.skip, SIZE_MAX));
		} else {
			ok = review_put_item(out, r, id, cursor);
		}
		if (!ok)
			return ENOMEM;
		if (dstr_len(out) - start > room) {
			dstr_erase(out, start + room, SIZE_MAX);
			*cursor = at;
			cursor->skip += start + room - before;
			return EAGAIN;
		}
		cursor->skip = 0;
	}
	return 0;
}

int
review_render (struct review const *r,
               struct dstr         *out,
               size_t               most)
{
	if (!out)
		return EFAULT;
	/* Every line of the text ends with a newline, and the last newline is
	 * dropped, so the text fits if its length is at most (most + 1). */
	size_t const start = dstr_len(out);
	struct review_cursor cursor = {};
	int const e = review_put_text(out, r, 0, &cursor, most < SIZE_MAX ? most + 1 : most);
	if (e == ENOMEM)
		return e;
	struct cstr_view const text = cstr_view_slice(cstr_view_dstr(out), start, SIZE_MAX);
	if (!e) {
		dstr_erase(out, start + text.len - 1, 1);
		return 0;
	}
	/* The text stopped at most + 1 bytes. Cut it after its last newline
	 * within most bytes. The text is not empty, and the test of text.len
	 * only shows that to the compiler. */
	char const *nl = text.len ? memrchr(text.data, '\n', text.len - 1) : nullptr;
	size_t const keep = nl ? (size_t)(nl - text.data) + 1 : 0;
	dstr_erase(out, start + keep, SIZE_MAX);
	return dstr_printf(out, "[askai: the record goes on past these %zu bytes: call findings "
	                   "with action show and \"offset\": %zu for the rest.]", keep, keep)
	       ? 0 : ENOMEM;
}

/** Appends the names @p names[i] whose bit i is set in @p mask to @p out,
 *  separated by ", ", with @p last before the final name: "a, b or c".
 */
static bool
review_put_names (struct dstr            *out,
                  struct cstr_view const *names,
                  size_t                  n,
                  uint32_t                mask,
                  struct cstr_view        last)
{
	bool ok = true;
	mask &= (1U << n) - 1;
	for (size_t i = 0; ok && i < n; ++i) {
		if (!(mask >> i & 1U))
			continue;
		mask &= ~(1U << i);
		ok = review_put(out, names[i]) &&
		     (!mask || review_put(out, mask & (mask - 1) ? CSTR_VIEW(", ") : last));
	}
	return ok;
}

/* Appends a refusal to @p out with dstr_printf(). Evaluates to EINVAL, or
 * to ENOMEM if the append fails.
 */
#define REFUSE(out, ...) (dstr_printf(out, __VA_ARGS__) ? EINVAL : ENOMEM)

/** Appends @p head, the names @p names[i] whose bit i is set in @p mask as
 *  "a, b or c", and @p tail to @p out. Returns EINVAL, or ENOMEM if the
 *  append fails.
 */
static int
review_refuse_names (struct dstr            *out,
                     struct cstr_view        head,
                     struct cstr_view const *names,
                     size_t                  n,
                     uint32_t                mask,
                     struct cstr_view        tail)
{
	return review_put(out, head) && review_put_names(out, names, n, mask, CSTR_VIEW(" or ")) &&
	       review_put(out, tail) ? EINVAL : ENOMEM;
}

/** The arguments of the findings tool. */
enum review_arg : uint8_t {
	REVIEW_ARG_ACTION,
	REVIEW_ARG_ID,
	REVIEW_ARG_CLAIM,
	REVIEW_ARG_LOCATION,
	REVIEW_ARG_STATE,
	REVIEW_ARG_EVIDENCE,
	REVIEW_ARG_OFFSET,
	REVIEW_ARGS
};

static struct cstr_view const review_arg_names[REVIEW_ARGS] = {
	[REVIEW_ARG_ACTION]   = CSTR_VIEW_INIT("action"),
	[REVIEW_ARG_ID]       = CSTR_VIEW_INIT("id"),
	[REVIEW_ARG_CLAIM]    = CSTR_VIEW_INIT("claim"),
	[REVIEW_ARG_LOCATION] = CSTR_VIEW_INIT("location"),
	[REVIEW_ARG_STATE]    = CSTR_VIEW_INIT("state"),
	[REVIEW_ARG_EVIDENCE] = CSTR_VIEW_INIT("evidence"),
	[REVIEW_ARG_OFFSET]   = CSTR_VIEW_INIT("offset"),
};

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
	struct cstr_view            prefix;    /**< the commit prefix of the
	                                            location, or empty */
	struct review_location_view location;  /**< location.path is empty if
	                                            there is no location */
	size_t                      id;        /**< 0 if there is none */
	enum review_action          action;
	enum review_state           state;     /**< REVIEW_STATES if there is
	                                            none */
	uint8_t                     trimmed;   /**< enum review_trimmed bits of
	                                            the texts that @p kept
	                                            trimmed */
};

/**
 * Parses the text @p t of the form "path:first" or "path:first-last" into
 * @p *loc. The path is the text before the last colon, and the line
 * numbers must satisfy 1 <= first <= last. The caller checks the path.
 * Returns 0 or EINVAL.
 */
static int
review_location_parse (struct review_location_view *loc,
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
	*loc = (struct review_location_view){
		{ t.data, (size_t)(colon - t.data) }, (uint32_t)first, (uint32_t)last,
	};
	return 0;
}

/**
 * Parses the location argument @p t, of the form [commit:]path:first[-last],
 * into a->location and a->prefix. A commit is REVIEW_OID_MIN to
 * REVIEW_OID_MAX hexadecimal digits, as git grep prints it. A leading
 * "HEAD:" means no commit. Returns 0 or EINVAL.
 */
static int
review_args_location (struct review_args *a,
                      struct cstr_view    t)
{
	struct review_location_view *loc = &a->location;
	if (review_location_parse(loc, t))
		return EINVAL;
	struct cstr_view const path = loc->path;
	size_t hex = 0;
	while (hex < path.len && hex <= REVIEW_OID_MAX && util_is_hex(path.data + hex, 1))
		++hex;
	size_t skip = 0;
	if (hex >= REVIEW_OID_MIN && hex <= REVIEW_OID_MAX && hex < path.len &&
	    path.data[hex] == ':') {
		a->prefix = (struct cstr_view){ path.data, hex };
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
		return REFUSE(out, "The %.*s must be a string of UTF-8 text, not empty.", len,
		              name.data);
	size_t const n = kept ? kept(s.data, s.len) : s.len;
	*t = s;
	if (n >= s.len)
		return 0;
	/* Also drop the spaces and line ends before the notices. */
	*t = cstr_view_trim_end((struct cstr_view){ s.data, n });
	if (!t->len)
		return REFUSE(out, "The %.*s has only askai's [askai: ...] lines, which askai adds "
		              "to tool results. Give the %.*s itself.", len, name.data, len,
		              name.data);
	a->trimmed |= bit;
	return 0;
}

/**
 * Parses the arguments @p args of a findings call into @p a. Each argument
 * must be one that the action accepts. The id must be the id of a finding
 * of @p r. Returns 0, or EINVAL with a refusal appended to @p out, or
 * ENOMEM if the append fails.
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
		return review_refuse_names(out, CSTR_VIEW("The action argument must be "),
		                           review_action_names, REVIEW_ACTIONS, ~0U, CSTR_VIEW("."));
	/* args is an object, because it contains the action. Find the first
	 * argument that the action does not accept. */
	uint32_t const accepted = review_action_args[a->action];
	struct json_object *of[REVIEW_ARGS] = {};
	struct cstr_view other = {};
	struct json_object_iter it;
	json_object_object_foreachC(args, it) {
		size_t const k = cstr_view_index(review_arg_names, REVIEW_ARGS, cstr_view(it.key));
		if (k < REVIEW_ARGS && accepted >> k & 1U) {
			of[k] = it.val;
			continue;
		}
		if (!other.data)
			other = (struct cstr_view){
				it.key, jb_utf8(it.key, strnlen(it.key, REVIEW_KEY_SHOWN))
			};
	}
	if (other.data)
		return review_put(out, review_action_names[a->action]) && PUT(out, " takes ") &&
		       review_put_names(out, review_arg_names + 1, REVIEW_ARGS - 1, accepted >> 1,
		                        CSTR_VIEW(" and ")) &&
		       dstr_printf(out, ", not %.*s.", (int)other.len, other.data) ? EINVAL : ENOMEM;
	struct json_object *id = of[REVIEW_ARG_ID];
	uint64_t n = 0;
	if (id && (jb_uint_checked(id, SIZE_MAX, &n) || !n))
		return REFUSE(out, "The id argument must be the number of a finding, such as 3.");
	if (n > r->n_findings) {
		/* %.0zu writes nothing for 0. */
		static char const *const have[] = {
			"no findings yet", "only finding ", "findings 1 to ",
		};
		size_t const k = r->n_findings;
		return REFUSE(out, "There is no finding %" PRIu64 "; the record has %s%.0zu.", n,
		              have[k < 2 ? k : 2], k);
	}
	a->id = (size_t)n;
	struct json_object *state = of[REVIEW_ARG_STATE];
	if (state) {
		a->state = (enum review_state)cstr_view_index(review_state_names, REVIEW_STATES,
		                                               jb_view(state));
		if (a->state == REVIEW_STATES)
			return review_refuse_names(out, CSTR_VIEW("The state argument must be "),
			                           review_state_names, REVIEW_STATES, ~0U,
			                           CSTR_VIEW("."));
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
	if (!e && review_args_location(a, text))
		e = REFUSE(out, "The location must be path:line or path:first-last, such as "
		           "src/mcp.c:120-140.");
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
	size_t hi = f->n;
	while (lo < hi) {
		size_t const mid = lo + (hi - lo) / 2;
		if (f->ranges[mid].first <= a)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo || f->ranges[lo - 1].last < a)
		return 0;
	struct review_range const *g = &f->ranges[lo - 1];
	struct review_range const *end = f->ranges + f->n;
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
};

/**
 * Selects the files of the read log of @p r with the path of @p loc and a
 * commit that starts with @p prefix. The best file contains all lines of
 * @p loc in one run of adjoining ranges, and among those files the file
 * whose oldest read of the lines is newest is best. Otherwise the best
 * file contains the longest run from loc->first, and files with a total
 * of 0 (unknown) or at least loc->last rank before the others.
 */
static struct review_cover
review_log_cover (struct review const               *r,
                  struct review_location_view const *loc,
                  struct cstr_view                   prefix)
{
	struct review_cover c = {};
	enum review_rank rank = REVIEW_RANK_NONE;
	uint32_t latest = 0;
	for (size_t i = 0; i < r->log.n; ++i) {
		struct review_file const *f = &r->log.files[i];
		if (!cstr_view_equal(cstr_view_dstr(&f->path), loc->path) ||
		    !cstr_view_starts(review_oid_view(&f->commit), prefix))
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
		if (k < rank || (k == rank && (k == REVIEW_RANK_ALL ? turn < latest : end < c.end)))
			continue;
		c = (struct review_cover){ f, c.largest, end };
		rank = k;
		latest = turn;
	}
	return c;
}

/**
 * Resolves loc->path against the paths of the read log of @p r whose
 * commit starts with @p prefix. If a logged path equals loc->path, stores
 * loc->path in found[0] and returns 1. Otherwise the matches are the
 * logged paths that end with '/' and loc->path. Stores up to @p max
 * distinct matches in @p found and returns their number.
 */
static size_t
review_log_resolve (struct review const               *r,
                    struct review_location_view const *loc,
                    struct cstr_view                   prefix,
                    struct cstr_view                  *found,
                    size_t                             max)
{
	size_t n = 0;
	size_t const tail = loc->path.len;
	for (size_t i = 0; i < r->log.n; ++i) {
		struct review_file const *f = &r->log.files[i];
		if (!cstr_view_starts(review_oid_view(&f->commit), prefix))
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
 * Appends to @p out the refusal of the state @p state for the location
 * @p loc, whose lines are not all in the read log of @p r, with the read
 * that adds them. @p c is the cover of @p loc (review_log_cover()), and
 * @p resolved is the number of logged paths that loc->path resolved to.
 * Returns EINVAL, or ENOMEM if the append fails.
 */
static int
review_refuse_unread (struct review const               *r,
                      struct review_location_view const *loc,
                      struct cstr_view                   prefix,
                      enum review_state                  state,
                      struct review_cover const         *c,
                      size_t                             resolved,
                      struct dstr                       *out)
{
	char const *name = review_state_names[state].data;
	int const len = (int)loc->path.len;
	char const *path = loc->path.data;
	struct review_file const *largest = c->largest;
	if (largest && largest->total && largest->total < loc->last)
		return REFUSE(out, "%.*s has %" PRIu32 " line%s at %.*s, so line %" PRIu32 " is "
		              "past its end; give a location within lines 1 to %" PRIu32 ".", len,
		              path, largest->total, largest->total == 1 ? "" : "s", REVIEW_SHORT_OID,
		              largest->commit.hex, loc->last, largest->total);
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
	bool const one = loc->first == loc->last;
	bool ok = dstr_printf(out, "%s needs the lines of its location in your read log, and %.*s",
	                      name, len, path) &&
	          (!prefix.len || dstr_printf(out, " at %.*s", (int)prefix.len, prefix.data)) &&
	          dstr_printf(out, " line%s ", one ? "" : "s") &&
	          review_put_lines(out, loc->first, loc->last);
	if (!c->best)
		ok = ok && dstr_printf(out, " %s not in it.", one ? "is" : "are");
	else
		ok = ok && dstr_printf(out, " are not all in it: it has line%s ",
		                       c->end > loc->first ? "s" : "") &&
		     review_put_lines(out, loc->first, c->end) &&
		     dstr_printf(out, " of them, read at %.*s", REVIEW_SHORT_OID,
		                 c->best->commit.hex) &&
		     (!shorter || dstr_printf(out, ", where the file has %" PRIu32 " line%s",
		                              c->best->total, c->best->total == 1 ? "" : "s")) &&
		     PUT(out, ".");
	/* The read is logged as a new pending file after a later turn. Its
	 * charge is estimated from the file to read, or from a file with this
	 * path and a 1-byte repository name. */
	size_t const charge = to ? review_file_charge(to, 1) : review_charge(2, 1 + loc->path.len);
	if (r->size + charge > REVIEW_MAX) {
		if (r->dropped_turn)
			ok = ok && dstr_printf(out, " The read log has been full since turn %"
			                       PRIu32 ",", r->dropped_turn);
		else
			ok = ok && PUT(out, " The record is full,");
		return ok && PUT(out, " so a read of them would not be logged: cite lines that the "
		                      "log has, or keep the finding a candidate.") ? EINVAL : ENOMEM;
	}
	static char const *const hints[] = {
		", where the path is the file's whole path from the top of the repository", "",
		", in as many calls as it takes",
	};
	char escaped[6 * REVIEW_NAME_MAX];
	size_t const escaped_len = jb_escape(escaped, path, loc->path.len);
	return ok && dstr_printf(out, " Read %s with the git tool's read command, with "
	                         "\"revisions\": [\"%.*s:%.*s\"] and \"line\": %" PRIu32 "%s. "
	                         "The log takes the lines of a read once you have seen them, so "
	                         "call again in a later turn.", rest ? "the rest" : "them",
	                         (int)rev.len, rev.data, (int)escaped_len, escaped,
	                         rest ? c->end + 1 : loc->first,
	                         hints[to ? 2 : resolved != 0]) ? EINVAL : ENOMEM;
}

/**
 * Checks the location @p loc of a finding in the state @p state against
 * the read log of @p r, and stores in @p *logged the logged file that
 * contains all lines of @p loc, or nullptr. A short path that resolves to
 * one logged path (review_log_resolve()) is replaced by that path. A
 * state that requires logged lines is refused for a record without
 * REVIEW_READ_LOG, without a location, or for lines that the log does not
 * contain. A short path that resolves to several logged paths is refused
 * if the arguments contain the location (@p in_args) or the state
 * requires logged lines. Returns 0, or EINVAL with a refusal appended to
 * @p out, or ENOMEM if the append fails.
 */
static int
review_check_location (struct review const          *r,
                       struct review_location_view  *loc,
                       struct cstr_view              prefix,
                       enum review_state             state,
                       bool                          in_args,
                       struct review_file const    **logged,
                       struct dstr                  *out)
{
	*logged = nullptr;
	bool const requires_log = review_state_logged(state);
	if (requires_log && !(r->flags & REVIEW_READ_LOG))
		return review_refuse_names(out, CSTR_VIEW("This ask call has no git tool, so a "
		                                          "finding cannot be "),
		                           review_state_names, REVIEW_STATES, REVIEW_LOGGED_STATES,
		                           CSTR_VIEW(" here; keep it a candidate, or retract it."));
	if (!loc->path.len)
		return !requires_log ? 0
		     : REFUSE(out, "%s needs a location: the lines that show it, such as "
		              "src/mcp.c:120-140, which you read with git read.",
		              review_state_names[state].data);
	struct cstr_view found[4];
	size_t const n = review_log_resolve(r, loc, prefix, found, array_size(found));
	if (n == 1)
		loc->path = found[0];
	if (n > 1 && (in_args || requires_log)) {
		/* List up to three paths, then "and others". */
		size_t const shown = n < array_size(found) ? n : array_size(found) - 1;
		bool ok = dstr_printf(out, "%.*s matches ", (int)loc->path.len, loc->path.data);
		for (size_t i = 0; ok && i < shown; ++i)
			ok = (!i || review_put(out, i + 1 < n ? CSTR_VIEW(", ")
			                                      : CSTR_VIEW(" and "))) &&
			     review_put(out, found[i]);
		return ok && (n == shown || PUT(out, " and others")) &&
		       PUT(out, " in your read log; give \"location\" with the whole path.")
		       ? EINVAL : ENOMEM;
	}
	struct review_cover const c = review_log_cover(r, loc, prefix);
	if (c.best && c.end >= loc->last)
		*logged = c.best;
	if (*logged || !requires_log)
		return 0;
	return review_refuse_unread(r, loc, prefix, state, &c, n, out);
}

/**
 * Returns 0 if a record of @p size bytes is within REVIEW_MAX. Otherwise
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
	int const mib = REVIEW_MAX >> 20;
	if (size <= REVIEW_MAX)
		return 0;
	if (fixed < REVIEW_MAX)
		return REFUSE(out, "The record is full: it holds at most %d MiB. Give a claim and "
		              "evidence of at most %zu bytes together.", mib, REVIEW_MAX - fixed);
	/* The smallest change is an update with 1 byte of evidence, charged
	 * REVIEW_ITEM + 1 bytes. Pending lines can make room: a compaction
	 * drops them, and the log may charge less for them. Without pending
	 * lines no room becomes free, and a new read cannot become pending,
	 * because a read is charged at least 2 * REVIEW_ITEM + 2 bytes. */
	return REFUSE(out, "The record is full: it holds at most %d MiB and %s; keep further "
	              "findings in your notes or your summary.", mib,
	              r->pending.n || r->size + REVIEW_ITEM < REVIEW_MAX
	              ? "has no room for this change" : "takes no more changes in this ask call");
}

/**
 * Appends the end of an add or update result for the finding @p f of @p r
 * to @p out. The end requests a git read if @p f is a candidate whose
 * lines were not read and the flags of @p r contain REVIEW_READ_LOG. If
 * @p trimmed is not 0, the end lists the texts whose trailing notices were
 * removed.
 */
static bool
review_put_tail (struct dstr                 *out,
                 struct review const         *r,
                 struct review_finding const *f,
                 uint8_t                      trimmed)
{
	static struct cstr_view const texts[] = {
		[REVIEW_TRIMMED_CLAIM]    = CSTR_VIEW_INIT("the claim"),
		[REVIEW_TRIMMED_EVIDENCE] = CSTR_VIEW_INIT("the evidence"),
		[REVIEW_TRIMMED_CLAIM | REVIEW_TRIMMED_EVIDENCE] =
			CSTR_VIEW_INIT("the claim and the evidence"),
	};
	struct review_change const *c = &f->changes[f->n - 1];
	bool const unread = c->state == REVIEW_CANDIDATE && dstr_len(&f->location.path) &&
	                    !c->commit.len && (r->flags & REVIEW_READ_LOG);
	return (!unread || PUT(out, " Read its lines with git read before you verify it.")) &&
	       (!trimmed ||
	        (PUT(out, " askai left out the [askai: ...] lines at the end of ") &&
	         review_put(out, texts[trimmed]) &&
	         PUT(out, ". askai adds those lines to tool results, and they are not part of the "
	                  "finding.")));
}

/** Makes room for one more finding in @p r. Returns 0 or ENOMEM. */
static int
review_findings_reserve (struct review *r)
{
	struct review_finding *findings = util_room(r->findings, &r->cap_findings,
	                                            r->n_findings + 1, sizeof *findings);
	if (!findings)
		return ENOMEM;
	r->findings = findings;
	return 0;
}

/** Runs the action add with the arguments @p a in turn @p turn. */
static int
review_add (struct review            *r,
            struct review_args const *a,
            uint32_t                  turn,
            struct dstr              *out)
{
	if (!a->claim.len)
		return REFUSE(out, "add needs a claim: what you found.");
	enum review_state const state = a->state == REVIEW_STATES ? REVIEW_CANDIDATE : a->state;
	if (state == REVIEW_RETRACTED)
		return review_refuse_names(out, CSTR_VIEW("add takes the state "), review_state_names,
		                           REVIEW_STATES, ~(1U << REVIEW_RETRACTED),
		                           CSTR_VIEW("; retract a finding with update."));
	struct review_location_view loc = a->location;
	struct review_file const *logged;
	int e = review_check_location(r, &loc, a->prefix, state, true, &logged, out);
	if (e)
		return e;
	if (review_state_logged(state) && !a->evidence.len)
		return REFUSE(out, "%s needs evidence: what you read that shows it%s.",
		              review_state_names[state].data,
		              state == REVIEW_DISPROVED ? " false" : "");
	/* The finding and its first change are charged REVIEW_ITEM each. */
	size_t const texts = a->claim.len + a->evidence.len;
	size_t const size = r->size + review_charge(2, texts + loc.path.len);
	e = review_check_size(r, size, size - texts, out);
	if (!e)
		e = review_findings_reserve(r);
	if (e)
		return e;
	struct review_finding f = {
		.claim    = DSTR_INIT,
		.location = review_location(loc.first, loc.last),
		.n        = 1,
	};
	f.changes = util_room(nullptr, &f.cap, 1, sizeof *f.changes);
	if (!f.changes)
		return ENOMEM;
	f.changes[0] = (struct review_change){
		.evidence     = DSTR_INIT,
		.old_claim    = DSTR_INIT,
		.old_location = review_location(0, 0),
		.turn   = turn,
		.state  = state,
		.commit = logged ? logged->commit : (struct review_oid){},
	};
	e = cstr_view_store(a->claim, &f.claim);
	if (!e)
		e = cstr_view_store(loc.path, &f.location.path);
	if (!e)
		e = cstr_view_store(a->evidence, &f.changes[0].evidence);
	if (e) {
		review_finding_fini(&f);
		return e;
	}
	r->findings[r->n_findings++] = f;
	r->size = size;
	return dstr_printf(out, "Added finding %zu: ", r->n_findings) &&
	       review_put_status(out, &f) && PUT(out, ".") &&
	       review_put_tail(out, r, &f, a->trimmed) ? 0 : ENOMEM;
}

/** Runs the action update with the arguments @p a in turn @p turn. */
static int
review_update (struct review            *r,
               struct review_args const *a,
               uint32_t                  turn,
               struct dstr              *out)
{
	if (!a->id)
		return REFUSE(out, "The id argument must be the number of a finding, such as 3.");
	if (a->state == REVIEW_STATES || !a->evidence.len)
		return REFUSE(out, "update needs state and evidence: what shows the new state, or why "
		              "you retract the finding.");
	struct review_finding *f = &r->findings[a->id - 1];
	struct cstr_view const old_claim = cstr_view_dstr(&f->claim);
	struct cstr_view const old_path = cstr_view_dstr(&f->location.path);
	bool const in_args = a->location.path.len;
	struct review_location_view loc = in_args ? a->location : (struct review_location_view){
		old_path, f->location.first, f->location.last,
	};
	struct review_file const *logged;
	int e = review_check_location(r, &loc, a->prefix, a->state, in_args, &logged, out);
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
	struct review_change *changes = util_room(f->changes, &f->cap, f->n + 1, sizeof *changes);
	if (!changes)
		return ENOMEM;
	f->changes = changes;
	struct review_change c = {
		.evidence     = DSTR_INIT,
		.old_claim    = DSTR_INIT,
		.old_location = review_location(f->location.first, f->location.last),
		.turn   = turn,
		.state  = a->state,
		.moved  = moved,
		.commit = logged ? logged->commit : (struct review_oid){},
	};
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
	enum review_state const old = f->changes[f->n - 1].state;
	f->location.first = loc.first;
	f->location.last = loc.last;
	f->changes[f->n++] = c;
	r->size = size;
	static char const *const verbs[] = { "is now", "stays" };
	static char const *const history[] = {
		"", " Its old claim is in its history.", " Its old location is in its history.",
		" Its old claim and location are in its history.",
	};
	bool const same = old == a->state;
	bool ok = dstr_printf(out, "Finding %zu %s ", a->id, verbs[same]) &&
	          review_put_status(out, f);
	if (same)
		ok = ok && PUT(out, "; the new evidence is in its history.");
	else
		ok = ok && dstr_printf(out, "; it was %s.", review_state_names[old].data);
	return ok && dstr_printf(out, "%s", history[new_claim | moved << 1]) &&
	       review_put_tail(out, r, f, a->trimmed) ? 0 : ENOMEM;
}

#undef PUT

int
review_call (struct review        *r,
             struct json_object   *args,
             uint32_t              turn,
             review_kept_fn       *kept,
             struct review_cursor *cursor,
             size_t                room,
             struct dstr          *out)
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
	/* A show drops the newline at the end of its last line. */
	e = review_put_text(out, r, a.id, cursor, room);
	if (!e)
		dstr_erase(out, dstr_len(out) - 1, 1);
	return e;
}

#undef REFUSE

/** Returns the file @p f as JSON, or nullptr if out of memory. */
static struct json_object *
review_file_json (struct review_file const *f)
{
	struct json_object *lines = json_object_new_array();
	for (size_t i = 0; lines && i < f->n; ++i) {
		struct review_range const *g = &f->ranges[i];
		lines = jb_push_or_free(lines, json_object_new_uint64(g->first));
		lines = jb_push_or_free(lines, json_object_new_uint64(g->last));
		lines = jb_push_or_free(lines, json_object_new_uint64(g->turn));
	}
	return jb_object("repo", jb_string(cstr_view_dstr(&f->repo)),
	                 "commit", jb_string(review_oid_view(&f->commit)),
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
		files = jb_push_or_free(files, review_file_json(&list->files[i]));
	return files;
}

/** Returns the location @p loc as a JSON string "path:first-last", or ""
 *  for a location without a path, or nullptr if out of memory.
 */
static struct json_object *
review_location_json (struct review_location const *loc)
{
	struct cstr_view const path = cstr_view_dstr(&loc->path);
	dstr_auto text = DSTR_INIT;
	if (path.len && !review_put_location(&text, path, loc->first, loc->last))
		return nullptr;
	return jb_string(cstr_view_dstr(&text));
}

/** Returns the change @p c as JSON, or nullptr if out of memory. */
static struct json_object *
review_change_json (struct review_change const *c)
{
	struct cstr_view const claim = cstr_view_dstr(&c->old_claim);
	struct json_object *obj = jb_object("turn", json_object_new_uint64(c->turn),
	                                    "state", jb_string(review_state_names[c->state]),
	                                    "commit", jb_string(review_oid_view(&c->commit)),
	                                    "evidence", jb_string(cstr_view_dstr(&c->evidence)),
	                                    nullptr);
	if (claim.len)
		obj = jb_set_or_free(obj, "claim", jb_string(claim));
	if (c->moved)
		obj = jb_set_or_free(obj, "location", review_location_json(&c->old_location));
	return obj;
}

/** Returns the finding @p f as JSON, or nullptr if out of memory. */
static struct json_object *
review_finding_json (struct review_finding const *f)
{
	struct cstr_view const path = cstr_view_dstr(&f->location.path);
	struct json_object *history = json_object_new_array();
	for (size_t i = 0; history && i < f->n; ++i)
		history = jb_push_or_free(history, review_change_json(&f->changes[i]));
	struct json_object *obj = jb_object("claim", jb_string(cstr_view_dstr(&f->claim)),
	                                    "first", json_object_new_uint64(f->location.first),
	                                    "last", json_object_new_uint64(f->location.last),
	                                    "history", history,
	                                    nullptr);
	return path.len ? jb_set_or_free(obj, "path", jb_string(path)) : obj;
}

struct json_object *
review_json (struct review const *r,
             int                 *e)
{
	if (!e)
		return nullptr;
	if (!r) {
		*e = EFAULT;
		return nullptr;
	}
	struct json_object *findings = json_object_new_array();
	for (size_t i = 0; findings && i < r->n_findings; ++i)
		findings = jb_push_or_free(findings, review_finding_json(&r->findings[i]));
	struct json_object *obj = jb_object("reads", review_files_json(&r->log),
	                                    "pending", review_files_json(&r->pending),
	                                    "full", json_object_new_uint64(r->dropped_turn),
	                                    "findings", findings,
	                                    nullptr);
	*e = obj ? 0 : ENOMEM;
	return obj;
}

/**
 * Parses the file @p obj of a saved list and inserts it into @p list of
 * @p r, as review_files_add() would insert it, and adds its charge to
 * r->size. The file must not be in @p list already, and its ranges must
 * fit into REVIEW_MAX. Returns 0, EINVAL or ENOMEM.
 */
static int
review_files_add_json (struct review            *r,
                       struct review_files      *list,
                       struct json_object const *obj)
{
	struct review_source key = {
		.repo   = jb_view(jb_get(obj, "repo")),
		.commit = jb_view(jb_get(obj, "commit")),
		.path   = jb_view(jb_get(obj, "path")),
	};
	uint64_t total;
	struct json_object const *lines = jb_get(obj, "lines");
	size_t const numbers = jb_len(lines);
	size_t const n = numbers / REVIEW_RANGE_NUMBERS;
	size_t at;
	if (!review_name_valid(key.repo) || !key.commit.data ||
	    !util_is_oid(key.commit.data, key.commit.len) || !review_name_valid(key.path) ||
	    jb_uint_checked(jb_get(obj, "total"), UINT32_MAX, &total) || !n ||
	    numbers % REVIEW_RANGE_NUMBERS || n > (REVIEW_MAX - r->size) / REVIEW_ITEM ||
	    review_files_find(list, &key, &at) < list->n)
		return EINVAL;
	key.total = (uint32_t)total;
	struct review_file f = { REVIEW_FILE_TEXTS, .cap = n };
	f.ranges = malloc(n * sizeof *f.ranges);
	int e = f.ranges ? review_file_set(&f, &key) : ENOMEM;
	/* The ranges must be sorted, must not overlap and must be within the
	 * file, and the turns of two adjoining ranges must differ. */
	for (uint64_t next = 1; !e && f.n < n; ++f.n) {
		uint64_t first;
		uint64_t last;
		uint64_t turn;
		size_t const k = REVIEW_RANGE_NUMBERS * f.n;
		if (jb_uint_checked(jb_at(lines, k), UINT32_MAX, &first) ||
		    jb_uint_checked(jb_at(lines, k + 1), UINT32_MAX, &last) ||
		    jb_uint_checked(jb_at(lines, k + 2), UINT32_MAX, &turn) || first < next ||
		    last < first || (key.total && last > key.total) ||
		    (f.n && first == next && turn == f.ranges[f.n - 1].turn)) {
			e = EINVAL;
			break;
		}
		f.ranges[f.n] = (struct review_range){
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
	r->size += review_file_charge(&f, f.n);
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
		struct review_file const *p = &r->pending.files[i];
		struct review_source const key = review_file_source(p);
		size_t at;
		size_t const k = review_files_find(&r->log, &key, &at);
		if (k == r->log.n)
			continue;
		struct review_file const *f = &r->log.files[k];
		uint32_t const total = f->total > p->total ? f->total : p->total;
		if ((f->total && p->total && f->total != p->total) ||
		    (total && (f->ranges[f->n - 1].last > total || p->ranges[p->n - 1].last > total)))
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
	/* The restore stops once the record exceeds REVIEW_MAX. */
	for (size_t k = 0; !e && k < array_size(lists); ++k) {
		struct json_object const *a = jb_get(obj, lists[k].key);
		e = json_object_is_type(a, json_type_array) ? 0 : EINVAL;
		for (size_t i = 0, n = jb_len(a); !e && r->size <= REVIEW_MAX && i < n; ++i)
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
 * Initializes the change @p c from the saved change @p obj. A change to a
 * state that requires logged lines must contain evidence and a commit, and
 * it is valid only in a finding with a location (@p located). Returns 0,
 * EINVAL or ENOMEM, and leaves @p c empty on failure.
 */
static int
review_change_init_json (struct review_change     *c,
                         struct json_object const *obj,
                         bool                      located)
{
	*c = (struct review_change){
		.evidence = DSTR_INIT, .old_claim = DSTR_INIT, .old_location = review_location(0, 0)
	};
	struct cstr_view const commit = jb_view(jb_get(obj, "commit"));
	struct cstr_view const evidence = jb_view(jb_get(obj, "evidence"));
	struct json_object *claim = jb_get(obj, "claim");
	/* The old location is "path:first-last", or "" for no location. */
	struct json_object *location = jb_get(obj, "location");
	struct cstr_view const where = jb_view(location);
	struct review_location_view loc = {};
	size_t const state = cstr_view_index(review_state_names, REVIEW_STATES,
	                                     jb_view(jb_get(obj, "state")));
	uint64_t turn;
	if (jb_uint_checked(jb_get(obj, "turn"), UINT32_MAX, &turn) || state == REVIEW_STATES ||
	    !commit.data || (commit.len && !util_is_oid(commit.data, commit.len)) ||
	    (location && !where.data) ||
	    (where.len && (review_location_parse(&loc, where) || !review_name_valid(loc.path))) ||
	    (review_state_logged(state) && (!located || !evidence.len || !commit.len)))
		return EINVAL;
	c->turn = (uint32_t)turn;
	c->state = (enum review_state)state;
	c->moved = location;
	c->commit = review_oid(commit);
	c->old_location.first = loc.first;
	c->old_location.last = loc.last;
	int e = review_text_json(&c->evidence, evidence, true);
	if (!e && claim)
		e = review_text_json(&c->old_claim, jb_view(claim), false);
	if (!e)
		e = cstr_view_store(loc.path, &c->old_location.path);
	if (e)
		review_change_fini(c);
	return e;
}

/**
 * Parses the saved finding @p obj, appends it to the findings of @p r, and
 * adds its charge to r->size. The first and last line of a finding without
 * a location are 0. Returns 0, EINVAL or ENOMEM.
 */
static int
review_findings_add_json (struct review            *r,
                          struct json_object const *obj)
{
	struct json_object *path = jb_get(obj, "path");
	struct cstr_view const where = jb_view(path);
	struct json_object const *history = jb_get(obj, "history");
	size_t const n = jb_len(history);
	uint64_t first;
	uint64_t last;
	if (jb_uint_checked(jb_get(obj, "first"), UINT32_MAX, &first) ||
	    jb_uint_checked(jb_get(obj, "last"), UINT32_MAX, &last) || !n ||
	    n > (REVIEW_MAX - r->size) / REVIEW_ITEM ||
	    !(path ? review_name_valid(where) && first && first <= last : !first && !last))
		return EINVAL;
	int e = review_findings_reserve(r);
	if (e)
		return e;
	struct review_finding f = {
		.claim    = DSTR_INIT,
		.location = review_location((uint32_t)first, (uint32_t)last),
		.cap      = n,
	};
	f.changes = malloc(n * sizeof *f.changes);
	if (!f.changes)
		return ENOMEM;
	e = review_text_json(&f.claim, jb_view(jb_get(obj, "claim")), false);
	if (!e)
		e = cstr_view_store(where, &f.location.path);
	for (; !e && f.n < n; ++f.n)
		e = review_change_init_json(&f.changes[f.n], jb_at(history, f.n), path);
	if (e) {
		review_finding_fini(&f);
		return e;
	}
	r->findings[r->n_findings++] = f;
	r->size += review_finding_charge(&f);
	return 0;
}

int
review_restore (struct review            *r,
                struct json_object const *obj)
{
	if (!r || !obj)
		return EFAULT;
	struct review got = { .flags = r->flags };
	struct json_object const *findings = jb_get(obj, "findings");
	int e = json_object_is_type(findings, json_type_array) ? 0 : EINVAL;
	/* The restore stops once the record exceeds REVIEW_MAX. */
	for (size_t i = 0, n = jb_len(findings); !e && got.size <= REVIEW_MAX && i < n; ++i)
		e = review_findings_add_json(&got, jb_at(findings, i));
	if (!e && got.size <= REVIEW_MAX && (got.flags & REVIEW_READ_LOG))
		e = review_restore_log(&got, obj);
	if (!e && got.size > REVIEW_MAX)
		e = EINVAL;
	if (e) {
		review_fini(&got);
		return e;
	}
	review_fini(r);
	*r = got;
	return 0;
}

#undef REVIEW_FILE_TEXTS
