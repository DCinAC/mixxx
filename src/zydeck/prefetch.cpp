#include "zydeck/prefetch.h"

#include <QFile>
#include <QStorageInfo>
#include <QThreadPool>

#include "mixer/playerinfo.h"
#include "track/track.h"

namespace zydeck {

namespace {
bool onNetwork(const QString& path) {
    const QByteArray fs = QStorageInfo(path).fileSystemType().toLower();
    return fs.contains("smb") || fs.contains("cifs") || fs.contains("nfs") || fs.contains("afp") || fs.contains("webdav") ||
            path.startsWith(QLatin1String("//")) || path.startsWith(QLatin1String("\\\\"));
}
} // namespace

void prefetchNetworkTracks() {
    QObject::connect(&PlayerInfo::instance(), &PlayerInfo::trackChanged, &PlayerInfo::instance(),
            [](const QString&, TrackPointer pNew, TrackPointer) {
                if (!pNew) {
                    return;
                }
                const QString path = pNew->getLocation();
                QThreadPool::globalInstance()->start([path] {
                    if (!onNetwork(path)) {
                        return;
                    }
                    QFile f(path);
                    if (!f.open(QIODevice::ReadOnly)) {
                        return;
                    }
                    QByteArray block(1 << 20, Qt::Uninitialized);   // 1 MB at a time, read and let go
                    while (f.read(block.data(), block.size()) > 0) {
                    }
                });
            });
}

} // namespace zydeck
