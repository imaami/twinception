/** @file Plugins.  A plugin provides kinds of tool (src/tool.h).  The calls
 *  of its tools use the call API of src/mcp.h.  The man tool and the git
 *  tool are plugins that are built into askai.
 *
 * Every other plugin is a shared object that askai's make rules build
 * (src/NAME.so.mk).  The configuration file lists the plugins in its member
 * "plugins" (README.md).  askai loads them when it starts, after it has
 * read the manifest that PLUGIN_MANIFEST() puts into each of them.  It
 * never unloads a plugin.
 *
 * A plugin exports only askai_plugin.  It calls only json-c, libc and the
 * functions that askai exports (src/export.h).  It has no constructor, no
 * mutable static variable and no thread-local variable.  It starts no
 * thread and no process, installs no signal handler, changes no setting of
 * the process, loads no other object, and never blocks or exits.  A call
 * of its tools sends an HTTP request through askai with mcp_slot_fetch()
 * (src/web.h).  Each of its functions that askai calls checks the pointers
 * that it gets once, at its start, and the plugin checks what a getter of
 * the call API returns before it uses that.
 *
 * A plugin has state in two places only.  The first is the pointer that
 * mcp_slot_own() returns for a call.  The second is an instance.  An
 * instance is instance_size bytes that askai allocates and that the plugin
 * initializes with instance_init().  If instance_size is not 0, each call
 * of the plugin's tools uses one instance, and mcp_slot_instance() returns
 * it.
 *
 * Three things are always true of an instance:
 * - All calls that use it have the same user.  mcp_slot_principal()
 *   returns the principal of a call's user.
 * - askai passes it only to functions that run on the thread of one client
 *   loop, so it needs no lock.
 * - askai calls instance_fini() for it only when no call uses it and no
 *   function of the plugin is running.
 *
 * Two calls of one user do not always get the same instance.  A user whose
 * connections are on several loops has an instance on each of these loops,
 * and the instances share nothing.  askai destroys instances that no call
 * uses, for example under memory pressure and after a reload of the
 * configuration.  A user can have more than one instance on one loop at
 * the same time.  Each call of an anonymous client, whose principal is 0,
 * gets a new instance.  A plugin must therefore work when a call gets a
 * new instance, which has none of the data that an earlier call of the
 * same user stored.  src/instance.h has the exact rules.
 */
#ifndef TWINCEPTION_SRC_PLUGIN_H_
#define TWINCEPTION_SRC_PLUGIN_H_

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "export.h"
#include "loop_export.h"
#include "manifest.h"
#include "mcp.h"
#include "tool.h"
#include "web.h"

/** The version of the plugin interface.  Increase it whenever the
 *  interface changes and no PLUGIN_ABI() value changes.  An example is a
 *  function whose behavior changes while its declaration stays the same.
 *  PLUGIN_MANIFEST() stores it in the manifest so that askai does not load
 *  a plugin that was built with a different value. */
static constexpr uint32_t PLUGIN_EPOCH = 4;

/** The largest value of instance_size, in bytes. */
static constexpr size_t PLUGIN_INSTANCE_MAX = 64 << 10;

/**
 * Initializes an instance.  @p instance points to instance_size bytes that
 * askai allocated.  The bytes are aligned for any type, and their content
 * is not defined.  @p principal is the user of the instance.  It is 0 for
 * an anonymous client.  @p loop is the loop of the instance.
 *
 * askai calls the function on the thread of @p loop, when a call of the
 * user starts there and needs a new instance.  It calls it before start()
 * of the call's kind.  The function must not block.
 *
 * Returns 0, or an error number.  After an error, askai frees the bytes and
 * does not call instance_fini().  Before the function returns an error, it
 * must therefore free what it allocated and stop the timers that it set.
 * askai then ends the request of the call in the way that mcp_slot_fail()
 * does.
 */
typedef int plugin_instance_init_fn (void        *instance,
                                     uint64_t     principal,
                                     struct loop *loop);

/** Finalizes an instance.  It frees what the members of the instance point
 *  to, and it stops the instance's timers.  askai frees the bytes of the
 *  instance itself.  askai calls the function on the thread of the
 *  instance's loop, when no call uses the instance and no function of the
 *  plugin is running. */
typedef void plugin_instance_fini_fn (void *instance);

/** Frees memory that @p instance has allocated only for reuse, at most
 *  @p max LOOP_RELEASE units.  askai calls it under memory pressure of the
 *  level @p level.  Returns the number of units freed.  It returns 0 when
 *  the instance has no such memory left. */
typedef size_t plugin_relieve_fn (void              *instance,
                                  enum buf_pressure  level,
                                  size_t             max);

/** The largest number of kinds that a plugin has.  It equals the number of
 *  tool names that a manifest has room for. */
static constexpr unsigned PLUGIN_TOOLS = MANIFEST_TOOLS;

struct plugin {
	/** The plugin's kinds.  tools[i] is the kind of the tool that the
	 *  manifest names at index i.  An entry is a kind if its start is set.
	 *  The kinds are the first entries, and every entry after them is all
	 *  zero. */
	struct tool_kind         tools[PLUGIN_TOOLS];
	/** Initializes an instance.  It is set if instance_size is not 0, and
	 *  it is nullptr if instance_size is 0. */
	plugin_instance_init_fn *instance_init;
	/** Finalizes an instance.  It is nullptr if an instance has nothing to
	 *  free.  It can be set only if instance_init is set. */
	plugin_instance_fini_fn *instance_fini;
	/** Frees memory that an instance has allocated only for reuse.  It is
	 *  nullptr if an instance has no such memory.  It can be set only if
	 *  instance_init is set. */
	plugin_relieve_fn       *relieve;
	/** The size of an instance in bytes, from 1 to PLUGIN_INSTANCE_MAX.
	 *  It is 0 if the plugin has no instances. */
	size_t                   instance_size;
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
	X(sizeof (struct plugin)) X(offsetof (struct plugin, instance_init)) \
	X(sizeof (struct handle)) X(sizeof (struct msg))                   \
	X(offsetof (struct msg, fn)) X(sizeof (struct loop_timer))         \
	X(offsetof (struct loop_timer, fn)) X(sizeof (struct web_request)) \
	X(sizeof (struct web_response))
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
 * name and each tool name must be 1 to 31 characters of [-0-9_a-z].  The
 * file must have no thread-local storage, no constructor or destructor,
 * and no data that is still writable when the plugin's code runs, because
 * a plugin has its state in instances.  The link flags -nostartfiles and
 * -Wl,-z,relro,-z,now give such a file (src/test_plugin.so.mk).  askai
 * that is built with -fsanitize=address, -fsanitize=thread or
 * -fsanitize=undefined does not check this.  It loads the shared object
 * after these checks, with dlopen() and RTLD_NOW.  RTLD_NOW makes dlopen()
 * fail if the plugin calls a function of askai that askai does not export.
 * For each tool that the manifest names, the kind at the same index in
 * askai_plugin must have definition() and start().  The other kinds must
 * have neither.  A kind's flags must be among TOOL_FLAGS (src/tool.h), and
 * the definition of a named tool must have the tool's name.  The shared
 * object stays loaded until askai exits.
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
