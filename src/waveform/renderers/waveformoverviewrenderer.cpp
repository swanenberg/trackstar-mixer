#include "waveformoverviewrenderer.h"

#include <QPainter>
#include <algorithm>
#include <cmath>
#include <vector>

#include "util/colorcomponents.h"
#include "util/math.h"
#include "util/timer.h"
#include "trackstar/tuning.h"
#include "waveform/renderers/waveformsignalcolors.h"

namespace waveformOverviewRenderer {

QImage render(ConstWaveformPointer pWaveform,
        mixxx::OverviewType type,
        const WaveformSignalColors& signalColors,
        bool mono) {
    const int dataSize = pWaveform->getDataSize();
    if (dataSize <= 0) {
        return QImage();
    }

    QImage image(dataSize / 2, 2 * 255, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(0, 0, 0, 0).value());

    QPainter painter(&image);
    painter.translate(0.0, static_cast<double>(image.height()) / 2.0);

    if (type == mixxx::OverviewType::HSV) {
        drawWaveformPartHSV(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    } else if (type == mixxx::OverviewType::Filtered &&
            trackstar::tuning(QStringLiteral("energy_overview"), 1.0) > 0.0) {
        // TrackStar: energy curve instead of the (flat, limited) waveform
        drawWaveformPartEnergy(&painter,
                pWaveform,
                dataSize,
                signalColors,
                mono);
    } else if (type == mixxx::OverviewType::Filtered) {
        drawWaveformPartLMH(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    } else {
        drawWaveformPartRGB(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    }

    // Evaluate waveform ratio peak
    float peak = 1;
    for (int i = 0; i < dataSize; i += 2) {
        peak = math_max3(
                peak,
                static_cast<float>(pWaveform->getAll(i)),
                static_cast<float>(pWaveform->getAll(i + 1)));
    }
    // Normalize
    float diffGain = 0;
    if (peak > 1) {
        diffGain = 255 - peak - 1;
    }

    const int topLeft = static_cast<int>(mono ? diffGain * 2 : diffGain);
    const QRect sourceRect(0,
            topLeft,
            image.width(),
            image.height() -
                    2 * static_cast<int>(diffGain));
    QImage croppedImage = image.copy(sourceRect);
    // Copy image, otherwise QPainter crashes when we alter it.
    QImage normImage = croppedImage.scaled(image.size(),
            Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);

    return normImage;
}

void drawWaveformPartRGB(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartRGB"));
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    const QColor lowColor = signalColors.getRgbLowColor();
    const QColor midColor = signalColors.getRgbMidColor();
    const QColor highColor = signalColors.getRgbHighColor();
    QColor color;

    float lowColor_r = 0, lowColor_g = 0, lowColor_b = 0,
          midColor_r = 0, midColor_g = 0, midColor_b = 0,
          highColor_r = 0, highColor_g = 0, highColor_b = 0,
          all = 0, low = 0, mid = 0, high = 0,
          red = 0, green = 0, blue = 0, max = 0;

    getRgbF(lowColor, &lowColor_r, &lowColor_g, &lowColor_b);
    getRgbF(midColor, &midColor_r, &midColor_g, &midColor_b);
    getRgbF(highColor, &highColor_r, &highColor_g, &highColor_b);

    if (mono) {
        // Mono means we're going to paint from bottom to top with l+r.
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        // shift y0 to bottom
        pPainter->translate(0, 2 * dy);
        // flip y-axis
        pPainter->scale(1, -1);
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            // Left
            all = pWaveform->getAll(i) + pWaveform->getAll(i + 1);
            low = pWaveform->getLow(i) + pWaveform->getLow(i + 1);
            mid = pWaveform->getMid(i) + pWaveform->getMid(i + 1);
            high = pWaveform->getHigh(i) + pWaveform->getHigh(i + 1);

            red = low * lowColor_r + mid * midColor_r + high * highColor_r;
            green = low * lowColor_g + mid * midColor_g + high * highColor_g;
            blue = low * lowColor_b + mid * midColor_b + high * highColor_b;
            // Normalize
            max = math_max3(red, green, blue);
            // Draw
            if (max > 0.0) {
                color.setRgbF(static_cast<float>(low / max),
                        static_cast<float>(mid / max),
                        static_cast<float>(high / max));
                pPainter->setPen(color);
                pPainter->drawLine(x, static_cast<int>(all), x, 0);
            }
        }
    } else { // stereo
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            // Left
            all = pWaveform->getAll(i);
            low = pWaveform->getLow(i);
            mid = pWaveform->getMid(i);
            high = pWaveform->getHigh(i);

            red = low * lowColor_r + mid * midColor_r + high * highColor_r;
            green = low * lowColor_g + mid * midColor_g + high * highColor_g;
            blue = low * lowColor_b + mid * midColor_b + high * highColor_b;
            // Normalize
            max = math_max3(red, green, blue);
            // Draw
            if (max > 0.0) {
                color.setRgbF(static_cast<float>(low / max),
                        static_cast<float>(mid / max),
                        static_cast<float>(high / max));
                pPainter->setPen(color);
                pPainter->drawLine(x, static_cast<int>(-all), x, 0);
            }

            // Right
            all = pWaveform->getAll(i + 1);
            low = pWaveform->getLow(i + 1);
            mid = pWaveform->getMid(i + 1);
            high = pWaveform->getHigh(i + 1);

            red = low * lowColor_r + mid * midColor_r + high * highColor_r;
            green = low * lowColor_g + mid * midColor_g + high * highColor_g;
            blue = low * lowColor_b + mid * midColor_b + high * highColor_b;

            max = math_max3(red, green, blue);

            if (max > 0.0) {
                color.setRgbF(static_cast<float>(low / max),
                        static_cast<float>(mid / max),
                        static_cast<float>(high / max));
                pPainter->setPen(color);
                pPainter->drawLine(x, 0, x, static_cast<int>(all));
            }
        }
    }

    if (start) {
        *start = end;
    }
}

void drawWaveformPartLMH(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartLMH"));
    const QColor lowColor = signalColors.getLowColor();
    const QColor midColor = signalColors.getMidColor();
    const QColor highColor = signalColors.getHighColor();
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    if (mono) {
        // Mono means we're going to paint from bottom to top with l+r.
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        // shift y0 to bottom
        pPainter->translate(0, 2 * dy);
        // flip y-axis
        pPainter->scale(1, -1);

        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            x = i / 2;
            pPainter->setPen(lowColor);
            pPainter->drawLine(QPoint(x, 0),
                    QPoint(x, pWaveform->getLow(i) + pWaveform->getLow(i + 1)));

            pPainter->setPen(midColor);
            pPainter->drawLine(QPoint(x, 0),
                    QPoint(x, pWaveform->getMid(i) + pWaveform->getMid(i + 1)));

            pPainter->setPen(highColor);
            pPainter->drawLine(QPoint(x, 0),
                    QPoint(x, pWaveform->getHigh(i) + pWaveform->getHigh(i + 1)));
        }
    } else { // stereo
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            x = i / 2;
            pPainter->setPen(lowColor);
            pPainter->drawLine(QPoint(x, -pWaveform->getLow(i)),
                    QPoint(x, pWaveform->getLow(i + 1)));

            pPainter->setPen(midColor);
            pPainter->drawLine(QPoint(x, -pWaveform->getMid(i)),
                    QPoint(x, pWaveform->getMid(i + 1)));

            pPainter->setPen(highColor);
            pPainter->drawLine(QPoint(x, -pWaveform->getHigh(i)),
                    QPoint(x, pWaveform->getHigh(i + 1)));
        }
    }

    if (start) {
        *start = end;
    }
}

void drawWaveformPartHSV(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartHSV"));
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    float h = 0, s = 0, v = 0, lo = 0, hi = 0, total = 0;
    // Get HSV of low color.
    const QColor lowColor = signalColors.getLowColor();
    getHsvF(lowColor, &h, &s, &v);
    QColor color;

    unsigned char low[2] = {0, 0};
    unsigned char high[2] = {0, 0};
    unsigned char mid[2] = {0, 0};
    unsigned char all[2] = {0, 0};

    if (mono) {
        // Mono means we're going to paint from bottom to top with l+r.
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        // shift y0 to bottom
        pPainter->translate(0, 2 * dy);
        // flip y-axis
        pPainter->scale(1, -1);
    }

    for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
        x = i / 2;
        all[0] = pWaveform->getAll(i);
        all[1] = pWaveform->getAll(i + 1);

        if (!all[0] && !all[1]) {
            continue;
        }

        low[0] = pWaveform->getLow(i);
        low[1] = pWaveform->getLow(i + 1);
        mid[0] = pWaveform->getMid(i);
        mid[1] = pWaveform->getMid(i + 1);
        high[0] = pWaveform->getHigh(i);
        high[1] = pWaveform->getHigh(i + 1);

        total = (low[0] + low[1] + mid[0] + mid[1] +
                        high[0] + high[1]) *
                1.2f;

        // Prevent division by zero
        if (total > 0) {
            // Normalize low and high
            // (mid not need, because it not change the color)
            lo = (low[0] + low[1]) / total;
            hi = (high[0] + high[1]) / total;
        } else {
            lo = hi = 0.0;
        }

        // Set color
        color.setHsvF(h, 1.0f - hi, 1.0f - lo);

        if (mono) {
            pPainter->setPen(color);
            pPainter->drawLine(QPoint(i / 2, 0),
                    QPoint(i / 2, all[0] + all[1]));
        } else {
            pPainter->setPen(color);
            pPainter->drawLine(QPoint(i / 2, -all[0]),
                    QPoint(i / 2, all[1]));
        }
    }

    if (start) {
        *start = end;
    }
}


// TrackStar energy overview. Club tracks are limited to a brick, so the plain
// overview shows a flat block. This draws the *energy* over time instead:
// amplitude squared, smoothed over ~2.5 s (three box blurs ~ gaussian),
// normalised on the 98th percentile, contrast ^1.3. Stacked from outside to
// inside: total energy (low colour), bass+mid (mid colour), bass (high colour),
// so the outer ring shows the highs and the core shows the bass.
//   [TrackStar],energy_overview        1 = on (default), 0 = plain Filtered overview
//   [TrackStar],energy_smooth_seconds  smoothing window (default 2.5)
//   [TrackStar],energy_contrast        exponent (default 1.3)
namespace {
void boxBlur(std::vector<float>& v, int radius) {
    if (radius < 1 || v.size() < 2) {
        return;
    }
    const int n = static_cast<int>(v.size());
    std::vector<float> out(n);
    double acc = 0.0;
    int count = 0;
    // sliding window [i-radius, i+radius], clamped at the edges
    for (int i = -radius; i <= radius && i < n; ++i) {
        if (i >= 0) {
            acc += v[i];
            count++;
        }
    }
    for (int i = 0; i < n; ++i) {
        out[i] = static_cast<float>(acc / std::max(count, 1));
        const int drop = i - radius, add = i + radius + 1;
        if (drop >= 0) {
            acc -= v[drop];
            count--;
        }
        if (add < n) {
            acc += v[add];
            count++;
        }
    }
    v.swap(out);
}
} // namespace

void drawWaveformPartEnergy(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawWaveformPartEnergy"));
    const int n = end / 2;
    if (n <= 0) {
        return;
    }
    std::vector<float> eAll(n), eLow(n), eMid(n);
    float peak = 1.f;
    for (int i = 0, x = 0; i + 1 < end; i += 2, ++x) {
        const float all = (pWaveform->getAll(i) + pWaveform->getAll(i + 1)) * 0.5f;
        const float low = (pWaveform->getLow(i) + pWaveform->getLow(i + 1)) * 0.5f;
        const float mid = (pWaveform->getMid(i) + pWaveform->getMid(i + 1)) * 0.5f;
        eAll[x] = all * all;
        eLow[x] = low * low;
        eMid[x] = mid * mid;
        peak = math_max3(peak,
                static_cast<float>(pWaveform->getAll(i)),
                static_cast<float>(pWaveform->getAll(i + 1)));
    }
    // smoothing window in samples (visual sample rate = samples per second)
    const double seconds = std::max(0.1, trackstar::tuning(QStringLiteral("energy_smooth_seconds"), 2.5));
    // visual samples per second: audio frames per visual sample is public, the
    // audio rate is not known here; 44.1 kHz is close enough for a smoothing window
    const double ratio = pWaveform->getAudioVisualRatio();
    const double rate = ratio > 0 ? 44100.0 / ratio : 6.0;
    const int radius = std::max(1, static_cast<int>(std::lround(seconds * rate / 2.0)));
    for (int pass = 0; pass < 3; ++pass) {
        boxBlur(eAll, radius);
        boxBlur(eLow, radius);
        boxBlur(eMid, radius);
    }
    // sqrt back to amplitude domain, normalise on P98 of the total energy
    std::vector<float> sorted(n);
    for (int x = 0; x < n; ++x) {
        eAll[x] = std::sqrt(eAll[x]);
        eLow[x] = std::sqrt(eLow[x]);
        eMid[x] = std::sqrt(eMid[x]);
        sorted[x] = eAll[x];
    }
    std::nth_element(sorted.begin(), sorted.begin() + (n * 98) / 100, sorted.end());
    const float p98 = std::max(sorted[(n * 98) / 100], 1e-3f);
    const float contrast = static_cast<float>(
            std::max(0.2, trackstar::tuning(QStringLiteral("energy_contrast"), 1.3)));
    // the caller crops/normalises the image on `peak` (max raw amplitude), so
    // scale the curve to that peak and it ends up filling the overview
    const float scale = peak;
    auto shape = [&](float v) {
        return std::pow(std::clamp(v / p98, 0.f, 1.f), contrast) * scale;
    };

    const QColor outer = signalColors.getLowColor();  // total energy
    const QColor middle = signalColors.getMidColor(); // bass + mid
    const QColor core = signalColors.getHighColor();  // bass

    if (mono) {
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        pPainter->translate(0, 2 * dy);
        pPainter->scale(1, -1);
        for (int x = 0; x < n; ++x) {
            const int hAll = static_cast<int>(2 * shape(eAll[x]));
            const int hMid = static_cast<int>(2 * shape(eMid[x]));
            const int hLow = static_cast<int>(2 * shape(eLow[x]));
            pPainter->setPen(outer);
            pPainter->drawLine(QPoint(x, 0), QPoint(x, hAll));
            pPainter->setPen(middle);
            pPainter->drawLine(QPoint(x, 0), QPoint(x, std::min(hMid, hAll)));
            pPainter->setPen(core);
            pPainter->drawLine(QPoint(x, 0), QPoint(x, std::min(hLow, hMid)));
        }
    } else {
        for (int x = 0; x < n; ++x) {
            const int hAll = static_cast<int>(shape(eAll[x]));
            const int hMid = std::min(static_cast<int>(shape(eMid[x])), hAll);
            const int hLow = std::min(static_cast<int>(shape(eLow[x])), hMid);
            pPainter->setPen(outer);
            pPainter->drawLine(QPoint(x, -hAll), QPoint(x, hAll));
            pPainter->setPen(middle);
            pPainter->drawLine(QPoint(x, -hMid), QPoint(x, hMid));
            pPainter->setPen(core);
            pPainter->drawLine(QPoint(x, -hLow), QPoint(x, hLow));
        }
    }
}

} // namespace waveformOverviewRenderer
