// ZyDeck Connect: a phone or tablet running ZyDeck in controller mode, as a MIDI controller for Mixxx on this
// computer (or any DJ app, with MIDI Learn), over the USB cable or Wi-Fi. What ZyDeck Link (Python) did, in C++:
//   zydeck-connect                     the window, and an icon in the menu bar / tray
//   zydeck-connect --no-window         without them (Ctrl+C stops it)
//   zydeck-connect --install-mapping   install the ZyDeck mapping in Mixxx and quit
//   zydeck-connect --write-mapping DIR write the mapping's files into DIR and quit

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QThreadPool>
#include <QTimer>
#include <cstdio>
#include <memory>

#include "core.h"
#include "system.h"
#include "window.h"

int main(int argc, char** argv) {
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray a = argv[i];
        headless = headless || a == "--no-window" || a == "--install-mapping" || a.startsWith("--write-mapping");
    }
    // no window: no GUI either (it runs where there's no display)
    std::unique_ptr<QCoreApplication> app(headless ? new QCoreApplication(argc, argv) : new QApplication(argc, argv));
    QCoreApplication::setApplicationName(QString::fromLatin1(zc::kAppName));
    QCoreApplication::setOrganizationName(QStringLiteral("ZyDeck"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser args;
    args.setApplicationDescription(QStringLiteral("A phone or tablet running ZyDeck as a MIDI controller for Mixxx here"));
    args.addHelpOption();
    args.addVersionOption();
    args.addOption({QStringLiteral("no-window"), QStringLiteral("Run without the window (Ctrl+C stops it).")});
    args.addOption({QStringLiteral("install-mapping"), QStringLiteral("Install the ZyDeck mapping in Mixxx and quit.")});
    args.addOption({QStringLiteral("write-mapping"), QStringLiteral("Write the mapping's files into DIR and quit."), QStringLiteral("DIR")});
    QCommandLineOption shot(QStringLiteral("screenshot"), QStringLiteral("Draw the window into FILE (a PNG) and quit (for checking its look)."), QStringLiteral("FILE"));
    shot.setFlags(QCommandLineOption::HiddenFromHelp);
    args.addOption(shot);
    QCommandLineOption frames(QStringLiteral("frames"), QStringLiteral("Draw the window every 25 ms for 1.2 s into DIR (the tag coming out) and quit."), QStringLiteral("DIR"));
    frames.setFlags(QCommandLineOption::HiddenFromHelp);
    args.addOption(frames);
    args.process(*app);

    if (args.isSet(QStringLiteral("write-mapping"))) {
        const QString dir = args.value(QStringLiteral("write-mapping"));
        QDir().mkpath(dir);
        const auto files = zc::mappingFiles();
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QFile f(dir + '/' + it.key());
            if (!f.open(QIODevice::WriteOnly) || f.write(it.value()) != it.value().size()) {
                std::fprintf(stderr, "couldn't write %s\n", qPrintable(f.fileName()));
                return 1;
            }
        }
        return 0;
    }
    if (args.isSet(QStringLiteral("install-mapping"))) {
        try {
            std::printf("%s\n", qPrintable(zc::installMapping()));
            return 0;
        } catch (const QString& why) {
            std::fprintf(stderr, "%s\n", qPrintable(why));
            return 1;
        }
    }

    zc::Core core;
    if (!core.listen()) {
        const QString why = QObject::tr("Port %1 is taken: ZyDeck Connect (or the old ZyDeck Link) is already running.").arg(zc::kTcpPort);
        if (headless) {
            std::fprintf(stderr, "%s\n", qPrintable(why));
        } else {
            QMessageBox::information(nullptr, QString::fromLatin1(zc::kAppName), why);
        }
        return 1;
    }
    std::unique_ptr<zc::Window> window;
    if (headless) {
        std::printf("%s: MIDI port '%s', %s port %d\n", zc::kAppName, zc::kPortName, qPrintable(zc::lanAddresses().join(", ")), zc::kTcpPort);
        std::fflush(stdout);
    } else {
        static_cast<QApplication*>(app.get())->setQuitOnLastWindowClosed(false);   // (the menu bar icon stays)
        window = std::make_unique<zc::Window>(&core);
        window->show();
        if (args.isSet(frames)) {
            QDir().mkpath(args.value(frames));
            auto* timer = new QTimer(window.get());
            auto* n = new int(0);
            QObject::connect(timer, &QTimer::timeout, window.get(), [&window, n, dir = args.value(frames)] {
                window->grab().save(QStringLiteral("%1/%2.png").arg(dir).arg(*n, 3, 10, QLatin1Char('0')));
                if (++*n >= 48) {
                    QCoreApplication::quit();
                }
            });
            timer->start(25);
        }
        if (args.isSet(shot)) {   // (after the tag has had time to come out, or a device to link)
            QTimer::singleShot(2500, window.get(), [&window, file = args.value(shot)] {
                window->grab().save(file);
                QCoreApplication::quit();
            });
        }
    }
    const int r = app->exec();
    QThreadPool::globalInstance()->waitForDone(3000);
    return r;
}
