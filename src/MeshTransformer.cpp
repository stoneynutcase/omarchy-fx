#include "MeshTransformer.hpp"
#include "globals.hpp"

#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/output/Monitor.hpp>

#ifdef OMARCHY_FX_HL057
#include <hyprland/src/helpers/DeformableMesh.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#else
#include <hyprland/src/output/MonitorResources.hpp>
#endif

#include <algorithm>
#include <array>

using namespace OmarchyFX;

CMeshTransformer::CMeshTransformer(SP<SMeshState> state) : m_state(state) {
    ;
}

CMeshTransformer::~CMeshTransformer() {
    if (m_state)
        m_state->alive = false;
}

// Cubic Bernstein weights, the basis of the Bezier surface the control lattice
// defines.
static void bernstein(double t, double out[4]) {
    const double S = 1.0 - t;
    out[0]         = S * S * S;
    out[1]         = 3.0 * S * S * t;
    out[2]         = 3.0 * S * t * t;
    out[3]         = t * t * t;
}

double CMeshTransformer::evaluateGrid(const CBox& box, int tess) const {
    const auto& OFFSETS = m_state->offsets;
    const int   SIDE    = tess + 1;

    // Where the window frame sits inside the bounding box we are handed. The
    // shadow and borders fall outside [0, 1] and the Bezier simply extrapolates
    // there, which keeps them attached to the window as it deforms.
    const Vector2D FRAMEPOS = Vector2D{box.x, box.y} + m_state->frameOffsetInBox;
    const Vector2D FRAMESZ  = m_state->frameSize;

    // Bernstein weights along each axis, evaluated once per mesh line.
    std::vector<std::array<double, 4>> bx(SIDE), by(SIDE);
    double                             maxOffset = 0;

    for (int i = 0; i < SIDE; ++i) {
        const double X = box.x + (box.w * i) / tess;
        bernstein(std::clamp((X - FRAMEPOS.x) / FRAMESZ.x, -0.25, 1.25), bx[i].data());
    }

    for (int j = 0; j < SIDE; ++j) {
        const double Y = box.y + (box.h * j) / tess;
        bernstein(std::clamp((Y - FRAMEPOS.y) / FRAMESZ.y, -0.25, 1.25), by[j].data());
    }

    m_grid.assign(SIDE * SIDE, Vector2D{});

    for (int j = 0; j < SIDE; ++j) {
        for (int i = 0; i < SIDE; ++i) {
            // The control net holds offsets, so the Bezier sum is the
            // displacement to add to the undeformed mesh point. An undeformed
            // net therefore yields exactly zero.
            Vector2D offset;
            for (int cj = 0; cj < Lattice::GRID; ++cj) {
                for (int ci = 0; ci < Lattice::GRID; ++ci) {
                    offset += OFFSETS[Lattice::index(ci, cj)] * (bx[i][ci] * by[j][cj]);
                }
            }

            m_grid[j * SIDE + i] = offset;
            maxOffset            = std::max({maxOffset, std::abs(offset.x), std::abs(offset.y)});
        }
    }

    return maxOffset;
}

// Below this the deformation is invisible, and we hand the framebuffer back
// rather than pay for a copy.
static constexpr double MIN_VISIBLE_OFFSET = 0.05;

#ifdef OMARCHY_FX_HL057

using namespace Render;

void CMeshTransformer::dropProgram() {
    ; // nothing of ours lives on the GPU: Hyprland's mesh renderer draws it
}

bool CMeshTransformer::active() const {
    // Hyprland destroys inactive transformers whenever it tidies the list, so
    // this has to stay true for as long as the manager holds a pointer to us.
    return m_state && m_state->alive;
}

CBox CMeshTransformer::transformedExtents(const CBox& currentBox) const {
    if (!m_state || currentBox.w <= 0 || currentBox.h <= 0 || m_state->frameSize.x <= 0 || m_state->frameSize.y <= 0)
        return currentBox;

    const int TESS = std::clamp(m_state->tessellation, 2, 64);
    if (evaluateGrid(currentBox, TESS) < MIN_VISIBLE_OFFSET)
        return currentBox;

    // The mesh is drawn as flat triangles between its vertices, so their
    // bounds are exactly what gets drawn. The window's own box is kept in the
    // union: a pulse that shrinks the window still has to clear where it was.
    double     minX = currentBox.x, minY = currentBox.y, maxX = currentBox.x + currentBox.w, maxY = currentBox.y + currentBox.h;
    const int  SIDE = TESS + 1;

    for (int j = 0; j < SIDE; ++j) {
        for (int i = 0; i < SIDE; ++i) {
            const Vector2D P = Vector2D{currentBox.x + (currentBox.w * i) / TESS, currentBox.y + (currentBox.h * j) / TESS} + m_grid[j * SIDE + i];
            minX             = std::min(minX, P.x);
            minY             = std::min(minY, P.y);
            maxX             = std::max(maxX, P.x);
            maxY             = std::max(maxY, P.y);
        }
    }

    // A pixel of slack for the linear filtering at the deformed edge.
    return CBox{minX, minY, maxX - minX, maxY - minY}.expand(2.0);
}

SWindowTransformBuffer CMeshTransformer::transform(CRenderContext& ctx, const SWindowTransformBuffer& in, const SWindowTransformContext& context) {
    // Snapshots (the close animation, for one) and standalone passes draw the
    // window as it is.
    if (!m_state || !m_state->alive || context.standalone || context.renderingSnapshot || !context.monitor || !in.framebuffer || !in.framebuffer->getTexture())
        return in;

    const CBox& BOX = context.currentBox;
    if (BOX.w <= 0 || BOX.h <= 0 || m_state->frameSize.x <= 0 || m_state->frameSize.y <= 0)
        return in;

    const int TESS = std::clamp(m_state->tessellation, 2, 64);
    const int SIDE = TESS + 1;

    if (evaluateGrid(BOX, TESS) < MIN_VISIBLE_OFFSET)
        return in;

    const double SCALE = context.monitor->m_scale;
    const auto   TEX   = in.framebuffer->getTexture();
    const auto   TEXSZ = TEX->m_size;
    if (SCALE <= 0 || TEXSZ.x <= 0 || TEXSZ.y <= 0)
        return in;

    // The output canvas Hyprland expects back: the part of our extents it
    // still needs, in monitor pixels, padded the way it pads its own.
    const auto CANVAS = pixelBoxForLogical(context.outputBox.copy().expand(1.0 / SCALE), SCALE);
    if (CANVAS.empty())
        return in;

    const auto OUT = g_pHyprRenderer->getWorkBuffer(ctx, CANVAS.size());
    if (!OUT)
        return {.framebuffer = in.framebuffer, .box = in.box, .success = false};

    // Positions are normalised to the output canvas, texture coordinates to
    // the input one. Both start from the same logical mesh point, so an
    // undeformed lattice is a pixel-exact copy.
    std::vector<SMeshRenderVertex> verts;
    verts.reserve(TESS * TESS * 6);

    const auto vertex = [&](int i, int j) -> SMeshRenderVertex {
        const Vector2D R = Vector2D{BOX.x + (BOX.w * i) / TESS, BOX.y + (BOX.h * j) / TESS};
        const Vector2D P = (R + m_grid[j * SIDE + i]) * SCALE - CANVAS.pos();
        const Vector2D T = R * SCALE - in.box.pos();
        return {
            .x = sc<float>(P.x / CANVAS.w),
            .y = sc<float>(P.y / CANVAS.h),
            .u = sc<float>(T.x / TEXSZ.x),
            .v = sc<float>(T.y / TEXSZ.y),
        };
    };

    for (int j = 0; j < TESS; ++j) {
        for (int i = 0; i < TESS; ++i) {
            verts.emplace_back(vertex(i, j));
            verts.emplace_back(vertex(i, j + 1));
            verts.emplace_back(vertex(i + 1, j));

            verts.emplace_back(vertex(i + 1, j));
            verts.emplace_back(vertex(i, j + 1));
            verts.emplace_back(vertex(i + 1, j + 1));
        }
    }

    GL::CFramebufferBindingGuard bindings{g_pHyprRenderer->glBackend()};
    auto                         drawState  = ctx.saveDrawState();
    auto&                        renderData = ctx.m_data;

    g_pHyprRenderer->bindFB(ctx, OUT);
    renderData.damage = CRegion{0.0, 0.0, CANVAS.w, CANVAS.h};
    renderData.fbSize = CANVAS.size();
    g_pHyprRenderer->setProjectionType(ctx, RPT_EXPORT);

    g_pHyprRenderer->draw(ctx, CClearPassElement::SClearData{CHyprColor(0, 0, 0, 0)});
    GL::g_pHyprOpenGL->renderTextureMesh(ctx, TEX, CBox{0, 0, CANVAS.w, CANVAS.h}, verts,
                                         GL::CHyprOpenGLImpl::STextureRenderData{
                                             .damage        = &renderData.damage,
                                             .a             = 1.F,
                                             .allowCustomUV = true,
                                         });

    return {.framebuffer = OUT, .box = CANVAS};
}

#else // Hyprland 0.56

static const char* VERT_SRC = R"#(#version 300 es
uniform mat3 proj;
in vec2 pos;
in vec2 texcoord;
out vec2 v_texcoord;

void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = texcoord;
}
)#";

static const char* FRAG_SRC = R"#(#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
out vec4 fragColor;

void main() {
    fragColor = texture(tex, v_texcoord);
}
)#";

// One program for every deformed window; created lazily on the render thread.
static GLuint g_program   = 0;
static GLint  g_uProj     = -1;
static GLint  g_uTex      = -1;
static GLint  g_aPos      = -1;
static GLint  g_aTexcoord = -1;
static GLuint g_vbo       = 0;

static GLuint compileShader(GLenum type, const char* src) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char  log[1024] = {0};
        GLint len       = 0;
        glGetShaderInfoLog(shader, sizeof(log) - 1, &len, log);
        FX_LOG(Log::ERR, "[omarchy-fx] shader compile failed: {}", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool CMeshTransformer::ensureProgram() {
    if (g_program)
        return true;

    const GLuint vert = compileShader(GL_VERTEX_SHADER, VERT_SRC);
    if (!vert)
        return false;

    const GLuint frag = compileShader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!frag) {
        glDeleteShader(vert);
        return false;
    }

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    glDetachShader(prog, vert);
    glDetachShader(prog, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char  log[1024] = {0};
        GLint len       = 0;
        glGetProgramInfoLog(prog, sizeof(log) - 1, &len, log);
        FX_LOG(Log::ERR, "[omarchy-fx] program link failed: {}", log);
        glDeleteProgram(prog);
        return false;
    }

    g_program   = prog;
    g_uProj     = glGetUniformLocation(prog, "proj");
    g_uTex      = glGetUniformLocation(prog, "tex");
    g_aPos      = glGetAttribLocation(prog, "pos");
    g_aTexcoord = glGetAttribLocation(prog, "texcoord");

    glGenBuffers(1, &g_vbo);

    return true;
}

void CMeshTransformer::dropProgram() {
    if (g_vbo) {
        glDeleteBuffers(1, &g_vbo);
        g_vbo = 0;
    }

    if (g_program) {
        glDeleteProgram(g_program);
        g_program = 0;
    }
}

void CMeshTransformer::preWindowRender(CSurfacePassElement::SRenderData* pRenderData) {
    // Snapshot and standalone passes never call amendTransformedRenderData, so
    // this is how we learn that the box we are about to get handed is stale.
    m_boxValid = false;
}

void CMeshTransformer::amendTransformedRenderData(const CBox& currentBox, SMotionBlurData* pMotionBlurData) {
    m_currentBox = currentBox;
    m_boxValid   = true;
}

SP<Render::IFramebuffer> CMeshTransformer::transform(SP<Render::IFramebuffer> in) {
    if (!m_state || !m_boxValid)
        return in;

    if (m_currentBox.w <= 0 || m_currentBox.h <= 0 || m_state->frameSize.x <= 0 || m_state->frameSize.y <= 0)
        return in;

    const auto PMONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!PMONITOR || !in || !in->getTexture())
        return in;

    const auto RESOURCES = PMONITOR->resources();
    if (!RESOURCES)
        return in;

    const auto OUT = RESOURCES->getUnusedWorkBuffer();
    if (!OUT || !OUT->isAllocated())
        return in;

    if (!ensureProgram())
        return in;

    const auto   FBSIZE = in->m_size;
    const double SCALE  = PMONITOR->m_scale;

    if (FBSIZE.x <= 0 || FBSIZE.y <= 0)
        return in;

    const int TESS = std::clamp(m_state->tessellation, 2, 64);
    const int SIDE = TESS + 1;

    // Normalised coordinates inside the work framebuffer. Positions and texture
    // coordinates share this space, so an undeformed lattice is a pixel-exact
    // copy of the input.
    const auto norm = [&FBSIZE, SCALE](const Vector2D& logical) -> Vector2D {
        return Vector2D{(logical.x * SCALE) / FBSIZE.x, (logical.y * SCALE) / FBSIZE.y};
    };

    if (evaluateGrid(m_currentBox, TESS) < MIN_VISIBLE_OFFSET)
        return in;

    m_verts.clear();
    m_verts.reserve(TESS * TESS * 6 * 4);

    const auto emit = [&](int i, int j) {
        const auto R = Vector2D{m_currentBox.x + (m_currentBox.w * i) / TESS, m_currentBox.y + (m_currentBox.h * j) / TESS};
        const auto P = norm(R + m_grid[j * SIDE + i]);
        const auto T = norm(R);
        m_verts.push_back(sc<float>(P.x));
        m_verts.push_back(sc<float>(P.y));
        m_verts.push_back(sc<float>(T.x));
        m_verts.push_back(sc<float>(T.y));
    };

    for (int j = 0; j < TESS; ++j) {
        for (int i = 0; i < TESS; ++i) {
            emit(i, j);
            emit(i + 1, j);
            emit(i, j + 1);

            emit(i + 1, j);
            emit(i + 1, j + 1);
            emit(i, j + 1);
        }
    }

    const auto PROJ = g_pHyprRenderer->projectBoxToTarget(CBox{0.0, 0.0, FBSIZE.x, FBSIZE.y});
    const auto MAT  = PROJ.getMatrix();

    // Save the bits of GL state Hyprland assumes it still owns.
    GLint oldProgram = 0, oldVAO = 0, oldArrayBuffer = 0, oldTexture = 0, oldUnit = GL_TEXTURE0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);

    {
        const auto GUARD = g_pHyprRenderer->bindTempFB(OUT);

        g_pHyprRenderer->disableScissor();
        g_pHyprRenderer->blend(false);

        glClearColor(0.F, 0.F, 0.F, 0.F);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindVertexArray(0);
        glUseProgram(g_program);
        glUniformMatrix3fv(g_uProj, 1, GL_TRUE, MAT.data());

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in->getTexture()->m_texID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUniform1i(g_uTex, 0);

        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, m_verts.size() * sizeof(float), m_verts.data(), GL_STREAM_DRAW);

        glVertexAttribPointer(g_aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        glVertexAttribPointer(g_aTexcoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), rc<void*>(2 * sizeof(float)));
        glEnableVertexAttribArray(g_aPos);
        glEnableVertexAttribArray(g_aTexcoord);

        glDrawArrays(GL_TRIANGLES, 0, sc<GLsizei>(m_verts.size() / 4));

        glDisableVertexAttribArray(g_aPos);
        glDisableVertexAttribArray(g_aTexcoord);
    }

    glBindBuffer(GL_ARRAY_BUFFER, oldArrayBuffer);
    glBindVertexArray(oldVAO);
    glBindTexture(GL_TEXTURE_2D, oldTexture);
    glActiveTexture(oldUnit);
    glUseProgram(oldProgram);
    g_pHyprRenderer->blend(true);

    return OUT;
}

#endif
