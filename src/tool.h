/** @file The tools that askai offers, as masks of their ids, which roles
 *  grant (src/policy.h) and requests carry (src/stream.h); and the kinds of
 *  tool they are, what their calls do.  start() ends or parks a call;
 *  step() does a share of its work; drop() lets go of its own word.  All
 *  run on the call's loop, and never block. */
#ifndef TWINCEPTION_SRC_TOOL_H_
#define TWINCEPTION_SRC_TOOL_H_

#include <stdint.h>

struct json_object;
struct mcp_slot;

/** The tools askai may offer at most: a mask's width; as an id, none. */
static constexpr unsigned TOOL_MAX = 128;

/** Tools, bit i that of id i. */
typedef unsigned _BitInt(TOOL_MAX) tool_mask;

enum tool_flag : uint64_t {
	TOOL_LENT   = 1u << 0,  /**< turns may call it, as the role grants */
	TOOL_PAGED  = 1u << 1,  /**< output read on from "offset" */
	TOOL_DETACH = 1u << 2,  /**< a turn may run it in the background */
	TOOL_STREAM = 1u << 3,  /**< parking opens the event stream */
};

/** A plugin's kinds may have these; askai's have more. */
static constexpr uint64_t TOOL_FLAGS = TOOL_LENT | TOOL_PAGED | TOOL_DETACH |
                                       TOOL_STREAM;

/** The tool's definition, as tools/list lists it, of @p inst, the kind's
 *  instance: a new reference, nullptr if out of memory.  Once, at start. */
typedef struct json_object *tool_definition_fn (void const *inst);

/** @p args: a client's anything, nullptr too; a turn's an object; valid
 *  until the call ends or parks. */
typedef void tool_start_fn (struct mcp_slot    *slot,
                            struct json_object *args);

typedef void tool_step_fn (struct mcp_slot *slot);

/** Let go of the call's own word, which is not nullptr; stop what would
 *  end the call. */
typedef void tool_drop_fn (struct mcp_slot *slot);

struct tool_kind {
	tool_definition_fn *definition;
	tool_start_fn      *start;
	tool_step_fn       *step;   /**< if it readies the slot's step */
	tool_drop_fn       *drop;   /**< if its calls own a word */
	uint64_t            flags;  /**< enum tool_flag */
};

#endif /* TWINCEPTION_SRC_TOOL_H_ */
