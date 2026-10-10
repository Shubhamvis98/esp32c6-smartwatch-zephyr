/**
 * @file app/ble_mgr.c
 * @brief Bluetooth LE peripheral: stack lifecycle, GATT services for both gadget
 *        modes, advertising, pairing and data paths.
 *
 *   Watch mode GATT                    HID mode GATT
 *   ├ GAP / GATT                        ├ GAP / GATT
 *   ├ Battery (0x180F)                  ├ Battery (0x180F)
 *   └ Nordic UART 6E400001-…            └ HID (0x1812)
 *       ├ RX 6E400002 (phone writes)        ├ Protocol Mode, HID Info, Ctrl Pt
 *       └ TX 6E400003 (we notify)           ├ Report Map
 *                                           ├ Input  report 1: keyboard
 *                                           ├ Output report 1: keyboard LEDs
 *                                           ├ Input  report 2: mouse
 *                                           └ Input  report 3: media keys
 */

#include "app/ble_mgr.h"
#include "app/gadgetbridge.h"
#include "app/settings.h"
#include "hal/battery.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/bas.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Zephyr port: the same behaviour on the Zephyr Bluetooth host.  Each mode
 * registers its own GATT services (dynamic database) and advertises from its
 * own identity: watch mode from the chip's public address, HID mode from a
 * static random address derived from it, so hosts see two devices and keep
 * separate bonds. */

static const char *TAG = "ble";

#define APPEARANCE_WATCH  0x00C0             // Generic Watch
#define APPEARANCE_HID    0x03C1             // HID Keyboard

#define WORKER_PERIOD_MS  30000              // battery level refresh

#define BIT_APPLY   (1 << 0)
#define BIT_FORGET  (1 << 1)
#define BIT_RENAME  (1 << 2)
#define BIT_ADV     (1 << 3)                 // restart advertising
#define BIT_RX      (1 << 4)                 // Gadgetbridge data waiting

/* Gadgetbridge data is handled on the worker, not in the Bluetooth thread
 * that received it: parsing it can mean JSON, file and RTC work, too much
 * for the Bluetooth threads' small stacks. */
RING_BUF_DECLARE(s_rx_ring, 2048);
static struct k_spinlock s_rx_lock;

/* Desired configuration (set from any task, applied by the worker) */
static volatile bool       s_want_enabled;
static volatile ble_mode_t s_want_mode;

/* Running state */
static volatile ble_state_t s_state = BLE_STATE_OFF;
static volatile ble_mode_t  s_mode = BLE_MODE_WATCH;
static volatile bool        s_running;
static volatile bool        s_enabled_stack;   // bt_enable() done
static volatile bool        s_secure;
static struct bt_conn      *s_conn;
static bt_addr_le_t         s_peer;
static int                  s_hid_id = -1;     // identity used in HID mode
static TaskHandle_t         s_worker;
static bool                 s_no_media;        // test: leave out the media keys
static SemaphoreHandle_t    s_tx_lock;         // serialises notifications

static uint8_t current_id(void) { return s_mode == BLE_MODE_HID ? (uint8_t)s_hid_id : BT_ID_DEFAULT; }

/* ── UUIDs ───────────────────────────────────────────────────────────────── */
#define NUS_UUID_VAL(x) BT_UUID_128_ENCODE(0x6E400000 | (x), 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)
static const struct bt_uuid_128 s_nus_svc = BT_UUID_INIT_128(NUS_UUID_VAL(1));
static const struct bt_uuid_128 s_nus_rx  = BT_UUID_INIT_128(NUS_UUID_VAL(2));
static const struct bt_uuid_128 s_nus_tx  = BT_UUID_INIT_128(NUS_UUID_VAL(3));

static volatile bool s_nus_subscribed;

/* ── HID data ────────────────────────────────────────────────────────────── */
#define RPT_ID_KEYBOARD   1
#define RPT_ID_MOUSE      2
#define RPT_ID_CONSUMER   3

static const uint8_t s_report_map[] = {
    /* Keyboard: modifiers, reserved, 6 keys; 5 LED outputs */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, RPT_ID_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    /* Mouse: 3 buttons, X, Y, wheel */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, RPT_ID_MOUSE,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0,
    0xC0,
    /* Consumer control: one 16-bit usage */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, RPT_ID_CONSUMER,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

/* Watch mode: only the media keys.  A keyboard here would make Android
 * treat the watch as a physical keyboard and hide its on-screen one. */
static const uint8_t s_report_map_watch[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, RPT_ID_CONSUMER,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

static const uint8_t s_hid_info[4] = { 0x11, 0x01, 0x00, 0x02 };  // HID 1.11, normally connectable
static uint8_t s_protocol_mode = 1;                               // report protocol

static uint8_t s_kbd_report[8];
static uint8_t s_kbd_leds;
static uint8_t s_mouse_report[4];
static uint8_t s_consumer_report[2];

/* Report Reference descriptor values: { report id, type (1=input, 2=output) } */
static const uint8_t s_ref_kbd_in[2]  = { RPT_ID_KEYBOARD, 1 };
static const uint8_t s_ref_kbd_out[2] = { RPT_ID_KEYBOARD, 2 };
static const uint8_t s_ref_mouse[2]   = { RPT_ID_MOUSE, 1 };
static const uint8_t s_ref_cons[2]    = { RPT_ID_CONSUMER, 1 };

static volatile bool s_kbd_sub, s_mouse_sub, s_cons_sub;

/* ── GATT callbacks ──────────────────────────────────────────────────────── */
static ssize_t read_static(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    const uint8_t *data = attr->user_data;
    size_t size = 0;
    if (data == s_hid_info)               size = sizeof(s_hid_info);
    else if (data == &s_protocol_mode)    size = 1;
    else if (data == s_kbd_report)        size = sizeof(s_kbd_report);
    else if (data == &s_kbd_leds)         size = 1;
    else if (data == s_mouse_report)      size = sizeof(s_mouse_report);
    else if (data == s_consumer_report)   size = sizeof(s_consumer_report);
    else                                  size = 2;               // report references
    return bt_gatt_attr_read(conn, attr, buf, len, offset, data, size);
}

static ssize_t read_map(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    if (s_mode == BLE_MODE_WATCH) {
        return bt_gatt_attr_read(conn, attr, buf, len, offset, s_report_map_watch,
                                 sizeof(s_report_map_watch));
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, s_report_map, sizeof(s_report_map));
}

static ssize_t write_byte(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (offset != 0 || len != 1) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    *(uint8_t *)attr->user_data = *(const uint8_t *)buf;     // protocol mode / LEDs
    return len;
}

static ssize_t write_ignore(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    return len;                                               // HID control point
}

static ssize_t nus_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    k_spinlock_key_t key = k_spin_lock(&s_rx_lock);
    uint32_t put = ring_buf_put(&s_rx_ring, buf, len);
    k_spin_unlock(&s_rx_lock, key);
    if (put < len) ESP_LOGW(TAG, "Gadgetbridge data dropped (%u bytes)", (unsigned)(len - put));
    if (s_worker) xTaskNotify(s_worker, BIT_RX, eSetBits);
    return len;
}

static void rx_drain(void)
{
    uint8_t buf[128];
    for (;;) {
        k_spinlock_key_t key = k_spin_lock(&s_rx_lock);
        uint32_t n = ring_buf_get(&s_rx_ring, buf, sizeof(buf));
        k_spin_unlock(&s_rx_lock, key);
        if (!n) break;
        gb_on_rx(buf, n);
    }
}

static void nus_ccc(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool on = value & BT_GATT_CCC_NOTIFY;
    ESP_LOGI(TAG, "Gadgetbridge channel notify %d", on);
    s_nus_subscribed = on;
    if (on) gb_on_ready();
}
static void kbd_ccc(const struct bt_gatt_attr *attr, uint16_t value)   { s_kbd_sub = value & BT_GATT_CCC_NOTIFY; }
static void mouse_ccc(const struct bt_gatt_attr *attr, uint16_t value) { s_mouse_sub = value & BT_GATT_CCC_NOTIFY; }
static void cons_ccc(const struct bt_gatt_attr *attr, uint16_t value)  { s_cons_sub = value & BT_GATT_CCC_NOTIFY; }

/* ── GATT tables ─────────────────────────────────────────────────────────── */
#define HID_RD   (BT_GATT_PERM_READ_ENCRYPT)
#define HID_RW   (BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)
#define REPORT_REF(ref) \
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, HID_RD, read_static, NULL, (void *)(ref))

/* HID mode: keyboard, mouse and media keys */
static struct bt_gatt_attr s_hid_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           HID_RW, read_static, write_byte, &s_protocol_mode),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           HID_RD, read_static, NULL, s_kbd_report),
    BT_GATT_CCC(kbd_ccc, HID_RW),
    REPORT_REF(s_ref_kbd_in),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           HID_RW, read_static, write_byte, &s_kbd_leds),
    REPORT_REF(s_ref_kbd_out),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           HID_RD, read_static, NULL, s_mouse_report),
    BT_GATT_CCC(mouse_ccc, HID_RW),
    REPORT_REF(s_ref_mouse),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           HID_RD, read_static, NULL, s_consumer_report),
    BT_GATT_CCC(cons_ccc, HID_RW),
    REPORT_REF(s_ref_cons),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, HID_RD, read_map, NULL, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, HID_RD, read_static, NULL,
                           (void *)s_hid_info),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_ignore, NULL),
};
#define HID_KBD_ATTR    (&s_hid_attrs[3])
#define HID_MOUSE_ATTR  (&s_hid_attrs[10])
#define HID_CONS_ATTR   (&s_hid_attrs[14])

/* Watch mode: Nordic UART for Gadgetbridge */
static struct bt_gatt_attr s_nus_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(&s_nus_svc),
    BT_GATT_CHARACTERISTIC(&s_nus_rx.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, nus_write, NULL),
    BT_GATT_CHARACTERISTIC(&s_nus_tx.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(nus_ccc, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};
#define NUS_TX_ATTR (&s_nus_attrs[3])

/* …and media keys on the same connection: the camera shutter (volume up).
 * A keyboard here would make Android treat the watch as a physical keyboard
 * and hide its on-screen one, so only the consumer report. */
static struct bt_gatt_attr s_media_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           HID_RW, read_static, write_byte, &s_protocol_mode),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           HID_RD, read_static, NULL, s_consumer_report),
    BT_GATT_CCC(cons_ccc, HID_RW),
    REPORT_REF(s_ref_cons),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, HID_RD, read_map, NULL, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, HID_RD, read_static, NULL,
                           (void *)s_hid_info),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_ignore, NULL),
};
#define MEDIA_CONS_ATTR (&s_media_attrs[3])

static struct bt_gatt_service s_hid_svc   = BT_GATT_SERVICE(s_hid_attrs);
static struct bt_gatt_service s_nus_gatt  = BT_GATT_SERVICE(s_nus_attrs);
static struct bt_gatt_service s_media_svc = BT_GATT_SERVICE(s_media_attrs);

static const struct bt_gatt_attr *cons_attr(void)
{
    return s_mode == BLE_MODE_HID ? HID_CONS_ATTR : MEDIA_CONS_ATTR;
}

/* ── Advertising ─────────────────────────────────────────────────────────── */
static void advertise(void)
{
    const char *name = ble_mgr_get_name();
    static const uint8_t mfg[] = { 0xFF, 0xFF, 'C', '6', 'W' };
    static const uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
    static const uint8_t nus_uuid[] = { NUS_UUID_VAL(1) };
    static const uint8_t hid_uuid[] = { BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL) };
    static const uint8_t appearance[] = { BT_BYTES_LIST_LE16(APPEARANCE_HID) };

    struct bt_data ad[4];
    struct bt_data sd[2];
    size_t n_ad = 0, n_sd = 0;

    ad[n_ad++] = (struct bt_data)BT_DATA(BT_DATA_FLAGS, &flags, 1);
    if (s_mode == BLE_MODE_WATCH) {
        /* 128-bit UUID fills most of the 31 bytes: name goes in the scan
         * response.  Gadgetbridge filters on this UUID. */
        ad[n_ad++] = (struct bt_data)BT_DATA(BT_DATA_UUID128_ALL, nus_uuid, sizeof(nus_uuid));
        sd[n_sd++] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name));
        /* Company 0xFFFF (testing) + "C6W": lets Gadgetbridge recognise the
         * watch whatever it is named.  Name (≤ 21) + this (7) fits in 31. */
        sd[n_sd++] = (struct bt_data)BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg, sizeof(mfg));
    } else {
        ad[n_ad++] = (struct bt_data)BT_DATA(BT_DATA_UUID16_ALL, hid_uuid, sizeof(hid_uuid));
        ad[n_ad++] = (struct bt_data)BT_DATA(BT_DATA_GAP_APPEARANCE, appearance, sizeof(appearance));
        ad[n_ad++] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name));
    }

    struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN,
                                                        BT_GAP_ADV_FAST_INT_MIN_2,
                                                        BT_GAP_ADV_FAST_INT_MAX_2, NULL);
    param.id = current_id();
    int rc = bt_le_adv_start(&param, ad, n_ad, n_sd ? sd : NULL, n_sd);
    if (rc != 0 && rc != -EALREADY) {
        ESP_LOGE(TAG, "Advertising start failed: %d", rc);
        return;
    }
    s_state = BLE_STATE_ADVERTISING;
}

static void reset_link_state(void)
{
    s_secure = false;
    s_nus_subscribed = false;
    s_kbd_sub = s_mouse_sub = s_cons_sub = false;
    memset(&s_peer, 0, sizeof(s_peer));
}

static const char *disconnect_reason(uint8_t reason)
{
    switch (reason) {
    case BT_HCI_ERR_REMOTE_USER_TERM_CONN: return "closed by the other device";
    case BT_HCI_ERR_LOCALHOST_TERM_CONN:   return "closed by the watch";
    case BT_HCI_ERR_CONN_TIMEOUT:          return "link timeout";
    case BT_HCI_ERR_PIN_OR_KEY_MISSING:    return "pairing key missing";
    case BT_HCI_ERR_AUTH_FAIL:             return "authentication failed";
    case BT_HCI_ERR_CONN_FAIL_TO_ESTAB:    return "connection failed to establish";
    default:                               return "other";
    }
}

/* ── Connection callbacks ────────────────────────────────────────────────── */
static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        if (s_worker) xTaskNotify(s_worker, BIT_ADV, eSetBits);
        return;
    }
    if (s_conn) {                          // only one link at a time
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }
    s_conn = bt_conn_ref(conn);
    s_peer = *bt_conn_get_dst(conn);

    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0) {
        const uint8_t *a = s_peer.a.val;
        ESP_LOGI(TAG, "Connected to %02X:%02X:%02X:%02X:%02X:%02X (type %d), interval %u, "
                 "timeout %u, bonded %d", a[5], a[4], a[3], a[2], a[1], a[0], s_peer.type,
                 info.le.interval_us / 1250, info.le.timeout,
                 bt_le_bond_exists(info.id, &s_peer));
    }
    s_state = BLE_STATE_CONNECTED;
    if (s_mode == BLE_MODE_HID) {
        bt_conn_set_security(conn, BT_SECURITY_L2);  // HID hosts expect a bonded link
    } else {
        /* A phone we're bonded with: encrypt straight away so the media keys
         * work.  A new phone pairs when the camera asks for it. */
        if (bt_le_bond_exists(BT_ID_DEFAULT, &s_peer)) bt_conn_set_security(conn, BT_SECURITY_L2);
        gb_on_connect();
    }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (conn != s_conn) return;
    ring_buf_reset(&s_rx_ring);
    ESP_LOGI(TAG, "Disconnected: %s (0x%x)", disconnect_reason(reason), reason);
    bt_conn_unref(s_conn);
    s_conn = NULL;
    reset_link_state();
    if (s_mode == BLE_MODE_WATCH) gb_on_disconnect();
}

/* The connection object is free again: advertising can restart */
static void on_recycled(void)
{
    if (s_running && !s_conn && s_worker) xTaskNotify(s_worker, BIT_ADV, eSetBits);
}

static void on_security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    if (!err && level >= BT_SECURITY_L2) {
        s_secure = true;
        ESP_LOGI(TAG, "Link encrypted");
    } else if (err) {
        ESP_LOGW(TAG, "Encryption failed, error %d", err);
    }
}

static void on_le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                                uint16_t timeout)
{
    ESP_LOGI(TAG, "Connection params: interval %u, latency %u, timeout %u", interval, latency, timeout);
}

BT_CONN_CB_DEFINE(s_conn_cb) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .recycled = on_recycled,
    .security_changed = on_security_changed,
    .le_param_updated = on_le_param_updated,
};

static void on_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
    ESP_LOGI(TAG, "MTU %u", tx);
}

static struct bt_gatt_cb s_gatt_cb = { .att_mtu_updated = on_mtu_updated };

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    ESP_LOGI(TAG, "Paired%s", bonded ? " and bonded" : "");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    ESP_LOGW(TAG, "Pairing failed: %d", reason);
}

static struct bt_conn_auth_info_cb s_auth_info = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

/* ── Stack lifecycle ─────────────────────────────────────────────────────── */
/* Our address in a mode, least significant byte first.  Watch mode uses the
 * chip's public Bluetooth MAC.  HID mode uses a static random address derived
 * from it (top two bits set as the spec requires), so hosts see a separate
 * device. */
static void own_addr(ble_mode_t mode, uint8_t out[6])
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);           // most significant byte first
    for (int i = 0; i < 6; i++) out[i] = mac[5 - i];
    if (mode == BLE_MODE_HID) {
        out[0] ^= 0x5A;
        out[5] |= 0xC0;
    }
}

/* Find or create the HID identity */
static void ensure_hid_identity(void)
{
    bt_addr_le_t want = { .type = BT_ADDR_LE_RANDOM };
    own_addr(BLE_MODE_HID, want.a.val);

    bt_addr_le_t ids[CONFIG_BT_ID_MAX];
    size_t count = ARRAY_SIZE(ids);
    bt_id_get(ids, &count);
    for (size_t i = 0; i < count; i++) {
        if (bt_addr_le_eq(&ids[i], &want)) {
            s_hid_id = i;
            return;
        }
    }
    int id = bt_id_create(&want, NULL);
    if (id < 0) ESP_LOGE(TAG, "HID identity failed: %d", id);
    s_hid_id = id < 0 ? BT_ID_DEFAULT : id;
}

static bool stack_enable(void)
{
    if (s_enabled_stack) return true;
    int rc = bt_enable(NULL);
    if (rc && rc != -EALREADY) {
        ESP_LOGE(TAG, "bt_enable failed: %d", rc);
        return false;
    }
    settings_load_subtree("bt");
    ensure_hid_identity();
    s_enabled_stack = true;
    return true;
}

static void stack_start(ble_mode_t mode)
{
    s_mode = mode;
    s_state = BLE_STATE_STARTING;
    reset_link_state();

    if (!stack_enable()) {
        s_state = BLE_STATE_OFF;
        return;
    }

    int rc = 0;
    if (mode == BLE_MODE_WATCH) {
        rc = bt_gatt_service_register(&s_nus_gatt);
        if (!rc && !s_no_media) rc = bt_gatt_service_register(&s_media_svc);
    } else {
        rc = bt_gatt_service_register(&s_hid_svc);
    }
    if (rc) ESP_LOGE(TAG, "GATT service registration failed: %d", rc);

    bt_set_name(ble_mgr_get_name());
    bt_set_appearance(mode == BLE_MODE_WATCH ? APPEARANCE_WATCH : APPEARANCE_HID);
    bt_bas_set_battery_level(battery_percent_from_mv(battery_read_mv()));

    s_running = true;
    advertise();
    ESP_LOGI(TAG, "Started in %s mode", mode == BLE_MODE_WATCH ? "smartwatch" : "HID");
}

static void stack_stop(void)
{
    if (!s_running) return;
    s_running = false;
    bt_le_adv_stop();
    if (s_conn) {
        bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        for (int i = 0; i < 20 && s_conn; i++) vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_mode == BLE_MODE_WATCH) {
        gb_on_disconnect();
        if (!s_no_media) bt_gatt_service_unregister(&s_media_svc);
        bt_gatt_service_unregister(&s_nus_gatt);
    } else {
        bt_gatt_service_unregister(&s_hid_svc);
    }
    /* Power the controller down too */
    if (bt_disable() == 0) s_enabled_stack = false;
    reset_link_state();
    s_state = BLE_STATE_OFF;
    ESP_LOGI(TAG, "Stopped");
}

static void worker_task(void *arg)
{
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(WORKER_PERIOD_MS));

        if (bits & BIT_FORGET) {
            if (stack_enable()) {
                bt_unpair(BT_ID_DEFAULT, NULL);
                if (s_hid_id > 0) bt_unpair(s_hid_id, NULL);
                ESP_LOGI(TAG, "All bonds deleted");
            }
            stack_stop();                           // drops any connected peer
            if (!s_running && s_enabled_stack && bt_disable() == 0) s_enabled_stack = false;
            bits |= BIT_APPLY;                      // restart if Bluetooth is on
        }

        if ((bits & BIT_RENAME) && s_running) {
            bt_set_name(ble_mgr_get_name());
            if (!s_conn) {
                bt_le_adv_stop();
                advertise();
            }
        }

        if (bits & BIT_APPLY) {
            bool want = s_want_enabled;
            ble_mode_t mode = s_want_mode;
            if (s_running && (!want || mode != s_mode)) stack_stop();
            if (want && !s_running) stack_start(mode);
        }

        if (bits & BIT_RX) rx_drain();

        if ((bits & BIT_ADV) && s_running && !s_conn) advertise();

        if (s_running) {
            bt_bas_set_battery_level(battery_percent_from_mv(battery_read_mv()));
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */
/* Shell test hook: watch mode without the media-key HID service, so a
 * desktop's HID plugin doesn't start pairing (takes effect on next start) */
void ble_mgr_test_no_media(bool on) { s_no_media = on; }

esp_err_t ble_mgr_start(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) return ESP_ERR_NO_MEM;

    gb_init();
    bt_gatt_cb_register(&s_gatt_cb);
    bt_conn_auth_info_cb_register(&s_auth_info);

    watch_settings_t *cfg = settings_get();
    s_want_enabled = cfg->ble_enabled;
    s_want_mode = cfg->ble_mode < BLE_MODE_COUNT ? (ble_mode_t)cfg->ble_mode : BLE_MODE_WATCH;
    s_mode = s_want_mode;

    if (xTaskCreate(worker_task, "ble_mgr", 4096, NULL, 3, &s_worker) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (s_want_enabled) xTaskNotify(s_worker, BIT_APPLY, eSetBits);
    return ESP_OK;
}

void ble_mgr_set(bool enabled, ble_mode_t mode)
{
    if (mode >= BLE_MODE_COUNT) mode = BLE_MODE_WATCH;
    s_want_enabled = enabled;
    s_want_mode = mode;
    if (!s_running) s_mode = mode;          // show the chosen mode while off

    watch_settings_t *cfg = settings_get();
    if (cfg->ble_enabled != enabled || cfg->ble_mode != mode) {
        cfg->ble_enabled = enabled;
        cfg->ble_mode = mode;
        watch_settings_save();
    }
    if (enabled && (!s_running || mode != s_mode)) s_state = BLE_STATE_STARTING;
    if (s_worker) xTaskNotify(s_worker, BIT_APPLY, eSetBits);
}

void ble_mgr_forget_all(void)
{
    if (s_worker) xTaskNotify(s_worker, BIT_FORGET, eSetBits);
}

ble_state_t ble_mgr_get_state(void) { return s_state; }
ble_mode_t  ble_mgr_get_mode(void)  { return s_mode; }
bool        ble_mgr_is_enabled(void){ return s_want_enabled; }
bool        ble_mgr_is_secure(void) { return s_secure; }

const char *ble_mgr_get_name(void)
{
    return settings_get()->ble_name;
}

bool ble_mgr_set_name(const char *name)
{
    /* Trim surrounding spaces */
    while (*name == ' ') name++;
    size_t len = strlen(name);
    while (len && name[len - 1] == ' ') len--;
    if (len == 0 || len > BLE_NAME_MAX_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        if (name[i] < 0x20 || name[i] > 0x7E) return false;
    }

    watch_settings_t *cfg = settings_get();
    if (strncmp(cfg->ble_name, name, len) == 0 && cfg->ble_name[len] == 0) return true;
    memcpy(cfg->ble_name, name, len);
    cfg->ble_name[len] = 0;
    watch_settings_save();
    ESP_LOGI(TAG, "Name set to \"%s\"", cfg->ble_name);

    if (s_worker) xTaskNotify(s_worker, BIT_RENAME, eSetBits);
    return true;
}

void ble_mgr_get_addr(ble_mode_t mode, char *buf, size_t len)
{
    uint8_t a[6];
    own_addr(mode, a);
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

void ble_mgr_get_peer(char *buf, size_t len)
{
    if (!s_conn) {
        buf[0] = 0;
        return;
    }
    const uint8_t *a = s_peer.a.val;
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* ── Notifications ───────────────────────────────────────────────────────── */
static bool notify(const struct bt_gatt_attr *attr, const void *data, uint16_t len)
{
    struct bt_conn *conn = s_conn;
    if (!conn) return false;
    for (int attempt = 0; attempt < 10; attempt++) {
        int rc = bt_gatt_notify(conn, attr, data, len);
        if (rc == 0) return true;
        if (rc != -ENOMEM && rc != -ENOBUFS) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

bool ble_uart_send_line(const char *line)
{
    if (s_mode != BLE_MODE_WATCH || !s_nus_subscribed || !s_conn) return false;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uint16_t chunk = bt_gatt_get_mtu(s_conn);
    chunk = chunk > 3 ? chunk - 3 : 20;

    /* Gadgetbridge strips the character before '\n', so end lines in "\r\n" */
    size_t n = strlen(line);
    bool ok = true;
    for (size_t off = 0; ok && off < n; off += chunk) {
        uint16_t len = (n - off) < chunk ? (uint16_t)(n - off) : chunk;
        ok = notify(NUS_TX_ATTR, line + off, len);
    }
    if (ok) ok = notify(NUS_TX_ATTR, "\r\n", 2);
    xSemaphoreGive(s_tx_lock);
    return ok;
}

bool ble_hid_ready(void)
{
    return s_mode == BLE_MODE_HID && s_secure && (s_kbd_sub || s_mouse_sub);
}

bool ble_media_ready(void)
{
    return s_secure && s_cons_sub;
}

void ble_media_pair(void)
{
    struct bt_conn *conn = s_conn;
    if (conn && !s_secure) bt_conn_set_security(conn, BT_SECURITY_L2);
}

void ble_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!s_mouse_sub) return;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_mouse_report[0] = buttons;
    s_mouse_report[1] = (uint8_t)dx;
    s_mouse_report[2] = (uint8_t)dy;
    s_mouse_report[3] = (uint8_t)wheel;
    notify(HID_MOUSE_ATTR, s_mouse_report, sizeof(s_mouse_report));
    xSemaphoreGive(s_tx_lock);
}

void ble_hid_key_tap(uint8_t modifier, uint8_t usage)
{
    if (!s_kbd_sub) return;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    memset(s_kbd_report, 0, sizeof(s_kbd_report));
    s_kbd_report[0] = modifier;
    s_kbd_report[2] = usage;
    notify(HID_KBD_ATTR, s_kbd_report, sizeof(s_kbd_report));
    memset(s_kbd_report, 0, sizeof(s_kbd_report));
    notify(HID_KBD_ATTR, s_kbd_report, sizeof(s_kbd_report));
    xSemaphoreGive(s_tx_lock);
}

void ble_hid_consumer_tap(uint16_t usage)
{
    if (!s_cons_sub) return;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_consumer_report[0] = usage & 0xFF;
    s_consumer_report[1] = usage >> 8;
    notify(cons_attr(), s_consumer_report, sizeof(s_consumer_report));
    s_consumer_report[0] = s_consumer_report[1] = 0;
    notify(cons_attr(), s_consumer_report, sizeof(s_consumer_report));
    xSemaphoreGive(s_tx_lock);
}
