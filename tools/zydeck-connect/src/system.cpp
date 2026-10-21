#include "system.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>

#include "library.h"

namespace zc {

QMap<QString, QByteArray> mappingFiles() {
    const auto read = [](const QString& path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    QString xml = QString::fromUtf8(read(QStringLiteral(":/mapping/Zydek-Tablet.midi.xml")));
    const auto first = [&xml](const QString& pattern, const QString& with) {
        const QRegularExpressionMatch m = QRegularExpression(pattern, QRegularExpression::DotMatchesEverythingOption).match(xml);
        if (m.hasMatch()) {
            xml.replace(m.capturedStart(), m.capturedLength(), with);
        }
    };
    first(QStringLiteral("<name>.*?</name>"), QStringLiteral("<name>ZyDeck</name>"));
    first(QStringLiteral("<description>.*?</description>"),
            QStringLiteral("<description>A phone or tablet running ZyDeck in controller mode: over the USB cable (choose MIDI "
                           "in Android's USB notification) or over Wi-Fi through ZyDeck Connect.</description>"));
    xml.replace(QStringLiteral("filename=\"Zydek-Tablet.script.js\""), QStringLiteral("filename=\"ZyDeck.script.js\""));
    return {{QStringLiteral("ZyDeck.midi.xml"), xml.toUtf8()},
            {QStringLiteral("ZyDeck.script.js"), read(QStringLiteral(":/mapping/Zydek-Tablet.script.js"))}};
}

bool mappingInstalled() {
    const QString d = mixxxDir();
    return !d.isEmpty() && QFileInfo(d + "/controllers/ZyDeck.midi.xml").isFile();
}

bool mappingCurrent() {
    const QString d = mixxxDir();
    if (d.isEmpty()) {
        return false;
    }
    const QMap<QString, QByteArray> files = mappingFiles();
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QFile f(d + "/controllers/" + it.key());
        if (!f.open(QIODevice::ReadOnly) || f.readAll() != it.value()) {
            return false;
        }
    }
    return true;
}

// ---- Mixxx's settings: "[Group]" lines, then "key value" lines (ConfigObject's format) ----------------------------
namespace {
// Mixxx's name for the port in its settings: the device's name with spaces (and slashes) made underscores
const QString kDeviceKey = QStringLiteral("ZyDeck_Connect");

QString cfgPath() {
    const QString d = mixxxDir();
    return d.isEmpty() ? QString() : d + "/mixxx.cfg";
}

/// key's value in group, or a null string
QString cfgValue(const QStringList& lines, const QString& group, const QString& key) {
    bool in = false;
    for (const QString& l : lines) {
        if (l.startsWith('[')) {
            in = l.trimmed() == group;
        } else if (in && (l == key || l.startsWith(key + ' '))) {
            return l.mid(key.size()).trimmed();
        }
    }
    return {};
}

/// sets key in group (adding the line, or the group, if it isn't there)
void cfgSet(QStringList& lines, const QString& group, const QString& key, const QString& value) {
    const QString line = key + ' ' + value;
    int header = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].startsWith('[')) {
            if (header >= 0) {   // the end of the group: add it there (before the blank line that closes it)
                int at = i;
                while (at > header + 1 && lines[at - 1].trimmed().isEmpty()) {
                    --at;
                }
                lines.insert(at, line);
                return;
            }
            header = lines[i].trimmed() == group ? i : -1;
        } else if (header >= 0 && (lines[i] == key || lines[i].startsWith(key + ' '))) {
            lines[i] = line;
            return;
        }
    }
    if (header >= 0) {
        lines.append(line);
        return;
    }
    if (!lines.isEmpty() && !lines.last().isEmpty()) {
        lines.append(QString());
    }
    lines << group << line << QString();
}

QStringList cfgLines() {
    QFile f(cfgPath());
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}
} // namespace

bool controllerOn() {
    const QStringList lines = cfgLines();
    return cfgValue(lines, QStringLiteral("[Controller]"), kDeviceKey) == QLatin1String("1") &&
            cfgValue(lines, QStringLiteral("[ControllerPreset]"), kDeviceKey).endsWith(QLatin1String("ZyDeck.midi.xml"));
}

void turnControllerOn() {
    const QString path = cfgPath();
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        throw QObject::tr("Mixxx's settings aren't there yet: start Mixxx once");
    }
    const QString backup = path + ".before-zydeck";
    if (!QFileInfo::exists(backup) && !QFile::copy(path, backup)) {
        throw QObject::tr("couldn't back up %1").arg(path);
    }
    QStringList lines = cfgLines();
    cfgSet(lines, QStringLiteral("[ControllerPreset]"), kDeviceKey, mixxxDir() + "/controllers/ZyDeck.midi.xml");
    cfgSet(lines, QStringLiteral("[Controller]"), kDeviceKey, QStringLiteral("1"));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write((lines.join('\n') + '\n').toUtf8()) < 0 || !f.commit()) {
        throw QObject::tr("couldn't write %1").arg(path);
    }
}

QString installMapping() {
    QString d = mixxxDir();
    if (d.isEmpty()) {
        d = mixxxDirCandidates().first();
    }
    const QString target = d + "/controllers";
    if (!QDir().mkpath(target)) {
        throw QObject::tr("couldn't make %1").arg(target);
    }
    const QMap<QString, QByteArray> files = mappingFiles();
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QSaveFile f(target + '/' + it.key());
        if (!f.open(QIODevice::WriteOnly) || f.write(it.value()) != it.value().size() || !f.commit()) {
            throw QObject::tr("couldn't write %1").arg(f.fileName());
        }
    }
    return target;
}

// ---- open at login --------------------------------------------------------------------------------------
namespace {
/// How to start this again: the .app on macOS (so it's the bundle that opens), the program elsewhere.
QStringList launchCommand() {
    const QString exe = QCoreApplication::applicationFilePath();
#ifdef Q_OS_MACOS
    const int app = exe.indexOf(QLatin1String(".app/"));
    if (app >= 0) {
        return {QStringLiteral("/usr/bin/open"), QStringLiteral("-a"), exe.left(app + 4)};
    }
#endif
    return {exe};
}

QString loginFile() {
    const QString home = QDir::homePath();
#if defined(Q_OS_MACOS)
    return home + "/Library/LaunchAgents/org.zydeck.connect.plist";
#elif defined(Q_OS_WIN)
    return qEnvironmentVariable("APPDATA", home) + "/Microsoft/Windows/Start Menu/Programs/Startup/ZyDeck Connect.cmd";
#else
    return qEnvironmentVariable("XDG_CONFIG_HOME", home + "/.config") + "/autostart/zydeck-connect.desktop";
#endif
}

QString escapeXml(QString s) {
    return s.replace('&', QLatin1String("&amp;")).replace('<', QLatin1String("&lt;")).replace('>', QLatin1String("&gt;"));
}
} // namespace

bool openAtLogin() {
    return QFileInfo(loginFile()).isFile();
}

void setOpenAtLogin(bool on) {
    const QString path = loginFile();
    if (!on) {
        QFile::remove(path);
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QStringList cmd = launchCommand();
    QString text;
#if defined(Q_OS_MACOS)
    QString args;
    for (const QString& a : cmd) {
        args += "<string>" + escapeXml(a) + "</string>";
    }
    text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>"
           "<key>Label</key><string>org.zydeck.connect</string><key>ProgramArguments</key><array>" + args +
           "</array><key>RunAtLoad</key><true/></dict></plist>\n";
#elif defined(Q_OS_WIN)
    QStringList quoted;
    for (const QString& a : cmd) {
        quoted << '"' + QDir::toNativeSeparators(a) + '"';
    }
    text = "@start \"\" " + quoted.join(' ') + "\r\n";
#else
    QStringList quoted;
    for (const QString& a : cmd) {
        quoted << '"' + a + '"';
    }
    text = "[Desktop Entry]\nType=Application\nName=ZyDeck Connect\nExec=" + quoted.join(' ') +
           "\nX-GNOME-Autostart-enabled=true\n";
#endif
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(text.toUtf8());
        f.commit();
    }
}

} // namespace zc
