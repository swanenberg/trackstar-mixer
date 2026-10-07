# TangoBigBPM — Mixxx skin

A clean, djay-Pro-style Mixxx skin derived from the built-in **Tango** skin, tailored for a
Pioneer **DDJ-FLX4** (everything that lives on the controller is hidden in the UI).

## Highlights

- Mixer/EQ/faders/crossfader/hotcues/loops/key/sampler/mic/preview/VU hidden — the
  FLX4 handles those physically. Tempo/rate got its own compact in-UI row again
  (`decks/tempo_row.xml`) so the tempo is adjustable without the controller.
- The spinny is kept, with a **big teal BPM number directly underneath** (doubles as a
  sync button). Next to it the **track key** (click = `sync_key`, right-click =
  `reset_key`).
- Vertical three-column layout: deck 1 · tracklist · deck 2, waveforms full-width below.
- Stem controls (Mixxx 2.6+ `.stem.mp4`) and a compact 4-pad sampler strip.

## Install

Copy this folder to your Mixxx user skins directory:

```
~/Library/Containers/org.mixxx.mixxx/Data/Library/Application Support/Mixxx/skins/TangoBigBPM/
```

Then select it in Mixxx → Preferences → Interface, or set in `mixxx.cfg`:

```
[Config]
ResizableSkin = TangoBigBPM
```

> Note: use the `[Config] ResizableSkin` key, **not** the legacy `[Config] Skin` key
> (the latter only maps a few built-in skins and falls back to LateNight otherwise).
> Keep the unique name — naming a user skin the same as a built-in ("Tango") causes a
> conflict and falls back to LateNight.

---

# Development notes (reference)

This is a **legacy XML skin** (not the QML skin in `mixxx-src/res/qml`). Verified on
Mixxx 2.7-alpha; config/skins are shared with 2.5/2.6 (same bundle id `org.mixxx.mixxx`).

## Layout architecture

- `skin.xml` `MainArea`, top strip is **horizontal: deck 1 | tracklist (expanding) | deck 2**;
  each deck is a narrow vertical `DeckZone` column (`MinimumSize 230`, `MaximumSize 260`).
  Decks 3/4 stack below 1/2 in the same column (visible with `show_4decks`). Waveforms are
  full-width at the bottom (height capped via `MaximumSize` on their WidgetGroup).
- The active per-deck template is **`decks/deck_right.xml`**, called directly from `skin.xml`
  with the `chanNum` variable. `decks_12.xml` / `decks_34.xml` are **unused** leftovers.
- `deck_right.xml` is a vertical column: spinny + big BPM (`decks/spinny_cover_maxi.xml`,
  wrapper `SpinnyWell`, `MinimumSize 120,158` — without it the BPM row gets clipped) →
  artist/title (`row_text_left.xml`, active because `[Tango] symmetric_time=0`) → overview →
  FX indicator → hotcues → stem strip → horizontal VU → expanding bottom spacer (keeps
  BPM + Play visible). Round Play (`#SpinnyPlayButton`) and Cue (`#SpinnyCueButton`) sit
  **beside** the spinny; remaining time (`#PlayPosition`, 13px) under the CUE column.
- Custom templates: `decks/stem_strip.xml` + `stem_row.xml` (visible when
  `[ChannelN],stem_count` > 0; mute `[ChannelN_StemM],mute`, volume slider),
  `decks/sampler_cell.xml` + `sampler_strip4.xml` (4-pad sampler strip, gated on
  `[Skin],show_samplers`), `decks/deck_fx_indicator.xml` (per-deck FX pill, visible via
  `[EffectRack1_EffectUnit1],group_[ChannelN]_enable`), `decks/crossfader_bar.xml`
  (`[Master],crossfader`, always visible, reuses the stem-volume SVGs — that slider's whole
  look lives in `knobs_sliders/stem_volume_{handle,scale}.svg`, it has no QSS),
  `decks/row_transport_column.xml` and `decks/hotcues_deck_column.xml` (8 hotcues, 2×4),
  `decks/tempo_row.xml` (per-deck tempo: `rate_perm_down` btn · horizontal `[ChannelN],rate`
  slider (right-click = reset, reuses the stem-volume SVGs) · `rate_perm_up` btn ·
  `WNumberRate` %-display; always visible — NOT gated on `[Skin],show_rate_controls`,
  which persists as 0 in mixxx.cfg).
- Effect rack `fx_units_12.xml` is in the MiddleStrip, gated on `[Skin],show_4effectunits`;
  the **"FX" button** in `library.xml`'s top bar toggles it. `skin.xml` sets
  `[EffectRack1_EffectUnit2],group_[MasterOutput]_enable` to 1 at every start
  (`persist="false"`) = master FX/limiter unit. The correct master-routing control is
  `group_[MasterOutput]_enable`; `group_[Master]_enable` is an old unused alias.
- Signal colors: `deck_right.xml` is called without `SignalColor_12` vars — overview and
  mini-spinny get colors from the `DeckOverviewSingleton`/`SpinnyCoverMini_Singleton`
  definitions at the top of `skin.xml`.

## XML gotchas (hard-won)

- Internal references must use **`skin:/...`** (current skin dir). `skins:<name>/...` only
  searches the *system* skin dir → "Could not open template file".
- **`stacked` z-order: FIRST child is ON TOP (catches clicks), LAST child is the
  background.** Confirmed against built-in Tango `fx/toggle_selector.xml`. Pattern:
  invisible clickable overlay first, colored background last. This caused the
  "loop hotcues only worked late in the track" bug: `controls/button_hotcue_deck.xml` had
  the `<HotcueButton>` first (on top), so clicks fired `hotcue_X_activate` (arms a saved
  loop, no jump) instead of the `gotoandplay`/`gotoandloop` overlays underneath.
- **The spinny is a native GL window** (`WSpinny → WGLWidget` wraps a `QOpenGLWindow` via
  `createWindowContainer` with Qt6/`MIXXX_USE_QOPENGL`): it always paints opaquely on top
  of sibling widgets. Widgets in/over the spinny rectangle **never render** — put buttons
  outside it, replace the spinny instead of overlaying.
- Big BPM = sync button: `#BpmHugeSync` stacked group in `spinny_cover_maxi.xml` — click
  layer (left = `sync_enabled`, right = `beatsync_tempo`) plus `<Number>` `#BpmHuge`. Use a
  **neutral ObjectName** for the click layer (`BpmSyncClick`): `SyncButtonOverlay` inherits
  QSS that draws a sync glyph + orange border over the number.
- Sizes: `<Size>180me,1me</Size>` on the overview made it a 1px sliver → use a fixed
  height (`40f`/`56f`). Expanding play button: `<Size>1me,46f</Size>` works directly on
  `button_2state_right_display.xml`; icon stays centered via `STRETCH_ASPECT`.
- **Horizontal VU**: legacy `<VuMeter>` with the vertical `vumeter_level.png` renders
  vertically even with `<Horizontal>true</Horizontal>`. Needs `scalemode="STRETCH"` on
  PathVu/PathBack **and** `<MaximumSize>10000,6</MaximumSize>`; use `[ChannelN],vu_meter`.
  Pattern copied from `Deere/vumeter_h.xml`. Mixxx 2.7: `<VuMeter>` rejects `<TooltipId>`
  ("Invalid TooltipId vumeter", widget skipped) — leave it out.
- Waveform playhead centering: the 14px `#BeatgridButtonToggler` + 1px spacer right of the
  waveforms offsets the playhead ~7.5px; a mirror `15f,1me` spacer on the left in
  `waveforms_container.xml` restores symmetry.
- **`<Key>` (WKey) gotchas** (2.7-alpha, hard-won 2026-07-18): (1) WKey reads the key from
  its own `[group],key` proxy — it needs an explicit `<Group>` (or valid `<Channel>`); the
  skin `<Connection>` only triggers the refresh. (2) Connect to **`,key`** — a `visual_key`
  control does NOT exist (LateNight's `visual_key` connection is a dead leftover); without a
  changing connection the widget never updates after construction. (3) With
  `key_colors_enabled 1` WKey's custom paintEvent renders **nothing at all on macOS**
  (no bar, no text — upstream bug, mixxx#15351-family), so keep `[Config]
  key_colors_enabled 0` in mixxx.cfg. A skin-side colored key bar was built and removed
  again on request; the working pattern, should it return: display-PushButton
  `NumberStates 25` connected to `,key`, per-`displayValue` QSS background colors keyed
  by OpenKey number (numeric 1-12 = major, 13-24 = minor, major & relative minor share a
  color), Mixxx Key Colors palette in `predefinedcolorpalettes.cpp`. (4) Key-sync
  click layer: same stacked pattern as `#BpmSyncClick` — overlay left = `sync_key`
  (transpose to compatible key of other deck), right = `reset_key`, neutral ObjectName
  `KeySyncClick`. (5) QSS: Qt 6.9 ignores `WKey#KeyHuge`-style selectors (mixxx#15351) —
  the working rule is the bare `WKey { ... }` block at the end of style.qss.

## QSS gotchas

- `#BpmHuge { color }` is overridden by a generic `WNumber` rule — use the **typed
  selector** `WNumber#BpmHuge { ... }` (higher specificity). Same idea: `#DeckN WLabel`
  beats `#TrackTitle`; use `#DeckN #TrackTitle`. `#TrackArtist` needs an explicit `color`
  (else dark-on-dark/invisible).
- The July 2026 restyle (cool blue-grey palette, card-style `#DeckZone`, gradient on
  `#Mixxx` with `#SkinContainer` transparent, `SpinnyWell` dark inset that also hides the
  black GL square) lives as an **override block at the end of `style.qss`**
  ("Restyle juli 2026"). QSS margin on a WWidgetGroup acts as an outer gutter.
- `#SourcesToggle` is shared by the Menu **and** FX buttons (same ObjectName).

## Config the skin depends on (`mixxx.cfg`)

`[Skin]` toggles: `show_samplers 1`, `show_4decks 0`, `show_xfader 0`, `show_starrating 0`,
`show_intro_outro_cues 0` (hides waveform marks *and* buttons), `show_8_hotcues 0`
(4-hotcue group active), `show_4effectunits` (FX button). `[Tango] show_sources` =
sources fly-out ("Menu" button). Note: the 2.6→2.7 config migration reset
`show_samplers` to 1. Hidden library columns are serialized header state, not config —
right-click the column header.

## Verify a change

1. `xmllint --noout` the edited XML (or `mixxx_skin_check.py` in `~/.claude/scripts/`).
2. Restart Mixxx (skin reload = restart; log rotates per start: `mixxx.log` → `.1`).
3. `mixxx.log` must say **"Loaded skin TangoBigBPM"** — LateNight fallback = parse error
   or name conflict. Also grep "Skin parsing failed" / "Could not open template".
4. Screenshot check: give Mixxx ~8 s to render (`screencapture -o`, crop with `sips`).
   Careful: `open -a Mixxx <file>` loads **and starts playback**.
