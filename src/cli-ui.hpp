/*
 * Adapted from llama.cpp tools/cli/cli-ui.h, commit
 * b76b13379d3b7d67ab0bc424516bb6fd2522bd8e.
 * Copyright (c) 2023-2026 The ggml authors. MIT license; see doc/llama-cli-LICENSE.
 */
#pragma once

#include <string>
#include <string_view>

#include "cli-console.hpp"

namespace cli_ui {

struct assistant_turn {
	std::string status_actor;
	std::string status_text;
	bool reasoning = false;
	bool trailing_newline = true;
	bool streaming = false;
	bool status_shown = false;
	bool status_pending = false;
	assistant_turn() {
		cli_console::set_display(cli_console::DISPLAY_TYPE_RESET);
	}
	~assistant_turn() {
		finish();
	}
	void newline() {
		if (!trailing_newline) {
			cli_console::log("\n");
			trailing_newline = true;
		}
	}
	void push(bool thinking, std::string_view text) {
		if (text.empty()) {
			return;
		}
		streaming = true;
		if (thinking != reasoning) {
			newline();
			if (thinking) {
				cli_console::set_display(cli_console::DISPLAY_TYPE_REASONING);
				cli_console::log("[Start thinking]\n\n");
			} else {
				cli_console::log("[End thinking]\n\n");
				cli_console::set_display(cli_console::DISPLAY_TYPE_RESET);
			}
			reasoning = thinking;
		}
		cli_console::write(text);
		trailing_newline = text.back() == '\n';
		cli_console::flush();
	}
	void finish() {
		newline();
		if (reasoning) {
			cli_console::log("[End thinking]\n");
			reasoning = false;
		}
		cli_console::set_display(cli_console::DISPLAY_TYPE_RESET);
		if (status_pending) {
			write_action(status_actor, status_text);
		}
		status_actor.clear();
		status_text.clear();
		streaming = status_shown = status_pending = false;
		cli_console::flush();
	}
	/** Keep token counts out of the streamed transcript. Show the first
	 * status before streaming, then its latest value at a real boundary. */
	void progress(std::string_view actor, std::string_view text) {
		if (text.empty() || (actor == status_actor && text == status_text)) {
			return;
		}
		status_actor.assign(actor);
		status_text.assign(text);
		status_pending = streaming || status_shown;
		if (!status_pending) {
			write_action(actor, text);
			status_shown = true;
		}
	}
	void action(std::string_view actor, std::string_view text) {
		finish();
		write_action(actor, text);
	}
private:
	static void write_action(std::string_view actor, std::string_view text) {
		cli_console::set_display(cli_console::DISPLAY_TYPE_INFO);
		cli_console::write("* ");
		cli_console::write(actor);
		cli_console::write(" ");
		cli_console::write(text);
		cli_console::write("\n");
		cli_console::set_display(cli_console::DISPLAY_TYPE_RESET);
		cli_console::flush();
	}
};

inline void show_message(std::string_view text) {
	cli_console::write(text);
	cli_console::log("\n");
	cli_console::flush();
}

inline void show_info(std::string_view text) {
	cli_console::set_display(cli_console::DISPLAY_TYPE_INFO);
	show_message(text);
	cli_console::set_display(cli_console::DISPLAY_TYPE_RESET);
}

inline void show_error(std::string_view title, std::string_view detail = {}) {
	cli_console::write_error("Error: ");
	cli_console::write_error(title);
	cli_console::write_error("\n");
	if (!detail.empty()) {
		cli_console::write_error(detail);
		cli_console::write_error("\n");
	}
}

}
