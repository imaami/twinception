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

/* finding_id_parse() reads IDs, and finding_path() and
 * finding_path_number() name the files.
 */
static void
test_ids (void)
{
	char author[MEMBER_NAME] = "x";
	uint32_t n = 9;
	CHECK(!finding_id_parse(CSTR_VIEW("bro#5"), author, &n) && !strcmp(author, "bro") && n == 5);
	CHECK(!finding_id_parse(CSTR_VIEW("first.last@ad#4294967295"), author, &n) &&
	      !strcmp(author, "first.last@ad") && n == UINT32_MAX);
	char const *const bad[] = { "", "bro", "bro#", "bro#0", "bro#05", "bro#4294967296", "#5",
	                            "c7#5", ".bro#5", "bro#5/1", "bro#5 ", "bro#-1", "b ro#5" };
	for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
		n = 9;
		strcpy(author, "x");
		CHECK(finding_id_parse(cstr_view(bad[i]), author, &n) == EINVAL && !*author && !n);
	}
	struct finding f = finding();
	strcpy(f.author, "cuh");
	f.number = 12;
	char id[FINDING_ID];
	finding_id(&f, id);
	CHECK(!strcmp(id, "cuh#12"));

	char path[FINDING_FILE];
	finding_path(5, path);
	CHECK(!strcmp(path, "findings/0/5.json") && finding_path_number(cstr_view(path)) == 5);
	finding_path(100, path);
	CHECK(!strcmp(path, "findings/1/100.json") && finding_path_number(cstr_view(path)) == 100);
	finding_path(UINT32_MAX, path);
	CHECK(!strcmp(path, "findings/42949672/4294967295.json") &&
	      finding_path_number(cstr_view(path)) == UINT32_MAX && strlen(path) == FINDING_FILE - 1);
	char const *const paths[] = { "findings/1/5.json", "findings/0/05.json", "findings/0/0.json",
	                              "archive/0/5.json", "findings/0/5.txt", "x/findings/0/5.json",
	                              "findings/00/5.json", ".json", "", "findings/0/.json" };
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

/* finding_check() checks the rules of each state, and finding_apply()
 * numbers the items and corrects the claim.
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
	strcpy(f.author, "bro");
	f.number = 5;
	CHECK(!cstr_view_store(CSTR_VIEW("First claim."), &f.claim));
	c = change_of(FINDING_CANDIDATE, "Looks so.", 2, 4, 2);
	CHECK(!finding_apply(&f, &c, CSTR_VIEW("")));
	CHECK(!c.evidence.n && !c.evidence.items && !dstr_len(&c.reason) && !dstr_len(&c.askai) &&
	      !c.state);
	CHECK(f.changes.n == 1 && finding_state(&f) == FINDING_CANDIDATE &&
	      f.changes.items[0].items == 2 &&
	      f.changes.items[0].evidence.items[0].item == 1 &&
	      f.changes.items[0].evidence.items[1].item == 2 &&
	      !dstr_len(&f.changes.items[0].claim_before));
	/* The same claim is no correction. */
	c = change_of(FINDING_VERIFIED, "Call c9 shows it.", 4, 9, 1);
	CHECK(!finding_apply(&f, &c, CSTR_VIEW("First claim.")));
	CHECK(f.changes.n == 2 && finding_state(&f) == FINDING_VERIFIED &&
	      f.changes.items[1].items == 3 &&
	      f.changes.items[1].evidence.items[0].item == 3 &&
	      !dstr_len(&f.changes.items[1].claim_before));
	c = change_of(FINDING_VERIFIED, "Corrected.", 5, 0, 0);
	CHECK(!finding_apply(&f, &c, CSTR_VIEW("Second claim.")));
	CHECK(f.changes.n == 3 && f.changes.items[2].items == 3);
	SAME(dstr_get(&f.claim), dstr_len(&f.claim), "Second claim.");
	SAME(dstr_get(&f.changes.items[2].claim_before), dstr_len(&f.changes.items[2].claim_before),
	     "First claim.");
	/* finding_unapply() takes the last change back, as before the apply.
	 * Applied again, it gets the same numbers. */
	finding_unapply(&f, &c);
	CHECK(f.changes.n == 2 && finding_state(&f) == FINDING_VERIFIED &&
	      c.state == FINDING_VERIFIED &&
	      !dstr_len(&c.claim_before) && dstr_len(&c.reason) && c.items == 3);
	SAME(dstr_get(&f.claim), dstr_len(&f.claim), "First claim.");
	CHECK(!finding_apply(&f, &c, CSTR_VIEW("Second claim.")) && f.changes.n == 3);
	finding_unapply(&f, &c);
	finding_change_fini(&c);
	finding_unapply(&f, &c);
	CHECK(f.changes.n == 1 && finding_state(&f) == FINDING_CANDIDATE && c.evidence.n == 1 &&
	      c.evidence.items[0].item == 3);
	finding_change_fini(&c);
	finding_unapply(&f, &c);
	CHECK(!f.changes.n && finding_state(&f) == FINDING_CANDIDATE && c.evidence.n == 2);
	finding_change_fini(&c);
	finding_unapply(&f, &c);
	CHECK(!f.changes.n && !c.evidence.n);
	finding_fini(&f);
	CHECK(!f.changes.items && !f.changes.n && !*f.author && !f.number);
	finding_fini(&f);
}

/* Returns the finding AUTHOR#NUMBER with the claim @p claim, the
 * conversation @p conversation and the about @p about, with one change to
 * @p state.
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
	strcpy(f.author, author);
	f.number = number;
	strcpy(f.about, about);
	CHECK(!hexid_init(&f.conversation, cstr_view(conversation)));
	CHECK(!cstr_view_store(cstr_view(claim), &f.claim));
	struct finding_change c = change_of(state, state ? "Shown." : "", number, number, 1);
	CHECK(!finding_apply(&f, &c, CSTR_VIEW("")));
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
	CHECK(!util_grow(c.external.items, c.external.cap, 1));
	struct dstr *x = &c.external.items[c.external.n++];
	*x = (struct dstr)DSTR_INIT;
	CHECK(!cstr_view_store(CSTR_VIEW("libclang sha256:9b1c"), x));
	CHECK(!finding_apply(&f, &c, CSTR_VIEW("git grep -L lists 246 files.")));
	struct dstr text = DSTR_INIT;
	CHECK(!finding_render(&f, &text));
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
	CHECK(back.number == 5 && !strcmp(back.author, "bro") && !strcmp(back.about, "cuh#2") &&
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
	CHECK(!finding_render(&back, &again));
	SAME(dstr_get(&again), dstr_len(&again), t);

	/* Files that askai does not read. */
	struct {
		char const *from;
		char const *to;
	} const edits[] = {
		{ "\"version\": 1", "\"version\": 2" },
		{ "\"version\": 1", "\"version\": \"1\"" },
		{ "\"id\": \"bro#5\"", "\"id\": \"bro\"" },
		{ "\"about\": \"cuh#2\"", "\"about\": \"cuh\"" },
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
		CHECK(!back.changes.n && !back.number && !dstr_len(&back.claim));
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
	CHECK(back.number == 1 && back.changes.n == 1 && !*back.about && !*back.conversation.hex &&
	      !*back.changes.items[0].ask.hex && !*back.changes.items[0].asked_by &&
	      !back.changes.items[0].items);
	finding_fini(&back);
	finding_fini(&f);
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
	CHECK(list[0].number == 12 && list[1].number == 5 && list[2].number == 4 &&
	      list[3].number == 3 && list[4].number == 2 && list[5].number == 1);
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
	(void)dstr_clear(&text);
	CHECK(!finding_list(list, n, CSTR_VIEW("test1"), "cuh", &text));
	SAME(dstr_get(&text), dstr_len(&text),
	     "Findings of cuh, open first (id, state, about, claim):\n"
	     "cuh#2  verified   -  git grep -L lists every file of the commit.\n"
	     "Closed findings:\n"
	     "cuh#1  retracted  -  grep -c ignores -e.\n");
	(void)dstr_clear(&text);
	CHECK(!finding_list(list, n, CSTR_VIEW("test1"), "unc", &text));
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

	/* show lists the findings about a finding and each change. */
	struct finding *cuh2 = &list[4];
	struct finding_change c = change_of(FINDING_VERIFIED, "The option table maps -L.", 27, 9, 1);
	strcpy(c.evidence.items[0].from, "unc#1/1");
	c.evidence.items[0].kind = EVIDENCE_LINES;
	c.evidence.items[0].flags = 0;
	CHECK(!util_grow(c.evidence.items[0].ranges.items, c.evidence.items[0].ranges.cap, 1));
	c.evidence.items[0].ranges.items[c.evidence.items[0].ranges.n++] =
		(struct evidence_range){ 161, 171 };
	CHECK(!cstr_view_store(CSTR_VIEW("{\"repo\":\"askai\",\"commit\":"
	                                 "\"d02cc2338c4f7d1e00000000000000000000aaaa\","
	                                 "\"path\":\"src/git.c\"}"), &c.evidence.items[0].ids));
	CHECK(!finding_apply(cuh2, &c, CSTR_VIEW("")));
	strcpy(cuh2->changes.items[1].asked_by, "");
	cuh2->changes.items[1].ask = (struct hexid){};
	(void)dstr_clear(&text);
	CHECK(!finding_show(cuh2, list, n, &text));
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
	CHECK(!finding_show(&list[1], list, n, &text));
	CHECK(strstr(dstr_get(&text), "Finding bro#5 by bro: candidate\nclaim: git grep -L at "
	                              "d02cc233 lists only the 246 files.\nabout: cuh#2\nChange 1: "
	                              "candidate, ask 9f3c2a71, turn 5, asked by imaami\n  evidence "
	                              "bro#5/1: call c5, git, 105 bytes\n"));

	/* The notice of the open findings of the agent bro. */
	struct finding *bro = list;
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
	test_ids();
	test_changes();
	test_file();
	test_texts();
	return check_report("finding");
}
