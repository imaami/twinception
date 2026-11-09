/** @file notes_tool.c
 *
 * The notes tool (src/notes_tool.h).
 */
#define _GNU_SOURCE /* memmem(), mempcpy() */
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "buf.h"
#include "child.h"
#include "cstr_view.h"
#include "datauri.h"
#include "dstr.h"
#include "git.h"
#include "hexid.h"
#include "jb.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "mcp.h"
#include "notes.h"
#include "notes_tool.h"
#include "notice.h"
#include "oid.h"
#include "plugin.h"
#include "review.h"
#include "schema.h"
#include "shelf.h"
#include "util.h"

/* A fetch value says why git runs on the notes (src/notes.h). The values
 * up to FETCH_KIND are kinds, and FETCH_KIND masks them. Each kind is a
 * row of notes_tool_fetch_steps[]. The bits above FETCH_KIND are flags.
 */
enum notes_tool_fetch : uint32_t {
	FETCH_CREATE,          /* create the shared repository for a write */
	FETCH_CHECK,           /* check the note name before a write */
	FETCH_ADD,             /* commit the note of a write */
	FETCH_LIST,            /* list the notes for history */
	FETCH_READ,            /* read a note */
	FETCH_RESUME,          /* list the notes for the prompt of an ask call */
	FETCH_COMPACT,         /* list the notes for a compaction */
	FETCH_BOOKS,           /* list the notebooks of the session */
	FETCH_FILES,           /* list the current files for list */
	FETCH_NEWEST,          /* read the newest note, after FETCH_RESUME or
	                          FETCH_COMPACT */
	FETCH_DIFF,            /* compare two revisions of a note */
	FETCH_STAT,            /* count the lines that a write changed */
	FETCH_GREP,            /* find the lines that match a pattern */
	FETCH_RECORD,          /* read the review record of a thread for the
	                          prompt of an ask call */
	FETCH_SAVE,            /* commit the review record of an ask call */
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
	FETCH_SAVING = 2048,   /* set on each git run of the save of a review
	                          record */
};

/* A row of notes_tool_fetch_steps[]: the step that reads the output of
 * git, and the git command for a fetch value. listed is nonzero if git
 * writes a list, and the call fails without one (notes_tool_listed()).
 */
struct notes_tool_fetch_step {
	tool_step_fn *out;
	uint32_t      cmd;     /* enum notes_cmd, widened over the padding */
	uint32_t      listed;
};

static struct notes_tool_fetch_step const *notes_tool_fetch_step (uint32_t fetch);

/* Returns true if git writes to the notes when it runs @p cmd. */
static bool
notes_tool_writes (enum notes_cmd cmd)
{
	return cmd == NOTES_CREATE || cmd == NOTES_ADD;
}

/* The flags of an action in notes_tool_actions[]. */
enum {
	NOTE_WRITE  = 1 << 0,    /* writes to the caller's own notebook */
	NOTE_FILE   = 1 << 1,    /* takes the file argument */
	NOTE_OFFSET = 1 << 2,    /* takes the offset argument */
	NOTE_REVISION = 1 << 3,  /* takes the revision argument */
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

/* The message for a name of a written note that notes_select() accepts. */
static struct cstr_view const selector_name =
	CSTR_VIEW_INIT("A note name cannot be in the form PARTICIPANT/WHO/SIDE of a notebook "
	               "selector. Writes always go to your own notebook.");

/* The message for a call that names a note that does not exist. */
static struct cstr_view const no_note =
	CSTR_VIEW_INIT("The note does not exist. list shows the current notes. add creates a "
	               "note.");

/* The name of the file of a review record in its commit. */
static struct cstr_view const review_file = CSTR_VIEW_INIT("review.json");

/* The part of the notes tool on one loop. */
struct notes_tool_loop {
	struct shelf             spare;  /* finished calls, kept for reuse with
	                                    the storage of their output */
	struct notes_tool const *tool;   /* the service */
};

/* The flags of a notes call (struct notes_tool_call). */
enum {
	CALL_STRIPPED = 1 << 0,  /* askai cut its notices off the text */
	CALL_AS       = 1 << 1,  /* the call gave the argument as */
};

/* The state of one notes call. It is the own word of the call's slot. An
 * ask call has one in its own slot while git reads the notes and the
 * review record for its prompt, and while git saves its review record.
 * For the record read, notebook is the ref of the record. For a save,
 * repo is that ref, notebook the caller's own notebook, revision.tip the
 * parent of the commit, text the trace of the ask call and then the step
 * after the save.
 */
struct notes_tool_call {
	struct buf              file;      /* the input of git, such as a
	                                      write's file part or the pattern
	                                      of a search, until notes_tool_run()
	                                      passes it to git */
	struct dstr             out;       /* the output of a tool call.
	                                      notes_tool_end() lends it to
	                                      the slot. */
	struct notes_part       part;      /* the offsets of the name and the
	                                      text in file */
	struct list             spare;     /* the call's node on loop->spare */
	struct child           *child;     /* the call's git, until a step has
	                                      read its output */
	struct notes_tool_loop *loop;      /* the part of the call's loop */
	struct dstr            *text;      /* &out for a tool call. For the
	                                      prompt notes, it is the text that
	                                      askai adds to the prompt. */
	tool_step_fn           *then;      /* the step that goes on with the ask
	                                      call after the prompt notes */
	struct hexid const     *thread;    /* the thread whose review record
	                                      git reads after the prompt notes,
	                                      or nullptr */
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
	                                      a write that changes a note. */
	union {
		UTIL_TEXT(NOTES_MESSAGE)  message;  /* the commit message of a
		                                       write, for its result */
		UTIL_TEXT(NOTES_NAME + 1) name;     /* the name of a copy, until
		                                       its file part is written */
		struct oid                id;       /* the object id of an
		                                       attached note, until the
		                                       upload's answer */
	};
	uint32_t                fetch;     /* why git runs (enum
	                                      notes_tool_fetch) */
	uint32_t                action;    /* the row of notes_tool_actions[] */
	uint32_t                flags;     /* CALL_STRIPPED and CALL_AS */
	char                    repo[NOTES_REPO];      /* the caller's own
	                                                  notebook */
	char                    notebook[NOTES_REPO];  /* the notebook that the
	                                                  call selected, or ""
	                                                  for repo */
};

void *
notes_tool_loop_create (void        *service,
                        struct loop *loop,
                        struct list *children,
                        int         *e)
{
	(void)loop;
	(void)children;
	struct notes_tool_loop *nl = malloc(sizeof *nl);
	if (!nl) {
		*e = ENOMEM;
		return nullptr;
	}
	*e = 0;
	*nl = (struct notes_tool_loop){ .spare.list = list(&nl->spare.list), .tool = service };
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
	free(nl);
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
		.text  = text ? text : &call->out,
		.spare = list(&call->spare),
	};
	*mcp_slot_own(slot) = call;
	return call;
}

/* Ends the notes call at @p *p_call, which @p loop gave out, and sets
 * @p *p_call to nullptr. A git that writes notes runs to its end, because
 * a kill could leave a lock file behind. Any other git is killed. Without
 * memory pressure, the function then puts the call on loop->spare for
 * reuse. dstr_clear() keeps the storage of the output. Under memory
 * pressure, the function frees the call and the output.
 */
static void
notes_tool_loop_put (struct notes_tool_loop  *loop,
                     struct notes_tool_call **p_call)
{
	struct notes_tool_call *call = *p_call;
	if (notes_tool_writes(notes_tool_fetch_step(call->fetch)->cmd))
		child_release(&call->child);
	else
		child_destroy(&call->child);
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
static void notes_tool_committed (struct mcp_slot *slot);

/* Releases the notes call in @p slot, the ask call's own slot, after git
 * has read the notes for the prompt or saved the review record. If @p e is
 * not 0, mcp_slot_fail() ends the ask call with @p e. Otherwise, if
 * call->thread is set, git first reads the review record of the thread.
 * Then the function calls call->then, which goes on with the ask call.
 */
static void
notes_tool_then (struct mcp_slot *slot,
                 int              e)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (!e && call->thread) {
		notes_review(call->repo, call->thread, call->notebook);
		call->thread = nullptr;
		child_destroy(&call->child);
		struct buf in = {};
		notes_tool_run(slot, FETCH_RECORD, &in,
		               notes_check_input(&in, call->notebook, review_file));
		return;
	}
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
 * without those notes, and the write's result without those lines. A
 * save ends with the sentence that its findings could not be saved. A call
 * of the notes tool, and the list and the review record for the start of
 * an ask call, end with the error.
 */
static void
notes_tool_failed (struct mcp_slot *slot,
                   int              e)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	char buf[96];
	struct cstr_view const why = child_spawn_why(e, "git", buf, sizeof buf);
	if (call->fetch & FETCH_SAVING) {
		notes_tool_committed(slot);
		return;
	}
	switch (call->fetch & FETCH_KIND) {
	case FETCH_NEWEST:
	case FETCH_COMPACT:
	case FETCH_STAT:
		notes_tool_fetch_step(call->fetch)->out(slot);
		return;
	case FETCH_RESUME:
	case FETCH_RECORD:
		notes_tool_drop(slot);
		break;
	}
	mcp_slot_tell(slot, why, TOOL_FAILED);
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

/* Spawns git for @p fetch in the shared repository and frees @p in, the
 * input of git, which may be empty. git reads the notebook that the call
 * selected, or else call->repo. Writes always go to call->repo. If
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
	struct notes const *notes = call->loop->tool->notes;
	struct loop *loop = mcp_slot_loop(slot);
	char const *repo = notes_tool_call_notebook(call);
	enum notes_cmd cmd = notes_tool_fetch_step(fetch)->cmd;
	/* git removes its own lock files on SIGTERM. notes_lock() removes a
	 * lock file that is older than the last boot. Any other lock file
	 * stays, because its writer can run outside askai. */
	bool writes = notes_tool_writes(cmd);
	int64_t age;
	int const lock = cmd == NOTES_ADD ? notes_lock(notes, call->repo, &age) : 0;
	if (lock)
		LOG(LOG_WARN, "notes: cannot check the lock of %s: %s", call->repo, strerror(lock));
	char const *argv[NOTES_ARGV];
	char const *const words[] = {
		repo, call->revision.file.len ? call->revision.file.data : nullptr
	};
	if (fetch & FETCH_REVISION)
		notes_revision_argv(&call->revision, notes, repo, cmd, argv);
	else
		notes_argv(notes, cmd, words, argv);
	char *const env[] = { util_env_path(), GIT_ENV_FIXED, nullptr };
	struct child_cmd const c = {
		.file = "git",
		.argv = argv,
		.envp = env,
		.in = buf_peek(in),
		.max = NOTES_MAX,
		.deadline = loop->now + NOTES_NS,
		.grace = writes ? GIT_GRACE : 0,
		/* For the commands that write, the output includes the errors. */
		.flags = writes ? CHILD_STDERR : 0
	};
	call->fetch = fetch;
	call->child = mcp_slot_spawn(slot, &c, &e);
	buf_fini(in);
	if (!call->child)
		notes_tool_failed(slot, e);
}

void
notes_tool_loop_prompt (struct notes_tool_loop *loop,
                        struct mcp_slot        *slot,
                        struct dstr            *prompt,
                        enum notes_tool_prompt  what,
                        size_t                  most,
                        struct hexid const     *thread,
                        tool_step_fn           *then)
{
	struct notes_tool_call *call = notes_tool_loop_take(loop, slot, prompt);
	if (!call)
		return;
	call->then = then;
	call->thread = thread;
	call->most = most;
	notes_repo(loop->tool->notes, mcp_slot_principal(slot), NOTES_UPSTREAM, call->repo);
	notes_tool_run(slot, what == NOTES_TOOL_RESUME ? FETCH_RESUME : FETCH_COMPACT,
	               &(struct buf){}, 0);
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

/* Returns why git read no notes for @p call. Exit status 128 means that
 * the notebook does not exist yet.
 */
static struct cstr_view
notes_tool_call_unread (struct notes_tool_call const *call)
{
	return notes_tool_none(call->child) ? CSTR_VIEW("The notebook contains no notes yet.")
	                                    : CSTR_VIEW("The notes could not be read.");
}

/* Returns true if the output of git shows that another writer changed the
 * branch, or created the repository, at the same time. Other instances of
 * askai can share the repository.
 */
static bool
notes_tool_raced (struct child const *child)
{
	struct cstr_view const raced[] = {
		CSTR_VIEW("Not updating "), CSTR_VIEW("not updating "),
		CSTR_VIEW(" but expected "), CSTR_VIEW("File exists"),
	};
	struct span out = buf_peek(&child->out);
	for (size_t i = 0; i < array_size(raced) && out.len; ++i)
		if (memmem(out.ptr, out.len, raced[i].data, raced[i].len))
			return true;
	return false;
}

/* Fails the write in @p slot with why git did not write the note. The
 * reasons are a timeout, a race with another writer, a lock file that a
 * dead or hung writer left, and other errors of git. The function logs the
 * last two.
 */
static void
notes_tool_unadded (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	if (c->end.flags & CHILD_LATE) {
		mcp_slot_tell(slot, CSTR_VIEW("Could not write the note. The write timed out."),
		              TOOL_FAILED);
		return;
	}
	if (notes_tool_raced(c)) {
		struct notes const *notes = call->loop->tool->notes;
		char const *repo = call->repo;
		int64_t age = 0;
		if (notes_lock(notes, repo, &age) || age <= NOTES_STALE) {
			mcp_slot_tell(slot, CSTR_VIEW("Could not write the note. Another call "
			                              "changed the notebook at the same time. "
			                              "Read the current note and retry."),
			              TOOL_FAILED);
			return;
		}
		LOG(LOG_WARN, "notes: %s/%s.lock is %" PRId64 " s old; remove it if no "
		    "git process uses the repository", notes_directory(notes), repo, age);
		mcp_slot_tellf(slot, TOOL_FAILED, "Could not write the note. The notebook has been "
		               "locked for %" PRId64 " s.", age);
		return;
	}
	struct span out = buf_peek(&c->out);
	char const *why = (char const *)out.ptr;
	LOG(LOG_WARN, "notes: git could not write a note: %.*s",
	    (int)util_clip(why, out.len, 300), why);
	mcp_slot_tell(slot, CSTR_VIEW("Could not write the note."), TOOL_FAILED);
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
	static_assert(sizeof "Copy  from  at : ( bytes)" + UTIL_DECIMAL + 2 * NOTES_NAME + 2 +
	              NOTES_REPO + OID_SHA256 + NOTES_NAME <= NOTES_MESSAGE);
	static_assert(sizeof "Restore  from : ( to  bytes)" + 2 * UTIL_DECIMAL + 2 * NOTES_NAME + 2 +
	              OID_SHA256 + NOTES_NAME <= NOTES_MESSAGE);
	uint32_t fetch = notes_tool_actions[call->action].fetch;
	struct cstr_view const verb = notes_action_names[call->action];
	char const up = (char)(verb.data[0] - 'a' + 'A');
	/* A restore names the commit of its revision, and a copy the source
	 * notebook and the commit it read. With as, the message names the
	 * source file after the commit, as commit:file. */
	char const *commit = call->revision.from.hex;
	char const *book = fetch & FETCH_COPY ? notes_selector(notes_tool_call_notebook(call)) : "";
	char const *source = call->flags & CALL_AS ? call->revision.file.data : "";
	int len = (int)call->part.name_len;
	char const *name = (char const *)buf_peek(&call->file).ptr + call->part.name_at;
	uint64_t after = call->part.data_len;
	util_text_clear(&call->message);
	int e = util_text_printf(&call->message, "%c%.*s %.*s%s%s%s%s%s%s", up, (int)verb.len - 1,
	                         verb.data + 1, len, name, *commit ? " from " : "", book,
	                         *book ? " at " : "", commit, *source ? ":" : "", source);
	return e ? e : before == NO_NOTE
	     ? util_text_printf(&call->message, " (%" PRIu64 " byte%s)", after, util_s(after))
	     : util_text_printf(&call->message, " (%" PRIu64 " to %" PRIu64 " bytes)", before, after);
}

/* Runs git to commit the note in call->file of @p slot. The new commit has
 * the parent @p tip, or no parent if @p tip is no name. The note had
 * @p before bytes, or there was none if @p before is NO_NOTE.
 */
static void
notes_tool_write (struct mcp_slot  *slot,
                  struct oid const *tip,
                  uint64_t          before)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct span file = buf_peek(&call->file);
	struct buf in = {};
	int e = notes_tool_call_message(call, before);
	e = e ? e : notes_add_input(&in, call->repo, call->repo, tip,
	                            util_text_view(&call->message), file);
	buf_fini(&call->file);
	notes_tool_run(slot, FETCH_ADD, &in, e);
}

/* Runs git to commit call->file, the review record of the ask call in
 * @p slot, on call->repo, the ref of its thread, with the parent
 * call->revision.tip.
 */
static void
notes_tool_write_record (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct buf in = {};
	int e = notes_add_input(&in, call->repo, call->notebook, &call->revision.tip,
	                        CSTR_VIEW("Save the review record"), buf_peek(&call->file));
	buf_fini(&call->file);
	notes_tool_run(slot, FETCH_SAVE | FETCH_SAVING, &in, e);
}

/* Handles the end of git init for a write. The function writes the note,
 * or ends the call if git failed.
 */
static void
notes_tool_created (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	/* A save runs the commit even if git init failed, because the commit
	 * then fails too. */
	if (call->fetch & FETCH_SAVING) {
		child_destroy(&call->child);
		notes_tool_write_record(slot);
		return;
	}
	if (child_ok(call->child) ||
	    (notes_tool_raced(call->child) && notes_exists(call->loop->tool->notes))) {
		child_destroy(&call->child);
		notes_tool_write(slot, &(struct oid){}, NO_NOTE);
		return;
	}
	notes_tool_unadded(slot);
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

/* Returns true if @p note, the note that NOTES_CHECK found, already has
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
	struct span file = buf_peek(&call->file);
	struct span data = { file.ptr + call->part.data_at, call->part.data_len };
	bool same = false;
	int e = notes_file_same(note, data, &same);
	if (e)
		LOG(LOG_WARN, "notes: cannot compare a note with its new bytes: %s", strerror(e));
	else if (data.len == note->bytes)
		loop_work(mcp_slot_loop(slot), LOOP_COPY, data.len);
	if (!same)
		return false;
	char const *name = (char const *)file.ptr + call->part.name_at;
	uint64_t n = note->bytes;
	e = dstr_printf(call->text, "Unchanged. %.*s at the newest commit %s already contains "
	                "the same %" PRIu64 " byte%s. No commit was made.",
	                (int)jb_utf8(name, call->part.name_len), name, tip->hex, n,
	                util_s(n)) ? 0 : ENOMEM;
	(void)mcp_slot_noted(slot);
	notes_tool_wrote(slot, e);
	return true;
}

/* Handles the output of NOTES_CHECK for a write. add and copy accept only a
 * name that is neither a note nor a directory of notes. No directory in the
 * name can be a note. The writes with FETCH_REPLACE accept only the name
 * of an existing note, and git reads the note for append. A replace or a
 * restore that leaves the bytes of the note unchanged makes no commit.
 * For a write that changes an existing note, the function stores the
 * newest commit, the parent of the write, in call->revision.tip.
 */
static void
notes_tool_checked (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct oid tip;
	struct notes_file note = {};
	int r = child_ok(c) ? notes_checked(buf_peek(&c->out), &tip, &note) : EIO;
	uint64_t before = r == EEXIST ? note.bytes : NO_NOTE;
	uint32_t fetch = notes_tool_actions[call->action].fetch;
	if (fetch & FETCH_REPLACE)
		r = r == EEXIST ? 0 : r && r != ENOTDIR ? r : ENOENT;
	if (fetch & FETCH_RESTORE) {
		if (r == EEXIST && !(call->flags & CALL_AS))
			r = 0;
		if (!r && !oid_equal(&tip, &call->revision.tip)) {
			mcp_slot_tell(slot, CSTR_VIEW("The notebook changed during restore. Read "
			                              "the history again and retry."), TOOL_FAILED);
			return;
		}
	}
	if (!r) {
		call->revision.tip = before == NO_NOTE ? (struct oid){} : tip;
		child_destroy(&call->child);
		if (fetch & FETCH_APPEND) {
			/* git reads the note at the tip that the write builds on. */
			struct buf in = {};
			int e = notes_read_input(&in, tip.hex, util_text_view(&call->revision.file));
			notes_tool_run(slot, FETCH_READ | FETCH_APPEND, &in, e);
		} else if (before == NO_NOTE || fetch & FETCH_DELETE ||
		           !notes_tool_unchanged(slot, &note, &tip)) {
			notes_tool_write(slot, &tip, before);
		}
		return;
	}
	mcp_slot_tell(slot,
	              r == EEXIST ? CSTR_VIEW("The note already exists. Change the note with "
	                                      "replace or append, or choose another name.")
	              : r == ENOENT ? no_note
	              : r == EISDIR ? dir_name
	              : r == ENOTDIR ? CSTR_VIEW("A part of the note name before a slash is a "
	                                         "note.")
	              : notes_tool_call_unread(call), TOOL_FAILED);
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
	if (!e && oid_len(&call->revision.tip))
		e = dstr_printf(call->text, " The previous version is in commit %s.",
		                call->revision.tip.hex) ? 0 : ENOMEM;
	notes_tool_wrote(slot, e);
}

/* Handles the output of NOTES_STAT after a replace or restore. The
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
 * the note. The caller's own notebook contains notes then, and the
 * function calls mcp_slot_noted(). After a replace or restore of an
 * existing note, git counts the lines that the write changed.
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
	if (notes_add_out(buf_peek(&call->child->out), &commit))
		LOG(LOG_WARN, "notes: git fast-import gave no commit id");
	struct cstr_view const p[] = {
		oid_len(&commit) ? CSTR_VIEW("Saved in commit ") : CSTR_VIEW("Saved"),
		oid_view(&commit),
		CSTR_VIEW(": "),
		{ call->message.data, jb_utf8(call->message.data, call->message.len) },
		CSTR_VIEW("."),
	};
	int e = cstr_view_join(p, array_size(p), UINT64_MAX, CSTR_VIEW(""), CSTR_VIEW(""),
	                       call->text);
	if (e || !oid_len(&commit) || !oid_len(&call->revision.tip) ||
	    notes_tool_actions[call->action].fetch & (FETCH_APPEND | FETCH_DELETE)) {
		notes_tool_saved(slot, e);
		return;
	}
	notes_revision_pin(&call->revision, &call->revision.tip, &commit);
	child_destroy(&call->child);
	notes_tool_run(slot, FETCH_STAT | FETCH_REVISION, &(struct buf){}, 0);
}

/* Appends the history in @p out, the output of git for NOTES_LIST, to
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
		if (!dstr_printf(d, "%" PRIu32 ". %.*s  ", ++k, (int)w, when) ||
		    !dstr_cat_n(d, name, jb_utf8(name, entry.name.len)) ||
		    !(entry.added == NOTES_BINARY ? dstr_cat(d, "  (binary)")
		      : dstr_printf(d, "  (+%" PRIu64 " -%" PRIu64 ")", entry.added, entry.removed)) ||
		    (history && !dstr_printf(d, "  commit %s%s%.*s", entry.commit.hex,
		                            subject_len ? "  " : "",
		                            (int)subject_len, subject_len ? subject : "")) ||
		    !dstr_cat_n(d, "\n", 1))
			return ENOMEM;
		*newest = entry.name;
	}
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	return 0;
}

/* Appends the current files in @p out, the output of git for NOTES_FILES,
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
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	bool const files = (call->fetch & FETCH_KIND) == FETCH_FILES;
	bool const cut = c->end.flags & CHILD_CUT;
	struct span const out = buf_peek(&c->out);
	struct span newest;
	int e = dstr_printf(call->text, "Notebook %s — %s:\n\n",
	                    notes_selector(notes_tool_call_notebook(call)),
	                    files ? "current files (bytes, blob)"
	                          : "history, oldest first (lines +added -removed, commit, "
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

/* Appends the session's notebooks to call->text: the selector of the
 * caller's own notebook, which does not need to have commits, then the
 * selector of each other notebook in the output of @p c, the git for
 * NOTES_BOOKS, or the line that no other notebook has notes. @p c is
 * nullptr before the first note of the session. git lists loose and
 * packed refs. Returns 0 or ENOMEM.
 */
static int
notes_tool_books (struct notes_tool_call const *call,
                  struct child const           *c)
{
	struct cstr_view const own = cstr_view(notes_selector(call->repo));
	struct span const out = c ? buf_peek(&c->out) : (struct span){};
	bool ok = dstr_printf(call->text, "Session notebooks (use a selector as notebook):\n"
	                      "Self (writable): %s\n", own.data);
	size_t peers = 0;
	struct cstr_view rest = { (char const *)out.ptr, out.len };
	for (struct cstr_view name; ok && cstr_view_cut(&rest, '\n', &name); ) {
		bool const peer = !cstr_view_equal(name, own) && !notes_select(name, nullptr);
		peers += peer;
		ok = !peer || dstr_printf(call->text, "%.*s (read-only)\n", (int)name.len, name.data);
	}
	char const *end = c && c->end.flags & CHILD_CUT ? "[The notebook list is incomplete.]\n"
	                : peers ? nullptr : "No other notebook contains notes yet.\n";
	return ok && (!end || dstr_cat(call->text, end)) ? 0 : ENOMEM;
}

/* Handles the output of git for notebooks. */
static void
notes_tool_got_books (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	notes_tool_end(slot, notes_tool_books(call, call->child), false);
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
 */
static void
notes_tool_revision_step (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (!child_ok(call->child) ||
	    notes_revision_next(&call->revision, buf_peek(&call->child->out))) {
		/* git pins the tip of the notebook first. */
		mcp_slot_tell(slot, oid_len(&call->revision.tip)
		                    ? CSTR_VIEW("The commit in revision, from or to is not in the "
		                                "notebook history. Use a full commit ID from "
		                                "action history.")
		                    : notes_tool_call_unread(call), TOOL_FAILED);
		return;
	}
	child_destroy(&call->child);
	int e = 0;
	if (notes_revision_ready(&call->revision) && (call->fetch & FETCH_KIND) == FETCH_READ)
		e = notes_read_input(&call->file, call->revision.from.hex,
		                     util_text_view(&call->revision.file));
	notes_tool_run(slot, call->fetch, &call->file, e);
}

/* Handles the output of git for diff. */
static void
notes_tool_got_diff (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct notes_revision const *r = &call->revision;
	struct span out = buf_peek(&c->out);
	struct dstr *d = call->text;
	char const *selector = notes_selector(notes_tool_call_notebook(call));
	(void)mcp_slot_shows(slot, selector);
	int e = dstr_printf(d, "Notebook %s — %s\nfrom %s\nto %s\n\n", selector, r->file.data,
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
	struct notes_tool_call const *call = *mcp_slot_own(slot);
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
	char const *repo = notes_tool_call_notebook(call);
	if (!e)
		e = dstr_printf(call->text, "Notebook %s — lines that match the pattern "
		                "(file:line:text):\n\n", notes_selector(repo)) ? 0 : ENOMEM;
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
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct span text = {};
	struct oid id;
	int r = child_ok(c) ? notes_read_out(buf_peek(&c->out), &text, &id) : EIO;
	if (r) {
		mcp_slot_tell(slot, r == ENOENT ? no_note : r == EISDIR ? dir_name
		                  : notes_tool_call_unread(call), TOOL_FAILED);
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
	char const *selector = notes_selector(notes_tool_call_notebook(call));
	(void)mcp_slot_shows(slot, selector);
	int e = 0;
	if (call->fetch & FETCH_REVISION)
		e = dstr_printf(call->text, "Notebook %s — %s at %s:\n\n", selector,
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
		 * that is longer than NOTES_MAX. */
		size_t len = 0;
		(void)notes_tool_view(&view, body, 0, &len);
		if (len > NOTES_MAX) {
			mcp_slot_tellf(slot, TOOL_FAILED, "The note is too large to attach. The view "
			               "of the note would be %zu bytes. The upload limit is %d MiB. "
			               "read shows the note in parts.", len, NOTES_MAX >> 20);
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
			CSTR_VIEW("\n[askai: the current text of your newest note, "),
			{ name.data, jb_utf8(name.data, name.len) },
			CSTR_VIEW(":]\n\n"),
		};
		e = cstr_view_join(p, array_size(p), UINT64_MAX, CSTR_VIEW(""), CSTR_VIEW(""),
		                   call->text);
		e = e ? e : notes_read_input(&in, call->repo, name);
	} else if (!e && name.len) {
		e = cstr_view_cat(CSTR_VIEW("\n[askai: the newest note could not "
		                            "be read.]\n"), call->text);
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
		r == ENOENT ? CSTR_VIEW("[askai: the note was deleted.]")
		: r ? CSTR_VIEW("[askai: the note could not be read.]") : body,
		more,
		CSTR_VIEW("\n"),
	};
	bool newline = body.len && body.data[body.len - 1] == '\n';
	notes_tool_then(slot, cstr_view_join(p, newline && !more.len ? 1 : array_size(p),
	                                     UINT64_MAX, CSTR_VIEW(""), CSTR_VIEW(""), call->text));
}

/* Handles the list of the notes for the start of an ask call. The list
 * goes into the prompt. If the notebook does not exist, the ask call goes
 * on without notes. If git failed otherwise, the ask call ends as failed.
 */
static void
notes_tool_resumed (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	if (!notes_tool_listed(c)) {
		if (notes_tool_none(c)) {
			notes_tool_then(slot, 0);
			return;
		}
		notes_tool_drop(slot);
		mcp_slot_tell(slot, CSTR_VIEW("Cannot read the session notes."), TOOL_FAILED);
		return;
	}
	(void)mcp_slot_noted(slot);
	notes_tool_prompt_list(slot, CSTR_VIEW("\n\n[askai: call notes with action list to "
	                                       "list your current notes. Call notes with "
	                                       "action notebooks to list the notebooks of the "
	                                       "other participants. A repeated name in the "
	                                       "history is an update. Your note history, "
	                                       "oldest first:]\n\n"));
}

/* Handles the list of the notes for a compaction. The list goes into the
 * prompt. If git failed, the function appends the line "[askai: your
 * notes could not be read.]" instead.
 */
static void
notes_tool_compacted (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	if (notes_tool_listed(call->child)) {
		notes_tool_prompt_list(slot, CSTR_VIEW("\n\n[askai: a repeated name in the "
		                                       "history is an update. Your note history, "
		                                       "oldest first:]\n\n"));
		return;
	}
	notes_tool_then(slot, cstr_view_cat(CSTR_VIEW("\n\n[askai: your "
	                                              "notes could not be "
	                                              "read.]\n"), call->text));
}

/* Handles the output of git for the review record of a thread. Without the
 * ref, the ask call starts with an empty record. Otherwise the findings of
 * the record go into the record of the ask call, and its text is appended
 * to the prompt after the notes. A record that askai cannot read ends the
 * ask call as failed, and the ref stays as it is.
 */
static void
notes_tool_carried (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	struct review *r = mcp_slot_review(slot);
	struct oid tip;
	struct span json = {};
	int e = child_ok(c) ? notes_review_out(buf_peek(&c->out), &tip, &json) : EIO;
	if (e == ENOENT) {
		notes_tool_then(slot, 0);
		return;
	}
	struct json_object *obj = e ? nullptr
	                        : jb_parse((char const *)json.ptr, json.len, JSON_TOKENER_STRICT, 0);
	loop_work(mcp_slot_loop(slot), LOOP_PARSE, json.len);
	e = e ? e : obj ? review_carry(r, obj, &tip) : EINVAL;
	json_object_put(obj);
	e = e ? e : cstr_view_cat(CSTR_VIEW("\n\n"), call->text) ? ENOMEM
	  : review_render(r, call->text, call->most);
	if (!e || e == ENOMEM) {
		notes_tool_then(slot, e);
		return;
	}
	LOG(LOG_WARN, "notes: cannot read the review record %s: %s", call->notebook, strerror(e));
	notes_tool_drop(slot);
	mcp_slot_tell(slot, CSTR_VIEW("Cannot read the review record of the thread."),
	              TOOL_FAILED);
}

/* Ends the save of the review record in @p slot. The function appends a
 * sentence about the save to call->text, the trace of the ask call, and
 * goes on with the ask call. call->child is nullptr if git did not start.
 */
static void
notes_tool_committed (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	if (child_ok(c)) {
		child_destroy(&call->child);
		notes_tool_then(slot, dstr_printf(call->text, "The review record is saved. An ask "
		                                  "call with thread %s starts with the saved "
		                                  "record.\n", strrchr(call->repo, '/') + 1)
		                      ? 0 : ENOMEM);
		return;
	}
	struct cstr_view why = CSTR_VIEW("The findings of the ask call could not be saved.\n");
	if (c) {
		struct span const out = buf_peek(&c->out);
		char const *text = (char const *)out.ptr;
		LOG(LOG_WARN, "notes: git could not save %s: %.*s", call->repo,
		    (int)util_clip(text, out.len, 300), text);
		if (notes_tool_raced(c))
			why = CSTR_VIEW("The findings of the ask call were not saved. Another ask "
			                "call of the thread saved a review record first.\n");
	}
	child_destroy(&call->child);
	notes_tool_then(slot, cstr_view_cat(why, call->text));
}

void
notes_tool_loop_save (struct notes_tool_loop *loop,
                      struct mcp_slot        *slot,
                      struct dstr            *told,
                      struct hexid const     *thread,
                      tool_step_fn           *then)
{
	struct notes_tool_call *call = notes_tool_loop_take(loop, slot, told);
	if (!call)
		return;
	call->then = then;
	notes_repo(loop->tool->notes, mcp_slot_principal(slot), NOTES_UPSTREAM, call->notebook);
	notes_review(call->notebook, thread, call->repo);
	/* review_carry_json() writes the parent of the commit to
	 * call->revision.tip. */
	int e;
	struct json_object *record = review_carry_json(mcp_slot_review(slot), &call->revision.tip,
	                                               &e);
	struct cstr_view const json = jb_wire(record);
	e = e ? e : json.data ? notes_add_file(&call->file, review_file, json, &call->part)
	  : ENOMEM;
	loop_work(mcp_slot_loop(slot), LOOP_COPY, json.len);
	json_object_put(record);
	if (e) {
		notes_tool_then(slot, e);
		return;
	}
	if (notes_exists(loop->tool->notes))
		notes_tool_write_record(slot);
	else
		notes_tool_run(slot, FETCH_CREATE | FETCH_SAVING, &(struct buf){}, 0);
}

static struct notes_tool_fetch_step const notes_tool_fetch_steps[] = {
	[FETCH_CREATE]  = { notes_tool_created,   NOTES_CREATE, 0 },
	[FETCH_CHECK]   = { notes_tool_checked,   NOTES_CHECK,  0 },
	[FETCH_ADD]     = { notes_tool_added,     NOTES_ADD,    0 },
	[FETCH_LIST]    = { notes_tool_got_list,  NOTES_LIST,   1 },
	[FETCH_READ]    = { notes_tool_got_note,  NOTES_READ,   0 },
	[FETCH_RESUME]  = { notes_tool_resumed,   NOTES_LIST,   0 },
	[FETCH_COMPACT] = { notes_tool_compacted, NOTES_LIST,   0 },
	[FETCH_BOOKS]   = { notes_tool_got_books, NOTES_BOOKS,  1 },
	[FETCH_FILES]   = { notes_tool_got_list,  NOTES_FILES,  1 },
	[FETCH_NEWEST]  = { notes_tool_newest,    NOTES_READ,   0 },
	[FETCH_DIFF]    = { notes_tool_got_diff,  NOTES_DIFF,   1 },
	[FETCH_STAT]    = { notes_tool_stated,    NOTES_STAT,   0 },
	[FETCH_GREP]    = { notes_tool_got_grep,  NOTES_GREP,   0 },
	[FETCH_RECORD]  = { notes_tool_carried,   NOTES_READ,   0 },
	[FETCH_SAVE]    = { notes_tool_committed, NOTES_ADD,    0 },
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
	/* Only attach reaches this step without a child. The step then handles
	 * the answer to the upload. */
	if (!call->child) {
		notes_tool_uploaded(slot);
		return;
	}
	if (call->fetch & FETCH_REVISION && !notes_revision_ready(&call->revision)) {
		notes_tool_revision_step(slot);
		return;
	}
	struct notes_tool_fetch_step const *row = notes_tool_fetch_step(call->fetch);
	if (row->listed && !notes_tool_listed(call->child))
		mcp_slot_tell(slot, notes_tool_call_unread(call), TOOL_FAILED);
	else
		row->out(slot);
}

static_assert(TOOL_TEXT_MAX == 4U << 20 && NOTES_NAME == 255 && HEXID_DIGITS == 32,
              "update the texts in this file that say 4 MiB, 255 bytes or 32 hexadecimal "
              "digits");

/* Runs git to check the name @p name of the note in call->file before the
 * write, or to create the shared repository if it does not exist. If
 * @p e is not 0, the function ends the call with @p e instead.
 */
static void
notes_tool_check (struct mcp_slot  *slot,
                  struct cstr_view  name,
                  int               e)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct buf in = {};
	uint32_t fetch = FETCH_CREATE;
	if (!e && notes_exists(call->loop->tool->notes)) {
		fetch = FETCH_CHECK;
		e = notes_check_input(&in, call->repo, name);
	}
	notes_tool_run(slot, fetch, &in, e);
}

/* Starts add, append or replace of the note @p name with the arguments
 * @p args. For append, the file part contains the text to append. The note
 * goes to the caller's own notebook. git creates the shared repository
 * first if it does not exist. A turn's text is cut before its
 * trailing notices (notice_kept()), because askai adds those notices only
 * to the results of a turn's calls.
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
	size_t kept = turn ? notice_kept(text.data, text.len) : text.len;
	struct datauri data = {};
	char big[96];
	struct cstr_view const problem =
		notes_name(&name) ? bad_name
		: !notes_select(name, nullptr) ? selector_name
		: !content ? (!text.len || text.len > TOOL_TEXT_MAX || memchr(text.data, '\0', text.len)
		              ? CSTR_VIEW("The text argument must be a string of 1 byte to 4 MiB "
		                          "without NUL bytes.")
		              : !kept ? CSTR_VIEW("The text contains only [askai: ...] lines. "
		                                  "askai adds [askai: ...] lines to tool results. "
		                                  "Give the text of the note.")
		              : (struct cstr_view){})
		: turn ? CSTR_VIEW("Only a client can give content. Give text instead.")
		: text.data ? CSTR_VIEW("Give text or content, not both.")
		: datauri_init(&data, jb_view(content))
		? CSTR_VIEW("The content argument must be a data: URI. Your client fills in "
		            "content with a file from the user.")
		: data.size > most
		? util_format_view(big, sizeof big, "The content file is larger than the limit "
		                   "of %zu bytes.", most)
		: !data.size && notes_tool_actions[call->action].fetch & FETCH_APPEND
		? CSTR_VIEW("The content file to append is empty.")
		: (struct cstr_view){};
	if (problem.data) {
		mcp_slot_tell(slot, problem, TOOL_FAILED);
		return;
	}
	(void)util_text_store(&call->revision.file, name);
	int e;
	if (content) {
		e = notes_add_head(&call->file, name, data.size, &call->part);
		if (!e)
			datauri_decode(&data, buf_peek(&call->file).ptr + call->part.data_at);
		loop_work(mcp_slot_loop(slot), LOOP_COPY, data.size);
	} else {
		/* askai adds a line feed to a turn's text that lacks a final
		 * one, if the note stays within 4 MiB. The byte after the kept
		 * text is that line feed, or the one that notes_add_head()
		 * writes after the text. */
		size_t const lf = turn && kept < TOOL_TEXT_MAX && text.data[kept - 1] != '\n';
		call->flags = kept < text.len ? CALL_STRIPPED : 0;
		e = notes_add_head(&call->file, name, kept + lf, &call->part);
		if (!e)
			*(char *)mempcpy(buf_peek(&call->file).ptr + call->part.data_at, text.data,
			                 kept) = '\n';
	}
	notes_tool_check(slot, name, e);
}

/* Writes @p text to the note call->name in the caller's own notebook. A
 * restore fails if @p text is longer than TOOL_TEXT_MAX bytes.
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
	int e = notes_add_file(&call->file, to, bytes, &call->part);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, text.len);
	child_destroy(&call->child);
	notes_tool_check(slot, to, e);
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
	int e = notes_delete_file(&call->file, name, &call->part);
	notes_tool_check(slot, name, e);
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
	struct notes_part part;
	int e = notes_add_head(&file, util_text_view(&call->revision.file), old.len + lf + add.len,
	                       &part);
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
	notes_tool_write(slot, &call->revision.tip, old.len);
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
	notes_tool_run(slot, FETCH_GREP | FETCH_REVISION, &(struct buf){},
	               buf_put(&call->file, pattern.data, pattern.len, SIZE_MAX));
}

/* Runs git to read the note @p name. @p fetch is the fetch of read or
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
	struct buf in = {};
	int e = notes_read_input(&in, notes_tool_call_notebook(*mcp_slot_own(slot)), name);
	notes_tool_run(slot, fetch, &in, e);
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
	notes_tool_run(slot, fetch, &(struct buf){}, 0);
}

/* Starts a call of the notes tool in @p slot with the arguments @p args.
 * This is the start() of the notes kind. All instances of askai that use
 * the same repository share the notes. Every write goes to call->repo,
 * the branch that notes_repo() makes for the caller.
 */
static void
notes_tool_start (struct mcp_slot    *slot,
                  struct json_object *args)
{
	struct notes_tool_call *call = notes_tool_loop_take(mcp_slot_part(slot), slot, nullptr);
	if (!call)
		return;
	enum notes_side const side = mcp_slot_from_client(slot) ? NOTES_CLIENT : NOTES_UPSTREAM;
	struct notes const *notes = call->loop->tool->notes;
	notes_repo(notes, mcp_slot_principal(slot), side, call->repo);
	size_t const offered = side == NOTES_UPSTREAM ? call->loop->tool->actions
	                                              : NOTES_ACTION_ATTACH;
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
	if (side == NOTES_CLIENT) {
		struct json_object *thread = jb_get(args, "thread");
		if (thread && !hexid_valid(jb_view(thread))) {
			mcp_slot_tell(slot, CSTR_VIEW("The thread argument must be 32 hexadecimal "
			                              "digits. The notes do not depend on "
			                              "thread."), TOOL_FAILED);
			return;
		}
	}
	/* A copy without a notebook argument reads the notebook of the other
	 * side if the caller is a client. A turn must give the notebook
	 * argument for a copy. */
	struct json_object *notebook = jb_get(args, "notebook");
	struct cstr_view const selected = jb_view(notebook);
	bool other = cstr_view_equal(selected, CSTR_VIEW("other"));
	if (!notebook && (fetch & FETCH_COPY)) {
		if (side == NOTES_UPSTREAM) {
			mcp_slot_tell(slot, CSTR_VIEW("copy needs notebook. Give the source "
			                              "notebook as a participant's name or a "
			                              "selector from action notebooks."),
			              TOOL_FAILED);
			return;
		}
		other = true;
	}
	if (other) {
		notes_repo(notes, mcp_slot_principal(slot),
		           side == NOTES_CLIENT ? NOTES_UPSTREAM : NOTES_CLIENT, call->notebook);
	} else if (!notebook || cstr_view_equal(selected, CSTR_VIEW("own"))) {
		/* The call reads the caller's own notebook, call->repo. */
	} else if (util_word(selected.data, selected.len, UTIL_WORD_MAX)) {
		notes_book(selected, mcp_slot_principal(slot), side, call->notebook);
	} else if (notes_select(selected, call->notebook)) {
		mcp_slot_tell(slot, CSTR_VIEW("The notebook argument must be own, a participant's "
		                              "name, or a selector from action notebooks."),
		              TOOL_FAILED);
		return;
	}
	if ((flags & NOTE_WRITE) && *call->notebook && strcmp(call->notebook, call->repo)) {
		mcp_slot_tell(slot, CSTR_VIEW("You can write notes only to your own notebook."),
		              TOOL_FAILED);
		return;
	}
	if ((flags & NOTE_OFFSET) && mcp_slot_offset(slot, args))
		return;
	struct cstr_view const name = jb_view(file);
	if (fetch & (FETCH_COPY | FETCH_RESTORE)) {
		/* The name of the copy or restore is the as argument, or else
		 * file. */
		struct json_object *as = jb_get(args, "as");
		struct cstr_view target = as ? jb_view(as) : name;
		struct cstr_view const why = notes_name(&target) ? bad_name
		                           : as && !notes_select(target, nullptr) ? selector_name
		                           : (struct cstr_view){};
		if (why.data) {
			mcp_slot_tell(slot, why, TOOL_FAILED);
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
	if (fetch & FETCH_REPLACE && !notes_exists(notes)) {
		mcp_slot_tell(slot, no_note, TOOL_FAILED);
		return;
	}
	if (fetch & FETCH_DELETE) {
		notes_tool_delete(slot, name);
		return;
	}
	if (flags & NOTE_WRITE) {
		notes_tool_add(slot, name, args);
		return;
	}
	if (!notes_exists(notes)) {
		/* This answer does not fail the call. A turn's offset applies to it,
		 * and a client's offset does not. */
		struct cstr_view const none = CSTR_VIEW("The session contains no notes yet.");
		if (fetch == FETCH_BOOKS)
			notes_tool_end(slot, notes_tool_books(call, nullptr), false);
		else if (side == NOTES_CLIENT)
			mcp_slot_tell(slot, none, 0);
		else
			notes_tool_end(slot, cstr_view_cat(none, call->text), false);
		return;
	}
	if (flags & NOTE_FILE)
		notes_tool_read(slot, name, fetch);
	else if (fetch == FETCH_GREP)
		notes_tool_search(slot, jb_view(jb_get(args, "pattern")));
	else
		notes_tool_run(slot, fetch, &(struct buf){}, 0);
}

/* Returns the definition of the notes tool for the side @p side. Only the
 * definition for clients contains the content and thread arguments. The
 * definition for the upstream contains only the first tool->actions
 * entries of notes_action_names in the enum of the action argument.
 */
static struct json_object *
notes_tool_schema (struct notes_tool const *tool,
                   enum notes_side          side)
{
	static char const *const descriptions[] = {
		[NOTES_UPSTREAM] =
			"Notes are files in notebooks. Every version of a note stays in the "
			"history. You can read every notebook. You can write only your own "
			"notebook. Each ask call and each compaction starts with your note history "
			"and your newest note.",
		[NOTES_CLIENT] =
			"Notes are files in notebooks. Every version of a note stays in the "
			"history. You can read every notebook. You can write only your own "
			"notebook.",
	};
	static char const *const notebooks[] = {
		[NOTES_UPSTREAM] = "The notebook to read: own, a participant's name, or a "
		                   "selector from action notebooks. The default is own. copy "
		                   "needs notebook.",
		[NOTES_CLIENT] = "The notebook to read: own, other, a participant's name, or a "
		                 "selector from action notebooks. other is the notebook of your "
		                 "ask_ai calls. The default is own, or other for copy.",
	};
	size_t const offered = side == NOTES_UPSTREAM ? tool->actions : NOTES_ACTION_ATTACH;
	struct json_object *actions = schema_names(notes_action_names, offered);
	struct json_object *props = jb_object(
		"action", jb_set_or_free(schema_enum(actions, nullptr), "description", schema_text3(
			"add creates the note named file with text. append adds text to the end of "
			"the note, on a new line. replace replaces the note with text. delete "
			"deletes the note. restore writes the note at the commit revision in a new "
			"commit. copy copies the note from notebook to your own notebook. list "
			"lists the current notes. history lists the changes, oldest first. read "
			"reads the note, or the note at the commit revision. diff shows the "
			"changes to the note between the commits from and to. search finds the "
			"lines of the current notes that match pattern. Note names are not "
			"searched. notebooks lists the notebooks of the session.",
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
		"notebook", schema_property("string", notebooks[side]),
		nullptr);
	if (side == NOTES_CLIENT) {
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
		props = jb_set_or_free(props, "thread", schema_property("string",
			"A legacy argument of 32 hexadecimal digits. The notes do not depend on "
			"thread."));
	}
	dstr_auto description = DSTR_INIT;
	if (!dstr_printf(&description, "Participant: %s. %s Prefer small edits. Before "
	                 "replace, read the whole note. Send the whole new text. Copy "
	                 "unrelated passages and evidence word for word. To rewrite most of a "
	                 "note, add a new file such as plan-v2.md. Keep the old file. In the "
	                 "new file, name the old file and the changes. NUL bytes and bytes of "
	                 "invalid UTF-8 show as \xe2\x9f\xa6XX\xe2\x9f\xa7.",
	                 notes_writer(tool->notes), descriptions[side])) {
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
	struct json_object *def = notes_tool_schema(tool, NOTES_CLIENT);
	/* The canonical common directory of the repository names the session,
	 * also for attached worktrees. The definition has a hash of the path
	 * instead of the path. The hash stays the same when askai or the client
	 * restarts. */
	char const *directory = notes_directory(tool->notes);
	unsigned char digest[SHA256_DIGEST_LENGTH];
	if (!SHA256((unsigned char const *)directory, strlen(directory), digest)) {
		json_object_put(def);
		return nullptr;
	}
	char session[2 * sizeof digest];
	util_hex(session, digest, sizeof digest);
	/* Clients use the notebooks action to find the participants that have
	 * notes. A new writer has no branch yet, so the definition names the
	 * participant. */
	return jb_set_or_free(def, "_meta", jb_object(
		"askai", jb_object(
			"session", json_object_new_string_len(session, sizeof session),
			"participant", json_object_new_string(notes_writer(tool->notes)),
			"notebooks", json_object_new_boolean(1),
			nullptr),
		nullptr));
}

struct json_object *
notes_tool_upstream_definition (void const *service)
{
	return notes_tool_schema(service, NOTES_UPSTREAM);
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
