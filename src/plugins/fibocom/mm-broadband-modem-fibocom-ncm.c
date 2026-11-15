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

#include <config.h>

#include "mm-broadband-modem-fibocom-ncm.h"
#include "mm-broadband-bearer-fibocom-ncm.h"
#include "mm-broadband-modem.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-signal.h"
#include "mm-log-object.h"
#include "mm-shared-xmm.h"
#include "mm-modem-helpers-xmm.h"
#include "mm-modem-helpers.h"

static void iface_modem_init                      (MMIfaceModemInterface                   *iface);
static void iface_modem_location_init             (MMIfaceModemLocationInterface           *iface);
static void shared_xmm_init                       (MMSharedXmmInterface                    *iface);
static void iface_modem_signal_init               (MMIfaceModemSignalInterface             *iface);

static MMIfaceModemLocationInterface           *iface_modem_location_parent;
static MMIfaceModemInterface                   *iface_modem_parent;

/*****************************************************************************/

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemFibocomNcm, mm_broadband_modem_fibocom_ncm, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_SIGNAL, iface_modem_signal_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_XMM,  shared_xmm_init))

/*****************************************************************************/
/* Create Bearer (Modem interface) */

static MMBaseBearer *
modem_create_bearer_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
broadband_bearer_fibocom_ncm_new_ready (GObject *source,
                                    GAsyncResult *res,
                                    GTask *task)
{
    MMBaseBearer *bearer = NULL;
    GError       *error = NULL;

    bearer = mm_broadband_bearer_fibocom_ncm_new_finish (res, &error);
    if (!bearer)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bearer, g_object_unref);
    g_object_unref (task);
}

static void
modem_create_bearer (MMIfaceModem       *self,
                     MMBearerProperties *properties,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, g_object_ref (properties), g_object_unref);

    mm_obj_dbg (self, "creating FIBOCOM_NCM bearer");
    mm_broadband_bearer_fibocom_ncm_new (MM_BROADBAND_MODEM_FIBOCOM_NCM (self),
                                     properties,
                                     NULL, /* cancellable */
                                     (GAsyncReadyCallback) broadband_bearer_fibocom_ncm_new_ready,
                                     task);
}

static void
xcesqi_urc_received (MMPortSerialAt             *port,
                     GMatchInfo                 *match_info,
                     MMBroadbandModemFibocomNcm *self)
{
    g_autofree gchar    *content = NULL;
    g_autofree gchar    *response = NULL;
    g_autoptr(MMSignal)  gsm = NULL;
    g_autoptr(MMSignal)  umts = NULL;
    g_autoptr(MMSignal)  lte = NULL;
    guint                quality = 0;
    gboolean             has_quality = FALSE;

    content = g_match_info_fetch (match_info, 1);
    if (!content)
        return;

    mm_obj_dbg (self, "received +XCESQI URC: %s", content);

    /* Prefix with mode "0," so the tested +XCESQ response parser can decode the URC payload */
    response = g_strdup_printf ("+XCESQ: 0,%s", content);
    if (!mm_xmm_xcesq_response_to_signal_info (response, self, &gsm, &umts, &lte, NULL))
        return;

    if (mm_xmm_signal_quality_from_lte (lte, &quality)) {
        has_quality = TRUE;
    } else if (umts) {
        gdouble rscp = mm_signal_get_rscp (umts);

        if (rscp != MM_SIGNAL_UNKNOWN) {
            quality = mm_signal_quality_from_range (rscp, -120, -25);
            has_quality = TRUE;
        }
    } else if (gsm) {
        gdouble rssi = mm_signal_get_rssi (gsm);

        if (rssi != MM_SIGNAL_UNKNOWN) {
            quality = MM_RSSI_TO_QUALITY (rssi);
            has_quality = TRUE;
        }
    }

    if (has_quality)
        mm_iface_modem_update_signal_quality (MM_IFACE_MODEM (self), quality);

    /* Also update Signal interface */
    mm_iface_modem_signal_update (MM_IFACE_MODEM_SIGNAL (self), NULL, NULL, gsm, umts, lte, NULL);
}

static void
setup_ports (MMBroadbandModem *self)
{
    MMPortSerialAt    *ports[2];
    guint              i;
    g_autoptr(GRegex)  xcesqi_regex = NULL;
    g_autoptr(GRegex)  sim_ready_regex = NULL;

    /* Parent XMM/base setup first */
    mm_shared_xmm_setup_ports (self);

    /* Loose regex to catch any +XCESQI URC */
    xcesqi_regex = g_regex_new ("\\+XCESQI:\\s*([^\\r\\n]+)",
                                G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    /* Ignore +SIM READY URCs */
    sim_ready_regex = g_regex_new ("\\r\\n\\+SIM READY\\r\\n",
                                   G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);

    ports[0] = mm_base_modem_peek_port_primary   (MM_BASE_MODEM (self));
    ports[1] = mm_base_modem_peek_port_secondary (MM_BASE_MODEM (self));

    for (i = 0; i < G_N_ELEMENTS (ports); i++) {
        if (!ports[i])
            continue;
        mm_obj_dbg (self, "adding +XCESQI URC handler to port %s", mm_port_get_device (MM_PORT (ports[i])));
        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i],
            xcesqi_regex,
            (MMPortSerialAtUnsolicitedMsgFn) xcesqi_urc_received,
            self,
            NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i],
            sim_ready_regex,
            NULL, NULL, NULL);
    }

    /* Enable signal reporting engine(55sec interval) and URCs fire-and-forget. */
    mm_base_modem_at_command (MM_BASE_MODEM (self), "AT+XCESQRC=1,2,0,0,0,0,0,0,0,55", 3, FALSE, NULL, NULL);
    mm_base_modem_at_command (MM_BASE_MODEM (self), "AT+XCESQ=1", 3, FALSE, NULL, NULL);
}

/*****************************************************************************/

MMBroadbandModemFibocomNcm *
mm_broadband_modem_fibocom_ncm_new (const gchar  *device,
                                const gchar  *physdev,
                                const gchar **drivers,
                                const gchar  *plugin,
                                guint16       vendor_id,
                                guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_FIBOCOM_NCM,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_PHYSDEV,    physdev,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, TRUE,
                         /* Load initial signal via +XCESQ? and rely on +XCESQI URCs afterwards */
                         MM_IFACE_MODEM_PERIODIC_SIGNAL_CHECK_DISABLED, TRUE,
                         NULL);
}

static void
mm_broadband_modem_fibocom_ncm_init (MMBroadbandModemFibocomNcm *self)
{
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);

    iface->load_supported_modes        = mm_shared_xmm_load_supported_modes;
    iface->load_supported_modes_finish = mm_shared_xmm_load_supported_modes_finish;
    iface->load_current_modes          = mm_shared_xmm_load_current_modes;
    iface->load_current_modes_finish   = mm_shared_xmm_load_current_modes_finish;
    iface->set_current_modes           = mm_shared_xmm_set_current_modes;
    iface->set_current_modes_finish    = mm_shared_xmm_set_current_modes_finish;

    iface->load_supported_bands        = mm_shared_xmm_load_supported_bands;
    iface->load_supported_bands_finish = mm_shared_xmm_load_supported_bands_finish;
    iface->load_current_bands          = mm_shared_xmm_load_current_bands;
    iface->load_current_bands_finish   = mm_shared_xmm_load_current_bands_finish;
    iface->set_current_bands           = mm_shared_xmm_set_current_bands;
    iface->set_current_bands_finish    = mm_shared_xmm_set_current_bands_finish;

    iface->load_power_state        = mm_shared_xmm_load_power_state;
    iface->load_power_state_finish = mm_shared_xmm_load_power_state_finish;
    iface->modem_power_up          = mm_shared_xmm_power_up;
    iface->modem_power_up_finish   = mm_shared_xmm_power_up_finish;
    iface->modem_power_down        = mm_shared_xmm_power_down;
    iface->modem_power_down_finish = mm_shared_xmm_power_down_finish;
    iface->modem_power_off         = mm_shared_xmm_power_off;
    iface->modem_power_off_finish  = mm_shared_xmm_power_off_finish;
    iface->reset                   = mm_shared_xmm_reset;
    iface->reset_finish            = mm_shared_xmm_reset_finish;

    iface->load_signal_quality        = mm_shared_xmm_load_signal_quality;
    iface->load_signal_quality_finish = mm_shared_xmm_load_signal_quality_finish;

    iface->create_bearer        = modem_create_bearer;
    iface->create_bearer_finish = modem_create_bearer_finish;
}

static void
iface_modem_location_init (MMIfaceModemLocationInterface *iface)
{
    iface_modem_location_parent = g_type_interface_peek_parent (iface);

    iface->load_capabilities                 = mm_shared_xmm_location_load_capabilities;
    iface->load_capabilities_finish          = mm_shared_xmm_location_load_capabilities_finish;
    iface->enable_location_gathering         = mm_shared_xmm_enable_location_gathering;
    iface->enable_location_gathering_finish  = mm_shared_xmm_enable_location_gathering_finish;
    iface->disable_location_gathering        = mm_shared_xmm_disable_location_gathering;
    iface->disable_location_gathering_finish = mm_shared_xmm_disable_location_gathering_finish;
    iface->load_supl_server                  = mm_shared_xmm_location_load_supl_server;
    iface->load_supl_server_finish           = mm_shared_xmm_location_load_supl_server_finish;
    iface->set_supl_server                   = mm_shared_xmm_location_set_supl_server;
    iface->set_supl_server_finish            = mm_shared_xmm_location_set_supl_server_finish;
}

static void
iface_modem_signal_init (MMIfaceModemSignalInterface *iface)
{
    iface->check_support        = mm_shared_xmm_signal_check_support;
    iface->check_support_finish = mm_shared_xmm_signal_check_support_finish;
    iface->load_values          = mm_shared_xmm_signal_load_values;
    iface->load_values_finish   = mm_shared_xmm_signal_load_values_finish;
}

static MMBroadbandModemClass *
peek_parent_broadband_modem_class (MMSharedXmm *self)
{
    return MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_fibocom_ncm_parent_class);
}

static MMIfaceModemLocationInterface *
peek_parent_location_interface (MMSharedXmm *self)
{
    return iface_modem_location_parent;
}

static MMIfaceModemInterface *
peek_parent_modem_interface (MMSharedXmm *self)
{
    return iface_modem_parent;
}

static void
shared_xmm_init (MMSharedXmmInterface *iface)
{
    iface->peek_parent_broadband_modem_class = peek_parent_broadband_modem_class;
    iface->peek_parent_location_interface    = peek_parent_location_interface;
    iface->peek_parent_modem_interface       = peek_parent_modem_interface;
}

static void
mm_broadband_modem_fibocom_ncm_class_init (MMBroadbandModemFibocomNcmClass *klass)
{
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    broadband_modem_class->setup_ports = setup_ports;
    broadband_modem_class->initialization_started = mm_shared_xmm_initialization_started;
    broadband_modem_class->initialization_started_finish = mm_shared_xmm_initialization_started_finish;
}
