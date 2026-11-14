/** @file chatgpt.c
 * OpenAI's public OAuth flow and renewable ChatGPT plan credentials.
 */
#define _GNU_SOURCE
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <stdio.h>
#include <sys/file.h>

#include "buf.h"
#include "chatgpt.h"
#include "clk.h"
#include "cstr_view.h"
#include "dstr.h"
#include "hexid.h"
#include "http.h"
#include "httpd.h"
#include "jb.h"
#include "loop.h"
#include "policy.h"
#include "registry.h"
#include "stream.h"
#include "util.h"

#define CHATGPT_ISSUER "https://auth.openai.com"
#define CHATGPT_AUTHORIZE CHATGPT_ISSUER "/api/accounts/authorize"
#define CHATGPT_TOKEN CHATGPT_ISSUER "/api/accounts/oauth/token"
#define CHATGPT_JWKS CHATGPT_ISSUER "/.well-known/jwks.json"
#define CHATGPT_RESOURCE "https://api.openai.com/v1"
#define CHATGPT_SCOPE "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
#define CHATGPT_DIRECT "chatgpt.tokens.use.direct"

enum { CHATGPT_LIMIT = 1 << 20, CHATGPT_TOKEN_LIMIT = 64 << 10, CHATGPT_WAIT = 300 };

struct chatgpt_file {
	struct json_object *record;
	struct dstr         name;
	int                 directory;
	int                 lock;
};

/* Keep JSON strings counted, including when a field contains a NUL. */
static struct cstr_view
chatgpt_text (struct json_object *object,
              char const         *key)
{
	return jb_text(jb_get(object, key), 1, CHATGPT_TOKEN_LIMIT, nullptr);
}

int
chatgpt_path (struct dstr *path)
{
	char const *base, *rest;
	int e = util_state_path(&base, &rest);
	dstr_clear(path);
	return e ? e : dstr_printf(path, "%s%s/chatgpt.json", base, rest) ? 0 : ENOMEM;
}

static void
chatgpt_file_fini (struct chatgpt_file *file)
{
	json_object_put(file->record);
	file->record = nullptr;
	dstr_cleanup(&file->name);
	util_close(&file->lock);
	util_close(&file->directory);
}

/* The credential and lock share an opened directory. No path component
 * may redirect the transaction through a symbolic link.
 */
static struct chatgpt_file
chatgpt_file (char const *path,
              bool        create,
              int        *e)
{
	struct chatgpt_file file = { .name = dstr(nullptr), .directory = -1, .lock = -1 };
	dstr_auto scratch = dstr(nullptr);
	*e = !path || !*path ? EINVAL : cstr_view_store(cstr_view(path), &scratch);
	if (!*e && create)
		*e = util_mkdir_parents((char *)dstr_raw(&scratch));
	int base = *e ? -1 : open(*path == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (!*e && base < 0)
		*e = errno;
	char const *leaf = nullptr;
	if (!*e)
		*e = util_parent_open(base, path + (*path == '/'), &leaf, &file.directory);
	util_close(&base);
	if (!*e)
		*e = util_private_fd(file.directory, S_IFDIR);
	if (!*e)
		*e = cstr_view_store(cstr_view(leaf), &file.name);
	dstr_clear(&scratch);
	if (!*e && !dstr_printf(&scratch, "%s.lock", dstr_get(&file.name)))
		*e = ENOMEM;
	if (!*e)
		*e = util_private_open(file.directory, dstr_get(&scratch), O_RDWR | O_CREAT | O_NONBLOCK, &file.lock);
	uint64_t start = clk_monotonic_ns();
	while (!*e && flock(file.lock, LOCK_EX | LOCK_NB)) {
		if (errno != EINTR && errno != EWOULDBLOCK) {
			*e = errno;
			break;
		}
		uint64_t now = clk_monotonic_ns();
		if (!start || !now || now - start >= 2 * NS) {
			*e = start && now ? EBUSY : EIO;
			break;
		}
		struct timespec delay = clk_timespec(20 * MS);
		int wait = clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, nullptr);
		if (wait && wait != EINTR)
			*e = wait;
	}
	int fd = -1;
	if (!*e)
		*e = util_private_open(file.directory, dstr_get(&file.name), O_RDONLY | O_NONBLOCK, &fd);
	if (*e == ENOENT && create && file.lock >= 0) {
		file.record = json_object_new_object();
		*e = file.record ? 0 : ENOMEM;
		return file;
	}
	struct stat st;
	if (!*e && fstat(fd, &st))
		*e = errno;
	struct buf bytes = buf();
	if (!*e)
		*e = st.st_size < 0 || st.st_size > CHATGPT_LIMIT ? EFBIG :
		     buf_read(&bytes, fd, (size_t)st.st_size, (size_t)st.st_size);
	util_close(&fd);
	if (!*e) {
		struct span text = buf_peek(&bytes);
		file.record = text.len ? jb_parse((char const *)text.ptr, text.len, JSON_TOKENER_STRICT, 16) : nullptr;
		if (!json_object_is_type(file.record, json_type_object))
			*e = EINVAL;
	}
	buf_wipe(&bytes);
	buf_fini(&bytes);
	return file;
}

static int
chatgpt_file_save (struct chatgpt_file *file,
                   struct json_object *record)
{
	struct hexid id = {};
	int e = hexid_make(&id);
	dstr_auto temporary = dstr(nullptr);
	if (!e && !dstr_printf(&temporary, ".chatgpt-%s", id.hex))
		e = ENOMEM;
	struct cstr_view text = jb_wire(record);
	if (!e && !text.data)
		e = ENOMEM;
	int fd = -1;
	if (!e)
		e = util_private_open(file->directory, dstr_get(&temporary), O_WRONLY | O_CREAT | O_EXCL, &fd);
	if (fd < 0)
		return e;
	e = fchmod(fd, 0600) ? errno : util_write_all(fd, text.data, text.len);
	if (!e && fsync(fd))
		e = errno;
	if (fd >= 0 && close(fd) && !e)
		e = errno;
	fd = -1;
	if (!e && (renameat(file->directory, dstr_get(&temporary), file->directory, dstr_get(&file->name)) ||
	           fsync(file->directory)))
		e = errno;
	if (e && dstr_len(&temporary))
		(void)unlinkat(file->directory, dstr_get(&temporary), 0);
	return e;
}

struct chatgpt_field {
	struct cstr_view name;
	struct cstr_view value;
};

/* Reserve once before copying secrets; escaped values remain ASCII. */
static int
chatgpt_form (struct dstr               *form,
              struct chatgpt_field const *fields,
              size_t                     count)
{
	dstr_clear(form);
	size_t size = 0;
	for (size_t i = 0; i < count; ++i) {
		if (!fields[i].value.data || fields[i].value.len > CHATGPT_TOKEN_LIMIT)
			return EINVAL;
		size += fields[i].name.len + 3 * fields[i].value.len + 2;
	}
	if (!dstr_reserve(form, size))
		return ENOMEM;
	int e = 0;
	for (size_t i = 0; !e && i < count; ++i) {
		char *escaped = curl_easy_escape(nullptr, fields[i].value.data, (int)fields[i].value.len);
		if (!escaped)
			return ENOMEM;
		struct cstr_view value = cstr_view(escaped);
		e = (i ? cstr_view_cat(CSTR_VIEW("&"), form) : 0) ||
		    cstr_view_cat(fields[i].name, form) || cstr_view_cat(CSTR_VIEW("="), form) ||
		    cstr_view_cat(value, form) ? ENOMEM : 0;
		OPENSSL_cleanse(escaped, value.len);
		curl_free(escaped);
	}
	return e;
}

static int
chatgpt_query (struct cstr_view    query,
               struct json_object **result)
{
	*result = nullptr;
	if (query.len > CHATGPT_TOKEN_LIMIT)
		return EFBIG;
	*result = json_object_new_object();
	if (!*result)
		return ENOMEM;
	dstr_auto text = dstr(nullptr);
	int e = dstr_reserve(&text, query.len) ? 0 : ENOMEM;
	while (!e && query.len) {
		struct cstr_view pair = query, name;
		if (!cstr_view_cut(&query, '&', &pair))
			query = cstr_view(nullptr);
		if (!cstr_view_cut(&pair, '=', &name)) {
			e = EINVAL;
			break;
		}
		struct json_object *parts[2] = {};
		struct cstr_view const encoded[] = { name, pair };
		for (size_t i = 0; !e && i < array_size(encoded); ++i) {
			e = cstr_view_store(encoded[i], &text);
			if (e)
				break;
			unsigned char *bytes = dstr_raw(&text);
			for (size_t j = 0; !e && j < encoded[i].len; ++j) {
				if (bytes[j] == '+')
					bytes[j] = ' ';
				else if (bytes[j] == '%' &&
				         (encoded[i].len - j < 3 || util_hex_digit(bytes[j + 1]) > 15 ||
				          util_hex_digit(bytes[j + 2]) > 15))
					e = EINVAL;
			}
			if (e)
				break;
			int length = 0;
			char *decoded = curl_easy_unescape(nullptr, dstr_get(&text), (int)encoded[i].len, &length);
			if (!decoded)
				e = ENOMEM;
			else if (memchr(decoded, '\0', (size_t)length))
				e = EINVAL;
			else if (!(parts[i] = json_object_new_string_len(decoded, length)))
				e = ENOMEM;
			if (decoded)
				OPENSSL_cleanse(decoded, (size_t)length);
			curl_free(decoded);
			OPENSSL_cleanse(dstr_raw(&text), dstr_len(&text));
		}
		struct cstr_view key = jb_view(parts[0]);
		if (!e && (!key.len || jb_get(*result, key.data)))
			e = EINVAL;
		if (!e)
			e = jb_set(*result, key.data, json_object_get(parts[1]));
		json_object_put(parts[0]);
		json_object_put(parts[1]);
	}
	OPENSSL_cleanse(dstr_raw(&text), dstr_len(&text));
	return e;
}

/* A token exchange runs on a blocker, or before serving during login.
 * Its private loop uses the same HTTP bridge as upstream calls. No loop
 * of the running server waits for this exchange.
 */
struct chatgpt_response {
	struct loop *loop;
	struct buf   body;
	long         status;
	int          error;
};

static size_t
chatgpt_response_receive (char const *data,
                          size_t      length,
                          void       *opaque)
{
	struct chatgpt_response *response = opaque;
	response->error = buf_put(&response->body, data, length, CHATGPT_LIMIT);
	if (response->error == ENOBUFS)
		response->error = EFBIG;
	return response->error ? 0 : length;
}

static void
chatgpt_response_done (CURLcode    result,
                       long        status,
                       char const *error,
                       void       *opaque)
{
	(void)error;
	struct chatgpt_response *response = opaque;
	response->status = status;
	if (!response->error && result != CURLE_OK)
		response->error = result == CURLE_OPERATION_TIMEDOUT ? ETIMEDOUT : EIO;
	loop_exit(response->loop);
}

/* Upstream bodies and curl's error text never enter diagnostics: an
 * authorization server may echo credentials in either.
 */
static int
chatgpt_http (char const          *url,
              struct cstr_view     form,
              struct json_object **result,
              long                *status)
{
	*result = nullptr;
	*status = 0;
	struct loop run = loop();
	int e = loop_init(&run);
	if (e) {
		loop_fini(&run);
		return e;
	}
	struct http *http = http_create(&run, nullptr, &e);
	struct chatgpt_response response = { .loop = &run, .body = buf() };
	/* Reserve once, so growth cannot leave copies of tokens in freed
	 * storage; wipe the whole buffer before giving it back. */
	struct span room = {};
	if (!e)
		e = buf_reserve(&response.body, CHATGPT_LIMIT, CHATGPT_LIMIT, &room);
	static char const *const headers[] = {
		"Content-Type: application/x-www-form-urlencoded",
		"User-Agent: askai/0.1",
		nullptr
	};
	struct http_request *request = nullptr;
	if (!e)
		e = http_post_json(http, url, form.data, form.len, headers, 60000,
		                   chatgpt_response_receive, chatgpt_response_done,
		                   &response, &request);
	if (!e)
		e = loop_exec(&run);
	if (!e)
		e = response.error;
	*status = response.status;
	if (!e) {
		struct cstr_view body = cstr_view_span(buf_peek(&response.body));
		*result = body.len ? jb_parse(body.data, body.len, JSON_TOKENER_STRICT, 24) : nullptr;
		if (!json_object_is_type(*result, json_type_object))
			e = EPROTO;
	}
	http_destroy(&http);
	loop_fini(&run);
	buf_wipe(&response.body);
	buf_fini(&response.body);
	return e;
}

/* The listener and its one client lane share this loop. httpd's usual
 * handoff messages still pass through the loop's inbox. */
struct chatgpt_browser {
	struct loop         loop;
	struct stream_app   app;
	struct httpd_hub   *hub;
	struct httpd_home  *home;
	struct httpd       *server;
	struct json_object *params;
	struct cstr_view    state;
	struct cstr_view    client;
	uint64_t            deadline;
	int                 error;
	uint16_t            port;
};

static void
chatgpt_browser_head (void          *opaque,
                      struct stream *stream,
                      struct span    method,
                      struct span    target,
                      size_t         length)
{
	/* Every request ends here, including a request with a chunked body.
	 * No body, cancellation or writable callback follows end(). */
	struct chatgpt_browser *browser = opaque;
	struct cstr_view const path = cstr_view_span(target);
	struct cstr_view const prefix = CSTR_VIEW("/auth/callback?");
	struct json_object *params = nullptr;
	int e = browser->params || length ||
	        !cstr_view_equal(cstr_view_span(method), CSTR_VIEW("GET")) ||
	        !cstr_view_starts(path, prefix) ? EINVAL
	      : chatgpt_query(cstr_view_slice(path, prefix.len, SIZE_MAX), &params);
	struct cstr_view state = jb_text(jb_get(params, "state"), 1, CHATGPT_TOKEN_LIMIT, nullptr);
	bool const matched = !e && state.len == browser->state.len &&
	                     !CRYPTO_memcmp(state.data, browser->state.data, state.len);
	if (!matched)
		e = EACCES;
	struct cstr_view const issued = jb_text(jb_get(params, "client_id"), 1, CHATGPT_TOKEN_LIMIT, nullptr);
	struct cstr_view const code = jb_text(jb_get(params, "code"), 1, CHATGPT_TOKEN_LIMIT, nullptr);
	if (!e && (jb_get(params, "error") || !code.len ||
	           (!browser->client.len && (!issued.len || cstr_view_equal(issued, CSTR_VIEW("dynamic_agent_client")))) ||
	           (browser->client.len && issued.len && !cstr_view_equal(browser->client, issued)) ||
	           (jb_get(params, "iss") && !jb_is(jb_get(params, "iss"), CHATGPT_ISSUER))))
		e = EACCES;
	static unsigned char fields[] = "Content-Type: text/plain\r\nCache-Control: no-store\r\n";
	static unsigned char accepted[] = "Authorization received. Return to askai to finish sign-in.\n";
	static unsigned char rejected[] = "Authorization callback rejected.\n";
	struct span const reply = e ? span(rejected, sizeof rejected - 1)
	                           : span(accepted, sizeof accepted - 1);
	int sent = stream->ops->head(stream, e ? 400 : 200,
	                             span(fields, sizeof fields - 1), reply.len);
	if (!sent)
		sent = stream->ops->data(stream, reply);
	stream->ops->end(stream, sent == EAGAIN ? 0 : sent);
	if (matched) {
		browser->params = params;
		params = nullptr;
		browser->error = e;
		browser->deadline = loop_clock() + 2 * NS;
		httpd_home_drain(browser->home);
		httpd_drain(browser->server, browser->deadline);
	}
	json_object_put(params);
}

static void
chatgpt_browser_policy (void *opaque)
{
	(void)opaque;
}

static void
chatgpt_browser_fini (struct chatgpt_browser *browser)
{
	if (browser->loop.epfd < 0)
		return;
	loop_exit(&browser->loop);
	httpd_destroy(&browser->server);
	httpd_home_destroy(&browser->home);
	loop_fini(&browser->loop);
	httpd_hub_destroy(&browser->hub);
	json_object_put(browser->params);
	browser->params = nullptr;
}

/* Initialize each field in place: the loop contains atomics and is never
 * copied. On failure there is nothing left to finalize. */
static int
chatgpt_browser_init (struct chatgpt_browser *browser)
{
	loop_place(&browser->loop);
	browser->app = (struct stream_app){
		.ctx = browser,
		.on_head = chatgpt_browser_head,
		.on_policy = chatgpt_browser_policy
	};
	browser->hub = nullptr;
	browser->home = nullptr;
	browser->server = nullptr;
	browser->params = nullptr;
	browser->state = cstr_view(nullptr);
	browser->client = cstr_view(nullptr);
	browser->deadline = 0;
	browser->error = 0;
	browser->port = 0;
	int e = loop_init(&browser->loop);
	if (e) {
		loop_fini(&browser->loop);
		return e;
	}
	/* An explicit policy admits only loopback, independent of ASKAI_TOKEN.
	 * The callback application offers no tools. */
	struct cstr_view const rules = CSTR_VIEW("{\"roles\":{\"callback\":{\"tools\":[]}},"
	                                        "\"anonymous\":{\"loopback\":\"callback\"}}");
	struct json_object *file = jb_parse(rules.data, rules.len, JSON_TOKENER_STRICT, 8);
	struct registry const registry = {};
	char why[128];
	struct policy *policy = file ? policy_create(file, &registry, 0, 0, why, sizeof why, &e) : nullptr;
	if (!file)
		e = ENOMEM;
	json_object_put(file);
	char const *const listen[] = { "127.0.0.1:0", nullptr };
	struct httpd_cfg cfg = {
		.listen = listen,
		.policy = policy,
		.limits = httpd_limits(),
		.quiet = true
	};
	cfg.limits.body = 1;
	cfg.limits.bodies = 8;
	cfg.limits.clients = 8;
	cfg.limits.head_time = cfg.limits.idle_time = cfg.limits.write_time = 5;
	static _Atomic uint64_t const calls = 0;
	if (!e)
		browser->hub = httpd_hub_create(1, &e);
	if (!e)
		browser->home = httpd_home_create(&browser->loop, browser->hub, &cfg, &calls, &e);
	if (!e)
		browser->server = httpd_create(&browser->loop, browser->hub, 0, &browser->app, &cfg, &e);
	if (!e)
		e = httpd_home_port(browser->home, 0, &browser->port);
	policy_put(policy);
	if (e)
		chatgpt_browser_fini(browser);
	return e;
}

static int
chatgpt_browser_wait (struct chatgpt_browser *browser,
                      struct cstr_view        state,
                      struct cstr_view        client,
                      struct json_object    **params)
{
	*params = nullptr;
	if (!state.len)
		return EINVAL;
	browser->state = state;
	browser->client = client;
	browser->deadline = loop_clock() + CHATGPT_WAIT * NS;
	httpd_home_open(browser->home);
	int e = 0;
	while (!(browser->loop.flags & LOOP_EXIT) && loop_clock() < browser->deadline)
		if ((e = loop_turn(&browser->loop, browser->deadline)))
			break;
	if (!e)
		e = browser->loop.error;
	if (!e)
		e = browser->params ? browser->error : ETIMEDOUT;
	*params = browser->params;
	browser->params = nullptr;
	return e;
}

/* PKCE, state and nonce all use 256 bits, encoded without padding. */
static int
chatgpt_encode (unsigned char const input[32],
                char                output[45])
{
	int n = EVP_EncodeBlock((unsigned char *)output, input, 32);
	if (n != 44)
		return EIO;
	output[43] = '\0';
	for (size_t i = 0; i < 43; ++i)
		if (output[i] == '+') output[i] = '-';
		else if (output[i] == '/') output[i] = '_';
	return 0;
}

/* JWT and JWK segments are ASCII; their decoded bytes belong in buf. */
static int
chatgpt_decode (struct cstr_view input,
                struct buf      *bytes)
{
	buf_clear(bytes);
	if (!input.len || input.len > CHATGPT_TOKEN_LIMIT || input.len % 4 == 1 ||
	    cstr_view_spn(input, UTIL_ALNUM "-_") != input.len)
		return EINVAL;
	dstr_auto encoded = dstr(nullptr);
	int e = cstr_view_store(input, &encoded);
	unsigned char *text = dstr_raw(&encoded);
	for (size_t i = 0; !e && i < input.len; ++i)
		if (text[i] == '-') text[i] = '+';
		else if (text[i] == '_') text[i] = '/';
	size_t pad = (4 - input.len % 4) % 4;
	if (!e)
		e = cstr_view_cat(cstr_view_n("===", pad), &encoded);
	struct span room = {};
	if (!e)
		e = buf_reserve(bytes, dstr_len(&encoded) / 4 * 3, CHATGPT_TOKEN_LIMIT, &room);
	if (!e) {
		int n = EVP_DecodeBlock(room.ptr, dstr_raw(&encoded), (int)dstr_len(&encoded));
		if (n < 0)
			e = EINVAL;
		else
			buf_commit(bytes, (size_t)n - pad);
	}
	return e;
}

static EVP_PKEY *
chatgpt_rsa (struct json_object *jwk)
{
	struct buf bytes = buf();
	int e = chatgpt_decode(chatgpt_text(jwk, "n"), &bytes);
	struct span decoded = buf_peek(&bytes);
	BIGNUM *n = e ? nullptr : BN_bin2bn(decoded.ptr, (int)decoded.len, nullptr);
	if (!e)
		e = chatgpt_decode(chatgpt_text(jwk, "e"), &bytes);
	decoded = buf_peek(&bytes);
	BIGNUM *exponent = e ? nullptr : BN_bin2bn(decoded.ptr, (int)decoded.len, nullptr);
	buf_fini(&bytes);
	OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
	OSSL_PARAM *params = nullptr;
	EVP_PKEY_CTX *ctx = nullptr;
	EVP_PKEY *key = nullptr;
	if (n && exponent && BN_num_bits(n) >= 2048 && builder &&
	    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n) &&
	    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, exponent) &&
	    (params = OSSL_PARAM_BLD_to_param(builder)) &&
	    (ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)) &&
	    EVP_PKEY_fromdata_init(ctx) > 0 &&
	    EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
		EVP_PKEY_free(key);
		key = nullptr;
	}
	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(builder);
	BN_free(n);
	BN_free(exponent);
	return key;
}

static bool
chatgpt_audience (struct json_object *claims,
                  struct cstr_view    client)
{
	struct json_object *aud = jb_get(claims, "aud");
	bool match = cstr_view_equal(jb_view(aud), client);
	for (size_t i = 0, n = jb_len(aud); i < n; ++i)
		match |= cstr_view_equal(jb_view(jb_at(aud, i)), client);
	struct cstr_view azp = chatgpt_text(claims, "azp");
	return match && (!azp.len || cstr_view_equal(azp, client)) && (jb_len(aud) <= 1 || azp.len);
}

/* Verify only with the pinned issuer's keys. JWT key URLs and algorithms
 * other than RS256 never select verification material.
 */
static int
chatgpt_identity (struct json_object  *keys,
                  struct cstr_view     token,
                  struct cstr_view     client,
                  struct cstr_view     nonce,
                  struct cstr_view     subject,
                  struct json_object **identity)
{
	*identity = nullptr;
	struct cstr_view signed_text = token, header, payload;
	if (!cstr_view_cut(&token, '.', &header) || !cstr_view_cut(&token, '.', &payload) ||
	    memchr(token.data, '.', token.len))
		return EINVAL;
	signed_text.len -= token.len + 1;
	struct json_object *parts[2] = {};
	struct cstr_view const encoded[] = { header, payload };
	struct buf bytes = buf();
	int e = 0;
	for (size_t i = 0; !e && i < array_size(encoded); ++i) {
		e = chatgpt_decode(encoded[i], &bytes);
		struct span text = buf_peek(&bytes);
		if (!e && !json_object_is_type(parts[i] = jb_parse((char const *)text.ptr, text.len,
		                                                   JSON_TOKENER_STRICT, 16), json_type_object))
			e = EINVAL;
	}
	struct json_object *head = parts[0], *claims = parts[1];
	struct cstr_view kid = chatgpt_text(head, "kid"), sub = chatgpt_text(claims, "sub");
	int64_t now = (int64_t)(clk_realtime_ns() / NS);
	if (!e && (!now || !jb_is(jb_get(head, "alg"), "RS256") || !kid.len || jb_get(head, "crit") ||
	           !jb_is(jb_get(claims, "iss"), CHATGPT_ISSUER) || !chatgpt_audience(claims, client) ||
	           !sub.len || jb_int(jb_get(claims, "exp"), 0) <= now ||
	           jb_int(jb_get(claims, "iat"), INT64_MAX) > now + 60 ||
	           (jb_get(claims, "nbf") && jb_int(jb_get(claims, "nbf"), INT64_MAX) > now + 60) ||
	           (nonce.len && !cstr_view_equal(chatgpt_text(claims, "nonce"), nonce)) ||
	           (subject.len && !cstr_view_equal(sub, subject))))
		e = EACCES;
	EVP_PKEY *key = nullptr;
	struct json_object *list = jb_get(keys, "keys");
	for (size_t i = 0, n = jb_len(list); !e && !key && i < n; ++i) {
		struct json_object *jwk = jb_at(list, i);
		if (cstr_view_equal(chatgpt_text(jwk, "kid"), kid) && jb_is(jb_get(jwk, "kty"), "RSA") &&
		    (!jb_get(jwk, "alg") || jb_is(jb_get(jwk, "alg"), "RS256")) &&
		    (!jb_get(jwk, "use") || jb_is(jb_get(jwk, "use"), "sig")))
			key = chatgpt_rsa(jwk);
	}
	if (!e)
		e = chatgpt_decode(token, &bytes);
	struct span signature = buf_peek(&bytes);
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!e && (!key || !ctx || EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) <= 0 ||
	           EVP_DigestVerify(ctx, signature.ptr, signature.len,
	                            (unsigned char const *)signed_text.data, signed_text.len) != 1))
		e = EACCES;
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(key);
	buf_fini(&bytes);
	json_object_put(head);
	if (!e)
		*identity = claims;
	else
		json_object_put(claims);
	return e;
}

static bool
chatgpt_scope (struct json_object *record)
{
	struct json_object *scope = jb_get(record, "scopes");
	for (size_t i = 0, n = jb_len(scope); i < n; ++i)
		if (jb_is(jb_at(scope, i), CHATGPT_DIRECT))
			return true;
	return false;
}

static int
chatgpt_tokens (struct chatgpt_file *file,
                struct json_object *tokens,
                struct json_object *keys,
                struct cstr_view    nonce,
                bool                login)
{
	struct cstr_view access = jb_text(jb_get(tokens, "access_token"), 1, CHATGPT_TOKEN_LIMIT, UTIL_ALNUM "-._~+/=");
	struct cstr_view id = chatgpt_text(tokens, "id_token");
	struct cstr_view refresh = chatgpt_text(tokens, "refresh_token");
	struct cstr_view scope = jb_text(jb_get(tokens, "scope"), 0, CHATGPT_TOKEN_LIMIT, nullptr);
	struct cstr_view client = chatgpt_text(file->record, "client_id");
	uint64_t seconds = jb_count(jb_get(tokens, "expires_in"), 86400 * 30), now = clk_realtime_ns() / NS;
	if (!client.len || cstr_view_equal(client, CSTR_VIEW("dynamic_agent_client")) || !access.len ||
	    !cstr_view_equal_case(chatgpt_text(tokens, "token_type"), CSTR_VIEW("Bearer")) ||
	    seconds <= 60 || !now || (login && (!id.len || !scope.len || !refresh.len)) ||
	    (jb_get(tokens, "id_token") && !id.len) || (jb_get(tokens, "refresh_token") && !refresh.len) ||
	    (jb_get(tokens, "scope") && !scope.data))
		return EPROTO;
	struct json_object *identity = nullptr, *record = nullptr;
	int e = id.len ? chatgpt_identity(keys, id, client, nonce, chatgpt_text(file->record, "subject"), &identity) : 0;
	if (!e && json_object_deep_copy(file->record, &record, nullptr))
		e = ENOMEM;
	static char const *const names[] = { "access_token", "refresh_token", "id_token" };
	for (size_t i = 0; !e && i < array_size(names); ++i)
		if (chatgpt_text(tokens, names[i]).len)
			e = jb_set(record, names[i], json_object_get(jb_get(tokens, names[i])));
	if (!e)
		e = jb_set(record, "expires_at", json_object_new_uint64(now + seconds));
	if (!e && identity)
		e = jb_set(record, "subject", json_object_get(jb_get(identity, "sub"))) ||
		    jb_set(record, "issuer", json_object_new_string(CHATGPT_ISSUER)) ? ENOMEM : 0;
	if (!e && identity && jb_get(identity, "email"))
		e = jb_set(record, "email", json_object_get(jb_get(identity, "email")));
	if (!e && scope.data) {
		struct json_object *list = json_object_new_array();
		while (!e && scope.len) {
			struct cstr_view word = scope;
			if (!cstr_view_cut(&scope, ' ', &word))
				scope = cstr_view(nullptr);
			if (word.len)
				e = jb_push(list, jb_string(word));
		}
		if (!e)
			e = jb_set(record, "scopes", json_object_get(list));
		json_object_put(list);
	}
	bool allowed = !e && chatgpt_scope(record);
	if (!e)
		e = chatgpt_file_save(file, record);
	if (!e) {
		json_object_put(file->record);
		file->record = record;
		record = nullptr;
	}
	json_object_put(record);
	json_object_put(identity);
	return e ? e : allowed ? 0 : EACCES;
}

static int
chatgpt_exchange (struct chatgpt_file *file,
                  struct cstr_view    form,
                  struct cstr_view    nonce,
                  bool                login)
{
	struct json_object *tokens = nullptr, *keys = nullptr;
	long status = 0;
	/* Fetch verification material before consuming a code or rotating a
	 * refresh token, so a JWKS outage leaves the grant usable. */
	int e = chatgpt_http(CHATGPT_JWKS, cstr_view(nullptr), &keys, &status);
	if (!e && (status != 200 || !jb_len(jb_get(keys, "keys"))))
		e = EIO;
	if (!e)
		e = chatgpt_http(CHATGPT_TOKEN, form, &tokens, &status);
	if (!e && status != 200) {
		static struct cstr_view const expired[] = {
			CSTR_VIEW_INIT("invalid_grant"), CSTR_VIEW_INIT("invalid_refresh_token"),
			CSTR_VIEW_INIT("token_expired"), CSTR_VIEW_INIT("refresh_token_expired"),
			CSTR_VIEW_INIT("refresh_token_invalidated"), CSTR_VIEW_INIT("refresh_token_reused")
		};
		if (!login && status >= 400 && status <= 403 &&
		    cstr_view_index(expired, array_size(expired), chatgpt_text(tokens, "error")) < array_size(expired)) {
			json_object_object_del(file->record, "access_token");
			json_object_object_del(file->record, "refresh_token");
			json_object_object_del(file->record, "id_token");
			json_object_object_del(file->record, "expires_at");
			e = chatgpt_file_save(file, file->record);
		}
		if (!e)
			e = status >= 400 && status <= 403 ? EACCES : EIO;
	}
	if (!e)
		e = chatgpt_tokens(file, tokens, keys, nonce, login);
	json_object_put(tokens);
	json_object_put(keys);
	return e;
}

int
chatgpt_header (char const  *path,
                struct dstr *header,
                char        *error,
                size_t       size)
{
	dstr_clear(header);
	CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
	int e;
	struct chatgpt_file file = chatgpt_file(path, false, &e);
	if (!e && curl_status != CURLE_OK)
		e = EIO;
	dstr_auto form = dstr(nullptr);
	struct cstr_view client = chatgpt_text(file.record, "client_id");
	if (!e && (!chatgpt_text(file.record, "subject").len || !client.len ||
	           cstr_view_equal(client, CSTR_VIEW("dynamic_agent_client")) || !chatgpt_scope(file.record) ||
	           !jb_text(jb_get(file.record, "access_token"), 1, CHATGPT_TOKEN_LIMIT, UTIL_ALNUM "-._~+/=").len ||
	           !jb_count(jb_get(file.record, "expires_at"), INT64_MAX)))
		e = EACCES;
	uint64_t now = clk_realtime_ns() / NS;
	if (!e && !now)
		e = EIO;
	if (!e && jb_uint(jb_get(file.record, "expires_at"), 0) <= now + 60) {
		struct cstr_view refresh = chatgpt_text(file.record, "refresh_token");
		struct chatgpt_field const fields[] = {
			{ CSTR_VIEW("grant_type"), CSTR_VIEW("refresh_token") },
			{ CSTR_VIEW("client_id"), chatgpt_text(file.record, "client_id") },
			{ CSTR_VIEW("refresh_token"), refresh },
			{ CSTR_VIEW("resource"), CSTR_VIEW(CHATGPT_RESOURCE) }
		};
		e = refresh.len ? chatgpt_form(&form, fields, array_size(fields)) : EACCES;
		if (!e)
			e = chatgpt_exchange(&file, cstr_view_dstr(&form), cstr_view(nullptr), false);
	}
	if (!e && !dstr_printf(header, "Authorization: Bearer %s", chatgpt_text(file.record, "access_token").data))
		e = ENOMEM;
	OPENSSL_cleanse(dstr_raw(&form), dstr_len(&form));
	chatgpt_file_fini(&file);
	if (curl_status == CURLE_OK)
		curl_global_cleanup();
	if (e)
		dstr_clear(header);
	if (e)
		util_format(error, size, e == EBUSY ? "ChatGPT credentials are busy; retry after sign-in or refresh" :
		            e == EACCES || e == ENOENT ? "ChatGPT credentials unavailable; run --chatgpt-login and allow plan usage" :
		            "ChatGPT authentication failed: %s", strerror(e));
	else if (size)
		*error = '\0';
	return e;
}

static int
chatgpt_host (struct chatgpt_file *file)
{
	if (chatgpt_text(file->record, "ext_agent_host_id").len)
		return 0;
	struct hexid id = {};
	int e = hexid_make(&id);
	if (e)
		return e;
	id.hex[12] = '4';
	id.hex[16] = "89ab"[util_hex_digit((unsigned char)id.hex[16]) & 3];
	UTIL_TEXT(47) host = {};
	e = util_text_printf(&host, "urn:uuid:%.8s-%.4s-%.4s-%.4s-%.12s",
	                     id.hex, id.hex + 8, id.hex + 12, id.hex + 16, id.hex + 20);
	if (!e)
		e = jb_set(file->record, "ext_agent_host_id", jb_string(util_text_view(&host)));
	return e ? e : chatgpt_file_save(file, file->record);
}

int
chatgpt_login (char const *path,
               char       *error,
               size_t      size)
{
	CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
	int e;
	struct chatgpt_file file = chatgpt_file(path, true, &e);
	if (!e && curl_status != CURLE_OK)
		e = EIO;
	if (!e)
		e = chatgpt_host(&file);
	unsigned char random[96], digest[32];
	char verifier[45], state[45], nonce[45], challenge[45];
	if (!e)
		e = util_random(random, sizeof random);
	if (!e)
		e = chatgpt_encode(random, verifier) || chatgpt_encode(random + 32, state) ||
		    chatgpt_encode(random + 64, nonce) ? EIO : 0;
	unsigned digest_size = 0;
	if (!e && (!EVP_Digest(verifier, 43, digest, &digest_size, EVP_sha256(), nullptr) || digest_size != sizeof digest))
		e = EIO;
	if (!e)
		e = chatgpt_encode(digest, challenge);
	struct chatgpt_browser browser;
	int browser_error = e;
	if (!e)
		e = browser_error = chatgpt_browser_init(&browser);
	dstr_auto form = dstr(nullptr), redirect = dstr(nullptr);
	if (!e && !dstr_printf(&redirect, "http://127.0.0.1:%u/auth/callback", (unsigned)browser.port))
		e = ENOMEM;
	struct cstr_view client = chatgpt_text(file.record, "client_id");
	if (!e) {
		struct chatgpt_field fields[] = {
			{ CSTR_VIEW("client_id"), client.len ? client : CSTR_VIEW("dynamic_agent_client") },
			{ CSTR_VIEW("ext_agent_host_id"), chatgpt_text(file.record, "ext_agent_host_id") },
			{ CSTR_VIEW("response_type"), CSTR_VIEW("code") },
			{ CSTR_VIEW("redirect_uri"), cstr_view_dstr(&redirect) },
			{ CSTR_VIEW("scope"), CSTR_VIEW(CHATGPT_SCOPE) },
			{ CSTR_VIEW("resource"), CSTR_VIEW(CHATGPT_RESOURCE) },
			{ CSTR_VIEW("state"), cstr_view_n(state, 43) },
			{ CSTR_VIEW("nonce"), cstr_view_n(nonce, 43) },
			{ CSTR_VIEW("code_challenge_method"), CSTR_VIEW("S256") },
			{ CSTR_VIEW("code_challenge"), cstr_view_n(challenge, 43) },
			{ client.len ? CSTR_VIEW("prompt") : CSTR_VIEW("agent_name_hint"),
			  client.len ? CSTR_VIEW("consent") : CSTR_VIEW("askai") }
		};
		e = chatgpt_form(&form, fields, array_size(fields) - (client.len && chatgpt_scope(file.record)));
	}
	if (!e && (fprintf(stderr, "Continue with ChatGPT: open this URL in your browser:\n%s?%s\n",
	                   CHATGPT_AUTHORIZE, dstr_get(&form)) < 0 || fflush(stderr)))
		e = EIO;
	struct json_object *params = nullptr;
	if (!e)
		e = chatgpt_browser_wait(&browser, cstr_view_n(state, 43), client, &params);
	if (!browser_error)
		chatgpt_browser_fini(&browser);
	if (!e && !client.len) {
		e = jb_set(file.record, "client_id", json_object_get(jb_get(params, "client_id")));
		if (!e)
			e = chatgpt_file_save(&file, file.record);
	}
	if (!e) {
		struct chatgpt_field const fields[] = {
			{ CSTR_VIEW("grant_type"), CSTR_VIEW("authorization_code") },
			{ CSTR_VIEW("client_id"), chatgpt_text(file.record, "client_id") },
			{ CSTR_VIEW("code"), chatgpt_text(params, "code") },
			{ CSTR_VIEW("code_verifier"), cstr_view_n(verifier, 43) },
			{ CSTR_VIEW("redirect_uri"), cstr_view_dstr(&redirect) },
			{ CSTR_VIEW("resource"), CSTR_VIEW(CHATGPT_RESOURCE) }
		};
		e = chatgpt_form(&form, fields, array_size(fields));
		if (!e)
			e = chatgpt_exchange(&file, cstr_view_dstr(&form), cstr_view_n(nonce, 43), true);
	}
	OPENSSL_cleanse(dstr_raw(&form), dstr_len(&form));
	OPENSSL_cleanse(verifier, sizeof verifier);
	OPENSSL_cleanse(random, sizeof random);
	json_object_put(params);
	chatgpt_file_fini(&file);
	if (curl_status == CURLE_OK)
		curl_global_cleanup();
	if (e)
		util_format(error, size, e == ETIMEDOUT ? "ChatGPT sign-in timed out" :
		            e == EACCES ? "ChatGPT sign-in rejected; verify the account and allow plan usage" :
		            "ChatGPT sign-in failed: %s", strerror(e));
	else if (size)
		*error = '\0';
	return e;
}
