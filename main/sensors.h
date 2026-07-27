#pragma once

#include <stddef.h>
#include <stdint.h>

extern float g_temp_c[2];
extern float g_temp_f[2];

void sensors_init(void);
void sensors_task(void *arg);
uint32_t sensors_seconds_since_update(void);
uint8_t sensors_update_progress_percent(void);
size_t sensors_count(void);
uint64_t sensors_address(size_t index);
