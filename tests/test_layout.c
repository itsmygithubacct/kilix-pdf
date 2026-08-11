/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kilix_pdf_layout.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",            \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                    \
        }                                                                    \
    } while (false)

static bool near(double left, double right)
{
    return fabs(left - right) < 0.0001;
}

static bool test_fit_and_scroll(void)
{
    kilix_pdf_layout layout;

    kilix_pdf_layout_init(&layout);
    CHECK(kilix_pdf_layout_set_viewport(&layout, 1000, 800, 48));
    CHECK(kilix_pdf_layout_set_page(&layout, 600.0, 800.0));
    CHECK(near(kilix_pdf_layout_scale(&layout), 0.9));
    CHECK(kilix_pdf_layout_page_width(&layout) == 540);
    CHECK(kilix_pdf_layout_page_height(&layout) == 720);
    CHECK(kilix_pdf_layout_max_scroll(&layout) == 0);

    kilix_pdf_layout_set_mode(&layout, KILIX_PDF_FIT_WIDTH);
    CHECK(near(kilix_pdf_layout_scale(&layout), 968.0 / 600.0));
    CHECK(kilix_pdf_layout_max_scroll(&layout) == 571);
    CHECK(kilix_pdf_layout_scroll(&layout, 80) == 80);
    CHECK(kilix_pdf_layout_scroll(&layout, -20) == -20);
    CHECK(kilix_pdf_layout_scroll(&layout, 10000) == 511);
    CHECK(layout.scroll_y == 571);
    CHECK(kilix_pdf_layout_scroll(&layout, -10000) == -571);
    CHECK(layout.scroll_y == 0);
    return true;
}

static bool test_zoom_and_clamps(void)
{
    kilix_pdf_layout layout;
    double before;

    kilix_pdf_layout_init(&layout);
    CHECK(!kilix_pdf_layout_set_viewport(&layout, 0, 800, 48));
    CHECK(!kilix_pdf_layout_set_viewport(&layout, 800, 48, 48));
    CHECK(kilix_pdf_layout_set_viewport(&layout, 800, 600, 40));
    CHECK(!kilix_pdf_layout_set_page(&layout, -1.0, 500.0));
    CHECK(kilix_pdf_layout_set_page(&layout, 400.0, 1200.0));
    kilix_pdf_layout_set_mode(&layout, KILIX_PDF_FIT_WIDTH);
    CHECK(kilix_pdf_layout_scroll(&layout, 300) == 300);
    before = kilix_pdf_layout_scale(&layout);
    CHECK(kilix_pdf_layout_zoom(&layout, 1.25));
    CHECK(layout.mode == KILIX_PDF_CUSTOM_ZOOM);
    CHECK(near(kilix_pdf_layout_scale(&layout), before * 1.25));
    CHECK(layout.scroll_y > 300);
    CHECK(!kilix_pdf_layout_zoom(&layout, 0.0));

    CHECK(kilix_pdf_layout_zoom(&layout, 1000.0));
    CHECK(near(kilix_pdf_layout_scale(&layout), 8.0));
    CHECK(kilix_pdf_layout_zoom(&layout, 0.00001));
    CHECK(near(kilix_pdf_layout_scale(&layout), 0.1));
    kilix_pdf_layout_set_mode(&layout, KILIX_PDF_FIT_PAGE);
    CHECK(layout.scroll_y == 0);
    return true;
}

int main(void)
{
    if (!test_fit_and_scroll() || !test_zoom_and_clamps()) return 1;
    (void)puts("2 layout tests passed");
    return 0;
}
