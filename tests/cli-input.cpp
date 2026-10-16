/** @file Console-only driver for PTY tests, independent of the MCP transport. */

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>

#include "autocomplete-file.h"
#include "autocomplete-fixture.h"
#include "cli-console.hpp"

static volatile sig_atomic_t cli_input_stop = 0;
static int cli_input_wake = -1;

static void cli_input_signal(int signal)
{
	int saved = errno;
	if (signal != SIGWINCH)
		cli_input_stop = signal;
	unsigned char byte = static_cast<unsigned char>(signal);
	(void)write(cli_input_wake, &byte, sizeof byte);
	errno = saved;
}

static int cli_input_record(int fd, std::string_view kind, std::string_view text = {})
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string record(kind);
	for (unsigned char byte : text) {
		record.push_back(digits[byte >> 4]);
		record.push_back(digits[byte & 15]);
	}
	record.push_back('\n');
	std::string_view remaining(record);
	while (!remaining.empty()) {
		ssize_t count = write(fd, remaining.data(), remaining.size());
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (!count)
			return EIO;
		remaining.remove_prefix(static_cast<size_t>(count));
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	std::string_view descriptor(argv[1]);
	int records = -1;
	auto parsed = std::from_chars(descriptor.data(), descriptor.data() + descriptor.size(), records);
	if (parsed.ec != std::errc{} || parsed.ptr != descriptor.data() + descriptor.size() || records < 0)
		return 2;
	bool simple = false, multiline = false, completion = false;
	for (int i = 2; i < argc; ++i) {
		std::string_view option(argv[i]);
		if (option == "--simple")
			simple = true;
		else if (option == "--multiline")
			multiline = true;
		else if (option == "--catalog")
			completion = true;
		else
			return 2;
	}
	int wake[2] = {-1, -1};
	if (pipe2(wake, O_CLOEXEC | O_NONBLOCK))
		return 1;
	cli_input_wake = wake[1];
	struct sigaction action{};
	action.sa_handler = cli_input_signal;
	if (sigemptyset(&action.sa_mask) || sigaction(SIGINT, &action, nullptr) ||
	    sigaction(SIGTERM, &action, nullptr) || sigaction(SIGWINCH, &action, nullptr))
		return 1;
	int error = 0;
	{
		cli_console::terminal terminal(simple, false, &cli_input_stop, error, wake[0]);
		if (error)
			return 1;
		struct autocomplete *catalog = autocomplete_create(&error);
		if (!catalog)
			return 1;
		struct autocomplete_file *files = autocomplete_file_create(&error);
		if (!files) {
			autocomplete_destroy(&catalog);
			return 1;
		}
		if (completion && autocomplete_fixture(catalog)) {
			autocomplete_file_destroy(&files);
			autocomplete_destroy(&catalog);
			return 1;
		}
		cli_console::set_completion_callback([catalog, files](std::string_view line, size_t cursor) {
			struct autocomplete_match const *matches = nullptr;
			size_t count = 0;
			int result = autocomplete_complete(catalog, {line.data(), line.size()}, cursor,
			                                   &matches, &count);
			static constexpr struct autocomplete_text commands[] = {{"/read ", sizeof("/read ") - 1}};
			if (!result && !count)
				result = autocomplete_file_complete(files, {line.data(), line.size()}, cursor,
				                                    commands, sizeof commands / sizeof *commands,
				                                    &matches, &count);
			return cli_console::completion_matches{result ? nullptr : matches, result ? 0 : count};
		});
		for (;;) {
			std::string text;
			bool eof = false;
			error = cli_console::read_input(text, multiline, eof, "test> ");
			if (error == EINTR) {
				error = cli_input_record(records, "STOP ", std::to_string(cli_input_stop));
				break;
			}
			if (error || eof) {
				if (!error)
					error = cli_input_record(records, "EOF");
				break;
			}
			error = cli_input_record(records, "LINE ", text);
			if (error)
				break;
		}
		cli_console::set_completion_callback({});
		autocomplete_file_destroy(&files);
		autocomplete_destroy(&catalog);
	}
	cli_input_wake = -1;
	close(wake[0]);
	close(wake[1]);
	close(records);
	return error ? 1 : 0;
}
