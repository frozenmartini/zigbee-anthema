#pragma once
/*
 * c6_discovery: the board's mDNS record, _zigbee-anthema._tcp on port 80 (protocol §2).
 *
 * Instance name: c6_base_name(). TXT: api=1, fw, id, mac, hw=nanoc6, state (unclaimed/claimed), role
 * (none/mirror/shortcut, from NVS c6cfg "role"). The hostname stays c6-<id>. No _http._tcp.
 * None of these may be called inside a critical section (they take mDNS's lock and read NVS).
 */
void c6_discovery_start(void);                 /* after mdns_init; from c6_base's start_services */
void c6_discovery_update(void);                /* re-sets the TXT state and role: claim, clear, release, a role change */
void c6_discovery_set_name(const char *name);  /* mdns_service_instance_name_set */
