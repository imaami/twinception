/** @file schema.c
 *
 * Builders of the JSON Schema parts of tool definitions.
 */
#include "cstr_view.h"
#include "dstr.h"
#include "jb.h"
#include "schema.h"

struct json_object *
schema_text3 (char const *a,
              char const *b,
              char const *c)
{
	dstr_auto d = DSTR_INIT;
	if (!dstr_cat(&d, a) || !dstr_cat(&d, b) || !dstr_cat(&d, c))
		return nullptr;
	return json_object_new_string_len(dstr_get(&d), (int)dstr_len(&d));
}

struct json_object *
schema_names (struct cstr_view const *names,
              size_t                  n)
{
	struct json_object *arr = json_object_new_array();
	for (size_t i = 0; arr && i < n; ++i)
		arr = jb_push_or_free(arr, json_object_new_string_len(names[i].data,
		                                                      (int)names[i].len));
	return arr;
}

/* Returns @p obj with the member description set to @p about. A nullptr
 * @p about leaves @p obj unchanged.
 */
static struct json_object *
schema_about (struct json_object *obj,
              char const         *about)
{
	return about ? jb_set_or_free(obj, "description", json_object_new_string(about))
	             : obj;
}

struct json_object *
schema_property (char const *type,
                 char const *about)
{
	return schema_about(jb_object("type", json_object_new_string(type), nullptr), about);
}

struct json_object *
schema_enum (struct json_object *names,
             char const         *about)
{
	return schema_about(jb_object("type", json_object_new_string("string"),
	                              "enum", names, nullptr), about);
}

struct json_object *
schema_count (int32_t     min,
              char const *about)
{
	return schema_about(jb_object("type", json_object_new_string("integer"),
	                              "minimum", json_object_new_int(min), nullptr), about);
}

struct json_object *
schema_object (struct json_object *properties,
               struct json_object *required)
{
	return jb_object("type", json_object_new_string("object"),
	                 "properties", properties,
	                 "required", required,
	                 "additionalProperties", json_object_new_boolean(0),
	                 nullptr);
}
