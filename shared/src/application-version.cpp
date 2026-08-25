#include <elder-terms/application-version.h>

#include "application-version-config.h"

namespace elder_terms {

const char *application_version() {
  return ELDER_TERMS_APPLICATION_VERSION;
}

} // namespace elder_terms
