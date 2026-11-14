/** @file OAuth tests with real cryptography, storage and callback HTTP. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <sys/socket.h>

#include "check.h"
#include "http.h"

static int chatgpt_test_post (struct http *, char const *, char const *, size_t,
                              char const *const *, long, http_write_cb *,
                              http_done_cb *, void *, struct http_request **);

/* Replace only the outbound bridge boundary. Login uses httpd unchanged. */
#define http_post_json chatgpt_test_post
#include "../src/chatgpt.c"
#undef http_post_json

struct chatgpt_test_reply {
	char const *url;
	char const *body;
	char const *grant;
	long        status;
	CURLcode    result;
};

static struct {
	struct chatgpt_test_reply const *replies;
	struct dstr                     last_form;
	size_t                          count;
	size_t                          next;
} chatgpt_test_http = { .last_form = DSTR_INIT };

static int
chatgpt_test_post (struct http          *http,
                   char const           *url,
                   char const           *body,
                   size_t                length,
                   char const *const    *headers,
                   long                  timeout,
                   http_write_cb        *write,
                   http_done_cb         *done,
                   void                 *user,
                   struct http_request **request)
{
	(void)http;
	*request = nullptr;
	CHECK(chatgpt_test_http.next < chatgpt_test_http.count);
	if (chatgpt_test_http.next >= chatgpt_test_http.count)
		return EIO;
	struct chatgpt_test_reply const *reply = &chatgpt_test_http.replies[chatgpt_test_http.next++];
	CHECK(!strcmp(url, reply->url));
	CHECK(timeout > 0);
	CHECK(cstr_view_store(cstr_view_n(body, length), &chatgpt_test_http.last_form) == 0);
	if (reply->grant) {
		CHECK(body && strstr(body, reply->grant));
		CHECK(headers && !strcmp(headers[0], "Content-Type: application/x-www-form-urlencoded"));
	}
	CURLcode result = reply->result;
	if (!result && reply->body) {
		size_t n = strlen(reply->body);
		if (write(reply->body, n, user) != n)
			result = CURLE_WRITE_ERROR;
	}
	done(result, reply->status, "never expose upstream secrets", user);
	return 0;
}

static void
chatgpt_test_replies (struct chatgpt_test_reply const *replies,
                      size_t                          count)
{
	chatgpt_test_http.replies = replies;
	chatgpt_test_http.count = count;
	chatgpt_test_http.next = 0;
}

static char *
chatgpt_test_base64 (void const *data,
                     size_t      size)
{
	CHECK(size <= INT_MAX);
	char *out = malloc(4 * ((size + 2) / 3) + 1);
	CHECK(out != nullptr);
	if (!out)
		return nullptr;
	int n = EVP_EncodeBlock((unsigned char *)out, data, (int)size);
	CHECK(n >= 0);
	for (int i = 0; i < n; ++i) {
		if (out[i] == '+')
			out[i] = '-';
		else if (out[i] == '/')
			out[i] = '_';
	}
	while (n > 0 && out[n - 1] == '=')
		--n;
	out[n] = 0;
	return out;
}

static char *
chatgpt_test_bn (EVP_PKEY   *key,
                 char const *name)
{
	BIGNUM *number = nullptr;
	CHECK(EVP_PKEY_get_bn_param(key, name, &number) == 1);
	if (!number)
		return nullptr;
	int n = BN_num_bytes(number);
	unsigned char *bytes = malloc((size_t)n);
	CHECK(bytes != nullptr);
	char *result = nullptr;
	if (bytes) {
		CHECK(BN_bn2bin(number, bytes) == n);
		result = chatgpt_test_base64(bytes, (size_t)n);
	}
	free(bytes);
	bytes = nullptr;
	BN_free(number);
	return result;
}

static char *
chatgpt_test_jwks (EVP_PKEY *key)
{
	char *n = chatgpt_test_bn(key, OSSL_PKEY_PARAM_RSA_N);
	char *e = chatgpt_test_bn(key, OSSL_PKEY_PARAM_RSA_E);
	char *result = nullptr;
	CHECK(n && e && asprintf(&result,
	      "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"test-key\",\"use\":\"sig\","
	      "\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}]}", n, e) >= 0);
	free(n);
	n = nullptr;
	free(e);
	e = nullptr;
	return result;
}

static char *
chatgpt_test_jwt (EVP_PKEY   *key,
                  char const *claims)
{
	char const prefix[] = "{\"alg\":\"RS256\",\"kid\":\"test-key\",\"typ\":\"JWT\"}";
	char *head = chatgpt_test_base64(prefix, sizeof prefix - 1);
	char *body = chatgpt_test_base64(claims, strlen(claims));
	char *message = nullptr;
	CHECK(head && body && asprintf(&message, "%s.%s", head, body) >= 0);
	free(head);
	head = nullptr;
	free(body);
	body = nullptr;
	if (!message)
		return nullptr;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	CHECK(ctx != nullptr);
	CHECK(EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1);
	CHECK(EVP_DigestSignUpdate(ctx, message, strlen(message)) == 1);
	size_t size = 0;
	CHECK(EVP_DigestSignFinal(ctx, nullptr, &size) == 1);
	unsigned char *signature = malloc(size);
	CHECK(signature != nullptr);
	CHECK(EVP_DigestSignFinal(ctx, signature, &size) == 1);
	EVP_MD_CTX_free(ctx);
	char *sig = chatgpt_test_base64(signature, size);
	free(signature);
	signature = nullptr;
	char *result = nullptr;
	CHECK(sig && asprintf(&result, "%s.%s", message, sig) >= 0);
	free(message);
	message = nullptr;
	free(sig);
	sig = nullptr;
	return result;
}

static void
chatgpt_test_identity (void)
{
	EVP_PKEY *key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", (size_t)2048);
	CHECK(key != nullptr);
	if (!key)
		return;
	char *jwks = chatgpt_test_jwks(key);
	struct json_object *keys = json_tokener_parse(jwks);
	struct json_object *claims = jb_object(
		"iss", json_object_new_string(CHATGPT_ISSUER),
		"aud", json_object_new_string("oaiapp_test"),
		"sub", json_object_new_string("user-test"),
		"nonce", json_object_new_string("nonce-test"),
		"iat", json_object_new_int64(time(nullptr)),
		"exp", json_object_new_int64(time(nullptr) + 3600), nullptr);
	CHECK(claims != nullptr);
	char *token = chatgpt_test_jwt(key, jb_wire(claims).data);
	struct json_object *identity = nullptr;
	CHECK(chatgpt_identity(keys, cstr_view(token), cstr_view("oaiapp_test"), cstr_view("nonce-test"), cstr_view(nullptr), &identity) == 0);
	CHECK(identity && jb_is(jb_get(identity, "sub"), "user-test"));
	json_object_put(identity);
	identity = nullptr;
	CHECK(chatgpt_identity(keys, cstr_view(token), cstr_view("oaiapp_other"), cstr_view("nonce-test"), cstr_view(nullptr), &identity) != 0);
	CHECK(chatgpt_identity(keys, cstr_view(token), cstr_view("oaiapp_test"), cstr_view("wrong"), cstr_view(nullptr), &identity) != 0);
	CHECK(chatgpt_identity(keys, cstr_view(token), cstr_view("oaiapp_test"), cstr_view(nullptr), cstr_view("another-user"), &identity) != 0);
	CHECK(identity == nullptr);
	/* A different signature with otherwise valid claims is never accepted. */
	char *signature = strrchr(token, '.') + 1;
	*signature = *signature == 'A' ? 'B' : 'A';
	CHECK(chatgpt_identity(keys, cstr_view(token), cstr_view("oaiapp_test"), cstr_view("nonce-test"), cstr_view(nullptr), &identity) != 0);
	CHECK(identity == nullptr);
	free(token);
	token = nullptr;
	struct {
		char const *field;
		char const *value;
	} const wrong[] = {
		{ "iss", "\"https://untrusted.example\"" },
		{ "exp", "1" },
		{ "exp", "NaN" },
		{ "iat", "9223372036854775807" },
		{ "nbf", "9223372036854775807" },
		{ "sub", "\"\"" },
		{ "aud", "[\"oaiapp_test\",\"other\"]" },
		{ "azp", "\"other\"" },
	};
	for (size_t i = 0; i < array_size(wrong); ++i) {
		struct json_object *copy = nullptr;
		CHECK(json_object_deep_copy(claims, &copy, nullptr) == 0);
		CHECK(jb_set(copy, wrong[i].field, json_tokener_parse(wrong[i].value)) == 0);
		token = chatgpt_test_jwt(key, jb_wire(copy).data);
		CHECK(chatgpt_identity(keys, cstr_view(token), cstr_view("oaiapp_test"), cstr_view("nonce-test"), cstr_view(nullptr), &identity) != 0);
		CHECK(identity == nullptr);
		free(token);
		token = nullptr;
		json_object_put(copy);
	}
	CHECK(jb_set(claims, "aud", json_tokener_parse("[\"oaiapp_test\",\"other\"]")) == 0);
	CHECK(jb_set(claims, "azp", json_object_new_string("oaiapp_test")) == 0);
	token = chatgpt_test_jwt(key, jb_wire(claims).data);
	CHECK(chatgpt_identity(keys, cstr_view(token), cstr_view("oaiapp_test"), cstr_view("nonce-test"), cstr_view("user-test"), &identity) == 0);
	json_object_put(identity);
	json_object_put(claims);
	free(token);
	token = nullptr;
	json_object_put(keys);
	free(jwks);
	jwks = nullptr;
	EVP_PKEY_free(key);
}

static int
chatgpt_test_request (uint16_t    port,
                      char const *query)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(fd >= 0);
	struct sockaddr_in address = { .sin_family = AF_INET,
	                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	                              .sin_port = htons(port) };
	CHECK(connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
	dstr_auto request = dstr(nullptr);
	CHECK(dstr_printf(&request, "GET /auth/callback?%s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", query));
	CHECK(util_write_all(fd, dstr_get(&request), dstr_len(&request)) == 0);
	return fd;
}

static void
chatgpt_test_callback (void)
{
	struct json_object *params = nullptr;
	CHECK(chatgpt_query(CSTR_VIEW("scope=openid+email&code=%2B"), &params) == 0);
	CHECK(jb_is(jb_get(params, "scope"), "openid email") && jb_is(jb_get(params, "code"), "+"));
	json_object_put(params);
	CHECK(chatgpt_query(CSTR_VIEW("state=a&state=b"), &params) == EINVAL);
	json_object_put(params);
	CHECK(chatgpt_query(CSTR_VIEW("state=%xy"), &params) == EINVAL);
	json_object_put(params);
	struct chatgpt_browser browser;
	int e = chatgpt_browser_init(&browser);
	CHECK(e == 0);
	if (e)
		return;
	uint16_t port = 0;
	CHECK(httpd_home_port(browser.home, 0, &port) == 0 && port == browser.port && port);
	CHECK(httpd_home_port(browser.home, 1, &port) == EINVAL);
	CHECK(httpd_home_port(nullptr, 0, &port) == EFAULT);
	CHECK(httpd_home_port(browser.home, 0, nullptr) == EFAULT);
	/* Losing a browser connection or receiving a wrong state leaves the
	 * pending authorization available to the following legitimate callback. */
	int lost = chatgpt_test_request(browser.port, "state=wrong&code=cancelled");
	util_close(&lost);
	int bad = chatgpt_test_request(browser.port, "state=wrong&code=x&client_id=oaiapp_test");
	int good = chatgpt_test_request(browser.port, "state=pending&code=ok&client_id=oaiapp_test");
	CHECK(chatgpt_browser_wait(&browser, CSTR_VIEW("pending"), cstr_view(nullptr), &params) == 0);
	CHECK(jb_is(jb_get(params, "code"), "ok"));
	CHECK(httpd_home_port(browser.home, 0, &port) == EBADF);
	json_object_put(params);
	util_close(&bad);
	util_close(&good);
	chatgpt_browser_fini(&browser);
	char const *const refused[] = {
		"state=pending&code=x&client_id=oaiapp_different",
		"state=pending&error=access_denied",
		"state=pending&code=x&iss=https%3A%2F%2Funtrusted.example",
	};
	for (size_t i = 0; i < array_size(refused); ++i) {
		e = chatgpt_browser_init(&browser);
		CHECK(e == 0);
		if (e)
			break;
		int fd = chatgpt_test_request(browser.port, refused[i]);
		CHECK(chatgpt_browser_wait(&browser, CSTR_VIEW("pending"), CSTR_VIEW("oaiapp_test"), &params) == EACCES);
		json_object_put(params);
		util_close(&fd);
		chatgpt_browser_fini(&browser);
	}
	/* Cancellation does not manufacture callback parameters. */
	e = chatgpt_browser_init(&browser);
	CHECK(e == 0);
	if (!e) {
		loop_exit(&browser.loop);
		CHECK(chatgpt_browser_wait(&browser, CSTR_VIEW("pending"), cstr_view(nullptr), &params) != 0);
		CHECK(params == nullptr);
		chatgpt_browser_fini(&browser);
	}
}

static struct json_object *
chatgpt_test_profile (void)
{
	return jb_object("issuer", json_object_new_string(CHATGPT_ISSUER),
	                 "subject", json_object_new_string("offline-user"),
	                 "client_id", json_object_new_string("oaiapp_offline-test"),
	                 "ext_agent_host_id", json_object_new_string("urn:uuid:01d4e40d-c4e6-40c4-bc1a-f2612469ed54"),
	                 "access_token", json_object_new_string("offline.access-token"),
	                 "refresh_token", json_object_new_string("offline-refresh-secret"),
	                 "id_token", json_object_new_string("offline.id-token"),
	                 "token_type", json_object_new_string("Bearer"),
	                 "expires_at", json_object_new_int64((int64_t)time(nullptr) + 3600),
	                 "scopes", jb_list(json_object_new_string(CHATGPT_DIRECT)),
	                 nullptr);
}

static int
chatgpt_test_save (char const         *path,
                   struct json_object *profile)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return errno;
	struct cstr_view wire = jb_wire(profile);
	int e = wire.data ? util_write_all(fd, wire.data, wire.len) : ENOMEM;
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	return e;
}

static char *
chatgpt_test_read (char const *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return nullptr;
	struct stat st;
	char *data = nullptr;
	if (!fstat(fd, &st) && st.st_size >= 0 && st.st_size < 1048576) {
		size_t got = 0;
		data = malloc((size_t)st.st_size + 1);
		if (data && !util_read_all(fd, data, (size_t)st.st_size, -1, &got))
			data[got] = '\0';
		else {
			free(data);
			data = nullptr;
		}
	}
	CHECK(!close(fd));
	fd = -1;
	return data;
}

static void
chatgpt_test_cached (char const *path)
{
	struct json_object *profile = chatgpt_test_profile();
	CHECK(profile);
	if (!profile)
		return;
	CHECK(!chatgpt_test_save(path, profile));
	char error[256] = {};
	dstr_auto header = dstr(nullptr);
	chatgpt_test_replies(nullptr, 0);
	CHECK(!chatgpt_header(path, &header, error, sizeof error));
	CHECK(cstr_view_equal(cstr_view_dstr(&header), CSTR_VIEW("Authorization: Bearer offline.access-token")));
	dstr_clear(&header);
	CHECK(!chmod(path, 0644));
	CHECK(chatgpt_header(path, &header, error, sizeof error));
	CHECK(!dstr_len(&header));
	CHECK(!chmod(path, 0600));
	char *link = nullptr;
	CHECK(asprintf(&link, "%s.link", path) >= 0);
	if (link) {
		CHECK(!symlink(path, link));
		CHECK(chatgpt_header(link, &header, error, sizeof error));
		CHECK(!dstr_len(&header));
		CHECK(!unlink(link));
		free(link);
		link = nullptr;
	}
	CHECK(asprintf(&link, "%s.hard", path) >= 0);
	if (link) {
		CHECK(linkat(AT_FDCWD, path, AT_FDCWD, link, 0) == 0);
		CHECK(chatgpt_header(path, &header, error, sizeof error) == EACCES);
		CHECK(!dstr_len(&header));
		CHECK(unlink(link) == 0);
		free(link);
		link = nullptr;
	}
	CHECK(!jb_set(profile, "access_token", json_object_new_string("bad\r\nAuthorization: injected")));
	CHECK(!chatgpt_test_save(path, profile));
	CHECK(chatgpt_header(path, &header, error, sizeof error));
	CHECK(!dstr_len(&header));
	CHECK(!jb_set(profile, "access_token", json_object_new_string_len("good\0hidden", 11)));
	CHECK(!chatgpt_test_save(path, profile));
	CHECK(chatgpt_header(path, &header, error, sizeof error));
	CHECK(!dstr_len(&header));
	CHECK(!jb_set(profile, "access_token", json_object_new_string("offline.access-token")));
	CHECK(!jb_set(profile, "scopes", jb_list(json_object_new_string("openid"))));
	CHECK(!chatgpt_test_save(path, profile));
	CHECK(chatgpt_header(path, &header, error, sizeof error));
	CHECK(!dstr_len(&header));
	json_object_put(profile);
}

struct chatgpt_test_worker {
	char const        *path;
	pthread_barrier_t *barrier;
	struct dstr        header;
	char               error[256];
	int                result;
};

static void *
chatgpt_test_header (void *opaque)
{
	struct chatgpt_test_worker *worker = opaque;
	int e = pthread_barrier_wait(worker->barrier);
	CHECK(e == 0 || e == PTHREAD_BARRIER_SERIAL_THREAD);
	worker->result = chatgpt_header(worker->path, &worker->header,
	                                worker->error, sizeof worker->error);
	return nullptr;
}

static void
chatgpt_test_refresh (char const *path)
{
	struct json_object *profile = chatgpt_test_profile();
	CHECK(jb_set(profile, "expires_at", json_object_new_int64(1)) == 0);
	CHECK(chatgpt_test_save(path, profile) == 0);
	char *before = chatgpt_test_read(path);
	CHECK(before != nullptr);
	char error[256];
	dstr_auto header = dstr(nullptr);
	/* A key-server outage must not consume a rotating refresh token. */
	struct chatgpt_test_reply unavailable = {
		CHATGPT_JWKS, "{\"error\":\"temporarily_unavailable\"}", nullptr, 503, CURLE_OK
	};
	chatgpt_test_replies(&unavailable, 1);
	CHECK(chatgpt_header(path, &header, error, sizeof error) != 0);
	CHECK(!dstr_len(&header) && chatgpt_test_http.next == 1);
	char *unchanged = chatgpt_test_read(path);
	CHECK(before && unchanged && !strcmp(before, unchanged));
	free(unchanged);
	unchanged = nullptr;
	struct chatgpt_test_reply failed[] = {
		{ CHATGPT_TOKEN, nullptr, "grant_type=refresh_token", 0, CURLE_OPERATION_TIMEDOUT },
		{ CHATGPT_TOKEN, "{\"error\":\"temporarily_unavailable\",\"error_description\":\"offline-refresh-secret\"}",
		  "grant_type=refresh_token", 503, CURLE_OK },
		{ CHATGPT_TOKEN, "{\"access_token\":\"new-token\",\"token_type\":\"Bearer\",\"expires_in\":NaN}",
		  "grant_type=refresh_token", 200, CURLE_OK },
		{ CHATGPT_TOKEN, "{\"access_token\":\"new-token\",\"token_type\":\"Bearer\",\"expires_in\":\"3600\"}",
		  "grant_type=refresh_token", 200, CURLE_OK },
		{ CHATGPT_TOKEN, "{\"access_token\":\"new-token\",\"token_type\":\"Bearer\",\"expires_in\":true}",
		  "grant_type=refresh_token", 200, CURLE_OK },
	};
	for (size_t i = 0; i < array_size(failed); ++i) {
		struct chatgpt_test_reply sequence[] = {
			{ CHATGPT_JWKS, "{\"keys\":[{}]}", nullptr, 200, CURLE_OK }, failed[i]
		};
		chatgpt_test_replies(sequence, array_size(sequence));
		CHECK(chatgpt_header(path, &header, error, sizeof error) != 0);
		CHECK(!dstr_len(&header));
		CHECK(chatgpt_test_http.next == 2);
		CHECK(!strstr(error, "offline-refresh-secret"));
		char *after = chatgpt_test_read(path);
		CHECK(after && before && !strcmp(before, after));
		free(after);
		after = nullptr;
	}
	struct chatgpt_test_reply success = {
		CHATGPT_TOKEN,
		"{\"access_token\":\"rotated.access\",\"refresh_token\":\"rotated.refresh\","
		"\"token_type\":\"Bearer\",\"expires_in\":3600}",
		"grant_type=refresh_token", 200, CURLE_OK
	};
	struct chatgpt_test_reply sequence[] = {
		{ CHATGPT_JWKS, "{\"keys\":[{}]}", nullptr, 200, CURLE_OK }, success
	};
	chatgpt_test_replies(sequence, array_size(sequence));
	struct stat old;
	CHECK(stat(path, &old) == 0);
	/* Concurrent callers refresh once; absent scope retains the prior grant. */
	pthread_barrier_t barrier;
	CHECK(pthread_barrier_init(&barrier, nullptr, 2) == 0);
	struct chatgpt_test_worker workers[2] = {
		{ .path = path, .barrier = &barrier, .header = DSTR_INIT },
		{ .path = path, .barrier = &barrier, .header = DSTR_INIT }
	};
	pthread_t threads[2];
	for (size_t i = 0; i < array_size(workers); ++i)
		CHECK(pthread_create(&threads[i], nullptr, chatgpt_test_header, &workers[i]) == 0);
	for (size_t i = 0; i < array_size(workers); ++i) {
		CHECK(pthread_join(threads[i], nullptr) == 0);
		CHECK(workers[i].result == 0);
		CHECK(!strcmp(dstr_get(&workers[i].header), "Authorization: Bearer rotated.access"));
		dstr_cleanup(&workers[i].header);
	}
	CHECK(pthread_barrier_destroy(&barrier) == 0);
	CHECK(chatgpt_test_http.next == 2);
	CHECK(strstr(dstr_get(&chatgpt_test_http.last_form), "client_id=oaiapp_offline-test"));
	CHECK(strstr(dstr_get(&chatgpt_test_http.last_form), "refresh_token=offline-refresh-secret"));
	CHECK(strstr(dstr_get(&chatgpt_test_http.last_form), "resource=https%3A%2F%2Fapi.openai.com%2Fv1"));
	CHECK(!strstr(dstr_get(&chatgpt_test_http.last_form), "scope="));
	struct stat fresh;
	CHECK(stat(path, &fresh) == 0);
	CHECK((fresh.st_mode & 0777) == 0600);
	CHECK(fresh.st_ino != old.st_ino);
	char *after = chatgpt_test_read(path);
	struct json_object *saved = after ? json_tokener_parse(after) : nullptr;
	CHECK(saved && jb_is(jb_get(saved, "refresh_token"), "rotated.refresh"));
	CHECK(saved && json_object_get_int64(jb_get(saved, "expires_at")) > time(nullptr) + 60);
	CHECK(saved && chatgpt_scope(saved));
	json_object_put(saved);
	free(after);
	after = nullptr;
	free(before);
	before = nullptr;
	/* Terminal revocation clears tokens, but preserves the registration. */
	CHECK(chatgpt_test_save(path, profile) == 0);
	struct chatgpt_test_reply revoked[] = {
		{ CHATGPT_JWKS, "{\"keys\":[{}]}", nullptr, 200, CURLE_OK },
		{ CHATGPT_TOKEN, "{\"error\":\"invalid_grant\"}", "grant_type=refresh_token", 400, CURLE_OK }
	};
	chatgpt_test_replies(revoked, array_size(revoked));
	CHECK(chatgpt_header(path, &header, error, sizeof error) == EACCES);
	CHECK(!dstr_len(&header) && chatgpt_test_http.next == 2);
	after = chatgpt_test_read(path);
	saved = after ? json_tokener_parse(after) : nullptr;
	CHECK(saved && !jb_get(saved, "access_token") && !jb_get(saved, "refresh_token") && !jb_get(saved, "id_token"));
	CHECK(saved && jb_is(jb_get(saved, "client_id"), "oaiapp_offline-test"));
	CHECK(saved && jb_is(jb_get(saved, "ext_agent_host_id"), "urn:uuid:01d4e40d-c4e6-40c4-bc1a-f2612469ed54"));
	json_object_put(saved);
	free(after);
	after = nullptr;
	chatgpt_test_replies(nullptr, 0);
	CHECK(chatgpt_header(path, &header, error, sizeof error) == EACCES);
	CHECK(!dstr_len(&header));
	json_object_put(profile);
}

static void
chatgpt_test_registration (char const *path)
{
	struct json_object *profile = chatgpt_test_profile();
	json_object_object_del(profile, "subject");
	json_object_object_del(profile, "ext_agent_host_id");
	CHECK(chatgpt_test_save(path, profile) == 0);
	json_object_put(profile);
	int e;
	struct chatgpt_file file = chatgpt_file(path, false, &e);
	CHECK(e == 0);
	e = e ? e : chatgpt_host(&file);
	CHECK(e == 0);
	if (e) {
		chatgpt_file_fini(&file);
		return;
	}
	char *host = strdup(chatgpt_text(file.record, "ext_agent_host_id").data);
	CHECK(host && !strncmp(host, "urn:uuid:", sizeof "urn:uuid:" - 1));
	CHECK(host && strlen(host) == 45);
	CHECK(chatgpt_host(&file) == 0);
	CHECK(host && !strcmp(host, chatgpt_text(file.record, "ext_agent_host_id").data));
	free(host);
	host = nullptr;
	char *before = chatgpt_test_read(path);
	EVP_PKEY *key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", (size_t)2048);
	CHECK(key != nullptr);
	char *jwks = chatgpt_test_jwks(key);
	struct json_object *keys = json_tokener_parse(jwks);
	struct json_object *claims = jb_object(
		"iss", json_object_new_string(CHATGPT_ISSUER),
		"aud", json_object_new_string("oaiapp_offline-test"),
		"sub", json_object_new_string("registered-user"),
		"email", json_object_new_string("offline@example.invalid"),
		"nonce", json_object_new_string("pending-nonce"),
		"iat", json_object_new_int64(time(nullptr)),
		"exp", json_object_new_int64(time(nullptr) + 3600), nullptr);
	char *token = chatgpt_test_jwt(key, jb_wire(claims).data);
	struct json_object *tokens = jb_object(
		"access_token", json_object_new_string("login.access"),
		"refresh_token", json_object_new_string("login.refresh"),
		"id_token", json_object_new_string(token),
		"token_type", json_object_new_string("Bearer"),
		"expires_in", json_object_new_int64(3600), nullptr);
	chatgpt_test_replies(nullptr, 0);
	/* Authentication alone is not consent to spend the ChatGPT plan. */
	CHECK(chatgpt_tokens(&file, tokens, keys, CSTR_VIEW("pending-nonce"), true) == EPROTO);
	CHECK(jb_set(tokens, "scope", json_object_new_string(CHATGPT_SCOPE)) == 0);
	CHECK(chatgpt_tokens(&file, tokens, keys, CSTR_VIEW("another-nonce"), true) == EACCES);
	char *after = chatgpt_test_read(path);
	CHECK(before && after && !strcmp(before, after));
	free(after);
	after = nullptr;
	/* Retain the verified account after declined plan consent, disabled. */
	CHECK(jb_set(tokens, "scope", json_object_new_string("openid email")) == 0);
	CHECK(chatgpt_tokens(&file, tokens, keys, CSTR_VIEW("pending-nonce"), true) == EACCES);
	CHECK(jb_is(jb_get(file.record, "subject"), "registered-user"));
	CHECK(!chatgpt_scope(file.record));
	CHECK(jb_set(tokens, "scope", json_object_new_string(CHATGPT_SCOPE)) == 0);
	CHECK(chatgpt_tokens(&file, tokens, keys, CSTR_VIEW("pending-nonce"), true) == 0);
	CHECK(jb_is(jb_get(file.record, "subject"), "registered-user"));
	CHECK(jb_is(jb_get(file.record, "issuer"), CHATGPT_ISSUER));
	CHECK(jb_is(jb_get(file.record, "email"), "offline@example.invalid"));
	CHECK(jb_is(jb_get(file.record, "refresh_token"), "login.refresh"));
	CHECK(chatgpt_scope(file.record));
	/* An existing registration cannot be replaced with another subject. */
	CHECK(jb_set(claims, "sub", json_object_new_string("other-user")) == 0);
	free(token);
	token = chatgpt_test_jwt(key, jb_wire(claims).data);
	CHECK(jb_set(tokens, "id_token", json_object_new_string(token)) == 0);
	CHECK(chatgpt_tokens(&file, tokens, keys, CSTR_VIEW("pending-nonce"), true) == EACCES);
	CHECK(jb_is(jb_get(file.record, "subject"), "registered-user"));
	/* An explicit empty scope revokes the previous direct-use permission. */
	json_object_object_del(tokens, "id_token");
	CHECK(jb_set(tokens, "scope", json_object_new_string("")) == 0);
	CHECK(chatgpt_tokens(&file, tokens, nullptr, cstr_view(nullptr), false) == EACCES);
	CHECK(!chatgpt_scope(file.record));
	chatgpt_file_fini(&file);
	json_object_put(claims);
	json_object_put(tokens);
	json_object_put(keys);
	EVP_PKEY_free(key);
	free(token);
	token = nullptr;
	free(jwks);
	jwks = nullptr;
	free(before);
	before = nullptr;
}

int
main (void)
{
	char temporary[] = "/tmp/askai-chatgpt-XXXXXX";
	char *directory = mkdtemp(temporary);
	CHECK(directory != nullptr);
	if (!directory)
		return check_report("chatgpt");
	char *path = nullptr;
	CHECK(asprintf(&path, "%s/profile.json", directory) >= 0);
	chatgpt_test_identity();
	chatgpt_test_callback();
	chatgpt_test_cached(path);
	/* A private credential is insufficient inside a shared parent directory. */
	struct json_object *profile = chatgpt_test_profile();
	CHECK(chatgpt_test_save(path, profile) == 0);
	json_object_put(profile);
	CHECK(chmod(directory, 0755) == 0);
	dstr_auto header = dstr(nullptr);
	char error[128];
	CHECK(chatgpt_header(path, &header, error, sizeof error) == EACCES);
	CHECK(chmod(directory, 0700) == 0);
	chatgpt_test_refresh(path);
	chatgpt_test_registration(path);
	dstr_cleanup(&chatgpt_test_http.last_form);
	free(path);
	path = nullptr;
	DIR *dir = opendir(directory);
	CHECK(dir != nullptr);
	if (dir) {
		struct dirent *entry;
		while ((entry = readdir(dir)))
			if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
				CHECK(unlinkat(dirfd(dir), entry->d_name, 0) == 0);
		CHECK(closedir(dir) == 0);
	}
	CHECK(rmdir(directory) == 0);
	return check_report("chatgpt");
}
