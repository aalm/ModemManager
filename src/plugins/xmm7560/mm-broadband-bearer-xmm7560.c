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

#include "mm-broadband-bearer-xmm7560.h"
#include "mm-broadband-modem-xmm7560.h"
#include "mm-base-modem-at.h"
#include "mm-bind.h"
#include "mm-log-object.h"

G_DEFINE_TYPE (MMBroadbandBearerXmm7560, mm_broadband_bearer_xmm7560, MM_TYPE_BROADBAND_BEARER)

/*****************************************************************************/
/* 3GPP Dialing (sub-step of the 3GPP Connection sequence) */

typedef struct {
    MMBroadbandModem *modem;
    MMPortSerialAt   *primary;
    guint             cid;
    MMPort           *data;
    gchar            *xdns_cmd;
    gchar            *xdatachannel_cmd;
    gchar            *cgact_cmd;
    gchar            *cgdata_cmd;
    gchar            *cgcontrdp_cmd;
} DialContext;

static void
dial_context_free (DialContext *ctx)
{
    g_object_unref (ctx->modem);
    g_object_unref (ctx->primary);
    g_clear_object (&ctx->data);
    g_free (ctx->xdns_cmd);
    g_free (ctx->xdatachannel_cmd);
    g_free (ctx->cgact_cmd);
    g_free (ctx->cgdata_cmd);
    g_free (ctx->cgcontrdp_cmd);
    g_slice_free (DialContext, ctx);
}

static MMPort *
dial_3gpp_finish (MMBroadbandBearer *self,
                  GAsyncResult       *res,
                  GError            **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
cgdata_ready (MMBaseModem  *modem,
              GAsyncResult *res,
              GTask        *task)
{
    MMBroadbandBearerXmm7560 *self;
    DialContext              *ctx;
    GError                   *error = NULL;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!self->ipv4_config && !self->ipv6_config) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "Failed to retrieve IP configuration during connection");
    } else {
        /* Return the data port, as expected by dial_3gpp */
        g_task_return_pointer (task,
                               g_object_ref (ctx->data),
                               g_object_unref);
    }
    g_object_unref (task);
}

static void
xdatachannel_ready (MMBaseModem  *modem,
                    GAsyncResult *res,
                    GTask        *task)
{
    DialContext *ctx;
    GError      *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_base_modem_at_command (modem,
                              ctx->cgdata_cmd,
                              10,
                              FALSE,
                              (GAsyncReadyCallback) cgdata_ready,
                              task);
}

static void
cgcontrdp_ready (MMBaseModem  *modem,
                 GAsyncResult *res,
                 GTask        *task)
{
    MMBroadbandBearerXmm7560 *self;
    DialContext              *ctx;
    const gchar              *response;
    g_auto(GStrv)             lines = NULL;
    guint                     i;
    GError                   *error = NULL;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    lines = g_strsplit (response, "\r\n", -1);
    for (i = 0; lines[i]; i++) {
        g_autofree gchar *local_address = NULL;
        g_autofree gchar *subnet = NULL;
        g_autofree gchar *gateway = NULL;
        g_autofree gchar *dns1 = NULL;
        g_autofree gchar *dns2 = NULL;
        MMBearerIpConfig *ip_config = NULL;

        if (!g_str_has_prefix (lines[i], "+CGCONTRDP:"))
            continue;

        if (mm_3gpp_parse_cgcontrdp_response (lines[i],
                                              NULL, NULL, NULL,
                                              &local_address,
                                              &subnet,
                                              &gateway,
                                              &dns1,
                                              &dns2,
                                              NULL)) {
            if (local_address) {
                GInetAddress *addr;

                addr = g_inet_address_new_from_string (local_address);
                if (addr) {
                    if (g_inet_address_get_family (addr) == G_SOCKET_FAMILY_IPV4) {
                        if (!self->ipv4_config)
                            self->ipv4_config = mm_bearer_ip_config_new ();
                        ip_config = self->ipv4_config;
                    } else if (g_inet_address_get_family (addr) == G_SOCKET_FAMILY_IPV6) {
                        if (!self->ipv6_config)
                            self->ipv6_config = mm_bearer_ip_config_new ();
                        ip_config = self->ipv6_config;
                    }
                    g_object_unref (addr);
                }

                if (ip_config) {
                    const gchar *dns[3] = { dns1, dns2, NULL };

                    mm_bearer_ip_config_set_method (ip_config, MM_BEARER_IP_METHOD_STATIC);
                    mm_bearer_ip_config_set_address (ip_config, local_address);
                    if (subnet)
                        mm_bearer_ip_config_set_prefix (ip_config, mm_netmask_to_cidr (subnet));
                    if (gateway)
                        mm_bearer_ip_config_set_gateway (ip_config, gateway);
                    if (dns1 || dns2)
                        mm_bearer_ip_config_set_dns (ip_config, dns);
                }
            }
        }
    }

    /* Continue with sequence regardless of whether we got IP info yet, 
     * but we'll check it in the final step. */
    mm_base_modem_at_command (modem,
                              ctx->xdatachannel_cmd,
                              10,
                              FALSE,
                              (GAsyncReadyCallback) xdatachannel_ready,
                              task);
}

static void
cgact_ready (MMBaseModem  *modem,
             GAsyncResult *res,
             GTask        *task)
{
    DialContext *ctx;
    GError      *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_base_modem_at_command (modem,
                              ctx->cgcontrdp_cmd,
                              10,
                              FALSE,
                              (GAsyncReadyCallback) cgcontrdp_ready,
                              task);
}

static void
xdns_ready (MMBaseModem  *modem,
            GAsyncResult *res,
            GTask        *task)
{
    DialContext *ctx;
    GError      *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_base_modem_at_command (modem,
                              ctx->cgact_cmd,
                              10,
                              FALSE,
                              (GAsyncReadyCallback) cgact_ready,
                              task);
}

static void
dial_3gpp (MMBroadbandBearer  *self,
           MMBaseModem        *modem,
           MMPortSerialAt     *primary,
           guint               cid,
           GCancellable       *cancellable,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
    DialContext *ctx;
    GTask       *task;

    ctx          = g_slice_new0 (DialContext);
    ctx->modem   = MM_BROADBAND_MODEM (g_object_ref (modem));
    ctx->primary = g_object_ref (primary);
    ctx->cid     = cid;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) dial_context_free);

    ctx->data = mm_base_modem_get_best_data_port (modem, MM_PORT_TYPE_NET);
    if (!ctx->data) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "No valid data port found to launch connection");
        g_object_unref (task);
        return;
    }

    ctx->xdns_cmd         = g_strdup_printf ("+XDNS=%u,1", cid);
    ctx->cgact_cmd        = g_strdup_printf ("+CGACT=1,%u", cid);
    ctx->cgcontrdp_cmd    = g_strdup_printf ("+CGCONTRDP=%u", cid);
    ctx->xdatachannel_cmd = g_strdup_printf ("+XDATACHANNEL=1,1,\"/USBCDC/0\",\"/USBHS/NCM/0\",2,%u", cid);
    ctx->cgdata_cmd       = g_strdup_printf ("+CGDATA=\"M-RAW_IP\",%u", cid);

    mm_base_modem_at_command (modem,
                              ctx->xdns_cmd,
                              10,
                              FALSE,
                              (GAsyncReadyCallback) xdns_ready,
                              task);
}

/*****************************************************************************/
/* 3GPP Disconnect sequence */

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer *self,
                        GAsyncResult       *res,
                        GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
disconnect_cgact_ready (MMBaseModem  *modem,
                        GAsyncResult *res,
                        GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (modem, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
disconnect_3gpp (MMBroadbandBearer *self,
                 MMBroadbandModem  *modem,
                 MMPortSerialAt    *primary,
                 MMPortSerialAt    *secondary,
                 MMPort            *data,
                 guint              cid,
                 GAsyncReadyCallback callback,
                 gpointer            user_data)
{
    GTask *task;
    gchar *cmd;
    MMBroadbandBearerXmm7560 *xmm_self = MM_BROADBAND_BEARER_XMM7560 (self);

    g_clear_object (&xmm_self->ipv4_config);
    g_clear_object (&xmm_self->ipv6_config);

    task = g_task_new (self, NULL, callback, user_data);

    cmd = g_strdup_printf ("+CGACT=0,%u", cid);
    mm_base_modem_at_command (MM_BASE_MODEM (modem),
                              cmd,
                              10,
                              FALSE,
                              (GAsyncReadyCallback) disconnect_cgact_ready,
                              task);
    g_free (cmd);
}

/*****************************************************************************/

static gboolean
get_ip_config_3gpp_finish (MMBroadbandBearer *self,
                           GAsyncResult *res,
                           MMBearerIpConfig **ipv4_config,
                           MMBearerIpConfig **ipv6_config,
                           GError **error)
{
    MMBroadbandBearerXmm7560 *xmm_self = MM_BROADBAND_BEARER_XMM7560 (self);

    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;

    if (ipv4_config && xmm_self->ipv4_config)
        *ipv4_config = g_object_ref (xmm_self->ipv4_config);
    if (ipv6_config && xmm_self->ipv6_config)
        *ipv6_config = g_object_ref (xmm_self->ipv6_config);

    return TRUE;
}

static void
get_ip_config_3gpp (MMBroadbandBearer *self,
                    MMBroadbandModem *modem,
                    MMPortSerialAt *primary,
                    MMPortSerialAt *secondary,
                    MMPort *data,
                    guint cid,
                    MMBearerIpFamily ip_family,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/*****************************************************************************/

MMBaseBearer *
mm_broadband_bearer_xmm7560_new_finish (GAsyncResult *res,
                                        GError      **error)
{
    GObject *bearer;
    GObject *source;

    source = g_async_result_get_source_object (res);
    bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);

    if (!bearer)
        return NULL;

    mm_base_bearer_export (MM_BASE_BEARER (bearer));
    return MM_BASE_BEARER (bearer);
}

void
mm_broadband_bearer_xmm7560_new (MMBroadbandModemXmm7560 *modem,
                                 MMBearerProperties      *properties,
                                 GCancellable            *cancellable,
                                 GAsyncReadyCallback      callback,
                                 gpointer                 user_data)
{
    g_async_initable_new_async (
        MM_TYPE_BROADBAND_BEARER_XMM7560,
        G_PRIORITY_DEFAULT,
        cancellable,
        callback,
        user_data,
        MM_BASE_BEARER_MODEM, modem,
        MM_BIND_TO, modem,
        MM_BASE_BEARER_CONFIG, properties,
        NULL);
}

static void
mm_broadband_bearer_xmm7560_init (MMBroadbandBearerXmm7560 *self)
{
}

static void
dispose (GObject *object)
{
    MMBroadbandBearerXmm7560 *self = MM_BROADBAND_BEARER_XMM7560 (object);

    g_clear_object (&self->ipv4_config);
    g_clear_object (&self->ipv6_config);

    G_OBJECT_CLASS (mm_broadband_bearer_xmm7560_parent_class)->dispose (object);
}

static MMBearerConnectionStatus
load_connection_status_finish (MMBaseBearer  *bearer,
                               GAsyncResult  *res,
                               GError       **error)
{
    g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED, "Connection status loading is unsupported");
    return MM_BEARER_CONNECTION_STATUS_UNKNOWN;
}

static void
load_connection_status (MMBaseBearer        *self,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_int (task, MM_BEARER_CONNECTION_STATUS_UNKNOWN);
    g_object_unref (task);
}

static void
mm_broadband_bearer_xmm7560_class_init (MMBroadbandBearerXmm7560Class *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBaseBearerClass *base_bearer_class = MM_BASE_BEARER_CLASS (klass);
    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    object_class->dispose = dispose;

    base_bearer_class->load_connection_status = load_connection_status;
    base_bearer_class->load_connection_status_finish = load_connection_status_finish;

    broadband_bearer_class->dial_3gpp = dial_3gpp;
    broadband_bearer_class->dial_3gpp_finish = dial_3gpp_finish;
    broadband_bearer_class->get_ip_config_3gpp = get_ip_config_3gpp;
    broadband_bearer_class->get_ip_config_3gpp_finish = get_ip_config_3gpp_finish;
    broadband_bearer_class->disconnect_3gpp = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;
}
