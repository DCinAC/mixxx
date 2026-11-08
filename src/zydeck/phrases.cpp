#include "zydeck/phrases.h"

#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "engine/engine.h"
#include "mixer/playerinfo.h"
#include "moc_phrases.cpp"
#include "rendergraph/context.h"
#include "rendergraph/geometry.h"
#include "rendergraph/material/rgbamaterial.h"
#include "rendergraph/material/texturematerial.h"
#include "rendergraph/texture.h"
#include "rendergraph/vertexupdaters/rgbavertexupdater.h"
#include "rendergraph/vertexupdaters/texturedvertexupdater.h"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"

using namespace rendergraph;

namespace zydeck {

namespace {

const QString kConnect = QStringLiteral("http://127.0.0.1:8771/web/structure");
constexpr int kStripPx = 4;      // the strip along the bottom
constexpr int kTagFontPx = 11;
constexpr int kTagHeightPx = 15;
constexpr int kPendingMs = 2000;   // the sections being worked out: asked again this often
constexpr int kNoneMs = 5000;      // none yet (no beat grid, no Connect): this often, kNoneTries times
constexpr int kNoneTries = 24;
constexpr int kGridEditMs = 3000;  // after an edit of the beat grid: its save, then the sections again

// as zydeck-wave.js's SECTION_COLORS
QColor sectionColor(const QString& label) {
    static const QHash<QString, QColor> colors{{"Intro", "#3fbf6f"}, {"Verse", "#4a90e2"}, {"Pre-Chorus", "#e0b020"},
            {"Chorus", "#e5484d"}, {"Interlude", "#9b6bd6"}, {"Bridge", "#2fb7b0"}, {"Outro", "#3fbf6f"}};
    return colors.value(label, QColor("#888888"));
}

QList<Phrase> phrasesFrom(const QJsonObject& structure) {
    QList<Phrase> out;
    QString letter, label;
    for (const QJsonValue& v : structure.value(QStringLiteral("sections")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString l = o.value(QStringLiteral("letter")).toString(), name = o.value(QStringLiteral("label")).toString();
        Phrase p;
        p.start = o.value(QStringLiteral("s")).toDouble();
        p.end = o.value(QStringLiteral("e")).toDouble();
        p.color = sectionColor(name);
        if (l != letter || name != label) {   // (a run of the same one: tagged once)
            p.tag = QStringLiteral("%1 %2").arg(l, name);
        }
        letter = l;
        label = name;
        out.append(p);
    }
    return out;
}

QFont tagFont() {
    QFont font;
    font.setPixelSize(kTagFontPx);
    font.setBold(true);
    return font;
}

QColor inkOn(const QColor& c) {
    return c.lightnessF() > 0.55 ? QColor(16, 16, 16) : QColor(255, 255, 255);
}

float tagWidth(const QString& tag) {
    return static_cast<float>(std::ceil(QFontMetricsF(tagFont()).horizontalAdvance(tag))) + 8.f;
}

void paintTag(QPainter* pPainter, const QRectF& box, const Phrase& phrase) {
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(phrase.color);
    pPainter->drawRoundedRect(box, 2, 2);
    pPainter->setPen(inkOn(phrase.color));
    pPainter->setFont(tagFont());
    pPainter->drawText(box, Qt::AlignCenter, phrase.tag);
}

// The overview's: over it, letting every click through
class PhraseStrip : public QWidget {
  public:
    PhraseStrip(QWidget* pOverview, const QString& group)
            : QWidget(pOverview),
              m_group(group) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        pOverview->installEventFilter(this);
        setGeometry(pOverview->rect());
        connect(Phrases::instance(), &Phrases::changed, this, [this](const QString& changed) {
            if (changed == m_group) {
                update();
            }
        });
    }

  protected:
    bool eventFilter(QObject* pObject, QEvent* pEvent) override {
        if (pObject == parent() && pEvent->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
        }
        return QWidget::eventFilter(pObject, pEvent);
    }
    void paintEvent(QPaintEvent*) override {
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(m_group);
        const double seconds = ControlObject::get(ConfigKey(m_group, QStringLiteral("duration")));
        if (!pTrack || seconds <= 0 || height() > width()) {   // (a vertical overview: not drawn)
            return;
        }
        const QList<Phrase> phrases = Phrases::instance()->of(m_group, pTrack);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal across = width() - 2, strip = std::min(kStripPx, height() / 8 + 1);
        const auto x = [across, seconds](double t) { return 1 + across * t / seconds; };
        for (const Phrase& p : phrases) {
            painter.fillRect(QRectF(x(p.start), height() - strip, x(p.end) - x(p.start), strip), p.color);
        }
        for (int i = 0; i < phrases.size(); ++i) {
            const Phrase& p = phrases[i];
            if (p.tag.isEmpty()) {
                continue;
            }
            // its letter, while there's room before the next tag
            double next = seconds;
            for (int j = i + 1; j < phrases.size() && next == seconds; ++j) {
                next = phrases[j].tag.isEmpty() ? seconds : phrases[j].start;
            }
            Phrase letter = p;
            letter.tag = p.tag.left(1);
            const float w = tagWidth(letter.tag);
            if (x(next) - x(p.start) >= w) {
                paintTag(&painter, QRectF(x(p.start), height() - strip - kTagHeightPx, w, kTagHeightPx), letter);
            }
        }
    }

  private:
    const QString m_group;
};

} // namespace

// ---- each deck's phrases ---------------------------------------------------------------------------------------
Phrases* Phrases::instance() {
    static Phrases* pPhrases = new Phrases(&PlayerInfo::instance());
    return pPhrases;
}

Phrases::Phrases(QObject* pParent)
        : QObject(pParent),
          m_pNet(new QNetworkAccessManager(this)) {
}

void Phrases::watch(UserSettingsPointer pConfig) {
    m_folder = pConfig->getSettingsPath();
    connect(&PlayerInfo::instance(), &PlayerInfo::trackChanged, this,
            [this](const QString& group, TrackPointer pNew, TrackPointer) { trackChanged(group, pNew); });
}

QList<Phrase> Phrases::of(const QString& group, const TrackPointer& pTrack) const {
    const auto it = m_decks.constFind(group);
    return it != m_decks.cend() && pTrack && it->id == pTrack->getId().toVariant().toInt() ? it->phrases : QList<Phrase>();
}

void Phrases::trackChanged(const QString& group, TrackPointer pNew) {
    if (!group.startsWith(QLatin1String("[Channel"))) {
        return;
    }
    Deck& deck = m_decks[group];
    disconnect(deck.beats);
    deck.id = pNew ? pNew->getId().toVariant().toInt() : 0;
    deck.tries = 0;
    set(group, {});
    if (deck.id <= 0) {
        return;
    }
    const int id = deck.id;
    deck.beats = connect(pNew.get(), &Track::beatsUpdated, this, [this, group, id] {
        m_decks[group].tries = 0;
        again(group, id, kGridEditMs);
    });
    ask(group, id);
}

void Phrases::ask(const QString& group, int id) {
    QUrl url(kConnect);
    url.setQuery(QUrlQuery{{QStringLiteral("id"), QString::number(id)}});
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader("X-ZyDeck", "mixxx");   // (Connect answers this computer's Mixxx ZyDeck without its key)
    QNetworkReply* pReply = m_pNet->get(request);
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, group, id] {
        pReply->deleteLater();
        const QJsonObject answer = QJsonDocument::fromJson(pReply->readAll()).object();
        if (!answer.isEmpty()) {
            answered(group, id, answer, true);
            return;
        }
        QFile file(QDir(m_folder).filePath(QStringLiteral("zydeck-structure/%1.json").arg(id)));   // (no Connect)
        answered(group, id, file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject(), false);
    });
}

void Phrases::answered(const QString& group, int id, const QJsonObject& answer, bool fromConnect) {
    if (m_decks.value(group).id != id) {
        return;   // (another track by now)
    }
    if (fromConnect && answer.value(QStringLiteral("pending")).toBool()) {
        again(group, id, kPendingMs);
        return;
    }
    if (answer.value(QStringLiteral("ok")).toBool() && !answer.value(QStringLiteral("sections")).toArray().isEmpty()) {
        set(group, phrasesFrom(answer));
        if (fromConnect) {
            return;
        }
    }
    if (m_decks[group].tries++ < kNoneTries) {   // (its beat grid may still be coming; Connect may be starting)
        again(group, id, kNoneMs);
    }
}

void Phrases::again(const QString& group, int id, int ms) {
    QTimer::singleShot(ms, this, [this, group, id] {
        if (m_decks.value(group).id == id) {
            ask(group, id);
        }
    });
}

void Phrases::set(const QString& group, QList<Phrase> phrases) {
    m_decks[group].phrases = std::move(phrases);
    ++m_version;
    emit changed(group);
}

void addPhraseStrip(QWidget* pOverview, const QString& group) {
    if (group.startsWith(QLatin1String("[Channel"))) {
        new PhraseStrip(pOverview, group);
    }
}

// ---- the scrolling waveform ------------------------------------------------------------------------------------
// The tags are one texture, made when the phrases change (as WaveformRenderMark's: QPainter only on a QImage), each
// tag a rectangle of it. Not drawn until there's a texture.
class PhraseRenderer::Tags : public GeometryNode {
  public:
    struct Tag {
        float v0, v1, width;
    };
    Tags() {
        initForRectangles<TextureMaterial>(0);
    }
    bool isSubtreeBlocked() const override {
        return !m_ready;
    }
    void setImage(Context* pContext, const QImage& image) {
        dynamic_cast<TextureMaterial&>(material()).setTexture(std::make_unique<Texture>(pContext, image));
        material().setUniform(1, 1.f);
        markDirtyMaterial();
        m_ready = true;
    }
    QHash<QString, Tag> tags;
    float imageWidth = 1;

  private:
    bool m_ready = false;
};

PhraseRenderer::PhraseRenderer(WaveformWidgetRenderer* pWaveformWidget)
        : ::WaveformRendererAbstract(pWaveformWidget) {
    initForRectangles<RGBAMaterial>(0);
    setUsePreprocess(true);
    auto pTags = std::make_unique<Tags>();
    m_pTags = pTags.get();
    appendChildNode(std::move(pTags));
}

void PhraseRenderer::makeTags(const QList<Phrase>& phrases, float devicePixelRatio) {
    QList<Phrase> tagged;
    QHash<QString, Tags::Tag> tags;
    float width = 1;
    for (const Phrase& p : phrases) {
        if (!p.tag.isEmpty() && !tags.contains(p.tag)) {
            const float y = static_cast<float>(tagged.size() * (kTagHeightPx + 1));
            tags.insert(p.tag, {y, y + kTagHeightPx, tagWidth(p.tag)});
            width = std::max(width, tagWidth(p.tag));
            tagged.append(p);
        }
    }
    const float height = std::max(1.f, static_cast<float>(tagged.size() * (kTagHeightPx + 1)));
    QImage image(static_cast<int>(width * devicePixelRatio), static_cast<int>(height * devicePixelRatio),
            QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    for (const Phrase& p : std::as_const(tagged)) {
        const Tags::Tag tag = tags.value(p.tag);
        paintTag(&painter, QRectF(0, tag.v0, tag.width, kTagHeightPx), p);
    }
    painter.end();
    for (Tags::Tag& tag : tags) {
        tag.v0 /= height;
        tag.v1 /= height;
    }
    m_pTags->tags = tags;
    m_pTags->imageWidth = width;
    m_pTags->setImage(m_waveformRenderer->getContext(), image);
}

void PhraseRenderer::preprocess() {
    const TrackPointer pTrack = m_waveformRenderer->getTrackInfo();
    const QList<Phrase> phrases = pTrack ? Phrases::instance()->of(m_waveformRenderer->getGroup(), pTrack) : QList<Phrase>();
    const double rate = pTrack ? pTrack->getSampleRate().toDouble() : 0;
    const float length = m_waveformRenderer->getLength(), breadth = m_waveformRenderer->getBreadth();
    if (phrases.isEmpty() || rate <= 0 || m_waveformRenderer->getTrackSamples() <= 0) {
        geometry().allocate(0);
        markDirtyGeometry();
        m_pTags->geometry().allocate(0);
        m_pTags->markDirtyGeometry();
        return;
    }
    const float ratio = m_waveformRenderer->getDevicePixelRatio();
    const int id = pTrack->getId().toVariant().toInt();
    if (m_tagsVersion != Phrases::instance()->version() || m_tagsId != id || m_tagsRatio != ratio) {
        makeTags(phrases, ratio);
        m_tagsVersion = Phrases::instance()->version();
        m_tagsId = id;
        m_tagsRatio = ratio;
    }

    const auto x = [this, rate, ratio](double seconds) {
        const double at = m_waveformRenderer->transformSamplePositionInRendererWorld(
                seconds * rate * static_cast<int>(mixxx::kEngineChannelOutputCount), ::WaveformRendererAbstract::Play);
        return static_cast<float>(std::round(at * ratio) / ratio);
    };
    // (slipping, the deck's wave is the lower half; the ghost above is Mixxx's)
    const float top = m_waveformRenderer->isSlipActive() ? breadth / 2.f : 0.f;
    const float strip = kStripPx, tagTop = breadth - strip - kTagHeightPx;

    int rectangles = 0, tags = 0;
    for (const Phrase& p : phrases) {
        if (x(p.end) >= 0 && x(p.start) <= length) {
            rectangles += p.tag.isEmpty() ? 1 : 2;
            tags += p.tag.isEmpty() ? 0 : 1;
        }
    }
    geometry().allocate(rectangles * 6);
    m_pTags->geometry().allocate(tags * 6);
    RGBAVertexUpdater bars{geometry().vertexDataAs<Geometry::RGBAColoredPoint2D>()};
    TexturedVertexUpdater labels{m_pTags->geometry().vertexDataAs<Geometry::TexturedPoint2D>()};
    for (const Phrase& p : phrases) {
        const float x0 = x(p.start), x1 = x(p.end);
        if (x1 < 0 || x0 > length) {
            continue;
        }
        float r, g, b, a;
        p.color.getRgbF(&r, &g, &b, &a);
        bars.addRectangle({std::max(x0, 0.f), breadth - strip}, {std::min(x1, length), breadth}, {r, g, b, 1.f});
        if (p.tag.isEmpty()) {
            continue;
        }
        bars.addRectangle({x0, top}, {x0 + 1.f, breadth - strip}, {r, g, b, 0.8f});
        const Tags::Tag tag = m_pTags->tags.value(p.tag);
        labels.addRectangle({x0, tagTop}, {x0 + tag.width, tagTop + kTagHeightPx},
                {0.f, tag.v0}, {tag.width / m_pTags->imageWidth, tag.v1});
    }
    markDirtyGeometry();
    markDirtyMaterial();
    m_pTags->markDirtyGeometry();
}

} // namespace zydeck
