/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KILIX_PDF_LAYOUT_H
#define KILIX_PDF_LAYOUT_H

#include <stdbool.h>

typedef enum kilix_pdf_fit_mode {
    KILIX_PDF_FIT_PAGE = 1,
    KILIX_PDF_FIT_WIDTH = 2,
    KILIX_PDF_CUSTOM_ZOOM = 3
} kilix_pdf_fit_mode;

typedef struct kilix_pdf_layout {
    double page_width_points;
    double page_height_points;
    double custom_scale;
    int frame_width;
    int frame_height;
    int toolbar_height;
    int margin;
    int scroll_y;
    kilix_pdf_fit_mode mode;
} kilix_pdf_layout;

void kilix_pdf_layout_init(kilix_pdf_layout *layout);
bool kilix_pdf_layout_set_viewport(kilix_pdf_layout *layout,
                                   int width, int height,
                                   int toolbar_height);
bool kilix_pdf_layout_set_page(kilix_pdf_layout *layout,
                               double width_points,
                               double height_points);
void kilix_pdf_layout_set_mode(kilix_pdf_layout *layout,
                               kilix_pdf_fit_mode mode);
bool kilix_pdf_layout_zoom(kilix_pdf_layout *layout, double factor);
double kilix_pdf_layout_scale(const kilix_pdf_layout *layout);
int kilix_pdf_layout_page_width(const kilix_pdf_layout *layout);
int kilix_pdf_layout_page_height(const kilix_pdf_layout *layout);
int kilix_pdf_layout_max_scroll(const kilix_pdf_layout *layout);
int kilix_pdf_layout_scroll(kilix_pdf_layout *layout, int delta);

#endif
