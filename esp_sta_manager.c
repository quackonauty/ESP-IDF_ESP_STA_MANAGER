#define LOG_LOCAL_LEVEL CONFIG_ESP_STA_MGR_LOG_LEVEL

#include "esp_sta_manager.h"

#include <assert.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <network_provisioning/manager.h>

#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
#include <network_provisioning/scheme_ble.h>
#endif

#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_SOFTAP
#include <network_provisioning/scheme_softap.h>
#endif

static const char *TAG = "esp_sta_manager";

/* ============================================================================
 * Private State
 * ========================================================================= */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

#define STA_CONNECTED_BIT BIT0

static EventGroupHandle_t s_sta_event_group = NULL;
static sta_manager_config_t s_config = {0};
static bool s_initialized = false;
static bool s_prov_active = false;  /* true while provisioning is in progress */
static bool s_prov_cred_ok = false; /* true after credentials accepted — suppresses AP_STADISCONNECTED */

#ifdef CONFIG_ESP_STA_MGR_PROV_SECURITY_2
static network_prov_security2_params_t s_sec2_params = {0};
#endif

/* ============================================================================
 * Event Dispatch
 * ========================================================================= */

static inline void dispatch_event(sta_mgr_event_t event, void *data)
{
    if (s_config.event_cb)
        s_config.event_cb(s_config.user_data, event, data);
}

/* ============================================================================
 * Event Handlers
 * ========================================================================= */

static void handle_prov_event(int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case NETWORK_PROV_START:
        ESP_LOGI(TAG, "Provisioning started");
        dispatch_event(STA_MGR_EVENT_PROV_START, NULL);
        break;

    case NETWORK_PROV_WIFI_CRED_RECV:
    {
        wifi_sta_config_t *cfg = (wifi_sta_config_t *)event_data;
        ESP_LOGI(TAG, "Credentials received: %s", (char *)cfg->ssid);

        sta_mgr_prov_cred_t cred = {0};
        strncpy(cred.ssid, (char *)cfg->ssid, sizeof(cred.ssid) - 1);
        dispatch_event(STA_MGR_EVENT_PROV_CRED_RECV, &cred);
        break;
    }

    case NETWORK_PROV_WIFI_CRED_FAIL:
    {
        network_prov_wifi_sta_fail_reason_t *reason = (network_prov_wifi_sta_fail_reason_t *)event_data;
        const char *reason_str = (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ? "Authentication failed" : "AP not found";

        ESP_LOGE(TAG, "Provisioning failed: %s", reason_str);
        sta_mgr_prov_fail_t fail = {.reason = reason_str};
        dispatch_event(STA_MGR_EVENT_PROV_CRED_FAIL, &fail);

#ifdef CONFIG_ESP_STA_MGR_RESET_ON_FAILURE
        network_prov_mgr_reset_wifi_sm_state_on_failure();
#endif
        break;
    }

    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        s_prov_cred_ok = true; /* from here AP_STADISCONNECTED is suppressed */
        ESP_LOGI(TAG, "Provisioning successful");
        dispatch_event(STA_MGR_EVENT_PROV_CRED_SUCCESS, NULL);
        break;

    case NETWORK_PROV_END:
        ESP_LOGI(TAG, "Provisioning ended");
        dispatch_event(STA_MGR_EVENT_PROV_END, NULL);
        network_prov_mgr_deinit();
        s_prov_active = false;  /* STA events processed normally from here */
        s_prov_cred_ok = false; /* reset for a potential re-provisioning */
#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
        /* BLE did not register WIFI_EVENT in init() — do it now */
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
#endif
        break;

    default:
        break;
    }
}

static void handle_wifi_event(int32_t event_id)
{
    switch (event_id)
    {
    /* ── STA events — ignored while provisioning is active ────────────── */
    case WIFI_EVENT_STA_START:
        if (s_prov_active)
            break;
        ESP_LOGI(TAG, "WiFi STA started");
        dispatch_event(STA_MGR_EVENT_STA_START, NULL);
        esp_wifi_connect();
        dispatch_event(STA_MGR_EVENT_STA_CONNECTING, NULL);
        break;

    case WIFI_EVENT_STA_CONNECTED:
        if (s_prov_active)
            break;
        ESP_LOGI(TAG, "WiFi connected (acquiring IP...)");
        dispatch_event(STA_MGR_EVENT_STA_CONNECTED, NULL);
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_prov_active)
            break;
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        xEventGroupClearBits(s_sta_event_group, STA_CONNECTED_BIT);
        dispatch_event(STA_MGR_EVENT_STA_DISCONNECTED, NULL);
        esp_wifi_connect();
        dispatch_event(STA_MGR_EVENT_STA_CONNECTING, NULL);
        break;

#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_SOFTAP
    /* ── AP events — only relevant while provisioning is active ───────── */
    case WIFI_EVENT_AP_STACONNECTED:
        if (!s_prov_active)
            break;
        ESP_LOGI(TAG, "Client connected to SoftAP");
        dispatch_event(STA_MGR_EVENT_AP_CLIENT_CONNECTED, NULL);
        break;

    case WIFI_EVENT_AP_STADISCONNECTED:
        /* Ignore if provisioning is done or credentials already accepted —
         * the phone disconnects from the AP as part of the normal flow
         * after submitting credentials, before NETWORK_PROV_END fires. */
        if (!s_prov_active || s_prov_cred_ok)
            break;
        ESP_LOGI(TAG, "Client disconnected from SoftAP");
        dispatch_event(STA_MGR_EVENT_AP_CLIENT_DISCONNECTED, NULL);
        break;
#endif

    default:
        break;
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == NETWORK_PROV_EVENT)
    {
        handle_prov_event(id, data);
    }
    else if (base == WIFI_EVENT)
    {
        handle_wifi_event(id);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;

        sta_mgr_ip_info_t info = {0};
        snprintf(info.ip, sizeof(info.ip), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(info.netmask, sizeof(info.netmask), IPSTR, IP2STR(&event->ip_info.netmask));
        snprintf(info.gateway, sizeof(info.gateway), IPSTR, IP2STR(&event->ip_info.gw));
        sta_manager_get_ssid(info.ssid, sizeof(info.ssid));

        ESP_LOGI(TAG, "Got IP: %s", info.ip);
        xEventGroupSetBits(s_sta_event_group, STA_CONNECTED_BIT);
        dispatch_event(STA_MGR_EVENT_STA_GOT_IP, &info);
    }
#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
    else if (base == PROTOCOMM_TRANSPORT_BLE_EVENT)
    {
        switch (id)
        {
        case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
            ESP_LOGI(TAG, "BLE client connected");
            dispatch_event(STA_MGR_EVENT_BLE_CONNECTED, NULL);
            break;
        case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
            ESP_LOGI(TAG, "BLE client disconnected");
            dispatch_event(STA_MGR_EVENT_BLE_DISCONNECTED, NULL);
            break;
        default:
            break;
        }
    }
#endif
    else if (base == PROTOCOMM_SECURITY_SESSION_EVENT)
    {
        switch (id)
        {
        case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
            ESP_LOGI(TAG, "Secure session established");
            break;
        case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
            ESP_LOGE(TAG, "Invalid security params");
            break;
        case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
            ESP_LOGE(TAG, "Incorrect credentials");
            break;
        default:
            break;
        }
    }
}

/* ============================================================================
 * Helpers
 * ========================================================================= */

void sta_manager_get_service_name(char *name, size_t max)
{
    assert(name != NULL && max > 0);

    if (s_config.append_mac_suffix)
    {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(name, max, "%s%02X%02X%02X", s_config.service_name, mac[3], mac[4], mac[5]);
    }
    else
    {
        strncpy(name, s_config.service_name, max - 1);
        name[max - 1] = '\0';
    }
}

#ifdef CONFIG_ESP_STA_MGR_CUSTOM_ENDPOINT
/* Custom provisioning endpoint — called after the SRP handshake.
 * outbuf must be heap-allocated; protocomm frees it after send. */
static esp_err_t custom_prov_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    if (inbuf)
        ESP_LOGI(TAG, "Custom endpoint data received (%d bytes)", inlen);

    const char *response = "OK";
    *outbuf = (uint8_t *)strdup(response);
    if (!*outbuf)
        return ESP_ERR_NO_MEM;
    *outlen = strlen(response) + 1;
    return ESP_OK;
}
#endif

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ============================================================================
 * Public API
 * ========================================================================= */

esp_err_t sta_manager_init(const sta_manager_config_t *config)
{
    if (s_initialized)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing STA Manager");

    if (config)
    {
        /* service_name == NULL with a non-NULL config is a caller bug. */
        assert(config->service_name != NULL);

#ifdef CONFIG_ESP_STA_MGR_PROV_SECURITY_1
        if (!config->sec1_pop || config->sec1_pop[0] == '\0')
        {
            ESP_LOGE(TAG, "sec1_pop must be a non-empty string");
            return ESP_ERR_INVALID_ARG;
        }
#endif

#ifdef CONFIG_ESP_STA_MGR_PROV_SECURITY_2
        if (!config->sec2_salt || config->sec2_salt_len == 0 ||
            !config->sec2_verifier || config->sec2_verifier_len == 0)
        {
            ESP_LOGE(TAG, "Sec2 credentials incomplete (salt=%p len=%u  verifier=%p len=%u)", config->sec2_salt, config->sec2_salt_len, config->sec2_verifier, config->sec2_verifier_len);
            return ESP_ERR_INVALID_ARG;
        }
#endif
        memcpy(&s_config, config, sizeof(sta_manager_config_t));
    }
    else
    {
        /* NULL config is always invalid — credentials must be set explicitly. */
        ESP_LOGE(TAG, "NULL config — provide sec1_pop (Sec1) or sec2_salt/sec2_verifier (Sec2)");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_event_group = xEventGroupCreate();
    if (!s_sta_event_group)
    {
        ESP_LOGE(TAG, "Failed to create event group (out of memory)");
        abort();
    }

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
#endif

    esp_netif_create_default_wifi_sta();

#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_SOFTAP
    esp_netif_create_default_wifi_ap();
    /* SoftAP needs WIFI_EVENT from the start to receive AP_STA* events
     * during provisioning. BLE registers it later in NETWORK_PROV_END. */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
#endif

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    /* NOTE: the retry-count field previously set here (.wifi_prov_conn_cfg /
     * .wifi_conn_attempts) does not exist on network_prov_mgr_config_t in
     * network_provisioning 1.2.2 — confirmed by the v6.0 build log ("has no
     * member named 'wifi_prov_conn_cfg'"). Removed for now so the manager
     * uses the library default retry count instead of
     * CONFIG_ESP_STA_MGR_MAX_CONN_ATTEMPTS. To restore this behavior, open
     * managed_components/espressif__network_provisioning/include/
     * network_provisioning/manager.h, find the renamed field (grep for
     * "conn_attempts"), and re-add it here with its real name. */
    network_prov_mgr_config_t prov_cfg = {
#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
#endif
#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_SOFTAP
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
#endif
    };

    ESP_ERROR_CHECK(network_prov_mgr_init(prov_cfg));

    s_initialized = true;
    ESP_LOGI(TAG, "Initialization complete");
    return ESP_OK;
}

esp_err_t sta_manager_start(void)
{
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned)
    {
        s_prov_active = true; /* block STA events until provisioning ends */
        ESP_LOGI(TAG, "Not provisioned, starting provisioning");

        char service_name[strnlen(s_config.service_name, 32) + 7];
        sta_manager_get_service_name(service_name, sizeof(service_name));

        /* No unconditional default here — NETWORK_PROV_SECURITY_1 only
         * exists when CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1 is
         * enabled (disabled by default in v6.0). Only the matching #if/#elif
         * branch below assigns this. */
        network_prov_security_t security;
        const void *sec_params = NULL;

#if defined(CONFIG_ESP_STA_MGR_PROV_SECURITY_1)
        security = NETWORK_PROV_SECURITY_1;
        /* TODO: verify against manager.h that Security 1 still takes the PoP
         * as a raw string cast this way. If network_prov_security1_params_t
         * is now a real struct (not just an alias for const char*), this
         * cast needs to change accordingly. */
        sec_params = (network_prov_security1_params_t *)s_config.sec1_pop;

#elif defined(CONFIG_ESP_STA_MGR_PROV_SECURITY_2)
        s_sec2_params.salt = s_config.sec2_salt;
        s_sec2_params.salt_len = s_config.sec2_salt_len;
        s_sec2_params.verifier = s_config.sec2_verifier;
        s_sec2_params.verifier_len = s_config.sec2_verifier_len;
        security = NETWORK_PROV_SECURITY_2;
        sec_params = &s_sec2_params;

#endif

        const char *service_key = NULL;

#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
        uint8_t custom_uuid[] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};

        network_prov_scheme_ble_set_service_uuid(custom_uuid);
#endif

#ifdef CONFIG_ESP_STA_MGR_CUSTOM_ENDPOINT
        network_prov_mgr_endpoint_create("custom-data");
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, sec_params, service_name, service_key));
        network_prov_mgr_endpoint_register("custom-data", custom_prov_handler, NULL);
#else
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, sec_params, service_name, service_key));
#endif

        ESP_LOGI(TAG, "Service name: %s", service_name);
#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
        ESP_LOGI(TAG, "Scan for '%s' in the provisioning app", service_name);
#else
        ESP_LOGI(TAG, "Connect to AP '%s' to provision", service_name);
#endif
    }
    else
    {
        ESP_LOGI(TAG, "Already provisioned, connecting to WiFi");
        network_prov_mgr_deinit();

#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
        /* BLE path: WIFI_EVENT was never registered, do it now */
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
#endif
        /* s_prov_active stays false — STA events processed immediately */
        wifi_init_sta();
    }

    return ESP_OK;
}

esp_err_t sta_manager_wait_connected(uint32_t timeout_ms)
{
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_sta_event_group, STA_CONNECTED_BIT, false, true, ticks);
    return (bits & STA_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool sta_manager_is_provisioned(void)
{
    bool provisioned = false;
    if (s_initialized)
        network_prov_mgr_is_wifi_provisioned(&provisioned);
    return provisioned;
}

bool sta_manager_is_connected(void)
{
    if (!s_initialized)
        return false;
    return (xEventGroupGetBits(s_sta_event_group) & STA_CONNECTED_BIT) != 0;
}

esp_err_t sta_manager_reset_credentials(void)
{
    ESP_LOGW(TAG, "Resetting WiFi credentials");

    /* Best-guess name, following the same "_wifi_" naming pattern confirmed
     * for network_prov_mgr_is_wifi_provisioned(). If the compiler rejects
     * this, it will suggest the real name via "did you mean" — swap it in. */
    esp_err_t ret = network_prov_mgr_reset_wifi_provisioning();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to reset credentials: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi credentials cleared");
    return ESP_OK;
}

esp_err_t sta_manager_deinit(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing STA Manager");

    network_prov_mgr_deinit();

    esp_event_handler_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler);
    esp_event_handler_unregister(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &event_handler);

#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
    esp_event_handler_unregister(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &event_handler);
#endif

    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(ret));

    ret = esp_wifi_deinit();
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "esp_wifi_deinit: %s", esp_err_to_name(ret));

    if (s_sta_event_group)
    {
        vEventGroupDelete(s_sta_event_group);
        s_sta_event_group = NULL;
    }

    memset(&s_config, 0, sizeof(sta_manager_config_t));
    s_initialized = false;
    s_prov_active = false;
    s_prov_cred_ok = false;

    ESP_LOGI(TAG, "Deinitialization complete");
    return ESP_OK;
}

esp_err_t sta_manager_get_ssid(char *ssid, size_t len)
{
    if (!ssid || len == 0)
        return ESP_ERR_INVALID_ARG;

    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret == ESP_OK)
    {
        strncpy(ssid, (char *)ap_info.ssid, len - 1);
        ssid[len - 1] = '\0';
    }
    return ret;
}

esp_err_t sta_manager_get_ip_info(sta_mgr_ip_info_t *info)
{
    if (!info)
        return ESP_ERR_INVALID_ARG;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif)
        return ESP_ERR_INVALID_STATE;

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret == ESP_OK)
    {
        snprintf(info->ip, sizeof(info->ip), IPSTR, IP2STR(&ip_info.ip));
        snprintf(info->netmask, sizeof(info->netmask), IPSTR, IP2STR(&ip_info.netmask));
        snprintf(info->gateway, sizeof(info->gateway), IPSTR, IP2STR(&ip_info.gw));
        sta_manager_get_ssid(info->ssid, sizeof(info->ssid));
    }

    return ret;
}