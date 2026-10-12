/** @file The tools that askai offers, as masks of their ids, which roles
 *  grant (src/policy.h) and requests carry (src/stream.h). */
#ifndef TWINCEPTION_SRC_TOOL_H_
#define TWINCEPTION_SRC_TOOL_H_

/** The tools askai may offer at most: a mask's width. */
enum { TOOL_MAX = 128 };

/** Tools, bit i that of id i. */
typedef unsigned _BitInt(TOOL_MAX) tool_mask;

#endif /* TWINCEPTION_SRC_TOOL_H_ */
