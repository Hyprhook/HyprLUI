#pragma once
//
// ContainerWidget.hpp
//
// The two container widget types Phase 1 ships with:
//
//   CStackWidget - manual/absolute positioning, the escape hatch. Children
//     keep whatever position they were constructed with; the stack's own
//     size-to-content is just the bounding box of its children. Does NOT
//     interpret its own padding() or a child's margin() (Phase 7,
//     Widget.hpp) - manual positioning already gives full control over
//     spacing, and silently offsetting explicit x/y would be surprising.
//
//   CFlexWidget - flexbox-lite Row/Column. Packs children along one axis
//     with a gap, insets from its own padding() (Phase 7's per-side
//     CWidget::padding(), replacing this class's earlier private uniform
//     double), additionally spaces each child out by its own margin()
//     (also Phase 7 - read per-child, CSS-flexbox-item style: adds to,
//     doesn't replace, `gap`), and aligns children on the cross axis
//     (start/center/end). No wrap, no justify/space-between - deliberately
//     out of scope for v1 (see DESIGN.md open questions).

#include "Widget.hpp"

namespace HyprLUI {

    class CStackWidget : public CWidget {
      public:
        using CWidget::CWidget;

      protected:
        void measureContent() override;
        // Phase 15's `fill` (see CWidget::setFill()'s doc comment) is the
        // ONE thing CStackWidget's arrangeChildren() actually does - a
        // `fill` child is resized to the stack's own full m_size at
        // position (0, 0), ignoring padding (consistent with this
        // class's own established "no padding interpretation" design, see
        // the file header comment). A non-`fill` child is untouched, same
        // as the base no-op default every other widget type still uses.
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
        // `padding` is no longer a constructor param - it's set via the
        // base CWidget::setPadding() like any other widget's (see
        // LuaBridge.cpp's buildWidget()), which is what let it become a
        // shared, per-side base property in Phase 7 instead of a private
        // uniform double duplicated here.
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
