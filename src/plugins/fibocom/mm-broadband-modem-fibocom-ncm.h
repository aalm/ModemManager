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

#ifndef MM_BROADBAND_MODEM_FIBOCOM_NCM_H
#define MM_BROADBAND_MODEM_FIBOCOM_NCM_H

#include "mm-broadband-modem.h"

#define MM_TYPE_BROADBAND_MODEM_FIBOCOM_NCM            (mm_broadband_modem_fibocom_ncm_get_type ())
#define MM_BROADBAND_MODEM_FIBOCOM_NCM(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_MODEM_FIBOCOM_NCM, MMBroadbandModemFibocomNcm))
#define MM_BROADBAND_MODEM_FIBOCOM_NCM_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_MODEM_FIBOCOM_NCM, MMBroadbandModemFibocomNcmClass))
#define MM_IS_BROADBAND_MODEM_FIBOCOM_NCM(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_MODEM_FIBOCOM_NCM))
#define MM_IS_BROADBAND_MODEM_FIBOCOM_NCM_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_MODEM_FIBOCOM_NCM))
#define MM_BROADBAND_MODEM_FIBOCOM_NCM_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_MODEM_FIBOCOM_NCM, MMBroadbandModemFibocomNcmClass))

typedef struct _MMBroadbandModemFibocomNcm MMBroadbandModemFibocomNcm;
typedef struct _MMBroadbandModemFibocomNcmClass MMBroadbandModemFibocomNcmClass;

struct _MMBroadbandModemFibocomNcm {
    MMBroadbandModem parent;
};

struct _MMBroadbandModemFibocomNcmClass{
    MMBroadbandModemClass parent;
};

GType mm_broadband_modem_fibocom_ncm_get_type (void);

MMBroadbandModemFibocomNcm *mm_broadband_modem_fibocom_ncm_new (const gchar  *device,
                                                                const gchar  *physdev,
                                                                const gchar **drivers,
                                                                const gchar  *plugin,
                                                                guint16       vendor_id,
                                                                guint16       product_id);

#endif /* MM_BROADBAND_MODEM_FIBOCOM_NCM_H */
