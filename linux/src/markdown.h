#ifndef DK_MARKDOWN_H
#define DK_MARKDOWN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DK_SPAN_NONE         = 0,
    DK_SPAN_H1           = 1u << 0,
    DK_SPAN_H2           = 1u << 1,
    DK_SPAN_H3           = 1u << 2,
    DK_SPAN_H4           = 1u << 3,
    DK_SPAN_H5           = 1u << 4,
    DK_SPAN_H6           = 1u << 5,
    DK_SPAN_STRONG       = 1u << 6,
    DK_SPAN_EMPHASIS     = 1u << 7,
    DK_SPAN_STRIKE       = 1u << 8,
    DK_SPAN_CODE         = 1u << 9,
    DK_SPAN_CODE_BLOCK   = 1u << 10,
    DK_SPAN_QUOTE        = 1u << 11,
    DK_SPAN_LIST         = 1u << 12,
    DK_SPAN_LINK         = 1u << 13,
    DK_SPAN_HR           = 1u << 14,
    DK_SPAN_TABLE        = 1u << 15,
    DK_SPAN_TABLE_HEADER = 1u << 16,
} DkSpanFlag;

typedef struct {
    uint32_t start;    /* byte offset in doc->text */
    uint32_t end;      /* byte offset in doc->text */
    uint32_t flags;    /* DkSpanFlag bit(s) */
    uint32_t url_off;  /* byte offset in doc->urls (when DK_SPAN_LINK) */
    uint32_t url_len;
} DkMdSpan;

typedef enum {
    DK_BLK_PARAGRAPH = 0,
    DK_BLK_HEADING,
    DK_BLK_CODE,
    DK_BLK_QUOTE,
    DK_BLK_LIST,
    DK_BLK_HR,
    DK_BLK_TABLE,
    DK_BLK_EMPTY,
} DkBlockType;

typedef struct {
    uint32_t start; /* byte offset in doc->text */
    uint32_t end;   /* byte offset in doc->text */
} DkMdCell;

typedef struct {
    DkBlockType type;
    uint32_t start;      /* byte offset in doc->text */
    uint32_t end;        /* byte offset in doc->text */
    uint8_t level;       /* heading 1..6, or quote depth, or list indent */
    uint32_t lang_off;   /* byte offset in doc->urls (code language) */
    uint32_t lang_len;
    /* For DK_BLK_TABLE: */
    uint32_t cell_start; /* index into doc->cells */
    uint16_t num_rows;
    uint16_t num_cols;
} DkMdBlock;

typedef struct {
    char *text;
    size_t text_len;
    char *urls;          /* pool for link destinations and code block languages */
    size_t urls_len;
    DkMdSpan *spans;
    size_t span_count;
    size_t span_cap;
    DkMdBlock *blocks;
    size_t block_count;
    size_t block_cap;
    DkMdCell *cells;
    size_t cell_count;
    size_t cell_cap;
} DkMdDoc;

DkMdDoc *dk_md_parse(const char *markdown);
void dk_md_free(DkMdDoc *doc);

/* Helper for querying combined span flags at a byte offset in doc->text. */
uint32_t dk_md_flags_at(const DkMdDoc *doc, size_t byte_offset);

/* Helper for finding a link URL at a byte offset (returns NULL if none). */
const char *dk_md_link_at(const DkMdDoc *doc, size_t byte_offset, size_t *out_len);

#endif
