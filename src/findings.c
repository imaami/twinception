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
#include "compat.h"
#include "dstr.h"
#include "evidence.h"
#include "finding.h"
#include "finding_id.h"
#include "findings.h"
#include "jb.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "member.h"
#include "notes.h"
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
	STAGE_REFS,     /* for-each-ref lists the records of the members, with
	                   the branches of findings (member_list) */
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

/* The stages whose step handles a git that failed. The steps of the other
 * stages need the output of a git that succeeded.
 */
enum { STAGES_FAILED = 1U << STAGE_REFS | 1U << STAGE_COMMIT | 1U << STAGE_CALLS };

/* The stored material of the items that one call reads, at most. */
enum { FINDINGS_MATERIAL = TOOL_OUTPUT_MAX / 2 };

/* The flags of a call. */
enum findings_flag : uint8_t {
	CALL_ABOUT = 1U << 0,  /* the finding of the about argument exists */
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

/* Returns the branch of findings with the tip @p tip, whose author is the
 * agent named @p author. Every byte of the array author after the name is
 * 0.
 */
static force_inline struct findings_branch
findings_branch (struct oid       tip,
                 struct cstr_view author)
{
	struct findings_branch b = { .tip = tip };
	member_name_set(b.author, author);
	return b;
}

/* A file of a finding that cat-file reads: its size, the ID of the
 * finding and its blob ID. cat-file reads a file without a blob ID by its
 * path on the branch of the author of the finding.
 */
struct findings_file {
	uint64_t          bytes;  /* from ls-tree, or 0 if unknown */
	struct finding_id id;     /* the ID of the finding */
	struct oid        blob;   /* from ls-tree, or no name */
};

/* The flags of a reference to an item of a finding (struct findings_cite).
 * The flags are set only on the first reference to an item.
 */
enum findings_cite_flag : uint8_t {
	CITE_SHOW = 1U << 0,  /* the turns had not seen the item, which the
	                         refusal prints */
	CITE_READ = 1U << 1,  /* the call reads the archive files of the item */
};

/* A reference of the evidence argument to an item of a finding, X#n/m or
 * X#n/m:A-B. askai reads the record and the archive files of an item once,
 * for the first reference to the item. The later references use them.
 */
struct findings_cite {
	struct dstr         record;    /* the JSON object of the item, from the
	                                  ring or from the file of its finding */
	uint32_t            index;     /* the place of the item in the evidence
	                                  of the change */
	uint32_t            first;     /* the place in call->cites of the first
	                                  reference to the item */
	struct evidence_ref ref;       /* ref.id is the ID of the item, such as
	                                  cuh#2/1 */
	struct oid          blobs[2];  /* the git IDs of the archive files of
	                                  the item, or no name */
	uint8_t             flags;     /* enum findings_cite_flag */
	enum evidence_kind  kind;      /* the kind of the item.
	                                  findings_cite_blobs() sets the member
	                                  of the first reference to the item. */
};

/* Returns the reference @p ref to an item of a finding, before askai reads
 * the item. @p index is the place of the item in the evidence of the
 * change, and @p first is the place in call->cites of the first reference
 * to the item. The record is the empty string, and every other member is
 * zero.
 */
static force_inline struct findings_cite
findings_cite (uint32_t            index,
               uint32_t            first,
               struct evidence_ref ref)
{
	return (struct findings_cite){
		.record = DSTR_INIT,
		.index  = index,
		.first  = first,
		.ref    = ref,
	};
}

/* An entry of the output of expand. The entry is the item e of the finding
 * f. If e is nullptr, the entry marks the end of the items of f.
 */
struct findings_print {
	struct finding const  *f;
	struct evidence const *e;
};

/* Returns the entry of the item @p e of the finding @p f. If @p e is
 * nullptr, the entry marks the end of the items of @p f.
 */
static force_inline struct findings_print
findings_print (struct finding const  *f,
                struct evidence const *e)
{
	return (struct findings_print){ .f = f, .e = e };
}

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
	UTIL_ARRAY(struct findings_branch) branches;  /* from the records of
	                                                 the members */
	UTIL_ARRAY(struct findings_file)   wanted;    /* the files that cat-file
	                                                 reads, in order */
	UTIL_ARRAY(struct finding)         findings;  /* the findings read */
	UTIL_ARRAY(struct findings_cite)   cites;     /* the items that the
	                                                 evidence cites again */
	UTIL_ARRAY(struct findings_print)  prints;    /* the output of expand,
	                                                 in order */
	struct child          *child;     /* the call's git, until a step has
	                                     read its output */
	struct json_object    *args;      /* the arguments of the call, while
	                                     log lists the calls (STAGE_CALLS) */
	struct dstr           *prompt;    /* the text that askai adds to the
	                                     prompt, for the call of
	                                     findings_prompt(), else nullptr */
	tool_step_fn          *then;      /* the step that goes on with the ask
	                                     call after findings_prompt() */
	size_t                 most;      /* the most bytes of lines of findings
	                                     for findings_prompt() */
	size_t                 archive;   /* bytes of files that are the
	                                     archive */
	size_t                 fit;       /* the prints before which the items
	                                     of expand fit in FINDINGS_MATERIAL
	                                     bytes */
	uint32_t               last;      /* the highest number of a finding of
	                                     the agent */
	uint32_t               branch;    /* the branch that ls-tree lists */
	uint32_t               done;      /* the files of wanted that cat-file
	                                     has read */
	uint32_t               batch;     /* the files of wanted up to which
	                                     the current cat-file reads */
	struct finding_id      id;        /* the ID of the id argument: a
	                                     finding, or for expand a finding
	                                     or an item. A commit stores the ID
	                                     of its finding here. For list,
	                                     id.author is the author argument,
	                                     or "" */
	struct finding_id      about;     /* the about argument, or no ID */
	uint8_t                action;    /* enum findings_action */
	uint8_t                stage;     /* enum findings_stage */
	uint16_t               tries;     /* commits that raced, at most
	                                     STORE_TRIES, widened over the
	                                     padding */
	uint32_t               flags;     /* enum findings_flag, widened over
	                                     the padding */
};

static_assert(EVIDENCE_REFS == 16 && EVIDENCE_EXTERNAL == 8 && TOOL_TEXT_MAX == 4U << 20,
              "update the texts in this file that say 16 items, 8 items or 4 MiB");

/* Returns the registry of the members, from the table of the members on
 * the loop of the call in @p slot.
 */
static struct member_registry const *
findings_registry (struct mcp_slot const *slot)
{
	return member_table_registry(mcp_slot_members(slot));
}

/* Returns the session of the findings of the call in @p slot and the agent
 * of askai in it.
 */
static struct member_session const *
findings_session (struct mcp_slot const *slot)
{
	return member_registry_session(findings_registry(slot));
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

/* Empties the arrays of @p call that a load fills: the branches, the files
 * to read, the findings and the output of expand.
 */
static void
findings_call_reset (struct findings_call *call)
{
	for (size_t i = 0; i < call->findings.n; ++i)
		finding_fini(&call->findings.items[i]);
	call->findings.n = 0;
	call->branches.n = 0;
	call->wanted.n = 0;
	call->prints.n = 0;
}

/* Frees the call at @p *p_call and sets @p *p_call to nullptr. */
static void
findings_call_destroy (struct findings_call **p_call)
{
	struct findings_call *call = *p_call;
	store_drop(&call->child);
	buf_fini(&call->files);
	dstr_cleanup(&call->out);
	dstr_cleanup(&call->claim);
	finding_change_fini(&call->change);
	findings_call_reset(call);
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
 * fails the call with @p e. The function passes @p why straight to
 * mcp_slot_tell(), so @p why can be a text in the scratch of the loop
 * (mcp_slot_scratch()).
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

/* Returns the refusal that a writer wrote into @p scratch, the scratch of
 * the loop, before the writer returned the error @p e. findings_args() and
 * evidence_recite() write a refusal only for EINVAL, so the scratch is read
 * only then (mcp_slot_scratch()). For any other error, the function returns
 * an empty view.
 */
static struct cstr_view
findings_refusal (struct tool_scratch const *scratch,
                  int                        e)
{
	return e == EINVAL ? cstr_view(scratch->text) : cstr_view(nullptr);
}

/* Refuses the call in @p slot for the finding @p id, which does not exist. */
static void
findings_no_finding (struct mcp_slot         *slot,
                     struct finding_id const *id)
{
	char text[FINDING_ID_TEXT];
	mcp_slot_tellf(slot, TOOL_FAILED, "No finding has the id %s. list shows the findings.",
	               finding_id_put(id, text).data);
}

/* Refuses the call in @p slot for the finding @p id, whose file askai
 * could not read.
 */
static void
findings_unreadable (struct mcp_slot         *slot,
                     struct finding_id const *id)
{
	char text[FINDING_ID_TEXT];
	mcp_slot_tellf(slot, TOOL_FAILED, "Could not read finding %s: unknown format.",
	               finding_id_put(id, text).data);
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
	struct store const *store = member_registry_store(findings_registry(slot));
	call->child = store_spawn(store, slot, cmd, words, ref, buf_peek(in), &e);
	buf_fini(in);
	if (call->child)
		return;
	struct tool_scratch *scratch = mcp_slot_scratch(slot);
	findings_stop(slot, EINVAL, child_spawn_why(e, "git", scratch->text, sizeof scratch->text));
}

/* Writes to @p ref the branch of the findings of the agent @p author. */
static void
findings_ref (char const author[static MEMBER_NAME],
              char       ref[static MEMBER_REF])
{
	struct member agent = { .kind = MEMBER_AGENT };
	memcpy(agent.name, author, sizeof agent.name);
	member_ref(&agent, MEMBER_BRANCH_FINDINGS, ref);
}

/* Starts the action of the call in @p slot anew: git lists the records of
 * the members, which name the branches of findings (member_list).
 */
static void
findings_load (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	findings_call_reset(call);
	call->branch = 0;
	call->done = 0;
	call->batch = 0;
	call->last = 0;
	call->flags &= CALL_CITED;
	struct buf in = buf();
	findings_run(slot, STAGE_REFS, STORE_REFS, member_list, nullptr, &in);
}

/* Runs git to read the next files of call->wanted, from call->done on, as
 * many as one output of git holds (store_read_fits()), at least one. The
 * input of cat-file names each file by its blob ID, or else by its path on
 * the branch of its author. The ref of the branch is made once for the
 * files of one author in a row.
 */
static void
findings_read_batch (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct buf in = buf();
	uint64_t used = 0;
	uint32_t k = call->done;
	char const *author = nullptr;
	char ref[MEMBER_REF];
	int e = 0;
	while (!e && k < call->wanted.n) {
		struct findings_file const *w = &call->wanted.items[k];
		bool const fits = store_read_fits(&used, w->bytes);
		if (!fits && k > call->done)
			break;
		if (oid_len(&w->blob)) {
			e = buf_printf(&in, "%s\n", w->blob.hex);
		} else {
			if (!author || strcmp(author, w->id.author)) {
				author = w->id.author;
				findings_ref(author, ref);
			}
			char path[FINDING_FILE];
			e = notes_read_input(&in, ref, finding_path(w->id.number, path));
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

/* Runs git to read the finding of the id of expand by the path of its
 * file on the branch of its author.
 */
static void
findings_read_one (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	int const e = util_push(call->wanted, (struct findings_file){
		.id = finding_id_without_item(&call->id),
	});
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
		struct buf in = buf();
		findings_run(slot, STAGE_FILES, STORE_FILES, words, nullptr, &in);
		return;
	}
	if (call->wanted.n) {
		findings_read_batch(slot);
		return;
	}
	findings_act(slot);
}

/* Copies the branches of findings that the action of the call in @p slot
 * needs from the records of the members, once their list has replaced the
 * records of the table (member_table_parse()): every branch for show and
 * for list without author, the branch of the author for list with author,
 * and else the branch of the agent. For add with about, cat-file first
 * checks the finding that about names on the branch of its author.
 */
static void
findings_got_refs (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct member_table *table = mcp_slot_members(slot);
	/* The table logs the cause of a read that failed. */
	int e = member_table_parse(table, call->child);
	if (e) {
		findings_stop(slot, e == ENOMEM ? ENOMEM : EINVAL, unread);
		return;
	}
	bool const every = call->action == FINDINGS_SHOW ||
	                   (call->action == FINDINGS_LIST && !*call->id.author);
	struct cstr_view const only = cstr_view(call->action == FINDINGS_LIST
	                                        ? call->id.author
	                                        : findings_session(slot)->agent.name);
	struct cstr_view const about = cstr_view(call->about.author);
	struct oid tip = {};
	size_t n;
	struct member_info const *const members = member_table_members(table, &n, nullptr);
	for (size_t i = 0; i < n; ++i) {
		struct member_info const *info = &members[i];
		if (info->kind != MEMBER_AGENT || !oid_len(&info->findings))
			continue;
		if (about.len && cstr_view_equal(info->name, about))
			tip = info->findings;
		if (!every && !cstr_view_equal(info->name, only))
			continue;
		if (util_push(call->branches, findings_branch(info->findings, info->name))) {
			findings_end(slot, ENOMEM, true);
			return;
		}
	}
	if (!oid_len(&tip)) {
		findings_next(slot);
		return;
	}
	char path[FINDING_FILE];
	struct buf in = buf();
	e = notes_read_input(&in, tip.hex, finding_path(call->about.number, path));
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
	struct cstr_view rest = cstr_view_span(buf_peek(&call->child->out));
	struct cstr_view line = {}, size = {};
	struct oid id = {};
	if (cstr_view_cut(&rest, '\n', &line) && store_kind(line, &id, &size) == STORE_KIND_BLOB)
		call->flags |= CALL_ABOUT;
	findings_next(slot);
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
	/* The highest number counts on the branch of the agent. The finding of
	 * id is on the branch of its author, and update reads only its file.
	 * list, show and expand read every file. */
	bool const own = !strcmp(b->author, findings_session(slot)->agent.name);
	uint32_t const id = strcmp(b->author, call->id.author) ? 0 : call->id.number;
	uint32_t const only = call->action == FINDINGS_UPDATE ? id : 0;
	bool const every = (ON_LIST | ON_SHOW | ON_EXPAND) >> call->action & 1;
	struct span out = buf_peek(&call->child->out);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	struct notes_file file = {};
	/* The files of the branch are findings of its author. */
	struct findings_file want = {};
	memcpy(want.id.author, b->author, sizeof want.id.author);
	int e = 0;
	while (!e && out.len && !(e = notes_file_next(&out, &file))) {
		uint32_t const n = finding_path_number(cstr_view_span(file.name));
		if (!n)
			continue;
		if (own && n > call->last)
			call->last = n;
		if (n == id)
			call->flags |= CALL_FOUND;
		if (!every && n != only)
			continue;
		want.bytes = file.bytes;
		want.id.number = n;
		want.blob = file.id;
		e = util_push(call->wanted, want);
	}
	if (e) {
		findings_stop(slot, e == EIO ? EINVAL : e, unread);
		return;
	}
	++call->branch;
	findings_next(slot);
}

/* Handles the output of cat-file, which is the files of call->wanted from
 * call->done to call->batch, in order. If askai cannot read a file, or the
 * ID of the finding in a file is not the ID that the file was read for,
 * the function refuses the call. For the call of findings_prompt(), the
 * function instead appends the notice that the findings could not be read
 * to the prompt (findings_prompted()). If every file of the batch was
 * read, cat-file reads the next files. After the last file, the findings
 * are sorted (finding_sort()), and the action goes on.
 */
static void
findings_got_read (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct span out = buf_peek(&call->child->out);
	loop_work(mcp_slot_loop(slot), LOOP_PARSE, out.len);
	int e = util_grow(call->findings.items, call->findings.cap, call->wanted.n);
	for (size_t i = call->done; !e && i < call->batch; ++i) {
		struct findings_file const *w = &call->wanted.items[i];
		struct finding *f = &call->findings.items[call->findings.n++];
		*f = finding();
		struct oid blob = {};
		struct span data = {};
		e = store_object_next(&out, &blob, &data);
		if (!e)
			e = finding_parse(f, cstr_view_span(data));
		if (e == ENOMEM || (!e && finding_id_equal(&f->id, &w->id)))
			continue;
		if (call->prompt) {
			findings_prompted(slot, EINVAL);
			return;
		}
		/* expand with id and the findings of the items that the evidence
		 * cites again read the file of a finding by its path. */
		if (e == ENOENT)
			findings_no_finding(slot, &w->id);
		else
			findings_unreadable(slot, &w->id);
		return;
	}
	if (e) {
		findings_end(slot, e, true);
		return;
	}
	call->done = call->batch;
	if (call->done < call->wanted.n) {
		findings_read_batch(slot);
		return;
	}
	finding_sort(call->findings.items, call->findings.n);
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
		struct evidence_ref ref = evidence_ref(e->call, EVIDENCE_REF_CALL);
		if (*e->from.author && k < call->cites.n) {
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
	struct member_session const *session = findings_session(slot);
	char const *agent = session->agent.name;
	char id[FINDING_ID_TEXT];
	finding_id_put(&call->id, id);
	if (child_ok(c)) {
		struct oid commit = {};
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
		int64_t const age = store_stale(member_registry_store(findings_registry(slot)),
		                                session->refs[MEMBER_BRANCH_FINDINGS], "findings");
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
	struct cstr_view const cause = store_cause(out);
	LOG(LOG_WARN, "findings: git could not record %s: %.*s", id, (int)cause.len, cause.data);
	if (c->end.flags & CHILD_LATE)
		mcp_slot_tell(slot, CSTR_VIEW("Could not record the finding. The write timed out."),
		              TOOL_FAILED);
	else
		mcp_slot_tell(slot, CSTR_VIEW("Could not record the finding."), TOOL_FAILED);
}

/* Writes to @p message the subject of the commit of the change of the
 * call to the finding @p id, whose state was @p before: "Add bro#5
 * (verified)", and for an update by CALL_SAME and CALL_CLAIM "Update bro#5
 * (candidate to verified)", "Update bro#5 (claim)" or both. Returns the
 * view of the subject in @p message.
 */
static struct cstr_view
findings_subject (struct findings_call const *call,
                  char const                 *id,
                  enum finding_state          before,
                  char                        message[static STORE_MESSAGE])
{
	char const *after = finding_state_names[call->change.state].data;
	char const *was = finding_state_names[before].data;
	if (call->action == FINDINGS_ADD)
		return util_format_view(message, STORE_MESSAGE, "Add %s (%s)", id, after);
	switch (call->flags & (CALL_SAME | CALL_CLAIM)) {
	case CALL_SAME:
		return util_format_view(message, STORE_MESSAGE, "Update %s (%s)", id, after);
	case CALL_SAME | CALL_CLAIM:
		return util_format_view(message, STORE_MESSAGE, "Update %s (claim)", id);
	case CALL_CLAIM:
		return util_format_view(message, STORE_MESSAGE, "Update %s (%s to %s, claim)", id, was,
		                        after);
	default:
		return util_format_view(message, STORE_MESSAGE, "Update %s (%s to %s)", id, was, after);
	}
}

/* Runs git to commit the change of the call in @p slot to @p f, the finding
 * of an update or a new finding without a claim: the file of the finding
 * with the change, and the archive of its evidence, on the branch of the
 * agent. The claim of the call is the claim of a new finding, and the
 * claim of an update if it is another claim.
 */
static void
findings_commit (struct mcp_slot      *slot,
                 struct finding const *f)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct member_session const *session = findings_session(slot);
	char const *ref = session->refs[MEMBER_BRANCH_FINDINGS];
	/* add and update read only the branch of the agent. */
	struct oid const tip = call->branches.n ? call->branches.items[0].tip : (struct oid){};
	enum finding_state const before = finding_state(f);
	struct cstr_view claim = cstr_view_dstr(&call->claim);
	if (call->action == FINDINGS_UPDATE) {
		if (cstr_view_equal(claim, cstr_view_dstr(&f->claim)))
			claim = cstr_view(nullptr);
		call->flags |= (before == call->change.state ? CALL_SAME : 0) |
		               (claim.len ? CALL_CLAIM : 0);
	}
	/* The items of the change are numbered after the items of f. */
	struct finding_change *c = &call->change;
	uint32_t items = f->changes.n ? f->changes.items[f->changes.n - 1].items : 0;
	for (size_t i = 0; i < c->evidence.n; ++i)
		c->evidence.items[i].item = ++items;
	c->items = items;
	dstr_auto json = DSTR_INIT;
	int e = finding_render(f, c, claim, &json);
	struct cstr_view const text = cstr_view_dstr(&json);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, text.len);
	if (e == EFBIG) {
		mcp_slot_tell(slot, CSTR_VIEW("The finding would be larger than 4 MiB. Nothing was "
		                              "recorded."), TOOL_FAILED);
		return;
	}
	char id[FINDING_ID_TEXT];
	char path[FINDING_FILE];
	char message[STORE_MESSAGE];
	finding_id_put(&f->id, id);
	call->id = f->id;
	struct extent part = {};
	e = e ? e : store_part_file(&part, &call->files, finding_path(f->id.number, path), text);
	struct store_commit commit = store_commit(
		.message = findings_subject(call, id, before, message),
		.files = buf_peek(&call->files),
		.ref = ref,
		.tip = &tip
	);
	struct member_byline byline = {};
	member_byline_put(&byline, &commit, &session->agent, mcp_slot_member(slot),
	                  mcp_slot_call(slot), session);
	struct buf in = buf();
	e = e ? e : store_commit_input(&commit, &in);
	/* The archive stays for a commit after a race. */
	buf_cut(&call->files, call->archive, commit.files.len - call->archive);
	if (e) {
		buf_fini(&in);
		findings_end(slot, e, true);
		return;
	}
	findings_run(slot, STAGE_COMMIT, STORE_COMMIT, nullptr, ref, &in);
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
 * material @p b on the branch of findings @p ref (findings_ref()). Returns
 * 0 or the error of buf_printf().
 */
static int
findings_archive_line (struct buf                 *in,
                       char const                 *ref,
                       struct evidence_blob const *b)
{
	char path[EVIDENCE_PATH];
	evidence_path(b->sha256, path);
	return notes_read_input(in, ref, cstr_view(path));
}

/* Appends to call->files the archive files of the item @p e: the
 * arguments of the call of a call item, and the material. A file
 * whose git ID is in @p blobs is that blob of the repository, and any
 * other file contains the material in @p m. Returns 0 or the error of
 * store_part_file() or store_part_blob().
 */
static int
findings_archive (struct findings_call           *call,
                  struct evidence const          *e,
                  struct evidence_material const *m,
                  struct oid const                blobs[static 2])
{
	struct evidence_blob const *const b[] = { &e->request, &e->result };
	struct cstr_view const data[] = { m->request, m->result };
	int r = 0;
	for (size_t k = e->kind == EVIDENCE_CALL ? 0 : 1; !r && k < array_size(b); ++k) {
		char path[EVIDENCE_PATH];
		struct extent part = {};
		evidence_path(b[k]->sha256, path);
		r = oid_len(&blobs[k]) ? store_part_blob(&call->files, cstr_view(path), &blobs[k])
		                       : store_part_file(&part, &call->files, cstr_view(path), data[k]);
	}
	return r;
}

/* Returns true if the archive files of the item @p e have the git IDs
 * @p blobs, so that a finding can cite the item again.
 */
static bool
findings_stored (struct evidence const *e,
                 struct oid const       blobs[static 2])
{
	return oid_len(&blobs[1]) && (e->kind != EVIDENCE_CALL || oid_len(&blobs[0]));
}

/* Records that the item @p key, such as cuh#2/1, whose record is in
 * shown->record and whose archive files have the git IDs @p blobs, is the
 * bytes @p output of the output of the call in @p slot, so that a finding
 * can cite the item once a result has shown it (mcp_slot_evidence()).
 * Returns 0 or the error of mcp_slot_evidence().
 */
static int
findings_shown (struct mcp_slot         *slot,
                struct seen_item        *shown,
                struct finding_id const *key,
                struct oid const         blobs[static 2],
                struct extent            output)
{
	memcpy(shown->blobs, blobs, sizeof shown->blobs);
	shown->key = *key;
	return mcp_slot_evidence(slot, shown, output);
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
 * the archive files of the items of call->prints that fit: each finding
 * with its items and its external items. Each item that a result can show
 * whole is recorded for the ring (findings_shown()). The output ends with
 * the items that did not fit, and for a turn, with what the turn saw.
 */
static void
findings_expanded (struct mcp_slot *slot,
                   struct span      out)
{
	struct findings_call *call = *mcp_slot_own(slot);
	bool const turn = !mcp_slot_from_client(slot);
	struct seen_item shown = { .record = DSTR_INIT };
	struct finding const *f = nullptr;
	/* key is the ID of the item of a print, and more_id is the ID of the
	 * first item that did not fit. In the loop, text is the text of the ID
	 * of f. After the loop, text is the text of the ID that a notice
	 * prints. */
	struct finding_id key = {};
	struct finding_id more_id = {};
	char text[FINDING_ID_TEXT] = "";
	size_t items = 0;
	size_t more = 0;
	size_t whole = 0;
	int e = 0;
	(void)dstr_clear(&call->out);
	for (size_t i = 0; !e && i < call->prints.n; ++i) {
		struct findings_print const *p = &call->prints.items[i];
		if (p->f != f) {
			f = p->f;
			key = f->id;
			finding_id_put(&f->id, text);
			items = 0;
			struct cstr_view const claim = finding_claim_line(f);
			if (!dstr_printf(&call->out, "Evidence of finding %s by %s: %.*s\n", text,
			                 f->id.author, (int)claim.len, claim.len ? claim.data : ""))
				e = ENOMEM;
		}
		struct evidence const *ev = p->e;
		if (!ev) {
			/* The end of the items of f. */
			size_t const externals = e || call->id.item ? 0 : findings_externals(call, f, text);
			if (externals == SIZE_MAX)
				e = ENOMEM;
			else if (!e && !items && !externals &&
			         !dstr_printf(&call->out, "[askai: %s has no evidence items.]\n", text))
				e = ENOMEM;
			continue;
		}
		++items;
		key.item = ev->item;
		if (i >= call->fit) {
			if (!more++)
				more_id = key;
			continue;
		}
		/* Missing material prints as missing. */
		struct oid blobs[2] = {};
		struct span request = {};
		struct span result = {};
		int r = 0;
		if (ev->kind == EVIDENCE_CALL)
			r = store_object_next(&out, &blobs[0], &request);
		if (r != EIO)
			r = store_object_next(&out, &blobs[1], &result);
		if (r == EIO) {
			e = EIO;
			continue;
		}
		size_t const at = dstr_len(&call->out);
		struct evidence_material const material = evidence_material(cstr_view_span(request),
		                                                            cstr_view_span(result));
		e = evidence_expand(ev, &key, &material, &call->out);
		if (e || !turn || !findings_stored(ev, blobs))
			continue;
		e = findings_record(ev, &shown.record);
		e = e ? e : findings_shown(slot, &shown, &key, blobs,
		                           extent(at, dstr_len(&call->out) - at));
		++whole;
	}
	dstr_cleanup(&shown.record);
	if (e == EIO) {
		mcp_slot_tell(slot, unread, TOOL_FAILED);
		return;
	}
	bool ok = !e;
	if (ok && !call->prints.n)
		ok = dstr_cat(&call->out, "You have no open findings.\n");
	if (ok && more)
		ok = dstr_printf(&call->out, "[askai: %zu more evidence item%s did not fit. Expand each "
		                 "item by id, such as %s.]\n", more, util_s(more),
		                 finding_id_put(&more_id, text).data);
	/* shown.key is the ID of the last item that a result can show whole. */
	if (ok && whole == 1) {
		finding_id_put(&shown.key, text);
		ok = dstr_printf(&call->out, "[askai: You saw %s whole. Cite %s for verified or "
		                 "disproved from your next turn on.]\n", text, text);
	} else if (ok && whole > 1) {
		ok = dstr_printf(&call->out, "[askai: You saw the %zu evidence items above whole. Cite "
		                 "the items for verified or disproved from your next turn on.]\n",
		                 whole);
	}
	if (!ok && !e)
		e = ENOMEM;
	findings_end(slot, e, false);
}

/* Adds the sizes of the stored material of the item @p e to @p *bytes, the
 * material of a call so far, and returns true, or returns false with
 * @p *bytes unchanged if the material would then be more than
 * FINDINGS_MATERIAL bytes.
 */
static bool
findings_fits (uint64_t              *bytes,
               struct evidence const *e)
{
	uint64_t const request = e->kind == EVIDENCE_CALL ? e->request.bytes : 0;
	if (request > FINDINGS_MATERIAL - *bytes ||
	    e->result.bytes > FINDINGS_MATERIAL - *bytes - request)
		return false;
	*bytes += request + e->result.bytes;
	return true;
}

/* Refuses the call in @p slot for the item @p id, which its finding does
 * not contain.
 */
static void
findings_no_item (struct mcp_slot         *slot,
                  struct finding_id const *id)
{
	struct finding_id const finding = finding_id_without_item(id);
	char item_text[FINDING_ID_TEXT];
	char finding_text[FINDING_ID_TEXT];
	mcp_slot_tellf(slot, TOOL_FAILED, "No evidence item has the id %s. show with id %s lists "
	               "the items.", finding_id_put(id, item_text).data,
	               finding_id_put(&finding, finding_text).data);
}

/* Records in call->prints the item @p ev of the finding @p f, and counts
 * the item in @p *items. An expand of one item skips every other item.
 * For an item before call->fit, adds the size of its material to
 * @p *bytes (findings_fits()) and the lines that read its archive files
 * on the branch @p ref to @p in. The first item that does not fit sets
 * call->fit. Returns 0, ENOMEM, or the error of findings_archive_line().
 */
static int
findings_expand_item (struct findings_call  *call,
                      struct finding const  *f,
                      struct evidence const *ev,
                      char const            *ref,
                      uint64_t              *bytes,
                      size_t                *items,
                      struct buf            *in)
{
	if (call->id.item && ev->item != call->id.item)
		return 0;
	++*items;
	int e = util_push(call->prints, findings_print(f, ev));
	if (e || call->fit < call->prints.n)
		return e;
	if (!findings_fits(bytes, ev)) {
		call->fit = call->prints.n - 1;
		return 0;
	}
	if (ev->kind == EVIDENCE_CALL)
		e = findings_archive_line(in, ref, &ev->request);
	return e ? e : findings_archive_line(in, ref, &ev->result);
}

/* Records in call->prints the items of the finding @p f that expand
 * prints (findings_expand_item()), and then the end of the items of
 * @p f. Returns 0, ENOMEM, or the error of findings_expand_item().
 */
static int
findings_expand_finding (struct findings_call *call,
                         struct finding const *f,
                         char const           *ref,
                         uint64_t             *bytes,
                         size_t               *items,
                         struct buf           *in)
{
	int e = 0;
	for (size_t k = 0; !e && k < f->changes.n; ++k) {
		struct finding_change const *c = &f->changes.items[k];
		for (size_t j = 0; !e && j < c->evidence.n; ++j)
			e = findings_expand_item(call, f, &c->evidence.items[j], ref, bytes, items, in);
	}
	return e ? e : util_push(call->prints, findings_print(f, nullptr));
}

/* Records in call->prints the output of expand: the items of the finding
 * of the id, or of the item of the id, or without id of the open findings
 * of the agent, each finding with the end of its items. call->fit is the
 * first item whose material would make the material of the call more than
 * FINDINGS_MATERIAL bytes. Then runs git to read the archive files of the
 * items before it, or without such items, writes the output at once.
 */
static void
findings_expand (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct buf in = buf();
	uint64_t bytes = 0;
	size_t items = 0;
	int e = 0;
	call->prints.n = 0;
	call->fit = SIZE_MAX;
	/* The call read only the finding of the id, or the findings of the
	 * agent. Either way the findings are of one author, so the ref of the
	 * branch is made once. */
	char own[MEMBER_REF];
	char const *ref = findings_session(slot)->refs[MEMBER_BRANCH_FINDINGS];
	if (*call->id.author) {
		findings_ref(call->id.author, own);
		ref = own;
	}
	for (size_t i = 0; !e && i < call->findings.n; ++i) {
		struct finding const *f = &call->findings.items[i];
		if (!*call->id.author && !(FINDING_OPEN >> finding_state(f) & 1))
			continue;
		e = findings_expand_finding(call, f, ref, &bytes, &items, &in);
	}
	if (!e && call->id.item && !items) {
		buf_fini(&in);
		findings_no_item(slot, &call->id);
		return;
	}
	if (e) {
		buf_fini(&in);
		findings_end(slot, e, true);
		return;
	}
	if (!buf_peek(&in).len) {
		findings_expanded(slot, span(nullptr, 0));
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

/* Does the action of the call in @p slot once git has read what it needs,
 * with the findings that finding_sort() sorted: writes the list or the
 * finding, or commits an add or an update. For the call of
 * findings_prompt(), it appends the open findings to the prompt
 * (findings_prompted()). An add or an update whose evidence cites items of
 * findings again first reads those items (findings_cite_records()), and
 * then comes back here.
 */
static void
findings_act (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct member_session const *session = findings_session(slot);
	child_destroy(&call->child);
	if (call->prompt) {
		findings_prompted(slot, 0);
		return;
	}
	struct finding_id const *missing = nullptr;
	switch (call->action) {
	case FINDINGS_LIST:
		findings_end(slot, finding_list(call->findings.items, call->findings.n, session->name,
		                                call->id.author, &call->out), false);
		return;
	case FINDINGS_EXPAND:
		findings_expand(slot);
		return;
	case FINDINGS_SHOW:
	case FINDINGS_UPDATE:
		if (!(call->flags & CALL_FOUND))
			missing = &call->id;
		break;
	case FINDINGS_ADD:
		if (*call->about.author && !(call->flags & CALL_ABOUT))
			missing = &call->about;
		break;
	}
	if (missing) {
		findings_no_finding(slot, missing);
		return;
	}
	/* The items that the evidence cites again are read after the checks
	 * above, so that the refusal of an item that the turns had not seen
	 * comes after the refusal of a finding that does not exist. */
	if (call->cites.n && !(call->flags & CALL_CITED)) {
		findings_cite_records(slot);
		return;
	}
	struct finding const *f = finding_find(call->findings.items, call->findings.n, &call->id);
	if (call->action == FINDINGS_SHOW) {
		if (!f) {
			findings_end(slot, EIO, true);
			return;
		}
		char id[FINDING_ID_TEXT];
		finding_id_put(&f->id, id);
		int e = finding_show(f, id, call->findings.items, call->findings.n, &call->out);
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
		               "of %s a higher number.", session->agent.name, session->agent.name);
		return;
	}
	struct finding added = finding();
	memcpy(added.id.author, session->agent.name, sizeof added.id.author);
	added.id.number = call->last + 1;
	added.about = call->about;
	added.conversation = mcp_slot_member(slot)->conversation;
	findings_commit(slot, &added);
	finding_fini(&added);
}

/* Writes to call->out the start of the refusal of the add or the update
 * of @p call, whose evidence cites items that the turns had not seen
 * (CITE_SHOW), and the IDs of those items to @p names, each once. Returns 0
 * or ENOMEM.
 */
static int
findings_unseen_head (struct findings_call *call,
                      struct dstr          *names)
{
	struct cstr_view views[EVIDENCE_REFS];
	char texts[EVIDENCE_REFS][FINDING_ID_TEXT];
	char id[FINDING_ID_TEXT];
	uint64_t mask = 0;
	for (size_t i = 0; i < call->cites.n; ++i) {
		struct findings_cite const *cite = &call->cites.items[i];
		if (!(cite->flags & CITE_SHOW))
			continue;
		views[i] = finding_id_put(&cite->ref.id, texts[i]);
		mask |= UINT64_C(1) << i;
	}
	(void)dstr_clear(&call->out);
	bool ok = !cstr_view_join(views, call->cites.n, mask, CSTR_VIEW(", "), CSTR_VIEW(" and "),
	                          names);
	if (ok && call->action == FINDINGS_ADD)
		ok = dstr_cat(&call->out, "The finding was not added. ");
	else if (ok)
		ok = dstr_printf(&call->out, "%s did not change. ", finding_id_put(&call->id, id).data);
	ok = ok && dstr_printf(&call->out, "%s needs evidence that askai showed you before this "
	                       "turn. askai had not shown you %s. ",
	                       finding_state_names[call->change.state].data, dstr_get(names));
	return ok ? 0 : ENOMEM;
}

/* Refuses the add or the update of the call in @p slot, whose evidence
 * cites the items @p names that the turns had not seen, because those
 * items are longer than one result. findings_unseen_head() wrote the start
 * of the refusal to call->out, and the function replaces the text after
 * its byte @p head.
 */
static void
findings_too_long (struct mcp_slot   *slot,
                   size_t             head,
                   struct dstr const *names)
{
	struct findings_call *call = *mcp_slot_own(slot);
	(void)dstr_erase(&call->out, head, SIZE_MAX);
	bool const ok = dstr_printf(&call->out, "The evidence is longer than one result. Read %s "
	                            "with expand, a page at a time. Repeat the %s in a later turn.",
	                            dstr_get(names), findings_actions[call->action].data);
	findings_end(slot, ok ? 0 : ENOMEM, true);
}

/* Refuses the add or the update of the call in @p slot, whose evidence
 * cites items that the turns had not seen (CITE_SHOW), and prints those
 * items, each once, with the material @p material, so that they count as
 * shown in this turn. Items that do not fit in one result together are
 * named instead, for expand to print a page at a time
 * (findings_too_long()).
 */
static void
findings_unseen (struct mcp_slot                *slot,
                 struct evidence_material const  material[static EVIDENCE_REFS])
{
	struct findings_call *call = *mcp_slot_own(slot);
	dstr_auto names = DSTR_INIT;
	int e = findings_unseen_head(call, &names);
	size_t const head = dstr_len(&call->out);
	if (!e && !dstr_printf(&call->out, "askai prints %s below. Repeat the %s in your next "
	                       "turn.\n", dstr_get(&names), findings_actions[call->action].data))
		e = ENOMEM;
	/* The place of each item in the output. end is the end of the
	 * output. */
	size_t at[EVIDENCE_REFS + 1] = {};
	size_t end = dstr_len(&call->out);
	struct evidence item = evidence();
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite const *cite = &call->cites.items[i];
		at[i] = end;
		if (!(cite->flags & CITE_SHOW))
			continue;
		e = evidence_parse_text(&item, cstr_view_dstr(&cite->record));
		if (!e)
			e = evidence_expand(&item, &cite->ref.id, &material[i], &call->out);
		end = dstr_len(&call->out);
	}
	evidence_fini(&item);
	at[call->cites.n] = end;
	if (!e && end > mcp_slot_window_max(slot)) {
		findings_too_long(slot, head, &names);
		return;
	}
	struct seen_item shown = { .record = DSTR_INIT };
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite const *cite = &call->cites.items[i];
		if (at[i] == at[i + 1])
			continue;
		e = cstr_view_store(cstr_view_dstr(&cite->record), &shown.record);
		e = e ? e : findings_shown(slot, &shown, &cite->ref.id, cite->blobs,
		                           extent(at[i], at[i + 1] - at[i]));
	}
	dstr_cleanup(&shown.record);
	findings_end(slot, e, true);
}

/* Makes the items that the evidence of the call in @p slot cites again,
 * from their records and, for lines, from the stored lines @p material, or
 * refuses the call for items that the turns had not seen (CITE_SHOW). A
 * reference uses the record, the git IDs and the material of the first
 * reference to its item. Lines of an item are stored anew, and a whole
 * item is the archive files of the item, by their git IDs. Then the action
 * goes on (findings_act()).
 */
static void
findings_cite_end (struct mcp_slot                *slot,
                   struct evidence_material const  material[static EVIDENCE_REFS])
{
	struct findings_call *call = *mcp_slot_own(slot);
	for (size_t i = 0; i < call->cites.n; ++i)
		if (call->cites.items[i].flags & CITE_SHOW) {
			findings_unseen(slot, material);
			return;
		}
	struct tool_scratch *scratch = mcp_slot_scratch(slot);
	dstr_auto lines = DSTR_INIT;
	int e = 0;
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite const *cite = &call->cites.items[i];
		struct findings_cite const *of = &call->cites.items[cite->first];
		struct evidence *item = &call->change.evidence.items[cite->index];
		e = evidence_recite(item, &lines, &cite->ref, cstr_view_dstr(&of->record),
		                    material[cite->first].result, scratch->text,
		                    sizeof scratch->text);
		if (e) {
			findings_stop(slot, e, findings_refusal(scratch, e));
			return;
		}
		if (cite->ref.form == EVIDENCE_REF_ITEM_LINES) {
			struct evidence_material const m = evidence_material(cstr_view(nullptr),
			                                                     cstr_view_dstr(&lines));
			e = findings_archive(call, item, &m, (struct oid const [2]){});
		} else {
			e = findings_archive(call, item, &(struct evidence_material){}, of->blobs);
		}
	}
	if (e) {
		findings_end(slot, e, true);
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
	struct evidence_material material[EVIDENCE_REFS] = {};
	for (size_t i = 0; i < call->cites.n; ++i) {
		struct findings_cite *cite = &call->cites.items[i];
		if (!(cite->flags & CITE_READ))
			continue;
		/* The archive files of an item are the arguments, then the
		 * material. */
		struct cstr_view *const m[] = { &material[i].request, &material[i].result };
		for (size_t k = cite->kind == EVIDENCE_CALL ? 0 : 1; k < array_size(m); ++k) {
			struct span data = {};
			int const e = store_object_next(&out, &cite->blobs[k], &data);
			if (e == ENOENT) {
				char id[FINDING_ID_TEXT];
				mcp_slot_tellf(slot, TOOL_FAILED, "The stored material of %s is missing.",
				               finding_id_put(&cite->ref.id, id).data);
				return;
			}
			if (e) {
				mcp_slot_tell(slot, unread, TOOL_FAILED);
				return;
			}
			*m[k] = cstr_view_span(data);
		}
	}
	findings_cite_end(slot, material);
}

/* Runs git to read the archive files of the items of call->cites that the
 * refusal of the call in @p slot prints or that are cited in part, which
 * findings_cite_add() marked CITE_READ, and of the items whose git IDs the
 * ring does not store. Each item is read once, by its first reference. The
 * call is refused before git runs if the material of the items that the
 * refusal prints is longer than one result. Without items to read, the
 * call goes on at once.
 */
static void
findings_cite_blobs (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	struct evidence item = evidence();
	struct buf in = buf();
	uint64_t bytes = 0;
	uint64_t shown = 0;
	char const *author = nullptr;
	char ref[MEMBER_REF];
	int e = 0;
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite *cite = &call->cites.items[i];
		if (cite->first != i)
			continue;
		e = evidence_parse_text(&item, cstr_view_dstr(&cite->record));
		if (e == EINVAL) {
			struct finding_id const finding = finding_id_without_item(&cite->ref.id);
			evidence_fini(&item);
			buf_fini(&in);
			findings_unreadable(slot, &finding);
			return;
		}
		if (e)
			break;
		cite->kind = item.kind;
		bool const asked = item.kind == EVIDENCE_CALL;
		bool const stored = findings_stored(&item, cite->blobs);
		if (!(cite->flags & CITE_READ) && stored)
			continue;
		cite->flags |= CITE_READ;
		if (!findings_fits(&bytes, &item)) {
			evidence_fini(&item);
			buf_fini(&in);
			mcp_slot_tell(slot, CSTR_VIEW("The cited evidence items have more than 8 MiB of "
			                              "stored material. Cite fewer items in one call."),
			              TOOL_FAILED);
			return;
		}
		if (cite->flags & CITE_SHOW)
			shown += (asked ? item.request.bytes : 0) + item.result.bytes;
		/* The ring stores the git IDs of an item that a result showed.
		 * For the other items, the ref of the branch of the author is made
		 * once for the items of one author in a row. */
		if (!stored && (!author || strcmp(author, cite->ref.id.author))) {
			author = cite->ref.id.author;
			findings_ref(author, ref);
		}
		for (size_t k = asked ? 0 : 1; !e && k < array_size(cite->blobs); ++k)
			e = oid_len(&cite->blobs[k])
			  ? buf_printf(&in, "%s\n", cite->blobs[k].hex)
			  : findings_archive_line(&in, ref, k ? &item.result : &item.request);
	}
	evidence_fini(&item);
	if (e) {
		buf_fini(&in);
		findings_end(slot, e, true);
		return;
	}
	if (shown > mcp_slot_window_max(slot)) {
		buf_fini(&in);
		dstr_auto names = DSTR_INIT;
		e = findings_unseen_head(call, &names);
		if (e)
			findings_end(slot, e, true);
		else
			findings_too_long(slot, dstr_len(&call->out), &names);
		return;
	}
	if (!buf_peek(&in).len) {
		findings_cite_end(slot, (struct evidence_material [EVIDENCE_REFS]){});
		return;
	}
	findings_run(slot, STAGE_BLOBS, STORE_READ, nullptr, nullptr, &in);
}

/* Makes the record of each item that the evidence of the call in @p slot
 * cites again and that the ring does not store, from the findings that the
 * call read, once for the first reference to the item. If such a finding
 * is not read yet, cat-file first reads the file of the finding by its
 * path (findings_read_batch()), and the call comes back here through
 * findings_act(). Then the call reads the archive files of the items
 * (findings_cite_blobs()).
 */
static void
findings_cite_records (struct mcp_slot *slot)
{
	struct findings_call *call = *mcp_slot_own(slot);
	size_t const read = call->wanted.n;
	int e = 0;
	for (size_t i = 0; !e && i < call->cites.n; ++i) {
		struct findings_cite *cite = &call->cites.items[i];
		if (cite->first != i || dstr_len(&cite->record))
			continue;
		struct finding_id const finding = finding_id_without_item(&cite->ref.id);
		struct finding const *f = finding_find(call->findings.items, call->findings.n, &finding);
		struct evidence const *item = finding_item(f, cite->ref.id.item);
		/* cat-file reads a finding that the call has not read yet, once. */
		bool queued = f != nullptr;
		for (size_t k = read; !queued && k < call->wanted.n; ++k)
			queued = finding_id_equal(&call->wanted.items[k].id, &finding);
		if (item) {
			e = findings_record(item, &cite->record);
		} else if (f) {
			findings_no_item(slot, &cite->ref.id);
			return;
		} else if (!queued) {
			e = util_push(call->wanted, (struct findings_file){ .id = finding });
		}
	}
	if (e) {
		findings_end(slot, e, true);
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
		if (cstr_view_store(cstr_view_trim_end(cstr_view_n(t.data, kept)), texts[i]))
			return ENOMEM;
	}
	return 0;
}

/* Adds the reference @p ref to an item of a finding, X#n/m or X#n/m:A-B,
 * the item @p index of the evidence of the change of the call in @p slot,
 * to call->cites. findings_cite.first of a later reference is the index of
 * the first reference to the item. For lines of a later reference,
 * CITE_READ is set on the first reference, so that askai reads the archive
 * files of the item. The function copies the record and the git IDs of the
 * item to the first reference if the ring stores the item. With @p strict,
 * an item that a result of this turn showed whole is refused, and the
 * refusal of the call prints an item that no result showed whole
 * (CITE_SHOW). Returns 0, EINVAL with the refusal in the @p len bytes at
 * @p why, or ENOMEM.
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
	size_t const n = call->cites.n;
	if (util_grow(call->cites.items, call->cites.cap, n + 1))
		return ENOMEM;
	size_t first = 0;
	while (first < n && !finding_id_equal(&call->cites.items[first].ref.id, &ref->id))
		++first;
	uint8_t const lines = ref->form == EVIDENCE_REF_ITEM_LINES ? CITE_READ : 0;
	struct findings_cite *cite = &call->cites.items[n];
	*cite = findings_cite((uint32_t)index, (uint32_t)first, *ref);
	if (first < n) {
		call->cites.items[first].flags |= lines;
		++call->cites.n;
		return 0;
	}
	struct seen_item const *s = seen_find_item(mcp_slot_seen(slot), &ref->id);
	/* The turn whose result showed the item whole, or 0. */
	uint32_t const turn = s ? s->turn : 0;
	if (strict && turn && turn >= mcp_slot_turn(slot)) {
		char id[FINDING_ID_TEXT];
		finding_id_put(&ref->id, id);
		return util_refuse(why, len, "askai showed you %s in this turn. Cite %s for verified or "
		                   "disproved in a later turn.", id, id);
	}
	cite->flags = strict && !turn ? CITE_SHOW | CITE_READ | lines : lines;
	if (s) {
		if (cstr_view_store(cstr_view_dstr(&s->record), &cite->record)) {
			dstr_cleanup(&cite->record);
			return ENOMEM;
		}
		memcpy(cite->blobs, s->blobs, sizeof cite->blobs);
	}
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
	if (util_grow(c->evidence.items, c->evidence.cap, n))
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
		struct evidence_ref ref = {};
		if (evidence_ref_parse(&ref, text))
			return util_refuse(why, len, "evidence[%zu] is not a reference: %.*s. Give a call "
			                   "such as c7, lines such as c7:120-140, or an evidence item such "
			                   "as %s#5/2.", i,
			                   (int)util_clip(text.data, text.len, 80), text.data ? text.data : "",
			                   findings_session(slot)->agent.name);
		struct evidence *item = &c->evidence.items[c->evidence.n++];
		*item = evidence();
		if (ref.form == EVIDENCE_REF_ITEM || ref.form == EVIDENCE_REF_ITEM_LINES) {
			e = findings_cite_add(slot, &ref, c->evidence.n - 1, strict, why, len);
			continue;
		}
		struct evidence_material material = {};
		e = evidence_cite(item, &material, &lines, &ref, &cite, strict, why, len);
		e = e ? e : findings_archive(call, item, &material, (struct oid const [2]){});
		loop_work(mcp_slot_loop(slot), LOOP_COPY, material.request.len + material.result.len);
	}
	for (size_t i = 0; !e && i < m; ++i) {
		struct cstr_view const text = jb_view(jb_at(external, i));
		e = evidence_external(text, i, why, len);
		e = e ? e : finding_change_external(c, text);
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
	char const *agent = findings_session(slot)->agent.name;
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
	    (finding_id_parse(&call->id, jb_view(id)) || call->id.item))
		return util_refuse(why, len, "%s needs id: a finding, such as %s#5.", action, agent);
	/* expand of a turn without id prints all open findings of the agent. */
	if (call->action == FINDINGS_EXPAND && (id || mcp_slot_from_client(slot)) &&
	    finding_id_parse(&call->id, jb_view(id)))
		return util_refuse(why, len, "expand needs id: a finding, such as %s#5, or an "
		                   "evidence item, such as %s#5/2.", agent, agent);
	if (call->action == FINDINGS_UPDATE && strcmp(call->id.author, agent)) {
		char text[FINDING_ID_TEXT];
		finding_id_put(&call->id, text);
		return util_refuse(why, len, "Only %s can change %s. Add your own finding with about "
		                   "%s.", call->id.author, text, text);
	}
	if (author) {
		struct cstr_view const name = jb_view(author);
		if (!member_name_valid(name))
			return util_refuse(why, len, "The author argument must be the name of a member, "
			                   "such as %s.", agent);
		member_name_set(call->id.author, name);
	}
	if (about && (finding_id_parse(&call->about, jb_view(about)) || call->about.item))
		return util_refuse(why, len, "about must be the id of a finding, such as %s#2.", agent);
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
	memcpy(c->asked_by, member->asker.name, sizeof c->asked_by);
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
	struct tool_scratch *scratch = mcp_slot_scratch(slot);
	int const e = findings_args(slot, args, scratch->text, sizeof scratch->text);
	if (e) {
		findings_stop(slot, e, findings_refusal(scratch, e));
		return;
	}
	if (call->action == FINDINGS_EXPAND && *call->id.author)
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
	struct cstr_view rest = child_ok(c) ? cstr_view_span(buf_peek(&c->out))
	                                    : cstr_view(nullptr);
	struct cstr_view const ask = hexid_view(&mcp_slot_member(slot)->ask);
	char digits[16];
	struct cstr_view const number = util_format_view(digits, sizeof digits, "%" PRIu32,
	                                                 mcp_slot_call(slot));
	while (rest.len) {
		struct cstr_view line = {};
		if (!cstr_view_cut(&rest, '\0', &line)) {
			line = rest;
			rest = cstr_view(nullptr);
		}
		struct oid commit = {};
		struct cstr_view a = {}, n = {}, verb = {}, id = {};
		struct finding_id found = {};
		/* The subject is "Add bro#5 (verified)" or "Update bro#5 (...)". A
		 * commit without the trailer line Ask names no ask call. */
		if (oid_init_field(&commit, &line, ' ') || !cstr_view_cut(&line, ' ', &a) ||
		    !cstr_view_cut(&line, ' ', &n) || !a.len || !cstr_view_equal(a, ask) ||
		    !cstr_view_equal(n, number) || !cstr_view_cut(&line, ' ', &verb) ||
		    !cstr_view_cut(&line, ' ', &id) || finding_id_parse(&found, id) || found.item)
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

/* Allocates a call of @p action. Returns the call, or nullptr with ENOMEM
 * in @p *e.
 */
static struct findings_call *
findings_call_create (uint8_t  action,
                      int     *e)
{
	struct findings_call *call = malloc(sizeof *call);
	*e = call ? 0 : ENOMEM;
	if (call)
		*call = (struct findings_call){
			.out    = DSTR_INIT,
			.claim  = DSTR_INIT,
			.change = finding_change(),
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
	struct findings_call *call = findings_call_create(action, &e);
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
		char const *const words[] = { findings_session(slot)->refs[MEMBER_BRANCH_FINDINGS] };
		call->args = args;
		struct buf in = buf();
		findings_run(slot, STAGE_CALLS, STORE_CALLS, words, nullptr, &in);
		return;
	}
	findings_begin(slot, args);
}

static_assert(ON_CLIENTS == (1U << FINDINGS_ACTIONS) - (1U << FINDINGS_LIST),
              "the actions of clients are the last actions, from list on");

/* Returns the definition of the findings tool for clients, which can only
 * list, show and expand. @p service is the registry of the members, which
 * names the agent.
 */
static struct json_object *
findings_definition (void const *service)
{
	struct member_session const *session = member_registry_session(service);
	char const *agent = session ? session->agent.name : "";
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
	struct member_session const *session = member_registry_session(service);
	char const *agent = session ? session->agent.name : "";
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
		.flags = TOOL_SERIAL | TOOL_RERUN | TOOL_PAGED
	}}
};

void
findings_prompt (struct mcp_slot *slot,
                 struct dstr     *prompt,
                 size_t           most,
                 tool_step_fn    *then)
{
	int e;
	struct findings_call *call = findings_call_create(FINDINGS_LIST, &e);
	if (!call) {
		mcp_slot_fail(slot, e);
		return;
	}
	*mcp_slot_own(slot) = call;
	/* The list of the agent, with the open findings as its result. */
	struct member_session const *session = findings_session(slot);
	if (!session) {
		findings_drop(slot);
		mcp_slot_fail(slot, EINVAL);
		return;
	}
	memcpy(call->id.author, session->agent.name, sizeof call->id.author);
	call->prompt = prompt;
	call->then = then;
	call->most = most;
	findings_load(slot);
}
