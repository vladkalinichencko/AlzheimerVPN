/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SYSTEMTRAYNOTIFICATIONHANDLER_H
#define SYSTEMTRAYNOTIFICATIONHANDLER_H

#include "notificationHandler.h"

#include "core/utils/connectionHealth.h"

#include <QColor>
#include <QMenu>
#include <QSystemTrayIcon>

class SystemTrayNotificationHandler : public NotificationHandler {
    Q_OBJECT

public:
    explicit SystemTrayNotificationHandler(QObject* parent);
    ~SystemTrayNotificationHandler();

    void setConnectionState(Vpn::ConnectionState state) override;

    void onTranslationsUpdated() override;

public slots:
    void setConnectionHealth(ConnectionHealth health);
    void updateWebsiteUrl(const QString &newWebsiteUrl);

protected:
    virtual void notify(Message type, const QString& title,
                        const QString& message, int timerMsec) override;

private:
    void showHideWindow();

    void setTrayState(Vpn::ConnectionState state);
    void updateTrayIcon();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

    void setTrayIcon(const QString &iconPath, bool useNativeMask,
                     qreal opacity = 1.0, const QColor &tint = QColor());

private:
    QMenu m_menu;
    QSystemTrayIcon m_systemTrayIcon;

    QAction* m_trayActionShow = nullptr;
    QAction* m_trayActionConnect = nullptr;
    QAction* m_trayActionDisconnect = nullptr;
    QAction* m_trayActionVisitWebSite = nullptr;
    QAction* m_trayActionQuit = nullptr;
    QAction* m_statusLabel = nullptr;    
    QAction* m_separator = nullptr;

    const QString ConnectedTrayIconName = "active.png";
    const QString ErrorTrayIconName = "error.png";
    Vpn::ConnectionState m_connectionState = Vpn::ConnectionState::Disconnected;
    ConnectionHealth m_connectionHealth = ConnectionHealth::Idle;
    QString  websiteUrl = "https://amnezia.org";
};

#endif  // SYSTEMTRAYNOTIFICATIONHANDLER_H
