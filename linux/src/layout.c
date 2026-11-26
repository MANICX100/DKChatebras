#include "layout.h"
#include "str.h"

#include <stdlib.h>
#include <string.h>

#define BODY_FONT_PX 15
#define CODE_FONT_PX 13
#define TABLE_FONT_PX 13

#define COL_TEXT_BODY      DK_RGB(220, 226, 233)
#define COL_TEXT_HEADING   DK_RGB(244, 247, 250)
#define COL_TEXT_QUOTE     DK_RGB(152, 164, 179)
#define COL_TEXT_LINK      DK_RGB(255, 138, 112)
#define COL_TEXT_CODE      DK_RGB(242, 192, 181)
#define COL_TEXT_CODE_BLK  DK_RGB(216, 224, 232)
#define COL_TEXT_LANG      DK_RGB(122, 136, 152)
#define COL_CODE_PILL_BG   DK_RGB(22, 29, 38)
#define COL_CODE_PILL_BD   DK_RGB(38, 48, 60)
#define COL_CODE_BLK_BG    DK_RGB(9, 13, 17)
#define COL_CODE_BLK_BD    DK_RGB(30, 38, 48)
#define COL_QUOTE_BAR      DK_RGB(255, 91, 56)
#define COL_HR_LINE        DK_RGB(40, 49, 60)
#define COL_TBL_BG         DK_RGB(12, 16, 22)
#define COL_TBL_HDR_BG     DK_RGB(22, 28, 37)
#define COL_TBL_LINE       DK_RGB(36, 45, 56)
#define COL_SEL_BG         DK_RGBA(255, 91, 56, 78)
#define COL_ACCENT         DK_RGB(255, 91, 56)

void dk_layout_init(DkMsgLayout *lay) {
    memset(lay, 0, sizeof(*lay));
}

void dk_layout_clear(DkMsgLayout *lay) {
    lay->line_count = 0;
    lay->box_count = 0;
    lay->total_height = 0;
    lay->cached_width = 0;
}

void dk_layout_free(DkMsgLayout *lay) {
    free(lay->lines);
    free(lay->boxes);
    memset(lay, 0, sizeof(*lay));
}

static void push_line(DkMsgLayout *lay, DkVisLine line) {
    if (lay->line_count == lay->line_cap) {
        size_t ncap = lay->line_cap ? lay->line_cap * 2 : 32;
        DkVisLine *p = realloc(lay->lines, ncap * sizeof(DkVisLine));
        if (!p) return;
        lay->lines = p;
        lay->line_cap = ncap;
    }
    lay->lines[lay->line_count++] = line;
}

static void push_box(DkMsgLayout *lay, DkVisBox box) {
    if (lay->box_count == lay->box_cap) {
        size_t ncap = lay->box_cap ? lay->box_cap * 2 : 16;
        DkVisBox *p = realloc(lay->boxes, ncap * sizeof(DkVisBox));
        if (!p) return;
        lay->boxes = p;
        lay->box_cap = ncap;
    }
    lay->boxes[lay->box_count++] = box;
}

static DkFontStyle style_for_flags(uint32_t flags) {
    bool bold = (flags & (DK_SPAN_STRONG | DK_SPAN_H1 | DK_SPAN_H2 | DK_SPAN_H3 |
                          DK_SPAN_H4 | DK_SPAN_H5 | DK_SPAN_H6 | DK_SPAN_TABLE_HEADER)) != 0;
    bool italic = (flags & (DK_SPAN_EMPHASIS | DK_SPAN_QUOTE)) != 0;
    bool mono = (flags & (DK_SPAN_CODE | DK_SPAN_CODE_BLOCK)) != 0;
    if (mono)
        return bold ? DK_FONT_MONO_BOLD : DK_FONT_MONO;
    if (bold && italic) return DK_FONT_BOLD_ITALIC;
    if (bold) return DK_FONT_BOLD;
    if (italic) return DK_FONT_ITALIC;
    return DK_FONT_REGULAR;
}

static int font_size_for_flags(uint32_t flags, int base_size) {
    if (flags & DK_SPAN_H1) return 23;
    if (flags & DK_SPAN_H2) return 20;
    if (flags & DK_SPAN_H3) return 18;
    if (flags & DK_SPAN_H4) return 16;
    if (flags & DK_SPAN_H5) return 15;
    if (flags & DK_SPAN_H6) return 14;
    if (flags & (DK_SPAN_CODE | DK_SPAN_CODE_BLOCK)) return base_size > 14 ? CODE_FONT_PX : base_size - 1;
    return base_size;
}

static uint32_t color_for_flags(uint32_t flags) {
    if (flags & DK_SPAN_LINK) return COL_TEXT_LINK;
    if (flags & DK_SPAN_CODE) return COL_TEXT_CODE;
    if (flags & DK_SPAN_CODE_BLOCK) return COL_TEXT_CODE_BLK;
    if (flags & (DK_SPAN_H1 | DK_SPAN_H2 | DK_SPAN_H3 | DK_SPAN_H4 | DK_SPAN_H5 | DK_SPAN_H6 | DK_SPAN_TABLE_HEADER))
        return COL_TEXT_HEADING;
    if (flags & DK_SPAN_QUOTE) return COL_TEXT_QUOTE;
    return COL_TEXT_BODY;
}

/* Word-wraps byte range [start, end) at (x, *io_y) within max_w. */
static void wrap_range(DkMsgLayout *lay, DkGfx *g, const DkMdDoc *doc,
                       const char *text, uint32_t start, uint32_t end,
                       int x, int hang_x, int max_w, int base_font_px,
                       uint32_t base_flags, int *io_y) {
    if (max_w < 32) max_w = 32;
    DkFontStyle base_style = style_for_flags(base_flags);
    int fpx = font_size_for_flags(base_flags, base_font_px);
    DkFontMetrics fm = dk_gfx_font_metrics(g, base_style, fpx);
    int lh = fm.line_height + 4;

    if (start >= end) {
        push_line(lay, (DkVisLine){
            .x = x, .y = *io_y, .w = 0, .h = lh,
            .baseline = fm.ascent + 2,
            .start = start, .end = start,
            .font_size = (uint16_t)base_font_px, .base_flags = base_flags
        });
        *io_y += lh;
        return;
    }

    uint32_t pos = start;
    bool first_line = true;

    while (pos < end) {
        int lx = first_line ? x : hang_x;
        int avail = max_w - (lx - x);
        if (avail < 32) avail = 32;

        uint32_t line_start = pos;
        uint32_t last_break = UINT32_MAX;
        int last_break_w = 0;
        int cur_w = 0;
        bool forced_nl = false;

        while (pos < end) {
            if (text[pos] == '\n') {
                forced_nl = true;
                break;
            }
            uint32_t cp = 0;
            size_t adv = dk_utf8_decode(text + pos, text + end, &cp);
            uint32_t fl = base_flags | (doc ? dk_md_flags_at(doc, pos) : 0);
            DkFontStyle st = style_for_flags(fl);
            int csz = font_size_for_flags(fl, base_font_px);
            int cw = dk_gfx_char_advance(g, st, csz, cp);
            if ((fl & DK_SPAN_CODE) && !(fl & DK_SPAN_CODE_BLOCK)) {
                bool prev_code = (pos > line_start) && doc && (dk_md_flags_at(doc, pos - 1) & DK_SPAN_CODE);
                bool next_code = (pos + adv < end) && doc && (dk_md_flags_at(doc, pos + adv) & DK_SPAN_CODE);
                if (!prev_code) cw += 4;
                if (!next_code) cw += 4;
            }

            if (cur_w > 0 && cur_w + cw > avail) {
                if (last_break != UINT32_MAX) {
                    pos = last_break;
                    cur_w = last_break_w;
                }
                break;
            }
            cur_w += cw;
            pos += (uint32_t)adv;
            if (cp == ' ' || cp == '\t') {
                last_break = pos;
                last_break_w = cur_w;
            }
        }

        uint32_t line_end = pos;
        if (line_end == line_start && pos < end && !forced_nl) {
            /* Always advance at least one codepoint to avoid infinite loop */
            uint32_t cp = 0;
            pos += (uint32_t)dk_utf8_decode(text + pos, text + end, &cp);
            line_end = pos;
        }

        push_line(lay, (DkVisLine){
            .x = lx, .y = *io_y, .w = cur_w, .h = lh,
            .baseline = fm.ascent + 2,
            .start = line_start, .end = line_end,
            .font_size = (uint16_t)base_font_px, .base_flags = base_flags
        });
        *io_y += lh;
        first_line = false;

        if (forced_nl) {
            pos++; /* consume '\n' */
            if (pos == end) {
                /* trailing explicit newline */
            }
        } else {
            /* Skip leading space after a soft wrap */
            while (pos < end && text[pos] == ' ')
                pos++;
        }
    }
}

/* Lays out a GFM table block with proportional auto-sized columns and grid boxes. */
static void layout_table(DkMsgLayout *lay, DkGfx *g, const DkMdDoc *doc,
                         const DkMdBlock *blk, int max_w, int *io_y) {
    uint16_t ncols = blk->num_cols;
    uint16_t nrows = blk->num_rows;
    if (ncols == 0 || nrows == 0) return;

    int pad_x = 10;
    int pad_y = 7;
    int col_w[32];
    int desired[32];
    int total_desired = 0;

    if (ncols > 32) ncols = 32;

    for (uint16_t c = 0; c < ncols; c++) {
        int max_cw = 48;
        for (uint16_t r = 0; r < nrows; r++) {
            const DkMdCell *cell = &doc->cells[blk->cell_start + (size_t)r * blk->num_cols + c];
            /* Measure longest line in cell (split on \n) */
            const char *p = doc->text + cell->start;
            const char *end = doc->text + cell->end;
            DkFontStyle st = (r == 0) ? DK_FONT_BOLD : DK_FONT_REGULAR;
            while (p <= end) {
                const char *nl = (p < end) ? memchr(p, '\n', (size_t)(end - p)) : NULL;
                const char *le = nl ? nl : end;
                int lw = dk_gfx_text_width(g, st, TABLE_FONT_PX, p, (size_t)(le - p)) + pad_x * 2 + 4;
                if (lw > max_cw) max_cw = lw;
                if (!nl) break;
                p = nl + 1;
            }
        }
        desired[c] = max_cw;
        total_desired += max_cw;
    }

    int tbl_w = total_desired < max_w ? total_desired : max_w;
    if (tbl_w < 120) tbl_w = max_w;

    int sum_w = 0;
    for (uint16_t c = 0; c < ncols; c++) {
        if (total_desired <= max_w) {
            col_w[c] = desired[c];
        } else {
            int w = (int)(((int64_t)desired[c] * tbl_w) / total_desired);
            if (w < 44) w = 44;
            col_w[c] = w;
        }
        sum_w += col_w[c];
    }
    if (sum_w > max_w) {
        /* Scale down proportionally */
        int rem = max_w;
        for (uint16_t c = 0; c < ncols; c++) {
            int w = (c + 1 == ncols) ? rem : (col_w[c] * max_w) / sum_w;
            if (w < 36) w = 36;
            col_w[c] = w;
            rem -= w;
        }
        tbl_w = max_w;
    } else {
        tbl_w = sum_w;
    }

    *io_y += 6;
    int tbl_y0 = *io_y;
    size_t bg_box_idx = lay->box_count;
    push_box(lay, (DkVisBox){ .kind = DK_BOX_TABLE_BG, .x = 0, .y = tbl_y0, .w = tbl_w, .h = 0 });

    int cur_y = tbl_y0;
    for (uint16_t r = 0; r < nrows; r++) {
        int row_y0 = cur_y;
        int max_row_y = row_y0 + pad_y;
        int cx = 0;
        size_t hdr_box_idx = 0;
        if (r == 0) {
            hdr_box_idx = lay->box_count;
            push_box(lay, (DkVisBox){ .kind = DK_BOX_TABLE_HEADER, .x = 1, .y = row_y0 + 1, .w = tbl_w - 2, .h = 0 });
        }

        uint32_t row_flags = DK_SPAN_TABLE | (r == 0 ? DK_SPAN_TABLE_HEADER : 0);
        for (uint16_t c = 0; c < ncols; c++) {
            const DkMdCell *cell = &doc->cells[blk->cell_start + (size_t)r * blk->num_cols + c];
            int cy = row_y0 + pad_y;
            wrap_range(lay, g, doc, doc->text, cell->start, cell->end,
                       cx + pad_x, cx + pad_x, col_w[c] - pad_x * 2,
                       TABLE_FONT_PX, row_flags, &cy);
            if (cy > max_row_y) max_row_y = cy;
            cx += col_w[c];
        }
        cur_y = max_row_y + pad_y - 2;
        if (r == 0) {
            lay->boxes[hdr_box_idx].h = cur_y - row_y0 - 1;
        }
        if (r + 1 < nrows) {
            push_box(lay, (DkVisBox){ .kind = DK_BOX_TABLE_HLINE, .x = 1, .y = cur_y, .w = tbl_w - 2, .h = 1 });
        }
    }

    int tbl_h = cur_y - tbl_y0;
    lay->boxes[bg_box_idx].h = tbl_h;

    /* Vertical column dividers */
    int vx = 0;
    for (uint16_t c = 0; c + 1 < ncols; c++) {
        vx += col_w[c];
        push_box(lay, (DkVisBox){ .kind = DK_BOX_TABLE_VLINE, .x = vx, .y = tbl_y0 + 1, .w = 1, .h = tbl_h - 2 });
    }

    *io_y = cur_y + 8;
}

void dk_layout_build(DkMsgLayout *lay, DkGfx *g, const DkMdDoc *doc,
                     const char *plain_text, size_t plain_len, int max_width) {
    dk_layout_clear(lay);
    if (max_width < 80) max_width = 80;
    lay->cached_width = max_width;

    int y = 0;

    if (!doc) {
        /* Plain streaming text fast path */
        wrap_range(lay, g, NULL, plain_text ? plain_text : "", 0, (uint32_t)plain_len,
                   0, 0, max_width, BODY_FONT_PX, 0, &y);
        lay->total_height = y;
        return;
    }

    for (size_t bi = 0; bi < doc->block_count; bi++) {
        const DkMdBlock *blk = &doc->blocks[bi];
        switch (blk->type) {
        case DK_BLK_EMPTY:
            if (y > 0) y += 8;
            break;
        case DK_BLK_HEADING: {
            uint32_t hflags[6] = {
                DK_SPAN_H1, DK_SPAN_H2, DK_SPAN_H3,
                DK_SPAN_H4, DK_SPAN_H5, DK_SPAN_H6
            };
            uint8_t lv = (blk->level >= 1 && blk->level <= 6) ? blk->level : 1;
            if (y > 0) y += (lv <= 2 ? 12 : 8);
            wrap_range(lay, g, doc, doc->text, blk->start, blk->end,
                       0, 0, max_width, BODY_FONT_PX, hflags[lv - 1], &y);
            y += 4;
            break;
        }
        case DK_BLK_CODE: {
            y += 6;
            int pad_x = 14;
            int pad_top = blk->lang_len > 0 ? 28 : 11;
            int pad_bot = 11;
            int box_y0 = y;
            size_t box_idx = lay->box_count;
            push_box(lay, (DkVisBox){
                .kind = DK_BOX_CODE_BG, .x = 0, .y = box_y0, .w = max_width, .h = 0,
                .lang_off = blk->lang_off, .lang_len = blk->lang_len
            });
            y += pad_top;
            if (blk->level > 0 && blk->end >= blk->start) {
                wrap_range(lay, g, doc, doc->text, blk->start, blk->end,
                           pad_x, pad_x, max_width - pad_x * 2, CODE_FONT_PX, DK_SPAN_CODE_BLOCK, &y);
            } else {
                DkFontMetrics fm = dk_gfx_font_metrics(g, DK_FONT_MONO, CODE_FONT_PX);
                y += fm.line_height + 2;
            }
            y += pad_bot;
            lay->boxes[box_idx].h = y - box_y0;
            y += 6;
            break;
        }
        case DK_BLK_QUOTE: {
            int qy0 = y;
            int indent = 14 + (blk->level > 1 ? (blk->level - 1) * 10 : 0);
            wrap_range(lay, g, doc, doc->text, blk->start, blk->end,
                       indent, indent, max_width - indent, BODY_FONT_PX, DK_SPAN_QUOTE, &y);
            push_box(lay, (DkVisBox){
                .kind = DK_BOX_QUOTE_BAR, .x = 2, .y = qy0 + 2, .w = 3, .h = (y - qy0) - 4
            });
            y += 2;
            break;
        }
        case DK_BLK_LIST: {
            int base_indent = 8 + blk->level * 6;
            int hang = base_indent + 16;
            wrap_range(lay, g, doc, doc->text, blk->start, blk->end,
                       base_indent, hang, max_width - base_indent, BODY_FONT_PX, DK_SPAN_LIST, &y);
            y += 2;
            break;
        }
        case DK_BLK_HR: {
            y += 10;
            push_box(lay, (DkVisBox){
                .kind = DK_BOX_HR, .x = 0, .y = y, .w = max_width, .h = 1
            });
            y += 12;
            break;
        }
        case DK_BLK_TABLE:
            layout_table(lay, g, doc, blk, max_width, &y);
            break;
        case DK_BLK_PARAGRAPH:
        default:
            wrap_range(lay, g, doc, doc->text, blk->start, blk->end,
                       0, 0, max_width, BODY_FONT_PX, 0, &y);
            y += 2;
            break;
        }
    }

    if (y < 20) y = 20;
    lay->total_height = y;
}

void dk_layout_paint(DkGfx *g, const DkMsgLayout *lay,
                     const DkMdDoc *doc, const char *plain_text, size_t plain_len,
                     int origin_x, int origin_y, int view_y0, int view_y1,
                     size_t sel_start, size_t sel_end, bool streaming_cursor) {
    const char *text = doc ? doc->text : (plain_text ? plain_text : "");
    size_t text_len = doc ? doc->text_len : plain_len;
    if (sel_start > sel_end) {
        size_t t = sel_start; sel_start = sel_end; sel_end = t;
    }

    /* 1. Draw container boxes */
    for (size_t i = 0; i < lay->box_count; i++) {
        const DkVisBox *b = &lay->boxes[i];
        int by = origin_y + b->y;
        if (by + b->h < view_y0 || by > view_y1)
            continue;
        int bx = origin_x + b->x;
        switch (b->kind) {
        case DK_BOX_CODE_BG:
            dk_gfx_fill_rrect(g, bx, by, b->w, b->h, 8.0f, COL_CODE_BLK_BG);
            dk_gfx_stroke_rrect(g, bx, by, b->w, b->h, 8.0f, COL_CODE_BLK_BD);
            if (doc && b->lang_len > 0) {
                dk_gfx_draw_text(g, bx + 14, by + 18, DK_FONT_MONO_BOLD, 11,
                                 COL_TEXT_LANG, doc->urls + b->lang_off, b->lang_len);
                dk_gfx_fill_rect(g, bx + 1, by + 23, b->w - 2, 1, DK_RGB(22, 28, 36));
            }
            break;
        case DK_BOX_QUOTE_BAR:
            dk_gfx_fill_rrect(g, bx, by, b->w, b->h, 1.5f, COL_QUOTE_BAR);
            break;
        case DK_BOX_HR:
            dk_gfx_fill_rect(g, bx, by, b->w, 1, COL_HR_LINE);
            break;
        case DK_BOX_TABLE_BG:
            dk_gfx_fill_rrect(g, bx, by, b->w, b->h, 6.0f, COL_TBL_BG);
            dk_gfx_stroke_rrect(g, bx, by, b->w, b->h, 6.0f, COL_TBL_LINE);
            break;
        case DK_BOX_TABLE_HEADER:
            dk_gfx_fill_rect(g, bx, by, b->w, b->h, COL_TBL_HDR_BG);
            break;
        case DK_BOX_TABLE_HLINE:
        case DK_BOX_TABLE_VLINE:
            dk_gfx_fill_rect(g, bx, by, b->w, b->h, COL_TBL_LINE);
            break;
        }
    }

    /* 2. Draw visible text lines in styled runs */
    int last_x = origin_x;
    int last_y = origin_y;
    int last_h = 18;

    for (size_t i = 0; i < lay->line_count; i++) {
        const DkVisLine *ln = &lay->lines[i];
        int ly = origin_y + ln->y;
        if (ly + ln->h < view_y0 || ly > view_y1)
            continue;

        int pen_x = origin_x + ln->x;
        int base_y = ly + ln->baseline;
        uint32_t pos = ln->start;
        uint32_t end = ln->end < text_len ? ln->end : (uint32_t)text_len;

        while (pos < end) {
            uint32_t fl = ln->base_flags | (doc ? dk_md_flags_at(doc, pos) : 0);
            bool in_sel = (sel_start < sel_end && pos >= sel_start && pos < sel_end);

            uint32_t run_end = pos;
            while (run_end < end) {
                uint32_t nfl = ln->base_flags | (doc ? dk_md_flags_at(doc, run_end) : 0);
                bool nsel = (sel_start < sel_end && run_end >= sel_start && run_end < sel_end);
                if (nfl != fl || nsel != in_sel) break;
                run_end += (uint32_t)dk_utf8_decode(text + run_end, text + end, NULL);
            }

            DkFontStyle st = style_for_flags(fl);
            int fpx = font_size_for_flags(fl, ln->font_size);
            uint32_t col = color_for_flags(fl);

            int pad_l = 0, pad_r = 0;
            if ((fl & DK_SPAN_CODE) && !(fl & DK_SPAN_CODE_BLOCK)) {
                pad_l = 4;
                pad_r = 4;
            }

            int run_w = dk_gfx_text_width(g, st, fpx, text + pos, run_end - pos);

            if (in_sel) {
                dk_gfx_fill_rect(g, pen_x, ly + 1, run_w + pad_l + pad_r, ln->h - 2, COL_SEL_BG);
            } else if ((fl & DK_SPAN_CODE) && !(fl & DK_SPAN_CODE_BLOCK)) {
                dk_gfx_fill_rrect(g, pen_x, ly + 2, run_w + pad_l + pad_r, ln->h - 4, 4.0f, COL_CODE_PILL_BG);
                dk_gfx_stroke_rrect(g, pen_x, ly + 2, run_w + pad_l + pad_r, ln->h - 4, 4.0f, COL_CODE_PILL_BD);
            }

            dk_gfx_draw_text(g, pen_x + pad_l, base_y, st, fpx, col, text + pos, run_end - pos);

            if (fl & DK_SPAN_LINK) {
                dk_gfx_fill_rect(g, pen_x + pad_l, base_y + 2, run_w, 1, col);
            }
            if (fl & DK_SPAN_STRIKE) {
                int sy = base_y - (fpx * 3) / 10;
                dk_gfx_fill_rect(g, pen_x + pad_l, sy, run_w, 1, col);
            }

            pen_x += run_w + pad_l + pad_r;
            pos = run_end;
        }

        if (i + 1 == lay->line_count) {
            last_x = pen_x;
            last_y = ly;
            last_h = ln->h;
        }
    }

    if (streaming_cursor) {
        dk_gfx_fill_rrect(g, last_x + 3, last_y + 3, 7, last_h - 6, 2.0f, COL_ACCENT);
    }
}

bool dk_layout_hit_test(DkGfx *g, const DkMsgLayout *lay,
                        const DkMdDoc *doc, const char *plain_text, size_t plain_len,
                        int rel_x, int rel_y,
                        size_t *out_byte, const char **out_url, size_t *out_url_len) {
    if (out_url) *out_url = NULL;
    if (out_url_len) *out_url_len = 0;
    if (!lay || lay->line_count == 0) return false;

    const char *text = doc ? doc->text : (plain_text ? plain_text : "");
    size_t text_len = doc ? doc->text_len : plain_len;

    const DkVisLine *best = NULL;
    for (size_t i = 0; i < lay->line_count; i++) {
        const DkVisLine *ln = &lay->lines[i];
        if (rel_y >= ln->y && rel_y < ln->y + ln->h) {
            if (rel_x >= ln->x - 8 && rel_x <= ln->x + ln->w + 16) {
                best = ln;
                break;
            }
            if (!best || abs(rel_x - ln->x) < abs(rel_x - best->x))
                best = ln;
        }
    }
    if (!best) {
        if (rel_y < lay->lines[0].y) best = &lay->lines[0];
        else best = &lay->lines[lay->line_count - 1];
    }

    int pen_x = best->x;
    uint32_t pos = best->start;
    uint32_t end = best->end < text_len ? best->end : (uint32_t)text_len;
    uint32_t hit = pos;

    while (pos < end) {
        uint32_t cp = 0;
        size_t adv = dk_utf8_decode(text + pos, text + end, &cp);
        uint32_t fl = best->base_flags | (doc ? dk_md_flags_at(doc, pos) : 0);
        DkFontStyle st = style_for_flags(fl);
        int fpx = font_size_for_flags(fl, best->font_size);
        int cw = dk_gfx_char_advance(g, st, fpx, cp);
        if ((fl & DK_SPAN_CODE) && !(fl & DK_SPAN_CODE_BLOCK)) {
            bool prev_code = (pos > best->start) && doc && (dk_md_flags_at(doc, pos - 1) & DK_SPAN_CODE);
            bool next_code = (pos + adv < end) && doc && (dk_md_flags_at(doc, pos + adv) & DK_SPAN_CODE);
            if (!prev_code) cw += 4;
            if (!next_code) cw += 4;
        }
        if (rel_x < pen_x + cw / 2) {
            hit = pos;
            if (doc && out_url && rel_x >= pen_x && rel_y >= best->y && rel_y < best->y + best->h) {
                *out_url = dk_md_link_at(doc, pos, out_url_len);
            }
            if (out_byte) *out_byte = hit;
            return true;
        }
        if (doc && out_url && rel_x >= pen_x && rel_x < pen_x + cw &&
            rel_y >= best->y && rel_y < best->y + best->h) {
            *out_url = dk_md_link_at(doc, pos, out_url_len);
        }
        pen_x += cw;
        pos += (uint32_t)adv;
        hit = pos;
    }

    if (out_byte) *out_byte = hit;
    return true;
}
