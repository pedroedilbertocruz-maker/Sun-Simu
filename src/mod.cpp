// Sun Simulation mod.
//
// Replaces the game's fixed sun arc with a physically based sun and moon: position from latitude,
// season and the in-game clock (with atmospheric refraction), and color/intensity from a simple
// Rayleigh + Mie + ozone atmosphere. The result is exported as SunService so other lighting mods
// (shadow_mod, ao_mod, ...) can share one light, and is optionally written back into the game's
// sun/moon positions so the sky sprite, lens flare and the game's own base light follow it.

#include "global.h"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "f_op/f_op_camera_mng.h"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include "solar.hpp"
#include "sun_mod/sun_service.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

DEFINE_MOD();
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(LogService, svc_log);

namespace {

// Distance at which the game places the sun/moon sprites (see dScnKy_env_light_c::setSunpos).
constexpr float kSkyDistance = 80000.0f;

ConfigVarHandle g_cvarEnabled = 0;
ConfigVarHandle g_cvarDriveGame = 0;
ConfigVarHandle g_cvarLatitude = 0;
ConfigVarHandle g_cvarDayOfYear = 0;
ConfigVarHandle g_cvarDaysPerGameDay = 0;
ConfigVarHandle g_cvarNorthYaw = 0;
ConfigVarHandle g_cvarTurbidity = 0;  // x10
ConfigVarHandle g_cvarMoonPhase = 0;
ConfigVarHandle g_cvarMoonCycle = 0;
ConfigVarHandle g_cvarMoonBrightness = 0;  // percent of the zenith sun

UiWindowHandle g_controlsWindow = 0;

DEFINE_HOOK(&dScnKy_env_light_c::setSunpos, EnvSetSunpos);
DEFINE_HOOK(&dScnKy_env_light_c::SetBaseLight, EnvSetBaseLight);

int64_t get_int(ConfigVarHandle handle, int64_t fallback) {
    int64_t value = fallback;
    if (handle == 0 || svc_config->get_int(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

bool get_bool(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

bool simulation_enabled() {
    return get_bool(g_cvarEnabled, true);
}

bool drives_game() {
    return simulation_enabled() && get_bool(g_cvarDriveGame, true);
}

float game_clock_hours() {
    const float degrees = dComIfGs_getTime();  // 15 degrees per hour
    if (!std::isfinite(degrees)) {
        return 12.0f;
    }
    float hours = std::fmod(degrees / 15.0f, 24.0f);
    return hours < 0.0f ? hours + 24.0f : hours;
}

float game_days_elapsed() {
    return static_cast<float>(dComIfGs_getDate());
}

solar::Params build_params(float timeHours) {
    solar::Params p;
    p.time_hours = timeHours;
    p.latitude_deg = static_cast<float>(std::clamp<int64_t>(get_int(g_cvarLatitude, 35), -89, 89));
    p.north_yaw_deg = static_cast<float>(std::clamp<int64_t>(get_int(g_cvarNorthYaw, 0), 0, 359));
    p.turbidity = static_cast<float>(std::clamp<int64_t>(get_int(g_cvarTurbidity, 25), 10, 100)) / 10.0f;
    p.moon_brightness =
        static_cast<float>(std::clamp<int64_t>(get_int(g_cvarMoonBrightness, 8), 0, 100)) / 100.0f;

    const float days = game_days_elapsed();
    const float baseDay = static_cast<float>(std::clamp<int64_t>(get_int(g_cvarDayOfYear, 128), 1, 365));
    const float seasonRate =
        static_cast<float>(std::clamp<int64_t>(get_int(g_cvarDaysPerGameDay, 0), 0, 30));
    p.day_of_year = std::fmod(baseDay - 1.0f + days * seasonRate, 365.0f) + 1.0f;

    float phase = static_cast<float>(std::clamp<int64_t>(get_int(g_cvarMoonPhase, 15), 0, 29));
    if (get_bool(g_cvarMoonCycle, true)) {
        phase += days + timeHours / 24.0f;
    }
    p.moon_phase_days = phase;
    return p;
}

void copy3(float out[3], const solar::Vec3& v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

void copy3(float out[3], const solar::Rgb& c) {
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
}

void fill_state(const solar::Params& p, const solar::Result& r, SunState* out) {
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(SunState));
    out->struct_size = size;

    uint32_t flags = 0;
    if (dKy_SunMoon_Light_Check()) {
        flags |= SUN_STATE_OUTDOOR;
    }
    if (r.moon_is_dominant) {
        flags |= SUN_STATE_MOON_DOMINANT;
    }
    if (dKy_darkworld_check()) {
        flags |= SUN_STATE_DARK_WORLD;
    }
    if (drives_game()) {
        flags |= SUN_STATE_DRIVES_GAME;
    }
    out->flags = flags;

    out->time_hours = p.time_hours;
    out->day_of_year = p.day_of_year;
    out->latitude_deg = p.latitude_deg;

    out->sun_elevation_deg = r.sun.elevation_deg;
    out->sun_azimuth_deg = r.sun.azimuth_deg;
    copy3(out->sun_dir, r.sun.dir);
    copy3(out->sun_color, r.sun_color);
    out->sun_intensity = r.sun_intensity;

    out->moon_elevation_deg = r.moon.elevation_deg;
    out->moon_azimuth_deg = r.moon.azimuth_deg;
    copy3(out->moon_dir, r.moon.dir);
    copy3(out->moon_color, r.moon_color);
    out->moon_intensity = r.moon_intensity;
    out->moon_illuminated_fraction = r.moon_illuminated_fraction;

    copy3(out->light_dir, r.light_dir);
    copy3(out->light_color, r.light_color);
    out->light_intensity = r.light_intensity;

    copy3(out->sky_color, r.sky_color);
    out->sky_intensity = r.sky_intensity;

    out->shadow_strength = r.shadow_strength;
    copy3(out->shadow_tint, r.shadow_tint);
    out->horizon_fade = r.horizon_fade;
}

ModResult evaluate_into(float timeHours, SunState* out) {
    if (out == nullptr || out->struct_size < sizeof(SunState)) {
        return MOD_INVALID_ARGUMENT;
    }
    if (!simulation_enabled()) {
        return MOD_UNAVAILABLE;
    }
    const solar::Params params = build_params(timeHours);
    fill_state(params, solar::evaluate(params), out);
    return MOD_OK;
}

ModResult svc_get_state(ModContext*, SunState* out) {
    return evaluate_into(game_clock_hours(), out);
}

ModResult svc_evaluate_at(ModContext*, float timeHours, SunState* out) {
    if (!std::isfinite(timeHours)) {
        return MOD_INVALID_ARGUMENT;
    }
    return evaluate_into(timeHours, out);
}

// --- Game integration -------------------------------------------------------------------------

// Mirrors the guard at the top of dScnKy_env_light_c::setSunpos: the original only writes the
// positions when a camera exists and outside the F_SP200 stage.
camera_process_class* sky_camera() {
    camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr || std::strcmp(dComIfGp_getStartStageName(), "F_SP200") == 0) {
        return nullptr;
    }
    return camera;
}

void on_set_sunpos_post(ModContext*, void* args, void*, void*) {
    if (!drives_game()) {
        return;
    }
    camera_process_class* camera = sky_camera();
    if (camera == nullptr) {
        return;
    }
    auto* env = mods::arg<dScnKy_env_light_c*>(args, 0);
    const solar::Result r = solar::evaluate(build_params(game_clock_hours()));
    const cXyz& eye = camera->view.lookat.eye;

    // sun_pos is absolute; moon_pos is stored relative to the camera like the original.
    env->sun_pos.x = eye.x + r.sun.dir.x * kSkyDistance;
    env->sun_pos.y = eye.y + r.sun.dir.y * kSkyDistance;
    env->sun_pos.z = eye.z + r.sun.dir.z * kSkyDistance;
    env->moon_pos.x = r.moon.dir.x * kSkyDistance;
    env->moon_pos.y = r.moon.dir.y * kSkyDistance;
    env->moon_pos.z = r.moon.dir.z * kSkyDistance;
}

// SetBaseLight picks sun or moon from a fixed daytime window; use the simulated dominant body
// instead so the game's own lighting agrees with the sun that is actually up.
void on_set_base_light_post(ModContext*, void* args, void*, void*) {
    if (!drives_game() || !dKy_SunMoon_Light_Check()) {
        return;
    }
    camera_process_class* camera = sky_camera();
    if (camera == nullptr) {
        return;
    }
    auto* env = mods::arg<dScnKy_env_light_c*>(args, 0);
    const solar::Result r = solar::evaluate(build_params(game_clock_hours()));
    const cXyz& eye = camera->view.lookat.eye;
    env->base_light.mPosition.x = eye.x + r.light_dir.x * kSkyDistance;
    env->base_light.mPosition.y = eye.y + r.light_dir.y * kSkyDistance;
    env->base_light.mPosition.z = eye.z + r.light_dir.z * kSkyDistance;
}

// --- UI ---------------------------------------------------------------------------------------

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    add_control(pane, control);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle cvar, int64_t min,
    int64_t max, int64_t step, const char* suffix, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.min = min;
    control.max = max;
    control.step = step;
    control.suffix = suffix;
    add_control(pane, control);
}

void on_log_state(ModContext*, void*) {
    SunState s = SUN_STATE_INIT;
    if (svc_get_state(mod_ctx, &s) != MOD_OK) {
        svc_log->info(mod_ctx, "sun simulation is disabled");
        return;
    }
    char line[512];
    std::snprintf(line, sizeof(line),
        "%05.2fh day %.0f lat %.0f | sun elev %.1f az %.1f I %.3f rgb(%.2f %.2f %.2f) | "
        "moon elev %.1f I %.4f lit %.0f%% | sky I %.4f rgb(%.2f %.2f %.2f) | shadow %.2f "
        "tint(%.2f %.2f %.2f) fade %.2f | %s%s",
        s.time_hours, s.day_of_year, s.latitude_deg, s.sun_elevation_deg, s.sun_azimuth_deg,
        s.sun_intensity, s.sun_color[0], s.sun_color[1], s.sun_color[2], s.moon_elevation_deg,
        s.moon_intensity, s.moon_illuminated_fraction * 100.0f, s.sky_intensity, s.sky_color[0],
        s.sky_color[1], s.sky_color[2], s.shadow_strength, s.shadow_tint[0], s.shadow_tint[1],
        s.shadow_tint[2], s.horizon_fade, (s.flags & SUN_STATE_MOON_DOMINANT) ? "moon" : "sun",
        (s.flags & SUN_STATE_OUTDOOR) ? ", outdoor" : ", indoor");
    svc_log->info(mod_ctx, line);
}

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;

    svc_ui->pane_add_section(mod_ctx, left, "Sun");
    add_toggle(left, "Enabled", g_cvarEnabled,
        "Runs the simulation and publishes it to other lighting mods.");
    add_toggle(left, "Drive Game Sun", g_cvarDriveGame,
        "Moves the game's sun and moon (sky sprites, lens flare, base light) to the simulated "
        "positions.");

    svc_ui->pane_add_section(mod_ctx, left, "Location & Season");
    add_number(left, "Latitude", g_cvarLatitude, -89, 89, 1, " deg",
        "Positive is north. Higher latitudes give lower suns and longer summer days.");
    add_number(left, "Day of Year", g_cvarDayOfYear, 1, 365, 1, nullptr,
        "Sets the season: 80 = March equinox, 172 = June solstice, 355 = December solstice. "
        "The default (128) matches the game's original sunrise and sunset times.");
    add_number(left, "Season Speed", g_cvarDaysPerGameDay, 0, 30, 1, " days",
        "Calendar days that pass per in-game day. 0 keeps the season fixed.");
    add_number(left, "North Direction", g_cvarNorthYaw, 0, 359, 5, " deg",
        "Rotates the compass in world space. 0 keeps the game's layout (sun rises toward +X).");

    svc_ui->pane_add_section(mod_ctx, left, "Atmosphere");
    add_number(left, "Turbidity", g_cvarTurbidity, 10, 100, 5, " x0.1",
        "Haze in the air. 10 = pristine, 20-30 = clear day, 60+ = hazy. More haze gives redder "
        "sunsets, softer shadows and a whiter sky.");

    svc_ui->pane_add_section(mod_ctx, left, "Moon");
    add_number(left, "Moon Phase", g_cvarMoonPhase, 0, 29, 1, " days",
        "Days since new moon: 0 = new, 7 = first quarter, 15 = full, 22 = last quarter.");
    add_toggle(left, "Moon Cycle", g_cvarMoonCycle, "Advances the phase one day per in-game day.");
    add_number(left, "Moon Brightness", g_cvarMoonBrightness, 0, 100, 1, "%",
        "Full-moon light relative to the midday sun. Real moonlight is far dimmer; games usually "
        "exaggerate it so nights stay playable.");

    svc_ui->pane_add_section(mod_ctx, left, "Debug");
    UiControlDesc logControl = UI_CONTROL_DESC_INIT;
    logControl.kind = UI_CONTROL_BUTTON;
    logControl.label = "Log Current Sun State";
    logControl.help_rml = "Writes position, color, intensity and shadow values to the console.";
    logControl.on_pressed = on_log_state;
    add_control(left, logControl);
    return MOD_OK;
}

void on_controls_window_closed(ModContext*, UiWindowHandle, void*) {
    g_controlsWindow = 0;
}

void on_open_controls(ModContext*, void*) {
    if (g_controlsWindow != 0) {
        return;
    }
    UiTabDesc tabs[1] = {UI_TAB_DESC_INIT};
    tabs[0].title = "Controls";
    tabs[0].build = build_controls_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = on_controls_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_controlsWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open sun controls window");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    add_toggle(panel, "Enabled", g_cvarEnabled, nullptr);

    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Controls";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
    return MOD_OK;
}

ModResult register_bool(const char* name, bool def, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = def;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register sun option");
    }
    return MOD_OK;
}

ModResult register_int(const char* name, int64_t def, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = def;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register sun option");
    }
    return MOD_OK;
}

constexpr SunService g_sunService{
    .header = SERVICE_HEADER(SunService, SUN_SERVICE_MAJOR, SUN_SERVICE_MINOR),
    .get_state = svc_get_state,
    .evaluate_at = svc_evaluate_at,
};
EXPORT_SERVICE(g_sunService);

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    struct BoolOpt {
        const char* name;
        bool def;
        ConfigVarHandle* handle;
    };
    struct IntOpt {
        const char* name;
        int64_t def;
        ConfigVarHandle* handle;
    };
    const BoolOpt bools[] = {
        {"effectEnabled", true, &g_cvarEnabled},
        {"driveGameSun", true, &g_cvarDriveGame},
        {"moonCycle", true, &g_cvarMoonCycle},
    };
    const IntOpt ints[] = {
        {"latitude", 35, &g_cvarLatitude},
        {"dayOfYear", 128, &g_cvarDayOfYear},
        {"daysPerGameDay", 0, &g_cvarDaysPerGameDay},
        {"northYaw", 0, &g_cvarNorthYaw},
        {"turbidity", 25, &g_cvarTurbidity},
        {"moonPhase", 15, &g_cvarMoonPhase},
        {"moonBrightness", 8, &g_cvarMoonBrightness},
    };
    for (const auto& opt : bools) {
        if (const ModResult r = register_bool(opt.name, opt.def, *opt.handle, error); r != MOD_OK) {
            return r;
        }
    }
    for (const auto& opt : ints) {
        if (const ModResult r = register_int(opt.name, opt.def, *opt.handle, error); r != MOD_OK) {
            return r;
        }
    }

    if (mods::hook::add_post<EnvSetSunpos>(on_set_sunpos_post) != MOD_OK ||
        mods::hook::add_post<EnvSetBaseLight>(on_set_base_light_post) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to hook the game's sun");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    // Hooks are removed by the loader; the game recomputes sun_pos/moon_pos every frame, so the
    // original arc returns on its own.
    g_cvarEnabled = g_cvarDriveGame = g_cvarLatitude = g_cvarDayOfYear = 0;
    g_cvarDaysPerGameDay = g_cvarNorthYaw = g_cvarTurbidity = 0;
    g_cvarMoonPhase = g_cvarMoonCycle = g_cvarMoonBrightness = 0;
    g_controlsWindow = 0;
    return MOD_OK;
}
}
