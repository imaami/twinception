/** @file Upstream AI endpoint protocols: request bodies and answer parsing. */
#ifndef TWINCEPTION_SRC_AI_H_
#define TWINCEPTION_SRC_AI_H_

#include <stddef.h>
#include <stdint.h>

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
	long                timeout_ms;   /**< a call's in the queue, and then
	                                       upstream; <= 0: none */
	long                chunk_ms;     /**< the longest pause in an answer once
	                                       it has begun; <= 0: none */
	uint32_t            slots;        /**< upstream calls at once; 0: as
	                                       llama-server's /props says, else 4 */
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

/** What an event of a streamed answer says besides its text. */
struct ai_event {
	char const *error;      /**< the upstream's error message, or nullptr */
	char const *reason;     /**< the answer's stop reason, or nullptr */
	uint32_t    truncated;  /**< bool: the reason is the output token limit */
	uint32_t    end;        /**< bool, widened over the padding: the answer
	                             says it has ended */
};

/** What ends an answer the output token limit cut short. */
#define AI_CUT_NOTE "\n\n[The answer was cut short by the output token limit.]"

/**
 * Read an event of a streamed answer, its data parsed as @p ev: hand each
 * piece of the answer's text it carries to @p put, and say what else it
 * says in @p out, zeroed to begin, which then points into @p ev.  Returns
 * the first nonzero that @p put returned, else 0.  An answer whose stream
 * ends before an event says it has ended, or chat completions' [DONE], is
 * cut off.
 */
extern int
ai_event (enum ai_protocol    protocol,
          struct json_object *ev,
          int               (*put) (void       *user,
                                    char const *text,
                                    size_t      len),
          void               *user,
          struct ai_event    *out);

/** Say in the @p size bytes at @p buf that the model gave no answer, and
 *  why, if its stop @p reason is not nullptr; returns snprintf()'s count. */
extern int
ai_no_answer (char       *buf,
              size_t      size,
              char const *reason);

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
