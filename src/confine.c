/** @file confine.c
 *
 * The confinement of askai-code.
 */
#define _GNU_SOURCE /* syscall() */
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "compat.h"
#include "confine.h"

#if defined(__x86_64__)
#define CONFINE_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define CONFINE_ARCH AUDIT_ARCH_AARCH64
#endif

/* What clang reads outside the tree: /usr, and the files that the clang
 * driver reads to detect the distribution, the time zone and the processor.
 */
static char const *const confine_reads[] = {
	"/usr", "/etc/os-release", "/etc/lsb-release", "/etc/debian_version",
	"/etc/redhat-release", "/etc/localtime", "/proc/cpuinfo",
	"/proc/sys/vm/overcommit_memory", "/sys/devices/system/cpu",
};

int
confine_abi (void)
{
	long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0,
	                   LANDLOCK_CREATE_RULESET_VERSION);
	return abi > 0 ? (int)abi : 0;
}

/* Lowers both limits of @p resource to @p most or the hard limit. */
static int
confine_limit (int    resource,
               rlim_t most)
{
	struct rlimit r;
	if (getrlimit(resource, &r))
		return errno;
	r.rlim_cur = r.rlim_max = r.rlim_max < most ? r.rlim_max : most;
	return setrlimit(resource, &r) ? errno : 0;
}

int
confine_limits (void)
{
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return errno;
	int e = confine_limit(RLIMIT_CORE, 0);
	if (!e)
		e = confine_limit(RLIMIT_FSIZE, 0);
	return e ? e : confine_limit(RLIMIT_AS, (rlim_t)1 << 30);
}

/* Allows @p rights beneath @p path, if it exists, in the ruleset @p rs,
 * without READ_DIR for a file, which Landlock rejects.
 */
static int
confine_grant (int         rs,
               char const *path,
               uint64_t    rights)
{
	int fd = open(path, O_PATH | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT ? 0 : errno;
	struct stat st;
	int e = fstat(fd, &st) ? errno : 0;
	struct landlock_path_beneath_attr const rule = {
		.allowed_access = !e && S_ISDIR(st.st_mode) ? rights
		                                            : rights & ~LANDLOCK_ACCESS_FS_READ_DIR,
		.parent_fd = fd,
	};
	if (!e && syscall(SYS_landlock_add_rule, rs, LANDLOCK_RULE_PATH_BENEATH, &rule, 0))
		e = errno;
	close(fd);
	return e;
}

static int
confine_seccomp (void)
{
#ifdef CONFINE_ARCH
	static unsigned const deny[] = {
		__NR_socket, __NR_socketpair, __NR_connect, __NR_bind, __NR_listen,
		__NR_accept, __NR_accept4, __NR_sendto, __NR_sendmsg, __NR_sendmmsg,
		__NR_execve, __NR_execveat, __NR_ptrace, __NR_process_vm_readv,
		__NR_process_vm_writev, __NR_io_uring_setup, __NR_io_uring_enter,
		__NR_io_uring_register, __NR_pidfd_open, __NR_pidfd_getfd,
		__NR_pidfd_send_signal, __NR_unshare, __NR_setns, __NR_mount, __NR_bpf,
		__NR_perf_event_open, __NR_userfaultfd, __NR_keyctl, __NR_add_key,
		__NR_request_key, __NR_tkill, __NR_setsid, __NR_setpgid, __NR_prlimit64,
		__NR_setpriority, __NR_sched_setaffinity, __NR_sched_setscheduler,
		__NR_sched_setparam, __NR_sched_setattr, __NR_ioprio_set, __NR_fchmod,
		__NR_fchmodat, __NR_fchown, __NR_fchownat, __NR_utimensat, __NR_setxattr,
		__NR_lsetxattr, __NR_fsetxattr, __NR_removexattr, __NR_lremovexattr,
		__NR_fremovexattr, __NR_truncate, __NR_ftruncate, __NR_fallocate,
#ifdef __x86_64__
		__NR_chmod, __NR_chown, __NR_lchown, __NR_utime, __NR_utimes, __NR_futimesat,
#endif
#ifdef __NR_fchmodat2
		__NR_fchmodat2,
#endif
#ifdef __NR_setxattrat
		__NR_setxattrat, __NR_removexattrat,
#endif
	};
	/* Allowed only with the own PID first, so that abort() works. */
	static unsigned const own[] = {
		__NR_kill, __NR_tgkill, __NR_rt_sigqueueinfo, __NR_rt_tgsigqueueinfo,
	};
	/* The jumps go forward: the denied calls to ERR, the own ones to OWN,
	 * and fcntl() with F_SETOWN or F_SETOWN_EX, which would send the
	 * signals of a descriptor to another process, to ERR. */
	enum { HEAD = 4, FCNTL = HEAD + array_size(deny) + array_size(own) };
	enum { OWN = FCNTL + 4, OK = OWN + 2, ERR = OK + 1, KILL = ERR + 1 };
	struct sock_filter f[KILL + 1] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, CONFINE_ARCH, 0, KILL - 2),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
		/* x32 calls on x86_64 have bit 30 set; no call elsewhere has. */
		BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, KILL - 4, 0),
		[FCNTL] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, OK - FCNTL - 1),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETOWN, ERR - FCNTL - 3, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETOWN_EX, ERR - FCNTL - 4, OK - FCNTL - 4),
		[OWN] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)getpid(), 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
	};
	for (size_t i = 0, n = HEAD; i < array_size(deny); ++i, ++n)
		f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, deny[i],
		                                    (uint8_t)(ERR - n - 1), 0);
	for (size_t i = 0, n = HEAD + array_size(deny); i < array_size(own); ++i, ++n)
		f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, own[i],
		                                    (uint8_t)(OWN - n - 1), 0);
	struct sock_fprog const prog = { .len = array_size(f), .filter = f };
	return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) ? errno : 0;
#else
	return ENOTSUP;
#endif
}

int
confine_cage (char const *tree)
{
	/* The number of file system rights of each Landlock ABI, the low bits
	 * of handled_access_fs: REFER came in ABI 2, TRUNCATE in 3, IOCTL_DEV
	 * in 5 and RESOLVE_UNIX in 9. */
	static unsigned char const rights[] = { 0, 13, 14, 15, 15, 16, 16, 16, 16, 17 };
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return errno;
	size_t const abi = (size_t)confine_abi();
	if (!abi)
		return ENOTSUP;
	uint64_t const handled = (UINT64_C(1) << rights[abi < array_size(rights) ? abi
	                                               : array_size(rights) - 1]) - 1;
	struct landlock_ruleset_attr const attr = { .handled_access_fs = handled };
	int rs = (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0);
	if (rs < 0)
		return errno;
	uint64_t const read = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
	int e = 0;
	for (size_t i = 0; !e && i < array_size(confine_reads); ++i)
		e = confine_grant(rs, confine_reads[i], read);
	if (!e && tree)
		e = confine_grant(rs, tree, read);
	if (!e)
		e = confine_grant(rs, "/dev/null", LANDLOCK_ACCESS_FS_READ_FILE |
		                                   LANDLOCK_ACCESS_FS_WRITE_FILE);
	if (!e && syscall(SYS_landlock_restrict_self, rs, 0))
		e = errno;
	close(rs);
	return e ? e : confine_seccomp();
}
