#pragma once
#include <stdint.h>

namespace geo {

constexpr double kEarthRadiusNm = 3440.065;

// Great-circle distance in nautical miles.
double distanceNm(double lat1, double lon1, double lat2, double lon2);

// Initial true bearing from point 1 to point 2, degrees in [0, 360).
double bearingDeg(double lat1, double lon1, double lat2, double lon2);

// Flat-earth projection about (lat0, lon0), accurate enough out to a few
// hundred nautical miles. Returns offsets in nautical miles: +east, +north.
void projectNm(double lat0, double lon0, double lat, double lon,
               float *eastNm, float *northNm);

// Repeated projections about one centre. cos(lat0) and the degrees-to-nm
// scaling are hoisted into the constructor and the vertices arrive as the
// int32 1e-7-degree units the basemap already stores, so the inner loop is
// float-only -- the ESP32-S3 has no hardware double, and the basemap projects
// several thousand vertices per frame.
class Projector {
   public:
    Projector(double lat0, double lon0);
    void project(int32_t latE7, int32_t lonE7, float *eastNm,
                 float *northNm) const;
    // Same projection for the degrees the tracker holds. Quantising to 1e-7
    // of a degree costs about a centimetre, which is well past anything a
    // 240-pixel plot or a 0.1 nm read-out can show.
    void project(double lat, double lon, float *eastNm, float *northNm) const;

   private:
    int32_t lat0E7_;
    int32_t lon0E7_;
    float eastScale_;   // nm per 1e-7 degree of longitude, at this latitude
    float northScale_;  // nm per 1e-7 degree of latitude
};

// Inverse of projectNm: the position lying eastNm east and northNm north of
// (lat0, lon0). Used to turn a pan offset back into a latitude/longitude.
void offsetNm(double lat0, double lon0, float eastNm, float northNm,
              double *lat, double *lon);

// Compass point ("NNE", "SW", ...) for a bearing.
const char *compassPoint(double bearing);

// Unit conversion helpers, driven by the UNITS_* settings in config.h.
float displayDistance(float nauticalMiles);
const char *distanceUnitLabel();
float displaySpeed(float knots);
const char *speedUnitLabel();

}  // namespace geo
