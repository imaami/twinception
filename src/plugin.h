/** @file Plugins: parts of askai that bring kinds of tool (src/tool.h),
 *  whose calls go through the call API (src/mcp.h).  A plugin has an
 *  instance, read-only or guarding itself, and a part of it on each client
 *  loop, that loop's alone.  askai's own man and git are plugins built
 *  in. */
#ifndef TWINCEPTION_SRC_PLUGIN_H_
#define TWINCEPTION_SRC_PLUGIN_H_

#include <stddef.h>

#include "buf.h"
#include "loop.h"
#include "mcp.h"
#include "tool.h"

/** On @p loop's thread, before its first call: the part of @p inst;
 *  nullptr, the error in @p *e. */
typedef void *plugin_part_create_fn (void        *inst,
                                     struct loop *loop,
                                     int         *e);

/** Its calls all dropped; sets *p_part to nullptr. */
typedef void plugin_part_destroy_fn (void **p_part);

/** Give back kept memory, up to @p max LOOP_RELEASE units; returns those
 *  done, 0 once nothing is kept. */
typedef size_t plugin_relieve_fn (void              *part,
                                  enum buf_pressure  level,
                                  size_t             max);

/** A plugin's kinds at most. */
static constexpr unsigned PLUGIN_TOOLS = 8;

struct plugin {
	/** Its kinds: the leading entries, those whose start is set; the
	 *  rest are all zero. */
	struct tool_kind        tools[PLUGIN_TOOLS];
	plugin_part_create_fn  *part_create;   /**< nullptr: part = instance,
	                                            for kinds whose calls end in
	                                            start(): no shares, timers
	                                            or posts */
	plugin_part_destroy_fn *part_destroy;  /**< exactly with part_create */
	plugin_relieve_fn      *relieve;       /**< needs part_create */
};

#endif /* TWINCEPTION_SRC_PLUGIN_H_ */
