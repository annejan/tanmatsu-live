#include "app_store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static char const TAG[] = "store";

static wl_handle_t          wl_handle = WL_INVALID_HANDLE;
static sd_pwr_ctrl_handle_t sd_pwr    = NULL;
static bool                 mounted   = false;
static char const*          root      = NULL;   // "/sd" or "/int"
static char const*          where     = "none";

// SDMMC needs its DMA buffer in internal RAM
static DRAM_DMA_ALIGNED_ATTR uint8_t sd_dma_buf[512 * 4];

static void slot_path(int slot, char* out, size_t len) {
    snprintf(out, len, "%s/live/set%d.txt", root, slot);
}

// Pins and the power control sequence follow the other Tanmatsu apps, which
// power cycle the card so it is not left in a mode from a previous session.
static bool mount_sd(void) {
    sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = 4};
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &sd_pwr) != ESP_OK) {
        return false;
    }
    sd_pwr_ctrl_set_io_voltage(sd_pwr, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    sd_pwr_ctrl_set_io_voltage(sd_pwr, 3300);
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,  // never reformat someone's card
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host       = SDMMC_HOST_DEFAULT();
    host.slot               = SDMMC_HOST_SLOT_0;
    host.max_freq_khz       = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle    = sd_pwr;
    host.dma_aligned_buffer = sd_dma_buf;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk                 = GPIO_NUM_43;
    slot.cmd                 = GPIO_NUM_44;
    slot.d0                  = GPIO_NUM_39;
    slot.d1                  = GPIO_NUM_40;
    slot.d2                  = GPIO_NUM_41;
    slot.d3                  = GPIO_NUM_42;
    slot.width               = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t* card = NULL;
    if (esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &cfg, &card) != ESP_OK) {
        sd_pwr_ctrl_del_on_chip_ldo(sd_pwr);
        sd_pwr = NULL;
        return false;
    }
    return true;
}

static bool mount_internal(void) {
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        // Never true. This partition is the launcher's, and holds the metadata
        // and icons of every installed app; reformatting it on a failed mount
        // would throw all of that away to save a text file.
        .format_if_mount_failed = false,
        .allocation_unit_size   = CONFIG_WL_SECTOR_SIZE,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl("/int", "locfd", &cfg, &wl_handle) == ESP_OK;
}

esp_err_t app_store_init(void) {
    // Internal first: a clean Tanmatsu has nothing else, so this is the one
    // place sets are always where you left them. A card is mounted too when
    // present, which is what makes carrying a set between badges possible.
    if (mount_internal()) {
        root  = "/int";
        where = "int";
    } else if (mount_sd()) {
        root  = "/sd";
        where = "sd";
    } else {
        ESP_LOGW(TAG, "No storage available");
        return ESP_FAIL;
    }

    mounted = true;
    char dir[32];
    snprintf(dir, sizeof(dir), "%s/live", root);
    mkdir(dir, 0777);  // fails harmlessly when it already exists
    ESP_LOGI(TAG, "Sets live in %s", dir);
    return ESP_OK;
}

char const* app_store_where(void) {
    return where;
}

bool app_store_available(void) {
    return mounted;
}

bool app_store_save(int slot, char const* text) {
    if (!mounted || slot < 0 || slot >= APP_STORE_SLOTS || !text) {
        return false;
    }
    char path[64];
    slot_path(slot, path, sizeof(path));

    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Could not open %s for writing", path);
        return false;
    }
    size_t len     = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Short write to %s", path);
        return false;
    }
    return true;
}

bool app_store_load(int slot, char* out, size_t len) {
    if (!mounted || slot < 0 || slot >= APP_STORE_SLOTS || !out || len == 0) {
        return false;
    }
    char path[64];
    slot_path(slot, path, sizeof(path));

    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(out, 1, len - 1, f);
    fclose(f);
    out[n] = 0;
    return n > 0;
}

bool app_store_label(int slot, char* out, size_t len) {
    if (!mounted || slot < 0 || slot >= APP_STORE_SLOTS || !out || len == 0) {
        return false;
    }
    char path[64];
    slot_path(slot, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char   line[96];
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    line[n] = 0;

    char* nl = strchr(line, '\n');
    if (nl) {
        *nl = 0;
    }
    // The first line is usually the set's own comment, which makes a good name
    char* p = line;
    while (*p == '#' || *p == ' ') {
        p++;
    }
    if (*p == 0) {
        p = line;
    }
    snprintf(out, len, "%s", p);
    return n > 0;
}

bool app_store_exists(int slot) {
    if (!mounted || slot < 0 || slot >= APP_STORE_SLOTS) {
        return false;
    }
    char path[64];
    slot_path(slot, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}
