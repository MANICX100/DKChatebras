#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

void dk_markdown_setup_buffer(GtkTextBuffer *buffer);
void dk_markdown_render(GtkTextBuffer *buffer, const char *markdown);

G_END_DECLS
