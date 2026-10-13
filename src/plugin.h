/** @file Plugins: parts of askai that bring kinds of tool (src/tool.h),
 *  whose calls go through the call API (src/mcp.h).  A plugin has an
 *  instance, read-only or guarding itself, and a part of it on each client
 *  loop, that loop's alone.  askai's own man and git are plugins built in.
 *
 * Others are shared objects that askai's make rules build (src/NAME.so.mk)
 * and askai loads as it starts, never to unload them, once it has read the
 * manifest that PLUGIN_MANIFEST() gives each.  A plugin exports
 * askai_plugin alone, and calls only what askai exports (src/export.h),
 * json-c and libc.  It has no constructor and no mutable static, starts no
 * thread or process, takes no signal, changes no process-wide setting,
 * loads nothing, and never blocks or exits.
 */
#ifndef TWINCEPTION_SRC_PLUGIN_H_
#define TWINCEPTION_SRC_PLUGIN_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "export.h"
#include "loop.h"
#include "manifest.h"
#include "mcp.h"
#include "tool.h"

/** Bumped as a meaning changes that no declaration shows, which changes
 *  the build key with it. */
static constexpr uint32_t PLUGIN_EPOCH = 1;

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

/** A plugin's kinds at most, as many as its manifest may name tools. */
static constexpr unsigned PLUGIN_TOOLS = MANIFEST_TOOLS;

struct plugin {
	/** Its kinds, in the order of its manifest's tools: the leading
	 *  entries, those whose start is set; the rest are all zero. */
	struct tool_kind        tools[PLUGIN_TOOLS];
	plugin_part_create_fn  *part_create;   /**< nullptr: part = instance,
	                                            for kinds whose calls end in
	                                            start(): no shares, timers
	                                            or posts */
	plugin_part_destroy_fn *part_destroy;  /**< exactly with part_create */
	plugin_relieve_fn      *relieve;       /**< needs part_create */
};

/** The layouts that askai's compiler and a plugin's must agree on, which
 *  the build key cannot tell: the plugin's own flags may change them. */
#define PLUGIN_ABI(X)                                                      \
	X(sizeof (void *)) X(sizeof (long)) X(alignof (max_align_t))       \
	X(sizeof (enum buf_pressure)) X(sizeof (struct tool_text))         \
	X(sizeof (struct tool_kind)) X(offsetof (struct tool_kind, flags)) \
	X(sizeof (struct plugin)) X(offsetof (struct plugin, part_create)) \
	X(sizeof (struct handle)) X(sizeof (struct msg))                   \
	X(offsetof (struct msg, fn)) X(sizeof (struct loop_timer))         \
	X(offsetof (struct loop_timer, fn)) X(offsetof (struct loop, now)) \
	X(offsetof (struct loop, work))
#define PLUGIN_ABI_U32(x) (uint32_t)(x),
#define PLUGIN_ABI_N(x) + 1
static_assert(0 PLUGIN_ABI(PLUGIN_ABI_N) <= MANIFEST_ABI);
#undef PLUGIN_ABI_N

/**
 * The plugin's manifest, of designated initializers of struct manifest but
 * the key and the probes, then the declaration of askai_plugin, which the
 * plugin defines: PLUGIN_MANIFEST(.name = "test", .tools = { "test_echo" });
 * Once per plugin, which a second use makes a link error.  It needs the
 * build key, PLUGIN_KEY: the plugin includes plugin_key.h, which make
 * generates from this header (KEY_ of mk/common.mk).
 */
#define PLUGIN_MANIFEST(...)                                         \
	MANIFEST_NO_ASAN                                             \
	[[gnu::section(".note.askai"), gnu::used, gnu::aligned(4),   \
	  gnu::visibility("hidden")]]                                \
	struct manifest_note const plugin_manifest = {               \
		.namesz = sizeof "askai",                            \
		.descsz = sizeof (struct manifest),                  \
		.type   = MANIFEST_TYPE,                             \
		.owner  = "askai",                                   \
		.desc   = { .key = PLUGIN_KEY,                       \
		            .abi = { PLUGIN_ABI(PLUGIN_ABI_U32) },   \
		            __VA_ARGS__ }                            \
	};                                                           \
	EXPORT extern struct plugin const askai_plugin

#endif /* TWINCEPTION_SRC_PLUGIN_H_ */
