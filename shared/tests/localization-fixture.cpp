#include <elder-terms/localization.h>
#include <elder-terms/settings.h>

#include <iostream>
#include <string>

#include <libintl.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: elder-terms-localization-fixture GLOBAL_CONFIG\n";
    return 2;
  }

  const elder_terms::ApplicationUiLanguage language =
      elder_terms::load_application_ui_language_preference(argv[1]);
  const elder_terms::LocalizationInitializationResult localization =
      elder_terms::initialize_localization(language);
  for (const std::string &warning : localization.warnings) {
    std::cerr << warning << '\n';
  }

  std::cout << dgettext("elder-terms", "Settings") << '\n';
  std::cout << dgettext("elder-terms", "Links") << '\n';
  std::cout << dgettext("elder-terms", "Calculating hash values…") << '\n';
  std::cout << dgettext("elder-terms", "Hash calculation cancelled") << '\n';
  std::cout << dgettext("elder-terms", "Hash calculation failed") << '\n';
  std::cout << dgettext("elder-terms", "Failed to calculate hash values")
            << '\n';
  std::cout << dgettext("elder-terms", "Hash calculation complete") << '\n';
  std::cout << dgettext("elder-terms", "File hash values") << '\n';
  std::cout << dgettext("elder-terms", "Calculate Hash Values") << '\n';
  return localization.requested_language_applied ? 0 : 1;
}
