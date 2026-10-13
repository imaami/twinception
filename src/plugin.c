/** @file askai's side of a plugin (plugin.h): reading the manifest from the
 *  plugin's file, and loading the plugin. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <link.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "plugin.h"
#include "util.h"

/* The ELF header of askai itself.  The linker defines the symbol. */
[[gnu::visibility("hidden")]] extern ElfW(Ehdr) const __ehdr_start;

/* The plugin's file while plugin_manifest_read() reads it. */
struct plugin_file {
	uint64_t         size;   /* of the file in bytes, from fstat() */
	struct manifest *dest;   /* where the manifest is copied to */
	char            *why;    /* where the reason for a refusal is written */
	size_t           len;    /* of why in bytes */
	uint32_t         found;  /* the number of askai manifests read so far */
	int              fd;
};

/* Writes the reason for a refusal into the len bytes at why.  Returns
 * EINVAL. */
[[gnu::format(printf, 3, 4)]]
static int
plugin_refuse (char       *why,
               size_t      len,
               char const *fmt,
               ...)
{
	va_list ap;
	va_start(ap, fmt);
	util_vformat(why, len, fmt, ap);
	va_end(ap);
	return EINVAL;
}

/* Reads the n bytes at offset off of the file into buf.  The bytes must
 * end at or before offset end, and end must not be greater than f->size.
 * Returns 0, or EINVAL with the reason in f->why. */
static int
plugin_file_read (struct plugin_file const *f,
                  uint64_t                  off,
                  uint64_t                  end,
                  void                     *buf,
                  size_t                    n)
{
	if (off > end || n > end - off)
		return plugin_refuse(f->why, f->len, "a truncated or malformed ELF file");
	for (size_t done = 0; done < n; ) {
		ssize_t r = pread(f->fd, (char *)buf + done, n - done,
		                  (off_t)(off + done));
		if (r > 0)
			done += (size_t)r;
		else if (!r)
			return plugin_refuse(f->why, f->len, "became shorter while it was read");
		else if (errno != EINTR)
			return plugin_refuse(f->why, f->len, "cannot read it: %s",
			                     strerror(errno));
	}
	return 0;
}

/* Reads the notes of one PT_NOTE segment: the bytes of the file from offset
 * off to offset end.  In each note, the header and the name together are
 * padded to a multiple of pad + 1 bytes.  The desc is padded to a multiple
 * of pad + 1 bytes too.  Copies the desc of an askai manifest to f->dest
 * and counts it in f->found.  Returns 0, or EINVAL with the reason in
 * f->why. */
static int
plugin_file_notes (struct plugin_file *f,
                   uint64_t            off,
                   uint64_t            end,
                   uint64_t            pad)
{
	while (off < end) {
		ElfW(Nhdr) nh;
		int e = plugin_file_read(f, off, end, &nh, sizeof nh);
		if (e)
			return e;
		/* The read succeeded, so off + sizeof nh <= end, and end is at
		 * most the greatest off_t.  n_namesz and n_descsz are 32 bits
		 * wide, so the sums below do not overflow 64 bits. */
		uint64_t name = off + sizeof nh;
		uint64_t desc = off + ((nh.n_namesz + pad + sizeof nh) & ~pad);
		off = desc + ((nh.n_descsz + pad) & ~pad);
		if (nh.n_type != MANIFEST_TYPE || nh.n_namesz != sizeof "askai")
			continue;
		char owner[sizeof "askai"];
		e = plugin_file_read(f, name, end, owner, sizeof owner);
		if (e)
			return e;
		if (memcmp(owner, "askai", sizeof owner))
			continue;
		if (f->found++)
			return plugin_refuse(f->why, f->len, "has two askai manifests");
		if (nh.n_descsz != sizeof *f->dest)
			return plugin_refuse(f->why, f->len, "its manifest has the wrong size");
		e = plugin_file_read(f, desc, end, f->dest, sizeof *f->dest);
		if (e)
			return e;
	}
	return 0;
}

/* Reads the manifest of the plugin whose file is open at fd into *dest.
 * The file must be a regular file.  Its contents must be a shared object
 * for the machine that askai runs on, and its PT_NOTE segments must contain
 * exactly one note with the owner "askai" and the type MANIFEST_TYPE.
 * Returns 0, or EINVAL with the reason in the len bytes at why. */
static int
plugin_manifest_read (struct manifest *dest,
                      int              fd,
                      char            *why,
                      size_t           len)
{
	struct stat st;
	if (fstat(fd, &st))
		return plugin_refuse(why, len, "cannot read it: %s", strerror(errno));
	if (!S_ISREG(st.st_mode))
		return plugin_refuse(why, len, "not a regular file");
	struct plugin_file f = {
		.size = st.st_size > 0 ? (uint64_t)st.st_size : 0,
		.dest = dest,
		.why  = why,
		.len  = len,
		.fd   = fd,
	};
	/* A file that is shorter than an ELF header is not read.  The header
	 * stays zero, and the comparison below refuses it. */
	ElfW(Ehdr) eh = {};
	if (f.size >= sizeof eh) {
		int e = plugin_file_read(&f, 0, f.size, &eh, sizeof eh);
		if (e)
			return e;
	}
	/* The first EI_VERSION bytes of e_ident are the magic number, the
	 * class and the byte order. */
	if (memcmp(eh.e_ident, __ehdr_start.e_ident, EI_VERSION) ||
	    eh.e_machine != __ehdr_start.e_machine || eh.e_type != ET_DYN ||
	    eh.e_phentsize != sizeof (ElfW(Phdr)))
		return plugin_refuse(why, len, "not a shared object of this machine");

	/* A program header that was read ends inside the file, so adding
	 * sizeof ph to its offset does not overflow. */
	uint64_t off = eh.e_phoff;
	for (uint32_t i = eh.e_phnum; i; --i, off += sizeof (ElfW(Phdr))) {
		ElfW(Phdr) ph;
		int e = plugin_file_read(&f, off, f.size, &ph, sizeof ph);
		if (e)
			return e;
		if (ph.p_type != PT_NOTE)
			continue;
		if (ph.p_offset > f.size || ph.p_filesz > f.size - ph.p_offset)
			return plugin_refuse(why, len, "a truncated or malformed ELF file");
		e = plugin_file_notes(&f, ph.p_offset, ph.p_offset + ph.p_filesz,
		                      ph.p_align == 8 ? 7 : 3);
		if (e)
			return e;
	}
	return f.found ? 0 : plugin_refuse(why, len, "has no askai manifest");
}

/* A name is 1 to 31 characters of [-0-9_a-z] followed by a NUL.  Returns
 * the length of the name in the MANIFEST_NAME bytes at name, or 0 if those
 * bytes are not a name. */
static uint8_t
plugin_name_len (char const *name)
{
	size_t n = strnlen(name, MANIFEST_NAME);
	return n < MANIFEST_NAME &&
	       strspn(name, "-0123456789_abcdefghijklmnopqrstuvwxyz") == n
	     ? (uint8_t)n : 0;
}

/* Checks the manifest *m, which was read from a plugin's file.  The epoch
 * and the ABI values must equal askai's own.  The plugin's name and the
 * tool names must be valid, and the tool names must be the leading entries
 * of m->tools.  Stores the number of tool names in *n_tools and the length
 * of m->tools[i] in lens[i].  Returns 0, or EINVAL with the reason in the
 * len bytes at why. */
static int
plugin_manifest_check (struct manifest const *m,
                       uint32_t              *n_tools,
                       uint8_t                lens[MANIFEST_TOOLS],
                       char                  *why,
                       size_t                 len)
{
	static uint32_t const abi[MANIFEST_ABI] = { PLUGIN_ABI(PLUGIN_ABI_U32) };
	if (m->epoch != PLUGIN_EPOCH)
		return plugin_refuse(why, len, "its plugin epoch is %" PRIu32
		                     ", askai's is %" PRIu32, m->epoch, PLUGIN_EPOCH);
	for (size_t i = 0; i < MANIFEST_ABI; ++i)
		if (m->abi[i] != abi[i])
			return plugin_refuse(why, len, "its ABI value %zu is %" PRIu32
			                     ", askai's is %" PRIu32, i, m->abi[i], abi[i]);
	if (!plugin_name_len(m->name))
		return plugin_refuse(why, len, "the name in its manifest is not 1 to "
		                     "31 characters of [-0-9_a-z]");
	uint32_t n = 0;
	for (; n < MANIFEST_TOOLS && m->tools[n][0]; ++n) {
		lens[n] = plugin_name_len(m->tools[n]);
		if (!lens[n])
			return plugin_refuse(why, len, "tool name %" PRIu32 " in its "
			                     "manifest is not 1 to 31 characters of "
			                     "[-0-9_a-z]", n);
	}
	for (uint32_t i = n + 1; i < MANIFEST_TOOLS; ++i)
		if (m->tools[i][0])
			return plugin_refuse(why, len, "its manifest has a tool name "
			                     "after an empty entry");
	if (!n)
		return plugin_refuse(why, len, "its manifest names no tool");
	*n_tools = n;
	return 0;
}

/* Loads the shared object at path and stores its askai_plugin in
 * dest->plugin.  dest->n_tools is the number of tools that its manifest
 * names: those kinds of the plugin must have definition() and start(), and
 * the kinds after them must have neither.  Returns 0, or EINVAL with the
 * reason in the len bytes at why. */
static int
plugin_loaded_load (struct plugin_loaded *dest,
                    char const           *path,
                    char                 *why,
                    size_t                len)
{
	/* The handle is not stored, because askai never unloads a plugin. */
	void *object = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	/* dlsym() is not called if dlopen() failed, so dlerror() returns the
	 * message of the one call that failed. */
	dest->plugin = object ? dlsym(object, "askai_plugin") : nullptr;
	if (!dest->plugin) {
		char const *err = dlerror();
		return plugin_refuse(why, len, "cannot load it: %s",
		                     err ? err : "the dynamic linker gave no reason");
	}
	for (uint32_t i = 0; i < PLUGIN_TOOLS; ++i) {
		struct tool_kind const *kind = &dest->plugin->tools[i];
		bool named = i < dest->n_tools;
		if (named != !!kind->definition || named != !!kind->start)
			return plugin_refuse(why, len, "kind %" PRIu32 " of its "
			                     "askai_plugin and its manifest do not agree: "
			                     "a named tool has definition() and start(), "
			                     "another kind has neither", i);
	}
	return 0;
}

/* Fills *dest for the plugin in the file at path: reads and checks the
 * manifest, then loads the shared object.  No code of the plugin runs
 * before its manifest has passed the checks.  Returns 0, or EINVAL with
 * the reason in the len bytes at why. */
static int
plugin_loaded_init (struct plugin_loaded *dest,
                    char const           *path,
                    char                 *why,
                    size_t                len)
{
	/* O_NONBLOCK: opening a FIFO must not wait for a writer. */
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return plugin_refuse(why, len, "cannot open it: %s", strerror(errno));
	int e = plugin_manifest_read(&dest->manifest, fd, why, len);
	(void)close(fd);
	fd = -1;
	if (e)
		return e;
	e = plugin_manifest_check(&dest->manifest, &dest->n_tools, dest->lens,
	                          why, len);
	if (e)
		return e;
	return plugin_loaded_load(dest, path, why, len);
}

struct plugin_loaded *
plugin_loaded_create (char const *path,
                      char       *why,
                      size_t      len,
                      int        *e)
{
	struct plugin_loaded *loaded = calloc(1, sizeof *loaded);
	if (!loaded) {
		*e = ENOMEM;
		return nullptr;
	}
	*e = plugin_loaded_init(loaded, path, why, len);
	if (*e)
		plugin_loaded_destroy(&loaded);
	return loaded;
}

void
plugin_loaded_destroy (struct plugin_loaded **p_loaded)
{
	free(*p_loaded);
	*p_loaded = nullptr;
}
