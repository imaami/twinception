/** @file schema.h
 *
 * Builders of the JSON Schema parts of tool definitions. Each builder
 * returns a new json-c value, or nullptr if memory runs out. A builder
 * consumes the json-c values passed to it, also when it fails.
 */
#ifndef TWINCEPTION_SRC_SCHEMA_H_
#define TWINCEPTION_SRC_SCHEMA_H_

#include <stddef.h>
#include <stdint.h>

struct json_object;
struct cstr_view;

/** Returns a JSON string that joins the texts @p a, @p b and @p c. */
extern struct json_object *
schema_text3 (char const *a,
              char const *b,
              char const *c);

/** Returns a JSON array of the @p n texts at @p names. */
extern struct json_object *
schema_names (struct cstr_view const *names,
              size_t                  n);

/**
 * Returns a property of the JSON type @p type with the description
 * @p about. A nullptr @p about gives a property without a description.
 */
extern struct json_object *
schema_property (char const *type,
                 char const *about);

/**
 * Returns a string property whose value must be one of the strings in the
 * array @p names. The description is @p about, as in schema_property().
 */
extern struct json_object *
schema_enum (struct json_object *names,
             char const         *about);

/**
 * Returns an integer property with the minimum @p min. The description is
 * @p about, as in schema_property().
 */
extern struct json_object *
schema_count (int32_t     min,
              char const *about);

/**
 * Returns the schema of an object with the members @p properties. The
 * array @p required names the members that the object must have. The
 * object can have no other members.
 */
extern struct json_object *
schema_object (struct json_object *properties,
               struct json_object *required);

#endif /* TWINCEPTION_SRC_SCHEMA_H_ */
