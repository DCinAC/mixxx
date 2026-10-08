#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>

#include "preferences/usersettings.h"

namespace zydek {

/// What the phone's library page needs from Mixxx: browsing and searching the library, loading a track
/// into a deck or sampler by file path, previewing it, and managing the music folders.
///
/// Reads go through its own read-only connection to Mixxx's database (it lives on the controller thread);
/// actions are handed to Mixxx's objects on the main thread.
class Library {
  public:
    explicit Library(UserSettingsPointer pConfig);
    ~Library();

    QJsonObject views();
    /// view: all | stems | crate:<id> | playlist:<id>. Every word of q must appear in artist/title/album/genre.
    QJsonArray tracks(const QString& q, const QString& view, const QString& sort, int limit, int offset);
    QJsonObject track(int trackId);
    QString location(int trackId);

    /// target: deck1..deck4, sampler (first empty), samplerN. Returns {ok, group} or {error}.
    QJsonObject load(int trackId, const QString& target);
    QJsonObject loadLocation(const QString& location, const QString& target);
    QJsonObject preview(int trackId);
    void stopPreview();
    QJsonObject decks() const;

    QJsonArray folders();
    QJsonObject addFolder(const QString& path);
    // crates (changes go through Mixxx, so its own views update too)
    QJsonObject createCrate(const QString& name);
    QJsonObject setCrateTrack(int crateId, int trackId, bool member);
    QJsonArray cratesOf(int trackId);
    QJsonObject listDirectory(const QString& path) const;
    void startScan();
    bool scanning() const;

  private:
    bool open();
    QJsonObject trackJson(const class QSqlQuery& query) const;

    UserSettingsPointer m_pConfig;
    QSqlDatabase m_db;
};

} // namespace zydek
