#define _POSIX_C_SOURCE 200809L

#include "gfx.h"
#include "str.h"

#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define CLIP_STACK_MAX 16
#define GLYPH_TABLE_SIZE 4096 /* power of 2 */
#define GLYPH_TABLE_MASK (GLYPH_TABLE_SIZE - 1)
#define MAX_FALLBACK_FACES 16
#define ARENA_BLOCK_SIZE (128 * 1024)

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    size_t cap;
    uint8_t data[];
} ArenaBlock;

typedef struct {
    uint64_t key; /* 0 = empty; high bit set for occupied */
    int16_t bearing_x;
    int16_t bearing_y;
    uint16_t width;
    uint16_t height;
    int16_t advance;
    uint8_t *bitmap;
} GlyphEntry;

typedef struct {
    FT_Face face;
    int current_size;
} FaceSlot;

struct DkGfx {
    Display *dpy;
    int screen;
    Window win;
    GC gc;
    Visual *visual;
    int depth;

    int w, h;
    uint32_t *pixels;
    XImage *ximg;
    XShmSegmentInfo shm_info;
    bool use_shm;
    bool shm_attached;

    DkRect clip_stack[CLIP_STACK_MAX];
    int clip_top;

    FT_Library ft;
    FcConfig *fc;
    FaceSlot primary[DK_FONT_COUNT];
    FaceSlot fallbacks[MAX_FALLBACK_FACES];
    int fallback_count;

    GlyphEntry glyphs[GLYPH_TABLE_SIZE];
    size_t glyph_used;
    ArenaBlock *arena;
};

static uint8_t *arena_alloc(DkGfx *g, size_t n) {
    if (n == 0) return NULL;
    if (!g->arena || g->arena->used + n > g->arena->cap) {
        size_t cap = n > ARENA_BLOCK_SIZE ? n : ARENA_BLOCK_SIZE;
        ArenaBlock *b = malloc(sizeof(ArenaBlock) + cap);
        if (!b) return NULL;
        b->next = g->arena;
        b->used = 0;
        b->cap = cap;
        g->arena = b;
    }
    uint8_t *p = g->arena->data + g->arena->used;
    g->arena->used += n;
    return p;
}

static void arena_free_all(DkGfx *g) {
    ArenaBlock *b = g->arena;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    g->arena = NULL;
}

static inline uint32_t blend_pixel(uint32_t dst, uint32_t src, uint32_t a) {
    if (a == 0) return dst;
    if (a >= 255) return src | 0xFF000000u;
    uint32_t inv = 255u - a;
    uint32_t rb = (((src & 0x00FF00FFu) * a + (dst & 0x00FF00FFu) * inv + 0x00800080u) >> 8) & 0x00FF00FFu;
    uint32_t g  = (((src & 0x0000FF00u) * a + (dst & 0x0000FF00u) * inv + 0x00008000u) >> 8) & 0x0000FF00u;
    return 0xFF000000u | rb | g;
}

static inline void put_pixel_alpha(DkGfx *g, int x, int y, uint32_t color, uint32_t alpha) {
    DkRect c = g->clip_stack[g->clip_top];
    if (x < c.x || x >= c.x + c.w || y < c.y || y >= c.y + c.h)
        return;
    uint32_t ca = (color >> 24) & 0xFFu;
    uint32_t a = (alpha * ca + 127u) / 255u;
    if (a == 0) return;
    uint32_t *dst = &g->pixels[(size_t)y * (size_t)g->w + (size_t)x];
    *dst = blend_pixel(*dst, color, a);
}

static void free_surface(DkGfx *g) {
    if (g->ximg) {
        if (g->shm_attached) {
            XShmDetach(g->dpy, &g->shm_info);
            XSync(g->dpy, False);
            shmdt(g->shm_info.shmaddr);
            g->shm_attached = false;
            g->ximg->data = NULL;
        }
        XDestroyImage(g->ximg);
        g->ximg = NULL;
        g->pixels = NULL;
    }
}

static bool alloc_surface(DkGfx *g, int w, int h) {
    free_surface(g);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    g->w = w;
    g->h = h;

    if (g->use_shm) {
        memset(&g->shm_info, 0, sizeof(g->shm_info));
        g->ximg = XShmCreateImage(g->dpy, g->visual, (unsigned)g->depth,
                                  ZPixmap, NULL, &g->shm_info, (unsigned)w, (unsigned)h);
        if (g->ximg) {
            size_t sz = (size_t)g->ximg->bytes_per_line * (size_t)h;
            g->shm_info.shmid = shmget(IPC_PRIVATE, sz, IPC_CREAT | 0600);
            if (g->shm_info.shmid >= 0) {
                g->shm_info.shmaddr = shmat(g->shm_info.shmid, NULL, 0);
                if (g->shm_info.shmaddr != (char *)-1) {
                    g->ximg->data = g->shm_info.shmaddr;
                    g->shm_info.readOnly = False;
                    if (XShmAttach(g->dpy, &g->shm_info)) {
                        XSync(g->dpy, False);
                        shmctl(g->shm_info.shmid, IPC_RMID, NULL);
                        g->shm_attached = true;
                        g->pixels = (uint32_t *)g->shm_info.shmaddr;
                        g->clip_stack[0] = (DkRect){ 0, 0, w, h };
                        g->clip_top = 0;
                        return true;
                    }
                    shmdt(g->shm_info.shmaddr);
                }
                shmctl(g->shm_info.shmid, IPC_RMID, NULL);
            }
            g->ximg->data = NULL;
            XDestroyImage(g->ximg);
            g->ximg = NULL;
        }
        g->use_shm = false;
    }

    size_t bytes = (size_t)w * (size_t)h * 4;
    char *buf = calloc(1, bytes);
    if (!buf) return false;
    g->ximg = XCreateImage(g->dpy, g->visual, (unsigned)g->depth, ZPixmap, 0,
                           buf, (unsigned)w, (unsigned)h, 32, w * 4);
    if (!g->ximg) {
        free(buf);
        return false;
    }
    g->pixels = (uint32_t *)buf;
    g->clip_stack[0] = (DkRect){ 0, 0, w, h };
    g->clip_top = 0;
    return true;
}

static bool load_fc_face(DkGfx *g, const char *family, int weight, int slant, FaceSlot *out) {
    FcPattern *pat = FcPatternCreate();
    if (!pat) return false;
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddInteger(pat, FC_WEIGHT, weight);
    FcPatternAddInteger(pat, FC_SLANT, slant);
    FcConfigSubstitute(g->fc, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult res;
    FcPattern *match = FcFontMatch(g->fc, pat, &res);
    FcPatternDestroy(pat);
    if (!match) return false;

    FcChar8 *file = NULL;
    int index = 0;
    bool ok = false;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        if (FT_New_Face(g->ft, (const char *)file, index, &out->face) == 0) {
            out->current_size = 0;
            ok = true;
        }
    }
    FcPatternDestroy(match);
    return ok;
}

static FaceSlot *find_fallback_face_for_cp(DkGfx *g, uint32_t cp, bool mono) {
    /* Check already loaded fallback faces */
    for (int i = 0; i < g->fallback_count; i++) {
        if (g->fallbacks[i].face && FT_Get_Char_Index(g->fallbacks[i].face, cp) != 0)
            return &g->fallbacks[i];
    }
    if (g->fallback_count >= MAX_FALLBACK_FACES)
        return NULL;

    FcPattern *pat = FcPatternCreate();
    FcCharSet *cs = FcCharSetCreate();
    if (!pat || !cs) {
        if (pat) FcPatternDestroy(pat);
        if (cs) FcCharSetDestroy(cs);
        return NULL;
    }
    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)(mono ? "monospace" : "sans-serif"));
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(g->fc, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult res;
    FcPattern *match = FcFontMatch(g->fc, pat, &res);
    FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    if (!match) return NULL;

    FcChar8 *file = NULL;
    int index = 0;
    FaceSlot *ret = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        FT_Face face = NULL;
        if (FT_New_Face(g->ft, (const char *)file, index, &face) == 0) {
            if (FT_Get_Char_Index(face, cp) != 0) {
                FaceSlot *slot = &g->fallbacks[g->fallback_count++];
                slot->face = face;
                slot->current_size = 0;
                ret = slot;
            } else {
                FT_Done_Face(face);
            }
        }
    }
    FcPatternDestroy(match);
    return ret;
}

static void set_face_size(FaceSlot *slot, int size_px) {
    if (!slot || !slot->face) return;
    if (slot->current_size != size_px) {
        FT_Set_Pixel_Sizes(slot->face, 0, (FT_UInt)size_px);
        slot->current_size = size_px;
    }
}

static uint64_t glyph_key(DkFontStyle style, int size_px, uint32_t cp) {
    return (1ULL << 63) | ((uint64_t)(style & 0xF) << 40) |
           ((uint64_t)(size_px & 0xFF) << 32) | (uint64_t)cp;
}

static uint32_t hash_u64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return (uint32_t)x;
}

static const GlyphEntry *get_glyph(DkGfx *g, DkFontStyle style, int size_px, uint32_t cp) {
    if (style >= DK_FONT_COUNT) style = DK_FONT_REGULAR;
    if (size_px < 6) size_px = 6;
    if (size_px > 96) size_px = 96;

    uint64_t key = glyph_key(style, size_px, cp);
    uint32_t idx = hash_u64(key) & GLYPH_TABLE_MASK;

    for (uint32_t probe = 0; probe < GLYPH_TABLE_SIZE; probe++) {
        GlyphEntry *e = &g->glyphs[(idx + probe) & GLYPH_TABLE_MASK];
        if (e->key == key)
            return e;
        if (e->key == 0) {
            /* If table is >75% full, clear cache cleanly */
            if (g->glyph_used >= (GLYPH_TABLE_SIZE * 3) / 4) {
                memset(g->glyphs, 0, sizeof(g->glyphs));
                arena_free_all(g);
                g->glyph_used = 0;
                e = &g->glyphs[idx];
            }

            FaceSlot *slot = &g->primary[style];
            FT_UInt gid = slot->face ? FT_Get_Char_Index(slot->face, cp) : 0;
            if (gid == 0) {
                bool mono = (style == DK_FONT_MONO || style == DK_FONT_MONO_BOLD);
                FaceSlot *fb = find_fallback_face_for_cp(g, cp, mono);
                if (fb && fb->face) {
                    slot = fb;
                    gid = FT_Get_Char_Index(slot->face, cp);
                }
            }
            if (!slot->face) {
                e->key = key;
                e->advance = (int16_t)(size_px / 2);
                g->glyph_used++;
                return e;
            }

            set_face_size(slot, size_px);
            if (FT_Load_Glyph(slot->face, gid, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
                e->key = key;
                e->advance = (int16_t)(size_px / 2);
                g->glyph_used++;
                return e;
            }

            FT_GlyphSlot gs = slot->face->glyph;
            FT_Bitmap *bm = &gs->bitmap;

            e->key = key;
            e->bearing_x = (int16_t)gs->bitmap_left;
            e->bearing_y = (int16_t)gs->bitmap_top;
            e->width = (uint16_t)bm->width;
            e->height = (uint16_t)bm->rows;
            e->advance = (int16_t)((gs->advance.x + 32) >> 6);
            if (cp == 0x2044 && bm->width > 0 && (e->bearing_x < 0 || e->advance < (int16_t)bm->width + 1)) {
                e->bearing_x = 1;
                e->advance = (int16_t)(bm->width + 2);
            }
            e->bitmap = NULL;

            if (bm->width > 0 && bm->rows > 0 && bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
                size_t sz = (size_t)bm->width * (size_t)bm->rows;
                uint8_t *dst = arena_alloc(g, sz);
                if (dst) {
                    int pitch = bm->pitch;
                    for (unsigned row = 0; row < bm->rows; row++) {
                        const uint8_t *src_row = pitch >= 0
                            ? bm->buffer + (size_t)row * (size_t)pitch
                            : bm->buffer + (size_t)(bm->rows - 1 - row) * (size_t)(-pitch);
                        memcpy(dst + (size_t)row * bm->width, src_row, bm->width);
                    }
                    e->bitmap = dst;
                }
            }
            g->glyph_used++;
            return e;
        }
    }
    return NULL;
}

DkGfx *dk_gfx_create(Display *dpy, int screen, Window win, int w, int h) {
    DkGfx *g = calloc(1, sizeof(DkGfx));
    if (!g) return NULL;
    g->dpy = dpy;
    g->screen = screen;
    g->win = win;
    g->visual = DefaultVisual(dpy, screen);
    g->depth = DefaultDepth(dpy, screen);
    g->gc = XCreateGC(dpy, win, 0, NULL);
    g->use_shm = XShmQueryExtension(dpy) ? true : false;

    if (!alloc_surface(g, w, h)) {
        XFreeGC(dpy, g->gc);
        free(g);
        return NULL;
    }

    if (FT_Init_FreeType(&g->ft) == 0) {
        g->fc = FcInitLoadConfigAndFonts();
        if (g->fc) {
            load_fc_face(g, "sans-serif", FC_WEIGHT_REGULAR, FC_SLANT_ROMAN, &g->primary[DK_FONT_REGULAR]);
            load_fc_face(g, "sans-serif", FC_WEIGHT_BOLD, FC_SLANT_ROMAN, &g->primary[DK_FONT_BOLD]);
            load_fc_face(g, "sans-serif", FC_WEIGHT_REGULAR, FC_SLANT_ITALIC, &g->primary[DK_FONT_ITALIC]);
            load_fc_face(g, "sans-serif", FC_WEIGHT_BOLD, FC_SLANT_ITALIC, &g->primary[DK_FONT_BOLD_ITALIC]);
            load_fc_face(g, "monospace", FC_WEIGHT_REGULAR, FC_SLANT_ROMAN, &g->primary[DK_FONT_MONO]);
            load_fc_face(g, "monospace", FC_WEIGHT_BOLD, FC_SLANT_ROMAN, &g->primary[DK_FONT_MONO_BOLD]);
        }
    }
    return g;
}

void dk_gfx_destroy(DkGfx *g) {
    if (!g) return;
    free_surface(g);
    arena_free_all(g);
    for (int i = 0; i < DK_FONT_COUNT; i++) {
        if (g->primary[i].face) FT_Done_Face(g->primary[i].face);
    }
    for (int i = 0; i < g->fallback_count; i++) {
        if (g->fallbacks[i].face) FT_Done_Face(g->fallbacks[i].face);
    }
    if (g->fc) FcConfigDestroy(g->fc);
    if (g->ft) FT_Done_FreeType(g->ft);
    if (g->gc) XFreeGC(g->dpy, g->gc);
    free(g);
}

bool dk_gfx_resize(DkGfx *g, int w, int h) {
    if (!g) return false;
    if (w == g->w && h == g->h) return true;
    return alloc_surface(g, w, h);
}

void dk_gfx_present(DkGfx *g) {
    if (!g || !g->ximg) return;
    if (g->shm_attached) {
        XShmPutImage(g->dpy, g->win, g->gc, g->ximg, 0, 0, 0, 0,
                     (unsigned)g->w, (unsigned)g->h, False);
    } else {
        XPutImage(g->dpy, g->win, g->gc, g->ximg, 0, 0, 0, 0,
                  (unsigned)g->w, (unsigned)g->h);
    }
    XFlush(g->dpy);
}

int dk_gfx_width(const DkGfx *g) { return g ? g->w : 0; }
int dk_gfx_height(const DkGfx *g) { return g ? g->h : 0; }

void dk_gfx_clip_push(DkGfx *g, DkRect r) {
    if (!g || g->clip_top + 1 >= CLIP_STACK_MAX) return;
    DkRect prev = g->clip_stack[g->clip_top];
    int x0 = r.x > prev.x ? r.x : prev.x;
    int y0 = r.y > prev.y ? r.y : prev.y;
    int x1 = (r.x + r.w) < (prev.x + prev.w) ? (r.x + r.w) : (prev.x + prev.w);
    int y1 = (r.y + r.h) < (prev.y + prev.h) ? (r.y + r.h) : (prev.y + prev.h);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    g->clip_stack[++g->clip_top] = (DkRect){ x0, y0, x1 - x0, y1 - y0 };
}

void dk_gfx_clip_pop(DkGfx *g) {
    if (g && g->clip_top > 0)
        g->clip_top--;
}

void dk_gfx_clear(DkGfx *g, uint32_t color) {
    if (!g || !g->pixels) return;
    uint32_t c = color | 0xFF000000u;
    size_t n = (size_t)g->w * (size_t)g->h;
    for (size_t i = 0; i < n; i++)
        g->pixels[i] = c;
}

void dk_gfx_fill_rect(DkGfx *g, int x, int y, int w, int h, uint32_t color) {
    if (!g || !g->pixels || w <= 0 || h <= 0) return;
    DkRect c = g->clip_stack[g->clip_top];
    int x0 = x > c.x ? x : c.x;
    int y0 = y > c.y ? y : c.y;
    int x1 = (x + w) < (c.x + c.w) ? (x + w) : (c.x + c.w);
    int y1 = (y + h) < (c.y + c.h) ? (y + h) : (c.y + c.h);
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t a = (color >> 24) & 0xFFu;
    if (a == 0) return;
    if (a == 255) {
        uint32_t solid = color | 0xFF000000u;
        for (int py = y0; py < y1; py++) {
            uint32_t *row = g->pixels + (size_t)py * (size_t)g->w;
            for (int px = x0; px < x1; px++)
                row[px] = solid;
        }
    } else {
        for (int py = y0; py < y1; py++) {
            uint32_t *row = g->pixels + (size_t)py * (size_t)g->w;
            for (int px = x0; px < x1; px++)
                row[px] = blend_pixel(row[px], color, a);
        }
    }
}

void dk_gfx_fill_rrect(DkGfx *g, int x, int y, int w, int h, float radius, uint32_t color) {
    if (!g || !g->pixels || w <= 0 || h <= 0) return;
    if (radius <= 0.5f) {
        dk_gfx_fill_rect(g, x, y, w, h, color);
        return;
    }
    float max_r = 0.5f * (float)(w < h ? w : h);
    if (radius > max_r) radius = max_r;

    int ri = (int)ceilf(radius);
    /* Fast path: interior vertical strip and middle band */
    dk_gfx_fill_rect(g, x + ri, y, w - 2 * ri, ri, color);
    dk_gfx_fill_rect(g, x, y + ri, w, h - 2 * ri, color);
    dk_gfx_fill_rect(g, x + ri, y + h - ri, w - 2 * ri, ri, color);

    /* Anti-aliased 4 corners */
    float cx[2] = { (float)x + radius, (float)(x + w) - radius };
    float cy[2] = { (float)y + radius, (float)(y + h) - radius };

    for (int corner = 0; corner < 4; corner++) {
        int sx = (corner & 1) ? (x + w - ri) : x;
        int ex = (corner & 1) ? (x + w) : (x + ri);
        int sy = (corner & 2) ? (y + h - ri) : y;
        int ey = (corner & 2) ? (y + h) : (y + ri);
        float ox = cx[corner & 1];
        float oy = cy[(corner >> 1) & 1];

        for (int py = sy; py < ey; py++) {
            float dy = ((float)py + 0.5f) - oy;
            for (int px = sx; px < ex; px++) {
                float dx = ((float)px + 0.5f) - ox;
                float dist = sqrtf(dx * dx + dy * dy) - radius;
                if (dist <= -0.5f) {
                    put_pixel_alpha(g, px, py, color, 255);
                } else if (dist < 0.5f) {
                    uint32_t cov = (uint32_t)((0.5f - dist) * 255.0f + 0.5f);
                    put_pixel_alpha(g, px, py, color, cov);
                }
            }
        }
    }
}

void dk_gfx_stroke_rrect(DkGfx *g, int x, int y, int w, int h, float radius, uint32_t color) {
    if (!g || !g->pixels || w <= 1 || h <= 1) return;
    if (radius <= 0.5f) {
        dk_gfx_fill_rect(g, x, y, w, 1, color);
        dk_gfx_fill_rect(g, x, y + h - 1, w, 1, color);
        dk_gfx_fill_rect(g, x, y + 1, 1, h - 2, color);
        dk_gfx_fill_rect(g, x + w - 1, y + 1, 1, h - 2, color);
        return;
    }
    float max_r = 0.5f * (float)(w < h ? w : h);
    if (radius > max_r) radius = max_r;
    int ri = (int)ceilf(radius);

    /* Straight edges */
    dk_gfx_fill_rect(g, x + ri, y, w - 2 * ri, 1, color);
    dk_gfx_fill_rect(g, x + ri, y + h - 1, w - 2 * ri, 1, color);
    dk_gfx_fill_rect(g, x, y + ri, 1, h - 2 * ri, color);
    dk_gfx_fill_rect(g, x + w - 1, y + ri, 1, h - 2 * ri, color);

    float cx[2] = { (float)x + radius, (float)(x + w) - radius };
    float cy[2] = { (float)y + radius, (float)(y + h) - radius };
    float r_mid = radius - 0.5f;

    for (int corner = 0; corner < 4; corner++) {
        int sx = (corner & 1) ? (x + w - ri) : x;
        int ex = (corner & 1) ? (x + w) : (x + ri);
        int sy = (corner & 2) ? (y + h - ri) : y;
        int ey = (corner & 2) ? (y + h) : (y + ri);
        float ox = cx[corner & 1];
        float oy = cy[(corner >> 1) & 1];

        for (int py = sy; py < ey; py++) {
            float dy = ((float)py + 0.5f) - oy;
            for (int px = sx; px < ex; px++) {
                float dx = ((float)px + 0.5f) - ox;
                float d = fabsf(sqrtf(dx * dx + dy * dy) - r_mid);
                if (d < 1.0f) {
                    uint32_t cov = (uint32_t)((1.0f - d) * 255.0f + 0.5f);
                    put_pixel_alpha(g, px, py, color, cov);
                }
            }
        }
    }
}

void dk_gfx_fill_circle(DkGfx *g, float cx, float cy, float radius, uint32_t color) {
    if (!g || !g->pixels || radius <= 0.0f) return;
    int x0 = (int)floorf(cx - radius - 1.0f);
    int x1 = (int)ceilf(cx + radius + 1.0f);
    int y0 = (int)floorf(cy - radius - 1.0f);
    int y1 = (int)ceilf(cy + radius + 1.0f);
    for (int py = y0; py <= y1; py++) {
        float dy = ((float)py + 0.5f) - cy;
        for (int px = x0; px <= x1; px++) {
            float dx = ((float)px + 0.5f) - cx;
            float dist = sqrtf(dx * dx + dy * dy) - radius;
            if (dist <= -0.5f) {
                put_pixel_alpha(g, px, py, color, 255);
            } else if (dist < 0.5f) {
                uint32_t cov = (uint32_t)((0.5f - dist) * 255.0f + 0.5f);
                put_pixel_alpha(g, px, py, color, cov);
            }
        }
    }
}

void dk_gfx_draw_line(DkGfx *g, float x0, float y0, float x1, float y1, float thickness, uint32_t color) {
    if (!g || !g->pixels) return;
    float half = thickness * 0.5f;
    float pad = half + 1.2f;
    int minx = (int)floorf((x0 < x1 ? x0 : x1) - pad);
    int maxx = (int)ceilf((x0 > x1 ? x0 : x1) + pad);
    int miny = (int)floorf((y0 < y1 ? y0 : y1) - pad);
    int maxy = (int)ceilf((y0 > y1 ? y0 : y1) + pad);

    float vx = x1 - x0;
    float vy = y1 - y0;
    float v2 = vx * vx + vy * vy;

    for (int py = miny; py <= maxy; py++) {
        float ppy = (float)py + 0.5f;
        for (int px = minx; px <= maxx; px++) {
            float ppx = (float)px + 0.5f;
            float t = v2 > 1e-6f ? ((ppx - x0) * vx + (ppy - y0) * vy) / v2 : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float dx = ppx - (x0 + t * vx);
            float dy = ppy - (y0 + t * vy);
            float dist = sqrtf(dx * dx + dy * dy) - half;
            if (dist <= -0.5f) {
                put_pixel_alpha(g, px, py, color, 255);
            } else if (dist < 0.5f) {
                uint32_t cov = (uint32_t)((0.5f - dist) * 255.0f + 0.5f);
                put_pixel_alpha(g, px, py, color, cov);
            }
        }
    }
}

void dk_gfx_draw_icon(DkGfx *g, DkIcon icon, int x, int y, int size, uint32_t color) {
    float s = (float)size;
    float fx = (float)x;
    float fy = (float)y;
    float t = s * 0.11f;
    if (t < 1.4f) t = 1.4f;

    switch (icon) {
    case DK_ICON_BRAND: {
        /* Signature 3 slanted DKChatebras bars */
        float bw = s * 0.19f;
        float gap = s * 0.28f;
        float skew = s * 0.12f;
        float cx = fx + s * 0.5f;
        float cy = fy + s * 0.5f;
        dk_gfx_draw_line(g, cx - gap + skew * 0.5f, cy - s * 0.22f, cx - gap - skew * 0.5f, cy + s * 0.22f, bw, color);
        dk_gfx_draw_line(g, cx + skew, cy - s * 0.42f, cx - skew, cy + s * 0.42f, bw, color);
        dk_gfx_draw_line(g, cx + gap + skew * 0.7f, cy - s * 0.30f, cx + gap - skew * 0.7f, cy + s * 0.30f, bw, color);
        break;
    }
    case DK_ICON_SIDEBAR: {
        dk_gfx_stroke_rrect(g, x + 1, y + 2, size - 2, size - 4, 2.5f, color);
        int div_x = x + (int)(s * 0.36f);
        dk_gfx_fill_rect(g, div_x, y + 3, 1, size - 6, color);
        break;
    }
    case DK_ICON_PLUS: {
        float m = s * 0.22f;
        dk_gfx_draw_line(g, fx + s * 0.5f, fy + m, fx + s * 0.5f, fy + s - m, t, color);
        dk_gfx_draw_line(g, fx + m, fy + s * 0.5f, fx + s - m, fy + s * 0.5f, t, color);
        break;
    }
    case DK_ICON_TRASH: {
        dk_gfx_draw_line(g, fx + s * 0.18f, fy + s * 0.26f, fx + s * 0.82f, fy + s * 0.26f, t, color);
        dk_gfx_draw_line(g, fx + s * 0.38f, fy + s * 0.16f, fx + s * 0.62f, fy + s * 0.16f, t, color);
        dk_gfx_draw_line(g, fx + s * 0.26f, fy + s * 0.28f, fx + s * 0.30f, fy + s * 0.84f, t, color);
        dk_gfx_draw_line(g, fx + s * 0.74f, fy + s * 0.28f, fx + s * 0.70f, fy + s * 0.84f, t, color);
        dk_gfx_draw_line(g, fx + s * 0.30f, fy + s * 0.84f, fx + s * 0.70f, fy + s * 0.84f, t, color);
        dk_gfx_draw_line(g, fx + s * 0.43f, fy + s * 0.42f, fx + s * 0.44f, fy + s * 0.70f, t * 0.85f, color);
        dk_gfx_draw_line(g, fx + s * 0.57f, fy + s * 0.42f, fx + s * 0.56f, fy + s * 0.70f, t * 0.85f, color);
        break;
    }
    case DK_ICON_KEY: {
        float cx = fx + s * 0.34f;
        float cy = fy + s * 0.5f;
        float r = s * 0.20f;
        /* Ring via 8 short segments */
        for (int i = 0; i < 12; i++) {
            float a0 = (float)i * (6.283185f / 12.0f);
            float a1 = (float)(i + 1) * (6.283185f / 12.0f);
            dk_gfx_draw_line(g, cx + cosf(a0) * r, cy + sinf(a0) * r,
                             cx + cosf(a1) * r, cy + sinf(a1) * r, t * 0.9f, color);
        }
        dk_gfx_draw_line(g, cx + r, cy, fx + s * 0.88f, cy, t, color);
        dk_gfx_draw_line(g, fx + s * 0.72f, cy, fx + s * 0.72f, cy + s * 0.16f, t, color);
        dk_gfx_draw_line(g, fx + s * 0.86f, cy, fx + s * 0.86f, cy + s * 0.14f, t, color);
        break;
    }
    case DK_ICON_SEND: {
        /* Upward arrow (clean modern chat send icon) */
        float cx = fx + s * 0.5f;
        dk_gfx_draw_line(g, cx, fy + s * 0.22f, cx, fy + s * 0.78f, t * 1.15f, color);
        dk_gfx_draw_line(g, cx, fy + s * 0.22f, fx + s * 0.26f, fy + s * 0.46f, t * 1.15f, color);
        dk_gfx_draw_line(g, cx, fy + s * 0.22f, fx + s * 0.74f, fy + s * 0.46f, t * 1.15f, color);
        break;
    }
    case DK_ICON_STOP: {
        int p = (int)(s * 0.26f);
        dk_gfx_fill_rrect(g, x + p, y + p, size - 2 * p, size - 2 * p, 2.5f, color);
        break;
    }
    case DK_ICON_COPY: {
        int d = (int)(s * 0.16f);
        int bw = (int)(s * 0.56f);
        int bh = (int)(s * 0.62f);
        dk_gfx_stroke_rrect(g, x + d + (int)(s * 0.14f), y + (int)(s * 0.12f), bw, bh, 2.0f, color);
        dk_gfx_stroke_rrect(g, x + (int)(s * 0.14f), y + d + (int)(s * 0.14f), bw, bh, 2.0f, color);
        break;
    }
    case DK_ICON_CHECK: {
        dk_gfx_draw_line(g, fx + s * 0.20f, fy + s * 0.52f, fx + s * 0.42f, fy + s * 0.74f, t * 1.1f, color);
        dk_gfx_draw_line(g, fx + s * 0.42f, fy + s * 0.74f, fx + s * 0.82f, fy + s * 0.28f, t * 1.1f, color);
        break;
    }
    case DK_ICON_CHEVRON_DOWN: {
        dk_gfx_draw_line(g, fx + s * 0.24f, fy + s * 0.38f, fx + s * 0.50f, fy + s * 0.64f, t, color);
        dk_gfx_draw_line(g, fx + s * 0.50f, fy + s * 0.64f, fx + s * 0.76f, fy + s * 0.38f, t, color);
        break;
    }
    case DK_ICON_CLOSE: {
        float m = s * 0.26f;
        dk_gfx_draw_line(g, fx + m, fy + m, fx + s - m, fy + s - m, t, color);
        dk_gfx_draw_line(g, fx + s - m, fy + m, fx + m, fy + s - m, t, color);
        break;
    }
    case DK_ICON_EYE: {
        float cy = fy + s * 0.5f;
        dk_gfx_draw_line(g, fx + s * 0.14f, cy, fx + s * 0.50f, fy + s * 0.26f, t * 0.9f, color);
        dk_gfx_draw_line(g, fx + s * 0.50f, fy + s * 0.26f, fx + s * 0.86f, cy, t * 0.9f, color);
        dk_gfx_draw_line(g, fx + s * 0.14f, cy, fx + s * 0.50f, fy + s * 0.74f, t * 0.9f, color);
        dk_gfx_draw_line(g, fx + s * 0.50f, fy + s * 0.74f, fx + s * 0.86f, cy, t * 0.9f, color);
        dk_gfx_fill_circle(g, fx + s * 0.50f, cy, s * 0.12f, color);
        break;
    }
    }
}

DkFontMetrics dk_gfx_font_metrics(DkGfx *g, DkFontStyle style, int size_px) {
    DkFontMetrics m = { size_px, size_px / 4, (int)(size_px * 1.45f) };
    if (!g || style >= DK_FONT_COUNT) return m;
    FaceSlot *slot = &g->primary[style];
    if (!slot->face) return m;
    set_face_size(slot, size_px);
    int asc = (int)((slot->face->size->metrics.ascender + 32) >> 6);
    int desc = (int)((-slot->face->size->metrics.descender + 32) >> 6);
    int lh = (int)((slot->face->size->metrics.height + 32) >> 6);
    if (asc > 0) m.ascent = asc;
    if (desc >= 0) m.descent = desc;
    if (lh > asc + desc) m.line_height = lh;
    else m.line_height = m.ascent + m.descent + 3;
    return m;
}

int dk_gfx_char_advance(DkGfx *g, DkFontStyle style, int size_px, uint32_t cp) {
    if (!g) return size_px / 2;
    if (cp == '\t') {
        const GlyphEntry *sp = get_glyph(g, style, size_px, ' ');
        return (sp ? sp->advance : size_px / 2) * 4;
    }
    const GlyphEntry *ge = get_glyph(g, style, size_px, cp);
    return ge ? ge->advance : size_px / 2;
}

int dk_gfx_text_width(DkGfx *g, DkFontStyle style, int size_px, const char *s, size_t len) {
    if (!g || !s || len == 0) return 0;
    const char *p = s;
    const char *end = s + len;
    int w = 0;
    while (p < end) {
        uint32_t cp = 0;
        p += dk_utf8_decode(p, end, &cp);
        if (cp == '\n' || cp == '\r') continue;
        w += dk_gfx_char_advance(g, style, size_px, cp);
    }
    return w;
}

int dk_gfx_draw_text(DkGfx *g, int x, int baseline_y, DkFontStyle style, int size_px,
                     uint32_t color, const char *s, size_t len) {
    if (!g || !g->pixels || !s || len == 0) return 0;
    DkRect c = g->clip_stack[g->clip_top];
    uint32_t ca = (color >> 24) & 0xFFu;
    if (ca == 0) return dk_gfx_text_width(g, style, size_px, s, len);

    int pen_x = x;
    const char *p = s;
    const char *end = s + len;
    int clip_x1 = c.x + c.w;
    int clip_y1 = c.y + c.h;

    while (p < end) {
        uint32_t cp = 0;
        p += dk_utf8_decode(p, end, &cp);
        if (cp == '\n' || cp == '\r') continue;
        if (cp == '\t') {
            pen_x += dk_gfx_char_advance(g, style, size_px, '\t');
            continue;
        }
        const GlyphEntry *ge = get_glyph(g, style, size_px, cp);
        if (!ge) continue;

        if (ge->bitmap && ge->width > 0 && ge->height > 0) {
            int gx0 = pen_x + ge->bearing_x;
            int gy0 = baseline_y - ge->bearing_y;
            int gx1 = gx0 + ge->width;
            int gy1 = gy0 + ge->height;

            int sx0 = gx0 > c.x ? gx0 : c.x;
            int sy0 = gy0 > c.y ? gy0 : c.y;
            int sx1 = gx1 < clip_x1 ? gx1 : clip_x1;
            int sy1 = gy1 < clip_y1 ? gy1 : clip_y1;

            if (sx0 < sx1 && sy0 < sy1) {
                for (int py = sy0; py < sy1; py++) {
                    const uint8_t *src = ge->bitmap + (size_t)(py - gy0) * ge->width + (size_t)(sx0 - gx0);
                    uint32_t *dst = g->pixels + (size_t)py * (size_t)g->w + (size_t)sx0;
                    int count = sx1 - sx0;
                    if (ca == 255) {
                        for (int i = 0; i < count; i++) {
                            uint32_t a = src[i];
                            if (a) dst[i] = blend_pixel(dst[i], color, a);
                        }
                    } else {
                        for (int i = 0; i < count; i++) {
                            uint32_t a = (src[i] * ca + 127u) / 255u;
                            if (a) dst[i] = blend_pixel(dst[i], color, a);
                        }
                    }
                }
            }
        }
        pen_x += ge->advance;
    }
    return pen_x - x;
}

void dk_gfx_draw_text_ellipsize(DkGfx *g, int x, int baseline_y, int max_w,
                                DkFontStyle style, int size_px, uint32_t color,
                                const char *s, size_t len) {
    if (!g || !s || len == 0 || max_w <= 0) return;
    int full_w = dk_gfx_text_width(g, style, size_px, s, len);
    if (full_w <= max_w) {
        dk_gfx_draw_text(g, x, baseline_y, style, size_px, color, s, len);
        return;
    }
    int ell_w = dk_gfx_char_advance(g, style, size_px, 0x2026);
    int limit = max_w - ell_w;
    if (limit <= 0) return;

    const char *p = s;
    const char *end = s + len;
    int w = 0;
    while (p < end) {
        uint32_t cp = 0;
        size_t adv = dk_utf8_decode(p, end, &cp);
        int cw = dk_gfx_char_advance(g, style, size_px, cp);
        if (w + cw > limit) break;
        w += cw;
        p += adv;
    }
    int drawn = dk_gfx_draw_text(g, x, baseline_y, style, size_px, color, s, (size_t)(p - s));
    dk_gfx_draw_text(g, x + drawn, baseline_y, style, size_px, color, "\xE2\x80\xA6", 3);
}
