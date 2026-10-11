/** @file askai's configuration: what its command line sets, and what a
 *  configuration file is to (design section 9, S7). */
#ifndef TWINCEPTION_SRC_CONFIG_H_
#define TWINCEPTION_SRC_CONFIG_H_

#include <stdint.h>

/** Options that are on or off, as bits of struct config's flags. */
enum config_flag {
	CFG_MAN       = 1u << 0,  /**< offer the man tool */
	CFG_TEST_FIFO = 1u << 1,  /**< test aid: tool calls wait for the upstream
	                               in one queue, first in first out */
};

struct config {
	uint32_t flags;        /**< enum config_flag */
	uint32_t calls;        /**< tool calls in flight, at most */
	uint32_t man_recheck;  /**< seconds that man's finding of a page's file
	                            holds (src/manual.h) */
};

#endif /* TWINCEPTION_SRC_CONFIG_H_ */
