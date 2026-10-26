/* Sun Simulation service: physically based sun/moon position, color and intensity.
 *
 * Provided by sun_mod. Lighting mods (shadows, AO, relighting...) import it to agree on where the
 * light comes from and what it looks like:
 *
 *     #include "sun_mod/sun_service.h"
 *     IMPORT_OPTIONAL_SERVICE(SunService, svc_sun);
 *
 *     SunState sun = SUN_STATE_INIT;
 *     if (svc_sun != NULL && svc_sun->get_state(mod_ctx, &sun) == MOD_OK) { ... }
 *
 * get_state returns MOD_UNAVAILABLE while the simulation is disabled in its mod panel, so callers
 * can fall back to the game's own sun.
 *
 * World space follows the game: +Y up; with the default compass +X is east and +Z is south.
 * Colors are linear RGB normalized so the largest channel is 1; intensities are relative to the
 * clear zenith sun (1.0).
 *
 * Within major version 1 fields are only ever appended to SunState and SunService.
 */
#pragma once

#include "mods/api.h"

#include <stdint.h>

#define SUN_SERVICE_ID "com.sunsim.sun"
#define SUN_SERVICE_MAJOR 1u
#define SUN_SERVICE_MINOR 0u

enum {
    SUN_STATE_OUTDOOR = 1u << 0,       /* the stage uses sun/moon lighting (dKy_SunMoon_Light_Check) */
    SUN_STATE_MOON_DOMINANT = 1u << 1, /* light_* describes the moon (sun is below the horizon) */
    SUN_STATE_DARK_WORLD = 1u << 2,    /* Twilight Realm is active */
    SUN_STATE_DRIVES_GAME = 1u << 3,   /* the game's sun/moon/base light positions are overridden */
};

typedef struct SunState {
    uint32_t struct_size; /* set by the caller (SUN_STATE_INIT) */
    uint32_t flags;       /* SUN_STATE_* */

    float time_hours;   /* clock used for this evaluation, 0..24 */
    float day_of_year;  /* 1..365 */
    float latitude_deg; /* + north */

    float sun_elevation_deg; /* apparent, includes refraction */
    float sun_azimuth_deg;   /* from north, clockwise toward east */
    float sun_dir[3];        /* world space, normalized, toward the sun */
    float sun_color[3];
    float sun_intensity;

    float moon_elevation_deg;
    float moon_azimuth_deg;
    float moon_dir[3];
    float moon_color[3];
    float moon_intensity;
    float moon_illuminated_fraction;

    /* Dominant light (sun while up, otherwise moon). Use this for shadows and direct shading. */
    float light_dir[3];
    float light_color[3];
    float light_intensity;

    /* Diffuse skylight on a horizontal surface (ambient term), including twilight. */
    float sky_color[3];
    float sky_intensity;

    /* Suggested darkness of a fully occluded point, 0..1 (1 = clear zenith sun). */
    float shadow_strength;
    /* Per-channel shadow darkening weights with mean 1 (shadows lean toward the sky's color). */
    float shadow_tint[3];
    /* 0..1 fade for shadow casting as the dominant light nears the horizon. */
    float horizon_fade;
} SunState;

#define SUN_STATE_INIT {sizeof(SunState), 0u}

typedef struct SunService {
    ServiceHeader header;

    /* State for the current game clock and the user's settings. */
    ModResult (*get_state)(ModContext* ctx, SunState* out);

    /* Same, at an arbitrary clock time (hours, 0..24). Useful for previews and look-ahead. */
    ModResult (*evaluate_at)(ModContext* ctx, float time_hours, SunState* out);
} SunService;

#ifdef __cplusplus
#include "mods/service.hpp"
template <>
struct mods::ServiceTraits<SunService> {
    static constexpr const char* id = SUN_SERVICE_ID;
    static constexpr uint16_t major_version = SUN_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = SUN_SERVICE_MINOR;
};
#endif
