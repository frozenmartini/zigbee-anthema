/* dc1: c6_discovery.c (mDNS) across the claim, clear, release and the settings PUTs that change the role and the name. */
static void probe_dc(void) {
    puts("==== dc: mDNS ====");
    reset_case(); g_auto_bearer = 0; s_claimed = false; memset(s_digest, 0, 32); s_window_until_us = fake_now + 600000000LL;
    int crit0 = g_call_in_crit, nest0 = g_nested; char host[16]; snprintf(host, sizeof host, "%s", s_host);
    c6_discovery_start(); c6_discovery_start();
    printf("dc1 start: adds=%d service=%s%s port=%d other services=%d instance='%s'\n", g_mdns_adds, g_mdns_service, g_mdns_proto, g_mdns_port, g_mdns_other_services, g_mdns_inst);
    CHECK(g_mdns_adds == 1 && !strcmp(g_mdns_service, "_zigbee-anthema") && !strcmp(g_mdns_proto, "_tcp") && g_mdns_port == 80 && g_mdns_other_services == 0 && !strcmp(g_mdns_inst, "Zigbee Anthema 112233"));
    const char *keys[] = {"api", "fw", "id", "mac", "hw", "state", "role"};
    int have = 1; for (int i = 0; i < 7; i++) have &= mdns_txt(keys[i]) != NULL;
    printf("dc1 TXT: api=%s fw=%s id=%s mac=%s hw=%s state=%s role=%s (%d items)\n", mdns_txt("api"), mdns_txt("fw"), mdns_txt("id"), mdns_txt("mac"), mdns_txt("hw"), mdns_txt("state"), mdns_txt("role"), g_mdns_ntxt);
    CHECK(have && g_mdns_ntxt == 7 && !strcmp(mdns_txt("api"), "1") && !strcmp(mdns_txt("fw"), "0.5.0") && !strcmp(mdns_txt("id"), "112233") && !strcmp(mdns_txt("mac"), "aa:bb:cc:11:22:33")
          && !strcmp(mdns_txt("hw"), "nanoc6") && !strcmp(mdns_txt("state"), "unclaimed") && !strcmp(mdns_txt("role"), "none"));
    httpd_req_t r; req_init(&r); api_call(api_claim_post, &r); const char *after_claim = mdns_txt("state");
    printf("dc1 after a claim: http=%d state=%s\n", r.code, after_claim);
    CHECK(r.code == 200 && !strcmp(after_claim, "claimed"));
    c6_auth_clear(); const char *after_clear = mdns_txt("state");
    printf("dc1 after a clear: state=%s\n", after_clear);
    CHECK(!strcmp(after_clear, "unclaimed"));
    au_state(true, false); g_auto_bearer = 1; httpd_req_t r2; req_init(&r2); api_call(api_release_post, &r2);
    printf("dc1 after a release: http=%d state=%s\n", r2.code, mdns_txt("state"));
    CHECK(r2.code == 200 && !strcmp(mdns_txt("state"), "unclaimed"));
    au_state(true, false); g_auto_bearer = 1;
    const char *roles[] = {"mirror", "shortcut", "none"}; int ok = 1; char body[48];
    for (int k = 0; k < 3; k++) {
        snprintf(body, sizeof body, "{\"role\":\"%s\"}", roles[k]); int st = bd_put(body); const char *t = mdns_txt("role");
        printf("dc1 PUT role=%s: http=%d TXT role=%s\n", roles[k], st, t); ok &= st == 200 && t && !strcmp(t, roles[k]);
    }
    CHECK(ok);
    int names0 = g_mdns_name_sets; int st = bd_put("{\"name\":\"zq9 light\"}");
    printf("dc1 PUT name: http=%d instance-name calls +%d, instance now '%s'; hostname still %s\n", st, g_mdns_name_sets - names0, g_mdns_inst, s_host);
    CHECK(st == 200 && g_mdns_name_sets == names0 + 1 && !strcmp(g_mdns_inst, "zq9 light") && !strcmp(s_host, host) && g_mdns_adds == 1);
    printf("dc1 mDNS calls inside a spinlock section: %d, nested sections: %d\n", g_call_in_crit - crit0, g_nested - nest0);
    CHECK(g_call_in_crit == crit0 && g_nested == nest0 && g_ctx_viol == 0);
    verdict("dc1", "c6_discovery_start adds one _zigbee-anthema._tcp on 80 (no _http) named after the board with api, fw, id, mac, hw, state, role; state follows claim, clear and release, role follows the PUT, a name PUT renames the instance; no mDNS call inside a section");
}
