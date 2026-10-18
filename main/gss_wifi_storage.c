#include "gss_wifi_storage.h"
#include "nvs.h"
bool gss_wifi_storage_read(void *data,size_t *size){nvs_handle_t h;if(nvs_open("gso_wifi",NVS_READONLY,&h)!=ESP_OK)return false;esp_err_t e=nvs_get_blob(h,"profile",data,size);nvs_close(h);return e==ESP_OK;}
bool gss_wifi_storage_write(const void *data,size_t size){nvs_handle_t h;if(nvs_open("gso_wifi",NVS_READWRITE,&h)!=ESP_OK)return false;esp_err_t e=nvs_set_blob(h,"profile",data,size);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e==ESP_OK;}
bool gss_wifi_storage_forget(void){nvs_handle_t h;esp_err_t e=nvs_open("gso_wifi",NVS_READWRITE,&h);if(e!=ESP_OK)return e==ESP_ERR_NVS_NOT_FOUND;e=nvs_erase_key(h,"profile");if(e==ESP_ERR_NVS_NOT_FOUND)e=ESP_OK;if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e==ESP_OK;}
