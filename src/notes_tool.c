/** @file notes_tool.c
 *
 * The notes tool (src/notes_tool.h).
 */
#define _GNU_SOURCE /* memmem() */
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <openssl/sha.h>
#include <stdio.h>
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
#include "notes.h"
#include "notes_tool.h"
#include "notice.h"
#include "oid.h"
#include "plugin.h"
#include "schema.h"
#include "shelf.h"
#include "util.h"

/* A fetch value says why git runs on the notes (src/notes.h). The values
 * below FETCH_KIND are kinds, and FETCH_KIND masks them. The bits above
 * FETCH_KIND are flags.
 */
enum notes_tool_fetch : uint16_t {
	FETCH_CREATE,      /* create the shared repository for a write */
	FETCH_CHECK,       /* check the note name before a write */
	FETCH_ADD,         /* commit the note of a write */
	FETCH_LIST,        /* list the notes for list and history */
	FETCH_READ,        /* read a note */
	FETCH_RESUME,      /* list the notes for the prompt of an ask call */
	FETCH_COMPACT,     /* list the notes for a compaction */
	FETCH_KIND = 7,    /* the mask of the kinds */
	FETCH_NEWEST = 8,  /* read the newest note, after FETCH_RESUME or
	                      FETCH_COMPACT */
	FETCH_OTHER = 16,  /* set with FETCH_LIST or FETCH_READ. git reads the
	                      notebook that the call selected. */
	FETCH_COPY = 32,   /* set with FETCH_READ for copy. The note goes to the
	                      caller's own notebook. */
	FETCH_BOOKS = 128, /* list the notebooks of the session */
	FETCH_FORMAT = FETCH_BOOKS | FETCH_CHECK, /* read the object format for
	                                             a notebook without commits */
	FETCH_ATTACH = 64, /* set with FETCH_READ for attach. askai uploads the
	                      note as a file for the turn. */
	FETCH_REPLACE = 256, /* set for replace. The note must exist. */
	FETCH_FILES = 512,   /* set with FETCH_LIST for list. git lists the
	                        current files, not the history. */
	FETCH_REVISION = 1024, /* check the revisions before git reads a note */
	FETCH_DIFF = 2048,    /* compare two revisions of a note */
	FETCH_RESTORE = 4096, /* write an old revision of a note in a new commit */
};

/* A row of notes_tool_fetch_steps[]. cmd is the git command for a fetch value,
 * and out is the step that reads the output of git.
 */
struct notes_tool_fetch_step {
	enum notes_cmd  cmd;
	tool_step_fn   *out;
};

static struct notes_tool_fetch_step const *notes_tool_fetch_step (uint16_t fetch);

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
	NOTE_UPLOAD = 1 << 3,    /* offered only if the upstream takes files */
	NOTE_REVISION = 1 << 4,  /* takes the revision argument */
};

/* The actions of the notes tool. notes_tool_start() looks up the action of a
 * call here, and notes_tool_schema() lists the names.
 */
static struct {
	struct cstr_view      name;
	char const           *verb;  /* begins the commit message of a write */
	uint32_t              flags;
	enum notes_tool_fetch fetch;
} const notes_tool_actions[] = {
#define NOTE(name, verb, flags, fetch) { CSTR_VIEW_INIT(name), verb, flags, fetch }
	NOTE("add",       "Add",     NOTE_WRITE | NOTE_FILE, 0),
	NOTE("replace",   "Replace", NOTE_WRITE | NOTE_FILE, FETCH_REPLACE),
	NOTE("restore",   "Restore", NOTE_WRITE | NOTE_FILE | NOTE_REVISION, FETCH_READ | FETCH_RESTORE),
	NOTE("notebooks", nullptr,   NOTE_OFFSET,            FETCH_BOOKS),
	NOTE("list",      nullptr,   NOTE_OFFSET,            FETCH_LIST | FETCH_FILES),
	NOTE("history",   nullptr,   NOTE_OFFSET,            FETCH_LIST),
	NOTE("read",      nullptr,   NOTE_FILE | NOTE_OFFSET | NOTE_REVISION, FETCH_READ),
	NOTE("diff",      nullptr,   NOTE_FILE | NOTE_OFFSET, FETCH_READ | FETCH_DIFF),
	NOTE("copy",      "Copy",    NOTE_FILE,              FETCH_READ | FETCH_OTHER | FETCH_COPY),
	NOTE("attach",    nullptr,   NOTE_FILE | NOTE_UPLOAD, FETCH_READ | FETCH_ATTACH),
#undef NOTE
};

/* The part of the notes tool on one loop. */
struct notes_tool_loop {
	struct shelf             spare;  /* finished calls, kept for reuse with
	                                    the storage of their output */
	struct notes_tool const *tool;   /* the service */
};

/* The state of one notes call. It is the own word of the call's slot. An
 * ask call has one in its own slot only while git reads the notes for its
 * prompt.
 */
struct notes_tool_call {
	struct buf              file;      /* a write's file part, until git has
	                                      its input */
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
	size_t                  upload;    /* bytes of the file that attach
	                                      uploads */
	size_t                  note;      /* bytes of the note that attach
	                                      uploads */
	size_t                  most;      /* the most bytes of the newest note
	                                      that the prompt shows */
	struct notes_revision   revision;  /* the revisions of a read, diff or
	                                      restore, or the file of a history;
	                                      zeroed for the other calls */
	uint16_t                fetch;     /* why git runs (enum
	                                      notes_tool_fetch) */
	uint16_t                len;       /* bytes in the union */
	uint8_t                 action;    /* the row of notes_tool_actions[] */
	uint8_t                 side;      /* enum notes_side of the caller */
	bool                    stripped;  /* askai cut its notices off the
	                                      text */
	char                    repo[NOTES_REPO];      /* the caller's own
	                                                  notebook */
	char                    notebook[NOTES_REPO];  /* the notebook that the
	                                                  call selected, or ""
	                                                  for repo */
	union {
		char message[NOTES_MESSAGE];  /* the commit message of a write,
		                                 for its result */
		char name[NOTES_NAME];        /* the name of a copy, until its
		                                 file part is written */
		struct oid id;                /* the object id of an attached note,
		                                 until the upload's answer */
	};
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
	*e = nl ? 0 : ENOMEM;
	if (!nl)
		return nullptr;
	*nl = (struct notes_tool_loop){ .tool = service };
	list_init(&nl->spare.list);
	return nl;
}

void
notes_tool_loop_destroy (void **p_part)
{
	struct notes_tool_loop *nl = *p_part;
	if (!nl)
		return;
	*p_part = nullptr;
	for (struct list *spare; (spare = shelf_take(&nl->spare)); ) {
		struct notes_tool_call *call = container_of(spare, struct notes_tool_call, spare);
		dstr_cleanup(&call->out);
		free(call);
	}
	free(nl);
}

/* Returns a notes call that uses the part @p loop and writes to @p text,
 * or to its own output if @p text is nullptr. The call is the last one
 * put on loop->spare, or a new one. Returns nullptr with ENOMEM in @p *e
 * if a new call does not fit in memory.
 */
static struct notes_tool_call *
notes_tool_loop_take (struct notes_tool_loop *loop,
                      struct dstr            *text,
                      int                    *e)
{
	struct list *spare = shelf_take(&loop->spare);
	struct notes_tool_call *call = spare ? container_of(spare, struct notes_tool_call, spare)
	                                     : malloc(sizeof *call);
	*e = call ? 0 : ENOMEM;
	if (call)
		*call = (struct notes_tool_call){ .out = dstr_move(spare ? &call->out : nullptr),
		                                  .loop = loop,
		                                  .text = text ? text : &call->out };
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
	if (atomic_load_explicit(&buf_pressure, memory_order_relaxed) == BUF_EASY) {
		(void)dstr_clear(&call->out);
		shelf_put(&loop->spare, &call->spare);
	} else {
		dstr_cleanup(&call->out);
		free(call);
	}
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

/* Releases the notes call in @p slot, the ask call's own slot, after git
 * has read the notes for the prompt. If @p e is not 0, mcp_slot_fail()
 * ends the ask call with @p e. Otherwise the function calls call->then,
 * which goes on with the ask call.
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
 * compaction and for the newest note, the step runs with no child, as if
 * git had failed. The ask call then goes on without those notes. A call of
 * the notes tool, and the list for the start of an ask call, end with the
 * error.
 */
static void
notes_tool_failed (struct mcp_slot *slot,
                   int              e)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	bool busy = e == EBUSY;
	if (!busy)
		LOG(LOG_WARN, "notes: cannot run git: %s", strerror(e));
	uint16_t kind = call->fetch & FETCH_KIND;
	if (call->fetch & FETCH_NEWEST || kind == FETCH_COMPACT) {
		notes_tool_fetch_step(call->fetch)->out(slot);
		return;
	}
	if (kind == FETCH_RESUME)
		notes_tool_drop(slot);
	if (busy)
		mcp_slot_tell(slot, CSTR_VIEW(CHILD_BUSY), TOOL_FAILED);
	else
		mcp_slot_tellf(slot, TOOL_FAILED, "The notes cannot be used now: %s.", strerror(e));
}

/* Returns the notebook that @p call selected, or else call->repo. */
static char const *
notes_tool_call_notebook (struct notes_tool_call const *call)
{
	return *call->notebook ? call->notebook : call->repo;
}

/* Spawns git for @p fetch in the shared repository and frees @p in, the
 * input of git. With FETCH_OTHER, git reads the notebook that the call
 * selected. Writes always go to call->repo.
 */
static void
notes_tool_run (struct mcp_slot *slot,
                uint16_t         fetch,
                struct buf      *in)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct notes const *notes = call->loop->tool->notes;
	struct loop *loop = mcp_slot_loop(slot);
	char const *repo = fetch & FETCH_OTHER ? notes_tool_call_notebook(call) : call->repo;
	enum notes_cmd cmd = notes_tool_fetch_step(fetch)->cmd;
	/* git removes its own lock files on SIGTERM. notes_lock() removes a
	 * lock file that is older than the last boot. Any other lock file
	 * stays, because its writer can run outside askai. */
	bool writes = notes_tool_writes(cmd);
	if (cmd == NOTES_ADD) {
		int64_t age;
		int e = notes_lock(notes, call->repo, &age);
		if (e)
			LOG(LOG_WARN, "notes: cannot check the lock of %s: %s",
			    notes_selector(call->repo), strerror(e));
	}
	char const *argv[NOTES_ARGV];
	char const *const words[] = {
		repo, call->revision.file.len ? call->revision.file.data : nullptr
	};
	if (fetch & FETCH_REVISION)
		notes_revision_argv(&call->revision, notes, notes_tool_call_notebook(call),
		                    fetch & FETCH_DIFF ? NOTES_DIFF : NOTES_READ, argv);
	else
		notes_argv(notes, cmd, words, argv);
	struct child_cmd const c = {
		.file = "git",
		.argv = argv,
		.envp = notes_env(notes),
		.in = in ? buf_peek(in) : (struct span){},
		.max = NOTES_MAX,
		.deadline = loop->now + NOTES_NS,
		.grace = writes ? NOTES_GRACE : 0,
		/* For the commands that write, the output includes the errors. */
		.flags = writes ? CHILD_STDERR : 0
	};
	call->fetch = fetch;
	int e = 0;
	call->child = mcp_slot_spawn(slot, &c, &e);
	if (in)
		buf_fini(in);
	if (!call->child) {
		notes_tool_failed(slot, e);
		return;
	}
	mcp_slot_park(slot);
}

void
notes_tool_loop_prompt (struct notes_tool_loop *loop,
                        struct mcp_slot        *slot,
                        struct dstr            *prompt,
                        enum notes_tool_prompt  what,
                        size_t                  most,
                        tool_step_fn           *then)
{
	int e = 0;
	struct notes_tool_call *call = notes_tool_loop_take(loop, prompt, &e);
	*mcp_slot_own(slot) = call;
	if (!call) {
		mcp_slot_fail(slot, e);
		return;
	}
	call->then = then;
	call->most = most;
	notes_repo(loop->tool->notes, mcp_slot_principal(slot), NOTES_UPSTREAM, call->repo);
	notes_tool_run(slot, what == NOTES_TOOL_RESUME ? FETCH_RESUME : FETCH_COMPACT,
	               nullptr);
}

/* Returns true if git wrote a list of the notes. A list that askai cut
 * contains the newest notes. @p c is nullptr if git did not start.
 */
static bool
notes_tool_listed (struct child const *c)
{
	return c && (c->flags & CHILD_CUT || child_ok(c));
}

/* Returns true if git exited with status 128. It does so if the
 * repository or the branch of the notes does not exist.
 */
static bool
notes_tool_none (struct child const *c)
{
	return child_status(c) == 128;
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

/* Appends to call->text why git read no notes. Exit status 128 means that
 * the notebook does not exist yet.
 */
static int
notes_tool_call_unread (struct notes_tool_call const *call)
{
	if (!notes_tool_none(call->child))
		return cstr_view_cat(CSTR_VIEW("The notes could not be read."), call->text);
	return cstr_view_cat(CSTR_VIEW("This notebook has no notes yet."), call->text);
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

/* Appends to call->text why git did not write the note. The reasons are a
 * timeout, a race with another writer, a lock file that a dead or hung
 * writer left, and other errors of git. The function logs the last two.
 */
static int
notes_tool_call_unadded (struct notes_tool_call const *call)
{
	struct child const *c = call->child;
	if (c->flags & CHILD_LATE)
		return cstr_view_cat(CSTR_VIEW("Could not write the note: it took "
		                               "too long."), call->text);
	if (notes_tool_raced(c)) {
		struct notes const *notes = call->loop->tool->notes;
		char const *repo = call->repo;
		int64_t age = 0;
		if (notes_lock(notes, repo, &age) || age <= NOTES_STALE)
			return cstr_view_cat(CSTR_VIEW("Could not write the note, "
			                               "because another call changed "
			                               "this notebook at the same "
			                               "time. Read its current state "
			                               "and retry."), call->text);
		LOG(LOG_WARN, "notes: %s/%s.lock is %" PRId64 " s old; remove it if no "
		    "git process uses the repository", notes_directory(notes), repo, age);
		char message[128];
		struct cstr_view const text = util_format_view(message, sizeof message, "Could not "
		                                               "write the note: its notebook has "
		                                               "been locked for %" PRId64 " s.", age);
		return cstr_view_cat(text, call->text);
	}
	struct span out = buf_peek(&c->out);
	char const *why = (char const *)out.ptr;
	LOG(LOG_WARN, "notes: git could not write a note: %.*s",
	    (int)util_clip(why, out.len, 300), why);
	return cstr_view_cat(CSTR_VIEW("Could not write the note."), call->text);
}

/* The size of a note before a write that creates the note. */
static constexpr uint64_t NO_NOTE = UINT64_MAX;

/* Writes to @p msg the commit message for the note that @p call writes.
 * Returns the length of the message, or 0 if it does not fit. @p before is
 * the size of the note before the write, or NO_NOTE.
 */
static size_t
notes_tool_call_message (struct notes_tool_call const *call,
                         uint64_t                      before,
                         char                          msg[NOTES_MESSAGE])
{
	static_assert(sizeof "Restore  from  ( to  bytes)" + 2 * UTIL_DECIMAL + 2 * NOTES_NAME + 2 +
	              NOTES_REPO <= NOTES_MESSAGE);
	uint16_t fetch = notes_tool_actions[call->action].fetch;
	char const *verb = notes_tool_actions[call->action].verb;
	char const *from = fetch & FETCH_RESTORE ? call->revision.from.hex
	                 : fetch & FETCH_COPY ? notes_selector(notes_tool_call_notebook(call)) : "";
	char const *sep = *from ? " from " : "";
	int len = (int)call->part.name_len;
	char const *name = (char const *)buf_peek(&call->file).ptr + call->part.name_at;
	uint64_t after = call->part.data_len;
	int n = before == NO_NOTE
	      ? snprintf(msg, NOTES_MESSAGE, "%s %.*s%s%s (%" PRIu64 " byte%s)", verb, len,
	                 name, sep, from, after, util_s(after))
	      : snprintf(msg, NOTES_MESSAGE, "%s %.*s%s%s (%" PRIu64 " to %" PRIu64 " bytes)",
	                 verb, len, name, sep, from, before, after);
	return n < 0 || n >= NOTES_MESSAGE ? 0 : (size_t)n;
}

/* Runs git to commit the note in call->file of @p slot. The new commit has
 * the parent @p tip. If @p tip is no name, the notebook has no commits,
 * and git first reads the object format for notes_tool_format(). The note
 * had @p before bytes, or there was none if @p before is NO_NOTE.
 */
static void
notes_tool_write (struct mcp_slot  *slot,
                  struct oid const *tip,
                  uint64_t          before)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (!oid_len(tip)) {
		notes_tool_run(slot, FETCH_FORMAT, nullptr);
		return;
	}
	struct span file = buf_peek(&call->file);
	struct buf in = {};
	call->len = notes_tool_call_message(call, before, call->message);
	struct span msg = { (unsigned char *)call->message, call->len };
	int e = !msg.len ? EINVAL : notes_add_input(&in, call->repo, tip, msg, file);
	buf_fini(&call->file);
	if (e) {
		buf_fini(&in);
		notes_tool_end(slot, e, true);
		return;
	}
	notes_tool_run(slot, FETCH_ADD, &in);
}

/* Reads the object format of the repository from the output of git. Then
 * the function writes the first commit of the notebook with a parent of
 * zeros. That parent makes the commit a root commit even if another writer
 * creates the branch after the name check.
 */
static void
notes_tool_format (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct span out = buf_peek(&call->child->out);
	struct cstr_view const format = { (char const *)out.ptr, out.len };
	struct oid const zero = oid_zero(oid_format(cstr_view_trim_end(format)));
	if (!child_ok(call->child) || !oid_len(&zero)) {
		int e = cstr_view_cat(CSTR_VIEW("Cannot determine the repository "
		                                "object format."), call->text);
		notes_tool_end(slot, e, true);
		return;
	}
	child_destroy(&call->child);
	notes_tool_write(slot, &zero, NO_NOTE);
}

/* Handles the end of git init for a write. The function writes the note,
 * or ends the call if git failed.
 */
static void
notes_tool_created (struct mcp_slot *slot)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (child_ok(call->child) ||
	    (notes_tool_raced(call->child) && notes_exists(call->loop->tool->notes))) {
		child_destroy(&call->child);
		notes_tool_write(slot, &(struct oid){}, NO_NOTE);
		return;
	}
	notes_tool_end(slot, notes_tool_call_unadded(call), true);
}

/* Ends the write in @p slot after it saved the note or left the note
 * unchanged. The caller's own notebook exists then, and mcp_slot_noted()
 * records that. If @p e is not 0, the call fails with @p e
 * (notes_tool_end()).
 */
static void
notes_tool_wrote (struct mcp_slot *slot,
                  int              e)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	(void)mcp_slot_noted(slot);
	if (!e && call->stripped)
		e = cstr_view_cat(CSTR_VIEW(" askai left out the [askai: ...] lines at the end of "
		                            "the text. askai adds those lines to tool results, and "
		                            "they are not part of the note."), call->text);
	notes_tool_end(slot, e, false);
}

/* Returns true if @p note, the note that NOTES_CHECK found, already has
 * the bytes of call->file. The function then answers the replace or
 * restore in @p slot without a commit. The answer names @p tip, the newest
 * commit of the branch. If the bytes cannot be compared, the function logs
 * a warning and returns false, and the write goes on.
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
	e = dstr_printf(call->text, "Unchanged: %.*s already has %s %" PRIu64 " byte%s, at "
	                "commit %s. No commit was made.", (int)jb_utf8(name, call->part.name_len),
	                name, n == 1 ? "this" : "these", n, util_s(n), tip->hex) ? 0 : ENOMEM;
	notes_tool_wrote(slot, e);
	return true;
}

/* Handles the output of NOTES_CHECK for a write. add and copy accept only
 * a name that is neither a note nor a directory of notes. No directory in
 * the name can be a note. replace accepts only the name of an existing
 * note. A replace or a restore that leaves the bytes of the note unchanged
 * makes no commit.
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
	uint16_t fetch = notes_tool_actions[call->action].fetch;
	if (fetch & FETCH_REPLACE)
		r = r == EEXIST ? 0 : r ? r : ENOENT;
	if (fetch & FETCH_RESTORE) {
		if (r == EEXIST)
			r = 0;
		if (!r && !oid_equal(&tip, &call->revision.tip)) {
			int e = cstr_view_cat(CSTR_VIEW("The notebook changed during "
			                                "restore. Read its current "
			                                "state and retry."), call->text);
			notes_tool_end(slot, e, true);
			return;
		}
	}
	if (!r) {
		if (before == NO_NOTE || !notes_tool_unchanged(slot, &note, &tip)) {
			child_destroy(&call->child);
			notes_tool_write(slot, &tip, before);
		}
		return;
	}
	struct cstr_view const message =
		r == EEXIST ? CSTR_VIEW("A note of that name exists. Use action replace to "
		                        "update it, or add under another name.")
		: r == ENOENT ? CSTR_VIEW("No note has that name. Use action add to create it.")
		: r == EISDIR ? CSTR_VIEW("That name is a directory of notes. Add the note "
		                          "under another name.")
		: r == ENOTDIR ? CSTR_VIEW("A directory in that name is a note. Add the note "
		                           "under another name.")
		: (struct cstr_view){};
	notes_tool_end(slot, message.data ? cstr_view_cat(message, call->text)
	                                  : notes_tool_call_unread(call), true);
}

/* Handles the end of git fast-import. The result contains the id of the
 * new commit and the commit message, or the reason why git did not save
 * the note.
 */
static void
notes_tool_added (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	if (!child_ok(call->child)) {
		notes_tool_end(slot, notes_tool_call_unadded(call), true);
		return;
	}
	struct oid commit;
	if (notes_add_out(buf_peek(&call->child->out), &commit))
		LOG(LOG_WARN, "notes: git fast-import gave no commit id");
	struct cstr_view const p[] = {
		oid_len(&commit) ? CSTR_VIEW("Saved in commit ") : CSTR_VIEW("Saved"),
		oid_view(&commit),
		CSTR_VIEW(": "),
		{ call->message, jb_utf8(call->message, call->len) },
		notes_tool_actions[call->action].fetch & (FETCH_REPLACE | FETCH_RESTORE)
		? CSTR_VIEW(". Previous versions remain in history.") : CSTR_VIEW("."),
	};
	notes_tool_wrote(slot, cstr_view_join(p, array_size(p), UINT64_MAX, CSTR_VIEW(""),
	                                      CSTR_VIEW(""), call->text));
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
	if (cut && cstr_view_cat(CSTR_VIEW("[The oldest notes are not in "
	                                   "this list.]\n"), call->text))
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
		    !(entry.lines == NOTES_BINARY ? dstr_cat(d, "  (binary)")
		      : dstr_printf(d, "  (%" PRIu64 " line%s)", entry.lines, util_s(entry.lines))) ||
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

/* Handles the output of git for list and history. */
static void
notes_tool_got_list (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	bool ok = notes_tool_listed(c);
	if (!ok) {
		notes_tool_end(slot, notes_tool_call_unread(call), true);
		return;
	}
	bool files = call->fetch & FETCH_FILES;
	struct dstr *d = call->text;
	int e = dstr_printf(d, "Notebook %s — %s:\n\n",
	                    notes_selector(notes_tool_call_notebook(call)),
	                    files ? "current files (bytes, blob)"
	                          : "history, oldest first (lines added, commit, message)")
	      ? 0 : ENOMEM;
	struct span out = buf_peek(&c->out);
	if (!files) {
		struct span newest;
		e = e ? e : notes_tool_history(slot, out, c->flags & CHILD_CUT, &newest);
	} else {
		while (!e && out.len) {
			struct notes_file file;
			int r = notes_file_next(&out, &file);
			if (r) {
				if (!(c->flags & CHILD_CUT))
					e = cstr_view_cat(CSTR_VIEW("Cannot decode the notebook "
					                            "file list."), call->text);
				ok = c->flags & CHILD_CUT;
				break;
			}
			char const *name = (char const *)file.name.ptr;
			if (!dstr_printf(d, "%.*s  (%" PRIu64 " bytes, %s)\n",
			                 (int)jb_utf8(name, file.name.len), name, file.bytes,
			                 file.id.hex))
				e = ENOMEM;
		}
		if (!e && c->flags & CHILD_CUT)
			e = cstr_view_cat(CSTR_VIEW("[The file list exceeded the capture limit and "
			                            "is incomplete.]\n"), call->text);
		loop_work(mcp_slot_loop(slot), LOOP_COPY, buf_peek(&c->out).len);
	}
	notes_tool_end(slot, e, !ok);
}

/* Appends the selector of the caller's own notebook to call->text. The
 * notebook does not need to have commits.
 */
static int
notes_tool_call_identity (struct notes_tool_call const *call)
{
	return dstr_printf(call->text, "Self (writable): %s\n",
	                   notes_selector(call->repo)) ? 0 : ENOMEM;
}

/* Handles the output of git for notebooks. The result lists the selector
 * of each notebook in the session. git lists loose and packed refs.
 */
static void
notes_tool_got_books (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	if (!notes_tool_listed(c)) {
		notes_tool_end(slot, notes_tool_call_unread(call), true);
		return;
	}
	struct span out = buf_peek(&c->out);
	int e = cstr_view_cat(CSTR_VIEW("Session notebooks (use a selector as "
	                                "notebook):\n"), call->text);
	if (!e)
		e = notes_tool_call_identity(call);
	struct cstr_view const own = cstr_view(notes_selector(call->repo));
	while (!e && out.len) {
		unsigned char const *end = memchr(out.ptr, '\n', out.len);
		if (!end)
			break;
		struct cstr_view const name = { (char const *)out.ptr, (size_t)(end - out.ptr) };
		if (!cstr_view_equal(name, own) && !notes_select(name, nullptr))
			e = dstr_printf(call->text, "%.*s (read-only)\n", (int)name.len, name.data)
			  ? 0 : ENOMEM;
		size_t n = name.len + 1;
		out.ptr += n;
		out.len -= n;
	}
	if (!e && c->flags & CHILD_CUT)
		e = cstr_view_cat(CSTR_VIEW("[The notebook list is incomplete.]\n"), call->text);
	notes_tool_end(slot, e, false);
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
	        "[askai: the note is not UTF-8 text: %zu bytes.  Each byte that is not "
	        "part of UTF-8 text, and each NUL, is shown as %sXX%s, its value in "
	        "hexadecimal, and a %s of the text as %s%s.  The note itself is not "
	        "changed.]\n\n", note.len, view_open, view_close, view_open, view_open,
	        view_open));
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

static void notes_tool_got_copy (struct mcp_slot *slot, struct span text);
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
		int e = cstr_view_cat(CSTR_VIEW("Cannot use that revision in this "
		                                "notebook. Use a full commit ID "
		                                "from its history; the notebook "
		                                "must exist and the revision must "
		                                "belong to it."), call->text);
		notes_tool_end(slot, e, true);
		return;
	}
	child_destroy(&call->child);
	struct buf in = {};
	int e = 0;
	if (notes_revision_ready(&call->revision) && !(call->fetch & FETCH_DIFF))
		e = notes_read_input(&in, call->revision.from.hex, util_text_view(&call->revision.file));
	if (e) {
		buf_fini(&in);
		notes_tool_end(slot, e, true);
		return;
	}
	notes_tool_run(slot, call->fetch, &in);
}

/* Handles the output of git for diff. */
static void
notes_tool_got_diff (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct child const *c = call->child;
	if (!notes_tool_listed(c)) {
		notes_tool_end(slot, notes_tool_call_unread(call), true);
		return;
	}
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
		  : jb_is_text(diff) ? cstr_view_cat(diff, call->text)
		  : notes_tool_view(d, diff, SIZE_MAX, nullptr);
	if (!e && c->flags & CHILD_CUT)
		e = cstr_view_cat(CSTR_VIEW("\n[The diff exceeded the capture limit "
		                            "and is incomplete.]\n"), call->text);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, out.len);
	notes_tool_end(slot, e, false);
}

/* Handles the output of git cat-file. The function answers read, or goes
 * on with copy, restore or attach.
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
		int e = r == ENOENT ? cstr_view_cat(CSTR_VIEW("No note has that name."), call->text)
		      : r == EISDIR ? cstr_view_cat(CSTR_VIEW("That name is a "
		                                              "directory of notes. "
		                                              "list shows the notes "
		                                              "in it."), call->text)
		      : notes_tool_call_unread(call);
		notes_tool_end(slot, e, true);
		return;
	}
	if (call->fetch & (FETCH_COPY | FETCH_RESTORE)) {
		notes_tool_got_copy(slot, text);
		return;
	}
	if (call->fetch & FETCH_ATTACH) {
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
	e = e ? e : jb_is_text(body) ? cstr_view_cat(body, call->text)
	      : notes_tool_view(call->text, body, SIZE_MAX, nullptr);
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
	bool ok = view ? dstr_printf(call->text, "The note, %zu bytes, is not UTF-8 text, so "
	                             "its view, %zu bytes, about %zu tokens, in which each "
	                             "other byte is %sXX%s, follows the results of this turn "
	                             "as a file.", len, bytes, bytes / 4, view_open, view_close)
	               : dstr_printf(call->text, "The note, %zu bytes, about %zu tokens, "
	                             "follows the results of this turn as a file.", len,
	                             bytes / 4);
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
			e = dstr_printf(call->text, "The note is too large to attach: its file "
			                "would have %zu bytes, and askai uploads %d MiB at most.",
			                len, NOTES_MAX >> 20) ? 0 : ENOMEM;
			notes_tool_end(slot, e, true);
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
		e = notes_tool_history(slot, buf_peek(&c->out), c->flags & CHILD_CUT, &newest);
	struct cstr_view const name = { (char const *)newest.ptr, newest.len };
	/* The name comes from git. git reads the note only if add would accept
	 * the name as it is. */
	struct cstr_view checked = name;
	bool named = !notes_name(&checked) && checked.len == name.len;
	if (!e && named) {
		struct cstr_view const p[] = {
			CSTR_VIEW("\n[askai: the current text of your most recently written note, "),
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
	notes_tool_run(slot, call->fetch | FETCH_NEWEST, &in);
}

/* Appends the newest note, which git read for the prompt, to call->text.
 * If git could not read the note, the function appends the line "[askai:
 * the note could not be read.]" instead. A note longer than call->most
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
	                              "\n[askai: bytes 0 to %zu of %zu; read the note with "
	                              "\"offset\": %zu for the rest.]", body.len, len, body.len)
	                            : (struct cstr_view){};
	struct cstr_view const p[] = {
		r ? CSTR_VIEW("[askai: the note could not be read.]") : body,
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
	notes_tool_prompt_list(slot, CSTR_VIEW("\n\n[askai: your session note history, "
	                                       "oldest first; repeated names are updates. "
	                                       "Use notes action list for current files and "
	                                       "notebooks to discover the other "
	                                       "participants:]\n\n"));
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
		notes_tool_prompt_list(slot, CSTR_VIEW("\n\n[askai: your note history, oldest "
		                                       "first; repeated names are updates:]\n\n"));
		return;
	}
	notes_tool_then(slot, cstr_view_cat(CSTR_VIEW("\n\n[askai: your "
	                                              "notes could not be "
	                                              "read.]\n"), call->text));
}

/* These rows of notes_tool_fetch_steps[] follow the rows of the kinds. */
enum { STEP_FORMAT = FETCH_COMPACT + 1, STEP_BOOKS, STEP_FILES, STEP_NEWEST };

static struct notes_tool_fetch_step const notes_tool_fetch_steps[] = {
	[FETCH_CREATE]  = { NOTES_CREATE, notes_tool_created },
	[FETCH_CHECK]   = { NOTES_CHECK,  notes_tool_checked },
	[FETCH_ADD]     = { NOTES_ADD,    notes_tool_added },
	[FETCH_LIST]    = { NOTES_LIST,   notes_tool_got_list },
	[FETCH_READ]    = { NOTES_READ,   notes_tool_got_note },
	[FETCH_RESUME]  = { NOTES_LIST,   notes_tool_resumed },
	[FETCH_COMPACT] = { NOTES_LIST,   notes_tool_compacted },
	[STEP_FORMAT]   = { NOTES_FORMAT, notes_tool_format },
	[STEP_BOOKS]    = { NOTES_BOOKS,  notes_tool_got_books },
	[STEP_FILES]    = { NOTES_FILES,  notes_tool_got_list },
	[STEP_NEWEST]   = { NOTES_READ,   notes_tool_newest },
};

/* Returns the row of notes_tool_fetch_steps[] for @p fetch. The flags of
 * the last four rows take precedence over the kind.
 */
static struct notes_tool_fetch_step const *
notes_tool_fetch_step (uint16_t fetch)
{
	return &notes_tool_fetch_steps[fetch == FETCH_FORMAT ? STEP_FORMAT
	                               : fetch & FETCH_BOOKS ? STEP_BOOKS
	                               : fetch & FETCH_FILES ? STEP_FILES
	                               : fetch & FETCH_NEWEST ? STEP_NEWEST
	                               : fetch & FETCH_KIND];
}

void
notes_tool_step (struct mcp_slot *slot)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	/* Only attach reaches this step without a child. The step then handles
	 * the answer to the upload. */
	(!call->child ? notes_tool_uploaded
	 : (call->fetch & FETCH_REVISION) && !notes_revision_ready(&call->revision)
	 ? notes_tool_revision_step
	 : call->fetch & FETCH_DIFF ? notes_tool_got_diff
	 : notes_tool_fetch_step(call->fetch)->out)(slot);
}

static_assert(TOOL_TEXT_MAX == 4U << 20 && NOTES_NAME == 255 && HEXID_DIGITS == 32,
              "update the texts in this file that say 4 MiB, 255 bytes or 32 hexadecimal "
              "digits");

/* The message for a file argument that is not a valid name. */
static struct cstr_view const bad_name =
	CSTR_VIEW_INIT("The file argument must be a file name of 1 to 255 bytes, such as "
	               "plan.md or findings/locks.md. It cannot have control characters or "
	               "empty parts, and no part can be ., .., .git or git~1, in any case "
	               "and also with spaces or dots after it.");

/* Runs git to check the name @p name of the note in call->file before the
 * write, or to create the shared repository if @p initialized is false. If
 * @p e is not 0, the function ends the call with @p e instead.
 */
static void
notes_tool_check (struct mcp_slot  *slot,
                  struct cstr_view  name,
                  bool              initialized,
                  int               e)
{
	struct notes_tool_call const *call = *mcp_slot_own(slot);
	struct buf in = {};
	uint16_t fetch = FETCH_CREATE;
	if (!e && initialized) {
		fetch = FETCH_CHECK;
		e = notes_check_input(&in, call->repo, name);
	}
	if (e) {
		buf_fini(&in);
		notes_tool_end(slot, e, true);
		return;
	}
	notes_tool_run(slot, fetch, &in);
}

/* Starts add or replace of the note @p name with the arguments @p args.
 * The note goes to the caller's own notebook. git creates the shared
 * repository first if it does not exist. A turn's text is cut before its
 * trailing notices (notice_kept()), because askai adds those notices only
 * to the results of a turn's calls.
 */
static void
notes_tool_add (struct mcp_slot    *slot,
                struct cstr_view    name,
                struct json_object *args)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	bool initialized = notes_exists(call->loop->tool->notes);
	bool turn = call->side == NOTES_UPSTREAM;
	size_t most = call->loop->tool->files_inline;
	struct json_object *content = jb_get(args, "content");
	struct cstr_view const text = jb_view(jb_get(args, "text"));
	size_t kept = turn ? notice_kept(text.data, text.len) : text.len;
	struct datauri data = {};
	char big[96];
	struct cstr_view const problem =
		notes_name(&name) ? bad_name
		: !content ? (!text.len || text.len > TOOL_TEXT_MAX || memchr(text.data, '\0', text.len)
		              ? CSTR_VIEW("The text argument must be a string of 1 byte to 4 MiB, "
		                          "and no NUL.")
		              : !kept ? CSTR_VIEW("The text has only askai's [askai: ...] lines, "
		                                  "which askai adds to tool results. Give the text "
		                                  "of the note itself.")
		              : (struct cstr_view){})
		: turn ? CSTR_VIEW("Only a client hands askai a file as content.")
		: text.data ? CSTR_VIEW("Give the text or the content of a note, not both.")
		: datauri_init(&data, jb_view(content))
		? CSTR_VIEW("The content argument must be a data: URI, which the client fills "
		            "in with a file that the user gave it.")
		: data.size > most
		? util_format_view(big, sizeof big, "The file has more than %zu bytes, which "
		                   "this server takes at most in one request.", most)
		: (struct cstr_view){};
	if (problem.data) {
		mcp_slot_tell(slot, problem, TOOL_FAILED);
		return;
	}
	if (!initialized && (notes_tool_actions[call->action].fetch & FETCH_REPLACE)) {
		mcp_slot_tell(slot, CSTR_VIEW("No note has that name. Use action add to "
		                              "create it."), TOOL_FAILED);
		return;
	}
	int e;
	if (content) {
		e = notes_add_head(&call->file, name, data.size, &call->part);
		if (!e)
			datauri_decode(&data, buf_peek(&call->file).ptr + call->part.data_at);
		loop_work(mcp_slot_loop(slot), LOOP_COPY, data.size);
	} else {
		call->stripped = kept < text.len;
		e = notes_add_file(&call->file, name, cstr_view_slice(text, 0, kept), &call->part);
	}
	notes_tool_check(slot, name, initialized, e);
}

/* Writes @p text to a note in the caller's own notebook. A copy uses the
 * name in call->name. A restore uses the file name of its revision, and
 * fails if @p text is longer than TOOL_TEXT_MAX bytes.
 */
static void
notes_tool_got_copy (struct mcp_slot *slot,
                     struct span      text)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	struct cstr_view to = { call->name, call->len };
	if (call->fetch & FETCH_RESTORE) {
		if (text.len > TOOL_TEXT_MAX) {
			int e = cstr_view_cat(CSTR_VIEW("That revision exceeds the 4 MiB note size "
			                                "limit; it was not restored."), call->text);
			notes_tool_end(slot, e, true);
			return;
		}
		to = util_text_view(&call->revision.file);
	}
	struct cstr_view const bytes = { (char const *)text.ptr, text.len };
	int e = notes_add_file(&call->file, to, bytes, &call->part);
	loop_work(mcp_slot_loop(slot), LOOP_COPY, text.len);
	child_destroy(&call->child);
	notes_tool_check(slot, to, notes_exists(call->loop->tool->notes), e);
}

/* Runs git to read the note @p name. @p fetch is the fetch of read,
 * attach or copy. For copy, notes_tool_got_copy() then writes the note
 * byte for byte to the caller's own notebook. The name of the copy is the
 * as argument in @p args, or else @p name. The text of a copied note does
 * not pass through the caller.
 */
static void
notes_tool_read (struct mcp_slot    *slot,
                 struct cstr_view    name,
                 struct json_object *args,
                 uint16_t            fetch)
{
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (notes_name(&name)) {
		mcp_slot_tell(slot, bad_name, TOOL_FAILED);
		return;
	}
	if (fetch & FETCH_COPY) {
		struct json_object *as = jb_get(args, "as");
		struct cstr_view to = jb_view(as);
		if (!as) {
			to = name;
		} else if (notes_name(&to)) {
			mcp_slot_tell(slot, CSTR_VIEW("The as argument must be a file name, as the "
			                              "file argument is."), TOOL_FAILED);
			return;
		}
		call->len = to.len;
		memcpy(call->name, to.data, to.len);
	}
	struct buf in = {};
	int e = notes_read_input(&in, notes_tool_call_notebook(call), name);
	if (e) {
		buf_fini(&in);
		notes_tool_end(slot, e, true);
		return;
	}
	notes_tool_run(slot, fetch, &in);
}

/* Starts diff, restore, read with a revision, or history with a file. The
 * note is @p file. @p revisions are the values of the arguments from and
 * to for diff, or of the argument revision and nullptr otherwise.
 */
static void
notes_tool_revision (struct mcp_slot          *slot,
                     struct cstr_view          file,
                     struct json_object *const revisions[2],
                     uint16_t                  fetch)
{
	struct oid ids[2] = {};
	bool bad = !revisions[0] && (fetch & (FETCH_DIFF | FETCH_RESTORE));
	for (size_t i = 0; !bad && i < array_size(ids); ++i) {
		struct cstr_view const v = jb_view(revisions[i]);
		bad = revisions[i] && oid_init(&ids[i], v) &&
		      !cstr_view_equal(v, CSTR_VIEW("current"));
	}
	if (bad) {
		mcp_slot_tell(slot, CSTR_VIEW("Use a full commit ID from action history, or "
		                              "current. diff requires from; restore requires "
		                              "revision."), TOOL_FAILED);
		return;
	}
	struct notes_tool_call *call = *mcp_slot_own(slot);
	if (notes_revision_init(&call->revision, file, &ids[0], &ids[1])) {
		mcp_slot_tell(slot, bad_name, TOOL_FAILED);
		return;
	}
	if ((fetch & FETCH_KIND) != FETCH_LIST)
		fetch |= FETCH_REVISION;
	notes_tool_run(slot, fetch, nullptr);
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
	int e = 0;
	struct notes_tool_call *call = notes_tool_loop_take(mcp_slot_part(slot), nullptr, &e);
	*mcp_slot_own(slot) = call;
	if (!call) {
		mcp_slot_fail(slot, e);
		return;
	}
	call->side = mcp_slot_from_client(slot) ? NOTES_CLIENT : NOTES_UPSTREAM;
	struct notes const *notes = call->loop->tool->notes;
	notes_repo(notes, mcp_slot_principal(slot), call->side, call->repo);
	bool uploads = call->side == NOTES_UPSTREAM && call->loop->tool->attach;
	struct cstr_view const action = jb_view(jb_get(args, "action"));
	size_t which = 0;
	while (which < array_size(notes_tool_actions) &&
	       !cstr_view_equal(action, notes_tool_actions[which].name))
		++which;
	if (which == array_size(notes_tool_actions) ||
	    ((notes_tool_actions[which].flags & NOTE_UPLOAD) && !uploads)) {
		dstr_auto why = DSTR_INIT;
		bool ok = dstr_cat(&why, "The action must be one of:");
		for (size_t i = 0; ok && i < array_size(notes_tool_actions); ++i)
			if (uploads || !(notes_tool_actions[i].flags & NOTE_UPLOAD))
				ok = dstr_printf(&why, " %.*s", (int)notes_tool_actions[i].name.len,
				                 notes_tool_actions[i].name.data);
		if (!ok || !dstr_cat_n(&why, ".", 1))
			mcp_slot_fail(slot, ENOMEM);
		else
			mcp_slot_tell(slot, cstr_view_dstr(&why), TOOL_FAILED);
		return;
	}
	call->action = which;
	uint32_t flags = notes_tool_actions[which].flags;
	uint16_t fetch = notes_tool_actions[which].fetch;
	bool diff = fetch & FETCH_DIFF;
	struct json_object *revision = jb_get(args, "revision");
	struct json_object *from = jb_get(args, "from");
	struct json_object *to = jb_get(args, "to");
	if ((revision && !(flags & NOTE_REVISION)) || ((from || to) && !diff)) {
		mcp_slot_tell(slot, CSTR_VIEW("revision applies to read and restore; from and to "
		                              "apply to diff. Use history to find commit IDs."),
		              TOOL_FAILED);
		return;
	}
	if (call->side == NOTES_CLIENT) {
		struct json_object *thread = jb_get(args, "thread");
		if (thread && !hexid_valid(jb_view(thread))) {
			mcp_slot_tell(slot, CSTR_VIEW("The optional thread must be 32 hexadecimal "
			                              "digits. It does not select the notes "
			                              "session."), TOOL_FAILED);
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
		if (call->side == NOTES_UPSTREAM) {
			mcp_slot_tell(slot, CSTR_VIEW("For copy, give the source notebook selector "
			                              "from action notebooks."), TOOL_FAILED);
			return;
		}
		other = true;
	}
	if (other) {
		enum notes_side side = call->side == NOTES_CLIENT ? NOTES_UPSTREAM : NOTES_CLIENT;
		notes_repo(notes, mcp_slot_principal(slot), side, call->notebook);
	} else if (notebook && !cstr_view_equal(selected, CSTR_VIEW("own")) &&
	           notes_select(selected, call->notebook)) {
		mcp_slot_tell(slot, CSTR_VIEW("The notebook must be own or a selector returned by "
		                              "action notebooks (participant/principal/side)."),
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
	struct json_object *file = jb_get(args, "file");
	struct cstr_view const name = jb_view(file);
	uint16_t const selection = *call->notebook ? FETCH_OTHER : 0;
	if (revision || (fetch & (FETCH_DIFF | FETCH_RESTORE)) || (fetch == FETCH_LIST && file)) {
		struct json_object *const revisions[] = { diff ? from : revision, to };
		notes_tool_revision(slot, name, revisions, fetch | selection);
		return;
	}
	if (flags & NOTE_WRITE) {
		notes_tool_add(slot, name, args);
		return;
	}
	if (!notes_exists(notes)) {
		/* This answer does not fail the call. A turn's offset applies to it,
		 * and a client's offset does not. */
		struct cstr_view const none = CSTR_VIEW("No notes have been added to this "
		                                        "session yet.");
		if (fetch == FETCH_BOOKS)
			notes_tool_end(slot, notes_tool_call_identity(call), false);
		else if (call->side == NOTES_CLIENT)
			mcp_slot_tell(slot, none, 0);
		else
			notes_tool_end(slot, cstr_view_cat(none, call->text), false);
		return;
	}
	if (flags & NOTE_FILE)
		notes_tool_read(slot, name, args, fetch | selection);
	else
		notes_tool_run(slot, fetch | selection, nullptr);
}

/* Returns the definition of the notes tool for the side @p side. Only the
 * definition for clients contains the content and thread arguments. The
 * definition for the upstream contains the action attach if tool->attach
 * is set, which means that the upstream accepts files.
 */
static struct json_object *
notes_tool_schema (struct notes_tool const *tool,
                   enum notes_side          side)
{
	static char const *const descriptions[] = {
		[NOTES_UPSTREAM] =
			"Your persistent notebook in this shared Git session. Add findings, "
			"decisions and plans as named files. replace can update your own "
			"existing files; Git preserves every previous version. All instances sharing the repository "
			"can read every notebook, but you can write only your own. Use action "
			"notebooks to discover the others, then list or read with that notebook "
			"selector. Your notes are included after compaction; read older notes "
			"when needed. Non-UTF-8 bytes are displayed as \xe2\x9f\xa6XX\xe2\x9f\xa7.",
		[NOTES_CLIENT] =
			"Your persistent notebook in this shared Git session. Notes are named "
			"files; add creates one, replace updates one while preserving history. All instances sharing the "
			"repository can read every notebook; only your own can be written. "
			"Use action notebooks to discover selectors, then list or read one. "
			"copy imports a note into your own notebook without changing its text. "
			"content keeps an attached user file. Threads do not partition notes. "
			"Non-UTF-8 bytes are displayed as \xe2\x9f\xa6XX\xe2\x9f\xa7.",
	};
	/* A turn can attach a note only if its upstream accepts files. */
	bool files = side == NOTES_UPSTREAM && tool->attach;
	size_t most = tool->files_inline;
	struct json_object *actions = json_object_new_array();
	for (size_t i = 0; actions && i < array_size(notes_tool_actions); ++i)
		if (files || !(notes_tool_actions[i].flags & NOTE_UPLOAD))
			actions = jb_push_or_free(actions, jb_string(notes_tool_actions[i].name));
	struct json_object *props = jb_object(
		"action", jb_set_or_free(schema_enum(actions, nullptr), "description", schema_text3(
			"add a note, replace an existing own note, list current files "
			"with their byte sizes and blob ids, show history oldest first "
			"with commit IDs and messages (line counts are lines added by each change), "
			"read a note, "
			"diff two versions, or restore an old version in a new own commit "
			"unless the note already has its bytes. "
			"To recover lost text: history with file, diff with file and from, then "
			"read with revision for selected text or restore with revision for the whole file. "
			"Use copy to import a note into yours, or discover all session notebooks with "
			"notebooks.", files ? " attach gives a note as a file." : "", "")),
		"file", jb_set_or_free(schema_property("string", nullptr), "description", schema_text3(
			"For add, replace, restore, read, diff", files ? ", copy and attach" : " and copy",
			": the file name of the note, at most 255 bytes. It can have "
			"directories, such as findings/locks.md. Its extension, such "
			"as .md or .txt, shows the format of the text. Optional for history "
			"to show only changes to that exact file name.")),
		"revision", schema_property("string",
			"For read: full commit ID from the selected notebook's history, "
			"or current (default). Required for restore, which copies the exact "
			"historical bytes into your own notebook in a new commit, preserving "
			"all intervening history; if the note already has those bytes, it "
			"makes no commit. No Git revision expressions."),
		"from", schema_property("string",
			"Required for diff: the older full commit ID from this notebook's history, "
			"or current. The diff compares that file at from against to."),
		"to", schema_property("string",
			"For diff: full commit ID from the same notebook, or current (default). "
			"Both revisions are resolved against one notebook snapshot. Output names "
			"the resolved IDs; reuse them when continuing with offset."),
		"as", schema_property("string",
			"For copy: the file name of the copy in your notebook. The "
			"default is the name of the note."),
		"text", schema_property("string",
			"For add and replace: the text of the note, at most 4 MiB."),
		"offset", schema_count(0,
			"For list, history, notebooks, read and diff: the byte of the output "
			"to start from, to read on past where it was cut."),
		"notebook", jb_set_or_free(schema_property("string", nullptr), "description", schema_text3(
			"For list, history, read, diff and copy", files ? ", and attach" : "",
			": own (default), or a participant/principal/side selector "
			"from action notebooks. That action identifies your writable "
			"notebook even before your first note. For copy, explicitly "
			"select the source. add, replace and restore write only your own notebook.")),
		nullptr);
	if (side == NOTES_CLIENT) {
		/* The client program, not the model, fills in content with a
		 * file (the extension io.modelcontextprotocol/files). */
		props = jb_set_or_free(props, "content", jb_object(
			"type", json_object_new_string("string"),
			"format", json_object_new_string("uri"),
			"description", json_object_new_string(
				"For add and replace, instead of text: a file that the user gave, "
				"which your client attaches as it is. Do not write it yourself."),
			"x-mcp-file", jb_object("maxSize", json_object_new_int64((int64_t)most),
			                        nullptr),
			nullptr));
		props = jb_set_or_free(props, "thread", schema_property("string",
			"Optional legacy ask-chain id (32 hexadecimal digits). Notes belong "
			"to the configured shared repository, independent of this value."));
	}
	dstr_auto description = DSTR_INIT;
	if (!dstr_printf(&description, "Participant: %s. %s "
	                 "Prefer small, controlled, incremental edits. Before using replace, "
	                 "read the complete current file; send the complete updated file "
	                 "while preserving unrelated passages, findings and evidence verbatim. "
	                 "If an edit would overhaul most of a document, prefer add with a "
	                 "new versioned filename, keep the previous file, and identify the "
	                 "predecessor and what changed in the new document. Preserve existing "
	                 "structure and useful detail across session resumes and model changes.%s",
	                 notes_writer(tool->notes),
	                 descriptions[side], files
	                 ? " attach gives you a whole note as a file after the results "
	                   "of the turn, where read shows a large note in parts." : "")) {
		json_object_put(props);
		return nullptr;
	}
	return jb_object(
		"name", json_object_new_string("notes"),
		"description", jb_string(cstr_view_dstr(&description)),
		"inputSchema", schema_object(props, &CSTR_VIEW("action"), 1),
		"annotations", jb_object(
			"readOnlyHint", json_object_new_boolean(0),
			"destructiveHint", json_object_new_boolean(0),
			"idempotentHint", json_object_new_boolean(0),
			"openWorldHint", json_object_new_boolean(0),
			nullptr),
		nullptr);
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
	char session[2 * sizeof digest + 1];
	util_hex(session, digest, sizeof digest);
	session[sizeof session - 1] = '\0';
	/* Clients use the notebooks action to find the participants that have
	 * notes. A new writer has no branch yet, so the definition names the
	 * participant. */
	return jb_set_or_free(def, "_meta", jb_object(
		"askai", jb_object(
			"session", json_object_new_string_len(session, sizeof session - 1),
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
