/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2024 Thomas Vogt
 */

#include <stdlib.h>
#include <gmodule.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log-object.h"
#include "mm-plugin-common.h"
#include "mm-broadband-modem.h"
#include "mm-broadband-modem-xmm.h"
#include "mm-broadband-modem-xmm7560.h"

#define MM_TYPE_PLUGIN_XMM7560 mm_plugin_xmm7560_get_type ()
MM_DEFINE_PLUGIN (XMM7560, xmm7560, Xmm7560)

/*****************************************************************************/

static MMBaseModem *
create_modem (MMPlugin     *self,
              const gchar  *uid,
              const gchar  *physdev,
              const gchar **drivers,
              guint16       vendor,
              guint16       product,
              guint16       subsystem_vendor,
              guint16       subsystem_device,
              GList        *probes,
              GError      **error)
{
    if (vendor == 0x8087 && product == 0x095a) {
        mm_obj_dbg (self, "Intel L860-GL XMM7560 modem found...");
        return MM_BASE_MODEM (mm_broadband_modem_xmm7560_new (uid,
                                                              physdev,
                                                              drivers,
                                                              mm_plugin_get_name (self),
                                                              vendor,
                                                              product));
    }

    if (mm_port_probe_list_is_xmm (probes)) {
        mm_obj_dbg (self, "XMM-based modem found...");
        return MM_BASE_MODEM (mm_broadband_modem_xmm_new (uid,
                                                          physdev,
                                                          drivers,
                                                          mm_plugin_get_name (self),
                                                          vendor,
                                                          product));
    }

    mm_obj_dbg (self, "Generic modem found...");
    return MM_BASE_MODEM (mm_broadband_modem_new (uid,
                                                  physdev,
                                                  drivers,
                                                  mm_plugin_get_name (self),
                                                  vendor,
                                                  product));
}

/*****************************************************************************/

MM_PLUGIN_NAMED_CREATOR_SCOPE MMPlugin *
mm_plugin_create_xmm7560 (void)
{
    static const gchar *subsystems[] = { "tty", "net", "usbmisc", NULL };
    static const guint16 vendor_ids[] = { 0x8087, 0 };
    static const mm_uint16_pair product_ids[] = { { 0x8087, 0x095a }, { 0, 0 } };

    return MM_PLUGIN (
        g_object_new (MM_TYPE_PLUGIN_XMM7560,
                      MM_PLUGIN_NAME,               "Xmm7560",
                      MM_PLUGIN_ALLOWED_SUBSYSTEMS, subsystems,
                      MM_PLUGIN_ALLOWED_VENDOR_IDS, vendor_ids,
                      MM_PLUGIN_ALLOWED_PRODUCT_IDS, product_ids,
                      MM_PLUGIN_ALLOWED_AT,         TRUE,
                      MM_PLUGIN_XMM_PROBE,          TRUE,
                      NULL));
}

static void
mm_plugin_xmm7560_init (MMPluginXmm7560 *self)
{
}

static void
mm_plugin_xmm7560_class_init (MMPluginXmm7560Class *klass)
{
    MMPluginClass *plugin_class = MM_PLUGIN_CLASS (klass);

    plugin_class->create_modem = create_modem;
}
