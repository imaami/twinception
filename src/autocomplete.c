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

enum { AUTOCOMPLETE_BYTES = 16 * 1024 * 1024 };

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
	AUTOCOMPLETE_TYPE_NULL    = UINT64_C(1) << 10,
	AUTOCOMPLETE_TYPE_BOOLEAN = UINT64_C(1) << 11,
	AUTOCOMPLETE_TYPE_NUMBER  = UINT64_C(1) << 12,
	AUTOCOMPLETE_TYPE_INTEGER = UINT64_C(1) << 13,
	AUTOCOMPLETE_TYPE_OBJECT  = UINT64_C(1) << 14,
	AUTOCOMPLETE_TYPE_ARRAY   = UINT64_C(1) << 15,
	AUTOCOMPLETE_TYPE_STRING  = UINT64_C(1) << 16,
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
	struct dstr *values;
	size_t       count;
	size_t       capacity;
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
	struct autocomplete_entry    *entries;
	size_t                        count;
	size_t                        capacity;
	size_t                        storage;
	struct autocomplete_names     lists[AUTOCOMPLETE_LISTS];
	struct autocomplete_notebook *notes;
	size_t                        notes_count;
	size_t                        notes_capacity;
	size_t                        notes_storage;
	struct autocomplete_token    *tokens;
	size_t                        token_count;
	size_t                        token_capacity;
	struct json_tokener          *parser;
	struct dstr                   nesting;
	struct dstr                   key;
	struct dstr                   decoded;
	struct dstr                   quoted;
	struct dstr                   json;
	struct dstr                   lines;
	struct autocomplete_match     matches[256];
	size_t                        offsets[256];
	size_t                        match_count;
	size_t                        match_begin;
};

static struct cstr_view
autocomplete_json_text (struct json_object *value)
{
	return json_object_is_type(value, json_type_string)
	       ? (struct cstr_view){json_object_get_string(value), (size_t)json_object_get_string_len(value)}
	       : CSTR_VIEW("");
}

static bool
autocomplete_space (unsigned char byte)
{
	return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static bool
autocomplete_name_byte (unsigned char byte)
{
	return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
	       (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte >= 0x80;
}

bool
autocomplete_line (struct cstr_view line)
{
	return line.data ? !memchr(line.data, '\0', line.len) && jb_utf8(line.data, line.len) == line.len
	                 : !line.len;
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
		    (identifier && (autocomplete_space(byte) || byte == '=' || byte == '\'' || byte == '"' || byte == '\\')))
			return false;
	}
	return true;
}

static int
autocomplete_diagnose (struct autocomplete_error *error,
                       int                        number,
                       struct cstr_view           message,
                       struct cstr_view           detail)
{
	if (!error)
		return number;
	error->len = 0;
	if (!error->data || !error->capacity)
		return number;
	size_t count = message.len < error->capacity - 1 ? message.len : error->capacity - 1;
	if (count)
		memcpy(error->data, message.data, count);
	error->len = count;
	count = detail.len < error->capacity - 1 - error->len ? detail.len : error->capacity - 1 - error->len;
	if (count)
		memcpy(error->data + error->len, detail.data, count);
	error->len += count;
	error->data[error->len] = '\0';
	return number;
}

static void *
autocomplete_grow (void   *memory,
                   size_t *capacity,
                   size_t  needed,
                   size_t  width)
{
	if (needed <= *capacity)
		return memory;
	if (needed > SIZE_MAX / width)
		return nullptr;
	size_t next = *capacity ? *capacity : 16;
	while (next < needed && next <= SIZE_MAX / 2)
		next *= 2;
	if (next < needed || next > SIZE_MAX / width)
		next = needed;
	void *grown = realloc(memory, next * width);
	if (grown)
		*capacity = next;
	return grown;
}

/** Reserve a whole operation while retaining geometric headroom. */
static int
autocomplete_buffer_reserve (struct dstr *buffer,
                             size_t       extra)
{
	struct dstr_info info = dstr_info(buffer);
	if (extra <= info.space)
		return 0;
	if (extra > SIZE_MAX - info.length - 1)
		return ENOMEM;
	size_t needed = info.length + extra;
	size_t capacity = info.length + info.space;
	size_t next = capacity > 256 ? capacity : 256;
	while (next < needed && next <= (SIZE_MAX - 1) / 2)
		next *= 2;
	if (next < needed)
		next = needed;
	return dstr_reserve(buffer, next - info.length) ? 0 : ENOMEM;
}

static int
autocomplete_unquote (struct dstr     *output,
                      struct cstr_view value)
{
	(void)dstr_clear(output);
	if (autocomplete_buffer_reserve(output, value.len))
		return ENOMEM;
	char mark = 0;
	bool escape = false;
	for (size_t i = 0; i < value.len; ++i) {
		char byte = value.data[i];
		if (escape) {
			(void)dstr_cat_n(output, &byte, 1);
			escape = false;
		} else if (byte == '\\' && mark != '\'')
			escape = true;
		else if (mark) {
			if (byte == mark)
				mark = 0;
			else
				(void)dstr_cat_n(output, &byte, 1);
		} else if (byte == '\'' || byte == '"')
			mark = byte;
		else
			(void)dstr_cat_n(output, &byte, 1);
	}
	return 0;
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
		needs |= autocomplete_space((unsigned char)byte) || byte == '\\' || byte == '\'' || byte == '"' ||
		         byte == '[' || byte == ']' || byte == '{' || byte == '}';
		size_t extra = mark == '\'' && byte == '\'' ? 3 : mark == '"' && (byte == '"' || byte == '\\');
		if (extra > SIZE_MAX - length)
			return ENOMEM;
		length += extra;
	}
	if (length > AUTOCOMPLETE_BYTES)
		return E2BIG;
	if (!needs)
		return cstr_view_store(value, output);
	(void)dstr_clear(output);
	if (autocomplete_buffer_reserve(output, length))
		return ENOMEM;
	if (!dstr_cat_n(output, &mark, 1))
		return ENOMEM;
	for (size_t i = 0; i < value.len; ++i) {
		char byte = value.data[i];
		if (mark == '\'' && byte == '\'') {
			if (!dstr_cat_n(output, "'\\''", 4))
				return ENOMEM;
		} else {
			if (mark == '"' && (byte == '"' || byte == '\\') && !dstr_cat_n(output, "\\", 1))
				return ENOMEM;
			if (!dstr_cat_n(output, &byte, 1))
				return ENOMEM;
		}
	}
	return dstr_cat_n(output, &mark, 1) ? 0 : ENOMEM;
}

static int
autocomplete_tokenize (struct autocomplete       *ac,
                       struct cstr_view           line,
                       bool                       strict,
                       struct autocomplete_error *diagnostic)
{
	ac->token_count = 0;
	for (size_t i = 0; i < line.len;) {
		if (autocomplete_space((unsigned char)line.data[i])) {
			++i;
			continue;
		}
		size_t begin = i;
		size_t equals = SIZE_MAX;
		char mark = 0;
		bool escape = false;
		(void)dstr_clear(&ac->nesting);
		for (; i < line.len; ++i) {
			char byte = line.data[i];
			if (equals == SIZE_MAX && byte == '=')
				equals = i;
			if (escape) {
				escape = false;
				continue;
			}
			if (byte == '\\' && mark != '\'') {
				escape = true;
				continue;
			}
			if (mark) {
				if (byte == mark)
					mark = 0;
				continue;
			}
			if (byte == '\'' || byte == '"')
				mark = byte;
			else if (byte == '[' || byte == '{') {
				if (autocomplete_buffer_reserve(&ac->nesting, 1))
					return ENOMEM;
				if (!dstr_cat_n(&ac->nesting, &byte, 1))
					return ENOMEM;
			} else if (byte == ']' || byte == '}') {
				size_t depth = dstr_len(&ac->nesting);
				if (depth && dstr_get(&ac->nesting)[depth - 1] == (byte == ']' ? '[' : '{'))
					(void)dstr_erase(&ac->nesting, depth - 1, 1);
				else if (strict)
					return autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Unmatched bracket in tool arguments"), CSTR_VIEW(""));
			} else if (autocomplete_space((unsigned char)byte) && !dstr_len(&ac->nesting))
				break;
		}
		if (strict && (mark || escape || dstr_len(&ac->nesting)))
			return autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Unterminated quote, escape or JSON value in tool arguments"), CSTR_VIEW(""));
		struct autocomplete_token *tokens = autocomplete_grow(ac->tokens, &ac->token_capacity,
		                                                      ac->token_count + 1, sizeof *tokens);
		if (!tokens)
			return ENOMEM;
		ac->tokens = tokens;
		tokens[ac->token_count++] = (struct autocomplete_token){begin, i, equals};
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
	int error = cstr_view_store(value, &autocomplete->json);
	if (error)
		return error;
	if (!autocomplete->parser) {
		autocomplete->parser = json_tokener_new();
		if (!autocomplete->parser)
			return ENOMEM;
		json_tokener_set_flags(autocomplete->parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	} else
		json_tokener_reset(autocomplete->parser);
	struct json_object *parsed = json_tokener_parse_ex(autocomplete->parser, dstr_get(&autocomplete->json), (int)value.len + 1);
	size_t end = json_tokener_get_parse_end(autocomplete->parser);
	while (end < value.len && autocomplete_space((unsigned char)value.data[end]))
		++end;
	if (json_tokener_get_error(autocomplete->parser) != json_tokener_success || end < value.len) {
		json_object_put(parsed);
		parsed = nullptr;
		return EINVAL;
	}
	*result = parsed;
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
		struct json_object *next = root;
		for (size_t begin = 2; next && begin <= ref.len;) {
			size_t end = begin;
			while (end < ref.len && ref.data[end] != '/')
				++end;
			(void)dstr_clear(&autocomplete->key);
			if (autocomplete_buffer_reserve(&autocomplete->key, end - begin))
				return ENOMEM;
			for (size_t i = begin; i < end; ++i) {
				char byte = ref.data[i];
				if (byte == '~' && i + 1 < end && (ref.data[i + 1] == '0' || ref.data[i + 1] == '1'))
					byte = ref.data[++i] == '0' ? '~' : '/';
				(void)dstr_cat_n(&autocomplete->key, &byte, 1);
			}
			next = jb_get(next, dstr_get(&autocomplete->key));
			if (end == ref.len)
				break;
			begin = end + 1;
		}
		if (!next || next == *schema)
			break;
		*schema = next;
	}
	return 0;
}

static uint64_t
autocomplete_type_flag (struct cstr_view type)
{
	static struct {
		struct cstr_view name;
		uint64_t         flag;
	} const types[] = {
		{CSTR_VIEW_INIT("null"), AUTOCOMPLETE_TYPE_NULL},
		{CSTR_VIEW_INIT("boolean"), AUTOCOMPLETE_TYPE_BOOLEAN},
		{CSTR_VIEW_INIT("number"), AUTOCOMPLETE_TYPE_NUMBER},
		{CSTR_VIEW_INIT("integer"), AUTOCOMPLETE_TYPE_INTEGER},
		{CSTR_VIEW_INIT("object"), AUTOCOMPLETE_TYPE_OBJECT},
		{CSTR_VIEW_INIT("array"), AUTOCOMPLETE_TYPE_ARRAY},
		{CSTR_VIEW_INIT("string"), AUTOCOMPLETE_TYPE_STRING},
	};
	for (size_t i = 0; i < array_size(types); ++i)
		if (cstr_view_equal(types[i].name, type))
			return types[i].flag;
	return 0;
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
	if (!(flags & AUTOCOMPLETE_TYPED))
		return true;
	switch (json_object_get_type(value)) {
	case json_type_null: return flags & AUTOCOMPLETE_TYPE_NULL;
	case json_type_boolean: return flags & AUTOCOMPLETE_TYPE_BOOLEAN;
	case json_type_double: {
		double number = json_object_get_double(value);
		return (flags & AUTOCOMPLETE_TYPE_NUMBER) ||
		       ((flags & AUTOCOMPLETE_TYPE_INTEGER) && isfinite(number) && floor(number) == number);
	}
	case json_type_int: return flags & (AUTOCOMPLETE_TYPE_INTEGER | AUTOCOMPLETE_TYPE_NUMBER);
	case json_type_object: return flags & AUTOCOMPLETE_TYPE_OBJECT;
	case json_type_array: return flags & AUTOCOMPLETE_TYPE_ARRAY;
	case json_type_string: return flags & AUTOCOMPLETE_TYPE_STRING;
	}
	return false;
}

static struct autocomplete_entry const *
autocomplete_entry_find (struct autocomplete const *ac,
                         struct cstr_view           name,
                         size_t                    *position)
{
	size_t begin = 0;
	size_t end = ac->count;
	while (begin < end) {
		size_t middle = begin + (end - begin) / 2;
		if (cstr_view_compare(ac->entries[middle].tool.name, name) < 0)
			begin = middle + 1;
		else
			end = middle;
	}
	if (position)
		*position = begin;
	return begin < ac->count && cstr_view_equal(ac->entries[begin].tool.name, name)
	       ? &ac->entries[begin] : nullptr;
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
	if (flags & AUTOCOMPLETE_TYPE_STRING)
		flags |= AUTOCOMPLETE_STRING;
	else if (!(flags & AUTOCOMPLETE_TYPED)) {
		if (json_object_is_type(constant, json_type_string))
			flags |= AUTOCOMPLETE_STRING;
		else if (count) {
			flags |= AUTOCOMPLETE_STRING;
			for (size_t i = 0; i < count; ++i)
				if (!json_object_is_type(json_object_array_get_idx(enumeration, i), json_type_string)) {
					flags &= ~AUTOCOMPLETE_STRING;
					break;
				}
		}
	}
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
		if (e)
			*e = ENOMEM;
		return nullptr;
	}
	*ac = (struct autocomplete){
		.nesting = dstr(nullptr), .key = dstr(nullptr), .decoded = dstr(nullptr),
		.quoted = dstr(nullptr), .json = dstr(nullptr), .lines = dstr(nullptr),
	};
	if (e)
		*e = 0;
	return ac;
}

void
autocomplete_clear_tools (struct autocomplete *ac)
{
	for (size_t i = 0; i < ac->count; ++i)
		autocomplete_entry_fini(&ac->entries[i]);
	ac->count = 0;
	ac->storage = 0;
	autocomplete_clear_note_files(ac);
}

static void
autocomplete_names_fini (struct autocomplete_names *names)
{
	for (size_t i = 0; i < names->capacity; ++i)
		dstr_cleanup(&names->values[i]);
	free(names->values);
	names->values = nullptr;
	names->count = 0;
	names->capacity = 0;
}

void
autocomplete_destroy (struct autocomplete **pp)
{
	if (!pp || !*pp)
		return;
	struct autocomplete *ac = *pp;
	autocomplete_clear_tools(ac);
	free(ac->entries);
	ac->entries = nullptr;
	free(ac->tokens);
	ac->tokens = nullptr;
	for (size_t i = 0; i < AUTOCOMPLETE_LISTS; ++i)
		autocomplete_names_fini(&ac->lists[i]);
	for (size_t i = 0; i < ac->notes_capacity; ++i) {
		dstr_cleanup(&ac->notes[i].tool);
		dstr_cleanup(&ac->notes[i].selector);
		autocomplete_names_fini(&ac->notes[i].paths);
	}
	free(ac->notes);
	ac->notes = nullptr;
	if (ac->parser)
		json_tokener_free(ac->parser);
	ac->parser = nullptr;
	dstr_cleanup(&ac->nesting);
	dstr_cleanup(&ac->key);
	dstr_cleanup(&ac->decoded);
	dstr_cleanup(&ac->quoted);
	dstr_cleanup(&ac->json);
	dstr_cleanup(&ac->lines);
	free(*pp);
	*pp = nullptr;
}

int
autocomplete_add_tool (struct autocomplete *ac,
                       struct json_object  *def)
{
	struct cstr_view name = autocomplete_json_text(jb_get(def, "name"));
	if (!autocomplete_safe(name, true))
		return 0;
	size_t position = 0;
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
	if ((!replace && ac->count >= 4096) || entry.storage > AUTOCOMPLETE_BYTES || remaining > AUTOCOMPLETE_BYTES - entry.storage) {
		error = E2BIG;
		goto fail;
	}
	error = autocomplete_resolve(ac, entry.root, &entry.root);
	if (error)
		goto fail;
	struct json_object *properties = jb_get(entry.root, "properties");
	if (json_object_is_type(properties, json_type_object)) {
		size_t count = (size_t)json_object_object_length(properties);
		if (count) {
			entry.properties = malloc(count * sizeof *entry.properties);
			if (!entry.properties) {
				error = ENOMEM;
				goto fail;
			}
		}
		struct json_object_iter iterator;
		json_object_object_foreachC(properties, iterator) {
			/* json-c does not expose key lengths. Measure once at discovery. */
			struct cstr_view property_name = cstr_view(iterator.key);
			if (!autocomplete_safe(property_name, true))
				continue;
			error = autocomplete_property_init(&entry.properties[entry.count], ac,
			                                   entry.tool.schema, iterator.val,
			                                   property_name);
			++entry.count;
			if (error)
				goto fail;
		}
	}
	if (!replace) {
		struct autocomplete_entry *entries = autocomplete_grow(ac->entries, &ac->capacity,
		                                                       ac->count + 1, sizeof *entries);
		if (!entries) {
			error = ENOMEM;
			goto fail;
		}
		ac->entries = entries;
		memmove(entries + position + 1, entries + position, (ac->count - position) * sizeof *entries);
		++ac->count;
	} else
		autocomplete_entry_fini(&ac->entries[position]);
	ac->entries[position] = entry;
	ac->storage = remaining + entry.storage;
	return 0;
fail:
	autocomplete_entry_fini(&entry);
	return error;
}

size_t
autocomplete_tool_count (struct autocomplete const *ac)
{
	return ac->count;
}

struct autocomplete_tool const *
autocomplete_tool_at (struct autocomplete const *ac,
                      size_t                     index)
{
	return index < ac->count ? &ac->entries[index].tool : nullptr;
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
	if (count > names->capacity) {
		size_t previous = names->capacity;
		struct dstr *grown = autocomplete_grow(names->values, &names->capacity, count, sizeof *grown);
		if (!grown)
			return ENOMEM;
		names->values = grown;
		for (size_t i = previous; i < names->capacity; ++i)
			names->values[i] = dstr(nullptr);
	}
	names->count = 0;
	for (size_t i = 0; i < count; ++i) {
		if (!autocomplete_safe(values[i], identifier))
			continue;
		int error = cstr_view_store(values[i], &names->values[names->count]);
		if (error)
			return error;
		++names->count;
	}
	if (names->count > 1)
		qsort(names->values, names->count, sizeof *names->values, autocomplete_names_compare);
	size_t used = 0;
	for (size_t i = 0; i < names->count; ++i) {
		if (used && cstr_view_equal(cstr_view_dstr(&names->values[used - 1]), cstr_view_dstr(&names->values[i])))
			continue;
		if (used != i) {
			struct dstr swap = names->values[used];
			names->values[used] = names->values[i];
			names->values[i] = swap;
		}
		++used;
	}
	names->count = used;
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

void
autocomplete_clear_note_files (struct autocomplete *ac)
{
	ac->notes_count = 0;
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
	size_t index = 0;
	for (; index < ac->notes_count; ++index) {
		struct autocomplete_notebook const *notes = &ac->notes[index];
		if (cstr_view_equal(cstr_view_dstr(&notes->tool), tool) &&
		    cstr_view_equal(cstr_view_dstr(&notes->selector), notebook))
			break;
	}
	bool replace = index < ac->notes_count;
	size_t remaining = ac->notes_storage - (replace ? ac->notes[index].storage : 0);
	if ((!replace && index >= 4096) || storage > AUTOCOMPLETE_BYTES - remaining)
		return E2BIG;
	if (!replace) {
		size_t previous = ac->notes_capacity;
		struct autocomplete_notebook *notes = autocomplete_grow(ac->notes, &ac->notes_capacity,
		                                                         index + 1, sizeof *notes);
		if (!notes)
			return ENOMEM;
		ac->notes = notes;
		for (size_t i = previous; i < ac->notes_capacity; ++i)
			notes[i] = (struct autocomplete_notebook){.tool = dstr(nullptr), .selector = dstr(nullptr)};
	}
	struct autocomplete_notebook *notes = &ac->notes[index];
	int error = cstr_view_store(tool, &notes->tool);
	if (!error)
		error = cstr_view_store(notebook, &notes->selector);
	if (!error)
		error = autocomplete_names_set(&notes->paths, paths, count, false);
	if (error) {
		notes->paths.count = 0;
		return error;
	}
	notes->storage = storage;
	ac->notes_storage = remaining + storage;
	if (!replace)
		++ac->notes_count;
	return 0;
}

size_t
autocomplete_participant_count (struct autocomplete const *ac)
{
	return ac->lists[AUTOCOMPLETE_PARTICIPANTS].count;
}

struct cstr_view
autocomplete_participant_at (struct autocomplete const *ac,
                             size_t                     index)
{
	struct autocomplete_names const *names = &ac->lists[AUTOCOMPLETE_PARTICIPANTS];
	return index < names->count ? cstr_view_dstr(&names->values[index]) : CSTR_VIEW("");
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
	if (ac->match_count == array_size(ac->matches))
		return 0;
	if (separate && end < line.len && autocomplete_space((unsigned char)line.data[end]))
		++end;
	if (value.len > SIZE_MAX - line.len - 2)
		return ENOMEM;
	size_t length = begin + value.len + separate + line.len - end;
	size_t offset = dstr_len(&ac->lines);
	if (length >= AUTOCOMPLETE_BYTES - offset)
		return 0;
	if (autocomplete_buffer_reserve(&ac->lines, length + 1))
		return ENOMEM;
	(void)dstr_cat_n(&ac->lines, line.data, begin);
	(void)dstr_cat_n(&ac->lines, value.data, value.len);
	if (separate)
		(void)dstr_cat_n(&ac->lines, " ", 1);
	(void)dstr_cat_n(&ac->lines, line.data + end, line.len - end);
	(void)dstr_cat_n(&ac->lines, "", 1);
	size_t index = ac->match_count++;
	if (!index || begin < ac->match_begin)
		ac->match_begin = begin;
	ac->offsets[index] = offset;
	ac->matches[index] = (struct autocomplete_match){
		.line = {nullptr, length},
		.cursor = begin + value.len + separate,
		.label_begin = (uint32_t)begin,
		.kind = kind,
	};
	return 0;
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
	for (size_t i = 2; i < ac->token_count; ++i) {
		struct autocomplete_token token = ac->tokens[i];
		if (token.equals == SIZE_MAX)
			continue;
		struct cstr_view key = cstr_view_slice(line, token.begin, token.equals - token.begin);
		struct autocomplete_token *selected = cstr_view_equal(key, CSTR_VIEW("action")) ? &action
		                                    : cstr_view_equal(key, CSTR_VIEW("notebook")) ? &notebook : nullptr;
		if (selected) {
			if (selected->equals != SIZE_MAX)
				return 0;
			*selected = token;
		}
	}
	bool copy = false;
	if (action.equals != SIZE_MAX) {
		int error = autocomplete_unquote(&ac->key, cstr_view_slice(line, action.equals + 1, action.end - action.equals - 1));
		if (error)
			return error;
		struct cstr_view value = cstr_view_dstr(&ac->key);
		copy = cstr_view_equal(value, CSTR_VIEW("copy"));
		if (!copy && !cstr_view_equal(value, CSTR_VIEW("read")) &&
		    !cstr_view_equal(value, CSTR_VIEW("attach")))
			return 0;
	}
	struct cstr_view selector = copy ? CSTR_VIEW("other") : CSTR_VIEW("own");
	if (notebook.equals != SIZE_MAX) {
		int error = autocomplete_unquote(&ac->key, cstr_view_slice(line, notebook.equals + 1, notebook.end - notebook.equals - 1));
		if (error)
			return error;
		selector = cstr_view_dstr(&ac->key);
	}
	for (size_t i = 0; i < ac->notes_count; ++i) {
		struct autocomplete_notebook const *notes = &ac->notes[i];
		if (cstr_view_equal(cstr_view_dstr(&notes->tool), tool) &&
		    cstr_view_equal(cstr_view_dstr(&notes->selector), selector)) {
			*paths = &notes->paths;
			break;
		}
	}
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
	if (!(flags & (AUTOCOMPLETE_CONSTANT | AUTOCOMPLETE_ENUMERATED))) {
		if (flags & AUTOCOMPLETE_TYPE_BOOLEAN) {
			error = autocomplete_value_match(ac, line, current, prefix, CSTR_VIEW("false"), 0, mark, AUTOCOMPLETE_VALUE);
			if (!error)
				error = autocomplete_value_match(ac, line, current, prefix, CSTR_VIEW("true"), 0, mark, AUTOCOMPLETE_VALUE);
			if (error)
				return error;
		}
		if (flags & AUTOCOMPLETE_TYPE_NULL) {
			error = autocomplete_value_match(ac, line, current, prefix, CSTR_VIEW("null"), 0, mark, AUTOCOMPLETE_VALUE);
			if (error)
				return error;
		}
	}
	if (!(flags & AUTOCOMPLETE_NAMES))
		return 0;
	struct autocomplete_names const *names = nullptr;
	enum autocomplete_kind kind = AUTOCOMPLETE_NAME;
	if (cstr_view_equal(property->name, CSTR_VIEW("participant")) ||
	    cstr_view_equal(property->name, CSTR_VIEW("recipient")) ||
	    cstr_view_equal(property->name, CSTR_VIEW("author")))
		names = &ac->lists[AUTOCOMPLETE_PARTICIPANTS];
	else if (cstr_view_equal(property->name, CSTR_VIEW("notebook")))
		names = &ac->lists[AUTOCOMPLETE_NOTEBOOKS];
	else if (cstr_view_equal(property->name, CSTR_VIEW("file"))) {
		kind = AUTOCOMPLETE_FILE;
		error = autocomplete_note_paths(ac, line, entry->tool.name, &names);
		if (error)
			return error;
	}
	flags = AUTOCOMPLETE_STRING | (flags & AUTOCOMPLETE_STRING ? 0 : AUTOCOMPLETE_QUOTED);
	for (size_t i = 0; names && i < names->count; ++i) {
		error = autocomplete_value_match(ac, line, current, prefix, cstr_view_dstr(&names->values[i]), flags, mark, kind);
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
	for (size_t i = 2; i < ac->token_count; ++i) {
		struct autocomplete_token token = ac->tokens[i];
		if (i != except && token.equals != SIZE_MAX &&
		    cstr_view_equal(name, cstr_view_slice(line, token.begin, token.equals - token.begin)))
			return true;
	}
	return false;
}

static int
autocomplete_commands (struct autocomplete      *ac,
                       struct cstr_view          line,
                       struct autocomplete_token current,
                       size_t                    cursor,
                       size_t                    index)
{
	struct cstr_view prefix = cstr_view_slice(line, current.begin, cursor - current.begin);
	if (!index) {
		struct cli_command_matches commands = cli_command_prefix(prefix);
		for (size_t i = 0; i < commands.count; ++i) {
			struct cli_command const *command = &commands.data[i];
			int error = autocomplete_replacement(ac, line, current.begin, current.end, command->name, true, AUTOCOMPLETE_COMMAND);
			if (error)
				return error;
		}
		return 0;
	}
	if (!ac->token_count)
		return 0;
	struct autocomplete_token command_token = ac->tokens[0];
	struct cli_command const *command = cli_command_find(cstr_view_slice(line, command_token.begin, command_token.end - command_token.begin));
	if (!command || (command->args != CLI_COMMAND_ARGS_TOOL && command->args != CLI_COMMAND_ARGS_TOOL_CALL))
		return 0;
	bool call = command->args == CLI_COMMAND_ARGS_TOOL_CALL;
	if (index == 1 || ac->token_count == 1) {
		for (size_t i = 0; i < ac->count; ++i) {
			struct cstr_view name = ac->entries[i].tool.name;
			if (!cstr_view_starts(name, prefix))
				continue;
			int error = autocomplete_replacement(ac, line, current.begin, current.end, name, true, AUTOCOMPLETE_TOOL);
			if (error)
				return error;
		}
		return 0;
	}
	if (!call || ac->token_count < 2 || (prefix.len && prefix.data[0] == '{'))
		return 0;
	struct autocomplete_token tool_token = ac->tokens[1];
	struct autocomplete_entry const *entry = autocomplete_entry_find(ac, cstr_view_slice(line, tool_token.begin, tool_token.end - tool_token.begin), nullptr);
	if (!entry)
		return 0;
	if (current.equals < cursor) {
		struct cstr_view key = cstr_view_slice(line, current.begin, current.equals - current.begin);
		return autocomplete_values(ac, line, current, cursor, entry, autocomplete_entry_property(entry, key));
	}
	for (size_t i = 0; i < entry->count; ++i) {
		struct cstr_view name = entry->properties[i].name;
		if (!cstr_view_starts(name, prefix) ||
		    autocomplete_used(ac, line, name,
		                     current.end > current.begin ? index : SIZE_MAX))
			continue;
		int error = cstr_view_store(name, &ac->quoted);
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
	bool separate = end == line.len || autocomplete_space((unsigned char)line.data[end]);
	struct autocomplete_names const *names = &ac->lists[AUTOCOMPLETE_CHAT_PARTICIPANTS];
	for (size_t i = 0; i < names->count; ++i) {
		struct cstr_view name = cstr_view_dstr(&names->values[i]);
		if (!cstr_view_starts(name, prefix))
			continue;
		(void)dstr_clear(&ac->quoted);
		if (mention && !dstr_cat_n(&ac->quoted, "@", 1))
			return ENOMEM;
		if (!dstr_cat_n(&ac->quoted, name.data, name.len))
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
 * qsort may allocate a merge buffer even when all strings are borrowed. */
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

void
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
	if (!autocomplete_line(line) || cursor > line.len ||
	    (cursor < line.len && ((unsigned char)line.data[cursor] & 0xc0) == 0x80))
		return EINVAL;
	if (!line.data)
		line.data = "";
	ac->match_count = 0;
	(void)dstr_clear(&ac->lines);
	int error;
	if (line.len && line.data[0] == '/') {
		error = autocomplete_tokenize(ac, line, false, nullptr);
		if (error)
			return error;
		struct autocomplete_token current = {cursor, cursor, SIZE_MAX};
		size_t index = 0;
		for (size_t i = 0; i < ac->token_count; ++i) {
			struct autocomplete_token token = ac->tokens[i];
			index = i;
			if (cursor < token.begin)
				break;
			if (cursor <= token.end) {
				current = token;
				break;
			}
			index = i + 1;
		}
		error = autocomplete_commands(ac, line, current, cursor, index);
	} else
		error = autocomplete_mentions(ac, line, cursor);
	if (error)
		return error;
	for (size_t i = 0; i < ac->match_count; ++i)
		ac->matches[i].line.data = dstr_get(&ac->lines) + ac->offsets[i];
	autocomplete_match_sort(ac->matches, ac->match_count, ac->match_begin);
	size_t used = 0;
	for (size_t i = 0; i < ac->match_count; ++i) {
		if (used && !autocomplete_match_compare(&ac->matches[used - 1], &ac->matches[i], ac->match_begin))
			continue;
		ac->matches[used++] = ac->matches[i];
	}
	*matches = ac->matches;
	*count = used;
	return 0;
}

static int
autocomplete_entry_validate (struct autocomplete_entry const *entry,
                             struct json_object              *arguments,
                             struct autocomplete_error       *diagnostic)
{
	struct json_object *required = jb_get(entry->root, "required");
	if (json_object_is_type(required, json_type_array))
		for (size_t i = 0, count = json_object_array_length(required); i < count; ++i) {
			struct cstr_view key = autocomplete_json_text(json_object_array_get_idx(required, i));
			struct json_object *unused = nullptr;
			if (!json_object_object_get_ex(arguments, key.data, &unused))
				return autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Missing required tool argument: "), key);
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
		error = autocomplete_json_parse(autocomplete, decoded, converted);
		if (!error) {
			if (!(flags & AUTOCOMPLETE_STRING) || autocomplete_accepted_type(flags, *converted))
				return 0;
		} else if (error != EINVAL || (flags & (AUTOCOMPLETE_STRING | AUTOCOMPLETE_TYPED)) == AUTOCOMPLETE_TYPED)
			return error;
	}
	json_object_put(*converted);
	*converted = nullptr;
	if (decoded.len > INT_MAX)
		return E2BIG;
	*converted = json_object_new_string_len(decoded.data, (int)decoded.len);
	return *converted ? 0 : ENOMEM;
}

int
autocomplete_parse_call (struct autocomplete             *ac,
                         struct cstr_view                 line,
                         struct autocomplete_tool const **tool,
                         struct json_object             **arguments,
                         struct autocomplete_error       *diagnostic)
{
	*tool = nullptr;
	*arguments = nullptr;
	(void)autocomplete_diagnose(diagnostic, 0, CSTR_VIEW(""), CSTR_VIEW(""));
	if (line.len > AUTOCOMPLETE_BYTES)
		return autocomplete_diagnose(diagnostic, E2BIG, CSTR_VIEW("Tool command is too long"), CSTR_VIEW(""));
	if (!autocomplete_line(line))
		return autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Invalid tool command bytes"), CSTR_VIEW(""));
	if (!line.data)
		line.data = "";
	int error = autocomplete_tokenize(ac, line, true, diagnostic);
	if (error)
		return error;
	struct cli_command const *command = nullptr;
	if (ac->token_count) {
		struct autocomplete_token token = ac->tokens[0];
		command = cli_command_find(cstr_view_slice(line, token.begin, token.end - token.begin));
	}
	if (ac->token_count < 2 || !command || command->id != CLI_COMMAND_TOOL)
		return autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Usage: "), cli_command_at(CLI_COMMAND_TOOL)->usage);
	struct autocomplete_token name_token = ac->tokens[1];
	struct cstr_view name = cstr_view_slice(line, name_token.begin, name_token.end - name_token.begin);
	struct autocomplete_entry const *entry = autocomplete_entry_find(ac, name, nullptr);
	if (!entry)
		return autocomplete_diagnose(diagnostic, ENOENT, CSTR_VIEW("Unknown or inaccessible tool: "), name);
	struct json_object *result = nullptr;
	if (ac->token_count > 2 && line.data[ac->tokens[2].begin] == '{') {
		error = autocomplete_json_parse(ac, cstr_view_slice(line, ac->tokens[2].begin, line.len - ac->tokens[2].begin), &result);
		if (error)
			return autocomplete_diagnose(diagnostic, error, CSTR_VIEW("Invalid JSON tool argument"), CSTR_VIEW(""));
		if (!json_object_is_type(result, json_type_object)) {
			error = autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Tool arguments must be a JSON object"), CSTR_VIEW(""));
			goto fail;
		}
	} else {
		result = json_object_new_object();
		if (!result)
			return ENOMEM;
		struct json_object *additional = jb_get(entry->root, "additionalProperties");
		for (size_t i = 2; i < ac->token_count; ++i) {
			struct autocomplete_token token = ac->tokens[i];
			if (token.equals == SIZE_MAX || !autocomplete_safe(cstr_view_slice(line, token.begin, token.equals - token.begin), true)) {
				error = autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Expected a tool argument as name=value"), CSTR_VIEW(""));
				goto fail;
			}
			struct cstr_view key = cstr_view_slice(line, token.begin, token.equals - token.begin);
			error = cstr_view_store(key, &ac->key);
			if (error)
				goto fail;
			struct json_object *existing = nullptr;
			if (json_object_object_get_ex(result, dstr_get(&ac->key), &existing)) {
				error = autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Duplicate tool argument: "), key);
				goto fail;
			}
			struct autocomplete_property const *property = autocomplete_entry_property(entry, key);
			if (!property && json_object_is_type(additional, json_type_boolean) && !json_object_get_boolean(additional)) {
				error = autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Unknown tool argument: "), key);
				goto fail;
			}
			uint64_t flags = property ? property->flags : 0;
			struct json_object *converted = nullptr;
			error = autocomplete_convert(ac, cstr_view_slice(line, token.equals + 1, token.end - token.equals - 1), flags, &converted);
			if (error) {
				error = autocomplete_diagnose(diagnostic, error, CSTR_VIEW("Invalid value for tool argument: "), key);
				goto fail;
			}
			if (property && !autocomplete_accepted_type(flags, converted)) {
				json_object_put(converted);
				converted = nullptr;
				error = autocomplete_diagnose(diagnostic, EINVAL, CSTR_VIEW("Wrong type for tool argument: "), key);
				goto fail;
			}
			if (json_object_object_add(result, dstr_get(&ac->key), converted)) {
				json_object_put(converted);
				converted = nullptr;
				error = ENOMEM;
				goto fail;
			}
		}
		error = autocomplete_entry_validate(entry, result, diagnostic);
		if (error)
			goto fail;
	}
	*tool = &entry->tool;
	*arguments = result;
	return 0;
fail:
	json_object_put(result);
	result = nullptr;
	return error;
}
