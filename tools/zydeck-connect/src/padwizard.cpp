#include "padwizard.h"

#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>

#include "hardware.h"
#include "midiin.h"

namespace zc {

namespace {

struct Target {
    const char* key;
    bool shift;
};
// The MK2's four mode keys (as printed on it), then each with SHIFT
const Target kTargets[] = {{"HOT CUE (M1)", false}, {"LOOP (M2)", false}, {"SLICER (M3)", false}, {"SAMPLER (M4)", false},
        {"HOT CUE (M1)", true}, {"LOOP (M2)", true}, {"SLICER (M3)", true}, {"SAMPLER (M4)", true}};
const int kCount = int(std::size(kTargets));

class PadWizard : public QDialog {
  public:
    explicit PadWizard(QWidget* parent)
            : QDialog(parent) {
        setWindowTitle(tr("Inpulse pad modes"));
        setAttribute(Qt::WA_DeleteOnClose);
        resize(520, 420);
        auto* l = new QVBoxLayout(this);
        auto* row = new QHBoxLayout;
        m_ports = new QComboBox;
        for (const QString& p : MidiReader::ports()) {
            m_ports->addItem(p);
            if (p.contains(QLatin1String("Inpulse"), Qt::CaseInsensitive)) {
                m_ports->setCurrentIndex(m_ports->count() - 1);
            }
        }
        auto* start = new QPushButton(tr("Start"));
        row->addWidget(m_ports, 1);
        row->addWidget(start);
        l->addLayout(row);
        m_say = new QLabel(tr("Pick the Inpulse, then Start. Use deck A (the left side)."));
        m_say->setWordWrap(true);
        QFont f = m_say->font();
        f.setPixelSize(18);
        f.setBold(true);
        m_say->setFont(f);
        m_say->setMinimumHeight(70);
        l->addWidget(m_say);
        m_log = new QPlainTextEdit;
        m_log->setReadOnly(true);
        l->addWidget(m_log, 1);
        auto* buttons = new QHBoxLayout;
        auto* skip = new QPushButton(tr("Skip this one"));
        auto* save = new QPushButton(tr("Save"));
        auto* close = new QPushButton(tr("Close"));
        buttons->addWidget(skip);
        buttons->addStretch();
        buttons->addWidget(save);
        buttons->addWidget(close);
        l->addLayout(buttons);
        connect(start, &QPushButton::clicked, this, [this] {
            if (!m_in.open(m_ports->currentText())) {
                m_say->setText(tr("Couldn't listen to %1.").arg(m_ports->currentText()));
                return;
            }
            m_i = 0;
            m_keys = {};
            m_log->clear();
            ask();
        });
        connect(skip, &QPushButton::clicked, this, [this] {
            if (m_i < 0 || m_i >= kCount) {
                return;
            }
            m_log->appendPlainText(tr("%1%2: skipped").arg(m_shiftText(), QString::fromLatin1(kTargets[m_i].key)));
            ++m_i;
            m_padNext = false;
            ask();
        });
        connect(save, &QPushButton::clicked, this, [this] { saveIt(); });
        connect(close, &QPushButton::clicked, this, &QDialog::close);
        m_in.context = this;
        m_in.onMessage = [this](quint8 st, quint8 d1, quint8 v) { message(st, d1, v); };
    }

  private:
    QString m_shiftText() const { return m_i >= 0 && m_i < kCount && kTargets[m_i].shift ? QStringLiteral("SHIFT + ") : QString(); }

    void ask() {
        if (m_i >= kCount) {
            m_say->setText(tr("All done. Click Save."));
            return;
        }
        const Target& t = kTargets[m_i];
        if (m_padNext) {
            m_say->setText(tr("Now let go of everything and press PAD 1 (deck A)."));
        } else if (t.shift) {
            m_say->setText(tr("%1 of %2: hold SHIFT, press %3, let go of both.").arg(m_i + 1).arg(kCount).arg(QString::fromLatin1(t.key)));
        } else {
            m_say->setText(tr("%1 of %2: press %3.").arg(m_i + 1).arg(kCount).arg(QString::fromLatin1(t.key)));
        }
    }

    void message(quint8 st, quint8 note, quint8 v) {
        if ((st & 0xF0) != 0x90) {
            return;
        }
        if (st == 0x91 && note == 0x04) {   // SHIFT (deck A)
            m_shift = v != 0;
            return;
        }
        if (v == 0 || m_i < 0 || m_i >= kCount) {
            return;
        }
        const Target& t = kTargets[m_i];
        if (!m_padNext) {
            if (st == 0x96 || st == 0x97) {
                return;   // (a pad: not yet)
            }
            if (t.shift && !m_shift) {
                m_say->setText(tr("SHIFT wasn't held. Hold SHIFT, then press %1.").arg(QString::fromLatin1(t.key)));
                return;
            }
            m_key = {st, note, m_shift, -1};
            m_padNext = true;
            ask();
            return;
        }
        if (st != 0x96) {
            return;   // (deck A's pads only)
        }
        m_key.block = note >> 4;
        m_keys.append(m_key);
        m_log->appendPlainText(tr("%1%2: %3 %4 -> pads block %5 (%6)")
                                       .arg(m_shiftText(), QString::fromLatin1(t.key))
                                       .arg(m_key.st, 2, 16, QLatin1Char('0'))
                                       .arg(m_key.note, 2, 16, QLatin1Char('0'))
                                       .arg(m_key.block)
                                       .arg(Hardware::blockName(m_key.block)));
        m_padNext = false;
        ++m_i;
        ask();
    }

    void saveIt() {
        QJsonArray keys;
        for (const Hardware::Key& k : std::as_const(m_keys)) {
            keys.append(QJsonObject{{"st", k.st}, {"note", k.note}, {"shift", k.shift}, {"block", k.block}, {"mode", Hardware::blockName(k.block)}});
        }
        const QString file = Hardware::learnedFile();
        QDir().mkpath(QFileInfo(file).absolutePath());
        QSaveFile f(file);
        if (!f.open(QIODevice::WriteOnly) ||
                f.write(QJsonDocument(QJsonObject{{"device", m_ports->currentText()}, {"keys", keys}}).toJson()) < 0 || !f.commit()) {
            m_log->appendPlainText(tr("Couldn't save %1").arg(file));
            return;
        }
        m_log->appendPlainText(tr("Saved %1 keys to %2").arg(keys.size()).arg(file));
    }

    MidiReader m_in;
    QComboBox* m_ports;
    QLabel* m_say;
    QPlainTextEdit* m_log;
    int m_i = -1;
    bool m_padNext = false, m_shift = false;
    Hardware::Key m_key;
    QList<Hardware::Key> m_keys;
};

QPointer<PadWizard> g_pad;

} // namespace

void showPadWizard(QWidget* parent) {
    if (!g_pad) {
        g_pad = new PadWizard(parent);
    }
    g_pad->show();
    g_pad->raise();
    g_pad->activateWindow();
}

} // namespace zc
