/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "options.hpp"

#include <limits>

#include <spa/utils/string.h>

#include "egrabber.hpp"

namespace egrabber_pipewire {

void read_options(Options &options, const struct spa_dict *info)
{
	const char *value;
	uint32_t parsed;

	if (info == nullptr)
		return;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_PRODUCER)))
		options.producer = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_SERIAL)))
		options.serial = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_USER_ID)))
		options.user_id = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_CONTROL)))
		options.control = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_INTERFACE_INDEX)) &&
			spa_atou32(value, &parsed, 0) &&
			parsed <= static_cast<uint32_t>(std::numeric_limits<int>::max()))
		options.interface_index = static_cast<int>(parsed);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_DEVICE_INDEX)) &&
			spa_atou32(value, &parsed, 0) &&
			parsed <= static_cast<uint32_t>(std::numeric_limits<int>::max()))
		options.device_index = static_cast<int>(parsed);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_STREAM_INDEX)) &&
			spa_atou32(value, &parsed, 0) &&
			parsed <= static_cast<uint32_t>(std::numeric_limits<int>::max()))
		options.stream_index = static_cast<int>(parsed);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_BUFFER_COUNT))) {
		uint32_t count;
		if (spa_atou32(value, &count, 0))
			options.buffer_count = count;
	}
}

} // namespace egrabber_pipewire
