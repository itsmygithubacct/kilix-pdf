/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kilix_pdf_layout.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

enum {
    DEFAULT_MARGIN = 16,
    MIN_CONTENT_PIXELS = 1
};

static double clamp_scale(double scale)
{
    if (scale < 0.10) return 0.10;
    if (scale > 8.0) return 8.0;
    return scale;
}

static int rounded_pixels(double points, double scale)
{
    const double pixels = ceil(points * scale);

    if (!isfinite(pixels) || pixels < 1.0 || pixels > (double)INT_MAX)
        return 0;
    return (int)pixels;
}

void kilix_pdf_layout_init(kilix_pdf_layout *layout)
{
    if (layout == NULL) return;
    *layout = (kilix_pdf_layout){0};
    layout->custom_scale = 1.0;
    layout->margin = DEFAULT_MARGIN;
    layout->mode = KILIX_PDF_FIT_PAGE;
}

bool kilix_pdf_layout_set_viewport(kilix_pdf_layout *layout,
                                   int width, int height,
                                   int toolbar_height)
{
    if (layout == NULL || width < MIN_CONTENT_PIXELS ||
        height < MIN_CONTENT_PIXELS || toolbar_height < 0 ||
        toolbar_height >= height) {
        return false;
    }
    layout->frame_width = width;
    layout->frame_height = height;
    layout->toolbar_height = toolbar_height;
    if (layout->scroll_y > kilix_pdf_layout_max_scroll(layout))
        layout->scroll_y = kilix_pdf_layout_max_scroll(layout);
    return true;
}

bool kilix_pdf_layout_set_page(kilix_pdf_layout *layout,
                               double width_points,
                               double height_points)
{
    if (layout == NULL || !isfinite(width_points) ||
        !isfinite(height_points) || width_points <= 0.0 ||
        height_points <= 0.0) {
        return false;
    }
    layout->page_width_points = width_points;
    layout->page_height_points = height_points;
    layout->scroll_y = 0;
    return true;
}

double kilix_pdf_layout_scale(const kilix_pdf_layout *layout)
{
    double width_scale;
    double height_scale;
    int content_width;
    int content_height;

    if (layout == NULL || layout->frame_width <= 0 ||
        layout->frame_height <= layout->toolbar_height ||
        layout->page_width_points <= 0.0 ||
        layout->page_height_points <= 0.0) {
        return 0.0;
    }
    content_width = layout->frame_width - 2 * layout->margin;
    content_height = layout->frame_height - layout->toolbar_height -
                     2 * layout->margin;
    if (content_width < MIN_CONTENT_PIXELS)
        content_width = MIN_CONTENT_PIXELS;
    if (content_height < MIN_CONTENT_PIXELS)
        content_height = MIN_CONTENT_PIXELS;
    width_scale = (double)content_width / layout->page_width_points;
    height_scale = (double)content_height / layout->page_height_points;
    if (layout->mode == KILIX_PDF_FIT_WIDTH)
        return clamp_scale(width_scale);
    if (layout->mode == KILIX_PDF_CUSTOM_ZOOM)
        return clamp_scale(layout->custom_scale);
    return clamp_scale(width_scale < height_scale ? width_scale
                                                   : height_scale);
}

int kilix_pdf_layout_page_width(const kilix_pdf_layout *layout)
{
    return layout == NULL ? 0 :
           rounded_pixels(layout->page_width_points,
                          kilix_pdf_layout_scale(layout));
}

int kilix_pdf_layout_page_height(const kilix_pdf_layout *layout)
{
    return layout == NULL ? 0 :
           rounded_pixels(layout->page_height_points,
                          kilix_pdf_layout_scale(layout));
}

int kilix_pdf_layout_max_scroll(const kilix_pdf_layout *layout)
{
    int page_height;
    int viewport_height;
    int64_t content_height;

    if (layout == NULL) return 0;
    page_height = kilix_pdf_layout_page_height(layout);
    viewport_height = layout->frame_height - layout->toolbar_height;
    if (page_height <= 0 || viewport_height <= 0) return 0;
    content_height = (int64_t)page_height + 2 * (int64_t)layout->margin;
    if (content_height <= viewport_height) return 0;
    if (content_height - viewport_height > INT_MAX) return INT_MAX;
    return (int)(content_height - viewport_height);
}

void kilix_pdf_layout_set_mode(kilix_pdf_layout *layout,
                               kilix_pdf_fit_mode mode)
{
    if (layout == NULL ||
        (mode != KILIX_PDF_FIT_PAGE && mode != KILIX_PDF_FIT_WIDTH &&
         mode != KILIX_PDF_CUSTOM_ZOOM)) {
        return;
    }
    if (mode == KILIX_PDF_CUSTOM_ZOOM &&
        layout->mode != KILIX_PDF_CUSTOM_ZOOM) {
        layout->custom_scale = kilix_pdf_layout_scale(layout);
    }
    layout->mode = mode;
    layout->scroll_y = 0;
}

bool kilix_pdf_layout_zoom(kilix_pdf_layout *layout, double factor)
{
    double old_scale;
    double new_scale;
    double center_points;
    double requested_scroll;
    int viewport_height;
    int target_scroll;

    if (layout == NULL || !isfinite(factor) || factor <= 0.0)
        return false;
    old_scale = kilix_pdf_layout_scale(layout);
    if (old_scale <= 0.0) return false;
    viewport_height = layout->frame_height - layout->toolbar_height;
    center_points = ((double)layout->scroll_y +
                     (double)viewport_height / 2.0 -
                     (double)layout->margin) / old_scale;
    new_scale = clamp_scale(old_scale * factor);
    layout->mode = KILIX_PDF_CUSTOM_ZOOM;
    layout->custom_scale = new_scale;
    requested_scroll = center_points * new_scale -
                       (double)viewport_height / 2.0 +
                       (double)layout->margin;
    if (requested_scroll <= 0.0)
        target_scroll = 0;
    else if (requested_scroll >= (double)INT_MAX)
        target_scroll = INT_MAX;
    else
        target_scroll = (int)llround(requested_scroll);
    layout->scroll_y = 0;
    (void)kilix_pdf_layout_scroll(layout, target_scroll);
    return new_scale != old_scale;
}

int kilix_pdf_layout_scroll(kilix_pdf_layout *layout, int delta)
{
    int old_scroll;
    int maximum;
    int64_t requested;

    if (layout == NULL || delta == 0) return 0;
    old_scroll = layout->scroll_y;
    maximum = kilix_pdf_layout_max_scroll(layout);
    requested = (int64_t)old_scroll + delta;
    if (requested < 0) requested = 0;
    if (requested > maximum) requested = maximum;
    layout->scroll_y = (int)requested;
    return layout->scroll_y - old_scroll;
}
