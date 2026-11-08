#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>

#include "preferences/usersettings.h"
#include "rendergraph/geometrynode.h"
#include "track/beats.h"
#include "track/track_decl.h"
#include "waveform/renderers/waveformrendererabstract.h"

class QNetworkAccessManager;
class QWidget;

namespace zydeck {

// ZyDeck: the phrases of the track on each deck, drawn in Mixxx as ZyDeck's pages draw them: a strip in each section's
// colour along the bottom of the overview and the scrolling waveform, and a tag where a section starts ("B Chorus").
// The sections are ZyDeck Connect's (zydeckstructure.h, as rekordbox's phrase analysis: /web/structure, or its file
// zydeck-structure/<id>.json here when Connect isn't running). No Connect and no file: nothing is drawn.
struct Phrase {
    double start = 0;   // seconds
    double end = 0;
    QString tag;        // "B Chorus" where a section starts, "" where the one before goes on
    QColor color;
};

class Phrases : public QObject {
    Q_OBJECT
  public:
    static Phrases* instance();
    void watch(UserSettingsPointer pConfig);   // follows the decks' tracks from now on
    /// A deck's phrases, if they're that track's
    QList<Phrase> of(const QString& group, const TrackPointer& pTrack) const;
    int version() const {   // goes up with every change, on any deck
        return m_version;
    }

  signals:
    void changed(const QString& group);

  private:
    struct Deck {
        int id = 0;
        int tries = 0;
        QList<Phrase> phrases;
        QMetaObject::Connection beats;
    };
    explicit Phrases(QObject* pParent);
    void trackChanged(const QString& group, TrackPointer pNew);
    void ask(const QString& group, int id);
    void answered(const QString& group, int id, const QJsonObject& answer, bool fromConnect);
    void again(const QString& group, int id, int ms);
    void set(const QString& group, QList<Phrase> phrases);

    QHash<QString, Deck> m_decks;
    QNetworkAccessManager* m_pNet;
    QString m_folder;
    int m_version = 0;
};

/// The overview's strip and tags: a see-through child of it, so Mixxx's own drawing stays as it is.
void addPhraseStrip(QWidget* pOverview, const QString& group);

/// The scrolling waveform's (allshader::WaveformWidget), over the wave and under the hot cues.
class ImageNode;

class PhraseRenderer final : public ::WaveformRendererAbstract, public rendergraph::GeometryNode {
  public:
    explicit PhraseRenderer(WaveformWidgetRenderer* pWaveformWidget);
    void setup(const QDomNode&, const SkinContext&) override {
    }
    void draw(QPainter*, QPaintEvent*) override {
    }
    void preprocess() override;

  private:
    class Tags;
    void makeTags(const QList<Phrase>& phrases, float devicePixelRatio);

    Tags* m_pTags;
    int m_tagsVersion = -1;
    int m_tagsId = 0;
    float m_tagsRatio = 0;
};

/// The bars left, as on ZyDeck's pages and rekordbox's: at the top of the play cursor those in the track (-8.3 bars:
/// bars.beats), at the lower left the section the deck is in and those left in it. Over everything on the waveform.
class BarsLeftRenderer final : public ::WaveformRendererAbstract, public rendergraph::GeometryNode {
  public:
    explicit BarsLeftRenderer(WaveformWidgetRenderer* pWaveformWidget);
    void setup(const QDomNode&, const SkinContext&) override {
    }
    void draw(QPainter*, QPaintEvent*) override {
    }
    void preprocess() override;

  private:
    void clear();

    ImageNode* m_pTrack;
    ImageNode* m_pSection;
    mixxx::BeatsPointer m_pBeats;
    mixxx::audio::FramePos m_beat;       // the beat the counts were made on, and the next
    mixxx::audio::FramePos m_nextBeat;
    int m_sectionIndex = -1;
    int m_version = -1;
    float m_ratio = 0;
};

} // namespace zydeck
