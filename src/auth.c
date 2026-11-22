/** @file auth.c
 *
 * The renewed credential of the upstream.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "auth.h"
#include "bearer.h"
#include "chatgpt.h"
#include "clk.h"
#include "cstr_view.h"
#include "flag.h"
#include "util.h"

/* The value of the range FLAG_WHEN of auth.flags while a read is under
 * way.
 */
enum : uint32_t {
	AUTH_BUSY = FLAG_DURING,  /* a blocker has the read, or its message is
	                             on the way back */
};

/* A read of the credential file on a blocker, with what it needs and what
 * it gives. It is no part of struct auth, because a blocker can still run
 * it when the auth is finalized: its completion frees it then.
 */
struct auth_io {
	struct blocker_job  work;
	struct auth        *owner;    /* nullptr once the auth has let go */
	struct dstr         path;     /* the credential file */
	struct dstr         command;  /* of --auth-command; empty for the
	                                 ChatGPT sign-in */
	struct dstr         header;   /* the Authorization line that it read */
	uint64_t            expires;  /* when the token of header ends */
	struct flag64       flags;    /* enum auth_flag of this read, widened
	                                 over the padding */
};

/* Overwrites the bytes of the token in @p text and frees them. */
static void
auth_text_wipe (struct dstr *text)
{
	explicit_bzero(dstr_raw(text), dstr_len(text));
	dstr_cleanup(text);
}

static void
auth_io_destroy (struct auth_io **p_io)
{
	struct auth_io *io = *p_io;
	*p_io = nullptr;
	dstr_cleanup(&io->path);
	dstr_cleanup(&io->command);
	auth_text_wipe(&io->header);
	free(io);
}

/* On a blocker. The members that it reads do not change after
 * auth_init(), and only this function writes header and expires while the
 * read is under way.
 */
static void
auth_io_work (struct blocker_job *work)
{
	struct auth_io *io = container_of(work, struct auth_io, work);
	uint32_t const flags = (uint32_t)io->flags.bits;
	work->error = dstr_len(&io->command)
	              ? bearer_header(dstr_get(&io->path), dstr_get(&io->command), flags,
	                              &io->header, &io->expires)
	              : chatgpt_header(dstr_get(&io->path), flags, &io->header, &io->expires);
}

/* Moves the waits on auth->waiting that the read with @p flags and the
 * error @p e answers to auth->ready, and has the step answer them. A read
 * that could only look at the file and failed answers the waits that asked
 * for no more. A wait that came during that read is left for the next one,
 * which can renew the credential.
 */
static void
auth_answer (struct auth *auth,
             int          e,
             uint64_t     flags)
{
	auth->error = e;
	if (!e || !(flags & AUTH_CACHED)) {
		list_transplant(&auth->ready, &auth->waiting);
	} else {
		struct list *next;
		for (struct list *pos = auth->waiting.next; pos != &auth->waiting; pos = next) {
			next = pos->next;
			if (flag_any(container_of(pos, struct auth_wait, hook)->flags, AUTH_CACHED))
				list_move(&auth->ready, pos);
		}
	}
	loop_ready_bulk(auth->loop, &auth->deliver);
}

/* Starts the read for the waits on auth->waiting. The read may renew the
 * credential unless every wait asked for the file alone.
 */
static void
auth_read (struct auth *auth)
{
	struct auth_io *io = auth->io;
	uint64_t flags = AUTH_CACHED;
	struct list const *pos;
	list_foreach(pos, &auth->waiting)
		flags &= container_of(pos, struct auth_wait, hook)->flags.bits;
	io->flags = flag64(flags);
	int const e = auth->blockers ? blocker_submit(auth->blockers, &io->work) : ENOSYS;
	if (e)
		auth_answer(auth, e, 0);
	else
		flag_put(&auth->flags, FLAG_WHEN, AUTH_BUSY);
}

/* The message of a read that has ended, on the loop. */
static void
auth_io_done (struct loop *loop,
              struct msg  *msg)
{
	struct auth_io *io = container_of(msg, struct auth_io, work.done);
	struct auth *auth = io->owner;
	if (!auth) {
		auth_io_destroy(&io);
		return;
	}
	flag_put(&auth->flags, FLAG_WHEN, 0);
	int const e = (int)io->work.error;
	/* After a failed read no line of before is given: the file can have
	 * changed. */
	auth->line = nullptr;
	auth_text_wipe(&auth->text);
	if (!e) {
		auth->text = dstr_move(&io->header);
		auth->line = dstr_get(&auth->text);
		auth->expires = io->expires;
		auth->review = loop->now + AUTH_REVIEW * NS;
	}
	auth_answer(auth, e, io->flags.bits);
}

/* Answers the waits on auth->ready, as many in a step as the share of the
 * step in requests allows: an answer starts a transfer. A token can have
 * ended while its waits were on the list, so the clock is read again.
 */
static void
auth_deliver_step (struct loop      *loop,
                   struct loop_step *step)
{
	struct auth *auth = container_of(step, struct auth, deliver);
	uint64_t const now = clk_realtime_ns() / NS;
	int e = auth->error;
	if (!e && !now)
		e = EIO;
	if (!e && now >= auth->expires)
		e = EKEYEXPIRED;
	char const *const line = e ? nullptr : auth->line;
	while (!list_is_empty(&auth->ready)) {
		if (loop_spent(loop, step, LOOP_REQS))
			return;
		struct auth_wait *wait = container_of(auth->ready.next, struct auth_wait, hook);
		list_del(&wait->hook);
		wait->done(wait, e, line);
		loop_work(loop, LOOP_REQS, 1);
	}
	if (!list_is_empty(&auth->waiting) && flag_when(auth->flags) != AUTH_BUSY)
		auth_read(auth);
}

int
auth_init (struct auth              *auth,
           struct loop              *loop,
           struct blocker           *blockers,
           struct ai_endpoint const *ep)
{
	*auth = (struct auth){
		.waiting    = list(&auth->waiting),
		.ready      = list(&auth->ready),
		.deliver.fn = auth_deliver_step,
		.loop       = loop,
		.blockers   = blockers,
		.text       = DSTR_INIT
	};
	char const *const command = ep->auth_command;
	char const *const path = command ? ep->auth_file : ep->chatgpt_auth;
	if (!path)
		return 0;
	struct auth_io *io = malloc(sizeof *io);
	if (!io)
		return ENOMEM;
	*io = (struct auth_io){
		.work    = blocker_job(auth_io_done, auth_io_work, loop),
		.owner   = auth,
		.path    = DSTR_INIT,
		.command = DSTR_INIT,
		.header  = DSTR_INIT
	};
	int e = cstr_view_store(cstr_view(path), &io->path);
	if (!e && command)
		e = cstr_view_store(cstr_view(command), &io->command);
	if (e) {
		auth_io_destroy(&io);
		return e;
	}
	auth->io = io;
	return 0;
}

void
auth_fini (struct auth *auth)
{
	if (!auth->loop)
		return;
	loop_unready(&auth->deliver);
	while (!list_is_empty(&auth->waiting))
		list_del(auth->waiting.next);
	while (!list_is_empty(&auth->ready))
		list_del(auth->ready.next);
	/* The message of a read under way frees the read. */
	if (auth->io) {
		if (flag_when(auth->flags) == AUTH_BUSY)
			auth->io->owner = nullptr;
		else
			auth_io_destroy(&auth->io);
	}
	auth->io = nullptr;
	auth->line = nullptr;
	auth_text_wipe(&auth->text);
}

int
auth_ask (struct auth       *auth,
          struct auth_wait  *wait,
          uint32_t           flags,
          char const       **line)
{
	*line = nullptr;
	uint64_t const now = clk_realtime_ns() / NS;
	if (!now)
		return EIO;
	if (auth->line && now + AUTH_EARLY < auth->expires && auth->loop->now < auth->review) {
		*line = auth->line;
		return 0;
	}
	wait->flags = flag64(flags);
	list_append(&auth->waiting, &wait->hook);
	/* The waits of one read are answered before the next read starts. */
	if (flag_when(auth->flags) != AUTH_BUSY && list_is_empty(&auth->ready))
		auth_read(auth);
	return 0;
}

char const *
auth_why (struct auth const *auth,
          int                e)
{
	return dstr_len(&auth->io->command) ? bearer_why(e) : chatgpt_why(e);
}
