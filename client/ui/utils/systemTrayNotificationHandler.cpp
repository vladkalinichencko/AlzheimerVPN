/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <QDebug>
#include "systemTrayNotificationHandler.h"


#ifdef Q_OS_MAC
#  include "platforms/macos/macosstatusicon.h"
#endif

#include <QApplication>
#include <QDesktopServices>
#include <QIcon>
#include <QPainter>
#include <QWindow>

#include "version.h"

SystemTrayNotificationHandler::SystemTrayNotificationHandler(QObject* parent) :
    NotificationHandler(parent)
#ifndef Q_OS_MAC
    , m_systemTrayIcon(parent)
#endif
{
    m_trayActionShow =  m_menu.addAction(QIcon(":/images/tray/application.png"), tr("Show") + " " + APPLICATION_NAME, this, [this](){
        emit raiseRequested();
    });
    m_menu.addSeparator();
    m_trayActionConnect = m_menu.addAction(tr("Connect"), this, [this](){ emit connectRequested(); });
    m_trayActionDisconnect = m_menu.addAction(tr("Disconnect"), this, [this](){ emit disconnectRequested(); });

    m_menu.addSeparator();

    m_trayActionVisitWebSite = m_menu.addAction(QIcon(":/images/tray/link.png"), tr("Visit Website"), [&](){
        QDesktopServices::openUrl(QUrl(websiteUrl));
    });

    // Quit action: disconnect VPN first on macOS NE, else quit directly
    m_trayActionQuit = m_menu.addAction(QIcon(":/images/tray/cancel.png"),
                                       tr("Quit") + " " + APPLICATION_NAME,
                                       this,
                                       [&](){ qApp->quit(); });

#ifdef Q_OS_MAC
    // QSystemTrayIcon::setContextMenu crashes on macOS 14+: its menu-tracking
    // observer reads -[NSEvent clickCount] off a non-mouse event. Own the
    // NSStatusItem and attach the native NSMenu instead.
    m_statusIcon = new MacOSStatusIcon(this);
    m_statusIcon->setMenu(&m_menu);
#else
    m_systemTrayIcon.show();
    connect(&m_systemTrayIcon, &QSystemTrayIcon::activated, this,
            &SystemTrayNotificationHandler::onTrayActivated);
    m_systemTrayIcon.setContextMenu(&m_menu);
#endif
    setTrayState(Vpn::ConnectionState::Disconnected);
}

SystemTrayNotificationHandler::~SystemTrayNotificationHandler() {
#ifdef Q_OS_MAC
    delete m_statusIcon;  // before m_menu: the status item references its NSMenu
    m_statusIcon = nullptr;
#endif
}

void SystemTrayNotificationHandler::setConnectionState(Vpn::ConnectionState state)
{
    setTrayState(state);
    NotificationHandler::setConnectionState(state);
}

void SystemTrayNotificationHandler::setConnectionHealth(ConnectionHealth health)
{
    m_connectionHealth = health;
    updateTrayIcon();
}

void SystemTrayNotificationHandler::onTranslationsUpdated()
{
    m_trayActionShow->setText(tr("Show") + " " + APPLICATION_NAME);
    m_trayActionConnect->setText(tr("Connect"));
    m_trayActionDisconnect->setText(tr("Disconnect"));
    m_trayActionVisitWebSite->setText(tr("Visit Website"));
    m_trayActionQuit->setText(tr("Quit")+ " " + APPLICATION_NAME);
}

void SystemTrayNotificationHandler::updateWebsiteUrl(const QString &newWebsiteUrl) {
    qDebug() << "Updated website URL:" << newWebsiteUrl;
    websiteUrl = newWebsiteUrl;
}

void SystemTrayNotificationHandler::setTrayIcon(const QString &iconPath, bool useNativeMask,
                                                qreal opacity, const QColor &tint)
{
#ifdef Q_OS_MAC
    m_statusIcon->setIcon(iconPath, useNativeMask, opacity, tint);
#else
    QPixmap styledPixmap = QPixmap(iconPath).scaled(128, 128, Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation);
    if (tint.isValid()) {
        QPainter tintPainter(&styledPixmap);
        tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tintPainter.fillRect(styledPixmap.rect(), tint);
    }
    if (opacity < 1.0) {
        QPixmap translucentPixmap(styledPixmap.size());
        translucentPixmap.fill(Qt::transparent);
        QPainter opacityPainter(&translucentPixmap);
        opacityPainter.setOpacity(opacity);
        opacityPainter.drawPixmap(0, 0, styledPixmap);
        styledPixmap = translucentPixmap;
    }

    QIcon trayIconMask(styledPixmap);
    trayIconMask.setIsMask(useNativeMask);
    m_systemTrayIcon.setIcon(trayIconMask);
#endif
}

void SystemTrayNotificationHandler::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if(reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
        emit raiseRequested();
    }
}

void SystemTrayNotificationHandler::setTrayState(Vpn::ConnectionState state)
{
    m_connectionState = state;

    switch (state) {
    case Vpn::ConnectionState::Disconnected:
        m_trayActionConnect->setEnabled(true);
        m_trayActionDisconnect->setEnabled(false);
        break;
    case Vpn::ConnectionState::Preparing:
        m_trayActionConnect->setEnabled(false);
        m_trayActionDisconnect->setEnabled(true);
        break;
    case Vpn::ConnectionState::Connecting:
        m_trayActionConnect->setEnabled(false);
        m_trayActionDisconnect->setEnabled(true);
        break;
    case Vpn::ConnectionState::Connected:
        m_trayActionConnect->setEnabled(false);
        m_trayActionDisconnect->setEnabled(true);
        break;
    case Vpn::ConnectionState::Disconnecting:
        m_trayActionConnect->setEnabled(false);
        m_trayActionDisconnect->setEnabled(true);
        break;
    case Vpn::ConnectionState::Reconnecting:
        m_trayActionConnect->setEnabled(false);
        m_trayActionDisconnect->setEnabled(true);
        break;
    case Vpn::ConnectionState::Error:
        m_trayActionConnect->setEnabled(true);
        m_trayActionDisconnect->setEnabled(false);
        break;
    case Vpn::ConnectionState::Unknown:
    default:
        m_trayActionConnect->setEnabled(false);
        m_trayActionDisconnect->setEnabled(true);
    }

    updateTrayIcon();

    //#ifdef Q_OS_MAC
    //    // Get theme from current user (note, this app can be launched as root application and in this case this theme can be different from theme of real current user )
    //    bool darkTaskBar = MacOSFunctions::instance().isMenuBarUseDarkTheme();
    //    darkTaskBar = forceUseBrightIcons ? true : darkTaskBar;
    //    resourcesPath = ":/images_mac/tray_icon/%1";
    //    useIconName = useIconName.replace(".png", darkTaskBar ? "@2x.png" : " dark@2x.png");
    //#endif
}

void SystemTrayNotificationHandler::updateTrayIcon()
{
    QString resourcesPath = ":/images/tray/%1";
    QString iconName = ConnectedTrayIconName;
    bool useNativeMask = true;
    qreal opacity = 0.45;
    QColor tint;

    if (m_connectionState == Vpn::ConnectionState::Error || connectionHealthProblem(m_connectionHealth)) {
        iconName = ErrorTrayIconName;
        useNativeMask = false;
        opacity = 1.0;
    } else if (m_connectionState == Vpn::ConnectionState::Reconnecting ||
               m_connectionHealth == ConnectionHealth::Recovering) {
        useNativeMask = false;
        opacity = 1.0;
        tint = QColor(QStringLiteral("#F5B700"));
    } else if (m_connectionState == Vpn::ConnectionState::Connected) {
        opacity = 1.0;
    }

    setTrayIcon(QString(resourcesPath).arg(iconName), useNativeMask, opacity, tint);
}


void SystemTrayNotificationHandler::notify(NotificationHandler::Message type,
                                           const QString& title,
                                           const QString& message,
                                           int timerMsec) {
  Q_UNUSED(type);

#ifdef Q_OS_MAC
  Q_UNUSED(timerMsec);
  m_statusIcon->showMessage(title, message);
#else
  QIcon icon(ConnectedTrayIconName);
  m_systemTrayIcon.showMessage(title, message, icon, timerMsec);
#endif
}

void SystemTrayNotificationHandler::showHideWindow() {
//  QmlEngineHolder* engine = QmlEngineHolder::instance();
//  if (engine->window()->isVisible()) {
//    engine->hideWindow();
//#ifdef MVPN_MACOS
//    MacOSUtils::hideDockIcon();
//#endif
//  } else {
//    engine->showWindow();
//#ifdef MVPN_MACOS
//    MacOSUtils::showDockIcon();
//#endif
//  }
}
