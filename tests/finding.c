/** @file Unit tests of the findings (src/finding.c): the IDs, the paths of
 *  the files, the changes and their rules, the file of version 1, and the
 *  texts of list, show and the notice of the prompt.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "dstr.h"
#include "evidence.h"
#include "finding.h"
#include "finding_id.h"
#include "hexid.h"

/* Two conversations and an ask call. */
#define CONV "68177013a5c94f0e8b1d2c3e4f506172"
#define ELSE "0123456789abcdef0123456789abcdef"
#define ASK  "9f3c2a71e0b14c55a1d2e3f405162738"

/* Check that the @p len bytes at @p got are the string @p want. */
#define SAME(got, len, want) same(__LINE__, got, len, want)

static void
same (int         line,
      void const *got,
      size_t      len,
      char const *want)
{
	size_t const n = strlen(want);
	if (got && len == n && !memcmp(got, want, n))
		return;
	++failures;
	fprintf(stderr, "%s:%d: got\n%.*s\nwant\n%s\n", __FILE__, line, (int)len,
	        got ? (char const *)got : "", want);
}

/* Returns the ID of the finding @p number of @p author, or of its item
 * @p item if @p item is not 0.
 */
static struct finding_id
id_of (char const *author,
       uint32_t    number,
       uint32_t    item)
{
	struct finding_id id = { .number = number, .item = item };
	snprintf(id.author, sizeof id.author, "%s", author);
	return id;
}

/* Check that finding_id_parse() reads @p text as the ID @p want, and that
 * finding_id_put() writes that ID as @p text again.
 */
static void
parsed (char const        *text,
        struct finding_id  want,
        int                line)
{
	struct finding_id id = id_of("x", 9, 9);
	char back[FINDING_ID_TEXT + 1];
	memset(back, 'y', sizeof back);
	int const e = finding_id_parse(&id, cstr_view(text));
	struct cstr_view const put = finding_id_put(&want, back);
	/* The bytes of author after the name are 0, so that two IDs of the
	 * same text are the same bytes. */
	if (!e && !memcmp(id.author, want.author, sizeof id.author) && id.number == want.number &&
	    id.item == want.item && finding_id_equal(&id, &want) && put.data == back &&
	    put.len == strlen(text) && !strcmp(back, text) && back[FINDING_ID_TEXT] == 'y')
		return;
	++failures;
	fprintf(stderr, "%s:%d: %s: error %d, got %s#%u/%u, put %s\n", __FILE__, line, text, e,
	        id.author, id.number, id.item, back);
}

/* finding_id_parse() reads the IDs of findings and of items and refuses
 * any other text, finding_id_put() writes them, finding_id_equal() compares
 * them, and finding_id_without_item() returns the ID of the finding of an
 * item.
 */
static void
test_id (void)
{
	parsed("bro#5", id_of("bro", 5, 0), __LINE__);
	parsed("bro#5/2", id_of("bro", 5, 2), __LINE__);
	parsed("first.last@ad#4294967295", id_of("first.last@ad", UINT32_MAX, 0), __LINE__);
	parsed("first.last@ad#12/4294967295", id_of("first.last@ad", 12, UINT32_MAX), __LINE__);
	parsed("c#1", id_of("c", 1, 0), __LINE__);
	parsed("c7x#10/10", id_of("c7x", 10, 10), __LINE__);
	/* The longest ID fills FINDING_ID_TEXT bytes with its NUL. */
	char longest[FINDING_ID_TEXT];
	memset(longest, 'a', UTIL_WORD_MAX);
	snprintf(longest + UTIL_WORD_MAX, sizeof longest - UTIL_WORD_MAX, "#4294967295/4294967295");
	CHECK(strlen(longest) == FINDING_ID_TEXT - 1);
	struct finding_id big = { .number = UINT32_MAX, .item = UINT32_MAX };
	memset(big.author, 'a', UTIL_WORD_MAX);
	parsed(longest, big, __LINE__);
	static_assert(sizeof big.author == UTIL_WORD_MAX + 1, "a member name and its NUL");

	static char const *const bad[] = {
		"", "bro", "bro#", "bro#0", "bro#05", "bro#4294967296", "#5", "c7#5", ".bro#5",
		"bro#5 ", " bro#5", "bro#-1", "bro#+1", "b ro#5", "bro#5#1", "bro##5", "bro#5x",
		"bro#5/", "bro#5/0", "bro#5/01", "bro#5/4294967296", "bro#5/1/1", "bro#5/1 ",
		"bro#5/-1", "bro#5/x", "bro#/1", "bro#0/1", "bro#05/1", "bro#5:1", "bro#5/1:1",
		"bro#5/1:1-2", "bro/1", "bro/1#5", "/1", "#", "/", "#/", "#5/1", "c7#5/1", "bro#5//1",
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
		struct finding_id id = id_of("x", 9, 9);
		if (finding_id_parse(&id, cstr_view(bad[i])) == EINVAL && !*id.author && !id.number &&
		    !id.item)
			continue;
		++failures;
		fprintf(stderr, "%s:%d: accepted %s\n", __FILE__, __LINE__, bad[i]);
	}
	/* A name of UTIL_WORD_MAX + 1 characters. */
	struct finding_id id = id_of("x", 9, 9);
	char name[UTIL_WORD_MAX + sizeof "a#5"];
	memset(name, 'a', UTIL_WORD_MAX + 1);
	memcpy(name + UTIL_WORD_MAX + 1, "#5", sizeof "#5");
	CHECK(finding_id_parse(&id, cstr_view(name)) == EINVAL && !*id.author);
	CHECK(!finding_id_parse(&id, cstr_view(name + 1)) && strlen(id.author) == UTIL_WORD_MAX);
	/* A view without bytes, and a text with a NUL inside. */
	CHECK(finding_id_parse(&id, (struct cstr_view){}) == EINVAL && !*id.author);
	CHECK(finding_id_parse(&id, (struct cstr_view){ "bro#5\0/1", 8 }) == EINVAL);
	CHECK(finding_id_parse(&id, (struct cstr_view){ "bro\0#5", 6 }) == EINVAL);
	/* The text ends where the view ends. */
	CHECK(!finding_id_parse(&id, (struct cstr_view){ "bro#51/23x", 6 }) && id.number == 51 &&
	      !id.item);
	CHECK(!finding_id_parse(&id, (struct cstr_view){ "bro#51/23x", 9 }) && id.number == 51 &&
	      id.item == 23);

	/* finding_id_equal() compares the author and both numbers. */
	struct finding_id const a = id_of("bro", 5, 2);
	struct finding_id const same = id_of("bro", 5, 2);
	struct finding_id const none = {};
	CHECK(finding_id_equal(&a, &a) && finding_id_equal(&a, &same) &&
	      finding_id_equal(&none, &none));
	struct finding_id const other[] = {
		id_of("bro", 5, 0), id_of("bro", 5, 3), id_of("bro", 6, 2), id_of("cuh", 5, 2),
		id_of("br", 5, 2), id_of("bro1", 5, 2), id_of("Bro", 5, 2), {},
	};
	for (size_t i = 0; i < sizeof other / sizeof *other; ++i)
		CHECK(!finding_id_equal(&a, &other[i]) && !finding_id_equal(&other[i], &a));

	/* The ID of the finding of an item, and of a finding. */
	struct finding_id const of = finding_id_without_item(&a);
	struct finding_id const whole = id_of("bro", 5, 0);
	CHECK(finding_id_equal(&of, &whole) && !of.item && a.item == 2);
	struct finding_id const again = finding_id_without_item(&of);
	CHECK(finding_id_equal(&again, &whole));

	/* finding_id_put() returns the view of the text. */
	char text[FINDING_ID_TEXT];
	struct cstr_view const v = finding_id_put(&a, text);
	CHECK(v.data == text && v.len == 7 && !strcmp(text, "bro#5/2"));
	CHECK(finding_id_put(&whole, text).len == 5 && !strcmp(text, "bro#5"));
	static_assert(FINDING_ID_TEXT == UTIL_WORD_MAX + sizeof "#4294967295/4294967295",
	              "a name, the numbers and the NUL");
}

/* finding_path() and finding_path_number() name the files. */
static void
test_paths (void)
{
	char path[FINDING_FILE];
	struct cstr_view p = finding_path(5, path);
	CHECK(!strcmp(path, "findings/0/5.json") && p.data == path && p.len == strlen(path) &&
	      finding_path_number(p) == 5);
	p = finding_path(100, path);
	CHECK(!strcmp(path, "findings/1/100.json") && p.len == strlen(path) &&
	      finding_path_number(p) == 100);
	p = finding_path(UINT32_MAX, path);
	CHECK(!strcmp(path, "findings/42949672/4294967295.json") &&
	      finding_path_number(p) == UINT32_MAX && p.len == FINDING_FILE - 1 &&
	      strlen(path) == FINDING_FILE - 1);
	p = finding_path(4294967200, path);
	CHECK(!strcmp(path, "findings/42949672/4294967200.json") && p.len == strlen(path) &&
	      finding_path_number(p) == 4294967200);
	p = finding_path(99, path);
	CHECK(finding_path_number(p) == 99 && p.len == strlen(path));
	char const *const paths[] = { "findings/1/5.json", "findings/0/05.json", "findings/0/0.json",
	                              "archive/0/5.json", "findings/0/5.txt", "x/findings/0/5.json",
	                              "findings/00/5.json", ".json", "", "findings/0/.json",
	                              "findings/.json", "findings//5.json", "findings/0/5/5.json",
	                              "findings/0/5.json/", "findings/2/199.json",
	                              "findings/01/100.json", "findings/1/100.jsonx",
	                              "findings/0/-5.json", "findings/0/+5.json",
	                              "findings/42949672/4294967296.json", "findings/1/1 00.json",
	                              "Findings/0/5.json", "findings/0/5.JSON" };
	for (size_t i = 0; i < sizeof paths / sizeof *paths; ++i)
		CHECK(!finding_path_number(cstr_view(paths[i])));
}

/* Returns an evidence item of the call @p call, of the tool @p tool. */
static struct evidence
item_of (uint32_t    call,
         char const *tool)
{
	struct evidence e = evidence();
	e.call = call;
	e.turn = call;
	e.output = 100 + call;
	e.flags = EVIDENCE_REQUEST;
	e.request.bytes = 10;
	e.result.bytes = 100 + call;
	memset(e.result.sha256, (int)call, sizeof e.result.sha256);
	CHECK(!hexid_init(&e.ask, CSTR_VIEW(ASK)));
	CHECK(!cstr_view_store(cstr_view(tool), &e.tool));
	CHECK(!cstr_view_store(CSTR_VIEW("call_1_0"), &e.call_id));
	return e;
}

/* Returns a change to @p state with the reason @p reason, in the turn
 * @p turn, and @p items evidence items of the calls from @p call on.
 */
static struct finding_change
change_of (enum finding_state  state,
           char const         *reason,
           uint32_t            turn,
           uint32_t            call,
           size_t              items)
{
	struct finding_change c = finding_change();
	c.state = state;
	c.turn = turn;
	c.call = turn + 1;
	CHECK(!hexid_init(&c.ask, CSTR_VIEW(ASK)));
	strcpy(c.asked_by, "imaami");
	CHECK(!cstr_view_store(CSTR_VIEW("0.9.0-412-gd02cc233"), &c.askai));
	if (*reason)
		CHECK(!cstr_view_store(cstr_view(reason), &c.reason));
	CHECK(!util_grow(c.evidence.items, c.evidence.cap, items ? items : 1));
	for (size_t i = 0; i < items; ++i)
		c.evidence.items[c.evidence.n++] = item_of(call + (uint32_t)i, "git");
	return c;
}

/* Adds the change @p c to @p f as its last change, with its evidence items
 * numbered after the items of @p f, as findings_commit() numbers them, and
 * moves the storage of @p c into @p f.
 */
static void
push (struct finding        *f,
      struct finding_change *c)
{
	uint32_t items = f->changes.n ? f->changes.items[f->changes.n - 1].items : 0;
	for (size_t i = 0; i < c->evidence.n; ++i)
		c->evidence.items[i].item = ++items;
	c->items = items;
	CHECK(!util_grow(f->changes.items, f->changes.cap, f->changes.n + 1));
	f->changes.items[f->changes.n++] = *c;
	*c = finding_change();
}

/* Returns the finding of the file that finding_render() writes for @p f,
 * the change @p c and the claim @p claim.
 */
static struct finding
rendered (struct finding const        *f,
          struct finding_change const *c,
          char const                  *claim)
{
	struct dstr text = DSTR_INIT;
	CHECK(!finding_render(f, c, cstr_view(claim), &text));
	struct finding back = finding();
	CHECK(!finding_parse(&back, cstr_view_dstr(&text)));
	dstr_cleanup(&text);
	return back;
}

/* finding_check() checks the rules of each state, and finding_render()
 * writes a new change after the changes of a finding, with its claim
 * correction, and changes nothing of the finding.
 */
static void
test_changes (void)
{
	char why[256] = "";
	struct finding_change c = change_of(FINDING_CANDIDATE, "", 1, 1, 0);
	CHECK(!finding_check(&c, why, sizeof why));
	c.state = FINDING_VERIFIED;
	CHECK(finding_check(&c, why, sizeof why) == EINVAL);
	CHECK(!strcmp(why, "verified needs at least one evidence item besides external."));
	c.state = FINDING_DISPROVED;
	CHECK(finding_check(&c, why, sizeof why) == EINVAL);
	CHECK(!strcmp(why, "disproved needs at least one evidence item besides external."));
	c.evidence.items[c.evidence.n++] = item_of(3, "git");
	CHECK(finding_check(&c, why, sizeof why) == EINVAL);
	CHECK(!strcmp(why, "disproved needs reason: why the evidence shows the state."));
	CHECK(!cstr_view_store(CSTR_VIEW("Call c3 shows it."), &c.reason));
	CHECK(!finding_check(&c, why, sizeof why));
	finding_change_fini(&c);
	c = change_of(FINDING_RETRACTED, "", 1, 1, 0);
	CHECK(finding_check(&c, why, sizeof why) == EINVAL);
	CHECK(!strcmp(why, "retracted needs reason: why you retract the finding."));
	CHECK(!cstr_view_store(CSTR_VIEW("Wrong."), &c.reason));
	CHECK(!finding_check(&c, why, sizeof why));
	finding_change_fini(&c);

	struct finding f = finding();
	f.id = id_of("bro", 5, 0);
	CHECK(!cstr_view_store(CSTR_VIEW("First claim."), &f.claim));
	c = change_of(FINDING_CANDIDATE, "Looks so.", 2, 4, 2);
	c.evidence.items[0].item = 1;
	c.evidence.items[1].item = 2;
	c.items = 2;
	struct finding back = rendered(&f, &c, "");
	CHECK(back.changes.n == 1 && finding_state(&back) == FINDING_CANDIDATE &&
	      back.changes.items[0].items == 2 &&
	      back.changes.items[0].evidence.items[0].item == 1 &&
	      back.changes.items[0].evidence.items[1].item == 2 &&
	      !dstr_len(&back.changes.items[0].claim_before));
	SAME(dstr_get(&back.claim), dstr_len(&back.claim), "First claim.");
	SAME(dstr_get(&back.changes.items[0].askai), dstr_len(&back.changes.items[0].askai),
	     "0.9.0-412-gd02cc233");
	/* f and c stay as they were. */
	CHECK(!f.changes.n && c.evidence.n == 2 && c.evidence.items[1].item == 2 && c.items == 2);
	finding_fini(&back);
	push(&f, &c);
	CHECK(f.changes.n == 1 && f.changes.items[0].items == 2 && !c.evidence.n);
	/* A new claim is the claim of the file, and the claim before of the
	 * change is the claim of f. */
	c = change_of(FINDING_VERIFIED, "Call c9 shows it.", 4, 9, 1);
	c.evidence.items[0].item = 3;
	c.items = 3;
	back = rendered(&f, &c, "Second claim.");
	CHECK(back.changes.n == 2 && finding_state(&back) == FINDING_VERIFIED &&
	      back.changes.items[1].items == 3 && back.changes.items[1].evidence.items[0].item == 3 &&
	      !dstr_len(&back.changes.items[0].claim_before));
	/* finding_item() finds each item in its change. */
	CHECK(finding_item(&back, 1) == &back.changes.items[0].evidence.items[0] &&
	      finding_item(&back, 2) == &back.changes.items[0].evidence.items[1] &&
	      finding_item(&back, 3) == &back.changes.items[1].evidence.items[0] &&
	      !finding_item(&back, 0) && !finding_item(&back, 4) && !finding_item(nullptr, 1));
	/* Numbers with gaps, which finding_parse() accepts if they grow. */
	back.changes.items[0].evidence.items[1].item = 3;
	back.changes.items[0].items = 3;
	back.changes.items[1].evidence.items[0].item = 5;
	back.changes.items[1].items = 5;
	CHECK(finding_item(&back, 3) == &back.changes.items[0].evidence.items[1] &&
	      finding_item(&back, 5) == &back.changes.items[1].evidence.items[0] &&
	      !finding_item(&back, 2) && !finding_item(&back, 4) && !finding_item(&back, 6));
	SAME(dstr_get(&back.claim), dstr_len(&back.claim), "Second claim.");
	SAME(dstr_get(&back.changes.items[1].claim_before),
	     dstr_len(&back.changes.items[1].claim_before), "First claim.");
	SAME(dstr_get(&f.claim), dstr_len(&f.claim), "First claim.");
	CHECK(f.changes.n == 1 && !dstr_len(&c.claim_before));
	finding_fini(&back);
	/* Without a claim, the claim before of the change is its own
	 * claim_before. */
	CHECK(!cstr_view_store(CSTR_VIEW("Zeroth claim."), &c.claim_before));
	back = rendered(&f, &c, "");
	SAME(dstr_get(&back.claim), dstr_len(&back.claim), "First claim.");
	SAME(dstr_get(&back.changes.items[1].claim_before),
	     dstr_len(&back.changes.items[1].claim_before), "Zeroth claim.");
	finding_fini(&back);
	/* Without a change, the file of f alone. */
	back = rendered(&f, nullptr, "");
	CHECK(back.changes.n == 1 && back.changes.items[0].items == 2 && back.id.number == 5 &&
	      !back.id.item && !strcmp(back.id.author, "bro"));
	finding_fini(&back);
	/* A long version of askai, such as one of git describe with a long
	 * tail, stays as it is. */
	struct dstr text = DSTR_INIT;
	CHECK(!finding_render(&f, &c, CSTR_VIEW(""), &text));
	char const *at = strstr(dstr_get(&text), "0.9.0-412-gd02cc233");
	CHECK(at);
	struct dstr longer = DSTR_INIT;
	CHECK(dstr_printf(&longer, "%.*s%s%s", (int)(at - dstr_get(&text)), dstr_get(&text),
	                  "0.9.0-412-gd02cc233-and-a-much-longer-tail-dirty", at + 19));
	CHECK(!finding_parse(&back, cstr_view_dstr(&longer)) && back.changes.n == 2);
	SAME(dstr_get(&back.changes.items[0].askai), dstr_len(&back.changes.items[0].askai),
	     "0.9.0-412-gd02cc233-and-a-much-longer-tail-dirty");
	finding_fini(&back);
	dstr_cleanup(&longer);
	dstr_cleanup(&text);
	finding_change_fini(&c);
	/* finding_change_external() appends an external item. */
	CHECK(!finding_change_external(&c, CSTR_VIEW("sha256:9b1c")) &&
	      !finding_change_external(&c, CSTR_VIEW("x")) && c.external.n == 2);
	SAME(dstr_get(&c.external.items[1]), dstr_len(&c.external.items[1]), "x");
	finding_change_fini(&c);
	CHECK(!c.external.n && !c.external.items);
	finding_fini(&f);
	CHECK(!f.changes.items && !f.changes.n && !*f.id.author && !f.id.number);
	finding_fini(&f);
}

/* Returns the finding AUTHOR#NUMBER with the claim @p claim, the
 * conversation @p conversation and the about @p about, an ID or "" for no
 * about, with one change to @p state.
 */
static struct finding
finding_of (char const         *author,
            uint32_t            number,
            char const         *claim,
            char const         *conversation,
            char const         *about,
            enum finding_state  state)
{
	struct finding f = finding();
	f.id = id_of(author, number, 0);
	if (*about)
		CHECK(!finding_id_parse(&f.about, cstr_view(about)));
	CHECK(!hexid_init(&f.conversation, cstr_view(conversation)));
	CHECK(!cstr_view_store(cstr_view(claim), &f.claim));
	struct finding_change c = change_of(state, state ? "Shown." : "", number, number, 1);
	push(&f, &c);
	return f;
}

/* finding_render() writes the file of version 1, and finding_parse()
 * reads it and refuses what is not one.
 */
static void
test_file (void)
{
	struct finding f = finding_of("bro", 5, "git grep -L at d02cc233 lists 246 files.", CONV,
	                              "cuh#2", FINDING_CANDIDATE);
	struct finding_change c = change_of(FINDING_VERIFIED, "Call c4 printed 246 files.", 4, 4,
	                                    1);
	CHECK(!finding_change_external(&c, CSTR_VIEW("libclang sha256:9b1c")));
	c.evidence.items[0].item = 2;
	c.items = 2;
	struct dstr text = DSTR_INIT;
	CHECK(!finding_render(&f, &c, CSTR_VIEW("git grep -L lists 246 files."), &text));
	char const *t = dstr_get(&text);
	CHECK(strstr(t, "\"version\": 1,") && strstr(t, "\"id\": \"bro#5\",") &&
	      strstr(t, "\"about\": \"cuh#2\",") && strstr(t, "\"conversation\": \"" CONV "\",") &&
	      strstr(t, "\"claim\": \"git grep -L lists 246 files.\",") &&
	      strstr(t, "\"claim_before\": \"git grep -L at d02cc233 lists 246 files.\",") &&
	      strstr(t, "\"asked_by\": \"imaami\",") && strstr(t, "\"external\": [") &&
	      strstr(t, "\"askai\": \"0.9.0-412-gd02cc233\",") &&
	      strstr(t, "\"item\": 2,") && !strstr(t, "\"item\": 3,") && t[dstr_len(&text) - 1] == '\n');
	/* The keys of the file come in this order. */
	char const *const keys[] = { "\"version\"", "\"id\"", "\"about\"", "\"conversation\"",
	                             "\"claim\"", "\"changes\"", "\"state\"", "\"ask\"", "\"turn\"",
	                             "\"call\"", "\"asked_by\"", "\"askai\"", "\"reason\"",
	                             "\"evidence\"", "\"external\"" };
	char const *at = t;
	for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i) {
		char const *next = strstr(at, keys[i]);
		CHECK(next);
		at = next ? next : at;
	}
	struct finding back = finding();
	CHECK(!finding_parse(&back, cstr_view_dstr(&text)));
	struct finding_id const bro5 = id_of("bro", 5, 0);
	struct finding_id const cuh2 = id_of("cuh", 2, 0);
	CHECK(finding_id_equal(&back.id, &bro5) && finding_id_equal(&back.about, &cuh2) &&
	      !strcmp(back.conversation.hex, CONV) && finding_state(&back) == FINDING_VERIFIED &&
	      back.changes.n == 2 && back.changes.items[1].items == 2 &&
	      back.changes.items[1].external.n == 1 && back.changes.items[1].evidence.n == 1 &&
	      back.changes.items[1].evidence.items[0].item == 2 &&
	      !strcmp(back.changes.items[1].asked_by, "imaami") &&
	      !strcmp(back.changes.items[1].ask.hex, ASK) && back.changes.items[1].turn == 4 &&
	      back.changes.items[1].call == 5);
	SAME(dstr_get(&back.changes.items[1].askai), dstr_len(&back.changes.items[1].askai),
	     "0.9.0-412-gd02cc233");
	struct dstr again = DSTR_INIT;
	CHECK(!finding_render(&back, nullptr, CSTR_VIEW(""), &again));
	SAME(dstr_get(&again), dstr_len(&again), t);

	/* Files that askai does not read. */
	struct {
		char const *from;
		char const *to;
	} const edits[] = {
		{ "\"version\": 1", "\"version\": 2" },
		{ "\"version\": 1", "\"version\": \"1\"" },
		{ "\"id\": \"bro#5\"", "\"id\": \"bro\"" },
		{ "\"id\": \"bro#5\"", "\"id\": \"bro#5/1\"" },
		{ "\"id\": \"bro#5\"", "\"id\": 5" },
		{ "\"about\": \"cuh#2\"", "\"about\": \"cuh\"" },
		{ "\"about\": \"cuh#2\"", "\"about\": \"cuh#2/1\"" },
		{ "\"about\": \"cuh#2\"", "\"about\": \"\"" },
		{ "\"about\": \"cuh#2\"", "\"about\": 2" },
		{ "\"conversation\": \"" CONV "\"", "\"conversation\": \"x\"" },
		{ "\"claim\": \"git grep -L lists 246 files.\"", "\"claim\": \"\"" },
		{ "\"state\": \"verified\"", "\"state\": \"open\"" },
		{ "\"kind\": \"call\"", "\"kind\": \"blob\"" },
		{ "\"item\": 2", "\"item\": 1" },
		{ "\"turn\": 4", "\"turn\": -4" },
		{ "\"asked_by\": \"imaami\"", "\"asked_by\": \"c7\"" },
		{ "\"askai\": \"0.9.0-412-gd02cc233\"", "\"askai\": 7" },
		{ "\"libclang sha256:9b1c\"", "7" },
		{ "\"external\": [", "\"external\": 7, \"x\": [" },
	};
	for (size_t i = 0; i < sizeof edits / sizeof *edits; ++i) {
		char const *p = strstr(t, edits[i].from);
		CHECK(p);
		if (!p)
			continue;
		struct dstr bad = DSTR_INIT;
		CHECK(dstr_printf(&bad, "%.*s%s%s", (int)(p - t), t, edits[i].to,
		                  p + strlen(edits[i].from)));
		CHECK(finding_parse(&back, cstr_view_dstr(&bad)) == EINVAL);
		CHECK(!back.changes.n && !back.id.number && !*back.id.author && !*back.about.author &&
		      !back.about.number && !back.about.item && !dstr_len(&back.claim));
		dstr_cleanup(&bad);
	}
	CHECK(finding_parse(&back, CSTR_VIEW("")) == EINVAL);
	CHECK(finding_parse(&back, CSTR_VIEW("[]")) == EINVAL);
	CHECK(finding_parse(&back, CSTR_VIEW("{\"version\":1,\"id\":\"bro#1\",\"claim\":\"c\","
	                                     "\"changes\":[]}")) == EINVAL);
	/* The smallest file. */
	CHECK(!finding_parse(&back, CSTR_VIEW("{\"version\":1,\"id\":\"bro#1\",\"claim\":\"c\","
	                                      "\"changes\":[{\"state\":\"candidate\",\"turn\":1,"
	                                      "\"call\":2,\"evidence\":[],\"external\":[]}]}")));
	CHECK(back.id.number == 1 && back.changes.n == 1 && !*back.about.author &&
	      !back.about.number && !*back.conversation.hex &&
	      !*back.changes.items[0].ask.hex && !*back.changes.items[0].asked_by &&
	      !back.changes.items[0].items);
	finding_fini(&back);
	finding_fini(&f);
	finding_change_fini(&c);
	dstr_cleanup(&again);
	dstr_cleanup(&text);
}

/* finding_list(), finding_show() and finding_notice() write the texts of
 * list, show and the prompt.
 */
static void
test_texts (void)
{
	struct finding list[] = {
		finding_of("cuh", 1, "grep -c ignores -e.", ELSE, "", FINDING_RETRACTED),
		finding_of("bro", 4, "The git tool labels every exit status 1 as \"No matches.\".", CONV,
		           "", FINDING_VERIFIED),
		finding_of("cuh", 2, "git grep -L lists every file of the commit.\nMore text.", ELSE, "",
		           FINDING_VERIFIED),
		finding_of("bro", 5, "git grep -L at d02cc233 lists only the 246 files.", CONV, "cuh#2",
		           FINDING_CANDIDATE),
		finding_of("bro", 12, "An old one.", ELSE, "", FINDING_CANDIDATE),
		finding_of("bro", 3, "Closed.", CONV, "", FINDING_DISPROVED),
	};
	size_t const n = sizeof list / sizeof *list;
	finding_sort(list, n);
	/* The open findings first, then by author, newest first. */
	CHECK(list[0].id.number == 12 && list[1].id.number == 5 && list[2].id.number == 4 &&
	      list[3].id.number == 2 && list[4].id.number == 3 && list[5].id.number == 1);
	CHECK(!strcmp(list[2].id.author, "bro") && !strcmp(list[3].id.author, "cuh") &&
	      !strcmp(list[4].id.author, "bro") && !strcmp(list[5].id.author, "cuh"));
	/* finding_find() finds each finding of the sorted list, open or
	 * closed, by its ID and by the ID of an item of the finding. */
	for (size_t i = 0; i < n; ++i) {
		struct finding_id item = list[i].id;
		item.item = 7;
		CHECK(finding_find(list, n, &list[i].id) == &list[i] &&
		      finding_find(list, n, &item) == &list[i]);
	}
	struct finding_id const bro12 = id_of("bro", 12, 0);
	struct finding_id const cuh1 = id_of("cuh", 1, 0);
	struct finding_id const none[] = { id_of("bro", 6, 0), id_of("bro", 1, 0), id_of("cuh", 3, 0),
	                                   id_of("dan", 1, 0), id_of("", 12, 0), id_of("br", 12, 0),
	                                   {} };
	for (size_t i = 0; i < sizeof none / sizeof *none; ++i)
		CHECK(!finding_find(list, n, &none[i]));
	CHECK(!finding_find(list, 0, &bro12) && !finding_find(nullptr, n, &bro12) &&
	      !finding_find(list, n, nullptr));
	CHECK(finding_find(list, 4, &bro12) == &list[0] && !finding_find(list, 4, &cuh1) &&
	      finding_find(list + 4, 2, &cuh1) == &list[5] && !finding_find(list + 4, 2, &bro12));
	struct dstr text = DSTR_INIT;
	CHECK(!finding_list(list, n, CSTR_VIEW("test1"), "", &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "Findings of the session test1, open first (id, state, about, claim):\n"
	     "bro#12  candidate  -      An old one.\n"
	     "bro#5   candidate  cuh#2  git grep -L at d02cc233 lists only the 246 files.\n"
	     "bro#4   verified   -      The git tool labels every exit status 1 as \"No matches.\".\n"
	     "cuh#2   verified   -      git grep -L lists every file of the commit.\n"
	     "Closed findings:\n"
	     "bro#3   disproved  -      Closed.\n"
	     "cuh#1   retracted  -      grep -c ignores -e.\n");
	/* The findings of one author. */
	struct finding const of_cuh[] = { list[3], list[5] };
	(void)dstr_clear(&text);
	CHECK(!finding_list(of_cuh, 2, CSTR_VIEW("test1"), "cuh", &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "Findings of cuh, open first (id, state, about, claim):\n"
	     "cuh#2  verified   -  git grep -L lists every file of the commit.\n"
	     "Closed findings:\n"
	     "cuh#1  retracted  -  grep -c ignores -e.\n");
	(void)dstr_clear(&text);
	CHECK(!finding_list(list, 0, CSTR_VIEW("test1"), "unc", &text));
	SAME(dstr_get(&text), dstr_len(&text), "unc has no findings yet.\n");
	(void)dstr_clear(&text);
	CHECK(!finding_list(list, 0, CSTR_VIEW("test1"), "", &text));
	SAME(dstr_get(&text), dstr_len(&text), "No member of the session test1 has findings yet.\n");
	/* A long claim is cut at FINDING_CLAIM bytes. */
	char claim[FINDING_CLAIM + 50];
	memset(claim, 'x', sizeof claim - 1);
	claim[sizeof claim - 1] = '\0';
	struct finding big = finding_of("unc", 1, claim, CONV, "", FINDING_CANDIDATE);
	(void)dstr_clear(&text);
	CHECK(!finding_list(&big, 1, CSTR_VIEW("test1"), "", &text));
	CHECK(strstr(dstr_get(&text), "unc#1  candidate  -  xxx") &&
	      dstr_len(&text) == sizeof "Findings of the session test1, open first (id, state, "
	                         "about, claim):\nunc#1  candidate  -  \n" - 1 + FINDING_CLAIM);

	/* Only open findings, and only closed findings. */
	(void)dstr_clear(&text);
	CHECK(!finding_list(list + 4, 2, CSTR_VIEW("test1"), "", &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "Findings of the session test1, open first (id, state, about, claim):\n"
	     "Closed findings:\n"
	     "bro#3  disproved  -  Closed.\n"
	     "cuh#1  retracted  -  grep -c ignores -e.\n");
	(void)dstr_clear(&text);
	CHECK(!finding_list(list + 2, 2, CSTR_VIEW("test1"), "", &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "Findings of the session test1, open first (id, state, about, claim):\n"
	     "bro#4  verified   -  The git tool labels every exit status 1 as \"No matches.\".\n"
	     "cuh#2  verified   -  git grep -L lists every file of the commit.\n");
	/* show lists the findings about a finding and each change. */
	struct finding *cuh2 = &list[3];
	struct finding_change c = change_of(FINDING_VERIFIED, "The option table maps -L.", 27, 9, 1);
	c.evidence.items[0].from = id_of("unc", 1, 1);
	c.evidence.items[0].kind = EVIDENCE_LINES;
	c.evidence.items[0].flags = 0;
	CHECK(!util_grow(c.evidence.items[0].ranges.items, c.evidence.items[0].ranges.cap, 1));
	c.evidence.items[0].ranges.items[c.evidence.items[0].ranges.n++] =
		(struct range){ 161, 171 };
	CHECK(!cstr_view_store(CSTR_VIEW("{\"repo\":\"askai\",\"commit\":"
	                                 "\"d02cc2338c4f7d1e00000000000000000000aaaa\","
	                                 "\"path\":\"src/git.c\"}"), &c.evidence.items[0].ids));
	push(cuh2, &c);
	strcpy(cuh2->changes.items[1].asked_by, "");
	cuh2->changes.items[1].ask = (struct hexid){};
	(void)dstr_clear(&text);
	char id[FINDING_ID_TEXT];
	finding_id_put(&cuh2->id, id);
	CHECK(!finding_show(cuh2, id, list, n, &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "Finding cuh#2 by cuh: verified\n"
	     "claim: git grep -L lists every file of the commit.\nMore text.\n"
	     "Findings about cuh#2: bro#5 (candidate)\n"
	     "Change 1: verified, ask 9f3c2a71, turn 2, asked by imaami\n"
	     "  reason: Shown.\n"
	     "  evidence cuh#2/1: call c2, git, 102 bytes\n"
	     "Change 2: verified, turn 27\n"
	     "  reason: The option table maps -L.\n"
	     "  evidence cuh#2/2 from unc#1/1: lines 161 to 171 of src/git.c in repository askai "
	     "at commit d02cc2338c4f\n");
	(void)dstr_clear(&text);
	finding_id_put(&list[1].id, id);
	CHECK(!finding_show(&list[1], id, list, n, &text));
	CHECK(strstr(dstr_get(&text), "Finding bro#5 by bro: candidate\nclaim: git grep -L at "
	                              "d02cc233 lists only the 246 files.\nabout: cuh#2\nChange 1: "
	                              "candidate, ask 9f3c2a71, turn 5, asked by imaami\n  evidence "
	                              "bro#5/1: call c5, git, 105 bytes\n"));

	/* The notice of the open findings of the agent bro, from the findings of
	 * bro, sorted. */
	struct finding const bro[] = { list[0], list[1], list[2], list[4] };
	(void)dstr_clear(&text);
	CHECK(!finding_notice(bro, 4, &(struct hexid){ CONV }, SIZE_MAX, &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "[askai: Your open findings in this conversation (id, state, claim):]\n"
	     "bro#5  candidate  git grep -L at d02cc233 lists only the 246 files.\n"
	     "bro#4  verified   The git tool labels every exit status 1 as \"No matches.\".\n"
	     "[askai: You have 1 open finding in other conversations. Call findings with action "
	     "list to see the findings of every member.]\n");
	/* Cut after the first line. */
	(void)dstr_clear(&text);
	CHECK(!finding_notice(bro, 4, &(struct hexid){ CONV }, 80, &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "[askai: Your open findings in this conversation (id, state, claim):]\n"
	     "bro#5  candidate  git grep -L at d02cc233 lists only the 246 files.\n"
	     "[askai: 1 more open finding. Call findings with action list.]\n"
	     "[askai: You have 1 open finding in other conversations. Call findings with action "
	     "list to see the findings of every member.]\n");
	(void)dstr_clear(&text);
	CHECK(!finding_notice(bro, 4, &(struct hexid){ CONV }, 0, &text));
	CHECK(strstr(dstr_get(&text), "[askai: 2 more open findings. Call findings with action "
	                              "list.]\n"));
	(void)dstr_clear(&text);
	CHECK(!finding_notice(bro, 4, &(struct hexid){ "fedcba9876543210fedcba9876543210" },
	                      SIZE_MAX, &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "[askai: You have no open findings in this conversation.]\n"
	     "[askai: You have 3 open findings in other conversations. Call findings with action "
	     "list to see the findings of every member.]\n");
	(void)dstr_clear(&text);
	CHECK(!finding_notice(bro, 0, &(struct hexid){ CONV }, SIZE_MAX, &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "[askai: You have no open findings in this conversation.]\n");
	for (size_t i = 0; i < n; ++i)
		finding_fini(&list[i]);
	finding_fini(&big);
	dstr_cleanup(&text);
}

int
main (void)
{
	test_id();
	test_paths();
	test_changes();
	test_file();
	test_texts();
	return check_report("finding");
}
