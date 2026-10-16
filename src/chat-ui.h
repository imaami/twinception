/*
 * Adapted from llama.cpp tools/cli/cli-ui.h, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "chat-console.h"

namespace chat_ui {

inline bool starts_with(std::string_view text, std::string_view prefix) {
	return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/** Command names are copied; names accepting paths include a trailing space. */
inline void install_completion(const std::vector<std::string> &commands,
                               const std::vector<std::string> &file_commands) {
	chat_console::set_completion_callback([commands, file_commands](std::string_view line, size_t cursor) {
		std::vector<std::pair<std::string, size_t>> matches;
		if (cursor > line.size()) {
			return matches;
		}
		for (const auto &command : commands) {
			if (cursor && starts_with(command, line.substr(0, cursor))) {
				matches.emplace_back(command + std::string(line.substr(cursor)), command.size());
			}
		}
		if (!matches.empty()) {
			return matches;
		}
		for (const auto &command : file_commands) {
			if (!starts_with(line, command) || cursor < command.size()) {
				continue;
			}
			std::string prefix(line.substr(command.size(), cursor - command.size()));
			std::string suffix(line.substr(cursor));
			if (prefix == "~") {
				std::string updated = command + "~/";
				matches.emplace_back(updated + suffix, updated.size());
				return matches;
			}
			std::string expanded = prefix;
			if (starts_with(prefix, "~/")) {
				const char *home = std::getenv("HOME");
				if (home && *home) {
					expanded = home + prefix.substr(1);
				}
			}
			std::filesystem::path path(expanded);
			std::filesystem::path directory = path.parent_path();
			if (directory.empty()) {
				directory = ".";
			}
			std::string basename = path.filename().string();
			size_t slash = prefix.find_last_of('/');
			std::string display_directory = slash == std::string::npos ? "" : prefix.substr(0, slash + 1);
			std::error_code error;
			std::filesystem::directory_iterator end;
			for (std::filesystem::directory_iterator it(directory, error); !error && it != end; it.increment(error)) {
				std::string name = it->path().filename().string();
				if (!starts_with(name, basename) || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
					continue;
				}
				if (it->is_directory(error)) {
					name.push_back('/');
				}
				if (error) {
					break;
				}
				std::string updated = command + display_directory + name;
				matches.emplace_back(updated + suffix, updated.size());
				if (matches.size() == 256) {
					break;
				}
			}
			std::sort(matches.begin(), matches.end());

			return matches;
		}
		return matches;
	});
}

struct assistant_turn {
	bool reasoning = false;
	bool trailing_newline = true;
	assistant_turn() {
		chat_console::set_display(chat_console::DISPLAY_TYPE_RESET);
	}
	~assistant_turn() {
		finish();
	}
	void newline() {
		if (!trailing_newline) {
			chat_console::log("\n");
			trailing_newline = true;
		}
	}
	void push(bool thinking, std::string_view text) {
		if (text.empty()) {
			return;
		}
		if (thinking != reasoning) {
			newline();
			if (thinking) {
				chat_console::set_display(chat_console::DISPLAY_TYPE_REASONING);
				chat_console::log("[Start thinking]\n\n");
			} else {
				chat_console::log("[End thinking]\n\n");
				chat_console::set_display(chat_console::DISPLAY_TYPE_RESET);
			}
			reasoning = thinking;
		}
		chat_console::write(text);
		trailing_newline = text.back() == '\n';
		chat_console::flush();
	}
	void finish() {
		newline();
		if (reasoning) {
			chat_console::log("[End thinking]\n");
			reasoning = false;
		}
		chat_console::set_display(chat_console::DISPLAY_TYPE_RESET);
		chat_console::flush();
	}
};

inline void show_message(std::string_view text) {
	chat_console::write(text);
	chat_console::log("\n");
	chat_console::flush();
}

inline void show_info(std::string_view text) {
	chat_console::set_display(chat_console::DISPLAY_TYPE_INFO);
	show_message(text);
	chat_console::set_display(chat_console::DISPLAY_TYPE_RESET);
}

inline void show_error(std::string_view title, std::string_view detail = {}) {
	chat_console::write_error("Error: ");
	chat_console::write_error(title);
	chat_console::write_error("\n");
	if (!detail.empty()) {
		chat_console::write_error(detail);
		chat_console::write_error("\n");
	}
}

}
