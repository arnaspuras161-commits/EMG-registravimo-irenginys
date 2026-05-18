#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int16_t gx, gy, gz;
    int16_t ax, ay, az;
} lsm6dsox_data_t;

bool lsm6dsox_init(void);
bool lsm6dsox_read(lsm6dsox_data_t *data);