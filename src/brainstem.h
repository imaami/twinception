/** @file The brainstem: the main thread, which minds the process and the
 *  machine it runs on, never client work (design sections 1 and 6).
 *
 * It blocks every asynchronous signal before it starts a thread, so that it
 * alone takes them, through a signalfd; grows the descriptor table while the
 * process has one thread, as the kernel stalls a threaded process for an RCU
 * grace period each time its table grows; starts the logger, the blockers and
 * the event loop's thread, each of which reports back within a time limit;
 * watches the loop's turns and the process's resources; and shuts down.  It
 * never takes a lock the loop holds nor waits for the loop, so it stays
 * responsive when everything else is stuck.
 *
 * Shutdown: the first SIGINT, SIGTERM or SIGHUP has the loop drain within the
 * grace period; a second signal, the grace period running out, or a loop
 * stuck in one turn while it should drain, ends the process at once, after the
 * logger has had a moment to write what it holds, with BRAINSTEM_FORCED.  Of
 * those signals, any the process started with ignored, as under nohup, stays
 * ignored.
 */
#ifndef TWINCEPTION_SRC_BRAINSTEM_H_
#define TWINCEPTION_SRC_BRAINSTEM_H_

#include <stdatomic.h>
#include <stdint.h>

struct blockers;
struct loop;

/** The exit status of a shutdown the brainstem forced. */
enum { BRAINSTEM_FORCED = 3 };

/** What runs on the loop's thread. */
struct brainstem_app {
	void *ctx;
	/** Set up on @p loop, before its first turn; an error fails the start.
	 *  stop() follows, whatever it returns. */
	int  (*start) (void            *ctx,
	               struct loop     *loop,
	               struct blockers *blockers);
	/** Drain: wind down, and have the loop exit, by @p deadline (ns). */
	void (*drain) (void            *ctx,
	               struct loop     *loop,
	               uint64_t         deadline);
	/** Tear down, after the loop's last turn. */
	void (*stop)  (void            *ctx,
	               struct loop     *loop);
};

struct brainstem_cfg {
	struct brainstem_app app;
	uint64_t             stall_ms;     /**< test aid: SIGUSR2 holds the loop in
	                                        one turn this long; 0: off */
	uint32_t             grace_ms;     /**< for draining */
	uint32_t             watchdog_ms;  /**< a turn longer than this is reported */
};

/** What the brainstem samples and counts; it is the one writer. */
struct brainstem_metrics {
	_Atomic uint64_t rss;           /**< resident bytes */
	_Atomic uint64_t fds;           /**< descriptors open */
	_Atomic uint64_t fd_limit;      /**< RLIMIT_NOFILE, soft */
	_Atomic uint64_t psi[3];        /**< cpu, memory, io: some avg10, in
	                                     hundredths of a percent */
	_Atomic uint64_t psi_total[3];  /**< their some total, in µs */
	_Atomic uint64_t psi_ok;        /**< bool: /proc/pressure is there */
	_Atomic uint64_t reports;       /**< long turns the watchdog reported */
	_Atomic uint64_t late;          /**< times the brainstem woke late */
};

extern struct brainstem_metrics brainstem_metrics;

/** Run the process's threads until shutdown, once; returns the exit status. */
extern int
brainstem_run (struct brainstem_cfg const *cfg);

#endif /* TWINCEPTION_SRC_BRAINSTEM_H_ */
