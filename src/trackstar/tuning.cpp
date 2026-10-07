#include "trackstar/tuning.h"

#include <QHash>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "preferences/configobject.h"

namespace trackstar {

namespace {
const QString kGroup = QStringLiteral("[TrackStar]");

// QHash needs copyable values, so ownership lives in a vector and the hash
// holds raw pointers (both static: the knobs live as long as the process).
QHash<QString, ControlObject*>& knobs() {
    static QHash<QString, ControlObject*> s_knobs;
    return s_knobs;
}
std::vector<std::unique_ptr<ControlObject>>& owner() {
    static std::vector<std::unique_ptr<ControlObject>> s_owner;
    return s_owner;
}
} // namespace

double tuning(const QString& key, double defaultValue) {
    auto& map = knobs();
    auto it = map.find(key);
    if (it == map.end()) {
        const ConfigKey ck(kGroup, key);
        if (ControlObject::exists(ck)) {
            // Created elsewhere (e.g. by the bridge): just read it.
            return ControlObject::get(ck);
        }
        // bIgnoreNops=true, bTrack=false, bPersist=true: value survives restarts via mixxx.cfg
        owner().push_back(std::make_unique<ControlObject>(ck, true, false, true, defaultValue));
        it = map.insert(key, owner().back().get());
    }
    return it.value()->get();
}

QColor displayColor(const QColor& trackColor) {
    // #F3F5F9 = the suite's ink token (trackstar/ui/tokens.css), "white" everywhere else too
    return tuning(QStringLiteral("mono_colors"), 1.0) > 0.0 ? QColor(0xF3, 0xF5, 0xF9) : trackColor;
}

QColor stemColor(const QString& label, int deckIdx, const QColor& fileColor) {
    if (tuning(QStringLiteral("stem_palette"), 1.0) <= 0.0) {
        return fileColor;
    }
    const QString l = label.toLower();
    const bool purple = deckIdx == 1;
    if (l.contains(QStringLiteral("voc"))) {
        return QColor(0xFF, 0xFF, 0xFF);
    }
    if (l.contains(QStringLiteral("drum"))) {
        // brighter than the deck colour so the beat stands out (Brecht 07-10: "drums too dark")
        return purple ? QColor(0xF2, 0xDD, 0xF5) : QColor(0xD6, 0xE3, 0xFF);
    }
    if (l.contains(QStringLiteral("bass"))) {
        return purple ? QColor(0x9A, 0x6A, 0xA4) : QColor(0x4F, 0x78, 0xB8);
    }
    return QColor(0x5C, 0x62, 0x73);   // other / unknown
}

} // namespace trackstar
