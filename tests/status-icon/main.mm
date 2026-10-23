#include <QCoreApplication>
#include <QImage>
#include <QResource>
#include <cstdio>
#include <cstdlib>

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include "platforms/macos/macosstatusicon.h"

@interface TestButton : NSObject
@property(retain) NSImage* image;
@property CGFloat alphaValue;
@property(retain) NSColor* contentTintColor;
@end
@implementation TestButton
@end

@interface TestStatusItem : NSObject
@property(retain) TestButton* button;
@property BOOL visible;
@property(retain) NSMenu* menu;
@end
@implementation TestStatusItem
@end

@interface TestStatusBar : NSObject
@property(retain) TestStatusItem* item;
@end
@implementation TestStatusBar
- (CGFloat)thickness { return 24; }
- (id)statusItemWithLength:(CGFloat)length { return self.item; }
- (void)removeStatusItem:(id)item {}
@end

static TestStatusBar* statusBar;
static id testStatusBar(id, SEL) { return statusBar; }

static void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static QImage pixels(NSImage* image)
{
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc]
        initWithCGImage:[image CGImageForProposedRect:nil context:nil hints:nil]];
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    QImage result = QImage::fromData(static_cast<const uchar*>(png.bytes), png.length);
    [bitmap release];
    require(!result.isNull(), "Native image has no pixels");
    return result;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    @autoreleasepool {
        statusBar = [TestStatusBar new];
        statusBar.item = [TestStatusItem new];
        statusBar.item.button = [TestButton new];
        method_setImplementation(class_getClassMethod([NSStatusBar class], @selector(systemStatusBar)),
                                 reinterpret_cast<IMP>(testStatusBar));
        MacOSStatusIcon icon(nullptr);
        TestButton* button = statusBar.item.button;
        icon.setIcon(":/images/tray/active.png", false, 1.0, QColor("#F5B700"));
        const QImage recovery = pixels(button.image);
        int opaquePixels = 0;
        bool transparent = false;
        for (int y = 0; y < recovery.height(); ++y) {
            for (int x = 0; x < recovery.width(); ++x) {
                const QColor pixel = recovery.pixelColor(x, y);
                transparent |= pixel.alpha() == 0;
                if (pixel.alpha() < 250) continue;
                ++opaquePixels;
                require(qAbs(pixel.red() - 245) <= 1 && qAbs(pixel.green() - 183) <= 1
                            && pixel.blue() <= 1, "Recovery pixels are not #F5B700");
            }
        }
        require(opaquePixels > 0 && transparent, "Recovery lost the icon or its transparency");
        require(!button.image.isTemplate, "Recovery must retain its explicit pixel colours");
        icon.setIcon(":/images/tray/error.png", false, 1.0, QColor());
        QResource errorResource(":/images/tray/error.png");
        NSImage* originalError = [[NSImage alloc] initWithData:errorResource.uncompressedData().toNSData()];
        require(!button.image.isTemplate && pixels(button.image) == pixels(originalError),
                "Error image colours changed");
        [originalError release];
        icon.setIcon(":/images/tray/active.png", true, 1.0, QColor());
        require(button.image.isTemplate && button.alphaValue == 1.0, "Connected appearance changed");
        icon.setIcon(":/images/tray/active.png", true, 0.45, QColor());
        require(button.image.isTemplate && button.alphaValue == 0.45, "Disconnected appearance changed");
        std::printf("PASS: %d yellow pixels; transparency, error, connected and disconnected preserved\n",
                    opaquePixels);
    }
}
