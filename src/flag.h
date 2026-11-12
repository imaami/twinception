/** @file flag.h
 *
 * The flag bits that are the same in the flag words of several modules,
 * and the rule for the bits of such a word.
 *
 * Some concepts are a bit in the flag words of several modules: a call
 * failed, an output was cut at a size limit, a deadline passed. Each such
 * concept is one bit of enum flag, and the bit is the same in every word.
 * A word that contains the concept uses that bit, under the FLAG_ name or
 * under a name that the module of the word defines as the FLAG_ name, as
 * TOOL_FAILED of src/tool.h is FLAG_FAILED. Code then copies the bit from
 * one word to another with a mask, such as `to |= from & FLAG_FAILED`,
 * and does not translate one bit to another.
 *
 * The rule for a word that contains a shared bit:
 * - The shared bits are the even bits of the low 16 bits: the bits 0, 2,
 *   4, 6, 8, 10, 12 and 14.
 * - The bits that only the module of the word defines are the odd bits of
 *   the low 16 bits, and then every bit from 16 on. The module defines
 *   them with FLAG_OWN(), so none of them is a shared bit.
 *
 * The bits of a word that contains no shared bit can be numbered from 0.
 *
 * This header adds no symbol, so a plugin can include it.
 */
#ifndef ASKAI_SRC_FLAG_H_
#define ASKAI_SRC_FLAG_H_

#include <stdint.h>

/** The shared bits. Each bit is a concept. What the concept is about
 *  depends on the word that contains the bit: a tool call, a job, a child
 *  process. In the comments below, that subject is the work. In a word of
 *  states, the bit is a state of the work. In a word of messages, the bit
 *  is the message.
 */
enum flag : uint16_t {
	FLAG_FAILED = 1u << 0,   /**< the work failed */
	FLAG_CUT    = 1u << 2,   /**< the output of the work was cut at a size
	                              limit, so the output is incomplete */
	FLAG_LATE   = 1u << 4,   /**< a time limit of the work passed before
	                              the work ended */
	FLAG_DONE   = 1u << 6,   /**< the work has ended, with or without
	                              success, and its result is final */
	FLAG_CANCEL = 1u << 8,   /**< the side that started the work cancels
	                              the work, or has cancelled it */
	FLAG_EXIT   = 1u << 10,  /**< the loop, the thread or the process that
	                              runs the work exits, or has exited */
	FLAG_SUBMIT = 1u << 12,  /**< the work is submitted to the side that
	                              runs it */
	FLAG_WRITE  = 1u << 14,  /**< the work writes to a repository, where
	                              other work only reads */
};

/** The n-th bit that a module defines for its own flag word. n counts
 *  from 0 and is at most 55. The bits from n = 0 to 7 are the bits 1, 3,
 *  5, 7, 9, 11, 13 and 15, and the bits from n = 8 on are the bits from 16
 *  on. So 4 of the bits fit in a word of 8 bits, 8 in a word of 16 bits,
 *  24 in a word of 32 bits and 56 in a word of 64 bits. The value is a
 *  uint64_t constant. An enum with a fixed type of 8, 16 or 32 bits does
 *  not compile if n is too large for the type.
 */
#define FLAG_OWN(n) (UINT64_C(1) << ((n) < 8 ? 2 * (n) + 1 : (n) + 8))

#endif /* ASKAI_SRC_FLAG_H_ */
