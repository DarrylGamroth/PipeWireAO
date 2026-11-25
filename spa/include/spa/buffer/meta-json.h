/* Simple Plugin API */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_META_JSON_H
#define SPA_META_JSON_H

#include <spa/buffer/meta.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spa_meta_profile {
	uint32_t count;
	struct spa_meta_requirement records[SPA_META_MAX_RECORDS];
};

static inline int spa_meta_json_uint(const char *value, int length, uint32_t *result)
{
	char text[64];
	if (length <= 0 || (size_t)length >= sizeof(text) || value[0] == '-')
		return -EINVAL;
	memcpy(text, value, length);
	text[length] = '\0';
	return spa_atou32(text, result, 0) ? 0 : -EINVAL;
}

/** Parse a queue or node's metadata declaration during setup. Header is always
 * required. NULL selects Header alone. On error the profile is incomplete. */
static inline int spa_meta_profile_parse(struct spa_meta_profile *profile, const char *text)
{
	struct spa_json root, array, object;
	const char *value;
	bool explicit_header = false;
	uint32_t bytes = SPA_ROUND_UP_N(sizeof(struct spa_meta_header), 8);
	int res;

	if (profile == NULL)
		return -EINVAL;
	memset(profile, 0, sizeof(*profile));
	profile->count = 1;
	profile->records[0].type = SPA_META_Header;
	profile->records[0].size = sizeof(struct spa_meta_header);
	profile->records[0].flags = SPA_META_REQUIREMENT_REQUIRED;
	if (text == NULL)
		return 0;
	size_t length = strlen(text);
	if (length > SPA_META_MAX_RECORDS * 256u)
		return -ENOSPC;
	spa_json_init(&root, text, length);
	if (spa_json_enter_array(&root, &array) <= 0)
		return -EINVAL;
	while ((res = spa_json_enter_object(&array, &object)) > 0) {
		struct spa_meta_requirement record = { 0, 0, 0, 0 };
		char key[32];
		uint32_t fields = 0, size;
		int len;
		while ((len = spa_json_get_string(&object, key, sizeof(key))) > 0) {
			uint32_t bit;
			len = spa_json_next(&object, &value);
			if (len <= 0)
				return -EINVAL;
			if (spa_streq(key, "type")) {
				bit = 1;
				if (spa_meta_json_uint(value, len, &record.type) < 0)
					return -EINVAL;
			} else if (spa_streq(key, "size")) {
				bit = 2;
				if (spa_meta_json_uint(value, len, &record.size) < 0)
					return -EINVAL;
			} else if (spa_streq(key, "required")) {
				bool required;
				bit = 4;
				if (spa_json_parse_bool(value, len, &required) <= 0)
					return -EINVAL;
				record.flags = required ? SPA_META_REQUIREMENT_REQUIRED : 0;
			} else {
				return -EINVAL;
			}
			if (fields & bit)
				return -EEXIST;
			fields |= bit;
		}
		if (len < 0 || fields != 7)
			return -EINVAL;
		if (record.type == SPA_META_Header) {
			if (explicit_header)
				return -EEXIST;
			explicit_header = true;
			if (record.size != sizeof(struct spa_meta_header) ||
			    record.flags != SPA_META_REQUIREMENT_REQUIRED)
				return -EINVAL;
			continue;
		}
		if (profile->count == SPA_META_MAX_RECORDS)
			return -ENOSPC;
		for (uint32_t i = 0; i < profile->count; i++)
			if (profile->records[i].type == record.type)
				return -EEXIST;
		if (!spa_meta_record_layout_validate(record.type, record.size, SPA_META_MAX_BYTES) ||
		    spa_meta_type_is_transport_owned(record.type))
			return -EINVAL;
		size = SPA_ROUND_UP_N(record.size, 8);
		if (size > SPA_META_MAX_BYTES - bytes)
			return -ENOSPC;
		bytes += size;
		profile->records[profile->count++] = record;
	}
	return res < 0 || spa_json_next(&root, &value) != 0 ? -EINVAL : 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* SPA_META_JSON_H */
