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

} // namespace trackstar
