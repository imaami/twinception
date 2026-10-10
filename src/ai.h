/** @file Upstream AI endpoint protocols: request bodies and answer parsing. */
#ifndef TWINCEPTION_SRC_AI_H_
#define TWINCEPTION_SRC_AI_H_

#include <stddef.h>

struct dstr;
struct json_object;

enum ai_protocol {
	AI_OPENAI,     /**< OpenAI-compatible chat completions */
	AI_RESPONSES,  /**< OpenAI Responses API */
	AI_ANTHROPIC,  /**< Anthropic Messages API */
	AI_GEMINI      /**< Google Gemini generateContent */
};

struct ai_endpoint {
	struct json_object *extra;        /**< deep-merged into each request body */
	struct json_object *temperature;  /**< JSON number, or nullptr */
	char const         *url;
	char const         *model;
	char const         *system;       /**< default system prompt, or nullptr */
	char const *const  *headers;      /**< nullptr-terminated header lines */
	long                max_tokens;   /**< <= 0 leaves the provider default */
	long                timeout_ms;
	enum ai_protocol    protocol;
};

/**
 * Build the request body asking @p prompt, with @p system instructions unless
 * that is nullptr; nullptr if out of memory.  Both are JSON strings, which the
 * body takes references to; what it takes from @p ep, it copies.
 */
extern struct json_object *
ai_request (struct ai_endpoint const *ep,
            struct json_object       *system,
            struct json_object       *prompt);

/**
 * Turn an upstream HTTP response, the @p len bytes at @p body, into tool
 * output in @p text, which must start empty.  @p root is the body parsed as
 * UTF-8 JSON, or nullptr if it is not that; it stays the caller's.
 * @p *failed is set when @p text explains an error instead of carrying an
 * answer.
 */
extern int
ai_answer (enum ai_protocol    protocol,
           long                status,
           struct json_object *root,
           char const         *body,
           size_t              len,
           struct dstr        *text,
           bool               *failed);

#endif /* TWINCEPTION_SRC_AI_H_ */
