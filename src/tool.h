/** @file tool.h
 *
 * The tools that askai offers, as masks of their ids, which roles
 * grant (src/policy.h) and requests carry (src/stream.h); and the kinds of
 * tool they are, what their calls do, with the call API of src/mcp.h.
 * start() ends or parks a call; step() does a share of its work; drop()
 * lets go of its own word. All run on the call's loop, and never block.
 */
#ifndef ASKAI_SRC_TOOL_H_
#define ASKAI_SRC_TOOL_H_

#include <stddef.h>
#include <stdint.h>

#include "flag.h"

struct json_object;
struct mcp_slot;

/** The tools askai may offer at most: a mask's width; as an id, none. */
enum { TOOL_MAX = 128 };

/** Tools, bit i that of id i. */
typedef unsigned _BitInt(TOOL_MAX) tool_mask;

enum {
	TOOL_TELL_MAX   =  4U << 10U,  /**< bytes mcp_slot_tell() takes */
	TOOL_TEXT_MAX   =  4U << 20U,  /**< bytes of one message at most, such
	                                    as a note, a summary, a manual
	                                    page, the output of git, the body
	                                    of a request or the file of a
	                                    finding */
	TOOL_OUTPUT_MAX = 16U << 20U,  /**< bytes a call may write */
};

enum tool_flag : uint64_t {
	TOOL_LENT   = 1U << 0U,  /**< turns may call it, as the role grants */
	TOOL_PAGED  = 1U << 1U,  /**< output read on from "offset", which
	                              start() takes with mcp_slot_offset() */
	TOOL_DETACH = 1U << 2U,  /**< a turn may run it in the background */
	TOOL_STREAM = 1U << 3U,  /**< parking opens the event stream */

	/** The flags that a kind of a loaded plugin can set. plugin.c refuses
	 *  a loaded plugin with a kind that sets another flag. Only the kinds
	 *  built into askai can set the flags after TOOL_FLAGS.
	 */
	TOOL_FLAGS  = TOOL_LENT | TOOL_PAGED | TOOL_DETACH | TOOL_STREAM,

	/** The calls of the tool run alone. A turn's call of such a tool
	 *  starts after the results of all earlier calls are written. Later
	 *  calls start after its result is written.
	 */
	TOOL_SERIAL = UINT64_C(1) << 32,

	/** The tool is one of askai's tools background, jobs, wait and timer.
	 *  A client's background call cannot start such a tool. A client's
	 *  call of such a tool never becomes a task. askai does not resume an
	 *  interrupted turn that called one.
	 */
	TOOL_AGENDA = UINT64_C(1) << 33,

	/** The journal does not record the calls of the tool, so a resumed
	 *  turn runs them again. The kind of the findings tool sets it.
	 */
	TOOL_RERUN  = UINT64_C(1) << 34,
};

/** The description of the argument offset of each tool whose kind has
 *  TOOL_PAGED.
 */
#define TOOL_OFFSET_ABOUT "The byte of the output to start from. The default is 0."

/** The flags of the functions of src/mcp.h that write the output of a
 *  call or end the call. TOOL_FAILED is the shared bit of a failure
 *  (src/flag.h), and TOOL_MORE is a bit of this word only.
 */
enum tool_out : uint32_t {
	TOOL_FAILED = FLAG_FAILED,  /**< the call failed. mcp_slot_tell(),
	                                 mcp_slot_tellf() and mcp_slot_finish()
	                                 read the flag. */
	TOOL_MORE   = FLAG_OWN(0),  /**< the call writes more output after the
	                                 text. mcp_slot_write() reads the
	                                 flag. */
};

/* The two values are part of the plugin ABI. A plugin that was built
 * before a change of flag.h still passes 1 and 2.
 */
static_assert(TOOL_FAILED == 1 && TOOL_MORE == 2, "the values are part of the plugin ABI");

/** The scratch text of a client loop. It is storage for one text of at
 *  most TOOL_TELL_MAX - 1 bytes and a NUL. There is one scratch for each
 *  client loop. The rules of its use are in the comment of
 *  mcp_slot_scratch() (src/mcp.h).
 */
struct tool_scratch {
	char text[TOOL_TELL_MAX];
};

/** Returns the definition of the tool as tools/list shows it. The result
 *  is a new reference, or nullptr if out of memory. askai calls the
 *  function when it starts. For a plugin that askai loads from a file, it
 *  also calls the function once as it loads the plugin, to check that the
 *  definition's member "name" is the tool's name in the manifest.
 *  @p service is the service of a built-in plugin, which is the object that
 *  every loop shares. For a plugin that askai loaded from a file, it is the
 *  plugin's configuration (src/plugin.h), or nullptr if the plugin has no
 *  configure().
 */
typedef struct json_object *tool_definition_fn (void const *service);

/** @p args: nullptr or an object, valid until the call ends or parks. A
 *  reference to it or to a container in it does not keep the members of
 *  the container after a park (jb_reaper_add()), so a call copies them.
 */
typedef void tool_start_fn (struct mcp_slot    *slot,
                            struct json_object *args);

typedef void tool_step_fn (struct mcp_slot *slot);

/** Frees what the call's own word (mcp_slot_own()) points to, and stops
 *  what would end the call later, such as a timer. askai calls it only if
 *  the own word is not nullptr. It can run inside the function that ends
 *  the call, before that function returns (src/mcp.h). The call has ended
 *  before drop() runs, so drop() cannot end it.
 */
typedef void tool_drop_fn (struct mcp_slot *slot);

/** The position in a text that a call writes in parts: the section, the
 *  item in the section, and the bytes of that item that are written. A
 *  zeroed struct is the start of the text.
 */
struct tool_cursor {
	size_t item;     /**< the next item of the section */
	size_t byte;     /**< the bytes of that item that are written */
	size_t section;  /**< the section of the text */
};

struct tool_kind {
	tool_definition_fn *definition;
	tool_start_fn      *start;
	tool_step_fn       *step;   /**< needed if a call of the kind calls
	                                 mcp_slot_ready() */
	tool_drop_fn       *drop;   /**< needed if a call of the kind sets its
	                                 own word */
	uint64_t            flags;  /**< enum tool_flag */
};

#endif /* ASKAI_SRC_TOOL_H_ */
