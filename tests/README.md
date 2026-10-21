# tests/ — the host harness for the firmware (firmware 0.7.0)

No board needed. `build_harness.py` copies functions out of `bulb/main/bulb.c` and `bulb/main/api/v1/settings.c`
by name (verbatim) into a stub-compiled C program; cJSON in `vendor/` is the real library. Since 0.5.0 it also
extracts from `state.c`, `events.c`, `light.c` and `components/c6_base/{c6_wifi,c6_led,c6_auth,c6_button,c6_discovery,improv,portal,dns}.c`
(`run.sh` derives every path from its own location; `BULB_C`, `SETTINGS_C`, `SETTINGS_H`, `STATE_C`, `EVENTS_C`, `LIGHT_C`, `AUTH_C`, `LED_C`, `DISC_C`, `BASE_C`, `WIFI_C`, `BUTTON_C`, `IMPROV_C`, `PORTAL_C`, `DNS_C`, `C6_COMP` override).

- Run: `./run.sh --old | --old-normal | --new | --board | --all | --compile` (needs `python3`, `gcc`). Output goes to
  `$HARNESS_OUT` (default `/tmp/c6-harness-3.0`), never into this tree. `HARNESS_VERBOSE=1` prints the firmware's log lines.
  `HARNESS_PROFILE=neutral|e11` picks the identity profile the firmware is compiled for (`neutral`, the default, is the public default build;
  `e11` is the Sengled-clone build option). Both must pass with the same counts: run `--all` once under each. `i3` (the address) and `hu1` (the
  manufacturer, model and code) assert the profile's own values.
- `--old`: the 21 probes of 0.3.24 (ids `1`-`9`, `a`-`g`, `N1`, `N2`, `N5`, `r`, `z`) with the role set to `shortcut`.
  `--old-normal`: the one normal-mode probe (`n`) with the role `mirror`. `--new`: the 0.4.0 probes, ids `c*` settings and
  PUT validation, `r*` role change, `h*` Zigbee controller tap by role, `p*` HTTP/restore by role, `w1`, `i*` IEEE, `gd`, `g1`, `s1`, `t1`, `x1`-`x4` (four regressions: webhook trailing `/`, `http_parser` check,
  NVS write-back, body nesting), `l1`, `z`. `vendor/http_parser.c` (IDF v5.5.5's) is compiled like cJSON, warnings off.
- `--new` also runs `c9` (the `hue` setting: default, boolean, u8 storage, refused values, `pending` beside `ext_pan`) and seven regressions (`cr1`-`cr7`): stale Off across role changes,
  masking URL path IDs beside queries/fragments, release retry cancellation, a pending settle update,
  held timeout changes, complete-body JSON parsing and decoded-NUL validation. Scheduling hooks run actual
  settings PUTs during lock waits, retry delays and a simulated blocked task wait. New helper extraction
  is optional so the same probes compile against untouched pre-fix sources and prove each defect first.
- `--board`: the 106 probes: the 0.5.0 package and 0.5.1's `zr5`, `zr6` and 0.5.2's `zr7`, `zr8` (69), and 0.6.0's `up1`-`up13`, `vf1`-`vf8`, `bk8`-`bk12`, and 0.7.0's `fl1`-`fl4`, `rb1`, `bt5`, `ld3`, `ld4`, `hu1`-`hu3` (`board_<area>.c`, run in this order, `cx` last), ids a two-letter prefix and a number:
  `wf` Wi-Fi (c6_wifi.c) · `ld` the colour LED (`ld1`-`ld4`: the five sources and their colours on a stubbed `led_strip`, identify's `rgb`, the claim) · `au` auth · `bt` button (`bt5`: the LED cues of a hold) · `dc` discovery/mDNS · `zr` Zigbee steer/retry · `gp` Zigbee controller group ·
  `st` /state · `hu` the colour shape of the Zigbee endpoint with `hue` on and off (`create_device` through stubs of the ZCL descriptor calls) and `zigbee.hue` as the boot value · `ev` /events (SSE) · `fl` the close flush before `event: end` and the 2 s restart delay (`rb1`) · `lt` /light · `ns` settings `name` and `ext_pan` · `im` Improv serial · `pt` portal and captive DNS ·
  `cv` code review regressions (in-flight save/erase, credential rollback, failed claim release, Zigbee controller/persistence SSE updates, echo guard during lock wait) ·
  `up` POST /api/v1/update (the real `update_post`: checks, cadence, flash and boot-slot failures, busy, the state object) ·
  `vf` the first-start check (the real `verify_task` on fake time, the Zigbee bit and its `on_signal` sites) ·
  `bk` an unknown settings key (the name of a setting that no longer exists), the API's release, the Zigbee controller and role cancels, the `off_after_s` default ·
  `cx` cross-cutting (cx1 critical-section counters, cx2 error shape, cx3 secrets scan). Stubs: `board_stubs.c`, `board_stubs_wifi.c`, `board_stubs_ota.c`, `board_after.c`.
  `board_stubs_ota.c` (0.6.0) is the OTA API (`esp_ota_*`, one `ota_1` slot with scripted failures, counters, the SHA-256 of the written bytes), the running/slot descriptions
  and a FreeRTOS event group on fake time (`xEventGroupWaitBits` moves `fake_now` and fires the bits a probe scheduled with `eg_at`); `board_stubs.c` has a real incremental
  SHA-256 (`mbedtls_sha256_*`, also behind the one-shot) and the IDF 5.5 layout of `esp_app_desc_t`. The Kconfig defaults the probes need (`C6_BUTTON_OFF_AFTER_S`,
  `C6_VERIFY_*`) are read from the Kconfig files by `build_harness.py`, so a changed default is a changed test. `c6_base_zigbee_ready` is compiled a second time with
  `CONFIG_C6_VERIFY_BREAK_TEST 1` (`brk_`), and `verify_task` against the weak hook (`weak_`), because both are decided by the preprocessor / the linker, not at run time.
  `--all` runs the four modes (21 + 1 + 44 + 106 = 172).
- Mutation check: copy `bulb components tools tests` to `/tmp/c6-mut-N`, `sed -i` one line of the copy, run
  that copy's `tests/run.sh --board`; the probe a mutation is meant to break must fail and no other.
- `test_cfg_push.py` is `tools/cfg_push.py`'s test against a stand-in HTTP server on 127.0.0.1 (108 `ok:`): the token from `--token <file>` or `ZIGBEE_ANTHEMA_TOKEN`, sent as both `X-OTA-Token` and `Authorization: Bearer` on every request, and the exit-2 form without one (or with a token a header cannot carry), `hue=true|false` as a JSON boolean, any other `hue` refused. `test_ota_push.py` is `tools/ota_push.py`'s offline test (`urlopen` replaced by a recording fake, 49 `ok:`), with the same token cases (both headers on each path that sends one) and the lost-answer path (a timeout, a reset or an answer cut short after the update POST's body went: one note line, then `/info` is watched; a failure while sending, wrapped in `URLError`, still exits). Run each by name: `PYTHONDONTWRITEBYTECODE=1 python3 test_cfg_push.py`.
