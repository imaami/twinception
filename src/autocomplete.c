/** @file autocomplete.c
 * Offline completion with connection-lifetime working storage.
 */
#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "autocomplete.h"
#include "cli_command.h"
#include "compat.h"
#include "dstr.h"
#include "jb.h"
#include "notes.h"
#include "util.h"

/* The most tools and the most lists of note paths of a catalog. */
enum {
	AUTOCOMPLETE_TOOL_COUNT = 4096,
	AUTOCOMPLETE_NOTE_LISTS = 4096,
};

enum autocomplete_flag : uint64_t {
	AUTOCOMPLETE_STRING       = UINT64_C(1) <<  0,
	AUTOCOMPLETE_QUOTED       = UINT64_C(1) <<  1,
	AUTOCOMPLETE_VALID        = UINT64_C(1) <<  2,
	AUTOCOMPLETE_STRUCTURED   = UINT64_C(1) <<  3,
	AUTOCOMPLETE_USE_STRING   = UINT64_C(1) <<  4,
	AUTOCOMPLETE_TYPED        = UINT64_C(1) <<  5,
	AUTOCOMPLETE_UNION        = UINT64_C(1) <<  6,
	AUTOCOMPLETE_CONSTANT     = UINT64_C(1) <<  7,
	AUTOCOMPLETE_ENUMERATED   = UINT64_C(1) <<  8,
	AUTOCOMPLETE_NAMES        = UINT64_C(1) <<  9,
	/* The flag of each JSON type is the flag of null shifted by the type. */
	AUTOCOMPLETE_TYPE_NULL    = UINT64_C(1) << 10,
	AUTOCOMPLETE_TYPE_BOOLEAN = AUTOCOMPLETE_TYPE_NULL << json_type_boolean,
	AUTOCOMPLETE_TYPE_NUMBER  = AUTOCOMPLETE_TYPE_NULL << json_type_double,
	AUTOCOMPLETE_TYPE_INTEGER = AUTOCOMPLETE_TYPE_NULL << json_type_int,
	AUTOCOMPLETE_TYPE_OBJECT  = AUTOCOMPLETE_TYPE_NULL << json_type_object,
	AUTOCOMPLETE_TYPE_ARRAY   = AUTOCOMPLETE_TYPE_NULL << json_type_array,
	AUTOCOMPLETE_TYPE_STRING  = AUTOCOMPLETE_TYPE_NULL << json_type_string,
};

struct autocomplete_candidate {
	struct dstr value;
	uint64_t    flags;
};

struct autocomplete_property {
	struct cstr_view               name;
	struct autocomplete_candidate *values;
	size_t                         count;
	uint64_t                       flags;
};

struct autocomplete_entry {
	struct autocomplete_tool      tool;
	struct json_object           *definition;
	struct json_object           *root;
	struct autocomplete_property *properties;
	size_t                        count;
	size_t                        storage;
};

struct autocomplete_names {
	UTIL_ARRAY(struct dstr);
};

struct autocomplete_notebook {
	struct dstr               tool;
	struct dstr               selector;
	struct autocomplete_names paths;
	size_t                    storage;
};

struct autocomplete_token {
	size_t begin;
	size_t end;
	size_t equals;
};

struct autocomplete {
	UTIL_ARRAY(struct autocomplete_entry) entries;
	size_t                        storage;
	struct autocomplete_names     lists[AUTOCOMPLETE_LISTS];
	UTIL_ARRAY(struct autocomplete_notebook) notes;
	size_t                        notes_storage;
	UTIL_ARRAY(struct autocomplete_token) tokens;
	struct json_tokener          *parser;
	struct dstr                   nesting;
	struct dstr                   key;
	struct dstr                   decoded;
	struct dstr                   quoted;
	struct autocomplete_matches   found;
};

/* Returns a view of the string value, or a view of "" if value is not a
 * string, so that data is always a C string.
 */
static struct cstr_view
autocomplete_json_text (struct json_object *value)
{
	struct cstr_view text = jb_view(value);
	return text.data ? text : CSTR_VIEW("");
}

static bool
autocomplete_name_byte (unsigned char byte)
{
	return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
	       (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte >= 0x80;
}

bool
autocomplete_safe (struct cstr_view value,
                   bool             identifier)
{
	if (!value.len || !value.data || jb_utf8(value.data, value.len) != value.len)
		return false;
	for (size_t i = 0; i < value.len; ++i) {
		unsigned char byte = (unsigned char)value.data[i];
		if (byte < 0x20 || byte == 0x7f ||
		    (byte == 0xc2 && i + 1 < value.len && (unsigned char)value.data[i + 1] < 0xa0) ||
		    (identifier && (cstr_view_space(byte) || byte == '=' || byte == '\'' || byte == '"' || byte == '\\')))
			return false;
	}
	return true;
}

/* What a byte of an argument is: quoting, text in quotes or after a
 * backslash, or plain text. Only plain text can end a token or nest.
 */
enum autocomplete_byte {
	AUTOCOMPLETE_BYTE_QUOTE,
	AUTOCOMPLETE_BYTE_TEXT,
	AUTOCOMPLETE_BYTE_PLAIN,
};

/* Move the quoting state, the open quote *mark and *escape, past byte.
 * Returns what byte is.
 */
static enum autocomplete_byte
autocomplete_quote_step (char  byte,
                         char *mark,
                         bool *escape)
{
	if (*escape) {
		*escape = false;
		return AUTOCOMPLETE_BYTE_TEXT;
	}
	if (byte == '\\' && *mark != '\'') {
		*escape = true;
		return AUTOCOMPLETE_BYTE_QUOTE;
	}
	if (*mark) {
		*mark = byte == *mark ? 0 : *mark;
		return *mark ? AUTOCOMPLETE_BYTE_TEXT : AUTOCOMPLETE_BYTE_QUOTE;
	}
	if (byte != '\'' && byte != '"')
		return AUTOCOMPLETE_BYTE_PLAIN;
	*mark = byte;
	return AUTOCOMPLETE_BYTE_QUOTE;
}

static int
autocomplete_unquote (struct dstr     *output,
                      struct cstr_view value)
{
	(void)dstr_clear(output);
	/* The appends below fit in the room that this reserves. */
	if (!dstr_reserve(output, value.len))
		return ENOMEM;
	char mark = 0;
	bool escape = false;
	for (size_t i = 0; i < value.len; ++i)
		if (autocomplete_quote_step(value.data[i], &mark, &escape) != AUTOCOMPLETE_BYTE_QUOTE)
			(void)dstr_cat_n(output, &value.data[i], 1);
	return 0;
}

/* Returns the bytes that are written before byte within the quotes of
 * mark. Within single quotes, a single quote is written as '\'', which
 * ends the quotes, writes an escaped quote and starts the quotes again.
 * Within double quotes, a double quote and a backslash are written after a
 * backslash.
 */
static struct cstr_view
autocomplete_escape (char mark,
                     char byte)
{
	return mark == '\'' && byte == '\'' ? CSTR_VIEW("'\\'")
	     : mark == '"' && (byte == '"' || byte == '\\') ? CSTR_VIEW("\\") : (struct cstr_view){};
}

static int
autocomplete_quote (struct dstr     *output,
                    struct cstr_view value,
                    char             preferred)
{
	bool needs = preferred || !value.len;
	char mark = preferred == '"' ? '"' : '\'';
	if (value.len > SIZE_MAX - 2)
		return ENOMEM;
	size_t length = value.len + 2;
	for (size_t i = 0; i < value.len; ++i) {
		char byte = value.data[i];
		needs |= cstr_view_space(byte) || byte == '\\' || byte == '\'' || byte == '"' ||
		         byte == '[' || byte == ']' || byte == '{' || byte == '}';
		size_t extra = autocomplete_escape(mark, byte).len;
		if (extra > SIZE_MAX - length)
			return ENOMEM;
		length += extra;
	}
	if (length > AUTOCOMPLETE_BYTES)
		return E2BIG;
	if (!needs)
		return cstr_view_store(value, output);
	(void)dstr_clear(output);
	if (!dstr_cat_n(output, &mark, 1))
		return ENOMEM;
	for (size_t i = 0; i < value.len; ++i)
		if (cstr_view_cat(autocomplete_escape(mark, value.data[i]), output) ||
		    !dstr_cat_n(output, &value.data[i], 1))
			return ENOMEM;
	return dstr_cat_n(output, &mark, 1) ? 0 : ENOMEM;
}

/* Track the brackets and braces of a JSON value in ac->nesting. An opening
 * byte is appended to it, and a closing byte removes the opening byte at
 * its end. Returns 0, ENOMEM, or when strict, EINVAL for a closing byte
 * that does not match.
 */
static int
autocomplete_nest (struct autocomplete *ac,
                   char                 byte,
                   bool                 strict,
                   char                *why,
                   size_t               size)
{
	if (byte == '[' || byte == '{')
		return dstr_cat_n(&ac->nesting, &byte, 1) ? 0 : ENOMEM;
	char open = byte == ']' ? '[' : byte == '}' ? '{' : 0;
	size_t depth = dstr_len(&ac->nesting);
	if (!open)
		return 0;
	if (!depth || dstr_get(&ac->nesting)[depth - 1] != open)
		return strict ? util_refuse(why, size, "Unmatched bracket in tool arguments") : 0;
	(void)dstr_erase(&ac->nesting, depth - 1, 1);
	return 0;
}

/* Set token->end to the offset of the first plain white space outside a
 * JSON value after token->begin in line, or to the end of the line. Set
 * token->equals to the offset of the first '=' of the token. Returns 0 or
 * an error number.
 */
static int
autocomplete_token_end (struct autocomplete       *ac,
                        struct cstr_view           line,
                        struct autocomplete_token *token,
                        bool                       strict,
                        char                      *why,
                        size_t                     size)
{
	char mark = 0;
	bool escape = false;
	(void)dstr_clear(&ac->nesting);
	for (token->end = token->begin; token->end < line.len; ++token->end) {
		char byte = line.data[token->end];
		if (token->equals == SIZE_MAX && byte == '=')
			token->equals = token->end;
		if (autocomplete_quote_step(byte, &mark, &escape) != AUTOCOMPLETE_BYTE_PLAIN)
			continue;
		if (cstr_view_space(byte) && !dstr_len(&ac->nesting))
			break;
		int e = autocomplete_nest(ac, byte, strict, why, size);
		if (e)
			return e;
	}
	if (strict && (mark || escape || dstr_len(&ac->nesting)))
		return util_refuse(why, size, "Unterminated quote, escape or JSON value in tool arguments");
	return 0;
}

static int
autocomplete_tokenize (struct autocomplete *ac,
                       struct cstr_view     line,
                       bool                 strict,
                       char                *why,
                       size_t               size)
{
	ac->tokens.n = 0;
	for (size_t i = 0; i < line.len;) {
		if (cstr_view_space(line.data[i])) {
			++i;
			continue;
		}
		struct autocomplete_token token = {.begin = i, .equals = SIZE_MAX};
		int e = autocomplete_token_end(ac, line, &token, strict, why, size);
		if (!e && util_grow(ac->tokens.items, ac->tokens.cap, ac->tokens.n + 1))
			e = ENOMEM;
		if (e)
			return e;
		ac->tokens.items[ac->tokens.n++] = token;
		i = token.end;
	}
	return 0;
}

static int
autocomplete_json_parse (struct autocomplete *autocomplete,
                         struct cstr_view     value,
                         struct json_object **result)
{
	*result = nullptr;
	if (value.len >= INT_MAX)
		return E2BIG;
	if (!autocomplete->parser) {
		autocomplete->parser = json_tokener_new();
		if (!autocomplete->parser)
			return ENOMEM;
		json_tokener_set_flags(autocomplete->parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	} else
		json_tokener_reset(autocomplete->parser);
	struct json_tokener *parser = autocomplete->parser;
	struct json_object *parsed = json_tokener_parse_ex(parser, value.data, (int)value.len);
	size_t end = json_tokener_get_parse_end(parser);
	/* A number or a literal at the end of the value ends only at a NUL. */
	if (json_tokener_get_error(parser) == json_tokener_continue) {
		parsed = json_tokener_parse_ex(parser, "", 1);
		end = value.len;
	}
	while (end < value.len && cstr_view_space(value.data[end]))
		++end;
	if (json_tokener_get_error(parser) != json_tokener_success || end < value.len) {
		json_object_put(parsed);
		parsed = nullptr;
		return EINVAL;
	}
	*result = parsed;
	return 0;
}

/* Decode a segment of a JSON pointer into ac->key, where ~0 is ~ and ~1
 * is /. Returns 0 or ENOMEM.
 */
static int
autocomplete_segment (struct autocomplete *ac,
                      struct cstr_view     segment)
{
	(void)dstr_clear(&ac->key);
	/* The appends below fit in the room that this reserves. */
	if (!dstr_reserve(&ac->key, segment.len))
		return ENOMEM;
	for (size_t i = 0; i < segment.len; ++i) {
		char byte = segment.data[i];
		bool tilde = byte == '~' && i + 1 < segment.len &&
		             (segment.data[i + 1] == '0' || segment.data[i + 1] == '1');
		if (tilde)
			byte = segment.data[++i] == '0' ? '~' : '/';
		(void)dstr_cat_n(&ac->key, &byte, 1);
	}
	return 0;
}

/* Set *target to the value at the JSON pointer path in root, or to nullptr
 * if there is none. path is the part of the pointer after "#/". Returns 0
 * or ENOMEM.
 */
static int
autocomplete_pointer (struct autocomplete  *ac,
                      struct json_object   *root,
                      struct cstr_view      path,
                      struct json_object  **target)
{
	*target = root;
	for (bool last = false; *target && !last;) {
		struct cstr_view segment = path;
		last = !cstr_view_cut(&path, '/', &segment);
		int error = autocomplete_segment(ac, segment);
		if (error)
			return error;
		*target = jb_get(*target, dstr_get(&ac->key));
	}
	return 0;
}

static int
autocomplete_resolve (struct autocomplete  *autocomplete,
                      struct json_object   *root,
                      struct json_object  **schema)
{
	for (unsigned depth = 0; *schema && depth < 16; ++depth) {
		struct cstr_view ref = autocomplete_json_text(jb_get(*schema, "$ref"));
		if (!cstr_view_starts(ref, CSTR_VIEW("#/")))
			break;
		struct json_object *next;
		int error = autocomplete_pointer(autocomplete, root, cstr_view_slice(ref, 2, SIZE_MAX), &next);
		if (error)
			return error;
		if (!next || next == *schema)
			break;
		*schema = next;
	}
	return 0;
}

static uint64_t
autocomplete_type_flag (struct cstr_view type)
{
	/* The names of the types in a JSON schema, by enum json_type. */
	static struct cstr_view const names[] = {
		[json_type_null]    = CSTR_VIEW_INIT("null"),
		[json_type_boolean] = CSTR_VIEW_INIT("boolean"),
		[json_type_double]  = CSTR_VIEW_INIT("number"),
		[json_type_int]     = CSTR_VIEW_INIT("integer"),
		[json_type_object]  = CSTR_VIEW_INIT("object"),
		[json_type_array]   = CSTR_VIEW_INIT("array"),
		[json_type_string]  = CSTR_VIEW_INIT("string"),
	};
	size_t i = cstr_view_index(names, array_size(names), type);
	return i < array_size(names) ? AUTOCOMPLETE_TYPE_NULL << i : 0;
}

static uint64_t
autocomplete_type_flags (struct json_object *type)
{
	if (!type)
		return 0;
	if (!json_object_is_type(type, json_type_array))
		return AUTOCOMPLETE_TYPED | autocomplete_type_flag(autocomplete_json_text(type));
	uint64_t flags = AUTOCOMPLETE_TYPED | AUTOCOMPLETE_UNION;
	for (size_t i = 0, count = json_object_array_length(type); i < count; ++i)
		flags |= autocomplete_type_flag(autocomplete_json_text(json_object_array_get_idx(type, i)));
	return flags;
}

static bool
autocomplete_accepted_type (uint64_t            flags,
                            struct json_object *value)
{
	/* An integer is a number too, and a whole number is an integer. */
	enum json_type type = json_object_get_type(value);
	if (!(flags & AUTOCOMPLETE_TYPED) || (flags & (AUTOCOMPLETE_TYPE_NULL << type)) ||
	    (type == json_type_int && (flags & AUTOCOMPLETE_TYPE_NUMBER)))
		return true;
	if (type != json_type_double || !(flags & AUTOCOMPLETE_TYPE_INTEGER))
		return false;
	double number = json_object_get_double(value);
	return isfinite(number) && floor(number) == number;
}

static struct autocomplete_entry const *
autocomplete_entry_find (struct autocomplete const *ac,
                         struct cstr_view           name,
                         size_t                    *position)
{
	size_t begin = 0;
	size_t end = ac->entries.n;
	while (begin < end) {
		size_t middle = begin + (end - begin) / 2;
		if (cstr_view_compare(ac->entries.items[middle].tool.name, name) < 0)
			begin = middle + 1;
		else
			end = middle;
	}
	if (position)
		*position = begin;
	return begin < ac->entries.n && cstr_view_equal(ac->entries.items[begin].tool.name, name)
	       ? &ac->entries.items[begin] : nullptr;
}

static struct autocomplete_property const *
autocomplete_entry_property (struct autocomplete_entry const *entry,
                             struct cstr_view                 name)
{
	for (size_t i = 0; i < entry->count; ++i)
		if (cstr_view_equal(entry->properties[i].name, name))
			return &entry->properties[i];
	return nullptr;
}

static void
autocomplete_entry_fini (struct autocomplete_entry *entry)
{
	for (size_t i = 0; i < entry->count; ++i) {
		struct autocomplete_property *property = &entry->properties[i];
		for (size_t j = 0; j < property->count; ++j)
			dstr_cleanup(&property->values[j].value);
		free(property->values);
		property->values = nullptr;
	}
	free(entry->properties);
	entry->properties = nullptr;
	json_object_put(entry->definition);
	entry->definition = nullptr;
	*entry = (struct autocomplete_entry){};
}

static int
autocomplete_candidate_init (struct autocomplete_candidate *candidate,
                             struct json_object            *value,
                             uint64_t                       flags)
{
	*candidate = (struct autocomplete_candidate){.value = dstr(nullptr)};
	if (json_object_is_type(value, json_type_string)) {
		candidate->flags = AUTOCOMPLETE_STRING;
		if ((flags & (AUTOCOMPLETE_UNION | AUTOCOMPLETE_STRING)) != AUTOCOMPLETE_STRING)
			candidate->flags |= AUTOCOMPLETE_QUOTED;
	}
	struct cstr_view text = autocomplete_json_text(value);
	if (!(candidate->flags & AUTOCOMPLETE_STRING)) {
		text.data = json_object_to_json_string_length(value, JSON_C_TO_STRING_PLAIN, &text.len);
		if (!text.data)
			return ENOMEM;
	}
	if (text.len && !autocomplete_safe(text, false))
		return 0;
	candidate->flags |= AUTOCOMPLETE_VALID;
	return cstr_view_store(text, &candidate->value);
}

static int
autocomplete_property_init (struct autocomplete_property *property,
                            struct autocomplete          *ac,
                            struct json_object           *root,
                            struct json_object           *schema,
                            struct cstr_view              name)
{
	*property = (struct autocomplete_property){.name = name};
	int error = autocomplete_resolve(ac, root, &schema);
	if (error)
		return error;
	struct json_object *constant = nullptr;
	uint64_t flags = autocomplete_type_flags(jb_get(schema, "type"));
	if (schema && json_object_object_get_ex(schema, "const", &constant))
		flags |= AUTOCOMPLETE_CONSTANT;
	struct json_object *enumeration = jb_get(schema, "enum");
	size_t count = 0;
	if (json_object_is_type(enumeration, json_type_array)) {
		flags |= AUTOCOMPLETE_ENUMERATED;
		count = json_object_array_length(enumeration);
	}
	/* Without a type, a constant string or an enumeration of strings is a
	 * string. */
	bool strings = count;
	for (size_t i = 0; strings && i < count; ++i)
		strings = json_object_is_type(json_object_array_get_idx(enumeration, i), json_type_string);
	if ((flags & AUTOCOMPLETE_TYPE_STRING) ||
	    (!(flags & AUTOCOMPLETE_TYPED) && (json_object_is_type(constant, json_type_string) || strings)))
		flags |= AUTOCOMPLETE_STRING;
	if (schema && !enumeration && !(flags & AUTOCOMPLETE_CONSTANT) &&
	    ((flags & (AUTOCOMPLETE_TYPED | AUTOCOMPLETE_TYPE_STRING)) != AUTOCOMPLETE_TYPED))
		flags |= AUTOCOMPLETE_NAMES;
	property->flags = flags;
	if (flags & AUTOCOMPLETE_CONSTANT)
		count = 1;
	if (!count)
		return 0;
	if (count > SIZE_MAX / sizeof *property->values)
		return ENOMEM;
	property->values = malloc(count * sizeof *property->values);
	if (!property->values)
		return ENOMEM;
	for (size_t i = 0; i < count; ++i) {
		struct json_object *value = flags & AUTOCOMPLETE_CONSTANT ? constant : json_object_array_get_idx(enumeration, i);
		error = autocomplete_candidate_init(&property->values[i], value, flags);
		++property->count;
		if (error)
			return error;
	}
	return 0;
}

struct autocomplete *
autocomplete_create (int *e)
{
	struct autocomplete *ac = malloc(sizeof *ac);
	if (!ac) {
		*e = ENOMEM;
		return nullptr;
	}
	*ac = (struct autocomplete){
		.nesting = dstr(nullptr), .key = dstr(nullptr), .decoded = dstr(nullptr),
		.quoted = dstr(nullptr), .found.lines = dstr(nullptr),
	};
	*e = 0;
	return ac;
}

void
autocomplete_clear_tools (struct autocomplete *ac)
{
	for (size_t i = 0; i < ac->entries.n; ++i)
		autocomplete_entry_fini(&ac->entries.items[i]);
	ac->entries.n = 0;
	ac->storage = 0;
	autocomplete_clear_note_files(ac);
}

static void
autocomplete_names_fini (struct autocomplete_names *names)
{
	for (size_t i = 0; i < names->cap; ++i)
		dstr_cleanup(&names->items[i]);
	free(names->items);
	names->items = nullptr;
	names->n = 0;
	names->cap = 0;
}

void
autocomplete_destroy (struct autocomplete **pp)
{
	if (!pp || !*pp)
		return;
	struct autocomplete *ac = *pp;
	autocomplete_clear_tools(ac);
	free(ac->entries.items);
	ac->entries.items = nullptr;
	free(ac->tokens.items);
	ac->tokens.items = nullptr;
	for (size_t i = 0; i < AUTOCOMPLETE_LISTS; ++i)
		autocomplete_names_fini(&ac->lists[i]);
	for (size_t i = 0; i < ac->notes.cap; ++i) {
		dstr_cleanup(&ac->notes.items[i].tool);
		dstr_cleanup(&ac->notes.items[i].selector);
		autocomplete_names_fini(&ac->notes.items[i].paths);
	}
	free(ac->notes.items);
	ac->notes.items = nullptr;
	if (ac->parser)
		json_tokener_free(ac->parser);
	ac->parser = nullptr;
	dstr_cleanup(&ac->nesting);
	dstr_cleanup(&ac->key);
	dstr_cleanup(&ac->decoded);
	dstr_cleanup(&ac->quoted);
	dstr_cleanup(&ac->found.lines);
	free(*pp);
	*pp = nullptr;
}

/* Add the properties with safe names in properties, an object, to entry.
 * Returns 0 or an error number, with the properties added so far in entry.
 */
static int
autocomplete_entry_properties (struct autocomplete       *ac,
                               struct autocomplete_entry *entry,
                               struct json_object        *properties)
{
	size_t count = jb_members(properties);
	if (!count)
		return 0;
	entry->properties = malloc(count * sizeof *entry->properties);
	if (!entry->properties)
		return ENOMEM;
	struct json_object_iter iterator;
	json_object_object_foreachC(properties, iterator) {
		/* json-c does not expose key lengths. Measure once at discovery. */
		struct cstr_view name = cstr_view(iterator.key);
		if (!autocomplete_safe(name, true))
			continue;
		int error = autocomplete_property_init(&entry->properties[entry->count++], ac, entry->tool.schema,
		                                       iterator.val, name);
		if (error)
			return error;
	}
	return 0;
}

int
autocomplete_add_tool (struct autocomplete *ac,
                       struct json_object  *def)
{
	struct cstr_view name = autocomplete_json_text(jb_get(def, "name"));
	if (!autocomplete_safe(name, true))
		return 0;
	size_t position;
	struct autocomplete_entry const *previous = autocomplete_entry_find(ac, name, &position);
	bool replace = previous != nullptr;
	struct autocomplete_entry entry = {
		.tool = {name, autocomplete_json_text(jb_get(def, "title")),
		         autocomplete_json_text(jb_get(def, "description")), jb_get(def, "inputSchema")},
		.definition = json_object_get(def),
		.root = jb_get(def, "inputSchema"),
	};
	size_t schema_size = 0;
	int error = 0;
	if (entry.root && !json_object_to_json_string_length(entry.root, JSON_C_TO_STRING_PLAIN, &schema_size)) {
		error = ENOMEM;
		goto fail;
	}
	entry.storage = name.len + entry.tool.title.len + entry.tool.description.len + schema_size;
	size_t remaining = ac->storage - (previous ? previous->storage : 0);
	if ((!replace && ac->entries.n >= AUTOCOMPLETE_TOOL_COUNT) || entry.storage > AUTOCOMPLETE_BYTES || remaining > AUTOCOMPLETE_BYTES - entry.storage) {
		error = E2BIG;
		goto fail;
	}
	error = autocomplete_resolve(ac, entry.root, &entry.root);
	if (!error)
		error = autocomplete_entry_properties(ac, &entry, jb_get(entry.root, "properties"));
	if (error)
		goto fail;
	/* A new entry in place of an old one needs no room, so the splice
	 * does not fail then. */
	if (replace)
		autocomplete_entry_fini(&ac->entries.items[position]);
	error = util_splice(ac->entries.items, ac->entries.n, ac->entries.cap, position, replace, 1);
	if (error)
		goto fail;
	ac->entries.items[position] = entry;
	ac->storage = remaining + entry.storage;
	return 0;
fail:
	autocomplete_entry_fini(&entry);
	return error;
}

size_t
autocomplete_tool_count (struct autocomplete const *ac)
{
	return ac->entries.n;
}

struct autocomplete_tool const *
autocomplete_tool_at (struct autocomplete const *ac,
                      size_t                     index)
{
	return index < ac->entries.n ? &ac->entries.items[index].tool : nullptr;
}

struct autocomplete_tool const *
autocomplete_find_tool (struct autocomplete const *ac,
                        struct cstr_view           name)
{
	struct autocomplete_entry const *entry = autocomplete_entry_find(ac, name, nullptr);
	return entry ? &entry->tool : nullptr;
}

static int
autocomplete_names_compare (void const *left,
                            void const *right)
{
	return cstr_view_compare(cstr_view_dstr(left), cstr_view_dstr(right));
}

static int
autocomplete_names_set (struct autocomplete_names *names,
                        struct cstr_view const    *values,
                        size_t                     count,
                        bool                       identifier)
{
	if (count && !values)
		return EINVAL;
	if (count > names->cap) {
		size_t previous = names->cap;
		if (util_grow(names->items, names->cap, count))
			return ENOMEM;
		for (size_t i = previous; i < names->cap; ++i)
			names->items[i] = dstr(nullptr);
	}
	names->n = 0;
	for (size_t i = 0; i < count; ++i) {
		if (!autocomplete_safe(values[i], identifier))
			continue;
		int error = cstr_view_store(values[i], &names->items[names->n]);
		if (error)
			return error;
		++names->n;
	}
	if (names->n > 1)
		qsort(names->items, names->n, sizeof *names->items, autocomplete_names_compare);
	size_t used = 0;
	for (size_t i = 0; i < names->n; ++i) {
		if (used && cstr_view_equal(cstr_view_dstr(&names->items[used - 1]), cstr_view_dstr(&names->items[i])))
			continue;
		if (used != i) {
			struct dstr swap = names->items[used];
			names->items[used] = names->items[i];
			names->items[i] = swap;
		}
		++used;
	}
	names->n = used;
	return 0;
}

int
autocomplete_set_names (struct autocomplete    *ac,
                        enum autocomplete_list  list,
                        struct cstr_view const *names,
                        size_t                  count)
{
	return list < AUTOCOMPLETE_LISTS ? autocomplete_names_set(&ac->lists[list], names, count, true) : EINVAL;
}

/* Returns the index of the entry of ac->notes whose tool and selector
 * members equal the arguments tool and selector, or ac->notes.n if no
 * entry matches.
 */
static size_t
autocomplete_notebook_find (struct autocomplete const *ac,
                            struct cstr_view           tool,
                            struct cstr_view           selector)
{
	size_t i = 0;
	while (i < ac->notes.n && (!cstr_view_equal(cstr_view_dstr(&ac->notes.items[i].tool), tool) ||
	                           !cstr_view_equal(cstr_view_dstr(&ac->notes.items[i].selector), selector)))
		++i;
	return i;
}

void
autocomplete_clear_note_files (struct autocomplete *ac)
{
	ac->notes.n = 0;
	ac->notes_storage = 0;
}

int
autocomplete_set_note_files (struct autocomplete    *ac,
                             struct cstr_view        tool,
                             struct cstr_view        notebook,
                             struct cstr_view const *paths,
                             size_t                  count)
{
	if (!notebook.len)
		notebook = CSTR_VIEW("own");
	if (tool.len > AUTOCOMPLETE_BYTES || notebook.len > AUTOCOMPLETE_BYTES - tool.len ||
	    count > AUTOCOMPLETE_BYTES / sizeof(struct dstr))
		return E2BIG;
	if (!autocomplete_safe(tool, true) || !autocomplete_safe(notebook, true) ||
	    (count && !paths) || !autocomplete_find_tool(ac, tool))
		return EINVAL;
	size_t storage = tool.len + notebook.len;
	if (count * sizeof(struct dstr) > AUTOCOMPLETE_BYTES - storage)
		return E2BIG;
	storage += count * sizeof(struct dstr);
	for (size_t i = 0; i < count; ++i) {
		if (paths[i].len > AUTOCOMPLETE_BYTES - storage)
			return E2BIG;
		storage += paths[i].len;
	}
	size_t index = autocomplete_notebook_find(ac, tool, notebook);
	bool replace = index < ac->notes.n;
	size_t remaining = ac->notes_storage - (replace ? ac->notes.items[index].storage : 0);
	if ((!replace && index >= AUTOCOMPLETE_NOTE_LISTS) || storage > AUTOCOMPLETE_BYTES - remaining)
		return E2BIG;
	if (!replace) {
		size_t previous = ac->notes.cap;
		if (util_grow(ac->notes.items, ac->notes.cap, index + 1))
			return ENOMEM;
		for (size_t i = previous; i < ac->notes.cap; ++i)
			ac->notes.items[i] = (struct autocomplete_notebook){.tool = dstr(nullptr), .selector = dstr(nullptr)};
	}
	struct autocomplete_notebook *notes = &ac->notes.items[index];
	int error = cstr_view_store(tool, &notes->tool);
	if (!error)
		error = cstr_view_store(notebook, &notes->selector);
	if (!error)
		error = autocomplete_names_set(&notes->paths, paths, count, false);
	if (error) {
		notes->paths.n = 0;
		return error;
	}
	notes->storage = storage;
	ac->notes_storage = remaining + storage;
	if (!replace)
		++ac->notes.n;
	return 0;
}

size_t
autocomplete_list_count (struct autocomplete const *ac,
                         enum autocomplete_list     list)
{
	return list < AUTOCOMPLETE_LISTS ? ac->lists[list].n : 0;
}

struct cstr_view
autocomplete_list_at (struct autocomplete const *ac,
                      enum autocomplete_list     list,
                      size_t                     index)
{
	return index < autocomplete_list_count(ac, list) ? cstr_view_dstr(&ac->lists[list].items[index])
	                                                 : CSTR_VIEW("");
}

static int
autocomplete_replacement (struct autocomplete    *ac,
                          struct cstr_view        line,
                          size_t                  begin,
                          size_t                  end,
                          struct cstr_view        value,
                          bool                    separate,
                          enum autocomplete_kind  kind)
{
	if (separate && end < line.len && cstr_view_space(line.data[end]))
		++end;
	struct cstr_view const parts[] = {
		cstr_view_slice(line, 0, begin), value, {" ", separate}, cstr_view_slice(line, end, SIZE_MAX),
	};
	return autocomplete_matches_add(&ac->found, parts, array_size(parts), begin + value.len + separate,
	                                begin, kind);
}

static int
autocomplete_value_match (struct autocomplete      *ac,
                          struct cstr_view          line,
                          struct autocomplete_token current,
                          struct cstr_view          prefix,
                          struct cstr_view          value,
                          uint64_t                  flags,
                          char                      mark,
                          enum autocomplete_kind    kind)
{
	if (!cstr_view_starts(value, prefix))
		return 0;
	if (flags & AUTOCOMPLETE_STRING) {
		int error = autocomplete_quote(&ac->quoted, value, mark ? mark : flags & AUTOCOMPLETE_QUOTED ? '\'' : 0);
		if (error)
			return error == E2BIG ? 0 : error;
		value = cstr_view_dstr(&ac->quoted);
	}
	return autocomplete_replacement(ac, line, current.equals + 1, current.end, value, true, kind);
}

static int
autocomplete_note_paths (struct autocomplete              *ac,
                         struct cstr_view                  line,
                         struct cstr_view                  tool,
                         struct autocomplete_names const **paths)
{
	*paths = nullptr;
	struct autocomplete_token action = {.equals = SIZE_MAX};
	struct autocomplete_token notebook = {.equals = SIZE_MAX};
	for (size_t i = 2; i < ac->tokens.n; ++i) {
		struct autocomplete_token token = ac->tokens.items[i];
		if (token.equals == SIZE_MAX)
			continue;
		struct cstr_view key = cstr_view_slice(line, token.begin, token.equals - token.begin);
		struct autocomplete_token *selected = cstr_view_equal(key, CSTR_VIEW("action")) ? &action
		                                    : cstr_view_equal(key, CSTR_VIEW("notebook")) ? &notebook : nullptr;
		if (!selected)
			continue;
		/* Either one given twice selects nothing. */
		if (selected->equals != SIZE_MAX)
			return 0;
		*selected = token;
	}
	bool copy = false;
	if (action.equals != SIZE_MAX) {
		int error = autocomplete_unquote(&ac->key, cstr_view_slice(line, action.equals + 1, action.end - action.equals - 1));
		if (error)
			return error;
		size_t which = cstr_view_index(notes_action_names, array_size(notes_action_names), cstr_view_dstr(&ac->key));
		copy = which == NOTES_ACTION_COPY;
		if (!copy && which != NOTES_ACTION_READ && which != NOTES_ACTION_ATTACH)
			return 0;
	}
	struct cstr_view selector = copy ? CSTR_VIEW("other") : CSTR_VIEW("own");
	if (notebook.equals != SIZE_MAX) {
		int error = autocomplete_unquote(&ac->key, cstr_view_slice(line, notebook.equals + 1, notebook.end - notebook.equals - 1));
		if (error)
			return error;
		selector = cstr_view_dstr(&ac->key);
	}
	size_t i = autocomplete_notebook_find(ac, tool, selector);
	*paths = i < ac->notes.n ? &ac->notes.items[i].paths : nullptr;
	return 0;
}

static int
autocomplete_values (struct autocomplete                *ac,
                     struct cstr_view                    line,
                     struct autocomplete_token           current,
                     size_t                              cursor,
                     struct autocomplete_entry const    *entry,
                     struct autocomplete_property const *property)
{
	if (!property)
		return 0;
	uint64_t flags = property->flags;
	struct cstr_view input = cstr_view_slice(line, current.equals + 1, cursor - current.equals - 1);
	int error = autocomplete_unquote(&ac->decoded, input);
	if (error)
		return error;
	struct cstr_view prefix = cstr_view_dstr(&ac->decoded);
	char mark = input.len && (input.data[0] == '\'' || input.data[0] == '"') ? input.data[0] : 0;
	for (size_t i = 0; i < property->count; ++i) {
		struct autocomplete_candidate const *candidate = &property->values[i];
		if (!(candidate->flags & AUTOCOMPLETE_VALID))
			continue;
		error = autocomplete_value_match(ac, line, current, prefix, cstr_view_dstr(&candidate->value),
		                                 candidate->flags, mark, AUTOCOMPLETE_VALUE);
		if (error)
			return error;
	}
	/* The literals of the types of a value without fixed values. */
	static struct {
		struct cstr_view word;
		uint64_t         type;
	} const literals[] = {
		{CSTR_VIEW_INIT("false"), AUTOCOMPLETE_TYPE_BOOLEAN},
		{CSTR_VIEW_INIT("true"), AUTOCOMPLETE_TYPE_BOOLEAN},
		{CSTR_VIEW_INIT("null"), AUTOCOMPLETE_TYPE_NULL},
	};
	uint64_t open = flags & (AUTOCOMPLETE_CONSTANT | AUTOCOMPLETE_ENUMERATED) ? 0 : flags;
	for (size_t i = 0; i < array_size(literals); ++i) {
		if (!(open & literals[i].type))
			continue;
		error = autocomplete_value_match(ac, line, current, prefix, literals[i].word, 0, mark, AUTOCOMPLETE_VALUE);
		if (error)
			return error;
	}
	if (!(flags & AUTOCOMPLETE_NAMES))
		return 0;
	/* The properties whose values are the names of a list. */
	static struct {
		struct cstr_view property;
		uint64_t         list;  /* enum autocomplete_list */
	} const lists[] = {
		{CSTR_VIEW_INIT("participant"), AUTOCOMPLETE_PARTICIPANTS},
		{CSTR_VIEW_INIT("recipient"), AUTOCOMPLETE_PARTICIPANTS},
		{CSTR_VIEW_INIT("author"), AUTOCOMPLETE_PARTICIPANTS},
		{CSTR_VIEW_INIT("notebook"), AUTOCOMPLETE_NOTEBOOKS},
	};
	struct autocomplete_names const *names = nullptr;
	for (size_t i = 0; !names && i < array_size(lists); ++i)
		if (cstr_view_equal(property->name, lists[i].property))
			names = &ac->lists[lists[i].list];
	enum autocomplete_kind kind = AUTOCOMPLETE_NAME;
	if (!names && cstr_view_equal(property->name, CSTR_VIEW("file"))) {
		kind = AUTOCOMPLETE_FILE;
		error = autocomplete_note_paths(ac, line, entry->tool.name, &names);
		if (error)
			return error;
	}
	flags = AUTOCOMPLETE_STRING | (flags & AUTOCOMPLETE_STRING ? 0 : AUTOCOMPLETE_QUOTED);
	for (size_t i = 0; names && i < names->n; ++i) {
		error = autocomplete_value_match(ac, line, current, prefix, cstr_view_dstr(&names->items[i]), flags, mark, kind);
		if (error)
			return error;
	}
	return 0;
}

static bool
autocomplete_used (struct autocomplete const *ac,
                   struct cstr_view           line,
                   struct cstr_view           name,
                   size_t                     except)
{
	for (size_t i = 2; i < ac->tokens.n; ++i) {
		struct autocomplete_token token = ac->tokens.items[i];
		if (i != except && token.equals != SIZE_MAX &&
		    cstr_view_equal(name, cstr_view_slice(line, token.begin, token.equals - token.begin)))
			return true;
	}
	return false;
}

/* Offer the tools whose names start with prefix, in place of current. */
static int
autocomplete_tool_names (struct autocomplete      *ac,
                         struct cstr_view          line,
                         struct autocomplete_token current,
                         struct cstr_view          prefix)
{
	int error = 0;
	for (size_t i = 0; !error && i < ac->entries.n; ++i) {
		struct cstr_view name = ac->entries.items[i].tool.name;
		error = cstr_view_starts(name, prefix)
		      ? autocomplete_replacement(ac, line, current.begin, current.end, name, true, AUTOCOMPLETE_TOOL) : 0;
	}
	return error;
}

static int
autocomplete_commands (struct autocomplete      *ac,
                       struct cstr_view          line,
                       struct autocomplete_token current,
                       size_t                    cursor,
                       size_t                    index)
{
	struct cstr_view prefix = cstr_view_slice(line, current.begin, cursor - current.begin);
	int error = 0;
	if (!index) {
		struct cli_command_matches commands = cli_command_prefix(prefix);
		for (size_t i = 0; !error && i < commands.count; ++i)
			error = autocomplete_replacement(ac, line, current.begin, current.end, commands.data[i].name, true,
			                                 AUTOCOMPLETE_COMMAND);
		return error;
	}
	if (!ac->tokens.n)
		return 0;
	struct autocomplete_token command_token = ac->tokens.items[0];
	struct cli_command const *command = cli_command_find(cstr_view_slice(line, command_token.begin, command_token.end - command_token.begin));
	if (!command || (command->args != CLI_COMMAND_ARGS_TOOL && command->args != CLI_COMMAND_ARGS_TOOL_CALL))
		return 0;
	bool call = command->args == CLI_COMMAND_ARGS_TOOL_CALL;
	if (index == 1 || ac->tokens.n == 1)
		return autocomplete_tool_names(ac, line, current, prefix);
	if (!call || ac->tokens.n < 2 || (prefix.len && prefix.data[0] == '{'))
		return 0;
	struct autocomplete_token tool_token = ac->tokens.items[1];
	struct autocomplete_entry const *entry = autocomplete_entry_find(ac, cstr_view_slice(line, tool_token.begin, tool_token.end - tool_token.begin), nullptr);
	if (!entry)
		return 0;
	if (current.equals < cursor) {
		struct cstr_view key = cstr_view_slice(line, current.begin, current.equals - current.begin);
		return autocomplete_values(ac, line, current, cursor, entry, autocomplete_entry_property(entry, key));
	}
	/* A name in the token being edited does not count as used. */
	size_t except = current.end > current.begin ? index : SIZE_MAX;
	for (size_t i = 0; i < entry->count; ++i) {
		struct cstr_view name = entry->properties[i].name;
		if (!cstr_view_starts(name, prefix) || autocomplete_used(ac, line, name, except))
			continue;
		error = cstr_view_store(name, &ac->quoted);
		if (error)
			return error;
		if (current.equals == SIZE_MAX && !dstr_cat_n(&ac->quoted, "=", 1))
			return ENOMEM;
		error = autocomplete_replacement(ac, line, current.begin, current.equals == SIZE_MAX ? current.end : current.equals,
		                                 cstr_view_dstr(&ac->quoted), false, AUTOCOMPLETE_PARAMETER);
		if (error)
			return error;
	}
	return 0;
}

static int
autocomplete_mentions (struct autocomplete *ac,
                       struct cstr_view     line,
                       size_t               cursor)
{
	size_t begin = cursor;
	size_t end = cursor;
	while (begin && autocomplete_name_byte((unsigned char)line.data[begin - 1]))
		--begin;
	if (begin && line.data[begin - 1] == '@')
		--begin;
	while (end < line.len && autocomplete_name_byte((unsigned char)line.data[end]))
		++end;
	if (begin == cursor)
		return 0;
	bool mention = line.data[begin] == '@';
	struct cstr_view prefix = cstr_view_slice(line, begin + mention, cursor - begin - mention);
	bool separate = end == line.len || cstr_view_space(line.data[end]);
	struct autocomplete_names const *names = &ac->lists[AUTOCOMPLETE_CHAT_PARTICIPANTS];
	for (size_t i = 0; i < names->n; ++i) {
		struct cstr_view name = cstr_view_dstr(&names->items[i]);
		if (!cstr_view_starts(name, prefix))
			continue;
		(void)dstr_clear(&ac->quoted);
		if (mention && !dstr_cat_n(&ac->quoted, "@", 1))
			return ENOMEM;
		if (cstr_view_cat(name, &ac->quoted))
			return ENOMEM;
		if (!mention && !begin && separate && !dstr_cat_n(&ac->quoted, ":", 1))
			return ENOMEM;
		int error = autocomplete_replacement(ac, line, begin, end, cstr_view_dstr(&ac->quoted), separate, AUTOCOMPLETE_NAME);
		if (error)
			return error;
	}
	return 0;
}

static int
autocomplete_match_compare (struct autocomplete_match const *a,
                            struct autocomplete_match const *b,
                            size_t                           begin)
{
	int difference = cstr_view_compare(cstr_view_slice(a->line, begin, a->line.len - begin),
	                                   cstr_view_slice(b->line, begin, b->line.len - begin));
	return difference ? difference : (a->cursor > b->cursor) - (a->cursor < b->cursor);
}

/** Heapsort keeps the bounded completion table off the allocator. libc
 * qsort may allocate a merge buffer even when all strings are borrowed.
 */
static void
autocomplete_match_sink (struct autocomplete_match *matches,
                         size_t                     count,
                         size_t                     parent,
                         size_t                     begin)
{
	for (;;) {
		size_t child = parent * 2 + 1;
		if (child >= count)
			return;
		if (child + 1 < count && autocomplete_match_compare(&matches[child], &matches[child + 1], begin) < 0)
			++child;
		if (autocomplete_match_compare(&matches[parent], &matches[child], begin) >= 0)
			return;
		struct autocomplete_match swap = matches[parent];
		matches[parent] = matches[child];
		matches[child] = swap;
		parent = child;
	}
}

/* Sort the matches in place, without allocating, by their lines from
 * begin, then by cursor.
 */
static void
autocomplete_match_sort (struct autocomplete_match *matches,
                         size_t                     count,
                         size_t                     begin)
{
	for (size_t i = count / 2; i; --i)
		autocomplete_match_sink(matches, count, i - 1, begin);
	for (size_t i = count; i > 1; --i) {
		struct autocomplete_match swap = matches[0];
		matches[0] = matches[i - 1];
		matches[i - 1] = swap;
		autocomplete_match_sink(matches, i - 1, 0, begin);
	}
}

int
autocomplete_complete (struct autocomplete              *ac,
                       struct cstr_view                  line,
                       size_t                            cursor,
                       struct autocomplete_match const **matches,
                       size_t                           *count)
{
	*matches = nullptr;
	*count = 0;
	if (line.len > AUTOCOMPLETE_BYTES)
		return E2BIG;
	if (!jb_is_text(line) || cursor > line.len ||
	    (cursor < line.len && ((unsigned char)line.data[cursor] & 0xc0) == 0x80))
		return EINVAL;
	if (!line.data)
		line.data = "";
	autocomplete_matches_clear(&ac->found);
	int error;
	if (line.len && line.data[0] == '/') {
		error = autocomplete_tokenize(ac, line, false, nullptr, 0);
		if (error)
			return error;
		/* index is the index of the first token that does not end before
		 * the cursor. current is that token if it contains the cursor, and
		 * otherwise an empty token at the cursor. */
		size_t index = 0;
		while (index < ac->tokens.n && cursor > ac->tokens.items[index].end)
			++index;
		struct autocomplete_token current = {cursor, cursor, SIZE_MAX};
		if (index < ac->tokens.n && cursor >= ac->tokens.items[index].begin)
			current = ac->tokens.items[index];
		error = autocomplete_commands(ac, line, current, cursor, index);
	} else
		error = autocomplete_mentions(ac, line, cursor);
	if (error)
		return error;
	*matches = autocomplete_matches_finish(&ac->found, count);
	return 0;
}

void
autocomplete_matches_clear (struct autocomplete_matches *set)
{
	set->count = 0;
	(void)dstr_clear(&set->lines);
}

int
autocomplete_matches_add (struct autocomplete_matches *set,
                          struct cstr_view const      *parts,
                          size_t                       n,
                          size_t                       cursor,
                          size_t                       label_begin,
                          enum autocomplete_kind       kind)
{
	if (set->count == array_size(set->matches))
		return 0;
	/* The line and its NUL must fit in the room that is left. */
	size_t room = AUTOCOMPLETE_BYTES - dstr_len(&set->lines);
	size_t length = 0;
	for (size_t i = 0; i < n; ++i) {
		if (parts[i].len >= room - length)
			return 0;
		length += parts[i].len;
	}
	for (size_t i = 0; i < n; ++i)
		if (cstr_view_cat(parts[i], &set->lines))
			return ENOMEM;
	if (!dstr_cat_n(&set->lines, "", 1))
		return ENOMEM;
	if (!set->count || label_begin < set->begin)
		set->begin = label_begin;
	set->matches[set->count++] = (struct autocomplete_match){
		.line = {nullptr, length},
		.cursor = cursor,
		.label_begin = (uint32_t)label_begin,
		.kind = kind,
	};
	return 0;
}

struct autocomplete_match const *
autocomplete_matches_finish (struct autocomplete_matches *set,
                             size_t                      *count)
{
	char const *line = dstr_get(&set->lines);
	for (size_t i = 0; i < set->count; ++i) {
		set->matches[i].line.data = line;
		line += set->matches[i].line.len + 1;
	}
	autocomplete_match_sort(set->matches, set->count, set->begin);
	size_t used = 0;
	for (size_t i = 0; i < set->count; ++i)
		if (!used || autocomplete_match_compare(&set->matches[used - 1], &set->matches[i], set->begin))
			set->matches[used++] = set->matches[i];
	*count = set->count = used;
	return set->matches;
}

static int
autocomplete_entry_validate (struct autocomplete_entry const *entry,
                             struct json_object              *arguments,
                             char                            *why,
                             size_t                           size)
{
	struct json_object *required = jb_get(entry->root, "required");
	for (size_t i = 0, count = jb_len(required); i < count; ++i) {
		struct cstr_view key = autocomplete_json_text(json_object_array_get_idx(required, i));
		struct json_object *unused = nullptr;
		if (!json_object_object_get_ex(arguments, key.data, &unused))
			return util_refuse(why, size, "Missing required tool argument: %.*s", (int)key.len, key.data);
	}
	return 0;
}

static int
autocomplete_convert (struct autocomplete *autocomplete,
                      struct cstr_view     raw,
                      uint64_t             flags,
                      struct json_object **converted)
{
	*converted = nullptr;
	if (raw.len) {
		if (raw.data[0] == '{' || raw.data[0] == '[')
			flags |= AUTOCOMPLETE_STRUCTURED;
		else if (raw.data[0] == '\'' || raw.data[0] == '"')
			flags |= AUTOCOMPLETE_QUOTED;
	}
	int error = flags & AUTOCOMPLETE_STRUCTURED ? cstr_view_store(raw, &autocomplete->decoded)
	                                            : autocomplete_unquote(&autocomplete->decoded, raw);
	if (error)
		return error;
	struct cstr_view decoded = cstr_view_dstr(&autocomplete->decoded);
	if ((flags & (AUTOCOMPLETE_QUOTED | AUTOCOMPLETE_TYPED)) == AUTOCOMPLETE_QUOTED ||
	    (flags & (AUTOCOMPLETE_STRING | AUTOCOMPLETE_QUOTED)) == (AUTOCOMPLETE_STRING | AUTOCOMPLETE_QUOTED) ||
	    (flags & (AUTOCOMPLETE_STRING | AUTOCOMPLETE_UNION)) == AUTOCOMPLETE_STRING)
		flags |= AUTOCOMPLETE_USE_STRING;
	if (!(flags & AUTOCOMPLETE_USE_STRING)) {
		/* A value that is not JSON of its type is a string, if it may be. */
		error = autocomplete_json_parse(autocomplete, decoded, converted);
		bool string = error ? error == EINVAL && (flags & (AUTOCOMPLETE_STRING | AUTOCOMPLETE_TYPED)) != AUTOCOMPLETE_TYPED
		                    : (flags & AUTOCOMPLETE_STRING) && !autocomplete_accepted_type(flags, *converted);
		if (!string)
			return error;
	}
	json_object_put(*converted);
	*converted = nullptr;
	if (decoded.len > INT_MAX)
		return E2BIG;
	*converted = json_object_new_string_len(decoded.data, (int)decoded.len);
	return *converted ? 0 : ENOMEM;
}

/* Add the name=value arguments in the tokens from the third on to result,
 * converted by the schema of entry, and check the required arguments.
 * Returns 0 or an error number, with the diagnostic in why.
 */
static int
autocomplete_parse_pairs (struct autocomplete             *ac,
                          struct autocomplete_entry const *entry,
                          struct cstr_view                 line,
                          struct json_object              *result,
                          char                            *why,
                          size_t                           size)
{
	struct json_object *additional = jb_get(entry->root, "additionalProperties");
	bool closed = json_object_is_type(additional, json_type_boolean) && !json_object_get_boolean(additional);
	for (size_t i = 2; i < ac->tokens.n; ++i) {
		struct autocomplete_token token = ac->tokens.items[i];
		struct cstr_view key = cstr_view_slice(line, token.begin, token.equals - token.begin);
		if (token.equals == SIZE_MAX || !autocomplete_safe(key, true))
			return util_refuse(why, size, "Expected a tool argument as name=value");
		int error = cstr_view_store(key, &ac->key);
		if (error)
			return error;
		char const *name = dstr_get(&ac->key);
		if (json_object_object_get_ex(result, name, nullptr))
			return util_refuse(why, size, "Duplicate tool argument: %.*s", (int)key.len, key.data);
		struct autocomplete_property const *property = autocomplete_entry_property(entry, key);
		if (!property && closed)
			return util_refuse(why, size, "Unknown tool argument: %.*s", (int)key.len, key.data);
		/* Without a property, flags is 0, and every type is valid. */
		uint64_t flags = property ? property->flags : 0;
		struct json_object *converted = nullptr;
		error = autocomplete_convert(ac, cstr_view_slice(line, token.equals + 1, token.end - token.equals - 1),
		                             flags, &converted);
		if (error) {
			(void)util_format(why, size, "Invalid value for tool argument: %.*s", (int)key.len, key.data);
			return error;
		}
		if (!autocomplete_accepted_type(flags, converted))
			error = util_refuse(why, size, "Wrong type for tool argument: %.*s", (int)key.len, key.data);
		else if (json_object_object_add(result, name, converted))
			error = ENOMEM;
		if (error) {
			json_object_put(converted);
			return error;
		}
	}
	return autocomplete_entry_validate(entry, result, why, size);
}

int
autocomplete_parse_call (struct autocomplete             *ac,
                         struct cstr_view                 line,
                         struct autocomplete_tool const **tool,
                         struct json_object             **arguments,
                         char                            *why,
                         size_t                           size)
{
	*tool = nullptr;
	*arguments = nullptr;
	if (why && size)
		*why = '\0';
	if (line.len > AUTOCOMPLETE_BYTES) {
		(void)util_format(why, size, "Tool command is too long");
		return E2BIG;
	}
	if (!jb_is_text(line))
		return util_refuse(why, size, "Invalid tool command bytes");
	if (!line.data)
		line.data = "";
	int error = autocomplete_tokenize(ac, line, true, why, size);
	if (error)
		return error;
	struct autocomplete_token const *tokens = ac->tokens.items;
	struct cli_command const *command = ac->tokens.n < 2 ? nullptr
	                                  : cli_command_find(cstr_view_slice(line, tokens[0].begin,
	                                                                     tokens[0].end - tokens[0].begin));
	if (!command || command->id != CLI_COMMAND_TOOL)
		return util_refuse(why, size, "Usage: %s", cli_command_at(CLI_COMMAND_TOOL)->usage.data);
	struct cstr_view name = cstr_view_slice(line, tokens[1].begin, tokens[1].end - tokens[1].begin);
	struct autocomplete_entry const *entry = autocomplete_entry_find(ac, name, nullptr);
	if (!entry) {
		(void)util_format(why, size, "Unknown or inaccessible tool: %.*s", (int)name.len, name.data);
		return ENOENT;
	}
	struct cstr_view rest = ac->tokens.n > 2 ? cstr_view_slice(line, tokens[2].begin, SIZE_MAX)
	                                         : (struct cstr_view){};
	struct json_object *result = nullptr;
	if (rest.len && rest.data[0] == '{') {
		error = autocomplete_json_parse(ac, rest, &result);
		if (error) {
			(void)util_format(why, size, "Invalid JSON tool argument");
			return error;
		}
		if (!json_object_is_type(result, json_type_object))
			error = util_refuse(why, size, "Tool arguments must be a JSON object");
	} else {
		result = json_object_new_object();
		if (!result)
			return ENOMEM;
		error = autocomplete_parse_pairs(ac, entry, line, result, why, size);
	}
	if (error) {
		json_object_put(result);
		return error;
	}
	*tool = &entry->tool;
	*arguments = result;
	return 0;
}
