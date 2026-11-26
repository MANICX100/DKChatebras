#include "markdown.h"
#include "str.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    DkStr text;
    DkStr urls;
    DkMdSpan *spans;
    size_t span_count;
    size_t span_cap;
    DkMdBlock *blocks;
    size_t block_count;
    size_t block_cap;
    DkMdCell *cells;
    size_t cell_count;
    size_t cell_cap;
    size_t lines;
} Renderer;

static void append_text(Renderer *r, const char *s, size_t len) {
    dk_str_append_len(&r->text, s, len);
}

static void append_literal(Renderer *r, const char *s) {
    dk_str_append(&r->text, s);
}

static uint32_t pool_string(Renderer *r, const char *s, size_t len) {
    uint32_t off = (uint32_t)r->urls.len;
    dk_str_append_len(&r->urls, s, len);
    dk_str_push(&r->urls, '\0');
    return off;
}

static void add_span(Renderer *r, uint32_t start, uint32_t end, uint32_t flags,
                     uint32_t url_off, uint32_t url_len) {
    if (start >= end)
        return;
    if (r->span_count == r->span_cap) {
        size_t ncap = r->span_cap ? r->span_cap * 2 : 32;
        DkMdSpan *p = realloc(r->spans, ncap * sizeof(DkMdSpan));
        if (!p) return;
        r->spans = p;
        r->span_cap = ncap;
    }
    r->spans[r->span_count++] = (DkMdSpan){
        .start = start,
        .end = end,
        .flags = flags,
        .url_off = url_off,
        .url_len = url_len,
    };
}

static DkMdBlock *add_block(Renderer *r, DkBlockType type, uint32_t start, uint32_t end, uint8_t level) {
    if (r->block_count == r->block_cap) {
        size_t ncap = r->block_cap ? r->block_cap * 2 : 16;
        DkMdBlock *p = realloc(r->blocks, ncap * sizeof(DkMdBlock));
        if (!p) return NULL;
        r->blocks = p;
        r->block_cap = ncap;
    }
    DkMdBlock *b = &r->blocks[r->block_count++];
    memset(b, 0, sizeof(*b));
    b->type = type;
    b->start = start;
    b->end = end;
    b->level = level;
    return b;
}

static void add_cell(Renderer *r, uint32_t start, uint32_t end) {
    if (r->cell_count == r->cell_cap) {
        size_t ncap = r->cell_cap ? r->cell_cap * 2 : 16;
        DkMdCell *p = realloc(r->cells, ncap * sizeof(DkMdCell));
        if (!p) return;
        r->cells = p;
        r->cell_cap = ncap;
    }
    r->cells[r->cell_count++] = (DkMdCell){ .start = start, .end = end };
}

static bool previous_is_space(const char *begin, const char *pos) {
    const char *prev = dk_utf8_prev(begin, pos);
    if (!prev) return false;
    uint32_t cp = 0;
    dk_utf8_decode(prev, pos, &cp);
    return dk_cp_is_space(cp);
}

static bool next_is_space(const char *pos, const char *end) {
    if (pos >= end) return true;
    uint32_t cp = 0;
    dk_utf8_decode(pos, end, &cp);
    return dk_cp_is_space(cp);
}

static bool previous_is_alnum(const char *begin, const char *pos) {
    const char *prev = dk_utf8_prev(begin, pos);
    if (!prev) return false;
    uint32_t cp = 0;
    dk_utf8_decode(prev, pos, &cp);
    return dk_cp_is_alnum(cp);
}

static bool next_is_alnum(const char *pos, const char *end) {
    if (pos >= end) return false;
    uint32_t cp = 0;
    dk_utf8_decode(pos, end, &cp);
    return dk_cp_is_alnum(cp);
}

static size_t marker_run(const char *pos, const char *end, char marker) {
    const char *c = pos;
    while (c < end && *c == marker)
        c++;
    return (size_t)(c - pos);
}

static const char *find_code_close(const char *pos, const char *end, size_t opener_len) {
    while (pos < end) {
        if (*pos == '`') {
            size_t len = marker_run(pos, end, '`');
            if (len == opener_len)
                return pos;
            pos += len;
        } else {
            pos = dk_utf8_next(pos, end);
        }
    }
    return NULL;
}

static const char *find_emphasis_close(const char *begin, const char *pos,
                                       const char *end, char marker, size_t marker_len) {
    while (pos < end) {
        if (*pos == '\\' && pos + 1 < end) {
            pos += 2;
            continue;
        }
        if (*pos == '`') {
            size_t clen = marker_run(pos, end, '`');
            const char *cend = find_code_close(pos + clen, end, clen);
            if (cend) {
                pos = cend + clen;
                continue;
            }
        }
        if (*pos == marker) {
            size_t len = marker_run(pos, end, marker);
            const char *after = pos + len;
            if (len >= marker_len && !previous_is_space(begin, pos)) {
                const char *close = after - marker_len;
                if (marker != '_' ||
                    !(previous_is_alnum(begin, pos) && next_is_alnum(after, end)))
                    return close;
            }
            pos += len;
            continue;
        }
        pos = dk_utf8_next(pos, end);
    }
    return NULL;
}

static const char *find_label_close(const char *pos, const char *end) {
    unsigned depth = 1;
    while (pos < end) {
        if (*pos == '\\' && pos + 1 < end) {
            pos += 2;
            continue;
        }
        if (*pos == '`') {
            size_t len = marker_run(pos, end, '`');
            const char *close = find_code_close(pos + len, end, len);
            if (close) {
                pos = close + len;
                continue;
            }
        }
        if (*pos == '[') {
            depth++;
        } else if (*pos == ']' && --depth == 0) {
            return pos;
        }
        pos = dk_utf8_next(pos, end);
    }
    return NULL;
}

static const char *find_destination_close(const char *pos, const char *end) {
    unsigned depth = 1;
    while (pos < end) {
        if (*pos == '\\' && pos + 1 < end) {
            pos += 2;
            continue;
        }
        if (*pos == '(') {
            depth++;
        } else if (*pos == ')' && --depth == 0) {
            return pos;
        }
        pos = dk_utf8_next(pos, end);
    }
    return NULL;
}

static void render_code_span(Renderer *r, const char *begin, const char *end) {
    uint32_t start = (uint32_t)r->text.len;
    bool trim_spaces = (end - begin) >= 2 && *begin == ' ' && end[-1] == ' ';
    bool all_spaces = true;
    for (const char *c = begin; c < end; c++) {
        if (*c != ' ') {
            all_spaces = false;
            break;
        }
    }
    if (trim_spaces && !all_spaces) {
        begin++;
        end--;
    }
    append_text(r, begin, (size_t)(end - begin));
    add_span(r, start, (uint32_t)r->text.len, DK_SPAN_CODE, 0, 0);
}

static size_t html_break_length(const char *pos, const char *end) {
    size_t rem = (size_t)(end - pos);
    if (rem >= 4 && pos[0] == '<') {
        if (rem >= 6 && dk_ascii_strncasecmp(pos, "<br />", 6) == 0) return 6;
        if (rem >= 5 && dk_ascii_strncasecmp(pos, "<br/>", 5) == 0) return 5;
        if (dk_ascii_strncasecmp(pos, "<br>", 4) == 0) return 4;
    }
    return 0;
}

static void render_inline(Renderer *r, const char *begin, const char *end, unsigned depth) {
    const char *cur = begin;
    if (depth > 64) {
        append_text(r, begin, (size_t)(end - begin));
        return;
    }

    while (cur < end) {
        size_t br_len = html_break_length(cur, end);
        if (br_len > 0) {
            append_literal(r, "\n");
            cur += br_len;
            continue;
        }

        if (*cur == '\\' && cur + 1 < end &&
            (unsigned char)cur[1] < 0x80 && dk_ascii_ispunct((unsigned char)cur[1])) {
            append_text(r, cur + 1, 1);
            cur += 2;
            continue;
        }

        if (*cur == '`') {
            size_t len = marker_run(cur, end, '`');
            const char *close = find_code_close(cur + len, end, len);
            if (close) {
                render_code_span(r, cur + len, close);
                cur = close + len;
                continue;
            }
        }

        if (*cur == '[') {
            const char *label_end = find_label_close(cur + 1, end);
            if (label_end && label_end + 1 < end && label_end[1] == '(') {
                const char *dest_end = find_destination_close(label_end + 2, end);
                if (dest_end) {
                    uint32_t start = (uint32_t)r->text.len;
                    render_inline(r, cur + 1, label_end, depth + 1);
                    const char *url_s = label_end + 2;
                    size_t url_l = (size_t)(dest_end - url_s);
                    while (url_l > 0 && (*url_s == ' ' || *url_s == '\t')) { url_s++; url_l--; }
                    while (url_l > 0 && (url_s[url_l - 1] == ' ' || url_s[url_l - 1] == '\t')) url_l--;
                    uint32_t url_off = pool_string(r, url_s, url_l);
                    add_span(r, start, (uint32_t)r->text.len, DK_SPAN_LINK, url_off, (uint32_t)url_l);
                    cur = dest_end + 1;
                    continue;
                }
            }
        }

        if (*cur == '~' && marker_run(cur, end, '~') >= 2 &&
            cur + 2 < end && !next_is_space(cur + 2, end)) {
            const char *close = find_emphasis_close(begin, cur + 2, end, '~', 2);
            if (close) {
                uint32_t start = (uint32_t)r->text.len;
                render_inline(r, cur + 2, close, depth + 1);
                add_span(r, start, (uint32_t)r->text.len, DK_SPAN_STRIKE, 0, 0);
                cur = close + 2;
                continue;
            }
        }

        if (*cur == '*' || *cur == '_') {
            char marker = *cur;
            size_t run = marker_run(cur, end, marker);
            size_t len = run >= 3 ? 3 : run >= 2 ? 2 : 1;
            const char *content = cur + len;
            bool can_open = content < end && !next_is_space(content, end) &&
                            (marker != '_' ||
                             !(previous_is_alnum(begin, cur) && next_is_alnum(content, end)));
            if (can_open) {
                const char *close = find_emphasis_close(begin, content, end, marker, len);
                if (close) {
                    uint32_t start = (uint32_t)r->text.len;
                    render_inline(r, content, close, depth + 1);
                    uint32_t flags = 0;
                    if (len >= 2) flags |= DK_SPAN_STRONG;
                    if (len == 1 || len == 3) flags |= DK_SPAN_EMPHASIS;
                    add_span(r, start, (uint32_t)r->text.len, flags, 0, 0);
                    cur = close + len;
                    continue;
                }
            }
        }

        /* Batch plain ASCII characters without special markdown triggers */
        const char *plain_start = cur;
        cur = dk_utf8_next(cur, end);
        while (cur < end) {
            unsigned char ch = (unsigned char)*cur;
            if (ch == '<' || ch == '\\' || ch == '`' || ch == '[' ||
                ch == '~' || ch == '*' || ch == '_')
                break;
            if (ch < 0x80)
                cur++;
            else
                cur = dk_utf8_next(cur, end);
        }
        append_text(r, plain_start, (size_t)(cur - plain_start));
    }
}

static void start_line(Renderer *r) {
    if (r->lines > 0)
        dk_str_push(&r->text, '\n');
    r->lines++;
}

static bool parse_fence(const char *line, const char *end,
                        char *marker, size_t *length, const char **remainder) {
    const char *cur = line;
    unsigned spaces = 0;
    while (cur < end && *cur == ' ' && spaces < 4) {
        cur++;
        spaces++;
    }
    if (spaces > 3 || cur >= end || (*cur != '`' && *cur != '~'))
        return false;
    *marker = *cur;
    *length = marker_run(cur, end, *marker);
    if (*length < 3)
        return false;
    *remainder = cur + *length;
    return true;
}

static bool is_closing_fence(const char *line, const char *end,
                             char marker, size_t opener_len) {
    char cand_m;
    size_t cand_l;
    const char *rem;
    if (!parse_fence(line, end, &cand_m, &cand_l, &rem) ||
        cand_m != marker || cand_l < opener_len)
        return false;
    while (rem < end && (*rem == ' ' || *rem == '\t'))
        rem++;
    return rem == end;
}

static bool is_horizontal_rule(const char *line, const char *end) {
    const char *cur = line;
    unsigned spaces = 0;
    while (cur < end && *cur == ' ' && spaces < 4) {
        cur++;
        spaces++;
    }
    if (spaces > 3 || cur >= end || (*cur != '*' && *cur != '-' && *cur != '_'))
        return false;
    char marker = *cur;
    unsigned count = 0;
    while (cur < end) {
        if (*cur == marker)
            count++;
        else if (*cur != ' ' && *cur != '\t')
            return false;
        cur++;
    }
    return count >= 3;
}

static const char *strip_blockquotes(const char *line, const char *end, unsigned *depth) {
    const char *cur = line;
    *depth = 0;
    while (cur < end) {
        const char *cand = cur;
        unsigned spaces = 0;
        while (cand < end && *cand == ' ' && spaces < 4) {
            cand++;
            spaces++;
        }
        if (spaces > 3 || cand >= end || *cand != '>')
            break;
        cur = cand + 1;
        if (cur < end && (*cur == ' ' || *cur == '\t'))
            cur++;
        (*depth)++;
    }
    return cur;
}

static bool parse_heading(const char *line, const char *end,
                          unsigned *level, const char **content, const char **content_end) {
    const char *cur = line;
    unsigned spaces = 0;
    while (cur < end && *cur == ' ' && spaces < 4) {
        cur++;
        spaces++;
    }
    if (spaces > 3 || cur >= end || *cur != '#')
        return false;
    *level = (unsigned)marker_run(cur, end, '#');
    if (*level == 0 || *level > 6)
        return false;
    cur += *level;
    if (cur < end && *cur != ' ' && *cur != '\t')
        return false;
    while (cur < end && (*cur == ' ' || *cur == '\t'))
        cur++;

    *content = cur;
    *content_end = end;
    while (*content_end > *content &&
           ((*content_end)[-1] == ' ' || (*content_end)[-1] == '\t'))
        (*content_end)--;

    const char *hashes = *content_end;
    while (hashes > *content && hashes[-1] == '#')
        hashes--;
    if (hashes < *content_end && hashes > *content &&
        (hashes[-1] == ' ' || hashes[-1] == '\t')) {
        *content_end = hashes - 1;
        while (*content_end > *content &&
               ((*content_end)[-1] == ' ' || (*content_end)[-1] == '\t'))
            (*content_end)--;
    }
    return true;
}

typedef struct {
    const char *s;
    size_t len;
} Slice;

static bool parse_table_cells(const char *line, const char *end,
                              Slice *out_cells, size_t max_cells, size_t *out_count) {
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;

    if (!memchr(line, '|', (size_t)(end - line)))
        return false;

    if (line < end && *line == '|') line++;
    if (end > line && end[-1] == '|') end--;

    size_t n = 0;
    const char *p = line;
    while (p <= end) {
        const char *pipe = (p < end) ? memchr(p, '|', (size_t)(end - p)) : NULL;
        const char *cs = p;
        const char *ce = pipe ? pipe : end;
        while (cs < ce && (*cs == ' ' || *cs == '\t')) cs++;
        while (ce > cs && (ce[-1] == ' ' || ce[-1] == '\t')) ce--;
        if (n < max_cells)
            out_cells[n++] = (Slice){ cs, (size_t)(ce - cs) };
        if (!pipe)
            break;
        p = pipe + 1;
    }
    *out_count = n;
    return n > 0;
}

static bool is_table_delimiter(const Slice *cells, size_t count) {
    if (count == 0)
        return false;
    for (size_t i = 0; i < count; i++) {
        const char *c = cells[i].s;
        const char *e = c + cells[i].len;
        unsigned dashes = 0;
        if (c < e && *c == ':') c++;
        while (c < e && *c == '-') {
            dashes++;
            c++;
        }
        if (c < e && *c == ':') c++;
        if (c != e || dashes < 3)
            return false;
    }
    return true;
}

static void render_table_row(Renderer *r, const Slice *cells, size_t count,
                             size_t target_cols, bool header) {
    start_line(r);
    uint32_t row_start = (uint32_t)r->text.len;
    for (size_t i = 0; i < target_cols; i++) {
        if (i > 0)
            append_literal(r, "  \xE2\x94\x82  "); /* "  │  " */
        uint32_t cstart = (uint32_t)r->text.len;
        if (i < count && cells[i].len > 0)
            render_inline(r, cells[i].s, cells[i].s + cells[i].len, 0);
        uint32_t cend = (uint32_t)r->text.len;
        add_cell(r, cstart, cend);
    }
    uint32_t row_end = (uint32_t)r->text.len;
    add_span(r, row_start, row_end, DK_SPAN_TABLE | (header ? DK_SPAN_TABLE_HEADER : 0), 0, 0);
}

static bool parse_list_item(const char *line, const char *end,
                            unsigned *indent, const char **content,
                            char prefix_buf[24], size_t *prefix_len) {
    const char *cur = line;
    unsigned cols = 0;
    while (cur < end && (*cur == ' ' || *cur == '\t')) {
        cols += (*cur == '\t') ? 4 : 1;
        cur++;
    }
    if (cur + 1 < end && (*cur == '-' || *cur == '*' || *cur == '+') &&
        (cur[1] == ' ' || cur[1] == '\t')) {
        *indent = cols > 12 ? 12 : cols;
        *content = cur + 2;
        memcpy(prefix_buf, "\xE2\x80\xA2 ", 4); /* "• " */
        *prefix_len = 4;
        return true;
    }
    const char *digits = cur;
    while (cur < end && dk_ascii_isdigit((unsigned char)*cur) && cur - digits < 9)
        cur++;
    if (cur > digits && cur + 1 < end && (*cur == '.' || *cur == ')') &&
        (cur[1] == ' ' || cur[1] == '\t')) {
        *indent = cols > 12 ? 12 : cols;
        *content = cur + 2;
        size_t dlen = (size_t)(cur - digits);
        memcpy(prefix_buf, digits, dlen);
        prefix_buf[dlen] = *cur;
        prefix_buf[dlen + 1] = ' ';
        *prefix_len = dlen + 2;
        return true;
    }
    return false;
}

/* Maps bare LaTeX macros to display text; wrapper macros map to "" and keep their argument. */
static const char *latex_symbol(const char *name, size_t len) {
    static const struct { const char *name; uint8_t len; const char *sym; } table[] = {
        { "times", 5, "\xC3\x97" }, { "div", 3, "\xC3\xB7" }, { "cdot", 4, "\xC2\xB7" },
        { "pm", 2, "\xC2\xB1" }, { "mp", 2, "\xE2\x88\x93" }, { "approx", 6, "\xE2\x89\x88" },
        { "le", 2, "\xE2\x89\xA4" }, { "leq", 3, "\xE2\x89\xA4" }, { "ge", 2, "\xE2\x89\xA5" }, { "geq", 3, "\xE2\x89\xA5" },
        { "ne", 2, "\xE2\x89\xA0" }, { "neq", 3, "\xE2\x89\xA0" }, { "sim", 3, "~" }, { "propto", 6, "\xE2\x88\x9D" },
        { "infty", 5, "\xE2\x88\x9E" }, { "to", 2, "\xE2\x86\x92" }, { "rightarrow", 10, "\xE2\x86\x92" },
        { "leftarrow", 9, "\xE2\x86\x90" }, { "Rightarrow", 10, "\xE2\x87\x92" }, { "Leftarrow", 9, "\xE2\x87\x90" },
        { "leftrightarrow", 14, "\xE2\x86\x94" }, { "sum", 3, "\xCE\xA3" }, { "prod", 4, "\xCE\xA0" },
        { "int", 3, "\xE2\x88\xAB" }, { "sqrt", 4, "\xE2\x88\x9A" }, { "ldots", 5, "\xE2\x80\xA6" },
        { "dots", 4, "\xE2\x80\xA6" }, { "cdots", 5, "\xE2\x8B\xAF" }, { "quad", 4, " " }, { "qquad", 5, "  " },
        { "left", 4, "" }, { "right", 5, "" }, { "text", 4, "" }, { "mathrm", 6, "" }, { "mathbf", 6, "" },
        { "mathit", 6, "" }, { "operatorname", 12, "" }, { "displaystyle", 12, "" }, { "boxed", 5, "" },
        { "alpha", 5, "\xCE\xB1" }, { "beta", 4, "\xCE\xB2" }, { "gamma", 5, "\xCE\xB3" }, { "delta", 5, "\xCE\xB4" },
        { "epsilon", 7, "\xCE\xB5" }, { "zeta", 4, "\xCE\xB6" }, { "eta", 3, "\xCE\xB7" }, { "theta", 5, "\xCE\xB8" },
        { "lambda", 6, "\xCE\xBB" }, { "mu", 2, "\xCE\xBC" }, { "nu", 2, "\xCE\xBD" }, { "xi", 2, "\xCE\xBE" },
        { "pi", 2, "\xCF\x80" }, { "rho", 3, "\xCF\x81" }, { "sigma", 5, "\xCF\x83" }, { "tau", 3, "\xCF\x84" },
        { "phi", 3, "\xCF\x86" }, { "chi", 3, "\xCF\x87" }, { "psi", 3, "\xCF\x88" }, { "omega", 5, "\xCF\x89" },
        { "Gamma", 5, "\xCE\x93" }, { "Delta", 5, "\xCE\x94" }, { "Theta", 5, "\xCE\x98" }, { "Lambda", 6, "\xCE\x9B" },
        { "Pi", 2, "\xCE\xA0" }, { "Sigma", 5, "\xCE\xA3" }, { "Phi", 3, "\xCE\xA6" }, { "Psi", 3, "\xCE\xA8" },
        { "Omega", 5, "\xCE\xA9" },
    };
    if (len < 2 || len > 14)
        return NULL;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].len == len && memcmp(table[i].name, name, len) == 0)
            return table[i].sym;
    }
    return NULL;
}

static ptrdiff_t match_brace(const char *text, size_t len, size_t open) {
    int depth = 0;
    for (size_t j = open; j < len; j++) {
        if (text[j] == '\\') { j++; continue; }
        if (text[j] == '{') depth++;
        else if (text[j] == '}' && --depth == 0) return (ptrdiff_t)j;
    }
    return -1;
}

static bool needs_math_or_glyph_pass(const char *seg, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)seg[i];
        if (c == '\\' || c == '$' || c == '_' || c == '^' || c >= 0xE2)
            return true;
    }
    return false;
}

static void strip_math_append(DkStr *dst, const char *seg, size_t seg_len) {
    if (seg_len == 0)
        return;
    if (!needs_math_or_glyph_pass(seg, seg_len)) {
        dk_str_append_len(dst, seg, seg_len);
        return;
    }

    DkStr cur, next_buf;
    dk_str_init_cap(&cur, seg_len + 16);
    dk_str_init_cap(&next_buf, seg_len + 16);
    dk_utf8_filter_problem_glyphs(&cur, seg, seg_len);

    for (int pass = 0; pass < 8; pass++) {
        const char *text = cur.data ? cur.data : "";
        size_t len = cur.len;
        dk_str_clear(&next_buf);
        size_t i = 0;

        while (i < len) {
            char c = text[i];
            if (c == '\\' && i + 1 < len) {
                char nx = text[i + 1];
                if (dk_ascii_isalpha((unsigned char)nx)) {
                    size_t wend = i + 1;
                    while (wend < len && dk_ascii_isalpha((unsigned char)text[wend]))
                        wend++;
                    const char *name = text + i + 1;
                    size_t nlen = wend - i - 1;
                    if (wend < len && text[wend] == '{') {
                        ptrdiff_t close = match_brace(text, len, wend);
                        if (close >= 0) {
                            bool is_frac =
                                (nlen == 4 && memcmp(name, "frac", 4) == 0) ||
                                (nlen == 5 && (memcmp(name, "dfrac", 5) == 0 || memcmp(name, "tfrac", 5) == 0));
                            if (is_frac && (size_t)close + 1 < len && text[close + 1] == '{') {
                                ptrdiff_t close2 = match_brace(text, len, (size_t)close + 1);
                                if (close2 >= 0) {
                                    dk_str_append_len(&next_buf, text + wend + 1, (size_t)close - wend - 1);
                                    dk_str_append_len(&next_buf, "\xE2\x81\x84", 3); /* ⁄ */
                                    dk_str_append_len(&next_buf, text + close + 2, (size_t)(close2 - close - 2));
                                    i = (size_t)close2 + 1;
                                    continue;
                                }
                            }
                            const char *sym = latex_symbol(name, nlen);
                            if (sym)
                                dk_str_append(&next_buf, sym);
                            dk_str_append_len(&next_buf, text + wend + 1, (size_t)close - wend - 1);
                            i = (size_t)close + 1;
                            continue;
                        }
                    }
                    const char *sym = latex_symbol(name, nlen);
                    if (sym) {
                        dk_str_append(&next_buf, sym);
                        i = wend;
                        continue;
                    }
                    dk_str_append_len(&next_buf, text + i, wend - i);
                    i = wend;
                    continue;
                }
                if (nx == '[' || nx == ']' || nx == '(' || nx == ')') {
                    i += 2;
                    continue;
                }
                if (nx == ';' || nx == ',' || nx == ':' || nx == '!') {
                    dk_str_push(&next_buf, ' ');
                    i += 2;
                    continue;
                }
            }
            if ((c == '_' || c == '^') && i + 1 < len && text[i + 1] == '{') {
                ptrdiff_t close = match_brace(text, len, i + 1);
                if (close >= 0) {
                    size_t arg_len = (size_t)close - i - 2;
                    if (arg_len <= 3) {
                        dk_str_append_len(&next_buf, text + i + 2, arg_len);
                    } else {
                        dk_str_append_len(&next_buf, " (", 2);
                        dk_str_append_len(&next_buf, text + i + 2, arg_len);
                        dk_str_push(&next_buf, ')');
                    }
                    i = (size_t)close + 1;
                    continue;
                }
            }
            if (c == '$' && i + 1 < len && text[i + 1] == '$') {
                i += 2;
                continue;
            }
            dk_str_push(&next_buf, c);
            i++;
        }

        if (next_buf.len == cur.len &&
            (cur.len == 0 || memcmp(next_buf.data, cur.data, cur.len) == 0))
            break;

        DkStr tmp = cur;
        cur = next_buf;
        next_buf = tmp;
    }

    if (cur.len > 0)
        dk_str_append_len(dst, cur.data, cur.len);
    dk_str_free(&cur);
    dk_str_free(&next_buf);
}

static char *normalize_markdown(const char *src, size_t len, size_t *out_len) {
    DkStr out;
    dk_str_init_cap(&out, len + 32);

    bool fenced = false;
    const char *p = src;
    const char *end = src + len;

    while (p <= end) {
        const char *nl = (p < end) ? memchr(p, '\n', (size_t)(end - p)) : NULL;
        const char *line_end = nl ? nl : end;

        const char *cur = p;
        while (cur < line_end && (*cur == ' ' || *cur == '\t'))
            cur++;

        size_t rem = (size_t)(line_end - cur);
        if ((rem >= 3 && memcmp(cur, "```", 3) == 0) ||
            (rem >= 3 && memcmp(cur, "~~~", 3) == 0)) {
            fenced = !fenced;
            dk_str_append_len(&out, p, (size_t)(line_end - p));
        } else if (fenced) {
            dk_str_append_len(&out, p, (size_t)(line_end - p));
        } else {
            const char *start = p;
            bool in_code = false;
            for (;;) {
                const char *tick = (start < line_end) ? memchr(start, '`', (size_t)(line_end - start)) : NULL;
                size_t part_len = tick ? (size_t)(tick - start) : (size_t)(line_end - start);
                if (in_code)
                    dk_str_append_len(&out, start, part_len);
                else
                    strip_math_append(&out, start, part_len);
                if (!tick)
                    break;
                dk_str_push(&out, '`');
                in_code = !in_code;
                start = tick + 1;
            }
        }

        if (!nl)
            break;
        dk_str_push(&out, '\n');
        p = nl + 1;
    }

    *out_len = out.len;
    return dk_str_steal(&out);
}

typedef struct {
    const char *s;
    const char *e;
} LineSlice;

DkMdDoc *dk_md_parse(const char *markdown) {
    const char *raw = markdown ? markdown : "";
    size_t raw_len = strlen(raw);
    char *valid = dk_utf8_sanitize(raw, raw_len);
    size_t norm_len = 0;
    char *normalized = normalize_markdown(valid, strlen(valid), &norm_len);
    free(valid);

    /* Split normalized text into zero-copy LineSlices */
    size_t line_cap = 32;
    size_t line_count = 0;
    LineSlice *lines = malloc(line_cap * sizeof(LineSlice));
    const char *p = normalized;
    const char *nend = normalized + norm_len;
    for (;;) {
        const char *nl = (p < nend) ? memchr(p, '\n', (size_t)(nend - p)) : NULL;
        const char *le = nl ? nl : nend;
        const char *trimmed_end = (le > p && le[-1] == '\r') ? le - 1 : le;
        if (line_count == line_cap) {
            line_cap *= 2;
            lines = realloc(lines, line_cap * sizeof(LineSlice));
        }
        lines[line_count++] = (LineSlice){ p, trimmed_end };
        if (!nl)
            break;
        p = nl + 1;
    }

    Renderer r;
    memset(&r, 0, sizeof(r));
    dk_str_init_cap(&r.text, norm_len + 32);
    dk_str_init_cap(&r.urls, 64);

    bool in_code_block = false;
    char fence_marker = '\0';
    size_t fence_len = 0;
    size_t code_block_idx = 0;

    for (size_t i = 0; i < line_count; i++) {
        const char *line = lines[i].s;
        const char *end = lines[i].e;

        if (in_code_block) {
            if (is_closing_fence(line, end, fence_marker, fence_len)) {
                in_code_block = false;
                continue;
            }
            bool first_in_code = (r.blocks[code_block_idx].start == r.blocks[code_block_idx].end &&
                                  r.blocks[code_block_idx].start == 0 &&
                                  r.blocks[code_block_idx].level == 0);
            start_line(&r);
            uint32_t start = (uint32_t)r.text.len;
            if (first_in_code) {
                r.blocks[code_block_idx].start = start;
                r.blocks[code_block_idx].level = 1; /* mark started */
            }
            append_text(&r, line, (size_t)(end - line));
            uint32_t stop = (uint32_t)r.text.len;
            r.blocks[code_block_idx].end = stop;
            add_span(&r, start, stop, DK_SPAN_CODE_BLOCK, 0, 0);
            continue;
        }

        /* GFM Table detection */
        if (i + 1 < line_count) {
            Slice hdr[32], delim[32];
            size_t nhdr = 0, ndelim = 0;
            if (parse_table_cells(line, end, hdr, 32, &nhdr) &&
                parse_table_cells(lines[i + 1].s, lines[i + 1].e, delim, 32, &ndelim) &&
                nhdr == ndelim && is_table_delimiter(delim, ndelim)) {
                uint32_t cell_start = (uint32_t)r.cell_count;
                uint32_t tbl_text_start = (uint32_t)(r.lines > 0 ? r.text.len + 1 : r.text.len);
                uint16_t num_rows = 1;
                render_table_row(&r, hdr, nhdr, nhdr, true);
                i++;

                while (i + 1 < line_count) {
                    Slice row[32];
                    size_t nrow = 0;
                    if (!parse_table_cells(lines[i + 1].s, lines[i + 1].e, row, 32, &nrow))
                        break;
                    render_table_row(&r, row, nrow, nhdr, false);
                    num_rows++;
                    i++;
                }
                DkMdBlock *tb = add_block(&r, DK_BLK_TABLE, tbl_text_start, (uint32_t)r.text.len, 0);
                if (tb) {
                    tb->cell_start = cell_start;
                    tb->num_rows = num_rows;
                    tb->num_cols = (uint16_t)nhdr;
                }
                continue;
            }
        }

        /* Fenced code block opener */
        char cand_m;
        size_t cand_l;
        const char *fence_rem;
        if (parse_fence(line, end, &cand_m, &cand_l, &fence_rem)) {
            if (cand_m != '`' || !memchr(fence_rem, '`', (size_t)(end - fence_rem))) {
                in_code_block = true;
                fence_marker = cand_m;
                fence_len = cand_l;
                const char *ls = fence_rem;
                const char *le = end;
                while (ls < le && (*ls == ' ' || *ls == '\t')) ls++;
                while (le > ls && (le[-1] == ' ' || le[-1] == '\t')) le--;
                DkMdBlock *cb = add_block(&r, DK_BLK_CODE, 0, 0, 0);
                if (cb && le > ls) {
                    cb->lang_off = pool_string(&r, ls, (size_t)(le - ls));
                    cb->lang_len = (uint32_t)(le - ls);
                }
                code_block_idx = r.block_count - 1;
                continue;
            }
        }

        start_line(&r);
        uint32_t line_start = (uint32_t)r.text.len;
        unsigned quote_depth = 0;
        const char *content = strip_blockquotes(line, end, &quote_depth);
        unsigned h_level = 0;
        const char *h_content = NULL, *h_end = NULL;

        if (parse_heading(content, end, &h_level, &h_content, &h_end)) {
            render_inline(&r, h_content, h_end, 0);
            uint32_t hflags[6] = {
                DK_SPAN_H1, DK_SPAN_H2, DK_SPAN_H3,
                DK_SPAN_H4, DK_SPAN_H5, DK_SPAN_H6
            };
            add_span(&r, line_start, (uint32_t)r.text.len, hflags[h_level - 1], 0, 0);
            add_block(&r, quote_depth ? DK_BLK_QUOTE : DK_BLK_HEADING,
                      line_start, (uint32_t)r.text.len, (uint8_t)h_level);
        } else if (is_horizontal_rule(content, end)) {
            append_literal(&r, "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80");
            add_span(&r, line_start, (uint32_t)r.text.len, DK_SPAN_HR, 0, 0);
            add_block(&r, DK_BLK_HR, line_start, (uint32_t)r.text.len, 0);
        } else {
            unsigned indent = 0;
            const char *item_content = NULL;
            char prefix_buf[24];
            size_t prefix_len = 0;
            if (parse_list_item(content, end, &indent, &item_content, prefix_buf, &prefix_len)) {
                for (unsigned s = 0; s < indent; s++)
                    dk_str_push(&r.text, ' ');
                append_text(&r, prefix_buf, prefix_len);
                render_inline(&r, item_content, end, 0);
                add_span(&r, line_start, (uint32_t)r.text.len, DK_SPAN_LIST, 0, 0);
                add_block(&r, quote_depth ? DK_BLK_QUOTE : DK_BLK_LIST,
                          line_start, (uint32_t)r.text.len, (uint8_t)indent);
            } else if (content == end && quote_depth == 0) {
                add_block(&r, DK_BLK_EMPTY, line_start, line_start, 0);
            } else {
                render_inline(&r, content, end, 0);
                add_block(&r, quote_depth ? DK_BLK_QUOTE : DK_BLK_PARAGRAPH,
                          line_start, (uint32_t)r.text.len, (uint8_t)quote_depth);
            }
        }

        if (quote_depth > 0)
            add_span(&r, line_start, (uint32_t)r.text.len, DK_SPAN_QUOTE, 0, 0);
    }

    free(lines);
    free(normalized);

    DkMdDoc *doc = calloc(1, sizeof(DkMdDoc));
    if (!doc) {
        dk_str_free(&r.text);
        dk_str_free(&r.urls);
        free(r.spans);
        free(r.blocks);
        free(r.cells);
        return NULL;
    }
    doc->text_len = r.text.len;
    doc->text = dk_str_steal(&r.text);
    doc->urls_len = r.urls.len;
    doc->urls = dk_str_steal(&r.urls);
    doc->spans = r.spans;
    doc->span_count = r.span_count;
    doc->span_cap = r.span_cap;
    doc->blocks = r.blocks;
    doc->block_count = r.block_count;
    doc->block_cap = r.block_cap;
    doc->cells = r.cells;
    doc->cell_count = r.cell_count;
    doc->cell_cap = r.cell_cap;
    return doc;
}

void dk_md_free(DkMdDoc *doc) {
    if (!doc)
        return;
    free(doc->text);
    free(doc->urls);
    free(doc->spans);
    free(doc->blocks);
    free(doc->cells);
    free(doc);
}

uint32_t dk_md_flags_at(const DkMdDoc *doc, size_t byte_offset) {
    if (!doc)
        return 0;
    uint32_t flags = 0;
    for (size_t i = 0; i < doc->span_count; i++) {
        if (byte_offset >= doc->spans[i].start && byte_offset < doc->spans[i].end)
            flags |= doc->spans[i].flags;
    }
    return flags;
}

const char *dk_md_link_at(const DkMdDoc *doc, size_t byte_offset, size_t *out_len) {
    if (!doc)
        return NULL;
    for (size_t i = 0; i < doc->span_count; i++) {
        if ((doc->spans[i].flags & DK_SPAN_LINK) &&
            byte_offset >= doc->spans[i].start && byte_offset < doc->spans[i].end) {
            if (out_len)
                *out_len = doc->spans[i].url_len;
            return doc->urls + doc->spans[i].url_off;
        }
    }
    return NULL;
}
