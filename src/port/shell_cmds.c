/*
 * Test shell commands (USB console):
 *   watch shot [name]          screenshot as hex lines ("PX 0 y w <rgb565...>")
 *   watch tap X Y              touch and release
 *   watch hold X Y MS          touch for MS milliseconds
 *   watch swipe X1 Y1 X2 Y2    drag over ~250 ms
 *   watch btn pwr|boot click|double|hold|long
 *   watch heap                 free C heap
 */
#include <zephyr/autoconf.h>
#if defined(CONFIG_SHELL)   /* test commands; left out of the release build */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include "lvgl.h"
#include "lvgl_private.h"
#include "lvgl_port.h"
#include "hal/button.h"
#include "esp_heap_caps.h"
#include "hal/audio.h"
#include "app/wifi_mgr.h"
#include "app/ble_mgr.h"
#include "app/ota_update.h"
#include "esp_ota_ops.h"
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/mcuboot.h>

void touch_fake(bool active, bool pressed, int x, int y);
void on_button(button_id_t id, button_event_t ev);

/* Screenshots: the next full redraw is printed as it is flushed to the panel,
 * so it shows exactly what's on screen, overlays included, without a frame buffer. */
static lv_display_flush_cb_t s_orig_flush;
static volatile bool s_capture;
static struct k_sem s_capture_done;
static char s_line[240 * 4 + 32];

static void capture_flush(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    if (s_capture) {
        bool swapped = lv_display_get_color_format(disp) == LV_COLOR_FORMAT_RGB565_SWAPPED;
        int w = lv_area_get_width(a);
        const uint16_t *p = (const uint16_t *)px;
        for (int y = a->y1; y <= a->y2; y++, p += w) {
            int n = snprintf(s_line, sizeof(s_line), "PX %d %d %d ", a->x1, y, w);
            for (int x = 0; x < w && n < (int)sizeof(s_line) - 5; x++) {
                uint16_t c = swapped ? (uint16_t)((p[x] >> 8) | (p[x] << 8)) : p[x];
                n += snprintf(s_line + n, sizeof(s_line) - n, "%04x", c);
            }
            printk("%s\n", s_line);
        }
        if (lv_display_flush_is_last(disp)) {
            s_capture = false;
            k_sem_give(&s_capture_done);
        }
    }
    s_orig_flush(disp, a, px);
}

static int cmd_shot(const struct shell *sh, size_t argc, char **argv)
{
    lvgl_lock();
    lv_display_t *disp = lv_display_get_default();
    if (!s_orig_flush) {
        k_sem_init(&s_capture_done, 0, 1);
        s_orig_flush = disp->flush_cb;
        lv_display_set_flush_cb(disp, capture_flush);
    }
    k_sem_reset(&s_capture_done);
    printk("FRAME %s\n", argc > 1 ? argv[1] : "shot");
    s_capture = true;
    lv_obj_invalidate(lv_screen_active());
    lvgl_unlock();
    int r = k_sem_take(&s_capture_done, K_SECONDS(10));
    s_capture = false;
    printk("ENDFRAME\n");
    return r;
}

static int cmd_tap(const struct shell *sh, size_t argc, char **argv)
{
    int x = atoi(argv[1]), y = atoi(argv[2]);
    int ms = argc > 3 ? atoi(argv[3]) : 80;
    touch_fake(true, true, x, y);
    k_msleep(ms);
    touch_fake(true, false, x, y);
    k_msleep(60);
    touch_fake(false, false, x, y);
    return 0;
}

static int cmd_swipe(const struct shell *sh, size_t argc, char **argv)
{
    int x1 = atoi(argv[1]), y1 = atoi(argv[2]), x2 = atoi(argv[3]), y2 = atoi(argv[4]);
    const int steps = 12;
    for (int i = 0; i <= steps; i++) {
        touch_fake(true, true, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
        k_msleep(20);
    }
    touch_fake(true, false, x2, y2);
    k_msleep(60);
    touch_fake(false, false, x2, y2);
    return 0;
}

static int cmd_btn(const struct shell *sh, size_t argc, char **argv)
{
    button_id_t id = strcmp(argv[1], "boot") == 0 ? BUTTON_BOOT : BUTTON_PWR;
    const char *e = argv[2];
    button_event_t ev = !strcmp(e, "double") ? BUTTON_DOUBLE : !strcmp(e, "hold") ? BUTTON_HOLD :
                        !strcmp(e, "long") ? BUTTON_LONG_HOLD : BUTTON_CLICK;
    on_button(id, ev);
    return 0;
}

static int cmd_heap(const struct shell *sh, size_t argc, char **argv)
{
    extern struct k_heap _system_heap;
    struct sys_memory_stats st;
    sys_heap_runtime_stats_get(&_system_heap.heap, &st);
    shell_print(sh, "heap free: %u", (unsigned)heap_caps_get_free_size(0));
    shell_print(sh, "kernel heap: used %u, free %u, peak %u", (unsigned)st.allocated_bytes,
                (unsigned)st.free_bytes, (unsigned)st.max_allocated_bytes);
    return 0;
}

static int cmd_beep(const struct shell *sh, size_t argc, char **argv)
{
    audio_beep(argc > 1 ? atoi(argv[1]) : 2000, 150, 100, 2);
    k_msleep(700);
    shell_print(sh, "playing=%d", audio_is_playing());
    return 0;
}

void audio_debug(char *buf, size_t len);
static int cmd_audio(const struct shell *sh, size_t argc, char **argv)
{
    char b[128];
    audio_debug(b, sizeof(b));
    shell_print(sh, "%s", b);
    return 0;
}

static int cmd_mic(const struct shell *sh, size_t argc, char **argv)
{
    int r = audio_mic_start(NULL);
    if (r) { shell_error(sh, "mic start: %d", r); return r; }
    if (argc > 1) audio_beep(atoi(argv[1]), 1000, 0, 1);   /* listen to our own beep */
    for (int i = 0; i < 5; i++) {
        k_msleep(250);
        shell_print(sh, "rms %.1f dBFS  peak %.1f dBFS  dropped %u",
                    (double)audio_mic_rms_dbfs(), (double)audio_mic_peak_dbfs(), audio_mic_dropped());
    }
    audio_mic_stop();
    return 0;
}

static int cmd_wifi(const struct shell *sh, size_t argc, char **argv)
{
    static const char *const cs[] = { "disconnected", "connecting", "connected", "failed" };
    static const char *const ts[] = { "disabled", "idle", "running", "ok", "failed" };
    if (argc >= 2 && !strcmp(argv[1], "scan")) {
        uint32_t gen = wifi_mgr_scan_generation();
        wifi_mgr_scan();
        for (int i = 0; i < 150 && wifi_mgr_scan_generation() == gen; i++) k_msleep(100);
        wifi_ap_info_t aps[WIFI_MGR_MAX_APS];
        int n = wifi_mgr_get_scan_results(aps, WIFI_MGR_MAX_APS);
        for (int i = 0; i < n; i++)
            shell_print(sh, "%4d %s %s", aps[i].rssi, aps[i].secure ? "*" : " ", aps[i].ssid);
        shell_print(sh, "%d networks", n);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "connect")) {
        wifi_mgr_connect(argv[2], argc >= 4 ? argv[3] : "");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "off"))  { wifi_mgr_set_enabled(false); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "on"))   { wifi_mgr_set_enabled(true); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "sync")) { time_sync_request(); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "disconnect")) { wifi_mgr_disconnect(); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "forget")) { wifi_mgr_forget(); return 0; }
    char ssid[33], ip[20];
    wifi_mgr_get_ssid(ssid, sizeof(ssid));
    wifi_mgr_get_ip(ip, sizeof(ip));
    shell_print(sh, "enabled=%d state=%s ssid=\"%s\" ip=%s rssi=%d keep=%d sync=%s fail=\"%s\"",
                wifi_mgr_is_enabled(), cs[wifi_mgr_get_state()], ssid, ip, wifi_mgr_get_rssi(),
                wifi_mgr_stays_connected(), ts[time_sync_get_status()],
                wifi_mgr_get_fail_reason());
    return 0;
}

/* "ota self": write the running image to the update slot through the same
 * esp_ota_* calls the web upload uses, then reboot into it (test swap). */
static int cmd_ota(const struct shell *sh, size_t argc, char **argv)
{
    esp_ota_img_states_t st;
    esp_ota_get_state_partition(esp_ota_get_running_partition(), &st);
    shell_print(sh, "version %s, running %s%s", ota_version(),
                esp_ota_get_running_partition()->label,
                st == ESP_OTA_IMG_PENDING_VERIFY ? " (not confirmed yet)" : "");
    shell_print(sh, "mcuboot swap type %d, confirmed %d", mcuboot_swap_type(),
                boot_is_img_confirmed());
    if (argc >= 2 && !strcmp(argv[1], "trailer")) {
        for (int slot = 0; slot < 2; slot++) {
            const struct flash_area *fa;
            if (flash_area_open(slot ? FIXED_PARTITION_ID(slot1_partition)
                                     : FIXED_PARTITION_ID(slot0_partition), &fa)) continue;
            uint8_t t[48];
            flash_area_read(fa, fa->fa_size - sizeof(t), t, sizeof(t));
            uint32_t m;
            flash_area_read(fa, 0, &m, 4);
            shell_print(sh, "slot%d off 0x%lx size 0x%x hdr %08x", slot, (long)fa->fa_off,
                        (unsigned)fa->fa_size, m);
            shell_hexdump(sh, t, sizeof(t));
            flash_area_close(fa);
        }
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "self")) return 0;

    const struct flash_area *fa;
    if (flash_area_open(FIXED_PARTITION_ID(slot0_partition), &fa)) return -EIO;
    uint32_t hdr[8];
    flash_area_read(fa, 0, hdr, sizeof(hdr));
    uint32_t hdr_size = hdr[2] & 0xffff, img_size = hdr[3];
    uint16_t tlv[2];
    flash_area_read(fa, hdr_size + img_size, tlv, sizeof(tlv));
    uint32_t total = hdr_size + img_size;
    if (tlv[0] == 0x6908) {                 /* protected TLVs come first */
        total += tlv[1];
        flash_area_read(fa, total, tlv, sizeof(tlv));
    }
    if (tlv[0] == 0x6907) total += tlv[1];
    shell_print(sh, "image: magic %08x, header %u, size %u, total %u",
                hdr[0], hdr_size, img_size, total);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (!part || esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        flash_area_close(fa);
        shell_error(sh, "begin failed");
        return -EIO;
    }
    static uint8_t buf[2048];
    for (uint32_t off = 0; off < total; off += sizeof(buf)) {
        uint32_t n = total - off < sizeof(buf) ? total - off : sizeof(buf);
        flash_area_read(fa, off, buf, n);
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            esp_ota_abort(h);
            flash_area_close(fa);
            shell_error(sh, "write failed at %u", off);
            return -EIO;
        }
    }
    flash_area_close(fa);
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        shell_error(sh, "finish failed");
        return -EIO;
    }
    shell_print(sh, "written; swap type now %d", mcuboot_swap_type());
    if (argc >= 3) return 0;               /* "ota self noreboot" */
    k_msleep(200);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}

#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dummy.h>
#include "app/face_server.h"

static void lo_up(struct net_if *iface, void *user)
{
    if (net_if_l2(iface) != &NET_L2_GET_NAME(DUMMY)) return;
    struct net_in_addr lo = { .s4_addr = { 127, 0, 0, 1 } };
    if (!net_if_ipv4_addr_lookup(&lo, NULL)) net_if_ipv4_addr_add(iface, &lo, NET_ADDR_MANUAL, 0);
}

/* "http METHOD PATH [BODY]": request to the watch's own web server over
 * loopback.  ' in BODY becomes " (the shell eats double quotes). */
static int cmd_http(const struct shell *sh, size_t argc, char **argv)
{
    net_if_foreach(lo_up, NULL);
    if (!face_server_is_running()) face_server_start();
    int fd = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
    struct net_sockaddr_in a = { .sin_family = NET_AF_INET, .sin_port = net_htons(80) };
    zsock_inet_pton(NET_AF_INET, "127.0.0.1", &a.sin_addr);
    struct zsock_timeval tv = { .tv_sec = 10 };
    zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
    if (zsock_connect(fd, (struct net_sockaddr *)&a, sizeof(a)) < 0) {
        shell_error(sh, "connect failed %d", errno);
        zsock_close(fd);
        return -EIO;
    }
    char *body = argc > 3 ? argv[3] : "";
    for (char *p = body; *p; p++) if (*p == '\'') *p = '"';
    char req[200];
    int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: watch\r\nContent-Length: %d\r\n\r\n",
                     argv[1], argv[2], (int)strlen(body));
    if (n >= (int)sizeof(req)) n = sizeof(req) - 1;
    zsock_send(fd, req, n, 0);
    if (body[0]) zsock_send(fd, body, strlen(body), 0);
    static char buf[513];
    int total = 0;
    while ((n = zsock_recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = 0;
        if (total < 1500) shell_fprintf(sh, SHELL_NORMAL, "%s", buf);
        total += n;
    }
    shell_print(sh, "\n[%d bytes]", total);
    zsock_close(fd);
    return 0;
}

static int cmd_ble(const struct shell *sh, size_t argc, char **argv)
{
    static const char *const st[] = { "off", "starting", "advertising", "connected" };
    extern void ble_mgr_test_no_media(bool on);
    if (argc >= 2 && !strcmp(argv[1], "watch")) {
        bool nomedia = argc >= 3 && !strcmp(argv[2], "nomedia");
        ble_mgr_set(false, BLE_MODE_WATCH);
        k_msleep(500);
        ble_mgr_test_no_media(nomedia);
        ble_mgr_set(true, BLE_MODE_WATCH);
    }
    else if (argc >= 2 && !strcmp(argv[1], "hid")) ble_mgr_set(true, BLE_MODE_HID);
    else if (argc >= 2 && !strcmp(argv[1], "off")) ble_mgr_set(false, ble_mgr_get_mode());
    else if (argc >= 2 && !strcmp(argv[1], "forget")) ble_mgr_forget_all();
    else if (argc >= 3 && !strcmp(argv[1], "name")) ble_mgr_set_name(argv[2]);
    if (argc >= 2) k_msleep(1500);
    char a[20], p[20];
    ble_mgr_get_addr(ble_mgr_get_mode(), a, sizeof(a));
    ble_mgr_get_peer(p, sizeof(p));
    shell_print(sh, "enabled=%d mode=%s state=%s name=\"%s\" addr=%s peer=%s secure=%d "
                "media_ready=%d hid_ready=%d",
                ble_mgr_is_enabled(), ble_mgr_get_mode() == BLE_MODE_HID ? "hid" : "watch",
                st[ble_mgr_get_state()], ble_mgr_get_name(), a, p, ble_mgr_is_secure(),
                ble_media_ready(), ble_hid_ready());
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(watch_cmds,
    SHELL_CMD_ARG(ble, NULL, "ble [watch [nomedia]|hid|off|forget|name NAME]", cmd_ble, 1, 2),
    SHELL_CMD_ARG(http, NULL, "http METHOD PATH [BODY] (loopback test)", cmd_http, 3, 1),
    SHELL_CMD_ARG(ota, NULL, "ota [self [noreboot]|trailer]: OTA state / self-update test", cmd_ota, 1, 2),
    SHELL_CMD_ARG(wifi, NULL, "wifi [scan|connect SSID [PASS]|on|off|sync|disconnect|forget]",
                  cmd_wifi, 1, 3),
    SHELL_CMD_ARG(beep, NULL, "Beep [Hz]", cmd_beep, 1, 1),
    SHELL_CMD_ARG(audio, NULL, "Audio state", cmd_audio, 1, 0),
    SHELL_CMD_ARG(mic, NULL, "Mic level [beep Hz]", cmd_mic, 1, 1),
    SHELL_CMD_ARG(shot, NULL, "Screenshot as hex lines", cmd_shot, 1, 1),
    SHELL_CMD_ARG(tap, NULL, "Tap X Y [ms]", cmd_tap, 3, 1),
    SHELL_CMD_ARG(swipe, NULL, "Swipe X1 Y1 X2 Y2", cmd_swipe, 5, 0),
    SHELL_CMD_ARG(btn, NULL, "pwr|boot click|double|hold|long", cmd_btn, 3, 0),
    SHELL_CMD_ARG(heap, NULL, "Free heap", cmd_heap, 1, 0),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(watch, &watch_cmds, "Smartwatch test commands", NULL);

#endif /* CONFIG_SHELL */
