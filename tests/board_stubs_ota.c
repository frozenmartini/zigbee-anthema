/* ===== BOARD STUBS (firmware 0.6.0): the OTA API, an event group on fake time. Included by build_harness.py after board_stubs.c
 * (the real SHA-256 and the IDF-layout esp_app_desc_t are there) and before any firmware. Dependencies only: nothing here is firmware logic. ===== */

/* ---- the OTA API (esp_ota_ops.h): one slot, scripted failures, counters; the bytes are not kept, their SHA-256 is ---- */
typedef uint32_t esp_ota_handle_t;
typedef struct {const char *label; uint32_t size;} esp_partition_t;
static esp_partition_t g_ota_next = {"ota_1", 1966080};      /* the slot esp_ota_get_next_update_partition() answers with (the other slot, ota_1) */
static esp_app_desc_t g_ota_slot_desc;                          /* what esp_ota_get_partition_description() copies out: the probe sets project_name / version */
static int g_ota_no_slot, g_ota_begin_err, g_ota_write_err_at, g_ota_end_err, g_ota_boot_err, g_ota_desc_err;     /* the failures a probe asks for (0: none) */
static int g_ota_begins, g_ota_writes, g_ota_ends, g_ota_aborts, g_ota_open, g_ota_desc_calls, g_ota_boot_calls;
static size_t g_ota_begin_len, g_ota_bytes;                     /* the length esp_ota_begin was given; the bytes esp_ota_write took */
static const esp_partition_t *g_ota_boot_set;                   /* what esp_ota_set_boot_partition() was given (only when it succeeded) */
static mbedtls_sha256_context g_ota_sha; static unsigned char g_ota_digest[32];   /* the SHA-256 of the bytes written, final after esp_ota_end */
static int g_marked_valid, g_rolled_back; static int64_t g_valid_at, g_rolled_back_at;
static const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *from) { (void)from; CRIT_CALL(); return g_ota_no_slot ? NULL : &g_ota_next; }
static int esp_ota_begin(const esp_partition_t *p, size_t len, esp_ota_handle_t *h) {
    (void)p; CRIT_CALL(); g_ota_begins++; g_ota_begin_len = len;
    if (g_ota_begin_err) return g_ota_begin_err;
    *h = 1; g_ota_open = 1; g_ota_writes = 0; g_ota_bytes = 0;
    { int f = g_sha_inc_fail; g_sha_inc_fail = 0; mbedtls_sha256_init(&g_ota_sha); mbedtls_sha256_starts(&g_ota_sha, 0); g_sha_inc_fail = f; }   /* a failure asked for is the firmware's call's, not this stub's */
    return 0;
}
static int esp_ota_write(esp_ota_handle_t h, const void *data, size_t n) {
    (void)h; CRIT_CALL(); g_ota_writes++;
    if (g_ota_write_err_at && g_ota_writes == g_ota_write_err_at) return ESP_FAIL;
    g_ota_bytes += n; { int f = g_sha_inc_fail; g_sha_inc_fail = 0; mbedtls_sha256_update(&g_ota_sha, (const unsigned char *)data, n); g_sha_inc_fail = f; } return 0;
}
static int esp_ota_end(esp_ota_handle_t h) {
    (void)h; CRIT_CALL(); g_ota_ends++; g_ota_open = 0;
    { int f = g_sha_inc_fail; g_sha_inc_fail = 0; mbedtls_sha256_finish(&g_ota_sha, g_ota_digest); g_sha_inc_fail = f; }
    return g_ota_end_err;
}
static int esp_ota_abort(esp_ota_handle_t h) { (void)h; CRIT_CALL(); g_ota_aborts++; g_ota_open = 0; return 0; }
static int esp_ota_set_boot_partition(const esp_partition_t *p) { CRIT_CALL(); g_ota_boot_calls++; if (g_ota_boot_err) return g_ota_boot_err; g_ota_boot_set = p; return 0; }
static int esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *d) {
    (void)p; CRIT_CALL(); g_ota_desc_calls++;
    if (g_ota_desc_err) return g_ota_desc_err;
    memcpy(d, &g_ota_slot_desc, sizeof *d); return 0;
}
static int esp_ota_mark_app_valid_cancel_rollback(void) { g_marked_valid++; g_valid_at = fake_now; return 0; }
static int esp_ota_mark_app_invalid_rollback_and_reboot(void) { g_rolled_back++; g_rolled_back_at = fake_now; return 0; }   /* on a board this reboots and never returns; the task has nothing more to do either way */
static void ota_set_slot_desc(const char *project, const char *version) {
    memset(&g_ota_slot_desc, 0, sizeof g_ota_slot_desc);
    snprintf(g_ota_slot_desc.project_name, sizeof g_ota_slot_desc.project_name, "%s", project);
    snprintf(g_ota_slot_desc.version, sizeof g_ota_slot_desc.version, "%s", version);
}
static void ota_stub_reset(void) {
    g_ota_no_slot = g_ota_begin_err = g_ota_write_err_at = g_ota_end_err = g_ota_boot_err = g_ota_desc_err = 0;
    g_ota_begins = g_ota_writes = g_ota_ends = g_ota_aborts = g_ota_open = g_ota_desc_calls = g_ota_boot_calls = 0;
    g_ota_begin_len = g_ota_bytes = 0; g_ota_boot_set = NULL; memset(g_ota_digest, 0, sizeof g_ota_digest);
    g_marked_valid = g_rolled_back = 0; g_valid_at = g_rolled_back_at = 0; g_sha_inc_fail = 0;
    ota_set_slot_desc("c6_bulb", "0.6.0");
    g_ota_next.label = "ota_1"; g_ota_next.size = 1966080;
}

/* ---- FreeRTOS event groups on fake time ----
 * xEventGroupWaitBits returns at once if the bits are there; otherwise it moves fake_now forward, firing the scheduled bit-sets (eg_at) in time order
 * until the bits are satisfied or the wait runs out, then runs g_delay_hook like vTaskDelay does. pdMS_TO_TICKS(x) is x, so `ticks` are milliseconds. */
typedef uint32_t EventBits_t;
typedef EventBits_t *EventGroupHandle_t;
#define BIT0 0x1u
#define BIT1 0x2u
#define BIT2 0x4u
static EventBits_t g_eg_word; static int g_eg_creates, g_eg_waits; static int g_eg_create_fail;
static struct {int64_t at; EventBits_t set; bool fired;} g_eg_sched[8]; static int g_eg_nsched;
static EventGroupHandle_t xEventGroupCreate(void) { CRIT_CALL(); g_eg_creates++; if (g_eg_create_fail) return NULL; g_eg_word = 0; return &g_eg_word; }
static EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b) { CRIT_CALL(); *g |= b; return *g; }
static EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b) { CRIT_CALL(); EventBits_t old = *g; *g &= ~b; return old; }
static void eg_at(int64_t at_us, EventBits_t set) { if (g_eg_nsched < 8) { g_eg_sched[g_eg_nsched].at = at_us; g_eg_sched[g_eg_nsched].set = set; g_eg_sched[g_eg_nsched].fired = false; g_eg_nsched++; } }
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t want, int clear, int all, uint32_t ticks) {
    CRIT_CALL(); g_eg_waits++;
    int64_t deadline = fake_now + (int64_t)ticks * 1000;
    for (;;) {
        EventBits_t b = *g;
        if (all ? (b & want) == want : (b & want) != 0) { if (clear) *g &= ~want; return b; }
        int best = -1;
        for (int i = 0; i < g_eg_nsched; i++) if (!g_eg_sched[i].fired && g_eg_sched[i].at <= deadline && (best < 0 || g_eg_sched[i].at < g_eg_sched[best].at)) best = i;
        if (best < 0) break;
        if (g_eg_sched[best].at > fake_now) fake_now = g_eg_sched[best].at;
        g_eg_sched[best].fired = true; *g |= g_eg_sched[best].set;
    }
    if (deadline > fake_now) fake_now = deadline;
    if (g_delay_hook) { void (*h)(void) = g_delay_hook; g_delay_hook = NULL; h(); }
    return *g;
}
static void eg_stub_reset(void) { g_eg_word = 0; g_eg_creates = g_eg_waits = 0; g_eg_create_fail = 0; memset(g_eg_sched, 0, sizeof g_eg_sched); g_eg_nsched = 0; }
