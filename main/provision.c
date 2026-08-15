#include "provision.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cat_bg.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdcard.h"

static const char *TAG = "provision";

#define WORLD_BYTES ((size_t)BG_WORLD_W * BG_STRIP_H * sizeof(uint16_t))
#define CHUNK 4096

static bool file_is_complete(const char *path, size_t want)
{
    struct stat st;
    return stat(path, &st) == 0 && (size_t)st.st_size == want;
}

// Fetch one file to a temporary name, then rename: a fetch cut off halfway
// must never leave a plausible-looking world behind.
static bool fetch(const char *url, const char *path, size_t want)
{
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.part", path);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = CHUNK,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) {
        return false;
    }
    bool ok = false;
    FILE *f = NULL;
    uint8_t *buf = NULL;

    if (esp_http_client_open(cl, 0) != ESP_OK) {
        ESP_LOGW(TAG, "cannot reach %s", url);
        goto done;
    }
    {
        const int len = esp_http_client_fetch_headers(cl);
        const int code = esp_http_client_get_status_code(cl);
        if (code != 200) {
            ESP_LOGW(TAG, "%s -> HTTP %d", url, code);
            goto done;
        }
        if (len > 0 && (size_t)len != want) {
            ESP_LOGW(TAG, "%s is %d bytes, expected %u", url, len,
                     (unsigned)want);
            goto done;
        }
    }

    f = fopen(tmp, "wb");
    buf = malloc(CHUNK);
    if (!f || !buf) {
        ESP_LOGW(TAG, "no room to receive %s", path);
        goto done;
    }

    size_t got = 0;
    int last_pct = -10;
    while (got < want) {
        const int n = esp_http_client_read(cl, (char *)buf, CHUNK);
        if (n <= 0) {
            break;
        }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            ESP_LOGW(TAG, "card write failed at %u bytes", (unsigned)got);
            goto done;
        }
        got += (size_t)n;
        const int pct = (int)(got * 100 / want);
        if (pct >= last_pct + 20) {
            last_pct = pct;
            ESP_LOGI(TAG, "  %s %d%%", path, pct);
        }
    }
    fclose(f);
    f = NULL;

    if (got != want) {
        ESP_LOGW(TAG, "%s stopped at %u of %u bytes", path, (unsigned)got,
                 (unsigned)want);
        remove(tmp);
        goto done;
    }
    remove(path);
    ok = (rename(tmp, path) == 0);
    if (!ok) {
        ESP_LOGW(TAG, "cannot rename %s into place", tmp);
    }

done:
    if (f) {
        fclose(f);
        remove(tmp);
    }
    free(buf);
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    return ok;
}

bool provision_run(void)
{
    if (!sdcard_ready()) {
        return false;
    }
    if (PROVISION_URL[0] == '\0') {
        return false;
    }

    bool wrote = false;
    for (int v = 0; v < BG_VARIANTS; v++) {
        char path[64], url[160];
        snprintf(path, sizeof(path), SDCARD_DIR "/world_%d.bin", v);
        if (file_is_complete(path, WORLD_BYTES)) {
            continue;
        }
        snprintf(url, sizeof(url), "%s/world_%d.bin", PROVISION_URL, v);
        ESP_LOGI(TAG, "fetching world %d from %s", v, url);
        if (fetch(url, path, WORLD_BYTES)) {
            ESP_LOGI(TAG, "world %d installed", v);
            wrote = true;
        }
    }
    return wrote;
}
