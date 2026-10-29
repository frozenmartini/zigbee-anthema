/* lt1-lt6: api/v1/light.c (POST /api/v1/light and /api/v1/button/release), with bulb.c's bulb_light_set / bulb_button_release behind it. */
static httpd_req_t lt_r;
static void lt_fresh(int on_now, int role) {                                 /* a claimed board in `role`, the light at on_now, the stack up */
    reset_case(); cfg_live.role = (uint8_t)role; s_light.val[L_ON] = s_hub.val[L_ON] = (uint16_t)on_now; model_on = on_now;
}
static int lt_post(const char *body) { req_init(&lt_r); req_body(&lt_r, body); api_call(light_v1_post, &lt_r); return lt_r.code; }
static int lt_names(const char *field) { char p[48]; snprintf(p, sizeof p, "\"message\":\"%s:", field); return strstr(lt_r.response, p) != NULL; }
static int lt_err_is(int code, const char *word) {                            /* a JSON error object with exactly error + message, as application/json */
    cJSON *j = cJSON_Parse(lt_r.response); int ok = lt_r.code == code && j && cJSON_IsObject(j) && !strcmp(lt_r.type, "application/json");
    const cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL, *m = j ? cJSON_GetObjectItemCaseSensitive(j, "message") : NULL;
    ok = ok && e && cJSON_IsString(e) && !strcmp(e->valuestring, word) && m && cJSON_IsString(m) && cJSON_GetArraySize(j) == 2; cJSON_Delete(j); return ok;
}
static int lt_marks_since(int n0, uint32_t mask) { int c = 0; for (int i = n0; i < g_nmarks && i < 512; i++) c += (g_marks[i] & mask) != 0; return c; }
static const char *lt_ok_word = "{\"ok\":true}";

static void lt_hook_release(void) { httpd_req_t q; req_init(&q); api_call(button_release_post, &q); }

static void probe_lt(void) {
    puts("==== lt: POST /api/v1/light and /button/release ====");
    /* lt1. the body: on, level, unknown keys, shapes, size */
    {   static char big[300];
        const struct {const char *body, *field;} rej[] = {
            {"{\"on\":2}", "on"}, {"{\"on\":\"1\"}", "on"}, {"{}", "on"}, {"{\"on\":1,\"level\":0}", "level"}, {"{\"on\":1,\"level\":255}", "level"},
            {"{\"on\":1,\"level\":\"5\"}", "level"}, {"{\"on\":1,\"level\":1.5}", "level"}, {"{\"on\":1,\"bri\":5}", "bri"}, {"null", "body"},
            {"{\"on\":1,\"level\":null}", "level"}, {"{\"on\":1,\"level\":{\"a\":1}}", "level"}, {"", "body"}, {"[1]", "body"}, {"not json", "body"},
            {"{\"on\":1,\"on\":0}", "on"}, {"{\"on\":true}", "on"}, {"{\"on\":-1}", "on"}};
        int nrej = (int)(sizeof rej / sizeof rej[0]), bad = 0;
        for (int i = 0; i < nrej; i++) {
            lt_fresh(0, CFG_ROLE_MIRROR); light_state_t before = s_light; int m0 = g_nmarks;
            lt_post(rej[i].body);
            int good = lt_err_is(400, "bad_request") && lt_names(rej[i].field) && g_set_calls == 0 && !memcmp(&before, &s_light, sizeof before) && g_nposts == 0 && lt_marks_since(m0, C6_ST_LIGHT) == 0;
            if (!good) { bad++; printf("  lt1 REFUSAL WRONG for body '%s': http=%d response=%s sets=%d\n", rej[i].body, lt_r.code, lt_r.response, g_set_calls); }
        }
        lt_fresh(0, CFG_ROLE_MIRROR); memset(big, ' ', 257); big[257] = 0; memcpy(big, "{\"on\":1}", 8); /* 257 bytes of valid JSON (an object, then spaces): only the size limit can refuse it */
        { int m0 = g_nmarks; (void)m0; lt_post(big); int over = lt_err_is(400, "bad_request") && lt_names("body") && g_set_calls == 0; if (!over) bad++; printf("  lt1 a 257-byte body: http=%d sets=%d\n", lt_r.code, g_set_calls); }
        printf("lt1 refused: %d bodies (on 2 / \"1\" / true / -1, missing on, level 0 / 255 / \"5\" / 1.5 / null / object, bri, null, empty, array, not JSON, duplicate, 257 bytes), wrong answers=%d\n", nrej + 1, bad);
        CHECK(bad == 0);
        const struct {const char *body; int on_now, on, level;} acc[] = {
            {"{\"on\":1}", 0, 1, -1}, {"{\"on\":0}", 1, 0, -1}, {"{\"on\":1,\"level\":1}", 0, 1, 1}, {"{\"on\":1,\"level\":254}", 0, 1, 254}, {"{\"on\":0,\"level\":100}", 1, 0, 100}};
        int abad = 0;
        for (size_t i = 0; i < sizeof acc / sizeof acc[0]; i++) {
            lt_fresh(acc[i].on_now, CFG_ROLE_MIRROR); lt_post(acc[i].body);
            int good = lt_r.code == 200 && !strcmp(lt_r.response, lt_ok_word) && g_set_calls > 0 && s_light.val[L_ON] == acc[i].on && (acc[i].level < 0 || s_light.val[L_LEVEL] == acc[i].level);
            if (!good) { abad++; printf("  lt1 ACCEPT WRONG for %s: http=%d response=%s sets=%d on=%u level=%u\n", acc[i].body, lt_r.code, lt_r.response, g_set_calls, s_light.val[L_ON], s_light.val[L_LEVEL]); }
        }
        lt_fresh(0, CFG_ROLE_MIRROR); memset(big, ' ', 256); big[256] = 0; memcpy(big, "{\"on\":1}", 8); int c256 = lt_post(big);       /* exactly 256 bytes */
        printf("lt1 accepted: 5 bodies (on 0/1, level 1 and 254), wrong answers=%d; a body of exactly %zu bytes: http=%d model_on=%d\n", abad, strlen(big), c256, model_on);
        CHECK(abad == 0 && strlen(big) == 256 && c256 == 200 && model_on == 1);
    }
    verdict("lt1", "POST /light: on 0/1 and level 1-254 are applied (a 256-byte body too); every other value, an unknown key, a duplicate, null, an array, an object value, not JSON, an empty or 257-byte body is 400 bad_request naming the field, with nothing applied");

    /* lt2. colour keys */
    {   const struct {const char *body; int mode, a, b; int which;} acc[] = {          /* which: 0 ct (a), 1 hs (a hue, b sat), 2 xy */
            {"{\"on\":1,\"ct\":1}", 2, 1, 0, 0}, {"{\"on\":1,\"ct\":65279}", 2, 65279, 0, 0}, {"{\"on\":1,\"ct\":300}", 2, 300, 0, 0},
            {"{\"on\":1,\"hs\":[0,0]}", 0, 0, 0, 1}, {"{\"on\":1,\"hs\":[254,254]}", 0, 254, 254, 1}, {"{\"on\":1,\"hs\":[10,20]}", 0, 10, 20, 1},
            {"{\"on\":1,\"xy\":[0,0]}", 1, 0, 0, 2}, {"{\"on\":1,\"xy\":[65279,65279]}", 1, 65279, 65279, 2}, {"{\"on\":1,\"xy\":[100,200]}", 1, 100, 200, 2}};
        int abad = 0;
        for (size_t i = 0; i < sizeof acc / sizeof acc[0]; i++) {
            lt_fresh(0, CFG_ROLE_MIRROR); lt_post(acc[i].body); int w = acc[i].which;
            int mapped = w == 0 ? (s_light.val[L_CT] == acc[i].a && (s_light.seen & (1u << L_CT)) && !(s_light.seen & ((1u << L_X) | (1u << L_Y) | (1u << L_HUE) | (1u << L_SAT))))
                       : w == 1 ? (s_light.val[L_HUE] == acc[i].a && s_light.val[L_SAT] == acc[i].b && (s_light.seen & (1u << L_HUE)) && (s_light.seen & (1u << L_SAT)) && !(s_light.seen & ((1u << L_X) | (1u << L_Y) | (1u << L_CT))))
                       : (s_light.val[L_X] == acc[i].a && s_light.val[L_Y] == acc[i].b && (s_light.seen & (1u << L_X)) && (s_light.seen & (1u << L_Y)) && !(s_light.seen & ((1u << L_HUE) | (1u << L_SAT) | (1u << L_CT))));
            int good = lt_r.code == 200 && !strcmp(lt_r.response, lt_ok_word) && mapped && (s_light.seen & (1u << L_MODE)) && s_light.val[L_MODE] == acc[i].mode;
            if (!good) { abad++; printf("  lt2 ACCEPT WRONG for %s: http=%d response=%s mode=%u\n", acc[i].body, lt_r.code, lt_r.response, s_light.val[L_MODE]); }
        }
        printf("lt2 accepted: 9 bodies (ct 1 / 65279 / 300 -> CT, mode 2; hs [0,0] / [254,254] / [10,20] -> hue + sat, mode 0, x y untouched; xy [0,0] / [65279,65279] / [100,200] -> x y, mode 1), wrong=%d\n", abad);
        CHECK(abad == 0);
        const struct {const char *body, *field;} rej[] = {
            {"{\"on\":1,\"ct\":0}", "ct"}, {"{\"on\":1,\"ct\":65280}", "ct"}, {"{\"on\":1,\"ct\":\"300\"}", "ct"}, {"{\"on\":1,\"ct\":1.5}", "ct"},
            {"{\"on\":1,\"hs\":[255,0]}", "hs"}, {"{\"on\":1,\"hs\":[0,255]}", "hs"}, {"{\"on\":1,\"hs\":[-1,0]}", "hs"}, {"{\"on\":1,\"hs\":[1]}", "hs"},
            {"{\"on\":1,\"hs\":[1,2,3]}", "hs"}, {"{\"on\":1,\"hs\":[\"a\",1]}", "hs"}, {"{\"on\":1,\"hs\":[[1],2]}", "hs"}, {"{\"on\":1,\"hs\":[{\"a\":1},2]}", "hs"},
            {"{\"on\":1,\"hs\":7}", "hs"}, {"{\"on\":1,\"xy\":[65280,0]}", "xy"}, {"{\"on\":1,\"xy\":[0,65280]}", "xy"}, {"{\"on\":1,\"xy\":[[1],[2]]}", "xy"},
            {"{\"on\":1,\"ct\":300,\"hs\":[1,2]}", "hs"}, {"{\"on\":1,\"ct\":300,\"xy\":[1,2]}", "xy"}, {"{\"on\":1,\"hs\":[1,2],\"xy\":[1,2]}", "xy"},
            {"{\"on\":1,\"ct\":300,\"hs\":[1,2],\"xy\":[1,2]}", "hs"}, {"{\"on\":1,\"ct\":300,\"ct\":301}", "ct"}};
        int nrej = (int)(sizeof rej / sizeof rej[0]), bad = 0;
        for (int i = 0; i < nrej; i++) {
            lt_fresh(0, CFG_ROLE_MIRROR); light_state_t before = s_light; lt_post(rej[i].body);
            int good = lt_err_is(400, "bad_request") && lt_names(rej[i].field) && g_set_calls == 0 && !memcmp(&before, &s_light, sizeof before);
            if (!good) { bad++; printf("  lt2 REFUSAL WRONG for '%s': http=%d response=%s sets=%d\n", rej[i].body, lt_r.code, lt_r.response, g_set_calls); }
        }
        printf("lt2 refused: %d bodies (ct 0 / 65280 / text / 1.5; hs outside 0-254, short, long, text, nested; xy outside 0-65279; each pair and the triple of colour keys; a duplicate), wrong answers=%d\n", nrej, bad);
        CHECK(bad == 0);
    }
    verdict("lt2", "ct 1-65279 sets CT with mode 2; hs [0-254,0-254] sets hue and sat with mode 0 and leaves x/y; xy [0-65279,...] sets x/y with mode 1; every out-of-range or malformed value, a nested array, and every pair of colour keys is 400 naming the key with nothing applied");

    /* lt3. the role and the echo window */
    {   lt_fresh(0, CFG_ROLE_SHORTCUT); int code = lt_post("{\"on\":1}");
        printf("lt3 role shortcut: http=%d error word ok=%d, attribute sets=%d, model_on=%d\n", code, lt_err_is(409, "wrong_role"), g_set_calls, model_on);
        CHECK(lt_err_is(409, "wrong_role") && g_set_calls == 0 && model_on == 0 && s_light.val[L_ON] == 0);
        const struct {const char *what; int64_t ago; int applied;} cs[] = {{"2.9 s ago", 2900000, 0}, {"3.0 s ago", 3000000, 1}, {"none ever", -1, 1}, {"0.1 s ago", 100000, 0}, {"3.1 s ago", 3100000, 1}};
        for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
            lt_fresh(0, CFG_ROLE_MIRROR); s_hub_cmd_us = cs[i].ago < 0 ? 0 : fake_now - cs[i].ago; int c = lt_post("{\"on\":1}");
            printf("lt3 mirror, Zigbee controller command %s: http=%d response=%s sets=%d model_on=%d\n", cs[i].what, c, lt_r.response, g_set_calls, model_on);
            if (cs[i].applied) CHECK(c == 200 && !strcmp(lt_r.response, lt_ok_word) && g_set_calls > 0 && model_on == 1);
            else CHECK(c == 200 && !strcmp(lt_r.response, "{\"ok\":false,\"ignored\":\"controller_command_3s\"}") && g_set_calls == 0 && model_on == 0 && s_light.val[L_ON] == 0);
        }
    }
    verdict("lt3", "the shortcut role is 409 wrong_role with nothing applied; in the mirror role a Zigbee controller command 2.9 s ago gives 200 ignored controller_command_3s with nothing applied; 3.0 s ago and none ever are applied");

    /* lt4. the outcomes */
    {   lt_fresh(0, CFG_ROLE_MIRROR); int m0 = g_nmarks; size_t l0 = g_loglen; lt_post("{\"on\":1,\"level\":77}");
        printf("lt4 ok: http=%d response=%s, light.by=%s, ST_LIGHT marks=%d, webhook posts=%d http clients=%d, log 'API-SET' lines=%d\n", lt_r.code, lt_r.response, s_light_by ? s_light_by : "(null)", lt_marks_since(m0, C6_ST_LIGHT), g_nposts, g_http_inits, log_count(l0, "API-SET"));
        CHECK(lt_r.code == 200 && !strcmp(lt_r.response, lt_ok_word) && s_light_by && !strcmp(s_light_by, "api") && lt_marks_since(m0, C6_ST_LIGHT) >= 1 && g_nposts == 0 && g_http_inits == 0
              && log_count(l0, "API-SET") >= 1 && model_on == 1 && s_light.val[L_LEVEL] == 77);
        lt_fresh(0, CFG_ROLE_MIRROR); s_zb_up = false; lt_post("{\"on\":1}");
        printf("lt4 stack not up: http=%d, sets=%d\n", lt_r.code, g_set_calls);
        CHECK(lt_err_is(503, "zigbee_busy") && g_set_calls == 0 && s_light.val[L_ON] == 0);
        lt_fresh(0, CFG_ROLE_MIRROR); lock_ok = false; lt_post("{\"on\":1}");
        printf("lt4 lock busy: http=%d, sets=%d\n", lt_r.code, g_set_calls);
        CHECK(lt_err_is(503, "zigbee_busy") && g_set_calls == 0 && s_light.val[L_ON] == 0);
        lt_fresh(0, CFG_ROLE_MIRROR); g_set_fail_on_off = 1; m0 = g_nmarks; lt_post("{\"on\":1}");
        printf("lt4 attribute refused: http=%d response=%s, on recorded=%u, webhook posts=%d\n", lt_r.code, lt_r.response, s_light.val[L_ON], g_nposts);
        CHECK(lt_r.code == 200 && !strncmp(lt_r.response, "{\"ok\":false,\"ignored\":\"", 23) && strstr(lt_r.response, "on=1") && strstr(lt_r.response, "0x86") && s_light.val[L_ON] == 0 && g_nposts == 0);
    }
    verdict("lt4", "ok: 200 {\"ok\":true} through light_apply as API-SET (by api, ST_LIGHT marked, no webhook); stack down or lock busy: 503 zigbee_busy; an attribute refused: 200 {\"ok\":false,\"ignored\":<light_apply's message>}");

    /* lt5. the credential: token class, developer header, and the old /zb/light */
    {   int bad = 0; const char *hs[2] = {"POST /light", "POST /button/release"};
        for (int h = 0; h < 2; h++) {
            for (int st = 0; st < 4; st++) {                                   /* 0 unclaimed closed, 1 unclaimed open, 2 claimed no token, 3 claimed wrong token */
                lt_fresh(h, h ? CFG_ROLE_SHORTCUT : CFG_ROLE_MIRROR); s_held = h; au_state(st >= 2, st == 1);
                req_init(&lt_r); req_body(&lt_r, "{\"on\":1}"); if (st == 3) lt_r.h_auth = "Bearer wrong-zq9-token-0123456789abcdef";
                api_call(h ? button_release_post : light_v1_post, &lt_r);
                int good = lt_err_is(st < 2 ? 403 : 401, st < 2 ? "claim_required" : "unauthorized") && lt_r.recvs == 0 && g_set_calls == 0 && (!h || s_held);
                if (!good) { bad++; printf("  lt5 WRONG: %s, state %d: http=%d response=%s recvs=%d sets=%d\n", hs[h], st, lt_r.code, lt_r.response, lt_r.recvs, g_set_calls); }
            }
            lt_fresh(h, h ? CFG_ROLE_SHORTCUT : CFG_ROLE_MIRROR); s_held = h;                                 /* claimed, the right Bearer */
            req_init(&lt_r); req_body(&lt_r, "{\"on\":1}"); api_call(h ? button_release_post : light_v1_post, &lt_r);
            int okb = lt_r.code == 200 && g_set_calls > 0; if (!okb) { bad++; printf("  lt5 WRONG: %s with the claim token: http=%d\n", hs[h], lt_r.code); }
            for (int claimed = 0; claimed < 2; claimed++) {                                                  /* the developer header, in a build with a 24-char token */
                lt_fresh(h, h ? CFG_ROLE_SHORTCUT : CFG_ROLE_MIRROR); s_held = h; g_kconfig_ota_token = AU_DEV24; au_state(claimed, false);
                req_init(&lt_r); req_body(&lt_r, "{\"on\":1}"); lt_r.h_ota = AU_DEV24; api_call(h ? button_release_post : light_v1_post, &lt_r);
                int okd = lt_r.code == 200 && g_set_calls > 0; if (!okd) { bad++; printf("  lt5 WRONG: %s with the developer header (claimed=%d): http=%d response=%s\n", hs[h], claimed, lt_r.code, lt_r.response); }
            }
        }
        printf("lt5 /light and /button/release x (unclaimed closed / open, claimed no token / wrong token -> JSON 403 claim_required / 401 unauthorized, no body read, nothing applied; the claim token and the developer header -> 200), wrong=%d\n", bad);
        CHECK(bad == 0);
        lt_fresh(0, CFG_ROLE_MIRROR); au_state(true, false); httpd_req_t z = {.on = "1"}; esp_err_t rc = light_post(&z);
        printf("lt5 old POST /zb/light, claimed, no credential: rc=%d http=%d response=%.30s sets=%d model_on=%d\n", rc, z.code, z.response, g_set_calls, model_on);
        CHECK(rc == 0 && z.code == 403 && g_set_calls == 0 && model_on == 0 && s_light.val[L_ON] == 0);
        lt_fresh(0, CFG_ROLE_MIRROR); g_kconfig_ota_token = AU_DEV24; au_state(true, false); httpd_req_t z2 = {.on = "1", .h_ota = AU_DEV24}; light_post(&z2);
        printf("lt5 old POST /zb/light with the developer header: http=%d sets=%d model_on=%d\n", z2.code, g_set_calls, model_on);
        CHECK(z2.code == 200 && g_set_calls > 0 && model_on == 1);
        lt_fresh(0, CFG_ROLE_MIRROR); httpd_req_t z3 = {.on = "1"}; light_post(&z3);                          /* the claim Bearer (g_auto_bearer) */
        printf("lt5 old POST /zb/light with the claim token: http=%d sets=%d\n", z3.code, g_set_calls);
        CHECK(z3.code == 200 && g_set_calls > 0);
    }
    verdict("lt5", "/light and /button/release: unclaimed 403 claim_required, claimed with no or a wrong token 401 unauthorized, both JSON with no body read and nothing applied; the claim token and the developer header work; the old POST /zb/light with no credential is 403 and applies nothing");

    /* lt6. POST /button/release */
    {   lt_fresh(1, CFG_ROLE_SHORTCUT); s_held = true; int m0 = g_nmarks; size_t l0 = g_loglen; req_init(&lt_r); api_call(button_release_post, &lt_r);
        printf("lt6 shortcut, held: http=%d response=%s, model_on=%d (Off sets %d), held=%d, release_by=%s, ST_BUTTON marks=%d, 'API-RELEASE' log lines=%d, posts=%d\n", lt_r.code, lt_r.response, model_on, g_off_sets, s_held,
               s_release_last ? s_release_last : "(null)", lt_marks_since(m0, C6_ST_BUTTON), log_count(l0, "API-RELEASE"), g_nposts);
        CHECK(lt_r.code == 200 && !strcmp(lt_r.response, lt_ok_word) && model_on == 0 && g_off_sets == 1 && !s_held && s_release_last && !strcmp(s_release_last, "api") && lt_marks_since(m0, C6_ST_BUTTON) >= 1 && log_count(l0, "API-RELEASE") >= 1);
        run_button();                                                                                     /* the client's own release is not echoed back to the webhook */
        printf("lt6 then the button task: release posts=%d, release_by=%s, light.by=%s, ST_BUTTON marks=%d\n", g_release_posts, s_release_by ? s_release_by : "(null)", s_light_by ? s_light_by : "(null)", lt_marks_since(m0, C6_ST_BUTTON));
        CHECK(g_release_posts == 0 && !s_release_by && s_light_by && !strcmp(s_light_by, "api") && lt_marks_since(m0, C6_ST_BUTTON) == 1);
        lt_fresh(1, CFG_ROLE_SHORTCUT); s_held = false; req_init(&lt_r); api_call(button_release_post, &lt_r);
        printf("lt6 shortcut, not held: http=%d, Off sets=%d\n", lt_r.code, g_off_sets);
        CHECK(lt_err_is(409, "no_press") && g_set_calls == 0 && model_on == 1);
        lt_fresh(1, CFG_ROLE_MIRROR); s_held = true; req_init(&lt_r); api_call(button_release_post, &lt_r);
        printf("lt6 role mirror: http=%d\n", lt_r.code); CHECK(lt_err_is(409, "wrong_role") && g_set_calls == 0 && model_on == 1);
        lt_fresh(1, CFG_ROLE_NONE); s_held = true; req_init(&lt_r); api_call(button_release_post, &lt_r);
        printf("lt6 role none: http=%d\n", lt_r.code); CHECK(lt_err_is(409, "wrong_role") && g_set_calls == 0);
        lt_fresh(1, CFG_ROLE_SHORTCUT); s_held = true; g_set_fail_on_off = 1; req_init(&lt_r); api_call(button_release_post, &lt_r);
        printf("lt6 the Off refused: http=%d, held=%d, model_on=%d\n", lt_r.code, s_held, model_on);
        CHECK(lt_err_is(503, "zigbee_busy") && s_held && model_on == 1);
        lt_fresh(1, CFG_ROLE_SHORTCUT); s_held = true; lock_ok = false; req_init(&lt_r); api_call(button_release_post, &lt_r);
        printf("lt6 the lock busy: http=%d, held=%d\n", lt_r.code, s_held);
        CHECK(lt_err_is(503, "zigbee_busy") && s_held && model_on == 1);
        /* the touchscreen's Off lands while the release waits for the Zigbee lock: already released, so 409 no_press (not 503), no Off written */
        lt_fresh(1, CFG_ROLE_SHORTCUT); s_held = true; g_acquire_hook = inject_hub_off; req_init(&lt_r); api_call(button_release_post, &lt_r);
        printf("lt6 a Zigbee controller Off while the release waits for the lock: http=%d, held=%d, release_by=%s, Off sets=%d\n", lt_r.code, s_held, s_release_by ? s_release_by : "(null)", g_off_sets);
        CHECK(lt_err_is(409, "no_press") && !s_held && s_release_by && !strcmp(s_release_by, "zigbee") && g_off_sets == 0);
        /* old probe 6, with the release arriving through the endpoint: the press webhook, then one release webhook by api */
        lt_fresh(1, CFG_ROLE_SHORTCUT); s_press_pending = true; g_press_hook = lt_hook_release; run_button();
        printf("lt6 old probe 6 through /button/release: posts=%d [", g_nposts);
        for (int i = 0; i < g_nposts; i++) printf("%s%s", i ? " | " : "", g_posts[i]);
        printf("], model_on=%d, held=%d, delivering=%d\n", model_on, s_held, s_delivering);
        CHECK(g_nposts == 2 && post_has(0, "\"press\":1") && post_has(1, "\"release\":1") && post_has(1, "\"by\":\"api\"") && g_press_posts == 1 && g_release_posts == 1 && !model_on && !s_held && !s_delivering && !s_release_by);
    }
    verdict("lt6", "POST /button/release: shortcut and held, Off applied (release_by api, held cleared, 200 ok) and old probe 6's two posts unchanged; not held 409 no_press; another role 409 wrong_role; the Off refused or the lock busy 503 zigbee_busy with the press still held; a Zigbee controller Off while it waits for the lock 409 no_press with no Off written");
}
