#include "camera_res.h"

#include <string.h>

const char *const camera_res_names[CAMERA_RES_COUNT] = {"640x480", "1280x720", "1600x1200"};

int camera_res_parse(const char *s, size_t len)
{
    for (int i = 0; i < CAMERA_RES_COUNT; i++) {
        if (strlen(camera_res_names[i]) == len && !memcmp(s, camera_res_names[i], len)) {
            return i;
        }
    }
    return -1;
}
