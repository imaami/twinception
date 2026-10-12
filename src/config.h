/** @file askai's configuration: its command line, and the configuration
 *  file that --config names, whose members are named as the long options
 *  are, the command line overriding the file (design section 9, S7). */
#ifndef TWINCEPTION_SRC_CONFIG_H_
#define TWINCEPTION_SRC_CONFIG_H_

#include <stddef.h>
#include <stdint.h>

#include "ai.h"
#include "brainstem.h"
#include "httpd.h"
#include "mcp.h"

struct policy;
struct ssl_ctx_st;

/** Options that are on or off, as bits of struct config's flags. */
enum config_flag {
	CFG_MAN       = 1u << 0,  /**< offer the man tool */
	CFG_TEST_FIFO = 1u << 1,  /**< test aid: tool calls wait for the upstream
	                               in one queue, first in first out */
	CFG_VERBOSE   = 1u << 2,  /**< log every tool call */
	CFG_TEST_TASK = 1u << 3,  /**< test aid: offer test_task, CPU work done as
	                               a task (src/task.h) */
	CFG_TLS_RSA   = 1u << 4,  /**< a TLS certificate's key may be RSA */
};

/**
 * askai's configuration, which config() returns by value: nothing in it
 * points into it.  Strings point into argv, the file's object or memory of
 * its own, which config_fini() frees.
 */
struct config {
	struct ai_endpoint    ep;
	struct mcp_tool       tool;
	struct mcp_limits     mcp;
	struct httpd_cfg      http;
	struct brainstem_cfg  brainstem;    /**< all but its apps */
	uint64_t              git_timeout;  /**< ns from a git call's start to its
	                                         deadline */
	struct json_object   *file;         /**< the configuration file's object,
	                                         which strings point into */
	struct policy        *policy;       /**< http.policy */
	struct ssl_ctx_st    *tls;          /**< http.tls: made from tls_cert and
	                                         tls_key if a listener is TLS's */
	char                **argv;         /**< the command line, as reloads
	                                         read it again */
	char const          **headers;      /**< ep.headers, at the start of the
	                                         block origins, listens and gits
	                                         are in */
	char const          **origins;      /**< http.origins */
	char const          **listens;      /**< http.listen */
	char const          **gits;         /**< the git tool's repositories */
	char                 *url;          /**< ep.url, when askai works it out */
	char                 *auth;         /**< the API key's header line */
	char                 *description;  /**< tool.description, by default */
	char const           *path;         /**< the configuration file's, or
	                                         nullptr */
	char const           *key_env;
	char const           *tls_cert;     /**< the TLS certificate chain's file */
	char const           *tls_key;      /**< its key's */
	size_t                n_headers;
	size_t                n_origins;
	size_t                n_listens;
	size_t                n_gits;
	uint32_t              flags;        /**< enum config_flag */
	uint32_t              calls;        /**< tool calls in flight, at most */
	uint32_t              man_recheck;  /**< seconds that man's finding of a
	                                         page's file holds (src/manual.h) */
	uint32_t              loops;        /**< client loops asked for; 0: one
	                                         fewer than the CPUs */
	int                   argc;
	uint32_t              logged;       /**< bool: what is wrong goes to the
	                                         log, as on a reload */
	uint32_t              restart;      /**< bool, once reloaded: the file's
	                                         options other than its policy are
	                                         not those askai runs with, which
	                                         take a restart */
	uint32_t              listen_file;  /**< bool: the listeners so far are
	                                         the file's */
};

/**
 * askai's configuration from its command line, @p argc and @p argv, and the
 * configuration file it names, saying on stderr what is wrong, with
 * @p *status -1 to serve, else the exit status: 0 after --help, 1 if out of
 * memory, 2 for an invalid option or environment.  Whatever the status,
 * config_fini() frees what it holds.
 */
extern struct config
config (int   argc,
        char *argv[],
        int  *status);

/**
 * Read the command line of @p from, the configuration file it names and
 * the TLS certificate and key, again, into @p cfg, as config() does, but on
 * any thread and saying what is wrong in the log: for a reload, of which
 * only the policy (src/policy.h) and the TLS context count, the restart flag
 * saying whether the rest differs from @p from's.  Returns the status
 * config() gives.
 */
extern int
config_reload (struct config       *cfg,
               struct config const *from);

extern void
config_fini (struct config *cfg);

#endif /* TWINCEPTION_SRC_CONFIG_H_ */
