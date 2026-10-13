/** @file The tool registry (registry.h). */
#include <errno.h>
#include <string.h>

#include "plugin.h"
#include "registry.h"
#include "util.h"

void
registry_fini (struct registry *reg)
{
	while (reg->n_loaded)
		plugin_loaded_destroy(&reg->loaded[--reg->n_loaded]);
	*reg = (struct registry){};
}

/* Returns the index of plugin in reg->plugins.  A plugin that is not there
 * yet is appended.  The caller has checked that the registry has room for
 * one more tool, and every plugin in it has at least one tool, so it has
 * room for one more plugin. */
static uint32_t
registry_part (struct registry     *reg,
               struct plugin const *plugin)
{
	uint32_t i = 0;
	while (i < reg->n_plugins && reg->plugins[i] != plugin)
		++i;
	if (i == reg->n_plugins)
		reg->plugins[reg->n_plugins++] = plugin;
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
	for (uint32_t id = 0; id < reg->n_tools; ++id) {
		struct registry_tool const *t = &reg->tools[id];
		if (t->len == tool->len && !memcmp(t->name, tool->name, tool->len))
			return EEXIST;
	}
	/* The tools that no role can grant have the ids from n_granted on.  A
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
registry_load (struct registry *reg,
               char const      *path,
               char            *why,
               size_t           len)
{
	if (reg->n_loaded == ARRAY_SIZE(reg->loaded)) {
		util_format(why, len, "askai cannot load more than %zu plugins",
		            ARRAY_SIZE(reg->loaded));
		return EINVAL;
	}
	int e;
	struct plugin_loaded *loaded = plugin_loaded_create(path, why, len, &e);
	if (!loaded)
		return e;
	/* The loop adds tools whose names are pointers into loaded->manifest.
	 * Therefore loaded is stored in reg->loaded first, and registry_fini()
	 * frees it even if registry_add() fails. */
	reg->loaded[reg->n_loaded++] = loaded;
	for (uint32_t i = 0; i < loaded->n_tools; ++i) {
		char const *name = loaded->manifest.tools[i];
		struct registry_tool const tool = {
			.name = name,
			.kind = &loaded->plugin->tools[i],
			.len  = loaded->lens[i],
		};
		e = registry_add(reg, loaded->plugin, &tool, REGISTRY_GRANTED);
		if (!e)
			continue;
		if (e == EEXIST)
			util_format(why, len, "its tool %s has the name of a tool that "
			            "askai already has", name);
		else
			util_format(why, len, "its tool %s does not fit: askai offers "
			            "at most %u tools", name, TOOL_MAX);
		return EINVAL;
	}
	return 0;
}
