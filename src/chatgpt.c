/** @file chatgpt.c
 *
 * The public Sign in with ChatGPT flow, with one account per credential
 * file. OAuth calls are deliberately confined to OpenAI's HTTPS origin.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "chatgpt.h"
#include "jb.h"
#include "util.h"

#define CHATGPT_ISSUER "https://auth.openai.com"
#define CHATGPT_AUTHORIZE CHATGPT_ISSUER "/api/accounts/authorize"
#define CHATGPT_TOKEN CHATGPT_ISSUER "/api/accounts/oauth/token"
#define CHATGPT_JWKS CHATGPT_ISSUER "/.well-known/jwks.json"
#define CHATGPT_RESOURCE "https://api.openai.com/v1"
#define CHATGPT_SCOPE "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
#define CHATGPT_DIRECT "chatgpt.tokens.use.direct"

enum { CHATGPT_LIMIT = 1 << 20, CHATGPT_TOKEN_LIMIT = 65536, CHATGPT_WAIT = 300 };

struct chatgpt_file {
	struct json_object *record;
	char               *name;
	int                 directory;
	int                 lock;
};

static pthread_once_t chatgpt_curl_once = PTHREAD_ONCE_INIT;
static CURLcode chatgpt_curl_status;

static int64_t
chatgpt_milliseconds (void);

static void
chatgpt_curl_init (void)
{
	chatgpt_curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

static int
chatgpt_curl_ready (void)
{
	int e = pthread_once(&chatgpt_curl_once, chatgpt_curl_init);
	return e ? e : chatgpt_curl_status == CURLE_OK ? 0 : EIO;
}

static int
chatgpt_error (char       *error,
               size_t      size,
               int         code,
               char const *message)
{
	if (size)
		snprintf(error, size, "%s", message);
	return code;
}

static int
chatgpt_printf (char       **out,
                char const  *format,
                ...)
{
	va_list args;
	va_start(args, format);
	int n = vasprintf(out, format, args);
	va_end(args);
	if (n >= 0)
		return 0;
	*out = nullptr;
	return ENOMEM;
}

static char const *
chatgpt_string (struct json_object *object,
                char const         *name)
{
	return jb_text(jb_get(object, name), 1, CHATGPT_TOKEN_LIMIT, nullptr).data;
}

int
chatgpt_path (char **path)
{
	*path = nullptr;
	char const *base = getenv("XDG_CONFIG_HOME");
	if (base && *base == '/')
		return chatgpt_printf(path, "%s/askai/chatgpt.json", base);
	base = getenv("HOME");
	return base && *base == '/' ? chatgpt_printf(path, "%s/.config/askai/chatgpt.json", base)
	                            : ENOENT;
}

static void
chatgpt_file_fini (struct chatgpt_file *file)
{
	json_object_put(file->record);
	file->record = nullptr;
	free(file->name);
	file->name = nullptr;
	if (file->lock >= 0)
		close(file->lock);
	file->lock = -1;
	if (file->directory >= 0)
		close(file->directory);
	file->directory = -1;
}

static int
chatgpt_private (int  fd,
                 bool directory)
{
	struct stat st;
	if (fstat(fd, &st))
		return errno;
	if (st.st_uid != geteuid() ||
	    (directory ? !S_ISDIR(st.st_mode) || (st.st_mode & 0022)
	               : !S_ISREG(st.st_mode) || (st.st_mode & 0077) || st.st_nlink != 1))
		return EACCES;
	return 0;
}

/* Keep an opened parent directory throughout the transaction, so a path
 * rename cannot redirect the lock, temporary file or final rename.
 */
static int
chatgpt_file_init (struct chatgpt_file *file,
                   char const         *path,
                   bool                create)
{
	*file = (struct chatgpt_file){ .directory = -1, .lock = -1 };
	if (!path || !*path)
		return EINVAL;
	char *copy = strdup(path);
	if (!copy)
		return ENOMEM;
	int e = create ? util_mkdir_parents(copy) : 0;
	char *slash = strrchr(copy, '/');
	char const *name = slash ? slash + 1 : copy;
	if (!e && (!*name || !strcmp(name, ".") || !strcmp(name, "..")))
		e = EINVAL;
	if (!e && !(file->name = strdup(name)))
		e = ENOMEM;
	if (slash)
		*slash = '\0';
	char const *directory = !slash ? "." : slash == copy ? "/" : copy;
	if (!e && (file->directory = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) < 0)
		e = errno;
	free(copy);
	copy = nullptr;
	if (!e)
		e = chatgpt_private(file->directory, true);
	char *lock = nullptr;
	if (!e)
		e = chatgpt_printf(&lock, "%s.lock", file->name);
	if (!e && (file->lock = openat(file->directory, lock,
	                              O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)) < 0)
		e = errno;
	free(lock);
	lock = nullptr;
	if (!e)
		e = chatgpt_private(file->lock, false);
	if (!e) {
		int64_t started = chatgpt_milliseconds();
		if (started < 0)
			e = EIO;
		while (!e && flock(file->lock, LOCK_EX | LOCK_NB)) {
			if (errno != EINTR && errno != EWOULDBLOCK) {
				e = errno;
				break;
			}
			int64_t now = chatgpt_milliseconds();
			if (now < 0 || now - started >= 2000) {
				e = now < 0 ? EIO : EBUSY;
				break;
			}
			struct timespec pause = { .tv_nsec = 100000000 };
			if (nanosleep(&pause, nullptr) && errno != EINTR)
				e = errno;
		}
	}
	if (e)
		return e;
	int fd = openat(file->directory, file->name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		if (errno != ENOENT || !create)
			return errno;
		file->record = json_object_new_object();
		return file->record ? 0 : ENOMEM;
	}
	e = chatgpt_private(fd, false);
	char *text = e ? nullptr : malloc(CHATGPT_LIMIT + 1);
	if (!e && !text)
		e = ENOMEM;
	size_t length = 0;
	if (!e)
		e = util_read_all(fd, text, CHATGPT_LIMIT + 1, -1, &length);
	if (!e && length > CHATGPT_LIMIT)
		e = EFBIG;
	close(fd);
	fd = -1;
	if (!e) {
		file->record = jb_parse(text, length, JSON_TOKENER_STRICT, 16);
		if (!json_object_is_type(file->record, json_type_object))
			e = EINVAL;
	}
	if (text)
		OPENSSL_cleanse(text, length);
	free(text);
	text = nullptr;
	return e;
}

static int
chatgpt_file_save (struct chatgpt_file  *file,
                   struct json_object  *record)
{
	unsigned char random[16];
	int e = util_random(random, sizeof random);
	if (e)
		return e;
	char temporary[sizeof ".chatgpt-" + 2 * sizeof random];
	memcpy(temporary, ".chatgpt-", sizeof ".chatgpt-" - 1);
	util_hex(temporary + sizeof ".chatgpt-" - 1, random, sizeof random);
	temporary[sizeof temporary - 1] = '\0';
	struct cstr_view text = jb_wire(record);
	if (!text.data)
		return ENOMEM;
	int fd = openat(file->directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return errno;
	e = fchmod(fd, 0600) ? errno : util_write_all(fd, text.data, text.len);
	if (!e && fsync(fd))
		e = errno;
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	if (!e && renameat(file->directory, temporary, file->directory, file->name))
		e = errno;
	if (!e && fsync(file->directory))
		e = errno;
	if (e)
		unlinkat(file->directory, temporary, 0);
	return e;
}

struct chatgpt_body {
	char   *data;
	size_t  length;
	int     error;
};

static size_t
chatgpt_receive (char   *data,
                 size_t  size,
                 size_t  count,
                 void   *opaque)
{
	struct chatgpt_body *body = opaque;
	if (size && count > (CHATGPT_LIMIT - body->length) / size) {
		body->error = EFBIG;
		return 0;
	}
	size_t n = size * count;
	char *next = realloc(body->data, body->length + n + 1);
	if (!next) {
		body->error = ENOMEM;
		return 0;
	}
	body->data = next;
	memcpy(next + body->length, data, n);
	body->length += n;
	next[body->length] = '\0';
	return n;
}

/* Do not include upstream response bodies in diagnostics: a token server
 * is allowed to echo request parameters in error descriptions.
 */
static int
chatgpt_http (char const          *url,
              char const          *form,
              struct json_object **result,
              long                *status)
{
	*result = nullptr;
	*status = 0;
	int e = chatgpt_curl_ready();
	if (e)
		return e;
	CURL *curl = curl_easy_init();
	if (!curl)
		return ENOMEM;
	struct chatgpt_body body = {};
#define CHATGPT_SET(option, value) \
        if (!e && curl_easy_setopt(curl, option, value) != CURLE_OK) e = EIO
	CHATGPT_SET(CURLOPT_URL, url);
	CHATGPT_SET(CURLOPT_PROTOCOLS_STR, "https");
	CHATGPT_SET(CURLOPT_FOLLOWLOCATION, 0L);
	CHATGPT_SET(CURLOPT_NOSIGNAL, 1L);
	CHATGPT_SET(CURLOPT_CONNECTTIMEOUT, 15L);
	CHATGPT_SET(CURLOPT_TIMEOUT, 60L);
	CHATGPT_SET(CURLOPT_WRITEFUNCTION, chatgpt_receive);
	CHATGPT_SET(CURLOPT_WRITEDATA, &body);
	CHATGPT_SET(CURLOPT_USERAGENT, "askai/0.1");
	if (form)
		CHATGPT_SET(CURLOPT_POSTFIELDS, form);
#undef CHATGPT_SET
	if (!e) {
		CURLcode status_code = curl_easy_perform(curl);
		if (status_code != CURLE_OK)
			e = body.error ? body.error : status_code == CURLE_OPERATION_TIMEDOUT ? ETIMEDOUT : EIO;
	}
	if (!e && curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status) != CURLE_OK)
		e = EIO;
	curl_easy_cleanup(curl);
	curl = nullptr;
	if (!e) {
		*result = body.length ? jb_parse(body.data, body.length, JSON_TOKENER_STRICT, 24) : nullptr;
		if (!json_object_is_type(*result, json_type_object))
			e = EPROTO;
	}
	if (body.data)
		OPENSSL_cleanse(body.data, body.length);
	free(body.data);
	body.data = nullptr;
	return e;
}

/* Build form/query text without exposing credentials to subprocesses. */
static int
chatgpt_form (char **form,
              ...)
{
	*form = nullptr;
	int e = chatgpt_curl_ready();
	if (e)
		return e;
	CURL *curl = curl_easy_init();
	if (!curl)
		return ENOMEM;
	va_list args;
	va_start(args, form);
	size_t length = 0;
	char const *key;
	while (!e && (key = va_arg(args, char const *))) {
		char const *value = va_arg(args, char const *);
		if (!value) {
			e = EINVAL;
			break;
		}
		char *escaped = curl_easy_escape(curl, value, 0);
		if (!escaped) {
			e = ENOMEM;
			break;
		}
		size_t k = strlen(key), v = strlen(escaped);
		char *next = realloc(*form, length + k + v + 3);
		if (!next)
			e = ENOMEM;
		else {
			*form = next;
			if (length)
				next[length++] = '&';
			memcpy(next + length, key, k);
			length += k;
			next[length++] = '=';
			memcpy(next + length, escaped, v + 1);
			length += v;
		}
		curl_free(escaped);
		escaped = nullptr;
	}
	va_end(args);
	curl_easy_cleanup(curl);
	curl = nullptr;
	if (e) {
		free(*form);
		*form = nullptr;
	}
	return e;
}

static int
chatgpt_base64 (unsigned char const *input,
                size_t               size,
                char                *out)
{
	if (size > INT_MAX)
		return EFBIG;
	int n = EVP_EncodeBlock((unsigned char *)out, input, (int)size);
	if (n < 0)
		return EIO;
	while (n && out[n - 1] == '=')
		--n;
	out[n] = '\0';
	for (int i = 0; i < n; ++i)
		if (out[i] == '+') out[i] = '-';
		else if (out[i] == '/') out[i] = '_';
	return 0;
}

static int
chatgpt_decode (char const     *input,
                size_t          length,
                unsigned char **out,
                size_t         *size)
{
	*out = nullptr;
	*size = 0;
	if (!length || length > CHATGPT_TOKEN_LIMIT || length % 4 == 1)
		return EINVAL;
	size_t padded = (length + 3) & ~(size_t)3;
	char *text = malloc(padded + 1);
	unsigned char *bytes = malloc(padded / 4 * 3 + 1);
	if (!text || !bytes) {
		free(text);
		text = nullptr;
		free(bytes);
		bytes = nullptr;
		return ENOMEM;
	}
	int e = 0;
	for (size_t i = 0; i < length; ++i) {
		unsigned char c = (unsigned char)input[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			text[i] = (char)c;
		else if (c == '-') text[i] = '+';
		else if (c == '_') text[i] = '/';
		else e = EINVAL;
	}
	memset(text + length, '=', padded - length);
	text[padded] = '\0';
	int n = e ? -1 : EVP_DecodeBlock(bytes, (unsigned char *)text, (int)padded);
	free(text);
	text = nullptr;
	if (n < 0) {
		free(bytes);
		bytes = nullptr;
		return e ? e : EINVAL;
	}
	*size = (size_t)n - (padded - length);
	*out = bytes;
	return 0;
}

static int
chatgpt_part (char const          *text,
              size_t               length,
              struct json_object **object)
{
	unsigned char *bytes = nullptr;
	size_t size = 0;
	int e = chatgpt_decode(text, length, &bytes, &size);
	*object = e ? nullptr : jb_parse((char *)bytes, size, JSON_TOKENER_STRICT, 16);
	if (!e && !json_object_is_type(*object, json_type_object))
		e = EINVAL;
	free(bytes);
	bytes = nullptr;
	return e;
}

static EVP_PKEY *
chatgpt_rsa (struct json_object *jwk)
{
	char const *n = chatgpt_string(jwk, "n"), *e = chatgpt_string(jwk, "e");
	unsigned char *nb = nullptr, *eb = nullptr;
	size_t ns = 0, es = 0;
	int error = !n || !e ? EINVAL : chatgpt_decode(n, strlen(n), &nb, &ns);
	if (!error)
		error = chatgpt_decode(e, strlen(e), &eb, &es);
	BIGNUM *bn = error ? nullptr : BN_bin2bn(nb, (int)ns, nullptr);
	BIGNUM *be = error ? nullptr : BN_bin2bn(eb, (int)es, nullptr);
	free(nb);
	nb = nullptr;
	free(eb);
	eb = nullptr;
	OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
	OSSL_PARAM *params = nullptr;
	EVP_PKEY_CTX *ctx = nullptr;
	EVP_PKEY *key = nullptr;
	if (bn && be && BN_num_bits(bn) >= 2048 && builder &&
	    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, bn) &&
	    OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, be) &&
	    (params = OSSL_PARAM_BLD_to_param(builder)) &&
	    (ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)) &&
	    EVP_PKEY_fromdata_init(ctx) > 0)
		if (EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
			EVP_PKEY_free(key);
			key = nullptr;
		}
	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(builder);
	BN_free(bn);
	BN_free(be);
	return key;
}

static bool
chatgpt_audience (struct json_object *claims,
                  char const         *client)
{
	struct json_object *aud = jb_get(claims, "aud");
	bool match = false;
	if (json_object_is_type(aud, json_type_string))
		match = cstr_view_equal(jb_view(aud), cstr_view(client));
	else
		for (size_t i = 0, n = jb_len(aud); i < n; ++i)
			match |= cstr_view_equal(jb_view(jb_at(aud, i)), cstr_view(client));
	char const *azp = chatgpt_string(claims, "azp");
	return match && (!azp || !strcmp(azp, client)) && (jb_len(aud) <= 1 || azp);
}

/* Ignore all key URLs supplied by the JWT. Only the pinned issuer's JWKS
 * may verify its signature, and only its advertised RS256 algorithm.
 */
static int
chatgpt_identity_keys (char const          *token,
                       char const          *client,
                       char const          *nonce,
                       char const          *subject,
                       struct json_object  *known_keys,
                       struct json_object **identity)
{
	*identity = nullptr;
	if (!token || strlen(token) > CHATGPT_TOKEN_LIMIT)
		return EINVAL;
	char const *first = strchr(token, '.');
	char const *second = first ? strchr(first + 1, '.') : nullptr;
	if (!second || strchr(second + 1, '.'))
		return EINVAL;
	struct json_object *head = nullptr, *claims = nullptr, *jwks = nullptr;
	int e = chatgpt_part(token, (size_t)(first - token), &head);
	if (!e)
		e = chatgpt_part(first + 1, (size_t)(second - first - 1), &claims);
	char const *kid = chatgpt_string(head, "kid");
	time_t now = time(nullptr);
	struct json_object *expires = jb_get(claims, "exp"), *issued = jb_get(claims, "iat");
	char const *sub = chatgpt_string(claims, "sub"), *got_nonce = chatgpt_string(claims, "nonce");
	if (!e && (!jb_is(jb_get(head, "alg"), "RS256") || !kid || jb_get(head, "crit") ||
	           !jb_is(jb_get(claims, "iss"), CHATGPT_ISSUER) ||
	           !chatgpt_audience(claims, client) || !sub || now < 0 ||
	           !json_object_is_type(expires, json_type_int) || json_object_get_int64(expires) <= now ||
	           !json_object_is_type(issued, json_type_int) || json_object_get_int64(issued) > now + 60 ||
	           (nonce && (!got_nonce || strcmp(nonce, got_nonce))) ||
	           (subject && strcmp(subject, sub))))
		e = EACCES;
	struct json_object *not_before = jb_get(claims, "nbf");
	if (!e && not_before && (!json_object_is_type(not_before, json_type_int) ||
	                        json_object_get_int64(not_before) > now + 60))
		e = EACCES;
	long status = 0;
	if (!e && known_keys) {
		jwks = json_object_get(known_keys);
		status = 200;
	} else if (!e)
		e = chatgpt_http(CHATGPT_JWKS, nullptr, &jwks, &status);
	if (!e && status != 200)
		e = EIO;
	EVP_PKEY *key = nullptr;
	struct json_object *keys = jb_get(jwks, "keys");
	for (size_t i = 0, n = jb_len(keys); !e && !key && i < n; ++i) {
		struct json_object *jwk = jb_at(keys, i);
		char const *id = chatgpt_string(jwk, "kid");
		if (id && !strcmp(kid, id) && jb_is(jb_get(jwk, "kty"), "RSA") &&
		    (!jb_get(jwk, "alg") || jb_is(jb_get(jwk, "alg"), "RS256")) &&
		    (!jb_get(jwk, "use") || jb_is(jb_get(jwk, "use"), "sig")))
			key = chatgpt_rsa(jwk);
	}
	unsigned char *signature = nullptr;
	size_t signature_size = 0;
	if (!e)
		e = chatgpt_decode(second + 1, strlen(second + 1), &signature, &signature_size);
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!e && (!key || !ctx || EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) <= 0 ||
	           EVP_DigestVerify(ctx, signature, signature_size, (unsigned char const *)token,
	                            (size_t)(second - token)) != 1))
		e = EACCES;
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(key);
	free(signature);
	signature = nullptr;
	json_object_put(head);
	json_object_put(jwks);
	if (!e)
		*identity = claims;
	else
		json_object_put(claims);
	return e;
}

static int
chatgpt_identity (char const          *token,
                  char const          *client,
                  char const          *nonce,
                  char const          *subject,
                  struct json_object **identity)
{
	return chatgpt_identity_keys(token, client, nonce, subject, nullptr, identity);
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
chatgpt_scopes (struct json_object  *record,
                char const          *scope)
{
	struct json_object *list = json_object_new_array();
	if (!list)
		return ENOMEM;
	int e = 0;
	while (!e && *scope) {
		scope += strspn(scope, " ");
		size_t n = strcspn(scope, " ");
		if (n)
			e = jb_push(list, json_object_new_string_len(scope, (int)n));
		scope += n;
	}
	if (!e)
		return jb_set(record, "scopes", list);
	json_object_put(list);
	return e;
}

static bool
chatgpt_bearer (char const *token)
{
	return token && strspn(token, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/=") == strlen(token);
}

static int
chatgpt_tokens (struct chatgpt_file  *file,
                struct json_object  *tokens,
                char const          *nonce,
                bool                 login,
                struct json_object  *keys)
{
	char const *access = chatgpt_string(tokens, "access_token");
	char const *refresh = chatgpt_string(tokens, "refresh_token");
	char const *id = chatgpt_string(tokens, "id_token");
	char const *scope = chatgpt_string(tokens, "scope");
	char const *client = chatgpt_string(file->record, "client_id");
	char const *subject = chatgpt_string(file->record, "subject");
	struct json_object *lifetime = jb_get(tokens, "expires_in");
	int64_t seconds = json_object_is_type(lifetime, json_type_int) ? json_object_get_int64(lifetime) : 0;
	time_t now = time(nullptr);
	if (!client || !strcmp(client, "dynamic_agent_client") || !chatgpt_bearer(access) ||
	    !jb_is(jb_get(tokens, "token_type"), "Bearer") ||
	    !json_object_is_type(lifetime, json_type_int) || seconds <= 60 || seconds > 86400 * 30 ||
	    now < 0 || (login && (!refresh || !id || !scope)))
		return EPROTO;
	struct json_object *identity = nullptr;
	int e = !id ? 0 : keys ? chatgpt_identity_keys(id, client, nonce, subject, keys, &identity)
	                       : chatgpt_identity(id, client, nonce, subject, &identity);
	struct json_object *record = nullptr;
	if (!e && json_object_deep_copy(file->record, &record, nullptr))
		e = ENOMEM;
	if (!e)
		e = jb_set(record, "access_token", json_object_new_string(access));
	if (!e)
		e = jb_set(record, "expires_at", json_object_new_int64((int64_t)now + seconds));
	if (!e && refresh)
		e = jb_set(record, "refresh_token", json_object_new_string(refresh));
	if (!e && id)
		e = jb_set(record, "id_token", json_object_new_string(id));
	if (!e && identity)
		e = jb_set(record, "subject", json_object_get(jb_get(identity, "sub")));
	if (!e && identity)
		e = jb_set(record, "issuer", json_object_new_string(CHATGPT_ISSUER));
	if (!e && identity && jb_get(identity, "email"))
		e = jb_set(record, "email", json_object_get(jb_get(identity, "email")));
	if (!e && scope)
		e = chatgpt_scopes(record, scope);
	bool authorized = !e && chatgpt_scope(record);
	if (!e)
		e = chatgpt_file_save(file, record);
	if (!e) {
		json_object_put(file->record);
		file->record = record;
		record = nullptr;
	}
	json_object_put(record);
	json_object_put(identity);
	return e ? e : authorized ? 0 : EACCES;
}

static int
chatgpt_expired (struct chatgpt_file *file,
                 struct json_object *tokens)
{
	static char const *const expired[] = {
		"invalid_grant", "invalid_refresh_token", "token_expired",
		"refresh_token_expired", "refresh_token_invalidated", "refresh_token_reused"
	};
	char const *code = chatgpt_string(tokens, "error");
	if (!code)
		return 0;
	bool found = false;
	for (size_t i = 0; i < array_size(expired); ++i)
		found |= !strcmp(code, expired[i]);
	if (!found)
		return 0;
	struct json_object *record = nullptr;
	if (json_object_deep_copy(file->record, &record, nullptr))
		return ENOMEM;
	json_object_object_del(record, "access_token");
	json_object_object_del(record, "refresh_token");
	json_object_object_del(record, "id_token");
	json_object_object_del(record, "expires_at");
	int e = chatgpt_file_save(file, record);
	if (!e) {
		json_object_put(file->record);
		file->record = record;
		record = nullptr;
	}
	json_object_put(record);
	return e;
}

static int
chatgpt_exchange (struct chatgpt_file *file,
                  char const         *form,
                  char const         *nonce,
                  bool                login,
                  char               *error,
                  size_t              size)
{
	struct json_object *tokens = nullptr, *keys = nullptr;
	long status = 0;
	/* Obtain verification material before exchanging a single-use code or
	 * rotating a refresh token. A JWKS outage must not consume either. */
	int e = chatgpt_http(CHATGPT_JWKS, nullptr, &keys, &status);
	if (!e && (status != 200 || !jb_len(jb_get(keys, "keys"))))
		e = EIO;
	if (!e)
		e = chatgpt_http(CHATGPT_TOKEN, form, &tokens, &status);
	if (!e && status != 200) {
		if (!login && (status == 400 || status == 401 || status == 403))
			e = chatgpt_expired(file, tokens);
		if (!e)
			e = status == 400 || status == 401 || status == 403 ? EACCES : EIO;
	}
	if (!e)
		e = chatgpt_tokens(file, tokens, nonce, login, keys);
	json_object_put(tokens);
	json_object_put(keys);
	if (e)
		chatgpt_error(error, size, e, e == EACCES ?
		              "ChatGPT authorization rejected; sign in again and allow ChatGPT plan usage" :
		              "ChatGPT token exchange failed; stored credentials were retained");
	return e;
}

int
chatgpt_header (char const *path,
                char      **header,
                char       *error,
                size_t      size)
{
	*header = nullptr;
	if (size)
		*error = '\0';
	struct chatgpt_file file;
	int e = chatgpt_file_init(&file, path, false);
	if (e)
		chatgpt_error(error, size, e, e == EBUSY ? "ChatGPT credentials are busy; retry after sign-in or refresh completes" :
		              "Cannot read private ChatGPT credentials; run --chatgpt-login first");
	char const *client = chatgpt_string(file.record, "client_id");
	char const *refresh = chatgpt_string(file.record, "refresh_token");
	char const *access = chatgpt_string(file.record, "access_token");
	struct json_object *expires = jb_get(file.record, "expires_at");
	if (!e && (!client || !strcmp(client, "dynamic_agent_client") || !chatgpt_string(file.record, "subject") ||
	           !chatgpt_scope(file.record) ||
	           !chatgpt_bearer(access) || !json_object_is_type(expires, json_type_int)))
		e = chatgpt_error(error, size, EACCES, "Incomplete ChatGPT credentials; run --chatgpt-login first");
	time_t now = time(nullptr);
	if (!e && now < 0)
		e = chatgpt_error(error, size, EIO, "Cannot read the clock for ChatGPT token expiry");
	if (!e && json_object_get_int64(expires) <= now + 60) {
		char *form = nullptr;
		e = refresh ? chatgpt_form(&form, "grant_type", "refresh_token", "client_id", client,
		                            "refresh_token", refresh, "resource", CHATGPT_RESOURCE, nullptr) : EACCES;
		if (!e)
			e = chatgpt_exchange(&file, form, nullptr, false, error, size);
		if (form)
			OPENSSL_cleanse(form, strlen(form));
		free(form);
		form = nullptr;
		if (e && size && !*error)
			chatgpt_error(error, size, e, "Cannot refresh ChatGPT credentials; run --chatgpt-login again");
	}
	if (!e)
		e = chatgpt_printf(header, "Authorization: Bearer %s", chatgpt_string(file.record, "access_token"));
	chatgpt_file_fini(&file);
	return e;
}

static int
chatgpt_host (struct chatgpt_file *file)
{
	if (chatgpt_string(file->record, "ext_agent_host_id"))
		return 0;
	unsigned char bytes[16];
	int e = util_random(bytes, sizeof bytes);
	if (e)
		return e;
	bytes[6] = (unsigned char)((bytes[6] & 15) | 0x40);
	bytes[8] = (unsigned char)((bytes[8] & 63) | 0x80);
	char hex[33];
	util_hex(hex, bytes, sizeof bytes);
	hex[32] = '\0';
	char host[46];
	int n = snprintf(host, sizeof host, "urn:uuid:%.8s-%.4s-%.4s-%.4s-%.12s",
	                 hex, hex + 8, hex + 12, hex + 16, hex + 20);
	if (n < 0 || n >= (int)sizeof host)
		return EIO;
	e = jb_set(file->record, "ext_agent_host_id", json_object_new_string(host));
	return e ? e : chatgpt_file_save(file, file->record);
}

static int
chatgpt_listener (int      *listener,
                   uint16_t *port)
{
	*listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (*listener < 0)
		return errno;
	struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	if (bind(*listener, (struct sockaddr *)&address, sizeof address) || listen(*listener, 8))
		return errno;
	socklen_t length = sizeof address;
	if (getsockname(*listener, (struct sockaddr *)&address, &length))
		return errno;
	*port = ntohs(address.sin_port);
	return 0;
}

static int
chatgpt_query_value (CURL        *curl,
                     char const  *text,
                     size_t       size,
                     char       **value,
                     int         *length)
{
	*value = nullptr;
	char *copy = strndup(text, size);
	if (!copy)
		return ENOMEM;
	for (size_t i = 0; i < size; ++i)
		if (copy[i] == '+')
			copy[i] = ' ';
	*value = curl_easy_unescape(curl, copy, (int)size, length);
	free(copy);
	copy = nullptr;
	return *value ? 0 : ENOMEM;
}

static int
chatgpt_query (char const          *query,
               struct json_object **params)
{
	*params = json_object_new_object();
	if (!*params)
		return ENOMEM;
	int e = chatgpt_curl_ready();
	if (e)
		return e;
	CURL *curl = curl_easy_init();
	if (!curl)
		return ENOMEM;
	while (*query && !e) {
		size_t length = strcspn(query, "&");
		char const *equal = memchr(query, '=', length);
		if (!equal) {
			e = EINVAL;
			break;
		}
		int kn = 0, vn = 0;
		char *key = nullptr, *value = nullptr;
		e = chatgpt_query_value(curl, query, (size_t)(equal - query), &key, &kn);
		if (!e)
			e = chatgpt_query_value(curl, equal + 1, length - (size_t)(equal - query) - 1, &value, &vn);
		if (!e && (!kn || memchr(key, '\0', (size_t)kn) || memchr(value, '\0', (size_t)vn) || jb_get(*params, key)))
			e = EINVAL;
		else if (!e)
			e = jb_set(*params, key, json_object_new_string_len(value, vn));
		curl_free(key);
		key = nullptr;
		curl_free(value);
		value = nullptr;
		query += length;
		if (*query)
			++query;
	}
	curl_easy_cleanup(curl);
	curl = nullptr;
	return e;
}

static int64_t
chatgpt_milliseconds (void)
{
	struct timespec now;
	return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 :
	       (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int
chatgpt_poll (int      fd,
              int64_t  deadline)
{
	for (;;) {
		int64_t now = chatgpt_milliseconds();
		if (now < 0)
			return EIO;
		if (now >= deadline)
			return ETIMEDOUT;
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int n = poll(&p, 1, (int)(deadline - now));
		if (n > 0)
			return p.revents & POLLIN ? 0 : EIO;
		if (!n)
			return ETIMEDOUT;
		if (errno != EINTR)
			return errno;
	}
}

static int
chatgpt_callback (int                  listener,
                  char const          *state,
                  char const          *client,
                  struct json_object **params)
{
	*params = nullptr;
	int64_t start = chatgpt_milliseconds();
	if (start < 0)
		return EIO;
	int64_t deadline = start + CHATGPT_WAIT * 1000;
	for (;;) {
		int e = chatgpt_poll(listener, deadline);
		if (e)
			return e;
		int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (fd < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return errno;
		}
		char request[16384];
		size_t used = 0;
		int64_t request_deadline = chatgpt_milliseconds() + 5000;
		if (request_deadline > deadline)
			request_deadline = deadline;
		while (!e && used < sizeof request - 1) {
			e = chatgpt_poll(fd, request_deadline);
			if (e)
				break;
			ssize_t n = recv(fd, request + used, sizeof request - 1 - used, 0);
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			if (n <= 0) {
				e = EIO;
				break;
			}
			used += (size_t)n;
			request[used] = '\0';
			if (strstr(request, "\r\n\r\n"))
				break;
		}
		if (!e && (memchr(request, '\0', used) || !strstr(request, "\r\n\r\n") ||
		           strncmp(request, "GET /auth/callback?", sizeof "GET /auth/callback?" - 1)))
			e = EINVAL;
		char *end = e ? nullptr : strchr(request + sizeof "GET /auth/callback?" - 1, ' ');
		if (!e && !end)
			e = EINVAL;
		if (!e) {
			*end = '\0';
			e = chatgpt_query(request + sizeof "GET /auth/callback?" - 1, params);
		}
		char const *got_state = chatgpt_string(*params, "state");
		if (!e && (!got_state || strlen(got_state) != strlen(state) ||
		           CRYPTO_memcmp(got_state, state, strlen(state))))
			e = EACCES;
		bool matched = !e;
		char const *issued = chatgpt_string(*params, "client_id");
		char const *code = chatgpt_string(*params, "code");
		if (!e && (chatgpt_string(*params, "error") || !code ||
		           (!client && (!issued || !strcmp(issued, "dynamic_agent_client"))) ||
		           (client && issued && strcmp(client, issued)) ||
		           (jb_get(*params, "iss") && !jb_is(jb_get(*params, "iss"), CHATGPT_ISSUER))))
			e = EACCES;
		char const *reply = !e ? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nAuthorization received. Return to askai to finish sign-in.\n" :
		                         "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nAuthorization callback rejected.\n";
		ssize_t ignored = send(fd, reply, strlen(reply), MSG_NOSIGNAL);
		(void)ignored;
		close(fd);
		fd = -1;
		if (matched)
			return e;
		json_object_put(*params);
		*params = nullptr;
	}
}

int
chatgpt_login (char const *path,
               char       *error,
               size_t      size)
{
	if (size)
		*error = '\0';
	struct chatgpt_file file;
	int e = chatgpt_file_init(&file, path, true);
	if (!e)
		e = chatgpt_host(&file);
	if (e)
		chatgpt_error(error, size, e, "Cannot create private ChatGPT credential storage");
	unsigned char random[96], digest[32];
	char verifier[45], state[45], nonce[45], challenge[45];
	if (!e)
		e = util_random(random, sizeof random);
	if (!e)
		e = chatgpt_base64(random, 32, verifier);
	if (!e)
		e = chatgpt_base64(random + 32, 32, state);
	if (!e)
		e = chatgpt_base64(random + 64, 32, nonce);
	unsigned digest_size = 0;
	if (!e && (!EVP_Digest(verifier, strlen(verifier), digest, &digest_size, EVP_sha256(), nullptr) ||
	           digest_size != sizeof digest))
		e = EIO;
	if (!e)
		e = chatgpt_base64(digest, sizeof digest, challenge);
	int listener = -1;
	uint16_t port = 0;
	if (!e)
		e = chatgpt_listener(&listener, &port);
	char redirect[64];
	int n = snprintf(redirect, sizeof redirect, "http://127.0.0.1:%u/auth/callback", (unsigned)port);
	if (!e && (n < 0 || n >= (int)sizeof redirect))
		e = EIO;
	char const *client = chatgpt_string(file.record, "client_id");
	char *query = nullptr;
	if (!e) {
		e = chatgpt_form(&query, "client_id", client ? client : "dynamic_agent_client",
		                  "ext_agent_host_id", chatgpt_string(file.record, "ext_agent_host_id"),
		                  "response_type", "code", "redirect_uri", redirect,
		                  "scope", CHATGPT_SCOPE, "resource", CHATGPT_RESOURCE,
		                  "state", state, "nonce", nonce, "code_challenge_method", "S256",
		                  "code_challenge", challenge, nullptr);
		if (!e && (!client || !chatgpt_scope(file.record))) {
			char *registration = nullptr;
			e = chatgpt_printf(&registration, "%s&%s", query,
			                    client ? "prompt=consent" : "agent_name_hint=askai");
			free(query);
			query = registration;
		}
	}
	if (!e && (fprintf(stderr, "Continue with ChatGPT: open this URL in your browser:\n%s?%s\n", CHATGPT_AUTHORIZE, query) < 0 || fflush(stderr)))
		e = EIO;
	free(query);
	query = nullptr;
	struct json_object *params = nullptr;
	if (!e)
		e = chatgpt_callback(listener, state, client, &params);
	if (listener >= 0)
		close(listener);
	listener = -1;
	if (!e && !client) {
		e = jb_set(file.record, "client_id", json_object_get(jb_get(params, "client_id")));
		if (!e)
			e = chatgpt_file_save(&file, file.record);
	}
	char *form = nullptr;
	if (!e)
		e = chatgpt_form(&form, "grant_type", "authorization_code", "client_id", chatgpt_string(file.record, "client_id"),
		                  "code", chatgpt_string(params, "code"), "code_verifier", verifier,
		                  "redirect_uri", redirect, "resource", CHATGPT_RESOURCE, nullptr);
	if (!e)
		e = chatgpt_exchange(&file, form, nonce, true, error, size);
	if (form)
		OPENSSL_cleanse(form, strlen(form));
	free(form);
	form = nullptr;
	OPENSSL_cleanse(verifier, sizeof verifier);
	OPENSSL_cleanse(random, sizeof random);
	json_object_put(params);
	chatgpt_file_fini(&file);
	if (e && size && !*error)
		chatgpt_error(error, size, e, e == ETIMEDOUT ? "ChatGPT sign-in timed out" : "ChatGPT sign-in failed");
	return e;
}
