#pragma once

extern float g_temp_c[2];
extern float g_temp_f[2];

void sensors_init(void);
void sensors_task(void *arg);
