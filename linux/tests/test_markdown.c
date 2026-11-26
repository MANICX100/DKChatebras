#define _POSIX_C_SOURCE 200809L
#include "json.h"
#include "markdown.h"
#include "store.h"
#include "str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { \
        g_pass++; \
    } else { \
        g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", label, __FILE__, __LINE__); \
    } \
} while (0)

static size_t find_offset(const char *haystack, const char *needle) {
    const char *m = strstr(haystack, needle);
    if (!m) {
        fprintf(stderr, "MISSING needle '%s' in:\n%s\n", needle, haystack);
        return (size_t)-1;
    }
    return (size_t)(m - haystack);
}

static void test_basic_markdown(void) {
    const char *markdown =
        "# Heading\n\n"
        "**bold** and *italic* with `code` and [link](https://example.com).\n\n"
        "- item\n\n"
        "```c\n"
        "int x = 1;\n"
        "```";
    const char *expected =
        "Heading\n\n"
        "bold and italic with code and link.\n\n"
        "\xE2\x80\xA2 item\n\n"
        "int x = 1;";

    DkMdDoc *doc = dk_md_parse(markdown);
    CHECK("basic doc non-null", doc != NULL);
    CHECK("basic exact text match", strcmp(doc->text, expected) == 0);
    if (strcmp(doc->text, expected) != 0)
        fprintf(stderr, "EXPECTED:\n[%s]\nGOT:\n[%s]\n", expected, doc->text);

    CHECK("h1 tag", (dk_md_flags_at(doc, find_offset(doc->text, "Heading")) & DK_SPAN_H1) != 0);
    CHECK("strong tag", (dk_md_flags_at(doc, find_offset(doc->text, "bold")) & DK_SPAN_STRONG) != 0);
    CHECK("emphasis tag", (dk_md_flags_at(doc, find_offset(doc->text, "italic")) & DK_SPAN_EMPHASIS) != 0);
    CHECK("code tag", (dk_md_flags_at(doc, find_offset(doc->text, "code")) & DK_SPAN_CODE) != 0);
    size_t link_off = find_offset(doc->text, "link");
    CHECK("link tag", (dk_md_flags_at(doc, link_off) & DK_SPAN_LINK) != 0);
    size_t ulen = 0;
    const char *url = dk_md_link_at(doc, link_off, &ulen);
    CHECK("link url exact", url != NULL && ulen == 19 && memcmp(url, "https://example.com", 19) == 0);
    CHECK("list tag", (dk_md_flags_at(doc, find_offset(doc->text, "\xE2\x80\xA2 item")) & DK_SPAN_LIST) != 0);
    CHECK("code-block tag", (dk_md_flags_at(doc, find_offset(doc->text, "int x")) & DK_SPAN_CODE_BLOCK) != 0);
    dk_md_free(doc);
}

static void test_table_and_html_breaks(void) {
    const char *markdown =
        "| # | Category | Details |\n"
        "|---|----------|---------|\n"
        "| 1 | Varieties | \xE2\x80\xA2 Navel <br>\xE2\x80\xA2 Valencia |\n"
        "| 2 | Nutrients | **Vitamin C**<br/>Fiber<br />Potassium |";
    const char *expected =
        "#  \xE2\x94\x82  Category  \xE2\x94\x82  Details\n"
        "1  \xE2\x94\x82  Varieties  \xE2\x94\x82  \xE2\x80\xA2 Navel \n\xE2\x80\xA2 Valencia\n"
        "2  \xE2\x94\x82  Nutrients  \xE2\x94\x82  Vitamin C\nFiber\nPotassium";

    DkMdDoc *doc = dk_md_parse(markdown);
    CHECK("table exact text match", strcmp(doc->text, expected) == 0);
    if (strcmp(doc->text, expected) != 0)
        fprintf(stderr, "EXPECTED:\n[%s]\nGOT:\n[%s]\n", expected, doc->text);
    CHECK("table no <br", strstr(doc->text, "<br") == NULL);
    CHECK("table no pipe", strchr(doc->text, '|') == NULL);
    CHECK("table-header tag", (dk_md_flags_at(doc, find_offset(doc->text, "Category")) & DK_SPAN_TABLE_HEADER) != 0);
    CHECK("table body tag", (dk_md_flags_at(doc, find_offset(doc->text, "Varieties")) & DK_SPAN_TABLE) != 0);
    CHECK("table cell not header", (dk_md_flags_at(doc, find_offset(doc->text, "Varieties")) & DK_SPAN_TABLE_HEADER) == 0);
    CHECK("table bold cell", (dk_md_flags_at(doc, find_offset(doc->text, "Vitamin C")) & DK_SPAN_STRONG) != 0);
    CHECK("table structured block", doc->block_count == 1 && doc->blocks[0].type == DK_BLK_TABLE &&
                                    doc->blocks[0].num_rows == 3 && doc->blocks[0].num_cols == 3);
    dk_md_free(doc);
}

static void test_latex_and_emoji_regression(void) {
    /* Full parity with Windows v3/latest regression suite (27 assertions) */
    const char *markdown =
        "plain first\n\nplain after breaks\n\n**bold only** and normal text\n\n"
        "| Name | Value |\n| :--- | ---: |\n| Alpha | **Yes** |\n| Beta | risk one<br>risk two |\n\n"
        "Break-even: \\(\\frac{149}{50}=2.98\\) months\n\n"
        "\\[ \\underbrace{C_{r}\\times N}_{\\text{total rent}} \\;<\\; \\underbrace{P - R}_{\\text{net loss}} \\]\n\n"
        "Path stays: C:\\Users\\Dan and code stays: `a \\times b`\n\n"
        "1\xEF\xB8\x8F\xE2\x83\xA3 First keycap and 2\xEF\xB8\x8F\xE2\x83\xA3 second";

    DkMdDoc *doc = dk_md_parse(markdown);
    const char *r = doc->text;

    CHECK("no ** markers", strstr(r, "**") == NULL);
    CHECK("no pipes", strchr(r, '|') == NULL);
    CHECK("no separator row", strstr(r, ":---") == NULL);
    CHECK("no <br>", strstr(r, "<br") == NULL);
    CHECK("no \\frac", strstr(r, "\\frac") == NULL);
    CHECK("no math delimiters", strstr(r, "\\(") == NULL && strstr(r, "\\[") == NULL);
    CHECK("fraction converted", strstr(r, "149\xE2\x81\x84" "50") != NULL);
    CHECK("br split first", strstr(r, "risk one") != NULL);
    CHECK("br split second", strstr(r, "risk two") != NULL);
    CHECK("no underbrace", strstr(r, "underbrace") == NULL);
    CHECK("no subscripts", strstr(r, "_{") == NULL);
    CHECK("no ;<;", strstr(r, ";<;") == NULL);
    CHECK("expression kept", strstr(r, "Cr\xC3\x97 N") != NULL);
    CHECK("annotation kept", strstr(r, "(total rent)") != NULL);
    CHECK("net loss annotation kept", strstr(r, "(net loss)") != NULL);
    CHECK("windows path kept", strstr(r, "C:\\Users\\Dan") != NULL);
    CHECK("inline code kept", strstr(r, "a \\times b") != NULL);
    CHECK("no variation selector", strstr(r, "\xEF\xB8\x8F") == NULL);
    CHECK("no combining keycap", strstr(r, "\xE2\x83\xA3") == NULL);
    CHECK("keycap one readable", strstr(r, "1. First keycap") != NULL);
    CHECK("keycap two readable", strstr(r, "2. second") != NULL);

    CHECK("bold applied", (dk_md_flags_at(doc, find_offset(r, "bold only")) & DK_SPAN_STRONG) != 0);
    CHECK("header bold", (dk_md_flags_at(doc, find_offset(r, "Name")) & DK_SPAN_TABLE_HEADER) != 0);
    CHECK("bold cell bold", (dk_md_flags_at(doc, find_offset(r, "Yes")) & DK_SPAN_STRONG) != 0);
    CHECK("paragraph not bold", (dk_md_flags_at(doc, find_offset(r, "plain after breaks")) & DK_SPAN_STRONG) == 0);
    CHECK("inline text not bold", (dk_md_flags_at(doc, find_offset(r, "normal text")) & DK_SPAN_STRONG) == 0);
    CHECK("body cell not bold", (dk_md_flags_at(doc, find_offset(r, "Alpha")) & (DK_SPAN_STRONG | DK_SPAN_TABLE_HEADER)) == 0);
    CHECK("br cell not bold", (dk_md_flags_at(doc, find_offset(r, "risk one")) & (DK_SPAN_STRONG | DK_SPAN_TABLE_HEADER)) == 0);

    dk_md_free(doc);
}

static void test_invalid_utf8(void) {
    const char md[] = { 'O', 'K', ' ', (char)0xFF, '\0' };
    DkMdDoc *doc = dk_md_parse(md);
    CHECK("invalid utf8 sanitized valid", dk_utf8_validate(doc->text, doc->text_len));
    CHECK("invalid utf8 prefix", strncmp(doc->text, "OK ", 3) == 0);
    CHECK("invalid utf8 len 4 cps", dk_utf8_strlen(doc->text, doc->text_len) == 4);
    dk_md_free(doc);
}

static void test_extra_markdown_constructs(void) {
    const char *md =
        "> quoted *text*\n\n"
        "~~struck~~ and foo_bar_baz and \\*escaped\\*\n\n"
        "---\n\n"
        "1. First\n"
        "2. Second\n\n"
        "Greek: \\alpha + \\beta \\le \\Omega";
    DkMdDoc *doc = dk_md_parse(md);
    CHECK("quote tag", (dk_md_flags_at(doc, find_offset(doc->text, "quoted")) & DK_SPAN_QUOTE) != 0);
    CHECK("strike tag", (dk_md_flags_at(doc, find_offset(doc->text, "struck")) & DK_SPAN_STRIKE) != 0);
    CHECK("intraword underscore preserved", strstr(doc->text, "foo_bar_baz") != NULL);
    CHECK("escaped asterisk preserved", strstr(doc->text, "*escaped*") != NULL);
    CHECK("ordered list kept", strstr(doc->text, "1. First") != NULL && strstr(doc->text, "2. Second") != NULL);
    CHECK("greek converted", strstr(doc->text, "\xCE\xB1 + \xCE\xB2 \xE2\x89\xA4 \xCE\xA9") != NULL);
    dk_md_free(doc);
}

static void test_json_roundtrip(void) {
    const char *sample = "{\"id\":\"abc\",\"count\":42,\"ok\":true,\"text\":\"hello\\n\\u00d7\\uD83D\\uDE00\",\"arr\":[1,null,false]}";
    DkJson *j = dk_json_parse(sample, strlen(sample));
    CHECK("json parsed", j != NULL && j->type == DK_JSON_OBJECT);
    CHECK("json str", strcmp(dk_json_get_str(j, "id", ""), "abc") == 0);
    CHECK("json int", dk_json_get_int(j, "count", 0) == 42);
    const char *txt = dk_json_get_str(j, "text", "");
    CHECK("json unicode + surrogate", strstr(txt, "\xC3\x97\xF0\x9F\x98\x80") != NULL);

    DkStr out;
    dk_str_init(&out);
    dk_json_write(j, &out, false);
    DkJson *j2 = dk_json_parse(out.data, out.len);
    CHECK("json re-parse ok", j2 != NULL && strcmp(dk_json_get_str(j2, "id", ""), "abc") == 0);
    dk_json_free(j2);
    dk_str_free(&out);
    dk_json_free(j);
}

static void test_store_roundtrip(void) {
    char tmpl[] = "/tmp/dk-store-test-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK("store mkdtemp", dir != NULL);
    if (!dir) return;

    setenv("XDG_DATA_HOME", dir, 1);
    dk_store_init();

    DkChat *c = dk_chat_new("gpt-oss-120b");
    CHECK("chat new", c != NULL && c->id != NULL && strlen(c->id) >= 32);
    dk_chat_set_title(c, "Test Conversation");
    CHECK("append user", dk_chat_append(c, "user", "Hello world"));
    CHECK("append assistant", dk_chat_append(c, "assistant", "**Hi** there!"));

    char err[256] = {0};
    CHECK("save sync", dk_store_save_chat_sync(c, err, sizeof(err)));

    DkMetaList list = {0};
    CHECK("load index", dk_store_load_index(&list, err, sizeof(err)));
    CHECK("index count 1", list.count == 1 && strcmp(list.items[0].id, c->id) == 0 &&
                           strcmp(list.items[0].title, "Test Conversation") == 0);
    dk_meta_list_free(&list);

    DkChat *loaded = dk_store_load_chat(c->id, err, sizeof(err));
    CHECK("load chat", loaded != NULL && loaded->msg_count == 2 &&
                       strcmp(loaded->messages[1].content, "**Hi** there!") == 0);

    /* Test async writer + clean shutdown flush */
    dk_chat_append(loaded, "user", "Follow up");
    dk_store_save_chat_async(loaded);
    dk_store_shutdown();

    dk_store_init();
    DkChat *reloaded = dk_store_load_chat(c->id, err, sizeof(err));
    CHECK("async save flushed", reloaded != NULL && reloaded->msg_count == 3 &&
                                strcmp(reloaded->messages[2].content, "Follow up") == 0);
    dk_chat_free(reloaded);

    CHECK("delete chat", dk_store_delete_chat(c->id, err, sizeof(err)));
    CHECK("index empty after delete", dk_store_load_index(&list, err, sizeof(err)) && list.count == 0);
    dk_meta_list_free(&list);

    dk_chat_free(loaded);
    dk_chat_free(c);
    dk_store_shutdown();

    /* Clean up temp dir */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s/dkchatebras/chats/index.json", dir);
    unlink(cmd);
    snprintf(cmd, sizeof(cmd), "%s/dkchatebras/chats", dir);
    rmdir(cmd);
    snprintf(cmd, sizeof(cmd), "%s/dkchatebras", dir);
    rmdir(cmd);
    rmdir(dir);
}

int main(void) {
    test_basic_markdown();
    test_table_and_html_breaks();
    test_latex_and_emoji_regression();
    test_invalid_utf8();
    test_extra_markdown_constructs();
    test_json_roundtrip();
    test_store_roundtrip();

    printf("Passed %d/%d assertions\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
