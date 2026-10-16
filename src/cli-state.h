/** @file Private, bounded restart state for one CLI conversation. */
#ifndef TWINCEPTION_SRC_CLI_STATE_H_
#define TWINCEPTION_SRC_CLI_STATE_H_

#ifdef __cplusplus
# include <cstddef>
# define STD(x) std::x
#else
# include <stddef.h>
# define STD(x) x
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct cli_state;

struct cli_state_text {
	char const *data;
	STD(size_t) len;
};

struct cli_state_turn {
	struct cli_state_text prompt;
	struct cli_state_text answer;
};

/** An empty path selects $XDG_STATE_HOME/askai/cli.git (or the HOME
 * fallback). The repository must be private and dedicated to CLI history.
 * Each writer forks a random branch from the latest saved conversation.
 * Recall merges exact text from up to 32 recent writer tips; conversation
 * turns come only from the latest writer. The full identity is checked
 * when loading, so a filename hash collision cannot join conversations. */
extern struct cli_state *
cli_state_create (struct cli_state_text path,
                  struct cli_state_text identity,
                  int                  *e);

extern void
cli_state_destroy (struct cli_state **state);

/** Relative recovery filename inside the repository after a Git failure.
 * An empty view means no complete uncommitted snapshot is available. */
extern struct cli_state_text
cli_state_recovery_file (struct cli_state const *state);

extern STD(size_t)
cli_state_input_count (struct cli_state const *state);

/** Views remain valid until the next mutation. */
extern struct cli_state_text
cli_state_input_at (struct cli_state const *state,
                    STD(size_t)            index);

extern STD(size_t)
cli_state_turn_count (struct cli_state const *state);

extern struct cli_state_turn
cli_state_turn_at (struct cli_state const *state,
                   STD(size_t)            index);

/** Each mutation commits a snapshot and readable records before returning.
 * A write failure leaves the in-memory update available for a retry.
 * Inputs retain their newlines, up to 256 entries and 1 MiB of text.
 * Oversized individual inputs return EFBIG rather than silently vanish. */
extern int
cli_state_add_input (struct cli_state     *state,
                     struct cli_state_text text);

/** Retains at most 512 completed turns and 16 MiB of text. Nonzero replace
 * replaces the last completed turn; incomplete turns never enter state. */
extern int
cli_state_add_turn (struct cli_state     *state,
                    struct cli_state_text prompt,
                    struct cli_state_text answer,
                    int                   replace);

/** Clears completed conversation turns, preserving input recall. */
extern int
cli_state_clear_turns (struct cli_state *state);

#ifdef __cplusplus
}
#endif

#undef STD
#endif
