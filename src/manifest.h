/** @file manifest.h
 *
 * A plugin's manifest: an ELF note, owner "askai", in a PT_NOTE
 * segment of the plugin's file, which askai reads before it loads
 * the plugin (src/plugin.h). Bytes and 32-bit words: the note's
 * alignment of 4 holds no wider member, and a pointer would need
 * a relocation.
 */
#ifndef TWINCEPTION_SRC_MANIFEST_H_
#define TWINCEPTION_SRC_MANIFEST_H_

#include <stddef.h>
#include <stdint.h>

/** The note type of a manifest; a note's owner gives its types meaning. */
static constexpr uint32_t MANIFEST_TYPE = 0x61736b01; // `{1,'k','s','a'}`

enum {
	MANIFEST_ABI   = 32, //!< Length of the @ref manifest::abi array
	MANIFEST_NAME  = 32, //!< Size of each name field in @ref manifest
	MANIFEST_NEEDS =  4, //!< Max item count of @ref manifest::needs
	MANIFEST_TOOLS =  8, //!< Max item count of @ref manifest::tools
};

/**
 * What a plugin says of itself.  A name is 1 to 31 of [-0-9_a-z], then
 * NULs; an entry of needs or tools is a name or all NULs, the names
 * first, each once, and none of needs the plugin's own.
 */
struct manifest {
	uint32_t epoch;               //!< PLUGIN_EPOCH
	uint32_t abi[MANIFEST_ABI];   //!< PLUGIN_ABI() followed by zeros
	char     name[MANIFEST_NAME]; //!< Plugin name followed by NULs

	/** Names of other plugins this plugin depends on; can be empty */
	char needs[MANIFEST_NEEDS][MANIFEST_NAME];

	/** Names of tools this plugin provides; must have at least one */
	char tools[MANIFEST_TOOLS][MANIFEST_NAME];
};

/** A whole note, as the plugin's file holds it. */
struct manifest_note {
	uint32_t        namesz;
	uint32_t        descsz;
	uint32_t        type;
	char            owner[8]; //!< `{'a', 's', 'k', 'a', 'i', 0, 0, 0}`
	struct manifest desc;
};

static_assert(sizeof (struct manifest) == 548);
static_assert(alignof (struct manifest_note) == 4);
static_assert(offsetof (struct manifest_note, desc) == 20);

/* clang's ASan would pad the note and align it to 32: no note then. */
#if defined __has_c_attribute && __has_c_attribute(clang::no_sanitize)
# define MANIFEST_NO_ASAN [[clang::no_sanitize("address")]]
#else
# define MANIFEST_NO_ASAN
#endif

#endif /* TWINCEPTION_SRC_MANIFEST_H_ */
