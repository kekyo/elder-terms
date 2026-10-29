/* elder-terms: A terminal application, inspired by personal computing in the 1990s.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT. */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_KEYS 1000u
#define MAX_FRAMES 30000u

typedef struct key_record {
    uint64_t sent_ns;
    uint64_t visible_ns;
} key_record;

typedef struct capture_state {
    Display *display;
    Window window;
    Window root;
    const char *output_dir;
    FILE *frames;
    FILE *concat;
    bool video;
    bool full_window;
    bool capture_root;
    int marker_x;
    int marker_y;
    unsigned int marker_width;
    unsigned int marker_height;
    unsigned int window_width;
    unsigned int window_height;
    uint64_t previous_end_ns;
    unsigned int frame_count;
} capture_state;

static uint64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static bool parse_uint(const char *text, unsigned long minimum, unsigned long maximum,
    unsigned long *result)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum) return false;
    *result = value;
    return true;
}

static bool send_key(Display *display, KeySym symbol)
{
    KeyCode code = XKeysymToKeycode(display, symbol);
    if (code == 0 || !XTestFakeKeyEvent(display, code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, code, False, CurrentTime)) return false;
    XFlush(display);
    return true;
}

static unsigned long marker_color(unsigned int sequence)
{
    unsigned int red = sequence == 0 ? 32u : 64u + (sequence % 160u);
    unsigned int green = sequence == 0 ? 64u : 80u + ((sequence / 160u) % 160u);
    unsigned int blue = sequence == 0 ? 96u : 80u;
    return ((unsigned long)red << 16) | ((unsigned long)green << 8) | blue;
}

static bool image_colors_are_direct(const XImage *image)
{
    return image->red_mask == 0xff0000ul && image->green_mask == 0xff00ul &&
        image->blue_mask == 0xfful;
}

static bool find_marker(capture_state *state)
{
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(state->display, state->window, &attributes) ||
        attributes.width <= 0 || attributes.height <= 0) return false;
    state->window_width = (unsigned int)attributes.width;
    state->window_height = (unsigned int)attributes.height;
    uint64_t deadline = now_ns() + UINT64_C(10000000000);
    while (now_ns() < deadline && state->marker_width == 0) {
        XImage *image = XGetImage(state->display, state->window, 0, 0,
            (unsigned int)attributes.width, (unsigned int)attributes.height,
            AllPlanes, ZPixmap);
        if (image == NULL || !image_colors_are_direct(image)) {
            if (image != NULL) XDestroyImage(image);
            return false;
        }
        for (int y = 0; y < image->height && state->marker_width == 0; y++) {
            for (int x = 0; x < image->width && state->marker_width == 0; x++) {
                if ((XGetPixel(image, x, y) & 0xfffffful) != marker_color(0)) continue;
                unsigned int width = 0;
                unsigned int height = 0;
                while (x + (int)width < image->width &&
                    (XGetPixel(image, x + (int)width, y) & 0xfffffful) == marker_color(0)) width++;
                while (y + (int)height < image->height &&
                    (XGetPixel(image, x, y + (int)height) & 0xfffffful) == marker_color(0)) height++;
                if (width >= 32 && height >= 8 &&
                    x + (int)width <= image->width && y + (int)height <= image->height) {
                    state->marker_x = x;
                    state->marker_y = y;
                    state->marker_width = width;
                    state->marker_height = height;
                }
            }
        }
        XDestroyImage(image);
        if (state->marker_width == 0) {
            struct timespec pause = {0, 10000000};
            (void)nanosleep(&pause, NULL);
        }
    }
    return state->marker_width != 0;
}

static bool write_ppm(const char *path, const XImage *image)
{
    FILE *output = fopen(path, "wb");
    if (output == NULL) return false;
    if (fprintf(output, "P6\n%d %d\n255\n", image->width, image->height) <= 0) {
        (void)fclose(output);
        return false;
    }
    for (int y = 0; y < image->height; y++) {
        for (int x = 0; x < image->width; x++) {
            unsigned long pixel = XGetPixel((XImage *)image, x, y);
            unsigned char rgb[3] = {
                (unsigned char)(pixel >> 16),
                (unsigned char)(pixel >> 8),
                (unsigned char)pixel
            };
            if (fwrite(rgb, 1, sizeof(rgb), output) != sizeof(rgb)) {
                (void)fclose(output);
                return false;
            }
        }
    }
    return fclose(output) == 0;
}

static bool sample(capture_state *state, unsigned long *color_out, uint64_t *end_out)
{
    if (state->frame_count == MAX_FRAMES) return false;
    int x = state->full_window ? 0 : state->marker_x;
    int y = state->full_window ? 0 : state->marker_y;
    unsigned int width = state->full_window ? state->window_width : state->marker_width;
    unsigned int height = state->full_window ? state->window_height : state->marker_height;
    int sample_x = state->full_window ?
        state->marker_x + (int)state->marker_width / 2 : (int)state->marker_width / 2;
    int sample_y = state->full_window ?
        state->marker_y + (int)state->marker_height / 2 : (int)state->marker_height / 2;
    Window source = state->window;
    if (state->capture_root) {
        Window child = None;
        if (!XTranslateCoordinates(state->display, state->window, state->root,
            x, y, &x, &y, &child)) return false;
        source = state->root;
    }
    uint64_t start = now_ns();
    XImage *image = XGetImage(state->display, source, x, y,
        width, height, AllPlanes, ZPixmap);
    uint64_t end = now_ns();
    if (image == NULL || !image_colors_are_direct(image)) {
        if (image != NULL) XDestroyImage(image);
        return false;
    }
    unsigned long color = XGetPixel(image, sample_x, sample_y) & 0xfffffful;
    if (fprintf(state->frames, "%u\t%" PRIu64 "\t%" PRIu64
        "\t%06lx\t%d\t%d\t%d\t%d\n",
        state->frame_count, start, end, color, x, y, sample_x, sample_y) < 0) {
        XDestroyImage(image);
        return false;
    }
    if (state->video) {
        char path[4096];
        if (snprintf(path, sizeof(path), "%s/frame-%05u.ppm",
            state->output_dir, state->frame_count) >= (int)sizeof(path) ||
            !write_ppm(path, image)) {
            XDestroyImage(image);
            return false;
        }
        if (state->frame_count != 0) {
            double duration = (double)(end - state->previous_end_ns) / 1e9;
            if (fprintf(state->concat, "duration %.9f\n", duration) < 0) {
                XDestroyImage(image);
                return false;
            }
        }
        if (fprintf(state->concat, "file frame-%05u.ppm\noption framerate 1000\n",
            state->frame_count) < 0) {
            XDestroyImage(image);
            return false;
        }
    }
    XDestroyImage(image);
    state->previous_end_ns = end;
    state->frame_count++;
    *color_out = color;
    *end_out = end;
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: %s WINDOW OUTPUT_DIR COUNT STOP_AFTER "
            "video|full|none window|root\n", argv[0]);
        return 2;
    }
    unsigned long window_number = 0;
    unsigned long count = 0;
    unsigned long stop_after = 0;
    if (!parse_uint(argv[1], 1, UINT64_MAX, &window_number) ||
        !parse_uint(argv[3], 1, MAX_KEYS, &count) ||
        !parse_uint(argv[4], 0, MAX_KEYS, &stop_after) || stop_after > count ||
        (strcmp(argv[5], "video") != 0 && strcmp(argv[5], "full") != 0 &&
            strcmp(argv[5], "none") != 0) ||
        (strcmp(argv[6], "window") != 0 && strcmp(argv[6], "root") != 0)) return 2;

    capture_state state = {.window = (Window)window_number, .output_dir = argv[2],
        .video = strcmp(argv[5], "none") != 0,
        .full_window = strcmp(argv[5], "full") == 0,
        .capture_root = strcmp(argv[6], "root") == 0};
    state.display = XOpenDisplay(NULL);
    if (state.display == NULL) return 1;
    state.root = DefaultRootWindow(state.display);
    int event = 0, error = 0, major = 0, minor = 0;
    if (!XTestQueryExtension(state.display, &event, &error, &major, &minor)) return 1;
    XSetInputFocus(state.display, state.window, RevertToParent, CurrentTime);
    XSync(state.display, False);
    if (!find_marker(&state)) {
        fprintf(stderr, "Could not find ready color marker\n");
        return 1;
    }
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/frames.tsv", argv[2]) >= (int)sizeof(path)) return 1;
    state.frames = fopen(path, "w");
    if (state.frames == NULL) return 1;
    fputs("frame\tstart_ns\tend_ns\tcolor\tx\ty\tsample_x\tsample_y\n", state.frames);
    if (state.video) {
        if (snprintf(path, sizeof(path), "%s/video.ffconcat", argv[2]) >= (int)sizeof(path)) return 1;
        state.concat = fopen(path, "w");
        if (state.concat == NULL) return 1;
        fputs("ffconcat version 1.0\n", state.concat);
    }
    key_record records[MAX_KEYS] = {{0}};
    unsigned long color = 0;
    uint64_t end = 0;
    if (!sample(&state, &color, &end) || color != marker_color(0)) return 1;
    if (stop_after > 0 && !send_key(state.display, XK_g)) return 1;
    for (unsigned int index = 0; index < count; index++) {
        if (index == stop_after && !send_key(state.display, XK_s)) return 1;
        records[index].sent_ns = now_ns();
        if (!send_key(state.display, XK_k)) return 1;
        uint64_t deadline = records[index].sent_ns + UINT64_C(10000000000);
        bool seen = false;
        while (now_ns() < deadline) {
            if (!sample(&state, &color, &end)) return 1;
            if (color == marker_color(index + 1u)) {
                records[index].visible_ns = end;
                seen = true;
                break;
            }
            struct timespec pause = {0, 16000000};
            (void)nanosleep(&pause, NULL);
        }
        if (!seen) {
            fprintf(stderr, "Timed out waiting for key %u marker; last color=%06lx\n",
                index + 1u, color);
            return 1;
        }
        struct timespec spacing = {0, 100000000};
        (void)nanosleep(&spacing, NULL);
    }
    if (stop_after == count && !send_key(state.display, XK_s)) return 1;
    if (!send_key(state.display, XK_q)) return 1;
    if (state.video && fputs("duration 0.016666667\n", state.concat) == EOF) return 1;
    if (state.concat != NULL && fclose(state.concat) != 0) return 1;
    if (fclose(state.frames) != 0) return 1;
    if (snprintf(path, sizeof(path), "%s/events.tsv", argv[2]) >= (int)sizeof(path)) return 1;
    FILE *events = fopen(path, "w");
    if (events == NULL) return 1;
    fputs("key\tsent_ns\tvisible_ns\n", events);
    for (unsigned int index = 0; index < count; index++) {
        fprintf(events, "%u\t%" PRIu64 "\t%" PRIu64 "\n", index + 1u,
            records[index].sent_ns, records[index].visible_ns);
    }
    if (fclose(events) != 0) return 1;
    XCloseDisplay(state.display);
    return 0;
}
