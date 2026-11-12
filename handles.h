#pragma once

#include "handle.h"

#include <cairo.h>
#include <glib-object.h>
#include <pango/pango.h>

template <class Object> using GObjectHandle = Handle<Object, g_object_unref>;
using GErrorHandle = Handle<GError, g_error_free>;
using GStringHandle = Handle<gchar, g_free>;

using LayoutHandle = GObjectHandle<PangoLayout>;
using AttributesHandle = Handle<PangoAttrList, pango_attr_list_unref>;
using FontHandle = Handle<PangoFontDescription, pango_font_description_free>;
using SurfaceHandle = Handle<cairo_surface_t, cairo_surface_destroy>;
using LayoutIterHandle = Handle<PangoLayoutIter, pango_layout_iter_free>;
using TabsHandle = Handle<PangoTabArray, pango_tab_array_free>;
using CairoHandle = Handle<cairo_t, cairo_destroy>;
using FontOptionsHandle = Handle<cairo_font_options_t, cairo_font_options_destroy>;

inline FontHandle font_description(const char* family, int pixels)
{
    FontHandle font{pango_font_description_from_string(family)};
    pango_font_description_set_absolute_size(font.get(), pixels * PANGO_SCALE);
    return font;
}
