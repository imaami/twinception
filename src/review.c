/** @file review.c
 *
 * The review record of an ask call (src/review.h).
 */
#define _GNU_SOURCE /* memrchr(), reallocarray() */

#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "dstr.h"
#include "jb.h"
#include "review.h"
#include "util.h"

#define NAME(s) { "" s, sizeof s - 1 }

struct tool_text const review_state_names[REVIEW_STATES] = {
	[REVIEW_CANDIDATE] = NAME("candidate"),
	[REVIEW_VERIFIED]  = NAME("verified"),
	[REVIEW_DISPROVED] = NAME("disproved"),
	[REVIEW_RETRACTED] = NAME("retracted"),
};

struct tool_text const review_action_names[REVIEW_ACTIONS] = {
	[REVIEW_ADD]    = NAME("add"),
	[REVIEW_UPDATE] = NAME("update"),
	[REVIEW_SHOW]   = NAME("show"),
};

/* REVIEW_SHORT is the number of commit ID digits that a render shows.
 * REVIEW_OID is the size of a full commit ID and its NUL.
 */
enum { REVIEW_SHORT = 12, REVIEW_OID = 64 + 1 };

/* An owned text. ptr is nullptr for no text; otherwise it holds len bytes
 * and a NUL.
 */
struct review_text {
	char   *ptr;
	size_t  len;
};

/* Lines first to last of a file, from 1. */
struct review_range {
	uint32_t first;
	uint32_t last;
	uint32_t turn;   /* the turn of the latest read that showed them */
};

/* The lines read from a file at one commit of one repository. The ranges
 * are sorted and do not overlap, and adjoining ranges have different turns.
 */
struct review_file {
	struct review_text   repo;
	struct review_text   path;
	struct review_range *ranges;
	size_t               n;
	size_t               cap;
	uint32_t             total;   /* line count; 0 if unknown */
	char                 commit[REVIEW_OID];
};

/* Files grouped by repository and commit. The groups, and the files in
 * each group, are in the order of their first read.
 */
struct review_files {
	struct review_file *files;
	size_t              n;
	size_t              cap;
};

/* One change of a finding. If the change replaced the claim, claim holds
 * the old claim, and otherwise claim.ptr is nullptr. location likewise
 * holds the old location as path:lines text, which is empty if the finding
 * had none. commit is the full ID of the commit at which the lines of the
 * location were read, or empty.
 */
struct review_change {
	struct review_text evidence;
	struct review_text claim;
	struct review_text location;
	uint32_t           turn;
	uint8_t            state;   /* enum review_state */
	char               commit[REVIEW_OID];
};

/* A finding has a claim, its changes, and a location path:first-last if
 * path.ptr is not nullptr. The last change holds the state.
 */
struct review_finding {
	struct review_text    claim;
	struct review_text    path;
	struct review_change *changes;
	size_t                n;
	size_t                cap;
	uint32_t              first;
	uint32_t              last;
};

struct review_read {
	struct review_file file;  /* the file, with no ranges */
	size_t             at;
	size_t             len;
	uint32_t           first;
	uint32_t           last;
};

struct review {
	struct review_files    log;       /* the read log */
	struct review_files    pending;   /* the pending lines (review.h) */
	struct review_finding *findings;  /* by id, from 1 */
	size_t                 n_findings;
	size_t                 cap_findings;
	size_t                 size;      /* charged bytes, pending lines too */
	uint32_t               full;      /* the first turn whose read did not
	                                     fit; 0 if none */
	uint32_t               flags;     /* enum review_flag */
};

/* Append a refusal to @p out with dstr_printf(). Evaluates to EINVAL, or
 * to ENOMEM if the append fails.
 */
#define REFUSE(out, ...) (dstr_printf(out, __VA_ARGS__) ? EINVAL : ENOMEM)

/* Returns the index of the string @p v in the @p n names at @p names, or
 * @p n if @p v is not one of them.
 */
static size_t
review_name_of (struct json_object     *v,
                struct tool_text const *names,
                size_t                  n)
{
	size_t i = 0;
	while (i < n && !jb_is(v, names[i].ptr))
		++i;
	return i;
}

/* Whether @p a and the @p len bytes at @p b are the same text. */
static bool
review_text_is (struct review_text const *a,
                char const               *b,
                size_t                    len)
{
	return a->len == len && (!len || !memcmp(a->ptr, b, len));
}

/* Copy the @p len bytes at @p s into @p t; returns 0 or ENOMEM. */
static int
review_text_set (struct review_text *t,
                 char const         *s,
                 size_t              len)
{
	char *p = malloc(len + 1);
	if (!p)
		return ENOMEM;
	if (len)
		memcpy(p, s, len);
	p[len] = '\0';
	*t = (struct review_text){ p, len };
	return 0;
}

static void
review_text_fini (struct review_text *t)
{
	free(t->ptr);
	*t = (struct review_text){};
}

/* Whether @p t is 1 to REVIEW_WORD bytes of UTF-8 text without NUL, as a
 * repository name or a path must be.
 */
static bool
review_word (struct tool_text t)
{
	return t.ptr && t.len && t.len <= REVIEW_WORD && jb_utf8(t.ptr, t.len) == t.len &&
	       !memchr(t.ptr, '\0', t.len);
}

/* Returns the number of newlines in the @p n bytes at @p p. */
static size_t
review_newlines (char const *p,
                 size_t      n)
{
	size_t k = 0;
	for (char const *end = p + n; n && (p = memchr(p, '\n', (size_t)(end - p))); ++p)
		++k;
	return k;
}

static void
review_file_fini (struct review_file *f)
{
	review_text_fini(&f->repo);
	review_text_fini(&f->path);
	free(f->ranges);
	f->ranges = nullptr;
}

static void
review_files_fini (struct review_files *list)
{
	for (size_t i = 0; i < list->n; ++i)
		review_file_fini(&list->files[i]);
	free(list->files);
	*list = (struct review_files){};
}

static void
review_change_fini (struct review_change *c)
{
	review_text_fini(&c->evidence);
	review_text_fini(&c->claim);
	review_text_fini(&c->location);
}

static void
review_finding_fini (struct review_finding *f)
{
	review_text_fini(&f->claim);
	review_text_fini(&f->path);
	for (size_t i = 0; i < f->n; ++i)
		review_change_fini(&f->changes[i]);
	free(f->changes);
	f->changes = nullptr;
}

/* Free what @p r holds and leave it empty. */
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

/* Returns the bytes charged for the file @p f with @p n ranges. */
static size_t
review_file_charge (struct review_file const *f,
                    size_t                    n)
{
	return REVIEW_ITEM + f->repo.len + f->path.len + n * REVIEW_ITEM;
}

/* Returns the bytes charged for the change @p c: its size and its texts. */
static size_t
review_change_charge (struct review_change const *c)
{
	return sizeof *c + c->evidence.len + c->claim.len + c->location.len;
}

/* Returns the bytes charged for the finding @p f: its size, its texts and
 * its changes.
 */
static size_t
review_finding_charge (struct review_finding const *f)
{
	size_t n = sizeof *f + f->claim.len + f->path.len;
	for (size_t i = 0; i < f->n; ++i)
		n += review_change_charge(&f->changes[i]);
	return n;
}

struct review *
review_create (uint32_t  flags,
               int      *e)
{
	if (!e)
		return nullptr;
	struct review *r = calloc(1, sizeof *r);
	*e = r ? 0 : ENOMEM;
	if (r)
		r->flags = flags;
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

struct review_read *
review_read_create (struct review_source const *src,
                    struct tool_text            out,
                    int                        *e)
{
	if (!e)
		return nullptr;
	if (!src || (!out.ptr && out.len)) {
		*e = EFAULT;
		return nullptr;
	}
	bool within = src->len && src->len <= out.len && src->at <= out.len - src->len;
	size_t lines = within ? review_newlines(out.ptr + src->at, src->len) : 0;
	/* A newline ends each line, but the last may have none. */
	size_t want = (size_t)(src->last - src->first) + 1;
	if (!src->commit.ptr || !util_is_oid(src->commit.ptr, src->commit.len) ||
	    !review_word(src->repo) || !review_word(src->path) || !src->first ||
	    src->last < src->first || (src->total && src->total < src->last) || !within ||
	    lines + (out.ptr[src->at + src->len - 1] != '\n') != want) {
		*e = EINVAL;
		return nullptr;
	}
	struct review_read *read = calloc(1, sizeof *read);
	*e = !read ? ENOMEM : review_text_set(&read->file.repo, src->repo.ptr, src->repo.len);
	if (!*e)
		*e = review_text_set(&read->file.path, src->path.ptr, src->path.len);
	if (*e) {
		review_read_destroy(&read);
		return nullptr;
	}
	memcpy(read->file.commit, src->commit.ptr, src->commit.len);
	read->file.total = src->total;
	read->at = src->at;
	read->len = src->len;
	read->first = src->first;
	read->last = src->last;
	return read;
}

void
review_read_destroy (struct review_read **p_read)
{
	if (!p_read || !*p_read)
		return;
	review_file_fini(&(*p_read)->file);
	free(*p_read);
	*p_read = nullptr;
}

/* Returns a JSON string of @p t, or nullptr if out of memory. */
static struct json_object *
review_string (struct review_text const *t)
{
	return json_object_new_string_len(t->ptr ? t->ptr : "", (int)t->len);
}

/* Returns a JSON integer of @p n, or nullptr if out of memory. */
static struct json_object *
review_int (uint64_t n)
{
	return json_object_new_int64((int64_t)n);
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
	struct json_object *obj = jb_object(
		"repo", review_string(&read->file.repo),
		"commit", json_object_new_string(read->file.commit),
		"path", review_string(&read->file.path),
		"at", review_int(read->at),
		"len", review_int(read->len),
		"first", review_int(read->first),
		"last", review_int(read->last),
		"total", review_int(read->file.total),
		nullptr);
	*e = obj ? 0 : ENOMEM;
	return obj;
}

/* Store the string member @p key of @p obj in @p *t. Returns false if
 * there is no such string.
 */
static bool
review_get_text (struct json_object const *obj,
                 char const               *key,
                 struct tool_text         *t)
{
	size_t len = 0;
	char const *s = jb_str(jb_get(obj, key), &len);
	*t = (struct tool_text){ s, len };
	return s;
}

/* Store the member @p key of @p obj in @p *n. Returns false unless it is
 * an integer from 0 to @p max.
 */
static bool
review_get_number (struct json_object const *obj,
                   char const               *key,
                   uint64_t                  max,
                   uint64_t                 *n)
{
	int64_t v = jb_int(jb_get(obj, key), -1);
	*n = v < 0 ? 0 : (uint64_t)v;
	return v >= 0 && *n <= max;
}

struct review_read *
review_read_restore (struct json_object const *obj,
                     struct tool_text          out,
                     int                      *e)
{
	if (!e)
		return nullptr;
	if (!obj) {
		*e = EFAULT;
		return nullptr;
	}
	struct review_source src = {};
	uint64_t n[5];
	if (!review_get_text(obj, "repo", &src.repo) ||
	    !review_get_text(obj, "commit", &src.commit) ||
	    !review_get_text(obj, "path", &src.path) ||
	    !review_get_number(obj, "at", SIZE_MAX, &n[0]) ||
	    !review_get_number(obj, "len", SIZE_MAX, &n[1]) ||
	    !review_get_number(obj, "first", UINT32_MAX, &n[2]) ||
	    !review_get_number(obj, "last", UINT32_MAX, &n[3]) ||
	    !review_get_number(obj, "total", UINT32_MAX, &n[4])) {
		*e = EINVAL;
		return nullptr;
	}
	src.at = (size_t)n[0];
	src.len = (size_t)n[1];
	src.first = (uint32_t)n[2];
	src.last = (uint32_t)n[3];
	src.total = (uint32_t)n[4];
	return review_read_create(&src, out, e);
}

/* Whether the files @p f and @p g were read at the same commit of the same
 * repository.
 */
static bool
review_file_with (struct review_file const *f,
                  struct review_file const *g)
{
	return review_text_is(&f->repo, g->repo.ptr, g->repo.len) && !strcmp(f->commit, g->commit);
}

/* Returns the index of the file of @p list with the repository, commit and
 * path of @p key, or list->n if there is none. @p *at gets the index where
 * such a file belongs: after the last file of the same repository and
 * commit, or at the end.
 */
static size_t
review_files_find (struct review_files const *list,
                   struct review_file const  *key,
                   size_t                    *at)
{
	*at = list->n;
	for (size_t i = 0; i < list->n; ++i) {
		struct review_file const *f = &list->files[i];
		if (!review_file_with(f, key))
			continue;
		if (review_text_is(&f->path, key->path.ptr, key->path.len))
			return i;
		*at = i + 1;
	}
	return list->n;
}

/* Insert @p f into @p list at index @p at, and give @p list its texts and
 * ranges. Returns 0, or ENOMEM, and then the caller still owns them.
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

/* Add the range @p lines, read in turn lines.turn, to the file of @p list
 * with the repository, commit and path of @p key, and add the file if
 * @p list does not have it. If key->total is not 0, it becomes the total
 * of the file. If this would take r->size past REVIEW_MAX, nothing
 * changes, and r->full gets lines.turn if it is 0. Returns 0 or ENOMEM.
 */
static int
review_log (struct review            *r,
            struct review_files      *list,
            struct review_file const *key,
            struct review_range       lines)
{
	size_t at;
	size_t k = review_files_find(list, key, &at);
	struct review_file added = {};
	struct review_file *f = k < list->n ? &list->files[k] : &added;
	struct review_range *p = f->ranges;
	/* The ranges before i end before line lines.first - 1, and those from j
	 * on start after line lines.last + 1. The ranges from i to j - 1
	 * overlap or adjoin @p lines. */
	size_t i = 0;
	while (i < f->n && (uint64_t)p[i].last + 1 < lines.first)
		++i;
	size_t j = i;
	while (j < f->n && p[j].first <= (uint64_t)lines.last + 1)
		++j;
	/* The part of p[i] before lines.first and the part of p[j - 1] after
	 * lines.last stay. A part of the same turn merges into @p lines, and a
	 * part of another turn stays a range of its own. */
	struct review_range lo = i < j ? p[i] : lines;
	struct review_range hi = i < j ? p[j - 1] : lines;
	bool keep_lo = lo.first < lines.first && lo.turn != lines.turn;
	bool keep_hi = hi.last > lines.last && hi.turn != lines.turn;
	lines.first = lo.first < lines.first && !keep_lo ? lo.first : lines.first;
	lines.last = hi.last > lines.last && !keep_hi ? hi.last : lines.last;
	lo.last = lines.first - 1;
	hi.first = lines.last + 1;
	struct review_range put[3];
	size_t m = 0;
	if (keep_lo)
		put[m++] = lo;
	put[m++] = lines;
	if (keep_hi)
		put[m++] = hi;
	size_t n = f->n - (j - i) + m;
	size_t before = f == &added ? 0 : review_file_charge(f, f->n);
	size_t after = review_file_charge(key, n);
	if (r->size - before + after > REVIEW_MAX) {
		r->full = r->full ? r->full : lines.turn;
		return 0;
	}
	p = util_room(f->ranges, &f->cap, n, sizeof *p);
	if (!p)
		return ENOMEM;
	f->ranges = p;
	memmove(&p[i + m], &p[j], (f->n - j) * sizeof *p);
	memcpy(&p[i], put, m * sizeof *p);
	f->n = n;
	if (f == &added) {
		int e = review_text_set(&added.repo, key->repo.ptr, key->repo.len);
		if (!e)
			e = review_text_set(&added.path, key->path.ptr, key->path.len);
		if (!e) {
			memcpy(added.commit, key->commit, sizeof added.commit);
			e = review_files_insert(list, &added, at);
		}
		if (e) {
			review_file_fini(&added);
			return e;
		}
		f = &list->files[at];
	}
	r->size = r->size - before + after;
	if (key->total)
		f->total = key->total;
	return 0;
}

int
review_shown (struct review            *r,
              struct review_read const *read,
              struct tool_text          out,
              size_t                    skip,
              size_t                    window,
              uint32_t                  turn)
{
	if (!r || !read || (!out.ptr && out.len))
		return EFAULT;
	if (read->len > out.len || read->at > out.len - read->len)
		return EINVAL;
	size_t end = read->at + read->len;
	if (skip >= end)
		return 0;
	size_t lo = skip > read->at ? skip : read->at;
	size_t hi = window < end - skip ? skip + window : end;
	if (lo >= hi)
		return 0;
	char const *p = out.ptr;
	uint64_t line = read->first + (uint64_t)review_newlines(p + read->at, lo - read->at);
	/* Skip a line that the start of the window cuts. */
	if (lo > read->at && p[lo - 1] != '\n') {
		char const *nl = memchr(p + lo, '\n', hi - lo);
		if (!nl)
			return 0;
		lo = (size_t)(nl - p) + 1;
		++line;
	}
	/* Skip a line that the end of the window cuts. The last line of the
	 * read may have no newline, and it counts if the window reaches the end
	 * of the read. */
	uint64_t n = review_newlines(p + lo, hi - lo) +
	             (hi == end && hi > lo && p[hi - 1] != '\n');
	if (!n)
		return 0;
	if (line + n - 1 > read->last)
		return EINVAL;
	return review_log(r, &r->pending, &read->file, (struct review_range){
		(uint32_t)line, (uint32_t)(line + n - 1), turn });
}

/* Move the pending lines out of @p r and return them. r->size loses their
 * charge.
 */
static struct review_files
review_pending_take (struct review *r)
{
	struct review_files pending = r->pending;
	r->pending = (struct review_files){};
	for (size_t i = 0; i < pending.n; ++i)
		r->size -= review_file_charge(&pending.files[i], pending.files[i].n);
	return pending;
}

int
review_answered (struct review *r)
{
	if (!r)
		return EFAULT;
	struct review_files pending = review_pending_take(r);
	int e = 0;
	for (size_t i = 0; !e && i < pending.n; ++i)
		for (size_t k = 0; !e && k < pending.files[i].n; ++k)
			e = review_log(r, &r->log, &pending.files[i], pending.files[i].ranges[k]);
	review_files_fini(&pending);
	return e;
}

void
review_compacted (struct review *r)
{
	if (!r)
		return;
	struct review_files pending = review_pending_take(r);
	review_files_fini(&pending);
}

/* The output of a render or a result. The text goes into ds after its
 * first start bytes. The first skip bytes of the text are dropped, and at
 * most room bytes after them are kept. The piece that crosses room is cut
 * and sets over, and later pieces are dropped.
 */
struct review_out {
	struct dstr *ds;
	size_t       start;
	size_t       room;
	size_t       skip;
	bool         over;    /* a piece went past room */
	bool         failed;  /* out of memory */
};

static void
review_out_put (struct review_out *o,
                char const        *s,
                size_t             n)
{
	if (o->over || o->failed)
		return;
	size_t k = o->skip < n ? o->skip : n;
	o->skip -= k;
	s += k;
	n -= k;
	size_t room = o->room - (dstr_len(o->ds) - o->start);
	if (n > room) {
		n = room;
		o->over = true;
	}
	o->failed = !dstr_cat_n(o->ds, s, n);
}

/* Write the string literal @p s to @p o. */
#define PUT(o, s) review_out_put(o, "" s, sizeof s - 1)

static void
review_out_text (struct review_out        *o,
                 struct review_text const *t)
{
	review_out_put(o, t->ptr, t->len);
}

static void
review_out_state (struct review_out *o,
                  uint8_t            state)
{
	review_out_put(o, review_state_names[state].ptr, review_state_names[state].len);
}

/* Write @p n in decimal to @p o. */
static void
review_out_number (struct review_out *o,
                   uint64_t           n)
{
	char digits[20];
	char *p = util_decimal(digits + sizeof digits, n);
	review_out_put(o, p, (size_t)(digits + sizeof digits - p));
}

/* Write the numbers of lines @p first to @p last to @p o, as "first" or
 * "first-last".
 */
static void
review_out_lines (struct review_out *o,
                  uint32_t           first,
                  uint32_t           last)
{
	review_out_number(o, first);
	if (last != first) {
		PUT(o, "-");
		review_out_number(o, last);
	}
}

/* Write the status of the finding @p f to @p o: the state, then the
 * location, if any, with the commit at which its lines were read or
 * "not read".
 */
static void
review_out_status (struct review_out           *o,
                   struct review_finding const *f)
{
	struct review_change const *c = &f->changes[f->n - 1];
	review_out_state(o, c->state);
	if (!f->path.ptr)
		return;
	PUT(o, ", ");
	review_out_text(o, &f->path);
	PUT(o, ":");
	review_out_lines(o, f->first, f->last);
	if (*c->commit) {
		PUT(o, " at ");
		review_out_put(o, c->commit, REVIEW_SHORT);
	} else {
		PUT(o, ", not read");
	}
}

/* Finish the text that @p o wrote. o->room is the limit plus one byte for
 * a final newline. If the text fits, drop its final newline, if any, and
 * return 0. Otherwise, if @p cut is false, truncate the text to the limit
 * and return EAGAIN. If @p cut is true, truncate it after its last newline
 * within the limit, append a notice that gives that offset, and return 0.
 * Returns ENOMEM if a write failed.
 */
static int
review_out_end (struct review_out *o,
                bool               cut)
{
	if (o->failed)
		return ENOMEM;
	size_t len = dstr_len(o->ds) - o->start;
	char const *text = dstr_get(o->ds) + o->start;
	if (!o->over) {
		if (len && text[len - 1] == '\n')
			dstr_erase(o->ds, o->start + len - 1, 1);
		return 0;
	}
	size_t most = o->room - 1;
	if (!cut) {
		dstr_erase(o->ds, o->start + most, SIZE_MAX);
		return EAGAIN;
	}
	char const *nl = len ? memrchr(text, '\n', len < most ? len : most) : nullptr;
	size_t keep = nl ? (size_t)(nl - text) + 1 : 0;
	dstr_erase(o->ds, o->start + keep, SIZE_MAX);
	return dstr_printf(o->ds, "[askai: the record goes on past these %zu bytes: call "
	                   "findings with action show and \"offset\": %zu for the "
	                   "rest.]", keep, keep) ? 0 : ENOMEM;
}

/* Write the findings of @p r to @p o. Open findings come first, each with
 * its claim and latest evidence. Closed findings follow, one line each.
 */
static void
review_out_findings (struct review_out   *o,
                     struct review const *r)
{
	if (!r || !r->n_findings) {
		PUT(o, "[askai: you have recorded no findings; the findings tool "
		       "records them.]\n");
		return;
	}
	PUT(o, "[askai: your findings; findings action show with an id gives one's "
	       "history:]\n");
	for (size_t pass = 0; pass < 2; ++pass) {
		for (size_t i = 0; i < r->n_findings && !o->over; ++i) {
			struct review_finding const *f = &r->findings[i];
			uint8_t state = f->changes[f->n - 1].state;
			bool closed = state == REVIEW_DISPROVED || state == REVIEW_RETRACTED;
			if (closed != (pass == 1))
				continue;
			review_out_number(o, i + 1);
			PUT(o, ". ");
			review_out_status(o, f);
			if (closed) {
				/* The claim's first line, at most REVIEW_CLOSED bytes. */
				size_t n = f->claim.len < REVIEW_CLOSED ? f->claim.len : REVIEW_CLOSED;
				char const *nl = memchr(f->claim.ptr, '\n', n);
				n = jb_utf8(f->claim.ptr, nl ? (size_t)(nl - f->claim.ptr) : n);
				PUT(o, ": ");
				review_out_put(o, f->claim.ptr, n);
				if (n < f->claim.len)
					PUT(o, "...");
				PUT(o, "\n");
				continue;
			}
			PUT(o, "\n   claim: ");
			review_out_text(o, &f->claim);
			size_t k = f->n;
			while (k && !f->changes[k - 1].evidence.len)
				--k;
			if (k) {
				PUT(o, "\n   evidence: ");
				review_out_text(o, &f->changes[k - 1].evidence);
			}
			PUT(o, "\n");
		}
	}
}

/* Write the read log of @p r to @p o. Each repository and commit gets a
 * line, and below it each file gets a line with its ranges and the number
 * of lines that they cover. Adjoining ranges are written as one.
 */
static void
review_out_reads (struct review_out   *o,
                  struct review const *r)
{
	if (!r)
		return;
	if (r->log.n)
		PUT(o, "[askai: what you read with git read in this call:]\n");
	for (size_t i = 0; i < r->log.n && !o->over; ++i) {
		struct review_file const *f = &r->log.files[i];
		if (!i || !review_file_with(f - 1, f)) {
			review_out_text(o, &f->repo);
			PUT(o, " ");
			review_out_put(o, f->commit, strlen(f->commit));
			PUT(o, "\n");
		}
		uint64_t lines = 0;
		for (size_t k = 0; k < f->n; ++k)
			lines += (uint64_t)(f->ranges[k].last - f->ranges[k].first) + 1;
		PUT(o, "  ");
		review_out_text(o, &f->path);
		if (lines == 1)
			PUT(o, "  line ");
		else
			PUT(o, "  lines ");
		for (size_t k = 0; k < f->n && !o->over;) {
			if (k)
				PUT(o, ", ");
			uint32_t first = f->ranges[k].first;
			uint32_t last = f->ranges[k].last;
			while (++k < f->n && f->ranges[k].first == (uint64_t)last + 1)
				last = f->ranges[k].last;
			review_out_lines(o, first, last);
		}
		/* The ranges lie within the file and do not overlap, so they
		 * cover the whole file if lines is f->total. */
		if (lines == f->total) {
			PUT(o, ": all ");
			review_out_number(o, lines);
			review_out_put(o, " lines", lines == 1 ? 5 : 6);
			PUT(o, "\n");
			continue;
		}
		PUT(o, ": ");
		review_out_number(o, lines);
		if (f->total) {
			PUT(o, " of ");
			review_out_number(o, f->total);
			PUT(o, " lines");
		} else {
			PUT(o, " of an unknown number of lines");
		}
		PUT(o, "\n");
	}
	if (r->full) {
		PUT(o, "[askai: the read log is full, so what you read from turn ");
		review_out_number(o, r->full);
		PUT(o, " on may be missing from it.]\n");
	}
}

/* Write the record @p r, or the empty record if @p r is nullptr, to @p o:
 * a header, the findings, then the read log.
 */
static void
review_out_record (struct review_out   *o,
                   struct review const *r)
{
	PUT(o, "[askai: your review record, which askai keeps through every compaction "
	       "of this ask call. askai also keeps your task word for word: the system "
	       "text, the earlier messages and the prompt, which is all that comes "
	       "before askai's first [askai: ...] line. You need not copy the task into "
	       "notes or a summary.]\n");
	review_out_findings(o, r);
	review_out_reads(o, r);
}

int
review_render (struct review const *r,
               struct dstr         *out,
               size_t               most)
{
	if (!out)
		return EFAULT;
	/* The room has a byte more for the final newline (review_out_end()). */
	struct review_out o = { .ds = out, .start = dstr_len(out),
	                        .room = most < SIZE_MAX ? most + 1 : most };
	review_out_record(&o, r);
	return review_out_end(&o, true);
}

/* Write the finding @p id of @p r and its changes to @p o. */
static void
review_out_history (struct review_out   *o,
                    struct review const *r,
                    size_t               id)
{
	struct review_finding const *f = &r->findings[id - 1];
	PUT(o, "Finding ");
	review_out_number(o, id);
	PUT(o, ": ");
	review_out_status(o, f);
	PUT(o, "\nclaim: ");
	review_out_text(o, &f->claim);
	PUT(o, "\n");
	for (size_t i = 0; i < f->n; ++i) {
		struct review_change const *c = &f->changes[i];
		PUT(o, "turn ");
		review_out_number(o, c->turn);
		PUT(o, ": ");
		review_out_state(o, c->state);
		if (*c->commit) {
			PUT(o, " at ");
			review_out_put(o, c->commit, REVIEW_SHORT);
		}
		PUT(o, "\n");
		if (c->evidence.len) {
			PUT(o, "  evidence: ");
			review_out_text(o, &c->evidence);
			PUT(o, "\n");
		}
		if (c->claim.ptr) {
			PUT(o, "  the claim was: ");
			review_out_text(o, &c->claim);
			PUT(o, "\n");
		}
		if (c->location.ptr) {
			PUT(o, "  the location was: ");
			if (c->location.len)
				review_out_text(o, &c->location);
			else
				PUT(o, "none");
			PUT(o, "\n");
		}
	}
}

/* A location as a call gives it. prefix is the start of a commit ID, or
 * empty. path and first to last are the file and its lines.
 */
struct review_place {
	struct tool_text prefix;
	struct tool_text path;   /* ptr is nullptr if there is no location */
	uint32_t         first;
	uint32_t         last;
};

/* Parse @p t, of the form [commit:]path:first[-last], into @p p. A commit
 * is 7 to 64 lowercase hexadecimal digits, as git grep prints it. A
 * leading "HEAD:" means no commit. Returns false for another form.
 */
static bool
review_place_parse (struct review_place *p,
                    struct tool_text     t)
{
	static char const head[] = "HEAD:";
	char const *end = t.ptr + t.len;
	char const *colon = memrchr(t.ptr, ':', t.len);
	if (!colon)
		return false;
	char const *q = colon + 1;
	uint64_t first;
	q += util_digits(q, (size_t)(end - q), UINT32_MAX, &first);
	uint64_t last = first;
	if (first && q < end && *q == '-')
		q += 1 + util_digits(q + 1, (size_t)(end - q - 1), UINT32_MAX, &last);
	if (!first || last < first || q != end)
		return false;
	size_t before = (size_t)(colon - t.ptr);
	size_t hex = 0;
	while (hex < before && hex <= 64 && util_is_hex(t.ptr + hex, 1))
		++hex;
	/* 7 to 64 hexadecimal digits and a ':' before the path are a commit. */
	bool prefixed = hex >= 7 && hex <= 64 && hex < before && t.ptr[hex] == ':';
	size_t skip = prefixed ? hex + 1 : 0;
	if (before > sizeof head - 1 && !memcmp(t.ptr, head, sizeof head - 1))
		skip = sizeof head - 1;
	struct tool_text path = { t.ptr + skip, before - skip };
	if (!review_word(path))
		return false;
	*p = (struct review_place){
		.prefix = { t.ptr, prefixed ? hex : 0 },
		.path   = path,
		.first  = (uint32_t)first,
		.last   = (uint32_t)last,
	};
	return true;
}

/* Whether the file @p f was read at a commit that starts with @p prefix. */
static bool
review_file_at (struct review_file const *f,
                struct tool_text          prefix)
{
	return !prefix.len || !memcmp(f->commit, prefix.ptr, prefix.len);
}

/* Find the range of @p f that holds line @p a, and follow the adjoining
 * ranges after it until the run reaches line @p b. Returns the last line
 * of the run, or 0 if no range holds @p a. If the result is not 0,
 * @p *turn gets the lowest turn of the run.
 */
static uint32_t
review_file_reach (struct review_file const *f,
                   uint32_t                  a,
                   uint32_t                  b,
                   uint32_t                 *turn)
{
	size_t lo = 0;
	size_t hi = f->n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (f->ranges[mid].first <= a)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo || f->ranges[lo - 1].last < a)
		return 0;
	struct review_range const *g = &f->ranges[lo - 1];
	uint32_t last = g->last;
	*turn = g->turn;
	for (struct review_range const *end = f->ranges + f->n;
	     last < b && ++g < end && g->first == (uint64_t)last + 1; last = g->last)
		*turn = g->turn < *turn ? g->turn : *turn;
	return last;
}

/* Returns the file of the read log of @p r that best covers the location
 * @p p, or nullptr if no file holds p->first. Only the files with the
 * path of @p p and a commit that starts with p->prefix are considered.
 * @p *reach gets the last line that the returned file holds without a gap
 * from p->first, or 0. The files that hold all lines of @p p rank first,
 * and among them the file whose oldest read of the lines is newest wins
 * (review_file_reach()). Next rank the files with a total of 0 (unknown)
 * or at least p->last, then the rest, and in these two ranks the furthest
 * reach wins. @p *most gets the considered file with the largest total,
 * where 0 is largest, or nullptr.
 */
static struct review_file const *
review_covering (struct review const       *r,
                 struct review_place const *p,
                 uint32_t                  *reach,
                 struct review_file const **most)
{
	struct review_file const *best = nullptr;
	uint32_t latest = 0;
	uint8_t rank = 0;
	*reach = 0;
	*most = nullptr;
	for (size_t i = 0; i < r->log.n; ++i) {
		struct review_file const *f = &r->log.files[i];
		if (!review_text_is(&f->path, p->path.ptr, p->path.len) ||
		    !review_file_at(f, p->prefix))
			continue;
		if (!*most || ((*most)->total && (!f->total || f->total > (*most)->total)))
			*most = f;
		uint32_t turn = 0;
		uint32_t last = review_file_reach(f, p->first, p->last, &turn);
		if (!last)
			continue;
		uint8_t k = last >= p->last ? 2 : !f->total || f->total >= p->last;
		if (!best || (k != rank ? k > rank : k == 2 ? turn >= latest : last >= *reach)) {
			best = f;
			rank = k;
			latest = turn;
			*reach = last;
		}
	}
	return best;
}

/* Resolve p->path against the files of the read log of @p r whose commit
 * starts with p->prefix. An exact match stores p->path in found[0] and
 * returns 1. Otherwise the matches are the logged paths that end with '/'
 * and p->path. Stores up to 4 distinct matches in @p found and returns
 * their count.
 */
static size_t
review_resolve (struct review const       *r,
                struct review_place const *p,
                struct tool_text           found[4])
{
	size_t n = 0;
	size_t tail = p->path.len;
	for (size_t i = 0; i < r->log.n; ++i) {
		struct review_file const *f = &r->log.files[i];
		if (!review_file_at(f, p->prefix))
			continue;
		if (review_text_is(&f->path, p->path.ptr, tail)) {
			found[0] = p->path;
			return 1;
		}
		if (f->path.len <= tail || f->path.ptr[f->path.len - tail - 1] != '/' ||
		    memcmp(f->path.ptr + f->path.len - tail, p->path.ptr, tail))
			continue;
		size_t k = 0;
		while (k < n && (found[k].len != f->path.len ||
		                 memcmp(found[k].ptr, f->path.ptr, f->path.len)))
			++k;
		if (k == n && n < 4)
			found[n++] = (struct tool_text){ f->path.ptr, f->path.len };
	}
	return n;
}

/* The arguments of an add or update call. */
struct review_args {
	struct tool_text    claim;     /* ptr is nullptr if there is none */
	struct tool_text    evidence;  /* likewise */
	struct review_place place;     /* place.path.ptr is nullptr if
	                                  there is none */
	uint8_t             state;     /* REVIEW_STATES if there is none */
	uint8_t             cut;       /* CUT_CLAIM and CUT_EVIDENCE bits for
	                                  the texts that review_kept_fn
	                                  trimmed */
};

enum { CUT_CLAIM = 1, CUT_EVIDENCE = 2 };

/* Store in @p *t the string member @p key of @p args, or a null text if
 * there is none. If @p kept is not nullptr, it trims the text, and @p *cut
 * says whether it did. Returns 0, or EINVAL or ENOMEM with the refusal in
 * @p out.
 */
static int
review_args_text (struct json_object *args,
                  char const         *key,
                  review_kept_fn     *kept,
                  struct tool_text   *t,
                  bool               *cut,
                  struct dstr        *out)
{
	*t = (struct tool_text){};
	struct json_object *v = jb_get(args, key);
	if (!v)
		return 0;
	size_t len = 0;
	char const *s = jb_str(v, &len);
	if (!s || !len || jb_utf8(s, len) != len)
		return REFUSE(out, "The %s must be a string of UTF-8 text, not empty.", key);
	size_t n = kept ? kept(s, len) : len;
	n = n < len ? n : len;
	/* Also drop the spaces and line ends before the dropped part. */
	while (n < len && n && memchr(" \t\r\n", s[n - 1], 4))
		--n;
	if (!n)
		return REFUSE(out, "The %s has only askai's [askai: ...] lines, which askai "
		              "adds to tool results. Give the %s itself.", key, key);
	*cut = n < len;
	*t = (struct tool_text){ s, n };
	return 0;
}

/* Read the claim, location, state and evidence of @p args into @p a.
 * Returns 0, or EINVAL or ENOMEM with the refusal in @p out.
 */
static int
review_args_get (struct review_args *a,
                 struct json_object *args,
                 review_kept_fn     *kept,
                 struct dstr        *out)
{
	static_assert(REVIEW_STATES == 4, "the problem names the states");
	*a = (struct review_args){ .state = REVIEW_STATES };
	bool cut[2] = {};
	struct tool_text location;
	int e = review_args_text(args, "claim", kept, &a->claim, &cut[0], out);
	if (!e)
		e = review_args_text(args, "location", nullptr, &location, &(bool){}, out);
	if (!e && location.ptr && !review_place_parse(&a->place, location))
		e = REFUSE(out, "The location must be path:line or path:first-last, such as "
		           "src/mcp.c:120-140.");
	struct json_object *state = jb_get(args, "state");
	if (!e && state) {
		a->state = (uint8_t)review_name_of(state, review_state_names, REVIEW_STATES);
		if (a->state == REVIEW_STATES)
			e = REFUSE(out, "The state argument must be candidate, verified, disproved "
			           "or retracted.");
	}
	if (!e)
		e = review_args_text(args, "evidence", kept, &a->evidence, &cut[1], out);
	a->cut = (uint8_t)(cut[0] * CUT_CLAIM | cut[1] * CUT_EVIDENCE);
	return e;
}

/* Whether the state @p state needs the lines of the location in the read
 * log.
 */
static bool
review_gated (uint8_t state)
{
	return state == REVIEW_VERIFIED || state == REVIEW_DISPROVED;
}

/* Resolve p->path against the read log of @p r (review_resolve()), and set
 * @p *read to the logged file that holds all lines of @p p, or nullptr. If
 * review_gated(@p state), refuse a record without REVIEW_READS, a missing
 * location, and lines that the log does not hold. Refuse a short path
 * that names several logged paths if the call gave the location (@p given)
 * or the state is gated. Otherwise p->path stays as it is. Returns 0, or
 * EINVAL or ENOMEM with the refusal in @p out.
 */
static int
review_check_place (struct review const       *r,
                    struct review_place       *p,
                    uint8_t                    state,
                    bool                       given,
                    struct review_file const **read,
                    struct dstr               *out)
{
	*read = nullptr;
	char const *name = review_state_names[state].ptr;
	if (review_gated(state) && !(r->flags & REVIEW_READS))
		return REFUSE(out, "This ask call has no git tool, so a finding cannot be verified "
		              "or disproved here; keep it a candidate, or retract it.");
	if (!p->path.ptr)
		return !review_gated(state) ? 0
		     : REFUSE(out, "%s needs a location: the lines that show it, such as "
		              "src/mcp.c:120-140, which you read with git read.", name);
	struct tool_text found[4];
	size_t n = review_resolve(r, p, found);
	if (n == 1)
		p->path = found[0];
	if (n > 1 && (given || review_gated(state))) {
		/* Name up to three paths, then "and others". */
		size_t shown = n < 3 ? n : 3;
		bool ok = dstr_printf(out, "%.*s matches %.*s", (int)p->path.len, p->path.ptr,
		                      (int)found[0].len, found[0].ptr);
		for (size_t i = 1; ok && i < shown; ++i)
			ok = dstr_printf(out, "%s%.*s", i + 1 == shown && n == shown ? " and " : ", ",
			                 (int)found[i].len, found[i].ptr);
		if (ok && n > shown)
			ok = dstr_printf(out, " and others");
		return ok && dstr_printf(out, " in your read log; give \"location\" with the whole "
		                         "path.") ? EINVAL : ENOMEM;
	}
	uint32_t reach;
	struct review_file const *most;
	struct review_file const *f = review_covering(r, p, &reach, &most);
	if (reach >= p->last)
		*read = f;
	if (*read || !review_gated(state))
		return 0;
	int len = (int)p->path.len;
	if (most && most->total && most->total < p->last)
		return REFUSE(out, "%.*s has %" PRIu32 " line%s at %.*s, so line %" PRIu32
		              " is past its end; give a location within lines 1 to %" PRIu32 ".",
		              len, p->path.ptr, most->total, most->total == 1 ? "" : "s",
		              REVIEW_SHORT, most->commit, p->last, most->total);
	/* Choose the read to suggest. If f, the best covering file, can have
	 * line p->last, the read goes on at line reach + 1 at the commit of f.
	 * If f has fewer lines (shorter), no file that can have p->last holds
	 * p->first, so the read starts at p->first at the commit of most.
	 * Without f, it starts at p->first at p->prefix or HEAD. */
	bool shorter = f && f->total && f->total < p->last;
	struct review_file const *to = shorter ? most : f;
	bool one = p->first == p->last;
	struct tool_text rev = p->prefix.len ? p->prefix : TOOL_TEXT("HEAD");
	bool ok = dstr_printf(out, "%s needs the lines of its location in your read log, and "
	                      "%.*s%s%.*s line%s %" PRIu32, name, len, p->path.ptr,
	                      p->prefix.len ? " at " : "", (int)p->prefix.len, rev.ptr,
	                      one ? "" : "s", p->first) &&
	          (one || dstr_printf(out, "-%" PRIu32, p->last));
	if (ok && !f)
		ok = dstr_printf(out, " %s not in it.", one ? "is" : "are");
	else if (ok)
		ok = dstr_printf(out, " are not all in it: it has line%s %" PRIu32,
		                 reach > p->first ? "s" : "", p->first) &&
		     (reach == p->first || dstr_printf(out, "-%" PRIu32, reach)) &&
		     dstr_printf(out, " of them, read at %.*s", REVIEW_SHORT, f->commit) &&
		     (!shorter || dstr_printf(out, ", where the file has %" PRIu32 " line%s",
		                              f->total, f->total == 1 ? "" : "s")) &&
		     dstr_cat_n(out, ".", 1);
	/* The read comes in a later turn, and its lines become a new pending
	 * file. Estimate its charge from the file to read (to), or from a file
	 * with this path and a 1-byte repository name. */
	struct review_file const guess = { .repo = { .len = 1 }, .path = { .len = p->path.len } };
	if (r->size + review_file_charge(to ? to : &guess, 1) > REVIEW_MAX)
		return ok && (r->full ? dstr_printf(out, " The read log has been full since turn %"
		                                    PRIu32 ",", r->full)
		                      : dstr_printf(out, " The record is full,")) &&
		       dstr_printf(out, " so a read of them would not be logged: cite lines "
		                   "that the log has, or keep the finding a candidate.")
		       ? EINVAL : ENOMEM;
	if (to)
		rev = (struct tool_text){ to->commit, REVIEW_SHORT };
	bool rest = to && !shorter;
	char path[6 * REVIEW_WORD];
	size_t escaped = jb_escape(path, p->path.ptr, p->path.len);
	return ok && dstr_printf(out, " Read %s with the git tool's read command, with "
	                         "\"revisions\": [\"%.*s:%.*s\"] and \"line\": %" PRIu32 "%s. "
	                         "The log takes the lines of a read once you have seen them, so "
	                         "call again in a later turn.", rest ? "the rest" : "them",
	                         (int)rev.len, rev.ptr, (int)escaped, path,
	                         rest ? reach + 1 : p->first,
	                         to ? ", in as many calls as it takes"
	                         : n ? "" : ", where the path is the file's whole path from the top "
	                                    "of the repository") ? EINVAL : ENOMEM;
}

/* Returns 0 if a record of @p size bytes fits in REVIEW_MAX; otherwise
 * EINVAL with the refusal for the record @p r in @p out, or ENOMEM.
 * @p fixed is the size of the record with an empty claim and evidence.
 */
static int
review_fits (struct review const *r,
             size_t               size,
             size_t               fixed,
             struct dstr         *out)
{
	static_assert(REVIEW_MAX == 4 << 20, "the problem says 4 MiB");
	if (size <= REVIEW_MAX)
		return 0;
	if (fixed < REVIEW_MAX)
		return REFUSE(out, "The record is full: it holds at most 4 MiB. Give a claim and "
		              "evidence of at most %zu bytes together.", REVIEW_MAX - fixed);
	/* The smallest change is an update with 1 byte of evidence. Pending
	 * lines can free room: a compaction drops them, and the log may charge
	 * less for them. Without pending lines nothing frees room, and a new
	 * read cannot become pending, because it needs more room than a change
	 * (static_assert below). */
	static_assert(sizeof(struct review_change) < 2 * REVIEW_ITEM + 2, "a read needs more");
	return REFUSE(out, "The record is full: it holds at most 4 MiB and %s; keep further "
	              "findings in your notes or your summary.",
	              r->pending.n || r->size + sizeof(struct review_change) < REVIEW_MAX
	              ? "has no room for this change" : "takes no more changes in this ask call");
}

/* Write to @p o the end of an add or update result for the finding @p f.
 * If @p f is a candidate whose lines were not read, and @p r has
 * REVIEW_READS, ask for a git read. If @p cut is not 0, say which texts
 * lost their trailing notices.
 */
static void
review_out_tail (struct review_out           *o,
                 struct review const         *r,
                 struct review_finding const *f,
                 uint8_t                      cut)
{
	static struct tool_text const texts[] = {
		[CUT_CLAIM]                = NAME("the claim"),
		[CUT_EVIDENCE]             = NAME("the evidence"),
		[CUT_CLAIM | CUT_EVIDENCE] = NAME("the claim and the evidence"),
	};
	struct review_change const *c = &f->changes[f->n - 1];
	if (c->state == REVIEW_CANDIDATE && f->path.ptr && !*c->commit && r->flags & REVIEW_READS)
		PUT(o, " Read its lines with git read before you verify it.");
	if (!cut)
		return;
	PUT(o, " askai left out the [askai: ...] lines at the end of ");
	review_out_put(o, texts[cut].ptr, texts[cut].len);
	PUT(o, ". askai adds those lines to tool results, and they are not part of the "
	       "finding.");
}

#undef NAME

/* Add the finding that @p args describe to @p r, in turn @p turn. */
static int
review_add (struct review      *r,
            struct json_object *args,
            uint32_t            turn,
            review_kept_fn     *kept,
            struct dstr        *out)
{
	if (!jb_get(args, "claim"))
		return REFUSE(out, "add needs a claim: what you found.");
	struct review_args a;
	int e = review_args_get(&a, args, kept, out);
	if (e)
		return e;
	uint8_t state = a.state == REVIEW_STATES ? REVIEW_CANDIDATE : a.state;
	if (state == REVIEW_RETRACTED)
		return REFUSE(out, "add takes the state candidate, verified or disproved; "
		              "retract a finding with update.");
	struct review_file const *read;
	e = review_check_place(r, &a.place, state, true, &read, out);
	if (e)
		return e;
	if (review_gated(state) && !a.evidence.ptr)
		return REFUSE(out, "%s needs evidence: what you read that shows it%s.",
		              review_state_names[state].ptr,
		              state == REVIEW_DISPROVED ? " false" : "");
	struct review_finding f = { .first = a.place.first, .last = a.place.last };
	struct review_finding *all = util_room(r->findings, &r->cap_findings, r->n_findings + 1,
	                                       sizeof *all);
	if (all)
		r->findings = all;
	/* Allocate one change. review_update() grows the array. */
	f.changes = all ? malloc(sizeof *f.changes) : nullptr;
	f.cap = 1;
	e = f.changes ? 0 : ENOMEM;
	if (!e) {
		f.changes[0] = (struct review_change){ .turn = turn, .state = state };
		f.n = 1;
		e = review_text_set(&f.claim, a.claim.ptr, a.claim.len);
	}
	if (!e && a.place.path.ptr)
		e = review_text_set(&f.path, a.place.path.ptr, a.place.path.len);
	if (!e && a.evidence.ptr)
		e = review_text_set(&f.changes[0].evidence, a.evidence.ptr, a.evidence.len);
	size_t size = r->size + review_finding_charge(&f);
	if (!e)
		e = review_fits(r, size, size - a.claim.len - a.evidence.len, out);
	if (e) {
		review_finding_fini(&f);
		return e;
	}
	if (read)
		memcpy(f.changes[0].commit, read->commit, sizeof f.changes[0].commit);
	r->findings[r->n_findings++] = f;
	r->size = size;
	struct review_out o = { .ds = out, .start = dstr_len(out), .room = SIZE_MAX };
	PUT(&o, "Added finding ");
	review_out_number(&o, r->n_findings);
	PUT(&o, ": ");
	review_out_status(&o, &f);
	PUT(&o, ".");
	review_out_tail(&o, r, &f, a.cut);
	return review_out_end(&o, false);
}

/* Store in @p *id the number of the finding that the id member of @p args
 * gives. Returns 0, or EINVAL or ENOMEM with the refusal in @p out.
 */
static int
review_id (struct review const *r,
           struct json_object  *args,
           size_t              *id,
           struct dstr         *out)
{
	int64_t n = jb_int(jb_get(args, "id"), 0);
	if (n < 1)
		return REFUSE(out, "The id argument must be the number of a finding, such as 3.");
	size_t k = r->n_findings;
	if ((uint64_t)n <= k) {
		*id = (size_t)n;
		return 0;
	}
	/* %.0zu writes nothing for 0. */
	return REFUSE(out, "There is no finding %" PRId64 "; the record has %s%.0zu.", n,
	              !k ? "no findings yet" : k == 1 ? "only finding " : "findings 1 to ", k);
}

/* Update a finding of @p r as @p args describe, in turn @p turn. */
static int
review_update (struct review      *r,
               struct json_object *args,
               uint32_t            turn,
               review_kept_fn     *kept,
               struct dstr        *out)
{
	size_t id;
	int e = review_id(r, args, &id, out);
	if (e)
		return e;
	if (!jb_get(args, "state") || !jb_get(args, "evidence"))
		return REFUSE(out, "update needs state and evidence: what shows the new state, "
		              "or why you retract the finding.");
	struct review_args a;
	e = review_args_get(&a, args, kept, out);
	if (e)
		return e;
	struct review_finding *f = &r->findings[id - 1];
	bool given = a.place.path.ptr;
	struct review_place p = given ? a.place : (struct review_place){
		.path = { f->path.ptr, f->path.len }, .first = f->first, .last = f->last,
	};
	struct review_file const *read;
	e = review_check_place(r, &p, a.state, given, &read, out);
	if (e)
		return e;
	/* If a short path now resolves, repath stores the full path. That is a
	 * new location (moved) only if the call gave a location. */
	bool repath = p.path.ptr && !review_text_is(&f->path, p.path.ptr, p.path.len);
	bool moved = given && (repath || f->first != p.first || f->last != p.last);
	bool renamed = a.claim.ptr && !review_text_is(&f->claim, a.claim.ptr, a.claim.len);
	/* If the location moves, keep the old one as path:lines text, which is
	 * empty if the finding had none. */
	struct dstr was = DSTR_INIT;
	struct review_out w = { .ds = &was, .room = SIZE_MAX };
	if (moved && f->path.ptr) {
		review_out_text(&w, &f->path);
		PUT(&w, ":");
		review_out_lines(&w, f->first, f->last);
	}
	struct review_change c = { .turn = turn, .state = a.state };
	struct review_text claim = {};
	struct review_text path = {};
	struct review_change *changes = w.failed ? nullptr
	                              : util_room(f->changes, &f->cap, f->n + 1, sizeof *changes);
	if (changes)
		f->changes = changes;
	e = changes ? 0 : ENOMEM;
	if (!e)
		e = review_text_set(&c.evidence, a.evidence.ptr, a.evidence.len);
	if (!e && renamed)
		e = review_text_set(&claim, a.claim.ptr, a.claim.len);
	if (!e && moved)
		e = review_text_set(&c.location, dstr_get(&was), dstr_len(&was));
	if (!e && repath)
		e = review_text_set(&path, p.path.ptr, p.path.len);
	dstr_cleanup(&was);
	/* c takes the old claim, and the finding takes the new claim and path.
	 * The older changes are charged already, so the size grows by the
	 * charge of the finding with its new texts and only c, minus its charge
	 * with its old texts and no changes. */
	if (renamed)
		c.claim = f->claim;
	struct review_finding const before = { .claim = f->claim, .path = f->path };
	struct review_finding const after = { .claim = renamed ? claim : f->claim,
	                                      .path = repath ? path : f->path,
	                                      .changes = &c, .n = 1 };
	size_t size = r->size - review_finding_charge(&before) + review_finding_charge(&after);
	if (!e)
		e = review_fits(r, size, size - c.evidence.len - claim.len, out);
	if (e) {
		/* c.claim is still f->claim, so it is not freed. */
		review_text_fini(&c.evidence);
		review_text_fini(&c.location);
		review_text_fini(&claim);
		review_text_fini(&path);
		return e;
	}
	uint8_t old = f->changes[f->n - 1].state;
	if (read)
		memcpy(c.commit, read->commit, sizeof c.commit);
	if (renamed)
		f->claim = claim;
	if (repath) {
		review_text_fini(&f->path);
		f->path = path;
	}
	f->first = p.first;
	f->last = p.last;
	f->changes[f->n++] = c;
	r->size = size;
	struct review_out o = { .ds = out, .start = dstr_len(out), .room = SIZE_MAX };
	PUT(&o, "Finding ");
	review_out_number(&o, id);
	if (old == a.state) {
		PUT(&o, " stays ");
		review_out_status(&o, f);
		PUT(&o, "; the new evidence is in its history.");
	} else {
		PUT(&o, " is now ");
		review_out_status(&o, f);
		PUT(&o, "; it was ");
		review_out_state(&o, old);
		PUT(&o, ".");
	}
	if (renamed && moved)
		PUT(&o, " Its old claim and location are in its history.");
	else if (renamed)
		PUT(&o, " Its old claim is in its history.");
	else if (moved)
		PUT(&o, " Its old location is in its history.");
	review_out_tail(&o, r, f, a.cut);
	return review_out_end(&o, false);
}

#undef PUT

int
review_call (struct review      *r,
             struct json_object *args,
             uint32_t            turn,
             review_kept_fn     *kept,
             size_t              from,
             size_t              most,
             struct dstr        *out)
{
	/* The members that each action takes. */
	static char const *const keys[REVIEW_ACTIONS][7] = {
		[REVIEW_ADD]    = { "action", "claim", "location", "state", "evidence" },
		[REVIEW_UPDATE] = { "action", "id", "state", "evidence", "claim", "location" },
		[REVIEW_SHOW]   = { "action", "id", "offset" },
	};
	static char const *const takes[REVIEW_ACTIONS] = {
		[REVIEW_ADD]    = "add takes claim, location, state and evidence",
		[REVIEW_UPDATE] = "update takes id, state, evidence, claim and location",
		[REVIEW_SHOW]   = "show takes id and offset",
	};
	if (!r || !out)
		return EFAULT;
	/* jb_get() returns nullptr for args that are not an object. */
	size_t a = review_name_of(jb_get(args, "action"), review_action_names, REVIEW_ACTIONS);
	if (a == REVIEW_ACTIONS)
		return REFUSE(out, "The action argument must be add, update or show.");
	struct json_object_iter it;
	json_object_object_foreachC(args, it) {
		size_t k = 0;
		while (keys[a][k] && strcmp(it.key, keys[a][k]))
			++k;
		if (!keys[a][k])
			return REFUSE(out, "%s, not %.*s.", takes[a],
			              (int)jb_utf8(it.key, strnlen(it.key, 64)), it.key);
	}
	if (a == REVIEW_ADD)
		return review_add(r, args, turn, kept, out);
	if (a == REVIEW_UPDATE)
		return review_update(r, args, turn, kept, out);
	size_t id = 0;
	int e = jb_get(args, "id") ? review_id(r, args, &id, out) : 0;
	if (e)
		return e;
	/* The room has a byte more for the final newline (review_out_end()). */
	struct review_out o = { .ds = out, .start = dstr_len(out), .skip = from,
	                        .room = most < SIZE_MAX ? most + 1 : most };
	if (id)
		review_out_history(&o, r, id);
	else
		review_out_record(&o, r);
	return review_out_end(&o, false);
}

#undef REFUSE

/* Returns the file @p f as JSON, or nullptr if out of memory. */
static struct json_object *
review_file_json (struct review_file const *f)
{
	/* Each range is three numbers: first, last and turn. */
	struct json_object *lines = json_object_new_array();
	for (size_t i = 0; lines && i < f->n; ++i) {
		struct review_range const *g = &f->ranges[i];
		lines = jb_push_or_free(lines, review_int(g->first));
		lines = jb_push_or_free(lines, review_int(g->last));
		lines = jb_push_or_free(lines, review_int(g->turn));
	}
	return jb_object("repo", review_string(&f->repo),
	                 "commit", json_object_new_string(f->commit),
	                 "path", review_string(&f->path),
	                 "total", review_int(f->total),
	                 "lines", lines,
	                 nullptr);
}

/* Returns the files of @p list as a JSON array, or nullptr if out of memory. */
static struct json_object *
review_files_json (struct review_files const *list)
{
	struct json_object *files = json_object_new_array();
	for (size_t i = 0; files && i < list->n; ++i)
		files = jb_push_or_free(files, review_file_json(&list->files[i]));
	return files;
}

/* Returns the change @p c as JSON, or nullptr if out of memory. */
static struct json_object *
review_change_json (struct review_change const *c)
{
	struct json_object *obj = jb_object(
		"turn", review_int(c->turn),
		"state", json_object_new_string(review_state_names[c->state].ptr),
		"commit", json_object_new_string(c->commit),
		"evidence", review_string(&c->evidence),
		nullptr);
	if (c->claim.ptr)
		obj = jb_set_or_free(obj, "claim", review_string(&c->claim));
	if (c->location.ptr)
		obj = jb_set_or_free(obj, "location", review_string(&c->location));
	return obj;
}

/* Returns the finding @p f as JSON, or nullptr if out of memory. */
static struct json_object *
review_finding_json (struct review_finding const *f)
{
	struct json_object *history = json_object_new_array();
	for (size_t i = 0; history && i < f->n; ++i)
		history = jb_push_or_free(history, review_change_json(&f->changes[i]));
	struct json_object *obj = jb_object(
		"claim", review_string(&f->claim),
		"first", review_int(f->first),
		"last", review_int(f->last),
		"history", history,
		nullptr);
	return f->path.ptr ? jb_set_or_free(obj, "path", review_string(&f->path)) : obj;
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
	                                    "full", review_int(r->full),
	                                    "findings", findings, nullptr);
	*e = obj ? 0 : ENOMEM;
	return obj;
}

/* Append the file in @p obj to @p list, and add its charge to r->size.
 * Its ranges must fit in the room that REVIEW_MAX leaves. Returns 0,
 * EINVAL or ENOMEM.
 */
static int
review_file_restore (struct review            *r,
                     struct review_files      *list,
                     struct json_object const *obj)
{
	struct tool_text repo;
	struct tool_text commit;
	struct tool_text path;
	uint64_t total;
	struct json_object const *lines = jb_get(obj, "lines");
	size_t n = jb_len(lines);
	if (!review_get_text(obj, "repo", &repo) || !review_word(repo) ||
	    !review_get_text(obj, "commit", &commit) || !util_is_oid(commit.ptr, commit.len) ||
	    !review_get_text(obj, "path", &path) || !review_word(path) ||
	    !review_get_number(obj, "total", UINT32_MAX, &total) || !n || n % 3 ||
	    n / 3 > (REVIEW_MAX - r->size) / REVIEW_ITEM)
		return EINVAL;
	struct review_file f = { .cap = n / 3, .total = (uint32_t)total };
	memcpy(f.commit, commit.ptr, commit.len);
	f.ranges = calloc(f.cap, sizeof *f.ranges);
	int e = !f.ranges ? ENOMEM : review_text_set(&f.repo, repo.ptr, repo.len);
	if (!e)
		e = review_text_set(&f.path, path.ptr, path.len);
	/* Each range is first, last and turn. The ranges must be sorted, not
	 * overlap and lie within the file, and adjoining ranges must have
	 * different turns. */
	for (uint64_t next = 1; !e && f.n < f.cap; ++f.n) {
		int64_t first = jb_int(jb_at(lines, 3 * f.n), 0);
		int64_t last = jb_int(jb_at(lines, 3 * f.n + 1), 0);
		int64_t turn = jb_int(jb_at(lines, 3 * f.n + 2), -1);
		if (first < 1 || (uint64_t)first < next || last < first || last > UINT32_MAX ||
		    (f.total && last > f.total) || turn < 0 || turn > UINT32_MAX ||
		    (f.n && (uint64_t)first == next && turn == f.ranges[f.n - 1].turn))
			e = EINVAL;
		f.ranges[f.n] = (struct review_range){ (uint32_t)first, (uint32_t)last,
		                                       (uint32_t)turn };
		next = (uint64_t)last + 1;
	}
	if (!e)
		e = review_files_insert(list, &f, list->n);
	if (e) {
		review_file_fini(&f);
		return e;
	}
	r->size += review_file_charge(&f, f.n);
	return 0;
}

/* Compare @p a and @p b byte by byte, as memcmp() does. If one text is a
 * prefix of the other, the shorter text comes first.
 */
static int
review_text_cmp (struct review_text const *a,
                 struct review_text const *b)
{
	int c = memcmp(a->ptr, b->ptr, a->len < b->len ? a->len : b->len);
	return c ? c : (a->len > b->len) - (a->len < b->len);
}

/* Order the files that @p a and @p b point to by repository, commit and
 * path, for qsort().
 */
static int
review_file_cmp (void const *a,
                 void const *b)
{
	struct review_file const *f = *(struct review_file const *const *)a;
	struct review_file const *g = *(struct review_file const *const *)b;
	int c = review_text_cmp(&f->repo, &g->repo);
	c = c ? c : strcmp(f->commit, g->commit);
	return c ? c : review_text_cmp(&f->path, &g->path);
}

/* Returns pointers to the files of @p list, sorted by repository, commit
 * and path, or nullptr with @p *e 0 if @p list is empty. On failure
 * returns nullptr with @p *e EINVAL if a file appears twice or the files of
 * one repository and commit are not next to each other, or ENOMEM.
 */
static struct review_file const **
review_files_sorted (struct review_files const *list,
                     int                       *e)
{
	*e = 0;
	if (!list->n)
		return nullptr;
	struct review_file const **by = reallocarray(nullptr, list->n, sizeof *by);
	if (!by) {
		*e = ENOMEM;
		return nullptr;
	}
	for (size_t i = 0; i < list->n; ++i)
		by[i] = &list->files[i];
	qsort(by, list->n, sizeof *by, review_file_cmp);
	/* The files of each repository and commit are next to each other if
	 * @p list has as many runs of them as there are groups. */
	size_t runs = 1;
	size_t groups = 1;
	for (size_t i = 1; !*e && i < list->n; ++i) {
		runs += !review_file_with(&list->files[i - 1], &list->files[i]);
		groups += !review_file_with(by[i - 1], by[i]);
		*e = review_file_cmp(&by[i - 1], &by[i]) ? 0 : EINVAL;
	}
	*e = *e || runs == groups ? *e : EINVAL;
	if (*e) {
		free(by);
		by = nullptr;
	}
	return by;
}

/* Check the @p n_pending files @p pending against the @p n_log files @p log.
 * Both arrays are sorted as review_files_sorted() sorts them. If two files
 * have the same repository, commit and path, their totals must be equal
 * unless one is 0, and no range of either may go past a nonzero total.
 * Returns 0 or EINVAL.
 */
static int
review_files_agree (struct review_file const *const *log,
                    size_t                           n_log,
                    struct review_file const *const *pending,
                    size_t                           n_pending)
{
	for (size_t i = 0, k = 0; i < n_log && k < n_pending;) {
		int c = review_file_cmp(&log[i], &pending[k]);
		i += c <= 0;
		k += c >= 0;
		if (c)
			continue;
		struct review_file const *f = log[i - 1];
		struct review_file const *g = pending[k - 1];
		uint32_t total = f->total > g->total ? f->total : g->total;
		if ((f->total && g->total && f->total != g->total) ||
		    (total && (f->ranges[f->n - 1].last > total ||
		               g->ranges[g->n - 1].last > total)))
			return EINVAL;
	}
	return 0;
}

/* Copy into @p t the text of the member @p key of @p obj, if it has one.
 * The text must be UTF-8, and not empty if @p empty is false. Returns 0,
 * EINVAL or ENOMEM.
 */
static int
review_text_restore (struct review_text       *t,
                     struct json_object const *obj,
                     char const               *key,
                     bool                      empty)
{
	struct tool_text s;
	if (!jb_get(obj, key))
		return 0;
	if (!review_get_text(obj, key, &s) || (!empty && !s.len) ||
	    jb_utf8(s.ptr, s.len) != s.len)
		return EINVAL;
	return review_text_set(t, s.ptr, s.len);
}

/* Restore the change in @p obj into @p c. Returns 0, EINVAL or ENOMEM. */
static int
review_change_restore (struct review_change     *c,
                       struct json_object const *obj)
{
	struct tool_text commit;
	uint64_t turn;
	size_t state = review_name_of(jb_get(obj, "state"), review_state_names, REVIEW_STATES);
	if (!review_get_number(obj, "turn", UINT32_MAX, &turn) || state == REVIEW_STATES ||
	    !review_get_text(obj, "commit", &commit) ||
	    (commit.len && !util_is_oid(commit.ptr, commit.len)) ||
	    !jb_str(jb_get(obj, "evidence"), &(size_t){}))
		return EINVAL;
	*c = (struct review_change){ .turn = (uint32_t)turn, .state = (uint8_t)state };
	memcpy(c->commit, commit.ptr, commit.len);
	int e = review_text_restore(&c->evidence, obj, "evidence", true);
	if (!e)
		e = review_text_restore(&c->claim, obj, "claim", false);
	if (!e)
		e = review_text_restore(&c->location, obj, "location", true);
	/* A verified or disproved change has evidence and a commit. */
	if (!e && review_gated(c->state) && (!c->evidence.len || !*c->commit))
		e = EINVAL;
	return e;
}

/* Append the finding in @p obj to @p r. Returns 0, EINVAL or ENOMEM. */
static int
review_finding_restore (struct review            *r,
                        struct json_object const *obj)
{
	struct tool_text path = {};
	uint64_t first;
	uint64_t last;
	struct json_object const *history = jb_get(obj, "history");
	size_t n = jb_len(history);
	bool located = jb_get(obj, "path");
	if ((located && (!review_get_text(obj, "path", &path) || !review_word(path))) ||
	    !review_get_number(obj, "first", UINT32_MAX, &first) ||
	    !review_get_number(obj, "last", UINT32_MAX, &last) ||
	    (located ? !first || last < first : first || last) || !n ||
	    n > (REVIEW_MAX - r->size) / sizeof(struct review_change))
		return EINVAL;
	struct review_finding f = { .cap = n, .first = (uint32_t)first, .last = (uint32_t)last };
	struct review_finding *all = util_room(r->findings, &r->cap_findings, r->n_findings + 1,
	                                       sizeof *all);
	if (all)
		r->findings = all;
	f.changes = all ? calloc(n, sizeof *f.changes) : nullptr;
	int e = !f.changes ? ENOMEM : review_text_restore(&f.claim, obj, "claim", false);
	if (!e && !f.claim.ptr)
		e = EINVAL;
	if (!e && located)
		e = review_text_set(&f.path, path.ptr, path.len);
	/* A finding never loses its location, so one without a location was
	 * never verified or disproved. */
	for (; !e && f.n < n; ++f.n) {
		e = review_change_restore(&f.changes[f.n], jb_at(history, f.n));
		if (!e && !located && review_gated(f.changes[f.n].state))
			e = EINVAL;
	}
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
	static char const *const keys[] = { "reads", "pending", "findings" };
	uint64_t full;
	int e = review_get_number(obj, "full", UINT32_MAX, &full) ? 0 : EINVAL;
	struct review got = { .full = (uint32_t)full, .flags = r->flags };
	struct review_files *lists[] = { &got.log, &got.pending, nullptr };
	/* The restore stops once the record is past REVIEW_MAX. */
	for (size_t k = 0; !e && k < sizeof keys / sizeof *keys; ++k) {
		struct json_object const *a = jb_get(obj, keys[k]);
		e = json_object_is_type(a, json_type_array) ? 0 : EINVAL;
		for (size_t i = 0, n = jb_len(a); !e && got.size <= REVIEW_MAX && i < n; ++i)
			e = lists[k] ? review_file_restore(&got, lists[k], jb_at(a, i))
			             : review_finding_restore(&got, jb_at(a, i));
	}
	if (!e && got.size > REVIEW_MAX)
		e = EINVAL;
	struct review_file const **logged = e ? nullptr : review_files_sorted(&got.log, &e);
	struct review_file const **waiting = e ? nullptr : review_files_sorted(&got.pending, &e);
	if (!e)
		e = review_files_agree(logged, got.log.n, waiting, got.pending.n);
	free(logged);
	logged = nullptr;
	free(waiting);
	waiting = nullptr;
	if (e) {
		review_fini(&got);
		return e;
	}
	review_fini(r);
	*r = got;
	return 0;
}
