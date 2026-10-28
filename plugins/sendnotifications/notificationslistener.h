/**
 * SPDX-FileCopyrightText: 2015 Holger Kaelberer <holger.k@elberer.de>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <optional>

#include <QHash>
#include <QIODevice>
#include <QSharedPointer>

class KdeConnectPlugin;
class NetworkPacket;
struct NotifyingApplication;

class NotificationsListener : public QObject
{
    Q_OBJECT

public:
    explicit NotificationsListener(KdeConnectPlugin *aPlugin);
    ~NotificationsListener() override;

    // Packets from the peer interacting with notifications we forwarded
    virtual void handlePacket(const NetworkPacket &np);

protected:
    bool checkApplicationName(const QString &appName, const QString iconName);
    bool checkIsInBlacklist(const QString &appName, const QString &content);
    QSharedPointer<QIODevice> iconFromQImage(const QImage &image) const;
    static QByteArray pngFromQImage(const QImage &image);

    KdeConnectPlugin *m_plugin;

private Q_SLOTS:
    void loadApplications();

private:
    QHash<QString, NotifyingApplication> m_applications;
};
