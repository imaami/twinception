/** @file A plugin for the tests (tests/askai.py).  It has three tools.
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
 * allow.  askai must go on working after them, and nothing of one call may
 * reach another call.  check_misuse (tests/askai.py) runs it. */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "plugin.h"

PLUGIN_MANIFEST(.name = "test",
                .tools = { "test_echo", "test_state", "test_misuse" });

/* The plugin's instance: what test_state stores for one user on one loop. */
struct test_plugin_instance {
	uint64_t principal;
	uint64_t calls;
	size_t   len;       /* the length of last in bytes */
	char     last[64];  /* the first bytes of the "text" argument that a
	                       call stored last, or "dropped" */
};

/* The memory of a test_state call that waits.  The pointer that
 * mcp_slot_own() returns for the call points to it. */
struct test_plugin_wait {
	struct loop_timer  timer;
	struct mcp_slot   *slot;
	bool               fail;  /* the timer ends the call with
	                             mcp_slot_fail() */
};

/* The plugin's instance_init().  It stores the user of the instance and
 * sets the number of calls to 0. */
static int
test_plugin_instance_init (void        *instance,
                           uint64_t     principal,
                           struct loop *loop)
{
	(void)loop;
	struct test_plugin_instance *inst = instance;
	*inst = (struct test_plugin_instance){ .principal = principal };
	return 0;
}

/* Returns the definition of test_echo as a new reference, or nullptr if
 * json-c cannot allocate memory. */
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
 * string. */
static void
test_plugin_echo_start (struct mcp_slot    *slot,
                        struct json_object *args)
{
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
 * json-c cannot allocate memory. */
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
 * mcp_slot_fail() and EIO. */
static void
test_plugin_wait_fired (struct loop       *loop,
                        struct loop_timer *timer)
{
	(void)loop;
	struct test_plugin_wait *w = (struct test_plugin_wait *)timer;
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
 * call's timer ends the call later. */
static void
test_plugin_state_start (struct mcp_slot    *slot,
                         struct json_object *args)
{
	struct test_plugin_instance *inst = mcp_slot_instance(slot);
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
	struct test_plugin_wait *w = malloc(sizeof *w);
	if (!w) {
		mcp_slot_fail(slot, ENOMEM);
		return;
	}
	struct loop *loop = mcp_slot_loop(slot);
	*w = (struct test_plugin_wait){
		.timer = { .due = UINT64_MAX, .fn = test_plugin_wait_fired },
		.slot = slot,
		.fail = json_object_get_boolean(fail),
	};
	*mcp_slot_own(slot) = w;
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
 * attempt.  check_misuse drops such calls. */
static void
test_plugin_state_drop (struct mcp_slot *slot)
{
	struct test_plugin_wait *w = *mcp_slot_own(slot);
	loop_timer_stop(mcp_slot_loop(slot), &w->timer);
	free(w);
	*mcp_slot_own(slot) = nullptr;
	static char const dropped[] = "dropped";
	struct test_plugin_instance *inst = mcp_slot_instance(slot);
	inst->len = sizeof dropped - 1;
	memcpy(inst->last, dropped, inst->len);
	mcp_slot_tell(slot, TOOL_TEXT("dropped"), 0);
}

/* Returns the definition of test_misuse as a new reference, or nullptr if
 * json-c cannot allocate memory. */
static struct json_object *
test_plugin_misuse_definition (void const *service)
{
	(void)service;
	return json_tokener_parse(
		"{\"name\":\"test_misuse\","
		"\"description\":\"Uses the call API in ways that it does not allow.\","
		"\"inputSchema\":{\"type\":\"object\","
		"\"properties\":{\"case\":{\"type\":\"string\","
		"\"enum\":[\"ready\",\"own\",\"after\"]}},"
		"\"required\":[\"case\"],"
		"\"additionalProperties\":false}}");
}

/* Starts a test_misuse call.  The kind of test_misuse has no step() and no
 * drop().  The argument "case" selects what the call does:
 * - "ready": the call calls mcp_slot_ready().  The kind has no step(), so
 *   askai ends the call as failed.
 * - "own": the call sets the mcp_slot_own() pointer, and then it ends with
 *   the text "own".  The kind has no drop(), so askai calls none.
 * - "after": the call ends with the text "first".  Then it calls each
 *   function of the call API that ends or changes a call, and askai must
 *   ignore each of these calls.  The function stores the error numbers
 *   that mcp_slot_write(), mcp_slot_name() and mcp_slot_offset() return in
 *   the call's instance, as the text "write=W name=N offset=O".  The next
 *   test_state call of the user shows that text.
 * Any other case ends the call as failed. */
static void
test_plugin_misuse_start (struct mcp_slot    *slot,
                          struct json_object *args)
{
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
	if (!strcmp(what, "own")) {
		*mcp_slot_own(slot) = inst;
		mcp_slot_tell(slot, TOOL_TEXT("own"), 0);
		return;
	}
	if (strcmp(what, "after")) {
		mcp_slot_tell(slot, TOOL_TEXT("The case argument is not known."), TOOL_FAILED);
		return;
	}
	mcp_slot_tell(slot, TOOL_TEXT("first"), 0);
	size_t taken = 0;
	struct handle who;
	int write = mcp_slot_write(slot, TOOL_TEXT("late"), 0, &taken);
	int name = mcp_slot_name(slot, &who);
	/* args is not valid after the end of the call, so the function passes
	 * nullptr. */
	int offset = mcp_slot_offset(slot, nullptr);
	mcp_slot_tell(slot, TOOL_TEXT("second"), 0);
	mcp_slot_finish(slot, 0);
	mcp_slot_fail(slot, EIO);
	mcp_slot_ready(slot);
	mcp_slot_park(slot);
	int n = snprintf(inst->last, sizeof inst->last, "write=%d name=%d offset=%d",
	                 write, name, offset);
	inst->len = n > 0 && n < (int)sizeof inst->last ? (size_t)n : 0;
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
	},
	.instance_init = test_plugin_instance_init,
	.instance_size = sizeof (struct test_plugin_instance)
};
