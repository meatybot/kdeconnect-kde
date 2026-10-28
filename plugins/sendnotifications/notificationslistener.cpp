/**
 * SPDX-FileCopyrightText: 2015 Holger Kaelberer <holger.k@elberer.de>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "notificationslistener.h"

#include <QBuffer>
#include <QImage>
#include <QStandardPaths>

#include <KConfig>
#include <KConfigGroup>

#include "notifyingapplication.h"
#include "plugin_sendnotifications_debug.h"
#include <core/kdeconnectplugin.h>
#include <core/kdeconnectpluginconfig.h>
#include <core/networkpacket.h>

NotificationsListener::NotificationsListener(KdeConnectPlugin *aPlugin)
    : QObject(aPlugin)
    , m_plugin(aPlugin)
{
    loadApplications();

    connect(m_plugin->config(), &KdeConnectPluginConfig::configChanged, this, &NotificationsListener::loadApplications);
}

NotificationsListener::~NotificationsListener()
{
    qCDebug(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Destroying NotificationsListener";
}

void NotificationsListener::handlePacket(const NetworkPacket &np)
{
    qCDebug(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Interacting with forwarded notifications is not supported on this platform, ignoring" << np.type();
}

void NotificationsListener::loadApplications()
{
    m_applications.clear();
    const QVariantList list = m_plugin->config()->getList(QStringLiteral("applications"));
    for (const auto &a : list) {
        NotifyingApplication app = a.value<NotifyingApplication>();
        if (!m_applications.contains(app.name))
            m_applications.insert(app.name, app);
    }
    // qCDebug(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Loaded" << m_applications.size() << " applications";
}

bool NotificationsListener::checkApplicationName(const QString &appName, const QString iconName)
{
    auto it = m_applications.constFind(appName);
    if (it == m_applications.cend()) {
        // new application -> add to config
        NotifyingApplication app;
        app.name = appName;
        app.icon = iconName;
        app.active = true;
        m_applications.insert(app.name, app);
        // update config:
        QVariantList list;
        for (const auto &a : std::as_const(m_applications)) {
            list << QVariant::fromValue<NotifyingApplication>(a);
        }
        m_plugin->config()->setList(QStringLiteral("applications"), list);

        return true;
    } else {
        return it->active;
    }
}

bool NotificationsListener::checkIsInBlacklist(const QString &appName, const QString &content)
{
    auto appIt = m_applications.constFind(appName);
    if (appIt == m_applications.cend()) {
        return false;
    }
    return appIt->blacklistExpression.isValid() && !appIt->blacklistExpression.pattern().isEmpty() && appIt->blacklistExpression.match(content).hasMatch();
}

QSharedPointer<QIODevice> NotificationsListener::iconFromQImage(const QImage &image) const
{
    const QByteArray png = pngFromQImage(image);
    if (png.isEmpty()) {
        return QSharedPointer<QIODevice>();
    }

    auto buffer = QSharedPointer<QBuffer>::create();
    buffer->setData(png);
    return buffer;
}

QByteArray NotificationsListener::pngFromQImage(const QImage &image)
{
    if (image.isNull()) {
        return QByteArray();
    }

    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        qCWarning(KDECONNECT_PLUGIN_SENDNOTIFICATIONS) << "Could not encode notification image";
        return QByteArray();
    }
    return png;
}

#include "moc_notificationslistener.cpp"
