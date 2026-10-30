/** @file
 * Private, bounded restart state for one CLI conversation.
 */
#ifndef TWINCEPTION_SRC_CLI_STATE_H_
#define TWINCEPTION_SRC_CLI_STATE_H_

#include <stdatomic.h>

#include "cstr_view.h"

struct cli_state;

struct cli_state_turn {
	struct cstr_view prompt;
	struct cstr_view answer;
};

/** An empty path selects $XDG_STATE_HOME/askai/cli.git (or the HOME
 * fallback). The repository must be private and dedicated to CLI history.
 * Each writer forks a random branch from the latest saved conversation.
 * Recall merges exact text from up to 32 recent writer tips; conversation
 * turns come only from the latest writer. The full identity is checked
 * when loading, so a filename hash collision cannot join conversations.
 * If cancel is nonnull, it must outlive the state; setting it interrupts
 * contended history locks without blocking the UI.
 */
extern struct cli_state *
cli_state_create (struct cstr_view    path,
                  struct cstr_view    identity,
                  _Atomic bool const *cancel,
                  int                *e);

extern void
cli_state_destroy (struct cli_state **state);

/** Relative recovery filename inside the repository after a Git failure.
 * An empty view means no complete uncommitted snapshot is available.
 */
extern struct cstr_view
cli_state_recovery_file (struct cli_state const *state);

extern size_t
cli_state_input_count (struct cli_state const *state);

/** Views remain valid until the next mutation. */
extern struct cstr_view
cli_state_input_at (struct cli_state const *state,
                    size_t                  index);

extern size_t
cli_state_turn_count (struct cli_state const *state);

extern struct cli_state_turn
cli_state_turn_at (struct cli_state const *state,
                   size_t                  index);

/** Each mutation commits a snapshot and readable records before returning.
 * A write failure leaves the in-memory update available for a retry.
 * Inputs retain their newlines, up to 256 entries and 1 MiB of text.
 * Oversized individual inputs return EFBIG rather than silently vanish.
 */
extern int
cli_state_add_input (struct cli_state *state,
                     struct cstr_view  text);

/** Retains at most 512 completed turns and 16 MiB of text. Nonzero replace
 * replaces the last completed turn; incomplete turns never enter state.
 */
extern int
cli_state_add_turn (struct cli_state *state,
                    struct cstr_view  prompt,
                    struct cstr_view  answer,
                    int               replace);

/** Clears completed conversation turns, preserving input recall. */
extern int
cli_state_clear_turns (struct cli_state *state);

#endif /* TWINCEPTION_SRC_CLI_STATE_H_ */
