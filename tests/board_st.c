/* st1-st5: state.c (state_json and the getters in bulb.c) and the state_mark sites. */
static void st_role(int role) { portENTER_CRITICAL(&s_light_mux); cfg_live.role = (uint8_t)role; portEXIT_CRITICAL(&s_light_mux); }
static cJSON *st_obj(uint32_t keys, char *buf, size_t n, int *len) { *len = state_json(buf, n, keys); return *len >= 0 ? cJSON_Parse(buf) : NULL; }
static int st_has_key(const cJSON *o, const char *k) { return o && cJSON_GetObjectItemCaseSensitive(o, k) != NULL; }
static const cJSON *st_get(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
static int st_is_null(const cJSON *o, const char *k) { const cJSON *i = st_get(o, k); return i && cJSON_IsNull(i); }
static void st_max_state(void) {                       /* every source at its maximum */
    reset_case(); s_zb_up = true; g_joined = true;
    memset(cfg_live.name, 'n', 40); cfg_live.name[40] = 0;
    snprintf(cfg_live.webhook, sizeof cfg_live.webhook, "http://192.0.2.200:8123/api/webhook/%s", "zq9-abcdefghijklmnopqrstuvwxyz-0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789");
    snprintf(cfg_live.ext_pan, sizeof cfg_live.ext_pan, "A1B2C3D4E5F60718"); cfg_live.role = CFG_ROLE_MIRROR; cfg_live.rx_wd_s = 3600; cfg_live.stored = (1u << CFG_K_COUNT) - 1;
    snprintf(g_info.ip, sizeof g_info.ip, "255.255.255.255"); g_info.uptime_s = 4294967295u; g_info.rssi = -100;
    s_light.seen = 0xff; for (int i = 0; i < L_COUNT; i++) { s_light.val[i] = 65535; s_hub.val[i] = 65535; } s_hub.seen = 0xff;
    s_light_by = "restore"; s_light_at_us = 9000000000000LL; s_hub_cmd_us = 9000000000000LL; s_hub_seq = 4294967295u; s_presses = 4294967295u; s_release_last = "zigbee"; s_button_at_us = 9000000000000LL; s_held = true;
    s_group = 0xFFFF; g_zb_rx_last = 5000000; s_reporting_ok = true; s_light_gen = 1; s_saved_gen = 0; fake_now = 9000000000000LL;
    s_update = (c6_update_t){.state = "rebooting", .version = "0123456789012345678901234567890", .percent = 100, .error = ""};   /* 0.6.0: the update object at its longest (31-character version) */
}

static void probe_st(void) {
    puts("==== st: the state object ====");
    /* st1. the whole object at its maximum */
    {   st_max_state(); static char buf[4096]; int len; cJSON *j = st_obj(ST_ALL, buf, sizeof buf, &len);
        const char *order[] = {"board", "settings", "zigbee", "light", "controller", "button", "update"}; int in_order = j != NULL; int idx = 0;
        for (const cJSON *c = j ? j->child : NULL; c; c = c->next, idx++) in_order &= idx < 7 && !strcmp(c->string, order[idx]);
        in_order &= idx == 7;
        printf("st1 state_json(ST_ALL) at the maximum: %d bytes (limit 1,536), parses=%d, seven keys in order=%d\n", len, j != NULL, in_order);
        CHECK(j && in_order && len > 0 && len < 1536);
        cJSON_Delete(j);
        const uint32_t bits[] = {ST_BOARD, ST_SETTINGS, ST_ZIGBEE, ST_LIGHT, ST_HUB, ST_BUTTON, ST_UPDATE}; int single = 1, two = 1;
        for (int i = 0; i < 7; i++) { cJSON *o = st_obj(bits[i], buf, sizeof buf, &len); int n = 0; for (const cJSON *c = o ? o->child : NULL; c; c = c->next) n++; single &= o && n == 1 && st_has_key(o, order[i]); cJSON_Delete(o); }
        for (int i = 0; i < 7; i++) { cJSON *o = st_obj(bits[i] | bits[(i + 3) % 7], buf, sizeof buf, &len); int n = 0; for (const cJSON *c = o ? o->child : NULL; c; c = c->next) n++; two &= o && n == 2; cJSON_Delete(o); }
        printf("st1 each single bit emits only its key=%d; two bits two keys=%d\n", single, two);
        CHECK(single && two);
        int full = state_json(buf, sizeof buf, ST_ALL); int guard_ok = 1; int short_rc = 0, exact_rc = 0;
        static char g[4096 + 64]; memset(g, 0xA5, sizeof g);
        short_rc = state_json(g + 32, (size_t)full, ST_ALL);                      /* one byte short: the NUL has no room */
        for (int i = 0; i < 32; i++) guard_ok &= (uint8_t)g[i] == 0xA5;
        for (int i = 32 + full; i < 64 + full; i++) guard_ok &= (uint8_t)g[i] == 0xA5;
        memset(g, 0xA5, sizeof g); exact_rc = state_json(g + 32, (size_t)full + 16, ST_ALL);        /* cJSON wants a few bytes of slack: 16 more is enough */
        for (int i = 0; i < 32; i++) guard_ok &= (uint8_t)g[i] == 0xA5;
        for (int i = 32 + full + 16; i < 64 + full + 16; i++) guard_ok &= (uint8_t)g[i] == 0xA5;
        int tiny = state_json(g + 32, 8, ST_ALL);
        printf("st1 a buffer one byte short: returns %d and writes an empty string=%d; 16 bytes of slack: returns %d; 8 bytes: %d; guard bytes untouched=%d\n", short_rc, g[32] == 0 || short_rc < 0, exact_rc, tiny, guard_ok);
        CHECK(short_rc < 0 && exact_rc == full && tiny < 0 && guard_ok);
    }
    verdict("st1", "state_json(ST_ALL) with every source at its maximum parses with the real cJSON, has the seven keys in the protocol's order and stays under 1,536 B; a single bit emits one key, two bits two; a buffer one byte short returns an error and never overruns");

    /* st2. board, settings and zigbee blocks */
    {   reset_case(); s_zb_up = true; g_joined = true; g_info.uptime_s = 42; snprintf(g_info.ip, sizeof g_info.ip, "192.0.2.50"); g_info.rssi = -55;
        snprintf(cfg_live.name, sizeof cfg_live.name, "Zigbee Anthema 112233"); snprintf(cfg_live.ext_pan, sizeof cfg_live.ext_pan, "A1B2C3D4E5F60718");
        s_group = 0x000A; fake_now += 100000000LL; g_zb_rx_last = fake_now - 12000000LL; s_reporting_ok = true;
        static char buf[4096]; int len; cJSON *j = st_obj(ST_BOARD | ST_SETTINGS | ST_ZIGBEE, buf, sizeof buf, &len);
        const cJSON *b = st_get(j, "board"), *se = st_get(j, "settings"), *z = st_get(j, "zigbee");
        const char *bk[] = {"id", "name", "fw", "api", "hw", "uptime_s", "ip", "rssi", "ota_state", "partition"}; int bok = b != NULL; for (int i = 0; i < 10; i++) bok &= st_has_key(b, bk[i]);
        const char *sk[] = {"role", "webhook", "rx_wd_s", "ext_pan", "name", "hue"}; int sok = se != NULL; for (int i = 0; i < 6; i++) sok &= st_has_key(se, sk[i]);
        const char *zk[] = {"joined", "ieee", "short", "channel", "pan", "ext_pan", "reporting", "last_rx_s", "profile", "hue", "group"}; int zok = z != NULL; for (int i = 0; i < 11; i++) zok &= st_has_key(z, zk[i]);
        const cJSON *grp = st_get(z, "group"), *rx = st_get(z, "last_rx_s"), *ep = st_get(z, "ext_pan");
        int noleak = !strstr(buf, "\"ssid\"") && !strstr(buf, "\"pass\"") && !strstr(buf, "\"password\"") && !strstr(buf, "test_zq9") && !strstr(buf, "zq9") && !strstr(buf, G_TOKEN);
        printf("st2 joined: board fields=%d settings fields=%d zigbee fields=%d; group=%s last_rx_s=%g ext_pan=%s; no ssid/pass/webhook id=%d\n", bok, sok, zok, grp && cJSON_IsString(grp) ? grp->valuestring : "?", rx && cJSON_IsNumber(rx) ? rx->valuedouble : -1.0, ep && cJSON_IsString(ep) ? ep->valuestring : "?", noleak);
        CHECK(bok && sok && zok && grp && cJSON_IsString(grp) && !strcmp(grp->valuestring, "0x000A") && rx && rx->valuedouble == 12 && ep && !strcmp(ep->valuestring, "0102030405060708") && noleak);
        const cJSON *wh = st_get(se, "webhook"), *nm = st_get(se, "name"), *sep = st_get(se, "ext_pan");
        printf("st2 settings: webhook masked as '%s'; name '%s'; ext_pan '%s'\n", wh && cJSON_IsString(wh) ? wh->valuestring : "?", nm && cJSON_IsString(nm) ? nm->valuestring : "?", sep && cJSON_IsString(sep) ? sep->valuestring : "?");
        CHECK(wh && cJSON_IsString(wh) && strncmp(wh->valuestring, "http://127.0.0.1:8123", 21) == 0 && !strstr(wh->valuestring, "zq9") && nm && !strcmp(nm->valuestring, "Zigbee Anthema 112233") && sep && !strcmp(sep->valuestring, "A1B2C3D4E5F60718"));
        cJSON_Delete(j);
        s_group = 0; g_joined = false; g_zb_rx_last = 0; lock_ok = false;
        j = st_obj(ST_ZIGBEE, buf, sizeof buf, &len); z = st_get(j, "zigbee");
        const cJSON *joined = st_get(z, "joined"), *g2 = st_get(z, "group"), *rx0 = st_get(z, "last_rx_s"), *ep2 = st_get(z, "ext_pan");
        printf("st2 lock busy after a joined read: the last copy is served (joined=%d, ext_pan=%s) while group=%s last_rx_s=%s are current\n", joined && cJSON_IsTrue(joined), ep2 && cJSON_IsString(ep2) ? ep2->valuestring : "?", g2 && cJSON_IsNull(g2) ? "null" : "?", rx0 && cJSON_IsNull(rx0) ? "null" : "?");
        CHECK(joined && cJSON_IsTrue(joined) && ep2 && !strcmp(ep2->valuestring, "0102030405060708") && g2 && cJSON_IsNull(g2) && rx0 && cJSON_IsNull(rx0));
        cJSON_Delete(j); lock_ok = true;
        j = st_obj(ST_ZIGBEE, buf, sizeof buf, &len); z = st_get(j, "zigbee"); joined = st_get(z, "joined"); ep2 = st_get(z, "ext_pan");
        printf("st2 lock free, not joined: joined=%d short/channel/pan null=%d/%d/%d, ext_pan='%s'\n", joined && cJSON_IsTrue(joined), st_is_null(z, "short"), st_is_null(z, "channel"), st_is_null(z, "pan"), ep2 ? ep2->valuestring : "?");
        CHECK(joined && cJSON_IsFalse(joined) && st_is_null(z, "short") && st_is_null(z, "channel") && st_is_null(z, "pan") && ep2 && ep2->valuestring[0] == 0);
        cJSON_Delete(j);
    }
    verdict("st2", "the board, settings and zigbee blocks from scripted getters: every field of section 2.4; webhook masked; name and ext_pan present; group null or \"0x000A\"; ext_pan empty when not joined; last_rx_s null for 0 and 12 for 12 s; no ssid or password key; a lock timeout serves the last copy");

    /* st3. the light block */
    {   reset_case(); s_light.seen = 0; memset(s_light.val, 0, sizeof s_light.val); s_light_by = NULL; s_light_at_us = 0; static char buf[2048]; int len;
        cJSON *j = st_obj(ST_LIGHT, buf, sizeof buf, &len); const cJSON *l = st_get(j, "light");
        const char *ak[] = {"on", "level", "mode", "ct", "x", "y", "hue", "sat"}; int allnull = 1; for (int i = 0; i < 8; i++) allnull &= st_is_null(l, ak[i]);
        printf("st3 nothing seen: attributes null=%d, by null=%d, at_s null=%d, unsaved=%d\n", allnull, st_is_null(l, "by"), st_is_null(l, "at_s"), cJSON_IsTrue(st_get(l, "unsaved")));
        CHECK(allnull && st_is_null(l, "by") && st_is_null(l, "at_s") && cJSON_IsFalse(st_get(l, "unsaved")));
        cJSON_Delete(j);
        const char *want_by[4] = {"zigbee", "api", "self", "restore"}; const char *how[4] = {"Zigbee controller frame", "/light", "ROLE-OFF", "light_restore"}; double last_at = -1; int ok = 1;
        for (int k = 0; k < 4; k++) {
            fake_now += 5000000; int64_t now = fake_now; char msg[120]; int want[L_COUNT]; for (int i = 0; i < L_COUNT; i++) want[i] = -1; want[L_LEVEL] = 10 + k;
            if (k == 0) { portENTER_CRITICAL(&s_light_mux); s_light.val[L_ON] = 1; s_light.seen |= 1u << L_ON; light_mark_changed_locked(now, "zigbee"); portEXIT_CRITICAL(&s_light_mux); }
            else if (k == 1) light_apply(want, "API-SET", msg, sizeof msg, false, NULL);
            else if (k == 2) light_apply(want, "ROLE-OFF", msg, sizeof msg, false, NULL);
            else { light_state_t sv = {.seen = 1u << L_ON | 1u << L_LEVEL}; sv.val[L_ON] = 1; sv.val[L_LEVEL] = 99; memcpy(g_restore_blob, &sv, sizeof sv); g_restore_n = sizeof sv; restore_in_signal_handler(); }
            j = st_obj(ST_LIGHT, buf, sizeof buf, &len); l = st_get(j, "light"); const cJSON *by = st_get(l, "by"), *at = st_get(l, "at_s");
            int good = by && cJSON_IsString(by) && !strcmp(by->valuestring, want_by[k]) && at && cJSON_IsNumber(at) && at->valuedouble > last_at;
            printf("st3 after %s: by=%s at_s=%g unsaved=%d\n", how[k], by && cJSON_IsString(by) ? by->valuestring : "?", at && cJSON_IsNumber(at) ? at->valuedouble : -1.0, cJSON_IsTrue(st_get(l, "unsaved")));
            ok &= good; if (at && cJSON_IsNumber(at)) last_at = at->valuedouble; cJSON_Delete(j);
        }
        CHECK(ok);
        { char msg[100]; int want[L_COUNT]; for (int i = 0; i < L_COUNT; i++) want[i] = -1; want[L_LEVEL] = 77; fake_now += 1000000; light_apply(want, "API-SET", msg, sizeof msg, false, NULL); }
        j = st_obj(ST_LIGHT, buf, sizeof buf, &len); l = st_get(j, "light"); int unsaved_before = cJSON_IsTrue(st_get(l, "unsaved")); cJSON_Delete(j);
        fake_now += 5000000; light_save_if_settled(fake_now);
        j = st_obj(ST_LIGHT, buf, sizeof buf, &len); l = st_get(j, "light"); int unsaved_after = cJSON_IsTrue(st_get(l, "unsaved")); cJSON_Delete(j);
        printf("st3 unsaved: true while s_light_gen != s_saved_gen (%d), false after the save (%d)\n", unsaved_before, unsaved_after);
        CHECK(unsaved_before && !unsaved_after);
    }
    verdict("st3", "the light block: unseen attributes null; by is zigbee, api, self, self, self, restore as driven; at_s moves with each change; unsaved is true until the save");

    /* st4. the Zigbee controller and button blocks */
    {   reset_case(); st_role(CFG_ROLE_SHORTCUT); s_zb_up = true; static char buf[2048]; int len; static const uint8_t ON[] = {1, 1, 1}, OFF[] = {1, 1, 0}, LV[] = {1, 1, 0x04, 100, 0, 0}, ODD[] = {1, 1, 0x77};
        cJSON *j = st_obj(ST_HUB | ST_BUTTON | ST_UPDATE, buf, sizeof buf, &len); const cJSON *hb = st_get(j, "controller"), *bt = st_get(j, "button"), *up = st_get(j, "update");
        printf("st4 nothing yet: controller.seq=%g settled=%d button.press=%g held=%d release_by=%s\n", st_get(hb, "seq")->valuedouble, cJSON_IsTrue(st_get(hb, "settled")), st_get(bt, "press")->valuedouble, cJSON_IsTrue(st_get(bt, "held")), st_get(bt, "release_by")->valuestring);
        CHECK(st_get(hb, "seq")->valuedouble == 0 && cJSON_IsTrue(st_get(hb, "settled")) && st_get(bt, "press")->valuedouble == 0 && !strcmp(st_get(bt, "release_by")->valuestring, "none"));
        const cJSON *u1 = up; int upd = cJSON_IsString(st_get(u1, "state")) && !strcmp(st_get(u1, "state")->valuestring, "idle") && st_is_null(u1, "version") && st_is_null(u1, "percent") && st_is_null(u1, "error"); cJSON_Delete(j);
        CHECK(upd);
        ctl(6, ON, sizeof ON); ctl(8, LV, sizeof LV); ctl(6, ODD, sizeof ODD);                    /* two decoded commands and an unknown one */
        hub_send(6, OFF, sizeof OFF, 1, EZB_ADDR_MODE_SHORT, 0x0000);                           /* APS status 1: ignored */
        hub_send(6, OFF, sizeof OFF, 0, EZB_ADDR_MODE_SHORT, 0x1234);                           /* not from the Zigbee controller: ignored */
        bulb_light_snap_t sn; bulb_light_snapshot(&sn); uint32_t seq3 = sn.hub_seq;
        s_light.seen |= 1u << L_ON; s_light.val[L_ON] = 0;                                      /* a group Off while the light is off is consumed */
        ezb_apsde_data_ind_t gd = {.status = 0, .profile_id = 0x104, .cluster_id = 6, .asdu = (uint8_t *)OFF, .asdu_length = 3, .src_address = {.addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = 0}, .dst_address = {.addr_mode = EZB_ADDR_MODE_GROUP}};
        bool dropped = hub_tap(&gd); bulb_light_snapshot(&sn);
        printf("st4 Zigbee controller frames: 2 decoded + 1 unknown command + 2 ignored frames: seq=%u; then a dropped group Off (decoded, consumed=%d): seq=%u\n", seq3, dropped, sn.hub_seq);
        CHECK(seq3 == 2 && dropped && sn.hub_seq == 3);
        const char *whos[2] = {"api", "zigbee"}; int seen_ok = 1;
        for (int k = 0; k < 2; k++) {
            reset_case(); st_role(CFG_ROLE_SHORTCUT); s_zb_up = true; s_presses = (uint32_t)k; s_release_last = k ? whos[k - 1] : NULL;
            s_press_pending = true; run_button();                                                  /* a press pending: delivered, held */
            bulb_button_snap_t bs; bulb_button_snapshot(&bs); int held1 = bs.held; uint32_t press1 = bs.press;
            char msg[100];
            if (k == 0) bulb_button_release(msg, sizeof msg);
            else ctl(6, OFF, sizeof OFF);
            bulb_button_snapshot(&bs); char buf2[600]; state_json(buf2, sizeof buf2, ST_BUTTON); cJSON *bj = cJSON_Parse(buf2); const cJSON *bb = st_get(bj, "button");
            printf("st4 press %d, release by %s: held after the press=%d press=%u; after the release held=%d release_by=%s json press=%g\n", k + 1, whos[k], held1, press1, bs.held, bs.release_by ? bs.release_by : "none", st_get(bb, "press")->valuedouble);
            seen_ok &= held1 == 1 && press1 == (uint32_t)k + 1 && !bs.held && bs.release_by && !strcmp(bs.release_by, whos[k]) && st_get(bb, "press")->valuedouble == k + 1 && !strcmp(st_get(bb, "release_by")->valuestring, whos[k]);
            cJSON_Delete(bj);
        }
        CHECK(seen_ok);
    }
    verdict("st4", "the Zigbee controller and button blocks: controller.seq counts decoded commands only (an unknown command or an ignored frame adds none); settled true; button.press counts, held follows s_held, release_by is the last of api and zigbee, and none before any; update is the constant object");

    /* st5. state_mark and its sites */
    {   reset_case(); events_start(); uint32_t d0; long a0, f0; int dl0, sem0, nl0, nt0;
        d0 = s_dirty; (void)d0; a0 = g_allocs; f0 = g_frees; dl0 = g_delays; sem0 = g_sem_calls; nl0 = g_nlogs; nt0 = notify_count(s_events_task); uint32_t seq0 = s_seq;
        state_mark(ST_LIGHT); state_mark(ST_HUB);
        printf("st5 direct: dirty=0x%x (ST_LIGHT|ST_HUB=0x%x), seq +%u, task notified +%d, logs +%d, allocations +%ld/%ld, delays +%d, semaphore calls +%d\n", s_dirty, ST_LIGHT | ST_HUB, s_seq - seq0, notify_count(s_events_task) - nt0, g_nlogs - nl0, g_allocs - a0, g_frees - f0, g_delays - dl0, g_sem_calls - sem0);
        CHECK(s_dirty == (ST_LIGHT | ST_HUB) && s_seq == seq0 + 2 && notify_count(s_events_task) == nt0 + 2 && g_nlogs == nl0 && g_allocs == a0 && g_frees == f0 && g_delays == dl0 && g_sem_calls == sem0);
        int n0 = g_nested; portENTER_CRITICAL(&s_light_mux); state_mark(ST_BUTTON); portEXIT_CRITICAL(&s_light_mux);
        printf("st5 inside s_light_mux: nested sections flagged +%d, call-in-section +%d\n", g_nested - n0, 0);
        CHECK(g_nested == n0 && (s_dirty & ST_BUTTON));
        int nr = 0; int site_ok = 1;
        #define ST_SITE(NAME, WANT, ...) do { s_dirty = 0; uint32_t sq = s_seq; int nm = g_nmarks, nt = notify_count(s_events_task); long al = g_allocs; int nl = g_nlogs; int dly = g_delays; __VA_ARGS__; \
            int marks = g_nmarks - nm; uint32_t got = s_dirty; int ok = got == (WANT) && marks >= 1 && s_seq - sq == (uint32_t)marks && notify_count(s_events_task) - nt == marks; (void)al; (void)nl; (void)dly; \
            printf("st5 site %-26s dirty=0x%02x want 0x%02x, marks=%d, seq +%u, notified +%d\n", NAME, got, (unsigned)(WANT), marks, s_seq - sq, notify_count(s_events_task) - nt); site_ok &= ok; nr++; } while (0)
        { int64_t now = fake_now + 1000; ST_SITE("light change", ST_LIGHT, { portENTER_CRITICAL(&s_light_mux); light_mark_changed_locked(now, "zigbee"); portEXIT_CRITICAL(&s_light_mux); }); }
        ST_SITE("Zigbee controller command, mirror", ST_HUB, { st_role(CFG_ROLE_MIRROR); static const uint8_t f[] = {1, 1, 1}; ctl(6, f, 3); });
        ST_SITE("Zigbee controller command, shortcut", ST_HUB | ST_BUTTON, { st_role(CFG_ROLE_SHORTCUT); static const uint8_t f[] = {1, 1, 0}; ctl(6, f, 3); });
        ST_SITE("button transition", ST_BUTTON, { st_role(CFG_ROLE_SHORTCUT); s_press_pending = true; run_button(); });
        ST_SITE("settings publish", ST_SETTINGS, { g_auto_bearer = 1; bd_put("{\"rx_wd_s\":60}"); });
        ST_SITE("join signal", ST_ZIGBEE, { s_steer_timer = NULL; static int ok0 = 0; ezb_app_signal_t sg = {EZB_BDB_SIGNAL_STEERING, &ok0}; on_signal(&sg); });
        ST_SITE("rejoin signal", ST_ZIGBEE, { g_factory_new = false; static int ok1 = 0; ezb_app_signal_t sg = {EZB_BDB_SIGNAL_DEVICE_REBOOT, &ok1}; on_signal(&sg); });
        ST_SITE("leave signal", ST_ZIGBEE, { ezb_zdo_signal_leave_params_t p = {.leave_type = EZB_ZDO_LEAVE_TYPE_RESET}; ezb_app_signal_t sg = {EZB_ZDO_SIGNAL_LEAVE, &p}; on_signal(&sg); });
        ST_SITE("group change", ST_ZIGBEE, { static const uint8_t a[] = {0x0A, 0x00}; s_group = 0; gp_cmd(0x00, a, 2); });
        ST_SITE("GOT_IP", ST_BOARD, { s_have_creds = true; wf_got_ip(); });
        #undef ST_SITE
        printf("st5 sites checked: %d, all mark exactly their keys, one seq step and one notify per mark: %d\n", nr, site_ok);
        CHECK(nr == 10 - 0 && site_ok);
    }
    verdict("st5", "state_mark ORs its keys, steps the counter and wakes the events task once per call, with no log, no allocation, no delay or semaphore, also inside s_light_mux; each site marks exactly its key (LIGHT; Zigbee controller, +BUTTON in shortcut; BUTTON; SETTINGS; ZIGBEE x3 and the group; BOARD)");
}
