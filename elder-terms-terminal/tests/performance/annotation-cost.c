/* elder-terms: A terminal application, inspired by personal computing in the 1990s.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * Headless isolation of the UI work performed for retained text annotations.
 * No recognizer, GTK, renderer, transport, or application scheduler is involved. */
#define _POSIX_C_SOURCE 200809L
#include <boxrig/terminal.h>
#include <boxrig/ui/view.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void require(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}

static size_t number(const char *text, size_t maximum)
{
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    require(*text != '\0' && *end == '\0' && value <= maximum, "Invalid number");
    return (size_t)value;
}

static uint64_t now_ns(clockid_t clock)
{
    struct timespec value;
    require(clock_gettime(clock, &value) == 0, "Read clock");
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t used = 0, length = strlen(text);
    require(boxrig_terminal_feed(terminal, (const uint8_t *)text, length, &used) ==
        BOXRIG_STATUS_OK && used == length, "Accept complete input");
}

int main(int argc, char **argv)
{
    require(argc == 4, "usage: annotation-cost HISTORY ANNOTATIONS ITERATIONS");
    const size_t history = number(argv[1], 10000);
    const size_t annotations = number(argv[2], 1000);
    const size_t iterations = number(argv[3], 10000);
    require(annotations <= history + 1 && iterations > 0, "Invalid annotation or iteration count");
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 45;
    config.columns = 193;
    config.scrollback_capacity = 10000;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    require(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK, "Create terminal");
    for (size_t index = 0; index < history + config.rows - 1; index++)
        feed(terminal, "https://example.org/retained plain ASCII terminal line\r\n");
    feed(terminal, "input ready");
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    require(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK, "Create model");
    require(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK, "Create view");
    require(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK, "Initial synchronize");
    for (size_t index = 0; index < annotations; index++) {
        /* Keep one visible range and spread the remainder through history.
         * Registration is setup work, excluded from the per-input measurements. */
        const size_t fragment = annotations == 1 ? history : index * history / (annotations - 1);
        boxrig_ui_text_annotation item = {0};
        item.source_id = 1;
        item.item_id = index + 1;
        require(boxrig_terminal_get_text_position(terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
            fragment, 0, &item.range.start) == BOXRIG_STATUS_OK, "Resolve range start");
        require(boxrig_terminal_get_text_position(terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
            fragment, 24, &item.range.end) == BOXRIG_STATUS_OK, "Resolve range end");
        item.style.field_mask = BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE;
        item.style.underline_style = BOXRIG_UNDERLINE_SINGLE;
        require(boxrig_ui_view_replace_text_annotations(view, 1, BOXRIG_TEXT_SCREEN_PRIMARY,
            item.range.start.line_id, item.range.start.line_revision, &item, 1) == BOXRIG_UI_OK,
            "Retain complete annotation");
    }
    puts("iteration\thistory\tannotations\tvisible_ranges\ttext_revision\tfeed_ns\tsync_ns\tsync_cpu_ns\tquery_ns\tquery_cpu_ns");
    for (size_t index = 0; index < iterations; index++) {
        char input[64];
        snprintf(input, sizeof(input), "\033[45;1Hinput %08zu", index);
        const uint64_t revision = boxrig_terminal_text_revision(terminal);
        uint64_t started = now_ns(CLOCK_MONOTONIC);
        feed(terminal, input);
        const uint64_t feed_ns = now_ns(CLOCK_MONOTONIC) - started;
        require(boxrig_terminal_text_revision(terminal) > revision, "Input changes terminal text");
        started = now_ns(CLOCK_MONOTONIC);
        uint64_t cpu = now_ns(CLOCK_THREAD_CPUTIME_ID);
        require(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK, "Synchronize input");
        const uint64_t sync_cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu;
        const uint64_t sync_ns = now_ns(CLOCK_MONOTONIC) - started;
        started = now_ns(CLOCK_MONOTONIC);
        cpu = now_ns(CLOCK_THREAD_CPUTIME_ID);
        size_t visible = 0;
        for (size_t row = 0; row < config.rows; row++) {
            boxrig_ui_text_annotation_visual_range ranges[1];
            size_t count = 0;
            require(boxrig_ui_view_query_text_annotation_visual_ranges(view, row, ranges,
                1, &count) == BOXRIG_UI_OK, "Query complete visible ranges");
            visible += count;
        }
        const uint64_t query_cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu;
        const uint64_t query_ns = now_ns(CLOCK_MONOTONIC) - started;
        printf("%zu\t%zu\t%zu\t%zu\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n", index,
            boxrig_terminal_scrollback_rows(terminal), annotations, visible,
            boxrig_terminal_text_revision(terminal), feed_ns, sync_ns, sync_cpu, query_ns, query_cpu);
    }
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return 0;
}
