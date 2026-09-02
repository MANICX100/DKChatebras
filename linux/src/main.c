#include "config.h"
#include "dk-application.h"

int
main(int argc, char *argv[])
{
  g_autoptr(DkApplication) application = dk_application_new();
  return g_application_run(G_APPLICATION(application), argc, argv);
}
