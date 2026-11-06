/** @file
 * Private, bounded restart state for one CLI conversation.
 */
#ifndef TWINCEPTION_SRC_CLI_STATE_H_
#define TWINCEPTION_SRC_CLI_STATE_H_

#include <stdatomic.h>

#include "cstr_view.h"
#include "dstr.h"

/** CLI_STATE_INPUTS is the maximum number of inputs that a state stores, and
 *  CLI_STATE_INPUT_BYTES is the maximum total size of their text. The input
 *  history of the console is bounded by the same values, so every restored
 *  input fits in it. CLI_STATE_TURNS is the maximum number of completed
 *  turns that a state stores, and cli_ask() refuses to send a conversation
 *  of that many turns.
 */
enum {
	CLI_STATE_INPUTS      = 256,
	CLI_STATE_INPUT_BYTES = 1 << 20,
	CLI_STATE_TURNS       = 512,
};

struct cli_state;

/** A completed turn of the conversation. */
struct cli_state_turn {
	struct dstr prompt;
	struct dstr answer;
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

/** Returns the completed turn at @p index, or nullptr if @p index is out of
 *  range. The turn stays valid until the next mutation.
 */
extern struct cli_state_turn const *
cli_state_turn_at (struct cli_state const *state,
                   size_t                  index);

/** Each mutation commits a snapshot and readable records before returning.
 * A write failure leaves the in-memory update available for a retry.
 * Inputs are stored with their newlines, at most CLI_STATE_INPUTS of them
 * with at most CLI_STATE_INPUT_BYTES bytes of text. For a longer input,
 * the function returns EFBIG and stores nothing.
 */
extern int
cli_state_add_input (struct cli_state *state,
                     struct cstr_view  text);

/** Retains at most CLI_STATE_TURNS completed turns and 16 MiB of text.
 * Nonzero replace replaces the last completed turn; incomplete turns
 * never enter state.
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
