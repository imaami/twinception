/** @file Completion, invocation parsing and retained working storage. */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "autocomplete_file.h"
#include "autocomplete_fixture.h"
#include "cli_command.h"

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

static void autocomplete_has (struct autocomplete *catalog, struct cstr_view input,
                              struct cstr_view expected, size_t cursor)
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

static void autocomplete_empty (struct autocomplete *catalog, struct cstr_view input,
                                size_t cursor)
{
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(!autocomplete_complete(catalog, input, cursor, &matches, &count));
	assert(!count);
}

static void autocomplete_only (struct autocomplete *catalog, struct cstr_view input,
                               size_t cursor, struct cstr_view expected, size_t point)
{
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(!autocomplete_complete(catalog, input, cursor, &matches, &count));
	assert(count == 1);
	assert(cstr_view_equal(matches[0].line, expected));
	assert(matches[0].cursor == point);
}

static struct json_object *autocomplete_parse (struct autocomplete *catalog,
                                               struct cstr_view input,
                                               struct cstr_view expected_name)
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

static void autocomplete_bad (struct autocomplete *catalog, struct cstr_view input)
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
                                 struct cstr_view expected)
{
	struct json_object *value = nullptr;
	assert(json_object_object_get_ex(object, key, &value));
	assert(json_object_is_type(value, json_type_string));
	assert(json_object_get_string_len(value) == (int)expected.len);
	assert(!memcmp(json_object_get_string(value), expected.data, expected.len));
}

static void autocomplete_reuse (struct autocomplete *catalog)
{
	static struct cstr_view const inputs[] = {
		CSTR_VIEW("/tool "), CSTR_VIEW("/tool git "), CSTR_VIEW("/tool lookup mixed="),
		CSTR_VIEW("/tool lookup structured="), CSTR_VIEW("/tool git participant="),
		CSTR_VIEW("/tool git notebook="), CSTR_VIEW("@"), CSTR_VIEW("/tool git command=stale count=2"),
		CSTR_VIEW("/tool notes action=read file="), CSTR_VIEW("/tool notes action=copy file="),
		CSTR_VIEW("/tool notes notebook=bob/0123456789abcdef/upstream file="),
		CSTR_VIEW("/tool notes action=read file='design docs/"), CSTR_VIEW("/tool notes file=o")
	};
	/* Grow the line workspace first, then alternate long and short requests.
	 * A long suffix also verifies that completion retains text after point. */
	char long_line[8192];
	memset(long_line, ' ', sizeof long_line);
	memcpy(long_line, "/tool git com", sizeof("/tool git com") - 1);
	struct cstr_view long_input = {long_line, sizeof long_line};
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
	static struct cstr_view const commands[] = {CSTR_VIEW("/read ")};
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(!autocomplete_file_complete(files, (struct cstr_view){nullptr, 0}, 0,
	                                   commands, 1, &matches, &count));
	assert(!count);
	struct cstr_view invalid = CSTR_VIEW("/read bad\xc0\xaf");
	assert(autocomplete_file_complete(files, invalid, invalid.len, commands, 1,
	                                  &matches, &count) == EINVAL);
	assert(!count);
	invalid = CSTR_VIEW("/read bad\0name");
	assert(autocomplete_file_complete(files, invalid, invalid.len, commands, 1,
	                                  &matches, &count) == EINVAL);
	assert(!count);

	char directory[] = "/tmp/askai-autocomplete-XXXXXX";
	assert(mkdtemp(directory));
	int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(fd >= 0);
	assert(!symlinkat("missing", fd, "dangling"));
	assert(!mkdirat(fd, "folder", 0700));
	assert(!symlinkat("folder", fd, "link-dir"));
	assert(!symlinkat("normal", fd, "link-file"));
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
	struct cstr_view input = {line, (size_t)length};
	assert(!autocomplete_file_complete(files, input, input.len, commands, 1, &matches, &count));
	static struct cstr_view const expected[] = {
		CSTR_VIEW("dangling "), CSTR_VIEW("folder/"), CSTR_VIEW("link-dir/"), CSTR_VIEW("link-file "), CSTR_VIEW("normal "),
	};
	assert(count == sizeof expected / sizeof *expected);
	for (size_t i = 0; i < count; ++i) {
		assert(matches[i].line.len == input.len + expected[i].len);
		assert(matches[i].cursor == matches[i].line.len);
		assert(!memcmp(matches[i].line.data, input.data, input.len));
		assert(!memcmp(matches[i].line.data + input.len, expected[i].data, expected[i].len));
	}
	autocomplete_count_allocations = true;
	for (size_t i = 0; i < 128; ++i) {
		assert(!autocomplete_file_complete(files, input, input.len, commands, 1, &matches, &count));
		assert(count == sizeof expected / sizeof *expected);
	}
	autocomplete_count_allocations = false;
	assert(!autocomplete_allocations && !autocomplete_frees);
	char complete[sizeof directory + sizeof("/read /normal ")];
	length = snprintf(complete, sizeof complete, "/read %s/normal ", directory);
	assert(length > 0 && length < (int)sizeof complete);
	struct cstr_view completed = {complete, (size_t)length};
	assert(!autocomplete_file_complete(files, completed, completed.len - 1, commands, 1, &matches, &count));
	assert(count == 1 && cstr_view_equal(matches[0].line, completed));
	assert(matches[0].cursor == completed.len);
	for (size_t i = 0; i < sizeof names / sizeof *names; ++i)
		assert(!unlinkat(fd, names[i], 0));
	assert(!unlinkat(fd, "dangling", 0));
	assert(!unlinkat(fd, "link-dir", 0));
	assert(!unlinkat(fd, "link-file", 0));
	assert(!unlinkat(fd, "folder", AT_REMOVEDIR));
	assert(!close(fd));
	fd = -1;
	assert(!rmdir(directory));
	autocomplete_file_destroy(&files);
	assert(!files);
}

static void autocomplete_note_cache(struct autocomplete *catalog)
{
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	struct cstr_view input = CSTR_VIEW("/tool notes file=");
	assert(!autocomplete_complete(catalog, input, input.len, &matches, &count));
	assert(count == 4); /* Unsafe paths omitted; duplicates removed. */
	struct cstr_view const paths[] = {CSTR_VIEW("updated plan.md"), CSTR_VIEW("new/entry.md")};
	assert(!autocomplete_set_note_files(catalog, CSTR_VIEW("notes"), CSTR_VIEW("own"), paths, 2));
	autocomplete_has(catalog, input, CSTR_VIEW("/tool notes file='updated plan.md'"), input.len);
	input = CSTR_VIEW("/tool notes file=find");
	autocomplete_empty(catalog, input, input.len);
	input = CSTR_VIEW("/tool notes action=copy file=");
	autocomplete_has(catalog, input, CSTR_VIEW("/tool notes action=copy file=model/thoughts.md"), input.len);
	assert(autocomplete_set_note_files(catalog, CSTR_VIEW("absent"), CSTR_VIEW("own"), paths, 2) == EINVAL);
	assert(autocomplete_set_note_files(catalog, CSTR_VIEW("notes"), CSTR_VIEW("own"), nullptr, 1) == EINVAL);
	assert(autocomplete_set_note_files(catalog, CSTR_VIEW("notes"), CSTR_VIEW("own"), paths, (size_t)-1) == E2BIG);
	struct cstr_view huge = {"", 16 * 1024 * 1024};
	assert(autocomplete_set_note_files(catalog, CSTR_VIEW("notes"), CSTR_VIEW("own"), &huge, 1) == E2BIG);
	input = CSTR_VIEW("/tool notes file=new");
	autocomplete_has(catalog, input, CSTR_VIEW("/tool notes file=new/entry.md"), input.len);
	/* Clearing and refilling reuses capacity, including path strings. */
	autocomplete_clear_note_files(catalog);
	autocomplete_empty(catalog, input, input.len);
	assert(!autocomplete_set_note_files(catalog, CSTR_VIEW("notes"), CSTR_VIEW(""), paths, 2));
	for (unsigned i = 0; i < 128; ++i) {
		autocomplete_count_allocations = true;
		autocomplete_clear_note_files(catalog);
		assert(!autocomplete_set_note_files(catalog, CSTR_VIEW("notes"), CSTR_VIEW(""), paths, 2));
		autocomplete_count_allocations = false;
	}
	assert(!autocomplete_allocations && !autocomplete_frees);
	autocomplete_has(catalog, input, CSTR_VIEW("/tool notes file=new/entry.md"), input.len);
}

static void autocomplete_limits (struct autocomplete *catalog)
{
	size_t const limit = 16U * 1024U * 1024U;
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	/* The size check must precede reading any bytes from an oversized span. */
	struct cstr_view oversized = {"", limit + 1};
	assert(autocomplete_complete(catalog, oversized, 0, &matches, &count) == E2BIG);
	assert(!matches && !count);
	struct autocomplete_tool const *tool = nullptr;
	struct json_object *arguments = nullptr;
	assert(autocomplete_parse_call(catalog, oversized, &tool, &arguments, nullptr) == E2BIG);
	assert(!tool && !arguments);
	struct cstr_view invalid = CSTR_VIEW("@ali trailing\xff");
	assert(autocomplete_complete(catalog, invalid, sizeof("@ali") - 1, &matches, &count) == EINVAL);
	assert(!matches && !count);
	invalid = CSTR_VIEW("/tool git message=invalid\xff");
	assert(autocomplete_parse_call(catalog, invalid, &tool, &arguments, nullptr) == EINVAL);
	assert(!tool && !arguments);

	size_t const length = 5U * 1024U * 1024U;
	char *line = malloc(length);
	assert(line);
	memset(line, ' ', length);
	memcpy(line, "/tool ", sizeof("/tool ") - 1);
	assert(!autocomplete_complete(catalog, (struct cstr_view){line, length},
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

static void autocomplete_commands (void)
{
	struct cli_command_matches all = cli_command_prefix(CSTR_VIEW(""));
	assert(all.count == CLI_COMMAND_COUNT);
	for (size_t i = 0; i < all.count; ++i) {
		struct cli_command const *command = &all.data[i];
		assert(cli_command_at(command->id) == command);
		assert(cli_command_find(command->name) == command);
		assert(cstr_view_starts(command->completion, command->name));
		assert(cstr_view_starts(command->usage, command->name));
		if (i)
			assert(cstr_view_compare(all.data[i - 1].name, command->name) < 0);
		for (size_t length = 0; length <= command->name.len; ++length) {
			struct cstr_view prefix = cstr_view_slice(command->name, 0, length);
			struct cli_command_matches matches = cli_command_prefix(prefix);
			size_t matched = 0;
			for (size_t j = 0; j < all.count; ++j)
				if (cstr_view_starts(all.data[j].name, prefix)) {
					assert(matched < matches.count);
					assert(&matches.data[matched++] == &all.data[j]);
				}
			assert(matched == matches.count);
		}
	}
	assert(cli_command_prefix(CSTR_VIEW("/tool")).count == 2);
	assert(cli_command_find((struct cstr_view){"/tools", 5})->id == CLI_COMMAND_TOOL);
	assert(cli_command_find(CSTR_VIEW("/tools"))->id == CLI_COMMAND_TOOLS);
	assert(!cli_command_find(CSTR_VIEW("/to")));
	assert(!cli_command_find(CSTR_VIEW("/toolbox")));
	assert(!cli_command_find(CSTR_VIEW("/tool ")));
	assert(!cli_command_find(CSTR_VIEW("/tool\0")));
	assert(!cli_command_prefix(CSTR_VIEW("/tool\0")).count);
	assert(!cli_command_at(CLI_COMMAND_COUNT));
	assert(!cli_command_at((enum cli_command_id)-1));
}

static void autocomplete_schema_flags (void)
{
	int error = 0;
	struct autocomplete *catalog = autocomplete_create(&error);
	assert(catalog && !error);
	struct json_object *definition = json_tokener_parse(
		"{\"name\":\"flags\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
		"\"participant\":{\"enum\":[]},\"recipient\":{\"enum\":true},"
		"\"author\":{\"type\":null,\"enum\":null},"
		"\"union\":{\"type\":[\"string\",\"integer\"],\"enum\":[\"42\",42]},"
		"\"constant\":{\"type\":[\"boolean\",\"null\"],\"const\":null},"
		"\"integer\":{\"type\":\"integer\"},\"number\":{\"type\":\"number\"},"
		"\"nullable\":{\"type\":[\"boolean\",\"null\"]},\"any\":{},\"empty\":{\"type\":[]}}}}");
	assert(definition && !autocomplete_add_tool(catalog, definition));
	json_object_put(definition);
	definition = nullptr;
	struct cstr_view name = CSTR_VIEW("alice");
	assert(!autocomplete_set_participants(catalog, &name, 1));
	autocomplete_empty(catalog, CSTR_VIEW("/tool flags participant="), sizeof("/tool flags participant=") - 1);
	autocomplete_empty(catalog, CSTR_VIEW("/tool flags recipient="), sizeof("/tool flags recipient=") - 1);
	autocomplete_has(catalog, CSTR_VIEW("/tool flags author="), CSTR_VIEW("/tool flags author='alice'"), sizeof("/tool flags author=") - 1);
	autocomplete_has(catalog, CSTR_VIEW("/tool flags union="), CSTR_VIEW("/tool flags union='42'"), sizeof("/tool flags union=") - 1);
	autocomplete_has(catalog, CSTR_VIEW("/tool flags union="), CSTR_VIEW("/tool flags union=42"), sizeof("/tool flags union=") - 1);
	autocomplete_has(catalog, CSTR_VIEW("/tool flags constant="), CSTR_VIEW("/tool flags constant=null"), sizeof("/tool flags constant=") - 1);
	autocomplete_empty(catalog, CSTR_VIEW("/tool flags constant=t"), sizeof("/tool flags constant=t") - 1);
	autocomplete_has(catalog, CSTR_VIEW("/tool flags nullable="), CSTR_VIEW("/tool flags nullable=true"), sizeof("/tool flags nullable=") - 1);
	autocomplete_has(catalog, CSTR_VIEW("/tool flags nullable="), CSTR_VIEW("/tool flags nullable=null"), sizeof("/tool flags nullable=") - 1);
	struct json_object *arguments = autocomplete_parse(catalog,
		CSTR_VIEW("/tool flags union=42 integer=2.0 number=2 any='true'"), CSTR_VIEW("flags"));
	struct json_object *value = nullptr;
	assert(json_object_object_get_ex(arguments, "union", &value) && json_object_is_type(value, json_type_int));
	assert(json_object_object_get_ex(arguments, "integer", &value) && json_object_is_type(value, json_type_double));
	assert(json_object_object_get_ex(arguments, "number", &value) && json_object_is_type(value, json_type_int));
	autocomplete_string(arguments, "any", CSTR_VIEW("true"));
	json_object_put(arguments);
	arguments = autocomplete_parse(catalog, CSTR_VIEW("/tool flags union='42'"), CSTR_VIEW("flags"));
	autocomplete_string(arguments, "union", CSTR_VIEW("42"));
	json_object_put(arguments);
	arguments = nullptr;
	autocomplete_bad(catalog, CSTR_VIEW("/tool flags integer=2.5"));
	autocomplete_bad(catalog, CSTR_VIEW("/tool flags empty=true"));
	autocomplete_destroy(&catalog);
}

static void autocomplete_chat_names (void)
{
	int error = 0;
	struct autocomplete *catalog = autocomplete_create(&error);
	assert(catalog && !error);
	assert(!autocomplete_fixture(catalog));
	static struct cstr_view const names[] = {CSTR_VIEW("rx6900xt"), CSTR_VIEW("rx9070xt")};
	assert(!autocomplete_set_participants(catalog, names, sizeof names / sizeof *names));
	assert(!autocomplete_set_chat_participants(catalog, names, 1));
	autocomplete_only(catalog, CSTR_VIEW("@rx"), 3, CSTR_VIEW("@rx6900xt "), 10);
	autocomplete_only(catalog, CSTR_VIEW("rx"), 2, CSTR_VIEW("rx6900xt: "), 10);
	autocomplete_empty(catalog, CSTR_VIEW("@rx9"), 4);
	assert(autocomplete_participant_count(catalog) == 2);
	assert(cstr_view_equal(autocomplete_participant_at(catalog, 1), names[1]));
	autocomplete_has(catalog, CSTR_VIEW("/tool git participant=rx9"),
	                 CSTR_VIEW("/tool git participant=rx9070xt"), sizeof("/tool git participant=rx9") - 1);
	/* A chat roster can contain several upstreams, independently of stored
	 * notebook identities. Clearing it must not fall back to those identities. */
	assert(!autocomplete_set_chat_participants(catalog, names, sizeof names / sizeof *names));
	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(!autocomplete_complete(catalog, CSTR_VIEW("@rx"), 3, &matches, &count));
	assert(count == 2);
	assert(!autocomplete_set_chat_participants(catalog, nullptr, 0));
	autocomplete_empty(catalog, CSTR_VIEW("@rx"), 3);
	assert(autocomplete_participant_count(catalog) == 2);
	autocomplete_destroy(&catalog);
}

int main(void)
{
	autocomplete_commands();
	autocomplete_schema_flags();
	autocomplete_chat_names();
	int error = 0;
	struct autocomplete *catalog = autocomplete_create(&error);
	assert(catalog && !error);
	assert(!autocomplete_fixture(catalog));
	autocomplete_only(catalog, CSTR_VIEW("/qu"), 3, CSTR_VIEW("/quit "), 6);
	autocomplete_only(catalog, CSTR_VIEW("/quit"), 5, CSTR_VIEW("/quit "), 6);
	autocomplete_only(catalog, CSTR_VIEW("/quit "), 5, CSTR_VIEW("/quit "), 6);
	autocomplete_only(catalog, CSTR_VIEW("/quit   "), 5, CSTR_VIEW("/quit   "), 6);
	autocomplete_only(catalog, CSTR_VIEW("/he after"), 3, CSTR_VIEW("/help after"), 6);
	autocomplete_only(catalog, CSTR_VIEW("/tools gi tail"), 9, CSTR_VIEW("/tools git tail"), 11);

#define HAS(input, expected) autocomplete_has(catalog, CSTR_VIEW(input), CSTR_VIEW(expected), sizeof(input) - 1)
#define AT(input, expected, cursor) autocomplete_has(catalog, CSTR_VIEW(input), CSTR_VIEW(expected), cursor)
#define EMPTY(input) autocomplete_empty(catalog, CSTR_VIEW(input), sizeof(input) - 1)
	HAS("/to", "/tool");
	HAS("/to", "/tools");
	HAS("/qu", "/quit");
	HAS("/his", "/history");
	HAS("/tools gi", "/tools git");
	EMPTY("/toolbox gi");
	HAS("/tool gi", "/tool git");
	HAS("/tool git com", "/tool git command=");
	HAS("/tool git command=lo", "/tool git command=log");
	HAS("/tool git recursive=t", "/tool git recursive=true");
	HAS("/tool git participant=al", "/tool git participant=alice");
	HAS("/tool git notebook=ali", "/tool git notebook=alice/anonymous/client");
	HAS("/tool notes action=read file=find", "/tool notes action=read file=findings/locks.md");
	HAS("/tool notes file=find", "/tool notes file=findings/locks.md");
	HAS("/tool notes notebook=own file=find", "/tool notes notebook=own file=findings/locks.md");
	HAS("/tool notes action=copy file=mo", "/tool notes action=copy file=model/thoughts.md");
	HAS("/tool notes action=copy notebook=own file=find", "/tool notes action=copy notebook=own file=findings/locks.md");
	HAS("/tool notes action=attach notebook=other file=mo", "/tool notes action=attach notebook=other file=model/thoughts.md");
	HAS("/tool notes notebook=bob/0123456789abcdef/upstream file=re", "/tool notes notebook=bob/0123456789abcdef/upstream file=reviews/reply.md");
	AT("/tool notes file=re notebook='bob/0123456789abcdef/upstream' action=read",
	   "/tool notes file=reviews/reply.md notebook='bob/0123456789abcdef/upstream' action=read",
	   sizeof("/tool notes file=re") - 1);
	AT("/tool notes file=mo action=copy", "/tool notes file=model/thoughts.md action=copy", sizeof("/tool notes file=mo") - 1);
	HAS("/tool notes file=des", "/tool notes file='design docs/first.md'");
	HAS("/tool notes file=\"design docs/", "/tool notes file=\"design docs/first.md\"");
	HAS("/tool notes file=design\\ docs/", "/tool notes file='design docs/first.md'");
	HAS("/tool notes file=ä", "/tool notes file=äiti.md");
	HAS("/tool notes file=o", "/tool notes file='o'\\''brien.md'");
	EMPTY("/tool git file=");
	EMPTY("/tool notes file=bad");
	EMPTY("/tool notes action=add file=");
	EMPTY("/tool notes action=list file=");
	EMPTY("/tool notes action=notebooks file=");
	EMPTY("/tool notes action=read action=copy file=");
	EMPTY("/tool notes notebook=own notebook=other file=");
	EMPTY("/tool notes notebook=absent file=");
	EMPTY("/tool notes notebook='' file=");
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
	assert(autocomplete_tool_count(catalog) == 5);
	assert(autocomplete_find_tool(catalog, (struct cstr_view){"gitJUNK", 3}));
	char counted[] = {'@', 'a', 'l'};
	autocomplete_has(catalog, (struct cstr_view){counted, sizeof counted}, CSTR_VIEW("@alice"),
	                 sizeof counted);

	struct autocomplete_match const *matches = nullptr;
	size_t count = 0;
	assert(autocomplete_complete(catalog, CSTR_VIEW("x"), 2, &matches, &count) == EINVAL);
	assert(!count);
	assert(autocomplete_complete(catalog, CSTR_VIEW("@al\0ignored"), 3, &matches, &count) == EINVAL);
	assert(!count);

#define PARSE(input, name) autocomplete_parse(catalog, CSTR_VIEW(input), CSTR_VIEW(name))
	struct json_object *arguments = PARSE(
		"/tool git command=log recursive=true count=42 message='hello world' "
		"options='[\"--oneline\"]' config='{\"depth\":2}'", "git");
	struct json_object *value = nullptr;
	assert(json_object_object_get_ex(arguments, "recursive", &value));
	assert(json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value));
	assert(json_object_object_get_ex(arguments, "count", &value));
	assert(json_object_is_type(value, json_type_int) && json_object_get_int(value) == 42);
	autocomplete_string(arguments, "message", CSTR_VIEW("hello world"));
	assert(json_object_object_get_ex(arguments, "options", &value));
	assert(json_object_is_type(value, json_type_array) && json_object_array_length(value) == 1);
	assert(json_object_object_get_ex(arguments, "config", &value));
	assert(json_object_is_type(value, json_type_object));
	json_object_put(arguments);
	arguments = nullptr;

	arguments = PARSE("/tool git message=true", "git");
	autocomplete_string(arguments, "message", CSTR_VIEW("true"));
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool git {\"message\":\"hello \\\"world\\\"\",\"options\":[1,null]}", "git");
	autocomplete_string(arguments, "message", CSTR_VIEW("hello \"world\""));
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool git repo=other\\ project", "git");
	autocomplete_string(arguments, "repo", CSTR_VIEW("other project"));
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool lookup code=001 depth=2", "lookup");
	autocomplete_string(arguments, "code", CSTR_VIEW("001"));
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
	autocomplete_string(arguments, "text", CSTR_VIEW("null"));
	json_object_put(arguments);
	arguments = nullptr;
	arguments = PARSE("/tool git {\"message\":\"a\\u0000b\"}", "git");
	autocomplete_string(arguments, "message", CSTR_VIEW("a\0b"));
	json_object_put(arguments);
	arguments = nullptr;
	static char const bounded[] = "/tool git message=tailJUNK";
	arguments = autocomplete_parse(catalog,
		(struct cstr_view){bounded, sizeof("/tool git message=tail") - 1}, CSTR_VIEW("git"));
	autocomplete_string(arguments, "message", CSTR_VIEW("tail"));
	json_object_put(arguments);
	arguments = nullptr;
	/* The server owns validation of full JSON, including complex schemas. */
	arguments = PARSE("/tool git {\"extension\":true}", "git");
	assert(json_object_object_get_ex(arguments, "extension", &value) && json_object_get_boolean(value));
	json_object_put(arguments);
	arguments = nullptr;
	static struct cstr_view const roundtrips[] = {
		CSTR_VIEW("/tool lookup mixed="), CSTR_VIEW("/tool lookup structured=")
	};
	for (size_t i = 0; i < sizeof roundtrips / sizeof *roundtrips; ++i) {
		assert(!autocomplete_complete(catalog, roundtrips[i], roundtrips[i].len, &matches, &count));
		assert(count);
		for (size_t j = 0; j < count; ++j) {
			struct cstr_view line = matches[j].line;
			arguments = autocomplete_parse(catalog, line, CSTR_VIEW("lookup"));
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
	static struct cstr_view const invalid[] = {
		CSTR_VIEW("/tools git"), CSTR_VIEW("/toolbox git"), CSTR_VIEW("/to git"),
		CSTR_VIEW("/tool absent"), CSTR_VIEW("/tool git recursive=maybe"), CSTR_VIEW("/tool git count=1.5"),
		CSTR_VIEW("/tool git message='unfinished"), CSTR_VIEW("/tool git command=log command=status"),
		CSTR_VIEW("/tool git unknown=1"), CSTR_VIEW("/tool git options=no"), CSTR_VIEW("/tool git config=[]"),
		CSTR_VIEW("/tool git {\"message\":\"hello\"} trailing"), CSTR_VIEW("/tool git message"),
		CSTR_VIEW("/tool required"), CSTR_VIEW("/tool git\0 message=ignored")
	};
	for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i)
		autocomplete_bad(catalog, invalid[i]);
	autocomplete_reuse(catalog);
	autocomplete_note_cache(catalog);
	autocomplete_files();
	autocomplete_limits(catalog);
	struct autocomplete *other = autocomplete_create(&error);
	assert(other && !error);
	struct cstr_view carol = CSTR_VIEW("carol");
	assert(!autocomplete_set_participants(other, &carol, 1));
	assert(!autocomplete_set_chat_participants(other, &carol, 1));
	autocomplete_empty(other, CSTR_VIEW("/tool gi"), sizeof("/tool gi") - 1);
	autocomplete_empty(other, CSTR_VIEW("@al"), sizeof("@al") - 1);
	autocomplete_has(other, CSTR_VIEW("@ca"), CSTR_VIEW("@carol"), sizeof("@ca") - 1);
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
