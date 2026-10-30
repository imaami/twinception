/** @file plugin.c
 *
 * askai's side of a plugin: reading the manifest from the plugin's file,
 * and loading the plugin.
 */
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

#include "jb.h"
#include "plugin.h"
#include "util.h"

/* The ELF header of askai itself.  The linker defines the symbol. */
[[gnu::visibility("hidden")]] extern ElfW(Ehdr) const __ehdr_start;

/* The address and thread sanitizers add constructors to the objects that
 * they instrument.  The undefined-behavior sanitizer adds writable data,
 * unless it is built with -fsanitize-trap.  A plugin that is built with one
 * of these sanitizers therefore has state outside its instances.  askai
 * that is built with -fsanitize=address, -fsanitize=thread or
 * -fsanitize=undefined refuses no plugin file for such state, so that a
 * plugin can be built with the same sanitizer.  It then also loads a file
 * that has thread-local storage.
 */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(undefined_behavior_sanitizer)
static constexpr bool PLUGIN_STATE_REFUSED = false;
#else
static constexpr bool PLUGIN_STATE_REFUSED = true;
#endif

/* What plugin_manifest_read() knows about the plugin's file while it reads
 * the file.
 */
struct plugin_file {
	uint64_t         size;        /* the size of the file in bytes, from
	                                 fstat() */
	uint64_t         write_from;  /* the lowest address of the writable
	                                 PT_LOAD segments */
	uint64_t         write_to;    /* the address after the last byte of the
	                                 writable PT_LOAD segments.  It is 0 if
	                                 the file has no such segment. */
	uint64_t         relro_from;  /* the first address of the PT_GNU_RELRO
	                                 segment */
	uint64_t         relro_to;    /* the address after the last byte of the
	                                 PT_GNU_RELRO segment */
	struct manifest *dest;        /* the manifest is copied to this object */
	char            *why;         /* the reason for a refusal is written to
	                                 this buffer */
	char const      *state;       /* a description of what the file has that
	                                 is state of the plugin outside its
	                                 instances.  It is nullptr if the file
	                                 has nothing of that sort. */
	size_t           len;         /* the size of why in bytes */
	uint32_t         found;       /* the number of askai manifests read so
	                                 far */
	int              fd;
};

/* Writes the reason for a refusal into the len bytes at why.  Returns
 * EINVAL.
 */
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
 * Returns 0, or EINVAL with the reason in f->why.
 */
static int
plugin_file_read (struct plugin_file const *f,
                  uint64_t                  off,
                  uint64_t                  end,
                  void                     *buf,
                  size_t                    n)
{
	if (off > end || n > end - off)
		return plugin_refuse(f->why, f->len, "a truncated or malformed ELF file");
	size_t got;
	int e = util_read_all(f->fd, buf, n, (off_t)off, &got);
	if (e)
		return plugin_refuse(f->why, f->len, "cannot read it: %s", strerror(e));
	if (got < n)
		return plugin_refuse(f->why, f->len, "became shorter while it was read");
	return 0;
}

/* Reads the notes of one PT_NOTE segment: the bytes of the file from offset
 * off to offset end.  In each note, the header and the name together are
 * padded to a multiple of pad + 1 bytes.  The desc is padded to a multiple
 * of pad + 1 bytes too.  Copies the desc of an askai manifest to f->dest
 * and counts it in f->found.  Returns 0, or EINVAL with the reason in
 * f->why.
 */
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

/* Records in f->state what the file has, unless something is recorded
 * there already.
 */
static void
plugin_file_state (struct plugin_file *f,
                   char const         *what)
{
	if (!f->state)
		f->state = what;
}

/* Reads the entries of the PT_DYNAMIC segment: the bytes of the file from
 * offset off to offset end.  Records in f->state an entry for a constructor
 * or a destructor.  Returns 0, or EINVAL with the reason in f->why.
 */
static int
plugin_file_dynamic (struct plugin_file *f,
                     uint64_t            off,
                     uint64_t            end)
{
	for (; off < end; off += sizeof (ElfW(Dyn))) {
		ElfW(Dyn) dyn;
		int e = plugin_file_read(f, off, end, &dyn, sizeof dyn);
		if (e)
			return e;
		switch (dyn.d_tag) {
		case DT_INIT:
		case DT_FINI:
		case DT_INIT_ARRAY:
		case DT_FINI_ARRAY:
		case DT_PREINIT_ARRAY:
			plugin_file_state(f, "has a constructor or a destructor (the C "
			                  "start files add both unless the plugin is "
			                  "linked with -nostartfiles)");
			break;
		default:
			break;
		}
	}
	return 0;
}

/* Looks at one program header of the file.  It reads the notes of a
 * PT_NOTE segment and the entries of the PT_DYNAMIC segment.  It records in
 * *f the addresses of the writable PT_LOAD segments and of the PT_GNU_RELRO
 * segment, and a PT_TLS segment in f->state.  Returns 0, or EINVAL with the
 * reason in f->why.
 */
static int
plugin_file_segment (struct plugin_file *f,
                     ElfW(Phdr) const   *ph)
{
	if (ph->p_offset > f->size || ph->p_filesz > f->size - ph->p_offset ||
	    ph->p_memsz > UINT64_MAX - ph->p_vaddr)
		return plugin_refuse(f->why, f->len, "a truncated or malformed ELF file");
	uint64_t end = ph->p_offset + ph->p_filesz;
	uint64_t top = ph->p_vaddr + ph->p_memsz;
	switch (ph->p_type) {
	case PT_NOTE:
		return plugin_file_notes(f, ph->p_offset, end, ph->p_align == 8 ? 7 : 3);
	case PT_DYNAMIC:
		return plugin_file_dynamic(f, ph->p_offset, end);
	case PT_TLS:
		plugin_file_state(f, "has thread-local storage");
		return 0;
	case PT_GNU_RELRO:
		f->relro_from = ph->p_vaddr;
		f->relro_to = top;
		return 0;
	case PT_LOAD:
		if (!(ph->p_flags & PF_W))
			return 0;
		f->write_from = ph->p_vaddr < f->write_from ? ph->p_vaddr : f->write_from;
		f->write_to = top > f->write_to ? top : f->write_to;
		return 0;
	default:
		return 0;
	}
}

/* Reads the manifest of the plugin whose file is open at fd into *dest.
 * The file must be a regular file.  Its contents must be a shared object
 * for the machine that askai runs on, and its PT_NOTE segments must contain
 * exactly one note with the owner "askai" and the type MANIFEST_TYPE.
 *
 * A plugin has its state in instances (plugin.h).  So the file must have no
 * thread-local storage, no constructor or destructor, and no writable data
 * that is still writable when the plugin's code runs.
 *
 * Returns 0, or EINVAL with the reason in the len bytes at why.
 */
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
		.size       = st.st_size > 0 ? (uint64_t)st.st_size : 0,
		.write_from = UINT64_MAX,
		.dest       = dest,
		.why        = why,
		.len        = len,
		.fd         = fd,
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
		e = plugin_file_segment(&f, &ph);
		if (e)
			return e;
	}
	if (!f.found)
		return plugin_refuse(why, len, "has no askai manifest");
	/* Before any code of the plugin runs, the dynamic linker makes the
	 * PT_GNU_RELRO segment read-only.  It rounds both ends of the segment
	 * down to a page, so a writable byte outside that range stays
	 * writable. */
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0)
		return plugin_refuse(why, len, "cannot check it: the page size is unknown");
	uint64_t mask = ~((uint64_t)page - 1);
	if (f.write_to && (f.write_from < (f.relro_from & mask) ||
	                   f.write_to > (f.relro_to & mask)))
		plugin_file_state(&f, "has writable data (a plugin has no mutable "
		                  "static and is linked with -Wl,-z,relro,-z,now)");
	if (PLUGIN_STATE_REFUSED && f.state)
		return plugin_refuse(why, len, "%s", f.state);
	return 0;
}

/* A name is 1 to 31 characters of [-0-9_a-z] followed by a NUL.  Returns
 * the length of the name in the MANIFEST_NAME bytes at name, or 0 if those
 * bytes are not a name.
 */
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
 * len bytes at why.
 */
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

/* Calls definition() of the kind of the manifest's tool i, and checks that
 * the definition is an object whose member "name" is the tool's name.
 * Returns 0, ENOMEM if definition() returned nullptr, or EINVAL with the
 * reason in the len bytes at why.
 */
static int
plugin_loaded_definition (struct plugin_loaded const *loaded,
                          uint32_t                    i,
                          char                       *why,
                          size_t                      len)
{
	struct json_object *def = loaded->plugin->tools[i].definition(loaded->config);
	if (!def)
		return ENOMEM;
	size_t n = 0;
	char const *name = jb_str(jb_get(def, "name"), &n);
	int e = name && n == loaded->lens[i] && !memcmp(name, loaded->manifest.tools[i], n)
	      ? 0
	      : plugin_refuse(why, len, "the definition of its tool %s has another "
	                      "name", loaded->manifest.tools[i]);
	json_object_put(def);
	return e;
}

/* Loads the shared object at path and stores its askai_plugin in
 * dest->plugin.  dest->n_tools is the number of tools that its manifest
 * names: those kinds of the plugin must have definition() and start(), and
 * the kinds after them must have neither.  The flags of every kind must be
 * among TOOL_FLAGS.  Returns 0, ENOMEM, or EINVAL with the reason in the
 * len bytes at why.
 */
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
	struct plugin const *p = dest->plugin;
	if (p->instance_init ? p->instance_size - 1 >= PLUGIN_INSTANCE_MAX
	                     : p->instance_size || p->instance_fini || p->relieve)
		return plugin_refuse(why, len, "the instance members of its askai_plugin "
		                     "do not agree: instance_init() goes with an "
		                     "instance_size of 1 to %zu, and instance_fini() and "
		                     "relieve() need instance_init()", PLUGIN_INSTANCE_MAX);
	for (uint32_t i = 0; i < PLUGIN_TOOLS; ++i) {
		struct tool_kind const *kind = &dest->plugin->tools[i];
		bool named = i < dest->n_tools;
		if (named != !!kind->definition || named != !!kind->start)
			return plugin_refuse(why, len, "kind %" PRIu32 " of its "
			                     "askai_plugin and its manifest do not agree: "
			                     "a named tool has definition() and start(), "
			                     "another kind has neither", i);
		if (kind->flags & ~TOOL_FLAGS)
			return plugin_refuse(why, len, "kind %" PRIu32 " of its "
			                     "askai_plugin has flags that tool.h does not "
			                     "define for plugins: %#" PRIx64, i,
			                     kind->flags & ~TOOL_FLAGS);
	}
	return 0;
}

/* Checks configure and config_size of dest->plugin, and stores the
 * plugin's configuration in dest->config: config_size zeroed bytes that
 * configure() fills from options.  options is the member "options" of the
 * plugin's element of "plugins", or nullptr.  A plugin without configure()
 * gets no configuration, and it is refused if options is not nullptr.  The
 * reason of configure() follows "options: " in why.  Returns 0, ENOMEM, or
 * EINVAL with the reason in the len bytes at why.
 */
static int
plugin_loaded_configure (struct plugin_loaded *dest,
                         struct json_object   *options,
                         char                 *why,
                         size_t                len)
{
	static char const prefix[] = "options: ";
	struct plugin const *p = dest->plugin;
	if (p->configure ? p->config_size - 1 >= PLUGIN_CONFIG_MAX : p->config_size)
		return plugin_refuse(why, len, "its askai_plugin has configure() without a "
		                     "config_size of 1 to %zu, or a config_size without "
		                     "configure()", PLUGIN_CONFIG_MAX);
	if (!p->configure)
		return options ? plugin_refuse(why, len, "%sthe plugin has no options",
		                               prefix)
		               : 0;
	dest->config = calloc(1, p->config_size);
	if (!dest->config)
		return ENOMEM;
	/* A buffer without room for a reason after the prefix gets the prefix
	 * alone. */
	if (len <= sizeof prefix)
		return plugin_refuse(why, len, "%s", prefix);
	/* The reason of configure() follows the prefix.  The text is cut at the
	 * end of the buffer, also if configure() stored no NUL. */
	char *reason = why + sizeof prefix - 1;
	size_t room = len - (sizeof prefix - 1);
	memcpy(why, prefix, sizeof prefix - 1);
	*reason = '\0';
	int e = p->configure(dest->config, options, reason, room);
	reason[room - 1] = '\0';
	if (!e || e == ENOMEM)
		return e;
	if (e != EINVAL)
		return plugin_refuse(why, len, "%sconfigure() returned the error "
		                     "number %d", prefix, e);
	if (!*reason)
		return plugin_refuse(why, len, "%sconfigure() refused the options "
		                     "without a reason", prefix);
	return EINVAL;
}

/* Fills *dest for the plugin in the file at path: reads and checks the
 * manifest, then loads the shared object, then stores the plugin's
 * configuration from options, and then checks that the definition of each
 * named tool has the tool's name.  definition() gets the configuration.  No
 * code of the plugin runs before its manifest has passed the checks.
 * Returns 0, ENOMEM, or EINVAL with the reason in the len bytes at why.
 */
static int
plugin_loaded_init (struct plugin_loaded *dest,
                    char const           *path,
                    struct json_object   *options,
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
	e = plugin_loaded_load(dest, path, why, len);
	if (!e)
		e = plugin_loaded_configure(dest, options, why, len);
	for (uint32_t i = 0; !e && i < dest->n_tools; ++i)
		e = plugin_loaded_definition(dest, i, why, len);
	return e;
}

struct plugin_loaded *
plugin_loaded_create (char const         *path,
                      struct json_object *options,
                      char               *why,
                      size_t              len,
                      int                *e)
{
	if (!e)
		return nullptr;
	if (!path || !why || !len) {
		*e = EFAULT;
		return nullptr;
	}
	size_t size = strlen(path) + 1;
	struct plugin_loaded *loaded = calloc(1, sizeof *loaded + size);
	if (!loaded) {
		*e = ENOMEM;
		return nullptr;
	}
	memcpy(loaded->path, path, size);
	loaded->options = options != nullptr;
	*e = plugin_loaded_init(loaded, path, options, why, len);
	if (*e)
		plugin_loaded_destroy(&loaded);
	return loaded;
}

void
plugin_loaded_destroy (struct plugin_loaded **p_loaded)
{
	if (*p_loaded) {
		free((*p_loaded)->config);
		(*p_loaded)->config = nullptr;
	}
	free(*p_loaded);
	*p_loaded = nullptr;
}
