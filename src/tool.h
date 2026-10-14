/** @file The tools that askai offers, as masks of their ids, which roles
 *  grant (src/policy.h) and requests carry (src/stream.h); and the kinds of
 *  tool they are, what their calls do, with the call API of src/mcp.h.
 *  start() ends or parks a call; step() does a share of its work; drop()
 *  lets go of its own word.  All run on the call's loop, and never block. */
#ifndef TWINCEPTION_SRC_TOOL_H_
#define TWINCEPTION_SRC_TOOL_H_

#include <stddef.h>
#include <stdint.h>

struct json_object;
struct mcp_slot;

/** The tools askai may offer at most: a mask's width; as an id, none. */
static constexpr unsigned TOOL_MAX = 128;

/** Tools, bit i that of id i. */
typedef unsigned _BitInt(TOOL_MAX) tool_mask;

enum {
	TOOL_TELL_MAX   = 4 << 10,   /**< bytes mcp_slot_tell() takes */
	TOOL_OUTPUT_MAX = 16 << 20,  /**< bytes a call may write */
};

/** UTF-8 text, its giver's. */
struct tool_text {
	char const *ptr;
	size_t      len;
};

/** A string literal's text; a pointer does not compile. */
#define TOOL_TEXT(s) ((struct tool_text){ "" s, sizeof s - 1 })

enum tool_flag : uint64_t {
	TOOL_LENT   = 1u << 0,  /**< turns may call it, as the role grants */
	TOOL_PAGED  = 1u << 1,  /**< output read on from "offset", which
	                             start() takes with mcp_slot_offset() */
	TOOL_DETACH = 1u << 2,  /**< a turn may run it in the background */
	TOOL_STREAM = 1u << 3,  /**< parking opens the event stream */
};

/** A plugin's kinds may have these; askai's have more. */
static constexpr uint64_t TOOL_FLAGS = TOOL_LENT | TOOL_PAGED | TOOL_DETACH |
                                       TOOL_STREAM;

/** How a call's output ends, or goes on. */
enum tool_out : uint32_t {
	TOOL_FAILED = 1u << 0,  /**< tell, finish: the call failed */
	TOOL_MORE   = 1u << 1,  /**< write: more output follows */
};

/** Returns the definition of the tool as tools/list shows it.  The result
 *  is a new reference, or nullptr if out of memory.  askai calls the
 *  function when it starts.  For a plugin that askai loads from a file, it
 *  also calls the function once as it loads the plugin, to check that the
 *  definition's member "name" is the tool's name in the manifest.
 *  @p service is the service of a built-in plugin, which is the object that
 *  every loop shares.  For a plugin that askai loaded from a file, it is the
 *  plugin's configuration (src/plugin.h), or nullptr if the plugin has no
 *  configure(). */
typedef struct json_object *tool_definition_fn (void const *service);

/** @p args: nullptr or an object, valid until the call ends or parks. */
typedef void tool_start_fn (struct mcp_slot    *slot,
                            struct json_object *args);

typedef void tool_step_fn (struct mcp_slot *slot);

/** Frees what the call's own word (mcp_slot_own()) points to, and stops
 *  what would end the call later, such as a timer.  askai calls it only if
 *  the own word is not nullptr.  It can run inside the function that ends
 *  the call, before that function returns (src/mcp.h).  The call has ended
 *  before drop() runs, so drop() cannot end it. */
typedef void tool_drop_fn (struct mcp_slot *slot);

struct tool_kind {
	tool_definition_fn *definition;
	tool_start_fn      *start;
	tool_step_fn       *step;   /**< needed if a call of the kind calls
	                                 mcp_slot_ready() */
	tool_drop_fn       *drop;   /**< needed if a call of the kind sets its
	                                 own word */
	uint64_t            flags;  /**< enum tool_flag */
};

#endif /* TWINCEPTION_SRC_TOOL_H_ */
