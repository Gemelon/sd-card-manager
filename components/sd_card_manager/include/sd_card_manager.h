#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char message[128];
} sd_card_manager_status_t;

/* Registers the authenticated UI extension and starts/retries non-destructive mounts. */
esp_err_t sd_card_manager_init(void);
void sd_card_manager_get_status(sd_card_manager_status_t *status);
