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

#include <arpa/inet.h>
#include <netinet/in.h>

#include "mm-broadband-bearer-fibocom-ncm.h"
#include "mm-broadband-modem-fibocom-ncm.h"
#include "mm-base-modem-at.h"
#include "mm-bind.h"
#include "mm-modem-helpers.h"
#include "mm-log-object.h"

G_DEFINE_TYPE (MMBroadbandBearerFibocomNcm, mm_broadband_bearer_fibocom_ncm, MM_TYPE_BROADBAND_BEARER)

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
    MMBroadbandBearerFibocomNcm *self;
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

/*****************************************************************************/
/* CGCONTRDP response parsing */

static guint
local_netmask6_to_cidr (const gchar *netmask)
{
    struct in6_addr num;
    guint           i;
    guint           cidr = 0;

    if (inet_pton (AF_INET6, netmask, &num) <= 0)
        return 0;

    for (i = 0; i < 16; i++)
        cidr += mm_count_bits_set (num.s6_addr[i]);

    return cidr;
}

static void
cgcontrdp_ready (MMBaseModem  *modem,
                 GAsyncResult *res,
                 GTask        *task)
{
    MMBroadbandBearerFibocomNcm *self;
    DialContext                 *ctx;
    const gchar                 *response;
    g_auto(GStrv)                lines = NULL;
    guint                        i;
    GError                      *error = NULL;

    self = MM_BROADBAND_BEARER_FIBOCOM_NCM (g_task_get_source_object (task));
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
        guint             prefix = 0;

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
                        if (subnet)
                            prefix = mm_netmask_to_cidr (subnet);
                        /* XXX if we get prefix 8, it's likely a dummy mask (255.0.0.0)
                         * often seen in Fibocom modems(?), default to 32. */
                        if (prefix == 8)
                            prefix = 32;
                    } else if (g_inet_address_get_family (addr) == G_SOCKET_FAMILY_IPV6) {
                        if (!self->ipv6_config)
                            self->ipv6_config = mm_bearer_ip_config_new ();
                        ip_config = self->ipv6_config;
                        if (subnet)
                            prefix = local_netmask6_to_cidr (subnet);
                        /* XXX if we get prefix 0, or it's a link-local address,
                         * default to 64. */
                        if (prefix == 0 || g_inet_address_get_is_link_local (addr))
                            prefix = 64;
                    }
                    g_object_unref (addr);
                }

                if (ip_config) {
                    const gchar *dns[3] = { NULL, NULL, NULL };
                    guint        n_dns = 0;

                    if (dns1)
                        dns[n_dns++] = dns1;
                    if (dns2)
                        dns[n_dns++] = dns2;

                    mm_bearer_ip_config_set_method (ip_config, MM_BEARER_IP_METHOD_STATIC);
                    mm_bearer_ip_config_set_address (ip_config, local_address);
                    if (prefix > 0)
                        mm_bearer_ip_config_set_prefix (ip_config, prefix);
                    if (gateway)
                        mm_bearer_ip_config_set_gateway (ip_config, gateway);
                    if (n_dns > 0)
                        mm_bearer_ip_config_set_dns (ip_config, dns);
                }
            }
        }
    }

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
    const gchar *data_name;
    guint        ncm_idx = 0;

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

    data_name = mm_port_get_device (ctx->data);
    if (g_str_has_prefix (data_name, "wwan"))
        ncm_idx = (guint) g_ascii_strtoull (data_name + 4, NULL, 10);

    /* Select IP family for XDNS command.
     * The modem supports:
     * 1: IPv4
     * 2: IPv6
     * If IPV4V6 is requested, default to IPv4 for the DNS request trigger.
     */
    switch (mm_bearer_properties_get_ip_type (mm_base_bearer_peek_config (MM_BASE_BEARER (self)))) {
    case MM_BEARER_IP_FAMILY_IPV4V6:
    case MM_BEARER_IP_FAMILY_IPV4:
    default:
        ctx->xdns_cmd = g_strdup_printf ("+XDNS=%u,1", cid);
        break;
    case MM_BEARER_IP_FAMILY_IPV6:
        ctx->xdns_cmd = g_strdup_printf ("+XDNS=%u,2", cid);
        break;
    }

    ctx->cgact_cmd        = g_strdup_printf ("+CGACT=1,%u", cid);
    ctx->cgcontrdp_cmd    = g_strdup_printf ("+CGCONTRDP=%u", cid);
    ctx->xdatachannel_cmd = g_strdup_printf ("+XDATACHANNEL=1,1,\"/USBCDC/0\",\"/USBHS/NCM/%u\",2,%u", ncm_idx, cid);
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
    MMBroadbandBearerFibocomNcm *xmm_self = MM_BROADBAND_BEARER_FIBOCOM_NCM (self);

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
    MMBroadbandBearerFibocomNcm *xmm_self = MM_BROADBAND_BEARER_FIBOCOM_NCM (self);

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
mm_broadband_bearer_fibocom_ncm_new_finish (GAsyncResult *res,
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
mm_broadband_bearer_fibocom_ncm_new (MMBroadbandModemFibocomNcm *modem,
                                 MMBearerProperties      *properties,
                                 GCancellable            *cancellable,
                                 GAsyncReadyCallback      callback,
                                 gpointer                 user_data)
{
    g_async_initable_new_async (
        MM_TYPE_BROADBAND_BEARER_FIBOCOM_NCM,
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
mm_broadband_bearer_fibocom_ncm_init (MMBroadbandBearerFibocomNcm *self)
{
}

static void
dispose (GObject *object)
{
    MMBroadbandBearerFibocomNcm *self = MM_BROADBAND_BEARER_FIBOCOM_NCM (object);

    g_clear_object (&self->ipv4_config);
    g_clear_object (&self->ipv6_config);

    G_OBJECT_CLASS (mm_broadband_bearer_fibocom_ncm_parent_class)->dispose (object);
}

static void
mm_broadband_bearer_fibocom_ncm_class_init (MMBroadbandBearerFibocomNcmClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBaseBearerClass *base_bearer_class = MM_BASE_BEARER_CLASS (klass);
    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    object_class->dispose = dispose;

    base_bearer_class->load_connection_status = NULL;
    base_bearer_class->load_connection_status_finish = NULL;

    broadband_bearer_class->dial_3gpp = dial_3gpp;
    broadband_bearer_class->dial_3gpp_finish = dial_3gpp_finish;
    broadband_bearer_class->get_ip_config_3gpp = get_ip_config_3gpp;
    broadband_bearer_class->get_ip_config_3gpp_finish = get_ip_config_3gpp_finish;
    broadband_bearer_class->disconnect_3gpp = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;
}
