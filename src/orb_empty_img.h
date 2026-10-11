#pragma once
// A 1x1 fully transparent image, for detaching an lv_img/lv_canvas from a buffer that is
// about to be freed. LVGL accepts NULL for that too, but logs "lv_img_set_src: unknown type"
// every time, and those warnings read like a fault in the serial log when nothing is wrong.
#include <lvgl.h>
#include <string.h>

inline const lv_img_dsc_t *orb_empty_img() {
    static const uint8_t px[LV_IMG_PX_SIZE_ALPHA_BYTE] = { 0 };
    static lv_img_dsc_t d = [] {
        lv_img_dsc_t x;
        memset(&x, 0, sizeof(x));
        x.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        x.header.w = 1;
        x.header.h = 1;
        x.data_size = sizeof(px);
        x.data = px;
        return x;
    }();
    return &d;
}
