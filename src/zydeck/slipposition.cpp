#include "zydeck/slipposition.h"

#include <QSharedPointer>
#include <QTimer>
#include <cmath>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "mixer/playerinfo.h"
#include "util/performancetimer.h"
#include "waveform/isynctimeprovider.h"
#include "waveform/visualplayposition.h"

namespace zydeck {

namespace {

constexpr int kDecks = 4;
constexpr int kEveryMs = 40;

// Mixxx's waveforms ask where the deck will be at their next frame; this asks where it is now
class NowSync : public VSyncTimeProvider {
  public:
    std::chrono::microseconds fromTimerToNextSync(const PerformanceTimer& timer) override {
        return std::chrono::microseconds(timer.elapsed().toIntegerMicros());
    }
    std::chrono::microseconds getSyncInterval() const override {
        return std::chrono::microseconds(16667);
    }
};

struct Deck {
    QString group;
    ControlObject* pSeconds;
    QSharedPointer<VisualPlayPosition> pPosition;
};

} // namespace

void publishSlipPositions() {
    auto* pTimer = new QTimer(&PlayerInfo::instance());
    auto pDecks = std::make_shared<std::vector<Deck>>();
    for (int i = 1; i <= kDecks; ++i) {
        const QString group = QStringLiteral("[Channel%1]").arg(i);
        auto* pSeconds = new ControlObject(ConfigKey(group, QStringLiteral("zydeck_slip_seconds")));
        pSeconds->setParent(pTimer);
        pDecks->push_back({group, pSeconds, {}});
    }
    QObject::connect(pTimer, &QTimer::timeout, pTimer, [pDecks] {
        static NowSync now;
        for (Deck& deck : *pDecks) {
            double seconds = 0;
            const ControlObject* pSlip = ControlObject::getControl(
                    ConfigKey(deck.group, QStringLiteral("slip_enabled")), ControlFlag::NoWarnIfMissing);
            if (pSlip && pSlip->toBool()) {
                if (!deck.pPosition) {
                    deck.pPosition = VisualPlayPosition::getVisualPlayPosition(deck.group);
                }
                double play = 0, slip = 0;
                // (as Mixxx's waveforms: slipping while the two positions differ)
                if (deck.pPosition->getPlaySlipAtNextVSync(&now, &play, &slip) && std::abs(slip - play) > 1e-9) {
                    seconds = slip * ControlObject::get(ConfigKey(deck.group, QStringLiteral("duration")));
                }
            }
            deck.pSeconds->set(seconds);
        }
    });
    pTimer->start(kEveryMs);
}

} // namespace zydeck
