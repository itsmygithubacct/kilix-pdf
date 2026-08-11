/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kilix_pdf_layout.h"
#include "kitty_terminal_session.h"

#include <cairo.h>
#include <errno.h>
#include <glib.h>
#include <math.h>
#include <poll.h>
#include <poppler.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#define KILIX_PDF_VERSION "0.1.0"

enum {
    MAX_PAGE_DIMENSION = 16384,
    MAX_PAGE_BYTES = 256 * 1024 * 1024,
    MAX_FRAME_BYTES = 256 * 1024 * 1024,
    MIN_TOOLBAR_HEIGHT = 40,
    MAX_TOOLBAR_HEIGHT = 56
};

typedef struct pdf_viewer {
    PopplerDocument *document;
    PopplerPage *page;
    cairo_surface_t *page_surface;
    cairo_surface_t *frame_surface;
    uint8_t *frame_rgba;
    size_t frame_rgba_capacity;
    kittyts_session terminal;
    kilix_pdf_layout layout;
    int page_index;
    int page_count;
    bool terminal_started;
} pdf_viewer;

static volatile sig_atomic_t stop_requested = 0;
static kittyts_session *signal_terminal = NULL;

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
    if (signal_terminal != NULL)
        kittyts_emergency_restore(signal_terminal);
}

static bool install_signal_handlers(void)
{
    struct sigaction action;

    (void)memset(&action, 0, sizeof action);
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0) return false;
    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0 &&
           sigaction(SIGHUP, &action, NULL) == 0;
}

static int poppler_error_status(const GError *error)
{
    if (error != NULL && error->domain == POPPLER_ERROR &&
        error->code == POPPLER_ERROR_ENCRYPTED) {
        return EX_UNAVAILABLE;
    }
    if (error != NULL && error->domain == G_FILE_ERROR &&
        (error->code == G_FILE_ERROR_NOENT ||
         error->code == G_FILE_ERROR_NOTDIR)) {
        return EX_NOINPUT;
    }
    return EX_DATAERR;
}

static PopplerDocument *open_document(const char *path, int *status)
{
    PopplerDocument *document;
    GError *error = NULL;
    gchar *absolute;
    gchar *uri;

    absolute = g_canonicalize_filename(path, NULL);
    uri = g_filename_to_uri(absolute, NULL, &error);
    g_free(absolute);
    if (uri == NULL) {
        (void)fprintf(stderr, "kilix-pdf: %s\n",
                      error != NULL ? error->message : "invalid PDF path");
        *status = poppler_error_status(error);
        g_clear_error(&error);
        return NULL;
    }
    document = poppler_document_new_from_file(uri, NULL, &error);
    g_free(uri);
    if (document == NULL) {
        const bool encrypted = error != NULL &&
                               error->domain == POPPLER_ERROR &&
                               error->code == POPPLER_ERROR_ENCRYPTED;

        (void)fprintf(stderr, "kilix-pdf: %s%s\n",
                      error != NULL ? error->message : "cannot open PDF",
                      encrypted ? "; opening Evince for password entry" : "");
        *status = poppler_error_status(error);
        g_clear_error(&error);
        return NULL;
    }
    *status = EX_OK;
    return document;
}

static bool page_size_allowed(int width, int height)
{
    size_t pixels;

    if (width <= 0 || height <= 0 || width > MAX_PAGE_DIMENSION ||
        height > MAX_PAGE_DIMENSION) {
        return false;
    }
    pixels = (size_t)width * (size_t)height;
    return pixels <= (size_t)MAX_PAGE_BYTES / 4u;
}

static cairo_surface_t *render_page_surface(PopplerPage *page, double scale,
                                            int *width_out, int *height_out)
{
    cairo_surface_t *surface;
    cairo_t *context;
    cairo_status_t status;
    double width_points;
    double height_points;
    double scaled_width;
    double scaled_height;
    int width;
    int height;

    poppler_page_get_size(page, &width_points, &height_points);
    scaled_width = ceil(width_points * scale);
    scaled_height = ceil(height_points * scale);
    if (!isfinite(scaled_width) || !isfinite(scaled_height) ||
        scaled_width > (double)MAX_PAGE_DIMENSION ||
        scaled_height > (double)MAX_PAGE_DIMENSION ||
        scaled_width < 1.0 || scaled_height < 1.0) {
        return NULL;
    }
    width = (int)scaled_width;
    height = (int)scaled_height;
    if (!page_size_allowed(width, height)) return NULL;

    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }
    context = cairo_create(surface);
    cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
    cairo_paint(context);
    cairo_scale(context, scale, scale);
    poppler_page_render(page, context);
    status = cairo_status(context);
    cairo_destroy(context);
    cairo_surface_flush(surface);
    if (status != CAIRO_STATUS_SUCCESS ||
        cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }
    *width_out = width;
    *height_out = height;
    return surface;
}

static bool rasterize_current_page(pdf_viewer *viewer)
{
    cairo_surface_t *surface;
    int width;
    int height;

    surface = render_page_surface(viewer->page,
                                  kilix_pdf_layout_scale(&viewer->layout),
                                  &width, &height);
    if (surface == NULL) {
        (void)fprintf(stderr,
                      "kilix-pdf: page raster exceeds the safe size limit or failed\n");
        return false;
    }
    if (width != kilix_pdf_layout_page_width(&viewer->layout) ||
        height != kilix_pdf_layout_page_height(&viewer->layout)) {
        cairo_surface_destroy(surface);
        (void)fputs("kilix-pdf: inconsistent page geometry\n", stderr);
        return false;
    }
    if (viewer->page_surface != NULL)
        cairo_surface_destroy(viewer->page_surface);
    viewer->page_surface = surface;
    return true;
}

static bool select_page(pdf_viewer *viewer, int index)
{
    PopplerPage *page;
    double width_points;
    double height_points;

    if (index < 0 || index >= viewer->page_count) return true;
    page = poppler_document_get_page(viewer->document, index);
    if (page == NULL) {
        (void)fprintf(stderr, "kilix-pdf: cannot load page %d\n", index + 1);
        return false;
    }
    poppler_page_get_size(page, &width_points, &height_points);
    if (!kilix_pdf_layout_set_page(&viewer->layout,
                                   width_points, height_points)) {
        g_object_unref(page);
        (void)fputs("kilix-pdf: invalid page geometry\n", stderr);
        return false;
    }
    if (viewer->page != NULL) g_object_unref(viewer->page);
    viewer->page = page;
    viewer->page_index = index;
    return rasterize_current_page(viewer);
}

static int toolbar_height_for(const kittyts_session *terminal, int height)
{
    int toolbar = kittyts_cell_height(terminal) * 2 + 8;

    if (toolbar < MIN_TOOLBAR_HEIGHT) toolbar = MIN_TOOLBAR_HEIGHT;
    if (toolbar > MAX_TOOLBAR_HEIGHT) toolbar = MAX_TOOLBAR_HEIGHT;
    if (toolbar >= height) toolbar = height > 1 ? height / 3 : 0;
    return toolbar;
}

static bool ensure_frame_surface(pdf_viewer *viewer, int width, int height)
{
    cairo_surface_t *surface;
    uint8_t *rgba;
    size_t bytes;

    if (width <= 0 || height <= 0 ||
        width > MAX_PAGE_DIMENSION || height > MAX_PAGE_DIMENSION ||
        (size_t)width > SIZE_MAX / 4u / (size_t)height ||
        (size_t)width * (size_t)height >
            (size_t)MAX_FRAME_BYTES / 4u) {
        return false;
    }
    bytes = (size_t)width * (size_t)height * 4u;
    rgba = realloc(viewer->frame_rgba, bytes);
    if (rgba == NULL) return false;
    viewer->frame_rgba = rgba;
    viewer->frame_rgba_capacity = bytes;
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }
    if (viewer->frame_surface != NULL)
        cairo_surface_destroy(viewer->frame_surface);
    viewer->frame_surface = surface;
    return true;
}

static const char *fit_mode_name(kilix_pdf_fit_mode mode)
{
    if (mode == KILIX_PDF_FIT_PAGE) return "fit page";
    if (mode == KILIX_PDF_FIT_WIDTH) return "fit width";
    return "custom";
}

static bool convert_frame_to_rgba(pdf_viewer *viewer)
{
    unsigned char *argb;
    int stride;
    int width = viewer->layout.frame_width;
    int height = viewer->layout.frame_height;
    size_t required;

    if (width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 4u / (size_t)height) {
        return false;
    }
    required = (size_t)width * (size_t)height * 4u;
    if (viewer->frame_surface == NULL ||
        viewer->frame_rgba_capacity < required) {
        return false;
    }
    cairo_surface_flush(viewer->frame_surface);
    argb = cairo_image_surface_get_data(viewer->frame_surface);
    stride = cairo_image_surface_get_stride(viewer->frame_surface);
    for (int y = 0; y < height; ++y) {
        const unsigned char *row = argb + (size_t)y * (size_t)stride;

        for (int x = 0; x < width; ++x) {
            uint32_t pixel;
            size_t output = ((size_t)y * (size_t)width + (size_t)x) * 4u;

            (void)memcpy(&pixel, row + (size_t)x * 4u, sizeof pixel);
            viewer->frame_rgba[output] = (uint8_t)(pixel >> 16);
            viewer->frame_rgba[output + 1u] = (uint8_t)(pixel >> 8);
            viewer->frame_rgba[output + 2u] = (uint8_t)pixel;
            viewer->frame_rgba[output + 3u] = (uint8_t)(pixel >> 24);
        }
    }
    return true;
}

static bool compose_frame(pdf_viewer *viewer)
{
    cairo_t *context;
    cairo_status_t status;
    int width = viewer->layout.frame_width;
    int height = viewer->layout.frame_height;
    int page_width = kilix_pdf_layout_page_width(&viewer->layout);
    int page_x = (width - page_width) / 2;
    int page_y = viewer->layout.toolbar_height + viewer->layout.margin -
                 viewer->layout.scroll_y;
    char title[192];
    char controls[192];
    int title_length;
    double progress;

    if (viewer->frame_surface == NULL || viewer->page_surface == NULL)
        return false;
    context = cairo_create(viewer->frame_surface);
    cairo_set_source_rgb(context, 0.105, 0.115, 0.135);
    cairo_paint(context);

    cairo_save(context);
    cairo_rectangle(context, 0.0, (double)viewer->layout.toolbar_height,
                    (double)width,
                    (double)(height - viewer->layout.toolbar_height));
    cairo_clip(context);
    cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.35);
    cairo_rectangle(context, (double)(page_x + 5), (double)(page_y + 6),
                    (double)page_width,
                    (double)kilix_pdf_layout_page_height(&viewer->layout));
    cairo_fill(context);
    cairo_set_source_surface(context, viewer->page_surface,
                             (double)page_x, (double)page_y);
    cairo_paint(context);
    cairo_restore(context);

    cairo_set_source_rgb(context, 0.075, 0.085, 0.105);
    cairo_rectangle(context, 0.0, 0.0, (double)width,
                    (double)viewer->layout.toolbar_height);
    cairo_fill(context);
    cairo_set_source_rgb(context, 0.24, 0.49, 0.92);
    cairo_rectangle(context, 0.0,
                    (double)(viewer->layout.toolbar_height - 2),
                    (double)width, 2.0);
    cairo_fill(context);

    title_length = snprintf(
        title, sizeof title, "Kilix PDF | page %d/%d | %.0f%% | %s",
        viewer->page_index + 1, viewer->page_count,
        kilix_pdf_layout_scale(&viewer->layout) * 100.0,
        fit_mode_name(viewer->layout.mode));
    if (title_length < 0 || (size_t)title_length >= sizeof title) {
        cairo_destroy(context);
        return false;
    }
    (void)snprintf(controls, sizeof controls,
                   "j/k scroll | PgUp/PgDn page | +/- zoom | 0/w fit | q quit");
    cairo_select_font_face(context, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(context, 15.0);
    cairo_set_source_rgb(context, 0.94, 0.96, 1.0);
    cairo_move_to(context, 14.0, 19.0);
    cairo_show_text(context, title);
    cairo_select_font_face(context, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 11.0);
    cairo_set_source_rgb(context, 0.65, 0.70, 0.79);
    cairo_move_to(context, 14.0, 35.0);
    cairo_show_text(context, controls);

    progress = viewer->page_count > 1
                   ? (double)viewer->page_index /
                         (double)(viewer->page_count - 1)
                   : 1.0;
    cairo_set_source_rgb(context, 0.24, 0.49, 0.92);
    cairo_rectangle(context, 0.0,
                    (double)(viewer->layout.toolbar_height - 4),
                    (double)width * progress, 2.0);
    cairo_fill(context);
    status = cairo_status(context);
    cairo_destroy(context);
    cairo_surface_flush(viewer->frame_surface);
    if (status != CAIRO_STATUS_SUCCESS ||
        cairo_surface_status(viewer->frame_surface) != CAIRO_STATUS_SUCCESS) {
        return false;
    }

    return convert_frame_to_rgba(viewer);
}

static bool present_full(pdf_viewer *viewer)
{
    return compose_frame(viewer) &&
           kittyts_present(&viewer->terminal, viewer->frame_rgba,
                           viewer->layout.frame_width,
                           viewer->layout.frame_height);
}

static bool present_scroll(pdf_viewer *viewer, int scroll_delta)
{
    const int terminal_dy = -scroll_delta;
    const kittyfb_rect viewport = {
        0,
        viewer->layout.toolbar_height,
        viewer->layout.frame_width,
        viewer->layout.frame_height
    };

    return compose_frame(viewer) &&
           kittyts_present_scroll_region(
               &viewer->terminal, viewer->frame_rgba,
               viewer->layout.frame_width, viewer->layout.frame_height,
               &viewport, 0, terminal_dy, NULL, 0u);
}

static int scroll_step(const pdf_viewer *viewer)
{
    int step = kittyts_cell_height(&viewer->terminal) * 3;

    if (step < 24) step = 24;
    if (step > viewer->layout.frame_height / 3)
        step = viewer->layout.frame_height / 3;
    return step > 0 ? step : 1;
}

static bool change_page(pdf_viewer *viewer, int index, bool *full_redraw)
{
    if (index < 0 || index >= viewer->page_count ||
        index == viewer->page_index) {
        return true;
    }
    if (!select_page(viewer, index)) return false;
    *full_redraw = true;
    return true;
}

static bool change_fit_mode(pdf_viewer *viewer, kilix_pdf_fit_mode mode,
                            bool *full_redraw)
{
    kilix_pdf_layout_set_mode(&viewer->layout, mode);
    if (!rasterize_current_page(viewer)) return false;
    *full_redraw = true;
    return true;
}

static bool change_zoom(pdf_viewer *viewer, double factor,
                        bool *full_redraw)
{
    const double before = kilix_pdf_layout_scale(&viewer->layout);

    if (!kilix_pdf_layout_zoom(&viewer->layout, factor)) return true;
    if (kilix_pdf_layout_scale(&viewer->layout) == before) return true;
    if (!rasterize_current_page(viewer)) return false;
    *full_redraw = true;
    return true;
}

static bool handle_key(pdf_viewer *viewer, const kittykb_event *event,
                       bool *running, bool *full_redraw)
{
    const uint32_t key = event->key;
    const int step = scroll_step(viewer);

    if (event->action == KITTYKB_ACTION_RELEASE) return true;
    if (key == (uint32_t)'q' || key == KITTYKB_KEY_ESCAPE) {
        *running = false;
        return true;
    }
    if (key == (uint32_t)'n' || key == (uint32_t)' ' ||
        key == KITTYKB_KEY_PAGE_DOWN || key == KITTYKB_KEY_RIGHT)
        return change_page(viewer, viewer->page_index + 1, full_redraw);
    if (key == (uint32_t)'p' || key == KITTYKB_KEY_PAGE_UP ||
        key == KITTYKB_KEY_LEFT)
        return change_page(viewer, viewer->page_index - 1, full_redraw);
    if (key == KITTYKB_KEY_HOME)
        return change_page(viewer, 0, full_redraw);
    if (key == KITTYKB_KEY_END)
        return change_page(viewer, viewer->page_count - 1, full_redraw);
    if (key == (uint32_t)'j' || key == KITTYKB_KEY_DOWN) {
        (void)kilix_pdf_layout_scroll(&viewer->layout, step);
        return true;
    }
    if (key == (uint32_t)'k' || key == KITTYKB_KEY_UP) {
        (void)kilix_pdf_layout_scroll(&viewer->layout, -step);
        return true;
    }
    if (key == (uint32_t)'+' || key == (uint32_t)'=')
        return change_zoom(viewer, 1.20, full_redraw);
    if (key == (uint32_t)'-' || key == (uint32_t)'_')
        return change_zoom(viewer, 1.0 / 1.20, full_redraw);
    if (key == (uint32_t)'0')
        return change_fit_mode(viewer, KILIX_PDF_FIT_PAGE, full_redraw);
    if (key == (uint32_t)'w')
        return change_fit_mode(viewer, KILIX_PDF_FIT_WIDTH, full_redraw);
    return true;
}

static bool handle_queued_events(pdf_viewer *viewer, bool *running,
                                 bool *full_redraw)
{
    kittyin_event event;

    while (kittyts_next_event(&viewer->terminal, &event)) {
        if (event.kind == KITTYIN_EVENT_KEY) {
            if (!handle_key(viewer, &event.data.key,
                            running, full_redraw)) {
                return false;
            }
        } else if (event.kind == KITTYIN_EVENT_MOUSE &&
                   event.data.mouse.action == KITTYIN_MOUSE_WHEEL) {
            (void)kilix_pdf_layout_scroll(
                &viewer->layout,
                (int)event.data.mouse.wheel_y * scroll_step(viewer));
        }
    }
    return true;
}

static bool handle_resize(pdf_viewer *viewer, bool *full_redraw)
{
    int width;
    int height;
    int toolbar;

    if (!kittyts_check_resize(&viewer->terminal, &width, &height))
        return true;
    toolbar = toolbar_height_for(&viewer->terminal, height);
    if (!kilix_pdf_layout_set_viewport(&viewer->layout,
                                       width, height, toolbar) ||
        !ensure_frame_surface(viewer, width, height)) {
        return false;
    }
    if (viewer->layout.mode != KILIX_PDF_CUSTOM_ZOOM &&
        !rasterize_current_page(viewer)) {
        return false;
    }
    *full_redraw = true;
    return true;
}

static void viewer_cleanup(pdf_viewer *viewer)
{
    signal_terminal = NULL;
    if (viewer->terminal_started) {
        kittyts_stop(&viewer->terminal);
        viewer->terminal_started = false;
    }
    if (viewer->frame_surface != NULL)
        cairo_surface_destroy(viewer->frame_surface);
    if (viewer->page_surface != NULL)
        cairo_surface_destroy(viewer->page_surface);
    if (viewer->page != NULL) g_object_unref(viewer->page);
    if (viewer->document != NULL) g_object_unref(viewer->document);
    free(viewer->frame_rgba);
}

static int run_interactive(const char *path)
{
    pdf_viewer viewer = {0};
    kittyts_options options;
    int document_status;
    int width;
    int height;
    int toolbar;
    bool running = true;
    int result = EX_OK;

    viewer.document = open_document(path, &document_status);
    if (viewer.document == NULL) return document_status;
    viewer.page_count = poppler_document_get_n_pages(viewer.document);
    if (viewer.page_count <= 0) {
        (void)fputs("kilix-pdf: document contains no pages\n", stderr);
        viewer_cleanup(&viewer);
        return EX_DATAERR;
    }

    kittyts_session_init(&viewer.terminal);
    kittyts_options_init(&options);
    options.mouse_tracking = KITTYIN_MOUSE_TRACKING_BUTTON;
    options.pixel_mouse = true;
    if (getenv("KILIX_PDF_TEST_TERMINAL") != NULL) {
        options.framebuffer.probe_graphics = false;
        options.framebuffer.install_winch_handler = false;
        options.keyboard.probe_timeout_ms = 0;
    }
    stop_requested = 0;
    signal_terminal = &viewer.terminal;
    if (!install_signal_handlers()) {
        (void)fprintf(stderr, "kilix-pdf: cannot install signal handlers: %s\n",
                      strerror(errno));
        viewer_cleanup(&viewer);
        return EX_OSERR;
    }
    if (kittyts_start(&viewer.terminal, STDIN_FILENO, STDOUT_FILENO,
                      &options) != 0) {
        (void)fprintf(stderr,
                      "kilix-pdf: native terminal graphics unavailable: %s\n",
                      strerror(errno));
        viewer_cleanup(&viewer);
        return EX_UNAVAILABLE;
    }
    viewer.terminal_started = true;

    width = kittyts_width(&viewer.terminal);
    height = kittyts_height(&viewer.terminal);
    toolbar = toolbar_height_for(&viewer.terminal, height);
    kilix_pdf_layout_init(&viewer.layout);
    if (!kilix_pdf_layout_set_viewport(&viewer.layout,
                                       width, height, toolbar) ||
        !ensure_frame_surface(&viewer, width, height) ||
        !select_page(&viewer, 0) || !present_full(&viewer)) {
        (void)fputs("kilix-pdf: initial render failed\n", stderr);
        viewer_cleanup(&viewer);
        return EX_SOFTWARE;
    }

    while (running && !stop_requested) {
        struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
        const int old_scroll = viewer.layout.scroll_y;
        bool full_redraw = false;
        int ready;

        do {
            ready = poll(&descriptor, 1u, 50);
        } while (ready < 0 && errno == EINTR && !stop_requested);
        if (ready < 0) {
            (void)fprintf(stderr, "kilix-pdf: input poll failed: %s\n",
                          strerror(errno));
            result = EX_IOERR;
            break;
        }
        if (ready > 0 && (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            result = EX_IOERR;
            break;
        }
        if (ready > 0 && (descriptor.revents & POLLIN) != 0 &&
            kittyts_read_input(&viewer.terminal) < 0) {
            (void)fprintf(stderr, "kilix-pdf: input read failed: %s\n",
                          strerror(errno));
            result = EX_IOERR;
            break;
        }
        if (!handle_queued_events(&viewer, &running, &full_redraw) ||
            !handle_resize(&viewer, &full_redraw)) {
            result = EX_SOFTWARE;
            break;
        }
        if (!running || stop_requested) break;
        if (full_redraw) {
            if (!present_full(&viewer)) {
                result = EX_IOERR;
                break;
            }
        } else if (viewer.layout.scroll_y != old_scroll &&
                   !present_scroll(&viewer,
                                   viewer.layout.scroll_y - old_scroll)) {
            result = EX_IOERR;
            break;
        }
        if (kittyts_failed(&viewer.terminal)) {
            result = EX_IOERR;
            break;
        }
        if (ready > 0 && (descriptor.revents & POLLHUP) != 0) break;
    }
    viewer_cleanup(&viewer);
    return result;
}

static int render_png(const char *path, const char *output)
{
    PopplerDocument *document;
    PopplerPage *page;
    cairo_surface_t *surface;
    cairo_status_t write_status;
    int document_status;
    int width;
    int height;

    document = open_document(path, &document_status);
    if (document == NULL) return document_status;
    page = poppler_document_get_page(document, 0);
    if (page == NULL) {
        g_object_unref(document);
        (void)fputs("kilix-pdf: cannot load first page\n", stderr);
        return EX_DATAERR;
    }
    surface = render_page_surface(page, 2.0, &width, &height);
    if (surface == NULL) {
        g_object_unref(page);
        g_object_unref(document);
        (void)fputs("kilix-pdf: headless raster failed\n", stderr);
        return EX_SOFTWARE;
    }
    write_status = cairo_surface_write_to_png(surface, output);
    cairo_surface_destroy(surface);
    g_object_unref(page);
    g_object_unref(document);
    if (write_status != CAIRO_STATUS_SUCCESS) {
        (void)fprintf(stderr, "kilix-pdf: cannot write %s: %s\n", output,
                      cairo_status_to_string(write_status));
        return EX_CANTCREAT;
    }
    (void)printf("rendered %dx%d CPU raster to %s\n", width, height, output);
    return EX_OK;
}

static int benchmark_render(const char *path)
{
    enum {
        FRAME_WIDTH = 1280,
        FRAME_HEIGHT = 720,
        TOOLBAR_HEIGHT = 48,
        COMPOSE_ITERATIONS = 40,
        CONVERT_ITERATIONS = 200
    };
    pdf_viewer viewer = {0};
    int document_status;
    uint64_t started;
    uint64_t raster_elapsed;
    uint64_t compose_elapsed;
    uint64_t convert_elapsed;
    const size_t frame_bytes =
        (size_t)FRAME_WIDTH * (size_t)FRAME_HEIGHT * 4u;

    viewer.document = open_document(path, &document_status);
    if (viewer.document == NULL) return document_status;
    viewer.page_count = poppler_document_get_n_pages(viewer.document);
    kilix_pdf_layout_init(&viewer.layout);
    if (viewer.page_count <= 0 ||
        !kilix_pdf_layout_set_viewport(&viewer.layout, FRAME_WIDTH,
                                       FRAME_HEIGHT, TOOLBAR_HEIGHT) ||
        !ensure_frame_surface(&viewer, FRAME_WIDTH, FRAME_HEIGHT)) {
        viewer_cleanup(&viewer);
        return EX_SOFTWARE;
    }
    kilix_pdf_layout_set_mode(&viewer.layout, KILIX_PDF_FIT_WIDTH);
    started = monotonic_nanoseconds();
    if (!select_page(&viewer, 0)) {
        viewer_cleanup(&viewer);
        return EX_SOFTWARE;
    }
    raster_elapsed = monotonic_nanoseconds() - started;

    started = monotonic_nanoseconds();
    for (int iteration = 0; iteration < COMPOSE_ITERATIONS; ++iteration) {
        if (!compose_frame(&viewer)) {
            viewer_cleanup(&viewer);
            return EX_SOFTWARE;
        }
    }
    compose_elapsed = monotonic_nanoseconds() - started;

    started = monotonic_nanoseconds();
    for (int iteration = 0; iteration < CONVERT_ITERATIONS; ++iteration) {
        if (!convert_frame_to_rgba(&viewer)) {
            viewer_cleanup(&viewer);
            return EX_SOFTWARE;
        }
    }
    convert_elapsed = monotonic_nanoseconds() - started;
    if (raster_elapsed == 0u || compose_elapsed == 0u ||
        convert_elapsed == 0u) {
        viewer_cleanup(&viewer);
        return EX_OSERR;
    }

    (void)printf("raster_cpu_ms %.3f\n",
                 (double)raster_elapsed / 1.0e6);
    (void)printf("compose_rgba_1280x720_ms %.3f\n",
                 (double)compose_elapsed / 1.0e6 /
                     (double)COMPOSE_ITERATIONS);
    (void)printf("argb_to_rgba_1280x720_ms %.3f\n",
                 (double)convert_elapsed / 1.0e6 /
                     (double)CONVERT_ITERATIONS);
    (void)printf("argb_to_rgba_gib_s %.3f\n",
                 ((double)frame_bytes * (double)CONVERT_ITERATIONS) /
                     ((double)convert_elapsed / 1.0e9) /
                     (1024.0 * 1024.0 * 1024.0));
    viewer_cleanup(&viewer);
    return EX_OK;
}

static void print_usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "usage: kilix-pdf-core PDF\n"
        "       kilix-pdf-core --render-page PDF OUTPUT.png\n"
        "       kilix-pdf-core --benchmark PDF\n"
        "       kilix-pdf-core --probe\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--probe") == 0) {
        (void)printf("kilix-pdf %s\n", KILIX_PDF_VERSION);
        (void)printf("poppler %s\n", poppler_get_version());
        (void)printf("cairo %s\n", cairo_version_string());
        (void)puts("rasterizer poppler-cairo-cpu");
        (void)puts("terminal-compose gpu-or-cpu-auto");
        return EX_OK;
    }
    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout);
        return EX_OK;
    }
    if (argc == 4 && strcmp(argv[1], "--render-page") == 0)
        return render_png(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "--benchmark") == 0)
        return benchmark_render(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--") == 0)
        return run_interactive(argv[2]);
    if (argc == 2 && argv[1][0] != '-') return run_interactive(argv[1]);
    print_usage(stderr);
    return EX_USAGE;
}
