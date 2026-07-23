/* app.h — orchestration: load config -> init -> join -> poll/encode/send/sleep. */
#pragma once
#include <stdbool.h>
#include "config.h"

void         app_setup(void);        /* load config, init HAL, join network */
bool         app_loop_once(void);    /* one poll -> encode -> uplink cycle; true to continue */
sq_config_t *app_config(void);       /* live config (for provisioning demos) */
