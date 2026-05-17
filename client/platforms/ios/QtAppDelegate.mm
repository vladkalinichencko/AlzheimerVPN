#import "QtAppDelegate.h"
#import "ios_controller.h"

#include <QFile>


@implementation QIOSApplicationDelegate (AmneziaVPNDelegate)
#if !MACOS_NE
- (BOOL)application:(UIApplication *)app
            openURL:(NSURL *)url
            options:(NSDictionary<UIApplicationOpenURLOptionsKey, id> *)options {
    if (url.fileURL) {
        QString filePath(url.path.UTF8String);
        if (filePath.isEmpty()) return NO;

        dispatch_async(dispatch_get_main_queue(), ^{
            NSLog(@"Application openURL: %@", url);

            if (filePath.contains("backup")) {
                IosController::Instance()->importBackupFromOutside(filePath);
            } else {
                QFile file(filePath);
                bool isOpenFile = file.open(QIODevice::ReadOnly);
                QByteArray data = file.readAll();

                IosController::Instance()->importConfigFromOutside(QString(data));
            }
        });

        return YES;
    }
    return NO;
}
#endif
@end
