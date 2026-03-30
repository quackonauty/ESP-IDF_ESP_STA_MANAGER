# ESP STA Manager

| Supported Targets | ESP32 | ESP32-S2 | ESP32-S3 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | --------- | -------- |

![Version](https://img.shields.io/badge/version-1.0.0-blue)

An ESP-IDF component that handles WiFi provisioning (BLE or SoftAP) and the full connection lifecycle — including automatic reconnection and event-driven state callbacks.

## Requirements

- **ESP-IDF**: v5.0+
- **Flash**: 4 MB minimum
- **RAM**: ~93 KB peak (SoftAP provisioning) / ~76 KB steady state

---

## How It Works

```
── SETUP ───────────────────────────────────────────────────────

nvs_flash_init()
    │
sta_manager_init(&config)        ← service_name + sec1_pop or sec2_salt/verifier + event_cb
    │ validates credentials, registers event handlers
    │ inits netif, WiFi stack + prov_mgr

── PROVISIONING ────────────────────────────────────────────────

sta_manager_start()
    │
    ├─ Not provisioned ───────► BLE/SoftAP provisioning
    │                           │ s_prov_active = true (STA events blocked)
    │                           │ waiting for provisioning app...
    │                           │
    │                           WIFI_PROV_CRED_RECV
    │                           WIFI_PROV_CRED_SUCCESS  ← s_prov_cred_ok = true
    │                           │                         AP_STADISCONNECTED suppressed
    │                           WIFI_PROV_END
    │                           │ deinit prov_mgr
    │                           │ s_prov_active = false
    │                           │ register WIFI_EVENT (BLE only — lazy)
    │                           │ wifi_init_sta()
    │
    └─ Already provisioned ───► deinit prov_mgr
                                register WIFI_EVENT (BLE only — lazy)
                                wifi_init_sta()

── CONNECTION ──────────────────────────────────────────────────

                                IP_EVENT_STA_GOT_IP
                                │ STA_CONNECTED_BIT set
                                │ → STA_MGR_EVENT_STA_GOT_IP dispatched
```

**Key design decisions:**

- Credential blobs are owned by the caller — the component copies the config struct but never deep-copies the pointed-to data, saving ~400 bytes BSS.
- `service_name` and `append_mac_suffix` are runtime config — no Kconfig string fields needed for naming.
- `WIFI_EVENT` registration strategy differs by transport: SoftAP registers at `init()` (needed immediately for AP events); BLE registers lazily after provisioning ends to avoid interfering with `prov_mgr`'s internal STA handling.
- `s_prov_active` flag gates all STA events during provisioning — prevents spurious `esp_wifi_connect()` calls while the AP is active.
- `s_prov_cred_ok` flag suppresses `AP_STADISCONNECTED` after credentials are accepted — the phone disconnects from the SoftAP as part of the normal flow before `WIFI_PROV_END` fires.
- `STA_CONNECTED_BIT` is never cleared on `wait_connected()` exit, keeping `sta_manager_is_connected()` accurate after the wait returns.

---

## Resource Footprint

Measured on **ESP32 rev 3.1** (160 MHz dual-core), ESP-IDF v5.5.3, Security 2 (SRP-6a), SoftAP transport. Baseline is a minimal ESP-IDF application with no components added.

### Flash & Static Memory — SoftAP Transport

| Region | Size |
|---|---|
| Flash Code (`.text`) | 656,986 B |
| Flash Data (`.rodata` + `.appdesc`) | 151,876 B |
| **Total binary** | **~937 KB** |
| IRAM used | 105,507 B (**80.5%**) — 25,565 B remaining |
| DRAM (`.data` + `.bss`) | 40,856 B (**32.8%**) — 83,724 B remaining |

> The binary fits comfortably in a 2 MB app partition with ~1.1 MB to spare — suitable for OTA.

### Runtime Heap — SoftAP First Boot (Provisioning)

| Checkpoint | Used | Free | Min watermark | Tasks |
|---|---|---|---|---|
| `boot` | 22,832 B (7.2%) | 293,828 B | — | 6 |
| `post-oled-init` | 24,668 B (7.8%) | 291,992 B | — | 6 |
| `post-nvs-init` | 26,112 B (8.2%) | 290,548 B | 290,096 B | 6 |
| `prov-start` | ~85,000 B (26.8%) | ~231,660 B | — | 11 |
| `got-ip` ← **peak** | **~93,500 B (29.5%)** | ~223,160 B | **216,372 B** | 11 |
| `prov-end` ← SoftAP teardown | 79,268 B (25.0%) | 237,392 B | — | 10 |

```
boot           ██░░░░░░░░░░░░░░░░░░░   22 KB / 316 KB
post-nvs-init  ██░░░░░░░░░░░░░░░░░░░   26 KB / 316 KB
prov-start     █████████░░░░░░░░░░░░   85 KB / 316 KB  (+59 KB — WiFi driver + SoftAP + HTTP)
got-ip (peak)  ██████████░░░░░░░░░░░   93 KB / 316 KB  ← worst case
prov-end       ████████░░░░░░░░░░░░░   79 KB / 316 KB  ← SoftAP teardown
```

> **Heap fragmentation is not a concern.** The largest contiguous free block stays at **110,592 B** throughout all phases.

### CPU

The ESP32 is dual-core. WiFi is pinned to Core 0 by ESP-IDF and cannot be moved. For time-sensitive tasks (motor control, sensor PID loops), pin them to Core 1:

```c
xTaskCreatePinnedToCore(my_task, "my_task", 4096, NULL, 5, NULL, 1);
```

Both cores share the same RAM heap and peripherals — use mutexes when accessing a peripheral from multiple tasks across cores.

---

## Installation

### Step 1: Add Component

```bash
idf.py add-dependency --git https://github.com/quackonauty/ESP-IDF_ESP_STA_MANAGER.git --git-ref 1.0.0 qck_esp_sta_manager
```

Or in `main/idf_component.yml`:

```yaml
dependencies:
  qck_esp_sta_manager:
    git: https://github.com/quackonauty/ESP-IDF_ESP_STA_MANAGER.git
    version: 1.0.0
```

### Step 2: Partition Table

A custom partition table is required. Requires 4 MB flash minimum.

Create `partitions.csv` in the project root:

**Hardcoded credentials (DEV mode)** — no `mfg_data` needed:

```csv
# Name,   Type, SubType, Offset,  Size,
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 2M,
```

**Per-device credentials from NVS (PROD mode)** — add a dedicated NVS partition:

```csv
# Name,     Type, SubType, Offset,   Size,
nvs,        data, nvs,     0x9000,   0x6000,
phy_init,   data, phy,     0xf000,   0x1000,
factory,    app,  factory, 0x10000,  2M,
mfg_data,   data, nvs,     0x210000, 0x6000,
```

Enable in `idf.py menuconfig` → **Partition Table**:

- **Partition Table** → `Custom partition table CSV`
- **Custom partition table CSV file** → `partitions.csv`
- **Offset of partition table** → leave as `0x8000`
- **Generate MD5 checksum** → leave enabled

Then build:

```bash
idf.py fullclean
idf.py build
```

---

## Provisioning

### BLE (Recommended)

Mobile apps:

- Android: [ESP BLE Provisioning](https://play.google.com/store/apps/details?id=com.espressif.provble)
- iOS: [ESP BLE Provisioning](https://apps.apple.com/app/esp-ble-provisioning/id1473590141)

Steps: flash firmware → open app → connect to `PROV_XXXXXX` → enter WiFi credentials.

### SoftAP

Enable in `idf.py menuconfig` → Component config → ESP STA Manager → Provisioning transport → SoftAP. Required for ESP32-S2 (no BLE).

---

## `menuconfig` Options

```bash
idf.py menuconfig
```

Navigate to: **Component config → ESP STA Manager**

| Option | Default | Description |
|--------|---------|-------------|
| Provisioning transport | BLE | BLE or SoftAP |
| Provisioning security | Security 2 | SRP6a (recommended) or PoP |
| Custom endpoint | Disabled | Extra data endpoint for custom provisioning apps |
| Reset on failure | Enabled | Re-enters provisioning after max retries |
| Max connection attempts | 5 | Retries before provisioning reset (visible only when reset is enabled) |
| Log level | Info | Component verbosity |

> **Service name and MAC suffix** are configured at runtime via `sta_manager_config_t` — not in Kconfig. This ensures the caller always sets them explicitly.

---

## Quick Start

Four minimal examples covering every combination of security level and credential source.

| Example | Security | Credentials | Use when |
|---------|----------|-------------|----------|
| [Sec1 DEV](#security-1--hardcoded-pop) | PoP | Hardcoded string | Development, all units share the same PoP |
| [Sec2 DEV](#security-2--hardcoded-credentials) | SRP-6a | Hardcoded salt+verifier | Development, all units share the same credentials |
| [Sec1 PROD](#security-1--per-device-pop-from-nvs) | PoP | Per-device from NVS | Production, each unit has a unique PoP |
| [Sec2 PROD](#security-2--per-device-credentials-from-nvs) | SRP-6a | Per-device from NVS | Production, each unit has unique credentials |

> `sta_manager_start()` is non-blocking — use the window before `wait_connected()` to initialize peripherals.

### Common Helpers

All examples share these helpers — copy them into your `app_main.c` once:

```c
#include "nvs_flash.h"
#include "driver/gpio.h"

/* NVS init — must be called before sta_manager_init().
 * Handles corrupt partition and version mismatch by erasing and reinitializing. */
static esp_err_t nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW("app", "NVS corrupt or version mismatch — erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/* Factory reset button — GPIO_NUM_0 (BOOT button on most ESP32 dev boards).
 * Active-low with internal pull-up. Hold RESET_HOLD_MS to trigger.
 *
 * Configuration:
 *   RESET_GPIO    — GPIO pin (active-low, internal pull-up required)
 *   RESET_HOLD_MS — hold duration before reset triggers (ms) */
#define RESET_GPIO    GPIO_NUM_0
#define RESET_HOLD_MS 5000

static void reset_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RESET_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    uint32_t held_ms = 0;
    while (1) {
        if (gpio_get_level(RESET_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= RESET_HOLD_MS) {
                ESP_LOGW("app", "Factory reset triggered");
                sta_manager_reset_credentials();
                nvs_flash_erase();
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

---

### Security 1 — Hardcoded PoP

**menuconfig**: Provisioning security → `Security 1 (PoP)`

All devices share the same PoP. Compatible with the official ESP-IDF provisioning app (`password="abcd1234"`).

```c
#include "esp_sta_manager.h"

static const char *S_POP = "abcd1234";

static void wifi_event_cb(void *user_data, sta_mgr_event_t event, void *event_data)
{
    switch (event) {
    case STA_MGR_EVENT_PROV_START:
        ESP_LOGI("app", "Provisioning — scan BLE or connect to SoftAP");
        break;
    case STA_MGR_EVENT_STA_GOT_IP: {
        sta_mgr_ip_info_t *info = (sta_mgr_ip_info_t *)event_data;
        ESP_LOGI("app", "Connected — IP: %s  SSID: %s", info->ip, info->ssid);
        break;
    }
    case STA_MGR_EVENT_STA_DISCONNECTED:
        ESP_LOGW("app", "Disconnected, reconnecting...");
        break;
    default: break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init());
    xTaskCreate(reset_task, "reset", 3072, NULL, 5, NULL);

    sta_manager_config_t cfg = STA_MANAGER_CONFIG_DEFAULT();
    cfg.event_cb          = wifi_event_cb;
    cfg.service_name      = "PROV_";
    cfg.append_mac_suffix = true;
    cfg.sec1_pop          = S_POP;

    ESP_ERROR_CHECK(sta_manager_init(&cfg));
    ESP_ERROR_CHECK(sta_manager_start());
    sta_manager_wait_connected(0); // 0 = wait forever

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

---

### Security 2 — Hardcoded Credentials

**menuconfig**: Provisioning security → `Security 2 (SRP6a)`

All devices share the same salt + verifier. Compatible with the official ESP-IDF provisioning app (`username="wifiprov"`, `password="abcd1234"`).

> To regenerate for a different username/password, see [Credential Generation](#credential-generation).

```c
#include "esp_sta_manager.h"

/* username="wifiprov"  password="abcd1234" — official ESP-IDF provisioning app defaults.
 * Regenerate: python mfg_flash_dev_creds.py --security sec2 --show-c */
static const uint8_t s_salt[] = {
    0x03, 0x6e, 0xe0, 0xc7, 0xbc, 0xb9, 0xed, 0xa8,
    0x4c, 0x9e, 0xac, 0x97, 0xd9, 0x3d, 0xec, 0xf4,
};
static const uint8_t s_verifier[] = {
    0x7c, 0x7c, 0x85, 0x47, 0x65, 0x08, 0x94, 0x6d, 0xd6, 0x36, 0xaf, 0x37, 0xd7, 0xe8, 0x91, 0x43,
    0x78, 0xcf, 0xfd, 0x61, 0x6c, 0x59, 0xd2, 0xf8, 0x39, 0x08, 0x12, 0x72, 0x38, 0xde, 0x9e, 0x24,
    0xa4, 0x70, 0x26, 0x1c, 0xdf, 0xa9, 0x03, 0xc2, 0xb2, 0x70, 0xe7, 0xb1, 0x32, 0x24, 0xda, 0x11,
    0x1d, 0x97, 0x18, 0xdc, 0x60, 0x72, 0x08, 0xcc, 0x9a, 0xc9, 0x0c, 0x48, 0x27, 0xe2, 0xae, 0x89,
    0xaa, 0x16, 0x25, 0xb8, 0x04, 0xd2, 0x1a, 0x9b, 0x3a, 0x8f, 0x37, 0xf6, 0xe4, 0x3a, 0x71, 0x2e,
    0xe1, 0x27, 0x86, 0x6e, 0xad, 0xce, 0x28, 0xff, 0x54, 0x46, 0x60, 0x1f, 0xb9, 0x96, 0x87, 0xdc,
    0x57, 0x40, 0xa7, 0xd4, 0x6c, 0xc9, 0x77, 0x54, 0xdc, 0x16, 0x82, 0xf0, 0xed, 0x35, 0x6a, 0xc4,
    0x70, 0xad, 0x3d, 0x90, 0xb5, 0x81, 0x94, 0x70, 0xd7, 0xbc, 0x65, 0xb2, 0xd5, 0x18, 0xe0, 0x2e,
    0xc3, 0xa5, 0xf9, 0x68, 0xdd, 0x64, 0x7b, 0xb8, 0xb7, 0x3c, 0x9c, 0xfc, 0x00, 0xd8, 0x71, 0x7e,
    0xb7, 0x9a, 0x7c, 0xb1, 0xb7, 0xc2, 0xc3, 0x18, 0x34, 0x29, 0x32, 0x43, 0x3e, 0x00, 0x99, 0xe9,
    0x82, 0x94, 0xe3, 0xd8, 0x2a, 0xb0, 0x96, 0x29, 0xb7, 0xdf, 0x0e, 0x5f, 0x08, 0x33, 0x40, 0x76,
    0x52, 0x91, 0x32, 0x00, 0x9f, 0x97, 0x2c, 0x89, 0x6c, 0x39, 0x1e, 0xc8, 0x28, 0x05, 0x44, 0x17,
    0x3f, 0x68, 0x02, 0x8a, 0x9f, 0x44, 0x61, 0xd1, 0xf5, 0xa1, 0x7e, 0x5a, 0x70, 0xd2, 0xc7, 0x23,
    0x81, 0xcb, 0x38, 0x68, 0xe4, 0x2c, 0x20, 0xbc, 0x40, 0x57, 0x76, 0x17, 0xbd, 0x08, 0xb8, 0x96,
    0xbc, 0x26, 0xeb, 0x32, 0x46, 0x69, 0x35, 0x05, 0x8c, 0x15, 0x70, 0xd9, 0x1b, 0xe9, 0xbe, 0xcc,
    0xa9, 0x38, 0xa6, 0x67, 0xf0, 0xad, 0x50, 0x13, 0x19, 0x72, 0x64, 0xbf, 0x52, 0xc2, 0x34, 0xe2,
    0x1b, 0x11, 0x79, 0x74, 0x72, 0xbd, 0x34, 0x5b, 0xb1, 0xe2, 0xfd, 0x66, 0x73, 0xfe, 0x71, 0x64,
    0x74, 0xd0, 0x4e, 0xbc, 0x51, 0x24, 0x19, 0x40, 0x87, 0x0e, 0x92, 0x40, 0xe6, 0x21, 0xe7, 0x2d,
    0x4e, 0x37, 0x76, 0x2f, 0x2e, 0xe2, 0x68, 0xc7, 0x89, 0xe8, 0x32, 0x13, 0x42, 0x06, 0x84, 0x84,
    0x53, 0x4a, 0xb3, 0x0c, 0x1b, 0x4c, 0x8d, 0x1c, 0x51, 0x97, 0x19, 0xab, 0xae, 0x77, 0xff, 0xdb,
    0xec, 0xf0, 0x10, 0x95, 0x34, 0x33, 0x6b, 0xcb, 0x3e, 0x84, 0x0f, 0xb9, 0xd8, 0x5f, 0xb8, 0xa0,
    0xb8, 0x55, 0x53, 0x3e, 0x70, 0xf7, 0x18, 0xf5, 0xce, 0x7b, 0x4e, 0xbf, 0x27, 0xce, 0xce, 0xa8,
    0xb3, 0xbe, 0x40, 0xc5, 0xc5, 0x32, 0x29, 0x3e, 0x71, 0x64, 0x9e, 0xde, 0x8c, 0xf6, 0x75, 0xa1,
    0xe6, 0xf6, 0x53, 0xc8, 0x31, 0xa8, 0x78, 0xde, 0x50, 0x40, 0xf7, 0x62, 0xde, 0x36, 0xb2, 0xba,
};

static void wifi_event_cb(void *user_data, sta_mgr_event_t event, void *event_data)
{
    switch (event) {
    case STA_MGR_EVENT_PROV_START:
        ESP_LOGI("app", "Provisioning — scan BLE or connect to SoftAP");
        break;
    case STA_MGR_EVENT_STA_GOT_IP: {
        sta_mgr_ip_info_t *info = (sta_mgr_ip_info_t *)event_data;
        ESP_LOGI("app", "Connected — IP: %s  SSID: %s", info->ip, info->ssid);
        break;
    }
    case STA_MGR_EVENT_STA_DISCONNECTED:
        ESP_LOGW("app", "Disconnected, reconnecting...");
        break;
    default: break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init());
    xTaskCreate(reset_task, "reset", 3072, NULL, 5, NULL);

    sta_manager_config_t cfg = STA_MANAGER_CONFIG_DEFAULT();
    cfg.event_cb          = wifi_event_cb;
    cfg.service_name      = "PROV_";
    cfg.append_mac_suffix = true;
    cfg.sec2_salt         = (const char *)s_salt;
    cfg.sec2_salt_len     = sizeof(s_salt);
    cfg.sec2_verifier     = (const char *)s_verifier;
    cfg.sec2_verifier_len = sizeof(s_verifier);

    ESP_ERROR_CHECK(sta_manager_init(&cfg));
    ESP_ERROR_CHECK(sta_manager_start());
    sta_manager_wait_connected(0); // 0 = wait forever

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

---

### Security 1 — Per-device PoP from NVS

**menuconfig**: Provisioning security → `Security 1 (PoP)`

Each device has a unique PoP stored in the `mfg_data` partition. Use `mfg_flash_dev_creds.py` to generate and flash it.

```c
#include "esp_sta_manager.h"
#include "nvs.h"

/* Static buffer — must outlive sta_manager_start(). */
static char s_pop[64];

static esp_err_t load_credentials(void)
{
    esp_err_t ret = nvs_flash_init_partition("mfg_data");
    if (ret != ESP_OK) {
        ESP_LOGE("app", "mfg_data init failed — flash credentials first");
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open_from_partition("mfg_data", "wifi_prov_sec1", NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    size_t len = sizeof(s_pop);
    ret = nvs_get_str(nvs, "sec1_pop", s_pop, &len);
    nvs_close(nvs);

    if (ret != ESP_OK)
        ESP_LOGE("app", "sec1_pop missing in mfg_data: %s", esp_err_to_name(ret));
    return ret;
}

static void wifi_event_cb(void *user_data, sta_mgr_event_t event, void *event_data)
{
    switch (event) {
    case STA_MGR_EVENT_PROV_START:
        ESP_LOGI("app", "Provisioning — scan BLE or connect to SoftAP");
        break;
    case STA_MGR_EVENT_STA_GOT_IP: {
        sta_mgr_ip_info_t *info = (sta_mgr_ip_info_t *)event_data;
        ESP_LOGI("app", "Connected — IP: %s  SSID: %s", info->ip, info->ssid);
        break;
    }
    case STA_MGR_EVENT_STA_DISCONNECTED:
        ESP_LOGW("app", "Disconnected, reconnecting...");
        break;
    default: break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init());
    ESP_ERROR_CHECK(load_credentials());
    xTaskCreate(reset_task, "reset", 3072, NULL, 5, NULL);

    sta_manager_config_t cfg = STA_MANAGER_CONFIG_DEFAULT();
    cfg.event_cb          = wifi_event_cb;
    cfg.service_name      = "PROV_";
    cfg.append_mac_suffix = true;
    cfg.sec1_pop          = s_pop;

    ESP_ERROR_CHECK(sta_manager_init(&cfg));
    ESP_ERROR_CHECK(sta_manager_start());
    sta_manager_wait_connected(0);

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

---

### Security 2 — Per-device Credentials from NVS

**menuconfig**: Provisioning security → `Security 2 (SRP6a)`

Each device has unique credentials stored in the `mfg_data` partition.

```c
#include "esp_sta_manager.h"
#include "nvs.h"

/* Static buffers — must outlive sta_manager_start() since prov_mgr holds
 * pointers until WIFI_PROV_END, which fires after the STA connects. */
static uint8_t s_salt[16];
static uint8_t s_verifier[384];

static esp_err_t load_credentials(void)
{
    esp_err_t ret = nvs_flash_init_partition("mfg_data");
    if (ret != ESP_OK) {
        ESP_LOGE("app", "mfg_data init failed — flash with mfg_flash_dev_creds.py first");
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open_from_partition("mfg_data", "wifi_prov_sec2", NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    size_t salt_len     = sizeof(s_salt);
    size_t verifier_len = sizeof(s_verifier);
    ret = nvs_get_blob(nvs, "sec2_salt",     s_salt,     &salt_len);
    if (ret == ESP_OK)
        ret = nvs_get_blob(nvs, "sec2_verifier", s_verifier, &verifier_len);

    nvs_close(nvs);
    if (ret != ESP_OK)
        ESP_LOGE("app", "Credentials missing in mfg_data: %s", esp_err_to_name(ret));
    return ret;
}

static void wifi_event_cb(void *user_data, sta_mgr_event_t event, void *event_data)
{
    switch (event) {
    case STA_MGR_EVENT_PROV_START:
        ESP_LOGI("app", "Provisioning — scan BLE or connect to SoftAP");
        break;
    case STA_MGR_EVENT_STA_GOT_IP: {
        sta_mgr_ip_info_t *info = (sta_mgr_ip_info_t *)event_data;
        ESP_LOGI("app", "Connected — IP: %s  SSID: %s", info->ip, info->ssid);
        break;
    }
    case STA_MGR_EVENT_STA_DISCONNECTED:
        ESP_LOGW("app", "Disconnected, reconnecting...");
        break;
    default: break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init());
    ESP_ERROR_CHECK(load_credentials());
    xTaskCreate(reset_task, "reset", 3072, NULL, 5, NULL);

    sta_manager_config_t cfg = STA_MANAGER_CONFIG_DEFAULT();
    cfg.event_cb          = wifi_event_cb;
    cfg.service_name      = "PROV_";
    cfg.append_mac_suffix = true;
    cfg.sec2_salt         = (const char *)s_salt;
    cfg.sec2_salt_len     = sizeof(s_salt);
    cfg.sec2_verifier     = (const char *)s_verifier;
    cfg.sec2_verifier_len = sizeof(s_verifier);

    ESP_ERROR_CHECK(sta_manager_init(&cfg));
    ESP_ERROR_CHECK(sta_manager_start());
    sta_manager_wait_connected(0);

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

---

## Factory Reset

`reset_task` calls `sta_manager_reset_credentials()` to erase stored WiFi credentials, then `nvs_flash_erase()` for a full NVS wipe before restarting. To clear only WiFi credentials without wiping all app data:

```c
if (long_press_detected()) {
    sta_manager_reset_credentials(); // clears only WiFi credentials
    esp_restart();
}
```

---

## Credential Generation

### mfg_flash_dev_creds.py

Requires the ESP-IDF environment active (`export.sh` / `export.ps1`).

**Commands**

```bash
# sec2 default — all devices share the same salt+verifier
python mfg_flash_dev_creds.py --security sec2

# sec2 unique — new salt+verifier from username+password
python mfg_flash_dev_creds.py --security sec2 --username ESP32 --password SN-A1B2C3

# sec1 default — all devices share the same PoP ("abcd1234")
python mfg_flash_dev_creds.py --security sec1

# sec1 unique — uses --password as the PoP
python mfg_flash_dev_creds.py --security sec1 --password SN-A1B2C3

# flash immediately after generating
python mfg_flash_dev_creds.py --security sec2 --username ESP32 --password SN-A1B2C3 --flash

# flash to explicit port
python mfg_flash_dev_creds.py --security sec2 --username ESP32 --password SN-A1B2C3 --flash --port COM3

# print C arrays ready to paste into app_main.c
python mfg_flash_dev_creds.py --security sec2 --show-c
```

**Arguments**

| Argument | Required | Description |
|----------|----------|-------------|
| `--security` | Yes | `sec1` → PoP string. `sec2` → SRP-6a salt + verifier |
| `--username` | sec2 unique | SRP username — must match what the provisioning app sends |
| `--password` | unique | [sec2] Per-device password. [sec1] The PoP itself |
| `--serial` | No | Device ID recorded in `device_db.csv`. Defaults to `--password` |
| `--flash` | No | Flash `dev_creds_nvs.bin` after generating |
| `--port` | No | Serial port. Auto-detected if omitted |
| `--address` | No | `mfg_data` flash offset — must match `partitions.csv` (default: `0x210000`) |
| `--nvs-size` | No | `mfg_data` partition size — must match `partitions.csv` (default: `0x6000`) |
| `--db` | No | Device database CSV path (default: `device_db.csv`) |
| `--show-c` | No | Print C declarations ready to paste into `app_main.c` |

**Manufacturing line example**

```bash
SERIAL="SN-$(date +%s%N | sha256sum | head -c8 | tr '[:lower:]' '[:upper:]')"

python mfg_flash_dev_creds.py \
    --security sec2 \
    --username ESP32 \
    --password "$SERIAL" \
    --serial  "$SERIAL" \
    --flash

# Print $SERIAL on device label / QR code
```

### Without ESP-IDF Environment

```bash
pip install srptools
```

```python
import srptools

USERNAME = "wifiprov"
PASSWORD = "abcd1234"

salt, verifier = srptools.client.create_verifier(
    USERNAME.encode(), PASSWORD.encode()
)

def to_c_array(name: str, data: bytes) -> str:
    rows = [data[i:i+16] for i in range(0, len(data), 16)]
    body = ",\n    ".join(", ".join(f"0x{b:02x}" for b in row) for row in rows)
    return f"static const uint8_t {name}[] = {{\n    {body},\n}};"

print(to_c_array("s_salt",     salt))
print()
print(to_c_array("s_verifier", verifier))
```

---

## API Reference

### Lifecycle

| Function | Description |
|----------|-------------|
| `sta_manager_init(config)` | Initialize stack, event handlers, and provisioning manager |
| `sta_manager_start()` | Start provisioning if needed, or connect to saved network |
| `sta_manager_deinit()` | Stop WiFi and release all resources |

### Status

| Function | Description |
|----------|-------------|
| `sta_manager_is_provisioned()` | Returns `true` if credentials are stored in NVS |
| `sta_manager_is_connected()` | Returns `true` if connected with a valid IP |
| `sta_manager_wait_connected(timeout_ms)` | Block until connected; `0` waits forever |

### Utilities

| Function | Description |
|----------|-------------|
| `sta_manager_reset_credentials()` | Erase stored WiFi credentials from NVS |
| `sta_manager_get_ip_info(info)` | Get current IP, netmask, gateway, and SSID |
| `sta_manager_get_ssid(ssid, len)` | Get connected AP SSID |
| `sta_manager_get_service_name(name, max)` | Get full service name (with MAC suffix if enabled) |

### Configuration

```c
typedef struct {
    sta_mgr_event_cb_t event_cb;    ///< Event callback (NULL = disabled)
    void              *user_data;   ///< Passed to every callback invocation

    const char *service_name;       ///< BLE advertisement name / SoftAP SSID (or prefix)
                                    ///< Required — assert fires if NULL
                                    ///< Must point to persistent memory (static or literal)
    bool append_mac_suffix;         ///< Append last 3 MAC bytes to service_name for uniqueness

#ifdef CONFIG_ESP_STA_MGR_PROV_SECURITY_1
    const char *sec1_pop;           ///< Proof of Possession string — required for Sec1
#endif

#ifdef CONFIG_ESP_STA_MGR_PROV_SECURITY_2
    const char *sec2_salt;          ///< SRP-6a salt blob (16 bytes)
    uint16_t    sec2_salt_len;      ///< Salt length in bytes
    const char *sec2_verifier;      ///< SRP-6a verifier blob (384 bytes)
    uint16_t    sec2_verifier_len;  ///< Verifier length in bytes
#endif
} sta_manager_config_t;
```

> **Note**: `service_name`, `sec2_salt`, and `sec2_verifier` must point to persistent memory (static arrays or string literals) that remains valid until `WIFI_PROV_END` fires.

Default macro — all fields that must be set are initialized to `NULL`/`0` to force explicit assignment:

```c
sta_manager_config_t cfg = STA_MANAGER_CONFIG_DEFAULT();
cfg.event_cb          = my_callback;
cfg.service_name      = "PROV_";
cfg.append_mac_suffix = true;
cfg.sec2_salt         = (const char *)s_salt;
cfg.sec2_salt_len     = sizeof(s_salt);
cfg.sec2_verifier     = (const char *)s_verifier;
cfg.sec2_verifier_len = sizeof(s_verifier);
```

---

## Event Reference

| Event | Trigger | `event_data` type |
|-------|---------|-------------------|
| `STA_MGR_EVENT_PROV_START` | Provisioning begins | `NULL` |
| `STA_MGR_EVENT_PROV_CRED_RECV` | Credentials received from provisioning app | `sta_mgr_prov_cred_t*` |
| `STA_MGR_EVENT_PROV_CRED_SUCCESS` | Credentials validated by connecting to AP | `NULL` |
| `STA_MGR_EVENT_PROV_CRED_FAIL` | Connection with received credentials failed | `sta_mgr_prov_fail_t*` |
| `STA_MGR_EVENT_PROV_END` | Provisioning session closed | `NULL` |
| `STA_MGR_EVENT_STA_START` | WiFi STA interface started | `NULL` |
| `STA_MGR_EVENT_STA_CONNECTING` | Attempting to connect to AP | `NULL` |
| `STA_MGR_EVENT_STA_CONNECTED` | Associated with AP (no IP yet) | `NULL` |
| `STA_MGR_EVENT_STA_GOT_IP` | IP address acquired | `sta_mgr_ip_info_t*` |
| `STA_MGR_EVENT_STA_DISCONNECTED` | Lost connection, reconnect triggered | `NULL` |
| `STA_MGR_EVENT_BLE_CONNECTED` | BLE client connected (BLE transport only) | `NULL` |
| `STA_MGR_EVENT_BLE_DISCONNECTED` | BLE client disconnected (BLE transport only) | `NULL` |
| `STA_MGR_EVENT_AP_CLIENT_CONNECTED` | Client joined SoftAP (SoftAP transport only) | `NULL` |
| `STA_MGR_EVENT_AP_CLIENT_DISCONNECTED` | Client left SoftAP before credentials accepted (SoftAP transport only) | `NULL` |

> **Important**: Callbacks run in the ESP-IDF event loop task. Avoid blocking calls — use FreeRTOS queues or task notifications to communicate with other tasks.

### Event Data Structures

```c
typedef struct { char ssid[33]; } sta_mgr_prov_cred_t;

typedef struct { const char *reason; } sta_mgr_prov_fail_t;

typedef struct {
    char ip[16];
    char netmask[16];
    char gateway[16];
    char ssid[33];
} sta_mgr_ip_info_t;
```

---

## Troubleshooting

**Device not visible in BLE app**: Enable Bluetooth and location on phone; confirm target supports BLE (ESP32-S2/C2 do not).

**Provisioning app rejects credentials**: Verify that the salt/verifier in your firmware were generated with the same username/password the app is sending. In DEV mode, the official ESP-IDF app uses `username="wifiprov"` and `password="abcd1234"`.

**Connection failures after provisioning**: Verify SSID and password, confirm 2.4 GHz network, increase `max_retry` in menuconfig.

**`mfg_data` init failed on boot**: The partition hasn't been flashed. Run `mfg_flash_dev_creds.py --flash` before starting.

**Stack overflow on reset task**: Increase task stack — `nvs_flash_erase()` requires ~2560 bytes minimum.

**Already provisioned, won't re-provision**: Hold the reset button for `RESET_HOLD_MS` (default 5 s), or erase NVS via esptool:

```bash
$IDF_PATH/components/esptool_py/esptool/esptool.py erase_region 0x9000 0x6000
```

**OLED or callback shows spurious "Client Disconnected" after WiFi connects (SoftAP)**: Upgrade to v1.0.0 — this was fixed with the `s_prov_cred_ok` flag that suppresses `AP_STADISCONNECTED` after credentials are accepted.

---

## Changelog

### 1.0.0

- **Initial release.**
- `service_name` and `append_mac_suffix` moved to runtime config (`sta_manager_config_t`) — no Kconfig string fields for naming. Forces the caller to always set them explicitly.
- `max_retry` removed from `sta_manager_config_t` — now configured via `CONFIG_ESP_STA_MGR_MAX_CONN_ATTEMPTS` in Kconfig, visible only when `ESP_STA_MGR_RESET_ON_FAILURE` is enabled (`depends on`).
- **Fix (SoftAP)**: `WIFI_EVENT` registered at `init()` for SoftAP transport, lazily at `WIFI_PROV_END` for BLE — ensures `AP_STACONNECTED` / `AP_STADISCONNECTED` events are received during provisioning.
- **Fix (SoftAP)**: `s_prov_active` flag added — gates all STA events (`STA_START`, `STA_CONNECTED`, `STA_DISCONNECTED`) during provisioning, preventing spurious `esp_wifi_connect()` calls while the AP is active.
- **Fix (SoftAP)**: `s_prov_cred_ok` flag added — suppresses `AP_STADISCONNECTED` after `WIFI_PROV_CRED_SUCCESS`. The phone disconnects from the SoftAP as part of the normal flow after submitting credentials, before `WIFI_PROV_END` fires.
- **Fix**: `STA_MGR_EVENT_PROV_CRED_FAIL` dispatches `sta_mgr_prov_fail_t*` — previously the example code incorrectly cast to `sta_mgr_prov_cred_t*`.
- **New**: `sta_manager_get_service_name(name, max)` made public — returns the full service name with MAC suffix applied.
- `sta_manager_deinit()` resets `s_prov_active` and `s_prov_cred_ok` for safe re-initialization.
- Kconfig log level `int` helper moved to immediately follow its `choice` block — consistent ordering across all components.

---

## License

Apache License 2.0. See [LICENSE](LICENSE) for details.

## Issues & Contributing

- **Issues**: [GitHub Issues](https://github.com/quackonauty/ESP-IDF_ESP_STA_MANAGER/issues)
- **Repository**: [https://github.com/quackonauty/ESP-IDF_ESP_STA_MANAGER](https://github.com/quackonauty/ESP-IDF_ESP_STA_MANAGER)

**Status**: Beta — production testing in progress.
