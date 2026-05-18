#pragma once

#include <stdint.h>
#include <stdbool.h>

void ads1292_init(void);

bool ads1292_read_sample(int16_t *ch1_uv, int16_t *ch2_uv);