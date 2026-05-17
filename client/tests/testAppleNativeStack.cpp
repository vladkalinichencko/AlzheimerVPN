#include <QFile>
#include <QTest>

class AppleNativeStackTest : public QObject
{
    Q_OBJECT

private slots:
    void appleArchitecturesAreConfigurable();
    void macosProcessLayerUsesAppKitActivationPolicy();
    void macosRuntimePatchesAreRemoved();
    void macosNetworkWatcherDoesNotParseWdutil();
    void iosSceneDelegateUsesSubclass();
    void iosLifecycleDoesNotUseBackgroundFetch();
    void iosAppearanceFollowsSystem();

private:
    QString readSource(const QString& relativePath) const;
};

QString AppleNativeStackTest::readSource(const QString& relativePath) const
{
    QFile file(QStringLiteral(AMNEZIA_SOURCE_ROOT) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QTest::qFail(qPrintable(relativePath), __FILE__, __LINE__);
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void AppleNativeStackTest::appleArchitecturesAreConfigurable()
{
    const QString platformSettings = readSource(QStringLiteral("cmake/platform_settings.cmake"));
    QVERIFY(platformSettings.contains(QStringLiteral("AMNEZIA_APPLE_ARCHITECTURES")));
    QVERIFY(!platformSettings.contains(QStringLiteral("set(CMAKE_OSX_ARCHITECTURES \"x86_64\" CACHE STRING \"\" FORCE)")));

    const QString deployBuild = readSource(QStringLiteral("deploy/build.sh"));
    QVERIFY(deployBuild.contains(QStringLiteral("--apple-arch")));
    QVERIFY(deployBuild.contains(QStringLiteral("-DAMNEZIA_APPLE_ARCHITECTURES=")));
}

void AppleNativeStackTest::macosProcessLayerUsesAppKitActivationPolicy()
{
    const QString macosUtil = readSource(QStringLiteral("client/ui/utils/macosUtil.mm"));
    QVERIFY(macosUtil.contains(QStringLiteral("NSApplicationActivationPolicyRegular")));
    QVERIFY(macosUtil.contains(QStringLiteral("NSApplicationActivationPolicyAccessory")));
    QVERIFY(!macosUtil.contains(QStringLiteral("ProcessSerialNumber")));
    QVERIFY(!macosUtil.contains(QStringLiteral("TransformProcessType")));
    QVERIFY(!macosUtil.contains(QStringLiteral("GetFrontProcess")));
    QVERIFY(!macosUtil.contains(QStringLiteral("ShowHideProcess")));
}

void AppleNativeStackTest::macosRuntimePatchesAreRemoved()
{
    const QString macosUtils = readSource(QStringLiteral("client/platforms/macos/macosutils.mm"));
    QVERIFY(macosUtils.contains(QStringLiteral("SMAppService")));
    QVERIFY(!macosUtils.contains(QStringLiteral("SMLoginItemSetEnabled")));
    QVERIFY(!macosUtils.contains(QStringLiteral("class_replaceMethod")));
    QVERIFY(!macosUtils.contains(QStringLiteral("class_addMethod")));
    QVERIFY(!macosUtils.contains(QStringLiteral("method_exchangeImplementations")));
    QVERIFY(!macosUtils.contains(QStringLiteral("NSStatusBarButton (Swizzle)")));
}

void AppleNativeStackTest::macosNetworkWatcherDoesNotParseWdutil()
{
    const QString watcher = readSource(QStringLiteral("client/platforms/macos/macosnetworkwatcher.mm"));
    QVERIFY(watcher.contains(QStringLiteral("CWWiFiClient.sharedWiFiClient")));
    QVERIFY(!watcher.contains(QStringLiteral("wdutil")));
    QVERIFY(!watcher.contains(QStringLiteral("QProcess")));
}

void AppleNativeStackTest::iosSceneDelegateUsesSubclass()
{
    const QString hook = readSource(QStringLiteral("client/platforms/ios/AmneziaSceneDelegateHooks.mm"));
    QVERIFY(hook.contains(QStringLiteral("@interface AmneziaSceneDelegate : QIOSWindowSceneDelegate")));
    QVERIFY(!hook.contains(QStringLiteral("method_setImplementation")));
    QVERIFY(!hook.contains(QStringLiteral("objc_getClass")));

    const QString plist = readSource(QStringLiteral("client/ios/app/Info.plist.in"));
    QVERIFY(plist.contains(QStringLiteral("<string>AmneziaSceneDelegate</string>")));
    QVERIFY(!plist.contains(QStringLiteral("<string>QIOSWindowSceneDelegate</string>")));
}

void AppleNativeStackTest::iosLifecycleDoesNotUseBackgroundFetch()
{
    const QString appDelegate = readSource(QStringLiteral("client/platforms/ios/QtAppDelegate.mm"));
    QVERIFY(!appDelegate.contains(QStringLiteral("setMinimumBackgroundFetchInterval")));
    QVERIFY(!appDelegate.contains(QStringLiteral("performFetchWithCompletionHandler")));
    QVERIFY(!appDelegate.contains(QStringLiteral("UIApplicationBackgroundFetchIntervalMinimum")));
}

void AppleNativeStackTest::iosAppearanceFollowsSystem()
{
    const QString plist = readSource(QStringLiteral("client/ios/app/Info.plist.in"));
    QVERIFY(!plist.contains(QStringLiteral("UIUserInterfaceStyle")));
    QVERIFY(plist.contains(QStringLiteral("UIApplicationSupportsMultipleScenes")));
    QVERIFY(plist.contains(QStringLiteral("UIRequiresFullScreen")));
    QVERIFY(plist.contains(QStringLiteral("<false/>")));
}

QTEST_MAIN(AppleNativeStackTest)
#include "testAppleNativeStack.moc"
