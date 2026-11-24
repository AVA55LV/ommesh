#ifndef OMMESH_BELLE_PHONE_LOCATION_POLICY_HPP
#define OMMESH_BELLE_PHONE_LOCATION_POLICY_HPP

#include <QtCore/QtGlobal>
#include <cmath>

namespace PhoneLocationPolicy {

constexpr double MinimumMovementMeters = 50.0;
constexpr double MaximumAccuracyMeters = 100.0;
constexpr qint64 MinimumWritePeriodMs = 5 * 60 * 1000;
constexpr qint64 RetryDelayMs = 30 * 1000;

struct Fix {
    double latitude;
    double longitude;
    double accuracyMeters;
};

enum class Decision { Write, Near, Wait, Rough, Invalid };

inline bool usableFix(const Fix& fix) {
    return fix.latitude >= -90.0 && fix.latitude <= 90.0 &&
           fix.longitude >= -180.0 && fix.longitude <= 180.0;
}

inline Decision followStep(const Fix& fix, qint32 radioLatE6, qint32 radioLonE6,
                           qint64 msSinceLastWrite = -1) {
    if (!usableFix(fix) || !std::isfinite(fix.accuracyMeters) || fix.accuracyMeters < 0.0)
        return Decision::Invalid;
    if (fix.accuracyMeters > MaximumAccuracyMeters)
        return Decision::Rough;

    const double radioLat = radioLatE6 / 1000000.0;
    const double radioLon = radioLonE6 / 1000000.0;
    // MeshCore uses (0, 0) for a radio whose position is unset.
    if ((radioLatE6 != 0 || radioLonE6 != 0) && usableFix({radioLat, radioLon, 0.0})) {
        constexpr double radians = 3.14159265358979323846 / 180.0;
        constexpr double earthRadiusMeters = 6371008.8;
        const double halfLat = std::sin((radioLat - fix.latitude) * radians / 2.0);
        const double halfLon = std::sin((radioLon - fix.longitude) * radians / 2.0);
        const double chordSquared = halfLat * halfLat +
            std::cos(fix.latitude * radians) * std::cos(radioLat * radians) * halfLon * halfLon;
        const double nearest = std::sin(MinimumMovementMeters / (2.0 * earthRadiusMeters));
        if (chordSquared <= nearest * nearest)
            return Decision::Near;
    }

    return msSinceLastWrite >= 0 && msSinceLastWrite < MinimumWritePeriodMs
        ? Decision::Wait : Decision::Write;
}

} // namespace PhoneLocationPolicy

#endif
