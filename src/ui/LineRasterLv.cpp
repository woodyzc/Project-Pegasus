#include "LineRasterLv.h"

// -D PEGASUS_LINE_RASTER=0 sends every line back through lv_draw_line, the
// way -D PEGASUS_LCD_DMA=0 takes the flush back to blocking: one rebuild to
// tell whether something wrong on the map is this path or not.
#ifndef PEGASUS_LINE_RASTER
#define PEGASUS_LINE_RASTER 1
#endif

static_assert(LV_COLOR_DEPTH == 16, "LineRaster writes 16-bit pixels");
static_assert(sizeof(lv_color_t) == sizeof(uint16_t), "LineRaster writes 16-bit pixels");

bool LineRasterLv_Surface(lv_draw_ctx_t *ctx, LineRasterSurface_t *out) {
    if (!PEGASUS_LINE_RASTER) {
        return false;
    }
    if (ctx == nullptr || out == nullptr || ctx->buf == nullptr || ctx->buf_area == nullptr ||
        ctx->clip_area == nullptr) {
        return false;
    }
    lv_disp_t *disp = _lv_refr_get_disp_refreshing();
    if (disp == nullptr || disp->driver == nullptr || disp->driver->set_px_cb != nullptr ||
        disp->driver->screen_transp) {
        return false;
    }
    lv_area_t clip;
    if (!_lv_area_intersect(&clip, ctx->clip_area, ctx->buf_area)) {
        return false;
    }
    if (lv_draw_mask_is_any(&clip)) {
        return false;
    }

    out->pixels = static_cast<uint16_t *>(ctx->buf);
    // The stride LVGL's own blend uses (lv_draw_sw_blend.c): the buffer is
    // laid out at the width of the area being refreshed, which is not always
    // the panel's width.
    out->stride = lv_area_get_width(ctx->buf_area);
    out->origin_x = ctx->buf_area->x1;
    out->origin_y = ctx->buf_area->y1;
    out->clip_x1 = clip.x1;
    out->clip_y1 = clip.y1;
    out->clip_x2 = clip.x2;
    out->clip_y2 = clip.y2;
    out->swapped = LV_COLOR_16_SWAP != 0;
    return true;
}
