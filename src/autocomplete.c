/** @file Offline completion with connection-lifetime working storage. */
#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "autocomplete.h"
#include "dstr.h"
#include "jb.h"

#define AUTOCOMPLETE_TEXT(s) ((struct autocomplete_text){(s), sizeof (s) - 1})

enum { AUTOCOMPLETE_BYTES = 16 * 1024 * 1024 };

enum autocomplete_candidate_flag {
	AUTOCOMPLETE_STRING = 1,
	AUTOCOMPLETE_QUOTED = 2,
	AUTOCOMPLETE_VALID  = 4,
};

struct autocomplete_candidate {
	struct dstr value;
	uint64_t    flags;
};

struct autocomplete_property {
	struct autocomplete_text       name;
	struct json_object            *schema;
	struct autocomplete_candidate *values;
	size_t                         count;
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

struct autocomplete_token {
	size_t begin;
	size_t end;
	size_t equals;
};

struct autocomplete {
	struct autocomplete_entry *entries;
	size_t                     count;
	size_t                     capacity;
	size_t                     storage;
	struct autocomplete_names  participants;
	struct autocomplete_names  notebooks;
	struct autocomplete_token *tokens;
	size_t                     token_count;
	size_t                     token_capacity;
	struct json_tokener       *parser;
	struct dstr                nesting;
	struct dstr                key;
	struct dstr                decoded;
	struct dstr                quoted;
	struct dstr                json;
	struct dstr                lines;
	struct autocomplete_match  matches[256];
	size_t                     offsets[256];
	size_t                     match_count;
	size_t                     match_begin;
};

static struct autocomplete_text
autocomplete_dstr (struct dstr const *value)
{
	return (struct autocomplete_text){dstr_get(value), dstr_len(value)};
}

static struct autocomplete_text
autocomplete_slice (struct autocomplete_text value,
                    size_t                   begin,
                    size_t                   end)
{
	return (struct autocomplete_text){value.data + begin, end - begin};
}

static struct json_object *
autocomplete_get (struct json_object *object,
                  char const         *key)
{
	struct json_object *value = nullptr;
	return object && json_object_object_get_ex(object, key, &value) ? value : nullptr;
}

static struct autocomplete_text
autocomplete_json_text (struct json_object *value)
{
	return json_object_is_type(value, json_type_string)
	       ? (struct autocomplete_text){json_object_get_string(value), (size_t)json_object_get_string_len(value)}
	       : AUTOCOMPLETE_TEXT("");
}

static int
autocomplete_compare (struct autocomplete_text left,
                      struct autocomplete_text right)
{
	size_t common = left.len < right.len ? left.len : right.len;
	int difference = common ? memcmp(left.data, right.data, common) : 0;
	return difference ? difference : (left.len > right.len) - (left.len < right.len);
}

static bool
autocomplete_equal (struct autocomplete_text left,
                    struct autocomplete_text right)
{
	return left.len == right.len && (!left.len || !memcmp(left.data, right.data, left.len));
}

static bool
autocomplete_starts (struct autocomplete_text value,
                     struct autocomplete_text prefix)
{
	return value.len >= prefix.len && (!prefix.len || !memcmp(value.data, prefix.data, prefix.len));
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

static bool
autocomplete_safe (struct autocomplete_text value,
                   bool                     identifier)
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
                       struct autocomplete_text   message,
                       struct autocomplete_text   detail)
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
autocomplete_set (struct dstr              *output,
                  struct autocomplete_text value)
{
	(void)dstr_clear(output);
	return dstr_cat_n(output, value.data, value.len) ? 0 : ENOMEM;
}

static int
autocomplete_unquote (struct dstr              *output,
                      struct autocomplete_text value)
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
autocomplete_quote (struct dstr              *output,
                    struct autocomplete_text value,
                    char                     preferred)
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
		return autocomplete_set(output, value);
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
autocomplete_tokenize (struct autocomplete       *autocomplete,
                       struct autocomplete_text  line,
                       bool                      strict,
                       struct autocomplete_error *diagnostic)
{
	autocomplete->token_count = 0;
	for (size_t i = 0; i < line.len;) {
		if (autocomplete_space((unsigned char)line.data[i])) {
			++i;
			continue;
		}
		size_t begin = i;
		size_t equals = SIZE_MAX;
		char mark = 0;
		bool escape = false;
		(void)dstr_clear(&autocomplete->nesting);
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
				if (autocomplete_buffer_reserve(&autocomplete->nesting, 1))
					return ENOMEM;
				if (!dstr_cat_n(&autocomplete->nesting, &byte, 1))
					return ENOMEM;
			} else if (byte == ']' || byte == '}') {
				size_t depth = dstr_len(&autocomplete->nesting);
				if (depth && dstr_get(&autocomplete->nesting)[depth - 1] == (byte == ']' ? '[' : '{'))
					(void)dstr_erase(&autocomplete->nesting, depth - 1, 1);
				else if (strict)
					return autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Unmatched bracket in tool arguments"), AUTOCOMPLETE_TEXT(""));
			} else if (autocomplete_space((unsigned char)byte) && !dstr_len(&autocomplete->nesting))
				break;
		}
		if (strict && (mark || escape || dstr_len(&autocomplete->nesting)))
			return autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Unterminated quote, escape or JSON value in tool arguments"), AUTOCOMPLETE_TEXT(""));
		struct autocomplete_token *tokens = autocomplete_grow(autocomplete->tokens, &autocomplete->token_capacity,
		                                                      autocomplete->token_count + 1, sizeof *tokens);
		if (!tokens)
			return ENOMEM;
		autocomplete->tokens = tokens;
		tokens[autocomplete->token_count++] = (struct autocomplete_token){begin, i, equals};
	}
	return 0;
}

static int
autocomplete_json_parse (struct autocomplete       *autocomplete,
                         struct autocomplete_text  value,
                         struct json_object       **result)
{
	*result = nullptr;
	if (value.len >= INT_MAX)
		return E2BIG;
	int error = autocomplete_set(&autocomplete->json, value);
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
		struct autocomplete_text ref = autocomplete_json_text(autocomplete_get(*schema, "$ref"));
		if (!autocomplete_starts(ref, AUTOCOMPLETE_TEXT("#/")))
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
			next = autocomplete_get(next, dstr_get(&autocomplete->key));
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

static bool
autocomplete_has_type (struct json_object       *schema,
                       struct autocomplete_text type)
{
	struct json_object *value = autocomplete_get(schema, "type");
	if (json_object_is_type(value, json_type_string))
		return autocomplete_equal(autocomplete_json_text(value), type);
	if (json_object_is_type(value, json_type_array))
		for (size_t i = 0, count = json_object_array_length(value); i < count; ++i)
			if (autocomplete_equal(autocomplete_json_text(json_object_array_get_idx(value, i)), type))
				return true;
	return false;
}

static bool
autocomplete_accepted_type (struct json_object *schema,
                            struct json_object *value)
{
	if (!autocomplete_get(schema, "type"))
		return true;
	switch (json_object_get_type(value)) {
	case json_type_null: return autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("null"));
	case json_type_boolean: return autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("boolean"));
	case json_type_double: {
		double number = json_object_get_double(value);
		return autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("number")) ||
		       (autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("integer")) && isfinite(number) && floor(number) == number);
	}
	case json_type_int: return autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("integer")) || autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("number"));
	case json_type_object: return autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("object"));
	case json_type_array: return autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("array"));
	case json_type_string: return autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("string"));
	}
	return false;
}

static bool
autocomplete_string_schema (struct json_object *schema)
{
	if (autocomplete_get(schema, "type"))
		return autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("string"));
	if (json_object_is_type(autocomplete_get(schema, "const"), json_type_string))
		return true;
	struct json_object *values = autocomplete_get(schema, "enum");
	if (!json_object_is_type(values, json_type_array) || !json_object_array_length(values))
		return false;
	for (size_t i = 0, count = json_object_array_length(values); i < count; ++i)
		if (!json_object_is_type(json_object_array_get_idx(values, i), json_type_string))
			return false;
	return true;
}

static struct autocomplete_entry const *
autocomplete_entry_find (struct autocomplete const *autocomplete,
                         struct autocomplete_text   name,
                         size_t                    *position)
{
	size_t begin = 0;
	size_t end = autocomplete->count;
	while (begin < end) {
		size_t middle = begin + (end - begin) / 2;
		if (autocomplete_compare(autocomplete->entries[middle].tool.name, name) < 0)
			begin = middle + 1;
		else
			end = middle;
	}
	if (position)
		*position = begin;
	return begin < autocomplete->count && autocomplete_equal(autocomplete->entries[begin].tool.name, name)
	       ? &autocomplete->entries[begin] : nullptr;
}

static struct autocomplete_property const *
autocomplete_entry_property (struct autocomplete_entry const *entry,
                             struct autocomplete_text         name)
{
	for (size_t i = 0; i < entry->count; ++i)
		if (autocomplete_equal(entry->properties[i].name, name))
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
                             struct json_object            *schema)
{
	*candidate = (struct autocomplete_candidate){.value = dstr(nullptr)};
	bool string = json_object_is_type(value, json_type_string);
	bool quoted = string &&
	              (json_object_is_type(autocomplete_get(schema, "type"), json_type_array) || !autocomplete_string_schema(schema));
	candidate->flags = (string ? AUTOCOMPLETE_STRING : 0) | (quoted ? AUTOCOMPLETE_QUOTED : 0);
	struct autocomplete_text text = autocomplete_json_text(value);
	if (!string) {
		text.data = json_object_to_json_string_length(value, JSON_C_TO_STRING_PLAIN, &text.len);
		if (!text.data)
			return ENOMEM;
	}
	if (text.len && !autocomplete_safe(text, false))
		return 0;
	candidate->flags |= AUTOCOMPLETE_VALID;
	return autocomplete_set(&candidate->value, text);
}

static int
autocomplete_property_init (struct autocomplete_property *property,
                            struct autocomplete          *autocomplete,
                            struct json_object           *root,
                            struct json_object           *schema,
                            struct autocomplete_text      name)
{
	*property = (struct autocomplete_property){.name = name, .schema = schema};
	int error = autocomplete_resolve(autocomplete, root, &property->schema);
	if (error)
		return error;
	schema = property->schema;
	struct json_object *constant = nullptr;
	bool has_constant = schema && json_object_object_get_ex(schema, "const", &constant);
	struct json_object *enumeration = autocomplete_get(schema, "enum");
	bool has_enum = json_object_is_type(enumeration, json_type_array);
	size_t count = has_constant ? 1 : has_enum ? json_object_array_length(enumeration) : 0;
	if (!count)
		return 0;
	if (count > SIZE_MAX / sizeof *property->values)
		return ENOMEM;
	property->values = malloc(count * sizeof *property->values);
	if (!property->values)
		return ENOMEM;
	for (size_t i = 0; i < count; ++i) {
		struct json_object *value = has_constant ? constant : json_object_array_get_idx(enumeration, i);
		error = autocomplete_candidate_init(&property->values[i], value, schema);
		++property->count;
		if (error)
			return error;
	}
	return 0;
}

struct autocomplete *
autocomplete_create (int *e)
{
	struct autocomplete *autocomplete = malloc(sizeof *autocomplete);
	if (!autocomplete) {
		if (e)
			*e = ENOMEM;
		return nullptr;
	}
	*autocomplete = (struct autocomplete){
		.nesting = dstr(nullptr), .key = dstr(nullptr), .decoded = dstr(nullptr),
		.quoted = dstr(nullptr), .json = dstr(nullptr), .lines = dstr(nullptr),
	};
	if (e)
		*e = 0;
	return autocomplete;
}

void
autocomplete_clear_tools (struct autocomplete *autocomplete)
{
	for (size_t i = 0; i < autocomplete->count; ++i)
		autocomplete_entry_fini(&autocomplete->entries[i]);
	autocomplete->count = 0;
	autocomplete->storage = 0;
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
autocomplete_destroy (struct autocomplete **p_autocomplete)
{
	if (!p_autocomplete || !*p_autocomplete)
		return;
	struct autocomplete *autocomplete = *p_autocomplete;
	autocomplete_clear_tools(autocomplete);
	free(autocomplete->entries);
	autocomplete->entries = nullptr;
	free(autocomplete->tokens);
	autocomplete->tokens = nullptr;
	autocomplete_names_fini(&autocomplete->participants);
	autocomplete_names_fini(&autocomplete->notebooks);
	if (autocomplete->parser)
		json_tokener_free(autocomplete->parser);
	autocomplete->parser = nullptr;
	dstr_cleanup(&autocomplete->nesting);
	dstr_cleanup(&autocomplete->key);
	dstr_cleanup(&autocomplete->decoded);
	dstr_cleanup(&autocomplete->quoted);
	dstr_cleanup(&autocomplete->json);
	dstr_cleanup(&autocomplete->lines);
	free(*p_autocomplete);
	*p_autocomplete = nullptr;
}

int
autocomplete_add_tool (struct autocomplete *autocomplete,
                       struct json_object  *definition)
{
	struct autocomplete_text name = autocomplete_json_text(autocomplete_get(definition, "name"));
	if (!autocomplete_safe(name, true))
		return 0;
	size_t position = 0;
	struct autocomplete_entry const *previous = autocomplete_entry_find(autocomplete, name, &position);
	bool replace = previous != nullptr;
	struct autocomplete_entry entry = {
		.tool = {name, autocomplete_json_text(autocomplete_get(definition, "title")),
		         autocomplete_json_text(autocomplete_get(definition, "description")), autocomplete_get(definition, "inputSchema")},
		.definition = json_object_get(definition),
		.root = autocomplete_get(definition, "inputSchema"),
	};
	size_t schema_size = 0;
	int error = 0;
	if (entry.root && !json_object_to_json_string_length(entry.root, JSON_C_TO_STRING_PLAIN, &schema_size)) {
		error = ENOMEM;
		goto fail;
	}
	entry.storage = name.len + entry.tool.title.len + entry.tool.description.len + schema_size;
	size_t remaining = autocomplete->storage - (previous ? previous->storage : 0);
	if ((!replace && autocomplete->count >= 4096) || entry.storage > AUTOCOMPLETE_BYTES || remaining > AUTOCOMPLETE_BYTES - entry.storage) {
		error = E2BIG;
		goto fail;
	}
	error = autocomplete_resolve(autocomplete, entry.root, &entry.root);
	if (error)
		goto fail;
	struct json_object *properties = autocomplete_get(entry.root, "properties");
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
			struct autocomplete_text property_name = {iterator.key, strlen(iterator.key)};
			if (!autocomplete_safe(property_name, true))
				continue;
			error = autocomplete_property_init(&entry.properties[entry.count], autocomplete, entry.tool.schema, iterator.val, property_name);
			++entry.count;
			if (error)
				goto fail;
		}
	}
	if (!replace) {
		struct autocomplete_entry *entries = autocomplete_grow(autocomplete->entries, &autocomplete->capacity,
		                                                      autocomplete->count + 1, sizeof *entries);
		if (!entries) {
			error = ENOMEM;
			goto fail;
		}
		autocomplete->entries = entries;
		memmove(entries + position + 1, entries + position, (autocomplete->count - position) * sizeof *entries);
		++autocomplete->count;
	} else
		autocomplete_entry_fini(&autocomplete->entries[position]);
	autocomplete->entries[position] = entry;
	autocomplete->storage = remaining + entry.storage;
	return 0;
fail:
	autocomplete_entry_fini(&entry);
	return error;
}

size_t
autocomplete_tool_count (struct autocomplete const *autocomplete)
{
	return autocomplete->count;
}

struct autocomplete_tool const *
autocomplete_tool_at (struct autocomplete const *autocomplete,
                      size_t                     index)
{
	return index < autocomplete->count ? &autocomplete->entries[index].tool : nullptr;
}

struct autocomplete_tool const *
autocomplete_find_tool (struct autocomplete const *autocomplete,
                        struct autocomplete_text   name)
{
	struct autocomplete_entry const *entry = autocomplete_entry_find(autocomplete, name, nullptr);
	return entry ? &entry->tool : nullptr;
}

static int
autocomplete_names_compare (void const *left,
                            void const *right)
{
	return autocomplete_compare(autocomplete_dstr(left), autocomplete_dstr(right));
}

static int
autocomplete_names_set (struct autocomplete_names      *names,
                        struct autocomplete_text const *values,
                        size_t                          count)
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
		if (!autocomplete_safe(values[i], true))
			continue;
		int error = autocomplete_set(&names->values[names->count], values[i]);
		if (error)
			return error;
		++names->count;
	}
	if (names->count > 1)
		qsort(names->values, names->count, sizeof *names->values, autocomplete_names_compare);
	size_t used = 0;
	for (size_t i = 0; i < names->count; ++i) {
		if (used && autocomplete_equal(autocomplete_dstr(&names->values[used - 1]), autocomplete_dstr(&names->values[i])))
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
autocomplete_set_participants (struct autocomplete            *autocomplete,
                               struct autocomplete_text const *names,
                               size_t                          count)
{
	return autocomplete_names_set(&autocomplete->participants, names, count);
}

int
autocomplete_set_notebooks (struct autocomplete            *autocomplete,
                            struct autocomplete_text const *names,
                            size_t                          count)
{
	return autocomplete_names_set(&autocomplete->notebooks, names, count);
}

size_t
autocomplete_participant_count (struct autocomplete const *autocomplete)
{
	return autocomplete->participants.count;
}

struct autocomplete_text
autocomplete_participant_at (struct autocomplete const *autocomplete,
                             size_t                     index)
{
	return index < autocomplete->participants.count ? autocomplete_dstr(&autocomplete->participants.values[index]) : AUTOCOMPLETE_TEXT("");
}

static int
autocomplete_replacement (struct autocomplete       *autocomplete,
                          struct autocomplete_text  line,
                          size_t                    begin,
                          size_t                    end,
                          struct autocomplete_text  value,
                          bool                      separate)
{
	if (autocomplete->match_count == sizeof autocomplete->matches / sizeof autocomplete->matches[0])
		return 0;
	bool space = separate && (end == line.len || !autocomplete_space((unsigned char)line.data[end]));
	if (value.len > SIZE_MAX - line.len - 2)
		return ENOMEM;
	size_t length = begin + value.len + space + line.len - end;
	size_t offset = dstr_len(&autocomplete->lines);
	if (length >= AUTOCOMPLETE_BYTES - offset)
		return 0;
	if (autocomplete_buffer_reserve(&autocomplete->lines, length + 1))
		return ENOMEM;
	(void)dstr_cat_n(&autocomplete->lines, line.data, begin);
	(void)dstr_cat_n(&autocomplete->lines, value.data, value.len);
	if (space)
		(void)dstr_cat_n(&autocomplete->lines, " ", 1);
	(void)dstr_cat_n(&autocomplete->lines, line.data + end, line.len - end);
	(void)dstr_cat_n(&autocomplete->lines, "", 1);
	size_t index = autocomplete->match_count++;
	if (!index || begin < autocomplete->match_begin)
		autocomplete->match_begin = begin;
	autocomplete->offsets[index] = offset;
	autocomplete->matches[index] = (struct autocomplete_match){{nullptr, length}, begin + value.len + space};
	return 0;
}

static int
autocomplete_value_match (struct autocomplete       *autocomplete,
                          struct autocomplete_text  line,
                          struct autocomplete_token current,
                          struct autocomplete_text  prefix,
                          struct autocomplete_text  value,
                          bool                      string,
                          bool                      quoted,
                          char                      mark)
{
	if (!autocomplete_starts(value, prefix))
		return 0;
	if (string) {
		int error = autocomplete_quote(&autocomplete->quoted, value, mark ? mark : quoted ? '\'' : 0);
		if (error)
			return error == E2BIG ? 0 : error;
		value = autocomplete_dstr(&autocomplete->quoted);
	}
	return autocomplete_replacement(autocomplete, line, current.equals + 1, current.end, value, true);
}

static int
autocomplete_values (struct autocomplete                *autocomplete,
                     struct autocomplete_text           line,
                     struct autocomplete_token          current,
                     size_t                             cursor,
                     struct autocomplete_property const *property)
{
	if (!property)
		return 0;
	struct json_object *schema = property->schema;
	struct autocomplete_text input = autocomplete_slice(line, current.equals + 1, cursor);
	int error = autocomplete_unquote(&autocomplete->decoded, input);
	if (error)
		return error;
	struct autocomplete_text prefix = autocomplete_dstr(&autocomplete->decoded);
	char mark = input.len && (input.data[0] == '\'' || input.data[0] == '"') ? input.data[0] : 0;
	struct json_object *constant = nullptr;
	bool has_constant = schema && json_object_object_get_ex(schema, "const", &constant);
	bool has_enum = json_object_is_type(autocomplete_get(schema, "enum"), json_type_array);
	for (size_t i = 0; i < property->count; ++i) {
		struct autocomplete_candidate const *candidate = &property->values[i];
		if (!(candidate->flags & AUTOCOMPLETE_VALID))
			continue;
		error = autocomplete_value_match(autocomplete, line, current, prefix, autocomplete_dstr(&candidate->value),
		                                 candidate->flags & AUTOCOMPLETE_STRING, candidate->flags & AUTOCOMPLETE_QUOTED, mark);
		if (error)
			return error;
	}
	if (!has_constant && !has_enum) {
		if (autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("boolean"))) {
			error = autocomplete_value_match(autocomplete, line, current, prefix, AUTOCOMPLETE_TEXT("false"), false, false, mark);
			if (!error)
				error = autocomplete_value_match(autocomplete, line, current, prefix, AUTOCOMPLETE_TEXT("true"), false, false, mark);
			if (error)
				return error;
		}
		if (autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("null"))) {
			error = autocomplete_value_match(autocomplete, line, current, prefix, AUTOCOMPLETE_TEXT("null"), false, false, mark);
			if (error)
				return error;
		}
	}
	bool names_allowed = schema && (!autocomplete_get(schema, "type") || autocomplete_has_type(schema, AUTOCOMPLETE_TEXT("string"))) &&
	                     !autocomplete_get(schema, "enum") && !has_constant;
	if (!names_allowed)
		return 0;
	struct autocomplete_names const *names = nullptr;
	if (autocomplete_equal(property->name, AUTOCOMPLETE_TEXT("participant")) ||
	    autocomplete_equal(property->name, AUTOCOMPLETE_TEXT("recipient")) ||
	    autocomplete_equal(property->name, AUTOCOMPLETE_TEXT("author")))
		names = &autocomplete->participants;
	else if (autocomplete_equal(property->name, AUTOCOMPLETE_TEXT("notebook")))
		names = &autocomplete->notebooks;
	bool quoted = !autocomplete_string_schema(schema);
	for (size_t i = 0; names && i < names->count; ++i) {
		error = autocomplete_value_match(autocomplete, line, current, prefix, autocomplete_dstr(&names->values[i]), true, quoted, mark);
		if (error)
			return error;
	}
	return 0;
}

static bool
autocomplete_used (struct autocomplete const *autocomplete,
                   struct autocomplete_text   line,
                   struct autocomplete_text   name,
                   size_t                     except)
{
	for (size_t i = 2; i < autocomplete->token_count; ++i) {
		struct autocomplete_token token = autocomplete->tokens[i];
		if (i != except && token.equals != SIZE_MAX &&
		    autocomplete_equal(name, autocomplete_slice(line, token.begin, token.equals)))
			return true;
	}
	return false;
}

static int
autocomplete_commands (struct autocomplete       *autocomplete,
                       struct autocomplete_text  line,
                       struct autocomplete_token current,
                       size_t                    cursor,
                       size_t                    index)
{
	struct autocomplete_text prefix = autocomplete_slice(line, current.begin, cursor);
	if (!index) {
		static struct autocomplete_text const commands[] = {
			AUTOCOMPLETE_TEXT("/clear"), AUTOCOMPLETE_TEXT("/exit"), AUTOCOMPLETE_TEXT("/help"),
			AUTOCOMPLETE_TEXT("/names"), AUTOCOMPLETE_TEXT("/read"), AUTOCOMPLETE_TEXT("/refresh"),
			AUTOCOMPLETE_TEXT("/regen"), AUTOCOMPLETE_TEXT("/tool"), AUTOCOMPLETE_TEXT("/tools"),
		};
		for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
			if (!autocomplete_starts(commands[i], prefix))
				continue;
			bool separate = autocomplete_equal(commands[i], AUTOCOMPLETE_TEXT("/tool")) || autocomplete_equal(commands[i], AUTOCOMPLETE_TEXT("/read"));
			int error = autocomplete_replacement(autocomplete, line, current.begin, current.end, commands[i], separate);
			if (error)
				return error;
		}
		return 0;
	}
	if (!autocomplete->token_count)
		return 0;
	struct autocomplete_token command_token = autocomplete->tokens[0];
	struct autocomplete_text command = autocomplete_slice(line, command_token.begin, command_token.end);
	bool call = autocomplete_equal(command, AUTOCOMPLETE_TEXT("/tool"));
	if (!call && !autocomplete_equal(command, AUTOCOMPLETE_TEXT("/tools")))
		return 0;
	if (index == 1 || autocomplete->token_count == 1) {
		for (size_t i = 0; i < autocomplete->count; ++i) {
			struct autocomplete_text name = autocomplete->entries[i].tool.name;
			if (!autocomplete_starts(name, prefix))
				continue;
			int error = autocomplete_replacement(autocomplete, line, current.begin, current.end, name, call);
			if (error)
				return error;
		}
		return 0;
	}
	if (!call || autocomplete->token_count < 2 || (prefix.len && prefix.data[0] == '{'))
		return 0;
	struct autocomplete_token tool_token = autocomplete->tokens[1];
	struct autocomplete_entry const *entry = autocomplete_entry_find(autocomplete, autocomplete_slice(line, tool_token.begin, tool_token.end), nullptr);
	if (!entry)
		return 0;
	if (current.equals < cursor) {
		struct autocomplete_text key = autocomplete_slice(line, current.begin, current.equals);
		return autocomplete_values(autocomplete, line, current, cursor, autocomplete_entry_property(entry, key));
	}
	for (size_t i = 0; i < entry->count; ++i) {
		struct autocomplete_text name = entry->properties[i].name;
		if (!autocomplete_starts(name, prefix) || autocomplete_used(autocomplete, line, name, index))
			continue;
		int error = autocomplete_set(&autocomplete->quoted, name);
		if (error)
			return error;
		if (current.equals == SIZE_MAX && !dstr_cat_n(&autocomplete->quoted, "=", 1))
			return ENOMEM;
		error = autocomplete_replacement(autocomplete, line, current.begin, current.equals == SIZE_MAX ? current.end : current.equals,
		                                 autocomplete_dstr(&autocomplete->quoted), false);
		if (error)
			return error;
	}
	return 0;
}

static int
autocomplete_mentions (struct autocomplete      *autocomplete,
                       struct autocomplete_text line,
                       size_t                   cursor)
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
	struct autocomplete_text prefix = autocomplete_slice(line, begin + mention, cursor);
	bool separate = end == line.len || autocomplete_space((unsigned char)line.data[end]);
	for (size_t i = 0; i < autocomplete->participants.count; ++i) {
		struct autocomplete_text name = autocomplete_dstr(&autocomplete->participants.values[i]);
		if (!autocomplete_starts(name, prefix))
			continue;
		(void)dstr_clear(&autocomplete->quoted);
		if (mention && !dstr_cat_n(&autocomplete->quoted, "@", 1))
			return ENOMEM;
		if (!dstr_cat_n(&autocomplete->quoted, name.data, name.len))
			return ENOMEM;
		if (!mention && !begin && separate && !dstr_cat_n(&autocomplete->quoted, ":", 1))
			return ENOMEM;
		int error = autocomplete_replacement(autocomplete, line, begin, end, autocomplete_dstr(&autocomplete->quoted), separate);
		if (error)
			return error;
	}
	return 0;
}

static int
autocomplete_match_compare (struct autocomplete const       *autocomplete,
                            struct autocomplete_match const *a,
                            struct autocomplete_match const *b)
{
	size_t begin = autocomplete->match_begin;
	int difference = autocomplete_compare(autocomplete_slice(a->line, begin, a->line.len),
	                                      autocomplete_slice(b->line, begin, b->line.len));
	return difference ? difference : (a->cursor > b->cursor) - (a->cursor < b->cursor);
}

/** Heapsort keeps the bounded completion table off the allocator. libc
 * qsort may allocate a merge buffer even when all strings are borrowed. */
static void
autocomplete_match_sink (struct autocomplete *autocomplete,
                         size_t               count,
                         size_t               parent)
{
	struct autocomplete_match *matches = autocomplete->matches;
	for (;;) {
		size_t child = parent * 2 + 1;
		if (child >= count)
			return;
		if (child + 1 < count && autocomplete_match_compare(autocomplete, &matches[child], &matches[child + 1]) < 0)
			++child;
		if (autocomplete_match_compare(autocomplete, &matches[parent], &matches[child]) >= 0)
			return;
		struct autocomplete_match swap = matches[parent];
		matches[parent] = matches[child];
		matches[child] = swap;
		parent = child;
	}
}

static void
autocomplete_matches_sort (struct autocomplete *autocomplete)
{
	size_t count = autocomplete->match_count;
	for (size_t i = count / 2; i; --i)
		autocomplete_match_sink(autocomplete, count, i - 1);
	for (size_t i = count; i > 1; --i) {
		struct autocomplete_match swap = autocomplete->matches[0];
		autocomplete->matches[0] = autocomplete->matches[i - 1];
		autocomplete->matches[i - 1] = swap;
		autocomplete_match_sink(autocomplete, i - 1, 0);
	}
}

int
autocomplete_complete (struct autocomplete              *autocomplete,
                       struct autocomplete_text         line,
                       size_t                           cursor,
                       struct autocomplete_match const **matches,
                       size_t                           *count)
{
	*matches = nullptr;
	*count = 0;
	if (line.len > AUTOCOMPLETE_BYTES)
		return E2BIG;
	if ((!line.data && line.len) || cursor > line.len ||
	    (line.len && (memchr(line.data, '\0', line.len) || jb_utf8(line.data, line.len) != line.len)) ||
	    (cursor < line.len && ((unsigned char)line.data[cursor] & 0xc0) == 0x80))
		return EINVAL;
	if (!line.data)
		line.data = "";
	autocomplete->match_count = 0;
	(void)dstr_clear(&autocomplete->lines);
	int error;
	if (line.len && line.data[0] == '/') {
		error = autocomplete_tokenize(autocomplete, line, false, nullptr);
		if (error)
			return error;
		struct autocomplete_token current = {cursor, cursor, SIZE_MAX};
		size_t index = 0;
		for (size_t i = 0; i < autocomplete->token_count; ++i) {
			struct autocomplete_token token = autocomplete->tokens[i];
			index = i;
			if (cursor < token.begin)
				break;
			if (cursor <= token.end) {
				current = token;
				break;
			}
			index = i + 1;
		}
		error = autocomplete_commands(autocomplete, line, current, cursor, index);
	} else
		error = autocomplete_mentions(autocomplete, line, cursor);
	if (error)
		return error;
	for (size_t i = 0; i < autocomplete->match_count; ++i)
		autocomplete->matches[i].line.data = dstr_get(&autocomplete->lines) + autocomplete->offsets[i];
	if (autocomplete->match_count > 1)
		autocomplete_matches_sort(autocomplete);
	size_t used = 0;
	for (size_t i = 0; i < autocomplete->match_count; ++i) {
		if (used && !autocomplete_match_compare(autocomplete, &autocomplete->matches[used - 1], &autocomplete->matches[i]))
			continue;
		autocomplete->matches[used++] = autocomplete->matches[i];
	}
	*matches = autocomplete->matches;
	*count = used;
	return 0;
}

static int
autocomplete_entry_validate (struct autocomplete_entry const *entry,
                       struct json_object             *arguments,
                       struct autocomplete_error      *diagnostic)
{
	struct json_object *required = autocomplete_get(entry->root, "required");
	if (json_object_is_type(required, json_type_array))
		for (size_t i = 0, count = json_object_array_length(required); i < count; ++i) {
			struct autocomplete_text key = autocomplete_json_text(json_object_array_get_idx(required, i));
			struct json_object *unused = nullptr;
			if (!json_object_object_get_ex(arguments, key.data, &unused))
				return autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Missing required tool argument: "), key);
		}
	return 0;
}

static int
autocomplete_convert (struct autocomplete       *autocomplete,
                      struct autocomplete_text  raw,
                      struct json_object       *schema,
                      struct json_object      **converted)
{
	*converted = nullptr;
	bool structured = raw.len && (raw.data[0] == '{' || raw.data[0] == '[');
	int error = structured ? autocomplete_set(&autocomplete->decoded, raw) : autocomplete_unquote(&autocomplete->decoded, raw);
	if (error)
		return error;
	struct autocomplete_text decoded = autocomplete_dstr(&autocomplete->decoded);
	bool string = autocomplete_string_schema(schema);
	bool quoted = raw.len && (raw.data[0] == '\'' || raw.data[0] == '"');
	bool use_string = (quoted && !autocomplete_get(schema, "type")) ||
	                  (string && (quoted || !json_object_is_type(autocomplete_get(schema, "type"), json_type_array)));
	if (!use_string) {
		error = autocomplete_json_parse(autocomplete, decoded, converted);
		if (!error)
			use_string = string && !autocomplete_accepted_type(schema, *converted);
		else if (error != EINVAL || (!string && autocomplete_get(schema, "type")))
			return error;
		else
			use_string = true;
	}
	if (use_string) {
		json_object_put(*converted);
		*converted = nullptr;
		if (decoded.len > INT_MAX)
			return E2BIG;
		*converted = json_object_new_string_len(decoded.data, (int)decoded.len);
		if (!*converted)
			return ENOMEM;
	}
	return 0;
}

int
autocomplete_parse_call (struct autocomplete             *autocomplete,
                         struct autocomplete_text        line,
                         struct autocomplete_tool const **tool,
                         struct json_object             **arguments,
                         struct autocomplete_error       *diagnostic)
{
	*tool = nullptr;
	*arguments = nullptr;
	(void)autocomplete_diagnose(diagnostic, 0, AUTOCOMPLETE_TEXT(""), AUTOCOMPLETE_TEXT(""));
	if (line.len > AUTOCOMPLETE_BYTES)
		return autocomplete_diagnose(diagnostic, E2BIG, AUTOCOMPLETE_TEXT("Tool command is too long"), AUTOCOMPLETE_TEXT(""));
	if ((!line.data && line.len) ||
	    (line.len && (memchr(line.data, '\0', line.len) || jb_utf8(line.data, line.len) != line.len)))
		return autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Invalid tool command bytes"), AUTOCOMPLETE_TEXT(""));
	if (!line.data)
		line.data = "";
	int error = autocomplete_tokenize(autocomplete, line, true, diagnostic);
	if (error)
		return error;
	if (autocomplete->token_count < 2 ||
	    !autocomplete_equal(autocomplete_slice(line, autocomplete->tokens[0].begin, autocomplete->tokens[0].end), AUTOCOMPLETE_TEXT("/tool")))
		return autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Usage: /tool NAME key=VALUE ... or /tool NAME {JSON}"), AUTOCOMPLETE_TEXT(""));
	struct autocomplete_token name_token = autocomplete->tokens[1];
	struct autocomplete_text name = autocomplete_slice(line, name_token.begin, name_token.end);
	struct autocomplete_entry const *entry = autocomplete_entry_find(autocomplete, name, nullptr);
	if (!entry)
		return autocomplete_diagnose(diagnostic, ENOENT, AUTOCOMPLETE_TEXT("Unknown or inaccessible tool: "), name);
	struct json_object *result = nullptr;
	if (autocomplete->token_count > 2 && line.data[autocomplete->tokens[2].begin] == '{') {
		error = autocomplete_json_parse(autocomplete, autocomplete_slice(line, autocomplete->tokens[2].begin, line.len), &result);
		if (error)
			return autocomplete_diagnose(diagnostic, error, AUTOCOMPLETE_TEXT("Invalid JSON tool argument"), AUTOCOMPLETE_TEXT(""));
		if (!json_object_is_type(result, json_type_object)) {
			error = autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Tool arguments must be a JSON object"), AUTOCOMPLETE_TEXT(""));
			goto fail;
		}
	} else {
		result = json_object_new_object();
		if (!result)
			return ENOMEM;
		struct json_object *additional = autocomplete_get(entry->root, "additionalProperties");
		for (size_t i = 2; i < autocomplete->token_count; ++i) {
			struct autocomplete_token token = autocomplete->tokens[i];
			if (token.equals == SIZE_MAX || !autocomplete_safe(autocomplete_slice(line, token.begin, token.equals), true)) {
				error = autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Expected a tool argument as name=value"), AUTOCOMPLETE_TEXT(""));
				goto fail;
			}
			struct autocomplete_text key = autocomplete_slice(line, token.begin, token.equals);
			error = autocomplete_set(&autocomplete->key, key);
			if (error)
				goto fail;
			struct json_object *existing = nullptr;
			if (json_object_object_get_ex(result, dstr_get(&autocomplete->key), &existing)) {
				error = autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Duplicate tool argument: "), key);
				goto fail;
			}
			struct autocomplete_property const *property = autocomplete_entry_property(entry, key);
			if (!property && json_object_is_type(additional, json_type_boolean) && !json_object_get_boolean(additional)) {
				error = autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Unknown tool argument: "), key);
				goto fail;
			}
			struct json_object *schema = property ? property->schema : nullptr;
			struct json_object *converted = nullptr;
			error = autocomplete_convert(autocomplete, autocomplete_slice(line, token.equals + 1, token.end), schema, &converted);
			if (error) {
				error = autocomplete_diagnose(diagnostic, error, AUTOCOMPLETE_TEXT("Invalid value for tool argument: "), key);
				goto fail;
			}
			if (property && !autocomplete_accepted_type(schema, converted)) {
				json_object_put(converted);
				converted = nullptr;
				error = autocomplete_diagnose(diagnostic, EINVAL, AUTOCOMPLETE_TEXT("Wrong type for tool argument: "), key);
				goto fail;
			}
			if (json_object_object_add(result, dstr_get(&autocomplete->key), converted)) {
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

#undef AUTOCOMPLETE_TEXT
