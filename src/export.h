/** @file
 *  What askai exports to the plugins it loads (src/plugin.h).
 *
 * Every object builds with -fvisibility=hidden (mk/common.mk), and askai
 * alone links with -rdynamic (src/askai.mk), so the only symbols of its own
 * that askai exports are those whose declarations say EXPORT.  A plugin
 * that calls any other function of askai's is refused at load: dlopen()
 * with RTLD_NOW fails, "undefined symbol".  Functions only: a plugin reads
 * no data of askai's.  A plugin, built the same way, exports askai_plugin
 * alone, which PLUGIN_MANIFEST() declares so.
 */
#ifndef TWINCEPTION_SRC_EXPORT_H_
#define TWINCEPTION_SRC_EXPORT_H_

/** Default visibility on a declaration: its definition, which sees it,
 *  takes it on, and a plugin's call of it stays an import however the
 *  plugin hides its own symbols. */
#define EXPORT [[gnu::visibility("default")]]

#endif /* TWINCEPTION_SRC_EXPORT_H_ */
