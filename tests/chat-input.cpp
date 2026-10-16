/** @file Console-only driver for PTY tests, independent of the MCP transport. */

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>

#include "chat-completion-fixture.h"
#include "chat-console.h"
#include "chat-ui.h"

static volatile sig_atomic_t chat_input_stop = 0;
static int chat_input_wake = -1;

static void chat_input_signal(int signal)
{
	int saved = errno;
	if (signal != SIGWINCH)
		chat_input_stop = signal;
	unsigned char byte = static_cast<unsigned char>(signal);
	(void)write(chat_input_wake, &byte, sizeof byte);
	errno = saved;
}

static int chat_input_record(int fd, std::string_view kind, std::string_view text = {})
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
	chat_input_wake = wake[1];
	struct sigaction action{};
	action.sa_handler = chat_input_signal;
	if (sigemptyset(&action.sa_mask) || sigaction(SIGINT, &action, nullptr) ||
	    sigaction(SIGTERM, &action, nullptr) || sigaction(SIGWINCH, &action, nullptr))
		return 1;
	int error = 0;
	{
		chat_console::terminal terminal(simple, false, &chat_input_stop, error, wake[0]);
		if (error)
			return 1;
		chat_completion::catalog catalog;
		if (completion) {
			chat_completion_fixture(catalog);
			chat_console::set_completion_callback([&catalog](std::string_view line, size_t cursor) {
				return catalog.complete(line, cursor);
			});
		} else {
			chat_ui::install_completion({"/help", "/clear", "/regen", "/read ", "/exit"}, {"/read "});
		}
		for (;;) {
			std::string text;
			bool eof = false;
			error = chat_console::read_input(text, multiline, eof, "test> ");
			if (error == EINTR) {
				error = chat_input_record(records, "STOP ", std::to_string(chat_input_stop));
				break;
			}
			if (error || eof) {
				if (!error)
					error = chat_input_record(records, "EOF");
				break;
			}
			error = chat_input_record(records, "LINE ", text);
			if (error)
				break;
		}
	}
	chat_input_wake = -1;
	close(wake[0]);
	close(wake[1]);
	close(records);
	return error ? 1 : 0;
}
