#include <elder-terms/localization.h>

#include <cstdlib>

#include <glib.h>
#include <libintl.h>

#include "localization-config.h"

namespace elder_terms {

static constexpr char gettext_package[] = "elder-terms";
static constexpr char locale_directory_environment[] =
    "ELDER_TERMS_LOCALE_DIR";

void initialize_localization() {
  const char *override_directory =
      g_getenv(locale_directory_environment);
  const char *locale_directory =
      override_directory != nullptr && override_directory[0] != '\0'
          ? override_directory
          : ELDER_TERMS_LOCALE_DIR;
  bindtextdomain(gettext_package, locale_directory);
  bind_textdomain_codeset(gettext_package, "UTF-8");
}

} // namespace elder_terms
