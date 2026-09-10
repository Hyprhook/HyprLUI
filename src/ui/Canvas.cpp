#include "Canvas.hpp"
#include "../render/gfx.hpp"

#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>
#include <cmath>

namespace HyprLUI {

    void CCanvas::render() {
        // Bindings first - they call setters like CTextNode::setText(),
        // which just mark content dirty. measure() (next) is what actually
        // rebuilds/re-measures against the NEW content, so it has to run
        // after this, not before.
        for (auto& binding : m_bindings)
            binding();

        if (m_root) {
            // Full-tree layout every frame - HUD-sized content, no need
            // for a layout-dirty flag (unlike CTextNode's texture cache,
            // which is the expensive part and stays cached).
            m_root->measure();

            // Sync this window's own outer size to what the tree actually
            // measured (size-to-content axes only - setFixedSize()'d axes
            // stay pinned). Without this, m_size is frozen at whatever it
            // was at creation forever, and every position/damage
            // calculation below keeps using that stale value - see
            // setFixedSize()'s doc comment for what that breaks.
            const Vector2D contentSize{m_fixedW ? *m_fixedW : m_root->size().x, m_fixedH ? *m_fixedH : m_root->size().y};
            if (contentSize.x != m_size.x || contentSize.y != m_size.y) {
                gfx::damageBox(fullDamageBox()); // old footprint, in case it shrunk - including debug overlay overflow, see fullDamageBox()'s doc comment
                m_size = contentSize;
                damage(); // new footprint (at the still-current position) + arms the redamage countdown
                if (m_onSizeChanged)
                    m_onSizeChanged(m_size);
            }

            // Root-fills-canvas (Phase 15, DESIGN.md) - if the root widget
            // wants to fill its window (CWidget::fill()) and THIS axis has
            // a determinate size (an explicit w/h from hyprlui.window() or
            // set_canvas_size(), i.e. m_fixedW/H is set), force the root's
            // just-measured size on that axis to match m_size (this
            // canvas's own just-settled size from the sync above) instead
            // of leaving it at whatever it naturally measured. An axis
            // left auto-sized has nothing determinate to fill and stays
            // untouched - same inherent limitation CSS stretch has against
            // an "auto" parent. Runs AFTER the sync above specifically so
            // it reads m_size's SETTLED value, not a stale one from a
            // previous frame.
            //
            // A leaf root (Box/Image/etc, not a container) used to have a
            // narrower version of the sticky-growth hazard CStackWidget::
            // measureContent()/CFlexWidget::measureContent()'s doc
            // comments describe for a fill CHILD - toggling the CANVAS
            // itself from an explicit size back to nil/auto wouldn't
            // shrink a leaf root back down, since nothing reset its size
            // once this override stopped running. Closed as a side effect
            // of CWidget::measureContent()'s default (Widget.hpp), which
            // now resets every leaf to its own natural size every frame
            // the same way a container already recomputed from children -
            // this override still runs first each frame regardless
            // (unconditionally, whenever m_root->fill() is set), so a
            // fixed-size canvas keeps overriding a leaf root exactly as
            // before; only the auto/nil case changed, from "stuck" to
            // "correctly falls back to natural size."
            if (m_root->fill()) {
                Vector2D size = m_root->size();
                if (m_fixedW)
                    size.x = m_size.x;
                if (m_fixedH)
                    size.y = m_size.y;
                m_root->setSize(size);
            }
        }

        // Style slide offset (Phase 16 follow-up, DESIGN.md) - CWidget::
        // styleOffset() computes this generically from the ROOT WIDGET's
        // own visibility animation (its own animationIn/animationOut
        // override if it has one, else the global hyprlui.animation()
        // config) - no canvas-specific mechanism needed. Returns {0,0} for
        // any style other than "slide" (including popin/gnome, handled
        // entirely separately below, NOT through this field - see the
        // render() call further down for why). Cached here (rather than
        // read inline where used below) so fullDamageBox() - used by the
        // redamage tick just below, which also runs while invisible - has
        // a value ready before this frame's real render happens. Never
        // added to the origin passed to m_root->render() below (that
        // would double-apply it - root's own boxAt()/render() ALREADY
        // apply this internally, since slide is a normal per-widget
        // mechanism as far as CWidget is concerned). m_position itself is
        // never touched here or anywhere else - anchor tracking, hit-
        // testing, and every other position-consumer keeps using the
        // real, settled position throughout the whole slide.
        m_styleOffset = m_root ? m_root->styleOffset() : Vector2D{0, 0};

        // Anchor tracking (if any) is re-resolved every frame too - cheap
        // (a handful of monitors, one name comparison each) and keeps the
        // window correctly placed across resolution/reserved-area changes
        // on its already-chosen monitor, or a content-size change just
        // above for anchors (e.g. bottom-right) whose position depends on
        // size. Damages old + new box itself when position actually moves.
        recomputeAnchorPosition();

        // Tick the redamage countdown regardless of visibility/root - this
        // is what actually clears stale pixels for a hidden/removed canvas
        // that isn't going to draw anything this frame. See damage()'s doc
        // comment in Canvas.hpp for why a single damage() call isn't
        // enough on its own.
        if (m_pendingRedamageFrames > 0) {
            gfx::damageBox(fullDamageBox());
            --m_pendingRedamageFrames;
        }

        if (!m_root || !m_root->visible())
            return;

        // An animation changes rendered opacity (or, since Phase 16, ALSO
        // position - CWidget::styleOffset() reads the exact same
        // m_visibilityAnim isAnimating() already checks, so no separate
        // check is needed here) every frame for its whole duration - keep
        // re-damaging every frame while anything in this window's tree
        // (including the root itself) is still animating, same reasoning
        // as damage()'s own multi-frame redamage countdown above, just
        // driven continuously instead of a fixed 4-frame burst. This is
        // also what keeps a removeCanvas()'d, fading/sliding-out canvas
        // alive in CUIManager's m_pendingRemoval for as long as its
        // animation actually takes: hasPendingRedamage() (which that
        // sweep checks) stays true as long as damage() keeps getting
        // called.
        if (m_root->isAnimating())
            damage();

        m_root->arrange();
        // No separate canvas-level opacity multiplier - the root widget's
        // own composedOpacity() already folds in its own animationIn/
        // animationOut progress (see CWidget::setVisible()), which is all
        // "a window fading" ever was under the hood - parentOpacity is a
        // plain 1.0, same as any other widget's topmost ancestor.
        //
        // `slide` is handled INTERNALLY by root's own boxAt()/render() (a
        // normal per-widget mechanism, see CWidget::styleOffset()) - only
        // `m_position` plain is needed here, adding m_styleOffset too
        // would double-apply it. `popin`/`gnome` (Phase 17, DESIGN.md)
        // have NO internal handling at all - root-only, so THEIR
        // offset+scale is computed right here, the one and only place
        // either ever gets APPLIED, into m_popinOffset/m_popinScale -
        // cached as members (not purely local) so hitTest() (Canvas.hpp,
        // called independently of render() - see its own doc comment) can
        // reuse this same frame's transform for click detection. NOT read
        // by fullDamageBox() though: popin/gnome always shrink WITHIN the
        // real, settled box (centered), so its existing m_size-based box
        // already safely covers them without any extension, unlike slide
        // (which moves OUTSIDE the real box and does need one). Math
        // mirrors Hyprland's own WindowAnimationController.cpp exactly:
        // popin's minimum-size percentage defaults to 0 if omitted
        // (`style == "popin"` alone, no trailing "N%"); gnome squashes the
        // Y axis only (X stays full width) to a horizontal line at the
        // box's own vertical center.
        m_popinOffset               = {0, 0};
        m_popinScale                = {1, 1};
        const std::string rootStyle = m_root->styleString();
        if (rootStyle == "gnome" || rootStyle == "gnomed") {
            const double progress = m_root->visibilityProgress();
            m_popinScale          = {1.0, progress};
            m_popinOffset         = {0.0, m_root->size().y * 0.5 * (1.0 - m_popinScale.y)};
        } else if (rootStyle == "popin" || rootStyle.starts_with("popin ")) {
            double minPerc = 0.0;
            if (const auto space = rootStyle.find(' '); space != std::string::npos) {
                const auto pct = rootStyle.substr(space + 1);
                try {
                    minPerc = std::stod(pct.substr(0, pct.size() - 1)) / 100.0; // trailing '%' already validated by LuaBridge.cpp's optStyleField()
                } catch (...) {}
            }
            const double progress = m_root->visibilityProgress();
            const double s        = minPerc + (1.0 - minPerc) * progress;
            m_popinScale          = {s, s};
            m_popinOffset         = m_root->size() * 0.5 * (Vector2D{1.0, 1.0} - m_popinScale);
        }

        if (m_popinScale != Vector2D{1, 1})
            m_root->render(m_position + m_popinOffset, 1.0F, m_popinScale);
        else
            m_root->render(m_position, 1.0F);

        // Debug overlay (box-model outlines/labels) is an entirely
        // separate pass, run AFTER normal content so it always paints on
        // top regardless of any widget's own z-index/opacity - it's
        // diagnostic, not real content. A fresh SDebugSpec{} here means
        // "nothing enabled, everything auto" at the root; a widget only
        // actually draws anything once it (or an ancestor that hasn't
        // walled itself off) sets `debug = true`.
        const auto debugBounds = m_root->renderDebug(m_position, {});

        // Recompute how far that overlay currently extends beyond box()
        // on each side, for fullDamageBox() to use on the NEXT damage()
        // call (this frame's own damage() calls above already happened,
        // using whatever this was last frame - one-frame-stale,
        // self-correcting, same tolerance this codebase already accepts
        // elsewhere for "redo it every frame" values). No overlay active
        // anywhere this frame (debugBounds is nullopt, the common case)
        // means zero overflow, i.e. fullDamageBox() == box() exactly.
        m_debugOverflow = {};
        if (debugBounds) {
            const auto b           = box();
            m_debugOverflow.left   = std::max(0.0, b.pos().x - debugBounds->pos().x);
            m_debugOverflow.top    = std::max(0.0, b.pos().y - debugBounds->pos().y);
            m_debugOverflow.right  = std::max(0.0, (debugBounds->pos().x + debugBounds->size().x) - (b.pos().x + b.size().x));
            m_debugOverflow.bottom = std::max(0.0, (debugBounds->pos().y + debugBounds->size().y) - (b.pos().y + b.size().y));
        }
    }

    void CCanvas::damage() {
        gfx::damageBox(fullDamageBox());
        m_pendingRedamageFrames = REDAMAGE_FRAMES;
    }

    bool CCanvas::recomputeAnchorPosition() {
        if (!m_anchor)
            return false;

        auto monitor = State::CMonitorQuery{*State::monitorState()}.name(m_anchorMonitor).run();
        if (!monitor)
            return false;

        // Read the monitor's LIVE combined reserved margins (config
        // baseline + every HyprLUI exclusive window's static-tier
        // contribution + Hyprland's own layer-shell/error-overlay dynamic
        // contributions - CReservedArea::calculate() already sums all of
        // that into these four numbers, see ReservedAreaComposer.hpp's
        // header comment for the full tier breakdown). If this window is
        // itself exclusive, exclude only ITS OWN contribution from
        // whichever single edge it reserves - a window only ever
        // contributes to one edge, so every other edge is already
        // correct as-is. This is what lets an exclusive window sit right
        // at its own natural position while still correctly avoiding
        // everything else already reserved (another HyprLUI exclusive
        // window on a different edge, the user's config baseline,
        // Hyprland's own error/debug overlay) - unlike the raw-box
        // approach this replaces, which avoided nothing, or the
        // unmodified logicalBoxMinusReserved() every other window uses,
        // which would self-referentially push this window inward by the
        // space it itself just reserved.
        double top    = monitor->m_reservedArea.top();
        double right  = monitor->m_reservedArea.right();
        double bottom = monitor->m_reservedArea.bottom();
        double left   = monitor->m_reservedArea.left();

        if (m_exclusiveEdge) {
            switch (*m_exclusiveEdge) {
                case EEdge::Top: top = std::max(0.0, top - m_size.y); break;
                case EEdge::Right: right = std::max(0.0, right - m_size.x); break;
                case EEdge::Bottom: bottom = std::max(0.0, bottom - m_size.y); break;
                case EEdge::Left: left = std::max(0.0, left - m_size.x); break;
            }
        }

        // Same math as CReservedArea::apply(logicalBox()) (i.e. what
        // logicalBoxMinusReserved() does internally) - just against the
        // margins above instead of the monitor's own live totals
        // unmodified, since those may include this window's own
        // contribution.
        const auto rawBox = monitor->logicalBox();
        const CBox box{rawBox.pos().x + left, rawBox.pos().y + top, rawBox.size().x - left - right, rawBox.size().y - top - bottom};
        const auto boxPos  = box.pos();
        const auto boxSize = box.size();

        Vector2D   pos;
        switch (*m_anchor) {
            case EAnchor::TopLeft: pos = {boxPos.x + m_anchorOffset.x, boxPos.y + m_anchorOffset.y}; break;
            case EAnchor::Top: pos = {boxPos.x + (boxSize.x - m_size.x) / 2.0 + m_anchorOffset.x, boxPos.y + m_anchorOffset.y}; break;
            case EAnchor::TopRight: pos = {boxPos.x + boxSize.x - m_size.x - m_anchorOffset.x, boxPos.y + m_anchorOffset.y}; break;
            case EAnchor::Left: pos = {boxPos.x + m_anchorOffset.x, boxPos.y + (boxSize.y - m_size.y) / 2.0 + m_anchorOffset.y}; break;
            case EAnchor::Center: pos = {boxPos.x + (boxSize.x - m_size.x) / 2.0 + m_anchorOffset.x, boxPos.y + (boxSize.y - m_size.y) / 2.0 + m_anchorOffset.y}; break;
            case EAnchor::Right: pos = {boxPos.x + boxSize.x - m_size.x - m_anchorOffset.x, boxPos.y + (boxSize.y - m_size.y) / 2.0 + m_anchorOffset.y}; break;
            case EAnchor::BottomLeft: pos = {boxPos.x + m_anchorOffset.x, boxPos.y + boxSize.y - m_size.y - m_anchorOffset.y}; break;
            case EAnchor::Bottom: pos = {boxPos.x + (boxSize.x - m_size.x) / 2.0 + m_anchorOffset.x, boxPos.y + boxSize.y - m_size.y - m_anchorOffset.y}; break;
            case EAnchor::BottomRight: pos = {boxPos.x + boxSize.x - m_size.x - m_anchorOffset.x, boxPos.y + boxSize.y - m_size.y - m_anchorOffset.y}; break;
        }

        // Center/Top/Left/Right/Bottom's own `/ 2.0` above can land on a
        // fractional pixel whenever (boxSize - m_size) is odd. Found live:
        // Hyprland samples text/image textures with GL_LINEAR unless it
        // detects an exact 1:1 pixel match (CHyprOpenGLImpl::
        // renderTextureInternal(), src/render/OpenGL.cpp) - a texture
        // drawn 1:1 but at a fractional DESTINATION offset still ends up
        // reading a blended average of two adjacent texels per pixel
        // instead of one exact texel each, which reads as visibly blurry
        // for anything with sharp contrast (text especially; a solid-
        // color rect looks fine since its neighboring texels are the same
        // color anyway). Rounding here means every OTHER position this
        // window's tree computes from m_position stays whole-pixel too.
        pos.x = std::round(pos.x);
        pos.y = std::round(pos.y);

        if (pos.x == m_position.x && pos.y == m_position.y)
            return false;

        // Damage both where the window WAS and where it's about to be -
        // a plain damage() after moving only covers the new box, leaving
        // the old one's pixels stale (same class of bug damage() itself
        // now guards against for mutations - see its doc comment).
        // fullDamageBox() (not the plain box()) so the debug overlay's
        // own overflow moves with it too.
        const auto oldBox = this->fullDamageBox();
        m_position        = pos;
        gfx::damageBox(oldBox);
        damage();
        return true;
    }

    void CCanvas::moveTo(const Vector2D& position) {
        clearAnchor();

        if (position.x == m_position.x && position.y == m_position.y)
            return;

        // Same old+new damage dance as recomputeAnchorPosition() above -
        // see its own comment for why a plain damage() after moving isn't
        // enough on its own.
        const auto oldBox = fullDamageBox();
        m_position        = position;
        gfx::damageBox(oldBox);
        damage();
    }

} // namespace HyprLUI
