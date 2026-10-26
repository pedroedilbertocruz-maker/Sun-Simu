// Physically based sun/moon model used by sun_mod.
//
// Pure math, no game dependencies, so it can be unit tested on its own.
//
// Coordinate conventions (matching the game's own sun arc in dScnKy_env_light_c::setSunpos):
//   +Y is up, +X is east and +Z is south (north is -Z) before the configurable `north_yaw`
//   rotation is applied around +Y.
#pragma once

namespace solar {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

struct Params {
    float time_hours = 12.0f;    // local solar time, 0..24
    float day_of_year = 172.0f;  // 1..365 (172 ~ June solstice)
    float latitude_deg = 35.0f;  // + north, - south
    float north_yaw_deg = 0.0f;  // rotates the compass around +Y in world space
    float turbidity = 2.5f;      // 1 = pristine air, 2-3 = clear, 6+ = hazy
    float moon_phase_days = 14.77f;  // 0 = new moon, ~14.77 = full moon, 29.53 = new again
    float moon_brightness = 0.08f;   // full-moon illuminance relative to the zenith sun
};

struct Body {
    float elevation_deg = 0.0f;  // apparent elevation (includes refraction)
    float azimuth_deg = 0.0f;    // from north, clockwise toward east
    Vec3 dir;                    // world space, normalized, pointing *toward* the body
};

struct Result {
    Body sun;
    Body moon;

    // Direct sunlight after the atmosphere. Color is normalized so its largest channel is 1;
    // intensity is relative illuminance on a surface facing the sun (1 = zenith sun, same air).
    Rgb sun_color;
    float sun_intensity = 0.0f;

    // Moonlight, already scaled by phase and moon_brightness.
    Rgb moon_color;
    float moon_intensity = 0.0f;
    float moon_illuminated_fraction = 0.0f;

    // Diffuse skylight on a horizontal surface (includes twilight glow). Color normalized like
    // sun_color; intensity in the same units as sun_intensity.
    Rgb sky_color;
    float sky_intensity = 0.0f;

    // Dominant light for shading/shadows: the sun while it is up, otherwise the moon.
    bool moon_is_dominant = false;
    Vec3 light_dir;
    Rgb light_color;
    float light_intensity = 0.0f;

    // How dark a fully occluded point should be compared with the zenith-sun reference (0..1):
    // ratio of direct to total light on the ground, normalized so the clear zenith sun is 1.
    float shadow_strength = 0.0f;

    // Per-channel darkening weights with mean 1. Channels where direct light dominates are
    // darkened more in shadow, so shadows pick up the sky's color (blue by day, neutral at dusk).
    Rgb shadow_tint{1.0f, 1.0f, 1.0f};

    // 0..1 fade for shadow casting as the dominant light approaches the horizon.
    float horizon_fade = 0.0f;
};

// Sun declination in degrees for a day of the year.
float declination_deg(float day_of_year);

// Geometric elevation -> apparent elevation (Bennett's refraction formula), degrees.
float refracted_elevation_deg(float geometric_deg);

// Relative optical air mass for an apparent elevation (Kasten & Young 1989).
float air_mass(float elevation_deg);

// Solves the position of a body with the given declination and hour angle.
Body body_position(float declination, float hour_angle_deg, float latitude_deg, float north_yaw_deg);

// Full evaluation.
Result evaluate(const Params& params);

// "Sun heat": multipliers for the game's key sunlight color. `heat` is the user setting (0..1) and
// `sun_intensity` the simulated direct sunlight (0 at night, ~1 at midday), so the effect follows
// the sun and vanishes after dusk. Brightens the light and pushes it toward warm yellow-orange.
Rgb heat_multipliers(float heat, float sun_intensity);

}  // namespace solar
