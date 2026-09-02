#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define DK_TYPE_WINDOW (dk_window_get_type())
G_DECLARE_FINAL_TYPE(DkWindow, dk_window, DK, WINDOW, AdwApplicationWindow)

DkWindow *dk_window_new(GtkApplication *application);

G_END_DECLS
