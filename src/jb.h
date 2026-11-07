/** @file jb.h
 *
 * Terse, leak-safe helpers for building and reading json-c values. The
 * static inline ones call only json-c and other static inline functions,
 * so plugins can use them.
 */
#ifndef ASKAI_SRC_JB_H_
#define ASKAI_SRC_JB_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <json-c/json.h>

#include "cstr_view.h"
#include "export.h"
#include "util.h"

/** Serialization flags for everything sent over the wire. */
#define JB_WIRE (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

/** The most bytes of a key that jb_args() reports. */
enum { JB_KEY_SHOWN = 64 };

/** The most bytes of a JSON number or literal that askai parses, as the
 *  scalar of a pool or of jb_scan(). json-c converts one whole, and no
 *  valid one needs more.
 */
enum { JB_SCALAR_MAX = 1 << 10 };

/**
 * Object from key, value pairs ending with a nullptr key. The values are
 * consumed; a nullptr value (a failed allocation) fails the whole object.
 */
extern struct json_object *
jb_object (char const *key,
           ...);

/** One-element array; consumes @p val. */
extern struct json_object *
jb_list (struct json_object *val);

/** Set @p key to @p val, which is consumed even on failure or when @p obj
 *  is nullptr; returns 0 or ENOMEM.
 */
static inline int
jb_set (struct json_object *obj,
        char const         *key,
        struct json_object *val)
{
	if (obj && val && !json_object_object_add(obj, key, val))
		return 0;
	json_object_put(val);
	return ENOMEM;
}

/** Append @p val, which is consumed even on failure or when @p arr is
 *  nullptr; returns 0 or ENOMEM.
 */
static inline int
jb_push (struct json_object *arr,
         struct json_object *val)
{
	if (arr && val && !json_object_array_add(arr, val))
		return 0;
	json_object_put(val);
	return ENOMEM;
}

/**
 * Set the member @p key of @p obj to @p val, and return @p obj. The call
 * consumes @p val. If the set fails, the call frees @p obj and returns
 * nullptr. A nullptr @p obj fails the set, so a chain of calls needs only
 * one check at its end.
 */
static inline struct json_object *
jb_set_or_free (struct json_object *obj,
                char const         *key,
                struct json_object *val)
{
	if (!jb_set(obj, key, val))
		return obj;
	json_object_put(obj);
	return nullptr;
}

/**
 * Append @p val to the array @p arr, and return @p arr. The call consumes
 * @p val. If the push fails, the call frees @p arr and returns nullptr.
 */
static inline struct json_object *
jb_push_or_free (struct json_object *arr,
                 struct json_object *val)
{
	if (!jb_push(arr, val))
		return arr;
	json_object_put(arr);
	return nullptr;
}

/** Member @p key, or nullptr if absent, null, or @p obj is no object. */
static inline struct json_object *
jb_get (struct json_object const *obj,
        char const               *key)
{
	struct json_object *val;
	return json_object_object_get_ex(obj, key, &val) ? val : nullptr;
}

/** Returns the length of the array @p val, or 0 if @p val is not an array. */
static inline size_t
jb_len (struct json_object const *val)
{
	return json_object_is_type(val, json_type_array) ? json_object_array_length(val)
	                                                 : 0;
}

/** Returns the number of members of the object @p val, or 0 if @p val is
 *  not an object.
 */
static inline size_t
jb_members (struct json_object const *val)
{
	return json_object_is_type(val, json_type_object) ? (size_t)json_object_object_length(val)
	                                                  : 0;
}

/** Element @p idx, or nullptr if out of range or @p arr is no array. */
static inline struct json_object *
jb_at (struct json_object const *arr,
       size_t                    idx)
{
	return idx < jb_len(arr) ? json_object_array_get_idx(arr, idx) : nullptr;
}

/** Returns a view of the string @p val, or an empty view with data nullptr
 *  if @p val is not a string.
 */
static inline struct cstr_view
jb_view (struct json_object *val)
{
	if (!json_object_is_type(val, json_type_string))
		return (struct cstr_view){};
	return (struct cstr_view){ json_object_get_string(val),
	                           (size_t)json_object_get_string_len(val) };
}

/** Returns jb_view(@p val) if the string is @p min to @p max bytes long,
 *  contains no NUL and, if @p set is not nullptr, contains only bytes of
 *  @p set. Otherwise returns an empty view with data nullptr. json-c
 *  writes a NUL after each string, so strlen() and strspn() stop at the
 *  end of the string at the latest.
 */
static inline struct cstr_view
jb_text (struct json_object *val,
         size_t              min,
         size_t              max,
         char const         *set)
{
	struct cstr_view const t = jb_view(val);
	return t.data && t.len >= min && t.len <= max &&
	       (set ? strspn(t.data, set) : strlen(t.data)) == t.len ? t : (struct cstr_view){};
}

/** Returns the JSON text of @p val, as JB_WIRE writes it, until @p val
 *  changes or is freed; an empty view with data nullptr if @p val is
 *  nullptr or memory runs out.
 */
static inline struct cstr_view
jb_wire (struct json_object *val)
{
	struct cstr_view text = {};
	if (val)
		text.data = json_object_to_json_string_length(val, JB_WIRE, &text.len);
	return text;
}

/** Returns a new JSON string with the bytes of @p t, or nullptr if out of
 *  memory.
 */
static inline struct json_object *
jb_string (struct cstr_view t)
{
	return json_object_new_string_len(t.data, (int)t.len);
}

/** Evaluates to true if @p val is a string equal to the string literal
 *  @p lit. The comparison includes every byte, so a string with a NUL
 *  inside does not equal the part before the NUL.
 */
#define jb_is(val, lit) cstr_view_equal(jb_view(val), CSTR_VIEW(lit))

/**
 * Parse exactly one JSON value spanning @p len bytes; nullptr on error. At
 * most @p depth containers may be open at once; 0 leaves json-c's default.
 */
extern struct json_object *
jb_parse (char const *data,
          size_t      len,
          int         flags,
          uint32_t    depth);

/** What jb_scan() has seen of a JSON text so far; zeroed to begin. */
struct jb_scan {
	uint32_t *members;  /**< members of each container open, when counted */
	size_t    copied;   /**< bytes of the strings ended, keys included: json-c
	                         copies each whole as it ends */
	size_t    run;      /**< bytes of the string or scalar under way */
	size_t    cap;      /**< the number of counts that fit in members */
	size_t    depth;    /**< containers open */
	uint32_t  values;   /**< values begun, object keys included */
	enum {
		JB_SCAN_GAP,
		JB_SCAN_STRING,
		JB_SCAN_ESCAPE,
		JB_SCAN_SCALAR
	}         state;
};

/**
 * Scan up to @p len more bytes of a JSON text, which may arrive in any pieces,
 * for what parsing it would cost. Stops before a byte that would open more
 * than @p depth containers at once, begin more than @p values values, give an
 * object more than @p members members, or take a number or literal past
 * @p scalar bytes (UINT32_MAX: no limit), and returns how many bytes it
 * scanned: @p len unless it stopped. It does not validate.
 */
extern size_t
jb_scan (struct jb_scan *scan,
         char const     *data,
         size_t          len,
         uint32_t        depth,
         uint32_t        values,
         uint32_t        members,
         uint32_t        scalar);

/** Free what a scan holds. */
extern void
jb_scan_fini (struct jb_scan *scan);

/** Begin a scan anew, keeping the storage @p scan holds. */
static inline void
jb_scan_reset (struct jb_scan *scan)
{
	*scan = (struct jb_scan){ .members = scan->members, .cap = scan->cap };
}

/** json-c values to free a few at a time; zeroed to begin. */
struct jb_reaper {
	struct json_object **stack;  /**< containers being taken apart */
	size_t               n;
	size_t               cap;
};

/**
 * Passes @p val to @p reaper, which frees it a few values at a time in
 * jb_reaper_step(). No other reference to a container within @p val may
 * exist. The function frees @p val at once if @p val is not a container,
 * or if memory runs out.
 */
extern void
jb_reaper_add (struct jb_reaper   *reaper,
               struct json_object *val);

/** Free up to @p values values; returns how many it freed. */
extern size_t
jb_reaper_step (struct jb_reaper *reaper,
                size_t            values);

/** Free what is left, all at once. */
extern void
jb_reaper_fini (struct jb_reaper *reaper);

/**
 * Tokeners to reuse, all with the same limits; zeroed but for those. A
 * tokener keeps the buffer of the longest string it has parsed, which
 * jb_pool_release() frees.
 */
struct jb_pool {
	struct json_tokener *free[4];
	size_t               bytes;   /**< of the tokeners' string buffers */
	uint32_t             n;
	uint32_t             depth;   /**< containers open at once, at most;
	                                   0: json-c's default */
	uint32_t             scalar;  /**< bytes of a number or literal, at most,
	                                   as json-c converts one whole; 0: any */
	int                  flags;   /**< json_tokener_set_flags() */
};

/** Free a tokener whose string buffer is over @p keep bytes; returns that
 *  buffer's size, 0 if there is none.
 */
extern size_t
jb_pool_release (struct jb_pool *pool,
                 size_t          keep);

/** @p val as an integer if it is a JSON integer, else @p otherwise. json-c
 *  parses NaN too, which json_object_get_int64() and the like cannot convert
 *  to an integer without undefined behavior.
 */
static inline int64_t
jb_int (struct json_object const *val,
        int64_t                   otherwise)
{
	return json_object_is_type(val, json_type_int) ? json_object_get_int64(val)
	                                               : otherwise;
}

/** As jb_int(), for a value up to UINT64_MAX; a negative integer gives 0. */
static inline uint64_t
jb_uint (struct json_object const *val,
         uint64_t                  otherwise)
{
	return json_object_is_type(val, json_type_int) ? json_object_get_uint64(val)
	                                               : otherwise;
}

/** Stores the JSON integer @p val in @p *n and returns 0 if it is from 0 to
 *  @p max. Otherwise returns EINVAL and leaves @p *n unchanged.
 */
static inline int
jb_uint_checked (struct json_object const *val,
                 uint64_t                  max,
                 uint64_t                 *n)
{
	if (!json_object_is_type(val, json_type_int) || json_object_get_int64(val) < 0)
		return EINVAL;
	uint64_t const v = json_object_get_uint64(val);
	if (v > max)
		return EINVAL;
	*n = v;
	return 0;
}

/** Returns the JSON integer @p val if it is from 1 to @p max, else 0. */
static inline uint64_t
jb_count (struct json_object const *val,
          uint64_t                  max)
{
	uint64_t n = 0;
	return jb_uint_checked(val, max, &n) ? 0 : n;
}

/** Returns true if @p val is the JSON value true. */
static inline bool
jb_true (struct json_object const *val)
{
	return json_object_is_type(val, json_type_boolean) && json_object_get_boolean(val);
}

/**
 * Look up each key of the object @p args among the @p n names at @p names,
 * @p n at most 64. For a key that equals names[i], store its value in
 * @p of[i] and set bit i of @p *seen; the other bits are 0. Returns 0, or
 * EINVAL at the first key that is none of the names whose bits @p accepted
 * sets, with that key, cut to at most JB_KEY_SHOWN bytes at the start of a
 * character, in @p *other. An @p args that is no object counts as an
 * object without keys.
 */
static inline int
jb_args (struct json_object      *args,
         struct cstr_view const  *names,
         size_t                   n,
         uint64_t                 accepted,
         struct json_object     **of,
         uint64_t                *seen,
         struct cstr_view        *other)
{
	*seen = 0;
	if (!json_object_is_type(args, json_type_object))
		return 0;
	struct json_object_iter it;
	json_object_object_foreachC(args, it) {
		struct cstr_view const key = cstr_view(it.key);
		size_t const i = cstr_view_index(names, n, key);
		if (i == n || !(accepted >> i & 1)) {
			*other = (struct cstr_view){ key.data, util_clip(key.data, key.len,
			                                                 JB_KEY_SHOWN) };
			return EINVAL;
		}
		of[i] = it.val;
		*seen |= UINT64_C(1) << i;
	}
	return 0;
}

/** A parse a slice at a time; zeroed to begin. */
struct jb_parser {
	struct json_tokener *tok;
	struct json_object  *root;    /**< once the value has ended */
	size_t               off;     /**< bytes parsed */
	struct jb_scan       cut;     /**< where slices end; cut.values: values
	                                   parsed, cut.copied: string bytes copied */
	uint32_t             done;    /**< bool, widened over the padding */
	int                  error;   /**< 0 if the parse succeeded, ENOMEM if
	                                   memory ran out, or EINVAL if the text
	                                   is not one JSON value or breaks the
	                                   pool's limits; valid once done is
	                                   set */
};

/**
 * Parse the next slice of the @p len bytes at @p data, as jb_parse() would,
 * with a tokener from @p pool and within its limits: at most @p bytes bytes,
 * 4 at least, and @p values values. Returns EAGAIN while bytes remain to be
 * parsed. Once the parse is over, returns 0, and jb_parser_take() gives the
 * value, or the error that parser->error also keeps. A slice never ends
 * within a character, whose bytes it leaves to the next.
 */
extern int
jb_parser_step (struct jb_parser *parser,
                struct jb_pool   *pool,
                char const       *data,
                size_t            len,
                size_t            bytes,
                uint32_t          values);

/** The parsed value, which the caller now owns; nullptr after an error. */
static inline struct json_object *
jb_parser_take (struct jb_parser *parser)
{
	struct json_object *root = parser->error ? nullptr : parser->root;
	if (root)
		parser->root = nullptr;
	return root;
}

/**
 * End a parse, over or not, giving its tokener back to @p pool and what it
 * built and nobody took to @p reaper, or, with none, freeing that at once.
 */
extern void
jb_parser_fini (struct jb_parser *parser,
                struct jb_pool   *pool,
                struct jb_reaper *reaper);

/** Length of the longest prefix of the @p len bytes at @p str that is UTF-8
 *  text (RFC 3629), which json-c checks more leniently.
 */
extern size_t
jb_utf8 (char const *str,
         size_t      len);

/** Whether @p t is UTF-8 text without a NUL, as an empty view is. */
extern bool
jb_is_text (struct cstr_view t);

/**
 * Make the @p n bytes at @p p UTF-8 text, in place, each byte that breaks it
 * becoming a '?', but for a character cut short at their end while more
 * bytes follow (@p last false), which those may complete; returns the bytes
 * made text, all but that character's.
 */
extern size_t
jb_mend (char   *p,
         size_t  n,
         bool    last);

/**
 * Write @p len bytes of UTF-8 text to @p dst, which has room for 6 * @p len,
 * as the inside of a JSON string, escaped as json-c would with JB_WIRE.
 * Returns the bytes written, 0 if @p dst or @p src is nullptr.
 */
EXPORT
extern size_t
jb_escape (char       *dst,
           char const *src,
           size_t      len);

/** Append the UTF-8 text @p text to @p dest as the inside of a JSON
 *  string, escaped as jb_escape() writes it. Returns 0, or ENOMEM with a
 *  part of the escapes appended.
 */
extern int
jb_escape_cat (struct cstr_view  text,
               struct dstr      *dest);

/**
 * Returns the length of the longest prefix of the @p len bytes of UTF-8
 * text at @p src that is whole characters and whose escapes, as
 * jb_escape() writes them, take at most @p room bytes; 0 if @p src is
 * nullptr.
 */
EXPORT
extern size_t
jb_escape_fit (char const *src,
               size_t      len,
               size_t      room);

#endif /* ASKAI_SRC_JB_H_ */
