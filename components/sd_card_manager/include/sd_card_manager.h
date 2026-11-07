/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool mounted;
    bool free_space_known;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char message[128];
} sd_card_manager_status_t;

typedef struct {
    bool spi;
    bool enabled;
    unsigned frequency_khz;
    int card_detect_gpio;
    int clk_gpio, cmd_gpio, d0_gpio, d1_gpio, d2_gpio, d3_gpio;
    int spi_miso_gpio, spi_mosi_gpio, spi_cs_gpio, spi_clk_gpio;
    char mount_point[64];
} sd_card_manager_config_t;

esp_err_t sd_card_manager_set_config(const sd_card_manager_config_t *config);
/* Registers the authenticated UI extension and starts/retries non-destructive mounts. */
esp_err_t sd_card_manager_init(void);
void sd_card_manager_get_status(sd_card_manager_status_t *status);
/* Acquires the shared card lock only while the SD card is mounted. */
esp_err_t sd_card_manager_storage_acquire(TickType_t timeout_ticks);
void sd_card_manager_storage_release(void);

#ifdef __cplusplus
}
#endif
