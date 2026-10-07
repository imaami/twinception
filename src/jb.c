/** @file Terse, leak-safe helpers for building and reading json-c values. */
#include <limits.h>
#include <stdarg.h>
#include <string.h>

#include "jb.h"

struct json_object *
jb_object (char const *key,
           ...)
{
	struct json_object *obj = json_object_new_object();
	va_list ap;
	va_start(ap, key);
	for (; key; key = va_arg(ap, char const *)) {
		struct json_object *val = va_arg(ap, struct json_object *);
		if (!obj) {
			json_object_put(val);
		} else if (!jb_set(obj, key, val)) {
			json_object_put(obj);
			obj = nullptr;
		}
	}
	va_end(ap);
	return obj;
}

struct json_object *
jb_list (struct json_object *val)
{
	struct json_object *arr = json_object_new_array();
	if (!arr) {
		json_object_put(val);
		return nullptr;
	}
	if (!jb_push(arr, val)) {
		json_object_put(arr);
		return nullptr;
	}
	return arr;
}

bool
jb_set (struct json_object *obj,
        char const         *key,
        struct json_object *val)
{
	if (obj && val && !json_object_object_add(obj, key, val))
		return true;
	json_object_put(val);
	return false;
}

bool
jb_push (struct json_object *arr,
         struct json_object *val)
{
	if (arr && val && !json_object_array_add(arr, val))
		return true;
	json_object_put(val);
	return false;
}

struct json_object *
jb_get (struct json_object const *obj,
        char const               *key)
{
	struct json_object *val;
	return json_object_object_get_ex(obj, key, &val) ? val : nullptr;
}

struct json_object *
jb_at (struct json_object const *arr,
       size_t                    idx)
{
	return json_object_is_type(arr, json_type_array) &&
	       idx < json_object_array_length(arr)
		? json_object_array_get_idx(arr, idx) : nullptr;
}

char const *
jb_text (struct json_object *val)
{
	return json_object_is_type(val, json_type_string)
		? json_object_get_string(val) : nullptr;
}

bool
jb_is (struct json_object *val,
       char const         *str)
{
	char const *text = jb_text(val);
	return text && !strcmp(text, str);
}

struct json_object *
jb_parse (char const *data,
          size_t      len,
          int         flags)
{
	if (len > INT_MAX)
		return nullptr;

	struct json_tokener *tok = json_tokener_new();
	if (!tok)
		return nullptr;
	json_tokener_set_flags(tok, flags);
	struct json_object *root = json_tokener_parse_ex(tok, data, (int)len);
	size_t end = json_tokener_get_parse_end(tok);
	/* A number or literal at the very end only ends at a terminator. */
	if (!root && json_tokener_get_error(tok) == json_tokener_continue)
		root = json_tokener_parse_ex(tok, "", 1);
	bool ok = json_tokener_get_error(tok) == json_tokener_success;
	json_tokener_free(tok);

	while (ok && end < len && (data[end] == ' ' || data[end] == '\t' ||
	                           data[end] == '\r' || data[end] == '\n'))
		++end;
	if (!ok || end != len) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}
