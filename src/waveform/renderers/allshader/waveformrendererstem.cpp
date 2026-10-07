#include "waveform/renderers/allshader/waveformrendererstem.h"

#include <QFont>
#include <QVarLengthArray>
#include <algorithm>
#include <QImage>
#include <QOpenGLTexture>

#include "control/controlproxy.h"
#include "engine/channels/enginedeck.h"
#include "engine/engine.h"
#include "rendergraph/material/rgbamaterial.h"
#include "rendergraph/vertexupdaters/rgbavertexupdater.h"
#include "track/track.h"
#include "trackstar/tuning.h"
#include "util/assert.h"
#include "util/math.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"
#include "waveform/waveformwidgetfactory.h"

namespace {
#ifdef __SCENEGRAPH__
// FIXME this is a workaround an issue with waveform only drawing partially in
// SG. The workaround is to reduce the the number of vertices, by reducing the
// precision of waveform strips.
const float kPixelPerStrip = 2;
#else
const float kPixelPerStrip = 1;
#endif
} // namespace

using namespace rendergraph;

namespace allshader {

WaveformRendererStem::WaveformRendererStem(
        WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type,
        ::WaveformRendererSignalBase::Options options)
        : WaveformRendererSignalBase(waveformWidget, options),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip),
          m_splitStemTracks(false),
          m_outlineOpacity(0.15f),
          m_opacity(0.75f) {
    initForRectangles<RGBAMaterial>(0);
    setUsePreprocess(true);
}

void WaveformRendererStem::onSetup(const QDomNode&) {
}

bool WaveformRendererStem::init() {
    m_pStemGain.clear();
    m_pStemMute.clear();

    // Keep a valid drawing order even for displays that provide a static track
    // without a channel group. The stem data itself is independent of the
    // per-stem volume/mute controls, so those controls are optional.
    m_stackOrder.resize(mixxx::kMaxSupportedStems);
    std::iota(m_stackOrder.begin(), m_stackOrder.end(), 0);

    if (m_waveformRenderer->getGroup().isEmpty()) {
        return true;
    }
    for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; stemIdx++) {
        QString stemGroup = EngineDeck::getGroupForStem(m_waveformRenderer->getGroup(), stemIdx);
        m_pStemGain.emplace_back(
                std::make_unique<ControlProxy>(stemGroup,
                        QStringLiteral("volume")));
        m_pStemMute.emplace_back(
                std::make_unique<ControlProxy>(stemGroup,
                        QStringLiteral("mute")));
        auto bringToForeground = [this, stemIdx](double) {
            if (!m_reorderOnChange) {
                return;
            }
            m_stackOrder.removeAll(stemIdx);
            m_stackOrder.append(stemIdx);
        };
        m_pStemGain.back()->connectValueChanged(this, bringToForeground);
        m_pStemMute.back()->connectValueChanged(this, bringToForeground);
    }

#ifndef __SCENEGRAPH__
    auto* pWaveformWidgetFactory = WaveformWidgetFactory::instance();
    setSplitStemTracks(pWaveformWidgetFactory->isStemSplitTracks());
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::stemSplitTracksChanged,
            this,
            &WaveformRendererStem::setSplitStemTracks);
    setReorderOnChange(pWaveformWidgetFactory->isStemReorderOnChange());
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::stemReorderOnChangeChanged,
            this,
            &WaveformRendererStem::setReorderOnChange);
    setOutlineOpacity(pWaveformWidgetFactory->getStemOutlineOpacity());
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::stemOutlineOpacityChanged,
            this,
            &WaveformRendererStem::setOutlineOpacity);
    setOpacity(pWaveformWidgetFactory->getStemOpacity());
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::stemOpacityChanged,
            this,
            &WaveformRendererStem::setOpacity);
#endif
    return true;
}

void WaveformRendererStem::preprocess() {
    if (!preprocessInner()) {
        if (geometry().vertexCount() != 0) {
            geometry().allocate(0);
            markDirtyGeometry();
        }
    }
}

bool WaveformRendererStem::preprocessInner() {
    TrackPointer pTrack = m_waveformRenderer->getTrackInfo();

    if (!pTrack || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return false;
    }

    auto stemInfo = pTrack->getStemInfo();
    // If this track isn't a stem track, skip the rendering
    if (stemInfo.isEmpty()) {
        return false;
    }
    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;

    ConstWaveformPointer waveform = pTrack->getWaveform();
    if (waveform.isNull()) {
        return false;
    }

    const int dataSize = waveform->getDataSize();
    if (dataSize <= 1) {
        return false;
    }

    const WaveformData* data = waveform->data();
    if (data == nullptr) {
        return false;
    }
    // If this waveform doesn't contain stem data, skip the rendering
    if (!waveform->hasStem()) {
        return false;
    }

    uint selectedStems = m_waveformRenderer->getSelectedStems();

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();
    const int length = static_cast<int>(m_waveformRenderer->getLength());
    const int pixelLength = static_cast<int>(m_waveformRenderer->getLength() * devicePixelRatio);
    const int stripLength = static_cast<int>(static_cast<float>(pixelLength) / kPixelPerStrip);
    const float invDevicePixelRatio = kPixelPerStrip / devicePixelRatio;
    const float halfStripSize = kPixelPerStrip / 2.0f / devicePixelRatio;

    // See waveformrenderersimple.cpp for a detailed explanation of the frame and index calculation
    const int visualFramesSize = dataSize / 2;
    const double firstVisualFrame =
            m_waveformRenderer->getFirstDisplayedPosition(positionType) * visualFramesSize;
    const double lastVisualFrame =
            m_waveformRenderer->getLastDisplayedPosition(positionType) * visualFramesSize;

    // Represents the # of visual frames per horizontal pixel.
    const double visualIncrementPerPixel =
            (lastVisualFrame - firstVisualFrame) / static_cast<double>(stripLength);

    // Per-band gain from the EQ knobs.
    float allGain(1.0);
    getGains(&allGain, nullptr, nullptr, nullptr);

    const float breadth = static_cast<float>(m_waveformRenderer->getBreadth());
    const float stemBreadth = m_splitStemTracks ? breadth / 4.0f : 0;
    const float halfBreadth = (m_splitStemTracks ? stemBreadth : breadth) / 2.0f;

    const float heightFactor = allGain * halfBreadth / m_maxValue;

    // Effective visual frame for x
    double xVisualFrame = qRound(firstVisualFrame / visualIncrementPerPixel) *
            visualIncrementPerPixel;

    const int numVerticesPerLine = 6; // 2 triangles

    const int reserved = numVerticesPerLine *
            (mixxx::audio::ChannelCount::stem() * stripLength + 1);

    geometry().setDrawingMode(Geometry::DrawingMode::Triangles);
    geometry().allocate(reserved);
    markDirtyGeometry();

    RGBAVertexUpdater vertexUpdater{geometry().vertexDataAs<Geometry::RGBAColoredPoint2D>()};
    vertexUpdater.addRectangle({0.f,
                                       halfBreadth - 0.5f},
            {static_cast<float>(length),
                    m_isSlipRenderer ? halfBreadth : halfBreadth + 0.5f},
            {0.f, 0.f, 0.f, 0.f});

    // TrackStar parallax: every stem gets its own zoom around the play marker and
    // a small vertical offset, so the front stem (last in the stack order, drawn
    // on top) runs "faster" than the ones behind it: a depth effect. Audio time
    // is untouched, all layers meet exactly at the play marker.
    //   [TrackStar],stem_parallax        depth 0..1 (0 = off; 0.45 = mockup default)
    //   [TrackStar],stem_parallax_spread vertical spread 0..1 (front lower, back higher)
    //   [TrackStar],stem_parallax_slip   also in the slip overlay (0/1)
    const double parallax = m_splitStemTracks
            ? 0.0
            : std::clamp(trackstar::tuning(QStringLiteral("stem_parallax"), 0.0), 0.0, 1.0);   // 07-10: uit (liep "naast" de playhead)
    const double spread = m_splitStemTracks
            ? 0.0
            : std::clamp(trackstar::tuning(QStringLiteral("stem_parallax_spread"), 0.30), 0.0, 1.0);
    const bool parallaxOn = parallax > 0.0 &&
            (!m_isSlipRenderer || trackstar::tuning(QStringLiteral("stem_parallax_slip"), 0.0) > 0.0);
    const int numStems = m_stackOrder.size();
    const double playMarkerFrame = firstVisualFrame +
            (lastVisualFrame - firstVisualFrame) * m_waveformRenderer->getPlayMarkerPosition();
    // TrackStar 2.6 palette + dimmed past: [TrackStar],stem_palette (1 = vocals white, drums deck
    // colour, bass darker, other grey), [TrackStar],stem_past_alpha (0..1, opacity of what has
    // already played; 1 = no dimming). Deck 2/4 = purple, deck 1/3 = blue.
    const QString group = m_waveformRenderer->getGroup();
    const int deckIdx = (group.contains(QStringLiteral("[Channel2]")) ||
                                group.contains(QStringLiteral("[Channel4]")))
            ? 1
            : 0;
    const float pastAlpha = static_cast<float>(
            std::clamp(trackstar::tuning(QStringLiteral("stem_past_alpha"), 0.45), 0.0, 1.0));
    const bool paletteOn = trackstar::tuning(QStringLiteral("stem_palette"), 1.0) > 0.0;
    // [TrackStar],stem_bottom (default 1): bars rise from the bottom edge (like the deck overview)
    // instead of mirroring around the centre; the parallax spread is ignored then.
    const bool bottomMode = !m_isSlipRenderer && !m_splitStemTracks &&
            trackstar::tuning(QStringLiteral("stem_bottom"), 1.0) > 0.0;
    // the design shows filled layers; the Mixxx opacity prefs tend to be low (outline look)
    const float fillAlpha = paletteOn ? std::max(m_opacity, 0.95f) : m_opacity;
    // the outline layer ignores volume/mute (it shows what *could* play): keep it a faint ghost, so a
    // stem the Autopilot or a pad took out visibly disappears ([TrackStar],stem_ghost_alpha)
    const float outlineAlpha = paletteOn
            ? static_cast<float>(std::clamp(trackstar::tuning(QStringLiteral("stem_ghost_alpha"), 0.18), 0.0, 1.0))
            : m_outlineOpacity;
    QVarLengthArray<QColor, mixxx::kMaxSupportedStems> paletteColor(stemInfo.size());
    // beat and vocals are what you want to see: drums + vocals full, bass/other as a dim backdrop
    // ([TrackStar],stem_backdrop_alpha), in a fixed stack order other → bass → drums → vocals (front)
    QVarLengthArray<float, mixxx::kMaxSupportedStems> paletteAlpha(stemInfo.size());
    QVarLengthArray<int, mixxx::kMaxSupportedStems> paletteRank(stemInfo.size());
    const float backdrop = static_cast<float>(
            std::clamp(trackstar::tuning(QStringLiteral("stem_backdrop_alpha"), 0.35), 0.0, 1.0));
    for (int i = 0; i < stemInfo.size(); i++) {
        const QString l = stemInfo[i].getLabel().toLower();
        paletteColor[i] = trackstar::stemColor(stemInfo[i].getLabel(), deckIdx, stemInfo[i].getColor());
        const bool front = l.contains(QStringLiteral("drum")) || l.contains(QStringLiteral("voc"));
        paletteAlpha[i] = front ? 1.f : backdrop;
        paletteRank[i] = l.contains(QStringLiteral("voc")) ? 3 : l.contains(QStringLiteral("drum")) ? 2 : l.contains(QStringLiteral("bass")) ? 1 : 0;
    }
    auto drawOrder = m_stackOrder;   // same container type as m_stackOrder
    if (paletteOn) {
        std::stable_sort(drawOrder.begin(), drawOrder.end(), [&](int a, int b) {
            return (a < paletteRank.size() ? paletteRank[a] : 0) < (b < paletteRank.size() ? paletteRank[b] : 0);
        });
    }
    // per stack position: zoom factor (>1 = stretched) and y offset
    QVarLengthArray<double, mixxx::kMaxSupportedStems> stemZoom(numStems);
    QVarLengthArray<float, mixxx::kMaxSupportedStems> stemYOffset(numStems);
    for (int layer = 0; layer < numStems; layer++) {
        const double depth = numStems > 1
                ? (static_cast<double>(layer) / (numStems - 1)) - 0.5 // -0.5 (back) .. +0.5 (front)
                : 0.0;
        stemZoom[layer] = parallaxOn ? 1.0 + parallax * depth : 1.0;
        stemYOffset[layer] = parallaxOn
                ? static_cast<float>(spread * depth * halfBreadth)
                : 0.f;
    }

    for (int visualIdx = 0; visualIdx < stripLength; visualIdx++) {
        int stemLayer = 0;
        for (int stemIdx : std::as_const(drawOrder)) {
            if (stemIdx >= stemInfo.size()) {
                continue;
            }
            // this stem's own visual frame for x (parallax) and sampling window
            const double zoom = stemZoom[stemLayer];
            const double stemVisualFrame = playMarkerFrame + (xVisualFrame - playMarkerFrame) / zoom;
            const double stemSamplingRange = visualIncrementPerPixel / zoom / 2.0;
            const int visualFrameStart = std::lround(stemVisualFrame - stemSamplingRange);
            const int visualFrameStop = std::lround(stemVisualFrame + stemSamplingRange);
            const int visualIndexStart = std::clamp(visualFrameStart * 2, 0, dataSize - 1);
            const int visualIndexStop =
                    std::clamp(std::max(visualFrameStop, visualFrameStart + 1) * 2, 0, dataSize - 1);
            const float fVisualIdx = static_cast<float>(visualIdx) * invDevicePixelRatio;
            const float yOffset = m_isSlipRenderer ? 0.f : stemYOffset[stemLayer];

            // Find the max values for current eq in the waveform data.
            // - Max of left and right
            uchar u8max{};
            for (int chn = 0; chn < 2; chn++) {
                // data is interleaved left / right
                for (int i = visualIndexStart + chn; i < visualIndexStop + chn && i < dataSize; i += 2) {
                    const WaveformData& waveformData = data[i];
                    u8max = math_max(u8max, waveformData.stems[stemIdx]);
                }
            }

            // Stem is drawn twice with different opacity level, this allow to
            // see the maximum signal by transparency
            for (int layerIdx = 0; layerIdx < 2; layerIdx++) {
                const QColor& stemColor = paletteColor[stemIdx];
                const bool past = !m_isSlipRenderer && xVisualFrame < playMarkerFrame;
                float color_r = stemColor.redF(),
                      color_g = stemColor.greenF(),
                      color_b = stemColor.blueF(),
                      color_a = stemColor.alphaF() * (layerIdx ? fillAlpha : outlineAlpha) *
                        (past ? pastAlpha : 1.f) * (paletteOn ? paletteAlpha[stemIdx] : 1.f);

                // Cast to float
                float max = static_cast<float>(u8max) * allGain;

                // Apply the gains
                if (layerIdx) {
                    if (selectedStems) {
                        max *= !(selectedStems & 1 << stemIdx)
                                ? 0.f
                                : 1.f;
                    } else if (!m_pStemMute.empty() && m_pStemMute[stemIdx]->toBool()) {
                        max = 0;
                    } else {
                        float volume = m_pStemGain.empty()
                                ? 1.f
                                : static_cast<float>(m_pStemGain[stemIdx]->get());
                        max *= volume;
                    }
                }

                // Lines are thin rectangles
                // shadow
                float height = heightFactor * max;
                if (m_splitStemTracks) {
                    height = std::min(height, halfBreadth);
                }
                const int yIndex = m_splitStemTracks ? stemIdx : stemLayer;
                const float yCenter = yIndex * stemBreadth + halfBreadth + yOffset;
                if (bottomMode && deckIdx == 1) {
                    // deck B (the lower waveform) mirrors deck A: bars hang from the top edge
                    const float yTop = yIndex * stemBreadth;
                    vertexUpdater.addRectangle(
                            {fVisualIdx - halfStripSize, yTop},
                            {fVisualIdx + halfStripSize, std::min(yTop + 2.f * height, yTop + 2.f * halfBreadth)},
                            {color_r, color_g, color_b, color_a});
                } else if (bottomMode) {
                    const float yBottom = yIndex * stemBreadth + 2.f * halfBreadth;
                    vertexUpdater.addRectangle(
                            {fVisualIdx - halfStripSize, std::max(yBottom - 2.f * height, yBottom - 2.f * halfBreadth)},
                            {fVisualIdx + halfStripSize, yBottom},
                            {color_r, color_g, color_b, color_a});
                } else {
                    vertexUpdater.addRectangle(
                            {fVisualIdx - halfStripSize,
                                    yCenter - height},
                            {fVisualIdx + halfStripSize,
                                    m_isSlipRenderer
                                            ? yCenter
                                            : yCenter + height},
                            {color_r, color_g, color_b, color_a});
                }
            }
            stemLayer++;
        }

        xVisualFrame += visualIncrementPerPixel;
    }

    DEBUG_ASSERT(reserved == vertexUpdater.index());

    markDirtyMaterial();

    return true;
}

} // namespace allshader
