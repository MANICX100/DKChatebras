#include "dk-markdown.h"

#include <glib.h>
#include <string.h>

static gint
text_offset(const char *text, const char *needle)
{
  const char *match = strstr(text, needle);
  g_assert_nonnull(match);
  return (gint) g_utf8_pointer_to_offset(text, match);
}

static void
assert_tag_at(GtkTextBuffer *buffer, const char *tag_name, gint offset)
{
  GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), tag_name);
  GtkTextIter iter;

  g_assert_nonnull(tag);
  gtk_text_buffer_get_iter_at_offset(buffer, &iter, offset);
  g_assert_true(gtk_text_iter_has_tag(&iter, tag));
}

static void
test_markdown_rendering(void)
{
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
    "• item\n\n"
    "int x = 1;";
  g_autoptr(GtkTextBuffer) buffer = gtk_text_buffer_new(NULL);

  dk_markdown_setup_buffer(buffer);
  dk_markdown_render(buffer, markdown);

  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *rendered = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

  g_assert_cmpstr(rendered, ==, expected);
  assert_tag_at(buffer, "h1", text_offset(rendered, "Heading"));
  assert_tag_at(buffer, "strong", text_offset(rendered, "bold"));
  assert_tag_at(buffer, "emphasis", text_offset(rendered, "italic"));
  assert_tag_at(buffer, "code", text_offset(rendered, "code"));
  assert_tag_at(buffer, "link", text_offset(rendered, "link"));
  assert_tag_at(buffer, "list", text_offset(rendered, "• item"));
  assert_tag_at(buffer, "code-block", text_offset(rendered, "int x"));
}

static void
test_invalid_utf8_is_visible(void)
{
  const char markdown[] = { 'O', 'K', ' ', (char) 0xff, '\0' };
  g_autoptr(GtkTextBuffer) buffer = gtk_text_buffer_new(NULL);

  dk_markdown_setup_buffer(buffer);
  dk_markdown_render(buffer, markdown);

  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *rendered = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

  g_assert_true(g_utf8_validate(rendered, -1, NULL));
  g_assert_true(g_str_has_prefix(rendered, "OK "));
  g_assert_cmpint(g_utf8_strlen(rendered, -1), ==, 4);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/markdown/rendering", test_markdown_rendering);
  g_test_add_func("/markdown/invalid-utf8", test_invalid_utf8_is_visible);
  return g_test_run();
}
