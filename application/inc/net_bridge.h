/*
 * UDP side of the bridge, plus Ethernet cable (PHY link) monitoring.
 */
#ifndef NET_BRIDGE_H
#define NET_BRIDGE_H

#include "nx_api.h"
#include "dgram_queue.h"

/* q_to_radio: datagrams received on Ethernet (producer), q_from_radio: datagrams to send (consumer). */
void net_bridge_start(NX_IP *ip, NX_PACKET_POOL *pool, dgram_queue_t *q_to_radio, dgram_queue_t *q_from_radio);

/* Wake the bridge thread: a datagram is waiting in q_from_radio. Safe from any thread. */
void net_bridge_notify(void);

#endif /* NET_BRIDGE_H */
