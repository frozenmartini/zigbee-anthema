/*
 * bulb — a board as a Zigbee end device that presents as a colour dimmable light.
 *
 * Bench phase: it joins the bench coordinator and logs everything it is
 * sent; it drives nothing yet. What it presents is chosen by CONFIG_C6_ZB_PROFILE_*: the
 * Espressif default (identity strings from Kconfig), or a clone of a real Sengled whose every
 * value was read off a real E11-N1EA.
 * CONFIG_C6_ZB_IEEE overrides the IEEE address; empty, it is the chip's own (0.7.0), or under the E11 profile
 * (0.4.0) B0CE18C600 and the last three bytes of the chip's base MAC (ieee_from_mac).
 *
 * Its scan covers CONFIG_C6_ZB_CHANNEL_MASK, every channel (11-26) since 0.5.0, and, when set, only the
 * extended PAN ID of the ext_pan setting (default CONFIG_C6_ZB_EXT_PANID), read when the stack starts.
 * The pin is what keeps a board off a neighbouring Zigbee2MQTT network (often on channel 11) and on the
 * Zigbee controller's. Until 0.4.0 the mask was channel 25 only.
 *
 * Joining (0.5.0): a factory-new board steers at boot, then again after each failed attempt: every
 * 10 s for the first 15 minutes after boot, a leave or POST /api/v1/zigbee/pair, then every 60 s,
 * until it joins. A board with a stored network rejoins it and is not steered while it does
 * (s_zb_rejoining). Since 0.5.1 the retries and pair do not ask "factory new", which the stack clears
 * once a steer has run. The Zigbee controller's own Add Group frame tells the board its group
 * (s_group, stored by the watchdog task in NVS c6zb/group).
 *
 * Log lines: ZB-RX (every frame received, from c6_zb), ZB-SET (every attribute the stack
 * changed because of a command), LIGHT (the resulting state), ZB-ACT (other ZCL callbacks).
 *
 * HTTP, on top of c6_base:
 *   GET  /zb          joined?, addresses, channel, the light state as last set
 *   POST /zb/steer    start network steering (token)
 *   POST /zb/leave    leave and forget the network (token)
 *   POST /zb/erase    erase all of NVS (every Zigbee dataset) and reboot (token)
 *   POST /zb/light?on=&level=&ct=   the client sets the light's state; On/Off reaches the Zigbee controller
 *                     as a report (ignored for 3 s after a Zigbee controller command). Since 0.5.0 it takes the
 *                     developer credential (c6_base_token_ok); it had no check before
 *   GET|PUT /api/v1/settings   the role, webhook, button timings and RX watchdog (token; 0.4.0,
 *                     api/v1/settings.c); ext_pan and name since 0.5.0
 *   POST /api/v1/zigbee/pair    start the fast join phase and steer now; 409 when already joined
 *   POST /api/v1/zigbee/leave   leave and forget the network, group cleared
 *   POST /api/v1/zigbee/erase   erase every NVS namespace but c6cfg, c6claim and c6wifi, then reboot
 *                     (0.5.0; the three take the claim token, JSON answers)
 *   GET  /api/v1/state          the state object, protocol §5 (owner; api/v1/state.c, 0.5.0)
 *   GET  /api/v1/events         the same as Server-Sent Events: state, then each change (owner; events.c)
 *   POST /api/v1/light          the mirror role's report of the real light, JSON (token; light.c)
 *   POST /api/v1/button/release the shortcut ran: release the held press (token; light.c)
 *   The state object is fed by state_mark() at each change (s_light, s_hub, the button, the network, the
 *   group, the settings); bulb_light_snapshot and bulb_button_snapshot copy this file's halves of it.
 *
 * Webhook bridge (0.3.18): each Zigbee controller light command, decoded from its frame, is POSTed as JSON to
 * the webhook (WEBHOOK in the log) with everything the Zigbee controller has asked for so far; the mirror role.
 *
 * Shortcut button (0.3.21; the shortcut role since 0.4.0): the touchscreen's light page
 * stays up for as long as the group is on, so the bulb does not stay on. A Zigbee controller command that leaves it on is a
 * press (BUTTON-PRESS), pushed once when the Zigbee controller has gone quiet, as {"press":N,...} with the level
 * and colour asked for. The client runs the shortcut and answers POST /zb/light?on=0, whose Off
 * report clears the touchscreen; failing that the bulb reports Off by itself after
 * off_after_s (BUTTON-TIMEOUT). Zigbee controller Offs are not pushed. Power-up is always Off.
 * 0.3.22: a pushed press is held until released, so Zigbee controller changes meanwhile are not new presses; an On
 * within rearm_ms of a release is released at once (BUTTON-LATE); settle 2 s.
 * 0.3.23: the client holds a switch on for the press and turns it off as its shortcut's last step; that Off is
 * the "done". A release the client did not make is pushed (BUTTON-RELEASE). Self-Off after 120 s.
 * 0.3.24: state is recorded
 * before the Zigbee lock is released; a self-release that fails is retried; a release before the press
 * reached the client is pushed after it; release pushes are retried, presses never; saves are tracked by
 * generation and retried on failure; the restore guard survives only crashes; POST /zb/light takes only
 * on=0 in button mode; /zb/report's remove= is withdrawn; the decoder takes only standard, complete
 * commands from the coordinator with APS success.
 * off_after_s defaults to 0 since 0.6.0: a press is held until the client releases it (mode 1) or the touchscreen
 * turns it off (mode 2).
 *
 * Watchdog: on the tested Zigbee controller the clone once stopped receiving at all (after a
 * run of colour commands) while Wi-Fi, the radio and the stack loop stayed up; a reboot fixed it.
 * The Zigbee controller acknowledges the bulb's On/Off report every 20 s, so once anything has been heard since
 * boot, rx_wd_s of silence (a setting since 0.4.0; 0 is off) logs ZB-STALL with every
 * task's state and reboots.
 * Consecutive stall reboots double the wait (RTC memory survives the restart); 30 healthy minutes
 * reset it. ZB-HEALTH is logged every minute either way.
 *   POST /zb/scan?dur=N[&mask=0x..][&type=ed]   active scan (each beacon logged as ZB-BEACON) or
 *                     energy scan (ZB-ENERGY per channel); mask defaults to the join mask (token)
 *   GET  /zb/binds    our own binding table, by Mgmt_Bind_req to ourselves: ZB-BIND lines in /log
 *   GET  /zb/key      the current network key, 32 hex in the stack's byte order (token; 0.3.25, for
 *                     decrypting a sniffer's captures). The key itself is never logged.
 *                     Only with CONFIG_C6_ZB_KEY_ENDPOINT (0.3.26; off by default).
 *
 * 0.3.26: the boards' sdkconfigs set CONFIG_LWIP_DHCP_RESTORE_LAST_IP, so a board asks for its last
 * address after a reboot (some routers give a fixed address only to a device that asks for one).
 *
 * 0.3.27: a group Off while already off is consumed before the stack sees it (ZB-DROP); the stack
 * answered one by reporting On for a second. See hub_tap.
 *
 * 0.4.0: one binary for every board. The role (none, mirror, shortcut), the webhook, the
 * button's timings and the RX watchdog are settings in NVS, changed live by PUT /api/v1/settings; an
 * empty store is role none: no pushes, no button. A role change while the light is on reports Off
 * first (ROLE-OFF), so the Zigbee controller starts clean.
 *
 * Power-up state: the stack keeps no attribute values across a restart, so a power cut
 * brought the clone back off, at level 0, with no colour, where a real Sengled comes back as it was.
 * Every attribute a command sets is saved to NVS once it has been still for 3 s (LIGHT-SAVE), and
 * written back into the data model when the bulb is back on the network (LIGHT-RESTORE), from the
 * stack's own signal handler. A boot that dies within 30 s of a restore makes the next one skip it.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_client.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/platform/datasets.h"
#include "ezbee/secur.h"
#include "c6_base.h"
#include "c6_auth.h"
#include "c6_zb.h"
#include "api/v1/settings.h"
#include "api/v1/state.h"

static const char *TAG = "bulb";

#define EP CONFIG_C6_ZB_ENDPOINT

/* The light's state: every attribute below, as a command last set it. `seen` marks the ones that have
 * been set at least once, so a restore never overwrites a default with a zero nobody chose. */
enum { L_ON, L_LEVEL, L_MODE, L_X, L_Y, L_CT, L_HUE, L_SAT, L_COUNT };
static const struct {
    uint16_t cluster, attr;
    uint8_t size;                 /* bytes in the attribute's type */
    const char *name;
} LIGHT_ATTRS[L_COUNT] = {
    [L_ON]    = {EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, 1, "on"},
    [L_LEVEL] = {EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID, 1, "level"},
    [L_MODE]  = {EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, 1, "mode"},
    [L_X]     = {EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID, 2, "x"},
    [L_Y]     = {EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID, 2, "y"},
    [L_CT]    = {EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID, 2, "ct"},
    [L_HUE]   = {EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID, 1, "hue"},
    [L_SAT]   = {EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, 1, "sat"},
};
typedef struct {
    uint16_t seen;                /* bit per L_* */
    uint16_t val[L_COUNT];
} light_state_t;
static light_state_t s_light;
portMUX_TYPE s_light_mux = portMUX_INITIALIZER_UNLOCKED;   /* not static: it guards the settings too (settings.h) */
/* Persistence bookkeeping, all under s_light_mux (0.3.24): every change to s_light bumps
 * s_light_gen; a save commits one generation and records it in s_saved_gen only once NVS has it. */
static int64_t s_light_changed_us;            /* when s_light last changed */
static uint32_t s_light_gen, s_saved_gen;     /* s_light's change count, and the one last committed */
#define LIGHT_NVS_NS   "c6bulb"
#define LIGHT_NVS_KEY  "light1"               /* bump the digit if light_state_t changes */
#define LIGHT_SAVE_AFTER_US (3 * 1000000LL)
static volatile bool s_zb_up;     /* the Zigbee lock exists only after esp_zigbee_init() */
static volatile uint32_t s_set_count;
static RTC_NOINIT_ATTR uint32_t s_stall_magic, s_stall_reboots;
#define STALL_MAGIC 0xC65A11EDu
static RTC_NOINIT_ATTR uint32_t s_restore_guard;   /* RESTORE_MAGIC while a restore is < 30 s old */
#define RESTORE_MAGIC 0xC6BE5702u
#define RESTORE_GUARD_US (30 * 1000000LL)
static int64_t s_restored_at_us;                   /* when this boot's restore ran; 0: none (s_light_mux) */
static volatile bool s_restart_for_stall;          /* the RX watchdog is restarting: keep the restore guard */
/* Webhook bridge (0.3.17, 0.3.18). What the Zigbee controller's commands ask for is decoded from the frames
 * themselves (hub_tap) and pushed to the webhook setting in the mirror role. Not from the stack's attributes: once
 * its colour control stopped applying MoveToColorTemperature after a hue/sat command
 * (Warm and Cool White left CT at 250), and its level ramps for 1.5 s. A set made here
 * (restore, POST /zb/light) is never pushed, so nothing travels back where it came from. */
static int64_t s_push_due_us;                      /* a Zigbee controller command not yet pushed; 0: none (s_light_mux) */
static int64_t s_hub_cmd_us;                       /* last Zigbee controller command, ever (s_light_mux) */
static TaskHandle_t s_push_task, s_button_task;    /* both exist from boot; hub_tap wakes both (0.4.0) */
static light_state_t s_hub;                        /* what the Zigbee controller's commands asked for (s_light_mux) */
#define ECHO_WINDOW_US (3 * 1000000LL)             /* POST /zb/light is ignored this long after a Zigbee controller command */
static int64_t s_press_at_us;                      /* button: last Zigbee controller command of a press not yet pushed; 0: none (s_light_mux) */
static int64_t s_off_at_us;                        /* button: report Off by itself at this time; 0: never (s_light_mux) */
static int64_t s_press_first_us;                   /* button: first Zigbee controller command of the press not yet pushed (s_light_mux) */
static bool s_held;                                /* button: a press was pushed and is not released yet (s_light_mux) */
static int64_t s_held_at_us;                       /* button: when this held press started; keeps a live timeout's elapsed time (s_light_mux) */
static uint32_t s_button_generation;              /* button: changes at each published role switch; cancels old work across waits (s_light_mux) */
static int64_t s_released_at_us;                   /* button: when the last pushed press was released; 0: never (s_light_mux) */
static const char *s_release_by;                   /* button: a release the client has not been told of; NULL: none (s_light_mux) */
static bool s_delivering;                          /* button: a press is decided and not yet posted to the webhook (s_light_mux) */
/* Joining (0.5.0): from factory new until joined, a steer at boot and then one after each
 * failed attempt, STEER_FAST_MS apart for STEER_FAST_FOR_US after boot, a leave or POST
 * /api/v1/zigbee/pair, then STEER_SLOW_MS apart. A board rejoining its stored network is not steered. */
#define STEER_FAST_MS     10000
#define STEER_SLOW_MS     60000
#define STEER_FAST_FOR_US (15 * 60 * 1000000LL)
static int64_t s_steer_fast_us;                    /* when the fast phase last began (s_light_mux) */
static esp_timer_handle_t s_steer_timer;           /* the next attempt (steer_timer_cb); created in zb_task */
/* The Zigbee controller group (0.5.0): learned from the Zigbee controller's own Add Group in hub_tap, stored by the watchdog task in
 * NVS c6zb/group (u16, 0: none), never from the stack's context. */
#define GROUP_NVS_NS  "c6zb"
#define GROUP_NVS_KEY "group"
static uint16_t s_group;                           /* 0: none (s_light_mux) */
static bool s_group_dirty;                         /* s_group not yet in NVS (s_light_mux) */
static volatile bool s_zb_erasing;                 /* POST /api/v1/zigbee/erase under way: the group is not stored */
static bool s_zb_saving;                           /* a watchdog NVS save is in flight (s_light_mux): erase must wait */
static volatile bool s_reporting_ok;               /* restore_reporting(1, 20) succeeded this boot, since the last leave */
/* Join state of our own (0.5.1; rejoining and searching 0.5.2), read without the Zigbee lock. */
static volatile bool s_zb_started;                 /* the stack has answered its start this boot (or the board joined):
                                                      until then pair answers 503 zigbee_busy */
static volatile bool s_zb_rejoining;               /* a stored network and not on it: a REBOOT that failed, or a
                                                      rejoin-type leave. Pair answers 409 rejoining, the timer does
                                                      not steer. Cleared by a join or a reset leave */
static volatile bool s_zb_searching;               /* a steer search is on: armed attempts may re-arm. Cleared by a join */
#define ZB_ERASE_MAX_NS 32                         /* namespaces POST /api/v1/zigbee/erase can collect */
/* The state object (0.5.0; api/v1/state.h), all under s_light_mux. */
static const char *s_light_by;                     /* who last changed s_light: "zigbee", "api", "self", "restore"; NULL: nobody */
static int64_t s_light_at_us;                      /* when; 0: never */
static uint32_t s_hub_seq;                         /* Zigbee controller commands decoded since boot (hub_tap's `any`) */
static uint32_t s_presses;                         /* button: presses since boot (button_task's local until 0.4.0) */
static const char *s_release_last;                 /* button: the last release, "api", "zigbee" or "timeout"; NULL: none */
static int64_t s_button_at_us;                     /* button: the last change of s_held or s_presses; 0: never */
_Static_assert((int)SNAP_COUNT == (int)L_COUNT && (int)SNAP_ON == (int)L_ON && (int)SNAP_LEVEL == (int)L_LEVEL
               && (int)SNAP_MODE == (int)L_MODE && (int)SNAP_X == (int)L_X && (int)SNAP_Y == (int)L_Y
               && (int)SNAP_CT == (int)L_CT && (int)SNAP_HUE == (int)L_HUE && (int)SNAP_SAT == (int)L_SAT,
               "state.h's SNAP_* follow L_*");     /* (int): two anonymous enums (-Werror=enum-compare) */
#define BUTTON_RETRY_US (5 * 1000000LL)            /* button: a self-release that could not be applied is retried */

/* ---------------------------------------------------------------- ZCL */

/* s_light changed, by `by` ("zigbee", "api", "self", "restore"): call inside s_light_mux. */
static inline void light_mark_changed_locked(int64_t now, const char *by)
{
    s_light_gen++;
    s_light_changed_us = now;
    s_light_by = by;
    s_light_at_us = now;
    state_mark(ST_LIGHT);
}

/* The light.by of a light_apply caller, from its log name: API-SET and API-RELEASE are the
 * client's; ROLE-OFF, BUTTON-TIMEOUT and BUTTON-LATE the board's own. */
static const char *light_by_of(const char *who)
{
    return strncmp(who, "API-", 4) == 0 ? "api" : "self";
}

/* The button.release_by of a release made by a light_apply caller: the client's, or the button's own
 * timeout (BUTTON-*); NULL for one that is neither (ROLE-OFF clears the button anyway). */
static const char *light_release_by_of(const char *who)
{
    if (strncmp(who, "API-", 4) == 0) {
        return "api";
    }
    return strncmp(who, "BUTTON-", 7) == 0 ? "timeout" : NULL;
}

static void log_light(const char *what, const light_state_t *l)
{
    char buf[160];
    int n = snprintf(buf, sizeof buf, "%s", what);
    for (int i = 0; i < L_COUNT && n < (int)sizeof buf; i++) {
        n += (l->seen & (1u << i)) ? snprintf(buf + n, sizeof buf - n, " %s=%u", LIGHT_ATTRS[i].name, l->val[i])
                                   : snprintf(buf + n, sizeof buf - n, " %s=-", LIGHT_ATTRS[i].name);
    }
    ESP_LOGI(TAG, "%s", buf);
}

static void on_set_attr(const ezb_zcl_set_attr_value_message_t *m)
{
    const ezb_zcl_attribute_t *a = &m->in.attribute;
    uint32_t v = 0;
    if (a->data.value) {
        memcpy(&v, a->data.value, MIN(a->data.size, sizeof v));
    }
    s_set_count++;
    ESP_LOGI(TAG, "ZB-SET ep=%u cluster=0x%04X attr=0x%04X type=0x%02X value=%lu status=0x%02X",
             m->info.dst_ep, m->info.cluster_id, a->id, a->data.type, (unsigned long)v, m->info.status);
    int i = 0;
    while (i < L_COUNT && !(LIGHT_ATTRS[i].cluster == m->info.cluster_id && LIGHT_ATTRS[i].attr == a->id)) {
        i++;
    }
    if (i == L_COUNT) {
        return;
    }
    light_state_t l;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_light_mux);
    s_light.val[i] = (uint16_t)v;
    s_light.seen |= 1u << i;
    light_mark_changed_locked(now, "zigbee");
    l = s_light;
    portEXIT_CRITICAL(&s_light_mux);
    log_light("LIGHT", &l);
}

/* The Zigbee controller group after a Groups-cluster (0x0004) command from the Zigbee controller, client to server: Add
 * Group (0x00) and Add Group If Identifying (0x05) take the u16 LE at p[0..1]; Remove Group (0x03) clears it
 * when it names `cur`; Remove All Groups (0x04) clears it. Any other command, or an add or a remove of
 * fewer than two payload bytes, leaves `cur`. Pure: hub_tap calls it inside s_light_mux. */
static uint16_t group_after_cmd(uint8_t cmd, const uint8_t *p, int n, uint16_t cur)
{
    uint16_t g = n >= 2 ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
    switch (cmd) {
    case 0x00:
    case 0x05:
        return n >= 2 ? g : cur;
    case 0x03:
        return (n >= 2 && g == cur) ? 0 : cur;
    case 0x04:
        return 0;
    default:
        return cur;
    }
}

/* A Zigbee controller light command, decoded from the frame: ZCL cluster-specific, client to server, On/Off, Level or
 * Color Control, standard (not manufacturer-specific), delivered with APS success, from the Zigbee controller's
 * coordinator (0x0000), with at least the mandatory payload (0.3.24). Group membership is not
 * checked here: the stack's APS layer is assumed to deliver only our groups' frames, which every logged
 * frame bears out, but the library does not document it.
 *
 * Returns true to consume the frame (0.3.27): a group Off while the light is already off. The stack
 * handles that Off by setting level 254 and On, then level 0 and Off, 0.1 s apart, so the bulb reported
 * On to the Zigbee controller and Off a second later. Seen with the tested Zigbee controller, whose
 * LIGHT_STS said the group OFF, ON, OFF within 0.6 s. A real bulb
 * that is off ignores an Off. The frame is still decoded here and pushed to the webhook as before; the stack
 * just never sees it. Group frames only: a unicast Off expects a Default Response from the stack. */
static bool hub_tap(const ezb_apsde_data_ind_t *ind)
{
    if (ind->status != 0 || ind->profile_id != 0x0104 || ind->asdu_length < 3) {
        return false;
    }
    if (ind->src_address.addr_mode != EZB_ADDR_MODE_SHORT || ind->src_address.u.short_addr != 0x0000) {
        return false;                             /* only the Zigbee controller's commands */
    }
    const uint8_t *a = ind->asdu;
    uint8_t fc = a[0];
    if ((fc & 0x03) != 0x01 || (fc & 0x08) || (fc & 0x04)) {
        return false;                             /* not cluster-specific, server to client, or manufacturer-specific */
    }
    uint8_t cmd = a[2];
    const uint8_t *p = a + 3;
    int n = ind->asdu_length - 3;
    if (ind->cluster_id == EZB_ZCL_CLUSTER_ID_GROUPS) {
        /* The Zigbee controller's own Add Group tells the board its group (0.5.0). The stack still handles the frame; the
         * watchdog task stores the change, never this context. */
        bool changed = false;
        uint16_t g;
        portENTER_CRITICAL(&s_light_mux);
        g = group_after_cmd(cmd, p, n, s_group);
        if (g != s_group) {
            s_group = g;
            s_group_dirty = true;
            changed = true;
        }
        portEXIT_CRITICAL(&s_light_mux);
        if (changed) {
            ESP_LOGI(TAG, "ZB-GROUP 0x%04X (command 0x%02X from the Zigbee controller)", (unsigned)g, cmd);
            state_mark(ST_ZIGBEE);
        }
        return false;
    }
    int set[L_COUNT];
    for (int i = 0; i < L_COUNT; i++) {
        set[i] = -1;
    }
    bool off_cmd = false;
    switch (ind->cluster_id) {
    case EZB_ZCL_CLUSTER_ID_ON_OFF:
        if (cmd == 0x00 || (cmd == 0x40 && n >= 2)) {
            set[L_ON] = 0;
            off_cmd = true;
        } else if (cmd == 0x01 || cmd == 0x41) {
            set[L_ON] = 1;
        } else if (cmd == 0x02) {
            set[L_ON] = 2;                        /* toggle, resolved below */
        } else if (cmd == 0x42 && n >= 5) {
            /* OnWithTimedOff: "accept only when on" (bit 0) is honoured; its timers are not modelled */
            set[L_ON] = (p[0] & 0x01) ? 3 : 1;
        }
        break;
    case EZB_ZCL_CLUSTER_ID_LEVEL:
        if ((cmd == 0x00 || cmd == 0x04) && n >= 3) {
            set[L_LEVEL] = p[0];
            if (cmd == 0x04) {
                set[L_ON] = p[0] > 0;             /* MoveToLevelWithOnOff */
            }
        }
        break;
    case EZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
        if (cmd == 0x0A && n >= 4) {
            set[L_CT] = p[0] | p[1] << 8;
            set[L_MODE] = 2;
        } else if (cmd == 0x06 && n >= 4) {
            set[L_HUE] = p[0];
            set[L_SAT] = p[1];
            set[L_MODE] = 0;
        } else if (cmd == 0x00 && n >= 4) {
            set[L_HUE] = p[0];
            set[L_MODE] = 0;
        } else if (cmd == 0x03 && n >= 3) {
            set[L_SAT] = p[0];
            set[L_MODE] = 0;
        } else if (cmd == 0x07 && n >= 6) {
            set[L_X] = p[0] | p[1] << 8;
            set[L_Y] = p[2] | p[3] << 8;
            set[L_MODE] = 1;
        }
        break;
    default:
        return false;
    }
    bool any = false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_light_mux);
    uint8_t role = cfg_live.role;
    /* What the stack holds, not what the Zigbee controller asked for: the stack's dance happens only when it is off. */
    bool drop = off_cmd && ind->dst_address.addr_mode == EZB_ADDR_MODE_GROUP
                && (s_light.seen & (1u << L_ON)) && !s_light.val[L_ON];
    if (!s_hub.seen) {
        s_hub = s_light;                          /* start from what the bulb was */
    }
    bool was_on = (s_hub.seen & (1u << L_ON)) && s_hub.val[L_ON];
    if (set[L_ON] == 2) {
        set[L_ON] = !was_on;
    } else if (set[L_ON] == 3) {
        set[L_ON] = was_on ? 1 : -1;              /* accept only when on: discarded while off */
    }
    for (int i = 0; i < L_COUNT; i++) {
        if (set[i] >= 0) {
            s_hub.val[i] = (uint16_t)set[i];
            s_hub.seen |= 1u << i;
            any = true;
        }
    }
    if (any) {
        s_hub_cmd_us = now;
        s_hub_seq++;                              /* one decoded Zigbee controller command: the state object's controller.seq */
        if (role == CFG_ROLE_MIRROR) {
            s_push_due_us = now;                  /* only push_task reads it */
        }
    }
    if (any && role == CFG_ROLE_SHORTCUT) {
        if ((s_hub.seen & (1u << L_ON)) && s_hub.val[L_ON]) {
            if (!s_held) {                        /* while held, a change is part of the press already pushed */
                if (!s_press_at_us) {
                    s_press_first_us = now;
                }
                s_press_at_us = now;              /* (re)start the quiet time */
            }
        } else {
            if (s_held) {
                s_released_at_us = now;
                s_release_by = "zigbee";          /* turned off at the touchscreen: tell the client */
                s_release_last = "zigbee";
                s_button_at_us = now;
            }
            s_press_at_us = s_off_at_us = 0;      /* the Zigbee controller turned it off: nothing to push or clear */
            s_held = false;
            s_held_at_us = 0;
        }
    }
    portEXIT_CRITICAL(&s_light_mux);
    if (any) {
        state_mark(role == CFG_ROLE_SHORTCUT ? ST_HUB | ST_BUTTON : ST_HUB);
    }
    if (any && s_push_task) {
        xTaskNotifyGive(s_push_task);             /* each task looks at the role when woken */
    }
    if (any && s_button_task) {
        xTaskNotifyGive(s_button_task);
    }
    if (drop) {
        ESP_LOGI(TAG, "ZB-DROP group 0x%04X Off: already off, not handed to the stack",
                 (unsigned)ind->dst_address.u.group_addr.group);
    }
    return drop;
}

/* Called from the watchdog task, once the light has been still for LIGHT_SAVE_AFTER_US: a level
 * transition sets the level every 100 ms, and flash should see the end of it, not every step. The
 * state and its generation are taken together, and only a committed save marks that generation saved:
 * a change made meanwhile stays unsaved, and a failed save is tried again on the next pass. */
static void light_save_if_settled(int64_t now)
{
    light_state_t l;
    uint32_t gen;
    bool due;
    portENTER_CRITICAL(&s_light_mux);
    gen = s_light_gen;
    due = !s_zb_erasing && gen != s_saved_gen && now - s_light_changed_us >= LIGHT_SAVE_AFTER_US;
    l = s_light;
    if (due) {
        s_zb_saving = true;
    }
    portEXIT_CRITICAL(&s_light_mux);
    if (!due) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(LIGHT_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, LIGHT_NVS_KEY, &l, sizeof l);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_light_mux);
    s_zb_saving = false;
    portEXIT_CRITICAL(&s_light_mux);
    static uint32_t s_save_fails;
    if (err != ESP_OK) {
        if (s_save_fails++ % 60 == 0) {           /* every 5 s while it fails: log the first and every 5 min */
            ESP_LOGW(TAG, "LIGHT-SAVE failed (%lu in a row): %s; trying again", (unsigned long)s_save_fails,
                     esp_err_to_name(err));
        }
        return;
    }
    s_save_fails = 0;
    portENTER_CRITICAL(&s_light_mux);
    bool unsaved = s_light_gen != s_saved_gen;
    s_saved_gen = gen;
    if (unsaved && s_light_gen == s_saved_gen) {
        state_mark(ST_LIGHT);
    }
    portEXIT_CRITICAL(&s_light_mux);
    log_light("LIGHT-SAVE", &l);
}

/* Called from the watchdog task (0.5.0): stores s_group in NVS c6zb/group (u16, 0: none) while it is dirty.
 * Dirty is cleared only after the commit, and only if the group is still the one written; a failure is tried
 * again on the next pass. Nothing is stored while POST /api/v1/zigbee/erase runs. */
static void group_save_if_dirty(void)
{
    uint16_t g;
    bool due;
    portENTER_CRITICAL(&s_light_mux);
    due = s_group_dirty && !s_zb_erasing;
    g = s_group;
    if (due) {
        s_zb_saving = true;
    }
    portEXIT_CRITICAL(&s_light_mux);
    if (!due) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(GROUP_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, GROUP_NVS_KEY, g);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_light_mux);
    s_zb_saving = false;
    portEXIT_CRITICAL(&s_light_mux);
    static uint32_t s_group_save_fails;
    if (err != ESP_OK) {
        if (s_group_save_fails++ % 60 == 0) {     /* every 5 s while it fails: log the first and every 5 min */
            ESP_LOGW(TAG, "ZB-GROUP save failed (%lu in a row): %s; trying again",
                     (unsigned long)s_group_save_fails, esp_err_to_name(err));
        }
        return;
    }
    s_group_save_fails = 0;
    portENTER_CRITICAL(&s_light_mux);
    if (s_group == g) {
        s_group_dirty = false;                    /* a change made during the write stays dirty */
    }
    portEXIT_CRITICAL(&s_light_mux);
    ESP_LOGI(TAG, "ZB-GROUP saved 0x%04X", (unsigned)g);
}

/* At boot, before the stack starts: the stored group; absent or unreadable, none. */
static void group_load(void)
{
    uint16_t g = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(GROUP_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u16(h, GROUP_NVS_KEY, &g);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        g = 0;
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "ZB-GROUP not readable (%s); none", esp_err_to_name(err));
        }
    }
    portENTER_CRITICAL(&s_light_mux);
    s_group = g;
    s_group_dirty = false;
    portEXIT_CRITICAL(&s_light_mux);
    ESP_LOGI(TAG, "ZB-GROUP at boot: 0x%04X%s", (unsigned)g, g ? "" : " (none)");
}

/* Forget the Zigbee controller group (a leave; POST /api/v1/zigbee/leave): 0, stored by the watchdog task. */
static void group_clear(void)
{
    portENTER_CRITICAL(&s_light_mux);
    s_group = 0;
    s_group_dirty = true;
    portEXIT_CRITICAL(&s_light_mux);
}

/* The restore guard protects against a restore that crashes the board (0.3.24). Only a
 * crash may carry it into the next boot: a deliberate restart (OTA, /zb/erase, any esp_restart() but
 * the RX watchdog's) clears it on the way down, a power-on or external reset clears it at boot, and so
 * does a fresh join, which has nothing to restore. */
static void restore_guard_on_shutdown(void)
{
    if (!s_restart_for_stall) {
        s_restore_guard = 0;
    }
}

static void restore_guard_at_boot(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    bool crash = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT
                 || rr == ESP_RST_CPU_LOCKUP;
    if (!crash && rr != ESP_RST_SW && s_restore_guard) {
        s_restore_guard = 0;                      /* power-on, external, brownout...: RTC memory is not ours */
    }
    esp_register_shutdown_handler(restore_guard_on_shutdown);
}

/* On return to the network, before reporting restarts: put the saved state back into the data
 * model, so the first report the Zigbee controller gets after a power cut says what the light was. Runs in the
 * stack's signal handler. If the boot that last tried this died within RESTORE_GUARD_US, skip it
 * once rather than crash on every boot. */
static void light_restore(void)
{
    if (s_restore_guard == RESTORE_MAGIC) {
        s_restore_guard = 0;
        ESP_LOGW(TAG, "LIGHT-RESTORE skipped: the last boot died within 30 s of one; starting from the defaults");
        return;
    }
    light_state_t l;
    size_t len = sizeof l;
    nvs_handle_t h;
    esp_err_t err = nvs_open(LIGHT_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, LIGHT_NVS_KEY, &l, &len);
        nvs_close(h);
    }
    if (err != ESP_OK || len != sizeof l) {
        ESP_LOGI(TAG, "LIGHT-RESTORE nothing saved (%s); starting from the defaults", esp_err_to_name(err));
        return;
    }
    if (cfg_role() == CFG_ROLE_SHORTCUT) {
        l.val[L_ON] = 0;          /* a button comes back released, never pressed */
        l.seen |= 1u << L_ON;
    }
    s_restore_guard = RESTORE_MAGIC;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_light_mux);
    s_restored_at_us = now;
    portEXIT_CRITICAL(&s_light_mux);
    for (int i = 0; i < L_COUNT; i++) {
        if (!(l.seen & (1u << i))) {
            continue;
        }
        uint8_t v8 = (uint8_t)l.val[i];
        uint16_t v16 = l.val[i];
        void *v = LIGHT_ATTRS[i].size == 1 ? (void *)&v8 : (void *)&v16;
        ezb_zcl_status_t st = ezb_zcl_set_attr_value(EP, LIGHT_ATTRS[i].cluster, EZB_ZCL_CLUSTER_SERVER,
                                                     LIGHT_ATTRS[i].attr, EZB_ZCL_STD_MANUF_CODE, v, false);
        if (st != EZB_ZCL_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "LIGHT-RESTORE %s: status 0x%02X", LIGHT_ATTRS[i].name, st);
        }
        if (i == L_MODE) {        /* EnhancedColorMode follows ColorMode for the modes the Zigbee controller uses */
            ezb_zcl_set_attr_value(EP, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_CLUSTER_SERVER,
                                   EZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID, EZB_ZCL_STD_MANUF_CODE, &v8, false);
        }
    }
    portENTER_CRITICAL(&s_light_mux);
    s_light = l;
    light_mark_changed_locked(now, "restore");    /* a save already under way then stays stale */
    s_saved_gen = s_light_gen;                    /* what NVS holds: nothing to save */
    portEXIT_CRITICAL(&s_light_mux);
    log_light("LIGHT-RESTORE", &l);
}

static void on_zcl_action(ezb_zcl_core_action_callback_id_t id, void *message)
{
    if (id == EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID) {
        on_set_attr(message);
    } else {
        ESP_LOGI(TAG, "ZB-ACT callback id=0x%04lX", (unsigned long)id);
    }
}

#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
#define PROFILE_MANUFACTURER "sengled"
#define PROFILE_MODEL        "E11-N1EA"
#define PROFILE_MANUF_CODE   0x1160

static void custom_noop(uint8_t ep_id)
{
}

/* Sengled E11-N1EA, as read off the real bulb.
 * Not reproducible here: its Diagnostics cluster 0x0B05 (no descriptor in esp-zigbee-lib 2.0.4) and its node descriptor's
 * server mask 0x0000 (no setter). */
static void sengled_e11_extras(ezb_af_ep_desc_t ep)
{
    ESP_ERROR_CHECK(ezb_af_ep_desc_set_app_version(ep, 0));

    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    static const uint8_t app = 1, stack = 0, hw = 1;
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &app);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &stack);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &hw);

    ezb_zcl_cluster_desc_t color = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_CLUSTER_SERVER);
    static const uint8_t zero8 = 0, intensity = 250;
    static const struct { uint16_t x_id, y_id, i_id, x, y; } prim[] = {
        {0x0011, 0x0012, 0x0013, 48128, 17152},
        {0x0015, 0x0016, 0x0017, 17920, 46848},
        {0x0019, 0x001A, 0x001B, 10752, 512},
        {0x0020, 0x0021, 0x0022, 29333, 26705},
    };
    for (size_t i = 0; i < sizeof prim / sizeof prim[0]; i++) {
        ezb_zcl_color_control_cluster_desc_add_attr(color, prim[i].x_id, &prim[i].x);
        ezb_zcl_color_control_cluster_desc_add_attr(color, prim[i].y_id, &prim[i].y);
        ezb_zcl_color_control_cluster_desc_add_attr(color, prim[i].i_id, &intensity);
    }

    ezb_zcl_metering_cluster_server_config_t mcfg = {
        .current_summation_delivered = 0,
        .status = 0,
        .unit_of_measure = 0,            /* kWh */
        .summation_formatting = 33,
        .metering_device_type = 0,       /* electric */
    };
    ezb_zcl_cluster_desc_t met = ezb_zcl_metering_create_cluster_desc(&mcfg, EZB_ZCL_CLUSTER_SERVER);
    static const uint32_t mult = 1, div = 10000;
    static const uint8_t fmt = 33;
    static const int32_t demand = 0;
    ezb_zcl_metering_cluster_desc_add_attr(met, EZB_ZCL_ATTR_METERING_MULTIPLIER_ID, &mult);
    ezb_zcl_metering_cluster_desc_add_attr(met, EZB_ZCL_ATTR_METERING_DIVISOR_ID, &div);
    ezb_zcl_metering_cluster_desc_add_attr(met, EZB_ZCL_ATTR_METERING_DEMAND_FORMATTING_ID, &fmt);
    ezb_zcl_metering_cluster_desc_add_attr(met, EZB_ZCL_ATTR_METERING_HISTORICAL_CONSUMPTION_FORMATTING_ID, &fmt);
    ezb_zcl_metering_cluster_desc_add_attr(met, EZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID, &demand);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, met));

    /* Sengled's manufacturer clusters, attributes manufacturer-specific under 0x1160 */
    ezb_zcl_custom_cluster_config_t fc03_cfg = {.cluster_id = 0xFC03, .init_func = custom_noop, .deinit_func = custom_noop};
    ezb_zcl_cluster_desc_t fc03 = ezb_zcl_custom_create_cluster_desc(&fc03_cfg, EZB_ZCL_CLUSTER_SERVER);
    static const uint16_t fc03_0 = 4559;
    static const uint32_t fc03_1 = 655370000, fc03_2 = 251209488, fc03_3 = 255002626;
    ezb_zcl_custom_cluster_desc_add_manuf_attr(fc03, 0x0000, EZB_ZCL_ATTR_TYPE_UINT16, EZB_ZCL_ATTR_ACCESS_READ_ONLY, PROFILE_MANUF_CODE, &fc03_0);
    ezb_zcl_custom_cluster_desc_add_manuf_attr(fc03, 0x0001, EZB_ZCL_ATTR_TYPE_UINT32, EZB_ZCL_ATTR_ACCESS_READ_ONLY, PROFILE_MANUF_CODE, &fc03_1);
    ezb_zcl_custom_cluster_desc_add_manuf_attr(fc03, 0x0002, EZB_ZCL_ATTR_TYPE_UINT32, EZB_ZCL_ATTR_ACCESS_READ_ONLY, PROFILE_MANUF_CODE, &fc03_2);
    ezb_zcl_custom_cluster_desc_add_manuf_attr(fc03, 0x0003, EZB_ZCL_ATTR_TYPE_UINT32, EZB_ZCL_ATTR_ACCESS_READ_ONLY, PROFILE_MANUF_CODE, &fc03_3);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, fc03));

    ezb_zcl_custom_cluster_config_t fc04_cfg = {.cluster_id = 0xFC04, .init_func = custom_noop, .deinit_func = custom_noop};
    ezb_zcl_cluster_desc_t fc04 = ezb_zcl_custom_create_cluster_desc(&fc04_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_custom_cluster_desc_add_manuf_attr(fc04, 0x0000, EZB_ZCL_ATTR_TYPE_UINT8, EZB_ZCL_ATTR_ACCESS_READ_ONLY, PROFILE_MANUF_CODE, &zero8);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, fc04));

    ezb_zcl_ota_upgrade_cluster_client_config_t ocfg = {
        .upgrade_server_id = UINT64_MAX,
        .file_offset = UINT32_MAX,
        .image_upgrade_status = 0,
        .manufacturer_id = PROFILE_MANUF_CODE,
        .image_type_id = 0,
    };
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_ota_upgrade_create_cluster_desc(&ocfg, EZB_ZCL_CLUSTER_CLIENT)));
}
#else
#define PROFILE_MANUFACTURER CONFIG_C6_ZB_MANUFACTURER
#define PROFILE_MODEL        CONFIG_C6_ZB_MODEL
#define PROFILE_MANUF_CODE   CONFIG_C6_ZB_MANUF_CODE
#endif
/* create_device's ZCL strings hold 32 characters, and their length byte is strlcpy's (the source's) length */
_Static_assert(sizeof(PROFILE_MANUFACTURER) <= 33 && sizeof(PROFILE_MODEL) <= 33,
               "C6_ZB_MANUFACTURER and C6_ZB_MODEL: at most 32 characters");

/* The Color Control server's colour shape, the same for both profiles: CurrentHue and CurrentSaturation only
 * with the hue answer on (off, the Zigbee controller's read of CurrentHue is answered 0x86 and it offers brightness only),
 * the colour temperature and its physical range always. create_device sets the capabilities to match. */
static void color_shape(ezb_af_ep_desc_t ep, bool hue)
{
    ezb_zcl_cluster_desc_t color = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_CLUSTER_SERVER);
    static const uint8_t zero8 = 0;
    static const uint16_t ct = 370, ct_min = 154, ct_max = 500;
    if (hue) {
        ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID, &zero8);
        ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, &zero8);
    }
    ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID, &ct);
    ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID, &ct_min);
    ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID, &ct_max);
}

static void create_device(void)
{
    /* ZCL character strings: a length byte, then the text; static, as the stack keeps the pointer */
    static char manuf[34], model[34];
    manuf[0] = (char)strlcpy(manuf + 1, PROFILE_MANUFACTURER, sizeof manuf - 1);
    model[0] = (char)strlcpy(model + 1, PROFILE_MODEL, sizeof model - 1);

    bool hue = cfg_hue();                      /* the setting, read once before the stack starts: a change
                                                * applies at the next reboot or /zigbee/erase */
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ezb_zha_color_dimmable_light_config_t cfg = EZB_ZHA_COLOR_DIMMABLE_LIGHT_CONFIG();
    cfg.basic_cfg.power_source = EZB_ZCL_BASIC_POWER_SOURCE_SINGLE_PHASE_MAINS;
    cfg.color_cfg.color_capabilities = hue ? 0x19 : 0x10;   /* hue/sat, xy, colour temperature; or colour temperature only */
#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
    cfg.basic_cfg.zcl_version = 1;
    cfg.color_cfg.current_x = 24939;
    cfg.color_cfg.current_y = 24701;
    cfg.color_cfg.color_mode = 2;              /* colour temperature */
    cfg.color_cfg.enhanced_color_mode = 2;
    cfg.color_cfg.number_of_primaries = 4;
#endif
    ezb_af_ep_desc_t ep = ezb_zha_create_color_dimmable_light(EP, &cfg);
    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC,
                                                                    EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manuf);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model);
    color_shape(ep, hue);
#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
    sengled_e11_extras(ep);
    ESP_ERROR_CHECK(ezb_af_ep_desc_set_app_device_id(ep, CONFIG_C6_ZB_E11_DEVICE_ID));
#endif
    ESP_LOGI(TAG, "ZB hue %s", hue ? "on" : "off");
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, ep));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev));
    ezb_af_node_desc_set_manuf_code(PROFILE_MANUF_CODE);
    ezb_zcl_core_action_handler_register(on_zcl_action);
    ESP_LOGI(TAG, "device: colour dimmable light, endpoint %d, \"%s\" / \"%s\", manufacturer code 0x%04X",
             EP, PROFILE_MANUFACTURER, PROFILE_MODEL, PROFILE_MANUF_CODE);
}

/* The Zigbee controller configures On/Off reporting (1-20 s) once, at pairing, and takes the reports as the
 * bulb's sign of life. A new firmware image comes back with that reporting at its default
 * (disabled) while the network membership survives, so the bulb goes silent and the Zigbee controller writes it
 * off (bulb 0.3.4-0.3.7). Restore it on every return to the network. Hold the lock. */
/* Which On/Off reporting entry the stack would hand back at this point; NULL when there is none. */
static void log_onoff_reporting(const char *when)
{
    ESP_LOGI(TAG, "ZB-REPORT On/Off entry %s: %p", when,
             ezb_zcl_reporting_info_find(EP, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                                         EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE));
}

static bool restore_reporting(uint16_t min_s, uint16_t max_s)
{
    ezb_zcl_reporting_info_t info = ezb_zcl_reporting_info_find(EP, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                                                                EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE);
    if (info == EZB_ZCL_INVALID_REPORTING_INFO) {
        ESP_LOGW(TAG, "ZB-REPORT no On/Off reporting info on endpoint %d", EP);
        return false;
    }
    ezb_zcl_attr_variable_t delta = {0};          /* NULL here crashed the stack (0.3.8) */
    ezb_err_t e1 = ezb_zcl_reporting_info_update(info, min_s, max_s, &delta);
    ezb_err_t e2 = ezb_zcl_reporting_start_attr_report(info);
    ESP_LOGI(TAG, "ZB-REPORT On/Off reporting %u-%u s (%p): update %d, start %d", min_s, max_s, info, e1, e2);
    return e1 == EZB_ERR_NONE && e2 == EZB_ERR_NONE;
}

/* On a join and on a rejoin: the one On/Off entry the Zigbee controller needs, and the result for /state. */
static void reporting_on(void)
{
    s_reporting_ok = restore_reporting(1, 20);
}

/* The stack keeps reporting entries as a list in its datasets (one item per entry) and loads them at
 * every boot. Log what is there, so a duplicate shows as more than one On/Off item. */
static void log_reporting_dataset(void)
{
    int i = 0;
    for (; i < 16; i++) {
        uint8_t buf[64];
        uint16_t len = sizeof buf;
        ezb_err_t err = ezb_plat_datasets_get(EZB_DATASETS_KEY_ZCL_REPORTING_INFO, i, buf, &len);
        if (err != EZB_ERR_NONE) {
            if (err != EZB_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "ZB-DATASET reporting item %d: error %d", i, err);
            }
            break;
        }
        char hex[2 * sizeof buf + 1];
        for (int j = 0; j < MIN(len, sizeof buf); j++) {
            snprintf(hex + 2 * j, 3, "%02X", buf[j]);
        }
        ESP_LOGI(TAG, "ZB-DATASET reporting item %d, %u bytes: %s", i, len, hex);
    }
    ESP_LOGI(TAG, "ZB-DATASET reporting: %d item(s)", i);
}

/* After a power-up the bulb once sent three On/Off reports every 20 s with one binding, and
 * one stored On/Off item: loading the stored list made duplicates in memory (removing them over HTTP
 * held only until the next boot). restore_reporting() sets the one entry the Zigbee controller needs on every
 * return to the network anyway, so start without the stored list. */
static void forget_stored_reporting(void)
{
    ezb_err_t err = ezb_plat_datasets_delete(EZB_DATASETS_KEY_ZCL_REPORTING_INFO, -1);
    ESP_LOGI(TAG, "ZB-DATASET reporting: stored items deleted before start (%d)", err);
}

/* ---------------------------------------------------------------- signals */

static void log_network(const char *what)
{
    ezb_extpanid_t ext;
    ezb_nwk_get_extended_panid(&ext);
    ESP_LOGI(TAG, "%s: PAN 0x%04X ext %016llX channel %d, our short 0x%04X", what, ezb_nwk_get_panid(),
             (unsigned long long)ext.u64, ezb_nwk_get_current_channel(), ezb_get_short_address());
}

/* The wait before the next steer, `since_us` after the fast phase began: STEER_FAST_MS for the first
 * STEER_FAST_FOR_US, then STEER_SLOW_MS. Pure. */
static uint32_t steer_retry_ms(int64_t since_us)
{
    return since_us < STEER_FAST_FOR_US ? STEER_FAST_MS : STEER_SLOW_MS;
}

/* The fast phase begins now: boot, a leave, POST /api/v1/zigbee/pair. */
static void steer_fast_from(int64_t now)
{
    portENTER_CRITICAL(&s_light_mux);
    s_steer_fast_us = now;
    portEXIT_CRITICAL(&s_light_mux);
}

/* Arm the next steer, steer_retry_ms() from `now`; replaces one already armed. Any context. */
static void steer_later(int64_t now)
{
    portENTER_CRITICAL(&s_light_mux);
    int64_t since = now - s_steer_fast_us;
    portEXIT_CRITICAL(&s_light_mux);
    uint32_t ms = steer_retry_ms(since);
    if (!s_steer_timer) {
        return;
    }
    esp_timer_stop(s_steer_timer);                /* ESP_ERR_INVALID_STATE when none is armed: nothing to stop */
    esp_timer_start_once(s_steer_timer, (uint64_t)ms * 1000);
    ESP_LOGI(TAG, "ZB-JOIN next attempt in %lu s", (unsigned long)(ms / 1000));
}

/* Steer now; hold the Zigbee lock. An armed attempt is dropped: the result signal arms the next one. A
 * request the stack refuses is just the next attempt. */
static void steer_now_locked(int64_t now)
{
    if (s_steer_timer) {
        esp_timer_stop(s_steer_timer);
    }
    if (c6_zb_commission(EZB_BDB_MODE_NETWORK_STEERING) != EZB_ERR_NONE) {
        steer_later(now);
    }
}

/* The armed attempt, from the esp_timer task: steer only a board that is not joined and not rejoining its
 * stored network. Not "factory new": the stack clears that once a steer has run, so a failed attempt
 * would end the search (0.5.1). Only a failed steer, a leave and pair arm the timer; a join stops it
 * and ends the search. Lock busy: this attempt becomes the next one, while the search is still on. */
static void steer_timer_cb(void *arg)
{
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        if (s_zb_searching) {
            steer_later(esp_timer_get_time());
        }
        return;
    }
    if (!ezb_bdb_dev_joined() && !s_zb_rejoining) {
        steer_now_locked(esp_timer_get_time());
    }
    esp_zigbee_lock_release();
}

static bool on_signal(const ezb_app_signal_t *sig)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(sig);
    const void *params = ezb_app_signal_get_params(sig);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        c6_zb_commission(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t st = *(const ezb_bdb_comm_status_t *)params;
        if (st != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "%s failed (0x%02X); retrying in 5 s", ezb_app_signal_to_string(type), st);
            if (type == EZB_BDB_SIGNAL_DEVICE_REBOOT && !ezb_bdb_is_factory_new()) {
                s_zb_rejoining = true;            /* its stored network did not answer: pair says so */
                s_zb_started = true;
            }
            c6_zb_commission_after(EZB_BDB_MODE_INITIALIZATION, 5000);
            break;
        }
        s_zb_started = true;
        s_zb_rejoining = false;
        if (ezb_bdb_is_factory_new()) {
            int64_t now = esp_timer_get_time();
            s_zb_searching = true;
            s_restore_guard = 0;                  /* a fresh join has nothing to restore */
            ESP_LOGI(TAG, "factory new: steering now, then every %d s for %d min, then every %d s, until joined",
                     STEER_FAST_MS / 1000, (int)(STEER_FAST_FOR_US / (60 * 1000000LL)), STEER_SLOW_MS / 1000);
            steer_fast_from(now);
            steer_now_locked(now);
            c6_base_zigbee_ready();               /* factory new: nothing to rejoin, the check must not wait (0.6.0) */
        } else {
            s_zb_searching = false;
            log_network("back on the network");
            log_onoff_reporting("on rejoin");
            light_restore();
            reporting_on();
            ESP_LOGI(TAG, "NWK outgoing frame counter restored as %lu", (unsigned long)ezb_nwk_get_frame_counter());
            state_mark(ST_ZIGBEE);
            c6_base_zigbee_ready();               /* rejoined: the first-start check's Zigbee half (0.6.0) */
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t st = *(const ezb_bdb_comm_status_t *)params;
        if (st == EZB_BDB_STATUS_SUCCESS) {
            s_zb_searching = false;               /* first: a lock-busy attempt must not re-arm */
            s_zb_started = true;
            s_zb_rejoining = false;
            if (s_steer_timer) {
                esp_timer_stop(s_steer_timer);    /* joined: no further attempt */
            }
            log_network("joined");
            reporting_on();                       /* as on a rejoin; until 0.5.1 only after a reboot */
            ESP_LOGI(TAG, "NWK outgoing frame counter %lu", (unsigned long)ezb_nwk_get_frame_counter());
            state_mark(ST_ZIGBEE);
            c6_base_zigbee_ready();               /* joined: the first-start check's Zigbee half (0.6.0) */
        } else {
            ESP_LOGW(TAG, "steering failed (0x%02X)", st);
            steer_later(esp_timer_get_time());    /* the timer steers only a board not joined */
        }
    } break;
    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *p = params;
        if (p->leave_type == EZB_ZDO_LEAVE_TYPE_RESET) {
            int64_t now = esp_timer_get_time();
            ESP_LOGW(TAG, "left the network (leave type %u): group cleared; looking for a network again",
                     p->leave_type);
            s_reporting_ok = false;
            s_zb_rejoining = false;               /* no stored network any more */
            s_zb_searching = true;
            group_clear();
            steer_fast_from(now);
            steer_later(now);
        } else {
            ESP_LOGW(TAG, "left the network (leave type %u, rejoin): the stack rejoins", p->leave_type);
            s_zb_rejoining = true;                /* until a join signal; pair and the timer leave it alone */
        }
        state_mark(ST_ZIGBEE);
    } break;
    default:
        ESP_LOGI(TAG, "signal %s (0x%04X)", ezb_app_signal_to_string(type), type);
        break;
    }
    return true;
}

/* ---------------------------------------------------------------- Zigbee task */

/* The IEEE address a board takes when CONFIG_C6_ZB_IEEE is empty (0.4.0): Sengled's OUI B0:CE:18, then
 * C6 00, then the last three bytes of the chip's base MAC, MSB first. For example, a base MAC of
 * AA:BB:CC:11:22:33 gives B0CE18C600112233. */
#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
static void ieee_from_mac(const uint8_t mac[6], uint8_t out[8])
{
    out[0] = 0xB0;
    out[1] = 0xCE;
    out[2] = 0x18;
    out[3] = 0xC6;
    out[4] = 0x00;
    out[5] = mac[3];
    out[6] = mac[4];
    out[7] = mac[5];
}
#endif

/* The join filter, set before esp_zigbee_start (0.5.0): both channel sets from `mask`, and,
 * when `ext_pan` is not empty, the only extended PAN ID to join (16 hex, MSB first: the ext_pan setting). A
 * pin that does not parse stops the boot, as the Kconfig one did in 0.4.0. */
static void zb_join_filter(uint32_t mask, const char *ext_pan)
{
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(mask));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(mask));
    if (ext_pan[0]) {
        ezb_extpanid_t ext;
        ESP_ERROR_CHECK(c6_zb_parse_eui64(ext_pan, &ext) ? ESP_OK : ESP_ERR_INVALID_ARG);
        ezb_set_use_extended_panid(&ext);
        ESP_LOGI(TAG, "will only join ext PAN %016llX (the ext_pan setting)", (unsigned long long)ext.u64);
    } else {
        ESP_LOGI(TAG, "will join any ext PAN (the ext_pan setting is empty)");
    }
}

static void zb_task(void *arg)
{
    esp_zigbee_config_t cfg = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config = {
                .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,
                .keep_alive = 3000,
            },
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    {
        /* Set the chip's 802.15.4 MAC itself, before the stack reads it. ezb_set_extended_address()
         * after init changed only the stack's idea of it: the radio kept the chip's own address,
         * and the join never completed (associate, no key, leave, every 10 s).
         * CONFIG_C6_ZB_IEEE overrides; empty, the address is the chip's own (0.7.0), or under the E11
         * profile derived from the base MAC (0.4.0). */
        uint8_t mac[8];
        const char *from;
        if (CONFIG_C6_ZB_IEEE[0]) {
            ezb_extaddr_t ieee;
            ESP_ERROR_CHECK(c6_zb_parse_eui64(CONFIG_C6_ZB_IEEE, &ieee) ? ESP_OK : ESP_ERR_INVALID_ARG);
            for (int i = 0; i < 8; i++) {
                mac[i] = ieee.u64 >> (56 - 8 * i);     /* MSB first, as esp_read_mac() returns it */
            }
            from = "from Kconfig";
        } else {
#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
            uint8_t base[6];
            ESP_ERROR_CHECK(esp_read_mac(base, ESP_MAC_BASE));
            ieee_from_mac(base, mac);
            from = "derived from the MAC (E11)";
#else
            ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_IEEE802154));
            from = "chip's own";
#endif
        }
        ESP_ERROR_CHECK(esp_iface_mac_addr_set(mac, ESP_MAC_IEEE802154));
        ESP_LOGI(TAG, "IEEE %02X%02X%02X%02X%02X%02X%02X%02X %s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 mac[6], mac[7], from);
    }
    group_load();
    {
        const esp_timer_create_args_t t = {.callback = steer_timer_cb, .name = "zb_steer"};
        ESP_ERROR_CHECK(esp_timer_create(&t, &s_steer_timer));
    }
    ESP_ERROR_CHECK(esp_zigbee_init(&cfg));
    s_zb_up = true;
    log_onoff_reporting("after init");

    ezb_aps_secur_enable_distributed_security(false);
    char ext_pan[17];
    cfg_ext_pan(ext_pan);                         /* the setting, read once: a change applies at the next boot */
    zb_join_filter(CONFIG_C6_ZB_CHANNEL_MASK, ext_pan);
    if (CONFIG_C6_ZB_INSTALL_CODE[0]) {
        uint8_t ic[18];
        ESP_ERROR_CHECK(c6_zb_parse_hex(CONFIG_C6_ZB_INSTALL_CODE, ic, sizeof ic) == 18 ? ESP_OK
                                                                                       : ESP_ERR_INVALID_ARG);
        ESP_ERROR_CHECK(ezb_secur_ic_set(EZB_SECUR_IC_TYPE_128, ic));
        ESP_LOGI(TAG, "install code set (128-bit, CRC %02X%02X)", ic[16], ic[17]);
    }
    ezb_nwk_set_rx_on_when_idle(true);
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(on_signal));
    c6_zb_install_rx_logger();
    c6_zb_set_rx_tap(hub_tap);
    /* Not c6_zb_install_zcl_logger(): with the raw ZCL hook registered (bulb 0.3.4-0.3.5) the Zigbee controller's
     * report acknowledgements stopped arriving at all. */
    create_device();
    log_onoff_reporting("after the endpoint is registered");
    log_reporting_dataset();
    forget_stored_reporting();

    ezb_extaddr_t ieee;
    char ieee_s[20];
    ezb_get_extended_address(&ieee);
    c6_zb_eui64_str(&ieee, ieee_s, sizeof ieee_s);
    ESP_LOGI(TAG, "IEEE %s, channel mask 0x%08lX", ieee_s, (unsigned long)CONFIG_C6_ZB_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- HTTP */

static esp_err_t zb_get(httpd_req_t *req)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, "{\"role\":\"%s\",", cfg_role_name(cfg_role()));
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    n += c6_zb_status_json(buf + n, sizeof buf - n);
    esp_zigbee_lock_release();
    light_state_t l;
    bool unsaved;
    uint16_t group;
    portENTER_CRITICAL(&s_light_mux);
    l = s_light;
    unsaved = s_light_gen != s_saved_gen;
    group = s_group;
    portEXIT_CRITICAL(&s_light_mux);
    char gs[16] = "null";                         /* "0x000A" in quotes, or null (0.5.0) */
    if (group) {
        snprintf(gs, sizeof gs, "\"0x%04X\"", (unsigned)group);
    }
    n += snprintf(buf + n, sizeof buf - n, ",\"group\":%s,\"light\":{", gs);
    for (int i = 0; i < L_COUNT && n < (int)sizeof buf; i++) {
        n += (l.seen & (1u << i)) ? snprintf(buf + n, sizeof buf - n, "%s\"%s\":%u", i ? "," : "", LIGHT_ATTRS[i].name, l.val[i])
                                  : snprintf(buf + n, sizeof buf - n, "%s\"%s\":null", i ? "," : "", LIGHT_ATTRS[i].name);
    }
    if (n < (int)sizeof buf) {
        snprintf(buf + n, sizeof buf - n, "},\"unsaved\":%s}\n", unsaved ? "true" : "false");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* The Zigbee half of the state object (api/v1/state.h). The stack's fields are read under the
 * Zigbee lock, waited for at most 100 ms; when it is busy or the stack is not up they are the last copy's.
 * The group, last_rx_us, reporting, hue and profile are always current. */
void bulb_zigbee_snapshot(zb_snapshot_t *out)
{
    static zb_snapshot_t s_last;                  /* the stack's fields as last read (s_light_mux) */
    zb_snapshot_t z;
    bool fresh = false;
    memset(&z, 0, sizeof z);
    if (s_zb_up && esp_zigbee_lock_acquire(pdMS_TO_TICKS(100))) {
        ezb_extaddr_t ieee;
        ezb_get_extended_address(&ieee);
        c6_zb_eui64_str(&ieee, z.ieee, sizeof z.ieee);
        z.joined = ezb_bdb_dev_joined();
        if (z.joined) {
            ezb_extpanid_t ext;
            ezb_nwk_get_extended_panid(&ext);
            snprintf(z.ext_pan, sizeof z.ext_pan, "%016llX", (unsigned long long)ext.u64);
            z.short_addr = ezb_get_short_address();
            z.pan = ezb_nwk_get_panid();
            z.channel = (uint8_t)ezb_nwk_get_current_channel();
        }
        esp_zigbee_lock_release();
        fresh = true;
    }
    portENTER_CRITICAL(&s_light_mux);
    if (fresh) {
        s_last = z;
    } else {
        z = s_last;
    }
    z.group = s_group;
    portEXIT_CRITICAL(&s_light_mux);
    uint32_t aps, zcl;
    int64_t last;
    c6_zb_rx_stats(&aps, &zcl, &last);
    z.last_rx_us = last;
    z.reporting = s_reporting_ok;
#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
    z.profile = "sengled_e11";
#else
    z.profile = "espressif";
#endif
    z.hue = cfg_hue_boot();                       /* the hue answer the stack started with: what the Zigbee controller can see */
    *out = z;
}

/* The light and controller halves of the state object (api/v1/state.h), copied in one section. */
void bulb_light_snapshot(bulb_light_snap_t *out)
{
    portENTER_CRITICAL(&s_light_mux);
    out->seen = s_light.seen;
    memcpy(out->val, s_light.val, sizeof out->val);
    out->by = s_light_by;
    out->at_us = s_light_at_us;
    out->unsaved = s_light_gen != s_saved_gen;
    out->hub_seen = s_hub.seen;
    memcpy(out->hub_val, s_hub.val, sizeof out->hub_val);
    out->hub_seq = s_hub_seq;
    out->hub_at_us = s_hub_cmd_us;
    portEXIT_CRITICAL(&s_light_mux);
}

/* The button half of the state object, copied in one section. */
void bulb_button_snapshot(bulb_button_snap_t *out)
{
    portENTER_CRITICAL(&s_light_mux);
    out->held = s_held;
    out->press = s_presses;
    out->release_by = s_release_last;
    out->at_us = s_button_at_us;
    portEXIT_CRITICAL(&s_light_mux);
}

static void binds_cb(const ezb_zdo_nwk_mgmt_bind_req_result_t *r, void *ctx)
{
    if (r->error != EZB_ERR_NONE || !r->rsp) {
        ESP_LOGW(TAG, "ZB-BIND request failed: %d", r->error);
        return;
    }
    const ezb_zdp_nwk_mgmt_bind_rsp_field_t *p = r->rsp;
    ESP_LOGI(TAG, "ZB-BIND status 0x%02X: %u entries, %u from index %u", p->status, p->binding_table_entries,
             p->binding_table_list_count, p->start_index);
    for (int i = 0; i < p->binding_table_list_count; i++) {
        const ezb_zdp_nwk_mgmt_bind_table_entry_t *e = &p->binding_table_list[i];
        char src[20], dst[24];
        c6_zb_eui64_str(&e->src_addr, src, sizeof src);
        if (e->dst_addr_mode == 0x01) {
            snprintf(dst, sizeof dst, "group:0x%04X", (unsigned)e->dst_addr.group_addr.group);
        } else {
            c6_zb_eui64_str(&e->dst_addr.extended_addr, dst, sizeof dst);
        }
        ESP_LOGI(TAG, "ZB-BIND %u: %s ep %u cluster 0x%04X -> mode %u %s ep %u", p->start_index + i, src, e->src_ep,
                 e->cluster_id, e->dst_addr_mode, dst, e->dst_ep);
    }
    uint16_t next = (uint16_t)p->start_index + p->binding_table_list_count;
    if (p->status == 0 && p->binding_table_list_count && next > p->start_index && next < p->binding_table_entries
        && next <= 0xFF) {                        /* stop on a page that does not advance */
        ezb_zdo_nwk_mgmt_bind_req_t req = {.dst_nwk_addr = ezb_get_short_address(), .field = {.start_index = next},
                                           .cb = binds_cb};
        ezb_zdo_nwk_mgmt_bind_req(&req);
    }
}

static esp_err_t binds_get(httpd_req_t *req)
{
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    ezb_zdo_nwk_mgmt_bind_req_t r = {.dst_nwk_addr = ezb_get_short_address(), .field = {.start_index = 0},
                                     .cb = binds_cb};
    ezb_err_t err = ezb_zdo_nwk_mgmt_bind_req(&r);
    esp_zigbee_lock_release();
    return httpd_resp_sendstr(req, err == EZB_ERR_NONE ? "ok: see ZB-BIND in /log\n" : "Mgmt_Bind_req refused\n");
}

static esp_err_t steer_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    ezb_err_t err = c6_zb_commission(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "steering requested over HTTP: %d", err);
    return httpd_resp_sendstr(req, err == EZB_ERR_NONE ? "ok: steering\n" : "steering refused\n");
}

static esp_err_t leave_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    ESP_LOGW(TAG, "leave requested over HTTP: local reset");
    ezb_bdb_reset_via_local_action();
    esp_zigbee_lock_release();
    return httpd_resp_sendstr(req, "ok: leaving\n");
}

#if CONFIG_C6_ZB_KEY_ENDPOINT
static esp_err_t key_get(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    uint8_t key[16] = {0};
    ezb_err_t err = ezb_secur_get_network_key(key);
    esp_zigbee_lock_release();
    if (err != EZB_ERR_NONE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no network key");
    }
    char hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(hex + 2 * i, 3, "%02x", key[i]);
    }
    ESP_LOGI(TAG, "network key read over HTTP");
    esp_err_t ret = httpd_resp_sendstr(req, hex);
    memset(key, 0, sizeof key);
    memset(hex, 0, sizeof hex);
    return ret;
}
#endif

static void beacon_cb(ezb_nwk_active_scan_result_t *r, void *ctx)
{
    if (!r) {
        ESP_LOGI(TAG, "ZB-BEACON scan finished");
        return;
    }
    ESP_LOGI(TAG, "ZB-BEACON channel %u pan 0x%04X ext %016llX from 0x%04X permit_join %d router_cap %d enddev_cap %d "
             "stack_profile %u protocol %u update_id %u", r->channel_number, r->panid,
             (unsigned long long)r->extpanid.u64, r->shortaddr, r->permit_join, r->router_capacity,
             r->enddev_capacity, r->stack_profile, r->protocol_version, r->nwk_update_id);
}

static void ed_cb(ezb_nwk_ed_scan_result_t *r, void *ctx)
{
    if (!r) {
        ESP_LOGI(TAG, "ZB-ENERGY scan finished");
        return;
    }
    ESP_LOGI(TAG, "ZB-ENERGY channel %u max_rssi %d", r->channel_number, r->max_rssi);
}

static esp_err_t scan_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    char v[8];
    uint8_t dur = c6_base_query(req, "dur", v, sizeof v) ? (uint8_t)atoi(v) : 5;   /* 5: ~0.5 s per channel */
    char m[16];
    uint32_t mask = c6_base_query(req, "mask", m, sizeof m) ? strtoul(m, NULL, 0) : CONFIG_C6_ZB_CHANNEL_MASK;
    bool energy = c6_base_query(req, "type", v, sizeof v) && !strcmp(v, "ed");
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    ezb_nwk_scan_req_t sr = {
        .scan_type = energy ? EZB_NWK_SCAN_TYPE_ED : EZB_NWK_SCAN_TYPE_ACTIVE,
        .scan_duration = dur,
        .scan_channels = mask,
    };
    if (energy) {
        sr.ed_scan_cb = ed_cb;
    } else {
        sr.active_scan_cb = beacon_cb;
    }
    ezb_err_t err = ezb_nwk_scan(&sr);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "%s scan requested over HTTP (duration %u, mask 0x%08lX): %d", energy ? "energy" : "active",
             dur, (unsigned long)mask, err);
    return httpd_resp_sendstr(req, err == EZB_ERR_NONE ? "ok: scanning; see ZB-BEACON in /log\n" : "scan refused\n");
}

static void log_tasks(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *st = malloc(n * sizeof *st);
    if (!st) {
        return;
    }
    n = uxTaskGetSystemState(st, n, NULL);
    static const char *states = "RrBSDI";      /* running ready blocked suspended deleted invalid */
    for (UBaseType_t i = 0; i < n; i++) {
        ESP_LOGW(TAG, "ZB-STALL task %-16s state %c prio %u stack_free %lu", st[i].pcTaskName,
                 st[i].eCurrentState < 6 ? states[st[i].eCurrentState] : '?', (unsigned)st[i].uxCurrentPriority,
                 (unsigned long)st[i].usStackHighWaterMark);
    }
    free(st);
}

static void watchdog_task(void *arg)
{
    if (s_stall_magic != STALL_MAGIC || esp_reset_reason() != ESP_RST_SW) {
        s_stall_magic = STALL_MAGIC;
        s_stall_reboots = 0;
    }
    uint32_t limit_s = cfg_rx_wd_s() << MIN(s_stall_reboots, 4);   /* the rx_wd_s setting, read each pass */
    ESP_LOGI(TAG, "rx watchdog: %lu s of silence (after %lu stall reboots in a row)", (unsigned long)limit_s,
             (unsigned long)s_stall_reboots);
    int64_t next_health = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t aps, zcl;
        int64_t last, now = esp_timer_get_time();
        portENTER_CRITICAL(&s_light_mux);
        bool survived = s_restored_at_us && now - s_restored_at_us > RESTORE_GUARD_US;
        if (survived) {
            s_restored_at_us = 0;
        }
        portEXIT_CRITICAL(&s_light_mux);
        if (survived) {
            s_restore_guard = 0;                      /* this boot survived its restore */
        }
        light_save_if_settled(now);
        group_save_if_dirty();
        c6_zb_rx_stats(&aps, &zcl, &last);
        float age = last ? (now - last) / 1e6 : -1;
        if (now >= next_health) {
            next_health = now + 60 * 1000000LL;
            ESP_LOGI(TAG, "ZB-HEALTH aps=%lu zcl=%lu set=%lu last_rx_age=%.1f s heap=%lu min_heap=%lu "
                     "wd_stack_free=%lu",
                     (unsigned long)aps, (unsigned long)zcl, (unsigned long)s_set_count, age,
                     (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
        }
        if (s_stall_reboots && now > 30 * 60 * 1000000LL) {
            s_stall_reboots = 0;                      /* 30 healthy minutes */
        }
        limit_s = cfg_rx_wd_s() << MIN(s_stall_reboots, 4);
        if (!limit_s || !last || age < limit_s) {
            continue;                                 /* off (0), or not armed until the network has spoken */
        }
        bool lock = esp_zigbee_lock_acquire(pdMS_TO_TICKS(2000));
        bool joined = lock && ezb_bdb_dev_joined();
        if (lock) {
            esp_zigbee_lock_release();
        }
        ESP_LOGW(TAG, "ZB-STALL nothing received for %.1f s (aps=%lu zcl=%lu set=%lu, lock %s, joined %d); "
                 "rebooting in 3 s", age, (unsigned long)aps, (unsigned long)zcl, (unsigned long)s_set_count,
                 lock ? "free" : "HELD", joined);
        log_tasks();
        s_stall_reboots++;
        s_restart_for_stall = true;                   /* a stall right after a restore keeps its guard */
        vTaskDelay(pdMS_TO_TICKS(3000));              /* time for the watcher to copy the log */
        esp_restart();
    }
}

static void erase_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "erasing NVS (all Zigbee state) and rebooting");
    nvs_flash_erase();
    esp_restart();
}

static esp_err_t report_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    char v[8];
    uint16_t mn = c6_base_query(req, "min", v, sizeof v) ? atoi(v) : 1;
    uint16_t mx = c6_base_query(req, "max", v, sizeof v) ? atoi(v) : 20;
    /* remove=N (0.3.11) dropped duplicate On/Off reporting entries, gone since 0.3.13. It could also drop
     * the last one, and the library has no call to create one again, so the Zigbee controller would hear nothing and
     * write the bulb off: withdrawn in 0.3.24. */
    if (c6_base_query(req, "remove", v, sizeof v)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "remove= was withdrawn in 0.3.24");
    }
    if (mx == 0 || mn > mx) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need 0 <= min <= max, max >= 1");
    }
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    bool ok = restore_reporting(mn, mx);
    esp_zigbee_lock_release();
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "On/Off reporting entry missing or not updated; see /log");
    }
    return httpd_resp_sendstr(req, "ok: see ZB-REPORT in /log\n");
}

static esp_err_t fc_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    char v[16];
    uint32_t add = c6_base_query(req, "add", v, sizeof v) ? strtoul(v, NULL, 0) : 0;
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    uint32_t before = ezb_nwk_get_frame_counter();
    ezb_err_t err = add ? ezb_nwk_set_frame_counter(before + add) : EZB_ERR_NONE;
    uint32_t after = ezb_nwk_get_frame_counter();
    esp_zigbee_lock_release();
    ESP_LOGW(TAG, "NWK outgoing frame counter %lu -> %lu over HTTP (err %d)", (unsigned long)before,
             (unsigned long)after, err);
    char msg[64];
    snprintf(msg, sizeof msg, "frame counter %lu -> %lu\n", (unsigned long)before, (unsigned long)after);
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t erase_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "ok: erasing NVS and rebooting\n");
    xTaskCreate(erase_task, "erase", 3072, NULL, 5, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------------------- /api/v1/zigbee (0.5.0) */

/* True if `ns` is one of keep[0..nkeep). */
static bool nvs_ns_kept(const char *ns, const char *const keep[], size_t nkeep)
{
    for (size_t i = 0; i < nkeep; i++) {
        if (strcmp(ns, keep[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* nvs_erase_all on every namespace of the default NVS partition but those in keep[]. The namespaces are
 * collected first and the iterator released before anything is erased; then each one is erased and
 * committed. ESP_OK only if every one was; more than ZB_ERASE_MAX_NS namespaces, or a walk that fails,
 * erases nothing. */
static esp_err_t nvs_erase_except(const char *const keep[], size_t nkeep)
{
    static char names[ZB_ERASE_MAX_NS][NVS_NS_NAME_MAX_SIZE];   /* the server runs one handler at a time */
    size_t count = 0;
    bool full = false;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        err = nvs_entry_info(it, &info);
        if (err != ESP_OK) {
            break;
        }
        info.namespace_name[sizeof info.namespace_name - 1] = 0;
        bool seen = nvs_ns_kept(info.namespace_name, keep, nkeep);
        for (size_t i = 0; i < count && !seen; i++) {
            seen = strcmp(names[i], info.namespace_name) == 0;
        }
        if (!seen && count == ZB_ERASE_MAX_NS) {
            full = true;
            break;
        }
        if (!seen) {
            snprintf(names[count], sizeof names[count], "%s", info.namespace_name);
            count++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);                     /* NULL at the end of the walk: allowed */
    if (full) {
        ESP_LOGE(TAG, "ZB-ERASE more than %d namespaces; nothing erased", ZB_ERASE_MAX_NS);
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "ZB-ERASE the namespace walk failed (%s); nothing erased", esp_err_to_name(err));
        return err == ESP_OK ? ESP_FAIL : err;
    }
    esp_err_t res = ESP_OK;
    for (size_t i = 0; i < count; i++) {
        nvs_handle_t h;
        esp_err_t e = nvs_open(names[i], NVS_READWRITE, &h);
        if (e == ESP_OK) {
            e = nvs_erase_all(h);
            if (e == ESP_OK) {
                e = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ZB-ERASE %s: %s", names[i], esp_err_to_name(e));
            res = e;
        } else {
            ESP_LOGI(TAG, "ZB-ERASE %s erased", names[i]);
        }
    }
    return res;
}

static void zigbee_reboot_task(void *arg)
{
    events_close();                               /* the stream ends before the reboot, as for /reboot */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "ZB-ERASE done: rebooting");
    esp_restart();
}

/* POST /api/v1/zigbee/pair: a board not on a network restarts the fast phase and steers now. Not gated on
 * factory new, which the stack clears after the first steer (0.5.1). 503 until the stack has answered
 * its start; 409 already_joined on a network; 409 rejoining while it rejoins its stored network (one that
 * never answers again needs /zigbee/erase). */
static esp_err_t zigbee_pair_post(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    if (!s_zb_up || !s_zb_started || s_zb_erasing || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return c6_json_error(req, 503, "zigbee_busy", "the Zigbee stack is not up or is busy; try again");
    }
    if (ezb_bdb_dev_joined()) {
        esp_zigbee_lock_release();
        return c6_json_error(req, 409, "already_joined", "the board is on a network; leave or erase it first");
    }
    if (s_zb_rejoining) {
        esp_zigbee_lock_release();
        return c6_json_error(req, 409, "rejoining",
                             "the board is rejoining its stored network; to pair it with another, erase it first");
    }
    int64_t now = esp_timer_get_time();
    s_zb_searching = true;
    steer_fast_from(now);
    steer_now_locked(now);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "ZB-JOIN pair requested over HTTP: steering now, fast phase restarted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* POST /api/v1/zigbee/leave: as /zb/leave, and the group is cleared. The leave signal restarts the search. */
static esp_err_t zigbee_leave_post(httpd_req_t *req)
{
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    if (!s_zb_up || s_zb_erasing || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return c6_json_error(req, 503, "zigbee_busy", "the Zigbee stack is not up or is busy; try again");
    }
    ESP_LOGW(TAG, "leave requested over HTTP (/api/v1/zigbee/leave): local reset, group cleared");
    ezb_bdb_reset_via_local_action();
    esp_zigbee_lock_release();
    group_clear();
    state_mark(ST_ZIGBEE);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* POST /api/v1/zigbee/erase: every NVS namespace but the settings, the claim and the Wi-Fi,
 * so the board comes back factory new; 200, then a reboot 500 ms later. The Zigbee lock, when the stack is
 * up, is held from before the erase until the reboot, so the stack cannot write its storage back. */
static esp_err_t zigbee_erase_post(httpd_req_t *req)
{
    static const char *const keep[] = {"c6cfg", "c6claim", "c6wifi"};
    if (!c6_auth_ok(req, C6_AUTH_TOKEN)) {
        return ESP_OK;
    }
    if (s_zb_erasing) {
        return c6_json_error(req, 503, "zigbee_busy", "an erase is already under way");
    }
    bool locked = false;
    if (s_zb_up) {
        if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
            return c6_json_error(req, 503, "zigbee_busy", "the Zigbee stack is busy; try again");
        }
        locked = true;
    }
    portENTER_CRITICAL(&s_light_mux);
    bool saving = s_zb_saving;
    if (!saving) {
        s_zb_erasing = true;                      /* atomically exclude saves before the group is cleared */
        s_group = 0;
        s_group_dirty = false;
    }
    portEXIT_CRITICAL(&s_light_mux);
    if (saving) {
        if (locked) {
            esp_zigbee_lock_release();
        }
        return c6_json_error(req, 503, "zigbee_busy", "the light or group is being saved; try again");
    }
    ESP_LOGW(TAG, "ZB-ERASE every NVS namespace but c6cfg, c6claim and c6wifi (/api/v1/zigbee/erase)");
    esp_err_t err = nvs_erase_except(keep, sizeof keep / sizeof keep[0]);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_light_mux);
        s_group_dirty = true;                     /* the group stays cleared; store that */
        s_zb_erasing = false;
        portEXIT_CRITICAL(&s_light_mux);
        if (locked) {
            esp_zigbee_lock_release();
        }
        return c6_json_error(req, 500, "storage", "the erase did not complete; not rebooting, try again");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    if (xTaskCreate(zigbee_reboot_task, "reboot", 6144, NULL, 5, NULL) != pdPASS) {   /* 6144: it ends the event stream first (0.5.0; 6 KB since 0.7.0) */
        esp_restart();                            /* the Zigbee lock is still held: reboot now */
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------- Webhook */

static int light_json(const light_state_t *l, bool settled, char *buf, size_t n)
{
    int k = snprintf(buf, n, "{\"settled\":%s", settled ? "true" : "false");
    for (int i = 0; i < L_COUNT && k < (int)n; i++) {
        if ((l->seen & (1u << i)) && (settled || i == L_ON)) {
            k += snprintf(buf + k, n - k, ",\"%s\":%u", LIGHT_ATTRS[i].name, l->val[i]);
        }
    }
    if (k < (int)n) {
        k += snprintf(buf + k, n - k, "}");
    }
    return k;
}

/* One POST to the webhook; true on a 2xx. Best effort: the caller decides whether to try again. The
 * 3 s timeout is per network phase, not a bound on the whole call. */
static bool webhook_post_to(const char *json, const char *url)
{
    esp_http_client_config_t c = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        ESP_LOGW(TAG, "WEBHOOK %s: no client", json);
        return false;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, json, strlen(json));
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(h);
    int status = err == ESP_OK ? esp_http_client_get_status_code(h) : -1;
    esp_http_client_cleanup(h);
    ESP_LOGI(TAG, "WEBHOOK %s -> %s, HTTP %d, %lld ms", json, esp_err_to_name(err), status,
             (esp_timer_get_time() - t0) / 1000);
    return err == ESP_OK && status >= 200 && status < 300;
}

static bool webhook_post(const char *json)
{
    char url[CFG_WEBHOOK_MAX + 1];
    if (!cfg_webhook(url, sizeof url)) {          /* copied under the mux: a PUT may change it meanwhile */
        ESP_LOGI(TAG, "WEBHOOK skipped: no webhook");
        return false;
    }
    return webhook_post_to(json, url);
}

/* Check the originating role switch and copy its current URL together, before starting an HTTP call.
 * -1: canceled by a role switch, 0: failed or no webhook, 1: posted. */
static int button_post(const char *json, uint32_t generation)
{
    char url[CFG_WEBHOOK_MAX + 1];
    portENTER_CRITICAL(&s_light_mux);
    bool active = cfg_live.role == CFG_ROLE_SHORTCUT && s_button_generation == generation;
    memcpy(url, cfg_live.webhook, sizeof url);
    portEXIT_CRITICAL(&s_light_mux);
    if (!active) {
        return -1;
    }
    if (!url[0]) {
        ESP_LOGI(TAG, "WEBHOOK skipped: no webhook");
        return 0;
    }
    return webhook_post_to(json, url) ? 1 : 0;
}

/* Each Zigbee controller command is pushed as soon as it is decoded, with everything the Zigbee controller has asked for so far:
 * its target, not the stack's 1.5 s ramp. Commands the push task has not caught up with are merged. */
static void push_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        light_state_t l;
        int64_t due;
        portENTER_CRITICAL(&s_light_mux);
        due = s_push_due_us;
        s_push_due_us = 0;
        l = s_hub;
        bool mirror = cfg_live.role == CFG_ROLE_MIRROR;
        portEXIT_CRITICAL(&s_light_mux);
        if (!due || !mirror) {
            continue;                             /* only the mirror role pushes; webhook_post skips without a webhook */
        }
        char buf[200];
        light_json(&l, true, buf, sizeof buf);
        webhook_post(buf);
    }
}

/* An unsolicited ZCL Report Attributes to the Zigbee controller's coordinator, endpoint CONFIG_C6_ZB_REPORT_DST_EP,
 * built here and sent at the APS layer. The Zigbee controller configured reporting for On/Off only; this tells it Level and CT too, to see
 * whether it follows them. Hold the lock. */
static void report_to_hub(uint16_t cluster, uint16_t attr, uint8_t type, uint16_t value, uint8_t size)
{
    static uint8_t seq = 0x80;
    uint8_t f[8] = {0x08, seq++, 0x0A, attr & 0xFF, attr >> 8, type, value & 0xFF, value >> 8};
    ezb_apsde_data_req_t req = {
        .dst_address = {.addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = 0x0000},
        .src_endpoint = EP,
        .dst_endpoint = CONFIG_C6_ZB_REPORT_DST_EP,
        .cluster_id = cluster,
        .profile_id = 0x0104,
        .tx_options = 0,
        .asdu_length = (uint16_t)(6 + size),
        .asdu = f,
    };
    ezb_err_t err = ezb_apsde_data_request(&req);
    ESP_LOGI(TAG, "ZB-TX report cluster=0x%04X attr=0x%04X value=%u: %d", cluster, attr, value, err);
}

/* Set the light's attributes to want[] (-1: leave), as POST /zb/light or the button's own Off asks. A
 * change of On/Off reaches the Zigbee controller as a report, as for a real bulb switched at the wall. Writes what
 * it set into msg, after "set:". Returns the length written, -1 if the stack is not up, -2 if an
 * attribute could not be set (those that were set are recorded; msg says which), -3 if need_held and no
 * press is held. *released (may be NULL) says whether this call released a held press. The shared state
 * is updated before the Zigbee lock is released, so a Zigbee controller command the stack handles next is not
 * overwritten (0.3.24); hub_tap runs under that lock too, so s_held cannot change in between.
 * 0.4.0: the body is light_apply_locked, for a caller that already holds the Zigbee lock (bulb_role_switch);
 * it logs nothing, returns -3, -2 or the length, and leaves the light's state as set in *out. A release
 * is the shortcut role's, read with the light's state. 0.5.0: `who` (the caller's log name) gives the state
 * object's light.by and button.release_by (light_by_of, light_release_by_of). */
static int light_apply_locked(int want[L_COUNT], const char *who, char *msg, size_t msz, bool need_held,
                              bool *released, light_state_t *out)
{
    if (released) {
        *released = false;
    }
    if (need_held) {
        bool held;
        portENTER_CRITICAL(&s_light_mux);
        held = s_held;
        portEXIT_CRITICAL(&s_light_mux);
        if (!held) {                              /* released while this waited for the lock */
            return -3;
        }
    }
    light_state_t cur;
    portENTER_CRITICAL(&s_light_mux);
    cur = s_light;
    bool shortcut = cfg_live.role == CFG_ROLE_SHORTCUT;
    portEXIT_CRITICAL(&s_light_mux);
    size_t n = (size_t)snprintf(msg, msz, "set:");
    bool release = shortcut && want[L_ON] == 0;
    bool failed = false;
    for (int i = 0; i < L_COUNT; i++) {
        if (want[i] < 0 || ((cur.seen & (1u << i)) && cur.val[i] == want[i])) {
            want[i] = -1;
            continue;
        }
        uint8_t v8 = (uint8_t)want[i];
        uint16_t v16 = (uint16_t)want[i];
        void *pv = LIGHT_ATTRS[i].size == 1 ? (void *)&v8 : (void *)&v16;
        ezb_zcl_status_t st = ezb_zcl_set_attr_value(EP, LIGHT_ATTRS[i].cluster, EZB_ZCL_CLUSTER_SERVER,
                                                     LIGHT_ATTRS[i].attr, EZB_ZCL_STD_MANUF_CODE, pv, false);
        if (i == L_MODE && st == EZB_ZCL_STATUS_SUCCESS) {
            ezb_zcl_set_attr_value(EP, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_CLUSTER_SERVER,
                                   EZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID, EZB_ZCL_STD_MANUF_CODE, &v8, false);
        }
        if (n < msz) {
            n += (size_t)snprintf(msg + n, msz - n, " %s=%d(0x%02X)", LIGHT_ATTRS[i].name, want[i], st);
        }
        if (st != EZB_ZCL_STATUS_SUCCESS) {
            failed = true;
            want[i] = -1;                         /* not set: not recorded */
            if (i == L_ON) {
                release = false;                  /* still on: the press stays held */
            }
        }
    }
    /* Tried in 0.3.19: the tested Zigbee controller acknowledges these reports but its LIGHT_STS keeps
     * its own brightness and colour, and one second after a ColorMode/CT report it said the group OFF
     * (ON again 17 s later). Off unless CONFIG_C6_ZB_REPORT_LEVEL_CT. */
    if (CONFIG_C6_ZB_REPORT_LEVEL_CT && want[L_LEVEL] >= 0) {
        report_to_hub(EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID, 0x20, want[L_LEVEL], 1);
    }
    if (CONFIG_C6_ZB_REPORT_LEVEL_CT && want[L_CT] >= 0) {
        report_to_hub(EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, 0x30, 2, 1);
        report_to_hub(EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID, 0x21,
                      want[L_CT], 2);
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_light_mux);
    bool changed = false;
    for (int i = 0; i < L_COUNT; i++) {
        if (want[i] >= 0) {
            s_light.val[i] = (uint16_t)want[i];
            s_light.seen |= 1u << i;
            changed = true;
        }
    }
    if (changed) {
        light_mark_changed_locked(now, light_by_of(who));
    }
    *out = s_light;
    if (s_hub.seen) {
        bool hub_changed = false;
        for (int i = 0; i < L_COUNT; i++) {
            if (want[i] >= 0) {
                hub_changed |= !(s_hub.seen & (1u << i)) || s_hub.val[i] != want[i];
                s_hub.val[i] = (uint16_t)want[i];
                s_hub.seen |= 1u << i;
            }
        }
        if (hub_changed) {
            state_mark(ST_HUB);
        }
    }
    /* A release counts (and restarts the re-arm window) only if it ends a held press or actually turns
     * the light off: a no-op Off must not swallow the next press. */
    if (release && (s_held || want[L_ON] == 0)) {
        if (s_held && s_delivering) {
            s_release_by = "api";                 /* released before the client heard of the press: follow it */
        }
        if (released) {
            *released = s_held;
        }
        if (s_held && light_release_by_of(who)) {
            s_release_last = light_release_by_of(who);
            s_button_at_us = now;
            state_mark(ST_BUTTON);
        }
        s_press_at_us = s_off_at_us = 0;          /* released: nothing left to push or clear */
        s_held = false;
        s_held_at_us = 0;
        s_released_at_us = now;
    }
    portEXIT_CRITICAL(&s_light_mux);
    if (msz && n >= msz) {
        n = msz - 1;                              /* truncated */
    }
    return failed ? -2 : (int)n;
}

/* light_apply_locked under the Zigbee lock, logged as `who`; -1 if the stack is not up or the lock busy. */
static int light_apply(int want[L_COUNT], const char *who, char *msg, size_t msz, bool need_held, bool *released)
{
    if (released) {
        *released = false;
    }
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return -1;
    }
    light_state_t l = {.seen = 0};
    int r = light_apply_locked(want, who, msg, msz, need_held, released, &l);
    esp_zigbee_lock_release();
    if (r != -3) {
        ESP_LOGI(TAG, "%s %s%s", who, msg + 4, r == -2 ? " (not all set)" : "");
        log_light("LIGHT", &l);
    }
    return r;
}

/* A button Off decided before a wait may belong to a role which has since ended. Check again under
 * the Zigbee lock before touching attributes. -3 cancels it without a retry. */
static int button_light_apply(int want[L_COUNT], const char *who, char *msg, size_t msz,
                              bool was_held, bool *released, uint32_t generation)
{
    *released = false;
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return -1;
    }
    portENTER_CRITICAL(&s_light_mux);
    bool active = cfg_live.role == CFG_ROLE_SHORTCUT && s_button_generation == generation;
    if (active && was_held && s_held &&
        (!cfg_live.off_after_s || (s_off_at_us && s_off_at_us > esp_timer_get_time()))) {
        active = false;                           /* the live timeout was disabled or moved later */
    }
    portEXIT_CRITICAL(&s_light_mux);
    if (!active) {
        esp_zigbee_lock_release();
        return -3;
    }
    light_state_t l = {.seen = 0};
    int r = light_apply_locked(want, who, msg, msz, false, released, &l);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "%s %s%s", who, msg + 4, r == -2 ? " (not all set)" : "");
    log_light("LIGHT", &l);
    return r;
}

/* settings.c calls this after publishing under the mux. Keep the timeout relative to the held press,
 * including a press which began with the timeout disabled. */
void bulb_settings_changed_locked(uint32_t keys)
{
    if ((keys & CFG_K_OFF_AFTER_S) && s_held) {
        s_off_at_us = cfg_live.off_after_s ? s_held_at_us + (int64_t)cfg_live.off_after_s * 1000000 : 0;
    }
}

void bulb_settings_changed(void)
{
    if (s_button_task) {
        xTaskNotifyGive(s_button_task);             /* recalculate a pending quiet time or held timeout */
    }
}

/* A role change, from PUT /api/v1/settings; see settings.h. Today's order: the Zigbee
 * lock, then the mux, nothing from the stack inside the mux. The lock is held from before the Off until
 * the new role is published, so no Zigbee controller command is handled in between under either role. With the stack
 * not up there is no lock to take, and nothing can have turned the light on. */
esp_err_t bulb_role_switch(const cfg_t *next, uint32_t keys)
{
    bool locked = false;
    if (s_zb_up) {
        if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
            ESP_LOGW(TAG, "ROLE-OFF not tried: the Zigbee lock is busy; the role stays");
            return ESP_ERR_INVALID_STATE;
        }
        locked = true;
    }
    portENTER_CRITICAL(&s_light_mux);
    bool on = (s_light.seen & (1u << L_ON)) && s_light.val[L_ON];
    portEXIT_CRITICAL(&s_light_mux);
    char msg[60] = "set:";
    light_state_t l = {.seen = 0};
    if (on) {
        int want[L_COUNT];
        for (int i = 0; i < L_COUNT; i++) {
            want[i] = -1;
        }
        want[L_ON] = 0;                           /* reported to the Zigbee controller, so it starts clean */
        int r = locked ? light_apply_locked(want, "ROLE-OFF", msg, sizeof msg, false, NULL, &l) : -1;
        if (r < 0) {
            if (locked) {
                esp_zigbee_lock_release();
            }
            if (r == -2) {
                ESP_LOGW(TAG, "ROLE-OFF %s (refused); the role stays", msg + 4);
                log_light("LIGHT", &l);
            } else {
                ESP_LOGW(TAG, "ROLE-OFF not tried: the Zigbee stack is not up; the role stays");
            }
            return ESP_ERR_INVALID_STATE;
        }
    }
    esp_err_t err = cfg_nvs_write(next, keys);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_light_mux);
        s_held = false;
        s_delivering = false;
        s_release_by = NULL;
        s_press_at_us = s_press_first_us = s_released_at_us = s_off_at_us = 0;
        s_held_at_us = 0;
        s_button_generation++;
        s_push_due_us = 0;
        cfg_publish_locked(next, keys);
        state_mark(ST_BUTTON);                    /* the button's state is cleared with the role */
        portEXIT_CRITICAL(&s_light_mux);
    }
    if (locked) {
        esp_zigbee_lock_release();
    }
    if (on) {
        ESP_LOGI(TAG, "ROLE-OFF %s", msg + 4);
        log_light("LIGHT", &l);
    }
    return err;
}

/* POST /zb/light?on=0|1[&level=1-254][&ct=mireds]: the client says what the real light is now
 * (light_apply). In the shortcut role, on=0 is the client saying the shortcut ran. Until 0.5.0 it
 * took no token; it now takes the developer credential (c6_base_token_ok), and
 * /api/v1/light is the client's way in. Ignored for ECHO_WINDOW_US after a Zigbee controller command,
 * so the real light's answer to that command does not come back as a second change. A button has no
 * real light to echo: there, on=0 is acted on at once, but only while a pushed press is held. */
static esp_err_t light_post(httpd_req_t *req)
{
    if (!c6_base_token_ok(req)) {
        return ESP_OK;
    }
    char v[8];
    int want[L_COUNT];
    for (int i = 0; i < L_COUNT; i++) {
        want[i] = -1;
    }
    if (c6_base_query(req, "on", v, sizeof v)) {
        want[L_ON] = atoi(v) ? 1 : 0;
    }
    if (c6_base_query(req, "level", v, sizeof v)) {
        want[L_LEVEL] = MAX(1, MIN(atoi(v), 254));
    }
    if (c6_base_query(req, "ct", v, sizeof v) && atoi(v) > 0) {
        want[L_CT] = MAX(1, MIN(atoi(v), 0xFEFF));
        want[L_MODE] = 2;
    }
    int64_t now = esp_timer_get_time();
    int64_t hub_cmd;
    bool shortcut;
    portENTER_CRITICAL(&s_light_mux);
    hub_cmd = s_hub_cmd_us;
    shortcut = cfg_live.role == CFG_ROLE_SHORTCUT;
    portEXIT_CRITICAL(&s_light_mux);
    if (shortcut) {
        if (want[L_ON] != 0) {                    /* an On here would bypass the press logic */
            ESP_LOGI(TAG, "API-SET ignored: a button takes on=0 only");
            return httpd_resp_sendstr(req, "ignored: a button takes on=0 only\n");
        }
        for (int i = 0; i < L_COUNT; i++) {
            if (i != L_ON) {
                want[i] = -1;
            }
        }
        bool held;
        portENTER_CRITICAL(&s_light_mux);
        held = s_held;
        portEXIT_CRITICAL(&s_light_mux);
        if (!held) {
            ESP_LOGI(TAG, "API-SET ignored: no press to release");
            return httpd_resp_sendstr(req, "ignored: no press to release\n");
        }
    } else if (hub_cmd && now - hub_cmd < ECHO_WINDOW_US) {
        ESP_LOGI(TAG, "API-SET ignored: Zigbee controller command %lld ms ago", (now - hub_cmd) / 1000);
        return httpd_resp_sendstr(req, "ignored: Zigbee controller command less than 3 s ago\n");
    }
    char msg[120];
    int n = light_apply(want, "API-SET", msg, sizeof msg - 1, shortcut, NULL);
    if (n == -1) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Zigbee stack not up");
    }
    if (n == -3) {
        ESP_LOGI(TAG, "API-SET ignored: no press to release");
        return httpd_resp_sendstr(req, "ignored: no press to release\n");
    }
    if (n == -2) {
        if (shortcut) {                           /* the press stays held; its timeout retries */
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Off could not be set; see /log");
        }
        n = (int)strlen(msg);                     /* as before 0.3.24: 200, with each attribute's status */
    }
    snprintf(msg + n, sizeof msg - n, "\n");
    return httpd_resp_sendstr(req, msg);
}

/* POST /api/v1/light's core (api/v1/light.c), after its body is checked: the shortcut role is
 * refused, a Zigbee controller command less than ECHO_WINDOW_US ago makes it ignored, else light_apply as API-SET (by "api").
 * want[] is indexed by SNAP_* (the L_* order), -1: leave. BULB_CMD_*; msg gets light_apply's text. */
int bulb_light_set(int want[SNAP_COUNT], char *msg, size_t msz)
{
    int64_t now = esp_timer_get_time();
    int64_t hub_cmd;
    bool shortcut;
    portENTER_CRITICAL(&s_light_mux);
    hub_cmd = s_hub_cmd_us;
    shortcut = cfg_live.role == CFG_ROLE_SHORTCUT;
    portEXIT_CRITICAL(&s_light_mux);
    if (shortcut) {
        return BULB_CMD_WRONG_ROLE;
    }
    if (hub_cmd && now - hub_cmd < ECHO_WINDOW_US) {
        goto echo;
    }
    if (!s_zb_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return BULB_CMD_BUSY;
    }
    /* A Zigbee controller command can arrive while this waits for the lock: it wins the echo window too. */
    now = esp_timer_get_time();
    portENTER_CRITICAL(&s_light_mux);
    hub_cmd = s_hub_cmd_us;
    portEXIT_CRITICAL(&s_light_mux);
    if (hub_cmd && now - hub_cmd < ECHO_WINDOW_US) {
        esp_zigbee_lock_release();
        goto echo;
    }
    light_state_t l = {.seen = 0};
    int n = light_apply_locked(want, "API-SET", msg, msz, false, NULL, &l);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "API-SET %s%s", msg + 4, n == -2 ? " (not all set)" : "");
    log_light("LIGHT", &l);
    return n == -2 ? BULB_CMD_REFUSED : BULB_CMD_OK;
echo:
    ESP_LOGI(TAG, "API-SET ignored: Zigbee controller command %lld ms ago", (now - hub_cmd) / 1000);
    return BULB_CMD_ECHO;
}

/* POST /api/v1/button/release's core (api/v1/light.c): in the shortcut role, with a press held, the Off that
 * POST /zb/light?on=0 makes (light_apply with need_held, logged API-RELEASE; release_by "api"). BULB_CMD_*: a
 * refused or untried Off is BULB_CMD_BUSY, and the press stays held for its own timeout to retry. */
int bulb_button_release(char *msg, size_t msz)
{
    bool shortcut, held;
    portENTER_CRITICAL(&s_light_mux);
    shortcut = cfg_live.role == CFG_ROLE_SHORTCUT;
    held = s_held;
    portEXIT_CRITICAL(&s_light_mux);
    if (!shortcut) {
        return BULB_CMD_WRONG_ROLE;
    }
    if (!held) {
        return BULB_CMD_NO_PRESS;
    }
    int want[L_COUNT];
    for (int i = 0; i < L_COUNT; i++) {
        want[i] = -1;
    }
    want[L_ON] = 0;
    int n = light_apply(want, "API-RELEASE", msg, msz, true, NULL);
    if (n == -3) {
        return BULB_CMD_NO_PRESS;                 /* released while this waited for the lock */
    }
    return n < 0 ? BULB_CMD_BUSY : BULB_CMD_OK;
}

/* The shortcut button: push a press once the Zigbee controller has been quiet for settle_ms, then report Off
 * by itself if the client has not within off_after_s. A press is held from its push to its
 * release, and Zigbee controller changes meanwhile are part of it. One that starts within rearm_ms of a
 * release is a change made on the light page as it cleared: released at once,
 * not pushed. A release the client did not ask for (timeout, the Zigbee controller's Off, or the client's Off arriving before the
 * press reached it) is pushed as {"release":N,"by":"timeout"|"zigbee"|"api"}, so the client's switch follows.
 *
 * Delivery policy (0.3.24): a release is idempotent, so a failed release push is tried twice more; a
 * press is never tried again, because a press the client did get could run the shortcut twice. A release push
 * always follows the press it releases: both are posted from this task, press first. A self-release
 * that cannot be applied (the Zigbee lock was busy, or the attribute was refused) is retried after
 * BUTTON_RETRY_US, and the client is told only once it has worked. Woken by hub_tap. 0.4.0: the task
 * exists from boot and is idle unless the role is shortcut; the three timings are settings, read live.
 * off_after_s defaults to 0 since 0.6.0: a press is held until the client releases it or the touchscreen turns
 * it off. */
static void button_release_push(const char *by, uint32_t press, uint32_t generation)
{
    char buf[64];
    snprintf(buf, sizeof buf, "{\"release\":%lu,\"by\":\"%s\"}", (unsigned long)press, by);
    ESP_LOGI(TAG, "BUTTON-RELEASE %lu by %s", (unsigned long)press, by);
    if (!cfg_has_webhook()) {
        return;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (button_post(buf, generation) != 0) {  /* success, or canceled while waiting between tries */
            return;
        }
    }
    ESP_LOGW(TAG, "BUTTON-RELEASE %lu: the client did not take it; its switch may still be on", (unsigned long)press);
}

static void button_task(void *arg)
{
    for (;;) {
        int64_t now = esp_timer_get_time(), next = 0;
        const char *by = NULL;
        portENTER_CRITICAL(&s_light_mux);
        bool active = cfg_live.role == CFG_ROLE_SHORTCUT;
        uint32_t generation = s_button_generation;
        uint32_t presses = s_presses;             /* only this task changes it (0.5.0: a static, for the state object) */
        if (active) {
            by = s_release_by;
            s_release_by = NULL;
        }
        portEXIT_CRITICAL(&s_light_mux);
        if (!active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* not the shortcut role: look again when woken */
            continue;
        }
        if (by) {
            button_release_push(by, presses, generation);
            continue;                             /* time has passed: look again */
        }
        portENTER_CRITICAL(&s_light_mux);
        if (cfg_live.role != CFG_ROLE_SHORTCUT || s_button_generation != generation) {
            portEXIT_CRITICAL(&s_light_mux);
            continue;
        }
        if (s_press_at_us) {
            next = s_press_at_us + (int64_t)cfg_live.settle_ms * 1000;
        } else if (s_off_at_us) {
            next = s_off_at_us;
        }
        portEXIT_CRITICAL(&s_light_mux);
        if (!next) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (next > now) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((next - now) / 1000) + 1);
            now = esp_timer_get_time();
        }
        bool press = false, timeout = false, late = false, was_held = false;
        light_state_t l;
        int64_t since = 0;
        portENTER_CRITICAL(&s_light_mux);
        if (cfg_live.role != CFG_ROLE_SHORTCUT || s_button_generation != generation) {
            portEXIT_CRITICAL(&s_light_mux);
            continue;                             /* a role switch ended the work which waited */
        }
        int64_t settle_us = (int64_t)cfg_live.settle_ms * 1000, rearm_us = (int64_t)cfg_live.rearm_ms * 1000,
                off_after_us = (int64_t)cfg_live.off_after_s * 1000000;
        if (s_press_at_us && now - s_press_at_us >= settle_us) {
            s_press_at_us = 0;
            since = s_press_first_us - s_released_at_us;
            if (s_released_at_us && since < rearm_us) {
                late = true;                      /* a change made as the last press cleared */
            } else {
                press = true;
                presses = ++s_presses;
                s_button_at_us = now;
                state_mark(ST_BUTTON);
                s_held = true;
                s_held_at_us = now;
                s_delivering = true;              /* a release from now until posted is followed by a push */
                s_off_at_us = off_after_us ? now + off_after_us : 0;
                l = s_hub;
            }
        } else if (!s_press_at_us && s_off_at_us && now >= s_off_at_us) {
            timeout = true;
            was_held = s_held;                    /* false: a retried late release */
            s_off_at_us = 0;
        }
        portEXIT_CRITICAL(&s_light_mux);
        if (press) {
            char st[200], buf[220];
            light_json(&l, true, st, sizeof st);
            snprintf(buf, sizeof buf, "{\"press\":%lu,%s", (unsigned long)presses, st + 1);
            ESP_LOGI(TAG, "BUTTON-PRESS %lu", (unsigned long)presses);
            if (cfg_has_webhook()) {
                button_post(buf, generation);
            }
            portENTER_CRITICAL(&s_light_mux);
            if (s_button_generation == generation) {
                s_delivering = false;             /* a release set meanwhile is pushed at the top of the loop */
            }
            portEXIT_CRITICAL(&s_light_mux);
        }
        if (late) {
            ESP_LOGI(TAG, "BUTTON-LATE on %lld ms after the last release: not a press, releasing",
                     since / 1000);
        }
        if (timeout || late) {
            const char *who = late ? "BUTTON-LATE" : was_held ? "BUTTON-TIMEOUT" : "BUTTON-LATE (again)";
            int want[L_COUNT];
            for (int i = 0; i < L_COUNT; i++) {
                want[i] = -1;
            }
            want[L_ON] = 0;
            char msg[60];
            bool released = false;
            int r = button_light_apply(want, who, msg, sizeof msg, was_held, &released, generation);
            if (r == -3) {
                continue;                         /* the originating role or timeout ended */
            }
            if (r < 0) {
                /* Retry only while there is still something to turn off: the press is still held, or (late)
                 * the light is still on with no new press pending. */
                bool armed = false;
                portENTER_CRITICAL(&s_light_mux);
                if (cfg_live.role != CFG_ROLE_SHORTCUT || s_button_generation != generation) {
                    portEXIT_CRITICAL(&s_light_mux);
                    continue;
                }
                bool live = was_held ? s_held : ((s_light.seen & (1u << L_ON)) && s_light.val[L_ON]);
                if (live && (!was_held || cfg_live.off_after_s) && !s_press_at_us && !s_off_at_us) {
                    s_off_at_us = esp_timer_get_time() + BUTTON_RETRY_US;
                    armed = true;
                }
                portEXIT_CRITICAL(&s_light_mux);
                ESP_LOGW(TAG, "%s: could not turn off%s", who, armed ? "; trying again in 5 s" : "; nothing left to do");
            } else if (timeout && released) {
                button_release_push("timeout", presses, generation);   /* only if this call ended the press */
            }
        }
    }
}

/* The first-start check's Zigbee half (0.6.0): a bulb image is valid only once it can join. */
bool c6_app_verify_zigbee(void)
{
    return true;
}

void app_main(void)
{
    restore_guard_at_boot();
    c6_base_start();
    cfg_load();                                   /* NVS is up; before any task or handler reads a setting */
    events_start();                               /* the stream's lock and task, before its handler exists */
    const httpd_uri_t uris[] = {
        {.uri = "/zb",       .method = HTTP_GET,  .handler = zb_get},
        {.uri = "/zb/steer", .method = HTTP_POST, .handler = steer_post},
        {.uri = "/zb/leave", .method = HTTP_POST, .handler = leave_post},
        {.uri = "/zb/erase", .method = HTTP_POST, .handler = erase_post},
        {.uri = "/zb/fc",    .method = HTTP_POST, .handler = fc_post},
        {.uri = "/zb/report", .method = HTTP_POST, .handler = report_post},
        {.uri = "/zb/scan",  .method = HTTP_POST, .handler = scan_post},
        {.uri = "/zb/binds", .method = HTTP_GET,  .handler = binds_get},
#if CONFIG_C6_ZB_KEY_ENDPOINT
        {.uri = "/zb/key",   .method = HTTP_GET,  .handler = key_get},
#endif
        {.uri = "/zb/light", .method = HTTP_POST, .handler = light_post},
        {.uri = "/api/v1/settings", .method = HTTP_GET, .handler = settings_get},
        {.uri = "/api/v1/settings", .method = HTTP_PUT, .handler = settings_put},
        {.uri = "/api/v1/zigbee/pair",  .method = HTTP_POST, .handler = zigbee_pair_post},
        {.uri = "/api/v1/zigbee/leave", .method = HTTP_POST, .handler = zigbee_leave_post},
        {.uri = "/api/v1/zigbee/erase", .method = HTTP_POST, .handler = zigbee_erase_post},
        {.uri = "/api/v1/state",  .method = HTTP_GET,  .handler = state_get},
        {.uri = "/api/v1/events", .method = HTTP_GET,  .handler = events_get},
        {.uri = "/api/v1/light",  .method = HTTP_POST, .handler = light_v1_post},
        {.uri = "/api/v1/button/release", .method = HTTP_POST, .handler = button_release_post},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(c6_base_httpd(), &uris[i]));
    }
    xTaskCreate(zb_task, "zigbee", 8192, NULL, 5, NULL);
    xTaskCreate(watchdog_task, "zb_watchdog", 4096, NULL, 4, NULL);   /* NVS saves run here: see wd_stack_free */
    /* Both from boot (0.4.0): each acts only in its own role, so a role change needs no reboot. */
    xTaskCreate(push_task, "webhook", 6144, NULL, 4, &s_push_task);
    xTaskCreate(button_task, "button", 6144, NULL, 4, &s_button_task);
}
