#include "WobblyTransformer.hpp"
#include "globals.hpp"

#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/output/MonitorResources.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <algorithm>
#include <array>

using namespace OmarchyFX;

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

// One program for every wobbling window; created lazily on the render thread.
static GLuint g_program  = 0;
static GLint  g_uProj    = -1;
static GLint  g_uTex     = -1;
static GLint  g_aPos     = -1;
static GLint  g_aTexcoord = -1;
static GLuint g_vbo      = 0;

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
        Log::logger->log(Log::ERR, "[omarchy-fx] shader compile failed: {}", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool CWobblyTransformer::ensureProgram() {
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
        Log::logger->log(Log::ERR, "[omarchy-fx] program link failed: {}", log);
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

void CWobblyTransformer::dropProgram() {
    if (g_vbo) {
        glDeleteBuffers(1, &g_vbo);
        g_vbo = 0;
    }

    if (g_program) {
        glDeleteProgram(g_program);
        g_program = 0;
    }
}

CWobblyTransformer::CWobblyTransformer(SP<SWobblyState> state) : m_state(state) {
    ;
}

CWobblyTransformer::~CWobblyTransformer() {
    if (m_state)
        m_state->alive = false;
}

void CWobblyTransformer::preWindowRender(CSurfacePassElement::SRenderData* pRenderData) {
    // Snapshot and standalone passes never call amendTransformedRenderData, so
    // this is how we learn that the box we are about to get handed is stale.
    m_boxValid = false;
}

void CWobblyTransformer::amendTransformedRenderData(const CBox& currentBox, SMotionBlurData* pMotionBlurData) {
    m_currentBox = currentBox;
    m_boxValid   = true;
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

SP<Render::IFramebuffer> CWobblyTransformer::transform(SP<Render::IFramebuffer> in) {
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

    const auto&  MODEL  = m_state->model;
    const auto   FBSIZE = in->m_size;
    const double SCALE  = PMONITOR->m_scale;

    if (FBSIZE.x <= 0 || FBSIZE.y <= 0)
        return in;

    const int TESS = std::clamp(m_state->params.tessellation, 2, 64);
    const int SIDE = TESS + 1;

    // Where the window frame sits inside the bounding box we are handed. The
    // shadow and borders fall outside [0, 1] and the Bezier simply extrapolates
    // there, which keeps them attached to the window as it deforms.
    const Vector2D FRAMEPOS = Vector2D{m_currentBox.x, m_currentBox.y} + m_state->frameOffsetInBox;
    const Vector2D FRAMESZ  = m_state->frameSize;

    // Normalised coordinates inside the work framebuffer. Positions and texture
    // coordinates share this space, so an undeformed lattice is a pixel-exact
    // copy of the input.
    const auto norm = [&FBSIZE, SCALE](const Vector2D& logical) -> Vector2D {
        return Vector2D{(logical.x * SCALE) / FBSIZE.x, (logical.y * SCALE) / FBSIZE.y};
    };

    // Bernstein weights along each axis, evaluated once per mesh line.
    std::vector<std::array<double, 4>> bx(SIDE), by(SIDE);
    double                             maxOffset = 0;

    for (int i = 0; i < SIDE; ++i) {
        const double X = m_currentBox.x + (m_currentBox.w * i) / TESS;
        bernstein(std::clamp((X - FRAMEPOS.x) / FRAMESZ.x, -0.25, 1.25), bx[i].data());
    }

    for (int j = 0; j < SIDE; ++j) {
        const double Y = m_currentBox.y + (m_currentBox.h * j) / TESS;
        bernstein(std::clamp((Y - FRAMEPOS.y) / FRAMESZ.y, -0.25, 1.25), by[j].data());
    }

    m_grid.assign(SIDE * SIDE, Vector2D{});

    for (int j = 0; j < SIDE; ++j) {
        for (int i = 0; i < SIDE; ++i) {
            // The control net holds offsets, so the Bezier sum is the
            // displacement to add to the undeformed mesh point. An undeformed
            // net therefore yields exactly zero.
            Vector2D offset;
            for (int cj = 0; cj < CWobblyModel::GRID; ++cj) {
                for (int ci = 0; ci < CWobblyModel::GRID; ++ci) {
                    offset += MODEL.controlOffset(ci, cj) * (bx[i][ci] * by[j][cj]);
                }
            }

            m_grid[j * SIDE + i] = offset;
            maxOffset            = std::max({maxOffset, std::abs(offset.x), std::abs(offset.y)});
        }
    }

    // Nothing to warp — hand the framebuffer straight back rather than pay for
    // a full-screen copy.
    if (maxOffset < 0.05)
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
