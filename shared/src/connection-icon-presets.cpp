// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/connection-icon-presets.h>

#include <algorithm>
#include <string_view>

#include <gio/gio.h>

namespace elder_terms {

static constexpr const char *preset_resource_root =
    "/net/kekyo/elder-terms/presets/";

static void append_catalog(std::vector<ConnectionIconPreset> &result,
                           const char *resource_path) {
  GError *error = nullptr;
  auto *bytes = g_resources_lookup_data(
      resource_path,
      G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
  if (error != nullptr) g_error_free(error);
  if (bytes == nullptr) return;
  gsize length = 0;
  const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &length));
  const std::string_view lines(data, length);
  std::size_t start = 0;
  while (start < lines.size()) {
    const auto end = lines.find('\n', start);
    const auto line = lines.substr(start, end == std::string_view::npos
                                          ? std::string_view::npos : end - start);
    const auto first_tab = line.find('\t');
    const auto second_tab = first_tab == std::string_view::npos
                                ? std::string_view::npos : line.find('\t', first_tab + 1);
    if (second_tab != std::string_view::npos) {
      const auto family = line.substr(second_tab + 1);
      if (family == "noto" || family == "material" || family == "glyphs-poly") {
        result.push_back({std::string(line.substr(0, first_tab)),
                          std::string(line.substr(first_tab + 1,
                                                  second_tab - first_tab - 1)),
                          family == "noto" ? ConnectionIconFamily::Noto :
                          family == "material" ? ConnectionIconFamily::Material :
                          ConnectionIconFamily::GlyphsPoly});
      }
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  g_bytes_unref(bytes);
}

const std::vector<ConnectionIconPreset> &connection_icon_presets() {
  static const auto catalog = [] {
    std::vector<ConnectionIconPreset> result;
    append_catalog(result, "/net/kekyo/elder-terms/presets/catalog.tsv");
    append_catalog(result, "/net/kekyo/elder-terms/presets/glyphs-poly/catalog.tsv");
    return result;
  }();
  return catalog;
}

std::shared_ptr<GdkPixbuf> connection_icon_preset_thumbnail(
    const std::string &specification, int pixel_size) {
  if (pixel_size <= 0 || pixel_size > 256) return nullptr;
  const bool white = specification.starts_with("material:white:");
  const auto catalog_specification = white
      ? "material:" + specification.substr(15) : specification;
  const auto &presets = connection_icon_presets();
  const auto found = std::find_if(presets.begin(), presets.end(),
      [&catalog_specification](const ConnectionIconPreset &preset) {
        return preset.specification == catalog_specification;
      });
  if (found == presets.end()) return nullptr;
  std::string resource_path = preset_resource_root;
  if (found->family == ConnectionIconFamily::Noto) {
    resource_path += "noto/" + specification.substr(5) + ".png";
  } else if (found->family == ConnectionIconFamily::GlyphsPoly) {
    resource_path += "glyphs-poly/" + specification.substr(12) + ".png";
  } else {
    resource_path += std::string(white ? "material-white/" : "material/") +
                     catalog_specification.substr(9) + ".png";
  }
  GError *error = nullptr;
  auto *raw = gdk_pixbuf_new_from_resource_at_scale(resource_path.c_str(),
      pixel_size, pixel_size, TRUE, &error);
  if (error != nullptr) g_error_free(error);
  if (raw == nullptr) return nullptr;
  return {raw, g_object_unref};
}

} // namespace elder_terms
