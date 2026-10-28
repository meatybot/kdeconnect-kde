/**
 * SPDX-FileCopyrightText: 2023 Albert Vaca <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#ifndef SSLHELPER_H
#define SSLHELPER_H

#include <QSslCertificate>
#include <QSslKey>
#include <QString>

#include "kdeconnectcore_export.h"

namespace SslHelper
{
KDECONNECTCORE_EXPORT QSslKey generateEcPrivateKey();
KDECONNECTCORE_EXPORT QSslKey generateRsaPrivateKey();
KDECONNECTCORE_EXPORT QSslCertificate generateSelfSignedCertificate(const QSslKey &privateKey, const QString &commonName);
}

#endif
