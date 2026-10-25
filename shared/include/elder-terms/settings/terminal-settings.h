// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/key-binding.h>
#include <elder-terms/settings/settings-store.h>
#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

/** Terminal columns assigned to Unicode East Asian Ambiguous characters. */
enum class TerminalAmbiguousWidth {
  narrow, /**< Use one terminal column, the built-in default. */
  wide,   /**< Use two terminal columns. */
};
/** @returns Key for the terminal width of ambiguous characters. */
ELDER_TERMS_API SettingKey terminal_ambiguous_width_setting_key();
/**
 * Reads the width policy used when a new terminal is created.
 * @param store Layered terminal settings.
 * @returns Effective one-column or two-column policy.
 * @remarks Changes do not alter an existing terminal or its history.
 */
ELDER_TERMS_API TerminalAmbiguousWidth terminal_ambiguous_width(const SettingsStore &store);

/** Display-only correction; terminal bytes and logical colors are preserved. */
enum class TerminalColorFilter {
  none,  /**< Preserve the terminal colors after connection defaults. */
  reverse_lightness, /**< Reflect Oklab lightness while preserving hue (A). */
  reverse_contrast, /**< Reverse relative luminance while preserving contrast ratios (B). */
  reverse_roles, /**< Reverse neutrals and adapt chromatic text to its displayed background (C). */
  reverse_cam16, /**< Add CAM16 appearance compensation between fixed viewing conditions (D). */
  reverse_palette, /**< Optimize the palette jointly and interpolate true colors continuously (E). */
  oklch, /**< Improve low-contrast text and markers of any hue, including neutrals, in Oklch. */
};
/** Color-vision profile used for display recoloring, not a diagnosis. */
enum class TerminalColorVision {
  none,   /**< Disable color-vision recoloring. */
  protan, /**< Assist red-green discrimination using the protan profile. */
  deutan, /**< Assist red-green discrimination using the deutan profile. */
  tritan, /**< Assist blue-yellow discrimination using the tritan profile. */
};
/** @returns Key for the selected display-color correction. */
ELDER_TERMS_API SettingKey terminal_color_filter_setting_key();
/** @param store Layered settings. @returns Selected display correction. */
ELDER_TERMS_API TerminalColorFilter terminal_color_filter(const SettingsStore &store);
/** Independent preferences for color discrimination assistance. */
struct TerminalColorVisionSettings {
  TerminalColorVision profile; /**< Recoloring profile; none disables assistance. */
  int strength; /**< Adjustment from zero through one hundred, not clinical severity. */
};
/** @returns Key for the color-vision profile. */
ELDER_TERMS_API SettingKey terminal_color_vision_setting_key();
/** @returns Key for the independent recoloring strength. */
ELDER_TERMS_API SettingKey terminal_color_vision_strength_setting_key();
/** @param store Layered settings. @returns Effective recoloring preferences. */
ELDER_TERMS_API TerminalColorVisionSettings terminal_color_vision_settings(const SettingsStore &store);

/** Desired desktop background mode; actual support is determined by the window owner. */
enum class TerminalBackgroundMode {
  opaque,      /**< Opaque application backing. */
  transparent, /**< Prefer desktop alpha composition when supported. */
  blur,        /**< Prefer compositor blur behind the transparent terminal region. */
};
/** Independent cell-background and tint-layer preferences. */
struct TerminalBackgroundSettings {
  TerminalBackgroundMode mode; /**< Requested desktop transparency. */
  int opacity;                /**< Cell background opacity, zero through one hundred. */
  RgbColor tint;              /**< Unassociated sRGB tint color. */
  int tint_opacity;           /**< Tint-layer opacity, zero through one hundred. */
};
/** @returns Key for the requested background mode. */
ELDER_TERMS_API SettingKey terminal_background_mode_setting_key();
/** @returns Key for terminal cell background opacity. */
ELDER_TERMS_API SettingKey terminal_background_opacity_setting_key();
/** @returns Key for the independent RGB tint. */
ELDER_TERMS_API SettingKey terminal_background_tint_color_setting_key();
/** @returns Key for tint-layer opacity. */
ELDER_TERMS_API SettingKey terminal_background_tint_opacity_setting_key();
/** @param store Layered settings. @returns Requested background preferences. */
ELDER_TERMS_API TerminalBackgroundSettings terminal_background_settings(const SettingsStore &store);

/** Centered background-image placement. */
enum class TerminalBackgroundImageFit {
  cover,   /**< Fill the viewport, cropping while preserving aspect ratio. */
  contain, /**< Fit the whole image, preserving aspect ratio and leaving margins. */
  stretch, /**< Scale both axes independently to fill the viewport. */
};
/** Background image preferences, independent of desktop transparency. */
struct TerminalBackgroundImageSettings {
  std::string path;                 /**< Absolute UTF-8 filename; empty explicitly disables the image. */
  int opacity;                      /**< Image opacity from zero through one hundred. */
  TerminalBackgroundImageFit fit;   /**< Centered placement in the terminal viewport. */
};
/** @returns Key for the absolute image path or explicit empty override. */
ELDER_TERMS_API SettingKey terminal_background_image_setting_key();
/** @returns Key for image opacity. */
ELDER_TERMS_API SettingKey terminal_background_image_opacity_setting_key();
/** @returns Key for image placement. */
ELDER_TERMS_API SettingKey terminal_background_image_fit_setting_key();
/** @param store Layered settings. @returns Effective image preferences. */
ELDER_TERMS_API TerminalBackgroundImageSettings terminal_background_image_settings(const SettingsStore &store);

/** User-selected cursor geometry, independent of the terminal's DECSCUSR state. */
enum class TerminalCursorShape {
  block,     /**< Full-cell box. */
  bar,       /**< Vertical insertion box. */
  underline, /**< Horizontal box along the cell's lower edge. */
};

/** Cursor presentation preferences; focused cursors are filled, unfocused ones outlined. */
struct TerminalCursorSettings {
  TerminalCursorShape shape; /**< Geometry used for both focus states. */
  bool blink;                /**< Blink while focused; unfocused outlines remain visible. */
};

/** @returns Key for the terminal cursor geometry (block, bar or underline). */
ELDER_TERMS_API SettingKey terminal_cursor_shape_setting_key();
/** @returns Key for the independent cursor blink preference. */
ELDER_TERMS_API SettingKey terminal_cursor_blink_setting_key();
/** @param store Layered settings. @returns Effective cursor preferences. */
ELDER_TERMS_API TerminalCursorSettings terminal_cursor_settings(const SettingsStore &store);

/**
 * Terminal grid, history and character size preferences.
 */
struct TerminalDisplaySettings {
  /** Initial terminal columns. */
  glong width;
  /** Initial terminal rows. */
  glong height;
  /** Maximum normal-screen scrollback history in lines. */
  glong scrollback_lines;
  /** Initial character size in 1/96-inch device-independent pixels. */
  gdouble font_size_dip;
};

/**
 * Ordered terminal font families.
 *
 * Later families supply glyphs missing from earlier candidates. Names contain
 * no size or style information.
 */
struct TerminalFontFamilies {
  /** Font family names in descending priority; an empty vector selects the renderer
   * default monospace family. */
  std::vector<std::string> families;
};

/**
 * Terminal BEL playback settings.
 */
struct TerminalBellSettings {
  /** Custom sound file, or no value to retain terminal's built-in audible bell. */
  std::optional<std::filesystem::path> sound_file;
};

/**
 * Keyboard bindings for terminal display actions.
 */
struct TerminalKeyBindings {
  /** Zoom-in binding, or no value when the action is disabled. */
  std::optional<KeyBinding> zoom_in;
  /** Zoom-out binding, or no value when the action is disabled. */
  std::optional<KeyBinding> zoom_out;
  /** BREAK binding, or no value when the action is disabled. */
  std::optional<KeyBinding> send_break;
};

/**
 * Builds the default terminal display settings.
 *
 * @param default_font_size_dip Default character size in device-independent pixels.
 * @returns Terminal display settings matching the current built-in behavior.
 */
ELDER_TERMS_API TerminalDisplaySettings
default_terminal_display_settings(gdouble default_font_size_dip);

/**
 * Returns the setting key for [terminal] width.
 *
 * @returns Setting key for terminal columns.
 */
ELDER_TERMS_API SettingKey terminal_width_setting_key();

/**
 * Returns the setting key for [terminal] height.
 *
 * @returns Setting key for terminal rows.
 */
ELDER_TERMS_API SettingKey terminal_height_setting_key();

/**
 * Returns the setting key for [terminal] scrollback_lines.
 *
 * @returns Setting key for the terminal scrollback buffer size.
 */
ELDER_TERMS_API SettingKey terminal_scrollback_lines_setting_key();

/**
 * Returns the setting key for [terminal] block_copy_trim_trailing_spaces.
 *
 * @returns Setting key for removing physical-row trailing spaces from
 * rectangular copies.
 */
ELDER_TERMS_API SettingKey terminal_block_copy_trim_trailing_spaces_setting_key();

/**
 * Returns the setting key for [terminal] font_size_dip.
 *
 * @returns Setting key for terminal character size in DIP.
 */
ELDER_TERMS_API SettingKey terminal_font_size_dip_setting_key();

/**
 * Returns the setting key for [terminal] font_families.
 *
 * @returns Setting key for the ordered terminal font family list.
 */
ELDER_TERMS_API SettingKey terminal_font_families_setting_key();

/**
 * Validates ordered terminal font family names without requiring installed fonts.
 * @param families Candidate list; an empty list selects the built-in defaults.
 * @param reason Receives an explanation when the list is invalid.
 * @returns True for unique, non-empty UTF-8 names without commas or controls.
 * @remarks Surrounding whitespace is ignored when detecting empty or duplicate
 * names. Control characters are rejected even at the edges of a name.
 */
ELDER_TERMS_API bool terminal_font_families_are_valid(
    const std::vector<std::string> &families, std::string *reason);

/**
 * Returns the setting key for [terminal] accessibility_enabled.
 * @returns Setting key controlling screen-reader access to terminal text.
 */
ELDER_TERMS_API SettingKey terminal_accessibility_enabled_setting_key();

/**
 * Returns the setting key for [terminal] bell_sound.
 *
 * @returns Setting key for terminal BEL playback.
 */
ELDER_TERMS_API SettingKey terminal_bell_sound_setting_key();

/**
 * Returns the setting key for [terminal] zoom_in_key.
 *
 * @returns Setting key for the terminal zoom-in binding.
 */
ELDER_TERMS_API SettingKey terminal_zoom_in_key_setting_key();

/**
 * Returns the setting key for [terminal] zoom_out_key.
 *
 * @returns Setting key for the terminal zoom-out binding.
 */
ELDER_TERMS_API SettingKey terminal_zoom_out_key_setting_key();

/**
 * Returns the setting key for [terminal] send_break_key.
 *
 * @returns Setting key for the terminal BREAK binding.
 */
ELDER_TERMS_API SettingKey terminal_send_break_key_setting_key();

/**
 * Returns the setting key for [terminal] encoding.
 *
 * @returns Setting key for the terminal wire character encoding.
 */
ELDER_TERMS_API SettingKey terminal_encoding_setting_key();

/**
 * Returns the setting key for [terminal] backspace_code.
 *
 * @returns Setting key for the Backspace send code.
 */
ELDER_TERMS_API SettingKey terminal_backspace_code_setting_key();

/**
 * Returns the setting key for [terminal] cursor_key_mode.
 *
 * @returns Setting key for cursor-key sequence handling.
 */
ELDER_TERMS_API SettingKey terminal_cursor_key_mode_setting_key();

/**
 * Returns the setting key for [terminal] return_code.
 *
 * @returns Setting key for the Enter/Return send code.
 */
ELDER_TERMS_API SettingKey terminal_return_code_setting_key();

/**
 * Checks whether iconv can convert between one encoding and UTF-8 in both
 * directions.
 *
 * @param encoding Candidate iconv encoding name. Surrounding whitespace is
 * ignored.
 * @param reason Receives a human-readable validation failure reason.
 * @returns True when both conversion directions can be opened.
 */
ELDER_TERMS_API bool terminal_encoding_name_is_valid(
    const std::string &encoding, std::string *reason);

/**
 * Returns supported representative terminal encoding choices.
 *
 * @returns Process-local choices filtered through bidirectional iconv checks.
 */
ELDER_TERMS_API std::vector<std::string> terminal_encoding_choices();

/**
 * Validates a terminal BEL sound setting.
 *
 * @param value `default` or an absolute path to an existing .oga, .ogg, or
 * .wav regular file.
 * @param reason Receives a human-readable validation failure reason.
 * @returns True when the built-in beep or a supported custom file is selected.
 */
ELDER_TERMS_API bool terminal_bell_sound_is_valid(
    const std::string &value, std::string *reason);

/**
 * Returns terminal setting definitions.
 *
 * @param terminal_defaults Default terminal display settings.
 * @returns Setting definitions for the terminal INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
terminal_setting_definitions(TerminalDisplaySettings terminal_defaults);

/**
 * Extracts terminal display settings from a store.
 *
 * @param store Source settings store.
 * @returns Typed terminal display settings.
 */
ELDER_TERMS_API TerminalDisplaySettings
terminal_display_settings(const SettingsStore &store);

/**
 * Extracts the rectangular-copy trailing-space preference from a store.
 *
 * @param store Source settings store.
 * @returns True when a rectangular copy removes only the selected portion of
 * a physical row's trailing ASCII spaces; true by default.
 */
ELDER_TERMS_API bool
terminal_block_copy_trim_trailing_spaces(const SettingsStore &store);

/**
 * Extracts the resolved ordered terminal font families from a store.
 *
 * @param store Source settings store.
 * @returns Ordered families without a font size. An absent or explicitly empty
 * list resolves to Noto Sans Mono followed by Monospace.
 */
ELDER_TERMS_API TerminalFontFamilies
terminal_font_families(const SettingsStore &store);

/**
 * Extracts the terminal text accessibility preference.
 * @param store Source settings store.
 * @returns True when terminal text should be exposed to assistive technology;
 * false by default. Standard GTK controls are unaffected.
 */
ELDER_TERMS_API bool terminal_accessibility_enabled(const SettingsStore &store);

/**
 * Extracts terminal BEL playback settings from a store.
 *
 * @param store Source settings store containing validated values.
 * @returns Built-in or custom terminal BEL playback settings.
 */
ELDER_TERMS_API TerminalBellSettings
terminal_bell_settings(const SettingsStore &store);

/**
 * Extracts the raw terminal zoom-in binding text from a store.
 *
 * @param store Source settings store.
 * @returns Configured binding text, or the built-in default.
 */
ELDER_TERMS_API std::string
terminal_zoom_in_key(const SettingsStore &store);

/**
 * Extracts the raw terminal zoom-out binding text from a store.
 *
 * @param store Source settings store.
 * @returns Configured binding text, or the built-in default.
 */
ELDER_TERMS_API std::string
terminal_zoom_out_key(const SettingsStore &store);

/**
 * Extracts the raw terminal BREAK binding text from a store.
 *
 * @param store Source settings store.
 * @returns Configured binding text, or an empty string when disabled.
 */
ELDER_TERMS_API std::string
terminal_send_break_key(const SettingsStore &store);

/**
 * Extracts parsed terminal keyboard bindings from a store.
 *
 * @param store Source settings store containing validated values.
 * @returns Parsed zoom-in and zoom-out bindings.
 */
ELDER_TERMS_API TerminalKeyBindings
terminal_key_bindings(const SettingsStore &store);

/**
 * Checks whether terminal actions share an enabled binding.
 *
 * @param store Source settings store containing individually valid values.
 * @returns True when any two enabled bindings conflict.
 */
ELDER_TERMS_API bool
terminal_key_bindings_conflict(const SettingsStore &store);

} // namespace elder_terms
