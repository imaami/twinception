/** @file The brainstem: the main thread, which minds the process and the
 *  machine it runs on, never client work (design sections 1 and 6).
 *
 * It blocks every asynchronous signal before it starts a thread, so that it
 * alone takes them, through a signalfd; grows the descriptor table while the
 * process has one thread, as the kernel stalls a threaded process for an RCU
 * grace period each time its table grows; starts the logger, the blockers and
 * the event loops' threads, home, the sidekick that runs the jobs and accepts
 * the clients, and then the client loops, each of which reports back within a
 * time limit, and once all run has home open its door; watches their turns
 * and the process's resources; and shuts down.  It never takes a lock a loop
 * holds nor waits for a loop, so it stays responsive when everything else is
 * stuck.
 *
 * Memory pressure (design section 5) is a level, enum buf_pressure, that the
 * brainstem works out each second from its inputs, each with hysteresis, and
 * publishes in buf_pressure.  When it rises, and every 10 s while it
 * lasts, the brainstem has every loop relieve it; each gives back what it
 * keeps, the last of them trims the allocator once for all, and the brainstem
 * itself frees nothing of theirs.
 *
 * Shutdown: the first SIGINT, SIGTERM or SIGHUP has the loops drain within
 * the grace period, as does a loop that exits on its own or fails to start.
 * Once the client loops have exited, home is told it is alone, and exits once
 * its jobs have ended.  A second signal, the grace period running out, or a
 * loop stuck in one turn while it should drain, ends the process at once,
 * after the logger has had a moment to write what it holds, with
 * BRAINSTEM_FORCED.  Of those signals, any the process started with ignored,
 * as under nohup, stays ignored.
 */
#ifndef TWINCEPTION_SRC_BRAINSTEM_H_
#define TWINCEPTION_SRC_BRAINSTEM_H_

#include <stdatomic.h>
#include <stdint.h>

struct blocker;
struct loop;

/** The exit status of a shutdown the brainstem forced; the time between the
 *  loops' reliefs of memory pressure that lasts. */
enum { BRAINSTEM_FORCED = 3, BRAINSTEM_RELIEF_MS = 10'000 };

/** The least default resident threshold: the default body budget, which
 *  bounds the working set the default limits allow. */
static constexpr uint64_t MEMORY_HIGH_MIN = 256 << 20;

/** The inputs to the memory pressure level. */
enum brainstem_input {
	PRESSURE_RESIDENT,  /**< resident memory, against memory_high */
	PRESSURE_SYSTEM,    /**< RAM in use: MemTotal less MemAvailable */
	PRESSURE_CGROUP,    /**< a cgroup's memory.current less its inactive
	                         file cache, against its limit */
	PRESSURE_PSI,       /**< memory pressure stall information */
	PRESSURE_TEST,      /**< the test aid's file */
	PRESSURE_INPUTS
};

/** What runs on a loop's thread, with the loop's context: @p ctx, or for
 *  the client loops an array of them, loop k's at ctx + (k - 1) * stride. */
struct brainstem_app {
	void   *ctx;
	size_t  stride;
	/** Set up on @p loop, before its first turn; an error fails the start.
	 *  stop() follows, whatever it returns. */
	int  (*start)   (void            *ctx,
	                 struct loop     *loop,
	                 struct blocker  *blockers);
	/** Home's only: every client loop runs; begin to serve. */
	void (*open)    (void            *ctx,
	                 struct loop     *loop);
	/** Drain: wind down, and have the loop exit, by @p deadline (ns). */
	void (*drain)   (void            *ctx,
	                 struct loop     *loop,
	                 uint64_t         deadline);
	/** Relieve memory pressure, as buf_pressure says, and once done call
	 *  brainstem_relieved(). */
	void (*relieve) (void            *ctx,
	                 struct loop     *loop);
	/** Home's only: the client loops have exited; exit once no work is
	 *  left. */
	void (*alone)   (void            *ctx,
	                 struct loop     *loop);
	/** Home's only, if set: SIGHUP, which then reloads the configuration
	 *  rather than stopping. */
	void (*reload)  (void            *ctx,
	                 struct loop     *loop);
	/** Tear down, after the loop's last turn. */
	void (*stop)    (void            *ctx,
	                 struct loop     *loop);
};

struct brainstem_cfg {
	struct brainstem_app home;         /**< runs on home, whose loop is 0 */
	struct brainstem_app client;       /**< runs on each client loop, 1 to
	                                        loops */
	uint64_t             memory_high;  /**< resident bytes from which memory
	                                        pressure is high; 0: a sixteenth of
	                                        the memory the process may use, at
	                                        least MEMORY_HIGH_MIN */
	uint64_t             stall_ms;     /**< test aid: SIGUSR2 holds each loop
	                                        in one turn this long; 0: off */
	char const          *pressure;     /**< test aid: a file holding a memory
	                                        pressure level, which the level is
	                                        at least; nullptr: none */
	size_t               loops;        /**< client loops, 1 to 255 */
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
	_Atomic uint64_t pressure;      /**< memory pressure, enum buf_pressure */
	_Atomic uint64_t inputs[PRESSURE_INPUTS];  /**< the level each gives */
	_Atomic uint64_t mem_high;      /**< resident bytes from which memory
	                                     pressure is high */
	_Atomic uint64_t mem_total;     /**< MemTotal, bytes */
	_Atomic uint64_t mem_avail;     /**< MemAvailable, bytes */
	_Atomic uint64_t cg_usage;      /**< memory.current, less inactive file
	                                     cache, of the cgroup nearest its
	                                     limit */
	_Atomic uint64_t cg_limit;      /**< that limit; 0: none */
	_Atomic uint64_t reports;       /**< long turns the watchdog reported */
	_Atomic uint64_t late;          /**< times the brainstem woke late */
};

extern struct brainstem_metrics brainstem_metrics;

/** Run the process's threads until shutdown, once; returns the exit status. */
extern int
brainstem_run (struct brainstem_cfg const *cfg);

/** The CPUs the process may run on: those of its affinity, but no more than
 *  the cpu.max quotas of its cgroup and their ancestors allow. */
extern size_t
brainstem_cpus (void);

/**
 * On a loop, as it ends the relief of memory pressure it was ordered, having
 * given back @p bytes of buffers: true for the last of the loops ordered,
 * which then trims the allocator, once for all, and calls
 * brainstem_trimmed().  A relief begins only once the last has ended, so one
 * trim at most is under way, and nobody waits for it.
 */
extern bool
brainstem_relieved (uint64_t bytes);

/** On the last loop of a relief, once its trim is done, or skipped as the
 *  pressure cleared: returns the bytes the loops gave back, and lets the
 *  next relief begin. */
extern uint64_t
brainstem_trimmed (void);

#endif /* TWINCEPTION_SRC_BRAINSTEM_H_ */
