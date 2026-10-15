/** @file HTTP requests of tool calls, and the stored responses (web.h).
 *
 * A request that home sends is a run.  The client loop of the call that
 * asks for a request makes the run, which has a copy of the request, its
 * key and a list of waits.  The loop posts the run to home with a message
 * that has a word of pending bits, as a job does (src/job.h).  Home starts
 * the transfer on its bridge, and it receives the body in bulk steps, as
 * much in a step as the step's share of receiving allows.  It ends the run
 * when the transfer ends or when the run's deadline passes.  At the end,
 * home makes the reply, which has a reference count.  No thread changes the
 * reply after that.  With the mutex locked, home moves the waits from the
 * run's list to a list of its own, and it puts the reply into the table if
 * the request asked for that and the reply is a whole 2xx response.  Then
 * it posts each wait to the wait's loop with a reference to the reply.
 *
 * A run has a reference count.  Home has one reference from the run's
 * creation to its end, and the run's message has one while it is posted.
 * A wait has no reference to its run.  While a wait is on the run's list,
 * which is accessed only with the mutex locked, the run has not ended, and
 * the wait's pointer to the run is valid.  Home sets that pointer to
 * nullptr for each wait that it moves.  When a call releases its wait, the
 * wait is removed from the list with the mutex locked.  If the list is
 * then empty and the run's keep is 0, the call posts CANCEL to home, and
 * home cancels the transfer.  A run whose keep is not 0 goes on to its
 * end, so that its response is stored.
 *
 * The table has a fixed number of buckets.  The stored responses are also
 * on a list in the order of the times at which their keep passes.  To make
 * room, and under memory pressure, askai frees responses from the front of
 * that list.  So it frees the responses whose keep has passed first, and
 * then those whose keep passes next.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>

#include "http.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "util.h"
#include "web.h"

enum {
	WEB_CACHE_MAX = 64 << 20,  /* bytes of the stored responses */
	WEB_HOST      = 2,         /* connections to one host at once */
	WEB_BUCKETS   = 1 << 12,   /* of the table */
	WEB_GROW      = 16 << 10,  /* bytes of the first storage of a body */
};

static constexpr uint64_t NS = 1'000'000'000;

/* The bits of the message of a run to home. */
enum {
	RUN_SUBMIT = 1,  /* send the request */
	RUN_CANCEL = 2,  /* no call waits for the run, and its keep is 0 */
};

/* The error text of a run that askai ends as it stops. */
static char const stopping[] = "askai is stopping.";

/* The key of a request.  Two requests are equal if their keys are equal.
 * The bytes of the key follow the struct that has the key.  They are the
 * URL, a NUL, the Accept, a NUL, the header lines, a NUL and the body.  The
 * header lines are the line of User-Agent and the lines of struct
 * web_ask, each of those after a NUL.  The URL, the Accept and the lines
 * have no NUL, and the key has the lengths, so the bytes have one reading.
 * The line of User-Agent has the name of the plugin, so the plugins do not
 * share responses. */
struct web_key {
	uint64_t hash;
	uint64_t max;     /* the request's max */
	uint32_t url;     /* the length of the URL */
	uint32_t accept;  /* the length of the Accept, 0 if it has none */
	uint32_t body;    /* the length of the body */
	uint16_t lines;   /* the length of the header lines */
	uint16_t post;    /* bool, widened over the padding: a POST */
};

/* A response, or the reason why none came.  No thread changes it after
 * home has made it.  Each wait that has it, and the table, have a reference
 * to it. */
struct web_reply {
	char            *body;      /* nullptr if the body is empty */
	char const      *type;      /* in text */
	char const      *error;     /* in text */
	size_t           len;       /* the length of the body */
	size_t           size;      /* the bytes of the reply and its body */
	uint64_t         received;  /* loop_clock() as home ended the run */
	_Atomic uint32_t refs;
	uint32_t         status;
	uint32_t         retry;
	int              e;
	char             text[];    /* the type, a NUL, the error, a NUL */
};

/* A stored response in the table.  It is accessed only with the mutex
 * locked. */
struct web_entry {
	struct web_entry *next;     /* in its bucket */
	struct list       hook;     /* on web->kept, in the order of expires */
	struct web_reply *reply;    /* the table's reference */
	uint64_t          expires;  /* the loop_clock() at which its keep
	                               passes */
	size_t            bytes;    /* what it counts against WEB_CACHE_MAX */
	struct web_key    key;
	char              data[];   /* the bytes of the key */
};

/* A request that home sends. */
struct web_run {
	struct msg           to_home;   /* a client loop posts it with
	                                   RUN_SUBMIT or RUN_CANCEL */
	struct web          *web;
	struct loop         *home;
	/* The mutex is locked for each access to hook and waits. */
	struct list          hook;      /* on web->runs while an equal request
	                                   can wait for the run */
	struct list          waits;     /* the waits for the run */
	/* Only home's thread uses the members from live to since. */
	struct list          live;      /* on web->live while home runs the
	                                   run */
	struct list          held;      /* on web->held while the transfer is
	                                   paused, or on web->late once the
	                                   deadline has passed */
	struct loop_timer    timer;     /* the deadline */
	struct http_request *transfer;
	char                *body;      /* the body received so far */
	size_t               len;       /* the length of the body */
	size_t               cap;       /* the room in body */
	uint64_t             since;     /* loop_clock() as home started the
	                                   run */
	struct web_key       key;
	_Atomic uint32_t     bits;      /* the bits for home */
	_Atomic uint32_t     refs;      /* one for home, one for the message
	                                   while it is posted */
	uint32_t             keep;      /* the keep of the request that made
	                                   the run.  It does not change. */
	uint32_t             most;      /* the largest keep of the requests
	                                   that wait for the run.  The mutex is
	                                   locked for each access. */
	uint32_t             timeout;   /* seconds */
	int                  fault;     /* EFBIG, ENOMEM or ETIMEDOUT once the
	                                   run has failed in that way, else 0.
	                                   Only home's thread uses it. */
	char                 data[];    /* the bytes of the key */
};

/* A call's wait for a reply. */
struct web_wait {
	struct msg        msg;    /* home posts it to loop with the reply */
	struct list       hook;   /* on the list of waits of its run.  The
	                             mutex is locked for each access. */
	struct web       *web;
	struct web_run   *run;    /* the run while the wait is on the run's
	                             list, else nullptr.  The mutex is locked
	                             for each access. */
	struct web_reply *reply;  /* a reference, once done is true.  It is
	                             nullptr if home ran out of memory. */
	struct loop      *loop;
	web_back_fn      *back;
	struct handle     who;
	uint64_t          done;   /* bool, widened over the padding: the wait
	                             has its reply.  Only the thread of loop
	                             uses it. */
};

struct web {
	pthread_mutex_t    lock;
	/* The mutex is locked for each access to the members from runs to
	 * n_runs. */
	struct list        runs;     /* the runs under way whose keep is not
	                                0 */
	struct list        kept;     /* the entries, the one whose keep passes
	                                first at the front */
	struct web_entry **buckets;
	size_t             bytes;    /* what the entries count against
	                                WEB_CACHE_MAX */
	uint64_t           n_runs;   /* the runs under way */
	uint64_t           seed;     /* of the keys' hash */
	/* Only home's thread uses the members from live to loop. */
	struct list        live;     /* the runs that home runs */
	struct list        held;     /* the runs whose transfer is paused */
	struct list        late;     /* the runs whose deadline has passed */
	struct loop_step   resume;   /* resumes the paused runs */
	struct loop_step   overdue;  /* ends the runs on late */
	struct http       *http;
	struct loop       *loop;
};

/* How a run ended. */
struct web_end {
	char const *type;    /* the Content-Type, or nullptr if none */
	char const *why;     /* the error text, "" if e is 0 */
	uint32_t    status;
	uint32_t    retry;
	int         e;
};

/* Returns the length of the bytes of @p key. */
static size_t
web_key_size (struct web_key const *key)
{
	return (size_t)key->url + 1 + key->accept + 1 + key->lines + 1 + key->body;
}

/* Returns the hash of @p key, whose bytes are at @p data, seeded with
 * @p seed. */
static uint64_t
web_key_hash (uint64_t              seed,
              struct web_key const *key,
              char const           *data)
{
	uint64_t h = util_hash(seed, data, web_key_size(key));
	return util_hash(h, &key->max, sizeof key->max) ^ key->post;
}

/* Returns whether the key @p a, whose bytes are at @p ad, is equal to the
 * key @p b, whose bytes are at @p bd. */
static bool
web_key_equal (struct web_key const *a,
               char const           *ad,
               struct web_key const *b,
               char const           *bd)
{
	return a->hash == b->hash && a->max == b->max && a->url == b->url &&
	       a->accept == b->accept && a->body == b->body && a->lines == b->lines &&
	       a->post == b->post && !memcmp(ad, bd, web_key_size(a));
}

static struct web_reply *
web_reply_get (struct web_reply *reply)
{
	atomic_fetch_add_explicit(&reply->refs, 1, memory_order_relaxed);
	return reply;
}

/* Releases a reference to @p reply, if it is not nullptr. */
static void
web_reply_put (struct web_reply *reply)
{
	if (reply && atomic_fetch_sub_explicit(&reply->refs, 1, memory_order_acq_rel) == 1) {
		free(reply->body);
		free(reply);
	}
}

/* Releases @p n references to @p run, and frees the run if they were the
 * last. */
static void
web_run_put (struct web_run *run,
             uint32_t        n)
{
	if (atomic_fetch_sub_explicit(&run->refs, n, memory_order_acq_rel) == n) {
		free(run->body);
		free(run);
	}
}

/* Frees the entries of the list @p gone, which the member next links, and
 * releases their references to the replies. */
static void
web_entries_free (struct web_entry *gone)
{
	for (struct web_entry *next; gone; gone = next) {
		next = gone->next;
		web_reply_put(gone->reply);
		free(gone);
	}
}

/* Returns the entry whose key is equal to @p key, whose bytes are at
 * @p data, or nullptr if the table has none.  The caller has locked the
 * mutex. */
static struct web_entry *
web_find (struct web const     *web,
          struct web_key const *key,
          char const           *data)
{
	struct web_entry *entry = web->buckets[key->hash & (WEB_BUCKETS - 1)];
	while (entry && !web_key_equal(&entry->key, entry->data, key, data))
		entry = entry->next;
	return entry;
}

/* Returns the run under way whose key is equal to @p key, whose bytes are
 * at @p data, whose timeout is @p timeout seconds and whose keep is not 0,
 * or nullptr if there is none.  The caller has locked the mutex. */
static struct web_run *
web_find_run (struct web const     *web,
              struct web_key const *key,
              char const           *data,
              uint32_t              timeout)
{
	struct list const *node;
	list_foreach (node, &web->runs) {
		struct web_run *run = container_of(node, struct web_run, hook);
		if (run->timeout == timeout && web_key_equal(&run->key, run->data, key, data))
			return run;
	}
	return nullptr;
}

/* Removes @p entry from the table and from the list of stored responses.
 * The caller has locked the mutex, and frees the entry after it has
 * unlocked the mutex. */
static void
web_unlink (struct web       *web,
            struct web_entry *entry)
{
	struct web_entry **p = &web->buckets[entry->key.hash & (WEB_BUCKETS - 1)];
	while (*p != entry)
		p = &(*p)->next;
	*p = entry->next;
	list_del(&entry->hook);
	web->bytes -= entry->bytes;
}

/*
 * Puts @p entry into the table, in place of an entry with an equal key.  It
 * puts the entry on the list of stored responses in the order of expires.
 * While the stored responses would count more than WEB_CACHE_MAX bytes, it
 * removes the entry at the front of the list.  Returns the entries that it
 * removed, which the member next links, for the caller to free after it
 * has unlocked the mutex.  The caller has locked the mutex.
 */
static struct web_entry *
web_keep (struct web       *web,
          struct web_entry *entry)
{
	struct web_entry *gone = web_find(web, &entry->key, entry->data);
	if (gone) {
		web_unlink(web, gone);
		gone->next = nullptr;
	}
	while (web->bytes + entry->bytes > WEB_CACHE_MAX && !list_is_empty(&web->kept)) {
		struct web_entry *first = container_of(web->kept.next, struct web_entry, hook);
		web_unlink(web, first);
		first->next = gone;
		gone = first;
	}
	struct web_entry **bucket = &web->buckets[entry->key.hash & (WEB_BUCKETS - 1)];
	entry->next = *bucket;
	*bucket = entry;
	/* The new entry's keep usually passes last, so the search begins at
	 * the back of the list. */
	struct list *at = web->kept.prev;
	while (at != &web->kept && container_of(at, struct web_entry, hook)->expires >
	                           entry->expires)
		at = at->prev;
	list_append(at->next, &entry->hook);
	web->bytes += entry->bytes;
	return gone;
}

/* Makes the reply of @p run from @p end.  If the run did not fail, the body
 * that the run received moves to the reply.  Otherwise the body is freed.
 * Returns the reply with one reference.  On failure it returns nullptr and
 * stores ENOMEM in @p *e. */
static struct web_reply *
web_reply_create (struct web_run       *run,
                  struct web_end const *end,
                  int                  *e)
{
	size_t type = end->type ? strlen(end->type) : 0;
	size_t why = strlen(end->why);
	size_t size = sizeof (struct web_reply) + type + 1 + why + 1;
	struct web_reply *reply = malloc(size);
	char *body = run->body;
	size_t len = run->len;
	run->body = nullptr;
	run->len = run->cap = 0;
	if (!reply || end->e || !len) {
		free(body);
		body = nullptr;
		len = 0;
	}
	if (!reply) {
		*e = ENOMEM;
		return nullptr;
	}
	/* The block of the body has room to grow.  realloc() can move the body
	 * to a smaller block. */
	char *fit = body ? realloc(body, len) : nullptr;
	reply->body = fit ? fit : body;
	reply->len = len;
	reply->size = size + len;
	if (type)
		memcpy(reply->text, end->type, type);
	reply->text[type] = '\0';
	memcpy(reply->text + type + 1, end->why, why + 1);
	reply->type = reply->text;
	reply->error = reply->text + type + 1;
	reply->received = loop_clock();
	atomic_init(&reply->refs, 1);
	reply->status = end->status;
	reply->retry = end->retry;
	reply->e = end->e;
	return reply;
}

/* Returns the bytes that an entry for @p reply, the response to @p run,
 * counts against WEB_CACHE_MAX. */
static size_t
web_entry_bytes (struct web_run const   *run,
                 struct web_reply const *reply)
{
	return sizeof (struct web_entry) + web_key_size(&run->key) + reply->size;
}

/* Makes the entry of the table for @p reply, the response to @p run, with
 * a reference to the reply.  Returns the entry.  On failure it returns
 * nullptr and stores ENOMEM in @p *e. */
static struct web_entry *
web_entry_create (struct web_run const *run,
                  struct web_reply     *reply,
                  int                  *e)
{
	size_t key = web_key_size(&run->key);
	size_t bytes = web_entry_bytes(run, reply);
	struct web_entry *entry = malloc(sizeof *entry + key);
	if (!entry) {
		*e = ENOMEM;
		return nullptr;
	}
	*entry = (struct web_entry){
		.reply = web_reply_get(reply),
		.bytes = bytes,
		.key = run->key
	};
	memcpy(entry->data, run->data, key);
	return entry;
}

/*
 * Ends @p run with the result @p end.  It makes the reply, and it puts the
 * reply into the table if the run's keep is not 0, the reply is a whole 2xx
 * response, and its entry would count at most a quarter of WEB_CACHE_MAX.
 * A reply or an entry that cannot be made for lack of memory is left out:
 * the waits get no reply, or the reply is not stored.  With the mutex
 * locked, it moves the run's waits to a list
 * of its own.  Then it posts the reply to each of them, and releases
 * home's reference to the run.  Home calls it, also inside loop_fini()
 * after home's loop has stopped.
 */
static void
web_run_end (struct web           *web,
             struct web_run       *run,
             struct web_end const *end)
{
	int e = 0;
	struct web_reply *reply = web_reply_create(run, end, &e);
	bool store = reply && run->keep && !end->e && end->status >= 200 &&
	             end->status < 300 && web_entry_bytes(run, reply) <= WEB_CACHE_MAX / 4;
	struct web_entry *entry = store ? web_entry_create(run, reply, &e) : nullptr;
	struct web_entry *gone = nullptr;
	struct list waits;
	list_init(&waits);
	struct list *node;
	pthread_mutex_lock(&web->lock);
	list_del(&run->hook);
	--web->n_runs;
	list_foreach (node, &run->waits)
		container_of(node, struct web_wait, hook)->run = nullptr;
	list_transplant(&waits, &run->waits);
	if (entry) {
		entry->expires = reply->received + run->most * NS;
		gone = web_keep(web, entry);
	}
	pthread_mutex_unlock(&web->lock);

	/* Once a wait is posted, only the thread of its loop uses it, so the
	 * pointer to the next wait is read before the post. */
	struct list *tmp;
	list_foreach_safe (node, tmp, &waits) {
		struct web_wait *wait = container_of(node, struct web_wait, hook);
		wait->reply = reply ? web_reply_get(reply) : nullptr;
		loop_post(wait->loop, &wait->msg);
	}
	web_entries_free(gone);
	web_reply_put(reply);
	web_run_put(run, 1);
}

/* Ends @p run, which home runs, with the result @p end, and logs the end
 * with --verbose. */
static void
web_run_finish (struct web           *web,
                struct web_run       *run,
                struct web_end const *end)
{
	loop_timer_stop(web->loop, &run->timer);
	list_del(&run->live);
	list_del(&run->held);
	char const *method = run->key.post ? "POST" : "GET";
	uint64_t ms = (loop_clock() - run->since) / 1'000'000;
	/* The URL without a user name and a password, which askai's own
	 * requests can have (struct web_ask). */
	char const *url = run->data;
	int scheme = 0;
	char const *host = util_url_shown(url, &scheme);
	if (end->e)
		LOG(LOG_VERBOSE, "web request %s %.*s%s failed after %" PRIu64 " ms: %s",
		    method, scheme, url, host, ms, end->why);
	else
		LOG(LOG_VERBOSE, "web request %s %.*s%s answered with status %" PRIu32
		    " after %" PRIu64 " ms (%zu bytes)", method, scheme, url, host,
		    end->status, ms, run->len);
	web_run_end(web, run, end);
}

/* Copies the @p len bytes at @p data to the body of the response to
 * @p user, a run.  If the step's share of receiving has no room for them,
 * the function pauses the transfer, and the resume step resumes it later.
 * A body that grows past the request's max fails the run with EFBIG.
 * Returns what a write callback of curl returns. */
static size_t
web_run_write (char const *data,
               size_t      len,
               void       *user)
{
	struct web_run *run = user;
	struct web *web = run->web;
	struct loop *loop = web->loop;
	if (!len)
		return 0;
	if (len > run->key.max - run->len) {
		run->fault = EFBIG;
		return 0;
	}
	uint64_t room = loop_room(loop, LOOP_RECV);
	if (room < len && room < LOOP_STEP_RECV) {
		if (list_is_empty(&run->held))
			list_append(&web->held, &run->held);
		loop_ready_bulk(loop, &web->resume);
		return CURL_WRITEFUNC_PAUSE;
	}
	if (len > run->cap - run->len) {
		size_t cap = run->cap ? 2 * run->cap : WEB_GROW;
		cap = cap < run->len + len ? run->len + len : cap;
		cap = cap < run->key.max ? cap : run->key.max;
		char *body = realloc(run->body, cap);
		if (!body) {
			run->fault = ENOMEM;
			return 0;
		}
		run->body = body;
		run->cap = cap;
	}
	memcpy(run->body + run->len, data, len);
	run->len += len;
	loop_work(loop, LOOP_RECV, len);
	return len;
}

/* The transfer of @p user, a run, has ended with @p result. */
static void
web_run_done (CURLcode    result,
              long        status,
              char const *error,
              void       *user)
{
	struct web_run *run = user;
	struct web *web = run->web;
	run->transfer = nullptr;
	char why[CURL_ERROR_SIZE] = "";
	int e = run->fault;
	switch (e) {
	case EFBIG:
		util_format(why, sizeof why, "The response body is longer than %" PRIu64
		            " bytes.", run->key.max);
		break;
	case ENOMEM:
		util_format(why, sizeof why, "askai ran out of memory.");
		break;
	case ETIMEDOUT:
		util_format(why, sizeof why, "The request took longer than %" PRIu32 " s.",
		            run->timeout);
		break;
	default:
		if (result == CURLE_OK)
			break;
		e = result == CURLE_OPERATION_TIMEDOUT ? ETIMEDOUT : EIO;
		util_format(why, sizeof why, "%s", *error ? error : curl_easy_strerror(result));
	}
	long retry = http_retry_after(web->http);
	web_run_finish(web, run, &(struct web_end){
		.type = http_type(web->http),
		.why = why,
		.status = status > 0 && status < 1000 ? (uint32_t)status : 0,
		.retry = retry > 0 && retry <= UINT32_MAX ? (uint32_t)retry : 0,
		.e = e
	});
}

/* The deadline of a run has passed, and the overdue step ends the run.
 * curl has a timeout too, but it begins only when curl gets the transfer,
 * after the lookup of the host. */
static void
web_run_late (struct loop       *loop,
              struct loop_timer *timer)
{
	struct web_run *run = container_of(timer, struct web_run, timer);
	struct web *web = run->web;
	list_del(&run->held);
	list_append(&web->late, &run->held);
	loop_ready_bulk(loop, &web->overdue);
}

/* Ends the runs whose deadline has passed with ETIMEDOUT, as many in a step
 * as the step's share of requests allows. */
static void
web_overdue_step (struct loop      *loop,
                  struct loop_step *step)
{
	struct web *web = container_of(step, struct web, overdue);
	for (; !list_is_empty(&web->late); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		struct web_run *run = container_of(web->late.next, struct web_run, held);
		run->fault = ETIMEDOUT;
		http_end(web->http, &run->transfer, CURLE_OPERATION_TIMEDOUT);
	}
}

/* Resumes the paused runs, the first paused first, while the step's share
 * of receiving has room for a write of curl. */
static void
web_resume_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct web *web = container_of(step, struct web, resume);
	while (!list_is_empty(&web->held)) {
		if (loop_room(loop, LOOP_RECV) < CURL_MAX_WRITE_SIZE) {
			loop_ready_bulk(loop, step);
			return;
		}
		struct web_run *run = container_of(web->held.next, struct web_run, held);
		list_del(&run->held);
		CURLcode result = http_resume(run->transfer);
		if (result)
			http_end(web->http, &run->transfer, result);
	}
}

/* Starts the transfer of @p run on home's bridge, or ends the run if it
 * cannot start. */
static void
web_run_start (struct web     *web,
               struct web_run *run)
{
	static char const name[] = "Accept: ";
	char const *url = run->data;
	char const *accept = url + run->key.url + 1;
	char const *lines = accept + run->key.accept + 1;
	char const *body = lines + run->key.lines + 1;
	char line[sizeof name + WEB_ACCEPT_MAX];
	/* The header lines, the line of Accept if any, and a nullptr. */
	size_t n = 2;
	for (char const *l = lines; l < body; l += strlen(l) + 1)
		++n;
	char const *few[3];
	char const **headers = n <= ARRAY_SIZE(few) ? few : malloc(n * sizeof *headers);
	size_t k = 0;
	for (char const *l = lines; headers && l < body; l += strlen(l) + 1)
		headers[k++] = l;
	if (headers && run->key.accept) {
		memcpy(line, name, sizeof name - 1);
		memcpy(line + sizeof name - 1, accept, run->key.accept + 1);
		headers[k++] = line;
	}
	run->since = loop_clock();
	list_append(&web->live, &run->live);
	int e = ENOMEM;
	if (headers) {
		headers[k] = nullptr;
		e = http_post_json(web->http, url, run->key.post ? body : nullptr,
		                   run->key.body, false, headers, run->timeout * 1000L,
		                   web_run_write, web_run_done, run, &run->transfer);
	}
	if (headers != few)
		free(headers);
	if (e) {
		char why[96];
		util_format(why, sizeof why, "askai cannot send the request: %s", strerror(e));
		web_run_finish(web, run, &(struct web_end){
			.why = why,
			.e = e == ENOMEM ? ENOMEM : EIO
		});
		return;
	}
	/* Without the encodings, the response comes uncompressed. */
	(void)http_accept_encoding(run->transfer);
	/* If the timer cannot be set, curl's timeout still ends the run. */
	(void)loop_timer_set(web->loop, &run->timer, run->since + run->timeout * NS);
}

/* Handles @p msg, the message of a run, on home's loop.  After home's loop
 * has stopped, home's part of the service is gone, and a new run ends with
 * ESHUTDOWN. */
static void
web_run_mail (struct loop *loop,
              struct msg  *msg)
{
	struct web_run *run = container_of(msg, struct web_run, to_home);
	struct web *web = run->web;
	uint32_t bits = atomic_exchange(&run->bits, 0);
	if (bits & RUN_SUBMIT) {
		if (loop->exit)
			web_run_end(web, run, &(struct web_end){ .why = stopping, .e = ESHUTDOWN });
		else if (bits & RUN_CANCEL)
			web_run_end(web, run, &(struct web_end){ .why = "", .e = ECANCELED });
		else
			web_run_start(web, run);
	} else if (bits & RUN_CANCEL && !list_is_empty(&run->live)) {
		http_cancel(web->http, &run->transfer);
		web_run_finish(web, run, &(struct web_end){ .why = "cancelled", .e = ECANCELED });
	}
	web_run_put(run, 1);
}

/* Handles @p msg, the message of a wait, on the wait's loop, when the wait
 * has its reply. */
static void
web_wait_mail (struct loop *loop,
               struct msg  *msg)
{
	struct web_wait *wait = container_of(msg, struct web_wait, msg);
	wait->done = true;
	wait->back(loop, wait->who, wait);
}

struct web *
web_create (int *e)
{
	struct web *web = malloc(sizeof *web);
	struct web_entry **buckets = calloc(WEB_BUCKETS, sizeof *buckets);
	*e = !web || !buckets ? ENOMEM : pthread_mutex_init(&web->lock, nullptr);
	if (*e) {
		free(buckets);
		free(web);
		return nullptr;
	}
	list_init(&web->runs);
	list_init(&web->kept);
	list_init(&web->live);
	list_init(&web->held);
	list_init(&web->late);
	web->resume = (struct loop_step){ .fn = web_resume_step };
	web->overdue = (struct loop_step){ .fn = web_overdue_step };
	web->buckets = buckets;
	web->http = nullptr;
	web->loop = nullptr;
	web->bytes = 0;
	web->n_runs = 0;
	/* Without random bytes, the clock seeds the hash. */
	web->seed = loop_clock();
	(void)getrandom(&web->seed, sizeof web->seed, GRND_NONBLOCK);
	return web;
}

void
web_destroy (struct web **p_web)
{
	if (!p_web || !*p_web)
		return;
	struct web *web = *p_web;
	*p_web = nullptr;
	while (!list_is_empty(&web->kept)) {
		struct web_entry *entry = container_of(web->kept.next, struct web_entry, hook);
		list_del(&entry->hook);
		web_reply_put(entry->reply);
		free(entry);
	}
	free(web->buckets);
	pthread_mutex_destroy(&web->lock);
	free(web);
}

int
web_home_start (struct web     *web,
                struct loop    *loop,
                struct blocker *blockers)
{
	int e;
	web->http = http_create(loop, nullptr, &e);
	if (!web->http)
		return e;
	http_resolver(web->http, blockers);
	http_connections(web->http, WEB_HOST);
	web->loop = loop;
	return 0;
}

void
web_home_stop (struct web *web)
{
	if (!web || !web->http)
		return;
	while (!list_is_empty(&web->live)) {
		struct web_run *run = container_of(web->live.next, struct web_run, live);
		http_cancel(web->http, &run->transfer);
		web_run_finish(web, run, &(struct web_end){ .why = stopping, .e = ESHUTDOWN });
	}
	loop_unready(&web->resume);
	loop_unready(&web->overdue);
	http_destroy(&web->http);
	web->loop = nullptr;
}

size_t
web_relieve (struct web        *web,
             enum buf_pressure  level,
             size_t             max)
{
	uint64_t now = loop_clock();
	struct web_entry *gone = nullptr;
	size_t units = 0;
	pthread_mutex_lock(&web->lock);
	while (!list_is_empty(&web->kept) && (!units || units < max)) {
		struct web_entry *first = container_of(web->kept.next, struct web_entry, hook);
		if (level < BUF_CRITICAL && first->expires > now)
			break;
		web_unlink(web, first);
		first->next = gone;
		gone = first;
		units += buf_units(first->bytes);
	}
	pthread_mutex_unlock(&web->lock);
	web_entries_free(gone);
	return units;
}

/* Whether the header line @p line goes with a request: all but a line of
 * User-Agent, as askai sends its own. */
static bool
web_line_kept (char const *line)
{
	static char const name[] = "User-Agent:";
	return strncasecmp(line, name, sizeof name - 1);
}

struct web_wait *
web_wait_create (struct web           *web,
                 struct web_ask const *ask,
                 int                  *e)
{
	static char const agent[] = "User-Agent: askai";
	static char const of[] = " (plugin ";
	struct web_request const *r = ask->request;
	size_t name = ask->plugin ? strlen(ask->plugin) : 0;
	size_t body = r->body ? r->len : 0;
	size_t line = sizeof agent - 1 + (name ? sizeof of - 1 + name + 1 : 0);
	for (char const *const *l = ask->lines; l && *l; ++l)
		line += web_line_kept(*l) ? 1 + strlen(*l) : 0;
	if (line > UINT16_MAX) {
		*e = EINVAL;
		return nullptr;
	}
	size_t key = ask->url + 1 + ask->accept + 1 + line + 1 + body;
	struct web_run *run = malloc(sizeof *run + key);
	struct web_wait *wait = malloc(sizeof *wait);
	if (!run || !wait) {
		free(run);
		free(wait);
		*e = ENOMEM;
		return nullptr;
	}
	char *p = run->data;
	memcpy(p, r->url, ask->url + 1);
	p += ask->url + 1;
	if (ask->accept)
		memcpy(p, r->accept, ask->accept);
	p[ask->accept] = '\0';
	p += ask->accept + 1;
	memcpy(p, agent, sizeof agent - 1);
	p += sizeof agent - 1;
	if (name) {
		memcpy(p, of, sizeof of - 1);
		p += sizeof of - 1;
		memcpy(p, ask->plugin, name);
		p += name;
		*p++ = ')';
	}
	for (char const *const *l = ask->lines; l && *l; ++l) {
		if (!web_line_kept(*l))
			continue;
		size_t n = strlen(*l);
		*p++ = '\0';
		memcpy(p, *l, n);
		p += n;
	}
	*p++ = '\0';
	if (body)
		memcpy(p, r->body, body);
	loop_work(ask->loop, LOOP_COPY, key);

	run->to_home = (struct msg){ .fn = web_run_mail };
	list_init(&run->hook);
	list_init(&run->waits);
	list_init(&run->live);
	list_init(&run->held);
	run->timer = (struct loop_timer){ .due = UINT64_MAX, .fn = web_run_late };
	run->web = web;
	run->home = ask->home;
	run->transfer = nullptr;
	run->body = nullptr;
	run->len = run->cap = 0;
	run->since = 0;
	run->key = (struct web_key){
		.max = r->max,
		.url = (uint32_t)ask->url,
		.accept = (uint32_t)ask->accept,
		.body = (uint32_t)body,
		.lines = (uint16_t)line,
		.post = r->body != nullptr
	};
	run->key.hash = web_key_hash(web->seed, &run->key, run->data);
	/* One reference for home, and one for the message. */
	atomic_init(&run->bits, RUN_SUBMIT);
	atomic_init(&run->refs, 2);
	run->keep = run->most = r->keep;
	run->timeout = r->timeout;
	run->fault = 0;
	*wait = (struct web_wait){
		.msg.fn = web_wait_mail,
		.web = web,
		.loop = ask->loop,
		.back = ask->back,
		.who = ask->who
	};
	list_init(&wait->hook);

	uint64_t const now = ask->loop->now;
	struct web_reply *hit = nullptr;
	struct web_run *join = nullptr;
	pthread_mutex_lock(&web->lock);
	if (r->keep) {
		struct web_entry *entry = web_find(web, &run->key, run->data);
		if (entry && entry->reply->received + r->keep * NS > now)
			hit = web_reply_get(entry->reply);
		else
			join = web_find_run(web, &run->key, run->data, r->timeout);
	}
	/* Only home changes a wait that is on a run's list, until home posts
	 * it.  So the function changes only a wait that has a stored
	 * response. */
	if (hit) {
		wait->reply = hit;
		wait->done = true;
	}
	bool full = !hit && !join && web->n_runs >= WEB_RUNS;
	struct web_run *to = hit || full ? nullptr : join ? join : run;
	if (to) {
		list_append(&to->waits, &wait->hook);
		wait->run = to;
		to->most = r->keep > to->most ? r->keep : to->most;
	}
	if (to == run) {
		++web->n_runs;
		if (r->keep)
			list_append(&web->runs, &run->hook);
	}
	pthread_mutex_unlock(&web->lock);

	if (to == run) {
		loop_post(ask->home, &run->to_home);
		*e = 0;
		return wait;
	}
	free(run);
	if (full) {
		free(wait);
		*e = EAGAIN;
		return nullptr;
	}
	*e = 0;
	return wait;
}

bool
web_wait_done (struct web_wait const *wait)
{
	return wait->done;
}

void
web_wait_response (struct web_wait const *wait,
                   struct web_response   *response,
                   uint64_t               now)
{
	struct web_reply const *reply = wait->reply;
	if (!reply) {
		*response = (struct web_response){
			.body = "",
			.type = "",
			.error = "askai ran out of memory.",
			.e = ENOMEM
		};
		return;
	}
	*response = (struct web_response){
		.body = reply->body ? reply->body : "",
		.len = reply->len,
		.type = reply->type,
		.error = reply->error,
		.status = reply->status,
		.age = now > reply->received ? (uint32_t)((now - reply->received) / NS) : 0,
		.retry = reply->retry,
		.e = reply->e
	};
}

void
web_wait_leave (struct web_wait **p_wait)
{
	if (!p_wait || !*p_wait)
		return;
	struct web_wait *wait = *p_wait;
	*p_wait = nullptr;
	if (wait->done) {
		web_reply_put(wait->reply);
		free(wait);
		return;
	}
	struct web *web = wait->web;
	bool cancel = false;
	pthread_mutex_lock(&web->lock);
	struct web_run *run = wait->run;
	if (run) {
		list_del(&wait->hook);
		/* A run whose keep is 0 has one wait.  The call posts the message
		 * with a reference if CANCEL makes the word of bits non-zero, so
		 * the run is not freed before home has handled the message. */
		cancel = !run->keep && !atomic_fetch_or(&run->bits, RUN_CANCEL);
		if (cancel)
			atomic_fetch_add_explicit(&run->refs, 1, memory_order_relaxed);
	}
	pthread_mutex_unlock(&web->lock);
	/* Without a run, home has moved the wait, and the handler of the
	 * wait's message frees it. */
	if (!run)
		return;
	free(wait);
	if (cancel)
		loop_post(run->home, &run->to_home);
}
