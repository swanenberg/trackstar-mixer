#pragma once

#include <QColor>
#include <QString>

/// TrackStar tuning knobs: lazily created, persistent [TrackStar] controls.
///
/// Each knob is a ControlObject in the "[TrackStar]" group that is created on
/// first use with the given default, persisted in mixxx.cfg on exit and
/// readable/settable at runtime through the TrackStar bridge
/// ({"cmd":"get"/"set","group":"[TrackStar]","key":...}). Renderers read
/// them every frame, so reads are a single hash lookup.
namespace trackstar {

/// Value of the knob, creating it with `defaultValue` if it does not exist yet.
/// Must be called from the main thread the first time (control creation).
double tuning(const QString& key, double defaultValue);

/// The colour a hotcue (button, waveform/overview mark) or stem label is shown in:
/// neutral white while [TrackStar],mono_colors is on (default), the track's own colour otherwise.
/// Keeps the Mixer calm: one accent colour, the rest greys and white.
QColor displayColor(const QColor& trackColor);

} // namespace trackstar
