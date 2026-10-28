/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "connecttargets.h"

bool ConnectTargets::add(const RelayProtocol::Target &target)
{
    if (!target.isValid()) {
        return false;
    }
    prune();
    m_entries.removeIf([&target](const Entry &e) {
        return e.target == target;
    });
    Entry entry{target, {}};
    entry.added.start();
    m_entries.append(entry);
    return true;
}

bool ConnectTargets::take(const QSslCertificate &certificate)
{
    prune();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (RelayProtocol::matches(it->target, certificate)) {
            m_entries.erase(it);
            return true;
        }
    }
    return false;
}

bool ConnectTargets::isEmpty()
{
    prune();
    return m_entries.isEmpty();
}

void ConnectTargets::prune()
{
    m_entries.removeIf([this](const Entry &e) {
        return e.added.elapsed() >= m_expiryMs;
    });
}
