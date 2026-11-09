/** @file notes_tool.c
 *
 * The notes tool (src/notes_tool.h).
 */
#define _GNU_SOURCE /* memmem(), mempcpy() */
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "buf.h"
#include "child.h"
#include "cstr_view.h"
#include "datauri.h"
#include "dstr.h"
#include "hexid.h"
#include "jb.h"
#include "list.h"
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
#include "shelf.h"
#include "store.h"
#include "util.h"

/* A fetch value says why git runs on the notes (src/store.h). The values
 * up to FETCH_KIND are kinds, and FETCH_KIND masks them. Each kind is a
 * row of notes_tool_fetch_steps[]. The bits above FETCH_KIND are flags.
 */
enum notes_tool_fetch : uint32_t {
	FETCH_CHECK,           /* check the note name before a write */
	FETCH_ADD,             /* commit the note of a write */
	FETCH_LIST,            /* list the notes for history */
	FETCH_READ,            /* read a note */
	FETCH_RESUME,          /* list the notes for the prompt of an ask call */
	FETCH_COMPACT,         /* list the notes for a compaction */
	FETCH_FILES,           /* list the current files for list */
	FETCH_NEWEST,          /* read the newest note, after FETCH_RESUME or
	                          FETCH_COMPACT */
	FETCH_DIFF,            /* compare two revisions of a note */
	FETCH_STAT,            /* count the lines that a write changed */
	FETCH_GREP,            /* find the lines that match a pattern */
	FETCH_REBASE,          /* check the note of a write at another commit:
	                          at the new tip after a race, or at the tip
	                          that a restore pinned */
	FETCH_KIND = 15,       /* the mask of the kinds */
	FETCH_APPEND = 16,     /* set for append, also with FETCH_READ to read
	                          the note to append to */
	FETCH_COPY = 32,       /* set with FETCH_READ for copy. The note goes to
	                          the caller's own notebook. */
	FETCH_ATTACH = 64,     /* set with FETCH_READ for attach. askai uploads
	                          the note as a file for the turn. */
	FETCH_REPLACE = 128,   /* set for a write that needs an existing note */
	FETCH_REVISION = 256,  /* check the revisions before git reads a note */
	FETCH_RESTORE = 512,   /* write an old revision of a note in a new
	                          commit */
	FETCH_DELETE = 1024,   /* set for delete */
};

/* A row of notes_tool_fetch_steps[]: the step that reads the output of
 * git, and the git command for a fetch value. listed is nonzero if git
 * writes a list, and the call fails without one (notes_tool_listed()).
 */
struct notes_tool_fetch_step {
	tool_step_fn *out;
	uint32_t      cmd;     /* enum store_cmd, widened over the padding */
	uint32_t      listed;
};

static struct notes_tool_fetch_step const *notes_tool_fetch_step (uint32_t fetch);

/* The flags of an action in notes_tool_actions[]. */
enum {
	NOTE_WRITE    = 1 << 0,  /* writes to the caller's own notebook */
	NOTE_FILE     = 1 << 1,  /* takes the file argument */
	NOTE_OFFSET   = 1 << 2,  /* takes the offset argument */
	NOTE_REVISION = 1 << 3,  /* takes the revision argument */
	NOTE_AUTHOR   = 1 << 4,  /* writes a note as the caller, which needs
	                            the member name of the caller */
};

/* The flags and the fetch value of each action (NOTES_ALL_ACTION_LIST). */
static struct {
	uint32_t              flags;
	enum notes_tool_fetch fetch;
} const notes_tool_actions[] = {
#define NOTES_ACTION_ROW(id, name, flags, fetch) { flags, fetch },
	NOTES_ALL_ACTION_LIST(NOTES_ACTION_ROW)
#undef NOTES_ACTION_ROW
};

/* The message for a file or as argument that is not a valid name. */
static struct cstr_view const bad_name =
	CSTR_VIEW_INIT("A note name must be 1 to 255 bytes without control characters. Examples: "
	               "plan.md, design/locks.md. No part between slashes can be empty, ., .., "
	               ".git or git~1. Letter case and spaces or dots at the end of a part do not "
	               "matter for .git and git~1.");

/* The message for a call that names a directory of notes. */
static struct cstr_view const dir_name =
	CSTR_VIEW_INIT("The note name is a directory of notes. list shows the notes in the "
	               "directory.");

/* The message for a call that names a note that does not exist. */
static struct cstr_view const no_note =
	CSTR_VIEW_INIT("The note does not exist. list shows the current notes. add creates a "
	               "note.");

/* The message for a write that raced another writer, if the note changed
 * or the write made STORE_TRIES commits.
 */
static struct cstr_view const raced =
	CSTR_VIEW_INIT("Could not write the note. Another call changed the notebook at the same "
	               "time. Read the current note and retry.");

/* The flags of a notes call (struct notes_tool_call). */
enum notes_tool_call_flag : uint8_t {
	CALL_STRIPPED = 1u << 0,  /* askai cut its notices off the text */
	CALL_AS       = 1u << 1,  /* the call gave the argument as */
	CALL_LOGIN    = 1u << 2,  /* the call waits for the member of the login
	                             of its client, without git */
	CALL_READ     = 1u << 3,  /* the call waits for the read of the records
	                             of the members (member_table_read()) */
};

/* The state of one notes call. It is the own word of the call's slot. An
 * ask call has one in its own slot while git reads the notes of the agent
 * for its prompt.
 */
struct notes_tool_call {
	struct buf              file;      /* the input of git, such as the
	                                      pattern of a search, until
	                                      notes_tool_run() passes it to
	                                      git, or the file part of a write,
	                                      until the call ends */
	struct dstr             out;       /* the output of a tool call.
	                                      notes_tool_end() lends it to
	                                      the slot. */
	struct store_part       part;      /* the offset and the bytes of the
	                                      text in file */
	struct list             spare;     /* the call's node on loop->spare */
	struct member_wait      wait;      /* the lookup of the member of the
	                                      login of a client, or the read of
	                                      the records of the members */
	struct child           *child;     /* the call's git, until a step has
	                                      read its output */
	struct notes_tool_loop *loop;      /* the part of the call's loop */
	struct mcp_slot        *slot;      /* the slot of the call */
	struct dstr            *text;      /* &out for a tool call. For the
	                                      prompt notes, it is the text that
	                                      askai adds to the prompt. */
	tool_step_fn           *then;      /* the step that goes on with the ask
	                                      call after the prompt notes */
	size_t                  upload;    /* bytes of the file that attach
	                                      uploads */
	size_t                  note;      /* bytes of the note that attach
	                                      uploads */
	size_t                  most;      /* the most bytes of the newest note
	                                      that the prompt shows */
	struct notes_revision   revision;  /* the revisions of a read, diff or
	                                      restore, or the file of a history
	                                      or of a write; zeroed for the
	                                      other calls. tip is the parent of
	                                      the commit of a write. */
	union {
		UTIL_TEXT(STORE_MESSAGE)  message;  /* the commit message of a
		                                       write, for its result */
		struct oid                id;       /* the object id of an
		                                       attached note, until the
		                                       upload's answer */
		char                      shows[MEMBER_NAME + sizeof " (person)"];
		                                    /* book and its kind, such as
		                                       "bro (agent)", for the
		                                       result of a read
		                                       (notes_tool_shows()) */
	};
	UTIL_TEXT(NOTES_NAME + 1) name;    /* the name of the note that a copy
	                                      or a restore writes: the as
	                                      argument, or else file */
	uint32_t                fetch;     /* why git runs (enum
	                                      notes_tool_fetch) */
	uint32_t                first;     /* the fetch value of the first git
	                                      run of a tool call, which runs
	                                      once the caller and the notebook
	                                      are known */
	uint8_t                 action;    /* the row of notes_tool_actions[] */
	uint8_t                 tries;     /* the commits of a write that
	                                      raced, at most STORE_TRIES */
	uint8_t                 flags;     /* enum notes_tool_call_flag */
	struct oid              blob;      /* the blob of the note of a write
	                                      at revision.tip, or no name for a
	                                      new note. The commit of the write
	                                      is made on another tip only if
	                                      the note is the same there. */
	struct member           self;      /* the caller: the agent for a turn,
	                                      the member of the login for a
	                                      client, or no name */
	struct member           book;      /* the member whose notebook the
	                                      call reads. Until
	                                      notes_tool_book() finds the
	                                      member, the name is the notebook
	                                      argument, or "" for the notebook
	                                      of the caller. */
	char                    repo[MEMBER_REF];      /* the notebook of the
	                                                  caller, or "" */
	char                    notebook[MEMBER_REF];  /* the notebook that the
	                                                  call selected, or ""
	                                                  for repo */
};

void *
notes_tool_loop_create (void           *service,
                        struct loop    *loop,
                        struct blocker *blockers,
                        struct list    *children,
                        int            *e)
{
	struct notes_tool const *tool = service;
	struct notes_tool_loop *nl = malloc(sizeof *nl);
	if (!nl) {
		*e = ENOMEM;
		return nullptr;
	}
	*e = 0;
	*nl = (struct notes_tool_loop){ .spare.list = list(&nl->spare.list), .tool = tool };
	if (tool->members &&
	    !(nl->members = member_table_create(tool->members, loop, blockers, children, e))) {
		free(nl);
		return nullptr;
	}
	return nl;
}

/* Frees the notes call at @p *p_call and its output, and sets @p *p_call
 * to nullptr.
 */
static void
notes_tool_call_destroy (struct notes_tool_call **p_call)
{
	dstr_cleanup(&(*p_call)->out);
	free(*p_call);
	*p_call = nullptr;
}

void
notes_tool_loop_age (struct notes_tool_loop *loop)
{
	shelf_age(&loop->spare);
}

size_t
notes_tool_loop_relieve (struct notes_tool_loop *loop,
                         enum buf_pressure       level,
                         size_t                  max)
{
	size_t done = 0;
	for (size_t n = shelf_unused(&loop->spare, level); n; --n) {
		struct notes_tool_call *call = container_of(loop->spare.list.next,
		                                            struct notes_tool_call, spare);
		size_t const units = buf_units(sizeof *call + DSTR_BLK_SIZE *
		                               dstr_get_block_count(&call->out));
		if (done && done + units > max)
			break;
		(void)shelf_drop(&loop->spare);
		notes_tool_call_destroy(&call);
		done += units;
	}
	return done;
}

void
notes_tool_loop_destroy (void **p_part)
{
	struct notes_tool_loop *nl = *p_part;
	if (!nl)
		return;
	*p_part = nullptr;
	(void)notes_tool_loop_relieve(nl, BUF_CRITICAL, SIZE_MAX);
	member_table_destroy(&nl->members);
	free(nl);
}

/* Called on the loop once the table of the members ends the wait @p wait
 * of a notes call. The step of the call then goes on (mcp_slot_ready()).
 */
static void
notes_tool_waited (struct member_wait *wait)
{
	(void)mcp_slot_ready(container_of(wait, struct notes_tool_call, wait)->slot);
}

/* Returns a notes call that uses the part @p loop and writes to @p text,
 * or to call->out if @p text is nullptr, and stores the call in the own
 * word of @p slot. The call is the last one put on loop->spare, or a new
 * one from malloc(). If malloc() fails, the function stores nullptr in the
 * own word, calls mcp_slot_fail(slot, ENOMEM) and returns nullptr.
 */
static struct notes_tool_call *
notes_tool_loop_take (struct notes_tool_loop *loop,
                      struct mcp_slot        *slot,
                      struct dstr            *text)
{
	struct list *spare = shelf_take(&loop->spare);
	struct notes_tool_call *call;
	struct dstr out = DSTR_INIT;
	if (spare) {
		call = container_of(spare, struct notes_tool_call, spare);
		out = dstr_move(&call->out);
	} else if (!(call = malloc(sizeof *call))) {
		*mcp_slot_own(slot) = nullptr;
		mcp_slot_fail(slot, ENOMEM);
		return nullptr;
	}
	*call = (struct notes_tool_call){
		.out   = out,
		.loop  = loop,
		.slot  = slot,
		.text  = text ? text : &call->out,
		.spare = list(&call->spare),
		.wait  = { .hook = list(&call->wait.hook), .done = notes_tool_waited },
	};
	*mcp_slot_own(slot) = call;
	return call;
}

/* Ends the notes call at @p *p_call, which @p loop gave out, and sets
 * @p *p_call to nullptr. store_drop() lets go of the call's git. Without
 * memory pressure, the function then puts the call on loop->spare for
 * reuse. dstr_clear() keeps the storage of the output. Under memory
 * pressure, the function frees the call and the output.
 */
static void
notes_tool_loop_put (struct notes_tool_loop  *loop,
                     struct notes_tool_call **p_call)
{
	struct notes_tool_call *call = *p_call;
	member_table_cancel(&call->wait);
	store_drop(&call->child);
	buf_fini(&call->file);
	if (atomic_load_explicit(&buf_pressure, memory_order_relaxed) != BUF_EASY) {
		notes_tool_call_destroy(p_call);
		return;
	}
	(void)dstr_clear(&call->out);
	shelf_put(&loop->spare, &call->spare);
	*p_call = nullptr;
}

void
notes_tool_drop (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (call)
		notes_tool_loop_put(call->loop, &call);
	*mcp_slot_own(slot) = nullptr;
}

static void notes_tool_run (struct mcp_slot *slot, uint32_t fetch, struct buf *in, int e);
static void notes_tool_launch (struct mcp_slot *slot);
static void notes_tool_logged_in (struct mcp_slot *slot);

/* Returns the session of the notes and the agent of askai in it. */
static struct member_session const *
notes_tool_session (struct notes_tool_call const *call)
{
	return member_registry_session(call->loop->tool->members);
}

/* Makes the agent of askai the caller of @p call. */
static void
notes_tool_agent (struct notes_tool_call *call)
{
	call->self.kind = MEMBER_AGENT;
	util_format(call->self.name, sizeof call->self.name, "%s", notes_tool_session(call)->agent);
}

/* Releases the notes call in @p slot, the ask call's own slot, after git
 * has read the notes for the prompt. If @p e is not 0, mcp_slot_fail() ends
 * the ask call with @p e. Otherwise the function calls call->then, which
 * goes on with the ask call.
 */
static void
notes_tool_then (struct mcp_slot *slot,
                 int              e)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	tool_step_fn *then = call->then;
	notes_tool_drop(slot);
	if (e)
		mcp_slot_fail(slot, e);
	else
		then(slot);
}

/* Handles the error @p e of spawning git for the call in @p slot. For a
 * compaction, for the newest note and for the lines of a write, the step
 * runs with no child, as if git had failed. The ask call then goes on
 * without those notes, and the write's result without those lines. A call
 * of the notes tool, and the list for the start of an ask call, end with
 * the error.
 */
static void
notes_tool_failed (struct mcp_slot *slot,
                   int              e)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	char buf[96];
	struct cstr_view const why = child_spawn_why(e, "git", buf, sizeof buf);
	switch (call->fetch & FETCH_KIND) {
	case FETCH_NEWEST:
	case FETCH_COMPACT:
	case FETCH_STAT:
		notes_tool_fetch_step(call->fetch)->out(slot);
		return;
	case FETCH_RESUME:
		notes_tool_drop(slot);
		break;
	}
	mcp_slot_tell(slot, why, TOOL_FAILED);
}

/* Writes the name and the kind of the member whose notebook @p call reads
 * to call->shows, such as "bro (agent)", and returns call->shows. Only a
 * call that writes no note and attaches no note can use call->shows,
 * which shares its storage.
 */
static char const *
notes_tool_shows (struct notes_tool_call *call)
{
	util_format(call->shows, sizeof call->shows, "%s (%s)", call->book.name,
	            member_kinds[call->book.kind].data);
	return call->shows;
}

/* Returns the notebook that @p call selected, or else call->repo. */
static char const *
notes_tool_call_notebook (struct notes_tool_call const *call)
{
	return *call->notebook ? call->notebook : call->repo;
}

/* Ends the call of the notes tool in @p slot. The function destroys the
 * child and frees the file part of the call. If @p e is not 0,
 * mcp_slot_fail() ends the call and its request with @p e. Otherwise the
 * call ends with its output, as a failed call if @p failed is true.
 */
static void
notes_tool_end (struct mcp_slot *slot,
                int              e,
                bool             failed)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	child_destroy(&call->child);
	buf_fini(&call->file);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	mcp_slot_lend(slot, cstr_view_dstr(&call->out));
	/* The offset of a list or a read does not apply to the output of a
	 * failed call. */
	if (failed)
		(void)mcp_slot_offset(slot, nullptr);
	mcp_slot_finish(slot, failed ? TOOL_FAILED : 0);
}

/* Spawns git for @p fetch in the shared repository (store_spawn()) and
 * frees @p in, the input of git, which may be empty. git reads the
 * notebook that the call selected, or else call->repo. Writes always go
 * to call->repo, whose lock file store_spawn() checks. If
 * @p e, the error of making @p in, is not 0, the function frees @p in and
 * ends the call with @p e instead (notes_tool_end()).
 */
static void
notes_tool_run (struct mcp_slot *slot,
                uint32_t         fetch,
                struct buf      *in,
                int              e)
{
	if (e) {
		buf_fini(in);
		notes_tool_end(slot, e, true);
		return;
	}
	struct notes_tool_call *call = *mcp_slot_own(slot);
	char const *repo = notes_tool_call_notebook(call);
	enum store_cmd cmd = notes_tool_fetch_step(fetch)->cmd;
	char const *words[STORE_WORDS] = {
		repo, call->revision.file.len ? call->revision.file.data : nullptr
	};
	if (fetch & FETCH_REVISION)
		cmd = notes_revision_words(&call->revision, repo, cmd, words);
	call->fetch = fetch;
	call->child = store_spawn(call->loop->tool->store, slot, cmd, words, call->repo,
	                          buf_peek(in), &e);
	buf_fini(in);
	if (!call->child)
		notes_tool_failed(slot, e);
}

/* Appends the notices of the agent and of the members to the prompt of the
 * ask call in @p slot, and runs git to list the notes of the agent.
 */
static void
notes_tool_prompt_notices (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	int e = cstr_view_cat(CSTR_VIEW("\n\n"), call->text);
	e = e ? e : member_notice(&call->self, mcp_slot_member(slot), call->text);
	e = e ? e : member_table_notice(call->loop->members, &call->self, call->text);
	if (e) {
		notes_tool_then(slot, e);
		return;
	}
	notes_tool_run(slot, call->first, &(struct buf){}, 0);
}

/* Parks the call @p call if @p e, the result of a function of the table of
 * the members that waits with call->wait, is EINPROGRESS, and sets
 * @p flag, the state from which notes_tool_step() goes on. Returns true if
 * the call is parked.
 */
static bool
notes_tool_park (struct notes_tool_call    *call,
                 int                        e,
                 enum notes_tool_call_flag  flag)
{
	if (e != EINPROGRESS)
		return false;
	call->flags |= flag;
	(void)mcp_slot_park(call->slot);
	return true;
}

void
notes_tool_loop_prompt (struct notes_tool_loop *loop,
                        struct mcp_slot        *slot,
                        struct dstr            *prompt,
                        enum notes_tool_prompt  what,
                        size_t                  most,
                        tool_step_fn           *then)
{
	struct notes_tool_call *call = notes_tool_loop_take(loop, slot, prompt);
	if (!call)
		return;
	call->then = then;
	call->most = most;
	notes_tool_agent(call);
	call->book = call->self;
	member_ref(&call->self, MEMBER_BRANCH_NOTES, call->repo);
	if (what == NOTES_TOOL_COMPACT) {
		call->first = FETCH_COMPACT;
		notes_tool_prompt_notices(slot);
		return;
	}
	call->first = FETCH_RESUME;
	/* The asker of an ask call whose turns get the notes becomes a member. */
	int const e = member_table_claim(loop->members, &mcp_slot_member(slot)->login, &call->wait);
	if (!notes_tool_park(call, e, CALL_LOGIN))
		notes_tool_logged_in(slot);
}

/* Returns true if git wrote a list of the notes. A list that askai cut
 * contains the newest notes. @p c is nullptr if git did not start.
 */
static bool
notes_tool_listed (struct child const *c)
{
	return c && (c->end.flags & CHILD_CUT || !child_status(c));
}

/* Returns true if git exited with status 128. It does so if the
 * repository or the branch of the notes does not exist.
 */
static bool
notes_tool_none (struct child const *c)
{
	return child_status(c) == 128;
}

/* Ends the call in @p slot with why git read no notes. Exit status 128
 * means that the notebook does not exist yet. For list, history and
 * search, a notebook without notes is an answer, not a failure. A turn's
 * offset applies to the answer, and a client's offset does not.
 */
static void
notes_tool_unread (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (!notes_tool_none(call->child)) {
		mcp_slot_tell(slot, CSTR_VIEW("The notes could not be read."), TOOL_FAILED);
		return;
	}
	static char const yours[] = "Your notebook %s contains no notes yet.";
	static char const other[] = "The notebook %s contains no notes yet.";
	char buf[sizeof yours + MEMBER_NAME];
	struct cstr_view const none = util_format_view(buf, sizeof buf,
	                                               *call->notebook ? other : yours,
	                                               call->book.name);
	if (notes_tool_actions[call->action].flags & NOTE_FILE)
		mcp_slot_tell(slot, none, TOOL_FAILED);
	else if (mcp_slot_from_client(slot))
		mcp_slot_tell(slot, none, 0);
	else
		notes_tool_end(slot, cstr_view_cat(none, call->text), false);
}

/* Fails the write in @p slot after its git failed. The reasons are a
 * timeout and other errors of git, which the function logs.
 */
static void
notes_tool_unwritten (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	if (c->end.flags & CHILD_LATE) {
		mcp_slot_tell(slot, CSTR_VIEW("Could not write the note. The write timed out."),
		              TOOL_FAILED);
		return;
	}
	struct span out = buf_peek(&c->out);
	char const *why = (char const *)out.ptr;
	LOG(LOG_WARN, "notes: git could not write a note: %.*s",
	    (int)util_clip(why, out.len, 300), why);
	mcp_slot_tell(slot, CSTR_VIEW("Could not write the note."), TOOL_FAILED);
}

/* Returns the name of the note that the write of @p call writes:
 * call->name for a copy or a restore, else call->revision.file.
 */
static struct cstr_view
notes_tool_target (struct notes_tool_call const *call)
{
	return notes_tool_actions[call->action].fetch & (FETCH_COPY | FETCH_RESTORE)
	     ? util_text_view(&call->name) : util_text_view(&call->revision.file);
}

/* Destroys the git of the call in @p slot, and runs git for @p fetch to
 * check at @p ref the note that the write of the call writes: FETCH_CHECK
 * at call->repo before a copy or a restore, or FETCH_REBASE at another
 * commit (notes_tool_rebased()). If @p e is not 0, the function ends the
 * call with @p e instead.
 */
static void
notes_tool_check (struct mcp_slot *slot,
                  uint32_t         fetch,
                  char const      *ref,
                  int              e)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct buf in = {};
	if (!e)
		e = store_check_input(&in, ref, notes_tool_target(call));
	child_destroy(&call->child);
	notes_tool_run(slot, fetch, &in, e);
}

/* Handles a commit of a write in @p slot that git did not make. After a
 * race with another writer, git checks the note at the new tip of the
 * notebook (notes_tool_rebased()), until the write has made STORE_TRIES
 * commits. The write then fails with the text raced. A lock file that a
 * dead or hung writer left fails the write, and the function logs the
 * lock file. Other failures of git fail the write in
 * notes_tool_unwritten().
 */
static void
notes_tool_unadded (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	if (c->end.flags & CHILD_LATE || !store_raced(buf_peek(&c->out))) {
		notes_tool_unwritten(slot);
		return;
	}
	int64_t const age = store_stale(call->loop->tool->store, call->repo, "notes");
	if (age) {
		mcp_slot_tellf(slot, TOOL_FAILED, "Could not write the note. Your notebook %s has "
		               "been locked for %" PRId64 " s.", call->self.name, age);
		return;
	}
	if (++call->tries >= STORE_TRIES) {
		mcp_slot_tell(slot, raced, TOOL_FAILED);
		return;
	}
	notes_tool_check(slot, FETCH_REBASE, call->repo, 0);
}

/* The size of a note before a write that creates the note. */
static constexpr uint64_t NO_NOTE = UINT64_MAX;

/* Writes to call->message the commit message for the note that @p call
 * writes. @p before is the size of the note before the write, or NO_NOTE.
 * Returns 0, or EINVAL if the message does not fit.
 */
static int
notes_tool_call_message (struct notes_tool_call *call,
                         uint64_t                before)
{
	uint32_t fetch = notes_tool_actions[call->action].fetch;
	struct cstr_view const verb = notes_action_names[call->action];
	char const up = (char)(verb.data[0] - 'a' + 'A');
	/* A restore names the commit of its revision, and a copy the source
	 * notebook and the commit it read. With as, the message names the
	 * source file after the commit, as commit:file. */
	char const *commit = call->revision.from.hex;
	char from[sizeof " from the notebook of  at " + MEMBER_NAME + OID_SHA256] = "";
	if (fetch & FETCH_COPY)
		util_format(from, sizeof from, " from the notebook of %s at %s", call->book.name,
		            commit);
	else if (*commit)
		util_format(from, sizeof from, " from %s", commit);
	char const *source = call->flags & CALL_AS ? call->revision.file.data : "";
	char name[STORE_QUOTED];
	static_assert(sizeof "Copy  from the notebook of  at : ( bytes)" + UTIL_DECIMAL +
	              sizeof name + MEMBER_NAME + OID_SHA256 + NOTES_NAME <= STORE_MESSAGE);
	static_assert(sizeof "Restore  from : ( to  bytes)" + 2 * UTIL_DECIMAL + sizeof name +
	              OID_SHA256 + NOTES_NAME <= STORE_MESSAGE);
	int const len = (int)(store_quote(name, notes_tool_target(call)) - name);
	uint64_t after = call->part.data_len;
	util_text_clear(&call->message);
	int e = util_text_printf(&call->message, "%c%.*s %.*s%s%s%s", up, (int)verb.len - 1,
	                         verb.data + 1, len, name, from, *source ? ":" : "", source);
	return e ? e : before == NO_NOTE
	     ? util_text_printf(&call->message, " (%" PRIu64 " byte%s)", after, util_s(after))
	     : util_text_printf(&call->message, " (%" PRIu64 " to %" PRIu64 " bytes)", before, after);
}

/* Runs git to commit the note in call->file of @p slot, with the message
 * call->message. The new commit has the parent call->revision.tip, or no
 * parent if call->revision.tip is no name. call->file stays for another
 * commit after a race. If @p e is not 0, the function ends the call with
 * @p e instead.
 */
static void
notes_tool_write (struct mcp_slot *slot,
                  int              e)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	/* The author is the caller, and the committer is this askai. The
	 * commit of a turn names the ask call, the call, the conversation and
	 * the asker. The request of a call of a client contains none of
	 * them. */
	struct buf in = {};
	struct store_commit commit = {
		.message = util_text_view(&call->message),
		.files = buf_peek(&call->file),
		.ref = call->repo,
		.tip = &call->revision.tip,
	};
	struct member_byline byline;
	member_byline_put(&byline, &commit, &call->self, mcp_slot_member(slot), mcp_slot_call(slot),
	                  notes_tool_session(call)->service);
	e = e ? e : store_commit_input(&commit, &in);
	notes_tool_run(slot, FETCH_ADD, &in, e);
}

/* Ends the write in @p slot after it saved the note or left the note
 * unchanged. If @p e is not 0, the call fails with @p e (notes_tool_end()).
 */
static void
notes_tool_wrote (struct mcp_slot *slot,
                  int              e)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	if (!e && call->flags & CALL_STRIPPED)
		e = cstr_view_cat(CSTR_VIEW(" askai removed the [askai: ...] lines from the end of "
		                            "the text. askai adds [askai: ...] lines to tool "
		                            "results, not to notes."), call->text);
	notes_tool_end(slot, e, false);
}

/* Returns true if @p note, the note that STORE_CHECK found, already has
 * the bytes of call->file. The function then answers the replace or
 * restore in @p slot without a commit. The answer names @p tip, the newest
 * commit of the branch. The caller's own notebook contains notes, and
 * the function calls mcp_slot_noted(). If the bytes cannot be compared,
 * the function logs a warning and returns false, and the write goes on.
 */
static bool
notes_tool_unchanged (struct mcp_slot         *slot,
                      struct notes_file const *note,
                      struct oid const        *tip)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct span const data = { buf_peek(&call->file).ptr + call->part.data_at,
	                           call->part.data_len };
	bool same = false;
	int e = notes_file_same(note, data, &same);
	if (e)
		LOG(LOG_WARN, "notes: cannot compare a note with its new bytes: %s", strerror(e));
	else if (data.len == note->bytes)
		loop_work(mcp_slot_loop(slot), LOOP_COPY, data.len);
	if (!same)
		return false;
	char name[STORE_QUOTED];
	size_t const len = (size_t)(store_quote(name, notes_tool_target(call)) - name);
	uint64_t n = note->bytes;
	e = dstr_printf(call->text, "Unchanged. %.*s at the newest commit %s already contains "
	                "the same %" PRIu64 " byte%s. No commit was made.",
	                (int)jb_utf8(name, len), name, tip->hex, n, util_s(n)) ? 0 : ENOMEM;
	(void)mcp_slot_noted(slot);
	notes_tool_wrote(slot, e);
	return true;
}

/* Handles the output of STORE_CHECK for a write. add and copy accept only a
 * name that is neither a note nor a directory of notes. No directory in the
 * name can be a note. The writes with FETCH_REPLACE accept only the name
 * of an existing note, and git reads the note for append. A replace or a
 * restore that leaves the bytes of the note unchanged makes no commit.
 * The function stores the newest commit, the parent of the write, in
 * call->revision.tip, and the blob of the note there in call->blob, or no
 * name for a write that makes a new note. A restore read its note at the
 * tip that git pinned, call->revision.tip. If the newest commit is another
 * one, the restore raced another writer, and git checks the note at the
 * pinned tip before the commit (notes_tool_rebased()).
 */
static void
notes_tool_checked (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct oid tip;
	struct notes_file note = {};
	int r = child_ok(c) ? store_checked(buf_peek(&c->out), &tip, &note.id, &note.bytes) : EIO;
	uint64_t before = r == EEXIST ? note.bytes : NO_NOTE;
	uint32_t fetch = notes_tool_actions[call->action].fetch;
	if (fetch & FETCH_REPLACE)
		r = r == EEXIST ? 0 : r && r != ENOTDIR ? r : ENOENT;
	if (fetch & FETCH_RESTORE && r == EEXIST && !(call->flags & CALL_AS))
		r = 0;
	if (!r) {
		struct oid const pinned = call->revision.tip;
		bool const moved = fetch & FETCH_RESTORE && !oid_equal(&tip, &pinned);
		call->revision.tip = tip;
		call->blob = note.id;
		child_destroy(&call->child);
		if (fetch & FETCH_APPEND) {
			/* git reads the note at the tip that the write builds on. */
			struct buf in = {};
			int e = notes_read_input(&in, tip.hex, util_text_view(&call->revision.file));
			notes_tool_run(slot, FETCH_READ | FETCH_APPEND, &in, e);
		} else if (before == NO_NOTE || fetch & FETCH_DELETE ||
		           !notes_tool_unchanged(slot, &note, &tip)) {
			int e = notes_tool_call_message(call, before);
			if (moved)
				notes_tool_check(slot, FETCH_REBASE, pinned.hex, e);
			else
				notes_tool_write(slot, e);
		}
		return;
	}
	if (r == EIO) {
		notes_tool_unread(slot);
		return;
	}
	mcp_slot_tell(slot,
	              r == EEXIST ? CSTR_VIEW("The note already exists. Change the note with "
	                                      "replace or append, or choose another name.")
	              : r == ENOENT ? no_note
	              : r == EISDIR ? dir_name
	              : CSTR_VIEW("A part of the note name before a slash is a note."),
	              TOOL_FAILED);
}

/* Ends the write in @p slot after its result named the new commit. If the
 * write changed an existing note, the result names the commit of the
 * previous version, call->revision.tip. If @p e is not 0, the call fails
 * with @p e.
 */
static void
notes_tool_saved (struct mcp_slot *slot,
                  int              e)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	if (!e && oid_len(&call->blob))
		e = dstr_printf(call->text, " The previous version is in commit %s.",
		                call->revision.tip.hex) ? 0 : ENOMEM;
	notes_tool_wrote(slot, e);
}

/* Handles the output of STORE_STAT after a replace or restore. The
 * function appends to the result the lines that the write removed and
 * added, unless git failed or counted the note as binary.
 */
static void
notes_tool_stated (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct span const out = child_ok(call->child) ? buf_peek(&call->child->out)
	                                              : (struct span){};
	struct cstr_view rest = { (char const *)out.ptr, out.len }, added, removed;
	uint64_t a, r;
	int e = cstr_view_cut(&rest, '\t', &added) && cstr_view_cut(&rest, '\t', &removed) &&
	        !util_number(added.data, added.len, UINT64_MAX, &a) &&
	        !util_number(removed.data, removed.len, UINT64_MAX, &r) &&
	        !dstr_printf(call->text, " %" PRIu64 " line%s removed, %" PRIu64 " added.", r,
	                     util_s(r), a) ? ENOMEM : 0;
	notes_tool_saved(slot, e);
}

/* Handles the end of git fast-import. The result contains the id of the
 * new commit and the commit message, or the reason why git did not save
 * the note. The result of a copy names the copy before the commit
 * (notes_tool_got_copy()). The caller's own notebook contains notes then,
 * and the function calls mcp_slot_noted(). After a replace or restore of
 * an existing note, git counts the lines that the write changed.
 */
static void
notes_tool_added (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (!child_ok(call->child)) {
		notes_tool_unadded(slot);
		return;
	}
	(void)mcp_slot_noted(slot);
	struct oid commit;
	if (store_committed(buf_peek(&call->child->out), &commit))
		LOG(LOG_WARN, "notes: git fast-import gave no commit id");
	/* The result of a copy names the copy instead of the message. */
	struct cstr_view colon = CSTR_VIEW(": ");
	struct cstr_view message = { call->message.data,
	                             jb_utf8(call->message.data, call->message.len) };
	if (notes_tool_actions[call->action].fetch & FETCH_COPY)
		colon = message = CSTR_VIEW("");
	struct cstr_view const p[] = {
		oid_len(&commit) ? CSTR_VIEW("Saved in commit ") : CSTR_VIEW("Saved"),
		oid_view(&commit),
		colon,
		message,
		CSTR_VIEW("."),
	};
	int e = cstr_view_join(p, array_size(p), UINT64_MAX, CSTR_VIEW(""), CSTR_VIEW(""),
	                       call->text);
	if (e || !oid_len(&commit) || !oid_len(&call->blob) ||
	    notes_tool_actions[call->action].fetch & (FETCH_APPEND | FETCH_DELETE)) {
		notes_tool_saved(slot, e);
		return;
	}
	notes_revision_pin(&call->revision, &call->revision.tip, &commit);
	child_destroy(&call->child);
	notes_tool_run(slot, FETCH_STAT | FETCH_REVISION, &(struct buf){}, 0);
}

/* Handles the output of STORE_CHECK for the note of the write in @p slot
 * at another commit than call->revision.tip: at the new tip after the
 * commit of the write raced another writer, or at the tip that a restore
 * pinned, before the first commit. If the note is call->blob there, or is
 * missing at both commits, and no directory in its name is a file, git
 * makes the commit of the write, after a race on the new tip. Otherwise
 * the write fails with the text raced, also if the ref names no commit. A
 * failed git or an output that store_checked() cannot read fails the
 * write as notes_tool_unwritten() does.
 */
static void
notes_tool_rebased (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct oid tip, blob;
	uint64_t bytes;
	int r = child_ok(c) ? store_checked(buf_peek(&c->out), &tip, &blob, &bytes) : EIO;
	if (r == EIO) {
		notes_tool_unwritten(slot);
		return;
	}
	if ((r && r != EEXIST) || !oid_len(&tip) || !oid_equal(&blob, &call->blob)) {
		mcp_slot_tell(slot, raced, TOOL_FAILED);
		return;
	}
	/* After a race, the new tip is the parent. Before the first commit,
	 * the check was at the tip that a restore pinned, and the parent
	 * stays. */
	if (call->tries)
		call->revision.tip = tip;
	child_destroy(&call->child);
	notes_tool_write(slot, 0);
}

/* Appends the history in @p out, the output of git for STORE_LOG, to
 * call->text, the oldest change first. Stores the name of the newest note
 * in @p *newest. @p cut is true if askai cut the output of git. A cut
 * output lacks the oldest changes. Returns 0 or ENOMEM.
 */
static int
notes_tool_history (struct mcp_slot *slot,
                    struct span      out,
                    bool             cut,
                    struct span     *newest)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct dstr *d = call->text;
	*newest = (struct span){};
	if (cut && cstr_view_cat(CSTR_VIEW("[The oldest changes are not in the list.]\n"),
	                         call->text))
		return ENOMEM;
	struct notes_cursor cursor = { .end = out.len };
	struct notes_entry entry;
	bool history = (call->fetch & FETCH_KIND) == FETCH_LIST;
	struct cstr_view const filter = util_text_view(&call->revision.file);
	uint32_t k = 0;
	while (notes_entry_before(out, &cursor, &entry)) {
		char const *name = (char const *)entry.name.ptr;
		/* Git also matches descendants if this path was once a directory. */
		if (filter.len && !cstr_view_equal(filter, (struct cstr_view){ name, entry.name.len }))
			continue;
		char when[sizeof "2026-10-14 12:34 UTC"];
		struct tm tm;
		time_t t = (time_t)entry.time;
		size_t w = gmtime_r(&t, &tm) ? strftime(when, sizeof when, "%Y-%m-%d %H:%M UTC",
		                                        &tm) : 0;
		char const *subject = (char const *)entry.subject.ptr;
		size_t subject_len = history ? jb_utf8(subject, entry.subject.len) : 0;
		char const *asker = (char const *)entry.asker.ptr;
		size_t const asker_len = jb_utf8(asker, entry.asker.len);
		if (!dstr_printf(d, "%" PRIu32 ". %.*s  ", ++k, (int)w, when) ||
		    !dstr_cat_n(d, name, jb_utf8(name, entry.name.len)) ||
		    !(entry.added == NOTES_BINARY ? dstr_cat(d, "  (binary)")
		      : dstr_printf(d, "  (+%" PRIu64 " -%" PRIu64 ")", entry.added, entry.removed)) ||
		    (history && !dstr_printf(d, "  commit %s%s%.*s", entry.commit.hex,
		                            subject_len ? "  " : "",
		                            (int)subject_len, subject_len ? subject : "")) ||
		    (asker_len && !dstr_printf(d, ", asked by %.*s", (int)asker_len, asker)) ||
		    !dstr_cat_n(d, "\n", 1))
			return ENOMEM;
		*newest = entry.name;
	}
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	return 0;
}

/* Appends the current files in @p out, the output of git for STORE_FILES,
 * to call->text. @p cut is true if askai cut the output, which then ends
 * within a record. Returns 0, ENOMEM, or EIO if a record of an output that
 * askai did not cut cannot be decoded.
 */
static int
notes_tool_files (struct mcp_slot *slot,
                  struct span      out,
                  bool             cut)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	struct notes_file file;
	int r = 0;
	while (out.len && !(r = notes_file_next(&out, &file))) {
		char const *name = (char const *)file.name.ptr;
		if (!dstr_printf(call->text, "%.*s  (%" PRIu64 " bytes, %s)\n",
		                 (int)jb_utf8(name, file.name.len), name, file.bytes, file.id.hex))
			return ENOMEM;
	}
	if (!cut)
		return r ? EIO : 0;
	return cstr_view_cat(CSTR_VIEW("[The file list is incomplete.]\n"), call->text);
}

/* Handles the output of git for list and history. */
static void
notes_tool_got_list (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	bool const files = (call->fetch & FETCH_KIND) == FETCH_FILES;
	bool const cut = c->end.flags & CHILD_CUT;
	struct span const out = buf_peek(&c->out);
	struct span newest;
	int e = dstr_printf(call->text, "Notebook %s: %s:\n\n", notes_tool_shows(call),
	                    files ? "current notes (bytes, blob)"
	                          : "changes, oldest first (lines +added -removed, commit, "
	                            "message)")
	      ? 0 : ENOMEM;
	if (!e)
		e = files ? notes_tool_files(slot, out, cut)
		          : notes_tool_history(slot, out, cut, &newest);
	bool const bad = e == EIO;
	if (bad)
		e = cstr_view_cat(CSTR_VIEW("Cannot decode the notebook file list."), call->text);
	notes_tool_end(slot, e, bad);
}

/* Appends to call->text the members of the session, each with the newest
 * change of the notebook, and who the caller is. Returns 0 or ENOMEM.
 */
static int
notes_tool_members_text (struct notes_tool_call *call)
{
	struct member_table const *table = call->loop->members;
	struct member_session const *session = notes_tool_session(call);
	int const width = member_table_width(table);
	bool ok = dstr_printf(call->text, "Members of the session %.*s (name, kind, newest change "
	                      "of the notebook):\n", (int)session->name.len, session->name.data);
	struct member_info const *info;
	for (size_t i = 0; ok && (info = member_table_info(table, i)); ++i) {
		ok = dstr_printf(call->text, "%-*.*s  %-6s  ", width, (int)info->name.len,
		                 info->name.data, member_kinds[info->kind].data);
		char when[sizeof "2026-10-14 12:34 UTC"];
		struct tm tm;
		time_t const t = (time_t)info->time;
		if (!(info->flags & MEMBER_NOTES) || !gmtime_r(&t, &tm) ||
		    !strftime(when, sizeof when, "%Y-%m-%d %H:%M UTC", &tm)) {
			ok = ok && dstr_cat(call->text, "no notes yet\n");
			continue;
		}
		int const change = (int)jb_utf8(info->change.data, info->change.len);
		int const asker = (int)jb_utf8(info->asker.data, info->asker.len);
		ok = ok && dstr_printf(call->text, "%s  %.*s%s%.*s\n", when, change, info->change.data,
		                       asker ? ", asked by " : "", asker, info->asker.data);
	}
	struct member const *self = &call->self;
	char const *kind = member_kinds[self->kind].data;
	if (!mcp_slot_from_client(call->slot))
		ok = ok && dstr_printf(call->text, "You are the %s %s.", kind, self->name);
	else if (*self->name)
		ok = ok && dstr_printf(call->text, "You write as the %s %s.", kind, self->name);
	else
		ok = ok && dstr_cat(call->text, "You have no member name, so you cannot write notes.");
	ok = ok && dstr_cat(call->text, " To read another notebook, give the member name as "
	                    "notebook.\n");
	return ok ? 0 : ENOMEM;
}

/* Appends to call->out the JSON object of the structured content of the
 * members for a client: "members", each with its name and kind, and the
 * time and the subject of the newest change of its notebook, and "you",
 * the member of a caller with a member name. Stores the length of the
 * object in @p *json. Returns 0 or ENOMEM.
 */
static int
notes_tool_members_json (struct notes_tool_call *call,
                         size_t                 *json)
{
	struct json_object *list = json_object_new_array();
	struct member_info const *info;
	for (size_t i = 0; list && (info = member_table_info(call->loop->members, i)); ++i) {
		struct json_object *member = jb_object(
			"name", jb_string(info->name),
			"kind", json_object_new_string(member_kinds[info->kind].data),
			nullptr);
		char stamp[sizeof "2026-10-14T12:34:56Z"];
		struct tm tm;
		time_t const t = (time_t)info->time;
		if (info->flags & MEMBER_NOTES && gmtime_r(&t, &tm) &&
		    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tm)) {
			struct cstr_view const change = { info->change.data,
			                                  jb_utf8(info->change.data, info->change.len) };
			member = jb_set_or_free(member, "time", json_object_new_string(stamp));
			member = jb_set_or_free(member, "change", jb_string(change));
		}
		list = jb_push_or_free(list, member);
	}
	struct member const *self = &call->self;
	struct json_object *structured = jb_object("members", list, nullptr);
	if (*self->name)
		structured = jb_set_or_free(structured, "you", jb_object(
			"name", json_object_new_string(self->name),
			"kind", json_object_new_string(member_kinds[self->kind].data),
			nullptr));
	if (!structured)
		return ENOMEM;
	struct cstr_view const wire = jb_wire(structured);
	int e = wire.data ? cstr_view_cat(wire, &call->out) : ENOMEM;
	*json = e ? 0 : wire.len;
	json_object_put(structured);
	return e;
}

/* Finds the member that the notebook argument of @p call names in the
 * table of the members, as call->book, and writes the ref of the notebook
 * of the member to call->notebook. Returns 0, or ENOENT if the table names
 * no such member.
 */
static int
notes_tool_book (struct notes_tool_call *call)
{
	int const e = member_table_name(call->loop->members, cstr_view(call->book.name),
	                                &call->book);
	if (!e)
		member_ref(&call->book, MEMBER_BRANCH_NOTES, call->notebook);
	return e;
}

/* Goes on with the call in @p slot once the table of the members has read
 * the records (notes_tool_read_members()): with the notices of the prompt
 * of an ask call, with the notebook argument of a call, or with the result
 * of members. The prompt and the notebook argument go on with the table as
 * it is if the read failed. A notebook argument that the table names no
 * member of then is refused, with the reason of a read that failed, or
 * else with the names of the members.
 */
static void
notes_tool_got_members (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	call->flags &= ~CALL_READ;
	int e = call->wait.e;
	if (e == ENOMEM) {
		notes_tool_end(slot, ENOMEM, true);
		return;
	}
	if (e)
		LOG(LOG_WARN, "notes: cannot read the members of the session");
	if (call->then) {
		notes_tool_prompt_notices(slot);
		return;
	}
	/* The table writes the reason of a read that failed. */
	struct cstr_view const why = cstr_view(call->wait.why);
	if (call->action != NOTES_ACTION_MEMBERS) {
		if (!notes_tool_book(call))
			notes_tool_launch(slot);
		else if (why.len)
			mcp_slot_tell(slot, why, TOOL_FAILED);
		else
			notes_tool_end(slot, member_table_unknown(call->loop->members,
			                                          cstr_view(call->book.name),
			                                          call->text), true);
		return;
	}
	if (e) {
		mcp_slot_tell(slot, why.len ? why : CSTR_VIEW("The members could not be read."),
		              TOOL_FAILED);
		return;
	}
	/* The output is the text, then the JSON of a client. */
	size_t json = 0;
	e = notes_tool_members_text(call);
	if (!e && mcp_slot_from_client(slot))
		e = notes_tool_members_json(call, &json);
	if (e) {
		mcp_slot_fail(slot, e);
		return;
	}
	struct cstr_view const out = cstr_view_dstr(&call->out);
	mcp_slot_lend(slot, cstr_view_slice(out, 0, out.len - json));
	if (json)
		(void)mcp_slot_structured(slot, cstr_view_slice(out, out.len - json, SIZE_MAX));
	mcp_slot_finish(slot, 0);
}

/* Reads the records of the members for the call in @p slot
 * (member_table_read()), and then goes on with notes_tool_got_members().
 */
static void
notes_tool_read_members (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	int const e = member_table_read(call->loop->members, &call->wait);
	if (!notes_tool_park(call, e, CALL_READ))
		notes_tool_got_members(slot);
}

/* The brackets around a byte that the view of a note shows in hexadecimal. */
static char const view_open[] = "\xe2\x9f\xa6";   /* U+27E6 */
static char const view_close[] = "\xe2\x9f\xa7";  /* U+27E7 */

/* Appends the @p n bytes at @p p to @p d if @p *size, the length of the
 * view before them, is less than @p max. Adds @p n to @p *size in either
 * case. Returns 0 or ENOMEM.
 */
static int
notes_tool_put (struct dstr *d,
                size_t      *size,
                size_t       max,
                char const  *p,
                size_t       n)
{
	bool put = *size < max;
	*size += n;
	return put && !dstr_cat_n(d, p, n) ? ENOMEM : 0;
}

/* Appends to @p d the view of @p note. askai shows a note as its view if
 * the note is not UTF-8 text or contains a NUL byte. The view starts with
 * a line that explains it. Valid UTF-8 text stays as it is, and each ⟦ in
 * it becomes ⟦⟦. Each other byte, and each NUL, becomes ⟦XX⟧ with its
 * value in hexadecimal. So the view shows every byte of the note. Only the
 * start of the view goes into @p d, up to the first piece that ends at or
 * past @p max bytes. The length of the whole view goes into @p *size if
 * @p size is not nullptr. Returns 0 or ENOMEM. With @p max 0, the function
 * appends nothing and returns 0.
 */
static int
notes_tool_view (struct dstr      *d,
                 struct cstr_view  note,
                 size_t            max,
                 size_t           *size)
{
	static char const hex[] = "0123456789ABCDEF";
	size_t const open = sizeof view_open - 1;
	size_t const close = sizeof view_close - 1;
	char head[320];
	size_t w = 0;
	int e = notes_tool_put(d, &w, max, head, util_format(head, sizeof head,
	        "[askai: a view of %zu byte%s with NUL bytes or invalid UTF-8. Each NUL byte "
	        "and each byte of invalid UTF-8 shows as %sXX%s in hexadecimal. A %s in the "
	        "text shows as %s%s.]\n\n", note.len, util_s(note.len), view_open, view_close,
	        view_open, view_open, view_open));
	for (size_t i = 0; !e && i < note.len; ) {
		size_t run = jb_utf8(note.data + i, note.len - i);
		char const *nul = memchr(note.data + i, '\0', run);
		if (nul)
			run = (size_t)(nul - note.data) - i;
		/* Copy the valid text and double each ⟦ in it. */
		for (size_t at = i; !e && at < i + run; ) {
			char const *m = memmem(note.data + at, i + run - at, view_open, open);
			size_t n = m ? (size_t)(m - note.data) - at + open : i + run - at;
			e = notes_tool_put(d, &w, max, note.data + at, n);
			e = e || !m ? e : notes_tool_put(d, &w, max, view_open, open);
			at += n;
		}
		i += run;
		if (e || i == note.len)
			break;
		unsigned char b = (unsigned char)note.data[i++];
		char x[] = { hex[b >> 4], hex[b & 15] };
		e = notes_tool_put(d, &w, max, view_open, open);
		e = e ? e : notes_tool_put(d, &w, max, x, 2);
		e = e ? e : notes_tool_put(d, &w, max, view_close, close);
	}
	if (size)
		*size = w;
	return e;
}

/* Appends @p note to @p d as it is if it is UTF-8 text without NUL, else
 * its view (notes_tool_view()). Returns 0 or ENOMEM.
 */
static int
notes_tool_show (struct dstr      *d,
                 struct cstr_view  note)
{
	return jb_is_text(note) ? cstr_view_cat(note, d) : notes_tool_view(d, note, SIZE_MAX, nullptr);
}

static void notes_tool_got_copy (struct mcp_slot *slot, struct span text);
static void notes_tool_got_append (struct mcp_slot *slot, struct span old);
static void notes_tool_got_attach (struct mcp_slot *slot, struct span text,
                                   struct oid const *id);

/* Handles the output of git for one stage of notes_revision_next(). git
 * reads no note before every revision is resolved and checked against the
 * notebook. Then git reads the note at the revision, or writes the diff.
 * call->file, the pattern of search, stays for that last stage.
 */
static void
notes_tool_revision_step (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (!child_ok(call->child) ||
	    notes_revision_next(&call->revision, buf_peek(&call->child->out))) {
		/* git pins the tip of the notebook first. */
		if (oid_len(&call->revision.tip))
			mcp_slot_tell(slot, CSTR_VIEW("The commit in revision, from or to is not in "
			                              "the notebook history. Use a full commit ID "
			                              "from action history."), TOOL_FAILED);
		else
			notes_tool_unread(slot);
		return;
	}
	child_destroy(&call->child);
	struct notes_revision const *r = &call->revision;
	if (!notes_revision_ready(r)) {
		struct buf in = {};
		int const e = notes_revision_input(r, &in);
		notes_tool_run(slot, call->fetch, &in, e);
		return;
	}
	int e = 0;
	if ((call->fetch & FETCH_KIND) == FETCH_READ)
		e = notes_read_input(&call->file, r->from.hex, util_text_view(&r->file));
	notes_tool_run(slot, call->fetch, &call->file, e);
}

/* Handles the output of git for diff. */
static void
notes_tool_got_diff (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct notes_revision const *r = &call->revision;
	struct span out = buf_peek(&c->out);
	struct dstr *d = call->text;
	(void)mcp_slot_shows(slot, notes_tool_shows(call));
	int e = dstr_printf(d, "Notebook %s: %s\nfrom %s\nto %s\n\n", call->shows, r->file.data,
	                    r->from.hex, r->to.hex) ? 0 : ENOMEM;
	struct cstr_view const diff = { (char const *)out.ptr, out.len };
	if (!e)
		e = !diff.len ? cstr_view_cat(CSTR_VIEW("No differences.\n"), call->text)
		  : notes_tool_show(d, diff);
	if (!e && c->end.flags & CHILD_CUT)
		e = cstr_view_cat(CSTR_VIEW("\n[The diff is incomplete.]\n"), call->text);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	notes_tool_end(slot, e, false);
}

/* Handles the output of git grep for search. git starts each line with
 * the commit that it searched, the notebook's tip, and a colon, which the
 * result leaves out. With -z, git quotes no name and ends the name and the
 * line number with a NUL, and the result puts a colon there instead. Exit
 * status 1 means that no line matches, and 128 that git refused the
 * pattern.
 */
static void
notes_tool_got_grep (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	bool const cut = c->end.flags & CHILD_CUT;
	int const status = child_status(c);
	if (status != 0 && status != 1 && !cut) {
		mcp_slot_tell(slot, status == 128 ? CSTR_VIEW("A line of the pattern is not an "
		                                              "extended regular expression.")
		                                  : CSTR_VIEW("The notes could not be read."),
		              TOOL_FAILED);
		return;
	}
	size_t const skip = oid_len(&call->revision.from) + 1;
	struct span const out = buf_peek(&c->out);
	struct cstr_view rest = { (char const *)out.ptr, out.len }, line;
	dstr_auto found = DSTR_INIT;
	int e = 0;
	while (!e && cstr_view_cut(&rest, '\n', &line)) {
		struct cstr_view text = cstr_view_slice(line, skip, SIZE_MAX), name, number;
		if (!cstr_view_cut(&text, '\0', &name) || !cstr_view_cut(&text, '\0', &number))
			continue;
		e = cstr_view_join((struct cstr_view const[]){ name, number, text }, 3, UINT64_MAX,
		                   CSTR_VIEW(":"), CSTR_VIEW(":"), &found) ||
		    !dstr_cat_n(&found, "\n", 1) ? ENOMEM : 0;
	}
	struct cstr_view const lines = cstr_view_dstr(&found);
	if (!e)
		e = dstr_printf(call->text, "Notebook %s: lines that match the pattern "
		                "(file:line:text):\n\n", notes_tool_shows(call)) ? 0 : ENOMEM;
	if (!e)
		e = lines.len ? notes_tool_show(call->text, lines)
		  : cstr_view_cat(CSTR_VIEW("No line of the current notes matches the "
		                            "pattern.\n"), call->text);
	if (!e && cut)
		e = cstr_view_cat(CSTR_VIEW("[The search output is incomplete.]\n"), call->text);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len + lines.len);
	notes_tool_end(slot, e, false);
}

/* Handles the output of git cat-file. The function answers read, or goes
 * on with copy, restore, append or attach.
 */
static void
notes_tool_got_note (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct span text = {};
	struct oid id;
	int r = child_ok(c) ? notes_read_out(buf_peek(&c->out), &text, &id) : EIO;
	if (r == EIO) {
		notes_tool_unread(slot);
		return;
	}
	if (r) {
		mcp_slot_tell(slot, r == ENOENT ? no_note : dir_name, TOOL_FAILED);
		return;
	}
	switch (call->fetch & (FETCH_COPY | FETCH_RESTORE | FETCH_APPEND | FETCH_ATTACH)) {
	case FETCH_COPY:
	case FETCH_RESTORE:
		notes_tool_got_copy(slot, text);
		return;
	case FETCH_APPEND:
		notes_tool_got_append(slot, text);
		return;
	case FETCH_ATTACH:
		notes_tool_got_attach(slot, text, &id);
		return;
	}
	struct cstr_view const body = { (char const *)text.ptr, text.len };
	(void)mcp_slot_shows(slot, notes_tool_shows(call));
	int e = 0;
	if (call->fetch & FETCH_REVISION)
		e = dstr_printf(call->text, "Notebook %s: %s at %s:\n\n", call->shows,
		                call->revision.file.data, call->revision.from.hex) ? 0 : ENOMEM;
	if (!e)
		e = notes_tool_show(call->text, body);
	notes_tool_end(slot, e, false);
}

/* Appends to call->text that the note of @p len bytes follows the results
 * of the turn as a file of @p bytes bytes. If @p view is true, the file is
 * the view of the note. Returns 0 or ENOMEM.
 */
static int
notes_tool_call_attached (struct notes_tool_call const *call,
                          size_t                        len,
                          size_t                        bytes,
                          bool                          view)
{
	bool ok = view ? dstr_printf(call->text, "The note is %zu byte%s with NUL bytes or "
	                             "invalid UTF-8. askai attaches a view of the note as a "
	                             "file after the results of the turn. The view is %zu "
	                             "bytes, about %zu tokens.", len, util_s(len), bytes,
	                             bytes / 4)
	               : dstr_printf(call->text, "The note is %zu byte%s, about %zu token%s. "
	                             "askai attaches the note as a file after the results of "
	                             "the turn.", len, util_s(len), bytes / 4,
	                             util_s(bytes / 4));
	return ok ? 0 : ENOMEM;
}

/* Attaches @p text, the note with the object id @p id, to the turn. If the
 * upstream still stores the file that askai uploaded for this note and
 * owner, askai attaches that file. Otherwise askai uploads the note, or its
 * view if the note is not UTF-8 text. The file must fit in the context.
 */
static void
notes_tool_got_attach (struct mcp_slot  *slot,
                       struct span       text,
                       struct oid const *id)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct cstr_view body = { (char const *)text.ptr, text.len };
	bool binary = !jb_is_text(body);
	size_t kept = 0;
	int e = mcp_slot_attach(slot, id, (struct cstr_view){}, &kept, call->text);
	if (e != ENOENT) {
		/* The upstream still stores the file of the note. */
		if (!e)
			e = notes_tool_call_attached(call, text.len, kept, binary);
		notes_tool_end(slot, e == EFBIG ? 0 : e, e == EFBIG);
		return;
	}
	dstr_auto view = DSTR_INIT;
	if (binary) {
		/* Measure the view before making it, so that askai makes no view
		 * that is longer than STORE_MAX. */
		size_t len = 0;
		(void)notes_tool_view(&view, body, 0, &len);
		if (len > STORE_MAX) {
			mcp_slot_tellf(slot, TOOL_FAILED, "The note is too large to attach. The view "
			               "of the note would be %zu bytes. The upload limit is %d MiB. "
			               "read shows the note in parts.", len, STORE_MAX >> 20);
			return;
		}
		if (notes_tool_view(&view, body, SIZE_MAX, nullptr)) {
			notes_tool_end(slot, ENOMEM, true);
			return;
		}
		body = cstr_view_dstr(&view);
	}
	call->upload = body.len;
	call->note = text.len;
	call->id = *id;
	e = mcp_slot_attach(slot, id, body, &kept, call->text);
	if (e == EINPROGRESS) {
		child_destroy(&call->child);
		mcp_slot_park(slot);
		return;
	}
	notes_tool_end(slot, e == EFBIG || e == EAGAIN ? 0 : e, true);
}

/* Handles the answer to the upload that attach sent. The function
 * attaches the file to the turn, or appends the reason why the upload
 * failed to call->text.
 */
static void
notes_tool_uploaded (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	int e = mcp_slot_attached(slot, &call->id, call->upload, call->text);
	if (e == EINPROGRESS) {
		mcp_slot_park(slot);
		return;
	}
	/* A view is longer than its note, so the sizes differ only for a view. */
	if (!e)
		e = notes_tool_call_attached(call, call->note, call->upload,
		                             call->upload != call->note);
	notes_tool_end(slot, e == EIO ? 0 : e, e == EIO);
}

/* Appends @p head and the list of the notes in the output of git to
 * call->text, the text that askai adds to the prompt. Then the function
 * runs git to read the newest note. If there are no notes, the ask call
 * continues.
 */
static void
notes_tool_prompt_list (struct mcp_slot  *slot,
                        struct cstr_view  head)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct span newest = {};
	struct buf in = {};
	int e = cstr_view_cat(head, call->text);
	if (!e)
		e = notes_tool_history(slot, buf_peek(&c->out), c->end.flags & CHILD_CUT, &newest);
	struct cstr_view const name = { (char const *)newest.ptr, newest.len };
	/* The name comes from git. git reads the note only if add would accept
	 * the name as it is. */
	struct cstr_view checked = name;
	bool named = !notes_name(&checked) && checked.len == name.len;
	if (!e && named) {
		struct cstr_view const p[] = {
			CSTR_VIEW("[askai: The current text of your newest note, "),
			{ name.data, jb_utf8(name.data, name.len) },
			CSTR_VIEW(":]\n"),
		};
		e = cstr_view_join(p, array_size(p), UINT64_MAX, CSTR_VIEW(""), CSTR_VIEW(""),
		                   call->text);
		e = e ? e : notes_read_input(&in, call->repo, name);
	} else if (!e && name.len) {
		e = cstr_view_cat(CSTR_VIEW("[askai: The newest note could not be read.]\n"),
		                  call->text);
	}
	child_destroy(&call->child);
	if (e || !named) {
		buf_fini(&in);
		notes_tool_then(slot, e);
		return;
	}
	notes_tool_run(slot, FETCH_NEWEST, &in, 0);
}

/* Appends the newest note, which git read for the prompt, to call->text.
 * If git could not read the note, the function appends the line "[askai:
 * the note could not be read.]" instead, or "[askai: the note was
 * deleted.]" if the note does not exist. A note longer than call->most
 * bytes is cut there, and the function appends a line with the offset of
 * the rest. Then the ask call continues.
 */
static void
notes_tool_newest (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct span text = {};
	int r = child_ok(c) ? notes_read_out(buf_peek(&c->out), &text, nullptr) : EIO;
	struct cstr_view body = { (char const *)text.ptr, text.len };
	size_t len = text.len;
	/* A note that is not UTF-8 text goes into the prompt as its view. The
	 * function makes only the start of the view, a little more than the
	 * call->most bytes that the prompt shows. */
	dstr_auto view = DSTR_INIT;
	if (!r && !jb_is_text(body)) {
		if (notes_tool_view(&view, body, call->most + 1, &len)) {
			notes_tool_then(slot, ENOMEM);
			return;
		}
		body = cstr_view_dstr(&view);
	}
	body = cstr_view_slice(body, 0, util_clip(body.data, body.len, call->most));
	char buf[128];
	struct cstr_view const more = body.len < len ? util_format_view(buf, sizeof buf,
	                              "\n[askai: bytes 0 to %zu of %zu. Read the note with "
	                              "\"offset\": %zu for the remaining bytes.]", body.len, len,
	                              body.len)
	                            : (struct cstr_view){};
	struct cstr_view const p[] = {
		r == ENOENT ? CSTR_VIEW("[askai: The note was deleted.]")
		: r ? CSTR_VIEW("[askai: The note could not be read.]") : body,
		more,
		CSTR_VIEW("\n"),
	};
	bool newline = body.len && body.data[body.len - 1] == '\n';
	notes_tool_then(slot, cstr_view_join(p, newline && !more.len ? 1 : array_size(p),
	                                     UINT64_MAX, CSTR_VIEW(""), CSTR_VIEW(""), call->text));
}

/* Handles the list of the notes of the agent for the prompt of an ask
 * call, as it starts or after a compaction. The history goes into the
 * prompt. A notebook without notes goes in as a notice that says so. If
 * git failed otherwise, the start of an ask call ends as failed, and a
 * compaction goes on with a notice that says so.
 */
static void
notes_tool_resumed (struct mcp_slot *slot)
{
	static char const history[] = "[askai: Your notebook is %s. Your note history, oldest "
	                              "first. A repeated name is an update:]\n";
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	char head[sizeof history + MEMBER_NAME];
	if (notes_tool_listed(c)) {
		(void)mcp_slot_noted(slot);
		notes_tool_prompt_list(slot, util_format_view(head, sizeof head, history,
		                                              call->self.name));
	} else if (c && notes_tool_none(c)) {
		notes_tool_then(slot, dstr_printf(call->text, "[askai: Your notebook %s contains "
		                                  "no notes yet.]\n", call->self.name) ? 0 : ENOMEM);
	} else if ((call->fetch & FETCH_KIND) == FETCH_COMPACT) {
		notes_tool_then(slot, cstr_view_cat(CSTR_VIEW("[askai: Your notes could not be "
		                                              "read.]\n"), call->text));
	} else {
		notes_tool_drop(slot);
		mcp_slot_tell(slot, CSTR_VIEW("Cannot read the session notes."), TOOL_FAILED);
	}
}

static struct notes_tool_fetch_step const notes_tool_fetch_steps[] = {
	[FETCH_CHECK]    = { notes_tool_checked,     STORE_CHECK,  0 },
	[FETCH_ADD]      = { notes_tool_added,       STORE_COMMIT, 0 },
	[FETCH_LIST]     = { notes_tool_got_list,    STORE_LOG,    1 },
	[FETCH_READ]     = { notes_tool_got_note,    STORE_READ,   0 },
	[FETCH_RESUME]   = { notes_tool_resumed,     STORE_LOG,    0 },
	[FETCH_COMPACT]  = { notes_tool_resumed,     STORE_LOG,    0 },
	[FETCH_FILES]    = { notes_tool_got_list,    STORE_FILES,  1 },
	[FETCH_NEWEST]   = { notes_tool_newest,      STORE_READ,   0 },
	[FETCH_DIFF]     = { notes_tool_got_diff,    STORE_DIFF,   1 },
	[FETCH_STAT]     = { notes_tool_stated,      STORE_STAT,   0 },
	[FETCH_GREP]     = { notes_tool_got_grep,    STORE_GREP,   0 },
	[FETCH_REBASE]   = { notes_tool_rebased,     STORE_CHECK,  0 },
};
static_assert(array_size(notes_tool_fetch_steps) <= FETCH_KIND + 1);

/* Returns the row of notes_tool_fetch_steps[] for the kind of @p fetch. */
static struct notes_tool_fetch_step const *
notes_tool_fetch_step (uint32_t fetch)
{
	return &notes_tool_fetch_steps[fetch & FETCH_KIND];
}

void
notes_tool_step (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	/* The lookup of a login, the read of the records of the members and
	 * attach reach this step without a child. For attach, the step
	 * handles the answer to the upload. */
	if (!call->child) {
		switch (call->flags & (CALL_LOGIN | CALL_READ)) {
		case CALL_LOGIN:
			notes_tool_logged_in(slot);
			return;
		case CALL_READ:
			notes_tool_got_members(slot);
			return;
		default:
			notes_tool_uploaded(slot);
			return;
		}
	}
	if (call->fetch & FETCH_REVISION && !notes_revision_ready(&call->revision)) {
		notes_tool_revision_step(slot);
		return;
	}
	struct notes_tool_fetch_step const *row = notes_tool_fetch_step(call->fetch);
	if (row->listed && !notes_tool_listed(call->child))
		notes_tool_unread(slot);
	else
		row->out(slot);
}

static_assert(TOOL_TEXT_MAX == 4U << 20 && NOTES_NAME == 255 && HEXID_DIGITS == 32,
              "update the texts in this file that say 4 MiB, 255 bytes or 32 hexadecimal "
              "digits");

/* Runs call->first, the first git run of the call in @p slot, once the
 * caller and the notebook are known. For a check of a write and for a
 * read without a revision, the input of git names the note
 * call->revision.file.
 */
static void
notes_tool_launch (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct cstr_view const name = util_text_view(&call->revision.file);
	struct buf in = {};
	int e = 0;
	if ((call->first & FETCH_KIND) == FETCH_CHECK)
		e = store_check_input(&in, call->repo, name);
	else if ((call->first & (FETCH_KIND | FETCH_REVISION)) == FETCH_READ)
		e = notes_read_input(&in, notes_tool_call_notebook(call), name);
	notes_tool_run(slot, call->first, &in, e);
}

/* Finds the notebook of the call in @p slot once the caller is known, and
 * runs the first git of the call. A call that writes a note as the caller
 * (NOTE_AUTHOR) needs a caller with a member name, and a write writes only
 * the notebook of the caller. The notebook argument names a member of the
 * table. For a name without a member in the table, and for members, the
 * table reads the records first (notes_tool_read_members()).
 */
static void
notes_tool_resolve (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	uint32_t const flags = notes_tool_actions[call->action].flags;
	bool const members = call->action == NOTES_ACTION_MEMBERS;
	bool const own = !*call->book.name || !strcmp(call->book.name, call->self.name);
	if (!*call->self.name) {
		/* The refusal of a login by a record comes with a reason. */
		struct cstr_view const why = cstr_view(call->wait.why);
		if (flags & NOTE_AUTHOR) {
			mcp_slot_tell(slot, why.len ? why
			              : CSTR_VIEW("Writing notes needs a member name. Connect through "
			                          "the Unix socket, or with the token of an "
			                          "identity."), TOOL_FAILED);
			return;
		}
		if (own && !members) {
			mcp_slot_tell(slot, why.len ? why
			              : CSTR_VIEW("You have no notebook, because you have no member "
			                          "name. Give a name as notebook. Action members "
			                          "lists the names."), TOOL_FAILED);
			return;
		}
	} else {
		member_ref(&call->self, MEMBER_BRANCH_NOTES, call->repo);
		if (flags & NOTE_WRITE && !own) {
			mcp_slot_tellf(slot, TOOL_FAILED, "You can write only your notebook, %s.",
			               call->self.name);
			return;
		}
	}
	if (members) {
		notes_tool_read_members(slot);
	} else if (own) {
		call->book = call->self;
		notes_tool_launch(slot);
	} else if (notes_tool_book(call)) {
		notes_tool_read_members(slot);
	} else {
		notes_tool_launch(slot);
	}
}

/* Goes on with the call in @p slot once the lookup of the member of the
 * login of its client has ended. A login without a member (ENOENT) and a
 * login that a record refuses (EPERM) go on without a member name. Any
 * other error of the lookup, such as EAGAIN for a busy askai, ends the
 * call with the reason. For the prompt of an ask call, the member is then
 * the asker, and the table reads the records of the members. A call of the
 * notes tool finds its notebook.
 */
static void
notes_tool_logged_in (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	call->flags &= ~CALL_LOGIN;
	int const e = call->wait.e;
	if (e && e != ENOENT && e != EPERM) {
		char why[MEMBER_WHY];
		memcpy(why, call->wait.why, sizeof why);
		notes_tool_drop(slot);
		if (*why)
			mcp_slot_tell(slot, cstr_view(why), TOOL_FAILED);
		else
			mcp_slot_fail(slot, e);
		return;
	}
	if (call->then) {
		(void)mcp_slot_asker(slot, &call->wait.member);
		notes_tool_read_members(slot);
		return;
	}
	if (!e)
		call->self = call->wait.member;
	notes_tool_resolve(slot);
}

/* Stages @p fetch as the first git run of the call in @p slot. The caller
 * of a turn is the agent of askai. For a client, the function first finds
 * the member of its login. A call that writes a note as the caller
 * (NOTE_AUTHOR) claims the member, which makes the record of a new member
 * (member_table_claim()). Any other call only finds the member
 * (member_table_find()). If @p e is not 0, the function ends the call with
 * @p e instead.
 */
static void
notes_tool_stage (struct mcp_slot *slot,
                  uint32_t         fetch,
                  int              e)
{
	if (e) {
		notes_tool_end(slot, e, true);
		return;
	}
	struct notes_tool_call *call = *mcp_slot_own(slot);
	call->first = fetch;
	if (!mcp_slot_from_client(slot)) {
		notes_tool_agent(call);
		notes_tool_resolve(slot);
		return;
	}
	struct member_login const *login = &mcp_slot_member(slot)->login;
	if (notes_tool_actions[call->action].flags & NOTE_AUTHOR)
		e = member_table_claim(call->loop->members, login, &call->wait);
	else
		e = member_table_find(call->loop->members, login, &call->wait);
	if (!notes_tool_park(call, e, CALL_LOGIN))
		notes_tool_logged_in(slot);
}

/* Starts add, append or replace of the note @p name with the arguments
 * @p args. For append, the file part contains the text to append. The note
 * goes to the caller's own notebook. A turn's text is cut before its
 * trailing notices, because askai adds those notices only to the results
 * of a turn's calls. A turn's text with the markup of a later tool call is
 * refused, and nothing is written (notice_text_check()).
 */
static void
notes_tool_add (struct mcp_slot    *slot,
                struct cstr_view    name,
                struct json_object *args)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	bool turn = !mcp_slot_from_client(slot);
	size_t most = call->loop->tool->files_inline;
	struct json_object *content = jb_get(args, "content");
	struct cstr_view const text = jb_view(jb_get(args, "text"));
	size_t kept = text.len;
	struct datauri data = {};
	char why[256];
	struct cstr_view problem = {};
	if (notes_name(&name)) {
		problem = bad_name;
	} else if (!content) {
		if (notice_text_check(text, "text", turn ? &kept : nullptr, why, sizeof why))
			problem = cstr_view(why);
	} else if (turn) {
		problem = CSTR_VIEW("Only a client can give content. Give text instead.");
	} else if (text.data) {
		problem = CSTR_VIEW("Give text or content, not both.");
	} else if (datauri_init(&data, jb_view(content))) {
		problem = CSTR_VIEW("The content argument must be a data: URI. Your client fills in "
		                    "content with a file from the user.");
	} else if (data.size > most) {
		problem = util_format_view(why, sizeof why, "The content file is larger than the "
		                           "limit of %zu bytes.", most);
	} else if (!data.size && notes_tool_actions[call->action].fetch & FETCH_APPEND) {
		problem = CSTR_VIEW("The content file to append is empty.");
	}
	if (problem.data) {
		mcp_slot_tell(slot, problem, TOOL_FAILED);
		return;
	}
	(void)util_text_store(&call->revision.file, name);
	int e;
	if (content) {
		e = store_part_head(&call->part, &call->file, name, data.size);
		if (!e)
			datauri_decode(&data, buf_peek(&call->file).ptr + call->part.data_at);
		loop_work(mcp_slot_loop(slot), LOOP_COPY, data.size);
	} else {
		/* askai adds a line feed to a turn's text that lacks a final
		 * one, if the note stays within 4 MiB. The byte after the kept
		 * text is that line feed, or the one that store_part_head()
		 * writes after the text. */
		size_t const lf = turn && kept < TOOL_TEXT_MAX && text.data[kept - 1] != '\n';
		call->flags = kept < text.len ? CALL_STRIPPED : 0;
		e = store_part_head(&call->part, &call->file, name, kept + lf);
		if (!e)
			*(char *)mempcpy(buf_peek(&call->file).ptr + call->part.data_at, text.data,
			                 kept) = '\n';
	}
	notes_tool_stage(slot, FETCH_CHECK, e);
}

/* Writes @p text to the note call->name in the caller's own notebook. A
 * restore fails if @p text is longer than TOOL_TEXT_MAX bytes. The result
 * of a copy begins with the note and the notebook that it copies.
 */
static void
notes_tool_got_copy (struct mcp_slot *slot,
                     struct span      text)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (call->fetch & FETCH_RESTORE && text.len > TOOL_TEXT_MAX) {
		mcp_slot_tell(slot, CSTR_VIEW("The note at revision is larger than the 4 MiB "
		                              "limit. Nothing was restored."), TOOL_FAILED);
		return;
	}
	struct cstr_view const to = util_text_view(&call->name);
	struct cstr_view const bytes = { (char const *)text.ptr, text.len };
	int e = store_part_file(&call->part, &call->file, to, bytes);
	if (!e && call->fetch & FETCH_COPY)
		e = dstr_printf(call->text, "Copied note %s of notebook %s at %s into your notebook "
		                "as %s. ", call->revision.file.data, call->book.name,
		                call->revision.from.hex, to.data) ? 0 : ENOMEM;
	loop_work(mcp_slot_loop(slot), LOOP_COPY, text.len);
	notes_tool_check(slot, FETCH_CHECK, call->repo, e);
}

/* Starts delete of the note @p name in the caller's own notebook. */
static void
notes_tool_delete (struct mcp_slot  *slot,
                   struct cstr_view  name)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (notes_name(&name)) {
		mcp_slot_tell(slot, bad_name, TOOL_FAILED);
		return;
	}
	(void)util_text_store(&call->revision.file, name);
	/* The size of the note after a delete is 0 bytes, as the commit
	 * message prints it. */
	call->part = (struct store_part){};
	notes_tool_stage(slot, FETCH_CHECK, store_part_delete(&call->file, name));
}

/* Writes the note @p old, which git read at call->revision.tip, and the
 * text in call->file after it, on a new line, as the note
 * call->revision.file in a new commit with that parent.
 */
static void
notes_tool_got_append (struct mcp_slot *slot,
                       struct span      old)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct span const add = { buf_peek(&call->file).ptr + call->part.data_at,
	                          call->part.data_len };
	size_t const lf = old.len && old.ptr[old.len - 1] != '\n';
	if (old.len + lf + add.len > TOOL_TEXT_MAX) {
		mcp_slot_tell(slot, CSTR_VIEW("The note would be larger than the 4 MiB limit. "
		                              "The text was not appended."), TOOL_FAILED);
		return;
	}
	struct buf file = {};
	struct store_part part;
	int e = store_part_head(&part, &file, util_text_view(&call->revision.file),
	                        old.len + lf + add.len);
	if (e) {
		buf_fini(&file);
		notes_tool_end(slot, e, true);
		return;
	}
	unsigned char *p = mempcpy(buf_peek(&file).ptr + part.data_at, old.ptr, old.len);
	if (lf)
		*p++ = '\n';
	memcpy(p, add.ptr, add.len);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, old.len + add.len);
	child_destroy(&call->child);
	buf_fini(&call->file);
	call->file = file;
	call->part = part;
	notes_tool_write(slot, notes_tool_call_message(call, old.len));
}

/* Starts search of the notebook of the call in @p slot. git grep -E -f -
 * takes @p pattern on its input: extended regular expressions, one on each
 * line. git pins the notebook's tip first, and then searches the notes at
 * that commit.
 */
static void
notes_tool_search (struct mcp_slot  *slot,
                   struct cstr_view  pattern)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (!pattern.len || memchr(pattern.data, '\0', pattern.len)) {
		mcp_slot_tell(slot, CSTR_VIEW("The pattern argument must be 1 or more lines "
		                              "without NUL bytes. Each line is an extended "
		                              "regular expression."), TOOL_FAILED);
		return;
	}
	notes_tool_stage(slot, FETCH_GREP | FETCH_REVISION,
	                 buf_put(&call->file, pattern.data, pattern.len, SIZE_MAX));
}

/* Starts the read of the note @p name. @p fetch is the fetch of read or
 * attach.
 */
static void
notes_tool_read (struct mcp_slot  *slot,
                 struct cstr_view  name,
                 uint32_t          fetch)
{
	if (notes_name(&name)) {
		mcp_slot_tell(slot, bad_name, TOOL_FAILED);
		return;
	}
	struct notes_tool_call *call = *mcp_slot_own(slot);
	(void)util_text_store(&call->revision.file, name);
	notes_tool_stage(slot, fetch, 0);
}

/* Starts diff, restore, copy, read with a revision, or history with a
 * file. The note is @p file. @p revisions are the values of the arguments
 * from and to for diff, or of the argument revision and nullptr otherwise.
 * For copy, notes_tool_got_copy() then writes the note byte for byte to
 * the caller's own notebook, and its text does not pass through the
 * caller.
 */
static void
notes_tool_revision (struct mcp_slot          *slot,
                     struct cstr_view          file,
                     struct json_object *const revisions[2],
                     uint32_t                  fetch)
{
	struct oid ids[2] = {};
	bool bad = !revisions[0] && ((fetch & FETCH_KIND) == FETCH_DIFF || fetch & FETCH_RESTORE);
	for (size_t i = 0; !bad && i < array_size(ids); ++i) {
		struct cstr_view const v = jb_view(revisions[i]);
		bad = revisions[i] && oid_init(&ids[i], v) &&
		      !cstr_view_equal(v, CSTR_VIEW("current"));
	}
	if (bad) {
		mcp_slot_tell(slot, CSTR_VIEW("revision, from and to must be a full commit ID "
		                              "from action history, or current. diff needs "
		                              "from. restore needs revision."), TOOL_FAILED);
		return;
	}
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (notes_revision_init(&call->revision, file, &ids[0], &ids[1])) {
		mcp_slot_tell(slot, bad_name, TOOL_FAILED);
		return;
	}
	if ((fetch & FETCH_KIND) != FETCH_LIST)
		fetch |= FETCH_REVISION;
	notes_tool_stage(slot, fetch, 0);
}

/* Starts a call of the notes tool in @p slot with the arguments @p args.
 * This is the start() of the notes kind. The function checks the
 * arguments and copies the parts of the arguments that the call needs,
 * because a client's arguments can be gone once the call waits. Then the
 * call finds the caller and the notebook (notes_tool_stage()). Every write
 * goes to call->repo, the notebook of the caller.
 */
static void
notes_tool_start (struct mcp_slot    *slot,
                  struct json_object *args)
{
	struct notes_tool_call *call = notes_tool_loop_take(mcp_slot_part(slot), slot, nullptr);
	if (!call)
		return;
	size_t const offered = mcp_slot_from_client(slot) ? NOTES_ACTION_ATTACH
	                                                  : call->loop->tool->actions;
	size_t const which = cstr_view_index(notes_action_names, offered,
	                                     jb_view(jb_get(args, "action")));
	if (which == offered) {
		mcp_slot_tellf(slot, TOOL_FAILED, "The action argument must be one of: "
		               XL_JOIN(", ", ", ", NOTES_COMMON_ACTION_LIST(XL_TEXT)) "%s.",
		               offered > NOTES_ACTION_ATTACH ? ", attach" : "");
		return;
	}
	call->action = which;
	uint32_t flags = notes_tool_actions[which].flags;
	uint32_t fetch = notes_tool_actions[which].fetch;
	bool diff = fetch == FETCH_DIFF;
	struct json_object *file = jb_get(args, "file");
	if ((flags & NOTE_FILE) && !file) {
		mcp_slot_tell(slot, CSTR_VIEW("The file argument is missing. Give the name of a "
		                              "note, such as plan.md."), TOOL_FAILED);
		return;
	}
	struct json_object *revision = jb_get(args, "revision");
	struct json_object *from = jb_get(args, "from");
	struct json_object *to = jb_get(args, "to");
	if ((revision && !(flags & NOTE_REVISION)) || ((from || to) && !diff)) {
		mcp_slot_tell(slot, CSTR_VIEW("Give revision only with read or restore. Give from "
		                              "and to only with diff."), TOOL_FAILED);
		return;
	}
	/* The notebook argument is the name of a member. members lists them
	 * all, without the notebook argument. */
	struct json_object *notebook = which == NOTES_ACTION_MEMBERS ? nullptr
	                                                             : jb_get(args, "notebook");
	struct cstr_view const selected = jb_view(notebook);
	if (notebook && !selected.len) {
		mcp_slot_tell(slot, CSTR_VIEW("The notebook argument must be the name of a member. "
		                              "Action members lists the names."), TOOL_FAILED);
		return;
	}
	if (notebook && !member_name_valid(selected)) {
		notes_tool_end(slot, member_table_unknown(call->loop->members, selected, call->text),
		               true);
		return;
	}
	if (!notebook && fetch & FETCH_COPY) {
		mcp_slot_tell(slot, CSTR_VIEW("copy needs notebook: the name of the member whose "
		                              "note you copy. Action members lists the names."),
		              TOOL_FAILED);
		return;
	}
	util_format(call->book.name, sizeof call->book.name, "%.*s", (int)selected.len,
	            notebook ? selected.data : "");
	if ((flags & NOTE_OFFSET) && mcp_slot_offset(slot, args))
		return;
	struct cstr_view const name = jb_view(file);
	if (fetch & (FETCH_COPY | FETCH_RESTORE)) {
		/* The name of the copy or restore is the as argument, or else
		 * file. */
		struct json_object *as = jb_get(args, "as");
		struct cstr_view target = as ? jb_view(as) : name;
		if (notes_name(&target)) {
			mcp_slot_tell(slot, bad_name, TOOL_FAILED);
			return;
		}
		(void)util_text_store(&call->name, target);
		call->flags = as ? CALL_AS : 0;
		notes_tool_revision(slot, name, (struct json_object *const[]){ revision, nullptr },
		                    fetch);
		return;
	}
	if (revision || diff || (fetch == FETCH_LIST && file)) {
		struct json_object *const revisions[] = { diff ? from : revision, to };
		notes_tool_revision(slot, name, revisions, fetch);
		return;
	}
	if (fetch & FETCH_DELETE)
		notes_tool_delete(slot, name);
	else if (flags & NOTE_WRITE)
		notes_tool_add(slot, name, args);
	else if (flags & NOTE_FILE)
		notes_tool_read(slot, name, fetch);
	else if (fetch == FETCH_GREP)
		notes_tool_search(slot, jb_view(jb_get(args, "pattern")));
	else
		notes_tool_stage(slot, fetch, 0);
}

/* Returns the definition of the notes tool for a turn if @p turn is true,
 * or else for clients. Only the definition for clients contains the
 * content argument. The definition for a turn contains only the first
 * tool->actions entries of notes_action_names in the enum of the action
 * argument.
 */
static struct json_object *
notes_tool_schema (struct notes_tool const *tool,
                   bool                     turn)
{
	struct member_session const *session = member_registry_session(tool->members);
	char const *agent = session ? session->agent : "";
	dstr_auto notebook = DSTR_INIT;
	if (!dstr_printf(&notebook, "The notebook to read: the name of a member, such as %s. "
	                 "Action members lists the names. The default is your notebook. copy "
	                 "needs notebook.", agent))
		return nullptr;
	size_t const offered = turn ? tool->actions : NOTES_ACTION_ATTACH;
	struct json_object *actions = schema_names(notes_action_names, offered);
	struct json_object *props = jb_object(
		"action", jb_set_or_free(schema_enum(actions, nullptr), "description", schema_text3(
			"add creates the note named file with text. append adds text to the end of "
			"the note, on a new line. replace replaces the note with text. delete "
			"deletes the note. restore writes the note at the commit revision in a new "
			"commit. copy copies the note from notebook to your notebook. list lists "
			"the current notes. history lists the changes, oldest first. read reads "
			"the note, or the note at the commit revision. diff shows the changes to "
			"the note between the commits from and to. search finds the lines of the "
			"current notes that match pattern. Note names are not searched. members "
			"lists the members of this session and the newest change of each "
			"notebook.",
			offered > NOTES_ACTION_ATTACH ? " attach sends you the whole note as a "
			                                "file after the results of the turn." : "",
			" To recover lost text: history with file, then diff, then read or restore "
			"with revision.")),
		"file", schema_property("string",
			"The note name, 1 to 255 bytes, such as plan.md or design/locks.md. Pick "
			"an extension that matches the format of the text. With file, history "
			"lists only the changes to the note."),
		"revision", schema_property("string",
			"A full commit ID from the notebook history, or current. The default for "
			"read is current. restore needs revision."),
		"from", schema_property("string",
			"The older commit: a full commit ID from the notebook history, or "
			"current. diff needs from."),
		"to", schema_property("string",
			"The newer commit: a full commit ID, or current. The default is current. "
			"With offset, give the commit IDs from the diff output as from and to."),
		"as", schema_property("string",
			"The name for the new note of copy or restore, instead of file."),
		"text", schema_property("string",
			"The text for add, append or replace, 1 byte to 4 MiB."),
		"pattern", schema_property("string",
			"Each line of pattern is an extended regular expression. Case is ignored. "
			"A line of a note matches if any expression matches."),
		"offset", schema_count(0, TOOL_OFFSET_ABOUT),
		"notebook", schema_property("string", dstr_get(&notebook)),
		nullptr);
	if (!turn) {
		/* The client program, not the model, fills in content with a
		 * file (the extension io.modelcontextprotocol/files). */
		props = jb_set_or_free(props, "content", jb_object(
			"type", json_object_new_string("string"),
			"format", json_object_new_string("uri"),
			"description", json_object_new_string(
				"A file from the user for add, append or replace, instead of "
				"text. Your client fills in content. Do not write content "
				"yourself."),
			"x-mcp-file", jb_object("maxSize",
			                        json_object_new_int64((int64_t)tool->files_inline),
			                        nullptr),
			nullptr));
	}
	dstr_auto description = DSTR_INIT;
	bool ok = dstr_cat(&description, "Notes are files in notebooks. Each member of this "
	                   "session has one notebook. The notebook has the name of the member. A "
	                   "member is a person or an agent. ");
	ok = ok && (turn ? dstr_printf(&description, "You are the agent %s. Your notebook is %s. "
	                               "You can read every notebook. You can write only your "
	                               "notebook. Every version of a note stays in the history. "
	                               "Each ask call and each compaction starts with your note "
	                               "history and your newest note.", agent, agent)
	                 : dstr_cat(&description, "Your calls write to the notebook of the member "
	                            "that your login or your token names. Action members shows "
	                            "your member name. The notebook of a person can contain text from "
	                            "every client program of the person. You can read every "
	                            "notebook. Every version of a note stays in the history."));
	ok = ok && dstr_cat(&description, " Prefer small edits. Before replace, read the whole "
	                    "note. Send the whole new text. Copy unrelated passages and evidence "
	                    "word for word. To rewrite most of a note, add a new file such as "
	                    "plan-v2.md. Keep the old file. In the new file, name the old file and "
	                    "the changes. NUL bytes and bytes of invalid UTF-8 show as "
	                    "\xe2\x9f\xa6XX\xe2\x9f\xa7.");
	if (!ok) {
		json_object_put(props);
		return nullptr;
	}
	struct json_object *def = schema_tool("notes", dstr_get(&description), props,
	                                      CSTR_VIEW("action"));
	return jb_set_or_free(def, "annotations", jb_object(
		"readOnlyHint", json_object_new_boolean(0),
		"destructiveHint", json_object_new_boolean(0),
		"idempotentHint", json_object_new_boolean(0),
		"openWorldHint", json_object_new_boolean(0),
		nullptr));
}

/* Returns the definition of the notes tool for clients. @p service is the
 * struct notes_tool.
 */
static struct json_object *
notes_tool_definition (void const *service)
{
	struct notes_tool const *tool = service;
	struct json_object *def = notes_tool_schema(tool, false);
	/* The session ID stays the same when askai or the client restarts.
	 * Clients find the members with the action members. */
	struct member_session const *session = member_registry_session(tool->members);
	if (!session)
		return def;
	return jb_set_or_free(def, "_meta", jb_object(
		"askai", jb_object(
			"session", jb_string(session->id),
			"agent", json_object_new_string(session->agent),
			"members", json_object_new_boolean(1),
			nullptr),
		nullptr));
}

struct json_object *
notes_tool_upstream_definition (void const *service)
{
	return notes_tool_schema(service, true);
}

struct plugin const notes_tool_plugin = {
	.tools = {{
		.definition = notes_tool_definition,
		.start = notes_tool_start,
		.step = notes_tool_step,
		.drop = notes_tool_drop,
		.flags = KF_SERIAL | TOOL_LENT | TOOL_PAGED
	}}
};
