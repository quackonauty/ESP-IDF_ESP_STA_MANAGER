#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ============================================================================
     * Events
     * ========================================================================= */

    typedef enum
    {
        STA_MGR_EVENT_PROV_START,
        STA_MGR_EVENT_PROV_CRED_RECV,
        STA_MGR_EVENT_PROV_CRED_SUCCESS,
        STA_MGR_EVENT_PROV_CRED_FAIL,
        STA_MGR_EVENT_PROV_END,
        STA_MGR_EVENT_STA_START,
        STA_MGR_EVENT_STA_CONNECTING,
        STA_MGR_EVENT_STA_CONNECTED,
        STA_MGR_EVENT_STA_DISCONNECTED,
        STA_MGR_EVENT_STA_GOT_IP,
#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_BLE
        STA_MGR_EVENT_BLE_CONNECTED,
        STA_MGR_EVENT_BLE_DISCONNECTED,
#endif
#ifdef CONFIG_ESP_STA_MGR_PROV_TRANSPORT_SOFTAP
        STA_MGR_EVENT_AP_CLIENT_CONNECTED,
        STA_MGR_EVENT_AP_CLIENT_DISCONNECTED,
#endif
    } sta_mgr_event_t;

    typedef struct
    {
        char ssid[33];
    } sta_mgr_prov_cred_t;

    typedef struct
    {
        const char *reason;
    } sta_mgr_prov_fail_t;

    typedef struct
    {
        char ip[16];
        char netmask[16];
        char gateway[16];
        char ssid[33];
    } sta_mgr_ip_info_t;

    /** @brief Event callback — fired for every STA Manager event. NULL to disable. */
    typedef void (*sta_mgr_event_cb_t)(void *user_data, sta_mgr_event_t event, void *event_data);

    /* ============================================================================
     * Configuration
     * ========================================================================= */

#define STA_MGR_MAX_RETRY_DEFAULT 5

    /**
     * @brief STA Manager configuration.
     *
     * @note All pointers must reference persistent memory valid for the
     *       component lifetime (static/global or string literals).
     *
     * @note When ESP_STA_MGR_SERVICE_NAME_APPEND_MAC is enabled, service_name
     *       is a prefix; 6 MAC hex chars are appended automatically.
     *
     * @note Credential buffers (sec1_pop, sec2_salt, sec2_verifier) are owned
     *       by the caller and never copied — declare them static const in app_main.
     *       Generate Sec2 values with mfg_flash_dev_creds.py.
     */
    typedef struct
    {
        sta_mgr_event_cb_t event_cb;
        void *user_data;

        const char *service_name;
        bool append_mac_suffix;

#ifdef CONFIG_ESP_STA_MGR_PROV_SECURITY_1
        const char *sec1_pop;
#endif
#ifdef CONFIG_ESP_STA_MGR_PROV_SECURITY_2
        const char *sec2_salt;
        uint16_t sec2_salt_len;
        const char *sec2_verifier;
        uint16_t sec2_verifier_len;
#endif

    } sta_manager_config_t;

    /**
     * @brief Default configuration — assign credentials before sta_manager_init().
     *
     * @code
     *   // Security 1:
     *   sta_manager_config_t cfg = STA_MANAGER_CONFIG_DEFAULT();
     *   cfg.event_cb  = my_cb;
     *   cfg.sec1_pop  = "abcd1234";
     *
     *   // Security 2:
     *   sta_manager_config_t cfg = STA_MANAGER_CONFIG_DEFAULT();
     *   cfg.event_cb          = my_cb;
     *   cfg.sec2_salt         = s_sec2_salt;
     *   cfg.sec2_salt_len     = sizeof(s_sec2_salt);
     *   cfg.sec2_verifier     = s_sec2_verifier;
     *   cfg.sec2_verifier_len = sizeof(s_sec2_verifier);
     * @endcode
     */
#ifdef CONFIG_ESP_STA_MGR_PROV_SECURITY_1
#define STA_MANAGER_CONFIG_DEFAULT() {      \
    .event_cb = NULL,                       \
    .user_data = NULL,                      \
    .service_name = NULL,                   \
    .append_mac_suffix = false,             \
    .sec1_pop = NULL,                       \
    .max_retry = STA_MGR_MAX_RETRY_DEFAULT, \
}
#elif defined(CONFIG_ESP_STA_MGR_PROV_SECURITY_2)
#define STA_MANAGER_CONFIG_DEFAULT() { \
    .event_cb = NULL,                  \
    .user_data = NULL,                 \
    .service_name = NULL,              \
    .append_mac_suffix = false,        \
    .sec2_salt = NULL,                 \
    .sec2_salt_len = 0,                \
    .sec2_verifier = NULL,             \
    .sec2_verifier_len = 0,            \
}
#endif

    /* ============================================================================
     * API
     * ========================================================================= */

    /**
     * @brief Initialize STA Manager.
     *
     * Sets up TCP/IP stack, event loop, WiFi driver, and provisioning manager.
     * Must be called before sta_manager_start(). NVS must be initialized first.
     *
     * @note Sec1 requires non-NULL sec1_pop.
     * @note Sec2 requires non-NULL sec2_salt/sec2_verifier with non-zero lengths.
     *
     * @param config  Configuration struct. NULL uses defaults (invalid for Sec1/Sec2).
     * @return ESP_OK or ESP_ERR_INVALID_ARG
     */
    esp_err_t sta_manager_init(const sta_manager_config_t *config);

    /**
     * @brief Start provisioning or connect to a saved network.
     *
     * Non-blocking — use sta_manager_wait_connected() to block until ready.
     *
     * @return ESP_OK or ESP_ERR_INVALID_STATE
     */
    esp_err_t sta_manager_start(void);

    /**
     * @brief Block until WiFi is connected and an IP is acquired.
     *
     * @param timeout_ms  Timeout in ms (0 = wait forever)
     * @return ESP_OK, ESP_ERR_TIMEOUT, or ESP_ERR_INVALID_STATE
     */
    esp_err_t sta_manager_wait_connected(uint32_t timeout_ms);

    /** @brief True if NVS contains stored WiFi credentials. */
    bool sta_manager_is_provisioned(void);

    /** @brief True if connected with a valid IP address. */
    bool sta_manager_is_connected(void);

    /**
     * @brief Erase stored credentials — device re-provisions on next boot.
     * @return ESP_OK or driver error
     */
    esp_err_t sta_manager_reset_credentials(void);

    /** @brief Release all resources. Safe to call even if never started. */
    esp_err_t sta_manager_deinit(void);

    /**
     * @brief Get the SSID of the current AP.
     *
     * @param[out] ssid  Output buffer (truncated to len-1 chars)
     * @param      len   Buffer size in bytes (min 1)
     * @return ESP_OK, ESP_ERR_INVALID_ARG, or driver error
     */
    esp_err_t sta_manager_get_ssid(char *ssid, size_t len);

    /**
     * @brief Get current IP configuration.
     *
     * @param[out] info  Filled with IP, netmask, gateway, and SSID
     * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE
     */
    esp_err_t sta_manager_get_ip_info(sta_mgr_ip_info_t *info);

    /**
     * @brief Get the full service name (with MAC suffix if enabled).
     *
     * @param[out] name  Output buffer
     * @param      max   Buffer size (min 1)
     */
    void sta_manager_get_service_name(char *name, size_t max);

#ifdef __cplusplus
}
#endif