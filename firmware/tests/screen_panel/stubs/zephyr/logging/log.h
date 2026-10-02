#pragma once
#define LOG_MODULE_REGISTER(n) _Static_assert(1, "log mock")
#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
