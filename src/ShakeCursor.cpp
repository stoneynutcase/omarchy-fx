#include "ShakeCursor.hpp"
#include "globals.hpp"

#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/pointer/cursor/CursorManager.hpp>
#include <hyprland/src/protocols/CursorShape.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <cairo/cairo.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

using namespace OmarchyFX;
using Hyprutils::Memory::sc;

// What comes back from the worker: tight ARGB rows, ready for a cursor buffer.
struct SRenderResult {
    std::string          shape;
    int                  px = 0;
    bool                 ok = false;
    std::vector<uint8_t> pixels;
    int                  size = 0;
    int                  hotspotX = 0, hotspotY = 0;
};

struct CShakeCursor::SWorker {
    std::string                              theme;
    std::thread                              thread;
    std::mutex                               mutex;
    std::condition_variable                  cv;
    bool                                     stop = false;
    std::deque<std::pair<std::string, int>>  jobs;
    std::vector<SRenderResult>               results;

    void run() {
        // Its own manager: nothing here is shared with the compositor thread.
        Hyprcursor::CHyprcursorManager mgr(theme.empty() ? nullptr : theme.c_str());

        while (true) {
            std::pair<std::string, int> job;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return stop || !jobs.empty(); });
                if (stop)
                    return;
                job = jobs.front();
                jobs.pop_front();
            }

            // Loading a style renders every shape in the theme at that size,
            // so once the requested shape is out, the shapes the pointer is
            // likely to pick up next — crossing into a text field, over a
            // link — cost only a copy each. Rendering them now is what keeps
            // the magnified cursor from dropping to normal size for a moment
            // when the shape changes under it.
            static const char* const COMMON[] = {"default", "text", "pointer", "grab", "grabbing", "crosshair", "move", "not-allowed", "wait", "progress",
                                                 "ew-resize", "ns-resize", "nwse-resize", "nesw-resize", "col-resize", "row-resize"};

            std::vector<SRenderResult> out;
            out.push_back(SRenderResult{.shape = job.first, .px = job.second});

            if (mgr.valid()) {
                const Hyprcursor::SCursorStyleInfo STYLE{.size = sc<unsigned int>(job.second)};
                if (mgr.loadThemeStyle(STYLE)) {
                    const auto grab = [&](const std::string& name, SRenderResult& r) {
                        auto data = mgr.getShape(name.c_str(), STYLE);
                        if (data.images.empty() || !data.images.front().surface)
                            return;

                        const auto& IMG  = data.images.front();
                        auto        surf = IMG.surface;
                        cairo_surface_flush(surf);

                        const int   W = cairo_image_surface_get_width(surf);
                        const int   H = cairo_image_surface_get_height(surf);
                        const int   S = cairo_image_surface_get_stride(surf);
                        const auto* D = cairo_image_surface_get_data(surf);

                        if (!D || W <= 0 || H != W || cairo_image_surface_get_format(surf) != CAIRO_FORMAT_ARGB32)
                            return;

                        r.pixels.resize(sc<size_t>(W) * H * 4);
                        for (int y = 0; y < H; ++y)
                            std::memcpy(r.pixels.data() + sc<size_t>(y) * W * 4, D + sc<size_t>(y) * S, sc<size_t>(W) * 4);
                        r.size     = W;
                        r.hotspotX = IMG.hotspotX;
                        r.hotspotY = IMG.hotspotY;
                        r.ok       = true;
                    };

                    grab(job.first, out.front());
                    if (!out.front().ok && job.first != "default")
                        grab("default", out.front());

                    for (const auto* name : COMMON) {
                        if (name == job.first)
                            continue;
                        SRenderResult r{.shape = name, .px = job.second};
                        grab(name, r);
                        if (r.ok)
                            out.push_back(std::move(r));
                    }

                    mgr.cursorSurfaceStyleDone(STYLE);
                }
            }

            std::lock_guard lock(mutex);
            for (auto& r : out)
                results.push_back(std::move(r));
        }
    }
};

// KWin's numbers, verbatim (src/plugins/shakecursor in kwin). The detector
// keeps the last WINDOW of motion; a shake is a trail SENSITIVITY times longer
// than the box it fits in, and that box has to be MIN_DIAGONAL across so hand
// tremor does not count.
static constexpr auto   SHAKE_WINDOW = std::chrono::milliseconds(1000);
static constexpr double SENSITIVITY  = 4.0;
static constexpr double MIN_DIAGONAL = 100.0;

// The magnification, also KWin's: the first shake takes the cursor to
// MAGNIFICATION times its size, every further one adds OVER on top with no
// ceiling, each change tweened over TWEEN, and it comes back DEFLATE_AFTER
// after the last shake. Not tunable: it is a find-the-cursor aid.
static constexpr float  MAGNIFICATION = 3.0F;
static constexpr float  OVER          = 1.0F;
static constexpr auto   TWEEN         = std::chrono::milliseconds(200);
static constexpr auto   DEFLATE_AFTER = std::chrono::milliseconds(2000);

// Raster fallback: the Xcursor theme is asked for its largest rendition and
// scaled from there, so a 24 px cursor at 3x is a downscale of a big image
// where the theme has one. Past the biggest size a theme ships (Adwaita stops
// at 96 px) it is an upscale and blurs — only a vector theme fixes that.
static constexpr int BIG_SIZE = 256;

// ...and so a raster theme stops growing a little past that largest image:
// this much upscale still reads as a cursor, more reads as a smear.
static constexpr double RASTER_STRETCH = 1.25;

// Vector path: sizes are rendered on this grid, and only for the size a shake
// is heading to; the tween in between scales that render. A style load
// renders the whole theme, so every render avoided is felt.
static constexpr int    VECTOR_STEP   = 16;
static constexpr size_t RENDER_CACHE  = 96; // sizes times shapes; a 240 px render is 230 KB

// KWin's: movements smaller than the tolerance count as movement in any
// direction, so a wobble along one axis is not a string of reversals.
static bool sameSign(double a, double b) {
    constexpr double TOLERANCE = 1.0;
    return (a >= -TOLERANCE && b >= -TOLERANCE) || (a <= TOLERANCE && b <= TOLERANCE);
}

static double easeInOutCubic(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

CShakeCursor::CShakeCursor()  = default;
CShakeCursor::~CShakeCursor() = default;

void CShakeCursor::init() {
    // The cursor-shape protocol is how clients name the shape they want;
    // remembering the last name is what lets the magnified cursor keep its
    // shape rather than always turning into an arrow.
    if (PROTO::cursorShape)
        m_listeners.emplace_back(PROTO::cursorShape->m_events.setShape.listen([this](const CCursorShapeProtocol::SSetShapeEvent& e) { m_shape = e.shapeName; }));
}

void CShakeCursor::shutdown() {
    restoreOriginal();
    m_listeners.clear();
    m_history.clear();

    if (m_worker) {
        {
            std::lock_guard lock(m_worker->mutex);
            m_worker->stop = true;
        }
        m_worker->cv.notify_all();
        if (m_worker->thread.joinable())
            m_worker->thread.join();
        m_worker.reset();
    }
}

void CShakeCursor::ensureWorker() {
    if (m_worker)
        return;

    m_worker = makeUnique<SWorker>();
    const auto THEME = getenv("HYPRCURSOR_THEME");
    m_worker->theme  = THEME && *THEME ? THEME : "";
    m_worker->thread = std::thread([w = m_worker.get()] { w->run(); });
}

void CShakeCursor::requestRender(const std::string& shape, int px) {
    for (const auto& P : m_pending) {
        if (P.first == shape && P.second == px)
            return;
    }
    for (const auto& R : m_renders) {
        if (R.shape == shape && R.px == px)
            return;
    }

    ensureWorker();
    m_pending.emplace_back(shape, px);
    {
        std::lock_guard lock(m_worker->mutex);
        m_worker->jobs.emplace_back(shape, px);
    }
    m_worker->cv.notify_one();
}

void CShakeCursor::drainRenders() {
    if (!m_worker)
        return;

    std::vector<SRenderResult> done;
    {
        std::lock_guard lock(m_worker->mutex);
        done.swap(m_worker->results);
    }

    for (auto& r : done) {
        std::erase_if(m_pending, [&r](const auto& p) { return p.first == r.shape && p.second == r.px; });
        if (!r.ok)
            continue;

        // The worker sends the common shapes along with the one asked for;
        // one it already sent, or a request that raced, is not stored twice.
        const bool HAVE = std::ranges::any_of(m_renders, [&r](const auto& R) { return R.shape == r.shape && R.px == r.px; });
        if (HAVE)
            continue;

        const auto SIZE = Vector2D{sc<double>(r.size), sc<double>(r.size)};
        const auto HOT  = Vector2D{sc<double>(r.hotspotX), sc<double>(r.hotspotY)};
        auto       buf  = makeShared<Pointer::Cursor::CCursorBuffer>(r.pixels.data(), SIZE, HOT);

        if (m_renders.size() >= RENDER_CACHE)
            m_renders.erase(m_renders.begin());
        m_renders.push_back({.shape = r.shape, .px = r.px, .buffer = buf, .size = SIZE, .hotspot = HOT});
    }

    // A sharper render may now be available for what is on screen.
    if (!done.empty() && m_active)
        m_applied = -1.F;
}

void CShakeCursor::setEnabled(bool enabled) {
    if (m_enabled == enabled)
        return;

    m_enabled = enabled;
    if (!enabled) {
        m_history.clear();
        m_target = 1.F;
        restoreOriginal();
    }
}

void CShakeCursor::onMove() {
    if (!m_enabled || !Pointer::mgr())
        return;

    // KWin: motion with a button held, or under a pointer constraint, is not
    // a shake and wipes the trail — so zigzagging a window around does not
    // grow the cursor, and a game locking the pointer does not either.
    const bool DRAGGING = g_layoutManager && g_layoutManager->dragController() && g_layoutManager->dragController()->target();
    if (DRAGGING || (g_pInputManager && (g_pInputManager->hasHeldButtons() || g_pInputManager->isConstrained()))) {
        m_history.clear();
        return;
    }

    const auto NOW = std::chrono::steady_clock::now();
    if (detect(NOW, Pointer::mgr()->position())) {
        m_lastShake = NOW;
        animateTo(std::min(maxZoom(), m_target <= 1.F ? MAGNIFICATION : m_target + OVER), NOW);
    }
}

void CShakeCursor::probeHyprcursor() {
    // Same lookup Hyprland's own cursor manager does: the theme named in the
    // environment, else whichever hyprcursor theme is installed. Tried once.
    if (m_hyprcursorTried)
        return;
    m_hyprcursorTried = true;

    const auto THEME = getenv("HYPRCURSOR_THEME");
    auto       mgr   = makeUnique<Hyprcursor::CHyprcursorManager>(THEME && *THEME ? THEME : nullptr);
    if (mgr->valid())
        m_hyprcursor = std::move(mgr);
}

float CShakeCursor::maxZoom() {
    probeHyprcursor();
    if (m_hyprcursor)
        return std::numeric_limits<float>::infinity();

    if (!renderRaster() || m_bigSize.x <= 0)
        return MAGNIFICATION;

    const auto   LOGICAL = Pointer::mgr()->cursorSizeLogical();
    const double BASE    = std::max(LOGICAL.x, 1.0);
    return std::max(1.5F, sc<float>(m_bigSize.x * RASTER_STRETCH / (BASE * outputScale())));
}

bool CShakeCursor::detect(std::chrono::steady_clock::time_point now, const Vector2D& pos) {
    while (!m_history.empty() && now - m_history.front().at >= SHAKE_WINDOW)
        m_history.pop_front();

    // Motion that keeps going the same way as the last leg extends that leg
    // rather than adding a point: the trail is a list of reversals.
    if (m_history.size() >= 2) {
        auto&       last = m_history.back();
        const auto& prev = m_history[m_history.size() - 2];
        if (sameSign(last.pos.x - prev.pos.x, pos.x - last.pos.x) && sameSign(last.pos.y - prev.pos.y, pos.y - last.pos.y)) {
            last = {now, pos};
            return false;
        }
    }

    m_history.push_back({now, pos});

    Vector2D min = m_history.front().pos, max = min;
    double   trail = 0.0;

    for (size_t i = 1; i < m_history.size(); ++i) {
        const auto& P = m_history[i].pos;
        trail += (P - m_history[i - 1].pos).size();
        min.x = std::min(min.x, P.x);
        min.y = std::min(min.y, P.y);
        max.x = std::max(max.x, P.x);
        max.y = std::max(max.y, P.y);
    }

    const double DIAGONAL = (max - min).size();
    if (DIAGONAL < MIN_DIAGONAL)
        return false;

    if (trail / DIAGONAL > SENSITIVITY) {
        m_history.clear();
        return true;
    }

    return false;
}

void CShakeCursor::animateTo(float target, std::chrono::steady_clock::time_point now) {
    m_animFrom  = m_zoom;
    m_animStart = now;
    m_target    = target;
}

void CShakeCursor::tick() {
    if (!Pointer::mgr())
        return;

    if (!m_enabled && !m_active)
        return;

    drainRenders();

    const auto NOW = std::chrono::steady_clock::now();

    if (m_target > 1.F && NOW - m_lastShake > DEFLATE_AFTER)
        animateTo(1.F, NOW);

    const double T = std::chrono::duration<double, std::milli>(NOW - m_animStart).count() / std::chrono::duration<double, std::milli>(TWEEN).count();
    m_zoom         = m_animFrom + (m_target - m_animFrom) * sc<float>(easeInOutCubic(T));

    if (m_target <= 1.F && T >= 1.0) {
        m_zoom = 1.F;
        if (m_active)
            restoreOriginal();
        return;
    }

    if (m_zoom <= 1.001F && !m_active)
        return;

    // Something else set a new cursor image while we were showing ours (the
    // pointer crossed into a text field, say). Take that as the new original
    // and magnify it instead.
    // Hyprland also re-sets its default cursor on every motion event while
    // the pointer is over no window at all, i.e. in the gap between two.
    // Either way what is showing is not ours any more, so the next apply must
    // set the buffer again even if zoom and buffer are what they were.
    if (m_active && Pointer::mgr()->currentCursorImage().pBuffer != m_bigBuffer) {
        m_active        = false;
        m_appliedBuffer = nullptr;
        captureOriginal();
    }

    if (!m_active)
        captureOriginal();

    if (!m_active || std::abs(m_zoom - m_applied) > 0.005F)
        apply(m_zoom);
}

void CShakeCursor::captureOriginal() {
    const auto& IMG = Pointer::mgr()->currentCursorImage();

    m_origBuffer  = IMG.pBuffer;
    m_origSurface = IMG.surface;
    m_origHotspot = IMG.hotspot;
    m_origSize    = IMG.size;
    m_origScale   = IMG.scale > 0.F ? IMG.scale : 1.F;
}

// Scale of the output the pointer is on, so the vector render is pixel-exact
// there. Falls back to the largest scale around.
double CShakeCursor::outputScale() const {
    double     best = 1.0;
    const auto POS  = Pointer::mgr()->position();

    for (const auto& M : State::monitorState()->monitors()) {
        if (!M)
            continue;
        if (M->logicalBox().containsPoint(POS))
            return M->m_scale;
        best = std::max(best, sc<double>(M->m_scale));
    }

    return best;
}

bool CShakeCursor::ensureBigImage(float zoom) {
    probeHyprcursor();

    // With a vector theme, never mix in the raster one while a render is on
    // its way: a few frames at normal size beat a few frames of the wrong
    // cursor.
    if (m_hyprcursor) {
        const double BASE = std::max(m_origSize.x / m_origScale, 1.0);
        const int    PX   = std::max(VECTOR_STEP, sc<int>(std::round(BASE * zoom * outputScale() / VECTOR_STEP)) * VECTOR_STEP);
        return renderVector(PX);
    }

    return renderRaster();
}

bool CShakeCursor::renderVector(int px) {
    // The best render in hand: the smallest one at least this big, since
    // shrinking scales down cleanly. Failing that, the biggest one there is,
    // scaled up until the sharp one arrives from the worker.
    const SRender* best   = nullptr;
    const SRender* backup = nullptr;
    for (const auto& R : m_renders) {
        if (R.shape != m_shape)
            continue;
        if (R.px >= px && (!best || R.px < best->px))
            best = &R;
        if (!backup || R.px > backup->px)
            backup = &R;
    }

    if (!best) {
        requestRender(m_shape, px);
        // One size ahead as well, so the next shake's render is in hand by
        // the time it is asked for.
        const double BASE = std::max(m_origSize.x / m_origScale, 1.0);
        requestRender(m_shape, px + std::max(VECTOR_STEP, sc<int>(std::round(BASE * OVER * outputScale() / VECTOR_STEP)) * VECTOR_STEP));
    }

    // Nothing rendered for this shape yet: keep showing whatever is on screen
    // now, the previous shape at its size, rather than drop to normal size
    // for the frames until the worker delivers. A wrong shape for a moment is
    // invisible; a size jump is not.
    const SRender* use = best ? best : backup;
    if (!use) {
        if (m_bigBuffer)
            return true;
        return false;
    }

    if (m_bigBuffer != use->buffer) {
        m_bigBuffer  = use->buffer;
        m_bigSize    = use->size;
        m_bigHotspot = use->hotspot;
        m_bigShape   = m_shape;
        m_bigPx      = use->px;
    }
    return true;
}

bool CShakeCursor::renderRaster() {
    if (!m_xcursor) {
        m_xcursor = makeUnique<CXCursorManager>();

        const auto THEME = getenv("XCURSOR_THEME");
        m_xcursor->loadTheme(THEME && *THEME ? THEME : "default", BIG_SIZE, 1.F);
    }

    if (m_bigBuffer && m_bigShape == m_shape && m_bigPx == 0)
        return true;

    auto shape = m_xcursor->getShape(m_shape, BIG_SIZE, 1.F);
    if (!shape || shape->images.empty())
        shape = m_xcursor->getShape("default", BIG_SIZE, 1.F);
    if (!shape || shape->images.empty())
        return false;

    const auto& FRAME = shape->images.front();
    if (FRAME.size.x <= 0 || FRAME.size.y <= 0 || FRAME.pixels.empty())
        return false;

    m_bigBuffer  = makeShared<Pointer::Cursor::CCursorBuffer>(rc<const uint8_t*>(FRAME.pixels.data()), FRAME.size, FRAME.hotspot);
    m_bigSize    = FRAME.size;
    m_bigHotspot = FRAME.hotspot;
    m_bigShape   = m_shape;
    m_bigPx      = 0;
    return true;
}

void CShakeCursor::apply(float zoom) {
    // Render for where the tween is heading, not for every frame on the way:
    // the frames in between show that render scaled, which is free.
    if (!ensureBigImage(std::max(zoom, m_target)))
        return;

    if (!m_locked) {
        Pointer::mgr()->lockSoftwareAll();
        m_locked = true;
    }

    // The logical size Hyprland shows is buffer size over scale. Aim for the
    // original's logical size times the zoom, whatever size the big image
    // actually came back at.
    const double BASE  = std::max(m_origSize.x / m_origScale, 1.0);
    const double SCALE = m_bigSize.x / (BASE * zoom);

    // The hotspot goes in logical units — what Hyprland's own cursor manager
    // hands over is the image hotspot divided by the scale — so the tip stays
    // on the pointer at every size and nothing jumps when the original
    // comes back.
    if (m_bigBuffer != m_appliedBuffer || std::abs(zoom - m_applied) > 0.005F) {
        Pointer::mgr()->setCursorBuffer(m_bigBuffer, m_bigHotspot / SCALE, sc<float>(SCALE));
        m_appliedBuffer = m_bigBuffer;
    }

    m_active  = true;
    m_applied = zoom;
}

void CShakeCursor::restoreOriginal() {
    if (m_active && Pointer::mgr()) {
        // Only hand back what is still ours to hand back.
        if (Pointer::mgr()->currentCursorImage().pBuffer == m_bigBuffer) {
            if (const auto SURF = m_origSurface.lock())
                Pointer::mgr()->setCursorSurface(SURF, m_origHotspot);
            else if (m_origBuffer)
                Pointer::mgr()->setCursorBuffer(m_origBuffer, m_origHotspot, m_origScale);
            else
                Pointer::mgr()->resetCursorImage();
        }
    }

    if (m_locked && Pointer::mgr()) {
        Pointer::mgr()->unlockSoftwareAll();
        m_locked = false;
    }

    m_active        = false;
    m_applied       = 1.F;
    m_appliedBuffer = nullptr;
    m_zoom          = 1.F;
    m_target        = 1.F;
    m_animFrom      = 1.F;
}
