/** @file registry.c
 *
 * The tool registry.
 */
#include <errno.h>
#include <string.h>

#include "plugin.h"
#include "registry.h"
#include "util.h"

void
registry_fini (struct registry *reg)
{
	for (uint32_t i = 0; i < reg->n_plugins; ++i)
		plugin_loaded_destroy(&reg->plugins[i].loaded);
	*reg = (struct registry){};
}

uint32_t
registry_find (struct registry const *reg,
               struct cstr_view       name,
               uint32_t               n)
{
	uint32_t const end = n < reg->n_tools ? n : reg->n_tools;
	uint32_t id = 0;
	while (id < end && !cstr_view_equal(reg->tools[id].name, name))
		++id;
	return id < end ? id : n;
}

/* Returns the index of plugin in reg->plugins. A plugin that is not there
 * yet is appended. The caller has checked that n_tools is less than
 * TOOL_MAX. Each plugin in reg->plugins is the plugin of at least one
 * tool, or registry_load() is adding its tools, so n_plugins is less than
 * TOOL_MAX too.
 */
static uint32_t
registry_part (struct registry     *reg,
               struct plugin const *plugin)
{
	uint32_t i = 0;
	while (i < reg->n_plugins && reg->plugins[i].plugin != plugin)
		++i;
	if (i == reg->n_plugins)
		reg->plugins[reg->n_plugins++] = (struct registry_plugin){ .plugin = plugin };
	return i;
}

int
registry_add (struct registry            *reg,
              struct plugin const        *plugin,
              struct registry_tool const *tool,
              uint32_t                    flags)
{
	if (reg->n_tools == TOOL_MAX)
		return ENOSPC;
	if (registry_find(reg, tool->name, TOOL_MAX) < TOOL_MAX)
		return EEXIST;
	/* The tools that no role can grant have the ids from n_granted on. A
	 * tool that a role can grant is inserted before them. */
	uint32_t id = reg->n_tools;
	if (flags & REGISTRY_GRANTED) {
		id = reg->n_granted++;
		memmove(&reg->tools[id + 1], &reg->tools[id],
		        (reg->n_tools - id) * sizeof *reg->tools);
	}
	reg->tools[id] = *tool;
	reg->tools[id].part = registry_part(reg, plugin);
	++reg->n_tools;
	return 0;
}

int
registry_load (struct registry    *reg,
               char const         *path,
               struct json_object *options,
               char               *why,
               size_t              len)
{
	if (reg->n_plugins == array_size(reg->plugins))
		return util_refuse(why, len, "askai cannot load more than %zu plugins",
		                   array_size(reg->plugins));
	int e = 0;
	struct plugin_loaded *loaded = plugin_loaded_create(path, options,
	                                                    why, len, &e);
	if (!loaded)
		return e;
	/* The loop adds tools whose names are pointers into loaded->manifest.
	 * Therefore the plugin is appended to reg->plugins first, and
	 * registry_fini() frees it even if registry_add() fails. */
	reg->plugins[reg->n_plugins++] = (struct registry_plugin){ loaded->plugin, loaded };
	for (uint32_t i = 0; i < loaded->n_tools; ++i) {
		char const *name = loaded->manifest.tools[i];
		struct registry_tool const tool = {
			.name = { name, loaded->lens[i] },
			.kind = &loaded->plugin->tools[i],
		};
		e = registry_add(reg, loaded->plugin, &tool, REGISTRY_GRANTED);
		if (!e)
			continue;
		if (e == EEXIST)
			return util_refuse(why, len, "its tool %s has the name of a tool "
			                   "that askai already has", name);
		return util_refuse(why, len, "its tool %s does not fit: askai offers "
		                   "at most %u tools", name, TOOL_MAX);
	}
	return 0;
}
