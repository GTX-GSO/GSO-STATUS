#pragma once
#include <stddef.h>
#include <stdbool.h>
bool gss_wifi_storage_read(void *data,size_t *size);
bool gss_wifi_storage_write(const void *data,size_t size);
bool gss_wifi_storage_forget(void);
