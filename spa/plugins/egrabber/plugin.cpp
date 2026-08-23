/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cerrno>

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#include "egrabber.hpp"

extern "C" {

SPA_LOG_TOPIC_DEFINE(egrabber_log_topic, "spa.egrabber");
SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory,
		uint32_t *index)
{
	spa_return_val_if_fail(factory != nullptr, -EINVAL);
	spa_return_val_if_fail(index != nullptr, -EINVAL);

	if (*index > 0)
		return 0;
	*factory = &spa_egrabber_source_factory;
	(*index)++;
	return 1;
}

}
