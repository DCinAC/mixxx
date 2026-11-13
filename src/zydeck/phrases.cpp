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
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QTimer>
#include <QTransform>
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
        p.label = name;
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

namespace {
// The intro and outro cues from the sections: an intro first, an outro last (a run of them as one); each cue only
// where the track has none yet (-1), so Mixxx's own (its analysis sets the intro's start and the outro's end where
// the sound starts and stops) and one set by hand stay
void fillIntroOutro(const QString& group, const QList<Phrase>& phrases) {
    const double rate = ControlObject::get(ConfigKey(group, QStringLiteral("track_samplerate")));
    if (phrases.isEmpty() || rate <= 0) {
        return;
    }
    const auto is = [](const Phrase& p, const char* kind) { return p.label.compare(QLatin1String(kind), Qt::CaseInsensitive) == 0; };
    const auto put = [&](const char* key, double seconds) {
        const ConfigKey ck(group, QString::fromLatin1(key));
        if (ControlObject::get(ck) < 0 && seconds >= 0) {
            ControlObject::set(ck, std::round(seconds * rate) * 2);   // (positions count both channels' samples)
        }
    };
    if (is(phrases.first(), "intro")) {
        int i = 0;
        while (i + 1 < phrases.size() && is(phrases[i + 1], "intro")) {
            ++i;
        }
        put("intro_start_position", phrases.first().start);
        put("intro_end_position", phrases[i].end);
    }
    if (is(phrases.last(), "outro")) {
        int i = static_cast<int>(phrases.size()) - 1;
        while (i > 0 && is(phrases[i - 1], "outro")) {
            --i;
        }
        put("outro_start_position", phrases[i].start);
        put("outro_end_position", phrases.last().end);
    }
}
} // namespace

void Phrases::set(const QString& group, QList<Phrase> phrases) {
    m_decks[group].phrases = std::move(phrases);
    ++m_version;
    fillIntroOutro(group, m_decks[group].phrases);
    emit changed(group);
}

void addPhraseStrip(QWidget* pOverview, const QString& group) {
    if (group.startsWith(QLatin1String("[Channel"))) {
        new PhraseStrip(pOverview, group);
    }
}

// ---- the scrolling waveform ------------------------------------------------------------------------------------
// A texture made with QPainter on a QImage (as WaveformRenderMark's), drawn as rectangles of it. Nothing until it has one.
class ImageNode : public GeometryNode {
  public:
    ImageNode() {
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
        imageWidth = static_cast<float>(image.width());
    }

    float imageWidth = 1;   // (its pixels)

  private:
    bool m_ready = false;
};

// The tags are one texture, made when the phrases change, each tag a rectangle of it.
class PhraseRenderer::Tags : public ImageNode {
  public:
    struct Tag {
        float v0, v1, width;
    };
    QHash<QString, Tag> tags;
    float tagsWidth = 1;   // (logical pixels)
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
    m_pTags->tagsWidth = width;
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
                {0.f, tag.v0}, {tag.width / m_pTags->tagsWidth, tag.v1});
    }
    markDirtyGeometry();
    markDirtyMaterial();
    m_pTags->markDirtyGeometry();
}

// ---- the bars left -----------------------------------------------------------------------------------------------
namespace {

constexpr int kCountHeightPx = 18;   // the time left
constexpr int kCountFontPx = 13;
constexpr qreal kBarsScale = 1.2;    // the bars left, drawn this much bigger
constexpr int kBarsHeightPx = 22;
constexpr float kSectionMinBreadth = 48;   // a waveform lower than this: only the track's count
constexpr double kSectionEndSlackSeconds = 0.05;   // a section's end is a beat's, to its rounding

// As zydeck-wave.js's fmtBarsLeft: -8.0 on a bar's first beat 8 bars out, -7.3, -7.2 … "0.0" at the end
QString barsText(int beats) {
    return beats <= 0 ? QStringLiteral("0.0") : QStringLiteral("-%1.%2").arg(beats / 4).arg(beats % 4);
}

void paintOutlined(QPainter* pPainter, const QFont& font, qreal x, qreal baseline, const QString& text, const QColor& color) {
    QPainterPath path;
    path.addText(x, baseline, font, text);
    pPainter->strokePath(path, QPen(QColor(0, 0, 0, 235), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    pPainter->fillPath(path, color);
}

// "[B Chorus] -3.2 bars", no ground: outlined, so it reads over the wave. tag "": without the coloured part.
QImage countImage(const QString& tag, const QColor& color, const QString& count, float devicePixelRatio) {
    QFont number;
    number.setPixelSize(kCountFontPx);
    number.setBold(true);
    QFont unit;
    unit.setPixelSize(kTagFontPx);
    const QString bars = QStringLiteral(" bars");
    const qreal tagW = tag.isEmpty() ? 0 : tagWidth(tag) + 4;
    const QFontMetricsF metrics(number);
    const qreal numberW = metrics.horizontalAdvance(count);
    const qreal unitW = QFontMetricsF(unit).horizontalAdvance(bars);
    constexpr qreal kMargin = 2;   // room for the outline
    const qreal width = std::ceil((2 * kMargin + tagW + numberW + unitW) * kBarsScale);
    const qreal height = kBarsHeightPx / kBarsScale;
    QImage image(static_cast<int>(width * devicePixelRatio), static_cast<int>(kBarsHeightPx * devicePixelRatio),
            QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.scale(kBarsScale, kBarsScale);
    qreal x = kMargin;
    if (!tag.isEmpty()) {
        Phrase phrase;
        phrase.tag = tag;
        phrase.color = color;
        paintTag(&painter, QRectF(x, (height - kTagHeightPx) / 2, tagW - 4, kTagHeightPx), phrase);
        x += tagW;
    }
    const qreal baseline = (height + metrics.ascent() - metrics.descent()) / 2;
    paintOutlined(&painter, number, x, baseline, count, Qt::white);
    paintOutlined(&painter, unit, x + numberW, baseline, bars, QColor(190, 190, 190));
    painter.end();
    return image;
}

// The time left in seven segments, as ZyDeck's pages: "-3:14"
QImage timeImage(const QString& text, float devicePixelRatio) {
    static const QHash<QChar, QString> lit{{'0', "abcdef"}, {'1', "bc"}, {'2', "abdeg"}, {'3', "abcdg"}, {'4', "bcfg"},
            {'5', "acdfg"}, {'6', "acdefg"}, {'7', "abc"}, {'8', "abcdefg"}, {'9', "abcdfg"}, {'-', "g"}};
    static const QPointF segments[7][6] = {   // on a digit of 12 x 20, a to g
            {{2.4, 0}, {9.6, 0}, {10.6, 1}, {9.6, 2}, {2.4, 2}, {1.4, 1}},
            {{11, 1.4}, {12, 2.4}, {12, 8.6}, {11, 9.6}, {10, 8.6}, {10, 2.4}},
            {{11, 10.4}, {12, 11.4}, {12, 17.6}, {11, 18.6}, {10, 17.6}, {10, 11.4}},
            {{2.4, 18}, {9.6, 18}, {10.6, 19}, {9.6, 20}, {2.4, 20}, {1.4, 19}},
            {{1, 10.4}, {2, 11.4}, {2, 17.6}, {1, 18.6}, {0, 17.6}, {0, 11.4}},
            {{1, 1.4}, {2, 2.4}, {2, 8.6}, {1, 9.6}, {0, 8.6}, {0, 2.4}},
            {{2.4, 9}, {9.6, 9}, {10.6, 10}, {9.6, 11}, {2.4, 11}, {1.4, 10}}};
    constexpr qreal kScale = 0.7, kDigit = 16 * kScale, kColon = 5, kMargin = 3;
    qreal width = 2 * kMargin;
    for (const QChar c : text) {
        width += c == ':' ? kColon : kDigit;
    }
    QImage image(static_cast<int>(std::ceil(width) * devicePixelRatio), static_cast<int>(kCountHeightPx * devicePixelRatio),
            QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 190));
    painter.drawRoundedRect(QRectF(0, 0, width, kCountHeightPx), 3, 3);
    qreal x = kMargin;
    for (const QChar c : text) {
        if (c == ':') {
            painter.setBrush(QColor(228, 226, 218));
            painter.drawRect(QRectF(x + 1.5, 6, 2, 2));
            painter.drawRect(QRectF(x + 1.5, 11, 2, 2));
            x += kColon;
            continue;
        }
        painter.save();
        painter.translate(x + 2.8 * kScale, 2);
        painter.scale(kScale, kScale);
        painter.setTransform(QTransform(1, 0, -std::tan(8 * M_PI / 180), 1, 0, 0), true);
        const QString on = lit.value(c);
        for (int i = 0; i < 7; ++i) {
            painter.setBrush(on.contains(QChar('a' + i)) ? QColor(255, 180, 58) : QColor(21, 18, 13));
            painter.drawConvexPolygon(segments[i], 6);
        }
        painter.restore();
        x += kDigit;
    }
    painter.end();
    return image;
}

void place(ImageNode* pNode, float x, float y, float width, float height = kCountHeightPx) {
    pNode->geometry().allocate(6);
    TexturedVertexUpdater vertices{pNode->geometry().vertexDataAs<Geometry::TexturedPoint2D>()};
    vertices.addRectangle({x, y}, {x + width, y + height}, {0.f, 0.f}, {1.f, 1.f});
    pNode->markDirtyGeometry();
}

void hide(ImageNode* pNode) {
    pNode->geometry().allocate(0);
    pNode->markDirtyGeometry();
}

} // namespace

BarsLeftRenderer::BarsLeftRenderer(WaveformWidgetRenderer* pWaveformWidget)
        : ::WaveformRendererAbstract(pWaveformWidget),
          m_show(ConfigKey(QStringLiteral("[Skin]"), QStringLiteral("zydeck_show_waveform_counters")), nullptr, ControlFlag::NoWarnIfMissing),
          m_rateRatio(ConfigKey(pWaveformWidget->getGroup(), QStringLiteral("rate_ratio")), nullptr, ControlFlag::NoWarnIfMissing) {
    initForRectangles<RGBAMaterial>(0);
    setUsePreprocess(true);
    auto pTrack = std::make_unique<ImageNode>();
    m_pTrack = pTrack.get();
    appendChildNode(std::move(pTrack));
    auto pSection = std::make_unique<ImageNode>();
    m_pSection = pSection.get();
    appendChildNode(std::move(pSection));
    auto pTime = std::make_unique<ImageNode>();
    m_pTime = pTime.get();
    appendChildNode(std::move(pTime));
}

void BarsLeftRenderer::clear() {
    hide(m_pTrack);
    hide(m_pSection);
    hide(m_pTime);
    m_pBeats.reset();
    m_timeLeft = -1;
}

void BarsLeftRenderer::preprocess() {
    const TrackPointer pTrack = m_waveformRenderer->getTrackInfo();
    const double rate = pTrack ? pTrack->getSampleRate().toDouble() : 0;
    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (rate <= 0 || trackSamples <= 0 || (m_show.valid() && !m_show.toBool())) {
        clear();
        return;
    }
    const float ratio = m_waveformRenderer->getDevicePixelRatio();
    const float length = m_waveformRenderer->getLength(), breadth = m_waveformRenderer->getBreadth();
    // (slipping, the deck's wave is the lower half)
    const float top = m_waveformRenderer->isSlipActive() ? breadth / 2.f : 0.f;
    const auto widthOf = [ratio](const ImageNode* pNode) {
        return pNode->imageWidth / ratio;
    };
    const auto pos = mixxx::audio::FramePos::fromEngineSamplePos(
            m_waveformRenderer->getTruePosSample(::WaveformRendererAbstract::Play));
    const double seconds = pos.value() / rate;
    const auto end = mixxx::audio::FramePos::fromEngineSamplePos(trackSamples);

    // The time left, at the upper right
    const double speed = m_rateRatio.valid() && m_rateRatio.get() > 0 ? m_rateRatio.get() : 1;
    const int timeLeft = static_cast<int>(std::lround(std::max(0.0, end.value() / rate - seconds) / speed));
    if (timeLeft != m_timeLeft || m_ratio != ratio) {
        m_timeLeft = timeLeft;
        m_pTime->setImage(m_waveformRenderer->getContext(),
                timeImage(QStringLiteral("-%1:%2").arg(timeLeft / 60).arg(timeLeft % 60, 2, 10, QChar('0')), ratio));
    }
    place(m_pTime, std::max(0.f, length - widthOf(m_pTime) - 4.f), top + 2.f, widthOf(m_pTime));

    // The bars: none without a beat grid
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    const QList<Phrase> phrases = pBeats ? Phrases::instance()->of(m_waveformRenderer->getGroup(), pTrack) : QList<Phrase>();
    int section = -1;
    for (int i = 0; i < phrases.size(); ++i) {
        if (seconds >= phrases[i].start && seconds < phrases[i].end) {
            section = i;
            break;
        }
    }
    const bool sameBeat = pBeats && m_pBeats == pBeats && m_beat.isValid() && m_nextBeat.isValid() && pos >= m_beat && pos < m_nextBeat;
    if (pBeats && (!sameBeat || section != m_sectionIndex || m_version != Phrases::instance()->version() || m_ratio != ratio)) {
        if (!pBeats->findPrevNextBeats(pos, &m_beat, &m_nextBeat, false)) {
            hide(m_pTrack);
            hide(m_pSection);
            m_pBeats.reset();
            m_ratio = ratio;
            markDirtyMaterial();
            return;
        }
        m_pBeats = pBeats;
        m_sectionIndex = section;
        m_version = Phrases::instance()->version();
        m_pTrack->setImage(m_waveformRenderer->getContext(),
                countImage(QString(), QColor(), barsText(pBeats->numBeatsInRange(m_beat, end)), ratio));
        if (section >= 0) {
            QString tag;   // (a run of the same one is tagged once, at its start)
            for (int i = section; i >= 0 && tag.isEmpty(); --i) {
                tag = phrases[i].tag;
            }
            const mixxx::audio::FramePos sectionEnd((phrases[section].end - kSectionEndSlackSeconds) * rate);
            m_pSection->setImage(m_waveformRenderer->getContext(),
                    countImage(tag, phrases[section].color, barsText(pBeats->numBeatsInRange(m_beat, sectionEnd)), ratio));
        }
    }
    m_ratio = ratio;
    if (!pBeats) {
        hide(m_pTrack);
        hide(m_pSection);
        m_pBeats.reset();
        markDirtyMaterial();
        return;
    }
    // The track's, its right edge just left of the play cursor, as rekordbox (cue labels are right of their line)
    const float cursor = static_cast<float>(m_waveformRenderer->getPlayMarkerPosition() * length);
    place(m_pTrack, std::max(0.f, cursor - widthOf(m_pTrack) - 3.f), top + 2.f, widthOf(m_pTrack), kBarsHeightPx);
    // The section's, at the lower left
    if (section >= 0 && breadth >= kSectionMinBreadth) {
        place(m_pSection, 4.f, breadth - kStripPx - kBarsHeightPx - 1.f, widthOf(m_pSection), kBarsHeightPx);
    } else {
        hide(m_pSection);
    }
    markDirtyMaterial();
}

} // namespace zydeck
