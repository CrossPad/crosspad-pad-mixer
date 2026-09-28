/**
 * @file PadMixerApp.cpp
 * @brief Pad Mixer -- sixteen channel strips, one per pad, on one screen.
 *
 * DAW-style: a thin track, a flat rectangular cap, the value above and the
 * pad number below in the pad's own colour. 320 px across sixteen strips
 * leaves each one 19 px, which is why the track is 3 px and the cap 13 --
 * wide enough to grab, narrow enough to leave a gap between neighbours.
 *
 * Each fader is the current kit's per-pad volume, applied to the engine as it
 * moves, so it can be ridden against a running sequence. The pad under the
 * finger picks the channel; the sequence playing back never does, and neither
 * does a pad while the encoder is clicked into a fader -- that one stays in
 * hand while the pads are played against it.
 */

#include "crosspad-pad-mixer/PadMixerApp.hpp"
#include "crosspad-pad-mixer/PadMixerPadLogic.hpp"

// `class App` lives in a different header per platform, but both derive from
// crosspad_gui::AppBase<App>, so the lifecycle calls used here are identical.
#if __has_include("pc_stubs/PcApp.hpp")
  #include "pc_stubs/PcApp.hpp"
#else
  #include "crosspad-gui/app/App.hpp"
#endif

#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/pad/IPadMix.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad-gui/widgets/gui_helpers.h"

#include "lvgl.h"

#include <memory>
#include <string>

// Encoder navigation is an embedded-only concern -- the simulator drives the UI
// with a pointer. Snapshot whichever group was active before the app opened and
// restore that, rather than a global launcher group that goes stale whenever the
// launcher is rebuilt.
#ifdef ESP_PLATFORM
extern "C" {
  extern lv_indev_t* indev_encoderTop;
}
#define PAD_MIXER_USE_ENCODER 1
#endif

namespace {

constexpr int kChannels = 16;
constexpr uint32_t kTickMs = 50;

const lv_color_t kBg        = lv_color_hex(0x121212);
const lv_color_t kStrip     = lv_color_hex(0x1c1c1c);
const lv_color_t kStripSel  = lv_color_hex(0x2a2a2a);
const lv_color_t kTrack     = lv_color_hex(0x3a3a3a);
const lv_color_t kFill      = lv_color_hex(0x2f8fd8);
const lv_color_t kCap       = lv_color_hex(0xd8d8d8);
const lv_color_t kText      = lv_color_hex(0xb0b0b0);
const lv_color_t kAccent    = lv_color_hex(0x3fa9f5);

struct Channel {
    lv_obj_t* strip  = nullptr;
    lv_obj_t* value  = nullptr;
    lv_obj_t* fader  = nullptr;
    lv_obj_t* number = nullptr;
};

Channel s_ch[kChannels];
lv_obj_t* s_root = nullptr;
lv_obj_t* s_title = nullptr;
/// The kit the faders show, by path: a kit swapped while the app is open
/// must not leave sixteen faders moving the old kit's numbers.
std::string s_kitPath;
lv_timer_t* s_timer = nullptr;
int s_selected = -1;

std::shared_ptr<PadMixerPadLogic> s_padLogic;

#ifdef PAD_MIXER_USE_ENCODER
lv_group_t* s_encGroup = nullptr;
lv_group_t* s_prevEncGroup = nullptr;
#endif

crosspad::KitInfo* current_kit()
{
    auto* km = crosspad::getKitManager();
    return km ? km->getCurrentKit() : nullptr;
}

/// The two engines count volume differently: 0..127 for a sample kit,
/// 0..100 for a pitched one.
int32_t volume_max(const crosspad::KitInfo* kit)
{
    return (kit && kit->engine == crosspad::KitEngine::Pitched) ? 100 : 127;
}

void select_channel(int idx)
{
    if (idx == s_selected) return;
    if (s_selected >= 0) lv_obj_set_style_bg_color(s_ch[s_selected].strip, kStrip, 0);
    s_selected = idx;
    if (idx >= 0) lv_obj_set_style_bg_color(s_ch[idx].strip, kStripSel, 0);
}

/// Into the kit and straight to whichever engine plays it. Marked modified
/// the way the pad editors mark it, so the Sampler's save prompt offers it.
void apply_volume(int idx, int32_t vol)
{
    crosspad::KitInfo* kit = current_kit();
    if (!kit) return;
    crosspad::Pad& pad = kit->pads[idx];
    pad.volume = vol;
    pad.dirtyModified = true;
    if (auto* mix = crosspad::getPlatformServices().padMix) {
        mix->setPadMix((uint8_t)idx, pad.volume, pad.pan);
    }
}

void fader_changed_cb(lv_event_t* e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t* fader = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int32_t vol = lv_slider_get_value(fader);
    lv_label_set_text_fmt(s_ch[idx].value, "%d", (int)vol);
    apply_volume(idx, vol);
}

void fader_focused_cb(lv_event_t* e)
{
    select_channel((int)(intptr_t)lv_event_get_user_data(e));
}

/// Faders, labels and title from the current kit's pads.
void sync_from_kit()
{
    crosspad::KitInfo* kit = current_kit();
    s_kitPath = kit ? kit->path : std::string();
    if (s_title) lv_label_set_text(s_title, kit ? kit->name.c_str() : "No kit");
    const int32_t max = volume_max(kit);
    for (int i = 0; i < kChannels; ++i) {
        Channel& ch = s_ch[i];
        if (!ch.fader) continue;
        const crosspad::Pad* pad = kit ? &kit->pads[i] : nullptr;
        int32_t vol = pad ? pad->volume : max;
        if (vol < 0) vol = 0;
        if (vol > max) vol = max;
        lv_slider_set_range(ch.fader, 0, max);
        lv_slider_set_value(ch.fader, vol, LV_ANIM_OFF);
        lv_label_set_text_fmt(ch.value, "%d", (int)vol);
        lv_obj_set_style_text_color(ch.number, pad ? lv_color_hex(pad->color) : kText, 0);
    }
}

void tick_cb(lv_timer_t*)
{
    crosspad::KitInfo* kit = current_kit();
    if ((kit ? kit->path : std::string()) != s_kitPath) sync_from_kit();
    if (!s_padLogic) return;
    const int hit = s_padLogic->takeHitPad();
    if (hit < 0 || hit >= kChannels) return;
#ifdef PAD_MIXER_USE_ENCODER
    // A fader being turned keeps the focus: the hit is dropped, not queued.
    if (s_encGroup && lv_group_get_editing(s_encGroup)) return;
    // Through the group, so the encoder then turns the fader that was hit.
    if (s_encGroup) { lv_group_focus_obj(s_ch[hit].fader); return; }
#endif
    select_channel(hit);
}

// Closing the app from inside the app. Never call destroyApp() straight from
// an event callback: it deletes the widget tree this very event belongs to.
void back_cb(lv_event_t* e)
{
    auto* app = static_cast<App*>(lv_event_get_user_data(e));
    if (app) app->destroyAppAsync();
}

lv_obj_t* make_fader(lv_obj_t* parent, int idx, int32_t max, int32_t value)
{
    lv_obj_t* f = lv_slider_create(parent);
    lv_obj_set_width(f, 3);
    lv_obj_set_flex_grow(f, 1);
    lv_slider_set_range(f, 0, max);
    lv_slider_set_value(f, value, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(f, kTrack, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(f, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(f, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(f, kFill, LV_PART_INDICATOR);
    lv_obj_set_style_radius(f, 1, LV_PART_INDICATOR);

    // The cap: the knob grows from the 3 px track by its padding, 5 a side
    // and 3 top and bottom -> 13 x 9, flat like a console fader's.
    lv_obj_set_style_bg_color(f, kCap, LV_PART_KNOB);
    lv_obj_set_style_radius(f, 1, LV_PART_KNOB);
    lv_obj_set_style_pad_left(f, 5, LV_PART_KNOB);
    lv_obj_set_style_pad_right(f, 5, LV_PART_KNOB);
    lv_obj_set_style_pad_top(f, 3, LV_PART_KNOB);
    lv_obj_set_style_pad_bottom(f, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(f, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(f, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(f, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_EDITED);
    lv_obj_set_style_outline_width(f, 0, LV_STATE_FOCUS_KEY);

    // A 3 px target is not something a finger finds; widen the touch area to
    // the strip.
    lv_obj_set_ext_click_area(f, 8);

    lv_obj_add_event_cb(f, fader_changed_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(f, fader_focused_cb, LV_EVENT_FOCUSED, (void*)(intptr_t)idx);
    return f;
}

} // namespace

lv_obj_t* PadMixer_create(lv_obj_t* parent, App* app)
{
#ifdef PAD_MIXER_USE_ENCODER
    if (indev_encoderTop) s_prevEncGroup = lv_indev_get_group(indev_encoderTop);
    if (!s_encGroup) s_encGroup = lv_group_create();
    if (indev_encoderTop) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif

    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, kBg, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 2, 0);
    lv_obj_set_style_pad_row(s_root, 2, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    crosspad::KitInfo* kit = current_kit();
    const int32_t max = volume_max(kit);

    // -- title bar: the way out, and whose pads these are --
    lv_obj_t* bar = lv_obj_create(s_root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 22);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* backBtn = lv_button_create(bar);
    lv_obj_set_size(backBtn, 48, 20);
    lv_obj_set_style_pad_all(backBtn, 0, 0);
    lv_obj_add_event_cb(backBtn, back_cb, LV_EVENT_CLICKED, app);
    lv_obj_t* backLabel = lv_label_create(backBtn);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_10, 0);
    lv_obj_center(backLabel);

    s_title = lv_label_create(bar);
    lv_obj_set_style_text_color(s_title, kText, 0);
    lv_obj_set_flex_grow(s_title, 1);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);

#ifdef PAD_MIXER_USE_ENCODER
    if (s_encGroup) lv_group_add_obj(s_encGroup, backBtn);
#endif

    // -- the console: sixteen strips sharing the width evenly --
    lv_obj_t* console = lv_obj_create(s_root);
    lv_obj_remove_style_all(console);
    lv_obj_set_width(console, LV_PCT(100));
    lv_obj_set_flex_grow(console, 1);
    lv_obj_set_style_pad_column(console, 1, 0);
    lv_obj_set_flex_flow(console, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(console, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < kChannels; ++i) {
        Channel& ch = s_ch[i];
        const crosspad::Pad* pad = kit ? &kit->pads[i] : nullptr;

        ch.strip = lv_obj_create(console);
        lv_obj_remove_style_all(ch.strip);
        lv_obj_set_height(ch.strip, LV_PCT(100));
        lv_obj_set_flex_grow(ch.strip, 1);
        lv_obj_set_style_bg_color(ch.strip, kStrip, 0);
        lv_obj_set_style_bg_opa(ch.strip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ch.strip, 2, 0);
        lv_obj_set_style_pad_ver(ch.strip, 3, 0);
        lv_obj_set_style_pad_row(ch.strip, 3, 0);
        lv_obj_set_flex_flow(ch.strip, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(ch.strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(ch.strip, LV_OBJ_FLAG_SCROLLABLE);

        int32_t vol = pad ? pad->volume : max;
        if (vol < 0) vol = 0;
        if (vol > max) vol = max;

        ch.value = lv_label_create(ch.strip);
        lv_obj_set_style_text_font(ch.value, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(ch.value, kText, 0);
        lv_label_set_text_fmt(ch.value, "%d", (int)vol);

        // Room above and below the track for the cap's overhang.
        lv_obj_t* lane = lv_obj_create(ch.strip);
        lv_obj_remove_style_all(lane);
        lv_obj_set_width(lane, LV_PCT(100));
        lv_obj_set_flex_grow(lane, 1);
        lv_obj_set_style_pad_ver(lane, 5, 0);
        lv_obj_set_flex_flow(lane, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(lane, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(lane, LV_OBJ_FLAG_SCROLLABLE);
        ch.fader = make_fader(lane, i, max, vol);

        ch.number = lv_label_create(ch.strip);
        lv_obj_set_style_text_font(ch.number, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(ch.number,
                                    pad ? lv_color_hex(pad->color) : kText, 0);
        lv_label_set_text_fmt(ch.number, "%d", i + 1);

#ifdef PAD_MIXER_USE_ENCODER
        if (s_encGroup) lv_group_add_obj(s_encGroup, ch.fader);
#endif
    }

    // -- pad logic --
    // Registration only. Activation is framework-owned: AppBase::start() pushes
    // the pad logic named in REGISTER_APP_PL and destroyApp() pops it, so this
    // app must never call setActivePadLogic() itself.
    if (!s_padLogic) {
        s_padLogic = std::make_shared<PadMixerPadLogic>();
        crosspad::getPadManager().registerPadLogic("PadMixer", s_padLogic);
    }

#ifdef PAD_MIXER_USE_ENCODER
    if (s_encGroup) lv_group_focus_obj(backBtn);
#endif

    sync_from_kit();
    s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);

    if (app) {
        app->setOnShow([](lv_obj_t*) {
            if (s_timer) lv_timer_resume(s_timer);
#ifdef PAD_MIXER_USE_ENCODER
            if (indev_encoderTop && s_encGroup) {
                lv_indev_set_group(indev_encoderTop, s_encGroup);
            }
#endif
        });
        app->setOnHide([](lv_obj_t*) {
            if (s_timer) lv_timer_pause(s_timer);
#ifdef PAD_MIXER_USE_ENCODER
            if (indev_encoderTop && s_prevEncGroup) {
                lv_indev_restore_group(indev_encoderTop, s_prevEncGroup);
            }
#endif
        });
    }

    return s_root;
}

void PadMixer_destroy(lv_obj_t* root)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    for (auto& ch : s_ch) ch = Channel{};
    s_selected = -1;
    s_root = nullptr;
    s_title = nullptr;
    s_kitPath.clear();

#ifdef PAD_MIXER_USE_ENCODER
    // Hand navigation back before the group dies, or the launcher comes back
    // unnavigable and only a reboot fixes it.
    if (indev_encoderTop && s_prevEncGroup) {
        lv_indev_restore_group(indev_encoderTop, s_prevEncGroup);
    }
    if (s_encGroup) {
        lv_group_delete(s_encGroup);
        s_encGroup = nullptr;
    }
    s_prevEncGroup = nullptr;
#endif

    lv_obj_delete_async(root);
}

// Auto-registration: the app shows up in the launcher with no wiring anywhere
// else. The last argument names the pad logic registered above.
REGISTER_APP_PL(PadMixer, nullptr, "pad-mixer.png",
                PadMixer_create, PadMixer_destroy,
                nullptr, nullptr, nullptr, 0, "PadMixer")
