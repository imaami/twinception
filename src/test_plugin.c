/** @file test_plugin.c
 *
 * A plugin for the tests (tests/askai.py).  It has five tools.
 *
 * The output of a test_echo call is the value of the call's string argument
 * "text".
 *
 * The output of a test_state call has four values of the call's instance,
 * as they are when the call ends.  The values are the principal of the
 * instance's user, the address of the instance, the number of test_state
 * calls that have started with the instance, and the text that the instance
 * stores.  A call without "wait_ms" ends first and then stores its "text"
 * argument in the instance, so its output has the text that an earlier call
 * stored.  A call with "wait_ms" stores its "text" argument when it starts,
 * and it ends that many milliseconds later.  With "fail" true as well, the
 * call then ends with mcp_slot_fail() instead.  The drop() of a call with
 * "wait_ms" stores the text "dropped" in the instance.
 *
 * test_misuse calls the call API in ways that tool.h and mcp.h do not
 * allow, and passes the exported functions inputs that they must refuse.
 * askai must go on working after them, and nothing of one call may reach
 * another call.  check_misuse (tests/askai.py) runs it.
 *
 * test_fetch sends an HTTP request with mcp_slot_fetch() and parks.  Its
 * step() writes the response's status, type, age, retry, error number and
 * error text, one to a line, then an empty line and the body.  check_web
 * (tests/askai.py) runs it.
 *
 * The plugin has one option, "greeting", a string of at most 63 bytes,
 * whose default is "hello".  The output of a test_config call is
 * "greeting=" and the greeting of the plugin's configuration, or "none" if
 * the call gets no configuration.  check_plugin runs it.
 *
 * Each function that askai calls checks the pointers that it gets once, at
 * its start, and the plugin checks what the getters of the call API
 * return before it uses that.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "plugin.h"

PLUGIN_MANIFEST(.name = "test",
                .tools = { "test_echo", "test_state", "test_misuse", "test_fetch",
                           "test_config" });

/* The plugin's configuration. */
struct test_plugin_config {
	char greeting[64];  /* the option "greeting", NUL-terminated */
};

/* The plugin's instance: what test_state stores for one user on one loop. */
struct test_plugin_instance {
	uint64_t principal;
	uint64_t calls;
	size_t   len;       /* the length of last in bytes */
	char     last[64];  /* the first bytes of the "text" argument that a
	                       call stored last, or "dropped" */
};

/* The memory of a test_state call that waits.  The pointer that
 * mcp_slot_own() returns for the call points to it.
 */
struct test_plugin_wait {
	struct loop_timer  timer;
	struct mcp_slot   *slot;
	bool               fail;  /* the timer ends the call with
	                             mcp_slot_fail() */
};

/* The plugin's instance_init().  It stores the user of the instance and
 * sets the number of calls to 0.  Returns 0, or EINVAL if askai passed no
 * instance or no loop.
 */
static int
test_plugin_instance_init (void        *instance,
                           uint64_t     principal,
                           struct loop *loop)
{
	if (!instance || !loop)
		return EINVAL;
	struct test_plugin_instance *inst = instance;
	*inst = (struct test_plugin_instance){ .principal = principal };
	return 0;
}

/* Returns the definition of test_echo as a new reference, or nullptr if
 * json-c cannot allocate memory.
 */
static struct json_object *
test_plugin_echo_definition (void const *service)
{
	(void)service;
	return json_tokener_parse(
		"{\"name\":\"test_echo\","
		"\"description\":\"Returns the text argument unchanged.\","
		"\"inputSchema\":{\"type\":\"object\","
		"\"properties\":{\"text\":{\"type\":\"string\"}},"
		"\"required\":[\"text\"],"
		"\"additionalProperties\":false}}");
}

/* Ends the call before it returns.  The output is the value of the string
 * argument "text".  The call fails if "text" is missing or is not a
 * string.
 */
static void
test_plugin_echo_start (struct mcp_slot    *slot,
                        struct json_object *args)
{
	if (!slot)
		return;
	struct json_object *text;
	if (!json_object_object_get_ex(args, "text", &text) ||
	    !json_object_is_type(text, json_type_string)) {
		mcp_slot_tell(slot, TOOL_TEXT("The text argument must be a string."),
		              TOOL_FAILED);
		return;
	}
	/* mcp_slot_write() copies the bytes, so nothing reads args after the
	 * call has ended. */
	struct tool_text const out = {
		json_object_get_string(text),
		(size_t)json_object_get_string_len(text)
	};
	size_t taken;
	int e = mcp_slot_write(slot, out, 0, &taken);
	if (e)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_finish(slot, 0);
}

/* Returns the definition of test_state as a new reference, or nullptr if
 * json-c cannot allocate memory.
 */
static struct json_object *
test_plugin_state_definition (void const *service)
{
	(void)service;
	return json_tokener_parse(
		"{\"name\":\"test_state\","
		"\"description\":\"Tells the state of the caller's instance.\","
		"\"inputSchema\":{\"type\":\"object\","
		"\"properties\":{\"text\":{\"type\":\"string\"},"
		"\"wait_ms\":{\"type\":\"integer\"},"
		"\"fail\":{\"type\":\"boolean\"}},"
		"\"additionalProperties\":false}}");
}

/* Ends the call.  The output is the state of the call's instance. */
static void
test_plugin_state_tell (struct mcp_slot *slot)
{
	struct test_plugin_instance const *inst = mcp_slot_instance(slot);
	if (!inst) {
		mcp_slot_tell(slot, TOOL_TEXT("The call has no instance."), TOOL_FAILED);
		return;
	}
	char out[160];
	int n = snprintf(out, sizeof out, "principal=%016" PRIx64 " instance=%p "
	                 "calls=%" PRIu64 " last=%.*s", inst->principal,
	                 (void const *)inst, inst->calls, (int)inst->len, inst->last);
	if (n < 0 || n >= (int)sizeof out)
		mcp_slot_fail(slot, ENOMEM);
	else
		mcp_slot_tell(slot, (struct tool_text){ out, (size_t)n }, 0);
}

/* The timer function of a call that waits.  It runs when the wait is over,
 * and it ends the call.  If the call has "fail" true, it ends the call with
 * mcp_slot_fail() and EIO.
 */
static void
test_plugin_wait_fired (struct loop       *loop,
                        struct loop_timer *timer)
{
	(void)loop;
	struct test_plugin_wait *w = (struct test_plugin_wait *)timer;
	if (!w || !w->slot)
		return;
	if (w->fail)
		mcp_slot_fail(w->slot, EIO);
	else
		test_plugin_state_tell(w->slot);
}

/* Starts a test_state call.  It adds 1 to the number of calls of the call's
 * instance.
 *
 * Without "wait_ms", it ends the call with the state of the instance and
 * then stores "text" in the instance.  The instance is still valid after
 * the end of the call, but args is not, so the function copies the text
 * before it ends the call.
 *
 * With "wait_ms", it stores "text" in the instance and parks the call.  The
 * call's timer ends the call later.
 */
static void
test_plugin_state_start (struct mcp_slot    *slot,
                         struct json_object *args)
{
	if (!slot)
		return;
	struct test_plugin_instance *inst = mcp_slot_instance(slot);
	if (!inst) {
		mcp_slot_tell(slot, TOOL_TEXT("The call has no instance."), TOOL_FAILED);
		return;
	}
	++inst->calls;
	if (inst->principal != mcp_slot_principal(slot)) {
		mcp_slot_tell(slot, TOOL_TEXT("The instance is another user's."), TOOL_FAILED);
		return;
	}
	struct json_object *text = nullptr;
	struct json_object *wait = nullptr;
	struct json_object *fail = nullptr;
	(void)json_object_object_get_ex(args, "text", &text);
	(void)json_object_object_get_ex(args, "fail", &fail);
	bool waits = json_object_object_get_ex(args, "wait_ms", &wait);
	uint64_t ms = (uint64_t)json_object_get_int64(wait);
	char now[sizeof inst->last];
	size_t len = text ? (size_t)json_object_get_string_len(text) : 0;
	if (len > sizeof now)
		len = sizeof now;
	if (len)
		memcpy(now, json_object_get_string(text), len);
	if (!waits)
		test_plugin_state_tell(slot);
	memcpy(inst->last, now, len);
	inst->len = len;
	if (!waits)
		return;
	struct loop *loop = mcp_slot_loop(slot);
	void **own = mcp_slot_own(slot);
	struct test_plugin_wait *w = loop && own ? malloc(sizeof *w) : nullptr;
	if (!w) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	*w = (struct test_plugin_wait){
		.timer = { .due = UINT64_MAX, .fn = test_plugin_wait_fired },
		.slot = slot,
		.fail = json_object_get_boolean(fail),
	};
	*own = w;
	int e = loop_timer_set(loop, &w->timer, loop_now(loop) + ms * 1'000'000);
	if (e)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_park(slot);
}

/* This function is the drop() of test_state.  askai calls drop() only for
 * a call whose mcp_slot_own() pointer is set, and test_state sets the
 * pointer only for a call with "wait_ms".  The function stops the call's
 * timer and frees the call's memory.  Then it stores the text "dropped" in
 * the call's instance.  check_instances (tests/askai.py) reads that text.
 * The text shows that mcp_slot_instance() returns the instance inside
 * drop().  Last, the function tries to end the call, which drop() must not
 * do.  askai has ended the call before it calls drop(), so it ignores the
 * attempt.  check_misuse drops such calls.
 */
static void
test_plugin_state_drop (struct mcp_slot *slot)
{
	void **own = slot ? mcp_slot_own(slot) : nullptr;
	struct test_plugin_wait *w = own ? *own : nullptr;
	if (!w)
		return;
	/* If the timer cannot be stopped, it can still fire, so the function
	 * keeps the memory that the timer is in. */
	if (loop_timer_stop(mcp_slot_loop(slot), &w->timer))
		return;
	free(w);
	*own = nullptr;
	static char const dropped[] = "dropped";
	struct test_plugin_instance *inst = mcp_slot_instance(slot);
	if (inst) {
		inst->len = sizeof dropped - 1;
		memcpy(inst->last, dropped, inst->len);
	}
	mcp_slot_tell(slot, TOOL_TEXT("dropped"), 0);
}

/* Returns the definition of test_misuse as a new reference, or nullptr if
 * json-c cannot allocate memory.
 */
static struct json_object *
test_plugin_misuse_definition (void const *service)
{
	(void)service;
	return json_tokener_parse(
		"{\"name\":\"test_misuse\","
		"\"description\":\"Uses the call API in ways that it does not allow.\","
		"\"inputSchema\":{\"type\":\"object\","
		"\"properties\":{\"case\":{\"type\":\"string\","
		"\"enum\":[\"ready\",\"own\",\"after\",\"invalid\",\"handles\"]}},"
		"\"required\":[\"case\"],"
		"\"additionalProperties\":false}}");
}

/* The timer function of the timers that test_plugin_misuse_invalid()
 * passes.  askai must never call it.
 */
static void
test_plugin_never (struct loop       *loop,
                   struct loop_timer *timer)
{
	(void)loop;
	(void)timer;
	abort();
}

/* The function of the message that test_plugin_misuse_invalid() posts.  It
 * frees the message.
 */
static void
test_plugin_msg_free (struct loop *loop,
                      struct msg  *msg)
{
	(void)loop;
	free(msg);
}

/* Passes each exported function of askai inputs that the function must
 * refuse: nullptr, a text with a length and no bytes, flag bits that the
 * function does not take, a timer without a function, a timer whose slot is
 * greater than the number of timers in the loop's heap, a timer whose slot
 * is the place of another timer in the heap, a message without
 * a function, a message that is posted already, and a unit of work out of
 * range.  Then it ends the call with
 * the text "refused R of N", where N is the number of these inputs and R is
 * the number of them that the functions refused as their documentation
 * says.
 */
static void
test_plugin_misuse_invalid (struct mcp_slot *slot,
                            struct loop     *loop)
{
	struct tool_text const none = { nullptr, 5 };
	struct loop_timer bare = { .due = UINT64_MAX };
	struct loop_timer stray = { .due = UINT64_MAX, .fn = test_plugin_never, .slot = 1000 };
	struct msg bare_msg = {};
	struct handle who = { 1 };
	enum loop_unit const beyond = (enum loop_unit)(LOOP_UNITS + 7);
	uint32_t const unknown = 1u << 7;
	size_t taken;
	unsigned refused = 0;
	unsigned checks = 0;
#define TEST_PLUGIN_REFUSED(cond) (++checks, refused += !!(cond))
	TEST_PLUGIN_REFUSED(!mcp_slot_instance(nullptr));
	TEST_PLUGIN_REFUSED(!mcp_slot_loop(nullptr));
	TEST_PLUGIN_REFUSED(!mcp_slot_principal(nullptr));
	TEST_PLUGIN_REFUSED(!mcp_slot_own(nullptr));
	TEST_PLUGIN_REFUSED(!mcp_slot_config(nullptr));
	TEST_PLUGIN_REFUSED(mcp_slot_offset(nullptr, nullptr) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_write(nullptr, TOOL_TEXT("x"), 0, &taken) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_write(slot, none, 0, &taken) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_write(slot, TOOL_TEXT("x"), 0, nullptr) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_write(slot, TOOL_TEXT("x"), unknown, &taken) == EINVAL);
	TEST_PLUGIN_REFUSED(mcp_slot_name(nullptr, &who) == EFAULT && !who.bits);
	TEST_PLUGIN_REFUSED(mcp_slot_name(slot, nullptr) == EFAULT);
	TEST_PLUGIN_REFUSED(!mcp_slot_named(nullptr, who));
	TEST_PLUGIN_REFUSED(mcp_slot_park(nullptr) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_ready(nullptr) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_tell(nullptr, TOOL_TEXT("x"), 0) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_tell(slot, none, 0) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_tell(slot, TOOL_TEXT("x"), unknown) == EINVAL);
	TEST_PLUGIN_REFUSED(mcp_slot_finish(nullptr, 0) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_finish(slot, unknown) == EINVAL);
	TEST_PLUGIN_REFUSED(mcp_slot_fail(nullptr, EIO) == EFAULT);
	TEST_PLUGIN_REFUSED(loop_post(nullptr, &bare_msg) == EFAULT);
	TEST_PLUGIN_REFUSED(loop_post(loop, nullptr) == EFAULT);
	TEST_PLUGIN_REFUSED(loop_post(loop, &bare_msg) == EFAULT);
	/* The message's function frees the message, so askai must call it
	 * once.  The second post must therefore be refused. */
	struct msg *twice = malloc(sizeof *twice);
	if (twice)
		*twice = (struct msg){ .fn = test_plugin_msg_free };
	TEST_PLUGIN_REFUSED(twice && !loop_post(loop, twice) &&
	                    loop_post(loop, twice) == EALREADY);
	TEST_PLUGIN_REFUSED(loop_timer_set(nullptr, &stray, UINT64_MAX) == EFAULT);
	TEST_PLUGIN_REFUSED(loop_timer_set(loop, nullptr, UINT64_MAX) == EFAULT);
	TEST_PLUGIN_REFUSED(loop_timer_set(loop, &bare, UINT64_MAX) == EFAULT);
	TEST_PLUGIN_REFUSED(loop_timer_set(loop, &stray, UINT64_MAX) == EINVAL);
	TEST_PLUGIN_REFUSED(loop_timer_stop(nullptr, &stray) == EFAULT);
	TEST_PLUGIN_REFUSED(loop_timer_stop(loop, nullptr) == EFAULT);
	TEST_PLUGIN_REFUSED(loop_timer_stop(loop, &stray) == EINVAL);
	/* A copy of a timer in the heap has the slot of that timer, but it is
	 * not that timer.  The heap must keep the timer, which the function
	 * then stops, because the timer is on the stack. */
	struct loop_timer real = { .due = UINT64_MAX, .fn = test_plugin_never };
	int const armed = loop_timer_set(loop, &real, UINT64_MAX);
	struct loop_timer copy = real;
	TEST_PLUGIN_REFUSED(!armed && loop_timer_set(loop, &copy, UINT64_MAX) == EINVAL);
	TEST_PLUGIN_REFUSED(!armed && loop_timer_stop(loop, &copy) == EINVAL);
	TEST_PLUGIN_REFUSED(!armed && real.slot && !loop_timer_stop(loop, &real) && !real.slot);
	TEST_PLUGIN_REFUSED(!loop_now(nullptr));
	TEST_PLUGIN_REFUSED(loop_work(nullptr, LOOP_COPY, 1) == EFAULT);
	TEST_PLUGIN_REFUSED(loop_work(loop, beyond, 1) == EINVAL);
	TEST_PLUGIN_REFUSED(!loop_room(nullptr, LOOP_COPY));
	TEST_PLUGIN_REFUSED(!loop_room(loop, beyond));
	/* The kind of test_misuse has no step(), but mcp_slot_fetch() checks
	 * its inputs first, so it does not end the call for them. */
	struct web_request const get = {
		.url = "http://127.0.0.1/", .max = 1, .timeout = 1
	};
	struct web_request const no_url = { .max = 1, .timeout = 1 };
	struct web_request const no_body = {
		.url = "http://127.0.0.1/", .len = 1, .max = 1, .timeout = 1
	};
	struct web_request const ftp = {
		.url = "ftp://127.0.0.1/", .max = 1, .timeout = 1
	};
	struct web_response response;
	TEST_PLUGIN_REFUSED(mcp_slot_fetch(nullptr, &get) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_fetch(slot, nullptr) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_fetch(slot, &no_url) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_fetch(slot, &no_body) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_fetch(slot, &ftp) == EINVAL);
	TEST_PLUGIN_REFUSED(mcp_slot_fetched(nullptr, &response) == EFAULT &&
	                    !response.body);
	TEST_PLUGIN_REFUSED(mcp_slot_fetched(slot, nullptr) == EFAULT);
	TEST_PLUGIN_REFUSED(mcp_slot_fetched(slot, &response) == ENOENT);
#undef TEST_PLUGIN_REFUSED
	char out[48];
	int n = snprintf(out, sizeof out, "refused %u of %u", refused, checks);
	if (n < 0 || n >= (int)sizeof out)
		mcp_slot_fail(slot, ENOMEM);
	else
		mcp_slot_tell(slot, (struct tool_text){ out, (size_t)n }, 0);
}

/* Names the call with mcp_slot_name(), and then makes handles of the slots
 * of the loop's table near the call's own, with the generations from 1 to
 * 256 more than its own.  mcp_slot_named() must return nullptr or the call's
 * own slot for each of them.  The table also names requests, and a handle
 * of a request must not give a slot.  The call ends with the text "foreign
 * F own O", where F is the number of handles that gave another object and O
 * is the number of handles that gave the call's own slot.
 */
static void
test_plugin_misuse_handles (struct mcp_slot *slot,
                            struct loop     *loop)
{
	struct handle own;
	if (mcp_slot_name(slot, &own)) {
		mcp_slot_tell(slot, TOOL_TEXT("The call has no handle."), TOOL_FAILED);
		return;
	}
	/* From the highest bit, a handle has 8 bits for the loop, 24 bits for
	 * the slot of the table and 32 bits for the generation. */
	uint64_t const at = own.bits >> 32 & 0xffffff;
	uint64_t const last = (own.bits & 0xffffffff) + 256;
	uint64_t const top = own.bits & UINT64_C(0xff) << 56;
	unsigned foreign = 0;
	unsigned found = 0;
	for (uint64_t i = at > 8 ? at - 8 : 0; i <= at + 8; ++i)
		for (uint64_t gen = 1; gen <= last; ++gen) {
			struct handle const h = { top | i << 32 | gen };
			struct mcp_slot const *named = mcp_slot_named(loop, h);
			foreign += named && named != slot;
			found += named == slot;
		}
	char out[48];
	int n = snprintf(out, sizeof out, "foreign %u own %u", foreign, found);
	if (n < 0 || n >= (int)sizeof out)
		mcp_slot_fail(slot, ENOMEM);
	else
		mcp_slot_tell(slot, (struct tool_text){ out, (size_t)n }, 0);
}

/* Starts a test_misuse call.  The kind of test_misuse has no step() and no
 * drop().  The argument "case" selects what the call does:
 * - "ready": the call calls mcp_slot_ready().  The kind has no step(), so
 *   askai ends the call as failed.
 * - "own": the call sets the mcp_slot_own() pointer, and then it ends with
 *   the text "own".  The kind has no drop(), so askai calls none.
 * - "after": the call ends with the text "first".  Then it calls each
 *   function of the call API that ends or changes a call, and
 *   mcp_slot_fetched().  Each of them must refuse with EINVAL.  The
 *   function stores the text "refused R of N" in the call's instance, where
 *   N is the number of these calls and R is the number of them that
 *   returned EINVAL.  The next test_state call of the user shows that
 *   text.
 * - "invalid": test_plugin_misuse_invalid() runs.
 * - "handles": test_plugin_misuse_handles() runs.
 * Any other case ends the call as failed.
 */
static void
test_plugin_misuse_start (struct mcp_slot    *slot,
                          struct json_object *args)
{
	if (!slot)
		return;
	struct json_object *arg = nullptr;
	(void)json_object_object_get_ex(args, "case", &arg);
	char const *what = json_object_get_string(arg);
	if (!what)
		what = "";
	if (!strcmp(what, "ready")) {
		mcp_slot_ready(slot);
		return;
	}
	struct test_plugin_instance *inst = mcp_slot_instance(slot);
	struct loop *loop = mcp_slot_loop(slot);
	void **own = mcp_slot_own(slot);
	if (!inst || !loop || !own) {
		mcp_slot_tell(slot, TOOL_TEXT("The call has no instance."), TOOL_FAILED);
		return;
	}
	if (!strcmp(what, "own")) {
		*own = inst;
		mcp_slot_tell(slot, TOOL_TEXT("own"), 0);
		return;
	}
	if (!strcmp(what, "invalid")) {
		test_plugin_misuse_invalid(slot, loop);
		return;
	}
	if (!strcmp(what, "handles")) {
		test_plugin_misuse_handles(slot, loop);
		return;
	}
	if (strcmp(what, "after")) {
		mcp_slot_tell(slot, TOOL_TEXT("The case argument is not known."), TOOL_FAILED);
		return;
	}
	mcp_slot_tell(slot, TOOL_TEXT("first"), 0);
	size_t taken = 0;
	struct handle who;
	struct web_request const get = {
		.url = "http://127.0.0.1/", .max = 1, .timeout = 1
	};
	struct web_response response;
	/* args is not valid after the end of the call, so the function passes
	 * nullptr to mcp_slot_offset(). */
	int const codes[] = {
		mcp_slot_write(slot, TOOL_TEXT("late"), 0, &taken),
		mcp_slot_name(slot, &who),
		mcp_slot_offset(slot, nullptr),
		mcp_slot_tell(slot, TOOL_TEXT("second"), 0),
		mcp_slot_finish(slot, 0),
		mcp_slot_fail(slot, EIO),
		mcp_slot_ready(slot),
		mcp_slot_park(slot),
		mcp_slot_fetch(slot, &get),
		mcp_slot_fetched(slot, &response),
	};
	unsigned refused = 0;
	for (size_t k = 0; k < sizeof codes / sizeof *codes; ++k)
		refused += codes[k] == EINVAL;
	int n = snprintf(inst->last, sizeof inst->last, "refused %u of %zu", refused,
	                 sizeof codes / sizeof *codes);
	inst->len = n > 0 && n < (int)sizeof inst->last ? (size_t)n : 0;
}

/* Returns the definition of test_fetch as a new reference, or nullptr if
 * json-c cannot allocate memory.
 */
static struct json_object *
test_plugin_fetch_definition (void const *service)
{
	(void)service;
	return json_tokener_parse(
		"{\"name\":\"test_fetch\","
		"\"description\":\"Sends an HTTP request through askai.\","
		"\"inputSchema\":{\"type\":\"object\","
		"\"properties\":{\"url\":{\"type\":\"string\"},"
		"\"body\":{\"type\":\"string\"},"
		"\"accept\":{\"type\":\"string\"},"
		"\"max\":{\"type\":\"integer\"},"
		"\"timeout\":{\"type\":\"integer\"},"
		"\"keep\":{\"type\":\"integer\"},"
		"\"twice\":{\"type\":\"boolean\"}},"
		"\"required\":[\"url\"],"
		"\"additionalProperties\":false}}");
}

/* Returns the string member @p name of @p args, and stores its length in
 * @p *len.  Returns nullptr if @p args has no such string.
 */
static char const *
test_plugin_string (struct json_object *args,
                    char const         *name,
                    size_t             *len)
{
	struct json_object *v = nullptr;
	*len = 0;
	if (!json_object_object_get_ex(args, name, &v) ||
	    !json_object_is_type(v, json_type_string))
		return nullptr;
	*len = (size_t)json_object_get_string_len(v);
	return json_object_get_string(v);
}

/* Returns the integer member @p name of @p args, or @p otherwise if
 * @p args has no such member.
 */
static int64_t
test_plugin_integer (struct json_object *args,
                     char const         *name,
                     int64_t             otherwise)
{
	struct json_object *v = nullptr;
	return json_object_object_get_ex(args, name, &v) ? json_object_get_int64(v)
	                                                 : otherwise;
}

/* Starts a test_fetch call.  It sends the request that the arguments
 * describe.  "url" is required.  "body" makes the request a POST.  "accept"
 * is the value of Accept.  "max" is 65536 bytes, "timeout" 10 seconds and
 * "keep" 0 unless the arguments have them.  askai checks the values, so the
 * function passes them as they are, cast to the types of web_request.  If
 * mcp_slot_fetch() fails, the call fails with the text "fetch: E", where E
 * is the error number.  With "twice" true, the function calls
 * mcp_slot_fetch() a second time after the first has succeeded, and the
 * call ends with the text "twice: E", where E is the error number of the
 * second call.
 */
static void
test_plugin_fetch_start (struct mcp_slot    *slot,
                         struct json_object *args)
{
	if (!slot)
		return;
	size_t url_len;
	size_t accept_len;
	struct web_request request = {
		.url = test_plugin_string(args, "url", &url_len),
		.accept = test_plugin_string(args, "accept", &accept_len),
		.max = (size_t)test_plugin_integer(args, "max", 65536),
		.timeout = (uint32_t)test_plugin_integer(args, "timeout", 10),
		.keep = (uint32_t)test_plugin_integer(args, "keep", 0)
	};
	request.body = test_plugin_string(args, "body", &request.len);
	struct json_object *arg = nullptr;
	(void)json_object_object_get_ex(args, "twice", &arg);
	int e = mcp_slot_fetch(slot, &request);
	bool twice = !e && json_object_get_boolean(arg);
	if (!e && !twice) {
		mcp_slot_park(slot);
		return;
	}
	if (twice)
		e = mcp_slot_fetch(slot, &request);
	char out[32];
	int n = snprintf(out, sizeof out, "%s: %d", twice ? "twice" : "fetch", e);
	if (n < 0 || n >= (int)sizeof out)
		mcp_slot_fail(slot, ENOMEM);
	else
		mcp_slot_tell(slot, (struct tool_text){ out, (size_t)n },
		              twice ? 0 : TOOL_FAILED);
}

/* The step() of test_fetch.  It writes the result of the call's request
 * and ends the call.  The kind has no drop(), because askai frees the
 * response.
 */
static void
test_plugin_fetch_step (struct mcp_slot *slot)
{
	if (!slot)
		return;
	struct web_response r;
	int e = mcp_slot_fetched(slot, &r);
	char head[640];
	int n = e ? snprintf(head, sizeof head, "fetched: %d", e)
	          : snprintf(head, sizeof head, "status %" PRIu32 "\ntype %.256s\nage %"
	                     PRIu32 "\nretry %" PRIu32 "\ne %d\nerror %.256s\n\n",
	                     r.status, r.type, r.age, r.retry, r.e, r.error);
	if (n < 0 || n >= (int)sizeof head) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	if (e) {
		mcp_slot_tell(slot, (struct tool_text){ head, (size_t)n }, TOOL_FAILED);
		return;
	}
	size_t taken;
	e = mcp_slot_write(slot, (struct tool_text){ head, (size_t)n }, 0, &taken);
	if (!e)
		e = mcp_slot_write(slot, (struct tool_text){ r.body, r.len }, 0, &taken);
	if (e)
		mcp_slot_fail(slot, e);
	else
		mcp_slot_finish(slot, 0);
}

/* Stores the reason of a refusal of the options in the @p len bytes at
 * @p why, formatted from @p fmt and its arguments.  Returns EINVAL.
 */
[[gnu::format(printf, 3, 4)]]
static int
test_plugin_refuse (char       *why,
                    size_t      len,
                    char const *fmt,
                    ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(why, len, fmt, ap);
	va_end(ap);
	if (n < 0)
		*why = '\0';
	return EINVAL;
}

/* The plugin's configure().  It stores the default greeting, and then the
 * option "greeting" if @p options has it.  It refuses another member, and a
 * greeting that is not a string of at most 63 bytes.
 */
static int
test_plugin_configure (void               *config,
                       struct json_object *options,
                       char               *why,
                       size_t              len)
{
	if (!config || !why || !len)
		return EINVAL;
	struct test_plugin_config *c = config;
	static char const hello[] = "hello";
	memcpy(c->greeting, hello, sizeof hello);
	if (!options)
		return 0;
	if (!json_object_is_type(options, json_type_object))
		return test_plugin_refuse(why, len, "not an object");
	struct json_object_iter it;
	json_object_object_foreachC(options, it) {
		if (strcmp(it.key, "greeting"))
			return test_plugin_refuse(why, len, "unknown option: %s", it.key);
		size_t n = json_object_is_type(it.val, json_type_string)
		         ? (size_t)json_object_get_string_len(it.val) : sizeof c->greeting;
		if (n >= sizeof c->greeting)
			return test_plugin_refuse(why, len, "greeting: not a string of at "
			                          "most %zu bytes", sizeof c->greeting - 1);
		memcpy(c->greeting, json_object_get_string(it.val), n + 1);
	}
	return 0;
}

/* Returns the definition of test_config as a new reference, or nullptr if
 * json-c cannot allocate memory.  @p service is the configuration, or
 * nullptr if the plugin has none.  The description ends with the greeting
 * in parentheses, or with "(none)".
 */
static struct json_object *
test_plugin_config_definition (void const *service)
{
	struct test_plugin_config const *c = service;
	char text[sizeof "Returns the greeting of the plugin's options ()." +
	          sizeof c->greeting];
	int n = snprintf(text, sizeof text, "Returns the greeting of the plugin's "
	                 "options (%s).", c ? c->greeting : "none");
	if (n < 0 || n >= (int)sizeof text)
		return nullptr;
	struct json_object *def = json_tokener_parse(
		"{\"name\":\"test_config\","
		"\"inputSchema\":{\"type\":\"object\","
		"\"properties\":{},"
		"\"additionalProperties\":false}}");
	struct json_object *desc = json_object_new_string_len(text, n);
	if (!def || !desc || json_object_object_add(def, "description", desc)) {
		json_object_put(desc);
		json_object_put(def);
		return nullptr;
	}
	return def;
}

/* Ends the call with "greeting=" and the greeting of the configuration, or
 * with "none" if mcp_slot_config() returns nullptr.
 */
static void
test_plugin_config_start (struct mcp_slot    *slot,
                          struct json_object *args)
{
	(void)args;
	if (!slot)
		return;
	struct test_plugin_config const *c = mcp_slot_config(slot);
	if (!c) {
		mcp_slot_tell(slot, TOOL_TEXT("none"), 0);
		return;
	}
	char out[sizeof "greeting=" + sizeof c->greeting];
	int n = snprintf(out, sizeof out, "greeting=%s", c->greeting);
	if (n < 0 || n >= (int)sizeof out)
		mcp_slot_fail(slot, ENOMEM);
	else
		mcp_slot_tell(slot, (struct tool_text){ out, (size_t)n }, 0);
}

struct plugin const askai_plugin = {
	.tools = {
		{
			.definition = test_plugin_echo_definition,
			.start = test_plugin_echo_start,
			.flags = TOOL_LENT
		},
		{
			.definition = test_plugin_state_definition,
			.start = test_plugin_state_start,
			.drop = test_plugin_state_drop,
			.flags = TOOL_LENT | TOOL_DETACH
		},
		{
			.definition = test_plugin_misuse_definition,
			.start = test_plugin_misuse_start,
			.flags = TOOL_LENT
		},
		{
			.definition = test_plugin_fetch_definition,
			.start = test_plugin_fetch_start,
			.step = test_plugin_fetch_step
		},
		{
			.definition = test_plugin_config_definition,
			.start = test_plugin_config_start
		},
	},
	.instance_init = test_plugin_instance_init,
	.configure = test_plugin_configure,
	.instance_size = sizeof (struct test_plugin_instance),
	.config_size = sizeof (struct test_plugin_config)
};
