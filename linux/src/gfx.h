#ifndef DK_GFX_H
#define DK_GFX_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DK_FONT_REGULAR = 0,
    DK_FONT_BOLD,
    DK_FONT_ITALIC,
    DK_FONT_BOLD_ITALIC,
    DK_FONT_MONO,
    DK_FONT_MONO_BOLD,
    DK_FONT_COUNT
} DkFontStyle;

typedef enum {
    DK_ICON_BRAND = 0,
    DK_ICON_SIDEBAR,
    DK_ICON_PLUS,
    DK_ICON_TRASH,
    DK_ICON_KEY,
    DK_ICON_SEND,
    DK_ICON_STOP,
    DK_ICON_COPY,
    DK_ICON_CHECK,
    DK_ICON_CHEVRON_DOWN,
    DK_ICON_CLOSE,
    DK_ICON_EYE,
} DkIcon;

typedef struct {
    int x, y, w, h;
} DkRect;

typedef struct {
    int ascent;
    int descent;
    int line_height;
} DkFontMetrics;

typedef struct DkGfx DkGfx;

/* Helper macro: 0xAARRGGBB */
#define DK_RGBA(r, g, b, a) \
    ((((uint32_t)(a) & 0xFFu) << 24) | (((uint32_t)(r) & 0xFFu) << 16) | \
     (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(b) & 0xFFu))
#define DK_RGB(r, g, b) DK_RGBA(r, g, b, 255)

DkGfx *dk_gfx_create(Display *dpy, int screen, Window win, int w, int h);
void dk_gfx_destroy(DkGfx *g);
bool dk_gfx_resize(DkGfx *g, int w, int h);
void dk_gfx_present(DkGfx *g);
int dk_gfx_width(const DkGfx *g);
int dk_gfx_height(const DkGfx *g);

/* Clipping */
void dk_gfx_clip_push(DkGfx *g, DkRect r);
void dk_gfx_clip_pop(DkGfx *g);

/* Shapes */
void dk_gfx_clear(DkGfx *g, uint32_t color);
void dk_gfx_fill_rect(DkGfx *g, int x, int y, int w, int h, uint32_t color);
void dk_gfx_fill_rrect(DkGfx *g, int x, int y, int w, int h, float radius, uint32_t color);
void dk_gfx_stroke_rrect(DkGfx *g, int x, int y, int w, int h, float radius, uint32_t color);
void dk_gfx_fill_circle(DkGfx *g, float cx, float cy, float radius, uint32_t color);
void dk_gfx_draw_line(DkGfx *g, float x0, float y0, float x1, float y1, float thickness, uint32_t color);
void dk_gfx_draw_icon(DkGfx *g, DkIcon icon, int x, int y, int size, uint32_t color);

/* Text */
DkFontMetrics dk_gfx_font_metrics(DkGfx *g, DkFontStyle style, int size_px);
int dk_gfx_char_advance(DkGfx *g, DkFontStyle style, int size_px, uint32_t cp);
int dk_gfx_text_width(DkGfx *g, DkFontStyle style, int size_px, const char *s, size_t len);
int dk_gfx_draw_text(DkGfx *g, int x, int baseline_y, DkFontStyle style, int size_px,
                     uint32_t color, const char *s, size_t len);
void dk_gfx_draw_text_ellipsize(DkGfx *g, int x, int baseline_y, int max_w,
                                DkFontStyle style, int size_px, uint32_t color,
                                const char *s, size_t len);

#endif
