#pragma once
//
// ContainerWidget.hpp
//
// The two container widget types:
//
//   CStackWidget - manual/absolute positioning, the escape hatch. Children
//     keep whatever position they were constructed with; the stack's own
//     size-to-content is the bounding box of its children. Does not
//     interpret its own padding() or a child's margin() - manual
//     positioning already gives full control over spacing.
//
//   CFlexWidget - flexbox-lite Row/Column. Packs children along one axis
//     with a gap, insets from its own padding(), additionally spaces each
//     child out by its own margin() (CSS-flexbox-item style - adds to,
//     doesn't replace, `gap`), and aligns children on the cross axis
//     (start/center/end). No wrap, no justify/space-between.

#include "Widget.hpp"

namespace HyprLUI {

    class CStackWidget : public CWidget {
      public:
        using CWidget::CWidget;

      protected:
        void measureContent() override;
        // `fill` is the one thing this override actually does - a fill
        // child is resized to the stack's own full m_size at (0, 0),
        // ignoring padding. A non-fill child is untouched.
        void arrangeChildren() override;
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
        // `padding` is not a constructor param - it's set via the base
        // CWidget::setPadding() like any other widget's.
        CFlexWidget(std::string id, const Vector2D& position, EFlexDirection direction, double gap = 0, EAlign align = EAlign::Start) :
            CWidget(std::move(id), position), m_direction(direction), m_gap(gap), m_align(align) {}

      protected:
        void measureContent() override;
        void arrangeChildren() override;

      private:
        EFlexDirection m_direction;
        double         m_gap;
        EAlign         m_align;
    };

} // namespace HyprLUI
