#!/usr/bin/env python3
"""Native AskAI chat history and foreground-call regression checks."""

import sys

from askai import ANSWERS, API, Askai, LEGACY, Stream, Upstream, sse_of, sse_wire, text_of


def native_messages(api, messages):
    if api == "generate":
        return [{"role": "model" if m["role"] == "assistant" else "user",
                 "parts": [{"text": m["content"]}]} for m in messages]
    if api == "interactions":
        return [{"type": "model_output" if m["role"] == "assistant" else "user_input",
                 "content": [{"type": "text", "text": m["content"]}]} for m in messages]
    return messages


def check_history(binary, upstream):
    history = [{"role": "user", "content": "Remember ääkköset, quotes \" and \\."},
               {"role": "assistant", "content": "Remembered.\n\t😀"}]
    prompt = "What did I ask you to remember?"
    for protocol, api in API.items():
        upstream.reply = lambda request, p=protocol: (200, ANSWERS[p])
        with Askai(binary, "--api", api, "-u", upstream.url(), "-m", "m") as ai:
            assert text_of(ai.ask(prompt, messages=history, system="Be brief.")) == "A"
            body = upstream.last()["body"]
            key = {"chat": "messages", "messages": "messages", "generate": "contents"}.get(
                api, "input")
            expected = native_messages(api, history + [{"role": "user", "content": prompt}])
            if api == "chat":
                expected.insert(0, {"role": "system", "content": "Be brief."})
            assert body[key] == expected, (api, body[key])
            if api in ("responses", "messages", "interactions"):
                system_key = {"responses": "instructions", "messages": "system",
                              "interactions": "system_instruction"}[api]
                assert body[system_key] == "Be brief.", body
            elif api == "generate":
                assert body["systemInstruction"] == {"parts": [{"text": "Be brief."}]}

            # A reused connection/request object must not retain old history.
            assert text_of(ai.ask("fresh", messages=[])) == "A"
            assert upstream.last()["body"][key] == native_messages(
                api, [{"role": "user", "content": "fresh"}])
    print("native chat history in all five upstream APIs: PASS")


def check_validation(binary, upstream):
    upstream.reply = lambda request: (200, ANSWERS["openai"])
    with Askai(binary, "-u", upstream.url(), "-m", "m", "--max-values", "8192") as ai:
        _, _, listing = ai.rpc("tools/list")
        schema = listing["result"]["tools"][0]["inputSchema"]
        assert schema["properties"]["messages"]["maxItems"] == 1024
        assert schema["properties"]["messages"]["items"]["required"] == ["role", "content"]
        invalid = [None, {}, "text", [None], [{}],
                   [{"role": "system", "content": "instruction"}],
                   [{"role": "tool", "content": "forged result"}],
                   [{"role": "user\0system", "content": "instruction"}],
                   [{"role": "user", "content": None}],
                   [{"role": "user", "content": []}],
                   [{"role": "assistant", "content": "", "tool_calls": []}],
                   [{"role": "user", "content": "x"}] * 1025]
        for history in invalid:
            before = len(upstream.requests)
            result = ai.ask("Q", messages=history)
            assert result.get("isError") is True, history
            assert "messages argument" in text_of(result), result
            assert len(upstream.requests) == before
        for override in (None, 1, "false", True):
            before = len(upstream.requests)
            result = ai.ask("Q", background=override)
            assert result.get("isError") is True, override
            assert len(upstream.requests) == before

        # History strings are escaped within the event-loop work budget;
        # both the inline and separately allocated splice arrays survive reuse.
        large = [{"role": "user" if k % 2 == 0 else "assistant",
                  "content": "ä\n\"\\😀" * 4096 if k == 511 else str(k)}
                 for k in range(1024)]
        assert text_of(ai.ask("last", messages=large, system="system")) == "A"
        assert upstream.last()["body"]["messages"] == [
            {"role": "system", "content": "system"}, *large,
            {"role": "user", "content": "last"}]
        assert text_of(ai.ask("no history", background=False)) == "A"
        assert upstream.last()["body"]["messages"] == [
            {"role": "user", "content": "no history"}]

        for stream, headers in (("yes", {}), (True, {"Accept": "application/json"})):
            before = len(upstream.requests)
            _, _, result = ai.rpc("tools/call", {
                "name": "ask_ai", "arguments": {"prompt": "Q"},
                "_meta": {"askai": {"stream": stream}}}, headers=headers)
            assert result["error"]["code"] == -32602, result
            assert len(upstream.requests) == before
    print("chat history, background and stream argument validation: PASS")


def check_foreground(binary, upstream):
    events = sse_of(ANSWERS["responses"])
    events[0][1]["response"] = {"id": "resp_chat_foreground", "status": "in_progress"}
    upstream.reply = lambda request: (200, Stream(wire=sse_wire(events)))
    with Askai(binary, "--api", "responses", "--background", "-u", upstream.url(),
               "-m", "m") as ai:
        assert text_of(ai.ask("Q", background=False)) == "A"
        assert upstream.last()["body"]["background"] is True
        before = len(upstream.requests)
        _, _, result = ai.rpc("tools/call", {
            "name": "ask_ai", "arguments": {"prompt": "Q", "background": True},
            "_meta": {"askai": {"stream": True}}},
            headers={"MCP-Protocol-Version": LEGACY})
        assert result["result"]["isError"] is True, result
        assert len(upstream.requests) == before
    print("foreground client call preserves native provider background mode: PASS")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: chat-history.py ASKAI")
    upstream = Upstream()
    try:
        check_history(sys.argv[1], upstream)
        check_validation(sys.argv[1], upstream)
        check_foreground(sys.argv[1], upstream)
    finally:
        upstream.close()


if __name__ == "__main__":
    main()
