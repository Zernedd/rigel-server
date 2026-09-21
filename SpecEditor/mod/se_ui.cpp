// se_ui.cpp - the editor UI. Render thread only: reads the published snapshot, pushes commands.
//
// Layout follows the Unreal editor closely enough to be muscle-memory compatible:
//   menu bar  |  Place Actors (left)  |  viewport overlay + gizmo toolbar (centre)  |
//   World Outliner (right top)  |  Details (right bottom)  |  Quests (tab beside Details)
//
// The viewport itself is the game's own render - we draw the gizmo and selection overlay on top with
// ImGui's foreground draw list, so there is no render-target work and nothing to keep in sync.

#include "se_core.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace se {
namespace {

enum class GizmoMode { Select, Translate, Rotate, Scale };
GizmoMode g_gizmo = GizmoMode::Translate;
bool      g_worldSpace = true;
float     g_gridSnap = 10.0f;
bool      g_snapEnabled = true;
float     g_rotSnap = 15.0f;

char g_paletteFilter[96] = {};
char g_outlinerFilter[96] = {};
std::string g_selected;          // handle
int  g_activeTab = 0;            // 0 details, 1 quests

// ── quest authoring model, held on the render thread and compiled through the game thread ────
struct QuestStep
{
    std::string objectHandle;
    std::string label;
    int         kind = 0;        // 0 reach, 1 collect, 2 interact
};
struct QuestDraft
{
    char        name[64] = "PKR_Custom_Quest";
    char        title[96] = "Custom Parkour Quest";
    char        glyph[32] = "PKRClimb5";
    int         repetition = 0;  // Once / Daily / Weekly / Monthly
    int         validSec = 0;    // ValidLengthSeconds: how long an activation stays valid (0 = open-ended)
    float       reqProgress = 0; // OptionalRequiredProgress (0 = the template's default)
    char        desc[200] = "";
    std::vector<QuestStep> steps;
};
QuestDraft g_quest;

const char* GizmoName(GizmoMode m)
{
    switch (m)
    {
    case GizmoMode::Select:    return "Select";
    case GizmoMode::Translate: return "Move";
    case GizmoMode::Rotate:    return "Rotate";
    default:                   return "Scale";
    }
}

// ── viewport projection ───────────────────────────────────────────────────────────────────────
// The game thread publishes the camera manager's real POV (location, rotation, horizontal FOV), so the
// overlay can project exactly like the engine does: UE is X forward, Y right, Z up, and the axes of a
// rotator are the rows of FRotationMatrix. With that, the gizmo sits ON the object and its handles run
// along the object's real axes, the way the Unreal editor's does.
struct View
{
    Vec3  eye;
    Vec3  fwd, right, up;
    float focal = 1.0f;          // pixels per unit at depth 1
    ImVec2 centre;
    bool  valid = false;
};

Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 Mul(const Vec3& a, double s)      { return { a.x * s, a.y * s, a.z * s }; }
double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Rows of UE's FRotationMatrix for a rotator given in degrees: X (forward), Y (right), Z (up).
void RotAxes(const Rot& r, Vec3& x, Vec3& y, Vec3& z)
{
    const double d2r = 3.14159265358979 / 180.0;
    const double sp = std::sin(r.pitch * d2r), cp = std::cos(r.pitch * d2r);
    const double sy = std::sin(r.yaw * d2r),   cy = std::cos(r.yaw * d2r);
    const double sr = std::sin(r.roll * d2r),  cr = std::cos(r.roll * d2r);
    x = { cp * cy, cp * sy, sp };
    y = { sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp };
    z = { -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp };
}

View MakeView(const Snapshot& snap)
{
    View v;
    const ImVec2 sz = ImGui::GetIO().DisplaySize;
    if (sz.x <= 0 || sz.y <= 0 || snap.cameraFov <= 1.0f) return v;
    v.eye = snap.cameraPos;
    RotAxes(snap.cameraRot, v.fwd, v.right, v.up);
    v.centre = ImVec2(sz.x * 0.5f, sz.y * 0.5f);
    v.focal  = static_cast<float>((sz.x * 0.5) / std::tan(snap.cameraFov * 0.5 * 3.14159265358979 / 180.0));
    v.valid  = true;
    return v;
}

// World -> screen. Returns false when the point is behind (or practically on) the camera plane.
bool W2S(const View& v, const Vec3& p, ImVec2& out, double* depth = nullptr)
{
    if (!v.valid) return false;
    const Vec3 d = Sub(p, v.eye);
    const double z = Dot(d, v.fwd);
    if (z < 1.0) return false;
    out = ImVec2(v.centre.x + static_cast<float>(Dot(d, v.right) * v.focal / z),
                 v.centre.y - static_cast<float>(Dot(d, v.up)    * v.focal / z));
    if (depth) *depth = z;
    return true;
}

float Snap(float v, float step, bool on);

// Interactive translate/rotate/scale gizmo, drawn on the selected object.
//
// Handles run along the world axes (World) or the object's own axes (Local), projected through the real
// camera and kept a constant size on screen, like the Unreal editor's.
//  * Move: the object follows the point on the axis closest to the mouse ray, so it stays under the
//    cursor at any view angle.
//  * Rotate: grab a RING (not an invisible axis line); the angle the mouse sweeps around the centre is
//    the angle applied, about the true axis -- world or local -- so it turns the way the ring is dragged.
//  * Scale: proportional to how far the handle is pulled.
// While dragging, the gizmo is drawn at the target, and the target goes to the game thread every frame
// through LiveDrag (se_core.h), which moves the object the same frame and forwards it to the server.
int    g_dragAxis = -1;         // 0 X, 1 Y, 2 Z, -1 none
bool   g_gizmoHover = false;    // mouse is over a handle this frame: viewport clicks must not select
Vec3   g_dragStartLoc, g_dragStartScale;
Rot    g_dragStartRot;
ImVec2 g_dragStartMouse;
Vec3   g_dragAxisWorld;         // the world direction being dragged along / rotated about
double g_dragAxisS0 = 0.0;      // Move: where on the axis the grab happened
double g_dragAngPrev = 0.0, g_dragAngSum = 0.0;   // Rotate: unwrapped swept angle, radians
bool   g_rotTangent = false;    // Rotate: ring nearly edge-on -> drag along its tangent instead of sweeping
ImVec2 g_rotTan;                // Rotate: screen direction of a positive turn at the grab point (unit)
Vec3   g_pendLoc, g_pendScale;
Rot    g_pendRot;
std::string g_dragHandle;
std::string g_dragPath;         // Content Browser tile being dragged into the viewport

Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
Vec3 Norm(const Vec3& a) { const double l = std::sqrt(Dot(a, a)); return l > 1e-9 ? Mul(a, 1.0 / l) : a; }

// Rodrigues: v rotated about unit axis k by t radians.
Vec3 RotateAbout(const Vec3& v, const Vec3& k, double t)
{
    const double c = std::cos(t), s = std::sin(t);
    return Add(Add(Mul(v, c), Mul(Cross(k, v), s)), Mul(k, Dot(k, v) * (1.0 - c)));
}
// Inverse of RotAxes: the rotator whose X/Y/Z axes these are.
Rot RotFromAxes(const Vec3& x, const Vec3& y, const Vec3& z)
{
    const double r2d = 180.0 / 3.14159265358979;
    Rot r;
    r.pitch = std::atan2(x.z, std::sqrt(x.x * x.x + x.y * x.y)) * r2d;
    r.yaw   = std::atan2(x.y, x.x) * r2d;
    r.roll  = std::atan2(-y.z, z.z) * r2d;
    return r;
}

// The world ray under a screen point.
Vec3 ScreenRay(const View& v, ImVec2 m)
{
    return Norm(Add(v.fwd, Add(Mul(v.right, (m.x - v.centre.x) / v.focal), Mul(v.up, -(m.y - v.centre.y) / v.focal))));
}
// Parameter s of the point on line (p0 + s*a) closest to the ray (e + t*r); false when nearly parallel.
bool AxisParamUnderRay(const Vec3& p0, const Vec3& a, const Vec3& e, const Vec3& r, double& s)
{
    const Vec3 w = Sub(p0, e);
    const double b = Dot(a, r), d = Dot(a, w), f = Dot(r, w);
    const double den = 1.0 - b * b;
    if (den < 0.02) return false;
    s = (b * f - d) / den;
    return true;
}

float SegDist(ImVec2 p, ImVec2 a, ImVec2 b)
{
    const float sx = b.x - a.x, sy = b.y - a.y, l2 = sx * sx + sy * sy;
    float t = l2 > 0 ? ((p.x - a.x) * sx + (p.y - a.y) * sy) / l2 : 0.0f;
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    const float dx = a.x + sx * t - p.x, dy = a.y + sy * t - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

void WriteDrag(bool active)
{
    LiveDrag& d = Drag();
    std::lock_guard<std::mutex> lk(d.mx);
    d.active = active;
    d.handle = g_dragHandle;
    d.loc = g_pendLoc; d.rot = g_pendRot; d.scale = g_pendScale;
    ++d.seq;
}

void DrawGizmoOverlay(const Snapshot& snap, const SceneObject* sel)
{
    g_gizmoHover = false;
    if (!sel || g_gizmo == GizmoMode::Select)
    {
        if (g_dragAxis >= 0) { WriteDrag(false); g_dragAxis = -1; }
        return;
    }
    const View v = MakeView(snap);
    const bool dragging = g_dragAxis >= 0 && g_dragHandle == sel->handle;

    // While dragging, draw at the target: the snapshot is a frame behind the game thread.
    const Vec3 at  = dragging ? g_pendLoc : sel->location;
    const Rot  atR = dragging ? g_pendRot : sel->rotation;
    ImVec2 c;
    double depth = 1.0;
    if (!W2S(v, at, c, &depth)) return;                // selection is behind the camera

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float  L = 95.0f;                            // handle length on screen, pixels
    const double worldPerPx = depth / v.focal;         // at the object's depth

    Vec3 ax[3];
    if (g_worldSpace) { ax[0] = { 1, 0, 0 }; ax[1] = { 0, 1, 0 }; ax[2] = { 0, 0, 1 }; }
    else              RotAxes(dragging && g_gizmo == GizmoMode::Rotate ? g_dragStartRot : atR, ax[0], ax[1], ax[2]);

    ImVec2 ends[3];
    bool   endOk[3];
    for (int i = 0; i < 3; ++i)
    {
        endOk[i] = W2S(v, Add(at, Mul(ax[i], L * worldPerPx)), ends[i]);
        if (!endOk[i]) ends[i] = c;
    }
    // Rotate rings, projected: the circle in the plane normal to each axis.
    constexpr int kRing = 64;
    ImVec2 ring[3][kRing];
    Vec3   ringW[3][kRing];         // the world point of each drawn sample
    int    ringN[3] = { 0, 0, 0 };
    if (g_gizmo == GizmoMode::Rotate)
        for (int i = 0; i < 3; ++i)
        {
            const Vec3& a = ax[(i + 1) % 3];
            const Vec3& b = ax[(i + 2) % 3];
            for (int k = 0; k < kRing; ++k)
            {
                const double t = k * 2.0 * 3.14159265358979 / kRing;
                const Vec3 p = Add(at, Add(Mul(a, std::cos(t) * L * worldPerPx), Mul(b, std::sin(t) * L * worldPerPx)));
                if (W2S(v, p, ring[i][ringN[i]])) { ringW[i][ringN[i]] = p; ++ringN[i]; }
            }
        }

    const ImU32 axisCol[3] = { IM_COL32(220, 70, 70, 255), IM_COL32(90, 200, 90, 255), IM_COL32(80, 140, 235, 255) };
    const ImU32 hot        = IM_COL32(255, 220, 90, 255);
    const ImVec2 mouse     = ImGui::GetIO().MousePos;
    const bool uiWantsMouse = ImGui::GetIO().WantCaptureMouse;

    // Pick what is actually drawn: the ring in Rotate, the handle segment otherwise.
    int hover = -1, hoverK = 0;
    if (g_dragAxis < 0 && !uiWantsMouse && !Cam().looking)
    {
        float best = g_gizmo == GizmoMode::Rotate ? 9.0f : 10.0f;
        for (int i = 0; i < 3; ++i)
        {
            float d = 1e9f;
            int   dk = 0;
            if (g_gizmo == GizmoMode::Rotate)
            {
                for (int k = 0; k < ringN[i]; ++k)
                {
                    const float sd = SegDist(mouse, ring[i][k], ring[i][(k + 1) % ringN[i]]);
                    if (sd < d) { d = sd; dk = k; }
                }
                // A ring seen nearly edge-on is a thin ellipse; people click its middle, not its rim, so
                // the whole flattened line through the centre counts.
                if (std::fabs(Dot(ax[i], v.fwd)) < 0.35)
                {
                    const Vec3 u = Norm(Cross(v.fwd, ax[i]));
                    ImVec2 e1, e2;
                    if (W2S(v, Add(at, Mul(u, L * worldPerPx)), e1) && W2S(v, Sub(at, Mul(u, L * worldPerPx)), e2))
                        d = (std::min)(d, SegDist(mouse, e1, e2));
                }
                // Where rings cross, prefer the one facing the camera: an edge-on ring is the hard one.
                d += static_cast<float>((1.0 - std::fabs(Dot(ax[i], v.fwd))) * 4.0);
            }
            else if (endOk[i])
            {
                const ImVec2 from(c.x + (ends[i].x - c.x) * 0.15f, c.y + (ends[i].y - c.y) * 0.15f);
                d = SegDist(mouse, from, ends[i]);
            }
            if (d < best) { best = d; hover = i; hoverK = dk; }
        }
    }
    g_gizmoHover = hover >= 0;

    if (hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !sel->lockedByOther)
    {
        g_dragAxis       = hover;
        g_dragStartLoc   = sel->location;
        g_dragStartRot   = sel->rotation;
        g_dragStartScale = sel->scale;
        g_dragStartMouse = mouse;
        g_dragAxisWorld  = ax[hover];
        g_dragHandle     = sel->handle;
        g_pendLoc = sel->location; g_pendRot = sel->rotation; g_pendScale = sel->scale;
        g_dragAngPrev = std::atan2(mouse.y - c.y, mouse.x - c.x);
        g_dragAngSum  = 0.0;
        g_rotTangent  = false;
        if (g_gizmo == GizmoMode::Rotate && ringN[hover] > 0)
        {
            // A ring seen nearly side-on has no angle to sweep: drag along the direction the grabbed point
            // would move for a positive turn (axis x radius), as Unreal's rotate widget does.
            g_rotTangent = std::fabs(Dot(ax[hover], v.fwd)) < 0.35;
            const Vec3 P = ringW[hover][hoverK];
            const Vec3 tan = Norm(Cross(ax[hover], Sub(P, at)));
            ImVec2 p0, p1;
            float tx = 0, ty = 0;
            if (W2S(v, P, p0) && W2S(v, Add(P, Mul(tan, 10.0 * worldPerPx)), p1)) { tx = p1.x - p0.x; ty = p1.y - p0.y; }
            float tl = std::sqrt(tx * tx + ty * ty);
            if (tl < 2.0f)                          // the tangent points at the camera: use the ring's own line
            {
                tx = -(ring[hover][hoverK].y - c.y); ty = ring[hover][hoverK].x - c.x;
                tl = std::sqrt(tx * tx + ty * ty);
            }
            g_rotTan = tl > 0.0f ? ImVec2(tx / tl, ty / tl) : ImVec2(1, 0);
        }
        double s0 = 0.0;
        g_dragAxisS0 = AxisParamUnderRay(g_dragStartLoc, g_dragAxisWorld, v.eye, ScreenRay(v, mouse), s0) ? s0 : 0.0;
    }

    if (g_dragAxis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        WriteDrag(false);               // release: the game thread sends the final transform
        g_dragAxis = -1;
    }

    if (g_dragAxis >= 0 && g_dragHandle == sel->handle)
    {
        Vec3 loc = g_dragStartLoc, scale = g_dragStartScale;
        Rot  rot = g_dragStartRot;
        const double rotSign = Dot(g_dragAxisWorld, v.fwd) < 0.0 ? 1.0 : -1.0;
        if (g_gizmo == GizmoMode::Translate)
        {
            double s = 0.0, dWorld;
            if (AxisParamUnderRay(g_dragStartLoc, g_dragAxisWorld, v.eye, ScreenRay(v, mouse), s))
                dWorld = s - g_dragAxisS0;
            else                                                   // axis points at the camera: screen drag
                dWorld = (g_dragStartMouse.y - mouse.y) * worldPerPx;
            dWorld = (std::max)(-50000.0, (std::min)(50000.0, dWorld));
            dWorld = Snap(static_cast<float>(dWorld), g_gridSnap, g_snapEnabled);
            loc = Add(g_dragStartLoc, Mul(g_dragAxisWorld, dWorld));
        }
        else if (g_gizmo == GizmoMode::Rotate)
        {
            const double ang = std::atan2(mouse.y - c.y, mouse.x - c.x);
            double da = ang - g_dragAngPrev;
            while (da >  3.14159265358979) da -= 2 * 3.14159265358979;
            while (da < -3.14159265358979) da += 2 * 3.14159265358979;
            g_dragAngPrev = ang;
            g_dragAngSum += da;
            // Screen y points down, so a clockwise sweep grows the angle. With the axis pointing at the
            // camera a positive turn looks clockwise; from behind it looks anticlockwise.
            double deg = g_dragAngSum * 180.0 / 3.14159265358979 * rotSign;
            if (g_rotTangent)                       // mouse travel along the tangent, one ring radius = 1 rad
            {
                const double along = (mouse.x - g_dragStartMouse.x) * g_rotTan.x + (mouse.y - g_dragStartMouse.y) * g_rotTan.y;
                deg = along / L * 180.0 / 3.14159265358979;
            }
            deg = Snap(static_cast<float>(deg), g_rotSnap, g_snapEnabled);
            Vec3 x0, y0, z0;
            RotAxes(g_dragStartRot, x0, y0, z0);
            const double t = deg * 3.14159265358979 / 180.0;
            rot = RotFromAxes(RotateAbout(x0, g_dragAxisWorld, t), RotateAbout(y0, g_dragAxisWorld, t),
                              RotateAbout(z0, g_dragAxisWorld, t));
            char a[32];
            snprintf(a, sizeof(a), "%+.1f deg", deg);
            dl->AddText(ImVec2(mouse.x + 16, mouse.y - 18), hot, a);
        }
        else
        {
            const ImVec2 dir(ends[g_dragAxis].x - c.x, ends[g_dragAxis].y - c.y);
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            const double along = len > 1.0f
                ? ((mouse.x - g_dragStartMouse.x) * dir.x + (mouse.y - g_dragStartMouse.y) * dir.y) / len : 0.0;
            const double f = (std::max)(0.05, 1.0 + along / L);
            if (g_dragAxis == 0) scale.x = (std::max)(0.01, g_dragStartScale.x * f);
            if (g_dragAxis == 1) scale.y = (std::max)(0.01, g_dragStartScale.y * f);
            if (g_dragAxis == 2) scale.z = (std::max)(0.01, g_dragStartScale.z * f);
        }
        const bool changed = loc.x != g_pendLoc.x || loc.y != g_pendLoc.y || loc.z != g_pendLoc.z ||
                             rot.pitch != g_pendRot.pitch || rot.yaw != g_pendRot.yaw || rot.roll != g_pendRot.roll ||
                             scale.x != g_pendScale.x || scale.y != g_pendScale.y || scale.z != g_pendScale.z;
        g_pendLoc = loc; g_pendRot = rot; g_pendScale = scale;
        if (changed) WriteDrag(true);
    }

    if (g_gizmo == GizmoMode::Rotate)
        for (int i = 0; i < 3; ++i)
            if (ringN[i] > 2)
            {
                const bool h = g_dragAxis == i || hover == i;
                dl->AddPolyline(ring[i], ringN[i], h ? hot : axisCol[i], ImDrawFlags_Closed, h ? 4.0f : 2.5f);
            }
    for (int i = 0; i < 3; ++i)
    {
        if (!endOk[i] || g_gizmo == GizmoMode::Rotate) continue;
        const bool h = g_dragAxis == i || hover == i;
        const ImU32 col = h ? hot : axisCol[i];
        dl->AddLine(c, ends[i], col, h ? 4.0f : 3.0f);
        if (g_gizmo == GizmoMode::Scale)
            dl->AddRectFilled(ImVec2(ends[i].x - 6, ends[i].y - 6), ImVec2(ends[i].x + 6, ends[i].y + 6), col);
        else
            dl->AddCircleFilled(ends[i], 7.0f, col);
    }
    dl->AddCircleFilled(c, 4.0f, IM_COL32(230, 230, 230, 255));
}

// ── clicking objects in the viewport ─────────────────────────────────────────────────────────
// No markers: click the object itself, as in Unreal. What is under the cursor is decided on the game
// thread by a line trace (PickTick, se_game.cpp); here we only draw the outlines: faint for the hovered
// object, orange for the selection.
Vec3 PickExtent(const SceneObject& o)
{
    const double m = 25.0;          // bare actors (no mesh) still get something to click
    return { (std::max)(m, o.boundsExt.x), (std::max)(m, o.boundsExt.y), (std::max)(m, o.boundsExt.z) };
}

void DrawBox(ImDrawList* dl, const View& v, const Vec3& c, const Vec3& h, ImU32 col, float th)
{
    Vec3 p[8];
    for (int i = 0; i < 8; ++i)
        p[i] = { c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 4) ? h.z : -h.z) };
    static const int e[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
    for (const auto& ed : e)
    {
        ImVec2 a, b;
        if (W2S(v, p[ed[0]], a) && W2S(v, p[ed[1]], b)) dl->AddLine(a, b, col, th);
    }
}

std::string PrettyName(const std::string& cls);

void DrawViewportMarkers(const Snapshot& snap)
{
    const View v = MakeView(snap);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool canPick = v.valid && snap.inEditor && !ImGui::GetIO().WantCaptureMouse && g_dragAxis < 0 &&
                         !g_gizmoHover && !Cam().looking && g_dragPath.empty();

    // Hand the mouse ray to the game thread; take its answer from the previous frame (see PickState).
    std::string hoveredHandle;
    {
        PickState& ps = Pick();
        std::lock_guard<std::mutex> lk(ps.mx);
        ps.valid = canPick;
        if (canPick) { ps.eye = v.eye; ps.dir = ScreenRay(v, mouse); }
        hoveredHandle = canPick ? ps.hovered : std::string();
    }
    const SceneObject* hovered = nullptr;
    for (const auto& o : snap.objects) if (!hoveredHandle.empty() && o.handle == hoveredHandle) { hovered = &o; break; }

    if (v.valid)
        for (const auto& o : snap.objects)
        {
            const bool isSel = o.handle == g_selected;
            if (!isSel && &o != hovered) continue;
            Vec3 ctr = Add(o.location, o.boundsOff);
            if (isSel && g_dragAxis >= 0 && g_dragHandle == o.handle) ctr = Add(g_pendLoc, o.boundsOff);
            DrawBox(dl, v, ctr, PickExtent(o),
                    isSel ? IM_COL32(255, 160, 40, 230) : o.lockedByOther ? IM_COL32(220, 120, 60, 150) : IM_COL32(230, 230, 230, 110),
                    isSel ? 2.0f : 1.2f);
        }
    if (hovered && hovered->handle != g_selected)
    {
        const std::string tip = PrettyName(hovered->className) + (hovered->lockedByOther ? "  (locked)" : "");
        dl->AddText(ImVec2(mouse.x + 16, mouse.y + 8), IM_COL32(230, 230, 230, 220), tip.c_str());
    }

    if (!canPick || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (hovered)
    {
        if (hovered->handle == g_selected || hovered->lockedByOther) return;
        if (!g_selected.empty()) { Command d{ CmdType::DeselectObject }; d.str = g_selected; State().Push(d); }
        g_selected = hovered->handle;
        Command s{ CmdType::SelectObject }; s.str = g_selected; State().Push(s);
    }
    else if (!g_selected.empty())
    {
        Command d{ CmdType::DeselectObject }; d.str = g_selected; State().Push(d);
        g_selected.clear();
    }
}

float Snap(float v, float step, bool on) { return (on && step > 0.0f) ? std::round(v / step) * step : v; }

// ── UE5 editor chrome ────────────────────────────────────────────────────────────────────────
// Laid out like UE5's default editor: main menu + main toolbar across the top, Place Actors docked left,
// Outliner over Details docked right, the Content Browser under the viewport, a status bar along the
// bottom, and the viewport's own transform toolbar in its top-right corner. Panels are fixed "docked"
// windows (the game's frame shows through the middle as the viewport), each headed by a tab, the way
// UE5 panels are. Icons are drawn with the draw list, so there is no texture to load or ship.

bool g_showPlace = true, g_showOutliner = true, g_showDetails = true, g_showContent = true;
char g_detailsFilter[64] = {};
char g_contentFilter[96] = {};
std::string g_cbFolder = "/Game/A2/Prefabs";   // current Content Browser folder
std::string g_placeCat;                          // Place Actors category ("" = Recently Placed)
std::vector<std::string> g_recent;               // class paths, most recent first
std::string g_dragName;

const ImU32 kSelBlue   = IM_COL32(0, 112, 224, 255);   // #0070E0
const ImU32 kFolderCol = IM_COL32(196, 164, 110, 255);
const ImU32 kFolderTab = IM_COL32(168, 138, 86, 255);
const ImU32 kBpBar     = IM_COL32(40, 140, 230, 255);   // Blueprint Class asset colour
const ImU32 kFg        = IM_COL32(192, 192, 192, 255);
const ImU32 kDim       = IM_COL32(128, 128, 128, 255);

// ---- icons ---------------------------------------------------------------------------------
void IconFolder(ImDrawList* dl, ImVec2 p, float s)
{
    dl->AddRectFilled(ImVec2(p.x, p.y + s * 0.16f), ImVec2(p.x + s * 0.46f, p.y + s * 0.34f), kFolderTab, s * 0.06f);
    dl->AddRectFilled(ImVec2(p.x, p.y + s * 0.26f), ImVec2(p.x + s, p.y + s * 0.88f), kFolderCol, s * 0.08f);
}
void IconCube(ImDrawList* dl, ImVec2 p, float s, ImU32 col)      // static-mesh prefab
{
    const float a = s * 0.18f;
    const ImVec2 f0(p.x + s * 0.12f, p.y + a + s * 0.12f), f1(p.x + s * 0.12f + s * 0.58f, p.y + s * 0.88f);
    dl->AddRect(f0, f1, col, 0, 0, 1.5f);
    dl->AddLine(f0, ImVec2(f0.x + a, f0.y - a), col, 1.5f);
    dl->AddLine(ImVec2(f1.x, f0.y), ImVec2(f1.x + a, f0.y - a), col, 1.5f);
    dl->AddLine(ImVec2(f1.x, f1.y), ImVec2(f1.x + a, f1.y - a), col, 1.5f);
    dl->AddLine(ImVec2(f0.x + a, f0.y - a), ImVec2(f1.x + a, f0.y - a), col, 1.5f);
    dl->AddLine(ImVec2(f1.x + a, f0.y - a), ImVec2(f1.x + a, f1.y - a), col, 1.5f);
}
void IconBlueprint(ImDrawList* dl, ImVec2 p, float s)               // blueprint prefab
{
    dl->AddRectFilled(ImVec2(p.x + s * 0.08f, p.y + s * 0.12f), ImVec2(p.x + s * 0.92f, p.y + s * 0.88f), IM_COL32(28, 72, 130, 255), s * 0.1f);
    const ImU32 n = IM_COL32(150, 200, 255, 255);
    dl->AddRectFilled(ImVec2(p.x + s * 0.2f, p.y + s * 0.3f), ImVec2(p.x + s * 0.42f, p.y + s * 0.46f), n, 2);
    dl->AddRectFilled(ImVec2(p.x + s * 0.58f, p.y + s * 0.54f), ImVec2(p.x + s * 0.8f, p.y + s * 0.7f), n, 2);
    dl->AddBezierCubic(ImVec2(p.x + s * 0.42f, p.y + s * 0.38f), ImVec2(p.x + s * 0.55f, p.y + s * 0.38f),
                       ImVec2(p.x + s * 0.46f, p.y + s * 0.62f), ImVec2(p.x + s * 0.58f, p.y + s * 0.62f), n, 1.5f);
}
void IconLevel(ImDrawList* dl, ImVec2 p, float s)
{
    dl->AddCircle(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.38f, kFg, 16, 1.4f);
    dl->AddLine(ImVec2(p.x + s * 0.12f, p.y + s * 0.5f), ImVec2(p.x + s * 0.88f, p.y + s * 0.5f), kFg, 1.2f);
    dl->AddEllipse(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), ImVec2(s * 0.16f, s * 0.38f), kFg, 0, 16, 1.2f);
}
void IconFor(ImDrawList* dl, ImVec2 p, float s, const std::string& cls)
{
    if (cls.rfind("LE_SM_", 0) == 0) IconCube(dl, p, s, IM_COL32(170, 190, 210, 255));
    else IconBlueprint(dl, p, s);
}
// Reserve an icon-sized slot in the layout and draw into it.
void InlineIcon(float s, void (*draw)(ImDrawList*, ImVec2, float))
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(s, s));
    draw(ImGui::GetWindowDrawList(), p, s);
}

std::string FolderOf(const std::string& classPath)
{
    const size_t slash = classPath.find_last_of('/');
    return slash == std::string::npos ? "/Game" : classPath.substr(0, slash);
}
std::string Leaf(const std::string& folder)
{
    const size_t slash = folder.find_last_of('/');
    return slash == std::string::npos ? folder : folder.substr(slash + 1);
}
std::string PrettyName(const std::string& cls)            // LE_BP_RedCoin_C -> RedCoin
{
    std::string n = cls;
    if (n.rfind("LE_BP_", 0) == 0 || n.rfind("LE_SM_", 0) == 0) n = n.substr(6);
    else if (n.rfind("LE_", 0) == 0) n = n.substr(3);
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "_C") == 0) n.resize(n.size() - 2);
    return n;
}
bool ContainsCi(const std::string& hay, const char* needle)
{
    if (!needle || !needle[0]) return true;
    std::string a = hay, b = needle;
    for (auto& ch : a) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    for (auto& ch : b) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return a.find(b) != std::string::npos;
}

// ---- spawning --------------------------------------------------------------------------------
void SpawnAt(const PaletteItem& it, const Vec3& loc, double yaw)
{
    Command c{ CmdType::SpawnItem };
    c.str = it.path;
    c.loc = loc;
    if (g_snapEnabled)
        c.loc = { Snap(static_cast<float>(loc.x), g_gridSnap, true), Snap(static_cast<float>(loc.y), g_gridSnap, true),
                  Snap(static_cast<float>(loc.z), g_gridSnap, true) };
    c.rot = { 0.0, yaw, 0.0 };
    State().Push(c);
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), it.path), g_recent.end());
    g_recent.insert(g_recent.begin(), it.path);
    if (g_recent.size() > 10) g_recent.resize(10);
}
// Place along a ray, ON the first surface it hits (the game thread line-traces the level), falling back to
// `fallback` units down the ray over empty space. Grid snap applies in X/Y so it stays on the surface.
void SpawnTraced(const PaletteItem& it, const Vec3& from, const Vec3& dir, double fallback, double yaw)
{
    Command c{ CmdType::SpawnTraced };
    c.str = it.path;
    c.loc = from;
    c.dir = dir;
    c.fallback = fallback;
    c.rot = { 0.0, yaw, 0.0 };
    c.snap = g_snapEnabled ? g_gridSnap : 0.0f;
    State().Push(c);
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), it.path), g_recent.end());
    g_recent.insert(g_recent.begin(), it.path);
    if (g_recent.size() > 10) g_recent.resize(10);
}
// Double-click: onto whatever you are looking at (Unreal places it on the surface under screen centre).
void SpawnInFront(const Snapshot& snap, const PaletteItem& it)
{
    Vec3 fwd, rgt, up;
    RotAxes(snap.cameraRot, fwd, rgt, up);
    SpawnTraced(it, snap.cameraPos, fwd, 400.0, snap.cameraRot.yaw + 180.0);   // facing you
}
// Drag-drop: onto the surface under the cursor, like dropping an asset into Unreal's viewport.
void SpawnUnderMouse(const Snapshot& snap, const PaletteItem& it, ImVec2 mouse)
{
    const View v = MakeView(snap);
    if (!v.valid) { SpawnInFront(snap, it); return; }
    Vec3 dir = Add(v.fwd, Add(Mul(v.right, (mouse.x - v.centre.x) / v.focal), Mul(v.up, -(mouse.y - v.centre.y) / v.focal)));
    const double len = std::sqrt(Dot(dir, dir));
    dir = Mul(dir, 1.0 / len);
    SpawnTraced(it, snap.cameraPos, dir, 600.0, snap.cameraRot.yaw + 180.0);
}
const PaletteItem* FindItem(const Snapshot& snap, const std::string& byPathOrName)
{
    for (const auto& it : snap.palette)
        if (it.path == byPathOrName || it.name == byPathOrName) return &it;
    return nullptr;
}

// A docked panel: a fixed window whose top is a UE-style tab strip.
bool BeginPanel(const char* id, ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
    const bool open = ImGui::Begin(id, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    return open;
}

// ---- main menu -------------------------------------------------------------------------------
void DrawMainMenu(const Snapshot& snap, const SceneObject* sel)
{
    if (!ImGui::BeginMainMenuBar()) return;
    // The Unreal "U" badge position: a small rounded logo mark, then the menus.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(18, 18));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(p.x + 9, p.y + 9), 8.0f, IM_COL32(230, 230, 230, 255));
        dl->AddText(ImVec2(p.x + 5, p.y + 2), IM_COL32(20, 20, 20, 255), "S");
    }
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem(snap.inEditor ? "Exit Level Editor" : "Enter Level Editor"))
            State().Push({ snap.inEditor ? CmdType::ExitEditor : CmdType::EnterEditor });
        if (ImGui::MenuItem("Refresh Content")) State().Push({ CmdType::RefreshPalette });
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, sel != nullptr) && sel)
            if (const PaletteItem* it = FindItem(snap, sel->className))
                SpawnAt(*it, Add(sel->location, Vec3{ 100, 0, 0 }), sel->rotation.yaw);
        if (ImGui::MenuItem("Delete", "Delete", false, sel != nullptr))
        {
            Command c{ CmdType::DeleteObject }; c.str = g_selected; State().Push(c); g_selected.clear();
        }
        ImGui::Separator();
        ImGui::MenuItem("Snapping", nullptr, &g_snapEnabled);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window"))
    {
        ImGui::MenuItem("Place Actors", nullptr, &g_showPlace);
        ImGui::MenuItem("Outliner", nullptr, &g_showOutliner);
        ImGui::MenuItem("Details", nullptr, &g_showDetails);
        ImGui::MenuItem("Content Browser", nullptr, &g_showContent);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Select"))
    {
        if (ImGui::MenuItem("Select None", "Esc", false, !g_selected.empty()))
        {
            Command c{ CmdType::DeselectObject }; c.str = g_selected; State().Push(c); g_selected.clear();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        ImGui::TextDisabled("Camera (while editing)");
        ImGui::BulletText("Hold right mouse: look");
        ImGui::BulletText("RMB + W A S D: fly,  Q / E: down / up");
        ImGui::BulletText("RMB + scroll: fly speed,  Shift: faster");
        ImGui::BulletText("F: frame the selection");
        ImGui::Separator();
        ImGui::TextDisabled("Editing");
        ImGui::BulletText("Q W E R: select / move / rotate / scale");
        ImGui::BulletText("Click a marker to select, drag a gizmo handle");
        ImGui::BulletText("Drag an asset into the viewport: it lands on the surface");
        ImGui::BulletText("Ctrl+D duplicate, Delete removes, Esc deselects");
        ImGui::BulletText("INSERT hides the editor and returns your view");
        ImGui::EndMenu();
    }
    // Level name on the right, as UE5 shows the open map there.
    const char* lvl = snap.inEditor ? "Station  (editing)" : "Station";
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(lvl).x - 16);
    ImGui::TextDisabled("%s", lvl);
    ImGui::EndMainMenuBar();
}

// ---- main toolbar ----------------------------------------------------------------------------
void DrawMainToolbar(const Snapshot& snap, ImVec2 pos, float w, float h)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::Begin("##maintoolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Selection mode dropdown.
    ImGui::SetNextItemWidth(120);
    const char* modes[] = { "Select", "Move", "Rotate", "Scale" };
    int m = static_cast<int>(g_gizmo);
    if (ImGui::Combo("##mode", &m, modes, 4)) g_gizmo = static_cast<GizmoMode>(m);
    ImGui::SameLine();

    // "+ Add": UE5's quick-add menu, one submenu per category.
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(56, 56, 56, 255));
    if (ImGui::Button(" + Add ")) ImGui::OpenPopup("quickadd");
    ImGui::PopStyleColor();
    if (ImGui::BeginPopup("quickadd"))
    {
        std::string cat;
        for (size_t i = 0; i < snap.palette.size();)
        {
            cat = snap.palette[i].category;
            size_t end = i;
            while (end < snap.palette.size() && snap.palette[end].category == cat) ++end;
            if (ImGui::BeginMenu(cat.c_str()))
            {
                for (size_t k = i; k < end; ++k)
                    if (ImGui::MenuItem(PrettyName(snap.palette[k].name).c_str())) SpawnInFront(snap, snap.palette[k]);
                ImGui::EndMenu();
            }
            i = end;
        }
        ImGui::EndPopup();
    }

    // Enter/Exit editing, in the slot and colours of UE5's Play controls.
    ImGui::SameLine(w * 0.5f - 70);
    const bool editing = snap.inEditor;
    ImGui::PushStyleColor(ImGuiCol_Button, editing ? IM_COL32(150, 40, 40, 255) : IM_COL32(40, 120, 40, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, editing ? IM_COL32(185, 55, 55, 255) : IM_COL32(55, 150, 55, 255));
    if (ImGui::Button(editing ? "  Stop Editing  " : "  Start Editing  "))
        State().Push({ editing ? CmdType::ExitEditor : CmdType::EnterEditor });
    ImGui::PopStyleColor(2);

    ImGui::SameLine(w - 260);
    ImGui::TextDisabled("%d placed  |  %s", static_cast<int>(snap.objects.size()), snap.status.c_str());
    ImGui::End();
}

// ---- Place Actors ----------------------------------------------------------------------------
void DrawPlaceActors(const Snapshot& snap, ImVec2 pos, ImVec2 size)
{
    if (!BeginPanel("##place", pos, size)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("##placetabs")) { if (ImGui::BeginTabItem("Place Actors")) ImGui::EndTabItem(); ImGui::EndTabBar(); }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##pf", "Search Classes", g_paletteFilter, sizeof(g_paletteFilter));

    // Left: categories. Right: items. The UE5 Place Actors split.
    ImGui::BeginChild("##placecats", ImVec2(122, 0), ImGuiChildFlags_None);
    ImGui::PushStyleColor(ImGuiCol_Header, kSelBlue);
    if (ImGui::Selectable("Recent", g_placeCat.empty())) g_placeCat.clear();
    std::string last;
    for (const auto& it : snap.palette)
    {
        if (it.category == last) continue;
        last = it.category;
        // Nested categories ("Meshes / Golf") show their last segment indented under the parent, so the
        // two "Golf" groups read as Golf and Meshes > Golf instead of the same name twice.
        std::string label = it.category;
        int depth = 0;
        for (size_t at = label.find(" / "); at != std::string::npos; at = label.find(" / ", at + 3)) ++depth;
        const size_t sl = label.find_last_of('/');
        if (sl != std::string::npos) label = label.substr(sl + 2);
        label = std::string(static_cast<size_t>(depth) * 2, ' ') + label;
        if (ImGui::Selectable((label + "##" + it.category).c_str(), g_placeCat == it.category)) g_placeCat = it.category;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.category.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##placeitems", ImVec2(0, 0), ImGuiChildFlags_None);
    auto row = [&](const PaletteItem& it) {
        ImGui::PushID(it.path.c_str());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::Selectable("##it", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 26));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nDouble-click to place in front of you\nor drag into the viewport", it.path.c_str());
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) { g_dragPath = it.path; g_dragName = it.name; }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        IconFor(dl, ImVec2(p.x + 2, p.y + 2), 22, it.name);
        dl->AddText(ImVec2(p.x + 30, p.y + 5), kFg, PrettyName(it.name).c_str());
        if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) SpawnInFront(snap, it);
        ImGui::PopID();
    };
    if (g_paletteFilter[0])
    {
        for (const auto& it : snap.palette) if (ContainsCi(it.name, g_paletteFilter)) row(it);
    }
    else if (g_placeCat.empty())
    {
        if (g_recent.empty()) ImGui::TextDisabled("Nothing placed yet.\nPick a category, or use\nthe Content Browser.");
        for (const auto& path : g_recent) if (const PaletteItem* it = FindItem(snap, path)) row(*it);
    }
    else
    {
        for (const auto& it : snap.palette) if (it.category == g_placeCat) row(it);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- Outliner --------------------------------------------------------------------------------
void SelectHandle(const std::string& h)
{
    if (h == g_selected) return;
    if (!g_selected.empty()) { Command d{ CmdType::DeselectObject }; d.str = g_selected; State().Push(d); }
    g_selected = h;
    Command c{ CmdType::SelectObject }; c.str = h; State().Push(c);
}

void DrawOutlinerPanel(const Snapshot& snap, ImVec2 pos, ImVec2 size)
{
    if (!BeginPanel("##outliner", pos, size)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("##outtabs")) { if (ImGui::BeginTabItem("Outliner")) ImGui::EndTabItem(); ImGui::EndTabBar(); }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##of", "Search...", g_outlinerFilter, sizeof(g_outlinerFilter));

    if (ImGui::BeginTable("##ol", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_Resizable, ImVec2(0, -20)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item Label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        // Level root row.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        InlineIcon(16, IconLevel);
        ImGui::SameLine();
        ImGui::TextUnformatted("Station (Editor)");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("World");

        ImGui::PushStyleColor(ImGuiCol_Header, kSelBlue);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(60, 60, 60, 255));
        for (const auto& o : snap.objects)
        {
            const std::string label = PrettyName(o.className);
            if (!ContainsCi(label, g_outlinerFilter) && !ContainsCi(o.className, g_outlinerFilter)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(o.handle.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Indent(18);
            if (ImGui::Selectable("##row", o.handle == g_selected, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 18)) && !o.lockedByOther)
                SelectHandle(o.handle);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            IconFor(dl, ImVec2(p.x + 18, p.y + 1), 16, o.className);
            dl->AddText(ImVec2(p.x + 40, p.y + 2), o.lockedByOther ? IM_COL32(230, 150, 60, 255) : kFg, label.c_str());
            ImGui::Unindent(18);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n(%.0f, %.0f, %.0f)%s", o.className.c_str(), o.location.x, o.location.y, o.location.z,
                                  o.lockedByOther ? "\nLocked by another editor" : "");
            ImGui::TableNextColumn();
            ImGui::TextDisabled(o.className.rfind("LE_SM_", 0) == 0 ? "Static Mesh" : "Blueprint");
            ImGui::PopID();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%d actors", static_cast<int>(snap.objects.size()));
    ImGui::End();
}

// ---- Details ---------------------------------------------------------------------------------
// One UE5-style vector row: three value fields, each with the axis colour down its left edge.
bool AxisRow(const char* label, float v[3], float speed, float mn = 0, float mx = 0)
{
    static const ImU32 axis[3] = { IM_COL32(200, 50, 50, 255), IM_COL32(100, 180, 40, 255), IM_COL32(40, 110, 220, 255) };
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    const float w = (ImGui::GetContentRegionAvail().x - 8) / 3.0f;
    for (int i = 0; i < 3; ++i)
    {
        if (i) ImGui::SameLine(0, 4);
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(w);
        changed |= ImGui::DragFloat("##v", &v[i], speed, mn, mx, "%.2f");
        const float h = ImGui::GetItemRectSize().y;
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 3, p.y + h), axis[i], 2.0f);
        ImGui::PopID();
    }
    return changed;
}

// ---- Details > Properties ----------------------------------------------------------------------
// Every editable property of the selected actor (and of what it owns), grouped by the class that declares
// it, like Unreal's Details panel. Edits commit when you press Enter or click away -- not per keystroke --
// then apply locally at once and on the server for real. Object references to the actor's own components
// or child actors get a ">" button to step inside them, which is how you reach e.g. a red-coin quest
// prefab's inner quest actor and its Duration.
void PushSetProperty(const std::string& handle, const std::string& path, const std::string& value)
{
    Command c{ CmdType::SetProperty };
    c.str = handle; c.str2 = path; c.str3 = value;
    State().Push(c);
}

void DrawProperties(const Snapshot& snap, const SceneObject* sel)
{
    if (snap.inspectHandle != sel->handle) { ImGui::TextDisabled("Reading properties..."); return; }

    // Where we are: the class, and a way back out of a sub-object.
    ImGui::TextDisabled("%s", snap.inspectClass.c_str());
    if (!snap.inspectPath.empty())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("< Back"))
        {
            const size_t d = snap.inspectPath.find_last_of('.');
            Command c{ CmdType::Inspect };
            c.str = sel->handle;
            c.str2 = d == std::string::npos ? std::string() : snap.inspectPath.substr(0, d);
            State().Push(c);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("inside %s", snap.inspectPath.c_str());
    }
    if (snap.props.empty()) { ImGui::TextDisabled("No editable properties on this object."); return; }

    const float labelW = ImGui::GetContentRegionAvail().x * 0.45f;
    std::string lastOwner;
    for (const auto& pi : snap.props)
    {
        if (g_detailsFilter[0] && !ContainsCi(pi.name, g_detailsFilter)) continue;
        if (pi.owner != lastOwner) { lastOwner = pi.owner; ImGui::SeparatorText(pi.owner.c_str()); }
        ImGui::PushID(pi.path.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(pi.name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (%s)\npath: %s", pi.name.c_str(), pi.owner.c_str(), pi.path.c_str());
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-1);

        switch (pi.type)
        {
        case PT_Bool:
        {
            bool b = pi.value == "1";
            if (ImGui::Checkbox("##v", &b)) PushSetProperty(sel->handle, pi.path, b ? "1" : "0");
            break;
        }
        case PT_Float: case PT_Double:
        {
            double d = atof(pi.value.c_str());
            ImGui::InputDouble("##v", &d, 0.0, 0.0, "%.4g");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                char b[64]; snprintf(b, sizeof(b), "%.9g", d);
                PushSetProperty(sel->handle, pi.path, b);
            }
            break;
        }
        case PT_Int: case PT_Int64: case PT_Byte:
        {
            long long v = _atoi64(pi.value.c_str());
            ImGui::InputScalar("##v", ImGuiDataType_S64, &v);
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                if (pi.type == PT_Byte) v = (std::max)(0LL, (std::min)(255LL, v));
                PushSetProperty(sel->handle, pi.path, std::to_string(v));
            }
            break;
        }
        case PT_Enum:
        {
            const long long cur = _atoi64(pi.value.c_str());
            const char* curName = "?";
            for (const auto& e : pi.enumNames) if (e.second == cur) curName = e.first.c_str();
            if (ImGui::BeginCombo("##v", curName))
            {
                for (const auto& e : pi.enumNames)
                    if (ImGui::Selectable(e.first.c_str(), e.second == cur))
                        PushSetProperty(sel->handle, pi.path, std::to_string(e.second));
                ImGui::EndCombo();
            }
            break;
        }
        case PT_Name:
        {
            char buf[128];
            strncpy_s(buf, pi.value.c_str(), _TRUNCATE);
            ImGui::InputText("##v", buf, sizeof(buf));
            if (ImGui::IsItemDeactivatedAfterEdit()) PushSetProperty(sel->handle, pi.path, buf);
            break;
        }
        case PT_Vector: case PT_Rotator: case PT_Vector2D: case PT_Color:
        {
            const int n = pi.type == PT_Vector2D ? 2 : pi.type == PT_Color ? 4 : 3;
            double v[4]{};
            {
                const char* c = pi.value.c_str();
                for (int i = 0; i < n && *c; ++i) { char* e = nullptr; v[i] = strtod(c, &e); c = e; while (*c == ',' || *c == ' ') ++c; }
            }
            ImGui::InputScalarN("##v", ImGuiDataType_Double, v, n, nullptr, nullptr, "%.3g");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                std::string out;
                for (int i = 0; i < n; ++i) { char b[40]; snprintf(b, sizeof(b), "%s%.9g", i ? "," : "", v[i]); out += b; }
                PushSetProperty(sel->handle, pi.path, out);
            }
            break;
        }
        case PT_Object:
            if (pi.link)
            {
                if (ImGui::Button((pi.value + "  >").c_str(), ImVec2(-1, 0)))
                {
                    Command c{ CmdType::Inspect }; c.str = sel->handle; c.str2 = pi.path; State().Push(c);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step inside %s to edit its properties", pi.value.c_str());
            }
            else ImGui::TextDisabled("%s", pi.value.c_str());
            break;
        case PT_Str: case PT_Text:
        {
            char buf[256];
            strncpy_s(buf, pi.value.c_str(), _TRUNCATE);
            ImGui::InputTextWithHint("##v", "(empty)", buf, sizeof(buf));
            if (ImGui::IsItemDeactivatedAfterEdit()) PushSetProperty(sel->handle, pi.path, buf);
            break;
        }
        default:
            ImGui::TextDisabled("%s", pi.value.empty() ? "(empty)" : pi.value.c_str());
            break;
        }
        ImGui::PopID();
    }
}

void DrawDetailsPanel(const Snapshot& snap, const SceneObject* sel, ImVec2 pos, ImVec2 size)
{
    if (!BeginPanel("##details", pos, size)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("##dettabs"))
    {
        if (ImGui::BeginTabItem("Details"))
        {
            if (!sel)
            {
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::TextDisabled("Select an object to view details.");
            }
            else
            {
                // Header: icon, name, class -- as UE5's Details header.
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(28, 28));
                IconFor(ImGui::GetWindowDrawList(), p, 28, sel->className);
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextUnformatted(PrettyName(sel->className).c_str());
                ImGui::TextDisabled("%s", sel->className.c_str());
                ImGui::EndGroup();
                if (sel->lockedByOther) ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1), "Locked by another editor - read only");

                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##df", "Search Details", g_detailsFilter, sizeof(g_detailsFilter));

                if (ContainsCi("Transform Location Rotation Scale", g_detailsFilter) &&
                    ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    float loc[3] = { (float)sel->location.x, (float)sel->location.y, (float)sel->location.z };
                    float rot[3] = { (float)sel->rotation.roll, (float)sel->rotation.pitch, (float)sel->rotation.yaw };
                    float scl[3] = { (float)sel->scale.x, (float)sel->scale.y, (float)sel->scale.z };
                    bool changed = false;
                    if (ImGui::BeginTable("##xf", 2, ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
                        changed |= AxisRow("Location", loc, 1.0f);
                        changed |= AxisRow("Rotation", rot, 0.5f);       // UE shows Roll, Pitch, Yaw
                        changed |= AxisRow("Scale", scl, 0.01f, 0.01f, 100.0f);
                        ImGui::EndTable();
                    }
                    if (changed && !sel->lockedByOther)
                    {
                        Command c{ CmdType::SetTransform };
                        c.str = sel->handle;
                        c.loc = { Snap(loc[0], g_gridSnap, g_snapEnabled), Snap(loc[1], g_gridSnap, g_snapEnabled),
                                  Snap(loc[2], g_gridSnap, g_snapEnabled) };
                        c.rot = { Snap(rot[1], g_rotSnap, g_snapEnabled), Snap(rot[2], g_rotSnap, g_snapEnabled),
                                  Snap(rot[0], g_rotSnap, g_snapEnabled) };
                        c.scale = { scl[0], scl[1], scl[2] };
                        State().Push(c);
                    }
                }
                if (ContainsCi("Snapping Grid", g_detailsFilter) && ImGui::CollapsingHeader("Snapping"))
                {
                    ImGui::Checkbox("Enabled", &g_snapEnabled);
                    ImGui::SetNextItemWidth(110); ImGui::DragFloat("Grid", &g_gridSnap, 1.0f, 1.0f, 1000.0f);
                    ImGui::SetNextItemWidth(110); ImGui::DragFloat("Rotation", &g_rotSnap, 1.0f, 1.0f, 90.0f);
                }
                if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen))
                    DrawProperties(snap, sel);
                if (ContainsCi("Quest Step", g_detailsFilter) && ImGui::CollapsingHeader("Quest", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (ImGui::Button("Add as Quest Step", ImVec2(-1, 0)))
                    {
                        QuestStep s; s.objectHandle = sel->handle; s.label = PrettyName(sel->className);
                        g_quest.steps.push_back(s);
                    }
                    ImGui::TextDisabled("%d step(s) in '%s'", static_cast<int>(g_quest.steps.size()), g_quest.name);
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Quest Editor"))
        {
            ImGui::TextWrapped("Author a parkour quest from placed objects. Publishing sends the definition to "
                               "the server, so every client that joins sees it and can play it.");
            ImGui::Separator();
            ImGui::InputText("Quest Id",  g_quest.name,  sizeof(g_quest.name));
            ImGui::InputText("Title",     g_quest.title, sizeof(g_quest.title));
            ImGui::InputText("Glyph Id",  g_quest.glyph, sizeof(g_quest.glyph));
            const char* reps[] = { "Once", "Daily", "Weekly", "Monthly" };
            ImGui::Combo("Repetition", &g_quest.repetition, reps, IM_ARRAYSIZE(reps));
            ImGui::InputInt("Valid for (s)", &g_quest.validSec, 60, 600);
            if (g_quest.validSec < 0) g_quest.validSec = 0;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ValidLengthSeconds on the quest definition: how long an activation stays valid.\n"
                                  "0 = open-ended. The red-coin RUN timer is separate: it is the Duration\n"
                                  "property on the placed red-coin quest (Details > Properties).");
            ImGui::InputFloat("Required progress", &g_quest.reqProgress, 1.0f, 5.0f, "%.0f");
            if (g_quest.reqProgress < 0) g_quest.reqProgress = 0;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("OptionalRequiredProgress: progress needed to complete. 0 = the template's default.");
            ImGui::InputTextMultiline("Description", g_quest.desc, sizeof(g_quest.desc), ImVec2(-1, 48));
            if (ImGui::CollapsingHeader("Steps", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (g_quest.steps.empty()) ImGui::TextDisabled("Select an object, then Details > Quest > Add as Quest Step.");
                int remove = -1;
                for (int i = 0; i < (int)g_quest.steps.size(); ++i)
                {
                    ImGui::PushID(i);
                    ImGui::Text("%d.", i + 1);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(90);
                    const char* kinds[] = { "Reach", "Collect", "Interact" };
                    ImGui::Combo("##kind", &g_quest.steps[i].kind, kinds, IM_ARRAYSIZE(kinds));
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", g_quest.steps[i].label.c_str());
                    ImGui::SameLine(ImGui::GetWindowWidth() - 62);
                    if (ImGui::SmallButton("Remove")) remove = i;
                    ImGui::PopID();
                }
                if (remove >= 0) g_quest.steps.erase(g_quest.steps.begin() + remove);
            }
            ImGui::BeginDisabled(g_quest.steps.empty());
            ImGui::PushStyleColor(ImGuiCol_Button, kSelBlue);
            if (ImGui::Button("Publish to Server", ImVec2(-1, 28)))
            {
                for (const auto& s : g_quest.steps)
                {
                    Command c{ CmdType::QuestAddStep }; c.str = s.objectHandle; c.str2 = std::to_string(s.kind);
                    State().Push(c);
                }
                Command c{ CmdType::QuestCompile }; c.str = g_quest.name; c.str2 = g_quest.title;
                c.str3 = g_quest.glyph; c.num = g_quest.repetition;
                c.num2 = g_quest.validSec; c.f1 = g_quest.reqProgress; c.str4 = g_quest.desc;
                State().Push(c);
            }
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Content Browser -------------------------------------------------------------------------
// Folder tree on the left, built from the class paths; asset tiles for the open folder on the right.
void FolderTree(const std::vector<std::string>& folders, const std::string& root, int depth)
{
    // children of root: folders whose parent is exactly root
    for (const auto& f : folders)
    {
        if (f.size() <= root.size() || f.compare(0, root.size(), root) != 0 || f[root.size()] != '/') continue;
        if (f.find('/', root.size() + 1) != std::string::npos) continue;   // not a direct child
        bool hasKids = false;
        for (const auto& g : folders)
            if (g.size() > f.size() && g.compare(0, f.size(), f) == 0 && g[f.size()] == '/') { hasKids = true; break; }

        ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
        if (!hasKids) fl |= ImGuiTreeNodeFlags_Leaf;
        if (f == g_cbFolder) fl |= ImGuiTreeNodeFlags_Selected;
        if (g_cbFolder.compare(0, f.size(), f) == 0) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool open = ImGui::TreeNodeEx(f.c_str(), fl, "      %s", Leaf(f).c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_cbFolder = f;
        IconFolder(ImGui::GetWindowDrawList(), ImVec2(p.x + ImGui::GetTreeNodeToLabelSpacing() - 2, p.y + 1), 15);
        if (open) { FolderTree(folders, f, depth + 1); ImGui::TreePop(); }
    }
}

void DrawContentBrowser(const Snapshot& snap, ImVec2 pos, ImVec2 size)
{
    if (!BeginPanel("##content", pos, size)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("##cbtabs")) { if (ImGui::BeginTabItem("Content Browser")) ImGui::EndTabItem(); ImGui::EndTabBar(); }

    // Every folder that holds an LE class, plus each of its ancestors up to /Game.
    std::vector<std::string> folders;
    for (const auto& it : snap.palette)
        for (std::string f = FolderOf(it.path); f.size() >= 5; f = FolderOf(f))
        {
            if (std::find(folders.begin(), folders.end(), f) == folders.end()) folders.push_back(f);
            if (f == "/Game") break;
        }
    std::sort(folders.begin(), folders.end());

    // Top bar: breadcrumb path + search, as UE5's.
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    std::string crumb;
    const std::string shown = "/All" + g_cbFolder;               // "/All/Game/A2/Prefabs"
    size_t at = 1;
    bool first = true;
    while (at <= shown.size())
    {
        size_t nx = shown.find('/', at);
        if (nx == std::string::npos) nx = shown.size();
        std::string part = shown.substr(at, nx - at);
        if (part == "Game") part = "Content";
        crumb = shown.substr(0, nx);
        if (!first) { ImGui::SameLine(0, 2); ImGui::TextDisabled(">"); ImGui::SameLine(0, 2); }
        first = false;
        const std::string target = crumb.size() > 4 ? crumb.substr(4) : "/Game";
        if (ImGui::SmallButton((part + "##c" + std::to_string(at)).c_str()) && target.size() >= 5) g_cbFolder = target;
        at = nx + 1;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 260);
    ImGui::SetNextItemWidth(250);
    ImGui::InputTextWithHint("##cf", "Search Content", g_contentFilter, sizeof(g_contentFilter));

    // Left: folder tree.
    ImGui::BeginChild("##cbtree", ImVec2(220, 0), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Header, kSelBlue);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool rootOpen = ImGui::TreeNodeEx("/Game", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                            (g_cbFolder == "/Game" ? ImGuiTreeNodeFlags_Selected : 0), "      Content");
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_cbFolder = "/Game";
    IconFolder(ImGui::GetWindowDrawList(), ImVec2(p.x + ImGui::GetTreeNodeToLabelSpacing() - 2, p.y + 1), 15);
    if (rootOpen) { FolderTree(folders, "/Game", 1); ImGui::TreePop(); }
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: tiles -- sub-folders first, then assets. While searching, every folder's matches.
    ImGui::BeginChild("##cbtiles", ImVec2(0, 0), ImGuiChildFlags_None);
    const float tileW = 92, tileH = 118, pad = 8;
    const int perRow = (std::max)(1, static_cast<int>((ImGui::GetContentRegionAvail().x + pad) / (tileW + pad)));
    int col = 0, count = 0;
    auto nextTile = [&]() { if (col++ % perRow) ImGui::SameLine(0, pad); };

    if (!g_contentFilter[0])
        for (const auto& f : folders)
        {
            if (FolderOf(f) != g_cbFolder || f == g_cbFolder) continue;
            nextTile();
            ImGui::PushID(f.c_str());
            const ImVec2 q = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##f", ImVec2(tileW, tileH)) || (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)))
                g_cbFolder = f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered()) dl->AddRectFilled(q, ImVec2(q.x + tileW, q.y + tileH), IM_COL32(56, 56, 56, 255), 3);
            IconFolder(dl, ImVec2(q.x + 10, q.y + 6), 72);
            std::string n = Leaf(f);
            if (ImGui::CalcTextSize(n.c_str()).x > tileW - 6)
            {
                while (n.size() > 3 && ImGui::CalcTextSize((n + "...").c_str()).x > tileW - 6) n.pop_back();
                n += "...";
            }
            dl->AddText(ImVec2(q.x + (tileW - ImGui::CalcTextSize(n.c_str()).x) * 0.5f, q.y + 86), kFg, n.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.c_str());
            ImGui::PopID();
            ++count;
        }

    ImGui::PushStyleColor(ImGuiCol_Header, kSelBlue);
    for (const auto& it : snap.palette)
    {
        if (g_contentFilter[0] ? !ContainsCi(it.name, g_contentFilter) : FolderOf(it.path) != g_cbFolder) continue;
        nextTile();
        ImGui::PushID(it.path.c_str());
        const ImVec2 q = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##t", ImVec2(tileW, tileH));
        const bool hov = ImGui::IsItemHovered();
        if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) SpawnInFront(snap, it);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) { g_dragPath = it.path; g_dragName = it.name; }
        if (hov) ImGui::SetTooltip("%s\n%s\nDouble-click: place in front of you\nDrag: drop into the viewport", PrettyName(it.name).c_str(), it.path.c_str());

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Thumbnail well, the type colour bar under it, then the name -- UE5's asset tile.
        dl->AddRectFilled(q, ImVec2(q.x + tileW, q.y + tileH), hov ? IM_COL32(64, 64, 64, 255) : IM_COL32(44, 44, 44, 255), 3);
        dl->AddRectFilled(ImVec2(q.x + 3, q.y + 3), ImVec2(q.x + tileW - 3, q.y + 78), IM_COL32(26, 26, 26, 255), 2);
        IconFor(dl, ImVec2(q.x + 20, q.y + 10), 52, it.name);
        dl->AddRectFilled(ImVec2(q.x + 3, q.y + 78), ImVec2(q.x + tileW - 3, q.y + 81), kBpBar);
        std::string n = PrettyName(it.name);
        if (ImGui::CalcTextSize(n.c_str()).x > tileW - 6)
        {
            while (n.size() > 3 && ImGui::CalcTextSize((n + "...").c_str()).x > tileW - 6) n.pop_back();
            n += "...";
        }
        dl->AddText(ImVec2(q.x + 4, q.y + 84), kFg, n.c_str());
        dl->AddText(ImVec2(q.x + 4, q.y + 100), kDim, it.name.rfind("LE_SM_", 0) == 0 ? "Mesh Prefab" : "Blueprint");
        ImGui::PopID();
        ++count;
    }
    ImGui::PopStyleColor();
    if (count == 0) ImGui::TextDisabled(g_contentFilter[0] ? "No matching assets." : "This folder is empty.");
    ImGui::EndChild();
    ImGui::End();
}

// ---- viewport transform toolbar (top-right of the viewport) --------------------------------------
void ToolGlyph(ImDrawList* dl, ImVec2 c, GizmoMode m, ImU32 col)
{
    switch (m)
    {
    case GizmoMode::Select:
        dl->AddTriangleFilled(ImVec2(c.x - 5, c.y - 7), ImVec2(c.x - 5, c.y + 6), ImVec2(c.x + 5, c.y + 2), col);
        break;
    case GizmoMode::Translate:
        dl->AddLine(ImVec2(c.x - 7, c.y), ImVec2(c.x + 7, c.y), col, 1.6f);
        dl->AddLine(ImVec2(c.x, c.y - 7), ImVec2(c.x, c.y + 7), col, 1.6f);
        dl->AddTriangleFilled(ImVec2(c.x + 8, c.y), ImVec2(c.x + 4, c.y - 3), ImVec2(c.x + 4, c.y + 3), col);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - 8), ImVec2(c.x - 3, c.y - 4), ImVec2(c.x + 3, c.y - 4), col);
        break;
    case GizmoMode::Rotate:
        dl->PathArcTo(c, 6.5f, 0.6f, 5.6f, 16);
        dl->PathStroke(col, 0, 1.6f);
        dl->AddTriangleFilled(ImVec2(c.x + 7, c.y - 4), ImVec2(c.x + 3, c.y - 1), ImVec2(c.x + 8, c.y + 1), col);
        break;
    case GizmoMode::Scale:
        dl->AddRect(ImVec2(c.x - 7, c.y - 1), ImVec2(c.x + 1, c.y + 7), col, 0, 0, 1.6f);
        dl->AddLine(ImVec2(c.x - 1, c.y + 1), ImVec2(c.x + 7, c.y - 7), col, 1.6f);
        dl->AddRectFilled(ImVec2(c.x + 4, c.y - 8), ImVec2(c.x + 8, c.y - 4), col);
        break;
    }
}

void DrawViewportToolbar(ImVec2 vpMin, ImVec2 vpMax)
{
    const float w = 336;
    ImGui::SetNextWindowPos(ImVec2(vpMax.x - w - 8, vpMin.y + 8), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, 30), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 3));
    ImGui::Begin("##vptools", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar(2);

    const GizmoMode modes[4] = { GizmoMode::Select, GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale };
    const char* tips[4] = { "Select (Q)", "Move (W)", "Rotate (E)", "Scale (R)" };
    for (int i = 0; i < 4; ++i)
    {
        if (i) ImGui::SameLine(0, 1);
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##g", ImVec2(24, 24))) g_gizmo = modes[i];
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool on = g_gizmo == modes[i];
        if (on || ImGui::IsItemHovered())
            dl->AddRectFilled(p, ImVec2(p.x + 24, p.y + 24), on ? kSelBlue : IM_COL32(70, 70, 70, 255), 3);
        ToolGlyph(dl, ImVec2(p.x + 12, p.y + 12), modes[i], on ? IM_COL32(255, 255, 255, 255) : kFg);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tips[i]);
        ImGui::PopID();
    }
    ImGui::SameLine(0, 8);
    if (ImGui::Button(g_worldSpace ? "World" : "Local", ImVec2(48, 24))) g_worldSpace = !g_worldSpace;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Coordinate system");

    // Grid and rotation snap, UE5's toggle + value pairs.
    auto snapCombo = [](const char* id, float* v, const float* opts, int n, const char* fmt) {
        char cur[16]; snprintf(cur, sizeof(cur), fmt, *v);
        ImGui::SameLine(0, 1);
        ImGui::SetNextItemWidth(46);
        if (ImGui::BeginCombo(id, cur, ImGuiComboFlags_NoArrowButton))
        {
            for (int i = 0; i < n; ++i)
            {
                char b[16]; snprintf(b, sizeof(b), fmt, opts[i]);
                if (ImGui::Selectable(b, *v == opts[i])) *v = opts[i];
            }
            ImGui::EndCombo();
        }
    };
    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Button, g_snapEnabled ? kSelBlue : IM_COL32(56, 56, 56, 255));
    if (ImGui::Button("#", ImVec2(24, 24))) g_snapEnabled = !g_snapEnabled;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grid snapping");
    static const float grid[] = { 1, 5, 10, 50, 100, 500, 1000 };
    snapCombo("##grid", &g_gridSnap, grid, 7, "%.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grid snap (units)");
    static const float rot[] = { 5, 10, 15, 30, 45, 90 };
    snapCombo("##rot", &g_rotSnap, rot, 6, "%.0f");   // no degree sign: the default font is ASCII-only
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotation snap (degrees)");
    ImGui::End();
}

// ---- status bar --------------------------------------------------------------------------------
void DrawStatusBar(const Snapshot& snap, ImVec2 pos, float w, float h)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 2));
    ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Button, g_showContent ? IM_COL32(56, 56, 56, 255) : IM_COL32(0, 0, 0, 0));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::Button("      Content Drawer")) g_showContent = !g_showContent;
    IconFolder(ImGui::GetWindowDrawList(), ImVec2(p.x + 4, p.y + 2), 14);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("|  %s  |  %d prefabs  |  RMB: look / fly", snap.status.c_str(), static_cast<int>(snap.palette.size()));
    const char* hint = "INSERT hides the editor";
    ImGui::SameLine(w - ImGui::CalcTextSize(hint).x - 14);
    ImGui::TextDisabled("%s", hint);
    ImGui::End();
}

// Finish a Content Browser / Place Actors drag: dropping over the bare viewport places the asset there.
void HandleAssetDrag(const Snapshot& snap)
{
    if (g_dragPath.empty()) return;
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        IconFor(fg, ImVec2(m.x + 12, m.y + 8), 28, g_dragName);
        fg->AddText(ImVec2(m.x + 44, m.y + 14), IM_COL32(255, 255, 255, 230), PrettyName(g_dragName).c_str());
        return;
    }
    // Released: over any editor panel means cancel; over the viewport means place.
    const bool overPanel = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (!overPanel)
        if (const PaletteItem* it = FindItem(snap, g_dragPath)) SpawnUnderMouse(snap, *it, m);
    g_dragPath.clear();
    g_dragName.clear();
}

void HandleShortcuts()
{
    // While flying, W/E/Q/A/S/D steer the camera; they must not also switch the gizmo.
    if (ImGui::GetIO().WantTextInput || Cam().looking) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) g_gizmo = GizmoMode::Select;
    if (ImGui::IsKeyPressed(ImGuiKey_W)) g_gizmo = GizmoMode::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) g_gizmo = GizmoMode::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) g_gizmo = GizmoMode::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !g_selected.empty())
    {
        Command c{ CmdType::DeselectObject }; c.str = g_selected; State().Push(c);
        g_selected.clear();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !g_selected.empty())
    {
        Command c{ CmdType::DeleteObject }; c.str = g_selected; State().Push(c);
        g_selected.clear();
    }
}

}  // namespace

void DrawEditorUI()
{
    const Snapshot snap = State().ReadSnapshot();

    const SceneObject* sel = nullptr;
    for (const auto& o : snap.objects)
        if (o.handle == g_selected) { sel = &o; break; }

    HandleShortcuts();

    // Details follows the selection: tell the game thread what to read properties from.
    static std::string s_inspected;
    if (g_selected != s_inspected)
    {
        s_inspected = g_selected;
        Command c{ CmdType::Inspect }; c.str = g_selected; State().Push(c);
    }
    // F frames the selection with the editor camera, as in Unreal.
    if (sel && !Cam().looking && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F))
    {
        Command c{ CmdType::FocusCamera }; c.loc = sel->location; State().Push(c);
    }
    // Ctrl+D duplicates the selection, 1m along X, as in Unreal.
    if (sel && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_D))
        if (const PaletteItem* it = FindItem(snap, sel->className))
            SpawnAt(*it, Add(sel->location, Vec3{ 100, 0, 0 }), sel->rotation.yaw);

    DrawMainMenu(snap, sel);

    // UE5's default docking, fixed: toolbar across the top, Place Actors left, Outliner over Details
    // right, Content Browser under the viewport, status bar along the bottom. The game's frame shows
    // through what is left in the middle -- that is the viewport.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float x0 = vp->WorkPos.x, y0 = vp->WorkPos.y, W = vp->WorkSize.x, H = vp->WorkSize.y;
    const float tbH = 34.0f, sbH = 24.0f, gap = 2.0f;
    const float top = y0 + tbH, bottom = y0 + H - sbH;
    const float leftW  = g_showPlace ? (std::min)(300.0f, W * 0.2f) : 0.0f;
    const float rightW = (g_showOutliner || g_showDetails) ? (std::min)(380.0f, W * 0.24f) : 0.0f;
    const float cbH    = g_showContent ? (std::min)(300.0f, (bottom - top) * 0.36f) : 0.0f;

    DrawMainToolbar(snap, ImVec2(x0, y0), W, tbH);

    if (g_showPlace)
        DrawPlaceActors(snap, ImVec2(x0, top + gap), ImVec2(leftW - gap, bottom - top - gap));

    if (rightW > 0)
    {
        const float rx = x0 + W - rightW + gap, rh = bottom - top - gap;
        if (g_showOutliner && g_showDetails)
        {
            const float oh = rh * 0.42f;
            DrawOutlinerPanel(snap, ImVec2(rx, top + gap), ImVec2(rightW - gap, oh - gap));
            DrawDetailsPanel(snap, sel, ImVec2(rx, top + gap + oh), ImVec2(rightW - gap, rh - oh));
        }
        else if (g_showOutliner) DrawOutlinerPanel(snap, ImVec2(rx, top + gap), ImVec2(rightW - gap, rh));
        else                     DrawDetailsPanel(snap, sel, ImVec2(rx, top + gap), ImVec2(rightW - gap, rh));
    }

    if (g_showContent)
        DrawContentBrowser(snap, ImVec2(x0 + leftW, bottom - cbH), ImVec2(W - leftW - rightW, cbH - gap));

    const ImVec2 vpMin(x0 + leftW, top), vpMax(x0 + W - rightW, bottom - cbH);
    DrawViewportToolbar(vpMin, vpMax);
    DrawStatusBar(snap, ImVec2(x0, bottom), W, sbH);

    DrawGizmoOverlay(snap, sel);   // first: a handle click must win over a marker click
    DrawViewportMarkers(snap);
    HandleAssetDrag(snap);
}

}  // namespace se
