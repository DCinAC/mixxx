#pragma once
// ZyDeck: a track's structure as a file, zydeck-structure/<track id>.json in Mixxx's settings folder: one shape for
// the hub (which works it out on the device) and ZyDeck Connect (on the computer, where Mixxx ZyDeck's Auto DJ reads
// it): {ok, sig, bar, gridBar, confidence, sections: [{b0, b1 (beats), s, e (seconds), letter, label, energy}]}.
// sig: what it was worked out from (the analysis's version and the beat grid); when it changes, it's out of date.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <cmath>
#include <vector>

#include "zydeckstructure.h"

namespace zydeck::structure {

constexpr int kFileVersion = 2;   // (a new analysis: worked out again)

inline QString signature(const std::vector<double>& beats, int gridBar) {
    if (beats.empty()) {
        return {};
    }
    return QStringLiteral("%1:%2:%3:%4:%5")
            .arg(kFileVersion)
            .arg(beats.size())
            .arg(beats.front(), 0, 'f', 3)
            .arg(beats.back(), 0, 'f', 3)
            .arg(gridBar);
}

inline QJsonObject toJson(const Result& r, const std::vector<double>& beats, int gridBar) {
    QJsonArray sections;
    const auto seconds = [&beats](int beat) {
        if (beat < static_cast<int>(beats.size())) {
            return beats[beat];
        }
        return beats.size() > 1 ? beats.back() + (beats.back() - beats[beats.size() - 2]) : beats.back();
    };
    for (const auto& s : r.sections) {
        sections.append(QJsonObject{{"b0", s.startBeat},
                {"b1", s.endBeat},
                {"s", std::round(seconds(s.startBeat) * 1000) / 1000},
                {"e", std::round(seconds(s.endBeat) * 1000) / 1000},
                {"letter", QString(QChar(s.letter))},
                {"label", QString::fromStdString(s.label)},
                {"energy", std::round(s.energy * 100) / 100}});
    }
    return {{"ok", true},
            {"sig", signature(beats, gridBar)},
            {"bar", r.bar},
            {"gridBar", gridBar},
            {"confidence", std::round(r.confidence * 100) / 100},
            {"sections", sections}};
}

} // namespace zydeck::structure
