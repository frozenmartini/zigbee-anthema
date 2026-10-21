/* zr1-zr8: bulb.c's Zigbee retry, the pair / leave / erase endpoints and the PAN filter. The steer timer is the real steer_timer_cb on the stub timers;
 * a failed steer is reported by the STEERING signal (status 0x03), as the stack would. */
static void zr_boot(void) {
    reset_case(); s_zb_up = true; s_zb_started = true; g_auto_bearer = 1;
    const esp_timer_create_args_t t = {.callback = steer_timer_cb, .name = "zb_steer"}; esp_timer_create(&t, &s_steer_timer);
}
static void zr_sig(int type, int st) { static int status; status = st; ezb_app_signal_t s = {type, &status}; on_signal(&s); }
static void zr_leave(int type) { ezb_zdo_signal_leave_params_t p = {.leave_type = type}; ezb_app_signal_t s = {EZB_ZDO_SIGNAL_LEAVE, &p}; on_signal(&s); }
static int zr_seen;
static void zr_pump(void) { if (g_commissions > zr_seen) { zr_seen = g_commissions; zr_sig(EZB_BDB_SIGNAL_STEERING, 3); } }
static void zr_run(int64_t seconds) {            /* run `seconds` of fake time, answering each steer with a failed STEERING signal */
    int64_t target = fake_now + seconds * 1000000;
    for (;;) {
        zr_pump(); int64_t d = timer_in_us("zb_steer");
        if (d < 0 || fake_now + d > target) break;
        fake_advance(d);
    }
    zr_pump(); fake_advance(target - fake_now); zr_pump();
}
static int zr_steer_times_ok(int from, int64_t t0, int64_t horizon_s, int *count) {     /* steer i at t0 + the §2.3 schedule: every 10 s through 900 s, then 60 s */
    int64_t t = 0; int n = 0, ok = 1;
    while (t <= horizon_s) {
        if (from + n >= g_commissions || g_commission_at[from + n] - t0 != t * 1000000) { ok = 0; printf("  steer %d expected at %lld s, at %lld s\n", n, (long long)t, from + n < g_commissions ? (long long)((g_commission_at[from + n] - t0) / 1000000) : -1LL); break; }
        n++; t += t < 900 ? 10 : 60;
    }
    *count = n; return ok && from + n == g_commissions;
}
static void zr_store_demo(void) {                 /* 8 namespaces, 40 entries, one name in 3 entries */
    const struct {const char *ns; int n;} sp[] = {{"nvs.net80211", 8}, {"c6cfg", 6}, {"c6claim", 1}, {"c6wifi", 2}, {"c6bulb", 4}, {"c6zb", 3}, {"zb_storage", 12}, {"phy", 4}};
    char k[16]; uint8_t b[4] = {1, 2, 3, 4};
    for (size_t i = 0; i < sizeof sp / sizeof sp[0]; i++) for (int j = 0; j < sp[i].n; j++) { snprintf(k, sizeof k, "k%d", j); fs_put(sp[i].ns, k, 4, 0, b, 4); }
}

static void probe_zr(void) {
    puts("==== zr: Zigbee retry, pair and leave, erase, PAN ====");
    /* zr1. the retry schedule */
    {   zr_boot(); g_factory_new = true; g_joined = false; g_commission_ret = 0; zr_seen = 0; int64_t t0 = fake_now;
        zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0); zr_run(1199);
        int n1; int ok1 = zr_steer_times_ok(0, t0, 1199, &n1);
        printf("zr1 factory-new boot, 20 min of failed steers: %d steers (every 10 s through 900 s, then every 60 s), schedule ok=%d, next in %lld s\n", g_commissions, ok1, (long long)(timer_in_us("zb_steer") / 1000000));
        CHECK(ok1 && n1 == 95 && g_commissions == 95);
        zr_run(7200 - 1199); int before = g_commissions; int64_t pair_t = fake_now;
        httpd_req_t r; req_init(&r); api_call(zigbee_pair_post, &r); zr_pump();
        printf("zr1 POST /zigbee/pair at 2 h: http=%d %s; steers now=%d (+%d)\n", r.code, r.response, g_commissions, g_commissions - before);
        CHECK(r.code == 200 && g_commissions == before + 1 && g_commission_at[before] == pair_t);
        zr_run(1199); int n2; int ok2 = zr_steer_times_ok(before, pair_t, 1199, &n2);
        printf("zr1 after the pair: %d steers in 20 min, schedule ok=%d\n", n2, ok2);
        CHECK(ok2 && n2 == 95);
        zr_run(3600); int slow = g_commissions; zr_run(600);
        printf("zr1 a late hour: slow steers 60 s apart (%d in 600 s)\n", g_commissions - slow);
        CHECK(g_commissions - slow == 10);
        s_group = 0x000A; s_group_dirty = false;                          /* a group is held when the Zigbee controller removes the board */
        int pre = g_commissions; zr_leave(EZB_ZDO_LEAVE_TYPE_RESET); int64_t lt = fake_now; int64_t due = timer_in_us("zb_steer");
        printf("zr1 a leave (reset): next steer in %lld s; group cleared=%d, marked for storing=%d\n", (long long)(due / 1000000), s_group == 0, (int)s_group_dirty);
        CHECK(due == 10000000 && s_group == 0 && s_group_dirty);
        zr_run(30); int64_t first = g_commission_at[pre] - lt, second = g_commission_at[pre + 1] - lt;
        printf("zr1 after the leave: steers at +%lld s and +%lld s (the fast phase again)\n", (long long)(first / 1000000), (long long)(second / 1000000));
        CHECK(g_commissions == pre + 3 && first == 10000000 && second == 20000000);
        s_group = 0x000A; int64_t d0 = timer_in_us("zb_steer"); zr_leave(1); int64_t d1 = timer_in_us("zb_steer");
        printf("zr1 a rejoin-type leave leaves the timer and the group alone: %lld -> %lld us, group 0x%04X\n", (long long)d0, (long long)d1, (unsigned)s_group);
        CHECK(d0 == d1 && s_group == 0x000A);
        zr_boot(); g_factory_new = true; zr_seen = 0; zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0); zr_pump(); int steers = g_commissions;
        lock_ok = false; fake_advance(10000000); int busy = g_commissions; int64_t rearm = timer_in_us("zb_steer"); lock_ok = true;
        printf("zr1 lock busy at an attempt: steers %d -> %d, re-armed in %lld s\n", steers, busy, (long long)(rearm / 1000000));
        CHECK(busy == steers && rearm == 10000000);
        zr_boot(); g_factory_new = false; g_joined = true; size_t m = g_loglen; zr_sig(EZB_BDB_SIGNAL_DEVICE_REBOOT, 0); fake_advance(3600000000LL); int marks = 0; for (int i = 0; i < g_nmarks; i++) marks += g_marks[i] == C6_ST_ZIGBEE;
        printf("zr1 reboot with a network: steers=%d, 'back on the network' lines=%d, light_restore lines=%d, reporting 1-20 s lines=%d, s_reporting_ok=%d, ZIGBEE marks=%d, timer armed=%d\n", g_commissions, log_count(m, "back on the network"), log_count(m, "LIGHT-RESTORE"), log_count(m, "On/Off reporting 1-20 s"), s_reporting_ok, marks, timer_in_us("zb_steer") >= 0);
        CHECK(g_commissions == 0 && log_count(m, "back on the network") == 1 && log_count(m, "LIGHT-RESTORE") == 1 && log_count(m, "On/Off reporting 1-20 s") == 1 && s_reporting_ok && marks == 1 && timer_in_us("zb_steer") < 0);
        zr_boot(); g_factory_new = true; zr_seen = 0; zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0); zr_sig(EZB_BDB_SIGNAL_STEERING, 0);
        printf("zr1 a successful steering stops the timer: armed=%d\n", timer_in_us("zb_steer") >= 0);
        CHECK(timer_in_us("zb_steer") < 0);
    }
    verdict("zr1", "a factory-new board steers at 0, every 10 s through 900 s, then every 60 s; a pair restarts the fast phase; a leave starts it again; a lock-busy attempt is the next one; a board with a network never steers and runs the rejoin path");

    /* zr2. /zigbee/pair and /zigbee/leave */
    {   static esp_err_t (*const post[2])(httpd_req_t *) = {zigbee_pair_post, zigbee_leave_post};
        for (int k = 0; k < 2; k++) {
            const char *nm = k ? "leave" : "pair";
            zr_boot(); s_group = 0x000A; s_group_dirty = false; httpd_req_t r; req_init(&r); api_call(post[k], &r);
            printf("zr2 %s with the stack up: http=%d %s; commissions=%d, local resets=%d, group=0x%04X dirty=%d, marks=%d\n", nm, r.code, r.response, g_commissions, g_local_resets, s_group, s_group_dirty, g_nmarks);
            CHECK(r.code == 200 && !strcmp(r.response, "{\"ok\":true}") && (k ? (g_local_resets == 1 && s_group == 0 && s_group_dirty && g_commissions == 0 && g_nmarks == 1) : (g_commissions == 1 && g_local_resets == 0 && s_group == 0x000A)));
            for (int w = 0; w < 3; w++) {
                zr_boot(); s_group = 0x000A; s_group_dirty = false;
                if (w == 0) s_zb_up = false; else if (w == 1) lock_ok = false; else s_zb_erasing = true;
                httpd_req_t q; req_init(&q); api_call(post[k], &q); char code[32]; int st = au_code(&q, code, sizeof code);
                printf("zr2 %s, %s: %d %s; commissions=%d resets=%d group=0x%04X\n", nm, w == 0 ? "stack down" : w == 1 ? "lock busy" : "erase running", st, code, g_commissions, g_local_resets, s_group);
                CHECK(st == 503 && !strcmp(code, "zigbee_busy") && !strcmp(q.type, "application/json") && g_commissions == 0 && g_local_resets == 0 && s_group == 0x000A && !s_group_dirty);
            }
            for (int c = 0; c < 2; c++) {
                zr_boot(); au_state(c, !c); s_group = 0x000A; httpd_req_t q; req_init(&q); api_call(post[k], &q); char code[32]; int st = au_code(&q, code, sizeof code);
                printf("zr2 %s without the token, %s: %d %s; commissions=%d resets=%d group=0x%04X\n", nm, c ? "claimed" : "unclaimed", st, code, g_commissions, g_local_resets, s_group);
                CHECK(st == (c ? 401 : 403) && g_commissions == 0 && g_local_resets == 0 && s_group == 0x000A);
            }
        }
        zr_boot(); g_joined = true; g_factory_new = false; httpd_req_t j; req_init(&j); api_call(zigbee_pair_post, &j); char code[32]; int st = au_code(&j, code, sizeof code);
        printf("zr2 pair on a board with a network: %d %s; commissions=%d\n", st, code, g_commissions);
        CHECK(st == 409 && !strcmp(code, "already_joined") && g_commissions == 0);
    }
    verdict("zr2", "POST /zigbee/pair steers once and POST /zigbee/leave resets, clears the group (dirty) and answers 200; stack down, lock busy and an erase under way give 503 zigbee_busy as JSON with nothing done; both refuse a request without the token");

    /* zr3. /zigbee/erase */
    {   zr_boot(); zr_store_demo(); int total = fs_count(NULL), cfg0 = fs_count("c6cfg"), claim0 = fs_count("c6claim"), wifi0 = fs_count("c6wifi"); s_group = 0x000A;
        httpd_req_t r; req_init(&r); api_call(zigbee_erase_post, &r);
        int erased_other = 1, kept_hit = 0; const char *want[] = {"nvs.net80211", "c6bulb", "c6zb", "zb_storage", "phy"};
        for (int i = 0; i < 5; i++) { int c = 0; for (int j = 0; j < g_nerased; j++) c += !strcmp(g_erased_ns[j], want[i]); erased_other &= c == 1 && fs_count(want[i]) == 0; }
        for (int j = 0; j < g_nerased; j++) kept_hit += !strcmp(g_erased_ns[j], "c6cfg") || !strcmp(g_erased_ns[j], "c6claim") || !strcmp(g_erased_ns[j], "c6wifi");
        printf("zr3 erase: %d entries in 8 namespaces; http=%d %s; nvs_erase_all calls=%d (5 expected, never c6cfg/c6claim/c6wifi: %d hits); kept keys %d/%d/%d (was %d/%d/%d); flash erased=%d; reboot task=%d\n",
               total, r.code, r.response, g_erase_alls, kept_hit, fs_count("c6cfg"), fs_count("c6claim"), fs_count("c6wifi"), cfg0, claim0, wifi0, g_flash_erases, tasks_named("reboot"));
        CHECK(total == 40 && r.code == 200 && g_erase_alls == 5 && g_nerased == 5 && erased_other && !kept_hit && fs_count("c6cfg") == cfg0 && fs_count("c6claim") == claim0 && fs_count("c6wifi") == wifi0 && g_flash_erases == 0 && tasks_named("reboot") == 1 && task_stack("reboot") == 6144 && s_group == 0);
        int t = 0; for (int i = 0; i < g_ntasks; i++) if (!strcmp(g_tasks[i].name, "reboot")) t = i;
        int64_t t0 = fake_now; g_tasks[t].fn(NULL);
        printf("zr3 the reboot task: waited %lld ms, restarts=%d, events_close=%d, answered 200 before it ran\n", (long long)((fake_now - t0) / 1000), g_restarts, g_closes);
        CHECK(fake_now - t0 == 500000 && g_restarts == 1 && g_closes == 1);
        zr_boot(); zr_store_demo(); g_erase_all_fail_at = 2; s_group = 0x000A; httpd_req_t f; req_init(&f); api_call(zigbee_erase_post, &f); char code[32]; int st = au_code(&f, code, sizeof code);
        printf("zr3 one nvs_erase_all failing: http=%d %s; reboot tasks=%d; erasing flag=%d; erase_alls=%d\n", st, code, tasks_named("reboot"), s_zb_erasing, g_erase_alls);
        CHECK(st != 200 && st >= 500 && tasks_named("reboot") == 0 && !s_zb_erasing && !strcmp(f.type, "application/json") && g_restarts == 0);
        zr_boot(); au_state(true, false); httpd_req_t u; req_init(&u); api_call(zigbee_erase_post, &u); st = au_code(&u, code, sizeof code);
        printf("zr3 without the token: %d %s; erase_alls=%d\n", st, code, g_erase_alls);
        CHECK(st == 401 && g_erase_alls == 0);
        zr_boot(); httpd_req_t o; req_init(&o); api_call(erase_post, &o);
        int et = -1; for (int i = 0; i < g_ntasks; i++) if (!strcmp(g_tasks[i].name, "erase")) et = i;
        printf("zr3 the old POST /zb/erase: http=%d %s; erase task made=%d\n", o.code, o.response, et >= 0);
        CHECK(o.code == 200 && et >= 0 && g_flash_erases == 0);
        g_tasks[et].fn(NULL);
        printf("zr3 the old erase task: nvs_flash_erase calls=%d, restarts=%d\n", g_flash_erases, g_restarts);
        CHECK(g_flash_erases == 1 && g_restarts == 1);
        {   /* inside the 500 ms before the restart a watchdog pass must not write c6bulb/light1 again */
            zr_boot(); zr_store_demo();
            s_light.seen |= 1u << L_ON; s_light.val[L_ON] = 1; s_light_gen = 5; s_saved_gen = 4; s_light_changed_us = fake_now - 2600000;
            httpd_req_t e; req_init(&e); api_call(zigbee_erase_post, &e);
            int sc0 = save_calls; g_blob_n = 0;
            fake_advance(400000);
            light_save_if_settled(fake_now);
            printf("zr3 erase then one watchdog pass 400 ms later: http=%d, erasing flag=%d, c6bulb opens=%d, light1 blob written=%zu bytes\n", e.code, (int)s_zb_erasing, save_calls - sc0, g_blob_n);
            CHECK(e.code == 200 && s_zb_erasing && save_calls == sc0 && g_blob_n == 0);
        }
    }
    verdict("zr3", "POST /zigbee/erase: nvs_erase_all on every namespace but c6cfg, c6claim, c6wifi (their keys stay), no flash erase, 200 before a 500 ms reboot task; the watchdog's light save writes nothing while it is under way; a partial erase does not answer 200 and does not reboot; the old /zb/erase still erases the flash");

    /* zr4. the PAN filter */
    {   const struct {const char *name; const char *stored; const char *kconfig; int calls; uint64_t pan;} cs[] = {
            {"stored empty beats the default", "", "1122334455667788", 0, 0},
            {"stored 16 hex", "A1B2C3D4E5F60718", "1122334455667788", 1, 0xA1B2C3D4E5F60718ull},
            {"absent, Kconfig default", NULL, "1122334455667788", 1, 0x1122334455667788ull},
            {"absent, Kconfig empty", NULL, "", 0, 0}};
        for (int k = 0; k < 4; k++) {
            reset_case(); g_kconfig_ext_pan[0] = 0; snprintf(g_kconfig_ext_pan, sizeof g_kconfig_ext_pan, "%s", cs[k].kconfig); if (cs[k].stored) kv_put_str("ext_pan", cs[k].stored);
            cfg_load(); g_set_use_pan_calls = 0; g_pan_u64 = 0; g_pri_mask = g_sec_mask = 0; zb_pan_path();
            printf("zr4 %s: ezb_set_use_extended_panid calls=%d, PAN 0x%016llX, channel sets 0x%08X / 0x%08X\n", cs[k].name, g_set_use_pan_calls, (unsigned long long)g_pan_u64, (unsigned)g_pri_mask, (unsigned)g_sec_mask);
            CHECK(g_set_use_pan_calls == cs[k].calls && g_pan_u64 == cs[k].pan && g_pri_mask == CONFIG_C6_ZB_CHANNEL_MASK && g_sec_mask == CONFIG_C6_ZB_CHANNEL_MASK);
        }
        printf("zr4 in zb_task the setting is read, the filter set and then esp_zigbee_start called, in that source order: %d\n", HARNESS_ZB_PAN_BEFORE_START);
        CHECK(HARNESS_ZB_PAN_BEFORE_START == 1);
    }
    verdict("zr4", "zb_task's PAN block: the ext_pan setting read, both channel sets equal CONFIG_C6_ZB_CHANNEL_MASK, ezb_set_use_extended_panid called with the 8 bytes or not at all (stored empty beats the default), all before esp_zigbee_start");

    /* zr5. 0.5.1: the real stack clears factory-new once a steer has run, so here
     * each failed steer clears g_factory_new as the stack does. */
    {   zr_boot(); g_factory_new = true; g_joined = false; zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0);
        for (int i = 0; i < 7; i++) { g_factory_new = false; zr_sig(EZB_BDB_SIGNAL_STEERING, 3); if (i < 6) fake_advance(10000000); }
        printf("zr5 factory-new cleared after each failed steer: %d steers in 60 s (7 expected), next in %lld s\n", g_commissions, (long long)(timer_in_us("zb_steer") / 1000000));
        CHECK(g_commissions == 7 && timer_in_us("zb_steer") == 10000000);
        zr_boot(); g_factory_new = false; g_joined = false; httpd_req_t r; req_init(&r); api_call(zigbee_pair_post, &r);
        printf("zr5 pair on a board not on a network and no longer factory new: http=%d %s; steers=%d\n", r.code, r.response, g_commissions);
        CHECK(r.code == 200 && !strcmp(r.response, "{\"ok\":true}") && g_commissions == 1);
    }
    verdict("zr5", "since 0.5.1: with factory-new cleared after every failed steer, as the stack does, the retry still steers every 10 s and POST /zigbee/pair still steers a board not on a network");

    /* zr6. since 0.5.1: a join sets up On/Off reporting. */
    {   zr_boot(); g_factory_new = true; g_joined = false; zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0); size_t m = g_loglen;
        g_factory_new = false; g_joined = true; zr_sig(EZB_BDB_SIGNAL_STEERING, 0);
        printf("zr6 a fresh join: reporting 1-20 s lines=%d, s_reporting_ok=%d, LIGHT-RESTORE lines=%d, timer armed=%d\n", log_count(m, "On/Off reporting 1-20 s"), s_reporting_ok, log_count(m, "LIGHT-RESTORE"), timer_in_us("zb_steer") >= 0);
        CHECK(log_count(m, "On/Off reporting 1-20 s") == 1 && s_reporting_ok && log_count(m, "LIGHT-RESTORE") == 0 && timer_in_us("zb_steer") < 0);
        zr_leave(EZB_ZDO_LEAVE_TYPE_RESET); bool after_leave = s_reporting_ok; g_joined = false; g_factory_new = true; fake_advance(10000000);
        g_factory_new = false; g_joined = true; zr_sig(EZB_BDB_SIGNAL_STEERING, 0);
        printf("zr6 a leave, then a join again: s_reporting_ok %d -> %d\n", after_leave, s_reporting_ok);
        CHECK(!after_leave && s_reporting_ok);
    }
    verdict("zr6", "since 0.5.1: a fresh join and a join after a leave set On/Off reporting 1-20 s, with no light restore");

    /* zr7. 0.5.2, pair's gates: 503 before the stack has answered, 409 rejoining
     * while a stored network is being rejoined (a failed REBOOT, a rejoin-type leave), 409 already_joined on it. */
    {   char code[32];
        zr_boot(); s_zb_started = false; g_factory_new = false; g_joined = false; httpd_req_t w; req_init(&w); api_call(zigbee_pair_post, &w); int st1 = au_code(&w, code, sizeof code);
        zr_sig(EZB_BDB_SIGNAL_DEVICE_REBOOT, 1); httpd_req_t w2; req_init(&w2); api_call(zigbee_pair_post, &w2); int st2 = au_code(&w2, code, sizeof code); bool rj = !strcmp(code, "rejoining");
        g_joined = true; zr_sig(EZB_BDB_SIGNAL_DEVICE_REBOOT, 0); httpd_req_t w3; req_init(&w3); api_call(zigbee_pair_post, &w3); int st3 = au_code(&w3, code, sizeof code);
        printf("zr7 a stored network: pair before the start %d, after a failed REBOOT %d (rejoining=%d), after its rejoin %d %s; steers=%d, rejoining flag=%d\n", st1, st2, rj, st3, code, g_commissions, s_zb_rejoining);
        CHECK(st1 == 503 && st2 == 409 && rj && st3 == 409 && !strcmp(code, "already_joined") && g_commissions == 0 && !s_zb_rejoining);
        zr_leave(1); g_joined = false; httpd_req_t w4; req_init(&w4); api_call(zigbee_pair_post, &w4); int st4 = au_code(&w4, code, sizeof code); bool rj4 = !strcmp(code, "rejoining");
        g_joined = true; zr_sig(EZB_BDB_SIGNAL_DEVICE_REBOOT, 0); bool cleared = !s_zb_rejoining;
        zr_leave(EZB_ZDO_LEAVE_TYPE_RESET); g_joined = false; g_factory_new = true; httpd_req_t w5; req_init(&w5); api_call(zigbee_pair_post, &w5);
        printf("zr7 a rejoin-type leave later in the boot: pair %d (rejoining=%d), cleared by the rejoin=%d; after a reset leave: pair %d; steers=%d\n", st4, rj4, cleared, w5.code, g_commissions);
        CHECK(st4 == 409 && rj4 && cleared && w5.code == 200 && g_commissions == 1);
        zr_boot(); s_zb_started = false; g_factory_new = true; zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 1); httpd_req_t w6; req_init(&w6); api_call(zigbee_pair_post, &w6); int st6 = au_code(&w6, code, sizeof code);
        printf("zr7 a factory-new start that failed: pair %d %s, rejoining=%d\n", st6, code, s_zb_rejoining);
        CHECK(st6 == 503 && !s_zb_rejoining);
        g_joined = true; g_factory_new = false; zr_sig(EZB_BDB_SIGNAL_STEERING, 0); httpd_req_t w7; req_init(&w7); api_call(zigbee_pair_post, &w7); int st7 = au_code(&w7, code, sizeof code);
        printf("zr7 joined by a steer before any successful start (/zb/steer): started=%d, pair %d %s\n", s_zb_started, st7, code);
        CHECK(s_zb_started && st7 == 409 && !strcmp(code, "already_joined"));
        zr_boot(); s_zb_started = false; g_factory_new = true; g_joined = false; zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0); zr_sig(EZB_BDB_SIGNAL_STEERING, 3);
        g_factory_new = false; httpd_req_t w8; req_init(&w8); api_call(zigbee_pair_post, &w8);
        printf("zr7 a factory-new start, a failed steer, then pair: http=%d, steers=%d\n", w8.code, g_commissions);
        CHECK(w8.code == 200 && g_commissions == 2);
    }
    verdict("zr7", "0.5.2 pair: 503 until the stack has answered its start; 409 rejoining after a failed REBOOT or a rejoin-type leave, cleared by the rejoin; 409 already_joined once joined, also when joined by a steer before any start; 200 after a reset leave or a failed steer");

    /* zr8. 0.5.2, the timer against a join and a rejoin. */
    {   zr_boot(); g_factory_new = true; g_joined = false; zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0); g_factory_new = false; zr_sig(EZB_BDB_SIGNAL_STEERING, 3);
        int before = g_commissions; g_joined = true; fake_advance(10000000);
        printf("zr8 an armed attempt that finds the board joined: steers %d -> %d, re-armed=%d\n", before, g_commissions, timer_in_us("zb_steer") >= 0);
        CHECK(g_commissions == before && timer_in_us("zb_steer") < 0);
        zr_boot(); g_factory_new = true; g_joined = false; zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0); g_factory_new = false; zr_sig(EZB_BDB_SIGNAL_STEERING, 3);
        before = g_commissions; lock_ok = false; g_joined = true; zr_sig(EZB_BDB_SIGNAL_STEERING, 0);
        esp_timer_start_once(s_steer_timer, 10000000); fake_advance(10000000); int64_t rearm = timer_in_us("zb_steer"); lock_ok = true;
        printf("zr8 a lock-busy attempt after the join: re-armed=%d, steers %d -> %d\n", rearm >= 0, before, g_commissions);
        CHECK(rearm < 0 && g_commissions == before);
        zr_boot(); g_factory_new = true; g_joined = false; zr_sig(EZB_BDB_SIGNAL_DEVICE_FIRST_START, 0); g_factory_new = false; zr_sig(EZB_BDB_SIGNAL_STEERING, 3);
        before = g_commissions; zr_leave(1); fake_advance(10000000);
        printf("zr8 an armed attempt during a rejoin: steers %d -> %d\n", before, g_commissions);
        CHECK(g_commissions == before);
    }
    verdict("zr8", "0.5.2 timer: an armed attempt does not steer a joined board or one rejoining its stored network, and a lock-busy attempt after a join does not re-arm");
}
