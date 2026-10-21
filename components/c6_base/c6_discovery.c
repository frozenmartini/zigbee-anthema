/*
 * c6_discovery — the board's mDNS record (protocol §2).
 *
 * One service, _zigbee-anthema._tcp on port 80, its instance the board's name (c6_base_name), with the TXT
 * record api=1, fw, id, mac, hw=nanoc6, state (unclaimed/claimed), role (none/mirror/shortcut). The role is
 * read from NVS c6cfg "role" (u8) the way c6_base_name reads the name: the component cannot include main's
 * settings.h. The hostname (c6-<id>, set by c6_base) does not change. The record carries no secret.
 *
 * c6_discovery_update re-sets state and role: after a claim, a clear or a release (c6_auth.c), and after a
 * role change (settings.c, after s_light_mux). Nothing here may run inside a critical section.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "mdns.h"
#include "nvs.h"
#include "c6_auth.h"
#include "c6_base.h"
#include "c6_discovery.h"

static const char *TAG = "mdns";

#define DISC_SERVICE "_zigbee-anthema"
#define DISC_PROTO   "_tcp"
#define DISC_PORT    80
#define DISC_TXT_N   7

static volatile bool s_started;            /* the service is added: update and set_name may touch it */

/* The stored role's word: NVS c6cfg u8 "role" 0/1/2; anything else, or nothing stored, is "none". */
static const char *disc_role(void)
{
    static const char *const names[] = {"none", "mirror", "shortcut"};
    uint8_t role = 0;
    nvs_handle_t h;
    if (nvs_open("c6cfg", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "role", &role) != ESP_OK || role >= sizeof names / sizeof names[0]) {
            role = 0;
        }
        nvs_close(h);
    }
    return names[role];
}

static const char *disc_state(bool claimed)
{
    return claimed ? "claimed" : "unclaimed";
}

/* The TXT record into txt[DISC_TXT_N]; the values are the caller's strings and must outlive the call that
 * uses txt (mdns_service_add copies them). */
static void disc_txt(mdns_txt_item_t *txt, const char *fw, const char *id, const char *mac, const char *state,
                     const char *role)
{
    txt[0] = (mdns_txt_item_t){.key = "api", .value = "1"};
    txt[1] = (mdns_txt_item_t){.key = "fw", .value = fw};
    txt[2] = (mdns_txt_item_t){.key = "id", .value = id};
    txt[3] = (mdns_txt_item_t){.key = "mac", .value = mac};
    txt[4] = (mdns_txt_item_t){.key = "hw", .value = "nanoc6"};
    txt[5] = (mdns_txt_item_t){.key = "state", .value = state};
    txt[6] = (mdns_txt_item_t){.key = "role", .value = role};
}

void c6_discovery_start(void)
{
    if (s_started) {
        return;
    }
    char name[64];
    c6_base_name(name, sizeof name);
    c6_base_info_t bi;
    c6_base_info(&bi);
    mdns_txt_item_t txt[DISC_TXT_N];
    disc_txt(txt, bi.fw, bi.id, bi.mac, disc_state(c6_auth_claimed()), disc_role());
    esp_err_t err = mdns_service_add(name, DISC_SERVICE, DISC_PROTO, DISC_PORT, txt, DISC_TXT_N);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS %s.%s not added (%s)", DISC_SERVICE, DISC_PROTO, esp_err_to_name(err));
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "MDNS %s.%s on %d as \"%s\"", DISC_SERVICE, DISC_PROTO, DISC_PORT, name);
}

void c6_discovery_update(void)
{
    if (!s_started) {
        return;
    }
    const char *state = disc_state(c6_auth_claimed());
    const char *role = disc_role();
    esp_err_t err = mdns_service_txt_item_set(DISC_SERVICE, DISC_PROTO, "state", state);
    if (err == ESP_OK) {
        err = mdns_service_txt_item_set(DISC_SERVICE, DISC_PROTO, "role", role);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MDNS TXT not updated (%s)", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "MDNS TXT state=%s role=%s", state, role);
}

void c6_discovery_set_name(const char *name)
{
    if (!s_started || !name || !name[0]) {
        return;
    }
    esp_err_t err = mdns_service_instance_name_set(DISC_SERVICE, DISC_PROTO, name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MDNS name not set (%s)", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "MDNS name \"%s\"", name);
}
