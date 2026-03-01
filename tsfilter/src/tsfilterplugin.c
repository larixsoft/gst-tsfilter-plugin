/*
 * tsfilterplugin.c - GStreamer plugin registration for tsfilter
 * Copyright (C) 2025 LarixSoft <https://github.com/larixsoft>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 */

#include "config.h"

#include <gst/gst.h>
#include "tsfilter.h"

/* Plugin initialization */
static gboolean
plugin_init(GstPlugin *plugin)
{
  gboolean ret = FALSE;

  /* Register the tsfilter element */
  ret = GST_ELEMENT_REGISTER(tsfilter, plugin);

  return ret;
}

/* Plugin description */
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
                 GST_VERSION_MINOR,
                 tsfilter,
                 "MPEG-TS PID filter plugin",
                 plugin_init,
                 VERSION,
                 "LGPL",
                 PACKAGE_NAME,
                 PACKAGE_ORIGIN)
