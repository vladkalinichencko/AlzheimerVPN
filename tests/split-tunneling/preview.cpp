#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QTimer>
#include <QTranslator>

class PreviewLanguage : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE int getLineHeightAppend() const { return 0; }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    if (argc != 4) {
        qCritical("Usage: split_preview REPO_PATH OUTPUT.png RUSSIAN.qm");
        return 1;
    }
    QTranslator translator;
    if (!translator.load(argv[3])) {
        return 1;
    }
    app.installTranslator(&translator);
    PreviewLanguage language;
    QQmlApplicationEngine engine;
    engine.addImportPath(QString::fromLocal8Bit(argv[1]) + "/client/ui/qml/Modules");
    engine.rootContext()->setContextProperty("LanguageUiController", &language);
    engine.load(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1]) + "/tests/split-tunneling/preview.qml"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QTimer::singleShot(500, &app, [&] {
        app.exit(window && window->grabWindow().save(QString::fromLocal8Bit(argv[2])) ? 0 : 1);
    });
    return app.exec();
}

#include "preview.moc"
