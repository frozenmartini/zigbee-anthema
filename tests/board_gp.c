/* gp1-gp3: the Zigbee controller's group (hub_tap on cluster 0x0004, the watchdog's write). */
static bool gp_tap(uint8_t cmd, const uint8_t *p, int n, int fc, int status, int mode, uint16_t src, int profile, uint16_t cluster) {
    uint8_t f[16] = {(uint8_t)fc, 7, cmd}; for (int i = 0; i < n; i++) f[3 + i] = p[i];
    ezb_apsde_data_ind_t ind = {.status = (uint8_t)status, .profile_id = (uint16_t)profile, .cluster_id = cluster, .asdu = f, .asdu_length = (uint16_t)(3 + n),
                                .src_address = {.addr_mode = mode, .u.short_addr = src}};
    g_in_stack_ctx = 1; bool r = hub_tap(&ind); g_in_stack_ctx = 0; return r;
}
static bool gp_cmd(uint8_t cmd, const uint8_t *p, int n) { return gp_tap(cmd, p, n, 0x01, 0, EZB_ADDR_MODE_SHORT, 0x0000, 0x0104, EZB_ZCL_CLUSTER_ID_GROUPS); }
static int gp_marks_zb(void) { int c = 0; for (int i = 0; i < g_nmarks && i < 512; i++) c += g_marks[i] == C6_ST_ZIGBEE; return c; }
static void gp_pass(void) { g_delay_budget = 1; if (!setjmp(loop_done)) watchdog_task(NULL); g_delay_budget = -1; }
static void gp_second_add(void) { static const uint8_t b[] = {0x0B, 0x00}; gp_cmd(0x00, b, 2); }

static void probe_gp(void) {
    puts("==== gp: the Zigbee controller's group ====");
    /* gp1. the commands */
    {   static const uint8_t a[] = {0x0A, 0x00}, b[] = {0x0B, 0x00};
        reset_case(); s_group = 0; s_group_dirty = false; int rets = 0;
        rets += gp_cmd(0x00, a, 2); int g1 = s_group, d1 = s_group_dirty, m1 = gp_marks_zb();
        s_group_dirty = false; s_group = 0; rets += gp_cmd(0x05, a, 2); int g2 = s_group, m2 = gp_marks_zb();
        rets += gp_cmd(0x03, b, 2); int g3 = s_group, m3 = gp_marks_zb();
        s_group_dirty = false; rets += gp_cmd(0x03, a, 2); int g4 = s_group, d4 = s_group_dirty, m4 = gp_marks_zb();
        rets += gp_cmd(0x00, a, 2); s_group_dirty = false; rets += gp_cmd(0x04, NULL, 0); int g5 = s_group, d5 = s_group_dirty, m5 = gp_marks_zb();
        zb_snapshot_t z; s_zb_up = true; bulb_zigbee_snapshot(&z); int unset = z.group;
        printf("gp1 add 0x000A: group 0x%04X dirty %d marks %d | If-Identifying: 0x%04X | remove another: 0x%04X (marks unchanged %d) | remove ours: %d dirty %d | remove all: %d dirty %d; returns true %d times; snapshot group after clear %d\n", g1, d1, m1, g2, g3, m3 == m2 + 0, g4, d4, g5, d5, rets, unset);
        CHECK(g1 == 0x000A && d1 && m1 == 1 && g2 == 0x000A && g3 == 0x000A && m3 == m2 && g4 == 0 && d4 && m4 == m3 + 1 && g5 == 0 && d5 && m5 == m4 + 2 && rets == 0 && g_ctx_viol == 0);
        reset_case(); s_group = 0x000A; char buf[2000]; state_json(buf, sizeof buf, C6_ST_ZIGBEE); cJSON *j = cJSON_Parse(buf); const cJSON *zb = j ? cJSON_GetObjectItemCaseSensitive(j, "zigbee") : NULL; const cJSON *gr = zb ? cJSON_GetObjectItemCaseSensitive(zb, "group") : NULL;
        int is_str = gr && cJSON_IsString(gr) && !strcmp(gr->valuestring, "0x000A"); cJSON_Delete(j);
        s_group = 0; state_json(buf, sizeof buf, C6_ST_ZIGBEE); j = cJSON_Parse(buf); zb = j ? cJSON_GetObjectItemCaseSensitive(j, "zigbee") : NULL; gr = zb ? cJSON_GetObjectItemCaseSensitive(zb, "group") : NULL; int is_null = gr && cJSON_IsNull(gr); cJSON_Delete(j);
        printf("gp1 reported group: \"0x000A\" as a string=%d, none as null=%d\n", is_str, is_null);
        CHECK(is_str && is_null);
    }
    verdict("gp1", "hub_tap on Groups: Add Group and Add If Identifying set the group; Remove of ours clears it; Remove of another leaves it; Remove All clears it; every call returns false; each change raises dirty and marks ZIGBEE; reported as \"0x000A\" or null");

    /* gp2. frames that must not change the group */
    {   static const uint8_t a[] = {0x0A, 0x00};
        const struct {const char *name; int fc, status, mode, profile; uint16_t src; int n;} cs[] = {
            {"source 0x1234", 0x01, 0, EZB_ADDR_MODE_SHORT, 0x0104, 0x1234, 2}, {"status not 0", 0x01, 1, EZB_ADDR_MODE_SHORT, 0x0104, 0, 2},
            {"manufacturer-specific", 0x05, 0, EZB_ADDR_MODE_SHORT, 0x0104, 0, 2}, {"not cluster-specific", 0x00, 0, EZB_ADDR_MODE_SHORT, 0x0104, 0, 2},
            {"server-to-client", 0x09, 0, EZB_ADDR_MODE_SHORT, 0x0104, 0, 2}, {"1-byte payload", 0x01, 0, EZB_ADDR_MODE_SHORT, 0x0104, 0, 1},
            {"profile not 0x0104", 0x01, 0, EZB_ADDR_MODE_SHORT, 0x0105, 0, 2}, {"IEEE source", 0x01, 0, EZB_ADDR_MODE_IEEE, 0x0104, 0, 2}};
        for (int k = 0; k < 8; k++) {
            reset_case(); s_group = 0; s_group_dirty = false; bool r = gp_tap(0x00, a, cs[k].n, cs[k].fc, cs[k].status, cs[k].mode, cs[k].src, cs[k].profile, EZB_ZCL_CLUSTER_ID_GROUPS);
            printf("gp2 %s: group 0x%04X dirty %d returned %d marks %d\n", cs[k].name, s_group, s_group_dirty, r, g_nmarks);
            CHECK(s_group == 0 && !s_group_dirty && !r && gp_marks_zb() == 0);
        }
    }
    verdict("gp2", "an Add Group frame from source 0x1234, with status not 0, manufacturer-specific, not cluster-specific, server-to-client, one payload byte, or another profile changes nothing");

    /* gp3. the watchdog writes the group, never the stack */
    {   static const uint8_t a[] = {0x0A, 0x00};
        reset_case(); s_group = 0; s_group_dirty = false; gp_cmd(0x00, a, 2);
        printf("gp3 after the add frame: nvs opens=%d sets=%d, context violations=%d, dirty=%d\n", g_fs_opens, g_fs_sets, g_ctx_viol, s_group_dirty);
        CHECK(g_fs_opens == 0 && g_fs_sets == 0 && g_ctx_viol == 0 && s_group_dirty && g_call_in_crit == 0);
        g_fs_commit_err = ESP_FAIL; gp_pass(); int d_fail = s_group_dirty, c_fail = g_fs_commits;
        g_fs_commit_err = 0; gp_pass(); int bi = fs_find("c6zb", "group");
        printf("gp3 a failing commit: dirty stays=%d (commits %d); next pass: stored=%d value 0x%04X type %d commits=%d dirty=%d\n", d_fail, c_fail, bi >= 0, bi >= 0 ? g_fs[bi].u : 0, bi >= 0 ? g_fs[bi].type : 0, g_fs_commits, s_group_dirty);
        CHECK(d_fail && c_fail == 1 && bi >= 0 && g_fs[bi].type == 5 && g_fs[bi].u == 0x000A && g_fs_commits == 2 && !s_group_dirty);
        gp_pass(); CHECK(g_fs_commits == 2);
        reset_case(); s_group = 0; s_group_dirty = false; gp_cmd(0x00, a, 2); g_fs_commit_hook = gp_second_add; gp_pass();
        bi = fs_find("c6zb", "group");
        printf("gp3 a second add during the write: stored 0x%04X, group now 0x%04X, dirty=%d\n", bi >= 0 ? g_fs[bi].u : 0, s_group, s_group_dirty);
        CHECK(bi >= 0 && g_fs[bi].u == 0x000A && s_group == 0x000B && s_group_dirty);
        gp_pass(); bi = fs_find("c6zb", "group");
        CHECK(bi >= 0 && g_fs[bi].u == 0x000B && !s_group_dirty);
        reset_case(); s_group = 0; s_group_dirty = false; int none0 = s_group; group_load(); int none = s_group;
        fs_put("c6zb", "group", 5, 0x000A, NULL, 0); group_load();
        printf("gp3 boot load: absent -> 0x%04X (was 0x%04X); stored 0x000A -> 0x%04X, dirty=%d\n", none, none0, s_group, s_group_dirty);
        CHECK(none == 0 && s_group == 0x000A && !s_group_dirty);
    }
    verdict("gp3", "the group is stored by the watchdog pass only (c6zb/group, u16, one commit, dirty cleared after it); a failed commit retries; a change during the write stays dirty; boot restores it, absent is 0; hub_tap makes no NVS call");
}
