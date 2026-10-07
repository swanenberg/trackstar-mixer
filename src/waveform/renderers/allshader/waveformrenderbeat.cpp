#include "waveform/renderers/allshader/waveformrenderbeat.h"

#include <QDomNode>
#include <cmath>

#include "engine/engine.h"
#include "moc_waveformrenderbeat.cpp"
#include "rendergraph/geometry.h"
#include "rendergraph/material/unicolormaterial.h"
#include "rendergraph/vertexupdaters/vertexupdater.h"
#include "skin/legacy/skincontext.h"
#include "trackstar/tuning.h"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/wskincolor.h"

using namespace rendergraph;

namespace allshader {

WaveformRenderBeat::WaveformRenderBeat(WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type)
        : ::WaveformRendererAbstract(waveformWidget),
          m_tickColor(Qt::white),
          m_pStarOutlineNode(nullptr),
          m_pTickNode(nullptr),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip) {
    initForRectangles<UniColorMaterial>(0);
    setUsePreprocess(true);
    // TrackStar: the ticks are a child node, drawn after (on top of) the lines; the star outline
    // is a child before it, so the white star lands on a dark halo
    {
        auto pNode = std::make_unique<GeometryNode>();
        m_pStarOutlineNode = pNode.get();
        m_pStarOutlineNode->initForRectangles<UniColorMaterial>(0);
        appendChildNode(std::move(pNode));
    }
    auto pNode = std::make_unique<GeometryNode>();
    m_pTickNode = pNode.get();
    m_pTickNode->initForRectangles<UniColorMaterial>(0);
    appendChildNode(std::move(pNode));
}

void WaveformRenderBeat::setup(const QDomNode& node, const SkinContext& skinContext) {
    m_color = QColor(skinContext.selectString(node, QStringLiteral("BeatColor")));
    m_color = WSkinColor::getCorrectColor(m_color).toRgb();
    const QString tick = skinContext.selectString(node, QStringLiteral("BeatTickColor"));
    m_tickColor = tick.isEmpty() ? QColor(Qt::white) : WSkinColor::getCorrectColor(QColor(tick)).toRgb();
}

void WaveformRenderBeat::draw(QPainter* painter, QPaintEvent* event) {
    Q_UNUSED(painter);
    Q_UNUSED(event);
    DEBUG_ASSERT(false);
}

void WaveformRenderBeat::preprocess() {
    if (!preprocessInner()) {
        geometry().allocate(0);
        markDirtyGeometry();
        m_pTickNode->geometry().allocate(0);
        m_pTickNode->markDirtyGeometry();
        m_pStarOutlineNode->geometry().allocate(0);
        m_pStarOutlineNode->markDirtyGeometry();
    }
}

bool WaveformRenderBeat::preprocessInner() {
    const TrackPointer trackInfo = m_waveformRenderer->getTrackInfo();

    if (!trackInfo || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return false;
    }

    const bool isStemTrack = trackInfo && trackInfo->hasStem() &&
            trackInfo->getWaveform() && trackInfo->getWaveform()->hasStem();
    const bool splitStemTracks = isStemTrack && WaveformWidgetFactory::isCreated() &&
            WaveformWidgetFactory::instance()->isStemSplitTracks();

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;

    mixxx::BeatsPointer trackBeats = trackInfo->getBeats();
    if (!trackBeats) {
        return false;
    }

#ifndef __SCENEGRAPH__
    int alpha = m_waveformRenderer->getBeatGridAlpha();
    if (alpha == 0) {
        return false;
    }
    m_color.setAlphaF(alpha / 100.0f);
#endif

    if (!m_color.alpha()) {
        // Don't render the beatgrid lines is there are fully transparent
        return false;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0.0) {
        return false;
    }

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition(positionType);
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition(positionType);

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);

    if (!startPosition.isValid() || !endPosition.isValid()) {
        return false;
    }

    const float rendererBreadth = m_waveformRenderer->getBreadth();

    const int numVerticesPerLine = 6; // 2 triangles

    // Count the number of beats in the range to reserve space in the m_vertices vector.
    // Note that we could also use
    //   int numBearsInRange = trackBeats->numBeatsInRange(startPosition, endPosition);
    // for this, but there have been reports of that method failing with a DEBUG_ASSERT.
    int numBeatsInRange = 0;
    int numDownbeatsInRange = 0;
    const int beatsPerBarCount = std::max(1,
            static_cast<int>(trackstar::tuning(QStringLiteral("beats_per_bar"), 4.0)));
    for (auto it = trackBeats->iteratorFrom(startPosition);
            it != trackBeats->cend() && *it <= endPosition;
            ++it) {
        numBeatsInRange++;
        const int off = it.beatOffset();
        if (((off % beatsPerBarCount) + beatsPerBarCount) % beatsPerBarCount == 0) {
            numDownbeatsInRange++;
        }
    }

    const int numBoxesPerBeat = (m_isSlipRenderer && splitStemTracks)
            ? mixxx::kMaxSupportedStems
            : 1;
    const int reserved = numBeatsInRange * numVerticesPerLine * numBoxesPerBeat;
    geometry().allocate(reserved);

    VertexUpdater vertexUpdater{geometry().vertexDataAs<Geometry::Point2D>()};

    // TrackStar: ticks (top + bottom) per beat, [TrackStar],beat_tick = height in px (0 = off);
    // the downbeat ("1") gets a five-pointed star at the top instead of a tick, like the TrackStar
    // logo: [TrackStar],downbeat_star = star radius in px (0 = plain big tick).
    const float tickHeight = static_cast<float>(
            std::max(0.0, trackstar::tuning(QStringLiteral("beat_tick"), 6.0)));
    const float starR = static_cast<float>(
            std::max(0.0, trackstar::tuning(QStringLiteral("downbeat_star"), 7.0)));
    const bool drawTicks = tickHeight > 0.f && !(m_isSlipRenderer && splitStemTracks);
    // deck B (Channel2/4) is drawn mirrored (bars hang from the top): its star and ticks sit at the bottom
    const QString beatGroup = m_waveformRenderer->getGroup();
    const bool mirrored = !m_isSlipRenderer &&
            (beatGroup.contains(QStringLiteral("[Channel2]")) || beatGroup.contains(QStringLiteral("[Channel4]")));
    const int kStarVertices = 10 * 3; // fan of 10 triangles
    // top tick per beat (no bottom ticks: the hotcue labels live there), star for the "1"
    const int reservedTicks = drawTicks
            ? numBeatsInRange * numVerticesPerLine +
                    (starR > 0.f ? numDownbeatsInRange * (kStarVertices - numVerticesPerLine) : 0)
            : 0;
    m_pTickNode->geometry().allocate(reservedTicks);
    VertexUpdater tickUpdater{m_pTickNode->geometry().vertexDataAs<Geometry::Point2D>()};
    const int reservedOutline = (drawTicks && starR > 0.f) ? numDownbeatsInRange * kStarVertices : 0;
    m_pStarOutlineNode->geometry().allocate(reservedOutline);
    VertexUpdater outlineUpdater{m_pStarOutlineNode->geometry().vertexDataAs<Geometry::Point2D>()};

    const float boxBreadth = splitStemTracks
            ? rendererBreadth / static_cast<float>(mixxx::kMaxSupportedStems)
            : rendererBreadth;

    // TrackStar: draw the downbeat (the "1") wider than the other beats.
    // [TrackStar],downbeat_width = line width in px (1 = same as other beats),
    // [TrackStar],beats_per_bar = bar length used to find the "1" from the
    // grid marker (repair_grids anchors the "1" on the marker).
    const float downbeatWidth = static_cast<float>(
            std::max(1.0, trackstar::tuning(QStringLiteral("downbeat_width"), 3.0)));
    const int beatsPerBar = std::max(1,
            static_cast<int>(trackstar::tuning(QStringLiteral("beats_per_bar"), 4.0)));

    for (auto it = trackBeats->iteratorFrom(startPosition);
            it != trackBeats->cend() && *it <= endPosition;
            ++it) {
        double beatPosition = it->toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(
                        beatPosition, positionType);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const int offset = it.beatOffset();
        const bool isDownbeat = ((offset % beatsPerBar) + beatsPerBar) % beatsPerBar == 0;
        const float halfExtra = isDownbeat ? (downbeatWidth - 1.f) / 2.f : 0.f;
        const float x1 = static_cast<float>(xBeatPoint) - halfExtra;
        const float x2 = x1 + 1.f + 2.f * halfExtra;

        if (m_isSlipRenderer && splitStemTracks) {
            for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; ++stemIdx) {
                const float posy1 = stemIdx * boxBreadth;
                const float posy2 = posy1 + boxBreadth / 2.f;
                vertexUpdater.addRectangle({x1, posy1}, {x2, posy2});
            }
        } else {
            const float bottom = m_isSlipRenderer ? rendererBreadth / 2 : rendererBreadth;
            vertexUpdater.addRectangle({x1, 0.f}, {x2, bottom});
            if (drawTicks) {
                const float th = isDownbeat ? tickHeight * 2.f : tickHeight;
                const float tw = isDownbeat ? 3.f : 1.f;   // extra half-width per side
                if (isDownbeat && starR > 0.f) {
                    // five-pointed star, tip up, centred on the beat line just under the top edge,
                    // on a dark halo 2 px wider (outline node, drawn first)
                    const float cx = (x1 + x2) / 2.f;
                    const float cy = mirrored ? bottom - starR - 3.f : starR + 3.f;
                    auto star = [&](VertexUpdater& u, float rOut) {
                        const float rIn = rOut * 0.42f;
                        float px[10], py[10];
                        for (int k = 0; k < 10; ++k) {
                            const float r = (k % 2 == 0) ? rOut : rIn;
                            const float a = static_cast<float>(-M_PI / 2.0 + k * M_PI / 5.0);
                            px[k] = cx + r * std::cos(a);
                            py[k] = cy + r * std::sin(a);
                        }
                        for (int k = 0; k < 10; ++k) {
                            const int k2 = (k + 1) % 10;
                            u.addTriangle(QVector2D(cx, cy), QVector2D(px[k], py[k]), QVector2D(px[k2], py[k2]));
                        }
                    };
                    star(outlineUpdater, starR + 2.f);
                    star(tickUpdater, starR);
                } else if (mirrored) {
                    tickUpdater.addRectangle({x1 - tw, bottom - th}, {x2 + tw, bottom});
                } else {
                    tickUpdater.addRectangle({x1 - tw, 0.f}, {x2 + tw, th});
                }
            }
        }
    }
    markDirtyGeometry();
    m_pTickNode->markDirtyGeometry();
    m_pStarOutlineNode->markDirtyGeometry();

    DEBUG_ASSERT(reserved == vertexUpdater.index());
    DEBUG_ASSERT(reservedTicks == tickUpdater.index());
    DEBUG_ASSERT(reservedOutline == outlineUpdater.index());

    material().setUniform(1, m_color);
    markDirtyMaterial();
    m_pTickNode->material().setUniform(1, m_tickColor);
    m_pTickNode->markDirtyMaterial();
    m_pStarOutlineNode->material().setUniform(1, QColor(0x11, 0x13, 0x18));
    m_pStarOutlineNode->markDirtyMaterial();

    return true;
}

} // namespace allshader
