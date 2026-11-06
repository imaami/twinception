/*
 * Adapted from llama.cpp common/console.h.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#ifndef TWINCEPTION_SRC_CLI_CONSOLE_H_
#define TWINCEPTION_SRC_CLI_CONSOLE_H_

#include "autocomplete.h"
#include "cli_style.h"
#include "cstr_view.h"
#include "dstr.h"
#include "list.h"
#include "tally.h"

struct cli_console;
struct cli_io;
struct cli_loop;

/** A line of input in a queue. */
struct cli_console_line {
	struct list hook;
	struct dstr text;
	size_t      flags;  /**< defined by the user of the queue */
};

/** A queue of lines of input, with the tally of the lines and of the
 *  total length of their text. A zeroed queue with its list initialized
 *  is empty.
 */
struct cli_console_lines {
	struct list  lines;
	struct tally used;
};

/** Move @p text into a new line with @p flags at the end of @p queue.
 *  Returns 0, or ENOMEM with @p text unchanged.
 */
extern int
cli_console_lines_push (struct cli_console_lines *queue,
                        struct dstr              *text,
                        size_t                    flags);

/** Remove the first line of @p queue, which must not be empty, and move
 *  its text into @p text, or free it if @p text is nullptr. Returns its
 *  flags.
 */
extern size_t
cli_console_lines_pop (struct cli_console_lines *queue,
                       struct dstr              *text);

/** Remove every line of @p queue. */
extern void
cli_console_lines_fini (struct cli_console_lines *queue);

/** The options of cli_console_create(). */
enum {
	CLI_CONSOLE_SIMPLE = 1u << 0,  /**< basic line input, also without a terminal */
	CLI_CONSOLE_COLOR  = 1u << 1,  /**< terminal colors */
};

/** Only the UI thread uses the console. loop and io must outlive it. flags
 *  are the CLI_CONSOLE options.
 */
extern struct cli_console *
cli_console_create (uint32_t         flags,
                    struct cli_loop *loop,
                    struct cli_io   *io,
                    int             *e);

/** Call after a completed reactor turn, never from descriptor dispatch. */
extern int
cli_console_service (struct cli_console *console);

extern void
cli_console_interrupt (struct cli_console *console,
                       bool                terminate);

extern void
cli_console_resize (struct cli_console *console);

extern int
cli_console_suspend (struct cli_console *console);

extern int
cli_console_resume (struct cli_console *console);

/** Keep the next draft editable while an operation runs. */
extern int
cli_console_busy (struct cli_console *console,
                  bool                busy);

/** Pending bytes are drained by UI turns; stop IO only once this is false. */
extern bool
cli_console_pending (struct cli_console const *console);

/** Accepted input or EOF is waiting; do not delay it for metadata refresh. */
extern bool
cli_console_queued (struct cli_console const *console);

/** Yield an interactive read once with EAGAIN, preserving its draft and
 * editor. Accepted lines and EOF take priority. Simple input is unaffected.
 */
extern void
cli_console_yield (struct cli_console *console);

/** Apply transport backpressure before the bounded output queue fills. */
extern bool
cli_console_blocked (struct cli_console const *console);

extern void
cli_console_destroy (struct cli_console **console);

/** Candidates are borrowed until the next callback. Their suffixes after
 * the cursor must be identical, unchanged suffixes of the input line.
 */
typedef int
cli_console_complete_fn (void                             *context,
                         struct cstr_view                  line,
                         size_t                            cursor,
                         struct autocomplete_match const **matches,
                         size_t                           *count);

extern void
cli_console_set_completion (struct cli_console      *console,
                            cli_console_complete_fn *callback,
                            void                    *context);

extern int
cli_console_remember (struct cli_console *console,
                      struct cstr_view    text);

/** Classification only; called inside Readline and must not perform work. */
typedef bool
cli_console_immediate_fn (void            *context,
                          struct cstr_view line);

extern void
cli_console_set_immediate (struct cli_console       *console,
                           cli_console_immediate_fn *callback,
                           void                     *context);

/** Take an immediate command outside Readline, preserving a queued prompt.
 * text has no appended newline. An occupied slot survives allocation errors.
 * ready is false while Readline is active or when no command is pending.
 */
extern int
cli_console_take_immediate (struct cli_console *console,
                            struct dstr        *text,
                            bool               *ready);

/** Read accepted lines in order, then EOF. The queue holds at most 32 lines
 * and 16 MiB; rejection keeps the editable draft and aborts a running macro.
 * EAGAIN yields without ending the editor. On error text is empty.
 */
extern int
cli_console_read (struct cli_console *console,
                  struct dstr        *text,
                  bool                multiline,
                  bool               *eof,
                  struct cstr_view    prompt);

extern int
cli_console_set_display (struct cli_console *console,
                         enum cli_style      display);

/** Write @p text to the conversation, or with @p reasoning to the
 *  reasoning, which is shown in its own bounded buffer and viewport in the
 *  full-screen UI. Peer-supplied terminal controls are filtered, including
 *  UTF-8 C1 controls.
 */
extern int
cli_console_write (struct cli_console *console,
                   bool                reasoning,
                   struct cstr_view    text);

/** A run of text, and the style it is written in. */
struct cli_console_span {
	struct cstr_view text;
	uint64_t         style;  /**< enum cli_style, widened over the padding */
};

/** Write the spans, each in its style, as cli_console_write() does, in one
 * output request; the display ends in the reset style. A span with a style
 * past the last of enum cli_style is EINVAL, and nothing is written.
 */
extern int
cli_console_print (struct cli_console            *console,
                   struct cli_console_span const *spans,
                   size_t                         count);

/** Write the parts to stderr, filtered as cli_console_write() filters. */
extern int
cli_console_write_error (struct cstr_view const *parts,
                         size_t                  count);

/** Full-screen diagnostics join scrollback; other modes retain stderr. */
extern int
cli_console_error (struct cli_console *console,
                   struct cstr_view    text);

extern int
cli_console_flush (struct cli_console *console);

#endif /* TWINCEPTION_SRC_CLI_CONSOLE_H_ */
