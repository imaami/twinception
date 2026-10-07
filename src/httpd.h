/** @file Minimal HTTP/1.1 server carrying MCP's Streamable HTTP transport. */
#ifndef TWINCEPTION_SRC_HTTPD_H_
#define TWINCEPTION_SRC_HTTPD_H_

struct httpd;
struct loop;
struct mcp;

struct httpd_cfg {
	char const *const *origins;  /**< allowed non-loopback Origins; "*": any */
	char const        *listen;   /**< [host:]port */
	char const        *token;    /**< bearer token clients must send, or nullptr */
};

extern int
httpd_init (struct httpd           **p_httpd,
            struct loop             *loop,
            struct mcp              *mcp,
            struct httpd_cfg const  *cfg);

extern void
httpd_fini (struct httpd **p_httpd);

#endif /* TWINCEPTION_SRC_HTTPD_H_ */
