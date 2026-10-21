/* ===== BOARD STUBS (0.5.0). Included by build_harness.py after the 0.4.0 stubs and before any firmware.
 * Dependencies only: nothing here is firmware logic. Names that a firmware header declares are NOT static. ===== */
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <strings.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
static uint32_t g_marks[512]; static int g_nmarks = 0, g_closes = 0;          /* every c6_app_state_mark and c6_app_events_close, recorded by board_after.c's wrappers */
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define ESP_ERR_HTTPD_RESULT_TRUNC 0x5002
#define pdPASS 1
#define pdFALSE 0
typedef unsigned UBaseType_t;
typedef void *SemaphoreHandle_t;
typedef const char *esp_event_base_t;

/* ---- contexts: g_in_stack_ctx is set while a timer callback / event handler / the Zigbee stack's code runs ---- */
static int g_in_stack_ctx = 0, g_ctx_viol = 0, g_ctx_viol_total = 0;       /* total: every scenario so far, for cx1 */
static void ctx_check(void) { if (g_in_stack_ctx) g_ctx_viol++; }
static void board_reset(void);

/* ---- esp_timer: scripted timers on fake time; fake_advance(us) fires them in order ---- */
typedef struct hq_timer {void (*cb)(void *); void *arg; const char *name; int64_t due; bool active; bool used; int starts, stops;} *esp_timer_handle_t;
typedef struct {void (*callback)(void *); void *arg; int dispatch_method; const char *name; bool skip_unhandled_events;} esp_timer_create_args_t;
static struct hq_timer g_timers[24]; static int g_ntimers = 0, g_timer_creates = 0;
static int esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out) {
    CRIT_CALL(); g_timer_creates++;
    if (g_ntimers >= 24) return ESP_ERR_NO_MEM;
    struct hq_timer *t = &g_timers[g_ntimers++]; t->cb = a->callback; t->arg = a->arg; t->name = a->name; t->used = true; t->active = false; *out = t; return 0;
}
static int esp_timer_start_once(esp_timer_handle_t t, uint64_t us) {
    if (!t) return ESP_ERR_INVALID_ARG;
    if (t->active) return ESP_ERR_INVALID_STATE;
    t->active = true; t->due = fake_now + (int64_t)us; t->starts++; return 0;
}
static int esp_timer_stop(esp_timer_handle_t t) {
    if (!t) return ESP_ERR_INVALID_ARG;
    if (!t->active) return ESP_ERR_INVALID_STATE;
    t->active = false; t->stops++; return 0;
}
static void fake_advance(int64_t us) {         /* move fake time forward, firing each due timer at its own due time */
    int64_t target = fake_now + us;
    for (;;) {
        struct hq_timer *n = NULL;
        for (int i = 0; i < g_ntimers; i++) if (g_timers[i].active && g_timers[i].due <= target && (!n || g_timers[i].due < n->due)) n = &g_timers[i];
        if (!n) break;
        if (n->due > fake_now) fake_now = n->due;
        n->active = false;
        int save = g_in_stack_ctx; g_in_stack_ctx = 1; n->cb(n->arg); g_in_stack_ctx = save;
    }
    fake_now = target;
}
static void timers_forget(void) { memset(g_timers, 0, sizeof g_timers); g_ntimers = 0; g_timer_creates = 0; }

/* ---- FreeRTOS extras: mutexes, task creation recorded by name, restart, heap, notify map ---- */
static int g_sem_calls = 0, g_mutex_made = 0;
static int g_sem_held[256], g_sem_viol = 0;                                   /* one held flag per mutex; a take of a held one (a deadlock on the board) or a give of a free one is a violation (cx1) */
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &g_sem_held[g_mutex_made++ % 256]; }
static int xSemaphoreTake(SemaphoreHandle_t s, unsigned ticks) { (void)ticks; CRIT_CALL(); g_sem_calls++; int *h = s; g_sem_viol += *h != 0; *h = 1; return pdTRUE; }
static int xSemaphoreGive(SemaphoreHandle_t s) { CRIT_CALL(); g_sem_calls++; int *h = s; g_sem_viol += *h == 0; *h = 0; return pdTRUE; }
static struct {char name[20]; unsigned stack; void (*fn)(void *); void *arg; int closes_at; int64_t at;} g_tasks[16]; static int g_ntasks = 0;
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, int prio, void **handle) {
    (void)prio; CRIT_CALL(); ctx_check();
    if (g_ntasks < 16) { snprintf(g_tasks[g_ntasks].name, sizeof g_tasks[0].name, "%s", name); g_tasks[g_ntasks].stack = stack; g_tasks[g_ntasks].fn = fn; g_tasks[g_ntasks].arg = arg; g_tasks[g_ntasks].closes_at = g_closes; g_tasks[g_ntasks].at = fake_now; g_ntasks++; }
    if (handle) *handle = &g_tasks[g_ntasks > 0 ? g_ntasks - 1 : 0];
    return pdPASS;
}
static int tasks_named(const char *name) { int n = 0; for (int i = 0; i < g_ntasks; i++) n += !strcmp(g_tasks[i].name, name); return n; }
static unsigned task_stack(const char *name) { for (int i = 0; i < g_ntasks; i++) if (!strcmp(g_tasks[i].name, name)) return g_tasks[i].stack; return 0; }
static void vTaskDelete(void *t) { (void)t; }
static int g_restarts = 0, g_flash_erases = 0;
static void esp_restart(void) { g_restarts++; }
static int nvs_flash_erase(void) { g_flash_erases++; return 0; }
static uint32_t esp_get_free_heap_size(void) { return 100000; }
static uint32_t esp_get_minimum_free_heap_size(void) { return 90000; }
static unsigned uxTaskGetStackHighWaterMark(void *t) { (void)t; return 1000; }
static void log_tasks(void) {}
static void *g_notify_h[8]; static int g_notify_n[8], g_nnotify = 0;
static void notify_other(void *t) { for (int i = 0; i < g_nnotify; i++) if (g_notify_h[i] == t) { g_notify_n[i]++; return; } if (g_nnotify < 8) { g_notify_h[g_nnotify] = t; g_notify_n[g_nnotify++] = 1; } }
static int notify_count(void *t) { for (int i = 0; i < g_nnotify; i++) if (g_notify_h[i] == t) return g_notify_n[i]; return 0; }

/* ---- NVS: namespaces other than c6cfg (handle 2) and c6bulb (handle 1) live in a small typed store, handle >= 10 ---- */
#define FS_MAX 64
#define NVS_NS_NAME_MAX_SIZE 16
#define NVS_TYPE_ANY 0xff
#define NVS_DEFAULT_PART_NAME "nvs"
typedef void *nvs_iterator_t;
typedef struct {char namespace_name[NVS_NS_NAME_MAX_SIZE]; char key[NVS_NS_NAME_MAX_SIZE]; int type;} nvs_entry_info_t;
static struct {char ns[16], key[16]; int type; uint32_t u; unsigned char b[96]; size_t n; bool used;} g_fs[FS_MAX];   /* type 1 u8, 2 u32, 3 str, 4 blob, 5 u16 */
static char g_fs_h[16][16]; static int g_fs_nh = 0;
static int g_fs_open_err = 0, g_fs_set_err = 0, g_fs_commit_err = 0, g_fs_sets = 0, g_fs_commits = 0, g_fs_opens = 0;
static int g_erase_alls = 0, g_erase_all_fail_at = 0, g_erase_keys = 0; static char g_erased_ns[16][16]; static int g_nerased = 0;
static int fs_find(const char *ns, const char *key) { for (int i = 0; i < FS_MAX; i++) if (g_fs[i].used && !strcmp(g_fs[i].ns, ns) && !strcmp(g_fs[i].key, key)) return i; return -1; }
static bool fs_ns_exists(const char *ns) { for (int i = 0; i < FS_MAX; i++) if (g_fs[i].used && !strcmp(g_fs[i].ns, ns)) return true; return false; }
static int fs_count(const char *ns) { int n = 0; for (int i = 0; i < FS_MAX; i++) n += g_fs[i].used && (!ns || !strcmp(g_fs[i].ns, ns)); return n; }
static int fs_put(const char *ns, const char *key, int type, uint32_t u, const void *b, size_t n) {   /* also the probes' way to preload the store */
    int i = fs_find(ns, key);
    if (i < 0) for (i = 0; i < FS_MAX && g_fs[i].used; i++) {}
    if (i >= FS_MAX) return -1;
    g_fs[i].used = true; snprintf(g_fs[i].ns, 16, "%s", ns); snprintf(g_fs[i].key, 16, "%s", key); g_fs[i].type = type; g_fs[i].u = u;
    g_fs[i].n = n < sizeof g_fs[i].b ? n : sizeof g_fs[i].b; if (b) memcpy(g_fs[i].b, b, g_fs[i].n); return i;
}
static void fs_clear(void) { memset(g_fs, 0, sizeof g_fs); g_fs_nh = 0; g_fs_open_err = g_fs_set_err = g_fs_commit_err = g_fs_sets = g_fs_commits = g_fs_opens = 0;
    g_erase_alls = g_erase_all_fail_at = g_erase_keys = g_nerased = 0; }
static const char *fs_str(const char *ns, const char *key) { int i = fs_find(ns, key); return i >= 0 && g_fs[i].type == 3 ? (const char *)g_fs[i].b : NULL; }
static int fs_open(const char *ns, int mode, int *h) {
    CRIT_CALL(); ctx_check(); g_fs_opens++;
    if (g_fs_open_err) return g_fs_open_err;
    if (mode == NVS_READONLY && !fs_ns_exists(ns)) return ESP_ERR_NVS_NOT_FOUND;
    int i = 0; while (i < g_fs_nh && strcmp(g_fs_h[i], ns)) i++;
    if (i == g_fs_nh) { if (g_fs_nh >= 16) return ESP_FAIL; snprintf(g_fs_h[g_fs_nh++], 16, "%s", ns); }
    *h = 10 + i; return 0;
}
static void (*g_fs_commit_hook)(void);                  /* runs once while a commit is in flight (a change during the write) */
static int fs_commit(int h) { (void)h; CRIT_CALL(); ctx_check(); g_fs_commits++; if (g_fs_commit_hook) { void (*f)(void) = g_fs_commit_hook; g_fs_commit_hook = NULL; f(); } return g_fs_commit_err; }
static const char *g_cv_fail_key;                    /* fail exactly one setter, allowing rollback */
static int fs_set(int h, const char *key, int type, uint32_t u, const void *b, size_t n) {
    CRIT_CALL(); ctx_check();
    if (g_cv_fail_key && !strcmp(key, g_cv_fail_key)) { g_cv_fail_key = NULL; return ESP_ERR_NVS_NOT_ENOUGH_SPACE; }
    if (g_fs_set_err) return g_fs_set_err;
    int i = fs_put(g_fs_h[h - 10], key, type, u, b, n);
    if (i < 0) return ESP_FAIL;
    g_fs_sets++; return 0;
}
static int fs_get(int h, const char *key, int type, uint32_t *u, void *b, size_t *n) {
    CRIT_CALL(); ctx_check();
    int i = fs_find(g_fs_h[h - 10], key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (g_fs[i].type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (type == 3 || type == 4) {
        if (!b) { *n = g_fs[i].n; return 0; }
        if (*n < g_fs[i].n) { *n = g_fs[i].n; return ESP_ERR_NVS_INVALID_LENGTH; }
        memcpy(b, g_fs[i].b, g_fs[i].n); *n = g_fs[i].n; return 0;
    }
    *u = g_fs[i].u; return 0;
}
static int nvs_set_u16(int h, const char *key, uint16_t v) { if (h >= 10) return fs_set(h, key, 5, v, NULL, 0); return ESP_FAIL; }
static int nvs_get_u16(int h, const char *key, uint16_t *v) { uint32_t u = 0; int e = h >= 10 ? fs_get(h, key, 5, &u, NULL, NULL) : ESP_FAIL; if (!e) *v = (uint16_t)u; return e; }
static int g_cv_erase_error;
static int nvs_erase_key(int h, const char *key) {
    CRIT_CALL(); ctx_check(); g_erase_keys++;
    if (g_cv_erase_error) return g_cv_erase_error;
    int i = fs_find(g_fs_h[h - 10], key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    g_fs[i].used = false; return 0;
}
static int nvs_erase_all(int h) {
    CRIT_CALL(); ctx_check(); g_erase_alls++;
    if (g_erase_all_fail_at && g_erase_alls == g_erase_all_fail_at) return ESP_FAIL;
    const char *nsn = h >= 10 ? g_fs_h[h - 10] : h == 1 ? "c6bulb" : "c6cfg";
    if (g_nerased < 16) snprintf(g_erased_ns[g_nerased++], 16, "%s", nsn);
    for (int i = 0; i < FS_MAX; i++) {
        if (g_fs[i].used && !strcmp(g_fs[i].ns, nsn)) g_fs[i].used = false;
    }
    return 0;
}
static int nvs_entry_find(const char *part, const char *ns, int type, nvs_iterator_t *it) {
    (void)part; (void)ns; (void)type; CRIT_CALL(); ctx_check();
    for (int i = 0; i < FS_MAX; i++) if (g_fs[i].used) { *it = (void *)(intptr_t)(i + 1); return 0; }
    *it = NULL; return ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_entry_next(nvs_iterator_t *it) {
    for (int i = (int)(intptr_t)*it; i < FS_MAX; i++) if (g_fs[i].used) { *it = (void *)(intptr_t)(i + 1); return 0; }
    *it = NULL; return ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *info) {
    int i = (int)(intptr_t)it - 1; snprintf(info->namespace_name, sizeof info->namespace_name, "%s", g_fs[i].ns);
    snprintf(info->key, sizeof info->key, "%s", g_fs[i].key); info->type = g_fs[i].type; return 0;
}
static void nvs_release_iterator(nvs_iterator_t it) { (void)it; }

/* ---- hash, randomness ---- */
static int g_rand_calls = 0; static uint8_t g_rand_seq[64]; static int g_rand_seq_n = 0;     /* scripted bytes (cycled); none: a counter pattern */
static void esp_fill_random(void *buf, size_t n) {
    g_rand_calls++;
    for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = g_rand_seq_n ? g_rand_seq[i % (size_t)g_rand_seq_n] : (uint8_t)(0x10 + i * 7 + g_rand_calls);
}
static unsigned char g_sha_in[300]; static size_t g_sha_len = 0; static int g_sha_calls = 0;
static uint8_t g_sha_fixed[32]; static bool g_sha_use_fixed = false;                         /* scripted digest, else a fixed function of the input */
/* 0.6.0: a REAL SHA-256 (FIPS 180-4), one-shot and incremental, so a probe can compute the digest the firmware must match. Until 0.5.2 the one-shot
 * was a fake hash (FNV), which no digest comparison against an outside value could have used. The int returns are IDF 5.5's. */
typedef struct mbedtls_sha256_context { uint32_t st[8]; uint64_t total; unsigned char buf[64]; size_t blen; } mbedtls_sha256_context;
static const uint32_t sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define SHA_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(mbedtls_sha256_context *c, const unsigned char *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = SHA_ROR(w[i - 15], 7) ^ SHA_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = SHA_ROR(w[i - 2], 17) ^ SHA_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->st[0]; b = c->st[1]; cc = c->st[2]; d = c->st[3]; e = c->st[4]; f = c->st[5]; g = c->st[6]; h = c->st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (SHA_ROR(e, 6) ^ SHA_ROR(e, 11) ^ SHA_ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (SHA_ROR(a, 2) ^ SHA_ROR(a, 13) ^ SHA_ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->st[0] += a; c->st[1] += b; c->st[2] += cc; c->st[3] += d; c->st[4] += e; c->st[5] += f; c->st[6] += g; c->st[7] += h;
}
static void mbedtls_sha256_init(mbedtls_sha256_context *c) { memset(c, 0, sizeof *c); }
static void mbedtls_sha256_free(mbedtls_sha256_context *c) { if (c) memset(c, 0, sizeof *c); }
static int g_sha_inc_fail = 0;                         /* != 0: the next starts/update/finish returns this error (MBEDTLS_ERR_*), once */
static int sha_inc_err(void) { int e = g_sha_inc_fail; g_sha_inc_fail = 0; return e; }
static int mbedtls_sha256_starts(mbedtls_sha256_context *c, int is224) {
    (void)is224; int e = sha_inc_err(); if (e) return e;
    static const uint32_t h0[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->st, h0, sizeof h0); c->total = 0; c->blen = 0; return 0;
}
static int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *in, size_t n) {
    int e = sha_inc_err(); if (e) return e;
    c->total += n;
    while (n) {
        size_t k = 64 - c->blen; if (k > n) k = n;
        memcpy(c->buf + c->blen, in, k); c->blen += k; in += k; n -= k;
        if (c->blen == 64) { sha_block(c, c->buf); c->blen = 0; }
    }
    return 0;
}
static int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char *out) {
    int e = sha_inc_err(); if (e) return e;
    uint64_t bits = c->total * 8; unsigned char pad = 0x80;
    uint64_t keep = c->total; mbedtls_sha256_update(c, &pad, 1); pad = 0;
    while (c->blen != 56) mbedtls_sha256_update(c, &pad, 1);
    unsigned char len[8]; for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    mbedtls_sha256_update(c, len, 8); c->total = keep;
    for (int i = 0; i < 8; i++) { out[4 * i] = (unsigned char)(c->st[i] >> 24); out[4 * i + 1] = (unsigned char)(c->st[i] >> 16); out[4 * i + 2] = (unsigned char)(c->st[i] >> 8); out[4 * i + 3] = (unsigned char)c->st[i]; }
    return 0;
}
static void sha256_hex_of(const void *data, size_t n, char hex[65]) {          /* the header value a probe sends: the hex SHA-256 of `data`, lower case */
    int keep = g_sha_inc_fail; g_sha_inc_fail = 0;
    mbedtls_sha256_context c; unsigned char d[32]; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0); mbedtls_sha256_update(&c, data, n); mbedtls_sha256_finish(&c, d);
    g_sha_inc_fail = keep;
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
}
static int mbedtls_sha256(const unsigned char *in, size_t len, unsigned char *out, int is224) {
    (void)is224; g_sha_calls++; g_sha_len = len; memcpy(g_sha_in, in, len < sizeof g_sha_in ? len : sizeof g_sha_in);
    if (g_sha_use_fixed) { memcpy(out, g_sha_fixed, 32); return 0; }
    int keep = g_sha_inc_fail; g_sha_inc_fail = 0;            /* the one-shot (the token digest) never fails: a failure asked for belongs to the incremental calls of the code under test */
    mbedtls_sha256_context c; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0); mbedtls_sha256_update(&c, in, len); mbedtls_sha256_finish(&c, out);
    g_sha_inc_fail = keep; return 0;
}
static void mbedtls_platform_zeroize(void *p, size_t n) { volatile unsigned char *v = p; while (n--) *v++ = 0; }

/* ---- more of the HTTP server: headers, chunks, async requests, registration ---- */
static int g_async_begun = 0, g_async_done = 0, g_chunks_n = 0, g_chunk_fail_at = 0, g_async_fail = 0;
static char g_chunks[16384]; static size_t g_chunks_len = 0;
static httpd_req_t g_async_pool[4];
#define G_TOKEN "tok-zq9-0123456789abcdef"            /* the made-up claim token every probe's claimed board knows */
#define G_BEARER "Bearer " G_TOKEN
static int g_auto_bearer = 0;                            /* 1: a request with no Authorization of its own carries G_BEARER */
static const char *hdr_of(httpd_req_t *r, const char *k) { return !strcasecmp(k, "Authorization") ? (r->h_auth ? r->h_auth : g_auto_bearer ? G_BEARER : NULL) : !strcasecmp(k, "X-OTA-Token") ? r->h_ota : !strcasecmp(k, "X-Firmware-SHA256") ? r->h_sha : NULL; }
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *k) { const char *v = hdr_of(r, k); return v ? strlen(v) : 0; }
static int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *k, char *buf, size_t n) {
    const char *v = hdr_of(r, k);
    if (!v) return ESP_ERR_NOT_FOUND;
    if (strlen(v) >= n) return ESP_ERR_HTTPD_RESULT_TRUNC;
    memcpy(buf, v, strlen(v) + 1); return 0;
}
static int httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) { if (r->nhdr < 6) { snprintf(r->hdr[r->nhdr][0], 64, "%s", k); snprintf(r->hdr[r->nhdr][1], 64, "%s", v); r->nhdr++; } return 0; }
static const char *resp_hdr(httpd_req_t *r, const char *k) { for (int i = 0; i < r->nhdr; i++) if (!strcmp(r->hdr[i][0], k)) return r->hdr[i][1]; return NULL; }
static int httpd_resp_send(httpd_req_t *r, const char *b, ssize_t n) { CRIT_CALL(); r->code = r->status_code ? r->status_code : 200; snprintf(r->response, sizeof r->response, "%.*s", (int)n, b); r->resp_len = (int)n; resp_note(r, b, (long)n); return 0; }
static httpd_req_t *g_chunk_want; static int g_chunk_wrong = 0;          /* a probe's stream request: a chunk sent on any other is counted */
static int httpd_resp_send_chunk(httpd_req_t *r, const char *b, ssize_t n) {
    CRIT_CALL(); g_chunks_n++; if (g_chunk_want && r != g_chunk_want) g_chunk_wrong++;
    if (b && n > 0 && !(g_chunk_fail_at && g_chunks_n == g_chunk_fail_at)) resp_note(NULL, b, (long)n);
    if (g_chunk_fail_at && g_chunks_n == g_chunk_fail_at) return ESP_FAIL;
    if (b && n > 0 && g_chunks_len + (size_t)n < sizeof g_chunks) { memcpy(g_chunks + g_chunks_len, b, (size_t)n); g_chunks_len += (size_t)n; g_chunks[g_chunks_len] = 0; }
    return 0;
}
static int httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out) {
    CRIT_CALL(); if (g_async_fail) return ESP_ERR_NO_MEM;
    httpd_req_t *a = &g_async_pool[g_async_begun % 4]; g_async_begun++; *a = *r; a->is_async = 1; *out = a; return 0;
}
static int httpd_req_async_handler_complete(httpd_req_t *r) { (void)r; CRIT_CALL(); g_async_done++; return 0; }
static int httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t n) { if (!r->query) return ESP_ERR_NOT_FOUND; snprintf(buf, n, "%s", r->query); return 0; }
static int httpd_query_key_value(const char *q, const char *key, char *val, size_t n) {
    size_t kl = strlen(key);
    for (const char *p = q; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') { size_t l = strcspn(p + kl + 1, "&"); if (l >= n) return ESP_ERR_HTTPD_RESULT_TRUNC; memcpy(val, p + kl + 1, l); val[l] = 0; return 0; }
        p = strchr(p, '&'); if (p) p++;
    }
    return ESP_ERR_NOT_FOUND;
}
static int httpd_resp_send_500(httpd_req_t *r) { r->status_code = 500; r->code = 500; snprintf(r->response, sizeof r->response, "500 Internal Server Error"); resp_note(r, r->response, 25); return 0; }
static void chunks_clear(void) { g_chunks_len = 0; g_chunks[0] = 0; g_chunks_n = 0; g_chunk_fail_at = 0; g_async_begun = g_async_done = 0; g_async_fail = 0; }

/* ---- the app description, host name, board info (stubs of c6_base.c's own readers) ---- */
typedef struct {            /* IDF 5.5's layout (esp_app_desc.h): the firmware takes sizeof of the char arrays, and 256 bytes of it live in a heap buffer */
    uint32_t magic_word; uint32_t secure_version; uint32_t reserv1[2]; char version[32]; char project_name[32]; char time[16]; char date[16]; char idf_ver[32];
    uint8_t app_elf_sha256[32]; uint16_t min_efuse_blk_rev_full; uint16_t max_efuse_blk_rev_full; uint8_t mmu_page_size; uint8_t reserv3[3]; uint32_t reserv2[18];
} esp_app_desc_t;
_Static_assert(sizeof(esp_app_desc_t) == 256, "esp_app_desc_t is 256 bytes");
static esp_app_desc_t g_run_desc = {.version = "0.5.0", .project_name = "c6_bulb", .time = "12:00:00", .date = "Oct 14 2026", .idf_ver = "v5.5"};   /* the RUNNING image's */
static const esp_app_desc_t *esp_app_get_description(void) { return &g_run_desc; }
static char s_host[16] = "c6-112233", s_mac[18] = "aa:bb:cc:11:22:33";
static c6_base_info_t g_info = {.id = "112233", .mac = "aa:bb:cc:11:22:33", .fw = "0.5.0", .uptime_s = 42, .ip = "192.0.2.50", .rssi = -55, .ota_state = "valid", .partition = "ota_0"};
static int g_got_ip_calls = 0, g_got_ip_last = -1;
const char *c6_base_host(void) { return s_host; }
void c6_base_got_ip(bool up) { g_got_ip_calls++; g_got_ip_last = up; }
void c6_base_info(c6_base_info_t *out) { *out = g_info; }

/* ---- mDNS ---- */
typedef struct {const char *key, *value;} mdns_txt_item_t;
static int g_mdns_adds = 0, g_mdns_txt_sets = 0, g_mdns_name_sets = 0, g_mdns_add_err = 0, g_mdns_other_services = 0;
static char g_mdns_service[24], g_mdns_proto[8], g_mdns_inst[64]; static int g_mdns_port = 0;
static char g_mdns_txt[8][2][48]; static int g_mdns_ntxt = 0;
static char g_mdns_all[65536]; static size_t g_mdns_all_len = 0;                 /* every instance name and TXT pair ever given to mDNS (cx3 scans it) */
static void mdns_note(const char *a, const char *b) { size_t n = strlen(a) + strlen(b) + 3; if (g_mdns_all_len + n < sizeof g_mdns_all) g_mdns_all_len += (size_t)snprintf(g_mdns_all + g_mdns_all_len, sizeof g_mdns_all - g_mdns_all_len, "%s=%s\n", a, b); }
static int mdns_service_add(const char *inst, const char *svc, const char *proto, uint16_t port, mdns_txt_item_t *txt, size_t n) {
    CRIT_CALL(); ctx_check(); if (g_mdns_add_err) return g_mdns_add_err;
    if (strcmp(svc, "_zigbee-anthema")) g_mdns_other_services++;
    g_mdns_adds++; snprintf(g_mdns_service, sizeof g_mdns_service, "%s", svc); snprintf(g_mdns_proto, sizeof g_mdns_proto, "%s", proto);
    snprintf(g_mdns_inst, sizeof g_mdns_inst, "%s", inst); mdns_note("instance", inst); g_mdns_port = port; g_mdns_ntxt = 0;
    for (size_t i = 0; i < n && i < 8; i++) { mdns_note(txt[i].key, txt[i].value); snprintf(g_mdns_txt[i][0], 48, "%s", txt[i].key); snprintf(g_mdns_txt[i][1], 48, "%s", txt[i].value); g_mdns_ntxt++; }
    return 0;
}
static int mdns_service_txt_item_set(const char *svc, const char *proto, const char *k, const char *v) {
    (void)svc; (void)proto; CRIT_CALL(); ctx_check(); g_mdns_txt_sets++;
    for (int i = 0; i < g_mdns_ntxt; i++) if (!strcmp(g_mdns_txt[i][0], k)) { mdns_note(k, v); snprintf(g_mdns_txt[i][1], 48, "%s", v); return 0; }
    return ESP_ERR_NOT_FOUND;
}
static int mdns_service_instance_name_set(const char *svc, const char *proto, const char *name) { (void)svc; (void)proto; CRIT_CALL(); ctx_check(); g_mdns_name_sets++; mdns_note("instance", name); snprintf(g_mdns_inst, sizeof g_mdns_inst, "%s", name); return 0; }
static const char *mdns_txt(const char *k) { for (int i = 0; i < g_mdns_ntxt; i++) if (!strcmp(g_mdns_txt[i][0], k)) return g_mdns_txt[i][1]; return NULL; }

/* ---- GPIO (the last level written to each pin; the button pin's scripted samples) ---- */
#define CONFIG_C6_LED_GPIO 8
#define CONFIG_C6_BUTTON_GPIO 9
typedef int gpio_num_t;
static int g_pin_lvl[40] = {[0 ... 39] = -1};                     /* the last level written to each pin; -1: never written */
static int gpio_set_level(int pin, uint32_t lvl) { if (pin >= 0 && pin < 40) g_pin_lvl[pin] = (int)lvl; return 0; }
static void gpio_reset_pin(int pin) { (void)pin; }
static int gpio_set_direction(int pin, int mode) { (void)pin; (void)mode; return 0; }
/* ---- led_strip: the colour LED (a WS2812 on the RMT). g_rgb is what the strip shows after each refresh, g_rgb_writes counts refreshes ---- */
#define CONFIG_C6_RGB_GPIO 20
#define CONFIG_C6_RGB_PWR_GPIO 19
#define CONFIG_C6_RGB_LEVEL 40
#define LED_MODEL_WS2812 1
#define LED_STRIP_COLOR_COMPONENT_FMT_GRB 1
#define RMT_CLK_SRC_DEFAULT 0
typedef void *led_strip_handle_t;
typedef struct {int strip_gpio_num; uint32_t max_leds; int led_model; int color_component_format;} led_strip_config_t;
typedef struct {int clk_src; uint32_t resolution_hz;} led_strip_rmt_config_t;
static int g_strip_new_ret = 0, g_strip_news = 0, g_strip_gpio = -1, g_strip_leds = 0, g_strip_model = 0, g_strip_fmt = 0; static uint32_t g_strip_res = 0;
static uint32_t g_rgb_pending = 0, g_rgb = 0; static int g_rgb_writes = 0;
static int g_strip_dummy;
static int led_strip_new_rmt_device(const led_strip_config_t *sc, const led_strip_rmt_config_t *rc, led_strip_handle_t *out) {
    CRIT_CALL(); ctx_check(); g_strip_news++; g_strip_gpio = sc->strip_gpio_num; g_strip_leds = (int)sc->max_leds; g_strip_model = sc->led_model; g_strip_fmt = sc->color_component_format; g_strip_res = rc->resolution_hz;
    if (g_strip_new_ret) return g_strip_new_ret;
    *out = &g_strip_dummy; return 0;
}
static int led_strip_set_pixel(led_strip_handle_t h, uint32_t idx, uint32_t r, uint32_t g, uint32_t b) { (void)h; CRIT_CALL(); ctx_check(); if (idx == 0) g_rgb_pending = r << 16 | g << 8 | b; return 0; }
static int led_strip_refresh(led_strip_handle_t h) { (void)h; CRIT_CALL(); ctx_check(); g_rgb = g_rgb_pending; g_rgb_writes++; return 0; }
#define GPIO_MODE_OUTPUT 2
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
static int (*g_btn_sample)(int64_t now_us);          /* the pin level (1 high / 0 low) at fake time; NULL: high */
static int gpio_get_level(int pin) { (void)pin; return g_btn_sample ? g_btn_sample(fake_now) : 1; }

/* ---- Zigbee descriptors: what create_device and color_shape ask of the stack (the hu probes). Handles are tags, not memory; every call is recorded ---- */
#ifndef EZB_ZCL_CLUSTER_ID_BASIC
#define EZB_ZCL_CLUSTER_ID_BASIC 0
#endif
#define EZB_ZCL_CLUSTER_CLIENT 0
#define EZB_ZCL_BASIC_POWER_SOURCE_SINGLE_PHASE_MAINS 1
#define EZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID 1
#define EZB_ZCL_ATTR_BASIC_STACK_VERSION_ID 2
#define EZB_ZCL_ATTR_BASIC_HW_VERSION_ID 3
#define EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID 4
#define EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID 5
#define EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID 0x400B
#define EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID 0x400C
typedef int ezb_zcl_core_action_callback_id_t;
static void on_zcl_action(ezb_zcl_core_action_callback_id_t id, void *message) { (void)id; (void)message; }      /* create_device only registers it */
typedef struct {int power_source, zcl_version;} hu_basic_cfg_t;
typedef struct {int color_capabilities, current_x, current_y, color_mode, enhanced_color_mode, number_of_primaries;} hu_color_cfg_t;
typedef struct {hu_basic_cfg_t basic_cfg; hu_color_cfg_t color_cfg;} ezb_zha_color_dimmable_light_config_t;
#define EZB_ZHA_COLOR_DIMMABLE_LIGHT_CONFIG() ((ezb_zha_color_dimmable_light_config_t){{0, 0}, {0, 0, 0, 0, 0, 0}})
typedef void *ezb_af_device_desc_t;
typedef void *ezb_af_ep_desc_t;
typedef void *ezb_zcl_cluster_desc_t;
static ezb_zha_color_dimmable_light_config_t g_hu_cfg; static int g_hu_creates = 0, g_hu_ep_id = -1, g_hu_registers = 0, g_hu_endpoints = 0, g_hu_manuf_code = -1, g_hu_handler_set = 0, g_hu_app_device_id = -1;
static int g_color_attrs[64], g_ncolor = 0; static uint16_t g_color_ct[3]; static int g_color_nct = 0;
static char g_hu_manuf[34], g_hu_model[34];                   /* the ZCL strings as given: a length byte, then the text */
static int g_hu_dev, g_hu_ep;
static ezb_af_device_desc_t ezb_af_create_device_desc(void) { CRIT_CALL(); return &g_hu_dev; }
static ezb_af_ep_desc_t ezb_zha_create_color_dimmable_light(int ep_id, const ezb_zha_color_dimmable_light_config_t *c) { CRIT_CALL(); g_hu_creates++; g_hu_ep_id = ep_id; g_hu_cfg = *c; return &g_hu_ep; }
static ezb_zcl_cluster_desc_t ezb_af_endpoint_get_cluster_desc(ezb_af_ep_desc_t ep, int cluster, int role) { (void)ep; (void)role; return (ezb_zcl_cluster_desc_t)(uintptr_t)(0x10000 + cluster); }
static int g_hu_wrong_cl = 0;                                 /* an attribute added to a cluster descriptor other than its own (the descriptor is 0x10000 + the cluster id) */
static void ezb_zcl_basic_cluster_desc_add_attr(ezb_zcl_cluster_desc_t cl, int id, const void *v) {
    g_hu_wrong_cl += cl != (ezb_zcl_cluster_desc_t)(uintptr_t)(0x10000 + EZB_ZCL_CLUSTER_ID_BASIC); if (id == EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID) memcpy(g_hu_manuf, v, 1 + (size_t)*(const unsigned char *)v); else if (id == EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID) memcpy(g_hu_model, v, 1 + (size_t)*(const unsigned char *)v);
}
static void ezb_zcl_color_control_cluster_desc_add_attr(ezb_zcl_cluster_desc_t cl, int id, const void *v) {
    g_hu_wrong_cl += cl != (ezb_zcl_cluster_desc_t)(uintptr_t)(0x10000 + EZB_ZCL_CLUSTER_ID_COLOR_CONTROL); if (g_ncolor < 64) g_color_attrs[g_ncolor++] = id;
    if (id == EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID || id == EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID || id == EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID) { if (g_color_nct < 3) g_color_ct[g_color_nct++] = *(const uint16_t *)v; }
}
static int ezb_af_device_add_endpoint_desc(ezb_af_device_desc_t d, ezb_af_ep_desc_t ep) { (void)d; (void)ep; g_hu_endpoints++; return 0; }
static int ezb_af_device_desc_register(ezb_af_device_desc_t d) { (void)d; g_hu_registers++; return 0; }
static void ezb_af_node_desc_set_manuf_code(int code) { g_hu_manuf_code = code; }
static void ezb_zcl_core_action_handler_register(void (*h)(ezb_zcl_core_action_callback_id_t, void *)) { g_hu_handler_set = h == on_zcl_action; }
#if CONFIG_C6_ZB_PROFILE_SENGLED_E11
#define CONFIG_C6_ZB_E11_DEVICE_ID 0x0102                       /* the Kconfig default */
#define EZB_ZCL_ATTR_METERING_MULTIPLIER_ID 1
#define EZB_ZCL_ATTR_METERING_DIVISOR_ID 2
#define EZB_ZCL_ATTR_METERING_DEMAND_FORMATTING_ID 3
#define EZB_ZCL_ATTR_METERING_HISTORICAL_CONSUMPTION_FORMATTING_ID 4
#define EZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID 5
#define EZB_ZCL_ATTR_TYPE_UINT8 0x20
#define EZB_ZCL_ATTR_TYPE_UINT16 0x21
#define EZB_ZCL_ATTR_TYPE_UINT32 0x23
#define EZB_ZCL_ATTR_ACCESS_READ_ONLY 1
typedef struct {int current_summation_delivered, status, unit_of_measure, summation_formatting, metering_device_type;} ezb_zcl_metering_cluster_server_config_t;
typedef struct {int cluster_id; void (*init_func)(uint8_t); void (*deinit_func)(uint8_t);} ezb_zcl_custom_cluster_config_t;
typedef struct {uint64_t upgrade_server_id; uint32_t file_offset; int image_upgrade_status, manufacturer_id, image_type_id;} ezb_zcl_ota_upgrade_cluster_client_config_t;
static int g_hu_e11_clusters = 0;
static int ezb_af_ep_desc_set_app_version(ezb_af_ep_desc_t ep, int v) { (void)ep; (void)v; return 0; }
static int ezb_af_ep_desc_set_app_device_id(ezb_af_ep_desc_t ep, int id) { (void)ep; g_hu_app_device_id = id; return 0; }
static ezb_zcl_cluster_desc_t ezb_zcl_metering_create_cluster_desc(const ezb_zcl_metering_cluster_server_config_t *c, int role) { (void)c; (void)role; return (ezb_zcl_cluster_desc_t)(uintptr_t)0x10702; }
static void ezb_zcl_metering_cluster_desc_add_attr(ezb_zcl_cluster_desc_t cl, int id, const void *v) { (void)cl; (void)id; (void)v; }
static ezb_zcl_cluster_desc_t ezb_zcl_custom_create_cluster_desc(const ezb_zcl_custom_cluster_config_t *c, int role) { (void)role; return (ezb_zcl_cluster_desc_t)(uintptr_t)(0x20000 + c->cluster_id); }
static void ezb_zcl_custom_cluster_desc_add_manuf_attr(ezb_zcl_cluster_desc_t cl, int id, int type, int access, int manuf, const void *v) { (void)cl; (void)id; (void)type; (void)access; (void)manuf; (void)v; }
static int ezb_af_endpoint_add_cluster_desc(ezb_af_ep_desc_t ep, ezb_zcl_cluster_desc_t cl) { (void)ep; (void)cl; g_hu_e11_clusters++; return 0; }
static ezb_zcl_cluster_desc_t ezb_zcl_ota_upgrade_create_cluster_desc(const ezb_zcl_ota_upgrade_cluster_client_config_t *c, int role) { (void)c; (void)role; return (ezb_zcl_cluster_desc_t)(uintptr_t)0x10019; }
#else
#define CONFIG_C6_ZB_MANUFACTURER "zq9-maker"                   /* the Kconfig strings and code are made-up here; the probes read them back from the Basic attributes */
#define CONFIG_C6_ZB_MODEL "zq9-model"
#define CONFIG_C6_ZB_MANUF_CODE 0x4A5B
#endif

/* ---- Zigbee: only what bulb.c's retry, group, erase and PAN code asks of the stack ---- */
#define EZB_ZCL_CLUSTER_ID_GROUPS 4
#define EZB_BDB_MODE_INITIALIZATION 1
#define EZB_BDB_MODE_NETWORK_STEERING 2
#define EZB_BDB_STATUS_SUCCESS 0
#define EZB_ZDO_LEAVE_TYPE_RESET 0
#define CONFIG_C6_ZB_CHANNEL_MASK 0x07FFF800
typedef ezb_extaddr_t ezb_extpanid_t;
typedef int ezb_bdb_comm_status_t;
typedef struct {int leave_type;} ezb_zdo_signal_leave_params_t;
typedef int ezb_app_signal_type_t;
enum {EZB_ZDO_SIGNAL_SKIP_STARTUP = 1, EZB_BDB_SIGNAL_DEVICE_FIRST_START, EZB_BDB_SIGNAL_DEVICE_REBOOT, EZB_BDB_SIGNAL_STEERING, EZB_ZDO_SIGNAL_LEAVE, EZB_SIGNAL_OTHER};
typedef struct {int type; const void *params;} ezb_app_signal_t;
static ezb_app_signal_type_t ezb_app_signal_get_type(const ezb_app_signal_t *s) { return s->type; }
static const void *ezb_app_signal_get_params(const ezb_app_signal_t *s) { return s->params; }
static const char *ezb_app_signal_to_string(int t) { (void)t; return "signal"; }
static bool g_factory_new = true, g_joined = false; static int g_commission_ret = 0, g_commissions = 0, g_commission_modes[512], g_commission_afters = 0, g_local_resets = 0;
static int64_t g_commission_at[512];
static int c6_zb_commission(int mode) { if (g_commissions < 512) { g_commission_modes[g_commissions] = mode; g_commission_at[g_commissions] = fake_now; } g_commissions++; return mode == EZB_BDB_MODE_NETWORK_STEERING ? g_commission_ret : 0; }
static void c6_zb_commission_after(int mode, unsigned ms) { (void)mode; (void)ms; g_commission_afters++; }
static bool ezb_bdb_is_factory_new(void) { return g_factory_new; }
static bool ezb_bdb_dev_joined(void) { return g_joined; }
static void ezb_bdb_reset_via_local_action(void) { g_local_resets++; }
static uint32_t ezb_nwk_get_frame_counter(void) { return 7; }
static void log_network(const char *what) { hlog("%s: PAN stub", what); }
static void log_onoff_reporting(const char *when) { hlog("onoff reporting %s", when); }
static int g_order = 0, g_order_pan = 0, g_order_start = 0, g_set_use_pan_calls = 0, g_pri_mask = 0, g_sec_mask = 0; static uint64_t g_pan_u64 = 0;
static int ezb_bdb_set_primary_channel_set(uint32_t m) { g_pri_mask = (int)m; return 0; }
static int ezb_bdb_set_secondary_channel_set(uint32_t m) { g_sec_mask = (int)m; return 0; }
static void ezb_set_use_extended_panid(const ezb_extpanid_t *e) { g_set_use_pan_calls++; g_pan_u64 = e->u64; g_order_pan = ++g_order; }
static int g_zb_rx_aps = 0, g_zb_rx_zcl = 0; static int64_t g_zb_rx_last = 0;
static void c6_zb_rx_stats(uint32_t *a, uint32_t *z, int64_t *last) { *a = (uint32_t)g_zb_rx_aps; *z = (uint32_t)g_zb_rx_zcl; *last = g_zb_rx_last; }
static void c6_zb_eui64_str(const ezb_extaddr_t *e, char *out, size_t n) { snprintf(out, n, "%016llX", (unsigned long long)e->u64); }
static void ezb_get_extended_address(ezb_extaddr_t *e) { e->u64 = 0xB0CE18C600112233ull; }
static void ezb_nwk_get_extended_panid(ezb_extpanid_t *e) { e->u64 = 0x0102030405060708ull; }
static uint16_t ezb_get_short_address(void) { return 0x1234; }
static uint16_t ezb_nwk_get_panid(void) { return 0xABCD; }
static int ezb_nwk_get_current_channel(void) { return 20; }

/* ---- prototypes of the firmware headers' functions, defined by the extracted files (a mismatch is a compile error) ---- */

static void claim_now(void);
#define CONFIG_C6_OTA_TOKEN g_kconfig_ota_token
static const char *g_kconfig_ota_token = "";

/* ---- small readers shared by the probes ---- */
static int str_count(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++; return n; }   /* non-overlapping */
static int log_count(size_t from, const char *needle) { return str_count(g_logbuf + from, needle); }
static struct hq_timer *timer_by_name(const char *name) { for (int i = 0; i < g_ntimers; i++) if (g_timers[i].name && !strcmp(g_timers[i].name, name)) return &g_timers[i]; return NULL; }
static int64_t timer_in_us(const char *name) { struct hq_timer *t = timer_by_name(name); return t && t->active ? t->due - fake_now : -1; }

/* ---- everything the firmware sent, for the secrets scan (cx3) and the error-shape sweep (cx2) ---- */
static char g_out_all[1048576]; static size_t g_out_len = 0; static int g_api_call = 0;
static struct {int code; char type[48]; char body[440];} g_errs[256]; static int g_nerrs = 0;
static void resp_note(httpd_req_t *r, const char *b, long n) {
    if (n > 0 && g_out_len + (size_t)n + 2 < sizeof g_out_all) { memcpy(g_out_all + g_out_len, b, (size_t)n); g_out_len += (size_t)n; g_out_all[g_out_len++] = '\n'; g_out_all[g_out_len] = 0; }
    if (r && g_api_call && r->code >= 400 && g_nerrs < 256) {
        g_errs[g_nerrs].code = r->code; snprintf(g_errs[g_nerrs].type, sizeof g_errs[0].type, "%s", r->type); snprintf(g_errs[g_nerrs].body, sizeof g_errs[0].body, "%.*s", (int)n, b); g_nerrs++;
    }
}
static int api_call(int (*h)(httpd_req_t *), httpd_req_t *r) { g_api_call++; int e = h(r); g_api_call--; return e; }
static void req_init(httpd_req_t *r) { memset(r, 0, sizeof *r); }
static void req_body(httpd_req_t *r, const char *body) { r->body = body; r->body_len = strlen(body); r->content_len = r->body_len; }

/* ---- the button pin's configuration ---- */
typedef struct {uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type;} gpio_config_t;
static gpio_config_t g_gpio_cfg; static int g_gpio_configs = 0;
static int gpio_config(const gpio_config_t *c) { g_gpio_cfg = *c; g_gpio_configs++; return 0; }

/* ---- allocation counters: the firmware's malloc/calloc/free (macros from here on) and cJSON's own hooks ---- */
static long g_allocs = 0, g_frees = 0, g_cj_live = 0, g_cj_allocs = 0;
static size_t g_malloc_fail_size = 0;                                          /* != 0: the next malloc of exactly this size fails once (returns NULL) */
static void *hq_malloc(size_t n) { if (g_malloc_fail_size && n == g_malloc_fail_size) { g_malloc_fail_size = 0; return NULL; } g_allocs++; return malloc(n); }
static void *hq_calloc(size_t a, size_t b) { g_allocs++; return calloc(a, b); }
static void hq_free(void *p) { if (p) g_frees++; free(p); }
#define malloc(n) hq_malloc(n)
#define calloc(a, b) hq_calloc(a, b)
#define free(p) hq_free(p)
static long g_cj_fail_at = 0;                                                  /* > 0: the n-th cJSON allocation from now fails (returns NULL) */
static void *cj_malloc(size_t n) { if (g_cj_fail_at > 0 && --g_cj_fail_at == 0) return NULL; g_cj_live++; g_cj_allocs++; return malloc(n); }
static void cj_free(void *p) { if (p) g_cj_live--; free(p); }
static void board_hooks(void) { cJSON_Hooks h = {cj_malloc, cj_free}; cJSON_InitHooks(&h); }
