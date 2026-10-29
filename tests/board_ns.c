/* ns1-ns2: settings.c's two new keys, name and ext_pan (PUT/GET /api/v1/settings through the real handlers; cfg_load reads the Kconfig default). */
#define NS_PAN_DEFAULT "0A1B2C3D4E5F6071"                      /* a made-up Kconfig default */
static char ns_buf[256];
static void ns_boot(const char *kconfig_pan) {                 /* a claimed board that has just run cfg_load with nothing stored */
    reset_case(); snprintf(g_kconfig_ext_pan, sizeof g_kconfig_ext_pan, "%s", kconfig_pan); cfg_load(); g_auto_bearer = 1;
}
static const char *ns_str(const char *k) {                     /* a string member of the last settings answer, or NULL */
    cJSON *j = cJSON_Parse(bd_req.response); const cJSON *c = j ? cJSON_GetObjectItemCaseSensitive(j, k) : NULL; const char *r = NULL;
    if (c && cJSON_IsString(c)) { snprintf(ns_buf, sizeof ns_buf, "%s", c->valuestring); r = ns_buf; }
    cJSON_Delete(j); return r;
}
static int ns_has(const char *arr, const char *k) {            /* the named member of an array member ("stored", "pending") of the last answer */
    cJSON *j = cJSON_Parse(bd_req.response); const cJSON *a = j ? cJSON_GetObjectItemCaseSensitive(j, arr) : NULL; int r = 0;
    for (const cJSON *c = a && cJSON_IsArray(a) ? a->child : NULL; c; c = c->next) r |= cJSON_IsString(c) && !strcmp(c->valuestring, k);
    cJSON_Delete(j); return r;
}
static int ns_names(const char *field) { char p[48]; snprintf(p, sizeof p, "\"message\":\"%s:", field); return strstr(bd_req.response, p) != NULL; }
static int ns_put_name(const char *name) { char b[160]; snprintf(b, sizeof b, "{\"name\":\"%s\"}", name); return bd_put(b); }

static void probe_ns(void) {
    puts("==== ns: name and ext_pan ====");
    /* ns1. name */
    {   char n40[41], n41[42]; memset(n40, 'n', 40); n40[40] = 0; memset(n41, 'n', 41); n41[41] = 0;
        ns_boot(""); int g = bd_get(); const char *nm = ns_str("name");
        printf("ns1 GET with none stored: http=%d name='%s', 'name' in stored=%d, cfg_live.stored=0x%x\n", g, nm ? nm : "(none)", ns_has("stored", "name"), cfg_live.stored);
        CHECK(g == 200 && nm && !strcmp(nm, "Zigbee Anthema 112233") && !ns_has("stored", "name") && !(cfg_live.stored & CFG_K_NAME) && !kv_s("name"));
        const char *ok[] = {"a", n40, "zq9 light", "Kitchen \xC3\xA9"};
        for (int i = 0; i < 4; i++) {
            ns_boot(""); int c = ns_put_name(ok[i]); const char *back = ns_str("name"); const char *st = kv_s("name");
            printf("ns1 PUT name of %zu bytes: http=%d answer name ok=%d, stored in NVS ok=%d, in stored=%d\n", strlen(ok[i]), c, back && !strcmp(back, ok[i]), st && !strcmp(st, ok[i]), ns_has("stored", "name"));
            CHECK(c == 200 && back && !strcmp(back, ok[i]) && st && !strcmp(st, ok[i]) && ns_has("stored", "name") && !strcmp(cfg_live.name, ok[i]));
        }
        const struct {const char *body; const char *what;} bad[] = {
            {"{\"name\":\"\"}", "empty"}, {NULL, "41 bytes"}, {"{\"name\":\"a\\u0007b\"}", "BEL"}, {"{\"name\":\"a\\nb\"}", "newline"}, {"{\"name\":\"a\\u007fb\"}", "DEL"},
            {"{\"name\":5}", "a number"}, {"{\"name\":null}", "null"},
            {"{\"name\":\"\",\"rx_wd_s\":300}", "empty + a valid key"}, {"{\"rx_wd_s\":300,\"name\":\"x\\u0001\"}", "valid key + control"}};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            char b41[80]; ns_boot(""); int s0 = g_cfg_sets, c0 = g_cfg_commits, m0 = g_nmarks; const char *body = bad[i].body;
            if (!body) { snprintf(b41, sizeof b41, "{\"name\":\"%s\"}", n41); body = b41; }
            int c = bd_put(body);
            printf("ns1 PUT name %s: http=%d names name=%d, NVS sets +%d, commits +%d, name stored=%d, rx_wd_s=%u, marks +%d\n", bad[i].what, c, ns_names("name"), g_cfg_sets - s0, g_cfg_commits - c0, kv_s("name") != NULL, cfg_live.rx_wd_s, g_nmarks - m0);
            CHECK(c == 400 && ns_names("name") && g_cfg_sets == s0 && g_cfg_commits == c0 && !kv_s("name") && cfg_live.rx_wd_s == 0 && !(cfg_live.stored & (CFG_K_NAME | CFG_K_RX_WD_S)) && g_nmarks == m0);
        }
        ns_boot(""); c6_discovery_start(); int n0 = g_mdns_name_sets, m0 = g_nmarks; int c = ns_put_name("zq9 light");
        int ms = 0, mb = 0; for (int i = m0; i < g_nmarks && i < 512; i++) { ms |= (g_marks[i] & C6_ST_SETTINGS) != 0; mb |= (g_marks[i] & C6_ST_BOARD) != 0; }
        printf("ns1 a change: http=%d, c6_discovery_set_name calls +%d, instance now '%s', SETTINGS marked=%d BOARD marked=%d\n", c, g_mdns_name_sets - n0, g_mdns_inst, ms, mb);
        CHECK(c == 200 && g_mdns_name_sets == n0 + 1 && !strcmp(g_mdns_inst, "zq9 light") && ms && mb);
        n0 = g_mdns_name_sets; m0 = g_nmarks; c = ns_put_name("zq9 light"); mb = 0; for (int i = m0; i < g_nmarks && i < 512; i++) mb |= (g_marks[i] & C6_ST_BOARD) != 0;
        printf("ns1 the same name again: http=%d, set_name calls +%d, BOARD marked=%d\n", c, g_mdns_name_sets - n0, mb);
        CHECK(c == 200 && g_mdns_name_sets == n0 && !mb);
        n0 = g_mdns_name_sets; c = bd_put("{\"rx_wd_s\":300}");
        printf("ns1 a PUT without name: http=%d, set_name calls +%d\n", c, g_mdns_name_sets - n0); CHECK(c == 200 && g_mdns_name_sets == n0);
    }
    verdict("ns1", "name: 1-40 bytes without a control character is accepted, stored and answered; empty, 41 bytes, a control character, a number, null are 400 naming name, all-or-nothing with a valid second key; none stored shows 'Zigbee Anthema <id>' from the host name and not in stored; a change calls c6_discovery_set_name once and marks SETTINGS and BOARD");

    /* ns2. ext_pan */
    {   ns_boot(NS_PAN_DEFAULT); int g = bd_get(); const char *p = ns_str("ext_pan");
        printf("ns2 GET with none stored: http=%d ext_pan='%s' (the Kconfig default), in stored=%d, pending=%d\n", g, p ? p : "(none)", ns_has("stored", "ext_pan"), ns_has("pending", "ext_pan"));
        CHECK(g == 200 && p && !strcmp(p, NS_PAN_DEFAULT) && !ns_has("stored", "ext_pan") && !ns_has("pending", "ext_pan") && !kv_s("ext_pan"));
        ns_boot(""); bd_get(); p = ns_str("ext_pan"); CHECK(p && !p[0] && !ns_has("stored", "ext_pan"));
        const char *ok[] = {"", "0123456789ABCDEF", "0123456789abcdef", "aBcDeF0123456789"};
        for (int i = 0; i < 4; i++) {
            ns_boot(NS_PAN_DEFAULT); char b[80]; snprintf(b, sizeof b, "{\"ext_pan\":\"%s\"}", ok[i]); int c = bd_put(b); const char *back = ns_str("ext_pan"); const char *st = kv_s("ext_pan");
            printf("ns2 PUT ext_pan '%s': http=%d answer as given=%d, stored as given=%d, in stored=%d, pending=%d\n", ok[i], c, back && !strcmp(back, ok[i]), st && !strcmp(st, ok[i]), ns_has("stored", "ext_pan"), ns_has("pending", "ext_pan"));
            CHECK(c == 200 && back && !strcmp(back, ok[i]) && st && !strcmp(st, ok[i]) && ns_has("stored", "ext_pan") && ns_has("pending", "ext_pan") && !strcmp(cfg_live.ext_pan, ok[i]));
        }
        ns_boot(NS_PAN_DEFAULT); int c = bd_put("{\"ext_pan\":\"0a1b2c3d4e5f6071\"}");
        printf("ns2 PUT the default in the other case: http=%d pending=%d (the stack reads either case alike)\n", c, ns_has("pending", "ext_pan"));
        CHECK(c == 200 && !ns_has("pending", "ext_pan") && ns_has("stored", "ext_pan"));
        const char *bad[] = {"0123456789ABCDE", "0123456789ABCDEF0", "0123456789ABCDEG", "0x0123456789ABCD", " 123456789ABCDEF", "0123456789ABCDEF ", "01234 6789ABCDEF", "0123456789ABCDE\\n", "z"};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            ns_boot(NS_PAN_DEFAULT); char b[120]; snprintf(b, sizeof b, "{\"ext_pan\":\"%s\",\"rx_wd_s\":300}", bad[i]); int s0 = g_cfg_sets, c0 = g_cfg_commits; c = bd_put(b);
            printf("ns2 PUT ext_pan '%s' + rx_wd_s: http=%d names ext_pan=%d, NVS sets +%d, commits +%d, ext_pan stored=%d, rx_wd_s=%u\n", bad[i], c, ns_names("ext_pan"), g_cfg_sets - s0, g_cfg_commits - c0, kv_s("ext_pan") != NULL, cfg_live.rx_wd_s);
            CHECK(c == 400 && ns_names("ext_pan") && g_cfg_sets == s0 && g_cfg_commits == c0 && !kv_s("ext_pan") && cfg_live.rx_wd_s == 0 && !(cfg_live.stored & (CFG_K_EXT_PAN | CFG_K_RX_WD_S)));
        }
        ns_boot(NS_PAN_DEFAULT); c = bd_put("{\"ext_pan\":5}"); printf("ns2 PUT ext_pan as a number: http=%d\n", c); CHECK(c == 400 && ns_names("ext_pan") && !kv_s("ext_pan"));
    }
    verdict("ns2", "ext_pan: empty or 16 hex digits in either case is accepted as given and stored; 15, 17, a non-hex digit, 0x+14 hex, a space, a number are 400 naming ext_pan with nothing of the request written; GET with nothing stored shows the Kconfig default and not in stored; the PUT answer lists ext_pan under pending (next reboot or erase), and does not when only the case differs from the one the stack started with");
}
