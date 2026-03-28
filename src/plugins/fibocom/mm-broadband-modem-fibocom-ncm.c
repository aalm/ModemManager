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
#include "mm-iface-modem-3gpp-profile-manager.h"
#include "mm-log-object.h"
#include "mm-shared-xmm.h"

static void iface_modem_init                      (MMIfaceModemInterface                   *iface);
static void iface_modem_3gpp_profile_manager_init (MMIfaceModem3gppProfileManagerInterface *iface);
static void iface_modem_location_init             (MMIfaceModemLocationInterface           *iface);
static void shared_xmm_init                       (MMSharedXmmInterface                    *iface);
static void iface_modem_signal_init               (MMIfaceModemSignalInterface             *iface);

static MMIfaceModemLocationInterface           *iface_modem_location_parent;
static MMIfaceModem3gppProfileManagerInterface *iface_modem_3gpp_profile_manager_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemFibocomNcm, mm_broadband_modem_fibocom_ncm, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP_PROFILE_MANAGER, iface_modem_3gpp_profile_manager_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_SIGNAL, iface_modem_signal_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_XMM,  shared_xmm_init))

/*****************************************************************************/
/* Profile manager interface */

static gboolean
modem_3gpp_profile_manager_check_format_finish (MMIfaceModem3gppProfileManager  *self,
                                                GAsyncResult                    *res,
                                                gboolean                        *new_id,
                                                gint                            *min_profile_id,
                                                gint                            *max_profile_id,
                                                GEqualFunc                      *apn_cmp,
                                                MM3gppProfileCmpFlags           *profile_cmp_flags,
                                                GError                         **error)
{
    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;

    if (new_id)
        *new_id = TRUE;
    if (min_profile_id)
        *min_profile_id = 1;
    if (max_profile_id)
        *max_profile_id = 1;
    if (apn_cmp)
        *apn_cmp = (GEqualFunc) mm_3gpp_cmp_apn_name;
    if (profile_cmp_flags)
        *profile_cmp_flags = (MM_3GPP_PROFILE_CMP_FLAGS_NO_AUTH |
                              MM_3GPP_PROFILE_CMP_FLAGS_NO_APN_TYPE |
                              MM_3GPP_PROFILE_CMP_FLAGS_NO_ACCESS_TYPE_PREFERENCE |
                              MM_3GPP_PROFILE_CMP_FLAGS_NO_ROAMING_ALLOWANCE |
                              MM_3GPP_PROFILE_CMP_FLAGS_NO_PROFILE_SOURCE);
    return TRUE;
}

static void
modem_3gpp_profile_manager_check_format (MMIfaceModem3gppProfileManager *self,
                                         MMBearerIpFamily                ip_type,
                                         GAsyncReadyCallback             callback,
                                         gpointer                        user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
iface_modem_3gpp_profile_manager_init (MMIfaceModem3gppProfileManagerInterface *iface)
{
    iface_modem_3gpp_profile_manager_parent = g_type_interface_peek_parent (iface);

    iface->check_format = modem_3gpp_profile_manager_check_format;
    iface->check_format_finish = modem_3gpp_profile_manager_check_format_finish;

    iface->list_profiles = iface_modem_3gpp_profile_manager_parent->list_profiles;
    iface->list_profiles_finish = iface_modem_3gpp_profile_manager_parent->list_profiles_finish;
    iface->get_profile = iface_modem_3gpp_profile_manager_parent->get_profile;
    iface->get_profile_finish = iface_modem_3gpp_profile_manager_parent->get_profile_finish;
    iface->store_profile = iface_modem_3gpp_profile_manager_parent->store_profile;
    iface->store_profile_finish = iface_modem_3gpp_profile_manager_parent->store_profile_finish;
    iface->delete_profile = iface_modem_3gpp_profile_manager_parent->delete_profile;
    iface->delete_profile_finish = iface_modem_3gpp_profile_manager_parent->delete_profile_finish;
}

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
                         NULL);
}

static void
mm_broadband_modem_fibocom_ncm_init (MMBroadbandModemFibocomNcm *self)
{
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
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

static void
shared_xmm_init (MMSharedXmmInterface *iface)
{
    iface->peek_parent_broadband_modem_class = peek_parent_broadband_modem_class;
    iface->peek_parent_location_interface    = peek_parent_location_interface;
}

static void
mm_broadband_modem_fibocom_ncm_class_init (MMBroadbandModemFibocomNcmClass *klass)
{
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    broadband_modem_class->setup_ports = mm_shared_xmm_setup_ports;
    broadband_modem_class->initialization_started = mm_shared_xmm_initialization_started;
    broadband_modem_class->initialization_started_finish = mm_shared_xmm_initialization_started_finish;
}
