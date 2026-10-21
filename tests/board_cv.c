/* cv1-cv6: regressions for six fixed defects.
 * Injection dependencies: g_cv_nvs_open_hook (one-shot at nvs_open entry),
 * g_cv_fail_key (one-shot fs_set key error), g_cv_erase_error (nvs_erase_key error).
 * These represent an RTOS preemption, an immediate NVS set failure, and erase failure.
 */
static httpd_req_t cv_erase_req;
static int cv_erase_injected;
static void cv_erase_before_open(void)
{
    /* A saver holding the Zigbee lock serializes this HTTP handler. In that case let
     * the saver complete, then perform the pending erase from the probe instead. */
    if (g_zb_locked) return;
    cv_erase_injected = 1;
    req_init(&cv_erase_req); api_call(zigbee_erase_post, &cv_erase_req);
}
static int cv_event_bool(const char *frame, const char *obj, const char *key, int want)
{
    const char *d = strstr(frame, "data: ");
    cJSON *j = d ? cJSON_Parse(d + 6) : NULL;
    const cJSON *o = j ? cJSON_GetObjectItemCaseSensitive(j, obj) : NULL;
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    int ok = v && (cJSON_IsNumber(v) ? v->valueint == want : want ? cJSON_IsTrue(v) : cJSON_IsFalse(v));
    cJSON_Delete(j); return ok;
}
static void probe_cv(void)
{
    puts("==== cv: regressions ====");
    /* cv1. Save passes its erase guard, is preempted, and resumes after successful erase. */
    for (int group = 0; group < 2; group++) {
        reset_case(); cfg_live.role = CFG_ROLE_MIRROR;
        s_light_gen = 1; s_saved_gen = 0; s_light_changed_us = fake_now - LIGHT_SAVE_AFTER_US;
        s_group = 0x000A; s_group_dirty = true;
        fs_put("c6zb", "group", 5, 0x000A, NULL, 0);
        g_blob_n = 0; cv_erase_injected = 0; g_cv_nvs_open_hook = cv_erase_before_open;
        if (group) group_save_if_dirty(); else light_save_if_settled(fake_now);
        if (!cv_erase_injected) { req_init(&cv_erase_req); api_call(zigbee_erase_post, &cv_erase_req); }
        int erase_code = cv_erase_req.code;
        int refused = erase_code == 503;
        /* A concurrent save may refuse erase; when it finishes a retry must succeed. */
        if (refused) { req_init(&cv_erase_req); api_call(zigbee_erase_post, &cv_erase_req); }
        int recreated = group ? fs_find("c6zb", "group") >= 0 : g_blob_n > 0 && cv_erase_injected && !refused;
        printf("cv1 %s: initial erase=%d final erase=%d injected=%d recreated=%d light blob bytes=%zu group stored=%d\n", group ? "group" : "light", erase_code, cv_erase_req.code, cv_erase_injected, recreated, g_blob_n, fs_find("c6zb", "group") >= 0);
        CHECK(cv_erase_req.code == 200 && !recreated);
    }
    /* A failed save must also relinquish its erase exclusion. */
    for (int fault = 0; fault < 4; fault++) {
        reset_case(); s_light_gen = 1; s_saved_gen = 0;
        s_light_changed_us = fake_now - LIGHT_SAVE_AFTER_US; s_group_dirty = true;
        if (fault == 0) { save_error = ESP_FAIL; light_save_if_settled(fake_now); }
        else {
            if (fault == 1) g_fs_open_err = ESP_FAIL;
            if (fault == 2) g_fs_set_err = ESP_FAIL;
            if (fault == 3) g_fs_commit_err = ESP_FAIL;
            group_save_if_dirty();
        }
        save_error = g_fs_open_err = g_fs_set_err = g_fs_commit_err = 0;
        req_init(&cv_erase_req); api_call(zigbee_erase_post, &cv_erase_req);
        CHECK(cv_erase_req.code == 200);
    }
    verdict("cv1", "a watchdog save in flight cannot recreate light/group storage after Zigbee erase");

    /* cv2. IDF writes immediately; a failed pass write must restore the previous pair. */
    for (int present = 0; present < 4; present++) {
      reset_case();
      if (present & 1) fs_put("c6wifi", "ssid", 3, 0, "oldnet", sizeof "oldnet");
      if (present & 2) fs_put("c6wifi", "pass", 3, 0, "old-password", sizeof "old-password");
      g_cv_fail_key = "pass"; int err = c6_wifi_set("newnet", "new-password");
      char ssid[33], pass[64]; int loaded = wifi_load(ssid, sizeof ssid, pass, sizeof pass);
      const char *stored_ssid = fs_str("c6wifi", "ssid"), *stored_pass = fs_str("c6wifi", "pass");
      int intact = (present & 1 ? stored_ssid && !strcmp(stored_ssid, "oldnet") : !stored_ssid)
                   && (present & 2 ? stored_pass && !strcmp(stored_pass, "old-password") : !stored_pass);
      printf("cv2 failed pass write, keys present=%d: err=%d loaded=%d previous keys intact=%d\n", present, err, loaded, intact);
      CHECK(err != ESP_OK && intact && loaded == !!(present & 1));
    }
    verdict("cv2", "a failed Wi-Fi pair write leaves the previous bootable pair intact");

    /* cv3. An unsuccessful persistent release must not report success or open owner access. */
    for (int erase = 0; erase < 2; erase++) {
        reset_case(); fs_put("c6claim", "sha", 4, 0, s_digest, sizeof s_digest);
        int closes = g_closes;
        if (erase) g_cv_erase_error = ESP_FAIL; else g_fs_open_err = ESP_FAIL;
        httpd_req_t r; req_init(&r); api_call(api_release_post, &r);
        int claimed = c6_auth_claimed(), open = c6_auth_window_open(), closed = g_closes - closes;
        g_fs_open_err = g_cv_erase_error = 0; c6_auth_start();
        printf("cv3 %s failure: release http=%d claimed before boot=%d open=%d stream closes=%d claimed after boot=%d\n", erase ? "erase" : "open", r.code, claimed, open, closed, c6_auth_claimed());
        CHECK(r.code == 500 && claimed && !open && closed == 0 && c6_auth_claimed());
    }
    verdict("cv3", "a claim that could not be erased stays claimed and /release reports storage failure");

    /* cv4. client Off updates the Zigbee controller shadow and must publish that changed object. */
    { reset_case(); cfg_live.role = CFG_ROLE_MIRROR; inject_hub_on(); fake_now += ECHO_WINDOW_US;
      events_start(); httpd_req_t r; ev_open(&r); size_t at = g_chunks_len;
      int code = lt_post("{\"on\":0}"); events_step(fake_now);
      printf("cv4 client Off: http=%d actual controller.on=%u SSE controller Off=%d tail=%s", code, s_hub.val[L_ON], cv_event_bool(g_chunks + at,"controller","on",0), g_chunks + at);
      CHECK(code == 200 && s_hub.val[L_ON] == 0 && cv_event_bool(g_chunks + at,"controller","on",0));
    }
    verdict("cv4", "client commands publish changed Zigbee controller shadow values through SSE");

    /* cv5. Saving clears light.unsaved and must publish that change without another command. */
    { reset_case(); cfg_live.role = CFG_ROLE_MIRROR; inject_hub_on(); events_start(); httpd_req_t r; ev_open(&r);
      size_t at = g_chunks_len; fake_now += LIGHT_SAVE_AFTER_US; light_save_if_settled(fake_now); events_step(fake_now);
      printf("cv5 settled save: gen=%u saved=%u SSE unsaved false=%d tail=%s\n", s_light_gen,s_saved_gen,cv_event_bool(g_chunks + at,"light","unsaved",0),g_chunks + at);
      CHECK(s_light_gen == s_saved_gen && cv_event_bool(g_chunks + at,"light","unsaved",0));
    }
    { reset_case(); cfg_live.role = CFG_ROLE_MIRROR; inject_hub_on(); events_start(); httpd_req_t r; ev_open(&r);
      fake_now += LIGHT_SAVE_AFTER_US; g_commit_hook = inject_level_change;
      size_t pending = g_chunks_len; light_save_if_settled(fake_now); events_step(fake_now);
      CHECK(s_light_gen != s_saved_gen && cv_event_bool(g_chunks + pending,"light","unsaved",1));
      fake_now += LIGHT_SAVE_AFTER_US; size_t at = g_chunks_len;
      light_save_if_settled(fake_now); events_step(fake_now);
      CHECK(s_light_gen == s_saved_gen && cv_event_bool(g_chunks + at,"light","unsaved",0));
    }
    verdict("cv5", "a settled successful save publishes light.unsaved=false through SSE");

    /* cv6. The stack processes a Zigbee controller On while client Off waits for its lock. */
    { reset_case(); cfg_live.role = CFG_ROLE_MIRROR; s_light.val[L_ON] = s_hub.val[L_ON] = 0; model_on = 0;
      g_acquire_hook = inject_hub_on; int code = lt_post("{\"on\":0}");
      printf("cv6 Zigbee controller On while client Off waits: http=%d body=%s attribute sets=%d actual On=%d\n", code,lt_r.response,g_set_calls,model_on);
      CHECK(code == 200 && !strcmp(lt_r.response,"{\"ok\":false,\"ignored\":\"controller_command_3s\"}") && g_set_calls == 0 && model_on == 1);
    }
    verdict("cv6", "the Zigbee controller echo guard is checked after acquiring the Zigbee lock");
}
