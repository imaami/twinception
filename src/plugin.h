/** @file Plugins: parts of askai that bring kinds of tool (src/tool.h),
 *  whose calls go through the call API (src/mcp.h).  A built-in plugin has
 *  an instance, read-only or guarding itself, and a part of it on each
 *  client loop, that loop's alone.  askai's own man and git are plugins
 *  built in.
 *
 * Others are shared objects that askai's make rules build (src/NAME.so.mk)
 * and askai loads as it starts, never to unload them, once it has read the
 * manifest that PLUGIN_MANIFEST() gives each.  The configuration file lists
 * them in its member "plugins" (README.md).  A plugin exports
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

/** The version of the plugin interface.  Increase it whenever the
 *  interface changes and no PLUGIN_ABI() value changes.  An example is a
 *  function whose behavior changes while its declaration stays the same.
 *  PLUGIN_MANIFEST() stores it in the manifest so that askai does not load
 *  a plugin that was built with a different value. */
static constexpr uint32_t PLUGIN_EPOCH = 1;

/** On @p loop's thread, before its first call: the part of @p inst;
 *  nullptr, the error in @p *e.  A plugin that askai loaded from a file has
 *  no instance, so its @p inst is nullptr. */
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

/** Sizes, alignments and offsets that must be the same in askai and in a
 *  plugin.  They can differ if askai and the plugin are built with
 *  different compilers or with different compiler options.
 *  PLUGIN_MANIFEST() stores the plugin's values in the manifest so that
 *  askai can compare them with its own. */
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
 * Defines the plugin's manifest and declares askai_plugin, which the plugin
 * must then define.  The arguments are designated initializers for members
 * of struct manifest:
 *
 *     PLUGIN_MANIFEST(.name = "test", .tools = { "test_echo" });
 *
 * Do not pass .epoch or .abi.  The macro sets them.  Use the macro once
 * per plugin.  Each use defines plugin_manifest.  A second use in the same
 * source file is a compile error.  A second use in another source file of
 * the plugin is a link error.
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
		.desc   = { .epoch = PLUGIN_EPOCH,                   \
		            .abi = { PLUGIN_ABI(PLUGIN_ABI_U32) },   \
		            __VA_ARGS__ }                            \
	};                                                           \
	EXPORT extern struct plugin const askai_plugin

/* The rest of this file is askai's side.  A plugin does not use it. */

/** A plugin that askai loaded from a file. */
struct plugin_loaded {
	struct plugin const *plugin;    /**< the askai_plugin of the shared
	                                     object */
	struct manifest      manifest;  /**< read from the file before the
	                                     shared object was loaded */
	uint32_t             n_tools;   /**< the number of tools that the
	                                     manifest names */
	/** lens[i] is the length in bytes of manifest.tools[i]. */
	uint8_t              lens[MANIFEST_TOOLS];
};

/**
 * Loads the plugin in the file at @p path.
 *
 * It reads the manifest from the file first.  The epoch and the
 * PLUGIN_ABI() values in the manifest must equal askai's own.  The plugin's
 * name and each tool name must be 1 to 31 characters of [-0-9_a-z].  It
 * loads the shared object after these checks, with dlopen() and RTLD_NOW.
 * RTLD_NOW makes dlopen() fail if the plugin calls a function of askai that
 * askai does not export.  For each tool that the manifest names, the kind
 * at the same index in askai_plugin must have definition() and start().
 * The other kinds must have neither.  The shared object stays loaded until
 * askai exits.
 *
 * Returns the loaded plugin.  On failure it returns nullptr and stores the
 * error number in @p *e: ENOMEM, or EINVAL with the reason in the @p len
 * bytes at @p why.  The reason is worded to follow the path and a colon.
 */
extern struct plugin_loaded *
plugin_loaded_create (char const *path,
                      char       *why,
                      size_t      len,
                      int        *e);

/** Frees the loaded plugin *@p p_loaded and sets it to nullptr.  The shared
 *  object stays loaded. */
extern void
plugin_loaded_destroy (struct plugin_loaded **p_loaded);

#endif /* TWINCEPTION_SRC_PLUGIN_H_ */
