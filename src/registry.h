/** @file registry.h
 *
 * The tool registry contains the tools that askai offers and the plugins
 * that provide them.
 *
 * A tool's id is its index in the registry. The tools that a role can
 * grant (src/policy.h) come first, so their ids are also the bits of a
 * role's tool mask. The tools that only the turns of an ask call use
 * (src/mcp.h) come after them. The names of the tools are unique.
 *
 * A zeroed registry is empty. Tools are added while askai reads its
 * configuration, on one thread. After that the registry does not change,
 * so any thread may read it. registry_fini() frees what the registry
 * allocated.
 */
#ifndef TWINCEPTION_SRC_REGISTRY_H_
#define TWINCEPTION_SRC_REGISTRY_H_

#include <stddef.h>
#include <stdint.h>

#include "cstr_view.h"
#include "tool.h"
#include "util.h"

struct json_object;
struct plugin;
struct plugin_loaded;

/** A tool in the registry. */
struct registry_tool {
	struct cstr_view        name;  /**< NUL-terminated after name.len
	                                    bytes. The caller of registry_add()
	                                    provides storage that outlives the
	                                    registry. */
	struct tool_kind const *kind;  /**< what a call of the tool does */
	void const             *data;  /**< the pointer that the caller of
	                                    registry_add() gave for its own use,
	                                    or nullptr */
	uint64_t                part;  /**< the index of the plugin of the
	                                    tool in plugins, which
	                                    registry_add() sets. It is widened
	                                    over the padding. */
};

/** How registry_add() adds a tool. */
enum registry_flag : uint32_t {
	REGISTRY_GRANTED = 1u << 0,  /**< a role can grant the tool */
};

/** A plugin in the registry. */
struct registry_plugin {
	struct plugin const  *plugin;
	struct plugin_loaded *loaded;  /**< the plugin as registry_load() loaded
	                                    it, which the registry frees, or
	                                    nullptr for a plugin built into
	                                    askai */
};

struct registry {
	struct registry_tool   tools[TOOL_MAX];    /**< by id */
	struct registry_plugin plugins[TOOL_MAX];  /**< in the order in which
	                                                they were loaded or
	                                                their first tools were
	                                                added */
	uint32_t               n_tools;
	uint32_t               n_granted;  /**< the tools with the ids 0 to
	                                        n_granted - 1 are those that a
	                                        role can grant */
	uint64_t               n_plugins;  /**< widened over the padding */
};

/** Frees the loaded plugins' records and leaves @p reg empty. The shared
 *  objects stay loaded (src/plugin.h).
 */
extern void
registry_fini (struct registry *reg);

/** Returns the id of the tool named @p name among the ids below @p n, or
 *  @p n if there is no such tool among them.
 */
extern uint32_t
registry_find (struct registry const *reg,
               struct cstr_view       name,
               uint32_t               n);

/** The names of tools of a registry, separated by ", ". */
struct registry_names {
	UTIL_TEXT(1024);
};

/** Returns the names of the tools of @p reg whose bits are set in @p mask,
 *  or "none" if there is no such tool, cut with "..." if they do not fit.
 */
extern struct registry_names
registry_names (struct registry const *reg,
                tool_mask              mask);

/**
 * Adds the tool @p tool of the plugin @p plugin.
 *
 * With REGISTRY_GRANTED in @p flags the tool receives the id after the ids
 * of the tools that a role can grant. The id of each tool that no role can
 * grant then increases by one. Without the flag the tool receives the id
 * after the ids of all the tools in the registry. The id of a tool that no
 * role can grant is therefore final only when every tool that a role can
 * grant has been added.
 *
 * Returns 0, EEXIST if the registry contains a tool of the same name, or
 * ENOSPC if the registry contains TOOL_MAX tools.
 */
extern int
registry_add (struct registry            *reg,
              struct plugin const        *plugin,
              struct registry_tool const *tool,
              uint32_t                    flags);

/**
 * Loads the plugin in the file at @p path with plugin_loaded_create()
 * (src/plugin.h), which passes @p options to the plugin's configure(), and
 * adds the tools listed in its manifest, in the order of the manifest, as
 * tools that a role can grant.
 *
 * Returns 0; ENOMEM; or EINVAL with the reason in the @p len bytes at
 * @p why. The reason is worded to follow the path and a colon.
 */
extern int
registry_load (struct registry    *reg,
               char const         *path,
               struct json_object *options,
               char               *why,
               size_t              len);

#endif /* TWINCEPTION_SRC_REGISTRY_H_ */
