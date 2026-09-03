#include "dk-markdown.h"

#include <string.h>

typedef struct {
  gint start;
  gint end;
  const char *tag;
} TextRange;

typedef struct {
  GString *text;
  GArray *ranges;
  gint offset;
  guint lines;
} Renderer;

static void
append_text(Renderer *renderer, const char *text, gssize length)
{
  gsize bytes = length < 0 ? strlen(text) : (gsize) length;

  g_string_append_len(renderer->text, text, bytes);
  renderer->offset += (gint) g_utf8_strlen(text, (gssize) bytes);
}

static void
append_literal(Renderer *renderer, const char *text)
{
  append_text(renderer, text, -1);
}

static void
add_range(Renderer *renderer, gint start, gint end, const char *tag)
{
  if (start < end) {
    TextRange range = { start, end, tag };
    g_array_append_val(renderer->ranges, range);
  }
}

static gboolean
previous_is_space(const char *begin, const char *position)
{
  const char *previous;

  if (position <= begin)
    return FALSE;

  previous = g_utf8_find_prev_char(begin, position);
  return previous != NULL && g_unichar_isspace(g_utf8_get_char(previous));
}

static gboolean
next_is_space(const char *position, const char *end)
{
  return position >= end || g_unichar_isspace(g_utf8_get_char(position));
}

static gboolean
previous_is_alnum(const char *begin, const char *position)
{
  const char *previous;

  if (position <= begin)
    return FALSE;

  previous = g_utf8_find_prev_char(begin, position);
  return previous != NULL && g_unichar_isalnum(g_utf8_get_char(previous));
}

static gboolean
next_is_alnum(const char *position, const char *end)
{
  return position < end && g_unichar_isalnum(g_utf8_get_char(position));
}

static gsize
marker_run(const char *position, const char *end, char marker)
{
  const char *cursor = position;

  while (cursor < end && *cursor == marker)
    cursor++;

  return (gsize) (cursor - position);
}

static const char *
find_code_close(const char *position, const char *end, gsize opener_length)
{
  while (position < end) {
    if (*position == '`') {
      gsize length = marker_run(position, end, '`');
      if (length == opener_length)
        return position;
      position += length;
    } else {
      position = g_utf8_next_char(position);
    }
  }

  return NULL;
}

static const char *
find_emphasis_close(const char *begin,
                    const char *position,
                    const char *end,
                    char marker,
                    gsize marker_length)
{
  while (position < end) {
    if (*position == '\\' && position + 1 < end) {
      position += 2;
      continue;
    }

    if (*position == '`') {
      gsize code_length = marker_run(position, end, '`');
      const char *code_end = find_code_close(position + code_length, end, code_length);
      if (code_end != NULL) {
        position = code_end + code_length;
        continue;
      }
    }

    if (*position == marker) {
      gsize length = marker_run(position, end, marker);
      const char *after = position + length;

      if (length >= marker_length && !previous_is_space(begin, position)) {
        const char *close = after - marker_length;

        if (marker != '_' ||
            !(previous_is_alnum(begin, position) && next_is_alnum(after, end)))
          return close;
      }

      position += length;
      continue;
    }

    position = g_utf8_next_char(position);
  }

  return NULL;
}

static const char *
find_label_close(const char *position, const char *end)
{
  guint depth = 1;

  while (position < end) {
    if (*position == '\\' && position + 1 < end) {
      position += 2;
      continue;
    }

    if (*position == '`') {
      gsize length = marker_run(position, end, '`');
      const char *close = find_code_close(position + length, end, length);
      if (close != NULL) {
        position = close + length;
        continue;
      }
    }

    if (*position == '[') {
      depth++;
    } else if (*position == ']' && --depth == 0) {
      return position;
    }

    position = g_utf8_next_char(position);
  }

  return NULL;
}

static const char *
find_destination_close(const char *position, const char *end)
{
  guint depth = 1;

  while (position < end) {
    if (*position == '\\' && position + 1 < end) {
      position += 2;
      continue;
    }

    if (*position == '(') {
      depth++;
    } else if (*position == ')' && --depth == 0) {
      return position;
    }

    position = g_utf8_next_char(position);
  }

  return NULL;
}

static void render_inline(Renderer *renderer,
                          const char *begin,
                          const char *end,
                          guint depth);

static void
render_code_span(Renderer *renderer, const char *begin, const char *end)
{
  gint start = renderer->offset;
  gboolean trim_spaces = end - begin >= 2 && *begin == ' ' && end[-1] == ' ';
  gboolean all_spaces = TRUE;

  for (const char *cursor = begin; cursor < end; cursor++) {
    if (*cursor != ' ') {
      all_spaces = FALSE;
      break;
    }
  }

  if (trim_spaces && !all_spaces) {
    begin++;
    end--;
  }

  append_text(renderer, begin, end - begin);
  add_range(renderer, start, renderer->offset, "code");
}

static gsize
html_break_length(const char *position, const char *end)
{
  gsize remaining = (gsize) (end - position);

  if (remaining >= 6 && g_ascii_strncasecmp(position, "<br />", 6) == 0)
    return 6;
  if (remaining >= 5 && g_ascii_strncasecmp(position, "<br/>", 5) == 0)
    return 5;
  if (remaining >= 4 && g_ascii_strncasecmp(position, "<br>", 4) == 0)
    return 4;
  return 0;
}

static void
render_inline(Renderer *renderer, const char *begin, const char *end, guint depth)
{
  const char *cursor = begin;

  if (depth > 64) {
    append_text(renderer, begin, end - begin);
    return;
  }

  while (cursor < end) {
    gsize break_length = html_break_length(cursor, end);
    if (break_length > 0) {
      append_literal(renderer, "\n");
      cursor += break_length;
      continue;
    }

    if (*cursor == '\\' && cursor + 1 < end &&
        ((guchar) cursor[1]) < 0x80 && g_ascii_ispunct(cursor[1])) {
      append_text(renderer, cursor + 1, 1);
      cursor += 2;
      continue;
    }

    if (*cursor == '`') {
      gsize length = marker_run(cursor, end, '`');
      const char *close = find_code_close(cursor + length, end, length);

      if (close != NULL) {
        render_code_span(renderer, cursor + length, close);
        cursor = close + length;
        continue;
      }
    }

    if (*cursor == '[') {
      const char *label_end = find_label_close(cursor + 1, end);

      if (label_end != NULL && label_end + 1 < end && label_end[1] == '(') {
        const char *destination_end = find_destination_close(label_end + 2, end);

        if (destination_end != NULL) {
          gint start = renderer->offset;
          render_inline(renderer, cursor + 1, label_end, depth + 1);
          add_range(renderer, start, renderer->offset, "link");
          cursor = destination_end + 1;
          continue;
        }
      }
    }

    if (*cursor == '~' && marker_run(cursor, end, '~') >= 2 &&
        cursor + 2 < end && !next_is_space(cursor + 2, end)) {
      const char *close = find_emphasis_close(begin, cursor + 2, end, '~', 2);

      if (close != NULL) {
        gint start = renderer->offset;
        render_inline(renderer, cursor + 2, close, depth + 1);
        add_range(renderer, start, renderer->offset, "strikethrough");
        cursor = close + 2;
        continue;
      }
    }

    if (*cursor == '*' || *cursor == '_') {
      char marker = *cursor;
      gsize run = marker_run(cursor, end, marker);
      gsize length = run >= 3 ? 3 : run >= 2 ? 2 : 1;
      const char *content = cursor + length;
      gboolean can_open = content < end && !next_is_space(content, end) &&
                          (marker != '_' ||
                           !(previous_is_alnum(begin, cursor) &&
                             next_is_alnum(content, end)));

      if (can_open) {
        const char *close = find_emphasis_close(begin, content, end, marker, length);

        if (close != NULL) {
          gint start = renderer->offset;
          render_inline(renderer, content, close, depth + 1);
          if (length >= 2)
            add_range(renderer, start, renderer->offset, "strong");
          if (length == 1 || length == 3)
            add_range(renderer, start, renderer->offset, "emphasis");
          cursor = close + length;
          continue;
        }
      }
    }

    const char *next = g_utf8_next_char(cursor);
    append_text(renderer, cursor, next - cursor);
    cursor = next;
  }
}

static void
start_line(Renderer *renderer)
{
  if (renderer->lines > 0)
    append_literal(renderer, "\n");
  renderer->lines++;
}

static gboolean
parse_fence(const char *line,
            const char *end,
            char *marker,
            gsize *length,
            const char **remainder)
{
  const char *cursor = line;
  guint spaces = 0;

  while (cursor < end && *cursor == ' ' && spaces < 4) {
    cursor++;
    spaces++;
  }

  if (spaces > 3 || cursor >= end || (*cursor != '`' && *cursor != '~'))
    return FALSE;

  *marker = *cursor;
  *length = marker_run(cursor, end, *marker);
  if (*length < 3)
    return FALSE;

  *remainder = cursor + *length;
  return TRUE;
}

static gboolean
is_closing_fence(const char *line,
                 const char *end,
                 char marker,
                 gsize opener_length)
{
  char candidate_marker;
  gsize candidate_length;
  const char *remainder;

  if (!parse_fence(line, end, &candidate_marker, &candidate_length, &remainder) ||
      candidate_marker != marker || candidate_length < opener_length)
    return FALSE;

  while (remainder < end && (*remainder == ' ' || *remainder == '\t'))
    remainder++;

  return remainder == end;
}

static gboolean
is_horizontal_rule(const char *line, const char *end)
{
  const char *cursor = line;
  char marker;
  guint count = 0;
  guint leading_spaces = 0;

  while (cursor < end && *cursor == ' ' && leading_spaces < 4) {
    cursor++;
    leading_spaces++;
  }
  if (leading_spaces > 3 || cursor >= end ||
      (*cursor != '*' && *cursor != '-' && *cursor != '_'))
    return FALSE;

  marker = *cursor;
  while (cursor < end) {
    if (*cursor == marker)
      count++;
    else if (*cursor != ' ' && *cursor != '\t')
      return FALSE;
    cursor++;
  }

  return count >= 3;
}

static const char *
strip_blockquotes(const char *line, const char *end, guint *depth)
{
  const char *cursor = line;

  *depth = 0;
  while (cursor < end) {
    const char *candidate = cursor;
    guint spaces = 0;

    while (candidate < end && *candidate == ' ' && spaces < 4) {
      candidate++;
      spaces++;
    }
    if (spaces > 3 || candidate >= end || *candidate != '>')
      break;

    cursor = candidate + 1;
    if (cursor < end && (*cursor == ' ' || *cursor == '\t'))
      cursor++;
    (*depth)++;
  }

  return cursor;
}

static gboolean
parse_heading(const char *line,
              const char *end,
              guint *level,
              const char **content,
              const char **content_end)
{
  const char *cursor = line;
  guint spaces = 0;

  while (cursor < end && *cursor == ' ' && spaces < 4) {
    cursor++;
    spaces++;
  }
  if (spaces > 3 || cursor >= end || *cursor != '#')
    return FALSE;

  *level = (guint) marker_run(cursor, end, '#');
  if (*level == 0 || *level > 6)
    return FALSE;
  cursor += *level;
  if (cursor < end && *cursor != ' ' && *cursor != '\t')
    return FALSE;
  while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
    cursor++;

  *content = cursor;
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

  return TRUE;
}

static GPtrArray *
parse_table_cells(const char *line, const char *end)
{
  g_autofree char *copy = g_strndup(line, end - line);
  char *body = g_strstrip(copy);

  if (strchr(body, '|') == NULL)
    return NULL;
  if (*body == '|')
    body++;

  gsize length = strlen(body);
  if (length > 0 && body[length - 1] == '|')
    body[length - 1] = '\0';

  g_auto(GStrv) parts = g_strsplit(body, "|", -1);
  GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; parts[i] != NULL; i++)
    g_ptr_array_add(cells, g_strdup(g_strstrip(parts[i])));
  return cells;
}

static gboolean
is_table_delimiter(GPtrArray *cells)
{
  if (cells == NULL || cells->len == 0)
    return FALSE;

  for (guint i = 0; i < cells->len; i++) {
    const char *cursor = g_ptr_array_index(cells, i);
    guint dashes = 0;

    if (*cursor == ':')
      cursor++;
    while (*cursor == '-') {
      dashes++;
      cursor++;
    }
    if (*cursor == ':')
      cursor++;
    if (*cursor != '\0' || dashes < 3)
      return FALSE;
  }

  return TRUE;
}

static void
render_table_row(Renderer *renderer, GPtrArray *cells, gboolean header)
{
  start_line(renderer);
  gint start = renderer->offset;

  for (guint i = 0; i < cells->len; i++) {
    const char *cell = g_ptr_array_index(cells, i);
    if (i > 0)
      append_literal(renderer, "  │  ");
    render_inline(renderer, cell, cell + strlen(cell), 0);
  }

  add_range(renderer, start, renderer->offset, "table");
  if (header)
    add_range(renderer, start, renderer->offset, "table-header");
}

static gboolean
parse_list_item(const char *line,
                const char *end,
                guint *indent,
                const char **content,
                char **prefix)
{
  const char *cursor = line;
  guint columns = 0;

  while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
    columns += *cursor == '\t' ? 4 : 1;
    cursor++;
  }

  if (cursor + 1 < end && (*cursor == '-' || *cursor == '*' || *cursor == '+') &&
      (cursor[1] == ' ' || cursor[1] == '\t')) {
    *indent = MIN(columns, 12);
    *content = cursor + 2;
    *prefix = g_strdup("• ");
    return TRUE;
  }

  const char *digits = cursor;
  while (cursor < end && g_ascii_isdigit(*cursor) && cursor - digits < 9)
    cursor++;
  if (cursor > digits && cursor + 1 < end && (*cursor == '.' || *cursor == ')') &&
      (cursor[1] == ' ' || cursor[1] == '\t')) {
    *indent = MIN(columns, 12);
    *content = cursor + 2;
    *prefix = g_strdup_printf("%.*s%c ", (int) (cursor - digits), digits, *cursor);
    return TRUE;
  }

  return FALSE;
}

void
dk_markdown_setup_buffer(GtkTextBuffer *buffer)
{
  gtk_text_buffer_create_tag(buffer, "h1", "weight", PANGO_WEIGHT_BOLD, "scale", 1.60, "pixels-above-lines", 12, "pixels-below-lines", 6, NULL);
  gtk_text_buffer_create_tag(buffer, "h2", "weight", PANGO_WEIGHT_BOLD, "scale", 1.35, "pixels-above-lines", 10, "pixels-below-lines", 5, NULL);
  gtk_text_buffer_create_tag(buffer, "h3", "weight", PANGO_WEIGHT_BOLD, "scale", 1.20, "pixels-above-lines", 8, "pixels-below-lines", 4, NULL);
  gtk_text_buffer_create_tag(buffer, "h4", "weight", PANGO_WEIGHT_BOLD, "scale", 1.10, "pixels-above-lines", 7, "pixels-below-lines", 3, NULL);
  gtk_text_buffer_create_tag(buffer, "h5", "weight", PANGO_WEIGHT_BOLD, "scale", 1.00, "pixels-above-lines", 6, "pixels-below-lines", 2, NULL);
  gtk_text_buffer_create_tag(buffer, "h6", "weight", PANGO_WEIGHT_BOLD, "scale", 0.90, "pixels-above-lines", 6, "pixels-below-lines", 2, NULL);
  gtk_text_buffer_create_tag(buffer, "strong", "weight", PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(buffer, "emphasis", "style", PANGO_STYLE_ITALIC, NULL);
  gtk_text_buffer_create_tag(buffer, "strikethrough", "strikethrough", TRUE, NULL);
  gtk_text_buffer_create_tag(buffer, "code", "family", "monospace", NULL);
  gtk_text_buffer_create_tag(buffer, "code-block", "family", "monospace", "left-margin", 12, "right-margin", 12, "pixels-above-lines", 8, "pixels-below-lines", 8, NULL);
  gtk_text_buffer_create_tag(buffer, "quote", "style", PANGO_STYLE_ITALIC, "left-margin", 18, "foreground", "gray", NULL);
  gtk_text_buffer_create_tag(buffer, "list", "left-margin", 12, "indent", -8, NULL);
  gtk_text_buffer_create_tag(buffer, "link", "underline", PANGO_UNDERLINE_SINGLE, "foreground", "#3584e4", NULL);
  gtk_text_buffer_create_tag(buffer, "horizontal-rule", "justification", GTK_JUSTIFY_CENTER, "foreground", "gray", "pixels-above-lines", 6, "pixels-below-lines", 6, NULL);
  gtk_text_buffer_create_tag(buffer, "table", "family", "monospace", "pixels-above-lines", 2, "pixels-below-lines", 2, NULL);
  gtk_text_buffer_create_tag(buffer, "table-header", "weight", PANGO_WEIGHT_BOLD, "underline", PANGO_UNDERLINE_SINGLE, NULL);
}

/* Maps bare LaTeX macros to display text; wrapper macros map to "" and keep their argument. */
static const char *
latex_symbol(const char *name, gsize length)
{
  static const struct { const char *name; const char *symbol; } table[] = {
    { "times", "\303\227" }, { "div", "\303\267" }, { "cdot", "\302\267" },
    { "pm", "\302\261" }, { "mp", "\342\210\223" }, { "approx", "\342\211\210" },
    { "le", "\342\211\244" }, { "leq", "\342\211\244" }, { "ge", "\342\211\245" }, { "geq", "\342\211\245" },
    { "ne", "\342\211\240" }, { "neq", "\342\211\240" }, { "sim", "~" }, { "propto", "\342\210\235" },
    { "infty", "\342\210\236" }, { "to", "\342\206\222" }, { "rightarrow", "\342\206\222" },
    { "leftarrow", "\342\206\220" }, { "Rightarrow", "\342\207\222" }, { "Leftarrow", "\342\207\220" },
    { "leftrightarrow", "\342\206\224" }, { "sum", "\316\243" }, { "prod", "\316\240" },
    { "int", "\342\210\253" }, { "sqrt", "\342\210\232" }, { "ldots", "\342\200\246" },
    { "dots", "\342\200\246" }, { "cdots", "\342\213\257" }, { "quad", " " }, { "qquad", "  " },
    { "left", "" }, { "right", "" }, { "text", "" }, { "mathrm", "" }, { "mathbf", "" },
    { "mathit", "" }, { "operatorname", "" }, { "displaystyle", "" }, { "boxed", "" },
    { "alpha", "\316\261" }, { "beta", "\316\262" }, { "gamma", "\316\263" }, { "delta", "\316\264" },
    { "epsilon", "\316\265" }, { "zeta", "\316\266" }, { "eta", "\316\267" }, { "theta", "\316\270" },
    { "lambda", "\316\273" }, { "mu", "\316\274" }, { "nu", "\316\275" }, { "xi", "\316\276" },
    { "pi", "\317\200" }, { "rho", "\317\201" }, { "sigma", "\317\203" }, { "tau", "\317\204" },
    { "phi", "\317\206" }, { "chi", "\317\207" }, { "psi", "\317\210" }, { "omega", "\317\211" },
    { "Gamma", "\316\223" }, { "Delta", "\316\224" }, { "Theta", "\316\230" }, { "Lambda", "\316\233" },
    { "Pi", "\316\240" }, { "Sigma", "\316\243" }, { "Phi", "\316\246" }, { "Psi", "\316\250" },
    { "Omega", "\316\251" },
  };

  for (gsize i = 0; i < G_N_ELEMENTS(table); i++) {
    if (strlen(table[i].name) == length && strncmp(table[i].name, name, length) == 0)
      return table[i].symbol;
  }
  return NULL;
}

static gssize
match_brace(const char *text, gsize length, gsize open)
{
  int depth = 0;

  for (gsize j = open; j < length; j++) {
    if (text[j] == '\\') { j++; continue; }
    if (text[j] == '{') depth++;
    else if (text[j] == '}' && --depth == 0) return (gssize) j;
  }
  return -1;
}

/* Generically unwraps LaTeX macros, scripts, and HTML breaks in prose text. */
static char *
strip_math(const char *segment, gsize segment_length)
{
  char *text = g_strndup(segment, segment_length);

  for (int pass = 0; pass < 8; pass++) {
    const gsize length = strlen(text);
    GString *out = g_string_sized_new(length);
    gsize i = 0;

    while (i < length) {
      const char c = text[i];

      /* <br> tags are left intact here; render_inline converts them to line
       * breaks after block parsing, so table rows are not split apart. */
      if (c == '\\' && i + 1 < length) {
        const char next = text[i + 1];
        if (g_ascii_isalpha(next)) {
          gsize word_end = i + 1;
          while (word_end < length && g_ascii_isalpha(text[word_end]))
            word_end++;
          const char *name = text + i + 1;
          const gsize name_length = word_end - i - 1;
          if (word_end < length && text[word_end] == '{') {
            const gssize close = match_brace(text, length, word_end);
            if (close >= 0) {
              const gboolean is_fraction =
                  (name_length == 4 && strncmp(name, "frac", 4) == 0) ||
                  (name_length == 5 && (strncmp(name, "dfrac", 5) == 0 || strncmp(name, "tfrac", 5) == 0));
              if (is_fraction && (gsize) close + 1 < length && text[close + 1] == '{') {
                const gssize close2 = match_brace(text, length, (gsize) close + 1);
                if (close2 >= 0) {
                  g_string_append_len(out, text + word_end + 1, close - (gssize) word_end - 1);
                  g_string_append(out, "\342\201\204");
                  g_string_append_len(out, text + close + 2, close2 - close - 2);
                  i = (gsize) close2 + 1;
                  continue;
                }
              }
              const char *symbol = latex_symbol(name, name_length);
              if (symbol != NULL)
                g_string_append(out, symbol);
              g_string_append_len(out, text + word_end + 1, close - (gssize) word_end - 1);
              i = (gsize) close + 1;
              continue;
            }
          }
          const char *symbol = latex_symbol(name, name_length);
          if (symbol != NULL) {
            g_string_append(out, symbol);
            i = word_end;
            continue;
          }
          g_string_append_len(out, text + i, word_end - i);
          i = word_end;
          continue;
        }
        if (next == '[' || next == ']' || next == '(' || next == ')') { i += 2; continue; }
        if (next == ';' || next == ',' || next == ':' || next == '!') {
          g_string_append_c(out, ' ');
          i += 2;
          continue;
        }
      }
      if ((c == '_' || c == '^') && i + 1 < length && text[i + 1] == '{') {
        const gssize close = match_brace(text, length, i + 1);
        if (close >= 0) {
          const gsize argument_length = (gsize) close - i - 2;
          if (argument_length <= 3) {
            g_string_append_len(out, text + i + 2, (gssize) argument_length);
          } else {
            g_string_append(out, " (");
            g_string_append_len(out, text + i + 2, (gssize) argument_length);
            g_string_append_c(out, ')');
          }
          i = (gsize) close + 1;
          continue;
        }
      }
      g_string_append_c(out, c);
      i++;
    }

    char *dollars;
    while ((dollars = strstr(out->str, "$$")) != NULL)
      g_string_erase(out, dollars - out->str, 2);

    if (g_str_equal(out->str, text)) {
      g_string_free(out, TRUE);
      break;
    }
    g_free(text);
    text = g_string_free(out, FALSE);
  }
  return text;
}

/* Applies strip_math outside fenced code blocks and inline code spans. */
static char *
normalize_markdown(const char *markdown)
{
  g_auto(GStrv) lines = g_strsplit(markdown, "\n", -1);
  GString *result = g_string_sized_new(strlen(markdown));
  gboolean fenced = FALSE;

  for (guint index = 0; lines[index] != NULL; index++) {
    const char *line = lines[index];
    const char *cursor = line;

    while (*cursor == ' ' || *cursor == '\t')
      cursor++;

    if (g_str_has_prefix(cursor, "```") || g_str_has_prefix(cursor, "~~~")) {
      fenced = !fenced;
      g_string_append(result, line);
    } else if (fenced) {
      g_string_append(result, line);
    } else {
      const char *start = line;
      gboolean code = FALSE;
      for (;;) {
        const char *tick = strchr(start, '`');
        const gsize part_length = tick != NULL ? (gsize) (tick - start) : strlen(start);
        if (code) {
          g_string_append_len(result, start, (gssize) part_length);
        } else {
          g_autofree char *stripped = strip_math(start, part_length);
          g_string_append(result, stripped);
        }
        if (tick == NULL)
          break;
        g_string_append_c(result, '`');
        code = !code;
        start = tick + 1;
      }
    }
    if (lines[index + 1] != NULL)
      g_string_append_c(result, '\n');
  }
  return g_string_free(result, FALSE);
}

void
dk_markdown_render(GtkTextBuffer *buffer, const char *markdown)
{
  g_autofree char *valid = g_utf8_make_valid(markdown != NULL ? markdown : "", -1);
  g_autofree char *normalized = normalize_markdown(valid);
  g_auto(GStrv) lines = g_strsplit(normalized, "\n", -1);
  g_autoptr(GString) text = g_string_new(NULL);
  g_autoptr(GArray) ranges = g_array_new(FALSE, FALSE, sizeof(TextRange));
  Renderer renderer = { text, ranges, 0, 0 };
  gboolean in_code_block = FALSE;
  char fence_marker = '\0';
  gsize fence_length = 0;

  for (guint i = 0; lines[i] != NULL; i++) {
    const char *line = lines[i];
    const char *end = line + strlen(line);
    char candidate_marker;
    gsize candidate_length;
    const char *fence_remainder;

    if (end > line && end[-1] == '\r')
      end--;

    if (in_code_block) {
      if (is_closing_fence(line, end, fence_marker, fence_length)) {
        in_code_block = FALSE;
        continue;
      }

      start_line(&renderer);
      gint start = renderer.offset;
      append_text(&renderer, line, end - line);
      add_range(&renderer, start, renderer.offset, "code-block");
      continue;
    }

    if (lines[i + 1] != NULL) {
      const char *delimiter_line = lines[i + 1];
      const char *delimiter_end = delimiter_line + strlen(delimiter_line);
      g_autoptr(GPtrArray) header_cells = parse_table_cells(line, end);
      g_autoptr(GPtrArray) delimiter_cells = parse_table_cells(delimiter_line, delimiter_end);

      if (header_cells != NULL && delimiter_cells != NULL &&
          header_cells->len == delimiter_cells->len &&
          is_table_delimiter(delimiter_cells)) {
        render_table_row(&renderer, header_cells, TRUE);
        i++;

        while (lines[i + 1] != NULL) {
          const char *row_line = lines[i + 1];
          const char *row_end = row_line + strlen(row_line);
          g_autoptr(GPtrArray) row_cells = parse_table_cells(row_line, row_end);
          if (row_cells == NULL)
            break;
          render_table_row(&renderer, row_cells, FALSE);
          i++;
        }
        continue;
      }
    }

    if (parse_fence(line, end, &candidate_marker, &candidate_length, &fence_remainder)) {
      if (candidate_marker != '`' ||
          memchr(fence_remainder, '`', (gsize) (end - fence_remainder)) == NULL) {
        in_code_block = TRUE;
        fence_marker = candidate_marker;
        fence_length = candidate_length;
        continue;
      }
    }

    start_line(&renderer);
    gint line_start = renderer.offset;
    guint quote_depth;
    const char *content = strip_blockquotes(line, end, &quote_depth);
    guint heading_level;
    const char *heading_content;
    const char *heading_end;

    if (parse_heading(content, end, &heading_level, &heading_content, &heading_end)) {
      render_inline(&renderer, heading_content, heading_end, 0);
      const char *heading_tags[] = { "h1", "h2", "h3", "h4", "h5", "h6" };
      add_range(&renderer, line_start, renderer.offset, heading_tags[heading_level - 1]);
    } else if (is_horizontal_rule(content, end)) {
      append_literal(&renderer, "────────────");
      add_range(&renderer, line_start, renderer.offset, "horizontal-rule");
    } else {
      guint indent;
      const char *item_content;
      g_autofree char *prefix = NULL;

      if (parse_list_item(content, end, &indent, &item_content, &prefix)) {
        for (guint space = 0; space < indent; space++)
          append_literal(&renderer, " ");
        append_literal(&renderer, prefix);
        render_inline(&renderer, item_content, end, 0);
        add_range(&renderer, line_start, renderer.offset, "list");
      } else {
        render_inline(&renderer, content, end, 0);
      }
    }

    if (quote_depth > 0)
      add_range(&renderer, line_start, renderer.offset, "quote");
  }

  gtk_text_buffer_set_text(buffer, renderer.text->str, (gint) renderer.text->len);
  for (guint i = 0; i < renderer.ranges->len; i++) {
    const TextRange *range = &g_array_index(renderer.ranges, TextRange, i);
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_iter_at_offset(buffer, &start, range->start);
    gtk_text_buffer_get_iter_at_offset(buffer, &end, range->end);
    gtk_text_buffer_apply_tag_by_name(buffer, range->tag, &start, &end);
  }
}
