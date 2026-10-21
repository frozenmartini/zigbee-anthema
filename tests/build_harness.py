#!/usr/bin/env python3
"""Build $HARNESS_OUT/harness.c (default /tmp/c6-harness-3.0) from the 0.4.0 bulb.c and api/v1/settings.c.

usage: build_harness.py [bulb.c [settings.c [settings.h]]]     (defaults: the repo paths; settings.h next to settings.c)

Firmware code is extracted VERBATIM and BY NAME (signature line to the closing brace at column 0), so
the extraction survives line shifts; a name that does not match exactly once stops the build. Only the
dependencies (Zigbee, NVS, HTTP server and client, RTOS, timers, reset reason, MAC) are stubbed. cJSON is
the real library (tests/vendor/, compiled by run.sh). Declarations (macros, light_state_t, the shared
globals, cfg_t) are extracted by marker.

What 0.4.0 changed here: the role is a runtime setting (cfg_live.role), so there is one build and the
probes set the role; the settings store (settings.c) and bulb_role_switch/ieee_from_mac are extracted too;
zb_task's address block is extracted by marker into a small wrapper (zb_address_path) for the IEEE probes.

The harness binary takes one argument: old (the 21 probes of 0.3.24, role shortcut), old-normal (the one
normal-mode probe, role mirror) or new (the 0.4.0 probes).
"""
import os
import re
import sys
from pathlib import Path

C6 = Path(os.environ.get('C6_DIR') or Path(__file__).resolve().parent.parent)   # the tree this script sits in (a mutation copy too)
BULB = Path(sys.argv[1] if len(sys.argv) > 1 else C6 / 'bulb/main/bulb.c')
SETT = Path(sys.argv[2] if len(sys.argv) > 2 else C6 / 'bulb/main/api/v1/settings.c')
HDR = Path(sys.argv[3]) if len(sys.argv) > 3 else SETT.with_name('settings.h')
OUT = Path(os.environ.get('HARNESS_OUT', '/tmp/c6-harness-3.0'))
KC_BULB = Path(os.environ.get('KCONFIG_BULB') or SETT.parent.parent.parent / 'Kconfig.projbuild')    # 0.6.0: the defaults the harness uses ARE the Kconfig ones,
KC_BASE = Path(os.environ.get('KCONFIG_BASE') or Path(os.environ.get('C6_COMP') or C6 / 'components/c6_base') / 'Kconfig')                  # so a mutation of a default in a copy reaches the probes


def kdefault(path, symbol):
    """The `default N` of `config <symbol>` (an int) in a Kconfig file; stops the build if it is not there."""
    lines = path.read_text().splitlines()
    for i, l in enumerate(lines):
        if re.match(r'\s*config\s+' + re.escape(symbol) + r'\s*$', l):
            for m in lines[i + 1:i + 6]:
                d = re.match(r'\s*default\s+(-?\d+)\s*$', m)
                if d:
                    return d.group(1)
            break
    sys.exit(f'{path}: no integer default for {symbol}')
OUT.mkdir(parents=True, exist_ok=True)
bulb = BULB.read_text().splitlines(True)
sett = SETT.read_text().splitlines(True)
hdr = HDR.read_text().splitlines(True)


def find_func(src, name, label, optional=False):
    """(first_line_idx, last_line_idx) of the definition of `name` (column-0 signature, `}` at col 0)."""
    sig = re.compile(r'^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w \t\*]*?\b' + re.escape(name) + r'\s*\(')
    hits = [i for i, l in enumerate(src) if sig.match(l) and not l.rstrip().endswith(';')]
    if optional and not hits:
        return None
    if len(hits) != 1:
        sys.exit(f'{label}: {name}: expected exactly one definition, found {len(hits)}')
    start = hits[0]
    for j in range(start, len(src)):
        if src[j].rstrip('\n') == '}':
            return start, j
    sys.exit(f'{label}: {name}: no closing brace')


def func(src, name, label):
    a, b = find_func(src, name, label)
    return a, ''.join(src[a:b + 1])


def optional_func(src, name, label):
    """Review-fix helpers are absent in the untouched before-fix firmware used for regressions."""
    bounds = find_func(src, name, label, optional=True)
    if bounds is None:
        return []
    a, b = bounds
    return [(a, ''.join(src[a:b + 1]))]


def block(src, first_re, last_re, label):
    """Lines from the first match of first_re to the next match of last_re (inclusive)."""
    a = next((i for i, l in enumerate(src) if re.search(first_re, l)), None)
    if a is None:
        sys.exit(f'{label}: start marker not found')
    b = next((j for j in range(a, len(src)) if re.search(last_re, src[j])), None)
    if b is None:
        sys.exit(f'{label}: end marker not found')
    return a, ''.join(src[a:b + 1])


def zb_address_path():
    """The IEEE block of zb_task (from `uint8_t mac[8];` to the IEEE log line), as a wrapper function."""
    a, b = find_func(bulb, 'zb_task', 'bulb.c')
    s = next((i for i in range(a, b) if 'uint8_t mac[8];' in bulb[i]), None)
    e = next((i for i in range(s or a, b) if 'mac[6], mac[7], from);' in bulb[i]), None)
    if s is None or e is None:
        sys.exit('zb_task: the IEEE block markers (uint8_t mac[8]; ... mac[6], mac[7], from);) were not found')
    return s, 'static void zb_address_path(void)\n{\n' + ''.join(bulb[s:e + 1]) + '}\n'


def zb_pan_path():
    """The PAN and channel block of zb_task (from `char ext_pan[17];` to the zb_join_filter call), as a wrapper function; the
    source order of the three steps (read the setting, set the filter, esp_zigbee_start) becomes a constant for zr4."""
    a, b = find_func(bulb, 'zb_task', 'bulb.c')
    s = next((i for i in range(a, b) if 'char ext_pan[17];' in bulb[i]), None)
    e = next((i for i in range(s or a, b) if 'zb_join_filter(' in bulb[i]), None)
    st = next((i for i in range(a, b) if 'esp_zigbee_start(' in bulb[i]), None)
    if s is None or e is None or st is None:
        sys.exit('zb_task: the PAN block markers (char ext_pan[17]; ... zb_join_filter( ... esp_zigbee_start() were not found')
    cfg = next((i for i in range(s, e) if 'cfg_ext_pan(' in bulb[i]), None)
    order_ok = cfg is not None and cfg < e < st and sum('zb_join_filter(' in l for l in bulb[a:b]) == 1 and sum('esp_zigbee_start(' in l for l in bulb[a:b]) == 1
    return s, (f'#define HARNESS_ZB_PAN_BEFORE_START {int(order_ok)}\n' + 'static void zb_pan_path(void)\n{\n' + ''.join(bulb[s:e + 1]) + '}\n')


# ---- extraction, in dependency order: cfg_t, bulb's declarations, settings.c, bulb's functions
types = [block(hdr, r'^enum \{ CFG_ROLE_NONE', r'^\} cfg_t;', 'settings.h cfg_t')]
decls = [block(bulb, r'^#define EP ', r'^#define EP ', 'EP'),
         block(bulb, r'^enum \{ L_ON, L_LEVEL', r'^#define BUTTON_RETRY_US', 'light state + globals')]
SETT_FUNCS = ['cfg_num_get', 'cfg_num_set', 'cfg_num_ok', 'cfg_role_name', 'cfg_role_parse', 'cfg_webhook_ok',
              'cfg_mask_webhook', 'cfg_ext_pan_ok', 'cfg_name_ok', 'cfg_default_name',
              'cfg_key_list', 'cfg_parse_item', 'cfg_nesting_ok', 'cfg_parse', 'cfg_load', 'cfg_get', 'cfg_role',
              'cfg_rx_wd_s', 'cfg_has_webhook', 'cfg_ext_pan', 'cfg_hue', 'cfg_hue_boot', 'cfg_webhook', 'cfg_nvs_set', 'cfg_nvs_write', 'cfg_publish_locked', 'cfg_store',
              'cfg_log_changes', 'cfg_send_json', 'cfg_json_add', 'cfg_send_settings', 'settings_get',
              'settings_put']
# CONFIG_C6_ZB_EXT_PANID is a string literal for the two compile-time uses (the _Static_assert and cfg_live's static
# initializer) and the variable g_kconfig_ext_pan (a char[17] the probes set) from there on, for cfg_load's runtime copy.
_ca, _ct = block(sett, r'^#define CFG_NVS_NS', r'^#define CFG_NUM_COUNT', 'settings.c tables')
_la, _lt = block(sett, r'^cfg_t cfg_live = \{', r'^\};', 'cfg_live')
_ea, _et = block(sett, r'^static char s_ext_pan_boot', r'^static char s_ext_pan_boot', 's_ext_pan_boot')
_ha, _ht = block(sett, r'^static bool s_hue_boot', r'^static bool s_hue_boot', 's_hue_boot')
sparts = [(_ca - 0.5, '#undef CONFIG_C6_ZB_EXT_PANID\n#define CONFIG_C6_ZB_EXT_PANID ""\n'), (_ca, _ct), (_la, _lt),
          (_la + 0.5, '#undef CONFIG_C6_ZB_EXT_PANID\n#define CONFIG_C6_ZB_EXT_PANID g_kconfig_ext_pan\n'), (_ea, _et), (_ha, _ht)]
sparts += [func(sett, f, 'settings.c') for f in SETT_FUNCS]
sparts.sort(key=lambda t: t[0])
BULB_FUNCS = ['light_mark_changed_locked', 'light_by_of', 'light_release_by_of', 'log_light', 'group_after_cmd', 'hub_tap',
              'light_save_if_settled', 'group_save_if_dirty', 'group_load', 'group_clear',
              'restore_guard_on_shutdown', 'restore_guard_at_boot', 'light_restore', 'restore_reporting', 'reporting_on',
              'report_post', 'light_json', 'webhook_post', 'push_task', 'report_to_hub', 'light_apply_locked',
              'light_apply', 'bulb_role_switch', 'light_post', 'button_release_push', 'button_task', 'c6_app_verify_zigbee',
              'ieee_from_mac', 'bulb_zigbee_snapshot', 'bulb_light_snapshot', 'bulb_button_snapshot', 'bulb_light_set',
              'bulb_button_release', 'steer_retry_ms', 'steer_fast_from', 'steer_later', 'steer_now_locked', 'steer_timer_cb',
              'on_signal', 'zb_join_filter', 'watchdog_task', 'nvs_ns_kept', 'nvs_erase_except', 'zigbee_reboot_task',
              'zigbee_pair_post', 'zigbee_leave_post', 'zigbee_erase_post', 'erase_task', 'erase_post', 'color_shape', 'create_device']
bparts = [func(bulb, f, 'bulb.c') for f in BULB_FUNCS] + [zb_address_path(), zb_pan_path()]
bparts.append(block(bulb, r'^#if CONFIG_C6_ZB_PROFILE_SENGLED_E11$', r'^#endif', 'bulb.c profile block'))   # PROFILE_* macros, custom_noop, sengled_e11_extras (E11) or the Kconfig strings (neutral)
for name in ('webhook_post_to', 'button_post', 'button_light_apply',
             'bulb_settings_changed_locked', 'bulb_settings_changed'):
    bparts += optional_func(bulb, name, 'bulb.c')
bparts.sort(key=lambda t: t[0])
cat = lambda parts: '\n'.join(t for _, t in parts)
generation_macro = '#define HARNESS_BUTTON_GENERATION 1\n' if 's_button_generation' in cat(decls) else ''

# ---- the 0.5.0 files: extracted by name like the two above (column-0 signature to the closing brace), headers as text
COMP = Path(os.environ.get('C6_COMP') or C6 / 'components/c6_base')
API = SETT.parent
INC = COMP / 'include'
NEWSRC = {n: Path(os.environ.get(n.upper() + '_C') or d / f) for n, d, f in (
    ('state', API, 'state.c'), ('events', API, 'events.c'), ('light', API, 'light.c'), ('auth', COMP, 'c6_auth.c'),
    ('led', COMP, 'c6_led.c'), ('disc', COMP, 'c6_discovery.c'), ('base', COMP, 'c6_base.c'),
    ('wifi', COMP, 'c6_wifi.c'), ('button', COMP, 'c6_button.c'), ('improv', COMP, 'improv.c'), ('portal', COMP, 'portal.c'),
    ('dns', COMP, 'dns.c'))}
STATE_H = Path(os.environ.get('STATE_H') or API / 'state.h')


def header_text(path):
    """A header as text: no #pragma once, no #include (the harness has no IDF headers); prototypes stay."""
    return ''.join(l for l in path.read_text().splitlines(True) if not l.startswith(('#pragma once', '#include')))


headers = ('typedef void *httpd_handle_t;\ntypedef struct esp_netif_obj esp_netif_t;\n' + ''.join(header_text(h) for h in (INC / 'c6_base.h', INC / 'c6_auth.h', INC / 'c6_led.h',
           INC / 'c6_discovery.h', INC / 'c6_button.h', INC / 'c6_wifi.h', STATE_H)))
efiles = []      # (label, parts) in the order they are emitted


def all_funcs(src):
    """Every function defined at column 0 of a file (static or not, prototypes and macros skipped), in file order."""
    names = []
    for i, l in enumerate(src):
        m = re.match(r'^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(', l)
        if m and l[0].isalpha() and not l.startswith(('typedef', 'enum', 'struct ')) and '=' not in l.split('(')[0] \
                and not l.rstrip().endswith(';') and m.group(1) not in names:
            names.append(m.group(1))
    return names


def efile(key, blocks=(), funcs=(), renames=(), optional=(), every=False):
    """Pieces of one file: `blocks` (first_re, last_re) by marker, `funcs` by name; `renames` (old, new) wrap the text in
    #define/#undef so two files' same-named statics do not meet in this one translation unit."""
    src = NEWSRC[key].read_text().splitlines(True)
    label = NEWSRC[key].name
    if every:
        funcs = list(funcs) + [n for n in all_funcs(src) if n not in funcs]
    parts = [block(src, a, b, f'{label} {a}') for a, b in blocks] + [func(src, f, label) for f in funcs]
    for f in optional:
        parts += optional_func(src, f, label)
    parts.sort(key=lambda t: t[0])
    text = (''.join(f'#define {o} {n}\n' for o, n in renames) + '\n'.join(t for _, t in parts) + '\n'
            + ''.join(f'#undef {o}\n' for o, _ in renames))
    efiles.append((label, parts))
    return text


new_text = [
    efile('state', blocks=[(r'^#define STATE_BUF_SIZE', r'^\};')],
          funcs=['st_num', 'st_str', 'st_null', 'st_bool', 'st_at', 'st_hex16', 'st_board', 'st_zigbee', 'st_light', 'st_hub',
                 'st_button', 'st_update', 'state_json', 'state_get']),
    efile('events', blocks=[(r'^#define EV_BUF_SIZE', r'^static char s_buf\[')],
          funcs=['c6_app_state_mark', 'ev_finish', 'c6_app_events_close', 'ev_frame', 'ev_send', 'events_step', 'events_task',
                 'events_start', 'events_get'],
          renames=[('c6_app_state_mark', 'real_c6_app_state_mark'), ('c6_app_events_close', 'real_c6_app_events_close')]),
    efile('light', blocks=[(r'^#define LIGHT_BODY_MAX', r'^#define LIGHT_K_COLOUR')],
          funcs=['light_field', 'light_nesting_ok', 'light_int', 'light_pair', 'light_item', 'light_parse', 'light_send_ok',
                 'light_v1_post', 'button_release_post']),
    efile('auth', blocks=[(r'^#define AUTH_NS', r'^static SemaphoreHandle_t s_lock')],
          funcs=['auth_wipe', 'auth_digest_equal', 'auth_text_equal', 'auth_dev_match', 'auth_window_is_open', 'auth_bearer_value',
                 'auth_hex', 'auth_decide', 'c6_auth_claimed', 'c6_auth_window_open', 'auth_led_update', 'auth_window_cb',
                 'c6_auth_window_open_for', 'auth_load', 'c6_auth_start', 'auth_lock', 'auth_unlock', 'auth_store', 'c6_auth_claim',
                 'c6_auth_clear', 'auth_bearer', 'auth_dev_header', 'c6_auth_dev_ok', 'auth_class_name', 'c6_auth_ok']),
    efile('led', blocks=[(r'^#define LED_TICK_MS', r'^static bool s_started;')],
          funcs=['led_winner', 'led_level', 'led_colour', 'led_scale', 'led_step', 'led_task', 'c6_led_start', 'c6_led_set', 'c6_led_identify_rgb'],
          renames=[('s_mux', 'led_s_mux'), ('s_started', 'led_s_started')]),
    efile('disc', blocks=[(r'^#define DISC_SERVICE', r'^static volatile bool s_started')],
          funcs=['disc_role', 'disc_state', 'disc_txt', 'c6_discovery_start', 'c6_discovery_update', 'c6_discovery_set_name'],
          renames=[('s_started', 'disc_s_started')]),
    efile('base', blocks=[(r'^#define LOG_RING', r'^static portMUX_TYPE s_ring_mux'), (r'^static esp_timer_handle_t s_identify_timer', r'^static esp_timer_handle_t s_identify_timer'),
                          (r'^#define BIT_GOT_IP', r'^static EventGroupHandle_t s_ev;'),             # 0.6.0: the bits and the event group,
                          (r'^static volatile bool s_ota_busy', r'^static bool s_zb_ready_logged'),   # the busy flag, the update object's mux and object, the once-flag
                          (r'^#define REBOOT_DELAY_MS', r'^#define REBOOT_DELAY_MS')],         # the restart delay, read from the firmware
          funcs=['update_percent_due', 'update_set', 'c6_base_update_info', 'update_fail', 'update_post', 'ota_fail', 'ota_post',
                 'c6_base_zigbee_ready', 'verify_task', 'ring_copy_in', 'c6_base_token_ok', 'json_escape', 'http_status_line', 'c6_json_error', 'c6_base_name', 'log_send', 'log_get',
                 'identify_end', 'identify_rgb', 'identify_start', 'identify_post', 'reboot_task', 'reboot_post', 'api_info_get', 'api_identify_post', 'api_log_get',
                 'api_claim_post', 'api_release_post', 'api_reboot_post', 'button_action'],
          renames=[('s_host', 's_host'), ]),
    efile('button', blocks=[(r'^#define BTN_POLL_MS', r'^static bool s_started;')], every=True,
          renames=[('s_cb', 'bt_s_cb'), ('s_started', 'bt_s_started')]),
    efile('wifi', blocks=[(r'^#define WIFI_NVS_NS', r'^static c6_wifi_ap_cb_t s_ap_cb;')], every=True,
          renames=[('s_mux', 'wf_s_mux'), ('s_state', 'wf_s_state'), ('s_ssid', 'wf_s_ssid')]),
    efile('improv', blocks=[(r'^#define IMPROV_VERSION', r'^static int64_t s_improv_deadline_us;')], every=True),
    efile('portal', blocks=[(r'^#define PORTAL_BODY_MAX', r'^\} portal_nets_t;')], every=True),
    efile('dns', blocks=[(r'^#define DNS_PORT', r'^static uint8_t s_dns_out\[DNS_BUF\];')], every=True,
          renames=[('socket', 'lw_socket'), ('bind', 'lw_bind'), ('setsockopt', 'lw_setsockopt'), ('recvfrom', 'lw_recvfrom'), ('sendto', 'lw_sendto'), ('close', 'lw_close')]),
]
# 0.6.0: three more copies of c6_base.c's functions, extracted from the same text by name.
#  - the weak c6_app_verify_zigbee default, under weak_ (bulb.c's strong one has the real name);
#  - c6_base_zigbee_ready compiled with CONFIG_C6_VERIFY_BREAK_TEST 1 (a preprocessor #if, not a run-time test), as brk_c6_base_zigbee_ready;
#  - verify_task as weak_verify_task, whose call to c6_app_verify_zigbee() is the weak default: the image of c6_base and c6_coord.
_bsrc = NEWSRC['base'].read_text().splitlines(True)
_weak = func(_bsrc, 'c6_app_verify_zigbee', 'c6_base.c')
_zr = func(_bsrc, 'c6_base_zigbee_ready', 'c6_base.c')
_vt = func(_bsrc, 'verify_task', 'c6_base.c')
base_extra = ('/* ---- c6_base.c, 0.6.0: the weak hook, the break-test copy, verify_task against the weak hook ---- */\n'
              + '#define c6_app_verify_zigbee weak_c6_app_verify_zigbee\n' + _weak[1] + '\n'
              + '#define verify_task weak_verify_task\n' + _vt[1] + '\n#undef verify_task\n#undef c6_app_verify_zigbee\n'
              + '#define c6_base_zigbee_ready brk_c6_base_zigbee_ready\n#undef CONFIG_C6_VERIFY_BREAK_TEST\n#define CONFIG_C6_VERIFY_BREAK_TEST 1\n' + _zr[1]
              + '\n#undef CONFIG_C6_VERIFY_BREAK_TEST\n#define CONFIG_C6_VERIFY_BREAK_TEST 0\n#undef c6_base_zigbee_ready\n')
efiles.append(('c6_base.c (weak / break-test copies)', [(_weak[0], _weak[1]), (_vt[0], _vt[1]), (_zr[0], _zr[1])]))
board_after = '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wunused-variable"\n' + (Path(__file__).resolve().parent / 'board_after.c').read_text() + '\n#pragma GCC diagnostic pop\n'
TESTS = Path(__file__).resolve().parent
HARNESS_PROFILE = os.environ.get('HARNESS_PROFILE', 'neutral')       # neutral: the public default (Espressif identity); e11: the Sengled build option
if HARNESS_PROFILE not in ('neutral', 'e11'):
    sys.exit(f'HARNESS_PROFILE must be neutral or e11, not {HARNESS_PROFILE!r}')
PROFILE_MACROS = ('#define CONFIG_C6_ZB_PROFILE_SENGLED_E11 %d\n#define CONFIG_C6_ZB_PROFILE_ESPRESSIF %d\n' % ((1, 0) if HARNESS_PROFILE == 'e11' else (0, 1)))
board_stubs = (PROFILE_MACROS + '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wunused-variable"\n#pragma GCC diagnostic ignored "-Wunused-parameter"\n'
               + ''.join(f.read_text() + '\n' for f in sorted(TESTS.glob('board_stubs*.c'))) + '\n#pragma GCC diagnostic pop\n')
BOARD_AREAS = ('wf', 'ld', 'au', 'bt', 'dc', 'zr', 'gp', 'st', 'hu', 'ev', 'fl', 'lt', 'ns', 'im', 'pt', 'cv', 'up', 'vf', 'bk', 'cx')       # run in this order; cx last
board_areas = [a for a in BOARD_AREAS if (TESTS / f'board_{a}.c').exists()]
board_probes = ''.join(f'/* ===== board_{a}.c ===== */\n' + (TESTS / f'board_{a}.c').read_text() + '\n' for a in board_areas)
board_probes += 'static void run_board(void) {\n    board_hooks();\n' + ''.join(f'    probe_{a}();\n' for a in board_areas) + '}\n'
NOWARN = ('#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wunused-function"\n'
          '#pragma GCC diagnostic ignored "-Wunused-parameter"\n#pragma GCC diagnostic ignored "-Wunused-variable"\n')
extracted = (generation_macro + cat(decls) + '\n'
             + 'esp_err_t bulb_role_switch(const cfg_t *next, uint32_t keys);\n'
             + 'void bulb_settings_changed_locked(uint32_t keys);\nvoid bulb_settings_changed(void);\n\n'
             + cat(sparts) + '\n' + NOWARN + '\n'.join(new_text) + base_extra + '\n#pragma GCC diagnostic pop\n' + cat(bparts))

pre = r'''
/* ===== STUBS (dependencies only) ===== */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "cJSON.h"
#include "http_parser.h"
static int g_json_parses = 0;                       /* parser calls by firmware (x4: deep bodies must not reach either parser) */
#define cJSON_Parse(s) (g_json_parses++, cJSON_Parse(s))
#define cJSON_ParseWithOpts(s, end, full) (g_json_parses++, cJSON_ParseWithOpts(s, end, full))
#define CONFIG_C6_ZB_ENDPOINT 1
#define CONFIG_C6_BUTTON_SETTLE_MS 2000
#define CONFIG_C6_BUTTON_OFF_AFTER_S @@OFF_AFTER_S@@          /* the Kconfig default (0 since 0.6.0), read from Kconfig.projbuild by build_harness.py */
#define HX_STR(x) #x
#define HX_XSTR(x) HX_STR(x)
#define HARNESS_OFF_AFTER_S 120                  /* what reset_case() starts a probe with: 0.5.2's timeout, which the 134 old probes were written for */
#define CONFIG_C6_VERIFY_TIMEOUT_S @@VERIFY_TIMEOUT_S@@
#define CONFIG_C6_VERIFY_STABLE_S @@VERIFY_STABLE_S@@
#define CONFIG_C6_VERIFY_ZIGBEE_S @@VERIFY_ZIGBEE_S@@
#define CONFIG_C6_VERIFY_BREAK_TEST 0            /* a preprocessor #if in c6_base_zigbee_ready: the break build is a second copy of it (brk_), compiled with 1 */
#define CONFIG_C6_BUTTON_REARM_MS 3000
#define CONFIG_C6_ZB_RX_WATCHDOG_S 0
#define CONFIG_C6_ZB_REPORT_LEVEL_CT 0
#define CONFIG_C6_ZB_REPORT_DST_EP 1
#define CONFIG_C6_ZB_IEEE g_kconfig_ieee         /* a string the probes set: "" (derive) or a Kconfig override */
#define RTC_NOINIT_ATTR
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

/* critical sections: count depth so the harness can see nesting or a stack/HTTP/NVS call made inside one */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static int g_crit = 0, g_nested = 0, g_call_in_crit = 0, g_crit_unbalanced = 0;
extern portMUX_TYPE s_light_mux;
static portMUX_TYPE s_ev_mux;                      /* events.c's own short section (tentative here; defined by the extracted events.c) */
static const void *g_crit_stack[16];
/* 0.5.0: c6_app_state_mark takes s_ev_mux for a few instructions, so it may sit inside s_light_mux (the one allowed nesting) */
static void crit_enter(const void *x) {
    if (g_crit > 0 && !(x == (const void *)&s_ev_mux && g_crit_stack[g_crit - 1] == (const void *)&s_light_mux)) g_nested++;
    if (g_crit < 16) g_crit_stack[g_crit] = x;
    g_crit++;
}
static void crit_exit(const void *x) { (void)x; if (--g_crit < 0) { g_crit = 0; g_crit_unbalanced++; } }
#define portENTER_CRITICAL(x) crit_enter(x)
#define portEXIT_CRITICAL(x) crit_exit(x)
#define CRIT_CALL() do { if (g_crit) g_call_in_crit++; } while (0)
/* a task notify is the one call allowed inside s_light_mux's section (state_mark's own); counted apart */
static int g_notify_in_crit = 0;
#define CRIT_CALL_NOTIFY() do { if (g_crit) g_notify_in_crit++; } while (0)
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX

/* logging: every line is kept (the probes read them, and the secrets probe scans them all); printed with HARNESS_VERBOSE=1 */
static int g_verbose = 0, g_nlogs = 0;
static char g_logbuf[262144]; static size_t g_loglen; static char g_lastlog[400];
static void hlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void hlog(const char *fmt, ...) {
    char line[400]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    g_nlogs++; snprintf(g_lastlog, sizeof g_lastlog, "%s", line);
    size_t n = strlen(line);
    if (g_loglen + n + 2 < sizeof g_logbuf) { memcpy(g_logbuf + g_loglen, line, n); g_loglen += n; g_logbuf[g_loglen++] = '\n'; g_logbuf[g_loglen] = 0; }
    if (g_verbose) printf("      [log] %s\n", line);
}
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); hlog(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); hlog(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); hlog(fmt, ##__VA_ARGS__); } while (0)
static const char *TAG = "bulb";
#define ESP_ERROR_CHECK(x) do { int e_ = (x); if (e_ != 0) { printf("ESP_ERROR_CHECK failed: %d (line %d)\n", e_, __LINE__); exit(3); } } while (0)

#define EZB_ZCL_CLUSTER_ID_ON_OFF 6
#define EZB_ZCL_CLUSTER_ID_LEVEL 8
#define EZB_ZCL_CLUSTER_ID_COLOR_CONTROL 0x300
#define EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID 0
#define EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID 0
#define EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID 8
#define EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID 3
#define EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID 4
#define EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID 7
#define EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID 0
#define EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID 1
#define EZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID 0x4001
#define EZB_ZCL_CLUSTER_SERVER 1
#define EZB_ZCL_STD_MANUF_CODE 0
#define EZB_ZCL_STATUS_SUCCESS 0
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1109
#define ESP_ERR_NVS_INVALID_LENGTH 0x110C
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_400_BAD_REQUEST 400
#define EZB_ADDR_MODE_GROUP 1
#define EZB_ADDR_MODE_SHORT 2
#define EZB_ADDR_MODE_IEEE 3
@@CFG_TYPES@@
extern cfg_t cfg_live;
typedef void *TaskHandle_t;
typedef int ezb_zcl_status_t;
typedef int esp_err_t;
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 0
typedef void *ezb_zcl_reporting_info_t;
typedef int ezb_err_t;
typedef struct {int dummy;} ezb_zcl_attr_variable_t;
#define EZB_ZCL_INVALID_REPORTING_INFO NULL
#define EZB_ERR_NONE 0

/* reporting table: one entry that remove= used to be able to delete */
static int reports_left = 1, report_starts = 0, report_update_err = 0;
static void *ezb_zcl_reporting_info_find(int ep, int cl, int role, int attr, int manuf) {(void)ep;(void)cl;(void)role;(void)attr;(void)manuf; return reports_left ? (void *)1 : NULL;}
static int ezb_zcl_reporting_info_update(void *i, int mn, int mx, void *delta) {(void)i;(void)mn;(void)mx;(void)delta; return report_update_err;}
static int ezb_zcl_reporting_start_attr_report(void *i) {(void)i; report_starts++; return 0;}

/* APS */
typedef struct {int addr_mode; union {uint16_t short_addr; uint8_t ieee[8]; struct {uint16_t group;} group_addr;} u;} ezb_addr_t;
typedef struct {uint8_t status; uint16_t profile_id, asdu_length, cluster_id; uint8_t *asdu; ezb_addr_t src_address, dst_address;} ezb_apsde_data_ind_t;
typedef struct {ezb_addr_t dst_address; uint8_t src_endpoint, dst_endpoint; uint16_t cluster_id, profile_id; uint8_t tx_options; uint16_t asdu_length; uint8_t *asdu;} ezb_apsde_data_req_t;
static int g_aps_tx = 0;
static ezb_err_t ezb_apsde_data_request(const ezb_apsde_data_req_t *r) {(void)r; g_aps_tx++; return 0;}

/* reset reason + shutdown handlers */
typedef enum {ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_BROWNOUT, ESP_RST_CPU_LOCKUP} esp_reset_reason_t;
static esp_reset_reason_t g_reset_reason = ESP_RST_POWERON;
static void (*g_shutdown_handler)(void);
static esp_reset_reason_t esp_reset_reason(void) {return g_reset_reason;}
static int esp_register_shutdown_handler(void (*h)(void)) {g_shutdown_handler = h; return 0;}

/* MAC and the 802.15.4 address (zb_task's address block, probes i1-i3) */
typedef struct {uint64_t u64;} ezb_extaddr_t;
enum {ESP_MAC_BASE = 1, ESP_MAC_IEEE802154 = 3};
static const char *g_kconfig_ieee = "";
static char g_kconfig_ext_pan[17];                 /* CONFIG_C6_ZB_EXT_PANID after the compile-time uses (settings.c extraction) */
static uint8_t g_mac[6]; static int g_read_mac_calls = 0, g_read_mac_type = -1;
static uint8_t g_iface_mac[8]; static int g_iface_set_calls = 0, g_iface_type = -1;
static int esp_read_mac(uint8_t *mac, int type) {              /* ESP_MAC_IEEE802154: eight bytes, the 48-bit MAC's three halves around FF FE (what ESP-IDF derives); any other type: six */
    CRIT_CALL(); g_read_mac_calls++; g_read_mac_type = type;
    if (type == ESP_MAC_IEEE802154) { memcpy(mac, g_mac, 3); mac[3] = 0xFF; mac[4] = 0xFE; memcpy(mac + 5, g_mac + 3, 3); }
    else memcpy(mac, g_mac, 6);
    return 0;
}
static int esp_iface_mac_addr_set(const uint8_t *mac, int type) {CRIT_CALL(); g_iface_set_calls++; g_iface_type = type; memcpy(g_iface_mac, mac, 8); return 0;}
static bool c6_zb_parse_eui64(const char *s, ezb_extaddr_t *out) {
    if (strlen(s) != 16) return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) { char c = s[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (d < 0) return false; v = v << 4 | (uint64_t)d; }
    out->u64 = v; return true;
}

/* clock + RTOS. Both tasks (the push task and the button task) exist from boot: hub_tap wakes two handles, counted separately. A task's
 * loop is stepped one pass at a time: ulTaskNotifyTake returns g_notify_pass times, then longjmps out. */
typedef struct {const char *on, *level, *ct, *remove, *min, *max, *s; char response[4096]; int code;
                size_t content_len; const char *body; size_t body_len, body_pos; int recv_fail, status_code;
                const char *query, *h_auth, *h_ota, *h_sha;         /* url query string; Authorization, X-OTA-Token and X-Firmware-SHA256 headers (NULL: absent) */
                char type[48]; char hdr[6][2][64]; int nhdr; int is_async; int recvs; int resp_len;} httpd_req_t;
static void resp_note(httpd_req_t *r, const char *b, long n);
static int64_t fake_now = 10000000;
static int g_timer_calls = 0, g_delays = 0;
static jmp_buf loop_done;
static int g_h_push, g_h_button, g_notify_push, g_notify_button, g_notify_pass;
static void (*g_delay_hook)(void), (*g_wait_hook)(void);
static int g_wait_budget, g_wait_wakes;             /* opt-in blocked-task simulation; ordinary probes still step with longjmp */
static int64_t esp_timer_get_time(void) {if (++g_timer_calls > 1000000) {puts("runaway loop"); exit(2);} return fake_now;}
static int g_delay_budget = -1;                    /* >= 0: a task loop's vTaskDelay returns that many times, then leaves the loop (watchdog_task, btn_task) */
static void vTaskDelay(unsigned ms) {
    if (g_delay_budget == 0) { g_delay_budget = -1; longjmp(loop_done, 1); }
    if (g_delay_budget > 0) g_delay_budget--;
    g_delays++; fake_now += (int64_t)ms * 1000;
    if (g_delay_hook) {void (*h)(void) = g_delay_hook; g_delay_hook = NULL; h();}
}
static void notify_other(void *t);
static void xTaskNotifyGive(void *t) {CRIT_CALL_NOTIFY(); if (t == &g_h_push) g_notify_push++; else if (t == &g_h_button) g_notify_button++; else notify_other(t);}
static unsigned ulTaskNotifyTake(int clear, unsigned wait) {
    (void)clear;
    if (g_notify_pass > 0) {g_notify_pass--; return 1;}
    if (g_wait_budget > 0 && wait != portMAX_DELAY) {
        g_wait_budget--;
        int64_t deadline = fake_now + (int64_t)wait * 1000;
        int notified = g_notify_button;
        if (g_wait_hook) {void (*h)(void) = g_wait_hook; g_wait_hook = NULL; h();}
        if (g_notify_button != notified) g_wait_wakes++;
        else if (fake_now < deadline) fake_now = deadline;
        return 1;
    }
    longjmp(loop_done, 1);
}

/* Zigbee lock + attribute setter */
static bool lock_ok = true, g_zb_locked = false;
static int model_on = 1, g_set_fail_on_off = 0, g_set_calls = 0;
static int g_onw_val[64], g_nonw = 0; static int64_t g_onw_at[64];     /* the On/Off writes since reset_case() (the bk probes) */
static int g_off_sets = 0, g_role_at_off = -1, g_model_on_at_role_store = -1;   /* ordering evidence for the role change */
static void (*g_unlock_hook)(void), (*g_acquire_hook)(void);
static bool esp_zigbee_lock_acquire(unsigned ticks) {
    (void)ticks; CRIT_CALL();
    if (g_acquire_hook) {void (*h)(void) = g_acquire_hook; g_acquire_hook = NULL; h();}   /* a frame handled while we waited for the lock */
    if (lock_ok) g_zb_locked = true;
    return lock_ok;
}
static void esp_zigbee_lock_release(void) {
    CRIT_CALL();
    g_zb_locked = false;
    if (g_unlock_hook) {void (*h)(void) = g_unlock_hook; g_unlock_hook = NULL; h();}   /* the stack runs a waiting frame */
}
static ezb_zcl_status_t ezb_zcl_set_attr_value(int ep, int cluster, int role, int attr, int manuf, void *v, bool check) {
    (void)ep; (void)role; (void)manuf; (void)check;
    CRIT_CALL();
    if (!g_zb_locked) g_call_in_crit += 1000;     /* attribute write without the Zigbee lock */
    g_set_calls++;
    if (cluster == 6 && attr == 0) {
        if (g_set_fail_on_off) return 0x86;       /* UNSUPPORTED_ATTRIBUTE: the model keeps its value */
        if (g_nonw < 64) {g_onw_val[g_nonw] = *(uint8_t *)v; g_onw_at[g_nonw] = fake_now;}   /* 0.6.0: every On/Off attribute write, in order, with its time */
        g_nonw++;
        model_on = *(uint8_t *)v;
        if (model_on == 0) { g_off_sets++; g_role_at_off = cfg_live.role; }
    }
    return EZB_ZCL_STATUS_SUCCESS;
}

/* NVS: handle 1 is the light's blob (c6bulb), handle 2 is the settings (c6cfg, a small typed key/value store); every other
 * namespace (c6wifi, c6claim, c6zb, ...) is a handle >= 10 into board_stubs.c's typed store (fs_*) */
static int fs_open(const char *ns, int mode, int *h);
static int fs_set(int h, const char *key, int type, uint32_t u, const void *b, size_t n);
static int fs_get(int h, const char *key, int type, uint32_t *u, void *b, size_t *n);
static int fs_commit(int h);
static void ctx_check(void);
static int save_error = 0, save_calls = 0, commit_calls = 0;
static void (*g_commit_hook)(void);
static uint8_t g_blob[64]; static size_t g_blob_n;
static uint8_t g_restore_blob[64]; static size_t g_restore_n;
static struct {char key[16]; int type; uint32_t u; char s[200];} g_kv[16];     /* type 0 free, 1 u8, 2 u32, 3 str */
static int g_cfg_ns = 0, g_cfg_open_err = 0, g_cfg_get_err = 0, g_cfg_set_err = 0, g_cfg_commit_err = 0;
static int g_cfg_sets = 0, g_cfg_commits = 0, g_cfg_opens = 0;
static int g_fail_set_at = 0;                      /* fail the n-th successful-so-far set once (ESP_ERR_NVS_NOT_ENOUGH_SPACE), then behave */
static char g_cfg_set_keys[200];
static int kv_slot(const char *key, bool create) {
    for (int i = 0; i < 16; i++) if (g_kv[i].type && !strcmp(g_kv[i].key, key)) return i;
    if (create) for (int i = 0; i < 16; i++) if (!g_kv[i].type) {snprintf(g_kv[i].key, sizeof g_kv[i].key, "%s", key); return i;}
    return -1;
}
static void kv_clear(void) {memset(g_kv, 0, sizeof g_kv); g_cfg_ns = 0; g_cfg_open_err = g_cfg_get_err = g_cfg_set_err = g_cfg_commit_err = 0; g_cfg_sets = g_cfg_commits = g_cfg_opens = 0; g_cfg_set_keys[0] = 0; g_fail_set_at = 0;}
static void kv_put_u8(const char *k, uint8_t v) {int i = kv_slot(k, true); g_kv[i].type = 1; g_kv[i].u = v; g_cfg_ns = 1;}
static void kv_put_u32(const char *k, uint32_t v) {int i = kv_slot(k, true); g_kv[i].type = 2; g_kv[i].u = v; g_cfg_ns = 1;}
static void kv_put_str(const char *k, const char *v) {int i = kv_slot(k, true); g_kv[i].type = 3; snprintf(g_kv[i].s, sizeof g_kv[i].s, "%s", v); g_cfg_ns = 1;}
static bool kv_u(const char *k, uint32_t *out) {int i = kv_slot(k, false); if (i < 0 || g_kv[i].type == 3) return false; *out = g_kv[i].u; return true;}
static const char *kv_s(const char *k) {int i = kv_slot(k, false); return i >= 0 && g_kv[i].type == 3 ? g_kv[i].s : NULL;}
static void (*g_cv_nvs_open_hook)(void);             /* one-shot task preemption before NVS opens */
static int nvs_open(const char *ns, int mode, int *h) {
    CRIT_CALL(); ctx_check();
    if (g_cv_nvs_open_hook) { void (*f)(void) = g_cv_nvs_open_hook; g_cv_nvs_open_hook = NULL; f(); }
    if (strcmp(ns, "c6cfg") && strcmp(ns, "c6bulb")) return fs_open(ns, mode, h);
    if (!strcmp(ns, "c6cfg")) {
        g_cfg_opens++; *h = 2;
        if (g_cfg_open_err) return g_cfg_open_err;
        if (mode == NVS_READONLY && !g_cfg_ns) return ESP_ERR_NVS_NOT_FOUND;   /* a namespace nobody has written */
        if (mode == NVS_READWRITE) g_cfg_ns = 1;
        return 0;
    }
    *h = 1; save_calls++; return save_error;
}
static int nvs_set_blob(int h, const char *key, const void *v, size_t n) {if (h >= 10) return fs_set(h, key, 4, 0, v, n); (void)key; CRIT_CALL(); g_blob_n = n; memcpy(g_blob, v, n < sizeof g_blob ? n : sizeof g_blob); return save_error;}
static int nvs_get_blob(int h, const char *key, void *v, size_t *n) {if (h >= 10) return fs_get(h, key, 4, NULL, v, n); (void)key; CRIT_CALL(); if (!g_restore_n) return -1; if (*n > g_restore_n) *n = g_restore_n; memcpy(v, g_restore_blob, *n); return 0;}
static int cfg_set_common(const char *key, int type, uint32_t u, const char *s) {
    CRIT_CALL(); ctx_check();
    if (g_cfg_set_err) return g_cfg_set_err;
    if (g_fail_set_at && g_cfg_sets + 1 == g_fail_set_at) {g_fail_set_at = 0; return 0x1105;}
    int i = kv_slot(key, true); if (i < 0) return ESP_FAIL;
    g_kv[i].type = type; g_kv[i].u = u; if (s) snprintf(g_kv[i].s, sizeof g_kv[i].s, "%s", s);
    g_cfg_sets++; size_t n = strlen(g_cfg_set_keys); snprintf(g_cfg_set_keys + n, sizeof g_cfg_set_keys - n, "%s%s", n ? " " : "", key);
    if (!strcmp(key, "role")) g_model_on_at_role_store = model_on;
    return 0;
}
static int nvs_set_u8(int h, const char *key, uint8_t v) {if (h >= 10) return fs_set(h, key, 1, v, NULL, 0); return cfg_set_common(key, 1, v, NULL);}
static int nvs_set_u32(int h, const char *key, uint32_t v) {if (h >= 10) return fs_set(h, key, 2, v, NULL, 0); return cfg_set_common(key, 2, v, NULL);}
static int nvs_set_str(int h, const char *key, const char *v) {if (h >= 10) return fs_set(h, key, 3, 0, v, strlen(v) + 1); return cfg_set_common(key, 3, 0, v);}
static int cfg_get_common(const char *key, int type, uint32_t *u, char *s, size_t *n) {
    CRIT_CALL(); ctx_check();
    if (g_cfg_get_err) return g_cfg_get_err;
    int i = kv_slot(key, false); if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (g_kv[i].type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (type == 3) {size_t need = strlen(g_kv[i].s) + 1; if (*n < need) {*n = need; return ESP_ERR_NVS_INVALID_LENGTH;} memcpy(s, g_kv[i].s, need); *n = need; return 0;}
    *u = g_kv[i].u; return 0;
}
static int nvs_get_u8(int h, const char *key, uint8_t *v) {uint32_t u = 0; int e = h >= 10 ? fs_get(h, key, 1, &u, NULL, NULL) : cfg_get_common(key, 1, &u, NULL, NULL); if (!e) *v = (uint8_t)u; return e;}
static int nvs_get_u32(int h, const char *key, uint32_t *v) {if (h >= 10) return fs_get(h, key, 2, v, NULL, NULL); return cfg_get_common(key, 2, v, NULL, NULL);}
static int nvs_get_str(int h, const char *key, char *s, size_t *n) {if (h >= 10) return fs_get(h, key, 3, NULL, s, n); return cfg_get_common(key, 3, NULL, s, n);}
static int nvs_commit(int h) {
    CRIT_CALL(); ctx_check();
    if (h >= 10) return fs_commit(h);
    if (h == 2) {g_cfg_commits++; return g_cfg_commit_err;}
    commit_calls++; if (g_commit_hook) {void (*c)(void) = g_commit_hook; g_commit_hook = NULL; c();} return save_error;
}
static void nvs_close(int h) {(void)h; CRIT_CALL(); ctx_check();}
static const char *esp_err_to_name(int err) {(void)err; return "stub";}

/* HTTP server side (handlers called directly) */
bool c6_base_query(httpd_req_t *r, const char *key, char *v, size_t n) {
    const char *s = !strcmp(key, "remove") ? r->remove : !strcmp(key, "on") ? r->on : !strcmp(key, "level") ? r->level : !strcmp(key, "ct") ? r->ct : !strcmp(key, "min") ? r->min : !strcmp(key, "max") ? r->max : !strcmp(key, "s") ? r->s : NULL;
    if (!s) return false;
    snprintf(v, n, "%s", s);
    return true;
}
static int httpd_resp_set_status(httpd_req_t *r, const char *s) {CRIT_CALL(); r->status_code = atoi(s); return 0;}
static int httpd_resp_set_type(httpd_req_t *r, const char *s) {CRIT_CALL(); snprintf(r->type, sizeof r->type, "%s", s); return 0;}
static int httpd_resp_sendstr(httpd_req_t *r, const char *s) {CRIT_CALL(); r->code = r->status_code ? r->status_code : 200; snprintf(r->response, sizeof r->response, "%s", s); r->resp_len = (int)strlen(s); resp_note(r, s, (long)strlen(s)); return 0;}
static int httpd_resp_send_err(httpd_req_t *r, int code, const char *s) {httpd_resp_sendstr(r, s); r->code = code; resp_note(r, s, (long)strlen(s)); return 0;}
#define HTTPD_SOCK_ERR_TIMEOUT (-3)
static void (*g_recv_hook)(httpd_req_t *r); static int g_recv_hook_at = 0;     /* 0.6.0: runs once, inside the n-th httpd_req_recv (another request arriving mid-body) */
static int httpd_req_recv(httpd_req_t *r, char *buf, size_t n) {
    CRIT_CALL(); r->recvs++;
    if (g_recv_hook && r->recvs == g_recv_hook_at) {void (*h)(httpd_req_t *) = g_recv_hook; g_recv_hook = NULL; h(r);}
    if (r->recv_fail) return -1;
    size_t left = r->body_len - r->body_pos; if (n > left) n = left;
    memcpy(buf, r->body + r->body_pos, n); r->body_pos += n; return (int)n;
}

/* HTTP client side: webhook_post() itself is extracted verbatim, only the client library is stubbed */
typedef struct {const char *url; int method; int timeout_ms;} esp_http_client_config_t;
#define HTTP_METHOD_POST 1
typedef struct {int dummy;} http_client_t;
typedef http_client_t *esp_http_client_handle_t;
static http_client_t g_client; static char g_body[256];
#define MAXPOSTS 16
static char g_posts[MAXPOSTS][256]; static int g_nposts = 0, g_press_posts = 0, g_release_posts = 0, g_http_inits = 0;
static char g_last_url[200];
static int g_post_fail_n = 0;            /* the next N posts fail (transport error) */
static void (*g_press_hook)(void);       /* runs while a press is being posted, like another task would */
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {CRIT_CALL(); g_http_inits++; snprintf(g_last_url, sizeof g_last_url, "%s", c->url); return &g_client;}
static void esp_http_client_set_header(esp_http_client_handle_t h, const char *k, const char *v) {(void)h; (void)k; (void)v;}
static void esp_http_client_set_post_field(esp_http_client_handle_t h, const char *s, int n) {(void)h; snprintf(g_body, sizeof g_body, "%.*s", n, s);}
static esp_err_t esp_http_client_perform(esp_http_client_handle_t h) {
    (void)h;
    CRIT_CALL();
    if (g_nposts < MAXPOSTS) snprintf(g_posts[g_nposts++], sizeof g_posts[0], "%s", g_body);
    if (strstr(g_body, "\"press\"")) {
        g_press_posts++;
        if (g_press_hook) {void (*p)(void) = g_press_hook; g_press_hook = NULL; p();}
    }
    if (strstr(g_body, "\"release\"")) g_release_posts++;
    if (g_post_fail_n > 0) {g_post_fail_n--; return -1;}
    return ESP_OK;
}
static int esp_http_client_get_status_code(esp_http_client_handle_t h) {(void)h; return 200;}
static void esp_http_client_cleanup(esp_http_client_handle_t h) {(void)h;}

@@HEADERS@@
@@BOARD_STUBS@@
/* ===== EXTRACTED FIRMWARE (verbatim, by name; see extract.txt) ===== */
'''

main = r'''
/* ===== HARNESS ===== */
static int fails_in_probe = 0;
#define CHECK(c) do { if (!(c)) { fails_in_probe++; printf("    CHECK FAILED (line %d): %s\n", __LINE__, #c); } } while (0)
static struct {const char *id, *name; int pass;} results[200]; static int nresults = 0;
static void verdict(const char *id, const char *name) {
    printf("  -> %s  %s %s\n", fails_in_probe ? "FAIL" : "PASS", id, name);
    results[nresults].id = id; results[nresults].name = name; results[nresults].pass = !fails_in_probe; nresults++;
    fails_in_probe = 0;
}

#define WH_TEST "http://127.0.0.1:8123/api/webhook/test_zq9"      /* a made-up webhook; "zq9" must never reach a log or a JSON */
static int g_mode_role = CFG_ROLE_NONE;                           /* the role reset_case() starts a probe in */

static void reset_case(void) {
    s_light = (light_state_t){.seen = 1u << L_ON, .val = {[L_ON] = 1}};
    s_hub = s_light; s_zb_up = true; s_held = false; s_delivering = false;
    s_press_at_us = s_press_first_us = s_off_at_us = s_released_at_us = s_hub_cmd_us = s_push_due_us = 0;
    s_light_changed_us = 0; s_light_gen = s_saved_gen = 0; s_release_by = NULL; s_restored_at_us = 0;
#ifdef HARNESS_BUTTON_GENERATION
    s_button_generation = 0; s_held_at_us = 0;
#endif
    s_restore_guard = 0; s_restart_for_stall = false; s_push_task = &g_h_push; s_button_task = &g_h_button;
    g_notify_push = g_notify_button = g_notify_pass = 0;
    cfg_live = (cfg_t){.role = (uint8_t)g_mode_role, .settle_ms = CONFIG_C6_BUTTON_SETTLE_MS, .rearm_ms = CONFIG_C6_BUTTON_REARM_MS,
                       .off_after_s = HARNESS_OFF_AFTER_S, .rx_wd_s = CONFIG_C6_ZB_RX_WATCHDOG_S, .hue = true};
    snprintf(cfg_live.webhook, sizeof cfg_live.webhook, "%s", WH_TEST);
    kv_clear(); g_kconfig_ieee = ""; g_kconfig_ext_pan[0] = 0; g_kconfig_ota_token = ""; board_reset(); claim_now();
    g_off_sets = 0; g_role_at_off = g_model_on_at_role_store = -1; g_nlogs = 0; g_http_inits = 0;
    g_nonw = 0; g_recv_hook = NULL; g_recv_hook_at = 0;
    lock_ok = true; g_zb_locked = false; g_unlock_hook = NULL; g_acquire_hook = NULL; g_restore_n = 0; g_set_calls = 0; g_commit_hook = NULL; g_press_hook = NULL;
    g_set_fail_on_off = 0; g_post_fail_n = 0; report_update_err = 0;
    fake_now = 10000000; model_on = 1;
    save_calls = commit_calls = save_error = 0; g_nposts = g_press_posts = g_release_posts = 0; g_delays = 0;
    g_delay_hook = g_wait_hook = NULL; g_wait_budget = g_wait_wakes = 0;
    reports_left = 1; report_starts = 0; g_aps_tx = 0;
}
static void run_button(void) {if (!setjmp(loop_done)) button_task(NULL);}
static void want_off(int want[L_COUNT]) {for (int i = 0; i < L_COUNT; i++) want[i] = -1; want[L_ON] = 0;}

static void hub_send(uint16_t cluster, const uint8_t *p, uint16_t n, uint8_t status, int mode, uint16_t src) {
    ezb_apsde_data_ind_t ind = {.status = status, .profile_id = 0x104, .cluster_id = cluster, .asdu = (uint8_t *)p,
                                .asdu_length = n, .src_address = {.addr_mode = mode, .u.short_addr = src}};
    hub_tap(&ind);
}
static void ctl(uint16_t cluster, const uint8_t *p, uint16_t n) {hub_send(cluster, p, n, 0, EZB_ADDR_MODE_SHORT, 0x0000);}

/* the stack handling a Zigbee controller On after our Off: attribute, on_set_attr's shadow update, and the tap */
static void inject_hub_on(void) {
    static const uint8_t f[] = {1, 1, 1};
    ctl(6, f, sizeof f);
    int64_t now = esp_timer_get_time();
    model_on = 1;
    portENTER_CRITICAL(&s_light_mux);
    s_light.val[L_ON] = 1; s_light.seen |= 1u << L_ON; light_mark_changed_locked(now, "zigbee");
    portEXIT_CRITICAL(&s_light_mux);
}
static void inject_hub_off(void) {
    static const uint8_t f[] = {1, 1, 0};
    ctl(6, f, sizeof f);
    int64_t now = esp_timer_get_time();
    model_on = 0;
    portENTER_CRITICAL(&s_light_mux);
    s_light.val[L_ON] = 0; s_light.seen |= 1u << L_ON; light_mark_changed_locked(now, "zigbee");
    portEXIT_CRITICAL(&s_light_mux);
}
static int64_t g_off_time;
static void inject_hub_off_then_on(void) {      /* the touchscreen clears, and 100 ms later a new press starts */
    g_off_time = esp_timer_get_time();
    inject_hub_off();
    fake_now += 100000;
    inject_hub_on();
}
static void restore_in_signal_handler(void) {g_zb_locked = true; light_restore(); g_zb_locked = false;}   /* the stack's handler holds its lock */
static void inject_level_change(void) {     /* a Zigbee callback changing the light mid-save */
    portENTER_CRITICAL(&s_light_mux);
    s_light.val[L_LEVEL] = 77; s_light.seen |= 1u << L_LEVEL; light_mark_changed_locked(esp_timer_get_time(), "zigbee");
    portEXIT_CRITICAL(&s_light_mux);
}
static void client_off_during_press_post(void) {
    int want[L_COUNT]; want_off(want); char msg[120];
    CHECK(light_apply(want, "API-SET", msg, sizeof msg, true, NULL) >= 0);
}
static bool post_has(int i, const char *s) {return i >= 0 && i < g_nposts && strstr(g_posts[i], s) != NULL;}

/* ---- the critical-section sanity probe (z): runs at the end of old and of new ---- */
static void sanity_probe(void) {
    printf("critical-section sanity over the whole run: nested=%d, zigbee/HTTP/NVS/attribute call inside a spinlock section or attribute write without the Zigbee lock=%d, unbalanced exits=%d, ended outside a section=%d\n",
           g_nested, g_call_in_crit, g_crit_unbalanced, g_crit == 0);
    if (g_nested || g_call_in_crit || g_crit_unbalanced || g_crit) fails_in_probe++;
    verdict("z", "no nested spinlock sections, no Zigbee/HTTP/NVS call under the spinlock, attribute writes under the Zigbee lock");
}

@@BOARD_PROBES@@
/* ===================================================================== the 21 probes of 0.3.24 */
static void run_old(void) {
    puts("==== The nine button probes, on 0.3.24 ====");

    /* 1. timeout, lock failure */
    reset_case(); s_held = true; s_off_at_us = fake_now; lock_ok = false; run_button();
    printf("1 timeout lock failure: held=%d, timer_in=%lld ms, model_on=%d, release_posts=%d\n", s_held, (long long)((s_off_at_us - fake_now) / 1000), model_on, g_release_posts);
    CHECK(s_held); CHECK(s_off_at_us == fake_now + BUTTON_RETRY_US); CHECK(model_on == 1); CHECK(g_release_posts == 0);
    lock_ok = true; fake_now += BUTTON_RETRY_US; run_button();
    printf("  then lock back, retry fires: model_on=%d, held=%d, timer=%lld, release_posts=%d (%s)\n", model_on, s_held, (long long)s_off_at_us, g_release_posts, g_nposts ? g_posts[0] : "-");
    CHECK(model_on == 0 && !s_held && !s_off_at_us && g_release_posts == 1 && post_has(0, "\"by\":\"timeout\""));
    verdict("1", "timeout lock failure: press held, retry timer, no release webhook");

    /* 2. late release, lock failure */
    reset_case(); s_released_at_us = 7000000; s_press_first_us = 7100000; s_press_at_us = 7100000; lock_ok = false; run_button();
    printf("2 late lock failure: held=%d, timer_in=%lld ms, press_due=%lld, model_on=%d, press_posts=%d\n", s_held, (long long)((s_off_at_us - fake_now) / 1000), (long long)s_press_at_us, model_on, g_press_posts);
    CHECK(!s_held); CHECK(s_off_at_us == fake_now + BUTTON_RETRY_US); CHECK(!s_press_at_us); CHECK(model_on == 1); CHECK(g_press_posts == 0);
    lock_ok = true; fake_now += BUTTON_RETRY_US; run_button();
    printf("  then lock back, retry fires: model_on=%d, timer=%lld, posts=%d\n", model_on, (long long)s_off_at_us, g_nposts);
    CHECK(model_on == 0 && !s_off_at_us && g_nposts == 0);
    verdict("2", "late lock failure: retry timer set, retried to Off, no webhook");

    /* 3. Zigbee controller On injected at lock release */
    reset_case(); s_held = true; s_off_at_us = fake_now + 120000000; g_unlock_hook = inject_hub_on;
    { int want[L_COUNT]; char msg[120]; want_off(want); CHECK(light_apply(want, "API-SET", msg, sizeof msg, true, NULL) >= 0); }
    printf("3 Zigbee controller On at lock release: model_on=%d, shadow_on=%u, hub_on=%u, held=%d, press_due=%s, off_due=%lld, gen=%u saved=%u\n",
           model_on, s_light.val[L_ON], s_hub.val[L_ON], s_held, s_press_at_us ? "queued" : "0", (long long)s_off_at_us, s_light_gen, s_saved_gen);
    CHECK(model_on == 1 && s_light.val[L_ON] == 1 && s_hub.val[L_ON] == 1);     /* shared state agrees with the model */
    CHECK(!s_held && !s_off_at_us);                                                /* our Off released the old press */
    CHECK(s_press_at_us != 0);                                                     /* the Zigbee controller's On was not erased by a stale write */
    CHECK(s_light_gen != s_saved_gen);                                             /* and is dirty for the next save */
    run_button();                                    /* not settled yet: nothing happens */
    CHECK(model_on == 1 && g_nposts == 0);
    fake_now += 2100000; run_button();               /* settled, but within REARM of our release: BUTTON-LATE */
    printf("  after settle (On arrived 0 ms after the release, inside the %d ms re-arm window): model_on=%d, posts=%d\n", CONFIG_C6_BUTTON_REARM_MS, model_on, g_nposts);
    CHECK(g_nposts == 0 && model_on == 0 && s_light.val[L_ON] == 0);
    verdict("3", "Zigbee controller On at lock release: model, shadow, Zigbee controller agree; press not clobbered");

    /* 4. NVS open fails once */
    reset_case(); portENTER_CRITICAL(&s_light_mux); light_mark_changed_locked(1000000, "zigbee"); portEXIT_CRITICAL(&s_light_mux);
    save_error = 1; light_save_if_settled(fake_now);
    int dirty_after_fail = s_light_gen != s_saved_gen;
    save_error = 0; light_save_if_settled(fake_now + 5000000);
    int dirty_after_ok = s_light_gen != s_saved_gen;
    light_save_if_settled(fake_now + 9000000);
    printf("4 NVS open fails once: save_attempts=%d, dirty_after_failure=%d, dirty_after_success=%d, third call attempts=%d\n", save_calls, dirty_after_fail, dirty_after_ok, save_calls - 2);
    CHECK(dirty_after_fail && save_calls == 2 && !dirty_after_ok);
    verdict("4", "NVS open fails once: second attempt, dirty clears only after success");

    /* 5. HTTP on=1 in button mode */
    reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0; model_on = 0;
    { httpd_req_t r = {.on = "1"}; CHECK(light_post(&r) == 0);
      printf("5 button HTTP on=1: model_on=%d, held=%d, press_due=%lld, off_due=%lld, response=%s", model_on, s_held, (long long)s_press_at_us, (long long)s_off_at_us, r.response);
      CHECK(!model_on && !s_held && !s_press_at_us && !s_off_at_us && strstr(r.response, "ignored"));
      r = (httpd_req_t){.on = "0"}; CHECK(light_post(&r) == 0 && strstr(r.response, "no press to release"));
      s_held = true; s_light.val[L_ON] = s_hub.val[L_ON] = 0; model_on = 0;                   /* even with a press held */
      r = (httpd_req_t){.on = "1"}; CHECK(light_post(&r) == 0 && strstr(r.response, "on=0 only") && !model_on && s_held); }
    verdict("5", "button on=1 rejected, model unchanged");

    /* 6. client release before press delivery (the press is still being posted) */
    reset_case(); s_press_at_us = s_press_first_us = 7000000; g_press_hook = client_off_during_press_post; run_button();
    printf("6 client release before press delivery: posts=%d [", g_nposts);
    for (int i = 0; i < g_nposts; i++) printf("%s%s", i ? " | " : "", g_posts[i]);
    printf("], model_on=%d, held=%d, delivering=%d, off_due=%lld\n", model_on, s_held, s_delivering, (long long)s_off_at_us);
    CHECK(g_nposts == 2 && post_has(0, "\"press\":1") && post_has(1, "\"release\":1") && post_has(1, "\"by\":\"api\""));
    CHECK(g_press_posts == 1 && g_release_posts == 1);
    CHECK(!model_on && !s_held && !s_delivering && !s_off_at_us && !s_release_by);
    verdict("6", "press webhook then one release webhook by api");

    /* 7. truncated MoveToLevelWithOnOff (decoder) */
    reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0;
    { static const uint8_t trunc[] = {1, 1, 4, 254}; ctl(8, trunc, sizeof trunc);
      printf("7 truncated MoveToLevelWithOnOff: hub_on=%u, hub_level_seen=%d, press_due=%lld\n", s_hub.val[L_ON], !!(s_hub.seen & (1u << L_LEVEL)), (long long)s_press_at_us);
      CHECK(!s_hub.val[L_ON] && !(s_hub.seen & (1u << L_LEVEL)) && !s_press_at_us && !s_hub_cmd_us);
      static const uint8_t full[] = {1, 1, 4, 254, 0, 0}; ctl(8, full, sizeof full);       /* control: the complete command works */
      CHECK(s_hub.val[L_ON] == 1 && s_hub.val[L_LEVEL] == 254 && s_press_at_us); }
    verdict("7", "truncated MoveToLevelWithOnOff ignored (complete one still accepted)");

    /* 8. manufacturer-specific frame (decoder) */
    reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0;
    { static const uint8_t vendor[] = {5, 0x34, 0x12, 1, 1}; ctl(6, vendor, sizeof vendor);
      printf("8 manufacturer 0x1234 cmd 1: hub_on=%u, press_due=%lld\n", s_hub.val[L_ON], (long long)s_press_at_us);
      CHECK(!s_hub.val[L_ON] && !s_press_at_us && !s_hub_cmd_us);
      static const uint8_t std_on[] = {1, 1, 1}; ctl(6, std_on, sizeof std_on);             /* control */
      CHECK(s_hub.val[L_ON] == 1 && s_press_at_us); }
    verdict("8", "manufacturer-specific On frame ignored (standard On still accepted)");

    /* 9. remove=1 */
    reset_case();
    { httpd_req_t rm = {.remove = "1"}; CHECK(report_post(&rm) == 0);
      printf("9 remove=1 with sole reporting entry: entries=%d, report_starts=%d, http=%d, response=%s", reports_left, report_starts, rm.code, rm.response);
      CHECK(reports_left == 1 && !report_starts && rm.code == 400);
      httpd_req_t ok = {0}; CHECK(report_post(&ok) == 0 && ok.code == 200 && report_starts == 1);      /* min/max setting still works */
      reports_left = 0; httpd_req_t none = {0}; CHECK(report_post(&none) == 0 && none.code == 500);
      reports_left = 1; report_starts = 0;
      httpd_req_t z = {.max = "0"}; CHECK(report_post(&z) == 0 && z.code == 400);
      httpd_req_t inv = {.min = "30", .max = "20"}; CHECK(report_post(&inv) == 0 && inv.code == 400);
      httpd_req_t good = {.min = "0", .max = "20"}; CHECK(report_post(&good) == 0 && good.code == 200);
      printf("  max=0 -> %d, min>max -> %d, 0..20 -> %d, report_starts=%d\n", z.code, inv.code, good.code, report_starts);
      CHECK(report_starts == 1); }
    verdict("9", "remove=1 rejected with 400, entry kept");

    puts("==== New probes ====");

    /* a. Zigbee controller change between a save's snapshot and its commit */
    reset_case(); portENTER_CRITICAL(&s_light_mux); light_mark_changed_locked(1000000, "zigbee"); portEXIT_CRITICAL(&s_light_mux);
    uint32_t gen0 = s_light_gen;
    g_commit_hook = inject_level_change; light_save_if_settled(fake_now);
    light_state_t b1; memcpy(&b1, g_blob, sizeof b1);
    int dirty = s_light_gen != s_saved_gen; uint32_t saved1 = s_saved_gen;
    int calls1 = save_calls; light_save_if_settled(fake_now);                          /* the new change is not settled yet */
    int calls_unsettled = save_calls - calls1;
    light_save_if_settled(fake_now + 4000000);
    light_state_t b2; memcpy(&b2, g_blob, sizeof b2);
    printf("a change between snapshot and commit: saved_gen=%u (snapshot gen %u, now %u), dirty=%d; first blob level_seen=%d, second blob level=%u; not-settled call saved=%d; clean after 2nd save=%d\n",
           saved1, gen0, s_light_gen, dirty, !!(b1.seen & (1u << L_LEVEL)), b2.val[L_LEVEL], calls_unsettled, s_light_gen == s_saved_gen);
    CHECK(dirty && saved1 == gen0 && !(b1.seen & (1u << L_LEVEL)) && calls_unsettled == 0);
    CHECK(save_calls == 2 && b2.val[L_LEVEL] == 77 && s_light_gen == s_saved_gen);
    verdict("a", "change mid-save stays dirty (generation check), saved next time");

    /* b. source address / APS status */
    reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0;
    { static const uint8_t on[] = {1, 1, 1};
      hub_send(6, on, sizeof on, 1, EZB_ADDR_MODE_SHORT, 0x0000);   /* APS status != 0 */
      int a = s_hub.val[L_ON] || s_press_at_us || s_hub_cmd_us;
      hub_send(6, on, sizeof on, 0, EZB_ADDR_MODE_SHORT, 0x1234);   /* another node */
      int b = s_hub.val[L_ON] || s_press_at_us || s_hub_cmd_us;
      hub_send(6, on, sizeof on, 0, EZB_ADDR_MODE_IEEE, 0x0000);    /* not a short address */
      int c = s_hub.val[L_ON] || s_press_at_us || s_hub_cmd_us;
      printf("b status!=0 acted=%d, src 0x1234 acted=%d, src IEEE-mode acted=%d\n", a, b, c);
      CHECK(!a && !b && !c);
      hub_send(6, on, sizeof on, 0, EZB_ADDR_MODE_SHORT, 0x0000);   /* control */
      CHECK(s_hub.val[L_ON] == 1 && s_press_at_us); }
    verdict("b", "non-coordinator source or APS status != 0 ignored");

    /* c. OnWithTimedOff 0x42 */
    { static const uint8_t accept_only_on[] = {1, 1, 0x42, 0x01, 0x10, 0x00, 0x20, 0x00};
      static const uint8_t unconditional[]  = {1, 1, 0x42, 0x00, 0x10, 0x00, 0x20, 0x00};
      static const uint8_t short42[]        = {1, 1, 0x42, 0x01};
      reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0; ctl(6, accept_only_on, sizeof accept_only_on);
      int off_ok = !s_hub.val[L_ON] && !s_press_at_us && !s_hub_cmd_us;
      reset_case(); ctl(6, accept_only_on, sizeof accept_only_on);
      int on_ok = s_hub.val[L_ON] == 1;
      reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0; ctl(6, unconditional, sizeof unconditional);
      int uncond_ok = s_hub.val[L_ON] == 1 && s_press_at_us;
      reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0; ctl(6, short42, sizeof short42);
      int short_ok = !s_hub.val[L_ON] && !s_press_at_us;
      printf("c 0x42 accept-only-when-on: while off discarded=%d, while on stays on=%d; unconditional while off turns on=%d; truncated ignored=%d\n", off_ok, on_ok, uncond_ok, short_ok);
      CHECK(off_ok && on_ok && uncond_ok && short_ok); }
    verdict("c", "OnWithTimedOff: accept-only-when-on honoured");

    /* d. toggle */
    { static const uint8_t tog[] = {1, 1, 2};
      reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0; ctl(6, tog, sizeof tog);
      int t1 = s_hub.val[L_ON] == 1 && s_press_at_us;
      ctl(6, tog, sizeof tog);
      int t2 = s_hub.val[L_ON] == 0 && !s_press_at_us;
      reset_case(); s_held = true; s_off_at_us = fake_now + 1; ctl(6, tog, sizeof tog);        /* toggling a held press releases it */
      int t3 = s_hub.val[L_ON] == 0 && !s_held && !s_off_at_us && s_release_by && !strcmp(s_release_by, "zigbee");
      reset_case(); s_hub.seen = 0; s_light.val[L_ON] = 0; ctl(6, tog, sizeof tog);              /* no Zigbee controller history yet: starts from s_light */
      int t4 = s_hub.val[L_ON] == 1;
      printf("d toggle: off->on=%d, on->off=%d, toggle of held press -> release by Zigbee controller=%d, first command starts from s_light=%d\n", t1, t2, t3, t4);
      CHECK(t1 && t2 && t3 && t4); }
    verdict("d", "toggle (0x02) still resolves correctly");

    /* e. attribute setter failure for On/Off in a timeout */
    reset_case(); s_held = true; s_off_at_us = fake_now; g_set_fail_on_off = 1;
    run_button();
    printf("e setter fails in timeout: held=%d, timer_in=%lld ms, model_on=%d, shadow_on=%u, hub_on=%u, release_posts=%d\n", s_held, (long long)((s_off_at_us - fake_now) / 1000), model_on, s_light.val[L_ON], s_hub.val[L_ON], g_release_posts);
    CHECK(s_held && s_off_at_us == fake_now + BUTTON_RETRY_US && model_on == 1 && s_light.val[L_ON] == 1 && s_hub.val[L_ON] == 1 && g_release_posts == 0);
    { int want[L_COUNT]; char msg[120]; want_off(want); int rc = light_apply(want, "T", msg, sizeof msg, true, NULL);
      printf("  light_apply directly: rc=%d msg=\"%s\"\n", rc, msg); CHECK(rc == -2 && strstr(msg, "on=0(0x86)")); }
    { httpd_req_t r = {.on = "0"}; light_post(&r);                                     /* a client's own Off while the setter refuses */
      printf("  client on=0 while the setter refuses: http=%d held=%d (%s)", r.code, s_held, r.response);
      CHECK(r.code == 500 && s_held && model_on == 1 && g_release_posts == 0); }
    g_set_fail_on_off = 0; fake_now += BUTTON_RETRY_US; run_button();
    printf("  setter back, retry fires: model_on=%d, held=%d, release_posts=%d\n", model_on, s_held, g_release_posts);
    CHECK(model_on == 0 && !s_held && g_release_posts == 1 && post_has(g_nposts - 1, "\"by\":\"timeout\""));
    verdict("e", "setter failure in a timeout: press held, retry scheduled, no release until it works");

    puts("==== Race probes (N1, N2, N5) ====");

    /* N1. timeout's apply fails on the lock; a Zigbee controller Off is processed before the re-arm */
    reset_case(); s_held = true; s_off_at_us = fake_now; lock_ok = false; g_acquire_hook = inject_hub_off;
    int64_t t_off = fake_now;
    run_button();
    printf("N1 timeout apply fails on lock, Zigbee controller Off meanwhile: held=%d, retry_timer=%lld, released_at_moved=%d, posts=%d [%s], model_on=%d\n",
           s_held, (long long)s_off_at_us, s_released_at_us != t_off, g_nposts, g_nposts ? g_posts[0] : "-", model_on);
    CHECK(!s_held && !s_off_at_us && s_released_at_us == t_off);
    CHECK(g_nposts == 1 && post_has(0, "\"by\":\"zigbee\"") && !post_has(0, "timeout"));
    lock_ok = true; fake_now += 5500000; int sets0 = g_set_calls; run_button();       /* well past where a retry would have fired */
    printf("  5.5 s later: retry ran=%d, released_at_moved=%d, posts=%d\n", g_set_calls != sets0, s_released_at_us != t_off, g_nposts);
    CHECK(g_set_calls == sets0 && s_released_at_us == t_off && g_nposts == 1);
    { static const uint8_t on[] = {1, 1, 1};
      ctl(6, on, sizeof on); fake_now += 2100000; run_button();                          /* a genuine On, 5.5 s after the Zigbee controller Off */
      printf("  genuine Zigbee controller On 5.5 s after the Off: press_posts=%d, model_on=%d, held=%d\n", g_press_posts, model_on, s_held);
      CHECK(g_press_posts == 1 && s_held); }
    /* literal reading: the On only 1 s after the Zigbee controller Off (inside the 3 s re-arm window) */
    reset_case(); s_held = true; s_off_at_us = fake_now; lock_ok = false; g_acquire_hook = inject_hub_off; run_button();
    { static const uint8_t on[] = {1, 1, 1}; lock_ok = true; fake_now += 1000000; inject_hub_on(); (void)on; fake_now += 2100000; run_button();
      printf("  [observation, not a check] Zigbee controller On 1 s after the Zigbee controller Off: press_posts=%d, model_on=%d (inside the %d ms re-arm window: BUTTON-LATE by design)\n", g_press_posts, model_on, CONFIG_C6_BUTTON_REARM_MS); }
    /* a no-op Off (light already off, nothing held) must not restart the re-arm window; a real one does */
    reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0; model_on = 0; s_released_at_us = 5000000;
    { int want[L_COUNT]; char msg[60]; bool rel = true; want_off(want);
      int rc = light_apply(want, "T", msg, sizeof msg, false, &rel);
      printf("  no-op Off on an off light: rc=%d, released=%d, released_at_moved=%d\n", rc, rel, s_released_at_us != 5000000);
      CHECK(rc >= 0 && !rel && s_released_at_us == 5000000);
      s_light.val[L_ON] = s_hub.val[L_ON] = 1; model_on = 1; want_off(want);
      rc = light_apply(want, "T", msg, sizeof msg, false, &rel);
      printf("  real Off on an on light, nothing held: rc=%d, released=%d, released_at_moved=%d\n", rc, rel, s_released_at_us != 5000000);
      CHECK(rc >= 0 && !rel && s_released_at_us == fake_now && !model_on); }
    verdict("N1", "no retry armed after a Zigbee controller Off; released_at untouched; later genuine On is pushed");

    /* N2. timeout decided while held; a Zigbee controller Off releases the press before light_apply takes the lock */
    reset_case(); s_held = true; s_off_at_us = fake_now; g_acquire_hook = inject_hub_off; run_button();
    printf("N2 Zigbee controller Off before the timeout's lock: posts=%d [", g_nposts);
    for (int i = 0; i < g_nposts; i++) printf("%s%s", i ? " | " : "", g_posts[i]);
    printf("], held=%d, model_on=%d, off_due=%lld\n", s_held, model_on, (long long)s_off_at_us);
    CHECK(g_nposts == 1 && g_release_posts == 1 && post_has(0, "\"by\":\"zigbee\"") && !post_has(0, "timeout"));
    CHECK(!s_held && !model_on && !s_off_at_us && !s_release_by);
    verdict("N2", "exactly one release webhook, by Zigbee controller, no timeout push");

    /* N5. client on=0 passes the early held check; a Zigbee controller Off (and a new press) comes before light_apply */
    reset_case(); s_held = true; s_off_at_us = fake_now + 120000000; g_acquire_hook = inject_hub_off_then_on;
    { httpd_req_t r = {.on = "0"}; int rc = light_post(&r);
      printf("N5 client on=0 raced by Zigbee controller Off + new press: rc=%d http=%d response=%s", rc, r.code, r.response);
      printf("   held=%d, press_due=%s, off_due=%lld, model_on=%d, released_at_is_hub_off=%d, set_calls=%d, posts=%d\n", s_held, s_press_at_us ? "queued" : "0", (long long)s_off_at_us, model_on, s_released_at_us == g_off_time, g_set_calls, g_nposts);
      CHECK(rc == 0 && r.code == 200 && strstr(r.response, "ignored: no press to release"));
      CHECK(!s_held && s_press_at_us == g_off_time + 100000 && s_press_first_us == g_off_time + 100000 && !s_off_at_us);
      CHECK(model_on == 1 && s_light.val[L_ON] == 1 && s_released_at_us == g_off_time && g_set_calls == 0);
      CHECK(g_nposts == 0 && s_release_by && !strcmp(s_release_by, "zigbee")); }          /* only the Zigbee controller's own release is pending */
    verdict("N5", "light_apply -3 -> ignored: no press to release; pending new press untouched");

    puts("==== Extra checks ====");

    /* f. delivery policy: press never retried, release retried */
    reset_case(); s_press_at_us = s_press_first_us = 7000000; g_post_fail_n = 5; run_button();
    int press_attempts = g_press_posts; int rel_before = g_release_posts;
    reset_case(); g_post_fail_n = 2; s_release_by = "zigbee"; run_button();
    printf("f press post fails: attempts=%d; release post fails twice: attempts=%d, delays=%d\n", press_attempts, g_release_posts, g_delays);
    CHECK(press_attempts == 1 && rel_before == 0 && g_release_posts == 3 && g_delays == 2);
    verdict("f", "press never retried; release retried twice more");

    /* g. restore guard */
    reset_case(); g_reset_reason = ESP_RST_POWERON; s_restore_guard = RESTORE_MAGIC; restore_guard_at_boot(); int g1 = s_restore_guard == 0;
    reset_case(); g_reset_reason = ESP_RST_PANIC; s_restore_guard = RESTORE_MAGIC; restore_guard_at_boot(); int g2 = s_restore_guard == RESTORE_MAGIC;
    reset_case(); g_reset_reason = ESP_RST_TASK_WDT; s_restore_guard = RESTORE_MAGIC; restore_guard_at_boot(); int g3 = s_restore_guard == RESTORE_MAGIC;
    reset_case(); g_reset_reason = ESP_RST_SW; s_restore_guard = RESTORE_MAGIC; restore_guard_at_boot(); int g4 = s_restore_guard == RESTORE_MAGIC;   /* a stall restart kept it */
    reset_case(); s_restore_guard = RESTORE_MAGIC; g_shutdown_handler(); int g5 = s_restore_guard == 0;           /* deliberate restart */
    reset_case(); s_restore_guard = RESTORE_MAGIC; s_restart_for_stall = true; g_shutdown_handler(); int g6 = s_restore_guard == RESTORE_MAGIC;
    printf("g restore guard: poweron clears=%d, panic keeps=%d, task-wdt keeps=%d, sw boot keeps (stall)=%d, deliberate restart clears=%d, stall restart keeps=%d\n", g1, g2, g3, g4, g5, g6);
    CHECK(g1 && g2 && g3 && g4 && g5 && g6);
    verdict("g", "restore guard cleared on deliberate restarts / power-on, kept for crashes and stalls");

    /* r. light_restore bumps the generation, so a save under way stays stale */
    reset_case(); portENTER_CRITICAL(&s_light_mux); light_mark_changed_locked(1000000, "zigbee"); portEXIT_CRITICAL(&s_light_mux);
    { light_state_t saved = {.seen = (1u << L_ON) | (1u << L_LEVEL), .val = {[L_ON] = 1, [L_LEVEL] = 50}}; memcpy(g_restore_blob, &saved, sizeof saved); g_restore_n = sizeof saved; }
    g_commit_hook = restore_in_signal_handler; light_save_if_settled(fake_now);
    int stale = s_light_gen != s_saved_gen;
    printf("r restore during a save: restored level=%u, on=%u (button mode: released), dirty_after_save=%d\n", s_light.val[L_LEVEL], s_light.val[L_ON], stale);
    CHECK(stale && s_light.val[L_LEVEL] == 50 && s_light.val[L_ON] == 0);
    light_save_if_settled(fake_now + 5000000);
    { light_state_t b; memcpy(&b, g_blob, sizeof b);
      CHECK(s_light_gen == s_saved_gen && b.val[L_LEVEL] == 50); }
    verdict("r", "light_restore during a save: the in-flight stale save stays dirty and is redone");
    sanity_probe();
}

/* ===================================================================== the one normal-mode probe */
static void run_old_normal(void) {
    /* role mirror: only light_post's -2 handling */
    reset_case(); s_light.val[L_ON] = s_hub.val[L_ON] = 0; model_on = 0; g_set_fail_on_off = 1;
    { httpd_req_t r = {.on = "1"}; light_post(&r);
      printf("normal mode, setter refuses On: http=%d response=%s", r.code, r.response);
      CHECK(r.code == 200 && strstr(r.response, "on=1(0x86)")); }
    verdict("n", "normal mode: -2 answers 200 with each attribute's status");
}

/* ===================================================================== the 0.4.0 probes */
static const uint8_t HUB_ON[] = {1, 1, 1};
static void set_role(int role) {portENTER_CRITICAL(&s_light_mux); cfg_live.role = (uint8_t)role; portEXIT_CRITICAL(&s_light_mux);}
static void light_state(int on) {s_light.seen |= 1u << L_ON; s_light.val[L_ON] = s_hub.val[L_ON] = (uint16_t)on; s_hub.seen |= 1u << L_ON; model_on = on;}
static httpd_req_t g_req;
static int put_body(const char *body) {g_req = (httpd_req_t){0}; g_req.body = body; g_req.body_len = g_req.content_len = strlen(body); settings_put(&g_req); return g_req.code;}
static int get_settings(void) {g_req = (httpd_req_t){0}; settings_get(&g_req); return g_req.code;}
static bool resp_has(const char *s) {return strstr(g_req.response, s) != NULL;}
static bool nothing_written(void) {return g_cfg_sets == 0 && g_cfg_commits == 0 && g_kv[0].type == 0;}
static void run_push(void) {g_notify_pass = 1; if (!setjmp(loop_done)) push_task(NULL); g_notify_pass = 0;}
static bool is_defaults(const cfg_t *c) {
    return c->role == CFG_ROLE_NONE && !c->webhook[0] && c->settle_ms == 2000 && c->rearm_ms == 3000 && c->off_after_s == CONFIG_C6_BUTTON_OFF_AFTER_S && c->rx_wd_s == 0 && c->stored == 0;   /* 0.6.0: the Kconfig default, 0 (was 120) */
}
static bool same_cfg(const cfg_t *a, const cfg_t *b) {
    return a->role == b->role && !strcmp(a->webhook, b->webhook) && a->settle_ms == b->settle_ms && a->rearm_ms == b->rearm_ms
           && a->off_after_s == b->off_after_s && a->rx_wd_s == b->rx_wd_s && a->stored == b->stored;
}
static uint32_t num_by_name(const cfg_t *c, const char *k) {
    return !strcmp(k, "settle_ms") ? c->settle_ms : !strcmp(k, "rearm_ms") ? c->rearm_ms : !strcmp(k, "off_after_s") ? c->off_after_s : c->rx_wd_s;
}
static void junk_cfg(void) {
    cfg_live = (cfg_t){.role = CFG_ROLE_SHORTCUT, .settle_ms = 1, .rearm_ms = 2, .off_after_s = 3, .rx_wd_s = 4, .stored = 0x3f};
    snprintf(cfg_live.webhook, sizeof cfg_live.webhook, "http://junk/j");
}
static bool is_off_and_released(void) {
    return !s_held && !s_delivering && !s_release_by && !s_press_at_us && !s_press_first_us && !s_released_at_us && !s_off_at_us && !s_push_due_us;
}
static void dirty_button_state(void) {
    s_held = true; s_delivering = true; s_press_at_us = 111; s_press_first_us = 111; s_released_at_us = 222; s_off_at_us = 333;
    s_release_by = "zigbee"; s_push_due_us = 444;
}
static bool button_state_untouched(void) {
    return s_held && s_delivering && s_press_at_us == 111 && s_press_first_us == 111 && s_released_at_us == 222 && s_off_at_us == 333
           && s_release_by && !strcmp(s_release_by, "zigbee") && s_push_due_us == 444;
}
#define WH_MASK "http://192.0.2.49:8123/api/webhook/c6_gfake_0123456789ab"    /* a made-up id */

/* The cr1-cr7 regressions use actual PUT handlers while firmware is blocked in dependencies. */
static bool cr_return_to_shortcut;
static void cr_switch_and_on(void) {
    CHECK(put_body("{\"role\":\"mirror\"}") == 200);
    if (cr_return_to_shortcut) CHECK(put_body("{\"role\":\"shortcut\"}") == 200);
    inject_hub_on();
}
static void cr_switch_during_release_retry(void) {
    CHECK(put_body("{\"role\":\"none\",\"webhook\":\"http://h/new\"}") == 200);
    if (cr_return_to_shortcut) CHECK(put_body("{\"role\":\"shortcut\"}") == 200);
}
static int cr_put_status;
static void cr_shorten_during_wait(void) {
    fake_now += 100000;
    cr_put_status = put_body("{\"settle_ms\":200}");
}
static void cr_stop_timed_wait(void) {g_wait_budget = 0;}
static int64_t cr_hold(uint32_t off_after) {
    reset_case(); set_role(CFG_ROLE_SHORTCUT); light_state(0);
    cfg_live.off_after_s = off_after;
    inject_hub_on(); fake_now += 2100000; run_button();
    CHECK(s_held && g_press_posts == 1 && model_on == 1);
    return fake_now;
}
static uint32_t cr_timeout_setting;
static void cr_timeout_change_at_lock(void) {
    char body[80]; snprintf(body, sizeof body, "{\"off_after_s\":%u}", cr_timeout_setting);
    CHECK(put_body(body) == 200);
}
static int put_bytes(const char *body, size_t n) {
    g_req = (httpd_req_t){.body = body, .body_len = n, .content_len = n};
    settings_put(&g_req); return g_req.code;
}

static void run_code_review(void) {
    puts("==== regressions (cr1-cr7) ====");

    /* cr1. Both timeout and late Off are canceled even after the role switches away and back. */
    for (int late = 0; late < 2; late++) for (int back = 0; back < 2; back++) {
        reset_case(); set_role(CFG_ROLE_SHORTCUT); cr_return_to_shortcut = back;
        if (late) {
            s_press_at_us = s_press_first_us = fake_now - 2100000;
            s_released_at_us = s_press_first_us - 100000;
        } else {
            s_held = true; s_off_at_us = fake_now;
        }
        g_acquire_hook = cr_switch_and_on; run_button();
        printf("cr1 %s, return-to-shortcut=%d: role=%s on=%d Off sets=%d pending=%lld releases=%d\n",
               late ? "late" : "timeout", back, cfg_role_name(cfg_live.role), model_on, g_off_sets,
               (long long)s_press_at_us, g_release_posts);
        CHECK(cfg_live.role == (back ? CFG_ROLE_SHORTCUT : CFG_ROLE_MIRROR));
        CHECK(model_on == 1 && s_light.val[L_ON] == 1 && g_off_sets == 1 && g_release_posts == 0);
        CHECK(back ? s_press_at_us != 0 : s_push_due_us != 0);
    }
    verdict("cr1", "stale timeout/late Off preserves a new Zigbee controller On across role changes, including away and back");

    /* cr2. Path masking excludes slashes and repeated credentials in query/fragment text. */
    { const char *urls[] = {"http://h/api/webhook/0123456789ab?next=/ok",
                           "http://h/api/webhook/0123456789ab#/ok",
                           "http://h/api/webhook/0123456789ab/?x=1",
                           "http://h/api/webhook/0123456789ab?next=/0123456789ab",
                           "http://h/api/webhook/0123456789ab#repeat=/0123456789ab"};
      for (size_t i = 0; i < sizeof urls / sizeof urls[0]; i++) {
          reset_case(); light_state(0);
          char body[300]; snprintf(body, sizeof body, "{\"webhook\":\"%s\"}", urls[i]);
          int code = put_body(body); bool put_safe = !resp_has("0123456789ab");
          int get = get_settings(); bool get_safe = !resp_has("0123456789ab");
          printf("cr2 URL variant %zu: PUT=%d masked=%d GET=%d masked=%d\n", i, code, put_safe, get, get_safe);
          CHECK(code == 200 && get == 200 && put_safe && get_safe);
          CHECK(!strcmp(cfg_live.webhook, urls[i]));
      } }
    verdict("cr2", "GET and PUT mask the actual last nonempty path segment and omit query/fragment credentials");

    /* cr3. A failed release stays retryable within its role, but never retries across a role switch. */
    for (int back = 0; back < 2; back++) {
        reset_case(); set_role(CFG_ROLE_SHORTCUT); light_state(0); s_release_by = "zigbee";
        cr_return_to_shortcut = back; g_post_fail_n = 1; g_delay_hook = cr_switch_during_release_retry;
        run_button();
        printf("cr3 return-to-shortcut=%d: role=%s release posts=%d retry delays=%d old URL=%d\n",
               back, cfg_role_name(cfg_live.role), g_release_posts, g_delays, !strcmp(g_last_url, WH_TEST));
        CHECK(cfg_live.role == (back ? CFG_ROLE_SHORTCUT : CFG_ROLE_NONE));
        CHECK(g_release_posts == 1 && !strcmp(g_last_url, WH_TEST));
    }
    reset_case(); set_role(CFG_ROLE_SHORTCUT); light_state(0); s_release_by = "zigbee";
    g_post_fail_n = 2; run_button();
    printf("cr3 ordinary failures: release posts=%d retry delays=%d\n", g_release_posts, g_delays);
    CHECK(g_release_posts == 3 && g_delays == 2 && !strcmp(g_last_url, WH_TEST));
    verdict("cr3", "release retries cancel after leaving shortcut, including away and back, while ordinary retries survive");

    /* cr4. Changing settle while blocked must wake the existing task before its old timeout. */
    reset_case(); set_role(CFG_ROLE_SHORTCUT); light_state(0); inject_hub_on();
    { int64_t started = fake_now;
      g_wait_hook = cr_shorten_during_wait; g_wait_budget = 2; g_press_hook = cr_stop_timed_wait;
      run_button();
      printf("cr4 settle2000->200 at +100ms: PUT=%d wakes=%d press at +%lldms posts=%d\n",
             cr_put_status, g_wait_wakes, (long long)((fake_now - started) / 1000), g_press_posts);
      CHECK(cr_put_status == 200 && g_wait_wakes == 1 && g_press_posts == 1 && s_held);
      CHECK(fake_now - started >= 200000 && fake_now - started <= 201000); }
    verdict("cr4", "shortening settle wakes a blocked task and delivers the pending press at the new deadline");

    /* cr5. Live timeouts stay relative to when the press became held, including enabling from zero. */
    { int64_t started = cr_hold(120), old_deadline = s_off_at_us;
      fake_now = started + 5000000; int code = put_body("{\"off_after_s\":0}");
      fake_now = old_deadline; run_button();
      printf("cr5 disable held timeout: PUT=%d on at old deadline=%d held=%d releases=%d\n", code, model_on, s_held, g_release_posts);
      CHECK(code == 200 && s_held && model_on == 1 && g_release_posts == 0); }
    { const uint32_t initial[] = {0, 120};
      for (size_t i = 0; i < sizeof initial / sizeof initial[0]; i++) {
          int64_t started = cr_hold(initial[i]);
          fake_now = started + 5000000; int code = put_body("{\"off_after_s\":10}");
          int64_t deadline = s_off_at_us;
          fake_now = started + 9999000; run_button(); CHECK(s_held && model_on == 1 && !g_release_posts);
          fake_now = started + 10000000; run_button();
          printf("cr5 %s at +5s: PUT=%d deadline=+%lldms on at +10s=%d releases=%d\n",
                 initial[i] ? "shorten120->10" : "enable0->10", code,
                 (long long)((deadline - started) / 1000), model_on, g_release_posts);
          CHECK(code == 200 && deadline == started + 10000000 && !s_held && model_on == 0 && g_release_posts == 1);
      } }
    { int64_t started = cr_hold(10);
      fake_now = started + 5000000; int code = put_body("{\"off_after_s\":20}");
      int64_t deadline = s_off_at_us;
      fake_now = started + 10000000; run_button(); CHECK(s_held && model_on == 1 && !g_release_posts);
      fake_now = started + 20000000; run_button();
      printf("cr5 lengthen10->20 at +5s: PUT=%d deadline=+%lldms on at +20s=%d releases=%d\n",
             code, (long long)((deadline - started) / 1000), model_on, g_release_posts);
      CHECK(code == 200 && deadline == started + 20000000 && !s_held && model_on == 0 && g_release_posts == 1); }
    { int64_t started = cr_hold(120);
      fake_now = started + 15000000; int code = put_body("{\"off_after_s\":10}"); run_button();
      printf("cr5 shorten to an elapsed deadline: PUT=%d on=%d releases=%d\n", code, model_on, g_release_posts);
      CHECK(code == 200 && !s_held && model_on == 0 && g_release_posts == 1); }
    for (int extend = 0; extend < 2; extend++) {
        int64_t started = cr_hold(10); fake_now = started + 10000000;
        cr_timeout_setting = extend ? 20 : 0; g_acquire_hook = cr_timeout_change_at_lock; run_button();
        printf("cr5 %s after timeout decision, while lock blocked: on=%d held=%d releases=%d\n",
               extend ? "extend" : "disable", model_on, s_held, g_release_posts);
        CHECK(s_held && model_on == 1 && !g_release_posts && !g_off_sets);
    }
    verdict("cr5", "held timeout changes disable, enable, shorten and lengthen using elapsed held time, including blocked Off");

    /* cr6. Parse the complete received body; the C-string prefix alone is not a request. */
    { const char *bad[] = {"{\"settle_ms\":500}junk", "{\"settle_ms\":500}{\"role\":\"bad\"}"};
      for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
          reset_case(); light_state(0); int code = put_body(bad[i]);
          printf("cr6 invalid suffix %zu: PUT=%d writes=%d\n", i, code, g_cfg_sets);
          CHECK(code == 400 && nothing_written() && cfg_live.settle_ms == 2000);
      }
      static const char raw_nul[] = "{\"settle_ms\":500}\0junk";
      reset_case(); light_state(0); int code = put_bytes(raw_nul, sizeof raw_nul - 1);
      printf("cr6 raw NUL in %zu received bytes: PUT=%d writes=%d\n", sizeof raw_nul - 1, code, g_cfg_sets);
      CHECK(code == 400 && nothing_written() && cfg_live.settle_ms == 2000);
      reset_case(); light_state(0); code = put_body(" \t\r\n{\"settle_ms\":500}\t\r\n ");
      printf("cr6 valid object with surrounding whitespace: PUT=%d writes=%d\n", code, g_cfg_sets);
      CHECK(code == 200 && cfg_live.settle_ms == 500 && g_cfg_sets == 1); }
    verdict("cr6", "junk, adjacent objects and raw NUL are rejected before writes; surrounding whitespace is valid");

    /* cr7. Decoded NULs cannot hide a suffix of a role, setting key, URL or clear sentinel. */
    { const char *bad[] = {"{\"role\":\"mirror\\u0000bad\"}",
                          "{\"settle_ms\\u0000garbage\":500}",
                          "{\"webhook\":\"http://h/abcd\\u0000 secret\"}",
                          "{\"webhook\":\"-\\u0000bad\"}"};
      for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
          reset_case(); light_state(0); int code = put_body(bad[i]);
          printf("cr7 decoded-NUL variant %zu: PUT=%d writes=%d\n", i, code, g_cfg_sets);
          CHECK(code == 400 && nothing_written() && cfg_live.role == CFG_ROLE_NONE && cfg_live.settle_ms == 2000);
          CHECK(!strcmp(cfg_live.webhook, WH_TEST));
      }
      reset_case(); light_state(0); int code = put_body("{\"webhook\":\"http://h/abcd\\\\u0000literal\"}");
      printf("cr7 literal escaped backslash-u0000: PUT=%d writes=%d\n", code, g_cfg_sets);
      CHECK(code == 200 && !strcmp(cfg_live.webhook, "http://h/abcd\\u0000literal")); }
    verdict("cr7", "decoded NUL in complete settings strings is rejected; literal backslash-u0000 stays valid");
}

static bool c9_in(const char *json, const char *arr, const char *k) {         /* the named member of an array member ("stored", "pending") of an answer */
    cJSON *j = cJSON_Parse(json); const cJSON *a = j ? cJSON_GetObjectItemCaseSensitive(j, arr) : NULL; bool found = false;
    for (const cJSON *e = a ? a->child : NULL; e; e = e->next) if (cJSON_IsString(e) && !strcmp(e->valuestring, k)) found = true;
    cJSON_Delete(j); return found;
}

static void run_new(void) {
    puts("==== Settings store: load ====");

    /* c1. cfg_load with every NVS read failing */
    reset_case(); junk_cfg(); cfg_load();
    int c1a = is_defaults(&cfg_live);
    reset_case(); kv_put_u8("role", 2); kv_put_u32("settle_ms", 1500); kv_put_str("webhook", "http://h/x"); g_cfg_get_err = ESP_FAIL; junk_cfg(); cfg_load();
    int c1b = is_defaults(&cfg_live);
    reset_case(); g_cfg_open_err = ESP_FAIL; junk_cfg(); cfg_load();
    int c1c = is_defaults(&cfg_live);
    printf("c1 no namespace: defaults=%d; namespace there, every read fails: defaults=%d; open fails: defaults=%d; role=%d settle=%u rearm=%u off=%u rx=%u stored=0x%x\n",
           c1a, c1b, c1c, cfg_live.role, cfg_live.settle_ms, cfg_live.rearm_ms, cfg_live.off_after_s, cfg_live.rx_wd_s, cfg_live.stored);
    CHECK(c1a && c1b && c1c && g_cfg_sets == 0);
    verdict("c1", "cfg_load, no namespace / every read failing: role none, no webhook, 2000/3000/the off_after_s default/0, nothing stored, nothing written");

    /* c2. role=2 and settle_ms=1500 stored */
    reset_case(); kv_put_u8("role", 2); kv_put_u32("settle_ms", 1500); junk_cfg(); cfg_load();
    printf("c2 role=%d settle=%u rearm=%u off=%u rx=%u webhook='%s' stored=0x%x\n", cfg_live.role, cfg_live.settle_ms, cfg_live.rearm_ms,
           cfg_live.off_after_s, cfg_live.rx_wd_s, cfg_live.webhook, cfg_live.stored);
    CHECK(cfg_live.role == CFG_ROLE_SHORTCUT && cfg_live.settle_ms == 1500 && cfg_live.rearm_ms == 3000 && cfg_live.off_after_s == CONFIG_C6_BUTTON_OFF_AFTER_S
          && cfg_live.rx_wd_s == 0 && !cfg_live.webhook[0] && cfg_live.stored == (CFG_K_ROLE | CFG_K_SETTLE_MS) && g_cfg_sets == 0);
    verdict("c2", "cfg_load with role=2, settle_ms=1500 stored: those from NVS, the rest defaults");

    /* c8. stored values that are not valid fall back to the default and do not count as stored */
    reset_case(); kv_put_u8("role", 7); kv_put_u32("settle_ms", 100); kv_put_u32("rearm_ms", 40000); kv_put_str("webhook", "ftp://x");
    kv_put_u32("off_after_s", 5); kv_put_u32("rx_wd_s", 10); junk_cfg(); cfg_load();
    int c8a = is_defaults(&cfg_live);
    reset_case(); kv_put_u32("role", 2); kv_put_u8("settle_ms", 9); junk_cfg(); cfg_load();         /* wrong NVS type */
    int c8b = is_defaults(&cfg_live);
    printf("c8 invalid stored values -> defaults=%d; wrong NVS types -> defaults=%d\n", c8a, c8b);
    CHECK(c8a && c8b);
    verdict("c8", "cfg_load: out-of-range, malformed or wrong-type stored values fall back to the defaults");

    puts("==== PUT /api/v1/settings: validation (all or nothing) ====");

    /* c3. role words */
    { static const struct {const char *word; int start, want;} ok[] = {{"none", CFG_ROLE_MIRROR, CFG_ROLE_NONE}, {"mirror", CFG_ROLE_NONE, CFG_ROLE_MIRROR}, {"shortcut", CFG_ROLE_NONE, CFG_ROLE_SHORTCUT}};
      char body[80]; int all = 1;
      for (int i = 0; i < 3; i++) {
          reset_case(); light_state(0); set_role(ok[i].start); snprintf(body, sizeof body, "{\"role\":\"%s\"}", ok[i].word);
          int code = put_body(body); uint32_t stored = 99; int kv = kv_u("role", &stored);
          int good = code == 200 && cfg_live.role == ok[i].want && kv && stored == (uint32_t)ok[i].want && resp_has(ok[i].word);
          printf("c3 role '%s': http=%d live=%d nvs=%u -> %s\n", ok[i].word, code, cfg_live.role, stored, good ? "accepted" : "WRONG");
          all &= good;
      }
      CHECK(all);
      static const char *bad[] = {"Mirror", "bulb", ""};
      for (int i = 0; i < 3; i++) {
          reset_case(); light_state(0); set_role(CFG_ROLE_MIRROR); snprintf(body, sizeof body, "{\"role\":\"%s\"}", bad[i]);
          int code = put_body(body);
          printf("c3 role '%s': http=%d live=%d written=%d %s\n", bad[i], code, cfg_live.role, g_cfg_sets, g_req.response);
          CHECK(code == 400 && cfg_live.role == CFG_ROLE_MIRROR && nothing_written() && resp_has("\"error\":\"bad_request\"") && resp_has("role"));
      } }
    verdict("c3", "role words: none/mirror/shortcut accepted and stored; Mirror, bulb and empty are 400 with nothing written");

    /* c4. ranges, all or nothing */
    { static const char *bad[] = {"\"settle_ms\":100", "\"settle_ms\":199", "\"settle_ms\":10001", "\"rearm_ms\":30001", "\"off_after_s\":5", "\"off_after_s\":9",
                                  "\"off_after_s\":3601", "\"rx_wd_s\":10", "\"rx_wd_s\":29", "\"rx_wd_s\":3601"};
      char body[200]; int all = 1;
      for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) for (int order = 0; order < 2; order++) {
          reset_case(); light_state(0); set_role(CFG_ROLE_MIRROR); cfg_live.webhook[0] = 0;
          snprintf(body, sizeof body, order ? "{%s,\"rearm_ms\":5000,\"webhook\":\"http://h/ok\"}" : "{\"rearm_ms\":5000,\"webhook\":\"http://h/ok\",%s}", bad[i]);
          int code = put_body(body);
          int good = code == 400 && nothing_written() && cfg_live.rearm_ms == 3000 && !cfg_live.webhook[0] && cfg_live.role == CFG_ROLE_MIRROR;
          if (!good) printf("c4 %s (order %d): http=%d written=%d rearm=%u webhook='%s'\n", bad[i], order, code, g_cfg_sets, cfg_live.rearm_ms, cfg_live.webhook);
          all &= good;
      }
      printf("c4 %zu out-of-range values, each beside two valid keys, in both orders: every one 400 with nothing written=%d\n", sizeof bad / sizeof bad[0], all);
      CHECK(all);
      static const struct {const char *k; uint32_t v;} edge[] = {{"settle_ms", 200}, {"settle_ms", 10000}, {"rearm_ms", 0}, {"rearm_ms", 30000}, {"off_after_s", 0},
                                                                 {"off_after_s", 10}, {"off_after_s", 3600}, {"rx_wd_s", 0}, {"rx_wd_s", 30}, {"rx_wd_s", 3600}};
      int edges = 1;
      for (size_t i = 0; i < sizeof edge / sizeof edge[0]; i++) {
          reset_case(); light_state(0); snprintf(body, sizeof body, "{\"%s\":%u}", edge[i].k, edge[i].v);
          int code = put_body(body); uint32_t stored = 0xFFFFFFFFu; int kv = kv_u(edge[i].k, &stored);
          int good = code == 200 && num_by_name(&cfg_live, edge[i].k) == edge[i].v && kv && stored == edge[i].v;
          if (!good) printf("c4 edge %s=%u: http=%d live=%u nvs=%u\n", edge[i].k, edge[i].v, code, num_by_name(&cfg_live, edge[i].k), stored);
          edges &= good;
      }
      printf("c4 in-range edges (200, 10000, 0, 30000, 0, 10, 3600, 0, 30, 3600) all written=%d\n", edges);
      CHECK(edges); }
    verdict("c4", "ranges: out-of-range 400 with no key of the request written; the edge values 200/10000/0/10/30/3600 written");

    /* c5. webhook values */
    { char body[400], url[300]; int all = 1;
      static const char *okurl[] = {"http://h/x", "https://h/api/webhook/abc"};
      for (int i = 0; i < 2; i++) {
          reset_case(); light_state(0); cfg_live.webhook[0] = 0; snprintf(body, sizeof body, "{\"webhook\":\"%s\"}", okurl[i]);
          int code = put_body(body); const char *s = kv_s("webhook");
          int good = code == 200 && !strcmp(cfg_live.webhook, okurl[i]) && s && !strcmp(s, okurl[i]);
          printf("c5 '%s': http=%d stored=%d\n", okurl[i], code, good); all &= good;
      }
      memset(url, 0, sizeof url); memcpy(url, "http://h/", 9); memset(url + 9, 'a', 159 - 9);          /* 159 characters */
      reset_case(); light_state(0); cfg_live.webhook[0] = 0; snprintf(body, sizeof body, "{\"webhook\":\"%s\"}", url);
      { int code = put_body(body); const char *s = kv_s("webhook"); int good = code == 200 && strlen(cfg_live.webhook) == 159 && s && !strcmp(s, url);
        printf("c5 159 characters: http=%d written=%d\n", code, good); all &= good; }
      url[159] = 'a'; url[160] = 0;                                                                         /* 160 characters */
      reset_case(); light_state(0); cfg_live.webhook[0] = 0; snprintf(body, sizeof body, "{\"webhook\":\"%s\"}", url);
      { int code = put_body(body); int good = code == 400 && nothing_written() && !cfg_live.webhook[0];
        printf("c5 160 characters: http=%d refused=%d\n", code, good); all &= good; }
      static const char *bad[] = {"\"webhook\":\"ftp://h\"", "\"webhook\":\"http://h/a b\"", "\"webhook\":\"http://h/a\\tb\"", "\"webhook\":5", "\"webhook\":null"};
      for (int i = 0; i < 5; i++) {
          reset_case(); light_state(0); cfg_live.webhook[0] = 0; snprintf(body, sizeof body, "{%s}", bad[i]);
          int code = put_body(body); int good = code == 400 && nothing_written() && !cfg_live.webhook[0] && resp_has("webhook");
          printf("c5 {%s}: http=%d refused=%d\n", bad[i], code, good); all &= good;
      }
      reset_case(); light_state(0); kv_put_str("webhook", WH_TEST);
      { int code = put_body("{\"webhook\":\"-\"}"); const char *s = kv_s("webhook"); int good = code == 200 && !cfg_live.webhook[0] && s && !s[0] && (cfg_live.stored & CFG_K_WEBHOOK) && resp_has("\"webhook\":\"\"");
        printf("c5 '-': http=%d cleared=%d\n", code, good); all &= good; }
      CHECK(all); }
    verdict("c5", "webhook: http/https and 159 characters written; ftp, spaces, a tab, 160 characters, a number and null refused; '-' clears");

    /* c7. body shapes */
    { char big[600]; int all = 1;
      struct {const char *name; const char *body; const char *must;} shape[] = {
          {"not JSON", "settle_ms=2000", "body"}, {"JSON array", "[{\"settle_ms\":2000}]", "body"}, {"settle_ms as a string", "{\"settle_ms\":\"2000\"}", "settle_ms"},
          {"settle_ms 2000.5", "{\"settle_ms\":2000.5}", "settle_ms"}, {"settle_ms -1", "{\"settle_ms\":-1}", "settle_ms"}, {"an unknown key", "{\"bogus_key\":1}", "bogus_key"},
          {"an unknown key beside a valid one", "{\"settle_ms\":2500,\"bogus_key\":1}", "bogus_key"}, {"a repeated key", "{\"settle_ms\":2500,\"settle_ms\":2600}", "settle_ms"},
          {"an empty object is fine", "{}", NULL},
      };
      for (size_t i = 0; i < sizeof shape / sizeof shape[0]; i++) {
          reset_case(); light_state(0); int code = put_body(shape[i].body);
          int good = shape[i].must ? code == 400 && nothing_written() && cfg_live.settle_ms == 2000 && resp_has(shape[i].must) && resp_has("\"error\":\"bad_request\"")
                                   : code == 200 && nothing_written();
          printf("c7 %s: http=%d written=%d %s\n", shape[i].name, code, g_cfg_sets, g_req.response);
          all &= good;
      }
      memset(big, ' ', sizeof big); memcpy(big, "{\"settle_ms\":2500}", 18); big[513] = 0;                    /* 513 bytes */
      reset_case(); light_state(0); { int code = put_body(big); int good = code == 400 && nothing_written() && cfg_live.settle_ms == 2000; printf("c7 513-byte body: http=%d refused=%d\n", code, good); all &= good; }
      big[512] = 0;                                                                                          /* 512 bytes: the limit itself */
      reset_case(); light_state(0); { int code = put_body(big); int good = code == 200 && cfg_live.settle_ms == 2500; printf("c7 512-byte body: http=%d accepted=%d\n", code, good); all &= good; }
      reset_case(); light_state(0); { g_req = (httpd_req_t){0}; g_req.body = ""; settings_put(&g_req); int good = g_req.code == 400 && nothing_written(); printf("c7 empty body: http=%d refused=%d\n", g_req.code, good); all &= good; }
      CHECK(all); }
    verdict("c7", "body shapes: not JSON, an array, a string/fraction/negative number, an unknown or repeated key, 513 bytes and an empty body are 400 with nothing written");

    /* c6. the mask */
    reset_case(); snprintf(cfg_live.webhook, sizeof cfg_live.webhook, "%s", WH_MASK);
    { int code = get_settings();
      printf("c6 GET: http=%d %s\n", code, g_req.response);
      CHECK(code == 200 && resp_has("\"webhook\":\"http://192.0.2.49:8123/api/webhook/c6_g\xE2\x80\xA6\"") && !resp_has("0123456789ab") && !resp_has("gfake"));
      light_state(0);
      code = put_body("{\"webhook\":\"" WH_MASK "\"}");
      CHECK(code == 200 && resp_has("c6_g\xE2\x80\xA6") && !resp_has("0123456789ab") && !resp_has("gfake"));       /* PUT answers with the same masked JSON */
      cfg_live.webhook[0] = 0; code = get_settings();
      CHECK(code == 200 && resp_has("\"webhook\":\"\"")); }
    verdict("c6", "GET masks the last path segment to 4 characters + '...'; the full id is in no JSON; an empty webhook shows as \"\"");

    /* c9. the hue setting: a boolean, stored as a u8, read once at boot; "pending" lists what differs from the boot value */
    {
        #define C9_STORED(k) c9_in(g_req.response, "stored", k)
        #define C9_PENDING(k) c9_in(g_req.response, "pending", k)
        /* the default on an empty store */
        reset_case(); junk_cfg(); cfg_load(); light_state(0); int gc = get_settings();
        printf("c9 empty store: http=%d hue live=%d boot=%d, in the GET document=%d, in stored=%d, written=%d\n", gc, cfg_live.hue, cfg_hue_boot(), resp_has("\"hue\":true"), c9_in(g_req.response, "stored", "hue"), g_cfg_sets);
        CHECK(gc == 200 && cfg_live.hue && cfg_hue() && cfg_hue_boot() && resp_has("\"hue\":true") && !c9_in(g_req.response, "stored", "hue") && !(cfg_live.stored & CFG_K_HUE) && g_cfg_sets == 0);
        /* stored 0, stored 1, stored 7 */
        reset_case(); kv_put_u8("hue", 0); junk_cfg(); cfg_load(); get_settings();
        printf("c9 stored 0: live=%d boot=%d, GET false=%d, in stored=%d\n", cfg_live.hue, cfg_hue_boot(), resp_has("\"hue\":false"), c9_in(g_req.response, "stored", "hue"));
        CHECK(!cfg_live.hue && !cfg_hue() && !cfg_hue_boot() && resp_has("\"hue\":false") && c9_in(g_req.response, "stored", "hue") && (cfg_live.stored & CFG_K_HUE));
        reset_case(); kv_put_u8("hue", 1); junk_cfg(); cfg_load(); get_settings();
        printf("c9 stored 1: live=%d boot=%d, in stored=%d\n", cfg_live.hue, cfg_hue_boot(), c9_in(g_req.response, "stored", "hue"));
        CHECK(cfg_live.hue && cfg_hue_boot() && c9_in(g_req.response, "stored", "hue") && (cfg_live.stored & CFG_K_HUE));
        reset_case(); kv_put_u8("hue", 7); junk_cfg(); size_t m9 = g_loglen; cfg_load(); get_settings();
        printf("c9 stored 7: live=%d boot=%d, in stored=%d, 'not usable (out of range)' lines=%d\n", cfg_live.hue, cfg_hue_boot(), c9_in(g_req.response, "stored", "hue"), log_count(m9, "CFG hue: stored value not usable (out of range)"));
        CHECK(cfg_live.hue && cfg_hue_boot() && !c9_in(g_req.response, "stored", "hue") && !(cfg_live.stored & CFG_K_HUE) && log_count(m9, "CFG hue: stored value not usable (out of range)") == 1);
        reset_case(); kv_put_u32("hue", 1); junk_cfg(); cfg_load();                              /* a wrong NVS type: the default, not stored */
        CHECK(cfg_live.hue && !(cfg_live.stored & CFG_K_HUE));
        /* PUT false and true */
        reset_case(); junk_cfg(); cfg_load(); light_state(0); m9 = g_loglen;
        int pf = put_body("{\"hue\":false}"); uint32_t v9 = 99;
        int k9 = kv_u("hue", &v9), t9 = kv_slot("hue", false) >= 0 ? g_kv[kv_slot("hue", false)].type : -1;
        printf("c9 PUT false: http=%d live=%d boot=%d, written as u8=%d value %u, in stored=%d, pending=%d, 'CFG hue' log lines=%d\n", pf, cfg_live.hue, cfg_hue_boot(), t9 == 1, v9, C9_STORED("hue"), C9_PENDING("hue"), log_count(m9, "CFG hue on -> off"));
        CHECK(pf == 200 && !cfg_live.hue && !cfg_hue() && cfg_hue_boot() && k9 && t9 == 1 && v9 == 0 && C9_STORED("hue") && C9_PENDING("hue") && resp_has("\"hue\":false") && resp_has("\"pending\":[\"hue\"]") && log_count(m9, "CFG hue on -> off") == 1);
        get_settings();
        CHECK(!resp_has("\"pending\"") && resp_has("\"hue\":false") && c9_in(g_req.response, "stored", "hue"));      /* GET never lists pending */
        int pt = put_body("{\"hue\":true}"); k9 = kv_u("hue", &v9); t9 = g_kv[kv_slot("hue", false)].type;
        printf("c9 PUT true after false (back to the boot value): http=%d live=%d, written as u8=%d value %u, pending present=%d\n", pt, cfg_live.hue, t9 == 1, v9, resp_has("\"pending\""));
        CHECK(pt == 200 && cfg_live.hue && k9 && t9 == 1 && v9 == 1 && !resp_has("\"pending\"") && C9_STORED("hue"));
        /* refused values */
        static const char *bad9[] = {"{\"hue\":1}", "{\"hue\":0}", "{\"hue\":\"on\"}", "{\"hue\":\"true\"}", "{\"hue\":null}", "{\"hue\":2.5}"};
        int refused = 1;
        for (size_t i = 0; i < sizeof bad9 / sizeof bad9[0]; i++) {
            reset_case(); junk_cfg(); cfg_load(); light_state(0); cfg_t before; cfg_get(&before);
            int c = put_body(bad9[i]); int named = resp_has("hue: true or false") && resp_has("bad_request");
            int good = c == 400 && named && nothing_written() && same_cfg(&before, &cfg_live);
            if (!good) printf("c9 %s: http=%d named=%d written=%d\n", bad9[i], c, named, g_cfg_sets);
            refused &= good;
        }
        printf("c9 refused values (1, 0, \"on\", \"true\", null, 2.5): every one 400 'hue: true or false', nothing written, cfg_live unchanged=%d\n", refused);
        CHECK(refused);
        reset_case(); junk_cfg(); cfg_load(); light_state(0);
        int mixed = put_body("{\"rearm_ms\":5000,\"hue\":1}");
        CHECK(mixed == 400 && nothing_written() && cfg_live.rearm_ms == 3000);                  /* all or nothing, as every key */
        /* pending with ext_pan and hue */
        reset_case(); junk_cfg(); cfg_load(); light_state(0);
        int pb = put_body("{\"ext_pan\":\"0123456789ABCDEF\",\"hue\":false}");
        printf("c9 PUT ext_pan and hue together: http=%d pending=[ext_pan,hue]=%d\n", pb, resp_has("\"pending\":[\"ext_pan\",\"hue\"]"));
        CHECK(pb == 200 && resp_has("\"pending\":[\"ext_pan\",\"hue\"]"));
        int pe = put_body("{\"hue\":true}");
        CHECK(pe == 200 && resp_has("\"pending\":[\"ext_pan\"]"));
        int pc = put_body("{\"ext_pan\":\"\",\"hue\":false}");
        CHECK(pc == 200 && resp_has("\"pending\":[\"hue\"]"));
        /* the boot values follow a reload, not the PUT */
        reset_case(); kv_put_u8("hue", 0); junk_cfg(); cfg_load(); light_state(0); put_body("{\"hue\":true}");
        int boot_after_put = cfg_hue_boot(), live_after_put = cfg_hue();
        junk_cfg(); cfg_load();
        printf("c9 stored 0, PUT true: live=%d boot=%d; after a reload (stored now 1): live=%d boot=%d\n", live_after_put, boot_after_put, cfg_hue(), cfg_hue_boot());
        CHECK(live_after_put && !boot_after_put && cfg_hue() && cfg_hue_boot());
        #undef C9_STORED
        #undef C9_PENDING
    }
    verdict("c9", "hue: default true (live and in GET, not in stored); stored 0/1 are false/true and in stored; stored 7 is true with the 'not usable (out of range)' line and not in stored; PUT false/true is 200, written as u8 0/1, in stored, the answer's pending lists hue exactly while it differs from the boot value (and ext_pan beside it, in that order); 1, 0, \"on\", null and other non-booleans are 400 'hue: true or false' with nothing written; the boot value is the one cfg_load read");

    /* g1. the GET document */
    reset_case(); kv_put_u8("role", 2); kv_put_u32("settle_ms", 1500); cfg_load(); get_settings();
    printf("g1 %s\n", g_req.response);
    CHECK(resp_has("\"role\":\"shortcut\"") && resp_has("\"settle_ms\":1500") && resp_has("\"rearm_ms\":3000") && resp_has("\"off_after_s\":" HX_XSTR(CONFIG_C6_BUTTON_OFF_AFTER_S))
          && resp_has("\"rx_wd_s\":0") && resp_has("\"stored\":[\"role\",\"settle_ms\"]"));
    reset_case(); cfg_load(); get_settings();
    CHECK(resp_has("\"role\":\"none\"") && resp_has("\"webhook\":\"\"") && resp_has("\"stored\":[]"));
    verdict("g1", "GET document: role word, webhook, the four numbers and the list of stored keys (empty when everything is a default)");

    /* s1. a PUT survives a reload */
    reset_case(); light_state(0); set_role(CFG_ROLE_NONE);
    { int code = put_body("{\"role\":\"mirror\",\"webhook\":\"http://h/persist\",\"settle_ms\":1234,\"rearm_ms\":4321,\"off_after_s\":77,\"rx_wd_s\":99}");
      cfg_t saved; cfg_get(&saved); junk_cfg(); cfg_load();
      printf("s1 http=%d, after cfg_load: role=%d settle=%u rearm=%u off=%u rx=%u stored=0x%x\n", code, cfg_live.role, cfg_live.settle_ms, cfg_live.rearm_ms, cfg_live.off_after_s, cfg_live.rx_wd_s, cfg_live.stored);
      CHECK(code == 200 && saved.stored == 0x3f && same_cfg(&saved, &cfg_live) && cfg_live.role == CFG_ROLE_MIRROR && !strcmp(cfg_live.webhook, "http://h/persist")); }
    verdict("s1", "all six keys written by one PUT are what cfg_load reads back (same keys, same types)");

    /* t1. the token */
    reset_case(); s_claimed = false; g_auto_bearer = 0; light_state(0);                       /* 0.5.0: c6_auth_ok, the real one */
    { int g = get_settings(); bool g_cr = resp_has("\"claim_required\""); int p = put_body("{\"role\":\"shortcut\"}"); bool p_cr = resp_has("\"claim_required\"");
      claim_now(); g_auto_bearer = 0;
      int g2 = get_settings(); bool g_un = resp_has("\"unauthorized\""); int p2 = put_body("{\"role\":\"shortcut\"}"); bool p_un = resp_has("\"unauthorized\"");
      printf("t1 unclaimed GET http=%d, PUT http=%d; claimed without a credential GET http=%d, PUT http=%d, written=%d, role=%d\n", g, p, g2, p2, g_cfg_sets, cfg_live.role);
      CHECK(g == 403 && g_cr && p == 403 && p_cr && g2 == 401 && g_un && p2 == 401 && p_un && nothing_written() && cfg_live.role == g_mode_role); }
    verdict("t1", "without a credential both GET and PUT are refused as JSON (403 claim_required unclaimed, 401 unauthorized claimed) and nothing is written");

    puts("==== Role change at runtime ====");

    /* r1. mirror, light On -> shortcut */
    reset_case(); set_role(CFG_ROLE_MIRROR); dirty_button_state();
    { int code = put_body("{\"role\":\"shortcut\"}"); uint32_t stored = 99; int kv = kv_u("role", &stored);
      printf("r1 http=%d: Off sets=%d (role in force at the Off: %d), model_on=%d, shadow_on=%u, role live=%d stored=%u (light was still on when stored: %d), posts=%d, lock held after=%d\n",
             code, g_off_sets, g_role_at_off, model_on, s_light.val[L_ON], cfg_live.role, stored, g_model_on_at_role_store, g_nposts, g_zb_locked);
      CHECK(code == 200 && g_off_sets == 1 && g_role_at_off == CFG_ROLE_MIRROR && !model_on && !s_light.val[L_ON] && !s_hub.val[L_ON]);
      CHECK(g_model_on_at_role_store == 0);                                       /* stored after the Off, not before */
      CHECK(cfg_live.role == CFG_ROLE_SHORTCUT && kv && stored == CFG_ROLE_SHORTCUT);
      CHECK(is_off_and_released() && g_nposts == 0 && g_press_posts == 0 && !g_zb_locked); }
    verdict("r1", "mirror, light on -> shortcut: Off first (under the old role), then button state cleared and the role stored; no press pushed");

    /* r2. shortcut with a press held -> mirror */
    reset_case(); set_role(CFG_ROLE_SHORTCUT); s_held = true; s_off_at_us = fake_now + 100000000; s_release_by = "zigbee";
    { int code = put_body("{\"role\":\"mirror\"}"); uint32_t stored = 99; int kv = kv_u("role", &stored);
      run_button(); run_push(); fake_now += 130000000; run_button();
      printf("r2 http=%d: Off sets=%d, model_on=%d, held=%d, release_by=%s, role live=%d stored=%u, posts=%d (release posts %d) after the button task ran\n",
             code, g_off_sets, model_on, s_held, s_release_by ? s_release_by : "-", cfg_live.role, stored, g_nposts, g_release_posts);
      CHECK(code == 200 && g_off_sets == 1 && !model_on && is_off_and_released() && cfg_live.role == CFG_ROLE_MIRROR && kv && stored == CFG_ROLE_MIRROR);
      CHECK(g_nposts == 0 && g_release_posts == 0); }
    verdict("r2", "shortcut with a press held -> mirror: Off, button state cleared, role stored, no release push");

    /* r3. mirror, light off -> shortcut */
    reset_case(); set_role(CFG_ROLE_MIRROR); light_state(0);
    { int code = put_body("{\"role\":\"shortcut\"}"); uint32_t stored = 99; int kv = kv_u("role", &stored);
      printf("r3 http=%d: attribute sets=%d, role live=%d stored=%u\n", code, g_set_calls, cfg_live.role, stored);
      CHECK(code == 200 && g_set_calls == 0 && cfg_live.role == CFG_ROLE_SHORTCUT && kv && stored == CFG_ROLE_SHORTCUT); }
    verdict("r3", "mirror, light off -> shortcut: no Off is sent, role stored");

    /* r4. the Zigbee lock is busy */
    reset_case(); set_role(CFG_ROLE_MIRROR); dirty_button_state(); lock_ok = false;
    { int code = put_body("{\"role\":\"shortcut\",\"settle_ms\":4000}");
      printf("r4 http=%d %s: role live=%d, written=%d, model_on=%d, settle=%u\n", code, g_req.response, cfg_live.role, g_cfg_sets, model_on, cfg_live.settle_ms);
      CHECK(code == 503 && resp_has("\"error\":\"zigbee_busy\"") && cfg_live.role == CFG_ROLE_MIRROR && nothing_written() && cfg_live.settle_ms == 2000);
      CHECK(model_on == 1 && s_light.val[L_ON] == 1 && button_state_untouched()); }
    verdict("r4", "lock busy: 503 zigbee_busy, role not stored (nor any other key of the request), state untouched");

    /* r5. the attribute setter refuses the Off */
    reset_case(); set_role(CFG_ROLE_MIRROR); dirty_button_state(); g_set_fail_on_off = 1;
    { int code = put_body("{\"role\":\"shortcut\"}");
      printf("r5 http=%d %s: role live=%d, written=%d, model_on=%d, lock held after=%d\n", code, g_req.response, cfg_live.role, g_cfg_sets, model_on, g_zb_locked);
      CHECK(code == 503 && resp_has("\"error\":\"zigbee_busy\"") && cfg_live.role == CFG_ROLE_MIRROR && nothing_written());
      CHECK(model_on == 1 && s_light.val[L_ON] == 1 && button_state_untouched() && !g_zb_locked); }
    verdict("r5", "Off refused by the attribute setter: 503, role not stored, state untouched, Zigbee lock released");

    /* r6. none -> mirror, then a Zigbee controller On */
    reset_case(); set_role(CFG_ROLE_NONE); light_state(0);
    { int code = put_body("{\"role\":\"mirror\"}"); ctl(6, HUB_ON, sizeof HUB_ON);
      printf("r6 http=%d: role=%d, push_due=%s, press_due=%lld, held=%d\n", code, cfg_live.role, s_push_due_us ? "set" : "0", (long long)s_press_at_us, s_held);
      CHECK(code == 200 && cfg_live.role == CFG_ROLE_MIRROR && s_push_due_us != 0 && !s_press_at_us && !s_held && !s_off_at_us); }
    verdict("r6", "none -> mirror: the next Zigbee controller On arms the push path and leaves the button state alone");

    puts("==== The Zigbee controller tap and the two tasks, by role ====");

    /* h1. none */
    reset_case(); set_role(CFG_ROLE_NONE); light_state(0);
    ctl(6, HUB_ON, sizeof HUB_ON);
    { int notified_push = g_notify_push, notified_button = g_notify_button;
      run_push(); run_button(); fake_now += 130000000; run_button();
      printf("h1 role none, Zigbee controller On: hub_on=%u push_due=%lld press_due=%lld held=%d, notifies push=%d button=%d, posts after both tasks ran=%d, sets=%d\n",
             s_hub.val[L_ON], (long long)s_push_due_us, (long long)s_press_at_us, s_held, notified_push, notified_button, g_nposts, g_set_calls);
      CHECK(s_hub.val[L_ON] == 1 && s_hub_cmd_us != 0 && !s_push_due_us && !s_press_at_us && !s_held && !s_off_at_us);
      CHECK(notified_push == 1 && notified_button == 1);
      CHECK(g_nposts == 0 && g_set_calls == 0 && g_http_inits == 0); }
    verdict("h1", "role none, Zigbee controller On: both tasks woken, nothing armed, nothing pushed");

    /* h2. mirror */
    reset_case(); set_role(CFG_ROLE_MIRROR); light_state(0);
    ctl(6, HUB_ON, sizeof HUB_ON);
    { int notified_push = g_notify_push, notified_button = g_notify_button; int64_t due = s_push_due_us;
      run_button(); fake_now += 130000000; run_button(); int posts_button = g_nposts; run_push();
      printf("h2 role mirror, Zigbee controller On: push_due=%s press_due=%lld, notifies push=%d button=%d, posts from the button task=%d, from the push task=%d [%s]\n",
             due ? "set" : "0", (long long)s_press_at_us, notified_push, notified_button, posts_button, g_nposts, g_nposts ? g_posts[0] : "-");
      CHECK(due != 0 && !s_press_at_us && !s_held && notified_push == 1 && notified_button == 1);
      CHECK(posts_button == 0 && g_nposts == 1 && post_has(0, "\"on\":1") && !post_has(0, "press") && s_push_due_us == 0 && !strcmp(g_last_url, WH_TEST)); }
    verdict("h2", "role mirror, Zigbee controller On: push armed, no press; only the push task posts, to the webhook setting");

    /* h3. shortcut */
    reset_case(); set_role(CFG_ROLE_SHORTCUT); light_state(0);
    ctl(6, HUB_ON, sizeof HUB_ON);
    { int notified_push = g_notify_push, notified_button = g_notify_button;
      run_push(); run_button(); int before = g_nposts; fake_now += 2100000; run_button();
      printf("h3 role shortcut, Zigbee controller On, quiet 2.1 s: notifies push=%d button=%d, posts before settle=%d, after=%d [%s], held=%d, push_due=%lld\n",
             notified_push, notified_button, before, g_nposts, g_nposts ? g_posts[0] : "-", s_held, (long long)s_push_due_us);
      CHECK(notified_push == 1 && notified_button == 1 && !s_push_due_us && before == 0);
      CHECK(g_nposts == 1 && g_press_posts == 1 && post_has(0, "\"press\":1") && s_held); }
    verdict("h3", "role shortcut, Zigbee controller On then quiet >= settle: one press pushed; the push task does nothing");

    /* h4. settle_ms changed live */
    reset_case(); set_role(CFG_ROLE_SHORTCUT); light_state(0);
    { int code = put_body("{\"settle_ms\":500}"); ctl(6, HUB_ON, sizeof HUB_ON);
      run_button(); fake_now += 400000; run_button(); int at400 = g_press_posts; fake_now += 200000; run_button();
      printf("h4 settle_ms 500 set live (http=%d): presses at +400 ms=%d, at +600 ms=%d\n", code, at400, g_press_posts);
      CHECK(code == 200 && at400 == 0 && g_press_posts == 1 && s_held); }
    verdict("h4", "settle_ms changed to 500 live: the press is pushed at the new quiet time, not the old one");

    /* gd. 0.3.27's group-Off drop is unchanged by the role */
    for (int role = CFG_ROLE_NONE; role <= CFG_ROLE_SHORTCUT; role++) {
        static const uint8_t off[] = {1, 1, 0};
        ezb_apsde_data_ind_t ind = {.status = 0, .profile_id = 0x104, .cluster_id = 6, .asdu = (uint8_t *)off, .asdu_length = sizeof off,
                                    .src_address = {.addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = 0}, .dst_address = {.addr_mode = EZB_ADDR_MODE_GROUP, .u.group_addr.group = 0x1234}};
        reset_case(); set_role(role); light_state(0); bool dropped = hub_tap(&ind);
        reset_case(); set_role(role); light_state(1); bool kept = !hub_tap(&ind);
        ind.dst_address.addr_mode = EZB_ADDR_MODE_SHORT; reset_case(); set_role(role); light_state(0); bool unicast = !hub_tap(&ind);
        printf("gd role %s: group Off while off dropped=%d, while on passed=%d, unicast Off passed=%d\n", cfg_role_name((uint8_t)role), dropped, kept, unicast);
        CHECK(dropped && kept && unicast);
    }
    verdict("gd", "hub_tap drops a group Off only while the light is off, in every role");

    puts("==== HTTP and restore, by role ====");

    /* p1. light_post on=1, shortcut */
    reset_case(); set_role(CFG_ROLE_SHORTCUT); light_state(0);
    { httpd_req_t r = {.on = "1"}; CHECK(light_post(&r) == 0);
      printf("p1 shortcut on=1: model_on=%d held=%d response=%s", model_on, s_held, r.response);
      CHECK(r.code == 200 && strstr(r.response, "a button takes on=0 only") && !model_on && !s_held && !g_set_calls); }
    verdict("p1", "light_post on=1 in the shortcut role: 'a button takes on=0 only', nothing set");

    /* p2. light_post on=1, mirror */
    reset_case(); set_role(CFG_ROLE_MIRROR); light_state(0);
    { httpd_req_t r = {.on = "1"}; CHECK(light_post(&r) == 0);
      printf("p2 mirror on=1: model_on=%d response=%s", model_on, r.response);
      CHECK(r.code == 200 && strstr(r.response, "set:") && strstr(r.response, "on=1") && model_on == 1 && s_light.val[L_ON] == 1);
      light_state(0); s_hub_cmd_us = fake_now - 1000000;                              /* control: a Zigbee controller command 1 s ago */
      httpd_req_t e = {.on = "1"}; CHECK(light_post(&e) == 0 && strstr(e.response, "Zigbee controller command less than 3 s ago") && model_on == 0); }
    verdict("p2", "light_post on=1 in the mirror role is applied (and ignored inside 3 s of a Zigbee controller command)");

    /* p3 / p4. light_restore by role */
    { light_state_t saved = {.seen = (1u << L_ON) | (1u << L_LEVEL), .val = {[L_ON] = 1, [L_LEVEL] = 50}};
      reset_case(); set_role(CFG_ROLE_SHORTCUT); light_state(0); memcpy(g_restore_blob, &saved, sizeof saved); g_restore_n = sizeof saved;
      g_zb_locked = true; light_restore(); g_zb_locked = false;       /* the stack's signal handler holds its lock */
      printf("p3 shortcut restore of an On state: on=%u level=%u model_on=%d\n", s_light.val[L_ON], s_light.val[L_LEVEL], model_on);
      CHECK(s_light.val[L_ON] == 0 && s_light.val[L_LEVEL] == 50 && model_on == 0);
      verdict("p3", "light_restore in the shortcut role forces On to 0 (level restored)");
      reset_case(); set_role(CFG_ROLE_MIRROR); light_state(0); memcpy(g_restore_blob, &saved, sizeof saved); g_restore_n = sizeof saved;
      g_zb_locked = true; light_restore(); g_zb_locked = false;
      printf("p4 mirror restore of an On state: on=%u level=%u model_on=%d\n", s_light.val[L_ON], s_light.val[L_LEVEL], model_on);
      CHECK(s_light.val[L_ON] == 1 && s_light.val[L_LEVEL] == 50 && model_on == 1);
      verdict("p4", "light_restore in the mirror role leaves On as saved"); }

    puts("==== webhook_post without a webhook ====");

    /* w1 */
    reset_case(); set_role(CFG_ROLE_MIRROR); cfg_live.webhook[0] = 0; g_nlogs = 0;
    { bool r = webhook_post("{\"on\":1}");
      printf("w1 empty webhook: webhook_post=%d, HTTP clients created=%d, posts=%d, log lines=%d [%s]\n", r, g_http_inits, g_nposts, g_nlogs, g_lastlog);
      CHECK(!r && g_http_inits == 0 && g_nposts == 0 && g_nlogs == 1 && strstr(g_lastlog, "WEBHOOK skipped: no webhook")); }
    reset_case(); set_role(CFG_ROLE_MIRROR); cfg_live.webhook[0] = 0; light_state(0); ctl(6, HUB_ON, sizeof HUB_ON); run_push();     /* the push task with no webhook */
    CHECK(g_http_inits == 0 && g_nposts == 0);
    verdict("w1", "webhook_post with an empty webhook: false, no HTTP call, one log line (also through the push task)");

    puts("==== IEEE address ====");

    /* i1 / i2 */
    { static const uint8_t macA[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33}, macB[6] = {0xAA, 0xBB, 0xCC, 0x44, 0x55, 0x66};
      static const uint8_t wantA[8] = {0xB0, 0xCE, 0x18, 0xC6, 0x00, 0x11, 0x22, 0x33}, wantB[8] = {0xB0, 0xCE, 0x18, 0xC6, 0x00, 0x44, 0x55, 0x66};
      uint8_t out[8];
      memset(out, 0xEE, sizeof out); ieee_from_mac(macA, out);
      printf("i1 AA:BB:CC:11:22:33 -> %02X%02X%02X%02X%02X%02X%02X%02X\n", out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
      CHECK(!memcmp(out, wantA, 8));
      verdict("i1", "IEEE from MAC AA:BB:CC:11:22:33 is B0CE18C600112233");
      memset(out, 0xEE, sizeof out); ieee_from_mac(macB, out);
      printf("i2 AA:BB:CC:44:55:66 -> %02X%02X%02X%02X%02X%02X%02X%02X\n", out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
      CHECK(!memcmp(out, wantB, 8));
      verdict("i2", "IEEE from MAC AA:BB:CC:44:55:66 is B0CE18C600445566");

      /* i3. zb_task's address block, Kconfig empty vs set. The chip's own address under the neutral profile, derived from the base MAC under E11 (HARNESS_PROFILE) */
      static const uint8_t kc[8] = {0xB0, 0xCE, 0x18, 0xC6, 0x00, 0x0A, 0x0B, 0x0C};
      reset_case(); memcpy(g_mac, macA, 6); g_kconfig_ieee = ""; g_iface_set_calls = g_read_mac_calls = 0; g_nlogs = 0; memset(g_iface_mac, 0, 8); zb_address_path();
      int a_calls = g_iface_set_calls, a_type = g_iface_type, a_read = g_read_mac_calls, a_rtype = g_read_mac_type; char a_log[400]; snprintf(a_log, sizeof a_log, "%s", g_lastlog);
#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
      int a_ok = !memcmp(g_iface_mac, wantA, 8), a_want_type = ESP_MAC_BASE, a_text = strstr(a_log, "derived from the MAC (E11)") != NULL;
#else
      static const uint8_t wantN[8] = {0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, 0x33};
      int a_ok = !memcmp(g_iface_mac, wantN, 8), a_want_type = ESP_MAC_IEEE802154, a_text = strstr(a_log, "chip's own") != NULL;
#endif
      reset_case(); memcpy(g_mac, macA, 6); g_kconfig_ieee = "B0CE18C6000A0B0C"; g_iface_set_calls = g_read_mac_calls = 0; memset(g_iface_mac, 0, 8); zb_address_path();
      int k_calls = g_iface_set_calls, k_ok = !memcmp(g_iface_mac, kc, 8), k_read = g_read_mac_calls; char k_log[400]; snprintf(k_log, sizeof k_log, "%s", g_lastlog);
      printf("i3 profile %s, Kconfig empty: set calls=%d address as expected=%d (iface type %d), MAC type read %d (want %d), read calls %d, text as expected=%d [%s]; Kconfig set: set calls=%d kconfig bytes=%d read calls=%d [%s]\n",
             CONFIG_C6_ZB_PROFILE_SENGLED_E11 ? "e11" : "neutral", a_calls, a_ok, a_type, a_rtype, a_want_type, a_read, a_text, a_log, k_calls, k_ok, k_read, k_log);
      CHECK(a_calls == 1 && a_ok && a_type == ESP_MAC_IEEE802154 && a_read == 1 && a_rtype == a_want_type && a_text);
      CHECK(k_calls == 1 && k_ok && k_read == 0 && strstr(k_log, "from Kconfig"));
      verdict("i3", "zb_task's address block: Kconfig empty reads the chip's own 802.15.4 address (neutral profile; AABBCCFFFE112233 from AA:BB:CC:11:22:33) or derives B0CE18C600112233 from the base MAC (HARNESS_PROFILE=e11); Kconfig set uses the Kconfig bytes and reads nothing; one esp_iface_mac_addr_set each"); }


    puts("==== Review fixes (x1-x4) ====");
#define X_ID "c6_gfake_0123456789ab"
#define X_URL "http://192.0.2.49:8123/api/webhook/" X_ID

    /* x1. a URL ending in '/' is refused: the mask would cut the empty last segment and show the whole id */
    { const char *bad[] = {X_URL "/", "https://192.0.2.49/api/webhook/" X_ID "/"};
      for (int i = 0; i < 2; i++) {
          char body[300]; snprintf(body, sizeof body, "{\"webhook\":\"%s\"}", bad[i]);
          reset_case(); light_state(0); int code = put_body(body);
          int get = get_settings();
          printf("x1 PUT webhook ending in '/' (#%d): http=%d written=%d; GET http=%d %s\n", i, code, g_cfg_sets, get, g_req.response);
          CHECK(code == 400 && nothing_written() && !strstr(cfg_live.webhook, X_ID));
          CHECK(get == 200 && !resp_has("0123456789ab"));
      }
      reset_case(); light_state(0); int ok = put_body("{\"webhook\":\"" X_URL "\"}");
      printf("x1 the same URL without the slash: http=%d\n", ok);
      CHECK(ok == 200 && !strcmp(cfg_live.webhook, X_URL)); }
    verdict("x1", "a webhook ending in '/' is refused with 400 and nothing written; GET never shows the id");

    /* x2. a URL http_parser_parse_url rejects (esp_http_client would log it whole on every push) is refused */
    { const char *bad[] = {"http://192.0.2.49:81234/api/webhook/" X_ID, "http://192.0.2.49:8l23/api/webhook/" X_ID, "http:///api/webhook/" X_ID};
      for (int i = 0; i < 3; i++) {
          char body[300]; snprintf(body, sizeof body, "{\"webhook\":\"%s\"}", bad[i]);
          reset_case(); light_state(0); int code = put_body(body);
          printf("x2 PUT webhook variant %d: http=%d written=%d\n", i, code, g_cfg_sets);
          CHECK(code == 400 && nothing_written());
      }
      reset_case(); light_state(0); int ok = put_body("{\"webhook\":\"https://192.0.2.49:8123/api/webhook/" X_ID "\"}");
      printf("x2 a good https URL with a port: http=%d\n", ok);
      CHECK(ok == 200); }
    verdict("x2", "a webhook http_parser rejects (port 81234, a letter in the port, no host) is 400 with nothing written");

    /* x3. an NVS failure part-way through a write puts back what the call already wrote (all or nothing, also after a reboot) */
    reset_case(); light_state(0); set_role(CFG_ROLE_NONE); g_fail_set_at = 2;
    { int code = put_body("{\"role\":\"mirror\",\"webhook\":\"http://h/x\"}");
      uint8_t live = cfg_live.role; uint32_t stored_role = 99; kv_u("role", &stored_role);
      cfg_load();
      printf("x3a role none->mirror + webhook, second set fails: http=%d live=%s NVS role=%u webhook=%s; after cfg_load role=%s %s\n",
             code, cfg_role_name(live), stored_role, kv_s("webhook") ? "stored" : "none", cfg_role_name(cfg_live.role), g_req.response);
      CHECK(code == 500 && live == CFG_ROLE_NONE && resp_has("\"storage\""));
      CHECK(stored_role == CFG_ROLE_NONE && cfg_live.role == CFG_ROLE_NONE && !kv_s("webhook")); }
    reset_case(); kv_put_u32("settle_ms", 1500); kv_put_u32("rearm_ms", 700); cfg_load(); g_fail_set_at = 2;
    { int code = put_body("{\"settle_ms\":900,\"rearm_ms\":500}");
      uint32_t a = 0, b = 0; kv_u("settle_ms", &a); kv_u("rearm_ms", &b);
      uint32_t live_s = cfg_live.settle_ms, live_r = cfg_live.rearm_ms;
      cfg_load();
      printf("x3b stored settle 1500 / rearm 700, second set fails: http=%d NVS %u/%u live %u/%u after cfg_load %u/%u\n",
             code, a, b, live_s, live_r, cfg_live.settle_ms, cfg_live.rearm_ms);
      CHECK(code == 500 && a == 1500 && b == 700 && live_s == 1500 && live_r == 700);
      CHECK(cfg_live.settle_ms == 1500 && cfg_live.rearm_ms == 700); }
    reset_case(); kv_put_u32("settle_ms", 1500); cfg_load(); g_cfg_commit_err = ESP_FAIL;
    { int code = put_body("{\"settle_ms\":900}");
      uint32_t a = 0; kv_u("settle_ms", &a);
      printf("x3c commit fails: http=%d NVS settle_ms=%u live=%u\n", code, a, cfg_live.settle_ms);
      CHECK(code == 500 && a == 1500 && cfg_live.settle_ms == 1500); }
    verdict("x3", "a PUT answered 500 storage leaves no key changed in NVS or live (second set fails, or the commit fails)");

    /* x4. a body nested more than one level is refused before cJSON recurses; strings and escapes do not count */
    { char body[520]; int d = 250; size_t n = 0;
      n += (size_t)snprintf(body + n, sizeof body - n, "{\"role\":");
      for (int i = 0; i < d; i++) body[n++] = '[';
      for (int i = 0; i < d; i++) body[n++] = ']';
      body[n++] = '}'; body[n] = 0;
      reset_case(); light_state(0); g_json_parses = 0; int deep = put_body(body);
      printf("x4 %zu-byte body, %d nested arrays: http=%d written=%d cJSON_Parse calls=%d %s\n", n, d, deep, g_cfg_sets, g_json_parses, g_req.response);
      CHECK(deep == 400 && nothing_written() && resp_has("\"bad_request\"") && g_json_parses == 0);
      const char *two[] = {"{\"role\":{\"a\":1}}", "[[1]]", "{\"x\":[1]}"};
      for (int i = 0; i < 3; i++) {
          reset_case(); light_state(0); g_json_parses = 0; int c = put_body(two[i]);
          printf("x4 two levels %s: http=%d cJSON_Parse calls=%d\n", two[i], c, g_json_parses);
          CHECK(c == 400 && nothing_written() && g_json_parses == 0);
      }
      reset_case(); light_state(0); g_json_parses = 0; int s1 = put_body("{\"webhook\":\"http://h/[{[{x\"}");
      printf("x4 brackets inside a string: http=%d\n", s1);
      CHECK(s1 == 200 && g_json_parses == 1 && !strcmp(cfg_live.webhook, "http://h/[{[{x"));
      reset_case(); light_state(0); int s2 = put_body("{\"webhook\":\"http://h/\\\"[[[x\",\"role\":\"mirror\"}");
      printf("x4 escaped quote then brackets inside a string: http=%d\n", s2);
      CHECK(s2 == 200 && cfg_live.role == CFG_ROLE_MIRROR); }
    verdict("x4", "a body nested more than one level is 400 bad_request, nothing written; brackets inside strings (even after an escaped quote) do not count");

    run_code_review();

    puts("==== Secrets ====");

    /* l1. no webhook value in any log line of this run */
    printf("l1 scanned %zu bytes of log lines from %d probes\n", g_loglen, nresults);
    CHECK(g_loglen > 1000 && !strstr(g_logbuf, "zq9") && !strstr(g_logbuf, "0123456789ab") && !strstr(g_logbuf, "gfake") && !strstr(g_logbuf, "api/webhook") && !strstr(g_logbuf, "http://h/persist"));
    verdict("l1", "no webhook URL or id appears in any log line written during the run");

    sanity_probe();
}

int main(int argc, char **argv) {
    g_verbose = getenv("HARNESS_VERBOSE") != NULL;
    const char *mode = argc > 1 ? argv[1] : "new";
    if (!strcmp(mode, "old")) {
        g_mode_role = CFG_ROLE_SHORTCUT; puts("mode: old (the 21 probes of 0.3.24, role shortcut)"); run_old();
    } else if (!strcmp(mode, "old-normal")) {
        g_mode_role = CFG_ROLE_MIRROR; puts("mode: old-normal (the normal-mode probe, role mirror)"); run_old_normal();
    } else if (!strcmp(mode, "new")) {
        g_mode_role = CFG_ROLE_NONE; puts("mode: new (the 0.4.0 probes)"); run_new();
    } else if (!strcmp(mode, "board")) {
        g_mode_role = CFG_ROLE_NONE; puts("mode: board (the 0.5.0 probes)"); run_board();
    } else {
        fprintf(stderr, "usage: harness old|old-normal|new|board\n"); return 2;
    }
    int bad = 0; for (int i = 0; i < nresults; i++) bad += !results[i].pass;
    printf("\nSUMMARY: %d probes, %d pass, %d fail\n", nresults, nresults - bad, bad);
    return bad ? 1 : 0;
}
'''

code = (pre.replace('@@OFF_AFTER_S@@', kdefault(KC_BULB, 'C6_BUTTON_OFF_AFTER_S'))
        .replace('@@VERIFY_TIMEOUT_S@@', kdefault(KC_BASE, 'C6_VERIFY_TIMEOUT_S')).replace('@@VERIFY_STABLE_S@@', kdefault(KC_BASE, 'C6_VERIFY_STABLE_S'))
        .replace('@@VERIFY_ZIGBEE_S@@', kdefault(KC_BASE, 'C6_VERIFY_ZIGBEE_S'))
        .replace('@@CFG_TYPES@@', cat(types)).replace('@@HEADERS@@', headers).replace('@@BOARD_STUBS@@', board_stubs)
        + extracted + '\n' + board_after + '\n' + main.replace('@@BOARD_PROBES@@', board_probes))
(OUT / 'harness.c').write_text(code)
print('wrote', OUT / 'harness.c', 'from', BULB, 'and', SETT)
for grp in (types, decls, sparts, bparts):
    for ln, t in grp:
        print(f'  extracted from line {ln + 1}: {t.splitlines()[0][:70]}')
for label, grp in efiles:
    for ln, t in grp:
        print(f'  extracted from {label} line {ln + 1}: {t.splitlines()[0][:70]}')
