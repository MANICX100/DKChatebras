#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define DK_TYPE_APPLICATION (dk_application_get_type())
G_DECLARE_FINAL_TYPE(DkApplication, dk_application, DK, APPLICATION, AdwApplication)

DkApplication *dk_application_new(void);

G_END_DECLS
