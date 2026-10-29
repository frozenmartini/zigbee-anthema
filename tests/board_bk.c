/* bk8, bk11, bk12: the settings of the shortcut role (the keys a firmware removed are not settings: settle_ms, rearm_ms and off_after_s since 0.8.0, blink_ms before) and the
 * critical-section discipline of the flows that release or cancel a held press (bulb.c: light_apply_locked's release block, hub_tap's clear, bulb_role_switch). A press is a Zigbee
 * controller On plus run_button() once, as the h* probes do; the release is bulb_button_release. The ids bk9 and bk10 (the off_after_s default and a stored value) are not used.
 * The On/Off attribute writes are read from the recorder in build_harness.py (g_onw_val / g_onw_at / g_nonw). */
static void bk_role(int role) { portENTER_CRITICAL(&s_light_mux); cfg_live.role = (uint8_t)role; portEXIT_CRITICAL(&s_light_mux); }
static void bk_boot(void) {                         /* a shortcut board, the stack up */
    reset_case(); bk_role(CFG_ROLE_SHORTCUT);
}
static void bk_press(void) { inject_hub_on(); run_button(); }       /* the Zigbee controller's On; the press is pushed at once and held */
static int bk_release(void) { char msg[100]; return bulb_button_release(msg, sizeof msg); }
static int bk_held(void) { bulb_button_snap_t bs; bulb_button_snapshot(&bs); return bs.held; }
static int bk_ncrit0, bk_nest0, bk_unbal0;

static void probe_bk(void) {
    puts("==== bk: the removed keys and the release flows ====");
    bk_ncrit0 = g_call_in_crit; bk_nest0 = g_nested; bk_unbal0 = g_crit_unbalanced;
    /* bk8. blink_ms (0.7.0) and settle_ms, rearm_ms, off_after_s (0.8.0) are unknown keys: refused on a PUT alone or beside a valid key, never read from the store, never listed */
    {   static const char *const key[4] = {"blink_ms", "settle_ms", "rearm_ms", "off_after_s"};
        for (int ki = 0; ki < 4; ki++) {
            const char *kn = key[ki]; char body[5][96];
            snprintf(body[0], sizeof body[0], "{\"%s\":1000}", kn);
            snprintf(body[1], sizeof body[1], "{\"%s\":200}", kn);
            snprintf(body[2], sizeof body[2], "{\"%s\":\"x\"}", kn);
            snprintf(body[3], sizeof body[3], "{\"rx_wd_s\":300,\"%s\":1000}", kn);        /* beside a valid key, valid key first */
            snprintf(body[4], sizeof body[4], "{\"%s\":1000,\"rx_wd_s\":300}", kn);        /* and last: the whole body is refused */
            for (int k = 0; k < 5; k++) {
                reset_case(); cfg_load(); g_auto_bearer = 1; cfg_t before = cfg_live; int c = bd_put(body[k]);
                int named = strstr(bd_req.response, kn) != NULL, why = strstr(bd_req.response, "not a setting") != NULL, bad = strstr(bd_req.response, "bad_request") != NULL;
                int same = !memcmp(&before, &cfg_live, sizeof before); int stored = ns_has("stored", kn); int kvset = kv_slot(kn, false) >= 0; int rxset = kv_slot("rx_wd_s", false) >= 0;
                int written = g_cfg_sets != 0 || g_cfg_commits != 0;
                int g = bd_get(); int in_get = strstr(bd_req.response, kn) != NULL;
                static char sb[4096]; int len; cJSON *j = st_obj(ST_SETTINGS, sb, sizeof sb, &len); const cJSON *so = j ? st_get(j, "settings") : NULL; int in_state = so && st_get(so, kn) != NULL; cJSON_Delete(j);
                printf("bk8 PUT %s: http=%d bad_request=%d names %s=%d \"not a setting\"=%d; cfg_live unchanged=%d; written=%d, rx_wd_s stored=%d; stored lists it=%d; GET http=%d lists it=%d; state object lists it=%d\n",
                       body[k], c, bad, kn, named, why, same, written, rxset, stored, g, in_get, in_state);
                CHECK(c == 400 && bad && named && why && same && !written && !rxset && !stored && !kvset && g == 200 && !in_get && !in_state);
            }
            /* a value an older firmware stored under that name: not read, not listed, and never erased (it stays in flash through cfg_load and a PUT) */
            reset_case(); kv_put_u32(kn, 1000); int er0 = g_erase_keys + g_erase_alls; cfg_load(); g_auto_bearer = 1; int g = bd_get(); int in_get = strstr(bd_req.response, kn) != NULL; int stored = ns_has("stored", kn);
            static char sb2[4096]; int len2; cJSON *j2 = st_obj(ST_SETTINGS, sb2, sizeof sb2, &len2); const cJSON *so2 = j2 ? st_get(j2, "settings") : NULL; int in_state = so2 && st_get(so2, kn) != NULL; cJSON_Delete(j2);
            uint32_t mask = cfg_live.stored; int put = bd_put("{\"rx_wd_s\":300}"); uint32_t kept = 0; int still = kv_u(kn, &kept) && kept == 1000; int erases = g_erase_keys + g_erase_alls - er0;
            printf("bk8 stored %s 1000 then cfg_load: GET http=%d lists it=%d; stored lists it=%d; state object lists it=%d; stored mask=0x%x; after a PUT of rx_wd_s (http=%d) still in NVS=%d, erase calls=%d\n", kn, g, in_get, stored, in_state, mask, put, still, erases);
            CHECK(g == 200 && !in_get && !stored && !in_state && mask == 0 && put == 200 && still && erases == 0);
        }
    }
    verdict("bk8", "blink_ms, settle_ms, rearm_ms and off_after_s are not settings: a PUT of each (1000, 200, \"x\", and beside a valid rx_wd_s in either order) is 400 bad_request naming the key with \"not a setting\", refuses the whole body and changes nothing (cfg_live, NVS); none is in GET /settings, stored or the state object; a value an older firmware stored under that name is not read, not listed and not erased (it is still in NVS after cfg_load and a PUT)");

    /* bk11. who releases a held press: the release names the API, or nothing */
    {   printf("bk11 light_release_by_of: API-RELEASE=%s ROLE-OFF=%s\n", light_release_by_of("API-RELEASE") ? light_release_by_of("API-RELEASE") : "NULL",
               light_release_by_of("ROLE-OFF") ? light_release_by_of("ROLE-OFF") : "NULL");
        CHECK(light_release_by_of("API-RELEASE") && !strcmp(light_release_by_of("API-RELEASE"), "api"));
        CHECK(light_release_by_of("ROLE-OFF") == NULL);
    }
    verdict("bk11", "light_release_by_of: API-RELEASE is \"api\", ROLE-OFF is NULL (the role switch clears the button anyway)");

    /* bk12. critical-section discipline over the flows that release or cancel a held press */
    {   int c0 = g_call_in_crit, n0 = g_nested, u0 = g_crit_unbalanced;
        bk_boot(); bk_press(); int held_a = bk_held(); int rc = bk_release(); int held_b = bk_held();                     /* the client's release */
        bk_boot(); bk_press(); g_auto_bearer = 1; int code = bd_put("{\"role\":\"mirror\"}");                              /* the role switch */
        bk_boot(); bk_press(); inject_hub_off(); run_button();                                                            /* the Zigbee controller's cancel of the press */
        printf("bk12 over a release, a role switch and a Zigbee controller cancel (held %d, release rc=%d, held after %d, role PUT http=%d): call-in-section +%d, nested +%d, unbalanced +%d; over the whole bk area: +%d / +%d / +%d; sections open now=%d; zigbee lock held=%d; total call-in-section so far=%d\n",
               held_a, rc, held_b, code, g_call_in_crit - c0, g_nested - n0, g_crit_unbalanced - u0, g_call_in_crit - bk_ncrit0, g_nested - bk_nest0, g_crit_unbalanced - bk_unbal0, g_crit, g_zb_locked, g_call_in_crit);
        CHECK(held_a == 1 && rc == 0 && !held_b && code == 200);
        CHECK(g_call_in_crit == c0 && g_nested == n0 && g_crit_unbalanced == u0 && g_crit == 0 && !g_zb_locked);
        CHECK(g_call_in_crit == bk_ncrit0 && g_nested == bk_nest0 && g_crit_unbalanced == bk_unbal0 && g_call_in_crit == 0);
    }
    verdict("bk12", "over the whole bk area (the client's release of a held press, the role switch and the Zigbee controller's cancel of a press): no log, allocation, delay, Zigbee or attribute call inside a critical section (the counter stays 0), no nesting, no unbalanced exit; the Zigbee lock is always taken before the mux and released");
}
