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
	long                context;      /**< tokens its context holds; <= 0: as
	                                       llama-server's /props says, else
	                                       unknown */
	uint32_t            slots;        /**< upstream calls at once; 0: as
	                                       llama-server's /props says, else 4 */
	enum ai_protocol    protocol;
};

/**
 * Build the request body asking @p prompt, with @p system instructions unless
 * that is nullptr, and offering the model @p tools, an array as the protocol
 * has them, unless that is nullptr; nullptr if out of memory.  Its messages
 * are its last member, so that the turns of a conversation (ai_format())
 * may follow them.  All three are JSON values, which the body takes
 * references to; what it takes from @p ep, it copies.
 */
extern struct json_object *
ai_request (struct ai_endpoint const *ep,
            struct json_object       *system,
            struct json_object       *prompt,
            struct json_object       *tools);

/** What an event of a streamed answer says besides its text. */
struct ai_event {
	char const *error;      /**< the upstream's error message, or nullptr */
	char const *reason;     /**< the answer's stop reason, or nullptr */
	uint32_t    used;       /**< the tokens of the request and the answer, if
	                             it says; else 0 */
	uint16_t    thought;    /**< bool: the event carries the model's thinking */
	uint8_t     truncated;  /**< bool: the reason is the output token limit */
	uint8_t     end;        /**< bool: the answer says it has ended */
};

/** A tool call's index that an event does not give: the call it says
 *  something of is a new one if it gives a new id, else the last. */
static constexpr uint32_t AI_CALL_NEXT = UINT32_MAX;

/** What an event says of a tool call that the model makes: of call @p index
 *  of its answer, or AI_CALL_NEXT, the parts it gives, each nullptr if it
 *  gives none. */
struct ai_call {
	char const *id;
	char const *name;
	char const *args;      /**< a piece of its arguments' JSON text */
	size_t      id_len;
	size_t      name_len;
	size_t      args_len;
	uint32_t    index;
};

/** Where the pieces of a streamed answer go: its text, and, unless nullptr,
 *  its thinking's text and its tool calls.  Each returns 0, or an error,
 *  which ends the reading of the event. */
struct ai_sink {
	int  (*text)    (void       *user,
	                 char const *text,
	                 size_t      len);
	int  (*thought) (void       *user,
	                 char const *text,
	                 size_t      len);
	int  (*call)    (void                 *user,
	                 struct ai_call const *call);
	void  *user;
};

/** What ends an answer the output token limit cut short. */
#define AI_CUT_NOTE "\n\n[The answer was cut short by the output token limit.]"

/**
 * Read an event of a streamed answer, its data parsed as @p ev: hand each
 * piece of it to @p sink, and say what else it says in @p out, zeroed to
 * begin, which then points into @p ev.  Returns the first nonzero that the
 * sink returned, else 0.  An answer whose stream ends before an event says
 * it has ended, or chat completions' [DONE], is cut off.
 */
extern int
ai_event (enum ai_protocol       protocol,
          struct json_object    *ev,
          struct ai_sink const  *sink,
          struct ai_event       *out);

/**
 * The JSON text that the turns of a conversation are made of, in a protocol
 * whose answers may call tools: each member a fragment that goes between
 * the texts of strings, which are escaped into it.  A model's turn is turn,
 * its text, thought and its thinking's text if it has any, and calls; then
 * each tool call: call (after the first, call_next), its id, name, its
 * name, args, its arguments' JSON text, and call_end; then turn_end.  The
 * result of a call is result, the call's id, output, the result's text, and
 * result_end.  prompt_end ends the prompt's text: what askai adds to the
 * prompt goes before it.
 */
struct ai_format {
	struct ai_bytes {
		char const *ptr;
		size_t      len;
	}               turn,
	                thought,     /**< only with some thinking */
	                calls,
	                call,
	                call_next,
	                name,
	                args,
	                call_end,
	                turn_end,
	                result,
	                output,
	                result_end,
	                prompt_end,
	                end;         /**< ends the messages, and the body */
};

/** Whether @p reason, an answer's stop reason, is @p protocol's output
 *  token limit, which cuts it short. */
extern bool
ai_cut (enum ai_protocol  protocol,
        char const       *reason);

/** The format of turns in @p protocol; nullptr if askai has none for it, as
 *  for every protocol but chat completions so far. */
extern struct ai_format const *
ai_format (enum ai_protocol protocol);

/** Say in the @p size bytes at @p buf that the model gave no answer, and
 *  why, if its stop @p reason is not nullptr; returns the length written. */
extern size_t
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
