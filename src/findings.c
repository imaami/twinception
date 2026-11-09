/** @file findings.c
 *
 * The findings tool (src/findings.h).
 */
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "child.h"
#include "dstr.h"
#include "evidence.h"
#include "finding.h"
#include "findings.h"
#include "jb.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "member.h"
#include "notes.h"
#include "notes_tool.h"
#include "notice.h"
#include "oid.h"
#include "plugin.h"
#include "schema.h"
#include "seen.h"
#include "store.h"
#include "util.h"
#include "version.h"
#include "xlist.h"

/* The actions of the findings tool: X(ID, name), the constant FINDINGS_ID
 * of enum findings_action and the name.
 */
#define FINDINGS_ACTION_LIST(X) \
        X(ADD,    "add")        \
        X(UPDATE, "update")     \
        X(LIST,   "list")       \
        X(SHOW,   "show")       \
        X(EXPAND, "expand")

enum findings_action : uint8_t {
#define FINDINGS_ACTION_ENUM(id, ...) FINDINGS_##id,
	FINDINGS_ACTION_LIST(FINDINGS_ACTION_ENUM)
#undef FINDINGS_ACTION_ENUM
	FINDINGS_ACTIONS,
};

/* The names of the actions. */
static struct cstr_view const findings_actions[] = { FINDINGS_ACTION_LIST(XL_VIEW) };

/* The actions as bits, 1 << enum findings_action, and the actions that
 * clients can call.
 */
enum : uint32_t {
	ON_ADD     = 1U << FINDINGS_ADD,
	ON_UPDATE  = 1U << FINDINGS_UPDATE,
	ON_LIST    = 1U << FINDINGS_LIST,
	ON_SHOW    = 1U << FINDINGS_SHOW,
	ON_EXPAND  = 1U << FINDINGS_EXPAND,
	ON_CLIENTS = ON_LIST | ON_SHOW | ON_EXPAND,
};

/* The names of the arguments of the findings tool, and the actions that
 * take each.
 */
static struct {
	struct cstr_view name;
	uint64_t         actions;  /* uint64_t to fill the padding */
} const findings_arg_rows[] = {
	{ CSTR_VIEW_INIT("action"),   ON_ADD | ON_UPDATE | ON_LIST | ON_SHOW | ON_EXPAND },
	{ CSTR_VIEW_INIT("id"),       ON_UPDATE | ON_SHOW | ON_EXPAND },
	{ CSTR_VIEW_INIT("claim"),    ON_ADD | ON_UPDATE },
	{ CSTR_VIEW_INIT("state"),    ON_ADD | ON_UPDATE },
	{ CSTR_VIEW_INIT("evidence"), ON_ADD | ON_UPDATE },
	{ CSTR_VIEW_INIT("external"), ON_ADD | ON_UPDATE },
	{ CSTR_VIEW_INIT("reason"),   ON_ADD | ON_UPDATE },
	{ CSTR_VIEW_INIT("about"),    ON_ADD },
	{ CSTR_VIEW_INIT("author"),   ON_LIST },
	{ CSTR_VIEW_INIT("offset"),   ON_LIST | ON_SHOW | ON_EXPAND },
};

/* The text for findings that git could not read. */
static struct cstr_view const unread = CSTR_VIEW_INIT("The findings could not be read.");

/* The git that a call runs. */
enum findings_stage : uint8_t {
	STAGE_REFS,     /* the table of the members reads the records, with the
	                   branches of findings (member_table_read()) */
	STAGE_CHECK,    /* cat-file checks the finding that about names */
	STAGE_FILES,    /* ls-tree lists the files of the findings of a branch */
	STAGE_READ,     /* cat-file reads files of findings */
	STAGE_COMMIT,   /* fast-import commits an add or an update */
	STAGE_BLOBS,    /* cat-file reads the archive files of the items that
	                   the evidence cites again */
	STAGE_ARCHIVE,  /* cat-file reads the archive files of the items that
	                   expand prints */
	STAGE_CALLS,    /* log lists the calls of the newest commits of the
	                   branch of the agent, for an add or an update that
	                   askai runs again after a restart */
};

/* The stages whose step handles a git or a read that failed. The steps of
 * the other stages need the output of a git that succeeded.
 */
static constexpr uint32_t STAGES_FAILED = 1U << STAGE_REFS | 1U << STAGE_COMMIT |
                                          1U << STAGE_CALLS;

/* The stored material of the items that one call reads, at most. */
static constexpr size_t FINDINGS_MATERIAL = TOOL_OUTPUT_MAX / 2;

/* The flags of a call. */
enum findings_flag : uint8_t {
	CALL_ABOUT = 1U << 0,  /* the finding that about names exists */
	CALL_FOUND = 1U << 1,  /* the finding of id exists */
	CALL_SAME  = 1U << 2,  /* the update does not change the state of the
	                          finding */
	CALL_CLAIM = 1U << 3,  /* the update corrects the claim */
	CALL_CITED = 1U << 4,  /* the items that the evidence cites again are
	                          in the archive of the commit */
};

/* A branch of findings: its tip and its author. The author is a copy,
 * because a read of the table of the members by another call can replace
 * the records between two stages of a call.
 */
struct findings_branch {
	struct oid tip;
	char       author[MEMBER_NAME];
};

/* A file of a finding that cat-file reads: its size, its author, the
 * number of the finding and its blob ID. cat-file reads a file without a
 * blob ID by its path on the branch of the author.
 */
struct findings_file {
	uint64_t    bytes;   /* from ls-tree, or 0 if unknown */
	char const *author;  /* in call->branches, call->cites, or
	                        call->author */
	uint32_t    number;
	struct oid  id;      /* from ls-tree, or no name */
};

/* The flags of a reference to an item of a finding (struct findings_cite). */
enum findings_cite_flag : uint16_t {
	CITE_SHOW    = 1U << 0,  /* the turns had not seen the item, which the
	                            refusal prints */
	CITE_READ    = 1U << 1,  /* the call reads the archive files of the
	                            item */
	CITE_REQUEST = 1U << 2,  /* the item stores the arguments of a call */
};

/* A reference of the evidence argument to an item of a finding, X#n/m or
 * X#n/m:A-B.
 */
struct findings_cite {
	struct dstr         record;    /* the JSON object of the item, from the
	                                  ring or from the file of its
	                                  finding */
	struct evidence_ref ref;
	uint32_t            index;     /* the place of the item in the evidence
	                                  of the change */
	uint32_t            turn;      /* the turn whose result showed the item
	                                  whole, or 0 */
	struct oid          blobs[2];  /* the git IDs of the archive files of
	                                  the item, or no name */
	uint16_t            flags;     /* enum findings_cite_flag */
};

/* An item that expand prints, and its finding. */
struct findings_print {
	struct finding const  *f;
	struct evidence const *e;
};

/* The state of a findings call. It is the own word of the call's slot. */
struct findings_call {
	struct buf             files;     /* the file parts of a commit: the
	                                     archive of the evidence, and the
	                                     file of the finding while git
	                                     commits */
	struct dstr            out;       /* the result of the call */
	struct dstr            claim;     /* the claim argument, without the
	                                     notices at its end */
	struct finding_change  change;    /* the change of add and update */
	struct member_wait     wait;      /* the read of the records of the
	                                     members (STAGE_REFS) */
	UTIL_ARRAY(struct findings_branch) branches;  /* from the records of
	                                                 the members */
	UTIL_ARRAY(struct findings_file)   wanted;    /* the files that cat-file
	                                                 reads, in order */
	UTIL_ARRAY(struct finding)         findings;  /* the findings read */
	UTIL_ARRAY(struct findings_cite)   cites;     /* the items that the
	                                                 evidence cites again */
	UTIL_ARRAY(struct findings_print)  prints;    /* the items that expand
	                                                 prints, in order */
	struct child          *child;     /* the call's git, until a step has
	                                     read its output */
	struct json_object    *args;      /* the arguments of the call, while
	                                     log lists the calls (STAGE_CALLS) */
	struct notes_tool_loop const *notes;  /* the part of the notes tool on
	                                         the loop of the call, with the
	                                         service and the table of the
	                                         members */
	struct mcp_slot       *slot;      /* the slot of the call */
	struct dstr           *prompt;    /* the text that askai adds to the
	                                     prompt, for the call of
	                                     findings_prompt(), else nullptr */
	tool_step_fn          *then;      /* the step that goes on with the ask
	                                     call after findings_prompt() */
	size_t                 most;      /* the most bytes of lines of findings
	                                     for findings_prompt() */
	size_t                 archive;   /* bytes of files that are the
	                                     archive */
	uint32_t               number;    /* N of id, or of a new finding */
	uint32_t               item;      /* m of the id X#n/m of expand, or 0 */
	uint32_t               last;      /* the highest number of a finding of
	                                     the agent */
	uint32_t               branch;    /* the branch that ls-tree lists */
	uint32_t               done;      /* the files of wanted that cat-file
	                                     has read */
	uint32_t               batch;     /* the files of wanted up to which
	                                     the current cat-file reads */
	uint8_t                action;    /* enum findings_action */
	uint8_t                stage;     /* enum findings_stage */
	uint16_t               tries;     /* commits that raced, at most
	                                     STORE_TRIES, widened over the
	                                     padding */
	uint8_t                flags;     /* enum findings_flag */
	char                   author[MEMBER_NAME];  /* the author of id, or the
	                                                author argument of list,
	                                                or "" */
	char                   about[FINDING_ID];    /* the about argument, or
	                                                "" */
};

static_assert(EVIDENCE_REFS == 16 && EVIDENCE_EXTERNAL == 8 && TOOL_TEXT_MAX == 4U << 20,
              "update the texts in this file that say 16 items, 8 items or 4 MiB");

/* Returns the session of the findings of @p call and the agent of askai in
 * it.
 */
static struct member_session const *
findings_session (struct findings_call const *call)
{
	return member_registry_session(call->notes->tool->members);
}

/* Returns the name of the tool with the registry ID @p tool, for
 * evidence_cite(). @p owner is the slot of the call.
 */
static char const *
findings_tool_name (void const *owner,
                    uint32_t    tool)
{
	return tool < TOOL_MAX ? mcp_slot_tool_name(owner, tool) : nullptr;
}

/* Frees the call at @p *p_call and sets @p *p_call to nullptr. */
static void
findings_call_destroy (struct findings_call **p_call)
{
	struct findings_call *call = *p_call;
	member_table_cancel(&call->wait);
	store_drop(&call->child);
	buf_fini(&call->files);
	dstr_cleanup(&call->out);
	dstr_cleanup(&call->claim);
	finding_change_fini(&call->change);
	for (size_t i = 0; i < call->findings.n; ++i)
		finding_fini(&call->findings.items[i]);
	free(call->findings.items);
	call->findings.items = nullptr;
	for (size_t i = 0; i < call->cites.n; ++i)
		dstr_cleanup(&call->cites.items[i].record);
	free(call->cites.items);
	call->cites.items = nullptr;
	free(call->prints.items);
	call->prints.items = nullptr;
	free(call->branches.items);
	call->branches.items = nullptr;
	free(call->wanted.items);
	call->wanted.items = nullptr;
	free(call);
	*p_call = nullptr;
}

void
findings_drop (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	if (call)
		findings_call_destroy(&call);
	*mcp_slot_own(slot) = nullptr;
}

/* Ends the call in @p slot with its output, failed if @p failed is true,
 * or with the error @p e (mcp_slot_fail()).
 */
static void
findings_end (struct mcp_slot *slot,
              int              e,
              bool             failed)
{
	struct findings_call *call = *mcp_slot_own(slot);
	child_destroy(&call->child);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	mcp_slot_lend(slot, cstr_view_dstr(&call->out));
	/* The offset of a list or a show does not apply to a refusal. */
	if (failed)
		(void)mcp_slot_offset(slot, nullptr);
	mcp_slot_finish(slot, failed ? TOOL_FAILED : 0);
}

/* Ends the reading of the open findings for the prompt in @p slot
 * (findings_prompt()). Appends the notices of the open findings of the
 * agent in the conversation of the ask call, or for @p e EINVAL the notice
 * that the findings could not be read, and then seen_notice(). Then frees
 * the call and calls call->then, or ends the ask call with ENOMEM.
 */
static void
findings_prompted (struct mcp_slot *slot,
                   int              e)
{
	struct findings_call const *call = *mcp_slot_own(slot);
	struct dstr *d = call->prompt;
	tool_step_fn *then = call->then;
	/* After the prompt or a summary, the notices start a paragraph. */
	int r = 0;
	if (!cstr_view_ends(cstr_view_dstr(d), CSTR_VIEW("\n")))
		r = cstr_view_cat(CSTR_VIEW("\n\n"), d);
	if (e)
		r = r ? r : cstr_view_cat(CSTR_VIEW("[askai: Your findings could not be read.]\n"), d);
	else
		r = r ? r : finding_notice(call->findings.items, call->findings.n,
		                           &mcp_slot_member(slot)->conversation, call->most, d);
	r = r ? r : seen_notice(mcp_slot_seen(slot), d);
	findings_drop(slot);
	if (r)
		mcp_slot_fail(slot, r);
	else
		then(slot);
}

/* Ends the call in @p slot after the error @p e of a step. For EINVAL with
 * a refusal @p why, the function refuses the call with @p why, or appends
 * the notice that the findings could not be read to the prompt for the
 * call of findings_prompt() (findings_prompted()). For any other error, it
 * fails the call with @p e.
 */
static void
findings_stop (struct mcp_slot  *slot,
               int               e,
               struct cstr_view  why)
{
	struct findings_call const *call = *mcp_slot_own(slot);
	if (e != EINVAL || !why.len)
		findings_end(slot, e, true);
	else if (call->prompt)
		findings_prompted(slot, e);
	else
		mcp_slot_tell(slot, why, TOOL_FAILED);
}

/* Spawns git for @p stage with the command @p cmd, the words @p words and
 * the input @p in, which the function frees. A commit updates @p ref. If
 * git cannot start, the call is refused with the reason (findings_stop()).
 */
static void
findings_run (struct mcp_slot   *slot,
              uint8_t            stage,
              enum store_cmd     cmd,
              char const *const *words,
              char const        *ref,
              struct buf        *in)
{
	struct findings_call *call = *mcp_slot_own(slot);
	int e = 0;
	call->stage = stage;
	child_destroy(&call->child);
	call->child = store_spawn(call->notes->tool->store, slot, cmd, words, ref, buf_peek(in),
	                          &e);
	buf_fini(in);
	if (call->child)
		return;
	char buf[96];
	findings_stop(slot, EINVAL, child_spawn_why(e, "git", buf, sizeof buf));
}

/* Writes to @p ref the branch of the findings of the agent @p author. */
static void
findings_ref (char const *author,
              char        ref[static MEMBER_REF])
{
	struct member agent = { .kind = MEMBER_AGENT };
	(void)util_format(agent.name, sizeof agent.name, "%s", author);
	member_ref(&agent, MEMBER_BRANCH_FINDINGS, ref);
}

static void findings_got_refs (struct mcp_slot *slot);

/* Starts the action of the call in @p slot anew: the table of the members
 * reads the records, which name the branches of findings
 * (member_table_read()).
 */
static void
findings_load (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	call->branches.n = 0;
	call->wanted.n = 0;
	call->prints.n = 0;
	call->branch = 0;
	call->done = 0;
	call->batch = 0;
	call->last = 0;
	call->flags &= CALL_CITED;
	for (size_t i = 0; i < call->findings.n; ++i)
		finding_fini(&call->findings.items[i]);
	call->findings.n = 0;
	call->stage = STAGE_REFS;
	child_destroy(&call->child);
	if (member_table_read(call->notes->members, &call->wait) == EINPROGRESS) {
		(void)mcp_slot_park(slot);
		return;
	}
	findings_got_refs(slot);
}

/* Runs git to read the next files of call->wanted, from call->done on, as
 * many as one output of git holds (store_read_fits()), at least one. The
 * input of cat-file names each file by its blob ID, or else by its path on
 * the branch of its author.
 */
static void
findings_read_batch (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct buf in = {};
	uint64_t used = 0;
	uint32_t k = call->done;
	int e = 0;
	while (!e && k < call->wanted.n) {
		struct findings_file const *w = &call->wanted.items[k];
		bool const fits = store_read_fits(&used, w->bytes);
		if (!fits && k > call->done)
			break;
		if (oid_len(&w->id)) {
			e = buf_printf(&in, "%s\n", w->id.hex);
		} else {
			char ref[MEMBER_REF];
			char path[FINDING_FILE];
			findings_ref(w->author, ref);
			finding_path(w->number, path);
			e = notes_read_input(&in, ref, cstr_view(path));
		}
		++k;
		/* A file that does not fit is read alone. */
		if (!fits)
			break;
	}
	if (e) {
		buf_fini(&in);
		findings_end(slot, e, true);
		return;
	}
	call->batch = k;
	findings_run(slot, STAGE_READ, STORE_READ, nullptr, nullptr, &in);
}

/* Appends @p file to the files that cat-file reads for @p call. Returns 0
 * or ENOMEM.
 */
static int
findings_queue (struct findings_call *call,
                struct findings_file  file)
{
	int const e = util_grow(call->wanted.items, call->wanted.cap, call->wanted.n + 1);
	if (!e)
		call->wanted.items[call->wanted.n++] = file;
	return e;
}

/* Runs git to read the finding of the id of expand by the path of its
 * file on the branch of its author.
 */
static void
findings_read_one (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	int const e = findings_queue(call, (struct findings_file){ .author = call->author,
	                                                           .number = call->number });
	if (e) {
		findings_end(slot, e, true);
		return;
	}
	findings_read_batch(slot);
}

static void findings_act (struct mcp_slot *slot);

/* Runs git to list the files of the findings of the branch call->branch,
 * or reads the files that the action needs once every branch is listed.
 * Without files to read, the action goes on at once.
 */
static void
findings_next (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	if (call->branch < call->branches.n) {
		char const *const words[] = { call->branches.items[call->branch].tip.hex, "findings" };
		findings_run(slot, STAGE_FILES, STORE_FILES, words, nullptr, &(struct buf){});
		return;
	}
	if (call->wanted.n) {
		findings_read_batch(slot);
		return;
	}
	findings_act(slot);
}

/* Copies the branches of findings that the action of the call in @p slot
 * needs from the records of the members, once the table has read them:
 * every branch for show and for list without author, the branch of the
 * author for list with author, and else the branch of the agent. For add
 * with about, cat-file first checks the finding that about names on the
 * branch of its author.
 */
static void
findings_got_refs (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	/* The table writes the reason of a read that failed. */
	if (call->wait.e) {
		findings_stop(slot, call->wait.e == ENOMEM ? ENOMEM : EINVAL,
		              *call->wait.why ? cstr_view(call->wait.why) : unread);
		return;
	}
	bool const every = call->action == FINDINGS_SHOW ||
	                   (call->action == FINDINGS_LIST && !*call->author);
	struct cstr_view const only = cstr_view(call->action == FINDINGS_LIST
	                                        ? call->author : findings_session(call)->agent);
	char author[MEMBER_NAME] = "";
	uint32_t number = 0;
	if (*call->about)
		(void)finding_id_parse(cstr_view(call->about), author, &number);
	struct cstr_view const about = cstr_view(author);
	struct oid tip = {};
	struct member_info const *info;
	for (size_t i = 0; (info = member_table_info(call->notes->members, i)); ++i) {
		if (info->kind != MEMBER_AGENT || !oid_len(&info->findings))
			continue;
		if (about.len && cstr_view_equal(info->name, about))
			tip = info->findings;
		if (!every && !cstr_view_equal(info->name, only))
			continue;
		if (util_grow(call->branches.items, call->branches.cap, call->branches.n + 1)) {
			findings_end(slot, ENOMEM, true);
			return;
		}
		struct findings_branch *b = &call->branches.items[call->branches.n++];
		b->tip = info->findings;
		(void)util_format(b->author, sizeof b->author, "%.*s", (int)info->name.len,
		                  info->name.data);
	}
	if (!oid_len(&tip)) {
		findings_next(slot);
		return;
	}
	char path[FINDING_FILE];
	finding_path(number, path);
	struct buf in = {};
	int const e = notes_read_input(&in, tip.hex, cstr_view(path));
	if (e) {
		buf_fini(&in);
		findings_end(slot, e, true);
		return;
	}
	findings_run(slot, STAGE_CHECK, STORE_CHECK, nullptr, nullptr, &in);
}

/* Handles the output of cat-file for the finding that about names: a blob
 * if the finding exists.
 */
static void
findings_got_check (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct span const out = buf_peek(&call->child->out);
	struct cstr_view rest = { (char const *)out.ptr, out.len };
	struct cstr_view line, size;
	struct oid id;
	if (cstr_view_cut(&rest, '\n', &line) && store_kind(line, &id, &size) == STORE_KIND_BLOB)
		call->flags |= CALL_ABOUT;
	findings_next(slot);
}

/* Returns true if the call reads the finding @p number of @p author. */
static bool
findings_wants (struct findings_call const *call,
                char const                 *author,
                uint32_t                    number)
{
	switch (call->action) {
	case FINDINGS_LIST:
	case FINDINGS_SHOW:
	case FINDINGS_EXPAND:
		return true;
	case FINDINGS_UPDATE:
		return number == call->number && !strcmp(author, call->author);
	}
	return false;
}

/* Handles the output of ls-tree for the branch call->branch. For each file
 * of a finding, the function notes the highest number of the agent and
 * whether the finding of id exists, and adds the file to the input of
 * cat-file if the action reads it.
 */
static void
findings_got_files (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct findings_branch const *b = &call->branches.items[call->branch];
	char const *agent = findings_session(call)->agent;
	struct span out = buf_peek(&call->child->out);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	struct notes_file file;
	int e = 0;
	while (!e && out.len && !(e = notes_file_next(&out, &file))) {
		uint32_t const n = finding_path_number((struct cstr_view){ (char const *)file.name.ptr,
		                                                          file.name.len });
		if (!n)
			continue;
		if (!strcmp(b->author, agent) && n > call->last)
			call->last = n;
		if (call->number == n && !strcmp(b->author, call->author))
			call->flags |= CALL_FOUND;
		if (findings_wants(call, b->author, n))
			e = findings_queue(call, (struct findings_file){
				.bytes  = file.bytes,
				.author = b->author,
				.number = n,
				.id     = file.id,
			});
	}
	if (e) {
		findings_stop(slot, e == EIO ? EINVAL : e, unread);
		return;
	}
	++call->branch;
	findings_next(slot);
}

/* Handles the output of cat-file: the files of call->wanted from
 * call->done to call->batch, in order. A file that askai cannot read, or
 * whose finding is not the one of its author and number, refuses the call.
 * Then cat-file reads the next files, or the action goes on after the last
 * file.
 */
static void
findings_got_read (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct span out = buf_peek(&call->child->out);
	loop_work(mcp_slot_loop(slot), LOOP_PARSE, out.len);
	char why[256] = "";
	int e = util_grow(call->findings.items, call->findings.cap, call->wanted.n);
	for (size_t i = call->done; !e && i < call->batch; ++i) {
		struct findings_file const *w = &call->wanted.items[i];
		struct finding *f = &call->findings.items[call->findings.n++];
		*f = finding();
		struct oid id;
		struct span data;
		e = store_object_next(&out, &id, &data);
		if (!e)
			e = finding_parse(f, (struct cstr_view){ (char const *)data.ptr, data.len });
		/* expand with id and the findings of the items that the evidence
		 * cites again read the file of a finding by its path. */
		if (e == ENOENT)
			e = util_refuse(why, sizeof why, "No finding has the id %s#%" PRIu32 ". list "
			                "shows the findings.", w->author, w->number);
		else if (e != ENOMEM &&
		         (e || f->number != w->number || strcmp(f->author, w->author)))
			e = util_refuse(why, sizeof why, "Could not read finding %s#%" PRIu32 ": "
			                "unknown format.", w->author, w->number);
	}
	if (e) {
		findings_stop(slot, e, cstr_view(why));
		return;
	}
	call->done = call->batch;
	if (call->done < call->wanted.n) {
		findings_read_batch(slot);
		return;
	}
	findings_act(slot);
}

/* Appends to call->out the evidence of call->change, the change that the
 * commit made, as " Evidence bro#5/1 from call c4, bro#5/2 from c7:3-9,
 * bro#5/3 from cuh#2/1." for the finding @p id: the reference of the cite
 * of an item cited again, and else the reference of the call or of its
 * lines (evidence_ref_put()). Returns true, or false if out of memory.
 */
static bool
findings_cited (struct findings_call *call,
                char const           *id)
{
	struct finding_change const *c = &call->change;
	/* The cites are in the order of the items that they make. */
	size_t k = 0;
	for (size_t i = 0; i < c->evidence.n; ++i) {
		struct evidence const *e = &c->evidence.items[i];
		struct evidence_ref ref = { .number = e->call, .form = EVIDENCE_REF_CALL };
		if (*e->from && k < call->cites.n) {
			ref = call->cites.items[k++].ref;
		} else if (e->kind == EVIDENCE_LINES) {
			ref.form = EVIDENCE_REF_LINES;
			ref.n_ranges = (uint16_t)e->ranges.n;
			memcpy(ref.ranges, e->ranges.items, e->ranges.n * sizeof *ref.ranges);
		}
		if (!dstr_printf(&call->out, "%s %s/%" PRIu32 " from %s", i ? "," : " Evidence", id,
		                 e->item, ref.form == EVIDENCE_REF_CALL ? "call " : "") ||
		    evidence_ref_put(&ref, &call->out))
			return false;
	}
	return !c->evidence.n || dstr_cat(&call->out, ".");
}

/* Handles the end of fast-import. After a commit, the result names the ID,
 * the state, the evidence items and the commit. After a race with another
 * writer, the call reads the branches again and makes the commit again,
 * until it has made STORE_TRIES commits.
 */
static void
findings_committed (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	char const *agent = findings_session(call)->agent;
	char id[FINDING_ID];
	finding_id_format(agent, call->number, id);
	if (child_ok(c)) {
		struct oid commit;
		if (store_committed(buf_peek(&c->out), &commit))
			LOG(LOG_WARN, "findings: git fast-import gave no commit id");
		char const *state = finding_state_names[call->change.state].data;
		(void)dstr_clear(&call->out);
		bool ok;
		if (call->action == FINDINGS_ADD)
			ok = dstr_printf(&call->out, "Added %s (%s).", id, state);
		else
			ok = dstr_printf(&call->out, "%s is %s %s.", id,
			                 call->flags & CALL_SAME ? "still" : "now", state);
		if (ok && call->flags & CALL_CLAIM)
			ok = dstr_cat(&call->out, " The claim changed.");
		if (ok)
			ok = findings_cited(call, id);
		if (ok && oid_len(&commit))
			ok = dstr_printf(&call->out, " Commit %.8s.", commit.hex);
		(void)mcp_slot_noted(slot);
		findings_end(slot, ok ? 0 : ENOMEM, false);
		return;
	}
	struct span const out = buf_peek(&c->out);
	if (!(c->end.flags & CHILD_LATE) && store_raced(out)) {
		char ref[MEMBER_REF];
		findings_ref(agent, ref);
		int64_t const age = store_stale(call->notes->tool->store, ref, "findings");
		if (age) {
			mcp_slot_tellf(slot, TOOL_FAILED, "Could not record the finding. The findings of %s "
			               "have been locked for %" PRId64 " s.", agent, age);
			return;
		}
		if (++call->tries < STORE_TRIES) {
			findings_load(slot);
			return;
		}
		mcp_slot_tellf(slot, TOOL_FAILED, "Could not record the finding. Other ask calls of %s "
		               "changed the findings at the same time. Repeat the call.", agent);
		return;
	}
	char const *why = (char const *)out.ptr;
	LOG(LOG_WARN, "findings: git could not record %s: %.*s", id,
	    (int)util_clip(why, out.len, 300), why);
	if (c->end.flags & CHILD_LATE)
		mcp_slot_tell(slot, CSTR_VIEW("Could not record the finding. The write timed out."),
		              TOOL_FAILED);
	else
		mcp_slot_tell(slot, CSTR_VIEW("Could not record the finding."), TOOL_FAILED);
}

/* Writes to @p message the subject of the commit of the change of the
 * call to the finding @p id, whose state was @p before: "Add bro#5
 * (verified)", "Update bro#5 (candidate to verified)", "Update bro#5
 * (claim)" or both.
 */
static void
findings_subject (struct findings_call const *call,
                  char const                 *id,
                  enum finding_state          before,
                  bool                        corrects,
                  char                        message[static STORE_MESSAGE])
{
	char const *after = finding_state_names[call->change.state].data;
	if (call->action == FINDINGS_ADD)
		(void)util_format(message, STORE_MESSAGE, "Add %s (%s)", id, after);
	else if (before == call->change.state)
		(void)util_format(message, STORE_MESSAGE, "Update %s (%s)", id, corrects ? "claim"
		                                                                     : after);
	else
		(void)util_format(message, STORE_MESSAGE, "Update %s (%s to %s%s)", id,
		                  finding_state_names[before].data, after, corrects ? ", claim" : "");
}

/* Runs git to commit the change of the call in @p slot to @p f, the finding
 * of an update or a new finding: the file of the finding with the change,
 * and the archive of its evidence, on the branch of the agent.
 */
static void
findings_commit (struct mcp_slot *slot,
                 struct finding  *f)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct member_session const *session = findings_session(call);
	struct member agent = { .kind = MEMBER_AGENT };
	(void)util_format(agent.name, sizeof agent.name, "%s", session->agent);
	char ref[MEMBER_REF];
	member_ref(&agent, MEMBER_BRANCH_FINDINGS, ref);
	struct oid tip = {};
	for (size_t i = 0; i < call->branches.n; ++i)
		if (!strcmp(call->branches.items[i].author, session->agent))
			tip = call->branches.items[i].tip;
	enum finding_state const before = finding_state(f);
	struct cstr_view const claim = cstr_view_dstr(&call->claim);
	bool const corrects = call->action == FINDINGS_UPDATE && claim.len &&
	                      !cstr_view_equal(claim, cstr_view_dstr(&f->claim));
	if (call->action == FINDINGS_UPDATE && before == call->change.state)
		call->flags |= CALL_SAME;
	if (corrects)
		call->flags |= CALL_CLAIM;
	dstr_auto json = DSTR_INIT;
	int e = finding_apply(f, &call->change, call->action == FINDINGS_UPDATE ? claim
	                                                                         : CSTR_VIEW(""));
	if (e) {
		findings_end(slot, e, true);
		return;
	}
	e = finding_render(f, &json);
	finding_unapply(f, &call->change);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, dstr_len(&json));
	if (e == EFBIG) {
		mcp_slot_tell(slot, CSTR_VIEW("The finding would be larger than 4 MiB. Nothing was "
		                              "recorded."), TOOL_FAILED);
		return;
	}
	char id[FINDING_ID];
	char path[FINDING_FILE];
	char message[STORE_MESSAGE];
	finding_id(f, id);
	finding_path(f->number, path);
	findings_subject(call, id, before, corrects, message);
	call->number = f->number;
	struct store_part part;
	e = e ? e : store_part_file(&part, &call->files, cstr_view(path), cstr_view_dstr(&json));
	struct store_commit commit = {
		.message = cstr_view(message),
		.files = buf_peek(&call->files),
		.ref = ref,
		.tip = &tip,
	};
	struct member_byline byline;
	member_byline_put(&byline, &commit, &agent, mcp_slot_member(slot), mcp_slot_call(slot),
	                  session->service);
	struct buf in = {};
	e = e ? e : store_commit_input(&commit, &in);
	/* The archive stays for a commit after a race. */
	struct span const files = buf_peek(&call->files);
	buf_cut(&call->files, call->archive, files.len - call->archive);
	if (e) {
		buf_fini(&in);
		findings_end(slot, e, true);
		return;
	}
	findings_run(slot, STAGE_COMMIT, STORE_COMMIT, nullptr, ref, &in);
}

/* Returns the evidence item @p item of @p f, or nullptr for no such item. */
static struct evidence const *
findings_item (struct finding const *f,
               uint32_t              item)
{
	for (size_t k = 0; k < f->changes.n; ++k)
		for (size_t i = 0; i < f->changes.items[k].evidence.n; ++i)
			if (f->changes.items[k].evidence.items[i].item == item)
				return &f->changes.items[k].evidence.items[i];
	return nullptr;
}

/* Stores in @p d the JSON object of the item @p e without spaces, as the
 * ring stores it (struct seen_item). Returns 0 or ENOMEM.
 */
static int
findings_record (struct evidence const *e,
                 struct dstr           *d)
{
	int err = 0;
	struct json_object *obj = evidence_json(e, &err);
	struct cstr_view const wire = jb_wire(obj);
	err = err ? err : wire.data ? cstr_view_store(wire, d) : ENOMEM;
	json_object_put(obj);
	return err;
}

/* Appends to @p in the line of cat-file that reads the archive file of the
 * material @p b on the branch of the findings of @p author. Returns 0 or
 * the error of buf_printf().
 */
static int
findings_archive_line (struct buf                 *in,
                       char const                 *author,
                       struct evidence_blob const *b)
{
	char ref[MEMBER_REF];
	char path[EVIDENCE_PATH];
	findings_ref(author, ref);
	evidence_path(b->sha256, path);
	return notes_read_input(in, ref, cstr_view(path));
}

/* Returns true if the archive files of the item @p e have the git IDs
 * @p blobs, so that a finding can cite the item again.
 */
static bool
findings_stored (struct evidence const *e,
                 struct oid const       blobs[static 2])
{
	return oid_len(&blobs[1]) && (!(e->flags & EVIDENCE_REQUEST) || oid_len(&blobs[0]));
}

/* Records that the item @p id, such as cuh#2/1, whose record is in
 * shown->record and whose archive files have the git IDs @p blobs, is the
 * @p len bytes of the output of the call in @p slot from @p at on, so that
 * a finding can cite the item once a result has shown it
 * (mcp_slot_evidence()). Returns 0 or the error of mcp_slot_evidence().
 */
static int
findings_shown (struct mcp_slot  *slot,
                struct seen_item *shown,
                char const       *id,
                struct oid const  blobs[static 2],
                size_t            at,
                size_t            len)
{
	memcpy(shown->blobs, blobs, sizeof shown->blobs);
	(void)util_format(shown->id, sizeof shown->id, "%s", id);
	return mcp_slot_evidence(slot, shown, at, len);
}

/* Returns true if expand prints the items of @p f: the finding of the id,
 * or without id an open finding of the agent @p agent.
 */
static bool
findings_expands (struct findings_call const *call,
                  struct finding const       *f,
                  char const                 *agent)
{
	return *call->author ? f->number == call->number && !strcmp(f->author, call->author)
	                     : FINDING_OPEN >> finding_state(f) & 1 && !strcmp(f->author, agent);
}

/* Appends to call->out the external items of @p f with the ID @p id, and
 * returns their number, or SIZE_MAX if out of memory.
 */
static size_t
findings_externals (struct findings_call *call,
                    struct finding const *f,
                    char const           *id)
{
	size_t n = 0;
	for (size_t k = 0; k < f->changes.n; ++k)
		for (size_t i = 0; i < f->changes.items[k].external.n; ++i, ++n) {
			struct cstr_view const t = cstr_view_dstr(&f->changes.items[k].external.items[i]);
			if (!dstr_printf(&call->out, "[askai: external item of %s: %.*s]\n[askai: askai "
			                 "does not store or show this material.]\n", id, (int)t.len,
			                 t.data))
				return SIZE_MAX;
		}
	return n;
}

/* Writes the output of expand from @p out, the output of STORE_READ for
 * the archive files of call->prints: each finding with its items and its
 * external items. Each item that a result can show whole is recorded for
 * the ring (findings_shown()). The output ends with the items that did not
 * fit, and for a turn, with what the turn saw.
 */
static void
findings_expanded (struct mcp_slot *slot,
                   struct span      out)
{
	struct findings_call *call = *mcp_slot_own(slot);
	char const *agent = findings_session(call)->agent;
	bool const turn = !mcp_slot_from_client(slot);
	struct seen_item shown = { .record = DSTR_INIT };
	char more_id[SEEN_ID] = "";
	char seen_id[SEEN_ID] = "";
	size_t printed = 0;
	size_t more = 0;
	size_t listed = 0;
	size_t whole = 0;
	int e = 0;
	(void)dstr_clear(&call->out);
	for (size_t i = 0; !e && i < call->findings.n; ++i) {
		struct finding const *f = &call->findings.items[i];
		if (!findings_expands(call, f, agent))
			continue;
		char id[FINDING_ID];
		finding_id(f, id);
		struct cstr_view const claim = finding_claim_line(f);
		if (!dstr_printf(&call->out, "Evidence of finding %s by %s: %.*s\n", id, f->author,
		                 (int)claim.len, claim.len ? claim.data : ""))
			e = ENOMEM;
		++listed;
		size_t items = 0;
		for (size_t k = 0; !e && k < f->changes.n; ++k) {
			struct finding_change const *c = &f->changes.items[k];
			for (size_t j = 0; !e && j < c->evidence.n; ++j) {
				struct evidence const *ev = &c->evidence.items[j];
				if (call->item && ev->item != call->item)
					continue;
				++items;
				if (printed == call->prints.n || call->prints.items[printed].e != ev) {
					if (!more++)
						(void)util_format(more_id, sizeof more_id, "%s/%" PRIu32, id,
						                  ev->item);
					continue;
				}
				++printed;
				/* Missing material prints as missing. */
				struct oid blobs[2] = {};
				struct span request = {};
				struct span result = {};
				int r = 0;
				if (ev->flags & EVIDENCE_REQUEST)
					r = store_object_next(&out, &blobs[0], &request);
				if (r != EIO)
					r = store_object_next(&out, &blobs[1], &result);
				if (r == EIO) {
					e = EIO;
					continue;
				}
				size_t const at = dstr_len(&call->out);
				e = evidence_expand(ev, id, f->author,
				                    (struct cstr_view){ (char const *)request.ptr, request.len },
				                    (struct cstr_view){ (char const *)result.ptr, result.len },
				                    &call->out);
				if (e || !turn || !findings_stored(ev, blobs))
					continue;
				(void)util_format(seen_id, sizeof seen_id, "%s/%" PRIu32, id, ev->item);
				e = findings_record(ev, &shown.record);
				e = e ? e : findings_shown(slot, &shown, seen_id, blobs, at,
				                           dstr_len(&call->out) - at);
				++whole;
			}
		}
		size_t externals = 0;
		if (!e && !call->item)
			externals = findings_externals(call, f, id);
		if (externals == SIZE_MAX)
			e = ENOMEM;
		if (!e && !items && !externals &&
		    !dstr_printf(&call->out, "[askai: %s has no evidence items.]\n", id))
			e = ENOMEM;
	}
	dstr_cleanup(&shown.record);
	if (e == EIO) {
		mcp_slot_tell(slot, unread, TOOL_FAILED);
		return;
	}
	bool ok = !e;
	if (ok && !listed)
		ok = dstr_cat(&call->out, "You have no open findings.\n");
	if (ok && more)
		ok = dstr_printf(&call->out, "[askai: %zu more evidence item%s did not fit. Expand each "
		                 "item by id, such as %s.]\n", more, util_s(more), more_id);
	if (ok && whole == 1)
		ok = dstr_printf(&call->out, "[askai: You saw %s whole. Cite %s for verified or "
		                 "disproved from your next turn on.]\n", seen_id, seen_id);
	else if (ok && whole > 1)
		ok = dstr_printf(&call->out, "[askai: You saw the %zu evidence items above whole. Cite "
		                 "the items for verified or disproved from your next turn on.]\n",
		                 whole);
	if (!ok && !e)
		e = ENOMEM;
	findings_end(slot, e, false);
}

/* Runs git to read the archive files of the items that expand prints, in
 * the order of the findings, their changes and their items, until the
 * material would be more than FINDINGS_MATERIAL bytes. Without such items,
 * the call writes the output at once.
 */
static void
findings_expand (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	char const *agent = findings_session(call)->agent;
	struct buf in = {};
	uint64_t bytes = 0;
	bool full = false;
	int e = 0;
	call->prints.n = 0;
	for (size_t i = 0; !e && !full && i < call->findings.n; ++i) {
		struct finding const *f = &call->findings.items[i];
		if (!findings_expands(call, f, agent))
			continue;
		for (size_t k = 0; !e && !full && k < f->changes.n; ++k) {
			struct finding_change const *c = &f->changes.items[k];
			for (size_t j = 0; !e && !full && j < c->evidence.n; ++j) {
				struct evidence const *ev = &c->evidence.items[j];
				if (call->item && ev->item != call->item)
					continue;
				bool const asked = ev->flags & EVIDENCE_REQUEST;
				uint64_t const request = asked ? ev->request.bytes : 0;
				full = request > FINDINGS_MATERIAL - bytes ||
				       ev->result.bytes > FINDINGS_MATERIAL - bytes - request;
				if (full)
					break;
				bytes += request + ev->result.bytes;
				e = util_grow(call->prints.items, call->prints.cap, call->prints.n + 1);
				e = e || !asked ? e : findings_archive_line(&in, f->author, &ev->request);
				e = e ? e : findings_archive_line(&in, f->author, &ev->result);
				if (!e)
					call->prints.items[call->prints.n++] = (struct findings_print){ f, ev };
			}
		}
	}
	if (e) {
		buf_fini(&in);
		findings_end(slot, e, true);
		return;
	}
	if (!buf_peek(&in).len) {
		findings_expanded(slot, (struct span){});
		return;
	}
	findings_run(slot, STAGE_ARCHIVE, STORE_READ, nullptr, nullptr, &in);
}

/* Handles the output of cat-file for the archive files of expand. */
static void
findings_got_archive (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct span const out = buf_peek(&call->child->out);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	findings_expanded(slot, out);
}

static void findings_cite_records (struct mcp_slot *slot);

/* Returns the finding @p number of @p author that @p call read, or
 * nullptr.
 */
static struct finding *
findings_find (struct findings_call const *call,
               char const                 *author,
               uint32_t                    number)
{
	for (size_t i = 0; i < call->findings.n; ++i)
		if (call->findings.items[i].number == number &&
		    !strcmp(call->findings.items[i].author, author))
			return &call->findings.items[i];
	return nullptr;
}

/* Does the action of the call in @p slot once git has read what it needs:
 * writes the list or the finding, or commits an add or an update. For the
 * call of findings_prompt(), it appends the open findings to the prompt
 * (findings_prompted()). An add or an update whose evidence cites items of
 * findings again first reads those items (findings_cite_records()), and
 * then comes back here.
 */
static void
findings_act (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct member_session const *session = findings_session(call);
	char id[FINDING_ID];
	finding_id_format(call->author, call->number, id);
	child_destroy(&call->child);
	finding_sort(call->findings.items, call->findings.n);
	if (call->prompt) {
		findings_prompted(slot, 0);
		return;
	}
	char const *missing = nullptr;
	switch (call->action) {
	case FINDINGS_LIST:
		findings_end(slot, finding_list(call->findings.items, call->findings.n, session->name,
		                                call->author, &call->out), false);
		return;
	case FINDINGS_EXPAND:
		if (call->item && call->findings.n && !findings_item(call->findings.items, call->item)) {
			mcp_slot_tellf(slot, TOOL_FAILED, "No evidence item has the id %s/%" PRIu32 ". "
			               "show with id %s lists the items.", id, call->item, id);
			return;
		}
		findings_expand(slot);
		return;
	case FINDINGS_SHOW:
	case FINDINGS_UPDATE:
		if (!(call->flags & CALL_FOUND))
			missing = id;
		break;
	case FINDINGS_ADD:
		if (*call->about && !(call->flags & CALL_ABOUT))
			missing = call->about;
		break;
	}
	if (missing) {
		mcp_slot_tellf(slot, TOOL_FAILED, "No finding has the id %s. list shows the findings.",
		               missing);
		return;
	}
	/* The items that the evidence cites again are read after the checks
	 * above, so that the refusal of an item that the turns had not seen
	 * comes after the refusal of a finding that does not exist. */
	if (call->cites.n && !(call->flags & CALL_CITED)) {
		findings_cite_records(slot);
		return;
	}
	struct finding *f = findings_find(call, call->author, call->number);
	if (call->action == FINDINGS_SHOW) {
		if (!f) {
			findings_end(slot, EIO, true);
			return;
		}
		int e = finding_show(f, call->findings.items, call->findings.n, &call->out);
		bool const items = f->changes.n && f->changes.items[f->changes.n - 1].items;
		if (!e && items) {
			char const *only = mcp_slot_from_client(slot)
			                 ? "" : " Only expand counts as reading the evidence.";
			if (!dstr_printf(&call->out, "[askai: expand with id %s prints the stored "
			                 "evidence.%s]\n", id, only))
				e = ENOMEM;
		}
		findings_end(slot, e, false);
		return;
	}
	if (call->action == FINDINGS_UPDATE) {
		if (f)
			findings_commit(slot, f);
		else
			findings_end(slot, EIO, true);
		return;
	}
	/* The number of a new finding is one more than the highest number of
	 * the agent. */
	static_assert(UINT32_MAX == 4294967295, "update the refusal");
	if (call->last == UINT32_MAX) {
		mcp_slot_tellf(slot, TOOL_FAILED, "%s#4294967295 exists. askai cannot give a finding "
		               "of %s a higher number.", session->agent, session->agent);
		return;
	}
	struct finding added = finding();
	(void)util_format(added.author, sizeof added.author, "%s", session->agent);
	(void)util_format(added.about, sizeof added.about, "%s", call->about);
	added.number = call->last + 1;
	added.conversation = mcp_slot_member(slot)->conversation;
	if (cstr_view_store(cstr_view_dstr(&call->claim), &added.claim)) {
		finding_fini(&added);
		findings_end(slot, ENOMEM, true);
		return;
	}
	findings_commit(slot, &added);
	finding_fini(&added);
}

/* Returns true if an earlier reference of call->cites than @p k names the
 * item of the reference @p k and is printed with it (CITE_SHOW).
 */
static bool
findings_cite_again (struct findings_call const *call,
                     size_t                      k)
{
	struct evidence_ref const *ref = &call->cites.items[k].ref;
	for (size_t i = 0; i < k; ++i) {
		struct findings_cite const *c = &call->cites.items[i];
		if (c->flags & CITE_SHOW && c->ref.number == ref->number && c->ref.item == ref->item &&
		    !strcmp(c->ref.author, ref->author))
			return true;
	}
	return false;
}

/* Refuses the add or update of the call in @p slot, whose evidence cites
 * items that the turns had not seen (CITE_SHOW), and prints those items,
 * each once, with the material @p material, so that they count as shown in
 * this turn. Items that do not fit in one result together are named
 * instead, for expand to print a page at a time.
 */
static void
findings_unseen (struct mcp_slot        *slot,
                 struct cstr_view const  material[static EVIDENCE_REFS][2])
{
	struct findings_call *call = *mcp_slot_own(slot);
	char ids[EVIDENCE_REFS][SEEN_ID];
	size_t n = 0;
	for (size_t i = 0; i < call->cites.n; ++i)
		if (call->cites.items[i].flags & CITE_SHOW && !findings_cite_again(call, i))
			evidence_ref_id(&call->cites.items[i].ref, ids[n++]);
	dstr_auto names = DSTR_INIT;
	bool ok = true;
	for (size_t i = 0; ok && i < n; ++i)
		ok = dstr_printf(&names, "%s%s", !i ? "" : i + 1 < n ? ", " : " and ", ids[i]);
	(void)dstr_clear(&call->out);
	if (ok && call->action == FINDINGS_ADD)
		ok = dstr_cat(&call->out, "The finding was not added. ");
	else if (ok)
		ok = dstr_printf(&call->out, "%s#%" PRIu32 " did not change. ", call->author,
		                 call->number);
	ok = ok && dstr_printf(&call->out, "%s needs evidence that askai showed you before this "
	                       "turn. askai had not shown you %s. ",
	                       finding_state_names[call->change.state].data, dstr_get(&names));
	size_t const head = dstr_len(&call->out);
	ok = ok && dstr_printf(&call->out, "askai prints %s below. Repeat the %s in your next "
	                       "turn.\n", dstr_get(&names), findings_actions[call->action].data);
	/* The place of each item in the output. */
	size_t at[EVIDENCE_REFS + 1] = {};
	struct evidence item = evidence();
	int e = ok ? 0 : ENOMEM;
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite *cite = &call->cites.items[i];
		at[i] = dstr_len(&call->out);
		if (!(cite->flags & CITE_SHOW) || findings_cite_again(call, i))
			continue;
		char finding[FINDING_ID];
		finding_id_format(cite->ref.author, cite->ref.number, finding);
		e = evidence_parse_text(&item, cstr_view_dstr(&cite->record));
		if (!e)
			e = evidence_expand(&item, finding, cite->ref.author, material[i][0],
			                    material[i][1], &call->out);
	}
	evidence_fini(&item);
	at[call->cites.n] = dstr_len(&call->out);
	if (!e && at[call->cites.n] > mcp_slot_window_max(slot)) {
		(void)dstr_erase(&call->out, head, SIZE_MAX);
		e = dstr_printf(&call->out, "The evidence is longer than one result. Read %s with "
		                "expand, a page at a time. Repeat the %s in a later turn.",
		                dstr_get(&names), findings_actions[call->action].data) ? 0 : ENOMEM;
		findings_end(slot, e, true);
		return;
	}
	struct seen_item shown = { .record = DSTR_INIT };
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite *cite = &call->cites.items[i];
		if (at[i] == at[i + 1])
			continue;
		char id[SEEN_ID];
		evidence_ref_id(&cite->ref, id);
		e = cstr_view_store(cstr_view_dstr(&cite->record), &shown.record);
		e = e ? e : findings_shown(slot, &shown, id, cite->blobs, at[i], at[i + 1] - at[i]);
	}
	dstr_cleanup(&shown.record);
	findings_end(slot, e, true);
}

/* Makes the items that the evidence of the call in @p slot cites again,
 * from their records and, for lines, from the stored lines @p material, or
 * refuses the call for items that the turns had not seen (CITE_SHOW).
 * Whole items go into the commit as the git IDs of their archive files.
 * Then the action goes on (findings_act()).
 */
static void
findings_cite_end (struct mcp_slot        *slot,
                   struct cstr_view const  material[static EVIDENCE_REFS][2])
{
	struct findings_call *call = *mcp_slot_own(slot);
	for (size_t i = 0; i < call->cites.n; ++i)
		if (call->cites.items[i].flags & CITE_SHOW) {
			findings_unseen(slot, material);
			return;
		}
	char why[1024] = "";
	dstr_auto lines = DSTR_INIT;
	int e = 0;
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite const *cite = &call->cites.items[i];
		struct evidence *item = &call->change.evidence.items[cite->index];
		e = evidence_recite(item, &lines, &cite->ref, cstr_view_dstr(&cite->record),
		                    material[i][1], why, sizeof why);
		char path[EVIDENCE_PATH];
		if (!e && cite->ref.form == EVIDENCE_REF_ITEM_LINES) {
			struct store_part part;
			evidence_path(item->result.sha256, path);
			e = store_part_file(&part, &call->files, cstr_view(path), cstr_view_dstr(&lines));
			continue;
		}
		if (!e && item->flags & EVIDENCE_REQUEST) {
			evidence_path(item->request.sha256, path);
			e = store_part_blob(&call->files, cstr_view(path), &cite->blobs[0]);
		}
		if (!e) {
			evidence_path(item->result.sha256, path);
			e = store_part_blob(&call->files, cstr_view(path), &cite->blobs[1]);
		}
	}
	if (e) {
		findings_stop(slot, e, cstr_view(why));
		return;
	}
	call->archive = buf_peek(&call->files).len;
	call->flags |= CALL_CITED;
	findings_act(slot);
}

/* Handles the output of cat-file for the archive files of the items that
 * the evidence cites again (CITE_READ): their git IDs, and their material
 * for findings_cite_end().
 */
static void
findings_got_blobs (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct span out = buf_peek(&call->child->out);
	struct cstr_view material[EVIDENCE_REFS][2] = {};
	for (size_t i = 0; i < call->cites.n; ++i) {
		struct findings_cite *cite = &call->cites.items[i];
		if (!(cite->flags & CITE_READ))
			continue;
		for (size_t k = cite->flags & CITE_REQUEST ? 0 : 1; k < array_size(cite->blobs); ++k) {
			struct span data;
			int const e = store_object_next(&out, &cite->blobs[k], &data);
			if (e == ENOENT) {
				char id[SEEN_ID];
				evidence_ref_id(&cite->ref, id);
				mcp_slot_tellf(slot, TOOL_FAILED, "The stored material of %s is missing.", id);
				return;
			}
			if (e) {
				mcp_slot_tell(slot, unread, TOOL_FAILED);
				return;
			}
			material[i][k] = (struct cstr_view){ (char const *)data.ptr, data.len };
		}
	}
	findings_cite_end(slot, material);
}

/* Decides which items of call->cites the refusal of the call in @p slot
 * prints (CITE_SHOW): for verified and disproved, the items that a result
 * did not show whole before this turn. Then runs git to read the archive
 * files of those items, of items cited in part, and of items whose git IDs
 * the ring does not store (CITE_READ). Without such items, the call goes
 * on at once.
 */
static void
findings_cite_blobs (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	bool const strict = FINDING_SHOWN >> call->change.state & 1;
	uint32_t const turn = mcp_slot_turn(slot);
	struct evidence item = evidence();
	struct buf in = {};
	uint64_t bytes = 0;
	int e = 0;
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite *cite = &call->cites.items[i];
		e = evidence_parse_text(&item, cstr_view_dstr(&cite->record));
		if (e == EINVAL) {
			evidence_fini(&item);
			buf_fini(&in);
			mcp_slot_tellf(slot, TOOL_FAILED, "Could not read finding %s#%" PRIu32 ": unknown "
			               "format.", cite->ref.author, cite->ref.number);
			return;
		}
		if (e)
			break;
		bool const seen = cite->turn && cite->turn < turn;
		bool const asked = item.flags & EVIDENCE_REQUEST;
		cite->flags = (uint16_t)((strict && !seen ? CITE_SHOW : 0) | (asked ? CITE_REQUEST : 0));
		if (!(cite->flags & CITE_SHOW) && cite->ref.form == EVIDENCE_REF_ITEM &&
		    findings_stored(&item, cite->blobs))
			continue;
		cite->flags |= CITE_READ;
		uint64_t const request = asked ? item.request.bytes : 0;
		if (request > FINDINGS_MATERIAL - bytes ||
		    item.result.bytes > FINDINGS_MATERIAL - bytes - request) {
			evidence_fini(&item);
			buf_fini(&in);
			mcp_slot_tell(slot, CSTR_VIEW("The cited evidence items have more than 8 MiB of "
			                              "stored material. Cite fewer items in one call."),
			              TOOL_FAILED);
			return;
		}
		bytes += request + item.result.bytes;
		/* The ring stores the git IDs of an item that a result showed. */
		for (size_t k = asked ? 0 : 1; !e && k < array_size(cite->blobs); ++k)
			e = oid_len(&cite->blobs[k])
			  ? buf_printf(&in, "%s\n", cite->blobs[k].hex)
			  : findings_archive_line(&in, cite->ref.author, k ? &item.result : &item.request);
	}
	evidence_fini(&item);
	if (e) {
		buf_fini(&in);
		findings_end(slot, e, true);
		return;
	}
	if (!buf_peek(&in).len) {
		findings_cite_end(slot, (struct cstr_view [EVIDENCE_REFS][2]){});
		return;
	}
	findings_run(slot, STAGE_BLOBS, STORE_READ, nullptr, nullptr, &in);
}

/* Makes the record of each item that the evidence of the call in @p slot
 * cites again and that the ring does not store, from the findings that the
 * call read. If such a finding is not read yet, cat-file first reads the
 * file of the finding by its path (findings_read_batch()), and the call
 * comes back here through findings_act(). Then the call reads the archive
 * files of the items (findings_cite_blobs()).
 */
static void
findings_cite_records (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	size_t const read = call->wanted.n;
	char why[256] = "";
	int e = 0;
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite *cite = &call->cites.items[i];
		struct evidence_ref const *ref = &cite->ref;
		if (dstr_len(&cite->record))
			continue;
		struct finding const *f = findings_find(call, ref->author, ref->number);
		struct evidence const *item = f ? findings_item(f, ref->item) : nullptr;
		/* cat-file reads a finding that the call has not read yet, once. */
		bool queued = f != nullptr;
		for (size_t k = read; !queued && k < call->wanted.n; ++k)
			queued = call->wanted.items[k].number == ref->number &&
			         !strcmp(call->wanted.items[k].author, ref->author);
		if (item)
			e = findings_record(item, &cite->record);
		else if (f)
			e = util_refuse(why, sizeof why, "No evidence item has the id %s#%" PRIu32 "/%"
			                PRIu32 ". show with id %s#%" PRIu32 " lists the items.",
			                ref->author, ref->number, ref->item, ref->author,
			                ref->number);
		else if (!queued)
			e = findings_queue(call, (struct findings_file){ .author = ref->author,
			                                                 .number = ref->number });
	}
	if (e) {
		findings_stop(slot, e, cstr_view(why));
		return;
	}
	if (call->wanted.n > read) {
		findings_read_batch(slot);
		return;
	}
	findings_cite_blobs(slot);
}

static void findings_got_calls (struct mcp_slot *slot);

void
findings_step (struct mcp_slot *slot)
{
	static tool_step_fn *const steps[] = {
		[STAGE_REFS]    = findings_got_refs,
		[STAGE_CHECK]   = findings_got_check,
		[STAGE_FILES]   = findings_got_files,
		[STAGE_READ]    = findings_got_read,
		[STAGE_COMMIT]  = findings_committed,
		[STAGE_BLOBS]   = findings_got_blobs,
		[STAGE_ARCHIVE] = findings_got_archive,
		[STAGE_CALLS]   = findings_got_calls,
	};
	struct findings_call const *call = *mcp_slot_own(slot);
	if (!(STAGES_FAILED >> call->stage & 1) && !child_ok(call->child)) {
		findings_stop(slot, EINVAL, unread);
		return;
	}
	steps[call->stage](slot);
}

/* Checks the texts claim and reason of @p args (notice_text_check()), and
 * stores them in call->claim and in the reason of call->change, without
 * the notices at their ends and the spaces and line ends before them.
 * Returns 0, EINVAL with the refusal in the @p len bytes at @p why, or
 * ENOMEM.
 */
static int
findings_texts (struct findings_call *call,
                struct json_object   *args,
                char                 *why,
                size_t                len)
{
	static char const *const names[] = { "claim", "reason" };
	struct dstr *const texts[] = { &call->claim, &call->change.reason };
	for (size_t i = 0; i < array_size(names); ++i) {
		struct json_object *v = jb_get(args, names[i]);
		struct cstr_view const t = jb_view(v);
		size_t kept;
		if (!v)
			continue;
		int const e = notice_text_check(t, names[i], &kept, why, len);
		if (e)
			return e;
		if (cstr_view_store(cstr_view_trim_end((struct cstr_view){ t.data, kept }), texts[i]))
			return ENOMEM;
	}
	return 0;
}

/* Adds the reference @p ref to an item of a finding, X#n/m or X#n/m:A-B,
 * the item @p index of the evidence of the change of the call in @p slot,
 * to call->cites, with the record and the git IDs of the item if the ring
 * stores the item. With @p strict, an item that a result of this turn
 * showed whole is refused. Returns 0, EINVAL with the refusal in the
 * @p len bytes at @p why, or ENOMEM.
 */
static int
findings_cite_add (struct mcp_slot           *slot,
                   struct evidence_ref const *ref,
                   size_t                     index,
                   bool                       strict,
                   char                      *why,
                   size_t                     len)
{
	struct findings_call *call = *mcp_slot_own(slot);
	char id[SEEN_ID];
	evidence_ref_id(ref, id);
	struct seen_item const *s = seen_find_item(mcp_slot_seen(slot), cstr_view(id));
	if (strict && s && s->turn && s->turn >= mcp_slot_turn(slot))
		return util_refuse(why, len, "askai showed you %s in this turn. Cite %s for verified or "
		                   "disproved in a later turn.", id, id);
	if (util_grow(call->cites.items, call->cites.cap, call->cites.n + 1))
		return ENOMEM;
	struct findings_cite *cite = &call->cites.items[call->cites.n];
	*cite = (struct findings_cite){
		.record = DSTR_INIT,
		.ref    = *ref,
		.index  = (uint32_t)index,
		.turn   = s ? s->turn : 0,
	};
	if (s && cstr_view_store(cstr_view_dstr(&s->record), &cite->record)) {
		dstr_cleanup(&cite->record);
		return ENOMEM;
	}
	if (s)
		memcpy(cite->blobs, s->blobs, sizeof cite->blobs);
	++call->cites.n;
	return 0;
}

/* Makes the evidence items of the references in the argument evidence of
 * @p args for the change of the call in @p slot, and appends the file parts
 * of the archive of their material to call->files. Then checks the
 * external items. Returns 0, EINVAL with the refusal in the @p len bytes at
 * @p why, or ENOMEM.
 */
static int
findings_evidence (struct mcp_slot    *slot,
                   struct json_object *args,
                   char               *why,
                   size_t              len)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct finding_change *c = &call->change;
	struct json_object *refs = jb_get(args, "evidence");
	struct json_object *external = jb_get(args, "external");
	size_t const n = jb_len(refs);
	size_t const m = jb_len(external);
	if (refs && !json_object_is_type(refs, json_type_array))
		return util_refuse(why, len, "The evidence argument must be an array of references, "
		                   "such as [\"c7\", \"c9:120-140\"].");
	if (n > EVIDENCE_REFS)
		return util_refuse(why, len, "evidence takes at most 16 items.");
	if (external && !json_object_is_type(external, json_type_array))
		return util_refuse(why, len, "The external argument must be an array of strings.");
	if (m > EVIDENCE_EXTERNAL)
		return util_refuse(why, len, "external takes at most 8 items.");
	if (util_grow(c->evidence.items, c->evidence.cap, n) ||
	    util_grow(c->external.items, c->external.cap, m))
		return ENOMEM;
	struct member_call const *member = mcp_slot_member(slot);
	struct evidence_cite const cite = {
		.seen  = mcp_slot_seen(slot),
		.ask   = &member->ask,
		.name  = findings_tool_name,
		.owner = slot,
		.call  = mcp_slot_call(slot),
		.turn  = mcp_slot_turn(slot),
	};
	bool const strict = FINDING_SHOWN >> c->state & 1;
	dstr_auto lines = DSTR_INIT;
	int e = 0;
	for (size_t i = 0; !e && i < n; ++i) {
		struct cstr_view const text = jb_view(jb_at(refs, i));
		struct evidence_ref ref;
		if (evidence_ref_parse(&ref, text))
			return util_refuse(why, len, "evidence[%zu] is not a reference: %.*s. Give a call "
			                   "such as c7, lines such as c7:120-140, or an evidence item such "
			                   "as %s#5/2.", i,
			                   (int)util_clip(text.data, text.len, 80), text.data ? text.data : "",
			                   findings_session(call)->agent);
		struct evidence *item = &c->evidence.items[c->evidence.n++];
		*item = evidence();
		if (ref.form == EVIDENCE_REF_ITEM || ref.form == EVIDENCE_REF_ITEM_LINES) {
			e = findings_cite_add(slot, &ref, c->evidence.n - 1, strict, why, len);
			continue;
		}
		struct evidence_material material;
		e = evidence_cite(item, &material, &lines, &ref, &cite, strict, why, len);
		char path[EVIDENCE_PATH];
		struct store_part part;
		if (!e && item->flags & EVIDENCE_REQUEST) {
			evidence_path(item->request.sha256, path);
			e = store_part_file(&part, &call->files, cstr_view(path), material.request);
		}
		if (!e) {
			evidence_path(item->result.sha256, path);
			e = store_part_file(&part, &call->files, cstr_view(path), material.result);
		}
		loop_work(mcp_slot_loop(slot), LOOP_COPY, material.request.len + material.result.len);
	}
	for (size_t i = 0; !e && i < m; ++i) {
		struct cstr_view const text = jb_view(jb_at(external, i));
		e = evidence_external(text, i, why, len);
		struct dstr *x = &c->external.items[c->external.n++];
		*x = (struct dstr)DSTR_INIT;
		e = e ? e : cstr_view_store(text, x);
	}
	return e;
}

/* Checks the arguments of the call in @p slot for its action and copies
 * what the call needs from them. Returns 0, or EINVAL with the refusal in
 * the @p len bytes at @p why, or ENOMEM.
 */
static int
findings_args (struct mcp_slot    *slot,
               struct json_object *args,
               char               *why,
               size_t              len)
{
	struct findings_call *call = *mcp_slot_own(slot);
	char const *agent = findings_session(call)->agent;
	char const *action = findings_actions[call->action].data;
	struct json_object_iter it;
	json_object_object_foreachC(args, it) {
		size_t k = 0;
		while (k < array_size(findings_arg_rows) &&
		       !cstr_view_equal(findings_arg_rows[k].name, cstr_view(it.key)))
			++k;
		if (k == array_size(findings_arg_rows) ||
		    !(findings_arg_rows[k].actions >> call->action & 1))
			return util_refuse(why, len, "The action %s takes no argument %.64s.", action,
			                   it.key);
	}
	struct json_object *id = jb_get(args, "id");
	struct json_object *state = jb_get(args, "state");
	struct json_object *about = jb_get(args, "about");
	struct json_object *author = jb_get(args, "author");
	if ((ON_UPDATE | ON_SHOW) >> call->action & 1 &&
	    finding_id_parse(jb_view(id), call->author, &call->number))
		return util_refuse(why, len, "%s needs id: a finding, such as %s#5.", action, agent);
	/* expand of a turn without id prints all open findings of the agent. */
	if (call->action == FINDINGS_EXPAND && (id || mcp_slot_from_client(slot))) {
		struct evidence_ref ref;
		if (evidence_ref_parse(&ref, jb_view(id)) ||
		    (ref.form != EVIDENCE_REF_FINDING && ref.form != EVIDENCE_REF_ITEM))
			return util_refuse(why, len, "expand needs id: a finding, such as %s#5, or an "
			                   "evidence item, such as %s#5/2.", agent, agent);
		memcpy(call->author, ref.author, sizeof call->author);
		call->number = ref.number;
		call->item = ref.item;
	}
	if (call->action == FINDINGS_UPDATE && strcmp(call->author, agent))
		return util_refuse(why, len, "Only %s can change %s#%" PRIu32 ". Add your own finding "
		                   "with about %s#%" PRIu32 ".", call->author, call->author,
		                   call->number, call->author, call->number);
	if (author) {
		struct cstr_view const name = jb_view(author);
		if (!member_name_valid(name))
			return util_refuse(why, len, "The author argument must be the name of a member, "
			                   "such as %s.", agent);
		(void)util_format(call->author, sizeof call->author, "%.*s", (int)name.len, name.data);
	}
	if (about) {
		struct cstr_view const text = jb_view(about);
		char who[MEMBER_NAME];
		uint32_t n;
		if (finding_id_parse(text, who, &n))
			return util_refuse(why, len, "about must be the id of a finding, such as %s#2.",
			                   agent);
		(void)util_format(call->about, sizeof call->about, "%.*s", (int)text.len, text.data);
	}
	if (!((ON_ADD | ON_UPDATE) >> call->action & 1))
		return 0;
	size_t const s = cstr_view_index(finding_state_names, FINDING_STATES, jb_view(state));
	if (state ? s == FINDING_STATES : call->action == FINDINGS_UPDATE)
		return util_refuse(why, len, "%s needs state: candidate, verified, disproved or "
		                   "retracted.", action);
	int e = findings_texts(call, args, why, len);
	if (e)
		return e;
	/* A claim of only spaces and line ends is no claim. */
	if (call->action == FINDINGS_ADD && !dstr_len(&call->claim))
		return util_refuse(why, len, "add needs claim: what you found.");
	struct finding_change *c = &call->change;
	struct member_call const *member = mcp_slot_member(slot);
	c->state = state ? (enum finding_state)s : FINDING_CANDIDATE;
	c->turn = mcp_slot_turn(slot);
	c->call = mcp_slot_call(slot);
	c->ask = member->ask;
	(void)util_format(c->asked_by, sizeof c->asked_by, "%s", member->asker.name);
	if (cstr_view_store(CSTR_VIEW(ASKAI_VERSION), &c->askai))
		return ENOMEM;
	e = findings_evidence(slot, args, why, len);
	call->archive = buf_peek(&call->files).len;
	return e ? e : finding_check(c, why, len);
}

/* Checks the arguments @p args of the call in @p slot, makes the evidence
 * items of the calls of the ring, and runs git to read the findings that
 * the action needs.
 */
static void
findings_begin (struct mcp_slot    *slot,
                struct json_object *args)
{
	struct findings_call *call = *mcp_slot_own(slot);
	char why[1024] = "";
	int const e = findings_args(slot, args, why, sizeof why);
	if (e) {
		findings_stop(slot, e, cstr_view(why));
		return;
	}
	if (call->action == FINDINGS_EXPAND && *call->author)
		findings_read_one(slot);
	else
		findings_load(slot);
}

/* Handles the output of git log for an add or an update that askai runs
 * again after a restart: the newest commits of the branch of the agent,
 * each "ID ASK CALL SUBJECT". If a commit names the ask call and the call,
 * the call made that commit before the restart, and the result says so
 * without a second commit. Otherwise the call goes on. Exit status 128,
 * for a branch that does not exist, means no such commit.
 */
static void
findings_got_calls (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	if (!child_ok(c) && child_status(c) != 128) {
		mcp_slot_tell(slot, unread, TOOL_FAILED);
		return;
	}
	struct span const out = buf_peek(&c->out);
	struct cstr_view rest = child_ok(c) ? (struct cstr_view){ (char const *)out.ptr, out.len }
	                                    : (struct cstr_view){};
	struct cstr_view const ask = hexid_view(&mcp_slot_member(slot)->ask);
	char digits[16];
	struct cstr_view const number = util_format_view(digits, sizeof digits, "%" PRIu32,
	                                                 mcp_slot_call(slot));
	while (rest.len) {
		struct cstr_view line;
		if (!cstr_view_cut(&rest, '\0', &line)) {
			line = rest;
			rest = (struct cstr_view){};
		}
		struct oid commit;
		struct cstr_view a, n, verb, id;
		char author[MEMBER_NAME];
		uint32_t k;
		/* The subject is "Add bro#5 (verified)" or "Update bro#5 (...)". A
		 * commit without the trailer line Ask names no ask call. */
		if (oid_init_field(&commit, &line, ' ') || !cstr_view_cut(&line, ' ', &a) ||
		    !cstr_view_cut(&line, ' ', &n) || !a.len || !cstr_view_equal(a, ask) ||
		    !cstr_view_equal(n, number) || !cstr_view_cut(&line, ' ', &verb) ||
		    !cstr_view_cut(&line, ' ', &id) || finding_id_parse(id, author, &k))
			continue;
		(void)dstr_clear(&call->out);
		bool const ok = dstr_printf(&call->out, "%.*s was already %s in commit %.8s before "
		                            "askai restarted.", (int)id.len, id.data,
		                            call->action == FINDINGS_ADD ? "added" : "updated",
		                            commit.hex);
		(void)mcp_slot_noted(slot);
		findings_end(slot, ok ? 0 : ENOMEM, false);
		return;
	}
	findings_begin(slot, call->args);
}

/* Called on the loop once the table of the members ends the read of the
 * records for the call of @p wait. The step of the call then goes on
 * (mcp_slot_ready()).
 */
static void
findings_waited (struct member_wait *wait)
{
	(void)mcp_slot_ready(container_of(wait, struct findings_call, wait)->slot);
}

/* Allocates a call of @p action in @p slot with the part @p notes of the
 * notes tool. Returns the call, or nullptr with ENOMEM in @p *e.
 */
static struct findings_call *
findings_call_create (struct mcp_slot              *slot,
                      struct notes_tool_loop const *notes,
                      uint8_t                       action,
                      int                          *e)
{
	struct findings_call *call = malloc(sizeof *call);
	*e = call ? 0 : ENOMEM;
	if (call)
		*call = (struct findings_call){
			.out    = DSTR_INIT,
			.claim  = DSTR_INIT,
			.change = finding_change(),
			.wait   = { .hook = list(&call->wait.hook), .done = findings_waited },
			.notes  = notes,
			.slot   = slot,
			.action = action,
		};
	return call;
}

/* Starts a call of the findings tool in @p slot with the arguments
 * @p args. An add or an update of the turn that askai resumes after a
 * restart may have made its commit before the restart, so git first lists
 * the calls of the newest commits of the branch of the agent.
 */
static void
findings_start (struct mcp_slot    *slot,
                struct json_object *args)
{
	if (mcp_slot_offset(slot, args))
		return;
	uint8_t const action = (uint8_t)cstr_view_index(findings_actions, FINDINGS_ACTIONS,
	                                                jb_view(jb_get(args, "action")));
	int e;
	struct findings_call *call = findings_call_create(slot, mcp_slot_notes(slot), action, &e);
	if (!call) {
		mcp_slot_fail(slot, e);
		return;
	}
	*mcp_slot_own(slot) = call;
	/* Clients can only read the findings. */
	bool const client = mcp_slot_from_client(slot);
	if (call->action == FINDINGS_ACTIONS || (client && !(ON_CLIENTS >> call->action & 1))) {
		mcp_slot_tell(slot, client ? CSTR_VIEW("The action argument must be one of: list, "
		                                       "show, expand.")
		                           : CSTR_VIEW("The action argument must be one of: "
		                                       XL_JOIN(", ", ", ",
		                                               FINDINGS_ACTION_LIST(XL_TEXT)) "."),
		              TOOL_FAILED);
		return;
	}
	if ((ON_ADD | ON_UPDATE) >> call->action & 1 && mcp_slot_rerun(slot)) {
		char ref[MEMBER_REF];
		findings_ref(findings_session(call)->agent, ref);
		char const *const words[] = { ref };
		call->args = args;
		findings_run(slot, STAGE_CALLS, STORE_CALLS, words, nullptr, &(struct buf){});
		return;
	}
	findings_begin(slot, args);
}

static_assert(ON_CLIENTS == (1U << FINDINGS_ACTIONS) - (1U << FINDINGS_LIST),
              "the actions of clients are the last actions, from list on");

/* Returns the definition of the findings tool for clients, which can only
 * list, show and expand. @p service is the struct notes_tool, whose
 * registry names the agent.
 */
static struct json_object *
findings_definition (void const *service)
{
	struct notes_tool const *tool = service;
	struct member_session const *session = member_registry_session(tool->members);
	char const *agent = session ? session->agent : "";
	dstr_auto about = DSTR_INIT;
	dstr_auto id = DSTR_INIT;
	if (!dstr_printf(&about, "Findings are claims that the agents of this session made. A "
	                 "finding has an id such as %s#5. show prints a finding. expand prints the "
	                 "stored evidence of a finding.", agent) ||
	    !dstr_printf(&id, "A finding, such as %s#5, for show and expand. An evidence item, such "
	                 "as %s#5/2, for expand.", agent, agent))
		return nullptr;
	struct json_object *props = jb_object(
		"action", schema_enum(schema_names(findings_actions + FINDINGS_LIST,
		                                   FINDINGS_ACTIONS - FINDINGS_LIST),
			"list lists the findings of every member, open first. show prints a finding "
			"and every change. expand prints the stored evidence of a finding or of one "
			"evidence item."),
		"id", schema_property("string", dstr_get(&id)),
		"author", schema_property("string",
			"For list: list only the findings of this member."),
		"offset", schema_count(0, TOOL_OFFSET_ABOUT),
		nullptr);
	struct json_object *def = schema_tool("findings", dstr_get(&about), props,
	                                      CSTR_VIEW("action"));
	return jb_set_or_free(def, "annotations", jb_object(
		"readOnlyHint", json_object_new_boolean(1),
		"destructiveHint", json_object_new_boolean(0),
		"idempotentHint", json_object_new_boolean(1),
		"openWorldHint", json_object_new_boolean(0),
		nullptr));
}

struct json_object *
findings_upstream_definition (void const *service)
{
	struct notes_tool const *tool = service;
	struct member_session const *session = member_registry_session(tool->members);
	char const *agent = session ? session->agent : "";
	dstr_auto about = DSTR_INIT;
	dstr_auto id = DSTR_INIT;
	dstr_auto add = DSTR_INIT;
	dstr_auto evidence = DSTR_INIT;
	if (!dstr_printf(&about, "Findings are claims that you make about material that askai "
	                 "showed you. A finding has an id such as %s#5, a claim, a state and a "
	                 "history of changes. The id stays the same in every ask call. Every "
	                 "member can read every finding. Only the author of a finding can change "
	                 "the finding. The states are candidate, verified, disproved and "
	                 "retracted. verified and disproved need evidence that askai showed you "
	                 "before this turn. Every tool result ends with a call number, such as "
	                 "[askai: call c7]. Give that call as evidence. askai then stores the "
	                 "call and the result exactly as you got them. expand prints the stored "
	                 "evidence of a finding. The evidence that expand prints counts as seen "
	                 "from your next turn.", agent) ||
	    !dstr_printf(&id, "A finding, such as %s#5, for update, show and expand. An evidence "
	                 "item, such as %s#5/2, for expand. expand without id prints the evidence "
	                 "of all your open findings.", agent, agent) ||
	    !dstr_printf(&add, "For add: the finding of another member that your finding checks, "
	                 "such as %s#2.", agent) ||
	    !dstr_printf(&evidence, "The evidence for the state, at most 16 items. c7 is your call "
	                 "c7 of this ask call. c7:120-140,150 is lines of a git read or a code "
	                 "describe in call c7. %s#5/2 is an evidence item that expand showed you. "
	                 "%s#5/2:120-140 is lines 120 to 140 of the lines item %s#5/2.", agent,
	                 agent, agent))
		return nullptr;
	struct json_object *props = jb_object(
		"action", schema_enum(schema_names(findings_actions, FINDINGS_ACTIONS),
			"add records a finding. update changes your finding. list lists the "
			"findings of every member, open first. show prints a finding and every "
			"change. expand prints the stored evidence of a finding, of one evidence "
			"item, or of all your open findings."),
		"id", schema_property("string", dstr_get(&id)),
		"claim", schema_property("string",
			"What you found. Required for add. For update, give claim only to correct "
			"the claim."),
		"state", schema_enum(schema_names(finding_state_names, FINDING_STATES),
			"candidate, verified, disproved or retracted. The default for add is "
			"candidate. Required for update."),
		"evidence", schema_array(schema_property("string", nullptr), EVIDENCE_REFS,
			dstr_get(&evidence)),
		"external", schema_array(schema_property("string", nullptr), EVIDENCE_EXTERNAL,
			"Material outside askai, at most 8 items. Each item names the material with "
			"an exact hash, such as: libclang-19-dev 1:19.1.7-3 amd64 sha256:9b1c... "
			"askai does not store or show this material. External items alone never "
			"make a finding verified or disproved."),
		"reason", schema_property("string",
			"Why the evidence shows the state, or why you retract the finding. Required "
			"for verified, disproved and retracted."),
		"about", schema_property("string", dstr_get(&add)),
		"author", schema_property("string",
			"For list: list only the findings of this member."),
		"offset", schema_count(0, TOOL_OFFSET_ABOUT),
		nullptr);
	return schema_tool("findings", dstr_get(&about), props, CSTR_VIEW("action"));
}

struct plugin const findings_plugin = {
	.tools = {{
		.definition = findings_definition,
		.start = findings_start,
		.step = findings_step,
		.drop = findings_drop,
		.flags = KF_SERIAL | KF_RERUN | TOOL_PAGED
	}}
};

void
findings_prompt (struct notes_tool_loop const *notes,
                 struct mcp_slot              *slot,
                 struct dstr                  *prompt,
                 size_t                        most,
                 tool_step_fn                 *then)
{
	int e;
	struct findings_call *call = findings_call_create(slot, notes, FINDINGS_LIST, &e);
	if (!call) {
		mcp_slot_fail(slot, e);
		return;
	}
	*mcp_slot_own(slot) = call;
	/* The list of the agent, with the open findings as its result. */
	(void)util_format(call->author, sizeof call->author, "%s", findings_session(call)->agent);
	call->prompt = prompt;
	call->then = then;
	call->most = most;
	findings_load(slot);
}
