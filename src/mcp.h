/** @file MCP JSON-RPC semantics behind the Streamable HTTP transport. */
#ifndef TWINCEPTION_SRC_MCP_H_
#define TWINCEPTION_SRC_MCP_H_

#include <stddef.h>

struct ai_endpoint;
struct http;
struct mcp;
struct mcp_call;

/** Request headers that MCP checks against the message body. */
struct mcp_meta {
	char const *version;  /**< MCP-Protocol-Version */
	char const *method;   /**< Mcp-Method */
	char const *name;     /**< Mcp-Name */
	char const *session;  /**< Mcp-Session-Id */
};

/**
 * Receives one reply: an HTTP status, a JSON body (nullptr with 202) and an
 * optional session ID to announce.  The pointers are valid during the call.
 */
typedef void mcp_reply_cb (void       *user,
                           unsigned    status,
                           char const *body,
                           size_t      len,
                           char const *session);

struct mcp_tool {
	char const *name;
	char const *description;
};

extern int
mcp_init (struct mcp               **p_mcp,
          struct http               *http,
          struct ai_endpoint const  *ep,
          struct mcp_tool const     *tool,
          bool                       verbose);

extern void
mcp_fini (struct mcp **p_mcp);

/**
 * Handle one POSTed JSON-RPC message.  @p reply runs exactly once: before
 * this returns, or later while @p *p_call is set, once the upstream AI has
 * answered.  mcp_cancel() withdraws a pending reply.
 */
extern void
mcp_post (struct mcp             *mcp,
          struct mcp_meta const  *meta,
          char const             *body,
          size_t                  len,
          mcp_reply_cb           *reply,
          void                   *user,
          struct mcp_call       **p_call);

extern void
mcp_cancel (struct mcp       *mcp,
            struct mcp_call **p_call);

#endif /* TWINCEPTION_SRC_MCP_H_ */
