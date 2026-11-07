/** @file confine.h
 *
 * The confinement of askai-code (src/cxi.h) while clang parses committed
 * code.
 */
#ifndef ASKAI_SRC_CONFINE_H_
#define ASKAI_SRC_CONFINE_H_

/** Returns the Landlock ABI version of the kernel, or 0 without Landlock. */
extern int
confine_abi (void);

/** Sets no_new_privs and the limits of the process: no core file, a file
 *  size of 0, so that a write to a regular file fails, and 1 GiB of address
 *  space. Returns 0 or the error number.
 */
extern int
confine_limits (void);

/**
 * Sets no_new_privs, but not the limits, so that askai-code --check can
 * write to the memfd of child_capture(). Then, with Landlock, the process
 * can read only /usr, a few system files that clang reads and @p tree
 * unless it is nullptr, and write only /dev/null. With seccomp, a system
 * call of another architecture kills the process, and the calls that
 * create sockets, run programs, trace or signal other processes, change
 * namespaces, the limits, priority or session of a process, or the mode,
 * owner, times, extended attributes or size of a file fail with EPERM.
 * Returns 0, ENOTSUP without Landlock or a seccomp filter, or the error
 * number.
 */
extern int
confine_cage (char const *tree);

#endif /* ASKAI_SRC_CONFINE_H_ */
