#pragma once
//
// ContainerWidget.hpp
//
// The two container widget types Phase 1 ships with:
//
//   CStackWidget - manual/absolute positioning, the escape hatch. Children
//     keep whatever position they were constructed with; the stack's own
//     size-to-content is just the bounding box of its children.
//
//   CFlexWidget - flexbox-lite Row/Column. Packs children along one axis
//     with a gap, applies padding on all sides, and aligns children on the
//     cross axis (start/center/end). No wrap, no justify/space-between -
//     deliberately out of scope for v1 (see DESIGN.md open questions).

#include "Widget.hpp"

namespace HyprLUI {

    class CStackWidget : public CWidget {
      public:
        using CWidget::CWidget;

      protected:
        void measureContent() override;
    };

    enum class EFlexDirection {
        Row,
        Column,
    };

    enum class EAlign {
        Start,
        Center,
        End,
    };

    class CFlexWidget : public CWidget {
      public:
        CFlexWidget(std::string id, const Vector2D& position, EFlexDirection direction, double gap = 0, double padding = 0, EAlign align = EAlign::Start) :
            CWidget(std::move(id), position), m_direction(direction), m_gap(gap), m_padding(padding), m_align(align) {}

      protected:
        void measureContent() override;
        void arrangeChildren() override;

      private:
        EFlexDirection m_direction;
        double         m_gap;
        double         m_padding;
        EAlign         m_align;
    };

} // namespace HyprLUI
