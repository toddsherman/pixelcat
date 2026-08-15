#include "wifi_time.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "rtc.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

static const char *TAG = "wifi_time";

static EventGroupHandle_t s_events;
static volatile bool s_synced;
#define BIT_CONNECTED BIT0
#define BIT_FAILED BIT1

bool wifi_time_synced(void)
{
    return s_synced;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        static int retries = 0;
        if (retries++ < 3) {
            esp_wifi_connect();
        } else {
            retries = 0;
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

static bool s_stack_up;

static void wifi_time_task(void *arg)
{
    (void)arg;

    s_events = xEventGroupCreate();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        goto out;
    }

    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) {
        goto out;
    }
    esp_netif_create_default_wifi_sta();

    {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&init_cfg) != ESP_OK) {
            goto out;
        }
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);

        wifi_config_t cfg = {0};
        strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
        strncpy((char *)cfg.sta.password, WIFI_PASS, sizeof(cfg.sta.password) - 1);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        esp_wifi_start();
    }

    {
        const EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(20000));
        if (!(bits & BIT_CONNECTED)) {
            ESP_LOGW(TAG, "no Wi-Fi connection; RTC keeps its own time");
            esp_wifi_stop();
            goto out;
        }
    }
    ESP_LOGI(TAG, "connected, syncing NTP");

    {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp_cfg);
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
            // Pacific local time with automatic DST.
            setenv("TZ", "PST8PDT,M3.2.0/2,M11.1.0/2", 1);
            tzset();
            s_synced = true;
            time_t now = time(NULL);
            struct tm lt;
            localtime_r(&now, &lt);
            if (pcf_set_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                              lt.tm_hour, lt.tm_min, lt.tm_sec) == ESP_OK) {
                ESP_LOGI(TAG, "RTC synced from NTP: %04d-%02d-%02d %02d:%02d",
                         lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                         lt.tm_hour, lt.tm_min);
            }
        } else {
            ESP_LOGW(TAG, "NTP timed out; RTC keeps its own time");
        }
        esp_netif_sntp_deinit();
    }

    esp_wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi off");

out:
    // Re-sync every 30 minutes so RTC drift and any missed boot sync can
    // never leave the scene schedule more than moments off; a failed round
    // retries sooner.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000));
        xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
        if (esp_wifi_start() != ESP_OK) {
            continue;
        }
        const EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(20000));
        if (bits & BIT_CONNECTED) {
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sntp_cfg);
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
                s_synced = true;
                time_t now = time(NULL);
                struct tm lt;
                localtime_r(&now, &lt);
                if (pcf_set_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                                  lt.tm_hour, lt.tm_min, lt.tm_sec) == ESP_OK) {
                    ESP_LOGI(TAG, "periodic RTC re-sync: %02d:%02d",
                             lt.tm_hour, lt.tm_min);
                }
            }
            esp_netif_sntp_deinit();
        }
        esp_wifi_stop();
    }
}

esp_err_t wifi_time_start(void)
{
    if (WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "no wifi_secrets.h configured; skipping NTP sync");
        return ESP_ERR_NOT_FOUND;
    }
    return (xTaskCreate(wifi_time_task, "wifi_time", 4096, NULL, 3, NULL) == pdPASS)
               ? ESP_OK
               : ESP_FAIL;
}
