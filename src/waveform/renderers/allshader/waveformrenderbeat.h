#pragma once

#include <QColor>

#include "rendergraph/geometrynode.h"
#include "util/class.h"
#include "waveform/renderers/waveformrendererabstract.h"

class QDomNode;
class SkinContext;

namespace allshader {
class WaveformRenderBeat;
} // namespace allshader

class allshader::WaveformRenderBeat final
        : public QObject,
          public ::WaveformRendererAbstract,
          public rendergraph::GeometryNode {
    Q_OBJECT
  public:
    explicit WaveformRenderBeat(WaveformWidgetRenderer* waveformWidget,
            ::WaveformRendererAbstract::PositionSource type =
                    ::WaveformRendererAbstract::Play);

    // Pure virtual from WaveformRendererAbstract, not used
    void draw(QPainter* painter, QPaintEvent* event) override final;

    void setup(const QDomNode& node, const SkinContext& skinContext) override;

    // Virtuals for rendergraph::Node
    void preprocess() override;

  public slots:
    void setColor(const QColor& color) {
        m_color = color;
    }

  private:
    QColor m_color;
    // TrackStar (2.6): short ticks at the top and bottom edge of every beat line, in their own
    // colour (skin <BeatTickColor>, default white), so the beat stays visible where the line itself
    // is dark (the line is drawn dark so it cuts through the light stem layers).
    QColor m_tickColor;
    rendergraph::GeometryNode* m_pStarOutlineNode;   // dark outline under the star (drawn first)
    rendergraph::GeometryNode* m_pTickNode;
    bool m_isSlipRenderer;

    bool preprocessInner();

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderBeat);
};
