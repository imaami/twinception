/** @file Completion, invocation parsing and retained working storage. */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "autocomplete-file.h"
#include "autocomplete-fixture.h"

/* The linker wraps calls in our objects, including dstr. JSON-c's own
 * allocations remain inside the shared library and are not counted. */
extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t count, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);

static bool autocomplete_count_allocations;
static size_t autocomplete_allocations;
static size_t autocomplete_frees;

void *__wrap_malloc(size_t size)
{
	if (autocomplete_count_allocations)
		++autocomplete_allocations;
	return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
	if (autocomplete_count_allocations)
		++autocomplete_allocations;
	return __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
	if (autocomplete_count_allocations)
		++autocomplete_allocations;
	return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr)
{
	if (autocomplete_count_allocations && ptr)
		++autocomplete_frees;
	__real_free(ptr);
}

#define TEXT(s) ((struct autocomplete_text){s, sizeof(s) - 1})

static void autocomplete_has (struct autocomplete *catalog, struct autocomplete_text input,
                              struct autocomplete_text expected, size_t cursor)
{
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(!autocomplete_complete(catalog, input, cursor, &matches, &count));
	for (size_t i = 0; i < count; ++i) {
		struct autocomplete_match const *match = &matches[i];
		assert(match->cursor <= match->line.len);
		size_t length = match->line.len;
		while (length && match->line.data[length - 1] == ' ')
			--length;
		if (length == expected.len && !memcmp(match->line.data, expected.data, length))
			return;
	}
	fprintf(stderr, "completion missing: [%.*s] -> [%.*s]\n", (int)input.len, input.data,
	        (int)expected.len, expected.data);
	for (size_t i = 0; i < count; ++i)
		fprintf(stderr, "  [%.*s], point %zu\n", (int)matches[i].line.len,
		        matches[i].line.data, matches[i].cursor);
	abort();
}

static void autocomplete_empty (struct autocomplete *catalog, struct autocomplete_text input,
                                size_t cursor)
{
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(!autocomplete_complete(catalog, input, cursor, &matches, &count));
	assert(!count);
}

static struct json_object *autocomplete_parse (struct autocomplete *catalog,
                                               struct autocomplete_text input,
                                               struct autocomplete_text expected_name)
{
	struct autocomplete_tool const *tool = nullptr;
	struct json_object *arguments = nullptr;
	char buffer[256];
	struct autocomplete_error diagnostic = {buffer, sizeof buffer, 0};
	int error = autocomplete_parse_call(catalog, input, &tool, &arguments, &diagnostic);
	if (error)
		fprintf(stderr, "parse failed: [%.*s]: %.*s\n", (int)input.len, input.data,
		        (int)diagnostic.len, diagnostic.data);
	assert(!error && tool);
	assert(tool->name.len == expected_name.len);
	assert(!memcmp(tool->name.data, expected_name.data, expected_name.len));
	assert(arguments && json_object_is_type(arguments, json_type_object));
	return arguments;
}

static void autocomplete_bad (struct autocomplete *catalog, struct autocomplete_text input)
{
	struct autocomplete_tool const *tool = nullptr;
	struct json_object *arguments = nullptr;
	char buffer[13];
	struct autocomplete_error diagnostic = {buffer, sizeof buffer, 0};
	assert(autocomplete_parse_call(catalog, input, &tool, &arguments, &diagnostic));
	assert(!tool && !arguments);
	assert(diagnostic.len && diagnostic.len < sizeof buffer && !buffer[diagnostic.len]);
}

static void autocomplete_string (struct json_object *object, char const *key,
                                 struct autocomplete_text expected)
{
	struct json_object *value = nullptr;
	assert(json_object_object_get_ex(object, key, &value));
	assert(json_object_is_type(value, json_type_string));
	assert(json_object_get_string_len(value) == (int)expected.len);
	assert(!memcmp(json_object_get_string(value), expected.data, expected.len));
}

static void autocomplete_reuse (struct autocomplete *catalog)
{
	static struct autocomplete_text const inputs[] = {
		TEXT("/tool "), TEXT("/tool git "), TEXT("/tool lookup mixed="),
		TEXT("/tool lookup structured="), TEXT("/tool git participant="),
		TEXT("/tool git notebook="), TEXT("@"), TEXT("/tool git command=stale count=2")
	};
	/* Grow the line workspace first, then alternate long and short requests.
	 * A long suffix also verifies that completion retains text after point. */
	char long_line[8192];
	memset(long_line, ' ', sizeof long_line);
	memcpy(long_line, "/tool git com", sizeof("/tool git com") - 1);
	struct autocomplete_text long_input = {long_line, sizeof long_line};
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(!autocomplete_complete(catalog, long_input, sizeof("/tool git com") - 1,
	                              &matches, &count));
	assert(count);
	for (size_t i = 0; i < sizeof inputs / sizeof *inputs; ++i)
		assert(!autocomplete_complete(catalog, inputs[i], inputs[i].len, &matches, &count));
	for (size_t i = 0; i < 128; ++i) {
		autocomplete_count_allocations = true;
		assert(!autocomplete_complete(catalog, long_input, sizeof("/tool git com") - 1,
		                              &matches, &count));
		for (size_t j = 0; j < sizeof inputs / sizeof *inputs; ++j)
			assert(!autocomplete_complete(catalog, inputs[j], inputs[j].len, &matches, &count));
		autocomplete_count_allocations = false;
	}
	if (autocomplete_allocations || autocomplete_frees)
		fprintf(stderr, "warm completion: %zu allocations, %zu frees\n",
		        autocomplete_allocations, autocomplete_frees);
	assert(!autocomplete_allocations && !autocomplete_frees);
}

static void autocomplete_files(void)
{
	int error = 0;
	struct autocomplete_file *files = autocomplete_file_create(&error);
	assert(files && !error);
	static struct autocomplete_text const commands[] = {TEXT("/read ")};
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(!autocomplete_file_complete(files, (struct autocomplete_text){nullptr, 0}, 0,
	                                   commands, 1, &matches, &count));
	assert(!count);
	struct autocomplete_text invalid = TEXT("/read bad\xc0\xaf");
	assert(autocomplete_file_complete(files, invalid, invalid.len, commands, 1,
	                                  &matches, &count) == EINVAL);
	assert(!count);
	invalid = TEXT("/read bad\0name");
	assert(autocomplete_file_complete(files, invalid, invalid.len, commands, 1,
	                                  &matches, &count) == EINVAL);
	assert(!count);

	char directory[] = "/tmp/askai-autocomplete-XXXXXX";
	assert(mkdtemp(directory));
	int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(fd >= 0);
	static char const *const names[] = {"normal", "bad\xc0\xaf", "bad\xc2\x9b", "bad\x1b[31m"};
	for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
		int file = openat(fd, names[i], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		assert(file >= 0);
		assert(!close(file));
		file = -1;
	}
	char line[sizeof directory + sizeof("/read /")];
	int length = snprintf(line, sizeof line, "/read %s/", directory);
	assert(length > 0 && length < (int)sizeof line);
	struct autocomplete_text input = {line, (size_t)length};
	assert(!autocomplete_file_complete(files, input, input.len, commands, 1, &matches, &count));
	assert(count == 1);
	assert(matches[0].line.len == input.len + sizeof("normal") - 1);
	assert(!memcmp(matches[0].line.data, input.data, input.len));
	assert(!memcmp(matches[0].line.data + input.len, "normal", sizeof("normal") - 1));
	autocomplete_count_allocations = true;
	for (size_t i = 0; i < 128; ++i) {
		assert(!autocomplete_file_complete(files, input, input.len, commands, 1, &matches, &count));
		assert(count == 1);
	}
	autocomplete_count_allocations = false;
	assert(!autocomplete_allocations && !autocomplete_frees);
	for (size_t i = 0; i < sizeof names / sizeof *names; ++i)
		assert(!unlinkat(fd, names[i], 0));
	assert(!close(fd));
	fd = -1;
	assert(!rmdir(directory));
	autocomplete_file_destroy(&files);
	assert(!files);
}

static void autocomplete_limits (struct autocomplete *catalog)
{
	size_t const limit = 16U * 1024U * 1024U;
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	/* The size check must precede reading any bytes from an oversized span. */
	struct autocomplete_text oversized = {"", limit + 1};
	assert(autocomplete_complete(catalog, oversized, 0, &matches, &count) == E2BIG);
	assert(!matches && !count);
	struct autocomplete_tool const *tool = nullptr;
	struct json_object *arguments = nullptr;
	assert(autocomplete_parse_call(catalog, oversized, &tool, &arguments, nullptr) == E2BIG);
	assert(!tool && !arguments);
	struct autocomplete_text invalid = TEXT("@ali trailing\xff");
	assert(autocomplete_complete(catalog, invalid, sizeof("@ali") - 1, &matches, &count) == EINVAL);
	assert(!matches && !count);
	invalid = TEXT("/tool git message=invalid\xff");
	assert(autocomplete_parse_call(catalog, invalid, &tool, &arguments, nullptr) == EINVAL);
	assert(!tool && !arguments);

	size_t const length = 5U * 1024U * 1024U;
	char *line = malloc(length);
	assert(line);
	memset(line, ' ', length);
	memcpy(line, "/tool ", sizeof("/tool ") - 1);
	assert(!autocomplete_complete(catalog, (struct autocomplete_text){line, length},
	                              sizeof("/tool ") - 1, &matches, &count));
	assert(count >= 2 && count < autocomplete_tool_count(catalog));
	size_t bytes = 0;
	for (size_t i = 0; i < count; ++i) {
		assert(matches[i].line.len >= length);
		assert(matches[i].line.len < limit - bytes);
		bytes += matches[i].line.len + 1;
	}
	assert(bytes <= limit);
	free(line);
	line = nullptr;
}

int main(void)
{
	int error = 0;
	struct autocomplete *catalog = autocomplete_create(&error);
	assert(catalog && !error);
	assert(!autocomplete_fixture(catalog));

#define HAS(input, expected) autocomplete_has(catalog, TEXT(input), TEXT(expected), sizeof(input) - 1)
#define AT(input, expected, cursor) autocomplete_has(catalog, TEXT(input), TEXT(expected), cursor)
#define EMPTY(input) autocomplete_empty(catalog, TEXT(input), sizeof(input) - 1)
	HAS("/to", "/tool");
	HAS("/tool gi", "/tool git");
	HAS("/tool git com", "/tool git command=");
	HAS("/tool git command=lo", "/tool git command=log");
	HAS("/tool git recursive=t", "/tool git recursive=true");
	HAS("/tool git participant=al", "/tool git participant=alice");
	HAS("/tool git notebook=ali", "/tool git notebook=alice/anonymous/client");
	HAS("/tool lookup mode=ex", "/tool lookup mode=exact");
	HAS("/tool lookup depth=1", "/tool lookup depth=1");
	HAS("/tool lookup mixed=t", "/tool lookup mixed='true'");
	HAS("/tool lookup mixed=4", "/tool lookup mixed='42'");
	HAS("/tool lookup mixed=4", "/tool lookup mixed=42");
	HAS("/tool lookup author=", "/tool lookup author=null");
	EMPTY("/tool lookup author=al");
	HAS("/tool lookup structured=", "/tool lookup structured={\"depth\":2}");
	HAS("/tool lookup structured=", "/tool lookup structured=[1,2]");
	HAS("/tool git repo=other", "/tool git repo='other project'");
	HAS("/tool git repo=\"other", "/tool git repo=\"other project\"");
	HAS("@al", "@alice");
	HAS("hello @al", "hello @alice");
	AT("hello @al, next", "hello @alice, next", 9);
	AT("al: hi", "alice: hi", 2);
	HAS("ä", "äiti:");
	AT("/tool git command=stale count=2", "/tool git command=status count=2", 20);
	EMPTY("/tool absent ");
	EMPTY("/tool git recipient=");
	EMPTY("/tool git command=log com");
	assert(autocomplete_participant_count(catalog) == 3);
	assert(autocomplete_tool_count(catalog) == 4);
	assert(autocomplete_find_tool(catalog, (struct autocomplete_text){"gitJUNK", 3}));
	char counted[] = {'@', 'a', 'l'};
	autocomplete_has(catalog, (struct autocomplete_text){counted, sizeof counted}, TEXT("@alice"),
	                 sizeof counted);

	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(autocomplete_complete(catalog, TEXT("x"), 2, &matches, &count) == EINVAL);
	assert(!count);
	assert(autocomplete_complete(catalog, TEXT("@al\0ignored"), 3, &matches, &count) == EINVAL);
	assert(!count);

#define PARSE(input, name) autocomplete_parse(catalog, TEXT(input), TEXT(name))
	struct json_object *arguments = PARSE(
		"/tool git command=log recursive=true count=42 message='hello world' "
		"options='[\"--oneline\"]' config='{\"depth\":2}'", "git");
	struct json_object *value = nullptr;
	assert(json_object_object_get_ex(arguments, "recursive", &value));
	assert(json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value));
	assert(json_object_object_get_ex(arguments, "count", &value));
	assert(json_object_is_type(value, json_type_int) && json_object_get_int(value) == 42);
	autocomplete_string(arguments, "message", TEXT("hello world"));
	assert(json_object_object_get_ex(arguments, "options", &value));
	assert(json_object_is_type(value, json_type_array) && json_object_array_length(value) == 1);
	assert(json_object_object_get_ex(arguments, "config", &value));
	assert(json_object_is_type(value, json_type_object));
	json_object_put(arguments);
	arguments = nullptr;

	arguments = PARSE("/tool git message=true", "git");
	autocomplete_string(arguments, "message", TEXT("true"));
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool git {\"message\":\"hello \\\"world\\\"\",\"options\":[1,null]}", "git");
	autocomplete_string(arguments, "message", TEXT("hello \"world\""));
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool git repo=other\\ project", "git");
	autocomplete_string(arguments, "repo", TEXT("other project"));
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool lookup code=001 depth=2", "lookup");
	autocomplete_string(arguments, "code", TEXT("001"));
	assert(json_object_object_get_ex(arguments, "depth", &value));
	assert(json_object_get_int(value) == 2);
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool required {\"text\":null}", "required");
	assert(json_object_object_get_ex(arguments, "text", &value) && !value);
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool required text=null", "required");
	assert(json_object_object_get_ex(arguments, "text", &value) && !value);
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool required text='null'", "required");
	autocomplete_string(arguments, "text", TEXT("null"));
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool git {\"message\":\"a\\u0000b\"}", "git");
	autocomplete_string(arguments, "message", TEXT("a\0b"));
	json_object_put(arguments);
	arguments = nullptr;
	static char const bounded[] = "/tool git message=tailJUNK";
	arguments = autocomplete_parse(catalog,
		(struct autocomplete_text){bounded, sizeof("/tool git message=tail") - 1}, TEXT("git"));
	autocomplete_string(arguments, "message", TEXT("tail"));
	json_object_put(arguments);
	arguments = nullptr;
	/* The server owns validation of full JSON, including complex schemas. */
	arguments = PARSE("/tool git {\"extension\":true}", "git");
	assert(json_object_object_get_ex(arguments, "extension", &value) && json_object_get_boolean(value));
	json_object_put(arguments);
	arguments = nullptr;
	static struct autocomplete_text const roundtrips[] = {
		TEXT("/tool lookup mixed="), TEXT("/tool lookup structured=")
	};
	for (size_t i = 0; i < sizeof roundtrips / sizeof *roundtrips; ++i) {
		assert(!autocomplete_complete(catalog, roundtrips[i], roundtrips[i].len, &matches, &count));
		assert(count);
		for (size_t j = 0; j < count; ++j) {
			struct autocomplete_text line = matches[j].line;
			arguments = autocomplete_parse(catalog, line, TEXT("lookup"));
			assert(json_object_object_get_ex(arguments, i ? "structured" : "mixed", &value));
			if (!i && memchr(line.data, '\'', line.len))
				assert(json_object_is_type(value, json_type_string));
			else if (!i)
				assert(json_object_is_type(value, json_type_int) && json_object_get_int(value) == 42);
			else if (memchr(line.data, '{', line.len))
				assert(json_object_is_type(value, json_type_object));
			else
				assert(json_object_is_type(value, json_type_array));
			json_object_put(arguments);
			arguments = nullptr;
		}
	}
	static struct autocomplete_text const invalid[] = {
		TEXT("/tool absent"), TEXT("/tool git recursive=maybe"), TEXT("/tool git count=1.5"),
		TEXT("/tool git message='unfinished"), TEXT("/tool git command=log command=status"),
		TEXT("/tool git unknown=1"), TEXT("/tool git options=no"), TEXT("/tool git config=[]"),
		TEXT("/tool git {\"message\":\"hello\"} trailing"), TEXT("/tool git message"),
		TEXT("/tool required"), TEXT("/tool git\0 message=ignored")
	};
	for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i)
		autocomplete_bad(catalog, invalid[i]);
	autocomplete_reuse(catalog);
	autocomplete_files();
	autocomplete_limits(catalog);
	struct autocomplete *other = autocomplete_create(&error);
	assert(other && !error);
	struct autocomplete_text carol = TEXT("carol");
	assert(!autocomplete_set_participants(other, &carol, 1));
	autocomplete_empty(other, TEXT("/tool gi"), sizeof("/tool gi") - 1);
	autocomplete_empty(other, TEXT("@al"), sizeof("@al") - 1);
	autocomplete_has(other, TEXT("@ca"), TEXT("@carol"), sizeof("@ca") - 1);
	autocomplete_clear_tools(catalog);
	EMPTY("/tool gi");
	HAS("@al", "@alice");
	autocomplete_destroy(&other);
	autocomplete_destroy(&catalog);
	assert(!other && !catalog);
	puts("autocomplete: tools, values, names, parsing, counted input, isolation and warm reuse: PASS");
}

#undef PARSE
#undef EMPTY
#undef AT
#undef HAS
#undef TEXT
