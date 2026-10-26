#include "solar.hpp"

#include <algorithm>
#include <cmath>

namespace solar {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg = kPi / 180.0f;
constexpr float kSynodicMonth = 29.530589f;
constexpr float kSunAngularRadius = 0.267f;  // degrees

// Representative wavelengths for the R, G and B channels, in micrometres.
constexpr float kLambda[3] = {0.680f, 0.550f, 0.440f};
// Rec.709 luminance weights.
constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};
// Ozone optical depth for ~0.35 atm-cm (Chappuis band peaks near green/orange).
constexpr float kOzoneTau[3] = {0.016f, 0.030f, 0.0016f};
// Twilight sky glow color (deep blue zenith from ozone absorption) and peak level.
constexpr float kTwilightColor[3] = {0.30f, 0.45f, 0.85f};
constexpr float kTwilightPeak = 0.012f;
// Perceptual (Purkinje) blue shift applied to moonlight.
constexpr float kMoonTint[3] = {0.80f, 0.90f, 1.00f};

struct Spectrum {
    float c[3] = {0.0f, 0.0f, 0.0f};
};

float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

float wrap_degrees(float deg) {
    float w = std::fmod(deg, 360.0f);
    return w < 0.0f ? w + 360.0f : w;
}

float luminance(const Spectrum& s) {
    return kLuma[0] * s.c[0] + kLuma[1] * s.c[1] + kLuma[2] * s.c[2];
}

// Rayleigh optical depth at sea level (Bucholtz-style fit), lambda in micrometres.
float rayleigh_tau(float lambda) {
    const float l2 = 1.0f / (lambda * lambda);
    const float l4 = l2 * l2;
    return 0.008569f * l4 * (1.0f + 0.0113f * l2 + 0.00013f * l4);
}

// Aerosol (Mie) optical depth from Preetham's turbidity -> Angstrom beta mapping.
float mie_tau(float lambda, float turbidity) {
    const float beta = std::max(0.0f, 0.04608f * turbidity - 0.04586f);
    return beta * std::pow(lambda / 0.55f, -1.3f);
}

struct Atmosphere {
    Spectrum rayleigh;
    Spectrum mie;
    Spectrum ozone;
};

Atmosphere make_atmosphere(float turbidity) {
    Atmosphere a;
    for (int i = 0; i < 3; ++i) {
        a.rayleigh.c[i] = rayleigh_tau(kLambda[i]);
        a.mie.c[i] = mie_tau(kLambda[i], turbidity);
        a.ozone.c[i] = kOzoneTau[i];
    }
    return a;
}

// Direct-beam transmittance along a path of `m` air masses.
Spectrum transmittance(const Atmosphere& a, float m) {
    Spectrum t;
    for (int i = 0; i < 3; ++i) {
        t.c[i] = std::exp(-m * (a.rayleigh.c[i] + a.mie.c[i] + a.ozone.c[i]));
    }
    return t;
}

// Single-scattering estimate of diffuse skylight on a horizontal surface: light scattered out of
// the beam, with roughly half of Rayleigh and three quarters of forward-peaked Mie reaching the
// ground, attenuated by ozone.
Spectrum diffuse_sky(const Atmosphere& a, float m, float mu) {
    Spectrum d;
    if (mu <= 0.0f) {
        return d;
    }
    for (int i = 0; i < 3; ++i) {
        const float scatterR = 1.0f - std::exp(-m * a.rayleigh.c[i]);
        const float scatterM = 1.0f - std::exp(-m * a.mie.c[i]);
        d.c[i] = mu * (0.5f * scatterR + 0.75f * scatterM) * std::exp(-m * a.ozone.c[i]);
    }
    return d;
}

Rgb normalized_color(const Spectrum& s) {
    const float peak = std::max({s.c[0], s.c[1], s.c[2]});
    if (peak <= 1e-8f) {
        return Rgb{1.0f, 1.0f, 1.0f};
    }
    return Rgb{s.c[0] / peak, s.c[1] / peak, s.c[2] / peak};
}

Vec3 to_world(float elevation_deg, float azimuth_deg, float north_yaw_deg) {
    const float e = elevation_deg * kDeg;
    const float az = azimuth_deg * kDeg;
    const float east = std::cos(e) * std::sin(az);
    const float north = std::cos(e) * std::cos(az);
    const float up = std::sin(e);
    // Unrotated world: +X east, +Y up, +Z south.
    const float x = east;
    const float z = -north;
    const float yaw = north_yaw_deg * kDeg;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    return Vec3{x * cy + z * sy, up, -x * sy + z * cy};
}

// Visible fraction of a body's disk given the apparent elevation of its center.
float disk_visibility(float elevation_deg) {
    return smoothstep(-kSunAngularRadius, kSunAngularRadius, elevation_deg);
}

}  // namespace

float declination_deg(float day_of_year) {
    return -23.44f * std::cos(2.0f * kPi / 365.0f * (day_of_year + 10.0f));
}

float refracted_elevation_deg(float geometric_deg) {
    // Saemundsson's formula; faded out below the horizon where it diverges.
    const float h = std::max(geometric_deg, -2.0f);
    const float arcmin = 1.02f / std::tan((h + 10.3f / (h + 5.11f)) * kDeg);
    const float fade = clamp01((geometric_deg + 5.0f) / 3.0f);
    return geometric_deg + fade * arcmin / 60.0f;
}

float air_mass(float elevation_deg) {
    const float e = std::max(elevation_deg, 0.0f);
    return 1.0f / (std::sin(e * kDeg) + 0.50572f * std::pow(e + 6.07995f, -1.6364f));
}

Body body_position(float declination, float hour_angle_deg, float latitude_deg, float north_yaw_deg) {
    const float phi = latitude_deg * kDeg;
    const float dec = declination * kDeg;
    const float h = hour_angle_deg * kDeg;

    const float sinE = std::sin(phi) * std::sin(dec) + std::cos(phi) * std::cos(dec) * std::cos(h);
    const float geometric = std::asin(std::clamp(sinE, -1.0f, 1.0f)) / kDeg;
    const float azimuth =
        std::atan2(-std::cos(dec) * std::sin(h),
            std::sin(dec) * std::cos(phi) - std::cos(dec) * std::cos(h) * std::sin(phi)) /
        kDeg;

    Body b;
    b.elevation_deg = refracted_elevation_deg(geometric);
    b.azimuth_deg = wrap_degrees(azimuth);
    b.dir = to_world(b.elevation_deg, b.azimuth_deg, north_yaw_deg);
    return b;
}

Result evaluate(const Params& params) {
    Result r;
    const float latitude = std::clamp(params.latitude_deg, -89.9f, 89.9f);
    const float day = params.day_of_year;
    const float hourAngle = (wrap_degrees(params.time_hours * 15.0f) / 15.0f - 12.0f) * 15.0f;
    const float phase = std::fmod(std::max(params.moon_phase_days, 0.0f), kSynodicMonth) / kSynodicMonth;

    // Positions. The moon is modelled on the ecliptic: it trails the sun by its phase angle in
    // hour angle and declination (opposite the sun at full moon).
    r.sun = body_position(declination_deg(day), hourAngle, latitude, params.north_yaw_deg);
    const float moonDecl = -23.44f * std::cos(2.0f * kPi / 365.0f * (day + 10.0f) + 2.0f * kPi * phase);
    r.moon = body_position(moonDecl, hourAngle - 360.0f * phase, latitude, params.north_yaw_deg);

    const Atmosphere atmo = make_atmosphere(std::max(params.turbidity, 1.0f));
    const Spectrum zenithT = transmittance(atmo, 1.0f);
    const Spectrum zenithSky = diffuse_sky(atmo, 1.0f, 1.0f);
    const float zenithLum = std::max(luminance(zenithT), 1e-6f);

    // Sun: direct beam relative to the zenith sun so midday reads white.
    const float sunVis = disk_visibility(r.sun.elevation_deg);
    const float sunMass = air_mass(r.sun.elevation_deg);
    const Spectrum sunT = transmittance(atmo, sunMass);
    Spectrum sunDirect;
    for (int i = 0; i < 3; ++i) {
        sunDirect.c[i] = sunVis * sunT.c[i] / zenithT.c[i];
    }
    r.sun_color = normalized_color(sunDirect);
    r.sun_intensity = sunVis * luminance(sunT) / zenithLum;

    // Sky: scattered sunlight plus twilight glow that persists until nautical dusk.
    const float sunMu = std::max(std::sin(r.sun.elevation_deg * kDeg), 0.0f);
    Spectrum sky = diffuse_sky(atmo, sunMass, sunMu);
    const float twilight = kTwilightPeak * smoothstep(-12.0f, 0.0f, r.sun.elevation_deg) *
                           (1.0f - smoothstep(0.0f, 8.0f, r.sun.elevation_deg));
    for (int i = 0; i < 3; ++i) {
        sky.c[i] = sky.c[i] / zenithT.c[i] + twilight * kTwilightColor[i];
    }

    // Moon: same atmosphere, scaled by illuminated fraction and brightness.
    r.moon_illuminated_fraction = 0.5f * (1.0f - std::cos(2.0f * kPi * phase));
    const float moonVis = disk_visibility(r.moon.elevation_deg);
    const float moonMass = air_mass(r.moon.elevation_deg);
    const Spectrum moonT = transmittance(atmo, moonMass);
    const float moonScale = std::max(params.moon_brightness, 0.0f) * r.moon_illuminated_fraction;
    Spectrum moonDirect;
    for (int i = 0; i < 3; ++i) {
        moonDirect.c[i] = moonScale * moonVis * kMoonTint[i] * moonT.c[i] / zenithT.c[i];
    }
    r.moon_color = normalized_color(moonDirect);
    r.moon_intensity = moonScale * moonVis * luminance(moonT) / zenithLum;
    const float moonMu = std::max(std::sin(r.moon.elevation_deg * kDeg), 0.0f);
    const Spectrum moonSky = diffuse_sky(atmo, moonMass, moonMu);
    for (int i = 0; i < 3; ++i) {
        sky.c[i] += moonScale * kMoonTint[i] * moonSky.c[i] / zenithT.c[i];
    }

    r.sky_color = normalized_color(sky);
    r.sky_intensity = luminance(sky);

    // Dominant light: the sun while any of its disk is up, otherwise the moon.
    r.moon_is_dominant = r.sun_intensity <= 0.0f && r.moon_intensity > 0.0f;
    const Body& light = r.moon_is_dominant ? r.moon : r.sun;
    const Spectrum& direct = r.moon_is_dominant ? moonDirect : sunDirect;
    r.light_dir = light.dir;
    r.light_color = r.moon_is_dominant ? r.moon_color : r.sun_color;
    r.light_intensity = r.moon_is_dominant ? r.moon_intensity : r.sun_intensity;

    // Shadow darkness: fraction of light on a sun-facing surface that comes from the direct
    // beam, normalized so the clear zenith sun gives 1.
    const float directLum = luminance(direct);
    const float skyLum = luminance(sky);
    float zenithSkyLum = 0.0f;
    for (int i = 0; i < 3; ++i) {
        zenithSkyLum += kLuma[i] * zenithSky.c[i] / zenithT.c[i];
    }
    const float refRatio = 1.0f / (1.0f + zenithSkyLum);
    const float ratio = directLum > 0.0f ? directLum / (directLum + skyLum) : 0.0f;
    r.shadow_strength = clamp01(ratio / refRatio);
    if (r.moon_is_dominant) {
        r.shadow_strength *= std::sqrt(r.moon_illuminated_fraction);
    }

    // Shadow tint: channels dominated by the direct beam lose more in shadow.
    float frac[3];
    float sum = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float total = direct.c[i] + sky.c[i];
        frac[i] = total > 1e-8f ? direct.c[i] / total : 0.0f;
        sum += frac[i];
    }
    if (sum > 1e-6f) {
        const float mean = sum / 3.0f;
        r.shadow_tint = Rgb{frac[0] / mean, frac[1] / mean, frac[2] / mean};
    }

    r.horizon_fade = r.light_intensity > 0.0f ? clamp01((r.light_dir.y - 0.03f) / 0.12f) : 0.0f;
    return r;
}

}  // namespace solar
