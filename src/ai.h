/** @file Upstream AI APIs: request bodies and answer parsing. */
#ifndef TWINCEPTION_SRC_AI_H_
#define TWINCEPTION_SRC_AI_H_

#include <stddef.h>
#include <stdint.h>

struct dstr;
struct json_object;

/** How the upstream's requests and answers are written: 0 only until the
 *  configuration has chosen. */
enum ai_api : uint16_t {
	AI_CHAT = 1,      /**< chat completions, OpenAI's and the many copies */
	AI_RESPONSES,     /**< OpenAI's Responses */
	AI_MESSAGES,      /**< Anthropic's Messages */
	AI_GENERATE,      /**< Gemini's generateContent */
	AI_INTERACTIONS,  /**< Gemini's Interactions */
};

/** Who serves the upstream, as askai knows them: their URLs, the variables
 *  of their keys and their default API. */
enum ai_provider : uint16_t {
	AI_NO_PROVIDER,  /**< a server that --url names alone */
	AI_LLAMA,        /**< llama-server, which says at /props what it has */
	AI_OPENAI,
	AI_ANTHROPIC,
	AI_GOOGLE,
	AI_DEEPINFRA,
	AI_OPENROUTER,
};

/** Whether @p p may be llama-server, which may need no key or model and
 *  says at /props what it has: llama, or a server that no provider names. */
static inline bool
ai_llama_like (enum ai_provider p)
{
	return p == AI_NO_PROVIDER || p == AI_LLAMA;
}

struct ai_endpoint {
	struct json_object *extra;        /**< deep-merged into each request body */
	struct json_object *temperature;  /**< JSON number, or nullptr */
	char const         *url;
	char const         *model;
	char const         *system;       /**< default system prompt, or nullptr */
	char const *const  *headers;      /**< nullptr-terminated header lines */
	uint64_t            background;   /**< nonzero: ask calls return jobs and
	                                       keep their upstream streams alive */
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
	enum ai_api         api;
	enum ai_provider    provider;
};

/** Whether @p ep requests native background execution as well as keeping
 *  its stream locally: OpenAI Responses or Gemini Interactions, including
 *  an explicit URL with no named provider. */
static inline bool
ai_background (struct ai_endpoint const *ep)
{
	return ep->background &&
	       ((ep->api == AI_RESPONSES &&
	         (ep->provider == AI_NO_PROVIDER || ep->provider == AI_OPENAI)) ||
	        (ep->api == AI_INTERACTIONS &&
	         (ep->provider == AI_NO_PROVIDER || ep->provider == AI_GOOGLE)));
}

/** Whether the upstream of @p ep may be llama-server, which says at /props
 *  what it has: one that ai_llama_like() allows, with an API that is not
 *  one of Gemini's. */
static inline bool
ai_props (struct ai_endpoint const *ep)
{
	return ai_llama_like(ep->provider) && ep->api != AI_GENERATE &&
	       ep->api != AI_INTERACTIONS;
}

/**
 * Build the request body asking @p prompt, with @p system instructions unless
 * that is nullptr, and offering the model @p tools, an array as the API
 * has them, unless that is nullptr; nullptr if out of memory.  Its messages
 * are its last member, so that the turns of a conversation (ai_format())
 * may follow them. @p history, unless nullptr, is an array of validated
 * user/assistant objects with string role and content, before the new prompt.
 * All four are JSON values, which the body takes references to; what it takes
 * from @p ep, it copies.
 */
extern struct json_object *
ai_request (struct ai_endpoint const *ep,
            struct json_object       *system,
            struct json_object       *prompt,
            struct json_object       *history,
            struct json_object       *tools);

/** What an event of a streamed answer says besides its text. */
struct ai_event {
	char const *error;      /**< the upstream's error message, or nullptr */
	char const *reason;     /**< the answer's stop reason, or nullptr */
	uint32_t    used;       /**< the tokens of the request and the answer, if
	                             it says; else 0 */
	uint8_t     thought;    /**< bool: the event carries the model's thinking */
	uint8_t     thought_end; /**< bool: a whole reasoning item, block or step
	                              explicitly ended, after this event's pieces */
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
	char const *extra;      /**< JSON text that the call carries back as it
	                             came, which the API's turns place (struct
	                             ai_format): Gemini's thought signature, in
	                             chat completions its extra_content, in
	                             Gemini's Interactions its signature */
	char const *args;       /**< a piece of its arguments' JSON text */
	size_t      id_len;
	size_t      name_len;
	size_t      extra_len;
	size_t      args_len;
	uint32_t    index;
};

/** Where the pieces of a streamed answer go: its text, and, unless nullptr,
 *  its thinking's text, its tool calls and its replay: the JSON text of
 *  what its turn gives back as it came (struct ai_format), in pieces, each
 *  of its items after a comma.  Each returns 0, or an error, which ends the
 *  reading of the event. */
struct ai_sink {
	int  (*text)    (void       *user,
	                 char const *text,
	                 size_t      len);
	int  (*thought) (void       *user,
	                 char const *text,
	                 size_t      len);
	int  (*call)    (void                 *user,
	                 struct ai_call const *call);
	int  (*replay)  (void       *user,
	                 char const *json,
	                 size_t      len);
	void  *user;
};

/** What the reading of a streamed answer keeps from one event to the next:
 *  zeroed before its first. */
struct ai_reader {
	uint32_t open;   /**< 1 + the content block whose item in the replay
	                      is open, in Anthropic's Messages, or 1 + the step
	                      whose item is open, in Gemini's Interactions;
	                      nonzero while a text part is, in Gemini's
	                      generateContent; else 0 */
	uint32_t calls;  /**< the answer's tool calls so far, in Gemini's
	                      generateContent, which numbers none; 1 + the step
	                      of the last function call that started, in
	                      Gemini's Interactions; 1 + the open thinking block
	                      in Anthropic's Messages, else 0 */
	union {
		uint32_t input;  /**< the request's tokens, in Anthropic's
		                      Messages, which says them first and the
		                      answer's last */
		struct {
			uint16_t kind;     /**< the type of the step whose item is
			                        open, in Gemini's Interactions:
			                        thought or model output; or the end
			                        of the answer, after which the
			                        events are ignored */
			uint8_t  summary;  /**< the state of the summary array of
			                        the open thought: not written, open
			                        or closed */
			uint8_t  sign;     /**< bool: the open thought has a
			                        signature */
		};
	};
};

/** What ends an answer the output token limit cut short. */
#define AI_CUT_NOTE "\n\n[The answer was cut short by the output token limit.]"

/**
 * Read an event of a streamed answer, its data parsed as @p ev, as
 * @p reader says the events before it left the reading: hand each piece of
 * it to @p sink, and say what else it says in @p out, zeroed to begin,
 * which then points into @p ev.  Returns the first nonzero that the sink
 * returned, else 0, or ENOMEM.  An answer whose stream ends before an
 * event says it has ended, or chat completions' [DONE], is cut off.
 */
extern int
ai_event (enum ai_api            api,
          struct ai_reader      *reader,
          struct json_object    *ev,
          struct ai_sink const  *sink,
          struct ai_event       *out);

/** How the turns of an API differ beyond their fragments (struct
 *  ai_format's flags). */
enum ai_format_flag {
	AI_ARGS_RAW     = 1u << 0,  /**< a call's arguments are a JSON object,
	                                 not the text of a JSON string */
	AI_RESULT_NAMED = 1u << 1,  /**< a result names its call by name, and
	                                 by id if the call had one or the turn
	                                 wrote the call (call) */
};

/**
 * The JSON text that the turns of a conversation are made of, in an API
 * whose answers may call tools: each fragment goes between the texts of
 * strings, which are escaped into it, and of JSON as it came.
 *
 * A model's turn is turn, which opens the list of its items, or, empty,
 * leaves them in the conversation's; then text and its text, if the API
 * has text, thought and its thinking's text if it has any, and calls, which
 * opens a list of their own if it is not empty.  Then the replay, its items
 * before each call, the call, and the items after the last: each call is
 * call, or call_next after an item in the list, its id, name, its name,
 * args, its arguments (AI_ARGS_RAW), args_end, extra and its extra's JSON
 * text if it has one (struct ai_call), and call_end, unless call is empty,
 * as the calls are in the replay.  Then turn_end.  An answer, a turn that
 * calls no tool, is the same but for calls, and ends with answer_end.
 *
 * The tools that a turn may call are an array as the API has them, which
 * tools and tools_end hold, of the items ai_tool() makes.
 *
 * The results of the calls follow: results, then each result after the
 * first result_sep, and result, the call's id, or its name and result_id
 * and its id, if it had one or call is not empty (AI_RESULT_NAMED), output,
 * the result's text, and result_end; then each file that the calls
 * attached, as file, its URI and file_end, if the API takes files
 * (ai_upload()); then results_end.
 *
 * prompt_end ends the prompt's text, after which end ends the conversation
 * and the body: what askai adds to the prompt goes before it.  A message
 * that askai adds to the conversation as the user's is user, its text, and
 * prompt_end.
 */
struct ai_format {
	struct ai_bytes {
		char const *ptr;
		size_t      len;
	}               tools,
	                tools_end,
	                turn,
	                text,
	                thought,
	                calls,
	                call,
	                call_next,
	                name,
	                args,
	                args_end,
	                extra,
	                call_end,
	                turn_end,
	                answer_end,
	                results,
	                result_sep,
	                result,
	                result_id,
	                output,
	                result_end,
	                results_end,
	                file,
	                file_end,
	                user,
	                prompt_end,
	                end;
	char const     *conversation;  /**< the body's member it is in */
	uint64_t        flags;         /**< enum ai_format_flag */
};

/**
 * The tool that @p def, a definition as MCP has it, defines, as @p api
 * offers it to the model, an item of the array that ai_format()'s tools and
 * tools_end hold; nullptr if out of memory.
 */
extern struct json_object *
ai_tool (enum ai_api               api,
         struct json_object const *def);

/** Whether @p reason, an answer's stop reason, is @p api's output
 *  token limit, which cuts it short. */
extern bool
ai_cut (enum ai_api       api,
        char const       *reason);

/** The format of turns in @p api. */
extern struct ai_format const *
ai_format (enum ai_api api);

/** How the upstream of an API takes a file that a turn attaches: a POST
 *  of the file, which is UTF-8 text, to the URL that ai_upload_url() makes,
 *  with the upstream's header lines and lines. */
struct ai_upload {
	char const        *path;   /**< at the root of the API */
	char const *const *lines;  /**< nullptr-terminated */
};

/** How the upstream of @p api takes a file, or nullptr if askai gives it
 *  none. */
extern struct ai_upload const *
ai_upload (enum ai_api api);

/**
 * The URL of @p path and then @p tail, unless it is nullptr, at the root of
 * Gemini's API that @p url, an upstream's URL, has: the part of @p url
 * before "/v1beta/", which may have a path of a gateway, and the parameter
 * key of the query of @p url, if it has one.  Returns a string for free(),
 * or nullptr if @p url has no "/v1beta/" or askai runs out of memory.
 */
extern char *
ai_gemini_url (char const *url,
               char const *path,
               char const *tail);

/** The URL to which the upstream of @p ep takes files (ai_upload()), a
 *  string for free(), or nullptr if it takes none or askai runs out of
 *  memory. */
extern char *
ai_upload_url (struct ai_endpoint const *ep);

/** The URL at which Gemini's API says what the model of @p ep has, its
 *  inputTokenLimit among them, a string for free(), or nullptr if @p ep has
 *  no model, no API of Gemini's, or no such URL (ai_gemini_url()). */
extern char *
ai_model_url (struct ai_endpoint const *ep);

/**
 * Read the answer of HTTP @p status to an upload (ai_upload()), the @p len
 * bytes at @p body: the URI of the file, printable ASCII, in @p uri, and
 * the time at which the file expires, in seconds since the epoch, in
 * @p *expires, or 0 if the answer does not say.  Returns 0, EIO with what
 * went wrong in @p why, ENOMEM, or EINVAL if an argument is not valid.
 */
extern int
ai_uploaded (enum ai_api  api,
             long         status,
             char const  *body,
             size_t       len,
             struct dstr *uri,
             int64_t     *expires,
             struct dstr *why);

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
ai_answer (enum ai_api         api,
           long                status,
           struct json_object *root,
           char const         *body,
           size_t              len,
           struct dstr        *text,
           bool               *failed);

/** Append only exposed reasoning text from a complete provider response
 *  @p root to @p text. Strings are decoded and concatenated without inserted
 *  separators; signatures, encrypted payloads and answer text are omitted.
 *  Missing reasoning is not an error. The caller retains both objects. */
extern int
ai_thoughts (enum ai_api         api,
             struct json_object *root,
             struct dstr        *text);

#endif /* TWINCEPTION_SRC_AI_H_ */
