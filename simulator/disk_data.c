#include "disk_data.h"
#ifdef _WIN32
#include <windows.h>
#endif
bool desk_disk_space(char letter,uint64_t *total,uint64_t *free_bytes){
#ifdef _WIN32
    char path[]={letter,':','\\',0};ULARGE_INTEGER capacity,free_space;
    if(!GetDiskFreeSpaceExA(path,NULL,&capacity,&free_space))return false;
    *total=capacity.QuadPart;*free_bytes=free_space.QuadPart;return *total>0;
#else
    (void)letter;(void)total;(void)free_bytes;return false;
#endif
}
