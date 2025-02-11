/*
Copyright (c) 2009-2020 Roger Light <roger@atchoo.org>
Copyright (C) 2021-2023 TMLake(Beijing) Technology Co., Ltd.

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"

#include <assert.h>
#include <string.h>

#define WITH_BROKER

#ifdef WITH_BROKER
#include "njet_iot_internal.h"
#endif

#include "mosquitto.h"
// #include "logging_mosq.h"
#include "memory_mosq.h"
#include "mqtt_protocol.h"
#include "njet_iot_packet_mosq.h"
#include "property_mosq.h"
#include "njet_iot_send_mosq.h"
#include "njet_iot_util_mosq.h"

int iot_send__unsubscribe(struct mosq_iot *mosq, int *mid, int topic_count, char *const *const topic, const mosquitto_property *properties)
{
	struct mosquitto__packet *packet = NULL;
	uint32_t packetlen;
	uint16_t local_mid;
	int rc;
	int i;
	size_t tlen;

	assert(mosq);
	assert(topic);

	packetlen = 2;
	for (i = 0; i < topic_count; i++)
	{
		tlen = strlen(topic[i]);
		if (tlen > UINT16_MAX)
		{
			return MOSQ_ERR_INVAL;
		}
		packetlen += 2U + (uint16_t)tlen;
	}

	packet = mosquitto__calloc(1, sizeof(struct mosquitto__packet));
	if (!packet)
		return MOSQ_ERR_NOMEM;

	if (mosq->protocol == mosq_p_mqtt5)
	{
		packetlen += property__get_remaining_length(properties);
	}

	packet->command = CMD_UNSUBSCRIBE | (1 << 1);
	packet->remaining_length = packetlen;
	rc = packet__alloc(packet);
	if (rc)
	{
		mosquitto__free(packet);
		return rc;
	}

	/* Variable header */
	local_mid = iot_mqtt__mid_generate(mosq);
	if (mid)
		*mid = (int)local_mid;
	packet__write_uint16(packet, local_mid);

	if (mosq->protocol == mosq_p_mqtt5)
	{
		/* We don't use User Property yet. */
		property__write_all(packet, properties, true);
	}

	/* Payload */
	for (i = 0; i < topic_count; i++)
	{
		packet__write_string(packet, topic[i], (uint16_t)strlen(topic[i]));
	}

#ifdef WITH_BROKER
#ifdef WITH_BRIDGE
	for (i = 0; i < topic_count; i++)
	{
		iot_log__printf(mosq, MOSQ_LOG_DEBUG, "Bridge %s sending UNSUBSCRIBE (Mid: %d, Topic: %s)", mosq->id, local_mid, topic[i]);
	}
#endif
#else
	for (i = 0; i < topic_count; i++)
	{
		iot_log__printf(mosq, MOSQ_LOG_DEBUG, "Client %s sending UNSUBSCRIBE (Mid: %d, Topic: %s)", mosq->id, local_mid, topic[i]);
	}
#endif
	return iot_packet__queue(mosq, packet);
}
