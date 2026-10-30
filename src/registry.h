/** @file registry.h
 *
 * The tool registry: the tools that askai offers, and the plugins
 * that have them.
 *
 * A tool's id is its index in the registry.  The tools that a role can
 * grant (src/policy.h) come first, so their ids are also the bits of a
 * role's tool mask.  The tools that only the turns of an ask call use
 * (src/mcp.h) follow them.  No two tools have the same name.
 *
 * A zeroed registry is empty.  Tools are added while askai reads its
 * configuration, on one thread.  After that the registry does not change,
 * so any thread may read it.  registry_fini() frees what the registry
 * allocated.
 */
#ifndef TWINCEPTION_SRC_REGISTRY_H_
#define TWINCEPTION_SRC_REGISTRY_H_

#include <stddef.h>
#include <stdint.h>

#include "tool.h"

struct json_object;
struct plugin;
struct plugin_loaded;

/** A tool in the registry. */
struct registry_tool {
	char const             *name;  /**< NUL-terminated.  The caller of
	                                    registry_add() provides storage that
	                                    outlives the registry. */
	struct tool_kind const *kind;  /**< what a call of the tool does */
	void const             *data;  /**< the pointer that the caller of
	                                    registry_add() gave for its own use,
	                                    or nullptr */
	uint32_t                len;   /**< the length of name in bytes */
	uint32_t                part;  /**< the index of the tool's plugin in
	                                    plugins; registry_add() sets it */
};

/** How registry_add() adds a tool. */
enum registry_flag : uint32_t {
	REGISTRY_GRANTED = 1u << 0,  /**< a role can grant the tool */
};

struct registry {
	struct registry_tool  tools[TOOL_MAX];    /**< by id */
	struct plugin const  *plugins[TOOL_MAX];  /**< each plugin that has a
	                                               tool, in the order in
	                                               which the plugins' first
	                                               tools were added */
	struct plugin_loaded *loaded[TOOL_MAX];   /**< the plugins that
	                                               registry_load() loaded;
	                                               the registry frees them */
	uint32_t              n_tools;
	uint32_t              n_granted;  /**< the tools with the ids 0 to
	                                       n_granted - 1 are those that a
	                                       role can grant */
	uint32_t              n_plugins;
	uint32_t              n_loaded;
};

/** Frees the loaded plugins' records and leaves @p reg empty.  The shared
 *  objects stay loaded (src/plugin.h).
 */
extern void
registry_fini (struct registry *reg);

/**
 * Adds the tool @p tool, which the plugin @p plugin has.
 *
 * With REGISTRY_GRANTED in @p flags the tool gets the id that follows the
 * ids of the tools that a role can grant.  The id of each tool that no role
 * can grant then increases by one.  Without the flag the tool gets the id
 * that follows the ids of all the tools in the registry.  The id of a tool
 * that no role can grant is therefore final only when every tool that a
 * role can grant has been added.
 *
 * Returns 0; EEXIST if the registry has a tool of the same name; or ENOSPC
 * if the registry has TOOL_MAX tools.
 */
extern int
registry_add (struct registry            *reg,
              struct plugin const        *plugin,
              struct registry_tool const *tool,
              uint32_t                    flags);

/**
 * Loads the plugin in the file at @p path with plugin_loaded_create()
 * (src/plugin.h), which passes @p options to the plugin's configure(), and
 * adds the tools that its manifest names, in the order of the manifest, as
 * tools that a role can grant.
 *
 * Returns 0; ENOMEM; or EINVAL with the reason in the @p len bytes at
 * @p why.  The reason is worded to follow the path and a colon.
 */
extern int
registry_load (struct registry    *reg,
               char const         *path,
               struct json_object *options,
               char               *why,
               size_t              len);

#endif /* TWINCEPTION_SRC_REGISTRY_H_ */
