/* hu1-hu3: the colour shape of the endpoint (create_device and color_shape of bulb.c, through the descriptor stubs of board_stubs.c) and the hue setting's boot value in the state object. */
#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
#define HU_MANUF "sengled"
#define HU_MODEL "E11-N1EA"
#define HU_CODE 0x1160
#else
#define HU_MANUF "zq9-maker"
#define HU_MODEL "zq9-model"
#define HU_CODE 0x4A5B
#endif
static int hu_has(int id) { for (int i = 0; i < g_ncolor; i++) if (g_color_attrs[i] == id) return 1; return 0; }
static int hu_zcl_str_is(const char *zs, const char *text) { return (size_t)(unsigned char)zs[0] == strlen(text) && !memcmp(zs + 1, text, strlen(text)); }
static void hu_create(bool hue) { reset_case(); cfg_load(); cfg_live.hue = hue; create_device(); }

static void probe_hu(void) {
    puts("==== hu: the colour shape ====");
    /* hu1. hue on (the default): hue and saturation, the three colour-temperature attributes, capabilities 0x19 */
    {   size_t m; reset_case(); cfg_load(); cfg_live.hue = true; m = g_loglen; create_device();
        printf("hu1 hue on: colour attributes added=%d (first five %d %d %d %d %d), capabilities 0x%02X, 'ZB hue on' lines=%d, 'ZB hue off' lines=%d, ct values %u %u %u; endpoint %d registered %d, manufacturer code 0x%04X\n",
               g_ncolor, g_ncolor > 0 ? g_color_attrs[0] : -1, g_ncolor > 1 ? g_color_attrs[1] : -1, g_ncolor > 2 ? g_color_attrs[2] : -1, g_ncolor > 3 ? g_color_attrs[3] : -1, g_ncolor > 4 ? g_color_attrs[4] : -1,
               g_hu_cfg.color_cfg.color_capabilities, log_count(m, "ZB hue on"), log_count(m, "ZB hue off"), g_color_ct[0], g_color_ct[1], g_color_ct[2], g_hu_ep_id, g_hu_registers, g_hu_manuf_code);
        CHECK(g_ncolor >= 5 && g_color_attrs[0] == EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID && g_color_attrs[1] == EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID
              && g_color_attrs[2] == EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID && g_color_attrs[3] == EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID && g_color_attrs[4] == EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID
              && g_hu_cfg.color_cfg.color_capabilities == 0x19 && log_count(m, "ZB hue on") == 1 && log_count(m, "ZB hue off") == 0
              && g_color_nct == 3 && g_color_ct[0] == 370 && g_color_ct[1] == 154 && g_color_ct[2] == 500
              && g_hu_creates == 1 && g_hu_ep_id == CONFIG_C6_ZB_ENDPOINT && g_hu_registers == 1 && g_hu_endpoints == 1 && g_hu_manuf_code == HU_CODE && g_hu_handler_set && g_hu_wrong_cl == 0);
        printf("hu1 the profile's strings: manufacturer '%s' / model '%s' (ZCL strings, length byte first)\n", g_hu_manuf + 1, g_hu_model + 1);
        CHECK(hu_zcl_str_is(g_hu_manuf, HU_MANUF) && hu_zcl_str_is(g_hu_model, HU_MODEL));
#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
        printf("hu1 E11: zcl_version=%d colour mode=%d primaries=%d, endpoint device id 0x%04X, extra clusters added=%d\n", g_hu_cfg.basic_cfg.zcl_version, g_hu_cfg.color_cfg.color_mode, g_hu_cfg.color_cfg.number_of_primaries, g_hu_app_device_id, g_hu_e11_clusters);
        CHECK(g_hu_cfg.basic_cfg.zcl_version == 1 && g_hu_cfg.color_cfg.color_mode == 2 && g_hu_cfg.color_cfg.number_of_primaries == 4 && g_hu_app_device_id == 0x0102 && g_hu_e11_clusters == 4);
#endif
    }
    verdict("hu1", "create_device with hue on (the setting's default): CurrentHue and CurrentSaturation then the three colour-temperature attributes (370, 154, 500 mireds), capabilities 0x19, one 'ZB hue on' line, the profile's manufacturer and model strings and manufacturer code, one endpoint registered");

    /* hu2. hue off: the colour-temperature attributes only, capabilities 0x10 */
    {   size_t m; reset_case(); cfg_load(); cfg_live.hue = false; m = g_loglen; create_device();
        printf("hu2 hue off: colour attributes added=%d (hue %d, saturation %d, ct %d %d %d), capabilities 0x%02X, 'ZB hue off' lines=%d, 'ZB hue on' lines=%d, ct values %u %u %u\n",
               g_ncolor, hu_has(EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID), hu_has(EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID), hu_has(EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID),
               hu_has(EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID), hu_has(EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID), g_hu_cfg.color_cfg.color_capabilities, log_count(m, "ZB hue off"), log_count(m, "ZB hue on"), g_color_ct[0], g_color_ct[1], g_color_ct[2]);
        CHECK(!hu_has(EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID) && !hu_has(EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID)
              && g_ncolor >= 3 && g_color_attrs[0] == EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID && g_color_attrs[1] == EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID && g_color_attrs[2] == EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID
              && g_hu_cfg.color_cfg.color_capabilities == 0x10 && log_count(m, "ZB hue off") == 1 && log_count(m, "ZB hue on") == 0
              && g_color_nct == 3 && g_color_ct[0] == 370 && g_color_ct[1] == 154 && g_color_ct[2] == 500 && g_hu_registers == 1 && g_hu_wrong_cl == 0);
        hu_create(true); int on_caps = g_hu_cfg.color_cfg.color_capabilities; hu_create(false);
        CHECK(on_caps == 0x19 && g_hu_cfg.color_cfg.color_capabilities == 0x10);                  /* the attribute set follows the setting, case after case */
    }
    verdict("hu2", "create_device with hue off: neither CurrentHue nor CurrentSaturation is added, the three colour-temperature attributes still are, capabilities 0x10, one 'ZB hue off' line");

    /* hu3. zigbee.hue is the boot value; settings.hue is the live one */
    {   static char buf[4096]; int len;
        reset_case(); kv_put_u8("hue", 0); cfg_load(); int pc = bd_put("{\"hue\":true}");
        cJSON *j = st_obj(ST_ALL, buf, sizeof buf, &len); const cJSON *se = j ? st_get(j, "settings") : NULL, *zb = j ? st_get(j, "zigbee") : NULL;
        const cJSON *sh = se ? st_get(se, "hue") : NULL, *zh = zb ? st_get(zb, "hue") : NULL;
        int s_true = sh && cJSON_IsTrue(sh), z_false = zh && cJSON_IsFalse(zh);
        printf("hu3 stored 0, then PUT true (http %d): settings.hue true=%d, zigbee.hue false=%d (the boot value)\n", pc, s_true, z_false);
        CHECK(pc == 200 && s_true && z_false);
        cJSON_Delete(j);
        cfg_load();                                                                    /* a reboot: the stored 1 is read */
        j = st_obj(ST_ALL, buf, sizeof buf, &len); se = j ? st_get(j, "settings") : NULL; zb = j ? st_get(j, "zigbee") : NULL; sh = se ? st_get(se, "hue") : NULL; zh = zb ? st_get(zb, "hue") : NULL;
        int s2 = sh && cJSON_IsTrue(sh), z2 = zh && cJSON_IsTrue(zh);
        printf("hu3 after the reboot: settings.hue true=%d, zigbee.hue true=%d\n", s2, z2);
        CHECK(s2 && z2);
        cJSON_Delete(j);
        reset_case(); cfg_load(); bd_put("{\"hue\":false}");
        j = st_obj(ST_ALL, buf, sizeof buf, &len); se = j ? st_get(j, "settings") : NULL; zb = j ? st_get(j, "zigbee") : NULL; sh = se ? st_get(se, "hue") : NULL; zh = zb ? st_get(zb, "hue") : NULL;
        int s3 = sh && cJSON_IsFalse(sh), z3 = zh && cJSON_IsTrue(zh);
        printf("hu3 default on, then PUT false: settings.hue false=%d, zigbee.hue true=%d (still the boot value)\n", s3, z3);
        CHECK(s3 && z3);
        cJSON_Delete(j);
    }
    verdict("hu3", "the state object: settings.hue is the live setting and zigbee.hue the value the stack started with, so a PUT shows in the first and not the second until the next boot reads it");
}
