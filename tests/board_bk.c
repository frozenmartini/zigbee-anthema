/* bk8-bk12: the settings of the shortcut role's held press (blink_ms is not a setting; off_after_s defaults to 0) and the critical-section discipline of the flows that
 * release or cancel a held press (bulb.c: light_apply_locked's release block, hub_tap's clear, bulb_role_switch). A press is a Zigbee controller On plus run_button() once the quiet time
 * has passed, as the h* probes do; the release is bulb_button_release. The On/Off attribute writes are read from the recorder in build_harness.py (g_onw_val / g_onw_at / g_nonw). */
static int bk_near(int64_t a, int64_t b, int tol_ms) { int64_t d = a > b ? a - b : b - a; return d <= (int64_t)tol_ms * 1000; }
static void bk_role(int role) { portENTER_CRITICAL(&s_light_mux); cfg_live.role = (uint8_t)role; portEXIT_CRITICAL(&s_light_mux); }
static void bk_boot(void) {                         /* a shortcut board, the stack up, off_after_s 0 (the 0.6.0 default) */
    reset_case(); bk_role(CFG_ROLE_SHORTCUT); cfg_live.off_after_s = 0;
}
static void bk_press(void) { inject_hub_on(); fake_now += 2500000; run_button(); }       /* the Zigbee controller's On, the quiet time (2 s), the press is pushed and held */
static int bk_release(void) { char msg[100]; return bulb_button_release(msg, sizeof msg); }
static int bk_held(void) { bulb_button_snap_t bs; bulb_button_snapshot(&bs); return bs.held; }
static int bk_ncrit0, bk_nest0, bk_unbal0;

static void probe_bk(void) {
    puts("==== bk: the two settings and the release flows ====");
    bk_ncrit0 = g_call_in_crit; bk_nest0 = g_nested; bk_unbal0 = g_crit_unbalanced;
    /* bk8. blink_ms is an unknown key: refused on a PUT, never read from the store, never listed */
    {   const struct {const char *body;} t[3] = {{"{\"blink_ms\":1000}"}, {"{\"blink_ms\":200}"}, {"{\"blink_ms\":\"x\"}"}};
        for (int k = 0; k < 3; k++) {
            reset_case(); cfg_load(); g_auto_bearer = 1; cfg_t before = cfg_live; int c = bd_put(t[k].body);
            int named = strstr(bd_req.response, "blink_ms") != NULL, why = strstr(bd_req.response, "not a setting") != NULL, bad = strstr(bd_req.response, "bad_request") != NULL;
            int same = !memcmp(&before, &cfg_live, sizeof before); int stored = ns_has("stored", "blink_ms"); int kvset = kv_s("blink_ms") != NULL;
            int g = bd_get(); int in_get = strstr(bd_req.response, "blink_ms") != NULL;
            static char sb[4096]; int len; cJSON *j = st_obj(ST_SETTINGS, sb, sizeof sb, &len); const cJSON *so = j ? st_get(j, "settings") : NULL; int in_state = so && st_get(so, "blink_ms") != NULL; cJSON_Delete(j);
            printf("bk8 PUT %s: http=%d bad_request=%d names blink_ms=%d \"not a setting\"=%d; cfg_live unchanged=%d; stored=%d; GET http=%d lists it=%d; state object lists it=%d\n", t[k].body, c, bad, named, why, same, stored, g, in_get, in_state);
            CHECK(c == 400 && bad && named && why && same && !stored && !kvset && g == 200 && !in_get && !in_state);
        }
        /* a value an older firmware stored under that name: not read, not listed */
        reset_case(); kv_put_u32("blink_ms", 1000); cfg_load(); g_auto_bearer = 1; int g = bd_get(); int in_get = strstr(bd_req.response, "blink_ms") != NULL; int stored = ns_has("stored", "blink_ms");
        static char sb2[4096]; int len2; cJSON *j2 = st_obj(ST_SETTINGS, sb2, sizeof sb2, &len2); const cJSON *so2 = j2 ? st_get(j2, "settings") : NULL; int in_state = so2 && st_get(so2, "blink_ms") != NULL; cJSON_Delete(j2);
        printf("bk8 stored blink_ms 1000 then cfg_load: GET http=%d lists it=%d; stored lists it=%d; state object lists it=%d\n", g, in_get, stored, in_state);
        CHECK(g == 200 && !in_get && !stored && !in_state);
    }
    verdict("bk8", "blink_ms is not a setting: a PUT of 1000, 200 or \"x\" is 400 bad_request naming blink_ms with \"not a setting\" and changes nothing; it is in neither GET /settings, stored nor the state object; a blink_ms an older firmware stored is not read and not listed");

    /* bk9. the default on an empty store */
    {   reset_case(); size_t m = g_loglen; cfg_load(); g_auto_bearer = 1; int g = bd_get();
        printf("bk9 empty store: off_after_s=%u; GET http=%d off_after_s:0=%d; stored has off_after_s=%d; CFG line reads 'off_after_s 0, rx_wd_s'=%d\n", cfg_live.off_after_s, g,
               strstr(bd_req.response, "\"off_after_s\":0") != NULL, ns_has("stored", "off_after_s"), log_count(m, "off_after_s 0, rx_wd_s") == 1);
        CHECK(cfg_live.off_after_s == 0 && g == 200 && strstr(bd_req.response, "\"off_after_s\":0"));
        CHECK(!ns_has("stored", "off_after_s") && log_count(m, "off_after_s 0, rx_wd_s") == 1);
    }
    verdict("bk9", "on an empty store the default is off_after_s 0: live, in GET /settings and the CFG line, and not listed as stored");

    /* bk10. a stored off_after_s still wins */
    {   reset_case(); kv_put_u32("off_after_s", 120); cfg_load(); bk_role(CFG_ROLE_SHORTCUT); g_auto_bearer = 1; bd_get();
        int stored = ns_has("stored", "off_after_s"); uint32_t live = cfg_live.off_after_s;
        bk_press(); int64_t held_at = s_held_at_us; int64_t due = s_off_at_us; int held = bk_held();
        g_wait_budget = 1; run_button(); g_wait_budget = 0;
        printf("bk10 stored 120: live=%u stored listed=%d; held=%d, timeout armed %lld s after the press; the Off was written at +%.3f s, writes=%d\n", live, stored, held, (long long)((due - held_at) / 1000000), g_nonw ? (g_onw_at[0] - held_at) / 1e6 : -1.0, g_nonw);
        CHECK(live == 120 && stored && held == 1 && due - held_at == 120000000LL && g_nonw == 1 && g_onw_val[0] == 0 && bk_near(g_onw_at[0] - held_at, 120000000LL, 20) && !bk_held());
    }
    verdict("bk10", "a stored off_after_s of 120 still wins over the new default of 0: live 120, listed as stored, and a held press times out 120 s after it was pushed");

    /* bk11. who releases a held press: the release names the API, the button's timeout, or nothing */
    {   printf("bk11 light_release_by_of: API-RELEASE=%s BUTTON-TIMEOUT=%s ROLE-OFF=%s\n", light_release_by_of("API-RELEASE") ? light_release_by_of("API-RELEASE") : "NULL",
               light_release_by_of("BUTTON-TIMEOUT") ? light_release_by_of("BUTTON-TIMEOUT") : "NULL", light_release_by_of("ROLE-OFF") ? light_release_by_of("ROLE-OFF") : "NULL");
        CHECK(light_release_by_of("API-RELEASE") && !strcmp(light_release_by_of("API-RELEASE"), "api"));
        CHECK(light_release_by_of("BUTTON-TIMEOUT") && !strcmp(light_release_by_of("BUTTON-TIMEOUT"), "timeout"));
        CHECK(light_release_by_of("ROLE-OFF") == NULL);
    }
    verdict("bk11", "light_release_by_of: API-RELEASE is \"api\", BUTTON-TIMEOUT is \"timeout\", ROLE-OFF is NULL (the role switch clears the button anyway)");

    /* bk12. critical-section discipline over the flows that release or cancel a held press */
    {   int c0 = g_call_in_crit, n0 = g_nested, u0 = g_crit_unbalanced;
        bk_boot(); bk_press(); int held_a = bk_held(); int rc = bk_release(); int held_b = bk_held();                     /* the client's release */
        bk_boot(); bk_press(); g_auto_bearer = 1; int code = bd_put("{\"role\":\"mirror\"}");                              /* the role switch */
        bk_boot(); bk_press(); inject_hub_off(); run_button(); fake_now += 10000000; g_wait_budget = 3; run_button(); g_wait_budget = 0;   /* the Zigbee controller's cancel of the press */
        printf("bk12 over a release, a role switch and a Zigbee controller cancel (held %d, release rc=%d, held after %d, role PUT http=%d): call-in-section +%d, nested +%d, unbalanced +%d; over the whole bk area: +%d / +%d / +%d; sections open now=%d; zigbee lock held=%d; total call-in-section so far=%d\n",
               held_a, rc, held_b, code, g_call_in_crit - c0, g_nested - n0, g_crit_unbalanced - u0, g_call_in_crit - bk_ncrit0, g_nested - bk_nest0, g_crit_unbalanced - bk_unbal0, g_crit, g_zb_locked, g_call_in_crit);
        CHECK(held_a == 1 && rc == 0 && !held_b && code == 200);
        CHECK(g_call_in_crit == c0 && g_nested == n0 && g_crit_unbalanced == u0 && g_crit == 0 && !g_zb_locked);
        CHECK(g_call_in_crit == bk_ncrit0 && g_nested == bk_nest0 && g_crit_unbalanced == bk_unbal0 && g_call_in_crit == 0);
    }
    verdict("bk12", "over the whole bk area (the client's release of a held press, the role switch and the Zigbee controller's cancel of a press): no log, allocation, delay, Zigbee or attribute call inside a critical section (the counter stays 0), no nesting, no unbalanced exit; the Zigbee lock is always taken before the mux and released");
}
