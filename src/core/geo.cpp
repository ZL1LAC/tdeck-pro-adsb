#include "geo.h"

#include <math.h>

#include "config.h"

namespace geo {
namespace {
constexpr double kDeg2Rad = M_PI / 180.0;
constexpr double kRad2Deg = 180.0 / M_PI;
}  // namespace

double distanceNm(double lat1, double lon1, double lat2, double lon2) {
    const double phi1 = lat1 * kDeg2Rad;
    const double phi2 = lat2 * kDeg2Rad;
    const double dPhi = (lat2 - lat1) * kDeg2Rad;
    const double dLam = (lon2 - lon1) * kDeg2Rad;

    const double sinPhi = sin(dPhi * 0.5);
    const double sinLam = sin(dLam * 0.5);
    const double a = sinPhi * sinPhi + cos(phi1) * cos(phi2) * sinLam * sinLam;
    return 2.0 * kEarthRadiusNm * atan2(sqrt(a), sqrt(1.0 - a));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double phi1 = lat1 * kDeg2Rad;
    const double phi2 = lat2 * kDeg2Rad;
    const double dLam = (lon2 - lon1) * kDeg2Rad;

    const double y = sin(dLam) * cos(phi2);
    const double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dLam);
    double b = atan2(y, x) * kRad2Deg;
    if (b < 0.0) b += 360.0;
    return b;
}

void projectNm(double lat0, double lon0, double lat, double lon,
               float *eastNm, float *northNm) {
    // 1 degree of latitude is 60 nm; a degree of longitude shrinks by cos(lat).
    double dLon = lon - lon0;
    if (dLon > 180.0) dLon -= 360.0;
    if (dLon < -180.0) dLon += 360.0;

    if (eastNm) *eastNm = static_cast<float>(dLon * 60.0 * cos(lat0 * kDeg2Rad));
    if (northNm) *northNm = static_cast<float>((lat - lat0) * 60.0);
}

Projector::Projector(double lat0, double lon0)
    : lat0E7_(static_cast<int32_t>(lat0 * 1e7)),
      lon0E7_(static_cast<int32_t>(lon0 * 1e7)),
      eastScale_(static_cast<float>(60.0 * 1e-7 * cos(lat0 * kDeg2Rad))),
      northScale_(static_cast<float>(60.0 * 1e-7)) {}

void Projector::project(int32_t latE7, int32_t lonE7, float *eastNm,
                        float *northNm) const {
    // Widened before subtracting: two longitudes either side of the
    // antimeridian are nearly +/-1.8e9 apart, which int32 cannot hold.
    int64_t dLon = static_cast<int64_t>(lonE7) - static_cast<int64_t>(lon0E7_);
    if (dLon > 1800000000LL) dLon -= 3600000000LL;
    if (dLon < -1800000000LL) dLon += 3600000000LL;

    if (eastNm) *eastNm = static_cast<float>(dLon) * eastScale_;
    if (northNm) *northNm = static_cast<float>(latE7 - lat0E7_) * northScale_;
}

void offsetNm(double lat0, double lon0, float eastNm, float northNm,
              double *lat, double *lon) {
    if (lat) *lat = lat0 + northNm / 60.0;
    if (lon) {
        const double cosLat = cos(lat0 * kDeg2Rad);
        *lon = lon0 + (fabs(cosLat) > 1e-6 ? eastNm / (60.0 * cosLat) : 0.0);
    }
}

const char *compassPoint(double bearing) {
    static const char *kPoints[16] = {"N",  "NNE", "NE", "ENE", "E",  "ESE",
                                      "SE", "SSE", "S",  "SSW", "SW", "WSW",
                                      "W",  "WNW", "NW", "NNW"};
    while (bearing < 0.0) bearing += 360.0;
    int idx = static_cast<int>((bearing + 11.25) / 22.5) % 16;
    return kPoints[idx];
}

float displayDistance(float nauticalMiles) {
#if UNITS_DISTANCE == 1
    return nauticalMiles * 1.15078f;
#elif UNITS_DISTANCE == 2
    return nauticalMiles * 1.852f;
#else
    return nauticalMiles;
#endif
}

const char *distanceUnitLabel() {
#if UNITS_DISTANCE == 1
    return "mi";
#elif UNITS_DISTANCE == 2
    return "km";
#else
    return "nm";
#endif
}

float displaySpeed(float knots) {
#if UNITS_SPEED == 1
    return knots * 1.15078f;
#elif UNITS_SPEED == 2
    return knots * 1.852f;
#else
    return knots;
#endif
}

const char *speedUnitLabel() {
#if UNITS_SPEED == 1
    return "mph";
#elif UNITS_SPEED == 2
    return "kph";
#else
    return "kt";
#endif
}

}  // namespace geo
