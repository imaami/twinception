/** @file Terse, leak-safe helpers for building and reading json-c values. */
#ifndef TWINCEPTION_SRC_JB_H_
#define TWINCEPTION_SRC_JB_H_

#include <stddef.h>

#include <json-c/json.h>

/** Serialization flags for everything sent over the wire. */
#define JB_WIRE (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

/**
 * Object from key, value pairs ending with a nullptr key.  The values are
 * consumed; a nullptr value (a failed allocation) fails the whole object.
 */
extern struct json_object *
jb_object (char const *key,
           ...);

/** One-element array; consumes @p val. */
extern struct json_object *
jb_list (struct json_object *val);

/** Set @p key to @p val, which is consumed even on failure or when @p obj
 *  is nullptr. */
extern bool
jb_set (struct json_object *obj,
        char const         *key,
        struct json_object *val);

/** Append @p val, which is consumed even on failure or when @p arr is
 *  nullptr. */
extern bool
jb_push (struct json_object *arr,
         struct json_object *val);

/** Member @p key, or nullptr if absent, null, or @p obj is no object. */
extern struct json_object *
jb_get (struct json_object const *obj,
        char const               *key);

/** Element @p idx, or nullptr if out of range or @p arr is no array. */
extern struct json_object *
jb_at (struct json_object const *arr,
       size_t                    idx);

/** String value, or nullptr if @p val is no string. */
extern char const *
jb_text (struct json_object *val);

/** Whether @p val is the string @p str. */
extern bool
jb_is (struct json_object *val,
       char const         *str);

/** Parse exactly one JSON value spanning @p len bytes; nullptr on error. */
extern struct json_object *
jb_parse (char const *data,
          size_t      len,
          int         flags);

#endif /* TWINCEPTION_SRC_JB_H_ */
