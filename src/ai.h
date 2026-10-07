/** @file Upstream AI endpoint protocols: request bodies and answer parsing. */
#ifndef TWINCEPTION_SRC_AI_H_
#define TWINCEPTION_SRC_AI_H_

#include <stddef.h>

#include "buf.h"

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

/** Build the request body asking @p prompt; nullptr if out of memory. */
extern struct json_object *
ai_request (struct ai_endpoint const *ep,
            char const               *system,
            char const               *prompt);

/**
 * Turn an upstream HTTP response into tool output.  @p *failed is set when
 * @p text explains an error instead of carrying an answer.
 */
extern int
ai_response (enum ai_protocol  protocol,
             long              status,
             char const       *body,
             size_t            len,
             struct buf       *text,
             bool             *failed);

#endif /* TWINCEPTION_SRC_AI_H_ */
