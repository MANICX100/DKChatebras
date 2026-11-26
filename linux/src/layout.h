#ifndef DK_LAYOUT_H
#define DK_LAYOUT_H

#include "gfx.h"
#include "markdown.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DK_BOX_CODE_BG = 0,
    DK_BOX_QUOTE_BAR,
    DK_BOX_HR,
    DK_BOX_TABLE_BG,
    DK_BOX_TABLE_HEADER,
    DK_BOX_TABLE_HLINE,
    DK_BOX_TABLE_VLINE,
} DkBoxKind;

typedef struct {
    DkBoxKind kind;
    int x, y, w, h;
    uint32_t lang_off;
    uint32_t lang_len;
} DkVisBox;

typedef struct {
    int x, y, w, h;
    int baseline;       /* offset from line y */
    uint32_t start;     /* byte offset in text */
    uint32_t end;       /* byte offset in text */
    uint16_t font_size;
    uint32_t base_flags;
} DkVisLine;

typedef struct {
    int cached_width;
    int total_height;
    DkVisLine *lines;
    size_t line_count;
    size_t line_cap;
    DkVisBox *boxes;
    size_t box_count;
    size_t box_cap;
} DkMsgLayout;

void dk_layout_init(DkMsgLayout *lay);
void dk_layout_free(DkMsgLayout *lay);
void dk_layout_clear(DkMsgLayout *lay);

/* Builds layout for a parsed DkMdDoc (if doc != NULL) or plain streaming text. */
void dk_layout_build(DkMsgLayout *lay, DkGfx *g, const DkMdDoc *doc,
                     const char *plain_text, size_t plain_len, int max_width);

/* Paints visible lines/boxes intersecting [view_y0, view_y1] in screen coords. */
void dk_layout_paint(DkGfx *g, const DkMsgLayout *lay,
                     const DkMdDoc *doc, const char *plain_text, size_t plain_len,
                     int origin_x, int origin_y, int view_y0, int view_y1,
                     size_t sel_start, size_t sel_end, bool streaming_cursor);

/* Maps message-relative (rel_x, rel_y) to byte offset & optional link URL. */
bool dk_layout_hit_test(DkGfx *g, const DkMsgLayout *lay,
                        const DkMdDoc *doc, const char *plain_text, size_t plain_len,
                        int rel_x, int rel_y,
                        size_t *out_byte, const char **out_url, size_t *out_url_len);

#endif
