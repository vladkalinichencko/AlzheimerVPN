#import <UIKit/UIKit.h>
#include <dispatch/dispatch.h>

#include <QByteArray>
#include <QFile>
#include <QString>

#include "ios_controller.h"

static void amnezia_handleURL(NSURL *url)
{
    if (!url || !url.isFileURL) {
        return;
    }

    QString filePath(url.path.UTF8String);
    if (filePath.isEmpty()) {
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        if (filePath.contains("backup")) {
            IosController::Instance()->importBackupFromOutside(filePath);
            return;
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            return;
        }

        const QByteArray data = file.readAll();
        IosController::Instance()->importConfigFromOutside(QString::fromUtf8(data));
    });
}

@interface QIOSWindowSceneDelegate : UIResponder <UIWindowSceneDelegate>
- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)URLContexts API_AVAILABLE(ios(13.0));
@end

@interface AmneziaSceneDelegate : QIOSWindowSceneDelegate
@end

@implementation AmneziaSceneDelegate

- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)contexts
{
    if (@available(iOS 13.0, *)) {
        if ([QIOSWindowSceneDelegate instancesRespondToSelector:_cmd]) {
            [super scene:scene openURLContexts:contexts];
        }

        if (!contexts || contexts.count == 0) {
            return;
        }

        for (UIOpenURLContext *context in contexts) {
            amnezia_handleURL(context.URL);
        }
    }
}

@end
