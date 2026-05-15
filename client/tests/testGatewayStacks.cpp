#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QSignalSpy>
#include <QTest>

#include "core/controllers/coreController.h"
#include "core/models/serverDescription.h"
#include "ui/controllers/serversUiController.h"
#include "ui/models/serversModel.h"
#include "vpnConnection.h"
#include "secureQSettings.h"

using namespace amnezia;

class TestGatewayStacks : public QObject
{
    Q_OBJECT

private:
    CoreController* m_coreController;
    SecureQSettings* m_settings;

    QString getValueFromIni(const QString &key)
    {
        QSettings settings("test_vars.ini", QSettings::IniFormat);
        return settings.value(key).toString();
    }

private slots:
    void initTestCase() {
        QString testOrg = "AmneziaVPN-Test-" + QUuid::createUuid().toString();
        m_settings = new SecureQSettings(testOrg, "amnezia-client", nullptr, false);
        
        auto vpnConnection = QSharedPointer<VpnConnection>::create(nullptr, nullptr);
        m_coreController = new CoreController(vpnConnection, m_settings, nullptr, this);
    }

    void cleanupTestCase() {
        m_settings->clearSettings();
        delete m_coreController;
        delete m_settings;
    }

    void init() {
        m_settings->clearSettings();
        m_coreController->m_serversRepository->invalidateCache();
        if (m_coreController->m_serversModel) {
            m_coreController->m_serversModel->updateModel(QVector<ServerDescription>(), -1);
        }
    }

    void testGatewayStacksRecomputeOnServerOperations() {
        QString awgKey = getValueFromIni("configs/TEST_CONFIG_AWG");

        QSignalSpy serverAddedSpy(m_coreController->m_serversRepository, &SecureServersRepository::serverAdded);
        QSignalSpy serverEditedSpy(m_coreController->m_serversRepository, &SecureServersRepository::serverEdited);
        QSignalSpy serverRemovedSpy(m_coreController->m_serversRepository, &SecureServersRepository::serverRemoved);
        QSignalSpy gatewayServersChangedSpy(m_coreController->m_serversUiController,
                                            &ServersUiController::hasServersFromGatewayApiChanged);

        auto importResult = m_coreController->m_importCoreController->extractConfigFromData(awgKey);
        m_coreController->m_importCoreController->importConfig(importResult.config);

        QVERIFY2(serverAddedSpy.count() == 1, "serverAdded signal should be emitted");
        QVERIFY2(!m_coreController->m_serversUiController->hasServersFromGatewayApi(),
                 "Self-hosted imports should not be treated as gateway API servers");

        const QString serverId = m_coreController->m_serversController->getServerId(0);
        QVERIFY2(m_coreController->m_serversController->renameServer(serverId, QStringLiteral("Edited Server")),
                 "Server rename should succeed");

        QVERIFY2(serverEditedSpy.count() == 1, "serverEdited signal should be emitted");

        m_coreController->m_serversController->removeServer(serverId);

        QVERIFY2(serverRemovedSpy.count() == 1, "serverRemoved signal should be emitted");
        QVERIFY2(!m_coreController->m_serversUiController->hasServersFromGatewayApi(),
                 "Gateway API server state should remain empty");
        QVERIFY2(gatewayServersChangedSpy.count() == 0,
                 "Self-hosted server operations should not emit gateway API state changes");
    }
};

QTEST_MAIN(TestGatewayStacks)
#include "testGatewayStacks.moc"

