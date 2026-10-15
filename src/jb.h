/** @file Terse, leak-safe helpers for building and reading json-c values. */
#ifndef TWINCEPTION_SRC_JB_H_
#define TWINCEPTION_SRC_JB_H_

#include <stddef.h>
#include <stdint.h>

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
 *  is nullptr; returns 0 or ENOMEM. */
extern int
jb_set (struct json_object *obj,
        char const         *key,
        struct json_object *val);

/** Append @p val, which is consumed even on failure or when @p arr is
 *  nullptr; returns 0 or ENOMEM. */
extern int
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

/** @p val's text and its length in @p *len, if it is a string; else
 *  nullptr. */
extern char const *
jb_str (struct json_object *val,
        size_t             *len);

/** Whether @p val is the string @p str. */
extern bool
jb_is (struct json_object *val,
       char const         *str);

/**
 * Parse exactly one JSON value spanning @p len bytes; nullptr on error.  At
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
	uint32_t  cap;      /**< of members */
	uint32_t  values;   /**< values begun, object keys included */
	uint32_t  depth;    /**< containers open */
	enum {
		JB_SCAN_GAP,
		JB_SCAN_STRING,
		JB_SCAN_ESCAPE,
		JB_SCAN_SCALAR
	}         state;
};

/**
 * Scan up to @p len more bytes of a JSON text, which may arrive in any pieces,
 * for what parsing it would cost.  Stops before a byte that would open more
 * than @p depth containers at once, begin more than @p values values, give an
 * object more than @p members members, or take a number or literal past
 * @p scalar bytes (UINT32_MAX: no limit), and returns how many bytes it
 * scanned: @p len unless it stopped.  It does not validate.
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
 * Hand @p val to @p reaper to free; nothing else may hold a container within
 * it.  Should memory run out, it is freed at once.
 */
extern void
jb_reaper_take (struct jb_reaper   *reaper,
                struct json_object *val);

/** Free up to @p values values; returns how many it freed. */
extern size_t
jb_reaper_step (struct jb_reaper *reaper,
                size_t            values);

/** Free what is left, all at once. */
extern void
jb_reaper_fini (struct jb_reaper *reaper);

/**
 * Tokeners to reuse, all with the same limits; zeroed but for those.  A
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
 *  buffer's size, 0 if there is none. */
extern size_t
jb_pool_release (struct jb_pool *pool,
                 size_t          keep);

/** A parse a slice at a time; zeroed to begin. */
struct jb_parser {
	struct json_tokener *tok;
	struct json_object  *root;    /**< once the value has ended */
	size_t               off;     /**< bytes parsed */
	struct jb_scan       cut;     /**< where slices end; cut.values: values
	                                   parsed, cut.copied: string bytes copied */
	uint32_t             done;    /**< bool, widened over the padding */
	uint32_t             failed;  /**< bool, likewise */
};

/**
 * Parse the next slice of the @p len bytes at @p data, as jb_parse() would,
 * with a tokener from @p pool and within its limits: at most @p bytes bytes,
 * 4 at least, and @p values values.  Returns true once the parse is over,
 * when jb_parser_take() gives the value, nullptr on error.  A slice never
 * ends within a character, whose bytes it leaves to the next.
 */
extern bool
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
	struct json_object *root = parser->failed ? nullptr : parser->root;
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
 *  text (RFC 3629), which json-c checks more leniently. */
extern size_t
jb_utf8 (char const *str,
         size_t      len);

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
 * Returns the bytes written.
 */
extern size_t
jb_escape (char       *dst,
           char const *src,
           size_t      len);

/**
 * Returns the length of the longest prefix of the @p len bytes of UTF-8
 * text at @p src that is whole characters and whose escapes, as
 * jb_escape() writes them, take at most @p room bytes; 0 if @p src is
 * nullptr.
 */
extern size_t
jb_escape_fit (char const *src,
               size_t      len,
               size_t      room);

#endif /* TWINCEPTION_SRC_JB_H_ */
