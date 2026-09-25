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
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <shlobj.h>
#include <shellapi.h>

namespace se {
namespace {
bool CanPlace();   // below (the open level): placing needs one

// ==== UI TOUR (tests: "uitour" op) ====
// Opens every tab and popup of the editor in turn -- with whatever is on the server -- so a crash in any UI
// path shows up in a test run instead of in a player's hands. Each step lasts ~1.2 s and logs "[uitour] <step>".
bool g_gmWindow = false;                 // the Game Modes window (toolbar / Details tab)
int g_tourStep = -1;
double g_tourAt = 0;
const char* const kTourSteps[] = { "tab:Details", "select:any", "tab:Details", "select:scripted", "tab:Details", "popup:slotcands",
                                   "tab:Quest Editor", "tab:Levels", "popup:saveas", "popup:nolevel", "popup:deletelevel",
                                   "tab:Game Modes", "gm:select", "popup:gm_new", "popup:gm_example", "popup:gm_preview",
                                   "popup:gm_delete", "window:gm", "window:gm", "popup:quickadd", "popup:problem", "tab:Details" };
const char* TourStep() { return g_tourStep >= 0 && g_tourStep < (int)(sizeof(kTourSteps) / sizeof(kTourSteps[0])) ? kTourSteps[g_tourStep] : ""; }
bool TourIs(const char* step) { return strcmp(TourStep(), step) == 0; }
std::string g_tourForce;                                // docs/tests: open this popup once (a tour step name)
std::string g_tabForce;                                 // docs/tests: select this Details tab once
float g_gmScrollReq = -1.0f;                            // docs/tests: scroll the Game Modes window's right pane
bool TourOnce(const char* step)                         // true on the first frame of that step
{
    if (!g_tourForce.empty() && g_tourForce == step) { g_tourForce.clear(); return true; }
    static int s_fired = -2;
    if (!TourIs(step) || s_fired == g_tourStep) return false;
    s_fired = g_tourStep;
    return true;
}
ImGuiTabItemFlags TourTab(const char* tab)
{
    if (!g_tabForce.empty() && g_tabForce == tab) { g_tabForce.clear(); return ImGuiTabItemFlags_SetSelected; }
    const std::string want = std::string("tab:") + tab;
    return TourIs(want.c_str()) || (strcmp(tab, "Game Modes") == 0 && TourStep()[0] == 'g') ||
           (strcmp(tab, "Game Modes") == 0 && strncmp(TourStep(), "popup:gm_", 9) == 0) ||
           (strcmp(tab, "Levels") == 0 && (TourIs("popup:deletelevel"))) ||
           (strcmp(tab, "Details") == 0 && TourIs("popup:slotcands"))
               ? ImGuiTabItemFlags_SetSelected : 0;
}

// ---- undo / redo (Ctrl+Z / Ctrl+Y): the model is defined further down, next to the shortcuts ----
struct UndoXf { std::string handle, cls, path; Vec3 loc; Rot rot; Vec3 scl{ 1, 1, 1 }; };
void UndoRecordMove(const std::vector<UndoXf>& before, const std::vector<UndoXf>& after, bool coalesce);
void UndoRecordDelete(const std::vector<std::string>& handles);
void UndoExpectNew(size_t count, const char* label);
UndoXf UndoXfOf(const std::string& handle);
struct UndoOp
{
    int kind = 0;                        // 1 move, 2 placed (place / duplicate), 3 deleted
    std::string label;
    std::vector<UndoXf> before, after;   // move: both; placed: after; deleted: before
    double at = 0.0;
    bool coalesce = false;
};
std::vector<UndoOp> g_undo, g_redo;
bool UndoApply(bool forward);

enum class GizmoMode { Select, Translate, Rotate, Scale };
GizmoMode g_gizmo = GizmoMode::Translate;
bool      g_worldSpace = false;   // Local by default (Unity): the handles follow the object's own axes
float     g_gridSnap = 10.0f;
bool      g_snapEnabled = true;
float     g_rotSnap = 15.0f;

char g_paletteFilter[96] = {};
char g_outlinerFilter[96] = {};
std::string g_selected;          // handle
std::vector<std::string> g_multiSel;   // Ctrl+click extras, besides g_selected (which keeps the gizmo and Details)
struct MultiKnown { std::string cls; Vec3 at; double lostAt = -1.0; };
std::unordered_map<std::string, MultiKnown> g_multiKnown;   // extras' class + last place (to re-find a rebuilt one)
int  g_activeTab = 0;            // 0 details, 1 quests

bool IsMultiSel(const std::string& h)
{
    return std::find(g_multiSel.begin(), g_multiSel.end(), h) != g_multiSel.end();
}
// Ctrl+click: add an object to the selection, or take it out. Extras are not locked on the server -- the
// lock is for the one being edited -- they are only duplicated or deleted together with it.
void ToggleMultiSel(const std::string& h)
{
    if (h.empty()) return;
    if (g_selected.empty())
    {
        g_selected = h;
        Command s{ CmdType::SelectObject }; s.str = h; State().Push(s);
        return;
    }
    if (h == g_selected)                  // the primary leaves: the next one takes the gizmo
    {
        Command d{ CmdType::DeselectObject }; d.str = g_selected; State().Push(d);
        g_selected.clear();
        if (!g_multiSel.empty())
        {
            g_selected = g_multiSel.front();
            g_multiSel.erase(g_multiSel.begin());
            Command s{ CmdType::SelectObject }; s.str = g_selected; State().Push(s);
        }
        return;
    }
    auto it = std::find(g_multiSel.begin(), g_multiSel.end(), h);
    if (it != g_multiSel.end()) g_multiSel.erase(it); else g_multiSel.push_back(h);
}
void ClearSelection()
{
    g_multiSel.clear();
    if (g_selected.empty()) return;
    Command d{ CmdType::DeselectObject }; d.str = g_selected; State().Push(d);
    g_selected.clear();
}
void DeleteSelected()
{
    {
        std::vector<std::string> all = g_multiSel;
        if (!g_selected.empty()) all.push_back(g_selected);
        UndoRecordDelete(all);
    }
    for (const std::string& h : g_multiSel) { Command c{ CmdType::DeleteObject }; c.str = h; State().Push(c); }
    g_multiSel.clear();
    if (g_selected.empty()) return;
    Command c{ CmdType::DeleteObject }; c.str = g_selected; State().Push(c);
    g_selected.clear();
}

// ── quest authoring model, held on the render thread and compiled through the game thread ────
struct QuestStep
{
    std::string objectHandle;
    std::string label;
};
// A quest you are writing. Players complete it by reaching each checkpoint in order; the SERVER watches
// where they are and credits the quest through the game's own completion RPC, so it works for everyone.
struct QuestDraft
{
    std::string id;                  // made from the title once, then fixed (the quest's identity)
    char        title[96] = "";
    char        desc[200] = "";
    char        glyph[48] = "PKRClimb5";
    int         repetition = 0;      // Once / Daily / Weekly / Monthly
    float       radiusM = 2.5f;      // touch distance
    bool        timed = false;
    int         timeLimit = 60;      // seconds from the first checkpoint
    std::vector<QuestStep> steps;
    // Red coin run (kind 1): the game's own TKB-style run. A start button activates the quest; the player
    // then has runSeconds to collect every coin, and the game completes the quest on their machine.
    int         kind = 0;            // 0 = checkpoint run, 1 = red coin run, 2 = quest group
    std::vector<std::string> parts;  // kind 2: ids of the quests in the group (each keeps its own icon)
    bool        hasButton = false;
    Vec3        buttonAt;
    std::vector<Vec3> coins;         // the published course (positions)
    // While you build the course each coin is a real red coin object -- the actual mesh, movable with the
    // gizmo like anything else. Publishing reads where they are, builds the run, and removes them.
    std::vector<std::string> coinObjs;
    struct PendingCoin { Vec3 at; double since; };
    std::vector<PendingCoin> pendingCoins;
    int         runSeconds = 60;
    double      coinLift = 50.0;     // a preview coin's box centre above its origin (published = centre)
    int         thrusters = 0;       // 0 = allowed (boost pads work), 1 = boosting fails the run (like TKB)
    bool        published = false;
    bool        dirty = true;        // changed since it was last published
};
std::vector<QuestDraft> g_quests;
int g_questSel = -1;
// "Place a new checkpoint here": the coin is spawned by the server, so we adopt it once it replicates back.
struct PendingCheckpoint { int quest = -1; std::string cls; std::vector<std::string> before; double at = 0; };
PendingCheckpoint g_pendingCp;

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
// A Ctrl+click selection moves as one, like Unity in Pivot mode: the gizmo's object is the pivot, and the
// others keep their offset from it -- shifted by a move, orbited and turned by a rotate, spread by a scale.
std::vector<LiveDrag::Member> g_dragGroupStart, g_dragGroup;
double g_groupT = 0.0, g_groupF = 1.0;   // this drag's rotate angle (radians) / scale factor so far
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

// Plane / centre handles: 3,4,5 = the plane whose normal is X,Y,Z; 6 = the centre (Move: the view plane,
// Scale: uniform). g_planeA/B span the plane being dragged, g_planeHit0 is where the grab ray met it.
Vec3 g_planeA, g_planeB, g_planeHit0;
bool RayPlane(const Vec3& e, const Vec3& dir, const Vec3& p0, const Vec3& n, Vec3& hit)
{
    const double den = Dot(dir, n);
    if (std::fabs(den) < 1e-4) return false;
    const double t = Dot(Sub(p0, e), n) / den;
    if (t < 0.0) return false;
    hit = Add(e, Mul(dir, t));
    return true;
}
bool PointInQuad(ImVec2 m, const ImVec2 q[4])
{
    int pos = 0, neg = 0;
    for (int k = 0; k < 4; ++k)
    {
        const ImVec2 a = q[k], b = q[(k + 1) % 4];
        const float cr = (b.x - a.x) * (m.y - a.y) - (b.y - a.y) * (m.x - a.x);
        if (cr > 0) ++pos; else if (cr < 0) ++neg;
    }
    return pos == 0 || neg == 0;
}

void WriteDrag(bool active)
{
    LiveDrag& d = Drag();
    std::lock_guard<std::mutex> lk(d.mx);
    d.active = active;
    d.handle = g_dragHandle;
    d.loc = g_pendLoc; d.rot = g_pendRot; d.scale = g_pendScale;
    d.group = g_dragGroup;
    for (const auto& m : g_dragGroup) { auto k = g_multiKnown.find(m.handle); if (k != g_multiKnown.end()) k->second.at = m.loc; }
    ++d.seq;
}

// A finished gizmo drag becomes one undo step: every object it moved, from where it started to where it is.
void UndoGizmoRelease()
{
    std::vector<UndoXf> before, after;
    UndoXf b = UndoXfOf(g_dragHandle), a = b;
    b.loc = g_dragStartLoc; b.rot = g_dragStartRot; b.scl = g_dragStartScale;
    a.loc = g_pendLoc; a.rot = g_pendRot; a.scl = g_pendScale;
    before.push_back(b); after.push_back(a);
    for (size_t i = 0; i < g_dragGroupStart.size() && i < g_dragGroup.size(); ++i)
    {
        UndoXf gb = UndoXfOf(g_dragGroupStart[i].handle), ga = gb;
        gb.loc = g_dragGroupStart[i].loc; gb.rot = g_dragGroupStart[i].rot; gb.scl = g_dragGroupStart[i].scale;
        ga.loc = g_dragGroup[i].loc; ga.rot = g_dragGroup[i].rot; ga.scl = g_dragGroup[i].scale;
        before.push_back(gb); after.push_back(ga);
    }
    UndoRecordMove(before, after, false);
}

void DrawGizmoOverlay(const Snapshot& snap, const SceneObject* sel)
{
    g_gizmoHover = false;
    if (!sel || g_gizmo == GizmoMode::Select)
    {
        if (g_dragAxis >= 0) { UndoGizmoRelease(); WriteDrag(false); g_dragAxis = -1; }
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

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
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
                // An axis pointing (nearly) at the camera shrinks to a dot on top of the centre handle; it
                // must not steal that click (it would drag along the view direction instead). Unreal does
                // the same: a foreshortened axis is not grabbable.
                const float sx = ends[i].x - c.x, sy = ends[i].y - c.y;
                if (sx * sx + sy * sy >= 15.0f * 15.0f)
                {
                    const ImVec2 from(c.x + sx * 0.15f, c.y + sy * 0.15f);
                    d = SegDist(mouse, from, ends[i]);
                }
            }
            if (d < best) { best = d; hover = i; hoverK = dk; }
        }
    }
    // Move: a small square in each plane (drag in two axes at once, as Unreal's widget); both modes: the
    // centre (Move: slide in the view plane; Scale: uniform). Axes win when both are under the mouse.
    ImVec2 quad[3][4];
    bool quadOk[3] = { false, false, false };
    if (g_gizmo == GizmoMode::Translate)
        for (int i = 0; i < 3; ++i)
        {
            const Vec3& a = ax[(i + 1) % 3];
            const Vec3& b = ax[(i + 2) % 3];
            const double s0 = 0.22 * L * worldPerPx, s1 = 0.42 * L * worldPerPx;
            const Vec3 w[4] = { Add(at, Add(Mul(a, s0), Mul(b, s0))), Add(at, Add(Mul(a, s1), Mul(b, s0))),
                                Add(at, Add(Mul(a, s1), Mul(b, s1))), Add(at, Add(Mul(a, s0), Mul(b, s1))) };
            quadOk[i] = true;
            for (int k = 0; k < 4; ++k) if (!W2S(v, w[k], quad[i][k])) quadOk[i] = false;
        }
    // The centre handle wins inside its own circle: the axis segments start a few pixels out from the centre,
    // so without this a click on the centre grabbed whichever axis passed nearest (Unreal gives the centre
    // priority the same way).
    if (g_dragAxis < 0 && !uiWantsMouse && !Cam().looking && g_gizmo != GizmoMode::Rotate &&
        (mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y) <= 81.0f)
        hover = 6;
    if (hover < 0 && g_dragAxis < 0 && !uiWantsMouse && !Cam().looking && g_gizmo != GizmoMode::Rotate)
    {
        for (int i = 0; i < 3 && hover < 0; ++i)
        {
            if (!quadOk[i]) continue;
            // A plane seen edge-on is a sliver: grabbing it would slide along an unstable ray/plane hit.
            const ImVec2* q = quad[i];
            const float area = std::fabs((q[2].x - q[0].x) * (q[3].y - q[1].y) - (q[3].x - q[1].x) * (q[2].y - q[0].y)) * 0.5f;
            if (area >= 60.0f && PointInQuad(mouse, q)) hover = 3 + i;
        }
        if (hover < 0 && (mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y) <= 81.0f) hover = 6;
    }
    g_gizmoHover = hover >= 0;

    if (hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !sel->lockedByOther)
    {
        g_dragAxis       = hover;
        g_dragStartLoc   = sel->location;
        g_dragStartRot   = sel->rotation;
        g_dragStartScale = sel->scale;
        g_dragStartMouse = mouse;
        g_dragAxisWorld  = hover < 3 ? ax[hover] : hover < 6 ? ax[hover - 3] : v.fwd;   // axis, or the plane's normal
        if (hover >= 3)
        {
            g_planeA = hover < 6 ? ax[(hover - 3 + 1) % 3] : v.right;
            g_planeB = hover < 6 ? ax[(hover - 3 + 2) % 3] : v.up;
            if (!RayPlane(v.eye, ScreenRay(v, mouse), sel->location, g_dragAxisWorld, g_planeHit0)) g_planeHit0 = sel->location;
        }
        g_dragHandle     = sel->handle;
        g_pendLoc = sel->location; g_pendRot = sel->rotation; g_pendScale = sel->scale;
        g_dragGroupStart.clear();
        for (const auto& o : snap.objects)
            if (o.handle != sel->handle && !o.lockedByOther && IsMultiSel(o.handle))
                g_dragGroupStart.push_back({ o.handle, o.location, o.scale, o.rotation });
        g_dragGroup = g_dragGroupStart;
        g_groupT = 0.0; g_groupF = 1.0;
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
        UndoGizmoRelease();
        WriteDrag(false);               // release: the game thread sends the final transform
        g_dragAxis = -1;
    }

    if (g_dragAxis >= 0 && g_dragHandle == sel->handle)
    {
        Vec3 loc = g_dragStartLoc, scale = g_dragStartScale;
        Rot  rot = g_dragStartRot;
        const double rotSign = Dot(g_dragAxisWorld, v.fwd) < 0.0 ? 1.0 : -1.0;
        if (g_gizmo == GizmoMode::Translate && g_dragAxis >= 3)
        {
            Vec3 hit;
            if (RayPlane(v.eye, ScreenRay(v, mouse), g_dragStartLoc, g_dragAxisWorld, hit))
            {
                const Vec3 d = Sub(hit, g_planeHit0);
                double da = (std::max)(-50000.0, (std::min)(50000.0, Dot(d, g_planeA)));
                double db = (std::max)(-50000.0, (std::min)(50000.0, Dot(d, g_planeB)));
                da = Snap(static_cast<float>(da), g_gridSnap, g_snapEnabled);
                db = Snap(static_cast<float>(db), g_gridSnap, g_snapEnabled);
                loc = Add(g_dragStartLoc, Add(Mul(g_planeA, da), Mul(g_planeB, db)));
            }
            else loc = g_pendLoc;                                  // plane edge-on: hold where it is
        }
        else if (g_gizmo == GizmoMode::Translate)
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
            g_groupT = t;
            rot = RotFromAxes(RotateAbout(x0, g_dragAxisWorld, t), RotateAbout(y0, g_dragAxisWorld, t),
                              RotateAbout(z0, g_dragAxisWorld, t));
            char a[32];
            snprintf(a, sizeof(a), "%+.1f deg", deg);
            dl->AddText(ImVec2(mouse.x + 16, mouse.y - 18), hot, a);
        }
        else
        {
            // An axis handle scales along its screen direction; the centre (6) scales uniformly with
            // right/up mouse travel.
            const ImVec2 dir = g_dragAxis < 3 ? ImVec2(ends[g_dragAxis].x - c.x, ends[g_dragAxis].y - c.y) : ImVec2(0.7071f, -0.7071f);
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            const double along = len > 1e-3f
                ? ((mouse.x - g_dragStartMouse.x) * dir.x + (mouse.y - g_dragStartMouse.y) * dir.y) / len : 0.0;
            const double f = (std::max)(0.05, 1.0 + along / L);
            if (g_dragAxis == 6)
            {
                scale.x = (std::max)(0.01, g_dragStartScale.x * f);
                scale.y = (std::max)(0.01, g_dragStartScale.y * f);
                scale.z = (std::max)(0.01, g_dragStartScale.z * f);
            }
            g_groupF = f;
            if (g_dragAxis == 0) scale.x = (std::max)(0.01, g_dragStartScale.x * f);
            if (g_dragAxis == 1) scale.y = (std::max)(0.01, g_dragStartScale.y * f);
            if (g_dragAxis == 2) scale.z = (std::max)(0.01, g_dragStartScale.z * f);
        }
        const bool changed = loc.x != g_pendLoc.x || loc.y != g_pendLoc.y || loc.z != g_pendLoc.z ||
                             rot.pitch != g_pendRot.pitch || rot.yaw != g_pendRot.yaw || rot.roll != g_pendRot.roll ||
                             scale.x != g_pendScale.x || scale.y != g_pendScale.y || scale.z != g_pendScale.z;
        g_pendLoc = loc; g_pendRot = rot; g_pendScale = scale;
        const Vec3 pivot = g_dragStartLoc, a = g_dragAxisWorld;
        for (size_t i = 0; i < g_dragGroupStart.size() && i < g_dragGroup.size(); ++i)
        {
            const LiveDrag::Member& m0 = g_dragGroupStart[i];
            LiveDrag::Member& m = g_dragGroup[i];
            const Vec3 d = Sub(m0.loc, pivot);
            if (g_gizmo == GizmoMode::Translate) m.loc = Add(m0.loc, Sub(loc, g_dragStartLoc));
            else if (g_gizmo == GizmoMode::Rotate)
            {
                m.loc = Add(pivot, RotateAbout(d, a, g_groupT));
                Vec3 x0, y0, z0;
                RotAxes(m0.rot, x0, y0, z0);
                m.rot = RotFromAxes(RotateAbout(x0, a, g_groupT), RotateAbout(y0, a, g_groupT), RotateAbout(z0, a, g_groupT));
            }
            else
            {
                m.loc = g_dragAxis == 6 ? Add(pivot, Mul(d, g_groupF)) : Add(Add(pivot, d), Mul(a, (g_groupF - 1.0) * Dot(d, a)));
                m.scale = m0.scale;
                if (g_dragAxis == 6) { m.scale.x = (std::max)(0.01, m0.scale.x * g_groupF); m.scale.y = (std::max)(0.01, m0.scale.y * g_groupF);
                                       m.scale.z = (std::max)(0.01, m0.scale.z * g_groupF); }
                if (g_dragAxis == 0) m.scale.x = (std::max)(0.01, m0.scale.x * g_groupF);
                if (g_dragAxis == 1) m.scale.y = (std::max)(0.01, m0.scale.y * g_groupF);
                if (g_dragAxis == 2) m.scale.z = (std::max)(0.01, m0.scale.z * g_groupF);
            }
        }
        if (changed) WriteDrag(true);
    }

    if (g_gizmo == GizmoMode::Rotate)
        for (int i = 0; i < 3; ++i)
            if (ringN[i] > 2)
            {
                const bool h = g_dragAxis == i || hover == i;
                dl->AddPolyline(ring[i], ringN[i], h ? hot : axisCol[i], ImDrawFlags_Closed, h ? 4.0f : 2.5f);
            }
    // Plane squares (Move), drawn under the axes: translucent in the colour of the axis they are normal to.
    if (g_gizmo == GizmoMode::Translate)
        for (int i = 0; i < 3; ++i)
        {
            if (!quadOk[i]) continue;
            const bool h = g_dragAxis == 3 + i || hover == 3 + i;
            const ImU32 base = axisCol[i];
            const ImU32 fill = h ? IM_COL32(255, 220, 90, 150) : ((base & 0x00FFFFFF) | (70u << 24));
            dl->AddQuadFilled(quad[i][0], quad[i][1], quad[i][2], quad[i][3], fill);
            dl->AddQuad(quad[i][0], quad[i][1], quad[i][2], quad[i][3], h ? hot : base, 1.5f);
        }
    static const char* kAxisName[3] = { "X", "Y", "Z" };
    for (int i = 0; i < 3; ++i)
    {
        if (!endOk[i] || g_gizmo == GizmoMode::Rotate) continue;
        const bool h = g_dragAxis == i || hover == i;
        const ImU32 col = h ? hot : axisCol[i];
        float dx = ends[i].x - c.x, dy = ends[i].y - c.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) continue;                           // the axis points at the camera: nothing to draw
        dx /= len; dy /= len;
        const ImVec2 shaft(ends[i].x - dx * (g_gizmo == GizmoMode::Scale ? 6.0f : 12.0f), ends[i].y - dy * (g_gizmo == GizmoMode::Scale ? 6.0f : 12.0f));
        dl->AddLine(c, shaft, col, h ? 3.5f : 2.5f);
        if (g_gizmo == GizmoMode::Scale)
            dl->AddRectFilled(ImVec2(ends[i].x - 6, ends[i].y - 6), ImVec2(ends[i].x + 6, ends[i].y + 6), col, 1.5f);
        else
        {
            // Cone head: tip past the end, base across the shaft (the flat silhouette of Unreal's cone).
            const ImVec2 tip(ends[i].x + dx * 6.0f, ends[i].y + dy * 6.0f);
            const ImVec2 b1(shaft.x - dy * 6.5f, shaft.y + dx * 6.5f), b2(shaft.x + dy * 6.5f, shaft.y - dx * 6.5f);
            dl->AddTriangleFilled(tip, b1, b2, col);
        }
        dl->AddText(ImVec2(ends[i].x + dx * 14.0f - 4.0f, ends[i].y + dy * 14.0f - 7.0f), col, kAxisName[i]);
    }
    if (g_gizmo != GizmoMode::Rotate)
    {
        const bool h = g_dragAxis == 6 || hover == 6;
        if (g_gizmo == GizmoMode::Scale)
            dl->AddRectFilled(ImVec2(c.x - 6, c.y - 6), ImVec2(c.x + 6, c.y + 6), h ? hot : IM_COL32(230, 230, 230, 255), 1.5f);
        else
        {
            dl->AddCircleFilled(c, 6.0f, h ? hot : IM_COL32(230, 230, 230, 235));
            dl->AddCircle(c, 6.0f, IM_COL32(40, 40, 40, 200), 0, 1.0f);
        }
    }
    else dl->AddCircleFilled(c, 4.0f, IM_COL32(230, 230, 230, 255));
}

// Test hook (script `gmove`): move the whole selection by dv through the gizmo's own release path.
void UiGroupMove(const Snapshot& snap, const Vec3& dv)
{
    const SceneObject* sel = nullptr;
    for (const auto& o : snap.objects) if (o.handle == g_selected) sel = &o;
    if (!sel) { Log("[ui] FAIL gmove: nothing selected"); return; }
    g_dragHandle = sel->handle;
    g_pendLoc = Add(sel->location, dv); g_pendRot = sel->rotation; g_pendScale = sel->scale;
    g_dragGroup.clear();
    for (const auto& o : snap.objects)
        if (o.handle != sel->handle && IsMultiSel(o.handle)) g_dragGroup.push_back({ o.handle, Add(o.location, dv), o.scale, o.rotation });
    g_dragStartLoc = sel->location; g_dragStartRot = sel->rotation; g_dragStartScale = sel->scale;
    g_dragGroupStart.clear();
    for (const auto& o : snap.objects)
        if (o.handle != sel->handle && IsMultiSel(o.handle)) g_dragGroupStart.push_back({ o.handle, o.location, o.scale, o.rotation });
    UndoGizmoRelease();
    WriteDrag(false);
    Log("[ui] group move: %zu object(s) by (%.0f,%.0f,%.0f)", 1 + g_dragGroup.size(), dv.x, dv.y, dv.z);
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

// Trigger / contact areas (a team changer's box, a button's sphere...): where a player actually has to be
// for the blueprint to fire. Cyan = generates overlap events (a real trigger); grey = collision-only shape.
bool g_showTriggers = true;
void DrawSeg3(ImDrawList* dl, const View& v, const Vec3& a, const Vec3& b, ImU32 col, float th)
{
    ImVec2 pa, pb;
    if (W2S(v, a, pa) && W2S(v, b, pb)) dl->AddLine(pa, pb, col, th);
}
void DrawRing3(ImDrawList* dl, const View& v, const Vec3& c, const Vec3& u, const Vec3& w, double r, ImU32 col, float th)
{
    const int kN = 32;
    Vec3 prev = Add(c, Mul(u, r));
    for (int k = 1; k <= kN; ++k)
    {
        const double t = k * 2.0 * 3.14159265358979 / kN;
        const Vec3 cur = Add(c, Add(Mul(u, std::cos(t) * r), Mul(w, std::sin(t) * r)));
        DrawSeg3(dl, v, prev, cur, col, th);
        prev = cur;
    }
}
void DrawTrigger(ImDrawList* dl, const View& v, const TriggerShape& t, bool hi)
{
    const ImU32 col = t.overlap ? IM_COL32(60, 220, 255, hi ? 235 : 120) : IM_COL32(160, 160, 180, hi ? 170 : 60);
    const float th = hi ? 2.0f : 1.2f;
    Vec3 x, y, z;
    RotAxes(t.rot, x, y, z);
    if (t.kind == 1 || t.kind == 4)
    {
        Vec3 p[8];
        for (int i = 0; i < 8; ++i)
            p[i] = Add(t.center, Add(Mul(x, (i & 1 ? 1 : -1) * t.ext.x), Add(Mul(y, (i & 2 ? 1 : -1) * t.ext.y), Mul(z, (i & 4 ? 1 : -1) * t.ext.z))));
        static const int e[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
        for (const auto& ed : e) DrawSeg3(dl, v, p[ed[0]], p[ed[1]], col, th);
    }
    else if (t.kind == 2)
    {
        DrawRing3(dl, v, t.center, x, y, t.ext.x, col, th);
        DrawRing3(dl, v, t.center, x, z, t.ext.x, col, th);
        DrawRing3(dl, v, t.center, y, z, t.ext.x, col, th);
    }
    else
    {
        const double r = t.ext.x, h = (std::max)(0.0, t.ext.z - r);
        const Vec3 top = Add(t.center, Mul(z, h)), bot = Sub(t.center, Mul(z, h));
        DrawRing3(dl, v, top, x, y, r, col, th);
        DrawRing3(dl, v, bot, x, y, r, col, th);
        DrawRing3(dl, v, t.center, x, z, 0.0, col, th);
        for (const Vec3& d : { x, Mul(x, -1.0), y, Mul(y, -1.0) }) DrawSeg3(dl, v, Add(top, Mul(d, r)), Add(bot, Mul(d, r)), col, th);
        DrawRing3(dl, v, top, x, z, r, col, th);
        DrawRing3(dl, v, bot, x, z, r, col, th);
    }
    if (hi && t.overlap)
    {
        ImVec2 c;
        if (W2S(v, t.center, c)) dl->AddText(ImVec2(c.x + 6, c.y - 6), col, "trigger area");
    }
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

bool SlotPickTake(const std::string& h);   // below (Outliner)
extern int g_coinPlaceQuest;                // construction mode (below, with the coin run editor)
const PaletteItem* FindItem(const Snapshot& snap, const std::string& byPathOrName);

void DrawViewportMarkers(const Snapshot& snap)
{
    const View v = MakeView(snap);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
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
            const bool isSel = o.handle == g_selected || IsMultiSel(o.handle);
            if (!isSel && &o != hovered) continue;
            Vec3 ctr = Add(o.location, o.boundsOff);
            if (isSel && g_dragAxis >= 0 && g_dragHandle == o.handle) ctr = Add(g_pendLoc, o.boundsOff);
            if (isSel && g_dragAxis >= 0 && g_dragHandle != o.handle)
                for (const auto& m : g_dragGroup) if (m.handle == o.handle) ctr = Add(m.loc, o.boundsOff);
            DrawBox(dl, v, ctr, PickExtent(o),
                    isSel ? IM_COL32(255, 160, 40, 230) : o.lockedByOther ? IM_COL32(220, 120, 60, 150) : IM_COL32(230, 230, 230, 110),
                    isSel ? 2.0f : 1.2f);
        }
    if (g_showTriggers && v.valid)
        for (const auto& o : snap.objects)
        {
            if (o.triggers.empty()) continue;
            const Vec3 d = Sub(o.location, v.eye);
            if (Dot(d, d) > 6000.0 * 6000.0) continue;          // nearby only: a whole station of wireframes is noise
            const bool hi = o.handle == g_selected || IsMultiSel(o.handle) || &o == hovered;
            for (const auto& t : o.triggers) DrawTrigger(dl, v, t, hi);
        }
    if (hovered && hovered->handle != g_selected)
    {
        const std::string tip = PrettyName(hovered->className) + (hovered->lockedByOther ? "  (locked)" : "");
        dl->AddText(ImVec2(mouse.x + 16, mouse.y + 8), IM_COL32(230, 230, 230, 220), tip.c_str());
    }

    if (g_coinPlaceQuest >= 0 && canPick)
    {
        bool hasHit = false;
        Vec3 hit;
        { PickState& ps = Pick(); std::lock_guard<std::mutex> lk(ps.mx); hasHit = ps.hasHit; hit = ps.hit; }
        ImVec2 sp;
        if (hasHit && W2S(v, Vec3{ hit.x, hit.y, hit.z + 50.0 }, sp))   // where the coin will be (its box rests on the surface)
        {
            dl->AddCircle(sp, 12.0f, IM_COL32(255, 70, 60, 255), 24, 2.5f);
            dl->AddCircleFilled(sp, 4.0f, IM_COL32(255, 70, 60, 255));
        }
        dl->AddText(ImVec2(mouse.x + 16, mouse.y + 8), IM_COL32(255, 200, 190, 230), "Click to place a coin   (Esc: stop)");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const PaletteItem* coin = FindItem(snap, "LE_BP_RedCoin_C");
            if (coin)
            {
                if (CanPlace()) { Command c{ CmdType::PlaceTraced }; c.str = coin->path; c.loc = v.eye; c.dir = ScreenRay(v, mouse); State().Push(c); }
            }
        }
        return;
    }
    if (!canPick || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (hovered && SlotPickTake(hovered->handle)) return;
    const bool ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;   // Unity: Ctrl or Shift toggles
    if (hovered)
    {
        if (hovered->lockedByOther) return;
        if (ctrl) { ToggleMultiSel(hovered->handle); return; }
        g_multiSel.clear();
        if (hovered->handle == g_selected) return;
        if (!g_selected.empty()) { Command d{ CmdType::DeselectObject }; d.str = g_selected; State().Push(d); }
        g_selected = hovered->handle;
        Command s{ CmdType::SelectObject }; s.str = g_selected; State().Push(s);
    }
    else if (!ctrl)
        ClearSelection();
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
std::string g_placeCat;                          // Place Actors category ("" = Recently Placed, "*fav" = Favorites)
std::vector<std::string> g_recent;               // class paths, most recent first

// ---- favorites: starred palette items, kept per PC in %LOCALAPPDATA%\RigelEditor\favorites.txt ----
std::vector<std::string> g_favorites;            // class paths, in the order they were starred
bool g_favLoaded = false;
std::wstring FavFile()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) + L"\\RigelEditor" : std::wstring(L".");
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\favorites.txt";
}
void FavLoad()
{
    g_favLoaded = true;
    g_favorites.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, FavFile().c_str(), L"rb") != 0 || !f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty() && std::find(g_favorites.begin(), g_favorites.end(), l) == g_favorites.end()) g_favorites.push_back(l);
    }
    fclose(f);
}
void FavSave()
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, FavFile().c_str(), L"wb") != 0 || !f) return;
    for (const auto& l : g_favorites) fprintf(f, "%s\n", l.c_str());
    fclose(f);
}
bool IsFav(const std::string& path)
{
    if (!g_favLoaded) FavLoad();
    return std::find(g_favorites.begin(), g_favorites.end(), path) != g_favorites.end();
}
void ToggleFav(const std::string& path)
{
    if (!g_favLoaded) FavLoad();
    auto it = std::find(g_favorites.begin(), g_favorites.end(), path);
    if (it != g_favorites.end()) g_favorites.erase(it); else g_favorites.push_back(path);
    FavSave();
}
// A five-point star (filled = favourite), for rows and tiles.
void DrawStar(ImDrawList* dl, ImVec2 c, float r, bool filled, ImU32 col)
{
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i)
    {
        const float a = -1.5707963f + i * 0.6283185f;
        const float rr = (i & 1) ? r * 0.45f : r;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    if (filled) dl->AddConcavePolyFilled(pts, 10, col);
    else dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.3f);
}
// The star button on a row/tile: click toggles. Returns true when it ate the click.
bool FavStarButton(ImDrawList* dl, ImVec2 c, float r, const std::string& path)
{
    const bool fav = IsFav(path);
    const ImVec2 m = ImGui::GetIO().MousePos;
    // AllowWhenBlockedByActiveItem: the row/tile under the star goes active on the same mouse-down, and a plain
    // IsWindowHovered() is false while any item is active -- so the star never saw its own click.
    const bool hov = std::fabs(m.x - c.x) <= r + 2 && std::fabs(m.y - c.y) <= r + 2 &&
                     ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    DrawStar(dl, c, r, fav, fav ? IM_COL32(250, 200, 60, 255) : hov ? IM_COL32(230, 230, 230, 230) : IM_COL32(150, 150, 150, 140));
    if (hov) ImGui::SetTooltip(fav ? "Remove from Favorites" : "Add to Favorites");
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { ToggleFav(path); return true; }
    return false;
}
std::string g_dragName;
int g_favClickArm = 0;                           // tests: click the star of the first Place Actors row drawn

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
// Pieces the game has no icon for (the sandbox prefabs: traps, buttons, timers, scoreboards ...) get a drawn one
// that says what they are, instead of the generic blueprint icon. Returns false when nothing specific fits.
bool IconGlyph(ImDrawList* dl, ImVec2 p, float s, const std::string& cls)
{
    auto has = [&](const char* k) { return cls.find(k) != std::string::npos; };
    const ImVec2 c(p.x + s * 0.5f, p.y + s * 0.5f);
    const float r = s * 0.38f, lw = (std::max)(1.5f, s * 0.07f);
    auto bg = [&](ImU32 col) { dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), col, s * 0.16f); };
    const ImU32 W = IM_COL32(245, 245, 245, 255), K = IM_COL32(25, 25, 25, 255);
    if (has("TeamChange"))                                          // a door arch with an arrow through it
    {
        bg(IM_COL32(40, 110, 200, 255));
        dl->AddRect(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x + r * 0.7f, c.y + r), W, r * 0.5f, 0, lw);
        dl->AddLine(ImVec2(c.x - r * 1.1f, c.y), ImVec2(c.x + r * 0.9f, c.y), W, lw);
        dl->AddTriangleFilled(ImVec2(c.x + r * 1.1f, c.y), ImVec2(c.x + r * 0.6f, c.y - r * 0.4f), ImVec2(c.x + r * 0.6f, c.y + r * 0.4f), W);
        return true;
    }
    if (has("Button"))                                              // a big round button on a base
    {
        bg(IM_COL32(60, 60, 66, 255));
        dl->AddRectFilled(ImVec2(c.x - r, c.y + r * 0.35f), ImVec2(c.x + r, c.y + r * 0.8f), IM_COL32(150, 150, 160, 255), 3);
        dl->AddCircleFilled(ImVec2(c.x, c.y), r * 0.62f, IM_COL32(220, 50, 45, 255));
        dl->AddCircle(ImVec2(c.x, c.y), r * 0.62f, W, 0, lw * 0.6f);
        return true;
    }
    if (has("Spinner") || has("Fan") || has("Rotating"))           // blades around a hub
    {
        bg(IM_COL32(200, 120, 30, 255));
        for (int i = 0; i < 3; ++i)
        {
            const float a = i * 2.0944f;
            dl->AddLine(c, ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r), W, lw * 1.4f);
            dl->AddCircleFilled(ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r), lw * 1.2f, W);
        }
        dl->AddCircleFilled(c, lw * 1.6f, K);
        return true;
    }
    if (has("Laser"))                                               // two emitters and a beam
    {
        bg(IM_COL32(120, 30, 40, 255));
        dl->AddRectFilled(ImVec2(p.x + s * 0.12f, c.y - r * 0.4f), ImVec2(p.x + s * 0.24f, c.y + r * 0.4f), W);
        dl->AddRectFilled(ImVec2(p.x + s * 0.76f, c.y - r * 0.4f), ImVec2(p.x + s * 0.88f, c.y + r * 0.4f), W);
        dl->AddLine(ImVec2(p.x + s * 0.24f, c.y), ImVec2(p.x + s * 0.76f, c.y), IM_COL32(255, 70, 70, 255), lw * 1.3f);
        return true;
    }
    if (has("Trap") || has("Flipper") || has("FallingBlock") || has("StickySlime") || has("BunnyHop"))   // warning sign
    {
        bg(IM_COL32(200, 120, 30, 255));
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y + r * 0.8f), ImVec2(c.x - r, c.y + r * 0.8f), IM_COL32(250, 210, 40, 255));
        dl->AddLine(ImVec2(c.x, c.y - r * 0.35f), ImVec2(c.x, c.y + r * 0.3f), K, lw * 1.2f);
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.55f), lw * 0.8f, K);
        return true;
    }
    if (has("ForceField") || has("Shield"))                         // a hex shield
    {
        bg(IM_COL32(20, 90, 110, 255));
        dl->AddNgon(c, r, IM_COL32(90, 230, 255, 255), 6, lw);
        dl->AddNgon(c, r * 0.5f, IM_COL32(90, 230, 255, 160), 6, lw * 0.7f);
        return true;
    }
    if (has("TimerDisplay"))                                        // a small screen reading 00:00
    {
        bg(IM_COL32(50, 50, 60, 255));
        dl->AddRectFilled(ImVec2(p.x + s * 0.1f, c.y - r * 0.55f), ImVec2(p.x + s * 0.9f, c.y + r * 0.55f), K, 3);
        dl->AddText(nullptr, s * 0.3f, ImVec2(p.x + s * 0.16f, c.y - s * 0.15f), IM_COL32(90, 255, 120, 255), "0:45");
        return true;
    }
    if (has("Timer"))                                               // a clock
    {
        bg(IM_COL32(70, 70, 140, 255));
        dl->AddCircle(c, r, W, 0, lw);
        dl->AddLine(c, ImVec2(c.x, c.y - r * 0.7f), W, lw);
        dl->AddLine(c, ImVec2(c.x + r * 0.5f, c.y), W, lw);
        return true;
    }
    if (has("TableScoreboard"))                                     // a table: rows and columns
    {
        bg(IM_COL32(40, 70, 60, 255));
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.8f), ImVec2(c.x + r, c.y + r * 0.8f), W, 2, 0, lw * 0.7f);
        for (int i = 1; i < 3; ++i) dl->AddLine(ImVec2(c.x - r, c.y - r * 0.8f + i * r * 0.53f), ImVec2(c.x + r, c.y - r * 0.8f + i * r * 0.53f), W, lw * 0.5f);
        dl->AddLine(ImVec2(c.x - r * 0.1f, c.y - r * 0.8f), ImVec2(c.x - r * 0.1f, c.y + r * 0.8f), W, lw * 0.5f);
        return true;
    }
    if (has("Score"))                                               // a scoreboard screen reading 2:1
    {
        bg(IM_COL32(40, 60, 100, 255));
        dl->AddRectFilled(ImVec2(p.x + s * 0.1f, c.y - r * 0.7f), ImVec2(p.x + s * 0.9f, c.y + r * 0.6f), K, 3);
        dl->AddText(nullptr, s * 0.36f, ImVec2(p.x + s * 0.26f, c.y - s * 0.2f), IM_COL32(255, 200, 60, 255), "2:1");
        dl->AddLine(ImVec2(c.x, c.y + r * 0.6f), ImVec2(c.x, c.y + r), W, lw);
        return true;
    }
    if (has("BallSpawner") || has("JakeBall") || has("DiscEntity"))   // a ball with a spawn arrow
    {
        bg(IM_COL32(30, 100, 60, 255));
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.2f), r * 0.6f, IM_COL32(240, 240, 240, 255));
        dl->AddCircle(ImVec2(c.x, c.y + r * 0.2f), r * 0.6f, K, 0, lw * 0.5f);
        dl->AddLine(ImVec2(c.x - r * 0.6f, c.y + r * 0.2f), ImVec2(c.x + r * 0.6f, c.y + r * 0.2f), K, lw * 0.5f);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r * 1.05f), ImVec2(c.x - r * 0.3f, c.y - r * 0.6f), ImVec2(c.x + r * 0.3f, c.y - r * 0.6f), W);
        return true;
    }
    if (has("Goal"))                                                // a net
    {
        bg(IM_COL32(30, 100, 60, 255));
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.7f), ImVec2(c.x + r, c.y + r * 0.7f), W, 0, 0, lw);
        for (int i = 1; i < 4; ++i) dl->AddLine(ImVec2(c.x - r + i * r * 0.5f, c.y - r * 0.7f), ImVec2(c.x - r + i * r * 0.5f, c.y + r * 0.7f), W, lw * 0.4f);
        return true;
    }
    if (has("_Text") || has("Text_C"))                              // a sign with a T
    {
        bg(IM_COL32(90, 90, 90, 255));
        dl->AddText(nullptr, s * 0.7f, ImVec2(c.x - s * 0.18f, c.y - s * 0.38f), W, "T");
        return true;
    }
    if (has("Teleport"))                                            // rings
    {
        bg(IM_COL32(90, 40, 140, 255));
        dl->AddCircle(c, r, IM_COL32(210, 160, 255, 255), 0, lw);
        dl->AddCircle(c, r * 0.55f, IM_COL32(210, 160, 255, 255), 0, lw);
        return true;
    }
    if (has("GameStateManager"))                                    // gears: the mode's controller
    {
        bg(IM_COL32(80, 80, 80, 255));
        dl->AddNgon(c, r, W, 8, lw);
        dl->AddCircleFilled(c, r * 0.35f, W);
        return true;
    }
    if (has("SlidingPlatform") || has("Platform"))                  // a slab with arrows
    {
        bg(IM_COL32(70, 90, 110, 255));
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.2f), ImVec2(c.x + r, c.y + r * 0.2f), W, 2);
        dl->AddTriangleFilled(ImVec2(c.x - r * 1.1f, c.y - r * 0.55f), ImVec2(c.x - r * 0.6f, c.y - r * 0.8f), ImVec2(c.x - r * 0.6f, c.y - r * 0.3f), W);
        dl->AddTriangleFilled(ImVec2(c.x + r * 1.1f, c.y - r * 0.55f), ImVec2(c.x + r * 0.6f, c.y - r * 0.8f), ImVec2(c.x + r * 0.6f, c.y - r * 0.3f), W);
        return true;
    }
    return false;
}
void IconFor(ImDrawList* dl, ImVec2 p, float s, const std::string& cls)
{
    if (const unsigned long long tex = IconTexture(cls))   // the game's own icon for this item
    {
        dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), IM_COL32(20, 20, 20, 255), s * 0.12f);
        dl->AddImageRounded(static_cast<ImTextureID>(tex), p, ImVec2(p.x + s, p.y + s), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, s * 0.12f);
        return;
    }
    if (IconGlyph(dl, p, s, cls)) return;
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
    if (!CanPlace()) return;
    if (!it.blocked.empty()) { Notes().Set(PrettyName(it.name) + ": " + it.blocked); return; }
    Command c{ CmdType::SpawnItem };
    c.str = it.path;
    c.loc = loc;
    if (g_snapEnabled)
        c.loc = { Snap(static_cast<float>(loc.x), g_gridSnap, true), Snap(static_cast<float>(loc.y), g_gridSnap, true),
                  Snap(static_cast<float>(loc.z), g_gridSnap, true) };
    c.rot = { 0.0, yaw, 0.0 };
    State().Push(c);
    UndoExpectNew(1, "Place");
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), it.path), g_recent.end());
    g_recent.insert(g_recent.begin(), it.path);
    if (g_recent.size() > 10) g_recent.resize(10);
}
// Duplicate works as in Unity: the copy lands where the original is -- same rotation, same size -- and the
// copies become the selection, so the gizmo drags them straight off the originals. (1 cm along X, not
// exactly on top: the server tells objects apart by class + position, and a perfect overlap is a coin toss.)
struct PendingDup { std::string cls; Vec3 at; bool primary = false; std::string found; };
static std::vector<PendingDup>  g_dupPending;
static std::vector<std::string> g_dupBefore;     // handles that existed when Duplicate was pressed
static double                   g_dupAt = -1.0;
static bool DuplicateSelection(const Snapshot& snap, const SceneObject& sel)
{
    const PaletteItem* it = FindItem(snap, sel.className);
    if (it && !it->blocked.empty()) { Notes().Set(PrettyName(it->name) + ": " + it->blocked); return false; }
    Command c{ CmdType::Duplicate };
    c.str   = sel.handle;
    c.str2  = it ? it->path : std::string();
    c.loc   = Add(sel.location, Vec3{ 1, 0, 0 });
    c.rot   = sel.rotation;
    c.scale = sel.scale;
    State().Push(c);
    return true;
}
// Ctrl+D / Edit > Duplicate: every selected object (Ctrl+click several to duplicate them together).
static void DuplicateSelected(const Snapshot& snap)
{
    if (!CanPlace()) return;
    g_dupPending.clear();
    g_dupBefore.clear();
    for (const auto& o : snap.objects) g_dupBefore.push_back(o.handle);
    for (const auto& o : snap.objects)
        if ((o.handle == g_selected || IsMultiSel(o.handle)) && DuplicateSelection(snap, o))
            g_dupPending.push_back({ o.className, Add(o.location, Vec3{ 1, 0, 0 }), o.handle == g_selected, "" });
    g_dupAt = ImGui::GetTime();
    UndoExpectNew(g_dupPending.size(), "Duplicate");
}
// Once the copies show up, select them (the one made from the gizmo's object gets the gizmo).
static void DupSelectTick(const Snapshot& snap)
{
    if (g_dupPending.empty()) return;
    bool all = true;
    for (auto& pd : g_dupPending)
    {
        if (!pd.found.empty()) continue;
        for (const auto& o : snap.objects)
        {
            if (o.className != pd.cls) continue;
            const Vec3 d = Sub(o.location, pd.at);
            if (Dot(d, d) > 5.0 * 5.0) continue;
            if (std::find(g_dupBefore.begin(), g_dupBefore.end(), o.handle) != g_dupBefore.end()) continue;
            bool taken = false;
            for (const auto& q : g_dupPending) if (q.found == o.handle) taken = true;
            if (taken) continue;
            pd.found = o.handle;
            break;
        }
        if (pd.found.empty()) all = false;
    }
    if (!all && ImGui::GetTime() - g_dupAt < 8.0) return;
    std::string primary;
    std::vector<std::string> rest;
    for (const auto& pd : g_dupPending)
    {
        if (pd.found.empty()) continue;
        if (pd.primary && primary.empty()) primary = pd.found; else rest.push_back(pd.found);
    }
    if (primary.empty() && !rest.empty()) { primary = rest.front(); rest.erase(rest.begin()); }
    g_dupPending.clear();
    if (primary.empty()) return;
    ClearSelection();
    g_selected = primary;
    Command sc{ CmdType::SelectObject }; sc.str = primary; State().Push(sc);
    g_multiSel = rest;
}
// Place along a ray, ON the first surface it hits (the game thread line-traces the level), falling back to
// `fallback` units down the ray over empty space. Grid snap applies in X/Y so it stays on the surface.
void SpawnTraced(const PaletteItem& it, const Vec3& from, const Vec3& dir, double fallback, double yaw)
{
    if (!CanPlace()) return;
    if (!it.blocked.empty()) { Notes().Set(PrettyName(it.name) + ": " + it.blocked); return; }
    Command c{ CmdType::SpawnTraced };
    c.str = it.path;
    c.loc = from;
    c.dir = dir;
    c.fallback = fallback;
    c.rot = { 0.0, yaw, 0.0 };
    c.snap = g_snapEnabled ? g_gridSnap : 0.0f;
    State().Push(c);
    UndoExpectNew(1, "Place");
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), it.path), g_recent.end());
    g_recent.insert(g_recent.begin(), it.path);
    if (g_recent.size() > 10) g_recent.resize(10);
}
// Double-click: onto whatever you are looking at (Unreal places it on the surface under screen centre).
void SpawnInFront(const Snapshot& snap, const PaletteItem& it)
{
    Vec3 fwd, rgt, up;
    RotAxes(snap.cameraRot, fwd, rgt, up);
    // Facing you, but on the world grid (nearest 90 deg): an arbitrary camera yaw left new pieces at odd angles
    // that no gizmo axis lined up with.
    SpawnTraced(it, snap.cameraPos, fwd, 400.0, std::round((snap.cameraRot.yaw + 180.0) / 90.0) * 90.0);
}
// Drag-drop: onto the surface under the cursor, like dropping an asset into Unreal's viewport.
void SpawnUnderMouse(const Snapshot& snap, const PaletteItem& it, ImVec2 mouse)
{
    const View v = MakeView(snap);
    if (!v.valid) { SpawnInFront(snap, it); return; }
    Vec3 dir = Add(v.fwd, Add(Mul(v.right, (mouse.x - v.centre.x) / v.focal), Mul(v.up, -(mouse.y - v.centre.y) / v.focal)));
    const double len = std::sqrt(Dot(dir, dir));
    dir = Mul(dir, 1.0 / len);
    SpawnTraced(it, snap.cameraPos, dir, 600.0, std::round((snap.cameraRot.yaw + 180.0) / 90.0) * 90.0);
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
// ---- the open level (a Unity-style scene) ------------------------------------------------------------
// One .a2level is "open" at a time (Documents\RigelLevels\<name>.a2level). Ctrl+S saves it (Save As the
// first time), it autosaves every 2 minutes while there are unsaved edits, opening another level closes this
// one (its objects leave the server) after saving it, and Upload puts it on the server's saved levels.
std::string g_sceneName;                     // "" = untitled (nothing open)
bool   g_saveAsOpen = false;
char   g_saveAsBuf[64] = "";
double g_sceneSavedAt = 0.0;
double g_autosaveSec = 120.0;
std::string SceneClean(const std::string& n)
{
    std::string c;
    for (char ch : n) if (isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == '_' || ch == '-') c += ch;
    return c.size() > 64 ? c.substr(0, 64) : c;
}
std::string SceneReadFile(const std::string& name)
{
    std::string text;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (LevelsDir() + L"\\" + std::wstring(name.begin(), name.end()) + L".a2level").c_str(), L"rb") == 0 && fp)
    {
        char buf[4096];
        for (size_t r; (r = fread(buf, 1, sizeof(buf), fp)) > 0;) text.append(buf, r);
        fclose(fp);
    }
    return text;
}
void SceneSave(const std::string& rawName, bool autosave = false)
{
    const std::string name = SceneClean(rawName);
    if (name.empty()) return;
    Command c{ CmdType::LevelExport }; c.str = name; c.str2 = "tag"; State().Push(c);
    g_sceneName = name;
    g_sceneDirty = false;
    g_sceneSavedAt = ImGui::GetTime();
    Log("[scene] %s '%s'", autosave ? "autosave" : "save", name.c_str());
    if (autosave) Notes().Set("Autosaved '" + name + "'.");
}
void SceneClose()
{
    if (g_sceneName.empty()) return;
    if (g_sceneDirty) SceneSave(g_sceneName, true);           // never lose work on a switch
    Command c{ CmdType::SendRaw }; c.str = "SE|LVCLOSE|" + g_sceneName; State().Push(c);
    Log("[scene] close '%s'", g_sceneName.c_str());
    g_sceneName.clear();
    g_sceneDirty = false;
}
void SceneOpen(const std::string& rawName)
{
    const std::string name = SceneClean(rawName);
    if (name.empty()) return;
    if (name == g_sceneName) { Notes().Set("'" + name + "' is already open."); return; }
    const std::string text = SceneReadFile(name);
    if (text.empty()) { Notes().Set("Couldn't read " + name + ".a2level."); return; }
    SceneClose();
    Command c{ CmdType::LevelImport }; c.str = name; c.str2 = "load"; c.str3 = text; State().Push(c);
    g_sceneName = name;
    g_sceneDirty = false;
    g_sceneSavedAt = ImGui::GetTime();
    Log("[scene] open '%s' (%zu bytes)", name.c_str(), text.size());
}
bool g_saveAsIsNew = false;                  // the name prompt is for New Level (title/wording)
bool g_needLevelAsk = false;                 // "open a level first" prompt pending
bool g_placeBypass = false;                  // test scripts place without a level
// Editors build INSIDE a level (a project): nothing can be placed until one is open, so every piece
// belongs to a file that can be saved, closed and uploaded -- no stray editor work left on the station.
bool CanPlace()
{
    if (!g_sceneName.empty() || g_placeBypass) return true;
    g_needLevelAsk = true;
    return false;
}
// New Level: close the open one (saved first if it has changes), then name the new one -- it exists
// (as an empty .a2level) from the moment it is named, so placing works straight away.
void SceneNew()
{
    SceneClose();
    g_saveAsIsNew = true;
    g_saveAsOpen = true;
    g_saveAsBuf[0] = 0;
}
std::string g_lvDeleteAsk;                   // a level waiting for "Delete?" confirmation
// Deletes Documents\RigelLevels\<name>.a2level to the Recycle Bin. The open level is closed first, without
// saving (saving would write the file straight back).
bool SceneDelete(const std::string& rawName)
{
    const std::string name = SceneClean(rawName);
    if (name.empty()) return false;
    if (name == g_sceneName)
    {
        Command c{ CmdType::SendRaw }; c.str = "SE|LVCLOSE|" + name; State().Push(c);
        g_sceneName.clear();
        g_sceneDirty = false;
    }
    std::wstring from = LevelsDir() + L"\\" + std::wstring(name.begin(), name.end()) + L".a2level";
    from.push_back(L'\0');                   // SHFileOperation wants a double-NUL list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const bool ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    Log("[scene] delete '%s' -> %s", name.c_str(), ok ? "recycled" : "FAILED");
    Notes().Set(ok ? "Deleted '" + name + "' (it's in the Recycle Bin)." : "Couldn't delete " + name + ".a2level.");
    return ok;
}
void SceneUpload()
{
    if (g_sceneName.empty()) { g_saveAsOpen = true; return; }
    if (g_sceneDirty) SceneSave(g_sceneName);
    Command c{ CmdType::SendRaw }; c.str = "SE|LVSAVE|" + g_sceneName; State().Push(c);
    Log("[scene] upload '%s'", g_sceneName.c_str());
}
void SceneSaveOrAsk()
{
    if (g_sceneName.empty()) { g_saveAsOpen = true; strncpy_s(g_saveAsBuf, "", _TRUNCATE); }
    else SceneSave(g_sceneName);
}
// Every frame: autosave, and the Save As prompt.
void SceneTick(const Snapshot& snap)
{
    // Tell the server which level is open (on change, and every 30 s in case it restarted): what this
    // editor places is that level's from the start.
    static std::string s_sentScene = "\x01";
    static double s_sentAt = -100.0;
    if (snap.inEditor && (s_sentScene != g_sceneName || ImGui::GetTime() - s_sentAt > 30.0))
    {
        Command c{ CmdType::SendRaw }; c.str = "SE|LVSCENE|" + g_sceneName; State().Push(c);
        s_sentScene = g_sceneName;
        s_sentAt = ImGui::GetTime();
    }
    static double dirtyAt = 0.0;               // autosave N seconds after the first unsaved edit, not after the last save
    static bool wasDirty = false;
    if (g_sceneDirty && !wasDirty) dirtyAt = ImGui::GetTime();
    wasDirty = g_sceneDirty;
    if (snap.inEditor && !g_sceneName.empty() && g_sceneDirty && ImGui::GetTime() - dirtyAt > g_autosaveSec)
        SceneSave(g_sceneName, true);
    if (TourOnce("popup:saveas")) g_saveAsOpen = true;
    if (TourOnce("popup:nolevel")) g_needLevelAsk = true;
    if (TourOnce("popup:deletelevel")) g_lvDeleteAsk = "ZUiTourNoSuchLevel";
    if (g_saveAsOpen) { ImGui::OpenPopup("Save Level As"); g_saveAsOpen = false; }
    if (g_needLevelAsk) { ImGui::OpenPopup("No Level Open"); g_needLevelAsk = false; }
    if (ImGui::BeginPopupModal("No Level Open", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Open or create a level before placing objects.");
        ImGui::TextDisabled("Everything you place belongs to the open level: Ctrl+S saves it,\nFile > Close Level removes it, Upload puts it on the server.");
        if (ImGui::Button("New Level...", ImVec2(130, 0))) { ImGui::CloseCurrentPopup(); SceneNew(); }
        ImGui::SameLine();
        if (ImGui::Button("Open Level...", ImVec2(130, 0))) { ImGui::CloseCurrentPopup(); g_showContent = true; g_cbFolder = "*levels"; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    static std::string s_delName;
    if (!g_lvDeleteAsk.empty()) { s_delName = g_lvDeleteAsk; g_lvDeleteAsk.clear(); ImGui::OpenPopup("Delete Level"); }
    if (ImGui::BeginPopupModal("Delete Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Delete '%s'?", s_delName.c_str());
        ImGui::TextDisabled(s_delName == g_sceneName ? "It's open: it will be closed (unsaved changes are lost).\nThe file goes to the Recycle Bin."
                                                     : "The file goes to the Recycle Bin.");
        if (ImGui::Button("Delete", ImVec2(120, 0))) { SceneDelete(s_delName); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save Level As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(g_saveAsIsNew ? "New level name (saved to Documents\\RigelLevels):" : "Level name (saved to Documents\\RigelLevels):");
        ImGui::SetNextItemWidth(320);
        const bool enter = ImGui::InputText("##saveas", g_saveAsBuf, sizeof(g_saveAsBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        const bool ok = !SceneClean(g_saveAsBuf).empty();
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button(g_saveAsIsNew ? "Create" : "Save", ImVec2(120, 0)) || (enter && ok))
        {
            if (g_saveAsIsNew && !SceneReadFile(SceneClean(g_saveAsBuf)).empty())
                Notes().Set("A level called '" + SceneClean(g_saveAsBuf) + "' already exists - open it from Levels, or pick another name.");
            else { SceneSave(g_saveAsBuf); g_saveAsIsNew = false; ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { g_saveAsIsNew = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

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
        ImGui::Separator();
        ImGui::BeginDisabled(!snap.inEditor);
        if (ImGui::MenuItem("New Level")) SceneNew();
        if (ImGui::MenuItem("Open Level...")) { g_showContent = true; g_cbFolder = "*levels"; }
        if (ImGui::MenuItem("Save Level", "Ctrl+S")) SceneSaveOrAsk();
        if (ImGui::MenuItem("Save Level As...")) { g_saveAsOpen = true; strncpy_s(g_saveAsBuf, g_sceneName.c_str(), _TRUNCATE); }
        if (ImGui::MenuItem("Upload Level to Server", nullptr, false, !g_sceneName.empty())) SceneUpload();
        if (ImGui::MenuItem("Close Level", nullptr, false, !g_sceneName.empty())) SceneClose();
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
        const std::string u = g_undo.empty() ? std::string("Undo") : "Undo " + g_undo.back().label;
        const std::string r = g_redo.empty() ? std::string("Redo") : "Redo " + g_redo.back().label;
        if (ImGui::MenuItem(u.c_str(), "Ctrl+Z", false, !g_undo.empty())) UndoApply(false);
        if (ImGui::MenuItem(r.c_str(), "Ctrl+Y", false, !g_redo.empty())) UndoApply(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, sel != nullptr) && sel)
            DuplicateSelected(snap);
        if (ImGui::MenuItem("Delete", "Delete", false, sel != nullptr))
            DeleteSelected();
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
        ImGui::Separator();
        ImGui::MenuItem("Trigger Areas", nullptr, &g_showTriggers);   // where a blueprint's contact zone is
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Select"))
    {
        if (ImGui::MenuItem("Select None", "Esc", false, !g_selected.empty()))
            ClearSelection();
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
        ImGui::BulletText("Ctrl+click adds to the selection (the gizmo moves it all); Ctrl+D duplicates in place and selects the copies");
        ImGui::BulletText("F12 hides the editor and returns your view");
        ImGui::EndMenu();
    }
    // Level name on the right, as UE5 shows the open map there.
    std::string lvlS = g_sceneName.empty() ? std::string(snap.inEditor ? "Untitled level" : "Station")
                                            : "Level: " + g_sceneName + (g_sceneDirty ? " *" : "");
    if (snap.inEditor && g_sceneName.empty() && g_sceneDirty) lvlS += " *";
    const char* lvl = lvlS.c_str();
    const float closeW = g_sceneName.empty() ? 0.0f : ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2 + 8;
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(lvl).x - 16 - closeW);
    ImGui::TextDisabled("%s", lvl);
    if (!g_sceneName.empty())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Close##level")) SceneClose();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close this level: it's saved if it has changes, and its objects leave the server.");
    }
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
    if (ImGui::Button(" Game Modes ")) g_gmWindow = !g_gmWindow;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make and run team games: teams, rounds, scores, traps, scoreboards, Luau.");
    ImGui::SameLine();
    if (ImGui::Button(" + Add ") || TourOnce("popup:quickadd")) ImGui::OpenPopup("quickadd");
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
                {
                    ImGui::PushID(static_cast<int>(k));     // LE_BP_X and LE_SM_X both read "X"
                    if (ImGui::MenuItem(PrettyName(snap.palette[k].name).c_str())) SpawnInFront(snap, snap.palette[k]);
                    ImGui::PopID();
                }
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
    if (ImGui::Selectable("Favorites", g_placeCat == "*fav")) g_placeCat = "*fav";
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
        if (ImGui::IsItemHovered())
        {
            if (!it.blocked.empty())      ImGui::SetTooltip("%s\n\n%s", PrettyName(it.name).c_str(), it.blocked.c_str());
            else if (!it.limited.empty()) ImGui::SetTooltip("%s\n\n%s\n\nDouble-click to place, or drag into the viewport", PrettyName(it.name).c_str(), it.limited.c_str());
            else                          ImGui::SetTooltip("%s\nDouble-click to place in front of you\nor drag into the viewport", it.path.c_str());
        }
        if (it.blocked.empty() && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) { g_dragPath = it.path; g_dragName = it.name; }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        IconFor(dl, ImVec2(p.x + 2, p.y + 2), 22, it.name);
        const std::string label = PrettyName(it.name) + (!it.blocked.empty() ? "  (unavailable)" : !it.limited.empty() ? "  (display only)" : "");
        dl->AddText(ImVec2(p.x + 30, p.y + 5), it.blocked.empty() ? kFg : kDim, label.c_str());
        const ImVec2 starC(p.x + ImGui::GetContentRegionAvail().x - 12, p.y + 13);
        if (g_favClickArm > 0 && --g_favClickArm == 0)   // (armed for a couple of frames so the list has settled)
        {
            POINT sp{ static_cast<LONG>(starC.x), static_cast<LONG>(starC.y) };
            ClientToScreen(static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw), &sp);
            StartSyntheticDrag(sp.x, sp.y, sp.x, sp.y);
            Log("[ui] favclick %s (fav=%d) at %ld,%ld", it.name.c_str(), (int)IsFav(it.path), sp.x, sp.y);
        }
        const bool starAte = FavStarButton(dl, starC, 7.0f, it.path);
        if (ImGui::BeginPopupContextItem("##favctx"))
        {
            if (ImGui::MenuItem(IsFav(it.path) ? "Remove from Favorites" : "Add to Favorites")) ToggleFav(it.path);
            ImGui::EndPopup();
        }
        if (!starAte && clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) SpawnInFront(snap, it);
        ImGui::PopID();
    };
    if (g_paletteFilter[0])
    {
        for (const auto& it : snap.palette) if (ContainsCi(it.name, g_paletteFilter)) row(it);
    }
    else if (g_placeCat == "*fav")
    {
        if (!g_favLoaded) FavLoad();
        if (g_favorites.empty()) ImGui::TextDisabled("No favorites yet.\nClick the star on any item\n(or right-click it) to add it.");
        const std::vector<std::string> favs = g_favorites;   // a copy: un-starring a row edits g_favorites mid-loop
        for (const auto& path : favs) if (const PaletteItem* it = FindItem(snap, path)) row(*it);
    }
    else if (g_placeCat.empty())
    {
        if (g_recent.empty()) ImGui::TextDisabled("Nothing placed yet.\nPick a category, or use\nthe Content Browser.");
        const std::vector<std::string> recent = g_recent;
        for (const auto& path : recent) if (const PaletteItem* it = FindItem(snap, path)) row(*it);
    }
    else
    {
        for (const auto& it : snap.palette) if (it.category == g_placeCat) row(it);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- Outliner --------------------------------------------------------------------------------
// "Click an object for this slot": set by clicking a script slot's field; the next object clicked (in the
// viewport or the Outliner) is wired to the slot instead of being selected.
struct SlotPick { bool on = false; std::string owner, script, slot, type; };
SlotPick g_slotPick;
bool SlotPickTake(const std::string& h)
{
    if (!g_slotPick.on) return false;
    Command c{ CmdType::LuauRef };
    c.str = g_slotPick.owner; c.str2 = g_slotPick.script; c.str3 = g_slotPick.slot; c.str4 = g_slotPick.type; c.str5 = h;
    State().Push(c);
    Notes().Set("Wiring " + g_slotPick.slot + "...");
    g_slotPick.on = false;
    return true;
}

void SelectHandle(const std::string& h)
{
    if (SlotPickTake(h)) return;
    g_multiSel.clear();
    if (h == g_selected) return;
    if (!g_selected.empty()) { Command d{ CmdType::DeselectObject }; d.str = g_selected; State().Push(d); }
    g_selected = h;
    Command c{ CmdType::SelectObject }; c.str = h; State().Push(c);
}

void DrawOutlinerPanel(const Snapshot& snap, ImVec2 pos, ImVec2 size)
{
    if (!BeginPanel("##outliner", pos, size)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("##outtabs")) { if (ImGui::BeginTabItem("Outliner")) ImGui::EndTabItem(); ImGui::EndTabBar(); }
    static bool s_nearFirst = true;
    ImGui::SetNextItemWidth(-110);
    ImGui::InputTextWithHint("##of", "Search...", g_outlinerFilter, sizeof(g_outlinerFilter));
    ImGui::SameLine();
    ImGui::Checkbox("Nearest first", &s_nearFirst);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by distance from the camera. Click a row to select it, double-click to fly to it.");

    // Row order: nearest first (or level order), and a running number on repeated names ("Light Switch 3")
    // so the station's many identical objects can be told apart.
    std::vector<int> order(snap.objects.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    auto dist2 = [&](const SceneObject& o) {
        const double dx = o.location.x - snap.cameraPos.x, dy = o.location.y - snap.cameraPos.y, dz = o.location.z - snap.cameraPos.z;
        return dx * dx + dy * dy + dz * dz;
    };
    if (s_nearFirst)
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return dist2(snap.objects[a]) < dist2(snap.objects[b]); });
    std::unordered_map<std::string, int> seen;
    std::vector<std::string> labels(snap.objects.size());
    for (size_t i = 0; i < snap.objects.size(); ++i)
    {
        const std::string base = PrettyName(snap.objects[i].className);
        const int k = ++seen[base];
        labels[i] = k > 1 ? base + " " + std::to_string(k) : base;
    }
    // Scroll the list to an object picked in the viewport.
    static std::string s_lastSel;
    const bool selChanged = g_selected != s_lastSel;
    s_lastSel = g_selected;

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
        for (const int oi : order)
        {
            const SceneObject& o = snap.objects[oi];
            const std::string& label = labels[oi];
            if (!ContainsCi(label, g_outlinerFilter) && !ContainsCi(o.className, g_outlinerFilter)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(o.handle.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Indent(18);
            if (ImGui::Selectable("##row", o.handle == g_selected || IsMultiSel(o.handle),
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 18)) &&
                !o.lockedByOther)
            {
                if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) && !g_slotPick.on) ToggleMultiSel(o.handle);
                else SelectHandle(o.handle);
                s_lastSel = o.handle;            // picked here: don't yank the scroll
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    Command c{ CmdType::FocusCamera }; c.loc = o.location; State().Push(c);
                }
            }
            if (selChanged && o.handle == g_selected) ImGui::SetScrollHereY(0.4f);
            if (ImGui::BeginDragDropSource())
            {
                // Drag an object onto a script slot (Details > Game data) to wire it.
                ImGui::SetDragDropPayload("SE_OBJ", o.handle.c_str(), o.handle.size() + 1);
                ImGui::Text("%s", PrettyName(o.className).c_str());
                ImGui::EndDragDropSource();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            IconFor(dl, ImVec2(p.x + 18, p.y + 1), 16, o.className);
            dl->AddText(ImVec2(p.x + 40, p.y + 2), o.lockedByOther ? IM_COL32(230, 150, 60, 255) : kFg, label.c_str());
            ImGui::Unindent(18);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n(%.0f, %.0f, %.0f)%s", o.className.c_str(), o.location.x, o.location.y, o.location.z,
                                  o.lockedByOther ? "\nLocked by another editor" : "");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s  %.0fm", o.className.rfind("LE_SM_", 0) == 0 ? "Mesh" : "Blueprint", std::sqrt(dist2(o)) / 100.0);
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
    ImGui::PushID(label);                                // table rows share one ID scope
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
    ImGui::PopID();
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
        ImGui::PushID(pi.owner.c_str());                 // a name can repeat across the class hierarchy
        ImGui::PushID(pi.path.c_str());
        ImGui::AlignTextToFramePadding();
        // Sync dot: green = the game sends this value to every player (Quest included); grey = it only exists on the
        // machine it is set on, so other players never see a change to it.
        ImGui::TextColored(pi.net ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f) : ImVec4(0.45f, 0.45f, 0.5f, 1.0f), pi.net ? "*" : ".");
        ImGui::SameLine(0, 4);
        if (pi.inert) ImGui::TextDisabled("%s", pi.name.c_str()); else ImGui::TextUnformatted(pi.name.c_str());
        if (ImGui::IsItemHovered())
        {
            const char* sync = pi.net ? "\n\nSynced: the game sends this value to every player, Quest included."
                                      : "\n\nNot synced: it only exists where it is set -- other players (Quest) keep their own copy,\n"
                                        "so a change here is not seen by them.";
            if (pi.inert)
                ImGui::SetTooltip("%s  (%s)\n\nNo effect: this is an input to the game's sandbox script, which doesn't run for\n"
                                  "editor-placed objects. Changing it is saved but nothing reads it.%s", pi.name.c_str(), pi.owner.c_str(), sync);
            else
                ImGui::SetTooltip("%s  (%s)\npath: %s%s", pi.name.c_str(), pi.owner.c_str(), pi.path.c_str(), sync);
        }
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
        default:   // read-only: replicated structs / arrays (PT_Composite) and anything else we don't edit
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", pi.value.empty() ? "(empty)" : pi.value.c_str());
            ImGui::PopStyleColor();
            break;
        }
        ImGui::PopID();
        ImGui::PopID();
    }
}

// ---- Quest Editor ------------------------------------------------------------------------------
// The quest's GUID as the server derives it from the id (SeQuestGuid in the server's specedit.h), in hex.
std::string QuestGuidHex(const std::string& questId)
{
    uint64_t h = 1469598103934665603ULL, h2 = 0x9E3779B97F4A7C15ULL;
    for (char c : questId) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ULL; }
    for (char c : questId) { h2 ^= static_cast<unsigned char>(c); h2 *= 0x100000001B3ULL; h2 ^= h2 >> 29; }
    char hx[40];
    snprintf(hx, sizeof(hx), "%08X%08X%08X%08X", static_cast<uint32_t>(h), static_cast<uint32_t>(h >> 32),
             static_cast<uint32_t>(h2), static_cast<uint32_t>(h2 >> 32) | 1u);
    return hx;
}
std::string QuestIdFrom(const char* title)
{
    std::string id = "SE_";
    for (const char* c = title; *c && id.size() < 40; ++c)
        id += isalnum(static_cast<unsigned char>(*c)) ? *c : '_';
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "_%04X", static_cast<unsigned>(GetTickCount64() & 0xFFFF));
    return id + suffix;
}

const SceneObject* FindObject(const Snapshot& snap, const std::string& handle)
{
    for (const auto& o : snap.objects) if (o.handle == handle) return &o;
    return nullptr;
}

// A point in front of the camera: where "place ... here" puts things.
Vec3 InFront(const Snapshot& snap, double dist)
{
    Vec3 fwd, rgt, up;
    RotAxes(snap.cameraRot, fwd, rgt, up);
    return Add(snap.cameraPos, Mul(fwd, dist));
}

// Construction mode (red coin runs): clicks in the viewport place coins on what they hit, for this quest.
int g_coinPlaceQuest = -1;                              // index into g_quests, -1 = off
size_t g_clickPlacedSeen = 0;

// Delete one of your quests: the server takes it out of every player's list (SE|QDEL) and removes its coin
// run; its unpublished preview coins go, and groups that listed it drop it.
void DeleteQuest(int i)
{
    if (i < 0 || i >= (int)g_quests.size()) return;
    QuestDraft& q = g_quests[i];
    if (!q.id.empty()) { Command c{ CmdType::SendRaw }; c.str = "SE|QDEL|" + q.id; State().Push(c); }
    for (const auto& h : q.coinObjs) { Command d{ CmdType::DeleteObject }; d.str = h; State().Push(d); }
    for (auto& g : g_quests)
    {
        const size_t before = g.parts.size();
        g.parts.erase(std::remove(g.parts.begin(), g.parts.end(), q.id), g.parts.end());
        if (g.parts.size() != before) g.dirty = true;
    }
    Notes().Set(std::string("Deleted quest '") + q.title + "'.");
    g_quests.erase(g_quests.begin() + i);
    if (g_pendingCp.quest == i) g_pendingCp.quest = -1; else if (g_pendingCp.quest > i) --g_pendingCp.quest;
    g_questSel = g_quests.empty() ? -1 : (std::min)(i, (int)g_quests.size() - 1);
}

// Quests published on this server by any editor (and by loaded levels) that aren't in your list -- e.g.
// made in an earlier session. They can be deleted from here.
void DrawServerQuests(const Snapshot& snap)
{
    std::vector<const Snapshot::QuestRef*> mine;
    for (const auto& r : snap.quests)
    {
        const std::string tag = " (editor)";
        if (r.title.size() <= tag.size() || r.title.compare(r.title.size() - tag.size(), tag.size(), tag) != 0) continue;
        bool local = false;
        for (const auto& d : g_quests) if (!d.id.empty() && QuestGuidHex(d.id) == r.id) local = true;
        if (!local) mine.push_back(&r);
    }
    if (mine.empty()) return;
    if (!ImGui::TreeNode("##srvq", "On the server, not in your list (%d)", (int)mine.size())) return;
    for (const auto* r : mine)
    {
        ImGui::PushID(r->id.c_str());
        ImGui::BulletText("%s", r->title.substr(0, r->title.size() - 9).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete"))
        {
            Command c{ CmdType::SendRaw }; c.str = "SE|QDEL|" + r->id; State().Push(c);
            Notes().Set("Deleted quest '" + r->title.substr(0, r->title.size() - 9) + "'.");
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void PublishQuest(QuestDraft& q)
{
    if (q.id.empty()) q.id = QuestIdFrom(q.title);
    if (q.kind == 2)
    {
        Command c{ CmdType::QuestCompile };
        c.str = q.id; c.str2 = q.title; c.str3 = q.glyph; c.num = q.repetition;
        c.str4 = q.desc[0] ? q.desc : q.title;
        for (const auto& id : q.parts) c.str5 += (c.str5.empty() ? "" : ";") + id;
        State().Push(c);
        q.published = true;
        q.dirty = false;
        Notes().Set(std::string("Published group '") + q.title + "' - its quests now show together under it, each with its own icon.");
        return;
    }
    if (q.kind == 1)
    {
        if (!q.coinObjs.empty())                       // the course as the preview coins stand now
        {
            const Snapshot snap = State().ReadSnapshot();
            q.coins.clear();
            // A live coin appears centred on its position, so publish the centre of each preview coin's box
            // (where its stand-in shows it) -- the object's origin sits at the bottom of the box, on the ground.
            for (const auto& h : q.coinObjs)
                if (const SceneObject* o = FindObject(snap, h))
                {
                    const double up = o->boundsExt.z > 0.0 ? o->boundsOff.z : 50.0;
                    q.coinLift = up;
                    q.coins.push_back(Vec3{ o->location.x, o->location.y, o->location.z + up });
                }
            for (const auto& h : q.coinObjs) { Command d{ CmdType::DeleteObject }; d.str = h; State().Push(d); }
            q.coinObjs.clear();
        }
        // The quest row first (no checkpoints: the game itself completes a coin run), then the run. The run
        // is anchored at its start button; coins are stored relative to it.
        Command c{ CmdType::QuestCompile };
        c.str = q.id; c.str2 = q.title; c.str3 = q.glyph; c.num = q.repetition;
        c.str4 = q.desc[0] ? q.desc : q.title;
        State().Push(c);
        std::string msg;
        char b[160];
        snprintf(b, sizeof(b), "SE|COINRUN|new|%.1f,%.1f,%.1f|%d|%s|", q.buttonAt.x, q.buttonAt.y, q.buttonAt.z, q.runSeconds, q.id.c_str());
        msg = b;
        for (size_t i = 0; i < q.coins.size(); ++i)
        {
            snprintf(b, sizeof(b), "%s%.1f,%.1f,%.1f", i ? ";" : "", q.coins[i].x, q.coins[i].y, q.coins[i].z);
            msg += b;
        }
        snprintf(b, sizeof(b), "|%.1f,%.1f,%.1f|%d", q.buttonAt.x, q.buttonAt.y, q.buttonAt.z, q.thrusters == 1 ? 1 : 2);
        msg += b;
        Command r{ CmdType::SendRaw }; r.str = msg; State().Push(r);
        q.published = true;
        q.dirty = false;
        Notes().Set(std::string("Published '") + q.title + "' - players press the start button, then collect every coin before time runs out.");
        return;
    }
    for (const auto& st : q.steps)
    {
        Command c{ CmdType::QuestAddStep }; c.str = st.objectHandle; c.str2 = "0"; State().Push(c);
    }
    Command c{ CmdType::QuestCompile };
    c.str = q.id; c.str2 = q.title; c.str3 = q.glyph; c.num = q.repetition;
    c.num2 = 0; c.f1 = 0.0f;                                  // no activation window, template progress
    c.str4 = q.desc[0] ? q.desc : q.title;
    c.radius = q.radiusM * 100.0;
    c.timeLimit = q.timed ? q.timeLimit : 0;
    State().Push(c);
    q.published = true;
    q.dirty = false;
    Notes().Set(std::string("Published '") + q.title + "' - players online get it now; it completes when they reach every checkpoint in order.");
}

// Adopt preview coins we asked for, once they replicate back: the new red coin nearest each request.
// Runs every frame for every quest, whichever tab is open, so no coin is ever left orphaned.
void AdoptPreviewCoins(const Snapshot& snap)
{
    if (g_coinPlaceQuest >= (int)g_quests.size() || (g_coinPlaceQuest >= 0 && g_quests[g_coinPlaceQuest].kind != 1)) g_coinPlaceQuest = -1;
    while (g_clickPlacedSeen < snap.clickPlaced.size())       // construction-mode coins: this quest's previews
    {
        const Vec3 at = snap.clickPlaced[g_clickPlacedSeen++];
        if (g_coinPlaceQuest >= 0)
        {
            g_quests[g_coinPlaceQuest].pendingCoins.push_back({ at, ImGui::GetTime() });
            g_quests[g_coinPlaceQuest].dirty = true;
        }
    }
    for (auto& q : g_quests)
    {
        if (q.pendingCoins.empty()) continue;
        for (auto it = q.pendingCoins.begin(); it != q.pendingCoins.end();)
        {
            const SceneObject* best = nullptr;
            double bestD = 200.0 * 200.0;
            for (const auto& o : snap.objects)
            {
                if (o.className != "LE_BP_RedCoin_C") continue;
                bool taken = false;
                for (const auto& d : g_quests)
                    if (std::find(d.coinObjs.begin(), d.coinObjs.end(), o.handle) != d.coinObjs.end()) { taken = true; break; }
                if (taken) continue;
                const Vec3 d = Sub(o.location, it->at);
                if (Dot(d, d) < bestD) { bestD = Dot(d, d); best = &o; }
            }
            if (best) { q.coinObjs.push_back(best->handle); it = q.pendingCoins.erase(it); }
            else if (ImGui::GetTime() - it->since > 12.0) it = q.pendingCoins.erase(it);   // gave up
            else ++it;
        }
    }
}

// A quest group: a folder in the players' quest list, with its own title and icon, holding several quests
// that each keep their own icon and complete on their own.
void DrawQuestGroup(const Snapshot& snap, QuestDraft& q)
{
    ImGui::SeparatorText("Quests in this group");
    int remove = -1;
    for (int i = 0; i < (int)q.parts.size(); ++i)
    {
        ImGui::PushID(i);
        const QuestDraft* part = nullptr;
        for (const auto& d : g_quests) if (d.id == q.parts[i]) part = &d;
        ImGui::AlignTextToFramePadding();
        if (part) ImGui::Text("%d. %s  [%s]%s", i + 1, part->title, part->glyph, part->published ? "" : "  (not published)");
        else ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%d. (removed quest)", i + 1);
        ImGui::SameLine(ImGui::GetWindowWidth() - 40);
        if (ImGui::SmallButton("X")) { remove = i; }
        ImGui::PopID();
    }
    if (remove >= 0) { q.parts.erase(q.parts.begin() + remove); q.dirty = true; }
    if (q.parts.empty()) ImGui::TextDisabled("Add quests you have made. Give each one its own icon in its own settings.");
    if (ImGui::BeginCombo("##addpart", "Add a quest to the group..."))
    {
        for (auto& d : g_quests)
        {
            if (&d == &q || d.kind == 2) continue;
            if (d.id.empty()) d.id = QuestIdFrom(d.title);
            if (std::find(q.parts.begin(), q.parts.end(), d.id) != q.parts.end()) continue;
            const std::string lab = std::string(d.title[0] ? d.title : "(untitled)") + "  [" + d.glyph + "]##" + d.id;
            if (ImGui::Selectable(lab.c_str())) { q.parts.push_back(d.id); q.dirty = true; }
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    bool unpublished = false;
    for (const auto& id : q.parts)
        for (const auto& d : g_quests) if (d.id == id && (!d.published || d.dirty)) unpublished = true;
    const char* problem = !q.title[0] ? "Give the group a name." :
                          q.parts.size() < 2 ? "Add at least two quests." :
                          unpublished ? "Publish every quest in the group first." :
                          !snap.inEditor ? "Click Start Editing first." : nullptr;
    ImGui::BeginDisabled(problem != nullptr);
    ImGui::PushStyleColor(ImGuiCol_Button, kSelBlue);
    if (ImGui::Button(q.published ? "Update group" : "Publish group", ImVec2(-1, 30))) PublishQuest(q);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (problem) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", problem);
    ImGui::TextDisabled("The group's icon is the one set above; each quest in it keeps its own.");
}

// The red coin run half of the quest editor: where it starts, the coins, the clock, publish.
void DrawCoinRun(const Snapshot& snap, QuestDraft& q, float lw)
{
    auto goBtn = [&](const Vec3& at) { Command c{ CmdType::FocusCamera }; c.loc = at; State().Push(c); };
    auto dist = [&](const Vec3& at) { const Vec3 d = Sub(at, snap.cameraPos); return std::sqrt(Dot(d, d)) / 100.0; };

    ImGui::SeparatorText("1. Start button");
    if (q.hasButton)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Placed  (%.0f m away)", dist(q.buttonAt));
        ImGui::SameLine(ImGui::GetWindowWidth() - 132);
        if (ImGui::SmallButton("Go##btn")) goBtn(q.buttonAt);
        ImGui::SameLine();
        if (ImGui::SmallButton("Move here")) { q.buttonAt = InFront(snap, 250.0); q.dirty = true; }
    }
    else
    {
        ImGui::TextWrapped("Players press this to start the run - the same button the TKB runs use.");
        if (ImGui::Button("Place start button here", ImVec2(-1, 0))) { q.buttonAt = InFront(snap, 250.0); q.hasButton = true; q.dirty = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("2.5 m in front of the camera.");
    }

    ImGui::SeparatorText("2. Coins (collect all of them)");
    const PaletteItem* coinItem = FindItem(snap, "LE_BP_RedCoin_C");
    auto placeCoin = [&](const Vec3& at) {
        if (!CanPlace()) return;
        if (!coinItem) { Notes().Set("The red coin prefab isn't in the palette, so coins can't be previewed."); return; }
        Command c{ CmdType::SpawnItem }; c.str = coinItem->path; c.loc = at; c.rot = { 0.0, 0.0, 0.0 }; State().Push(c);
        q.pendingCoins.push_back({ at, ImGui::GetTime() });
    };
    const bool editing = !q.coinObjs.empty() || !q.pendingCoins.empty();
    if (editing)
    {
        int remove = -1;
        for (int i = 0; i < (int)q.coinObjs.size(); ++i)
        {
            ImGui::PushID(i);
            const SceneObject* o = FindObject(snap, q.coinObjs[i]);
            ImGui::AlignTextToFramePadding();
            if (o) ImGui::Text("Coin %d", i + 1); else ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "Coin %d (deleted)", i + 1);
            ImGui::SameLine();
            if (o) ImGui::TextDisabled("%.0f m away", dist(o->location));
            ImGui::SameLine(ImGui::GetWindowWidth() - 132);
            ImGui::BeginDisabled(!o);
            if (ImGui::SmallButton("Select")) SelectHandle(q.coinObjs[i]);
            ImGui::SameLine(); if (ImGui::SmallButton("Go")) goBtn(o->location);
            ImGui::EndDisabled();
            ImGui::SameLine(); if (ImGui::SmallButton("X")) remove = i;
            ImGui::PopID();
        }
        if (remove >= 0)
        {
            Command d{ CmdType::DeleteObject }; d.str = q.coinObjs[remove]; State().Push(d);
            q.coinObjs.erase(q.coinObjs.begin() + remove);
            q.dirty = true;
        }
        if (!q.pendingCoins.empty()) ImGui::TextDisabled("Placing %d coin(s)...", (int)q.pendingCoins.size());
        ImGui::TextDisabled("Select a coin and drag it with the move tool to fine-tune it.");
    }
    else if (!q.coins.empty())
    {
        ImGui::Text("%d coin(s) in the published run.", (int)q.coins.size());
        if (ImGui::Button("Edit coins (show them)", ImVec2(-1, 0)))
            for (const auto& c : q.coins) placeCoin(Vec3{ c.x, c.y, c.z - q.coinLift });   // back to the box's bottom
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brings the coins back as movable objects. Update the run when you're done.");
    }
    else ImGui::TextDisabled("No coins yet. Fly to a spot and add one.");
    const int total = (int)(q.coinObjs.size() + q.pendingCoins.size());
    ImGui::BeginDisabled(total >= 30 || (!editing && !q.coins.empty()));
    if (ImGui::Button("Add coin here", ImVec2(-1, 0))) { placeCoin(InFront(snap, 250.0)); q.dirty = true; }
    const int qi = static_cast<int>(&q - g_quests.data());
    const bool constructing = g_coinPlaceQuest == qi;
    if (constructing) ImGui::PushStyleColor(ImGuiCol_Button, kSelBlue);
    if (ImGui::Button(constructing ? "Placing by clicking - click here or press Esc to stop" : "Place coins by clicking (construction mode)", ImVec2(-1, 0)))
    {
        g_coinPlaceQuest = constructing ? -1 : qi;
        g_clickPlacedSeen = snap.clickPlaced.size();
    }
    if (constructing) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click in the world to drop a coin where you click (it rests on the surface you click).\n"
                                                  "Right-drag still flies the camera. Esc stops.");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(total >= 30 ? "30 coins is the most one run can hold." :
                          (!editing && !q.coins.empty()) ? "Click Edit coins first." : "Places a real coin 2.5 m in front of the camera. Move it like any object.");

    ImGui::SeparatorText("3. Time limit");
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Seconds"); ImGui::SameLine(lw); ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("##qrs", &q.runSeconds, 5, 30)) q.dirty = true;
    q.runSeconds = (std::max)(10, (std::min)(600, q.runSeconds));
    ImGui::TextDisabled("%d:%02d from pressing the button. Miss it and the run resets.", q.runSeconds / 60, q.runSeconds % 60);

    ImGui::SeparatorText("4. Boosting");
    if (ImGui::RadioButton("Allowed (boost pads work)", q.thrusters == 0)) { q.thrusters = 0; q.dirty = true; }
    if (ImGui::RadioButton("Boosting fails the run (like TKB)", q.thrusters == 1)) { q.thrusters = 1; q.dirty = true; }

    // Unpublished coins and the button, drawn in the viewport so you can see the course you are building.
    {
        const View v = MakeView(snap);
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImVec2 sp;
        for (size_t i = 0; v.valid && i < q.pendingCoins.size(); ++i)
            if (W2S(v, q.pendingCoins[i].at, sp))
            {
                dl->AddCircleFilled(sp, 9.0f, IM_COL32(235, 60, 50, 230));
                dl->AddCircle(sp, 9.0f, IM_COL32(255, 220, 120, 255), 0, 2.0f);
                char n[8]; snprintf(n, sizeof(n), "%d", (int)i + 1);
                dl->AddText(ImVec2(sp.x + 11, sp.y - 8), IM_COL32(255, 255, 255, 230), n);
            }
        if (v.valid && q.hasButton && W2S(v, q.buttonAt, sp))
        {
            dl->AddRectFilled(ImVec2(sp.x - 8, sp.y - 8), ImVec2(sp.x + 8, sp.y + 8), IM_COL32(60, 150, 255, 230), 3.0f);
            dl->AddText(ImVec2(sp.x + 11, sp.y - 8), IM_COL32(255, 255, 255, 230), "Start");
        }
    }

    ImGui::Spacing();
    const char* problem = !q.title[0] ? "Give the quest a name." :
                          !q.hasButton ? "Place the start button." :
                          (q.coins.empty() && q.coinObjs.empty()) ? "Add at least one coin." :
                          !q.pendingCoins.empty() ? "Waiting for coins to appear..." :
                          !snap.inEditor ? "Click Start Editing first." : nullptr;
    ImGui::BeginDisabled(problem != nullptr);
    ImGui::PushStyleColor(ImGuiCol_Button, kSelBlue);
    if (ImGui::Button(q.published ? "Update run" : "Publish run", ImVec2(-1, 30))) PublishQuest(q);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (problem) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", problem);
    else if (q.published && !q.dirty) ImGui::TextDisabled("Live. Updating rebuilds the run and its button.");
    ImGui::Spacing();
    ImGui::TextDisabled("Runs live until the server restarts.");
}

// Test scripts: build a red coin run quest as the UI would (button 2.5 m ahead, coins in a ring) and publish
// it through the same PublishQuest the Publish button uses.
void ScriptPublishCoinRun(const Snapshot& snap, const std::string& title, int seconds, int coins)
{
    g_quests.emplace_back();
    QuestDraft& q = g_quests.back();
    strncpy_s(q.title, title.c_str(), _TRUNCATE);
    q.kind = 1;
    q.runSeconds = seconds;
    q.buttonAt = InFront(snap, 250.0);
    q.hasButton = true;
    for (int i = 0; i < coins; ++i)
    {
        const double a = 6.2831853 * i / (coins > 0 ? coins : 1);
        q.coins.push_back({ q.buttonAt.x + std::cos(a) * 400.0, q.buttonAt.y + std::sin(a) * 400.0, q.buttonAt.z + 80.0 });
    }
    g_questSel = (int)g_quests.size() - 1;
    PublishQuest(q);
}

void DrawQuestEditor(const Snapshot& snap, const SceneObject* sel)
{
    // Adopt a checkpoint coin we asked the server to place, once it has replicated back to us.
    if (g_pendingCp.quest >= 0 && g_pendingCp.quest < (int)g_quests.size())
    {
        for (const auto& o : snap.objects)
            if (o.className == g_pendingCp.cls &&
                std::find(g_pendingCp.before.begin(), g_pendingCp.before.end(), o.handle) == g_pendingCp.before.end())
            {
                g_quests[g_pendingCp.quest].steps.push_back({ o.handle, "Checkpoint coin" });
                g_quests[g_pendingCp.quest].dirty = true;
                g_pendingCp.quest = -1;
                break;
            }
        if (g_pendingCp.quest >= 0 && ImGui::GetTime() - g_pendingCp.at > 10.0) g_pendingCp.quest = -1;   // gave up
    }

    // ── quest list ──
    ImGui::TextUnformatted("Quests");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
    if (ImGui::SmallButton("+ New quest"))
    {
        g_quests.emplace_back();
        snprintf(g_quests.back().title, sizeof(g_quests.back().title), "New quest %d", (int)g_quests.size());
        g_questSel = (int)g_quests.size() - 1;
    }
    if (g_quests.empty())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("Two kinds of quest, both working for everyone (Quest players included):");
        ImGui::BulletText("Red coin run - like the TKB runs: a start button, then collect\nevery coin before the timer runs out.");
        ImGui::BulletText("Checkpoint run - reach objects you placed, in order,\noptionally against the clock.");
        ImGui::Spacing();
        ImGui::TextDisabled("Click  + New quest  to start.");
        return;
    }
    if (ImGui::BeginListBox("##quests", ImVec2(-1, (std::min)(4, (int)g_quests.size()) * ImGui::GetTextLineHeightWithSpacing() + 6)))
    {
        for (int i = 0; i < (int)g_quests.size(); ++i)
        {
            const QuestDraft& q = g_quests[i];
            char label[160];
            const int n = q.kind == 1 ? (int)(q.coins.size() + q.coinObjs.size()) : q.kind == 2 ? (int)q.parts.size() : (int)q.steps.size();
            snprintf(label, sizeof(label), "%s  -  %d %s%s  %s##q%d", q.title[0] ? q.title : "(untitled)", n,
                     q.kind == 1 ? "coin" : q.kind == 2 ? "quest" : "checkpoint", n == 1 ? "" : "s",
                     !q.published ? "(not published)" : q.dirty ? "(changed)" : "(live)", i);
            if (ImGui::Selectable(label, g_questSel == i)) g_questSel = i;
        }
        ImGui::EndListBox();
    }
    DrawServerQuests(snap);
    if (g_questSel < 0 || g_questSel >= (int)g_quests.size()) { ImGui::TextDisabled("Pick a quest to edit it."); return; }
    if (ImGui::SmallButton("Delete quest")) ImGui::OpenPopup("##qdel");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes it from every player's quest list, with its coins / start button.");
    if (ImGui::BeginPopup("##qdel"))
    {
        ImGui::Text("Delete '%s' for everyone?", g_quests[g_questSel].title);
        if (ImGui::Button("Delete")) { DeleteQuest(g_questSel); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_questSel < 0 || g_questSel >= (int)g_quests.size()) return;
    QuestDraft& q = g_quests[g_questSel];
    ImGui::Separator();

    // ── what players see ──
    const float lw = 110.0f;
    auto label = [&](const char* t) { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(t); ImGui::SameLine(lw); ImGui::SetNextItemWidth(-1); };
    label("Name");        if (ImGui::InputTextWithHint("##qt", "What players see in their quest list", q.title, sizeof(q.title))) q.dirty = true;
    label("Description"); if (ImGui::InputTextWithHint("##qd", "Optional - e.g. Reach the roof in 60 seconds", q.desc, sizeof(q.desc))) q.dirty = true;
    label("Icon");
    if (ImGui::BeginCombo("##qg", q.glyph))
    {
        if (snap.glyphs.empty()) ImGui::TextDisabled("(icons appear once the game has sent you its quests)");
        for (const auto& g : snap.glyphs)
            if (ImGui::Selectable(g.c_str(), g == q.glyph)) { strncpy_s(q.glyph, g.c_str(), _TRUNCATE); q.dirty = true; }
        ImGui::EndCombo();
    }
    label("Repeats");
    {
        const char* reps[] = { "Once", "Every day", "Every week", "Every month" };
        if (ImGui::Combo("##qr", &q.repetition, reps, IM_ARRAYSIZE(reps))) q.dirty = true;
    }

    label("Type");
    {
        const char* kinds[] = { "Checkpoint run - reach points in order", "Red coin run - start button + coins (like TKB)",
                                "Quest group - shows several quests together (folder)" };
        ImGui::BeginDisabled(q.published);                   // a published quest keeps its type
        if (ImGui::Combo("##qk", &q.kind, kinds, IM_ARRAYSIZE(kinds))) q.dirty = true;
        ImGui::EndDisabled();
    }
    if (q.kind == 1) { DrawCoinRun(snap, q, lw); return; }
    if (q.kind == 2) { DrawQuestGroup(snap, q); return; }

    // ── checkpoints ──
    ImGui::SeparatorText("Checkpoints (reached in this order)");
    int remove = -1, up = -1, down = -1;
    for (int i = 0; i < (int)q.steps.size(); ++i)
    {
        ImGui::PushID(i);
        const SceneObject* o = FindObject(snap, q.steps[i].objectHandle);
        char dist[48] = "";
        if (o)
        {
            const Vec3 d = Sub(o->location, snap.cameraPos);
            snprintf(dist, sizeof(dist), "%.0f m away", std::sqrt(Dot(d, d)) / 100.0);
        }
        ImGui::AlignTextToFramePadding();
        if (o) ImGui::Text("%d. %s", i + 1, q.steps[i].label.c_str());
        else   ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%d. %s (deleted)", i + 1, q.steps[i].label.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dist);
        const float x = ImGui::GetWindowWidth() - 132;
        ImGui::SameLine(x);
        ImGui::BeginDisabled(!o);
        if (ImGui::SmallButton("Go")) { Command c{ CmdType::FocusCamera }; c.loc = o->location; State().Push(c); }
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(i == 0); if (ImGui::ArrowButton("##up", ImGuiDir_Up)) up = i; ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(i + 1 == (int)q.steps.size()); if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) down = i; ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::SmallButton("X")) remove = i;
        ImGui::PopID();
    }
    if (remove >= 0) { q.steps.erase(q.steps.begin() + remove); q.dirty = true; }
    if (up > 0) { std::swap(q.steps[up], q.steps[up - 1]); q.dirty = true; }
    if (down >= 0 && down + 1 < (int)q.steps.size()) { std::swap(q.steps[down], q.steps[down + 1]); q.dirty = true; }
    if (q.steps.empty()) ImGui::TextDisabled("No checkpoints yet.");

    const bool selIsStep = sel && std::any_of(q.steps.begin(), q.steps.end(), [&](const QuestStep& st) { return st.objectHandle == sel->handle; });
    ImGui::BeginDisabled(!sel || selIsStep);
    if (ImGui::Button("Add selected object", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 4, 0)))
    {
        q.steps.push_back({ sel->handle, PrettyName(sel->className) });
        q.dirty = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(!sel ? "Click an object in the viewport first." : selIsStep ? "That object is already a checkpoint." : "Use the selected object as the next checkpoint.");
    ImGui::SameLine();
    if (ImGui::Button("Place new checkpoint here", ImVec2(-1, 0)))
    {
        if (const PaletteItem* it = FindItem(snap, "LE_BP_RedCoin_C"))
        {
            g_pendingCp = { g_questSel, it->name, {}, ImGui::GetTime() };
            for (const auto& o : snap.objects) if (o.className == it->name) g_pendingCp.before.push_back(o.handle);
            SpawnInFront(snap, *it);
        }
        else Notes().Set("The red coin prefab isn't in the palette, so a checkpoint can't be placed for you. Place any object and use Add selected object.");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Places a red coin in front of the camera and makes it the next checkpoint.");
    if (g_pendingCp.quest == g_questSel) ImGui::TextDisabled("Placing checkpoint...");

    // ── rules ──
    ImGui::SeparatorText("Rules");
    label("Touch distance"); if (ImGui::SliderFloat("##qrad", &q.radiusM, 1.0f, 10.0f, "%.1f m")) q.dirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How close a player has to get to a checkpoint for it to count.");
    ImGui::AlignTextToFramePadding();
    if (ImGui::Checkbox("Time limit", &q.timed)) q.dirty = true;
    if (q.timed)
    {
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("##qtl", &q.timeLimit, 5, 30)) q.dirty = true;
        q.timeLimit = (std::max)(5, (std::min)(3600, q.timeLimit));
        ImGui::TextDisabled("%d:%02d from the first checkpoint; running out starts the player over.", q.timeLimit / 60, q.timeLimit % 60);
    }

    // ── publish ──
    ImGui::Spacing();
    const char* problem = !q.title[0] ? "Give the quest a name." :
                          q.steps.empty() ? "Add at least one checkpoint." :
                          std::any_of(q.steps.begin(), q.steps.end(), [&](const QuestStep& st) { return !FindObject(snap, st.objectHandle); })
                              ? "A checkpoint was deleted - remove it from the list." :
                          !snap.inEditor ? "Click Start Editing first." : nullptr;
    ImGui::BeginDisabled(problem != nullptr);
    ImGui::PushStyleColor(ImGuiCol_Button, kSelBlue);
    if (ImGui::Button(q.published ? "Update quest" : "Publish quest", ImVec2(-1, 30))) PublishQuest(q);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (problem) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", problem);
    else if (q.published && !q.dirty) ImGui::TextDisabled("Live. Changes you make will show as (changed) until you update.");
    ImGui::Spacing();
    ImGui::TextDisabled("Quests live until the server restarts.");
}


// ---- Luau: scripts are files you write in VS Code ---------------------------------------------------------
// Your scripts live in Documents\RigelScripts (created on first use and seeded with the IntelliSense kit:
// type definitions, VS Code settings, examples, the guide). Pick a file and attach it to the selected object;
// every save in VS Code is sent again automatically and the objects running it pick up the new code.
struct ScriptFile { std::wstring path; std::string name, rel; };
std::vector<ScriptFile> g_scriptFiles;
int g_scriptSel = -1;
double g_scriptsListed = -100.0;
bool g_autoResend = true;
struct WatchedScript { std::wstring path; std::string name; FILETIME written{}; };
std::vector<WatchedScript> g_watched;
int g_scriptPick = -1;
bool g_showGameScripts = false;

std::wstring ScriptsDir()
{
    static std::wstring dir;
    if (!dir.empty()) return dir;
    wchar_t docs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) dir = std::wstring(docs) + L"\\RigelScripts";
    else dir = L"C:\\RigelScripts";
    // Every launch: copy in the kit shipped beside the game (RigelLuau\) -- new examples and ready-made
    // scripts are added and your own files are never touched; the type definitions and guides are kept
    // current (they are the kit's, not yours).
    const bool fresh = GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (fresh) CreateDirectoryW(dir.c_str(), nullptr);
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring root(exe);
        for (int up = 0; up < 4; ++up) { const size_t c = root.find_last_of(L'\\'); if (c == std::wstring::npos) break; root.resize(c); }
        const std::wstring kit = root + L"\\RigelLuau";
        std::error_code ec;
        if (std::filesystem::exists(kit, ec))
        {
            std::filesystem::copy(kit, dir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing, ec);
            for (const wchar_t* ours : { L"types", L"tools", L"mcp", L"Rigel-Luau-Guide.pdf", L"Rigel-Quest-Guide.pdf", L"Rigel-GameModes-Guide.pdf",
                                         L"Rigel-MCP-Guide.pdf", L"GAMEMODES.md", L"README.md",
                                         L".vscode\\tasks.json" })   // the Rigel checker task (Problems panel)
            {
                const std::wstring from = kit + L"\\" + ours, to = dir + L"\\" + ours;
                if (std::filesystem::exists(from, ec))
                    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            }
            // Example game mode projects: their levels go where levels live, and each mode's own code goes where the
            // Game Modes window looks for it (GameModes\<mode name>.luau). Never over a file the user has.
            wchar_t docs2[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs2)))
            {
                const std::wstring levels = std::wstring(docs2) + L"\\RigelLevels";
                std::filesystem::create_directories(levels, ec);
                if (std::filesystem::exists(kit + L"\\levels", ec))
                    std::filesystem::copy(kit + L"\\levels", levels, std::filesystem::copy_options::skip_existing, ec);
            }
            std::filesystem::create_directories(dir + L"\\GameModes", ec);
            if (std::filesystem::exists(kit + L"\\examples\\gamemodes", ec))
                std::filesystem::copy(kit + L"\\examples\\gamemodes", dir + L"\\GameModes", std::filesystem::copy_options::skip_existing, ec);
        }
    }
    if (fresh)
    {
        const std::wstring hello = dir + L"\\MyScript.luau";
        if (GetFileAttributesW(hello.c_str()) == INVALID_FILE_ATTRIBUTES)
            if (FILE* f = _wfopen(hello.c_str(), L"wb"))
            {
                const char* t = "--!strict\n-- Attach me to an object in the Spec Editor (Details > Luau script).\n"
                                "-- Every save here is sent to the game automatically.\n"
                                "log(\"Hello from MyScript\")\n";
                fwrite(t, 1, strlen(t), f);
                fclose(f);
            }
    }
    return dir;
}
std::string ReadFileUtf8(const std::wstring& path)
{
    std::string out;
    if (FILE* f = _wfopen(path.c_str(), L"rb"))
    {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < 400000) out.append(buf, n);
        fclose(f);
    }
    return out;
}
FILETIME WrittenAt(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    FILETIME t{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) t = a.ftLastWriteTime;
    return t;
}
void ListScripts()
{
    g_scriptFiles.clear();
    const std::wstring dir = ScriptsDir();
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file(ec) || it->path().extension() != L".luau") continue;
        const std::wstring rel = std::filesystem::relative(it->path(), dir, ec).wstring();
        if (rel.rfind(L"types", 0) == 0 || rel.rfind(L"game-scripts", 0) == 0) continue;   // not attachable
        ScriptFile f;
        f.path = it->path().wstring();
        f.name = it->path().stem().string();
        f.rel = std::string(rel.begin(), rel.end());
        g_scriptFiles.push_back(f);
    }
    std::sort(g_scriptFiles.begin(), g_scriptFiles.end(), [](const ScriptFile& a, const ScriptFile& b) { return a.rel < b.rel; });
    g_scriptsListed = ImGui::GetTime();
}
void WatchScript(const std::wstring& path, const std::string& name)
{
    for (auto& w : g_watched) if (w.path == path) { w.name = name; w.written = WrittenAt(path); return; }
    g_watched.push_back({ path, name, WrittenAt(path) });
}
// Every frame: a watched file that changed on disk is sent again (VS Code save -> game).
// ---- script problems: explained in plain words, with the line to look at --------------------------------
// Syntax errors are caught BEFORE a script is sent (luau-compile from the kit, RigelScripts\tools) -- the game
// itself silently skips a script that doesn't compile. Runtime errors are read from this game's own log,
// where every machine's Luau reports them, and shown as a popup too.
std::string ExplainLuauError(const std::string& msg)
{
    auto has = [&](const char* t) { return msg.find(t) != std::string::npos; };
    if (has("SyntaxError") || has("Expected ") || has("Incomplete statement") || has("Malformed"))
        return "The script has a typo, so none of it can run. Look at the line shown: usually a missing 'then' (after if), "
               "'do' (after for / while), 'end', or a bracket or quote that isn't closed. VS Code underlines these in red as you type.";
    if (has("attempt to index nil"))
    {
        std::string what;
        const size_t q = msg.find('\'', msg.find("attempt to index nil"));
        if (q != std::string::npos) what = msg.substr(q + 1, msg.find('\'', q + 1) - q - 1);
        return "Something was nil (empty) when the script used '" + what + "' on it. If it's a slot (a line like  "
               "local Target: PhysicalComponent = nil), it isn't wired yet: select the scripted object, open Details > Game data, "
               "click the slot and then click the object it should use. Otherwise make sure the variable has a value before "
               "that line, or guard it with  if x ~= nil then ... end.";
    }
    if (has("attempt to call"))
        return "The script called a function that doesn't exist. Check the spelling and capitals (VS Code's autocomplete lists "
               "the real names) and use ':' for methods, e.g. Target:hideLua() rather than Target.hideLua().";
    if (has("attempt to perform arithmetic"))
        return "The script did maths on something that isn't a number (often nil). Make sure both sides are numbers, "
               "e.g. (x or 0) + 1 or tonumber(x).";
    if (has("attempt to concatenate"))
        return "Joining text with .. only works on text and numbers. Wrap other values in tostring(...), e.g. \"hits: \" .. tostring(n).";
    if (has("attempt to compare"))
        return "The script compared two values that can't be compared (e.g. a number with nil). Check both sides have a value of the same kind.";
    if (has("stack overflow"))
        return "A function keeps calling itself forever. Add a condition that makes it stop.";
    if (has("timeout") || has("exhausted"))
        return "The script ran too long without pausing - probably a loop that never ends. Wait with LuauClock.timeout(seconds) instead.";
    return "The script stopped at the line shown; the message above says what went wrong. Nothing else is affected - the "
           "object and the game keep running.";
}

std::string WideToUtf8(const std::wstring& w)
{
    std::string out(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), (int)out.size(), nullptr, nullptr);
    return out;
}
std::wstring Utf8ToWide(const std::string& s)
{
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), (int)out.size());
    return out;
}

// luau-compile --null <file>: "" when the file compiles (or the checker isn't installed), else its error line.
std::string LuauSyntaxError(const std::wstring& file, int* line)
{
    const std::wstring exe = ScriptsDir() + L"\\tools\\luau-compile.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return std::string();
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return std::string();
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" --null \"" + file + L"\"";
    std::string out;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(wr);
        wr = nullptr;
        char buf[1024];
        DWORD n = 0;
        while (out.size() < 16000 && ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);   // until it exits
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    // "C:/.../Name.luau(4,3): SyntaxError: Expected 'then' when parsing if statement, got 'log'"
    size_t b = 0;
    while (b < out.size())
    {
        size_t e = out.find('\n', b);
        if (e == std::string::npos) e = out.size();
        std::string l = out.substr(b, e - b);
        b = e + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.find("Error") == std::string::npos || l.rfind("Compiled", 0) == 0) continue;
        const size_t paren = l.find(".luau(");
        if (paren != std::string::npos)
        {
            if (line) *line = atoi(l.c_str() + paren + 6);
            const size_t colon = l.find("): ", paren);
            if (colon != std::string::npos) l = l.substr(colon + 3);
        }
        return l;
    }
    return std::string();
}

// Typed locals the game can't turn into editor properties (the same rule the server applies before it stores
// a script -- specedit.h SeLuauSanitize): every `local name: Type` becomes a property of the object, and a
// type like `{ [string]: boolean }` made the object's build crash natively on the server and every client.
// Returns the offenders as "line N (name)"; strings and comments are skipped.
struct UnsafeLocal { int line; std::string name, type; };
std::vector<UnsafeLocal> LuauUnsafeTypedLocals(const std::string& src)
{
    std::vector<UnsafeLocal> out;
    auto isId = [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    auto isProperty = [](std::string t) {
        while (!t.empty() && isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        size_t a = 0; while (a < t.size() && isspace(static_cast<unsigned char>(t[a]))) ++a;
        t = t.substr(a);
        if (!t.empty() && t.back() == '?') t.pop_back();
        if (t == "number" || t == "string" || t == "boolean") return true;
        if (t.size() > 9 && t.compare(t.size() - 9, 9, "Component") == 0)
        {
            for (char c : t) if (!(isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
            return true;
        }
        return false;
    };
    auto longBracket = [&](size_t i) -> size_t {
        if (i >= src.size() || src[i] != '[') return std::string::npos;
        size_t j = i + 1, eq = 0;
        while (j < src.size() && src[j] == '=') { ++eq; ++j; }
        if (j >= src.size() || src[j] != '[') return std::string::npos;
        const std::string close = "]" + std::string(eq, '=') + "]";
        const size_t e = src.find(close, j + 1);
        return e == std::string::npos ? src.size() : e + close.size();
    };
    auto lineOf = [&](size_t at) { return 1 + static_cast<int>(std::count(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(at), '\n')); };
    size_t i = 0;
    while (i < src.size())
    {
        const char c = src[i];
        if (c == '-' && i + 1 < src.size() && src[i + 1] == '-')
        {
            const size_t lb = longBracket(i + 2);
            if (lb != std::string::npos) { i = lb; continue; }
            const size_t e = src.find('\n', i);
            i = e == std::string::npos ? src.size() : e;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`')
        {
            size_t j = i + 1;
            while (j < src.size() && src[j] != c && src[j] != '\n') { if (src[j] == '\\') ++j; ++j; }
            i = j + 1;
            continue;
        }
        if (c == '[') { const size_t lb = longBracket(i); if (lb != std::string::npos) { i = lb; continue; } }
        if (c == 'l' && src.compare(i, 5, "local") == 0 && (i == 0 || !isId(src[i - 1])) && i + 5 < src.size() && !isId(src[i + 5]))
        {
            size_t j = i + 5;
            for (;;)
            {
                while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
                const size_t n0 = j;
                while (j < src.size() && isId(src[j])) ++j;
                if (j == n0 || (j - n0 == 8 && src.compare(n0, 8, "function") == 0)) break;
                const std::string name = src.substr(n0, j - n0);
                while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
                if (j < src.size() && src[j] == '<') { const size_t e = src.find('>', j); if (e == std::string::npos) break; j = e + 1; }
                while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
                if (j < src.size() && src[j] == ':')
                {
                    const size_t colon = j;
                    size_t k = j + 1;
                    int depth = 0;
                    while (k < src.size())
                    {
                        const char d = src[k];
                        if (d == '{' || d == '(' || d == '<') ++depth;
                        else if (d == '}' || d == ')' || d == '>') { if (depth > 0) --depth; }
                        else if (depth == 0 && (d == '=' || d == ',' || d == '\n' || d == ';')) break;
                        else if (d == '-' && k + 1 < src.size() && src[k + 1] == '-' && depth == 0) break;
                        ++k;
                    }
                    std::string type = src.substr(colon + 1, k - colon - 1);
                    if (!isProperty(type))
                    {
                        for (char& ch : type) if (ch == '\n' || ch == '\t') ch = ' ';
                        out.push_back({ lineOf(colon), name, type.size() > 60 ? type.substr(0, 57) + "..." : type });
                    }
                    j = k;
                }
                while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
                if (j < src.size() && src[j] == ',') { ++j; continue; }
                break;
            }
            i = j;
            continue;
        }
        ++i;
    }
    return out;
}

// True when the file is fine to send; otherwise explains the problem in a popup.
bool CheckScriptFile(const std::wstring& path, const std::string& name)
{
    int line = 0;
    const std::string err = LuauSyntaxError(path, &line);
    if (err.empty())
    {
        // Compiles -- but a typed local the game can't build as a property would crash every player. The server
        // removes those types itself (the script still works), so this is a heads-up, not a refusal.
        const auto bad = LuauUnsafeTypedLocals(ReadFileUtf8(path));
        if (!bad.empty())
        {
            ProblemBox::Item it;
            it.title = name + ".luau: " + std::to_string(bad.size()) + " type(s) removed when sent (line " + std::to_string(bad[0].line) + ")";
            std::string list;
            for (const auto& b : bad) list += "  line " + std::to_string(b.line) + ":  local " + b.name + ":" + b.type + "\n";
            it.text = "Every typed local at the top of a script (local Name: Type = ...) becomes a property of the object in the "
                      "editor. Only these types can be properties: ...Component slots, number, string and boolean. Any other "
                      "type - a table like { [string]: boolean }, a function type, a union - crashed the server and every "
                      "player's game when the object was built.\n\n" + list +
                      "\nThe server removes those type annotations before anyone runs the script, so it still works exactly as "
                      "written and these variables keep their values. To silence this, drop the type yourself, e.g.\n"
                      "  local " + bad[0].name + " = ...";
            it.file = WideToUtf8(path);
            it.line = bad[0].line;
            Problems().Push(it);
            Log("[script] %s.luau: %zu typed local(s) the game can't build as properties (first: line %d %s)", name.c_str(), bad.size(),
                bad[0].line, bad[0].name.c_str());
        }
        return true;
    }
    ProblemBox::Item it;
    it.title = "Typo in " + name + ".luau" + (line ? " (line " + std::to_string(line) + ")" : std::string()) + " - not sent";
    it.text = err + "\n\n" + ExplainLuauError(err) + "\n\nFix it and save the file. A script that's already attached is sent again automatically; otherwise click Attach again.";
    it.file = WideToUtf8(path);
    it.line = line;
    Problems().Push(it);
    return false;
}

// Runtime errors from this game's log (%LOCALAPPDATA%\A2\Saved\Logs): the newest A2*.log created since we started.
void WatchLuauErrors()
{
    static double s_last = -10.0;
    if (ImGui::GetTime() - s_last < 1.0) return;
    s_last = ImGui::GetTime();
    static std::wstring s_log;
    static long long s_off = 0;
    if (s_log.empty())
    {
        wchar_t la[MAX_PATH] = {};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return;
        const std::wstring dir = std::wstring(la) + L"\\A2\\Saved\\Logs\\";
        FILETIME created{}, ex{}, kt{}, ut{};
        GetProcessTimes(GetCurrentProcess(), &created, &ex, &kt, &ut);
        ULARGE_INTEGER since; since.LowPart = created.dwLowDateTime; since.HighPart = created.dwHighDateTime;
        since.QuadPart -= 10ULL * 10000000ULL;                 // 10 s of slack
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"A2*.log").c_str(), &fd);
        ULONGLONG best = 0;
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcsstr(fd.cFileName, L"backup")) continue;
                ULARGE_INTEGER c; c.LowPart = fd.ftCreationTime.dwLowDateTime; c.HighPart = fd.ftCreationTime.dwHighDateTime;
                if (c.QuadPart >= since.QuadPart && c.QuadPart > best) { best = c.QuadPart; s_log = dir + fd.cFileName; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (s_log.empty()) return;
    }
    HANDLE f = CreateFileW(s_log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { s_log.clear(); return; }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    if (size.QuadPart < s_off) s_off = 0;
    std::string text;
    if (size.QuadPart > s_off)
    {
        const long long want = (std::min)(size.QuadPart - s_off, 1LL << 20);
        text.resize(static_cast<size_t>(want));
        LARGE_INTEGER at; at.QuadPart = s_off;
        SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
        DWORD got = 0;
        ReadFile(f, text.data(), static_cast<DWORD>(want), &got, nullptr);
        text.resize(got);
        const size_t lastNl = text.rfind('\n');
        text.resize(lastNl == std::string::npos ? 0 : lastNl + 1);   // only whole lines
        s_off += static_cast<long long>(text.size());
    }
    CloseHandle(f);
    if (!text.empty() && ImGui::GetTime() - g_scriptsListed > 5.0) ListScripts();   // know your script names
    static std::unordered_set<std::string> s_seen;
    size_t b = 0;
    while (b < text.size())
    {
        size_t e = text.find('\n', b);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(b, e - b);
        b = e + 1;
        // [string "Name.luau"]:5: attempt to index nil with 'hideLua'
        const size_t st = l.find("[string \"");
        if (st == std::string::npos) continue;
        if (l.find("LogLuau: Error") == std::string::npos && l.find("[RigelError]") == std::string::npos &&
            l.find("Traceback") == std::string::npos) continue;
        const size_t ne = l.find('"', st + 9);
        if (ne == std::string::npos) continue;
        std::string name = l.substr(st + 9, ne - st - 9);
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".luau") == 0) name.resize(name.size() - 5);
        const ScriptFile* sf = nullptr;
        for (const auto& fl : g_scriptFiles) if (fl.name == name) sf = &fl;
        if (!sf) continue;                                     // only scripts from your folder, not the game's own
        int line = 0;
        std::string msg;
        if (ne + 3 < l.size() && l[ne + 1] == ']' && l[ne + 2] == ':')
        {
            line = atoi(l.c_str() + ne + 3);
            const size_t c = l.find(':', ne + 3);
            msg = c == std::string::npos ? std::string() : l.substr(c + 1);
        }
        const size_t tb = msg.find(" Traceback");
        if (tb != std::string::npos) msg.resize(tb);
        while (!msg.empty() && (msg.front() == ' ')) msg.erase(msg.begin());
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == ' ')) msg.pop_back();
        if (!s_seen.insert(name + ":" + std::to_string(line) + ":" + msg).second) continue;   // once per session
        ProblemBox::Item it;
        it.title = "Error in " + name + ".luau" + (line ? " (line " + std::to_string(line) + ")" : std::string());
        it.text = msg + "\n\n" + ExplainLuauError(msg);
        it.file = WideToUtf8(sf->path);
        it.line = line;
        Problems().Push(it);
    }
}

// The popup: one problem at a time, with the fix and a jump to the line.
void DrawProblems()
{
    ProblemBox::Item it;
    if (!Problems().Front(it)) return;
    if (!ImGui::IsPopupOpen("Problem##problems")) ImGui::OpenPopup("Problem##problems");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Problem##problems", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.25f, 1.0f), "%s", it.title.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s", it.text.c_str());
        ImGui::Spacing();
        if (!it.file.empty())
        {
            if (ImGui::Button("Open in VS Code"))
            {
                const std::wstring arg = L"-g \"" + Utf8ToWide(it.file) + L":" + std::to_wstring(it.line > 0 ? it.line : 1) + L"\"";
                if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"code", arg.c_str(), nullptr, SW_HIDE)) <= 32)
                    ShellExecuteW(nullptr, L"open", Utf8ToWide(it.file).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::SameLine();
        }
        // Copy the whole problem (to paste into a message, a bug report or an AI chat). Ctrl+C does the same.
        const bool copyKey = ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false);
        if (ImGui::Button("Copy error") || copyKey)
        {
            std::string all = it.title + "\n\n" + it.text;
            if (!it.file.empty()) all += "\n\nFile: " + it.file + (it.line > 0 ? ":" + std::to_string(it.line) : std::string());
            ImGui::SetClipboardText(all.c_str());
            Notes().Set("Copied the error to the clipboard.");
        }
        ImGui::SameLine();
        if (ImGui::Button("OK", ImVec2(80, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter))
        {
            Problems().Pop();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ResendChangedScripts()
{
    static double s_last = 0;
    if (!g_autoResend || ImGui::GetTime() - s_last < 1.0) return;
    s_last = ImGui::GetTime();
    for (auto& w : g_watched)
    {
        const FILETIME t = WrittenAt(w.path);
        if (CompareFileTime(&t, &w.written) == 0) continue;
        w.written = t;
        if (!CheckScriptFile(w.path, w.name)) continue;          // typo: explained, not sent
        Command c{ CmdType::LuauUpdate }; c.str2 = w.name; c.str3 = ReadFileUtf8(w.path); State().Push(c);
        Notes().Set("Sent " + w.name + ".luau again (saved in your editor).");
    }
}
const ScriptFile* ScriptCombo(const char* id, int& sel)
{
    const char* cur = sel >= 0 && sel < (int)g_scriptFiles.size() ? g_scriptFiles[sel].rel.c_str() : "Pick a script file";
    if (ImGui::BeginCombo(id, cur))
    {
        for (int i = 0; i < (int)g_scriptFiles.size(); ++i)
            if (ImGui::Selectable(g_scriptFiles[i].rel.c_str(), i == sel)) sel = i;
        ImGui::EndCombo();
    }
    return sel >= 0 && sel < (int)g_scriptFiles.size() ? &g_scriptFiles[sel] : nullptr;
}
void AttachScriptFile(const SceneObject* sel, const ScriptFile& f, const std::string& asName)
{
    if (!CheckScriptFile(f.path, f.name)) return;             // typo: explained, not attached
    WatchScript(f.path, asName);
    Command c{ CmdType::LuauAttach }; c.str = sel->handle; c.str2 = asName; c.str3 = ReadFileUtf8(f.path); State().Push(c);
    WatchScript(f.path, asName);
}

void DrawScriptSlots(const Snapshot& snap, const SceneObject* sel, const std::string& script);   // below
void DrawLuau(const Snapshot& snap, const SceneObject* sel)
{
    if (ImGui::GetTime() - g_scriptsListed > 5.0) ListScripts();
    const std::wstring dir = ScriptsDir();
    ImGui::TextWrapped("Write scripts in VS Code (IntelliSense included) in your RigelScripts folder, then attach "
                       "one here. It runs for every player, is saved with levels, and every save is sent again "
                       "automatically.");
    if (ImGui::Button("Open folder in VS Code"))
    {
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"code", (L"\"" + dir + L"\"").c_str(), nullptr, SW_HIDE)) <= 32)
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::Button("Refresh##luau")) ListScripts();
    ImGui::SetNextItemWidth(-1);
    const ScriptFile* f = ScriptCombo("##scriptfile", g_scriptSel);
    if (g_scriptFiles.empty()) ImGui::TextDisabled("No .luau files yet - create one in the folder.");
    ImGui::BeginDisabled(!f || !snap.inEditor);
    ImGui::PushStyleColor(ImGuiCol_Button, kSelBlue);
    if (ImGui::Button("Attach to this object", ImVec2(-1, 0)) && f) AttachScriptFile(sel, *f, f->name);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::Checkbox("Send again every time I save the file", &g_autoResend);
    if (!g_watched.empty())
    {
        ImGui::TextDisabled("Watching:");
        for (const auto& w : g_watched) { ImGui::SameLine(); ImGui::TextDisabled("%s.luau", w.name.c_str()); }
    }
    // The scripts this object runs, each with its slots -- right here, under the button that attached them.
    // (They were only listed up in Game data, out of sight after attaching: "nothing comes up to configure".)
    ImGui::Spacing();
    if (snap.dataHandle != sel->handle)
    {
        static std::string s_asked;                          // Game data may be collapsed: ask once ourselves
        if (s_asked != sel->handle) { s_asked = sel->handle; Command c{ CmdType::DataRequest }; c.str = sel->handle; State().Push(c); }
        ImGui::TextDisabled("Loading this object's scripts...");
    }
    else
    {
        bool any = false;
        ImGui::PushID("##luau-slots");                       // the same slots are drawn in Game data too
        for (const auto& e : snap.data)
        {
            if (e.kind != "script") continue;
            any = true;
            ImGui::SeparatorText(("On this object: " + e.value).c_str());
            ImGui::PushID(e.value.c_str());
            DrawScriptSlots(snap, sel, e.value);
            ImGui::PopID();
        }
        ImGui::PopID();
        if (!any) ImGui::TextDisabled("No script on this object yet. Attach one above and its slots show up here.");
    }
    ImGui::Spacing();
    if (ImGui::Button("Copy the game's own scripts into the folder (examples)"))
    {
        Command c{ CmdType::ScanScripts }; State().Push(c);
        g_showGameScripts = true;
    }
    if (g_showGameScripts && !snap.gameScripts.empty())
    {
        const std::wstring gdir = dir + L"\\game-scripts";
        CreateDirectoryW(gdir.c_str(), nullptr);
        for (const auto& gs : snap.gameScripts)
        {
            std::wstring fn = gdir + L"\\" + std::wstring(gs.where.begin(), gs.where.end()) + L"__" + std::wstring(gs.name.begin(), gs.name.end());
            if (FILE* out = _wfopen(fn.c_str(), L"wb")) { fwrite(gs.source.data(), 1, gs.source.size(), out); fclose(out); }
        }
        Notes().Set("Copied " + std::to_string(snap.gameScripts.size()) + " game script(s) into RigelScripts\\game-scripts.");
        g_showGameScripts = false;
    }
}

// ---- Game data: every value the game syncs for the selected object ---------------------------------------
// What the game's own level editor configures -- a kiosk's quests, a button's target quest, a run's timer,
// a switch's state -- read from the server and written back through the replicated setters, so every player
// (vanilla Quest included) gets the change. Values are shown by type; quest ids get a picker.
std::string g_dataFor;                                   // handle the section was last filled for
int g_dataSeen = -1;
std::unordered_map<std::string, std::string> g_dataEdit;  // path -> the text being edited

std::string DataLabel(const std::string& path)
{
    std::string n = path.substr(path.find('/') + 1);
    std::string out;
    for (size_t i = 0; i < n.size(); ++i)                // camelCase -> "camel Case"
    {
        if (i && isupper((unsigned char)n[i]) && islower((unsigned char)n[i - 1])) out += ' ';
        out += n[i] == '_' ? ' ' : n[i];
    }
    if (path.rfind("gd/", 0) == 0) out += "  (live state)";
    if (path.rfind("script/", 0) == 0) out = "Script " + path.substr(7);
    return out;
}
const char* QuestTitle(const Snapshot& snap, const std::string& id)
{
    for (const auto& q : snap.quests) if (q.id == id) return q.title.empty() ? "(untitled quest)" : q.title.c_str();
    return nullptr;
}
bool QuestPicker(const Snapshot& snap, const char* label, std::string& id)
{
    bool changed = false;
    const char* cur = id.empty() ? "(none)" : QuestTitle(snap, id);
    if (ImGui::BeginCombo(label, cur ? cur : id.c_str(), ImGuiComboFlags_HeightLarge))
    {
        static char filter[64] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##qf", "Search quests", filter, sizeof(filter));
        for (const auto& q : snap.quests)
        {
            if (filter[0] && !ContainsCi(q.title, filter)) continue;
            const std::string lab = (q.title.empty() ? q.id : q.title) + "##" + q.id;
            if (ImGui::Selectable(lab.c_str(), q.id == id)) { id = q.id; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// The slots a script declares: `local Name: SomethingComponent = nil` (the game's "External Dependencies").
struct ScriptSlot { std::string name, type; };
std::vector<ScriptSlot> ParseSlots(const std::string& src)
{
    std::vector<ScriptSlot> out;
    size_t b = 0;
    while (b < src.size())
    {
        size_t e = src.find('\n', b);
        if (e == std::string::npos) e = src.size();
        std::string line = src.substr(b, e - b);
        b = e + 1;
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line.compare(i, 6, "local ") != 0) continue;
        i += 6;
        size_t n0 = i; while (i < line.size() && (isalnum((unsigned char)line[i]) || line[i] == '_')) ++i;
        const std::string name = line.substr(n0, i - n0);
        while (i < line.size() && line[i] == ' ') ++i;
        if (name.empty() || i >= line.size() || line[i] != ':') continue;
        ++i; while (i < line.size() && line[i] == ' ') ++i;
        size_t t0 = i; while (i < line.size() && (isalnum((unsigned char)line[i]) || line[i] == '_')) ++i;
        const std::string type = line.substr(t0, i - t0);
        const std::string rest = line.substr(i);
        if (type.size() > 9 && type.compare(type.size() - 9, 9, "Component") == 0 && rest.find("nil") != std::string::npos)
            out.push_back({ name, type });
    }
    return out;
}
// Under a script in Game data: each slot, what it points at, and a drop target (drag from the Outliner).
void DrawScriptSlots(const Snapshot& snap, const SceneObject* sel, const std::string& script)
{
    std::string stem = script;
    if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".luau") == 0) stem.resize(stem.size() - 5);
    const ScriptFile* file = nullptr;
    for (const auto& f : g_scriptFiles) if (f.name == stem) file = &f;
    if (!file) { ImGui::TextDisabled("  (no %s.luau in your RigelScripts folder - its slots can't be shown)", stem.c_str()); return; }
    const auto slots = ParseSlots(ReadFileUtf8(file->path));
    if (slots.empty()) { ImGui::TextDisabled("  No slots. Declare one with:  local Target: PhysicalComponent = nil"); return; }
    for (const auto& sl : slots)
    {
        ImGui::PushID(sl.name.c_str());
        std::string current = "drop an object here";
        for (const auto& d : snap.data)
            if (d.kind == "slot" && d.path == "slot/" + script + "/" + sl.name)
                current = PrettyName(d.value.substr(0, d.value.find('@')));
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::Text("%s", sl.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", sl.type.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f);
        const bool picking = g_slotPick.on && g_slotPick.owner == sel->handle && g_slotPick.script == script && g_slotPick.slot == sl.name;
        if (picking) ImGui::PushStyleColor(ImGuiCol_Button, kSelBlue);
        if (ImGui::Button(picking ? "click an object... (Esc)" : current.c_str(), ImVec2(-52, 0)))
        {
            if (picking) g_slotPick.on = false;
            else g_slotPick = { true, sel->handle, script, sl.name, sl.type };
        }
        if (picking) ImGui::PopStyleColor();
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SE_OBJ"))
            {
                Command c{ CmdType::LuauRef }; c.str = sel->handle; c.str2 = script; c.str3 = sl.name; c.str4 = sl.type;
                c.str5 = static_cast<const char*>(pl->Data);
                State().Push(c);
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click here, then click the object in the viewport or the Outliner -- or drag it here from the Outliner,\n"
                              "or pick from the list (v). It must have a %s.", sl.type.c_str());
        ImGui::SameLine();
        if (ImGui::ArrowButton("##cands", ImGuiDir_Down))
        {
            Command c{ CmdType::SlotScan }; c.str = sl.type; State().Push(c);
            ImGui::OpenPopup("##slotcands");   // (the tour opens it through the button path)
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Objects that fit this slot (they have a %s)", sl.type.c_str());
        if (ImGui::BeginPopup("##slotcands"))
        {
            ImGui::TextDisabled("Objects with a %s", sl.type.c_str());
            ImGui::Separator();
            int shown = 0;
            std::vector<std::string> kinds;                  // item types that have this component
            if (snap.slotCandType == sl.type)
            {
                std::vector<std::pair<double, const Snapshot::SlotCand*>> list;
                // Objects the editor placed are named with their GUID id; the station's own can't be wired.
                auto placed = [](const std::string& h) { return h.size() >= 36 && h[8] == '-' && h[13] == '-' && h[18] == '-' && h[23] == '-'; };
                for (const auto& cand : snap.slotCands)
                    for (const auto& ob : snap.objects)
                        if (ob.handle == cand.handle)
                        {
                            const std::string k = PrettyName(ob.className);
                            if (std::find(kinds.begin(), kinds.end(), k) == kinds.end() && kinds.size() < 12) kinds.push_back(k);
                            if (!placed(ob.handle)) continue;
                            const double dx = ob.location.x - snap.cameraPos.x, dy = ob.location.y - snap.cameraPos.y, dz = ob.location.z - snap.cameraPos.z;
                            list.push_back({ std::sqrt(dx * dx + dy * dy + dz * dz), &cand });
                        }
                std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                for (const auto& [dist, cand] : list)
                {
                    if (shown >= 40) break;
                    std::string cls;
                    for (const auto& ob : snap.objects) if (ob.handle == cand->handle) cls = ob.className;
                    const std::string label = PrettyName(cls) + (cand->handle == sel->handle ? "  (this object)" : "") +
                                              "   " + cand->comp + "  " + std::to_string(static_cast<int>(dist / 100.0)) + "m##" + cand->handle;
                    if (ImGui::Selectable(label.c_str()))
                    {
                        Command c{ CmdType::LuauRef }; c.str = sel->handle; c.str2 = script; c.str3 = sl.name; c.str4 = sl.type; c.str5 = cand->handle;
                        State().Push(c);
                    }
                    ++shown;
                }
            }
            if (!shown) ImGui::TextDisabled(snap.slotCandType == sl.type ? "None of your placed objects has one yet." : "Looking...");
            if (snap.slotCandType == sl.type && !kinds.empty())
            {
                std::string k;
                for (const auto& x : kinds) k += (k.empty() ? "" : ", ") + x;
                ImGui::Separator();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360);
                ImGui::TextDisabled("Items that have one: %s", k.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
        {
            Command c{ CmdType::LuauRef }; c.str = sel->handle; c.str2 = script; c.str3 = sl.name; c.str4 = sl.type; State().Push(c);
        }
        ImGui::PopID();
    }
}

void DrawGameData(const Snapshot& snap, const SceneObject* sel)
{
    auto request = [&]() { Command c{ CmdType::DataRequest }; c.str = sel->handle; State().Push(c); };
    if (g_dataFor != sel->handle) { g_dataFor = sel->handle; g_dataEdit.clear(); request(); }
    if (snap.dataSerial != g_dataSeen) { g_dataSeen = snap.dataSerial; g_dataEdit.clear(); }
    auto set = [&](const Snapshot::DataEntry& e, const std::string& v) {
        Command c{ CmdType::DataSet }; c.str = sel->handle; c.str2 = e.path; c.str3 = e.kind; c.str4 = v; State().Push(c);
    };
    {
        // One line whenever what this section shows changes: the first thing to read when someone says
        // "nothing comes up to configure".
        std::string sig = sel->handle + (snap.dataHandle == sel->handle ? " loaded" : " LOADING (data is for " + snap.dataHandle + ")");
        sig += ", " + std::to_string(snap.data.size()) + " value(s)";
        for (const auto& e : snap.data)
            if (e.kind == "script")
            {
                std::string stem = e.value;
                if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".luau") == 0) stem.resize(stem.size() - 5);
                const ScriptFile* file = nullptr;
                for (const auto& f : g_scriptFiles) if (f.name == stem) file = &f;
                sig += "; script " + e.value + (file ? ": " + std::to_string(ParseSlots(ReadFileUtf8(file->path)).size()) + " slot(s)" : ": NO LOCAL FILE");
            }
        static std::string s_sig;
        if (sig != s_sig) { s_sig = sig; Log("[ui] Game data %s", sig.c_str()); }
    }
    if (snap.dataHandle != sel->handle) { ImGui::TextDisabled("Loading..."); return; }
    if (snap.data.empty())
    {
        ImGui::TextDisabled("Nothing to configure on this object.");
        if (ImGui::SmallButton("Refresh")) request();
        return;
    }
    const float lw = 150.0f;
    for (const auto& e : snap.data)
    {
        if (e.kind == "slot") continue;                  // drawn under its script
        ImGui::PushID(e.path.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(DataLabel(e.path).c_str());
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(-1);
        const bool questy = e.path.find("Quest") != std::string::npos;
        if (e.kind == "bool")
        {
            bool b = e.value == "1";
            if (ImGui::Checkbox("##v", &b)) set(e, b ? "1" : "0");
        }
        else if (questy && (e.kind == "guid" || (e.kind == "str" && e.value.size() <= 32)))
        {
            std::string id = e.value;
            if (QuestPicker(snap, "##v", id)) set(e, id);
        }
        else if (e.kind == "guids")
        {
            std::vector<std::string> ids;
            for (size_t b = 0; b < e.value.size();) { size_t x = e.value.find(',', b); if (x == std::string::npos) x = e.value.size(); if (x > b) ids.push_back(e.value.substr(b, x - b)); b = x + 1; }
            ImGui::NewLine();
            int drop = -1;
            for (int i = 0; i < (int)ids.size(); ++i)
            {
                ImGui::PushID(i);
                const char* t = QuestTitle(snap, ids[i]);
                ImGui::BulletText("%s", t ? t : ids[i].c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - 40);
                if (ImGui::SmallButton("X")) drop = i;
                ImGui::PopID();
            }
            std::string add;
            ImGui::SetNextItemWidth(-1);
            if (QuestPicker(snap, "##add", add) && !add.empty()) ids.push_back(add), drop = -2;
            if (drop != -1)
            {
                if (drop >= 0) ids.erase(ids.begin() + drop);
                std::string v;
                for (size_t i = 0; i < ids.size(); ++i) v += (i ? "," : "") + ids[i];
                set(e, v);
            }
        }
        else if (e.kind == "script")
        {
            // A script this object already runs (the game's own, or one attached earlier): replace its code
            // with a file from RigelScripts.
            ImGui::NewLine();
            ImGui::SetNextItemWidth(-160);
            static int pick = -1;
            const ScriptFile* sf = ScriptCombo("##rep", pick);
            ImGui::SameLine();
            ImGui::BeginDisabled(!sf);
            if (ImGui::Button("Replace") && sf) AttachScriptFile(sel, *sf, e.value);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Sends the file's code as %s. Objects you placed are rebuilt with it right away; "
                                  "station objects give it to players who join from now on.", e.value.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Remove")) ImGui::OpenPopup("##rmscript");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take %s off this object (it is rebuilt without it, for everyone).", e.value.c_str());
            if (ImGui::BeginPopup("##rmscript"))
            {
                ImGui::Text("Remove %s from this object?", e.value.c_str());
                if (ImGui::Button("Remove##yes"))
                {
                    Command c{ CmdType::LuauRemove }; c.str = sel->handle; c.str2 = e.value; State().Push(c);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            DrawScriptSlots(snap, sel, e.value);
        }
        else if (e.kind == "hex")
            ImGui::TextDisabled("(%zu bytes, not editable here)", e.value.size() / 2);
        else
        {
            auto it = g_dataEdit.find(e.path);
            if (it == g_dataEdit.end()) it = g_dataEdit.emplace(e.path, e.value).first;
            char buf[256];
            strncpy_s(buf, it->second.c_str(), _TRUNCATE);
            const bool numeric = e.kind == "num" || e.kind == "float" || e.kind == "double" || e.kind == "int" || e.kind == "byte";
            if (ImGui::InputText("##v", buf, sizeof(buf), numeric ? ImGuiInputTextFlags_CharsScientific : 0)) it->second = buf;
            if (ImGui::IsItemDeactivatedAfterEdit() && it->second != e.value) set(e, it->second);
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Refresh")) request();
    ImGui::SameLine();
    ImGui::TextDisabled("Changes reach every player.");
}


// ---- Levels: save what you built to the backend, and load saved levels on this server ------------------
// A level is everything placed in the editor (objects, their Game data, quests, groups, coin runs). Saving
// stores everything built on the server (loaded levels included) under that name. Autoload (load on server boot) is
// set on the dashboard; Load / Unload here or there apply to the server within ~15 s.
char g_levelName[64] = "";
double g_levelsAsked = -100.0;
void DrawLevels(const Snapshot& snap)
{
    auto send = [](const std::string& m) { Command c{ CmdType::SendRaw }; c.str = m; State().Push(c); };
    auto refresh = [&]() { send("SE|LVLIST"); g_levelsAsked = ImGui::GetTime(); };
    if (snap.inEditor && ImGui::GetTime() - g_levelsAsked > 20.0) refresh();

    ImGui::SeparatorText("Save");
    ImGui::TextWrapped("Saves everything built on this server -- including levels you loaded -- under this name: "
                       "objects, their Game data, quests, groups and coin runs.");
    ImGui::SetNextItemWidth(-110);
    ImGui::InputTextWithHint("##lvname", "Level name", g_levelName, sizeof(g_levelName),
                             ImGuiInputTextFlags_CallbackCharFilter, [](ImGuiInputTextCallbackData* d) {
                                 const ImWchar c = d->EventChar;
                                 return (c < 128 && (isalnum(c) || c == ' ' || c == '-' || c == '_')) ? 0 : 1;
                             });
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_levelName[0] || !snap.inEditor);
    ImGui::PushStyleColor(ImGuiCol_Button, kSelBlue);
    if (ImGui::Button("Save level", ImVec2(-1, 0))) { send(std::string("SE|LVSAVE|") + g_levelName); g_levelsAsked = ImGui::GetTime() - 17.0; }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Saved levels");
    if (snap.levels.empty()) ImGui::TextDisabled(snap.levelsSerial ? "No saved levels yet." : "Asking the server...");
    for (const auto& l : snap.levels)
    {
        ImGui::PushID(l.name.c_str());
        ImGui::AlignTextToFramePadding();
        if (l.here) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", l.name.c_str());
        else ImGui::TextUnformatted(l.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", l.here ? "loaded here" : "not loaded", l.autoload ? "  |  loads on boot" : "");
        ImGui::SameLine(ImGui::GetWindowWidth() - 130);
        if (ImGui::SmallButton("Use name")) strncpy_s(g_levelName, l.name.c_str(), _TRUNCATE);
        ImGui::SameLine();
        if (!l.here) { if (ImGui::SmallButton("Load")) { send("SE|LVLOAD|" + l.name); g_levelsAsked = ImGui::GetTime() - 5.0; } }
        else if (ImGui::SmallButton("Unload")) { send("SE|LVUNLOAD|" + l.name); g_levelsAsked = ImGui::GetTime() - 5.0; }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Refresh")) refresh();
    ImGui::Spacing();
    ImGui::TextDisabled("Autoload on server boot is set on the dashboard (station > Editor levels).");

    // Local projects: level files on this PC (Documents\RigelLevels\*.a2level). Export what is built here to
    // one; load one onto this server (as a level you can Unload), or upload it as a saved level.
    ImGui::SeparatorText("Local projects (.a2level)");
    {
        const std::string nm = g_levelName[0] ? std::string(g_levelName) : std::string("Untitled");
        ImGui::BeginDisabled(!snap.inEditor);
        if (ImGui::Button(("Export to file: " + nm + ".a2level").c_str(), ImVec2(-1, 0)))
        {
            Command c{ CmdType::LevelExport }; c.str = nm; State().Push(c);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everything built on this server, saved to Documents\\RigelLevels on this PC.\nUses the name typed above.");

        static std::vector<std::wstring> s_files;
        static double s_listed = -100.0;
        if (ImGui::GetTime() - s_listed > 3.0)
        {
            s_listed = ImGui::GetTime();
            s_files.clear();
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((LevelsDir() + L"\\*.a2level").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do { std::wstring f = fd.cFileName; s_files.push_back(f.substr(0, f.size() - 8)); } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (s_files.empty()) ImGui::TextDisabled("No level files yet. Export one, or drop .a2level files into Documents\\RigelLevels.");
        for (const auto& wf : s_files)
        {
            const std::string f(wf.begin(), wf.end());
            ImGui::PushID(f.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(f.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 150);
            auto readFile = [&]() -> std::string {
                std::string text;
                FILE* fp = nullptr;
                if (_wfopen_s(&fp, (LevelsDir() + L"\\" + wf + L".a2level").c_str(), L"rb") == 0 && fp)
                {
                    char buf[4096];
                    for (size_t r; (r = fread(buf, 1, sizeof(buf), fp)) > 0;) text.append(buf, r);
                    fclose(fp);
                }
                return text;
            };
            std::string clean;
            for (char ch : f) if (isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == '_' || ch == '-') clean += ch;
            ImGui::BeginDisabled(!snap.inEditor || clean.empty());
            if (ImGui::SmallButton("Load here"))
            {
                Command c{ CmdType::LevelImport }; c.str = clean; c.str2 = "load"; c.str3 = readFile(); State().Push(c);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Place this level on this server now (not saved to the server - Unload removes it).");
            ImGui::SameLine();
            if (ImGui::SmallButton("Upload"))
            {
                Command c{ CmdType::LevelImport }; c.str = clean; c.str2 = "save"; c.str3 = readFile(); State().Push(c);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save it to the server's levels (so it can autoload / be loaded anywhere) and load it here.");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Open folder")) ShellExecuteW(nullptr, L"open", LevelsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}



// ==== GAME MODES (the fabricator) ========================================================================
// A game mode is an area of its own on the server (its own teams, state and scores -- see specedit.h, "GAME MODE
// FABRICATOR"). This tab makes them, shows them live, gives the objects inside roles, and builds the mode's
// controller script from those roles plus the author's own code (RigelScripts\GameModes\<name>.luau).
struct GmInfo
{
    std::string id, name, state, level;
    double at[3]{};
    int nTeams = 0, round = 0, time = 0, winner = 0;
    std::string names[8];
    int maxes[8]{}, sizes[8]{}, scores[8]{};
    std::map<std::string, std::string> sets;
};
const double kGmHalf[3] = { 3881.0, 4733.0, 950.0 };     // a mode's area (the slot class's box), half-size in cm
std::vector<std::string> GmSplit(const std::string& s, char sep)
{
    std::vector<std::string> v;
    for (size_t b = 0; b <= s.size();) { size_t e = s.find(sep, b); if (e == std::string::npos) e = s.size(); v.push_back(s.substr(b, e - b)); b = e + 1; }
    return v;
}
GmInfo GmParse(const std::string& rec)
{
    GmInfo g;
    const auto f = GmSplit(rec, '~');
    if (f.size() < 14) return g;
    g.id = f[0]; g.name = f[1];
    sscanf_s(f[2].c_str(), "%lf,%lf,%lf", &g.at[0], &g.at[1], &g.at[2]);
    g.nTeams = (std::min)(8, atoi(f[3].c_str()));
    const auto nm = GmSplit(f[4], ','), mx = GmSplit(f[5], ','), sz = GmSplit(f[11], ','), sc = GmSplit(f[12], ',');
    for (int t = 0; t < g.nTeams; ++t)
    {
        g.names[t] = t < (int)nm.size() ? nm[t] : "";
        g.maxes[t] = t < (int)mx.size() ? atoi(mx[t].c_str()) : 0;
        g.sizes[t] = t < (int)sz.size() ? atoi(sz[t].c_str()) : 0;
        g.scores[t] = t < (int)sc.size() ? atoi(sc[t].c_str()) : 0;
    }
    for (const auto& kv : GmSplit(f[6], ';'))
    {
        const size_t e = kv.find('=');
        if (e != std::string::npos && e > 0) g.sets[kv.substr(0, e)] = kv.substr(e + 1);
    }
    g.state = f[7]; g.level = f[8]; g.round = atoi(f[9].c_str()); g.time = atoi(f[10].c_str()); g.winner = atoi(f[13].c_str());
    return g;
}
std::string GmSetting(const GmInfo& g, const std::string& k, const char* def)
{
    auto it = g.sets.find(k);
    return it == g.sets.end() ? std::string(def) : it->second;
}
bool GmInside(const GmInfo& g, const Vec3& p, double margin = 0.0)
{
    return fabs(p.x - g.at[0]) <= kGmHalf[0] + margin && fabs(p.y - g.at[1]) <= kGmHalf[1] + margin && fabs(p.z - g.at[2]) <= kGmHalf[2] + margin;
}
std::string GmGuid(const std::string& handle) { return handle.substr(0, handle.find('_')); }
// A number field that reports once the edit is finished: typing commits when the field is left (or Enter), the
// +/- buttons commit straight away. (ImGui's EnterReturnsTrue isn't allowed on number fields -- it asserted.)
bool GmIntCommit(const char* label, int* v, int stepFast)
{
    // *v is refreshed from the server every frame, so what the user is typing lives here until it's committed.
    static std::unordered_map<ImGuiID, int> s_edit;
    const ImGuiID id = ImGui::GetID(label);
    auto it = s_edit.find(id);
    int val = it != s_edit.end() ? it->second : *v;
    const bool changed = ImGui::InputInt(label, &val, 1, stepFast);
    const bool active = ImGui::IsItemActive();
    if (active || changed) s_edit[id] = val;
    if ((changed && !active) || ImGui::IsItemDeactivatedAfterEdit()) { *v = val; s_edit.erase(id); return true; }
    if (!active) s_edit.erase(id);
    return false;
}
void GmSend(const std::string& line) { Command c{ CmdType::SendRaw }; c.str = line; State().Push(c); }

// Roles: what an object does in the mode. Each becomes a typed slot in the generated script.
struct GmRole { const char* key; const char* label; const char* type; const char* help; bool param; const char* paramLabel; };
const GmRole kGmRoles[] = {
    { "", "(no role)", "", "Just part of the arena.", false, "" },
    { "start", "Start button", "BasicButtonComponent", "Pressing it starts a round (when no round is on).", false, "" },
    { "score", "Score button", "BasicButtonComponent", "Pressing it during a round gives the team points.", true, "team" },
    { "goal", "Goal (ball in = points)", "GoalComponent", "When the ball goes into this goal during a round, the team gets the goal's points (1, or more for long shots) and the ball goes back to its spawner. Team = the team that SCORES here.", true, "team" },
    { "score_zone", "Score box (ball passes through)", "PhysicalComponent", "The ball flies through it; each pass during a round gives the team a point and resets the ball. Team 0 = by side: crossing along the box's forward arrow scores for team 1, the other way for team 2.", true, "team" },
    { "start_ring", "Ball start ring", "PhysicalComponent", "Like the driftball arenas: between rounds the ball waits, carry it into the ring to start a round. The ring hides while a round is on.", false, "" },
    { "midfield", "Midfield trigger", "PhysicalComponent", "A trigger the ball crosses at midfield (e.g. BP_DiscTriggerC_C): the Jakeball flow skips the next-point countdown when the ball is carried over it. Slot Midfield1, Midfield2, ...", false, "" },
    { "celebrate", "Goal celebration (team N)", "ToggleableComponent", "A goal celebration (e.g. LE_BP_VFX_TackleBallGoal01a_C) your mode code switches on when team N scores: slot Celebrate1, Celebrate2, ... Off at the start.", true, "team" },
    { "trap_round", "Trap - on during rounds", "ToggleableComponent", "Switched on when a round starts, off when it ends.", false, "" },
    { "trap_pulse", "Trap - pulses during rounds", "ToggleableComponent", "Flips on/off every N seconds while a round is on.", true, "seconds" },
    { "trap_fired", "Trap - fired by trap buttons", "ToggleableComponent", "Switched on for a while when a trap button is pressed.", false, "" },
    { "trap_button", "Trap button", "BasicButtonComponent", "Fires every 'fired by trap buttons' trap for N seconds (during rounds).", true, "seconds" },
    { "wall_lobby", "Wall - between rounds only", "PhysicalComponent", "Solid and visible between rounds, gone while a round is on (keep players out / in).", false, "" },
    { "wall_round", "Wall - during rounds only", "PhysicalComponent", "Only there while a round is on.", false, "" },
    { "timer", "Round timer", "TimerComponent", "Counts the countdown, then the round (or up, with no round limit).", false, "" },
    { "ball", "Ball spawner", "BallSpawnerComponent", "The server spawns (or resets) its ball when a round starts. Rigel.resetBalls() does it any time.", false, "" },
    { "score_board", "Scoreboard (score + clock)", "ScoreComponent", "The classic Score board: shows team 1 / team 2 points.", false, "" },
    { "score_table", "Score table", "DataTableComponent", "A table: one row per team with its name, score, players and rounds won.", false, "" },
};
const GmRole* GmRoleOf(const std::string& key)
{
    for (const auto& r : kGmRoles) if (key == r.key) return &r;
    return &kGmRoles[0];
}
// The component a role needs, as the palette/object knows it: does this class plausibly have it?
bool GmRoleFits(const GmRole& r, const std::string& cls)
{
    const std::string t = r.type;
    if (t.empty()) return true;
    if (t == "BasicButtonComponent") return cls.find("Button") != std::string::npos;
    if (t == "ToggleableComponent") return cls.find("Trap") != std::string::npos || cls.find("Switch") != std::string::npos || cls.find("Sliding") != std::string::npos;
    if (t == "TimerComponent") return cls.find("Timer") != std::string::npos;
    if (t == "BallSpawnerComponent") return cls.find("BallSpawner") != std::string::npos;
    if (t == "ScoreComponent") return cls == "BP_Score_C";
    if (t == "DataTableComponent") return cls.find("TableScoreboard") != std::string::npos;
    if (t == "GoalComponent") return cls.find("Goal") != std::string::npos && cls.find("VFX") == std::string::npos;
    return true;   // PhysicalComponent: most placed pieces
}

bool GmIsToggleWall(const std::string& cls)   // force fields / shields: toggled, not shown / hidden
{
    return cls.find("ForceField") != std::string::npos || cls.find("Shield") != std::string::npos;
}
std::string GmSlotName(const std::string& roleKey, int n)
{
    std::string s;
    bool up = true;
    for (char c : roleKey) { if (c == '_') { up = true; continue; } s += up ? static_cast<char>(toupper(static_cast<unsigned char>(c))) : c; up = false; }
    return s + std::to_string(n);
}
std::string GmFileStem(const std::string& name)
{
    std::string o;
    for (char c : name) o += (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    return o;
}
std::wstring GmCodePath(const GmInfo& g)
{
    const std::wstring dir = ScriptsDir() + L"\\GameModes";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::string stem = GmFileStem(g.name);
    return dir + L"\\" + std::wstring(stem.begin(), stem.end()) + L".luau";
}
// The author's own code: made from a template the first time.
std::string GmUserCode(const GmInfo& g, bool create)
{
    const std::wstring path = GmCodePath(g);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && create)
        if (FILE* f = _wfopen(path.c_str(), L"wb"))
        {
            const std::string t =
                "-- Your code for the game mode \"" + g.name + "\". The Game Modes window puts it into the mode's controller\n"
                "-- script (with the roles you gave objects) whenever you click Apply -- or on every save if Auto-apply is on.\n"
                "-- Don't write BeginPlay here (the controller has it); use these hooks instead -- all optional:\n"
                "--   OnLobby()                          no round on (after a reset or a round's result)\n"
                "--   OnCountdown(seconds)               a round is about to start\n"
                "--   OnRoundStart(round)                a round started (round = 1, 2, ...)\n"
                "--   OnRoundEnd(winner)                 a round ended (winner = team number, 0 = draw)\n"
                "--   OnTeamChanged(team, size, oldSize) someone joined / left a team\n"
                "--   OnScore(team, score, oldScore)     a team's score changed\n"
                "--   OnTime(secondsLeft)                every second of a countdown or round\n"
                "-- Everything in Rigel.* works here: Rigel.addScore(1), Rigel.endRound(2), Rigel.teamSize(1), Rigel.setting(\"round_time\") ...\n"
                "-- (see the Game Modes guide). Objects with roles are available by their slot names, shown in the window.\n\n"
                "function OnRoundStart(round: number)\n\tlog(\"Round \" .. round .. \" started\")\nend\n\n"
                "function OnRoundEnd(winner: number)\n\tif winner > 0 then\n\t\tlog(Rigel.teamName(winner) .. \" won\")\n\telse\n\t\tlog(\"Draw\")\n\tend\nend\n";
            fwrite(t.data(), 1, t.size(), f);
            fclose(f);
        }
    return ReadFileUtf8(path);
}
struct GmSlot { std::string slot, type, targetHandle; };
// The object the mode's script runs on: its script host (an invisible Timer the server makes with the mode) when it
// is there, else the controller (modes from before script hosts). A script must never reference the object it sits
// on -- that self-reference crashed every client -- so the arena slots below need a host.
static const SceneObject* GmObjectBySetting(const Snapshot& snap, const GmInfo& g, const char* key)
{
    auto it = g.sets.find(key);
    if (it == g.sets.end() || it->second.empty()) return nullptr;
    for (const auto& o : snap.objects) if (o.handle.rfind(it->second, 0) == 0) return &o;
    return nullptr;
}
static const SceneObject* GmScriptObject(const Snapshot& snap, const GmInfo& g)
{
    if (const SceneObject* h = GmObjectBySetting(snap, g, "scripthost")) return h;
    return GmObjectBySetting(snap, g, "controller");
}
// An object's forward vector (X axis) from its rotation -- baked into the script for "by side" score boxes.
static void GmForward(const Rot& r, double* f)
{
    const double p = r.pitch * 3.14159265358979 / 180.0, y = r.yaw * 3.14159265358979 / 180.0;
    f[0] = cos(p) * cos(y); f[1] = cos(p) * sin(y); f[2] = sin(p);
}
// Build the controller script. Returns the source; fills the slots to wire.
std::string GmBuildScript(const Snapshot& snap, const GmInfo& g, std::vector<GmSlot>& slots)
{
    std::map<std::string, int> counters;
    std::vector<std::pair<std::string, std::string>> roleOf;   // slot -> role value
    std::string head, body;
    head += "-- [Rigel game mode] " + g.name + " -- generated by the Game Modes window. Don't edit this one: give objects roles in\n"
            "-- the window, and write your own code in RigelScripts\\GameModes\\" + GmFileStem(g.name) + ".luau (included below).\n";
    const SceneObject* scriptObj = GmScriptObject(snap, g);
    for (const auto& o : snap.objects)
    {
        if (!GmInside(g, o.location)) continue;
        if (scriptObj && o.handle == scriptObj->handle) continue;      // no self-references (see GmScriptObject)
        auto it = g.sets.find("role." + GmGuid(o.handle));
        if (it == g.sets.end() || it->second.empty()) continue;
        const std::string key = it->second.substr(0, it->second.find(':'));
        const GmRole* r = GmRoleOf(key);
        if (!r->type[0]) continue;
        // a celebration's slot is named by its team (Celebrate1 = team 1), so the mode's code knows which is which
        const size_t colon = it->second.find(':');
        const std::string slot = key == "celebrate" && colon != std::string::npos
            ? "Celebrate" + std::to_string((std::max)(1, atoi(it->second.c_str() + colon + 1)))
            : GmSlotName(key, ++counters[key]);
        // A wall is whatever the object is: force fields / shields only have a ToggleableComponent (on = the wall
        // is up), other pieces are shown / hidden through their PhysicalComponent.
        std::string type = r->type;
        if (key.rfind("wall_", 0) == 0 && GmIsToggleWall(o.className)) type = "ToggleableComponent";
        head += "local " + slot + ": " + type + " = nil\n";
        slots.push_back({ slot, type, o.handle });
        if (key == "score_zone")
        {
            double f[3];
            GmForward(o.rotation, f);
            char b[160];
            snprintf(b, sizeof(b), "local %s_fwd = { %.4f, %.4f, %.4f }\n", slot.c_str(), f[0], f[1], f[2]);
            head += b;
        }
        // a goal's scoring team, for the mode's own code (the game's goalInfo.scoringTeam doesn't tell our goals apart)
        if (key == "goal")
            head += "local " + slot + "_team = " + std::to_string((std::max)(1, atoi(it->second.c_str() + (colon == std::string::npos ? it->second.size() : colon + 1)))) + "\n";
        roleOf.push_back({ slot, it->second + (type == "ToggleableComponent" && key.rfind("wall_", 0) == 0 ? "|toggle" : "") });
    }
    // The controller is a Game State Manager: its GameTimeComponent is the arena clock the scoreboard monitors show,
    // its GameStateManagerComponent's state drives the monitors' screens and the announcer, its ScoreComponent the
    // score -- exactly what the station's own arena script drives. Wire all three.
    bool arena = false;
    // script_flow = 1: the mode's own code runs the arena (clock, arena state, ring, goals, the ball) -- e.g. the
    // Jakeball flow; the generated wiring then only declares the slots, keeps the boards and calls the hooks
    const bool flow = g.sets.count("script_flow") && g.sets.at("script_flow") == "1";
    const SceneObject* ctlObj = GmObjectBySetting(snap, g, "controller");
    if (ctlObj && scriptObj && scriptObj != ctlObj)
        for (const auto& o : snap.objects)
            if (o.handle == ctlObj->handle)
            {
                head += "local ModeTimer: GameTimeComponent = nil\nlocal ModeState: GameStateManagerComponent = nil\nlocal ModeScore: ScoreComponent = nil\n";
                slots.push_back({ "ModeTimer", "GameTimeComponent", o.handle });
                slots.push_back({ "ModeState", "GameStateManagerComponent", o.handle });
                slots.push_back({ "ModeScore", "ScoreComponent", o.handle });
                arena = true;
                break;
            }
    std::string user = GmUserCode(g, false);
    body += "\n-- ==== your code (GameModes\\" + GmFileStem(g.name) + ".luau) ====\n" + user + "\n-- ==== generated wiring ====\n";
    body += "local function __call(name: string, ...) local f = _G[name] or getfenv()[name]; if type(f) == \"function\" then local ok, err = pcall(f, ...); if not ok then warn(\"[RigelError] \" .. name .. \": \" .. tostring(err)) end end end\n";
    body += "local __trapFiredUntil = 0\n";
    {
        // Is this disc the mode's ball? (any ball when the mode has no "Ball spawner" role)
        std::string balls;
        for (const auto& sr : roleOf) if (sr.second.rfind("ball", 0) == 0) balls += (balls.empty() ? "" : ", ") + sr.first;
        body += "local function __isModeBall(disc: any): boolean\n";
        if (balls.empty()) body += "\treturn disc ~= nil\n";
        else body += "\tfor _, sp in { " + balls + " } do local ok, b = pcall(function() return sp:getSpawnedBall() end); if ok and b == disc then return true end end\n\treturn false\n";
        body += "end\n";
    }
    // Scoreboards: the monitor boards show the game mode's own team scores / rounds won (Gamemode:setTeamScore,
    // as the stock TKB mode does); the classic Score board and the score table get theirs through their slots.
    // (no function-type annotation on f: typed function locals are what the game's property system can't build)
    body += "local function __arena(what: string, f)\n"
            "\tlocal ok, err = pcall(f)\n"
            "\tif ok then log(\"[RigelArena] \" .. what .. \" ok\") else warn(\"[RigelError] arena \" .. what .. \": \" .. tostring(err)) end\n"
            "end\n";
    body += "local function __boards()\n\tfor t = 1, Rigel.teams() do\n"
            "\t\tpcall(function() Gamemode:setTeamScore(t - 1, Rigel.score(t)) end)\n"
            "\t\tpcall(function() Gamemode:setTeamRoundsWon(t - 1, Rigel.roundsWon(t)) end)\n";
    if (arena) body += "\t\t__arena(\"points \" .. t, function() ModeScore:setTeamPoints(t - 1, Rigel.score(t)) end)\n";
    for (const auto& sr : roleOf)
        if (sr.second == "score_board") body += "\t\tpcall(function() " + sr.first + ":setTeamPoints(t - 1, Rigel.score(t)) end)\n";
    body += "\tend\n";
    for (const auto& sr : roleOf)
        if (sr.second == "score_table")
        {
            const std::string& s = sr.first;
            body += "\tpcall(function()\n\t\t" + s + ":beginDataTransaction()\n"
                    "\t\tfor c, h in { \"Team\", \"Score\", \"Players\", \"Wins\" } do " + s + ":setCellText(h, 0, c - 1) end\n"
                    "\t\tfor t = 1, Rigel.teams() do\n"
                    "\t\t\t" + s + ":setCellText(Rigel.teamName(t), t, 0)\n"
                    "\t\t\t" + s + ":setCellText(tostring(Rigel.score(t)), t, 1)\n"
                    "\t\t\t" + s + ":setCellText(Rigel.teamSize(t) .. \"/\" .. Rigel.teamMax(t), t, 2)\n"
                    "\t\t\t" + s + ":setCellText(tostring(Rigel.roundsWon(t)), t, 3)\n"
                    "\t\tend\n\t\t" + s + ":endDataTransaction()\n\tend)\n";
        }
    body += "end\n";
    body += "function BeginPlay()\n";
    body += "\tlocal __lastState = \"\"\n";
    body += "\tlocal function onState(st: string)\n";
    if (arena && !flow)
        body += "\t\t-- the arena's own game state (monitors + announcer) and clock, as the station's arenas do\n"
                "\t\tlocal rt = Rigel.settingNumber(\"round_time\") * 1000\n"
                "\t\tif st == \"idle\" then\n"
                "\t\t\t__arena(\"state 1\", function() ModeState:updateGameState(1) end)\n"
                "\t\t\t__arena(\"timer lobby\", function() ModeTimer:luaSetGameBegun(false); ModeTimer:resetTimer(); ModeTimer:setRoundTime(rt) end)\n"
                "\t\t\t__arena(\"score reset\", function() ModeScore:resetScore() end)\n"
                "\t\telseif st == \"countdown\" then\n"
                "\t\t\t__arena(\"state 4\", function() ModeState:updateGameState(4) end)\n"
                // The countdown length comes from the SETTING: Rigel.timeLeft() can still be 0 the moment the state flips,
                // and a 0 ms countdown leaves the clock "counting down" forever (the monitor then shows its countdown
                // page all round and never ticks). Always startTimerWithCountdown, as the stock arenas do: plain
                // startTimer leaves the same stuck countdown. >= 1 ms = no countdown.
                "\t\t\t__arena(\"timer countdown\", function() ModeTimer:resetTimer(); ModeTimer:setRoundTime(rt); ModeTimer:startTimerWithCountdown(math.max(1, Rigel.settingNumber(\"countdown\") * 1000)) end)\n"
                "\t\telseif st == \"running\" then\n"
                "\t\t\t__arena(\"state 5\", function() ModeState:updateGameState(5) end)\n"
                "\t\t\t__arena(\"game begun\", function() ModeTimer:luaSetGameBegun(true) end)\n"
                "\t\t\tif __lastState ~= \"countdown\" then __arena(\"timer start\", function() ModeTimer:resetTimer(); ModeTimer:setRoundTime(rt); ModeTimer:startTimerWithCountdown(1) end) end\n"
                "\t\telseif st == \"ended\" then\n"
                "\t\t\t__arena(\"state 10\", function() ModeState:updateGameState(10) end)\n"
                "\t\t\t__arena(\"timer pause\", function() ModeTimer:pauseTimer(); ModeTimer:luaSetGameBegun(false) end)\n"
                "\t\tend\n"
                "\t\t__lastState = st\n";
    // state-driven roles
    for (const auto& sr : roleOf)
    {
        const std::string key = sr.second.substr(0, sr.second.find_first_of(":|"));
        const std::string& s = sr.first;
        if (key == "trap_round") body += "\t\tpcall(function() if st == \"running\" then " + s + ":luaEnable() else " + s + ":luaDisable() end end)\n";
        else if (key == "trap_pulse" || key == "trap_fired") body += "\t\tif st ~= \"running\" then pcall(function() " + s + ":luaDisable() end) end\n";
        else if (key == "wall_lobby" || key == "wall_round")
        {
            const bool toggle = sr.second.find("|toggle") != std::string::npos;
            const std::string up = key == "wall_lobby" ? "st ~= \"running\"" : "st == \"running\"";   // when the wall stands
            if (toggle) body += "\t\tpcall(function() if " + up + " then " + s + ":luaEnable() else " + s + ":luaDisable() end end)\n";
            else body += "\t\tpcall(function() if " + up + " then " + s + ":showLua(); " + s + ":setDefaultCollision() else " + s + ":hideLua(); " + s + ":disableCollisionLua() end end)\n";
        }
        else if (key == "start_ring" && !flow)
            body += "\t\tpcall(function() if st == \"idle\" or st == \"ended\" then " + s + ":showLua(); " + s + ":setTriggerCollision() else " + s + ":hideLua(); " + s + ":disableCollisionLua() end end)\n";
        else if (key == "timer" && !flow) body += "\t\tpcall(function()\n\t\t\tif st == \"countdown\" then " + s + ":start(math.max(1, Rigel.settingNumber(\"countdown\")))\n\t\t\telseif st == \"running\" then if Rigel.settingNumber(\"round_time\") > 0 then " + s + ":start(Rigel.settingNumber(\"round_time\")) else " + s + ":startCountUpFromZero() end\n\t\t\telse " + s + ":stopAndResetTimer() end\n\t\tend)\n";
        // "ball": the server spawns / resets the ball itself when a round starts (a Luau spawnBall on every machine made
        // client-only balls). The slot stays available to your code: Ball1:getSpawnedBall(), Rigel.resetBalls().
    }
    body += "\tend\n";
    body += "\tonState(Rigel.state())\n\t__boards()\n";
    body += "\tRigel.onStateChanged(function(st: string, old: string)\n\t\tonState(st)\n\t\t__boards()\n";
    if (!flow)
        body += "\t\tif st == \"running\" and old ~= \"running\" then pcall(function() Gamemode:startGame() end)\n"
            "\t\telseif (st == \"ended\" or st == \"idle\") and (old == \"running\" or old == \"countdown\") then pcall(function() Gamemode:stopGame() end) end\n";
    body += ""
            "\t\tif st == \"idle\" then __call(\"OnLobby\")\n"
            "\t\telseif st == \"countdown\" then __call(\"OnCountdown\", math.max(Rigel.timeLeft(), Rigel.settingNumber(\"countdown\")))\n"
            "\t\telseif st == \"running\" then __call(\"OnRoundStart\", Rigel.round())\n"
            "\t\telseif st == \"ended\" then __call(\"OnRoundEnd\", Rigel.winner()) end\n\tend)\n";
    body += "\tRigel.onTeamChanged(function(team: number, size: number, old: number) __boards(); __call(\"OnTeamChanged\", team, size, old) end)\n";
    body += "\tRigel.onScoreChanged(function(team: number, score: number, old: number) __boards(); __call(\"OnScore\", team, score, old) end)\n";
    body += "\tRigel.onTimeChanged(function(t: number) __call(\"OnTime\", t) end)\n";
    // event-driven roles
    std::string firedList;
    for (const auto& sr : roleOf) if (sr.second.rfind("trap_fired", 0) == 0) firedList += (firedList.empty() ? "" : ", ") + sr.first;
    for (const auto& sr : roleOf)
    {
        const std::string key = sr.second.substr(0, sr.second.find_first_of(":|"));
        const std::string param = sr.second.find(':') == std::string::npos ? std::string() : sr.second.substr(sr.second.find(':') + 1);
        const std::string& s = sr.first;
        if (key == "start")
            body += "\tif " + s + " then " + s + ".OnButtonPressEvent.Listen(function() local st = Rigel.state(); if st == \"idle\" or st == \"ended\" then Rigel.startRound() end end) end\n";
        else if (key == "score")
            body += "\tif " + s + " then " + s + ".OnButtonPressEvent.Listen(function() if Rigel.isRunning() then Rigel.addScore(" + std::to_string((std::max)(1, atoi(param.c_str()))) + ", 1) end end) end\n";
        else if (key == "goal")
        {
            // The goal's own team is the side DEFENDING it (the game scores a ball in team N's goal for the other side):
            // with two teams that is the other team, otherwise the scoring team's colour.
            const std::string team = std::to_string((std::max)(1, atoi(param.c_str())));
            body += "\tif " + s + " then\n"
                    "\t\tlocal owner = Rigel.teams() == 2 and (" + team + " == 1 and 1 or 0) or (" + team + " - 1)\n"
                    "\t\tpcall(function() " + s + ":updateCppTeam(owner) end)\n"
                    "\t\tpcall(function() " + s + ":setColorByIndex(owner) end)\n"
                    "\t\tpcall(function() " + s + ":enableGoal(true) end)\n";
            if (!flow)
                body += "\t\t" + s + ".onGoalScored.Listen(function(info: any)\n"
                    "\t\t\tif not Rigel.isRunning() then return end\n"
                    "\t\t\tlocal pts = 1\n"
                    "\t\t\tpcall(function() if info and info.goalPoints and info.goalPoints > 0 then pts = info.goalPoints end end)\n"
                    "\t\t\tRigel.goal(" + team + ", pts, \"" + s + "\")\n"
                    "\t\tend)\n";
            body += "\tend\n";
        }
        else if (key == "score_zone")
        {
            const int team = (std::max)(0, atoi(param.c_str()));
            body += "\tif " + s + " then " + s + ".onOverlapByDisc.Listen(function(disc: any)\n"
                    "\t\tif not Rigel.isRunning() or not __isModeBall(disc) then return end\n"
                    "\t\tlocal team = " + std::to_string(team) + "\n"
                    "\t\tif team == 0 then\n"
                    "\t\t\tlocal v = disc:getDiscVelocity()\n"
                    "\t\t\tteam = (v.x * " + s + "_fwd[1] + v.y * " + s + "_fwd[2] + v.z * " + s + "_fwd[3]) >= 0 and 1 or 2\n"
                    "\t\tend\n"
                    "\t\tRigel.goal(team, 1, \"" + s + "\")\n"
                    "\tend) end\n";
        }
        else if (key == "start_ring" && !flow)
            body += "\tif " + s + " then " + s + ".onOverlapByDisc.Listen(function(disc: any)\n"
                    "\t\tlocal st = Rigel.state()\n"
                    "\t\t-- the server ignores the first 3 s of a lobby (the ball being put back is not a player carrying it in)\n"
                    "\t\tif (st == \"idle\" or st == \"ended\") and __isModeBall(disc) then Rigel.ringStart() end\n"
                    "\tend) end\n";
        else if (key == "trap_pulse")
        {
            const double sec = (std::max)(0.5, atof(param.c_str()) > 0 ? atof(param.c_str()) : 3.0);
            char b[32]; snprintf(b, sizeof(b), "%.2f", sec);
            body += "\tdo local on = false; LuauClock.createTimer(" + std::string(b) + ", function() if Rigel.isRunning() then on = not on; pcall(function() if on then " + s + ":luaEnable() else " + s + ":luaDisable() end end) end end) end\n";
        }
        else if (key == "trap_button" && !firedList.empty())
        {
            const double sec = atof(param.c_str()) > 0 ? atof(param.c_str()) : 5.0;
            char b[32]; snprintf(b, sizeof(b), "%.2f", sec);
            body += "\tif " + s + " then " + s + ".OnButtonPressEvent.Listen(function()\n\t\tif not Rigel.isRunning() or LuauClock.getTime() < __trapFiredUntil then return end\n"
                    "\t\t__trapFiredUntil = LuauClock.getTime() + " + std::string(b) + "\n"
                    "\t\tfor _, t in { " + firedList + " } do pcall(function() t:luaEnable() end) end\n"
                    "\t\tLuauClock.timeout(" + std::string(b) + "):andThen(function() for _, t in { " + firedList + " } do pcall(function() t:luaDisable() end) end end)\n\tend) end\n";
        }
    }
    body += "end\n";
    return head + body;
}

std::string g_gmSel;                     // selected mode id
char g_gmNewName[40] = "My Game";
int  g_gmNewTeams = 2;
int  g_gmNewMax[4] = { 4, 4, 4, 4 };
char g_gmNewNames[4][24] = { "Blue", "Red", "Green", "Yellow" };
bool g_gmAutoApply = true;
std::map<std::string, FILETIME> g_gmCodeWritten;   // per mode id: the custom code file's time when last applied
std::map<std::string, std::vector<std::string>> g_gmWired;   // per mode id: the actors its script's slots point at
std::string g_gmPreview;                 // "Show generated script"

std::set<std::string> g_gmCtlCleaned;   // modes whose controller had its old script copy taken off (this session)
void GmApply(const Snapshot& snap, const GmInfo& g)
{
    const SceneObject* ctl = GmScriptObject(snap, g);
    if (!ctl) { Notes().Set("The game mode's controller (its Game State Manager) isn't here any more - make the mode again."); return; }
    // Modes from before script hosts ran the script on the controller: take that copy off once the host has it.
    if (const SceneObject* oldCtl = GmObjectBySetting(snap, g, "controller"))
        if (oldCtl != ctl && g_gmCtlCleaned.insert(g.id).second)
        {
            Command c{ CmdType::LuauRemove }; c.str = oldCtl->handle; c.str2 = "GM_" + GmFileStem(g.name) + ".luau"; State().Push(c);
        }
    std::vector<GmSlot> slots;
    GmUserCode(g, true);
    const std::string src = GmBuildScript(snap, g, slots);
    const std::string script = "GM_" + GmFileStem(g.name) + ".luau";
    { Command c{ CmdType::LuauAttach }; c.str = ctl->handle; c.str2 = script; c.str3 = src; State().Push(c); }
    for (const auto& sl : slots) { Command c{ CmdType::LuauRef }; c.str = ctl->handle; c.str2 = script; c.str3 = sl.slot; c.str4 = sl.type; c.str5 = sl.targetHandle; State().Push(c); }
    auto& wired = g_gmWired[g.id];
    wired.clear();
    for (const auto& sl : slots) wired.push_back(sl.targetHandle);
    Log("[gamemode] applied %s: %zu slot(s), %zu byte(s)", script.c_str(), slots.size(), src.size());
    Notes().Set("Applied the script for '" + g.name + "' (" + std::to_string(slots.size()) + " object(s) wired).");
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(GmCodePath(g).c_str(), GetFileExInfoStandard, &fa)) g_gmCodeWritten[g.id] = fa.ftLastWriteTime;
}

// A piece to place, with its icon: class in the palette, and a role it gets straight away.
struct GmPiece { const char* cls; const char* label; const char* role; };
const GmPiece kGmPieces[] = {
    { "BP_BasicButton_C", "Start button", "start" },
    { "BP_BasicButton_C", "Score button", "score:1" },
    { "BP_BasicButton_C", "Trap button", "trap_button:5" },
    { "BP_Trap_Spinner_C", "Spinner trap", "trap_round" },
    { "BP_Trap_TrapDoor_C", "Trap door", "trap_fired" },
    { "BP_Trap_Laser_C", "Laser trap", "trap_pulse:3" },
    { "BP_Trap_Fan_C", "Fan trap", "trap_round" },
    { "BP_Trap_Flipper_C", "Flipper trap", "trap_fired" },
    { "BP_Trap_FallingBlock_C", "Falling block", "trap_fired" },
    { "BP_Trap_BunnyHop_C", "Bunny hop", "trap_round" },
    { "BP_Trap_StickySlime_C", "Sticky slime", "trap_round" },
    { "BP_Trap_RotatingPlatform_C", "Rotating platform", "trap_round" },
    { "BP_SlidingPlatform_C", "Sliding platform", "trap_round" },
    { "BP_ForceFieldA_C", "Force field", "wall_lobby" },
    { "BP_Timer_C", "Round timer", "timer" },
    { "BP_TimerDisplay_C", "Timer display", "" },
    { "BP_ScoreboardA_C", "Scoreboard monitor", "" },
    { "LE_BP_ScoreboardA_Sideboard_C", "Side scoreboard", "" },
    { "BP_Score_C", "Score board", "score_board" },
    { "LE_BP_TableScoreboard_C", "Score table", "score_table" },
    { "LE_BP_Text_C", "Text sign", "" },
    { "BP_BallSpawner_C", "Ball spawner", "ball" },
    { "BP_JakeBallSpawner_C", "Jake ball spawner", "ball" },
    { "BP_GoalJakeBall_C", "Driftball goal", "goal:1" },
    { "BP_Goal_C", "Goal", "goal:1" },
    { "BP_DiscTriggerC_C", "Score box", "score_zone:0" },
    { "Prefab_BP_CylinderPrimitive_Trigger_C", "Ball start ring", "start_ring" },
    { "BP_DeathrunResetTeleporter_C", "Reset teleporter", "" },
};
struct GmPendingRole { std::string mode, role, cls; std::vector<std::string> before; double at = 0; };
std::vector<GmPendingRole> g_gmPendingRoles;   // placed from the piece list: give the role once it shows up

void GmAdoptPending(const Snapshot& snap)
{
    for (size_t i = 0; i < g_gmPendingRoles.size();)
    {
        auto& p = g_gmPendingRoles[i];
        bool done = false;
        for (const auto& o : snap.objects)
            if (o.className == p.cls && std::find(p.before.begin(), p.before.end(), o.handle) == p.before.end())
            {
                if (!p.role.empty()) GmSend("SE|GMSET|" + p.mode + "|role." + GmGuid(o.handle) + "|" + p.role);
                done = true;
                break;
            }
        if (done || ImGui::GetTime() - p.at > 15.0) g_gmPendingRoles.erase(g_gmPendingRoles.begin() + i); else ++i;
    }
}

// Moving / resizing a ball, attaching a script etc. rebuilds the object as a new actor under the same id, and the
// mode script's slot still points at the destroyed one (a rebuilt ball spawner: Ball1:getSpawnedBall() is nil, so the
// start ring and the goals stop knowing the mode's ball). Wire the script again once the rebuilt actor is there.
void GmRewireRebuilt(const Snapshot& snap)
{
    static double s_last = 0;
    static std::map<std::string, int> s_seen;            // mode id -> consecutive checks with a rebuilt target
    static std::map<std::string, double> s_at;           // mode id -> last rewire (never more than once per 10 s)
    if (ImGui::GetTime() - s_last < 1.0) return;
    s_last = ImGui::GetTime();
    for (const auto& rec : snap.gameModes)
    {
        const GmInfo g = GmParse(rec);
        if (g.id.empty() || !GmScriptObject(snap, g)) continue;
        // what the mode's slots point at NOW (by id), against what its script was wired to
        std::vector<GmSlot> sl;
        GmBuildScript(snap, g, sl);
        auto& wired = g_gmWired[g.id];
        const size_t known = wired.size();
        bool rebuilt = false;
        for (const auto& x : sl)
        {
            auto w = std::find_if(wired.begin(), wired.end(), [&](const std::string& h) { return GmGuid(h) == GmGuid(x.targetHandle); });
            if (w == wired.end()) wired.push_back(x.targetHandle);      // not seen yet (still loading, or applied elsewhere)
            else if (*w != x.targetHandle) rebuilt = true;
        }
        if (wired.size() != known) Log("[gamemode] %s: watching %zu wired object(s) for rebuilds", g.name.c_str(), wired.size());
        if (!rebuilt) { s_seen.erase(g.id); continue; }
        if (++s_seen[g.id] < 2) continue;                // let the rebuild settle for a second
        if (ImGui::GetTime() - s_at[g.id] < 10.0) continue;
        s_seen.erase(g.id);
        s_at[g.id] = ImGui::GetTime();
        Log("[gamemode] %s: a wired object was rebuilt -- wiring the script again", g.name.c_str());
        GmApply(snap, g);                                // (records the new handles in g_gmWired)
    }
}

void DrawGameModeBoxes(const Snapshot& snap)
{
    GmAdoptPending(snap);
    GmRewireRebuilt(snap);
    if (snap.gameModes.empty()) return;
    const View v = MakeView(snap);
    if (!v.valid) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const auto& rec : snap.gameModes)
    {
        const GmInfo g = GmParse(rec);
        if (g.id.empty()) continue;
        const bool sel = g.id == g_gmSel;
        const ImU32 col = g.state == "running" ? IM_COL32(80, 220, 120, sel ? 230 : 140) : IM_COL32(240, 160, 40, sel ? 230 : 120);
        DrawBox(dl, v, Vec3{ g.at[0], g.at[1], g.at[2] }, Vec3{ kGmHalf[0], kGmHalf[1], kGmHalf[2] }, col, sel ? 2.0f : 1.2f);
        ImVec2 sp;
        if (W2S(v, Vec3{ g.at[0], g.at[1], g.at[2] + kGmHalf[2] }, sp))
        {
            const std::string label = g.name + "  [" + g.state + "]";
            dl->AddText(ImVec2(sp.x - ImGui::CalcTextSize(label.c_str()).x * 0.5f, sp.y - 16), col, label.c_str());
        }
    }
}

void DrawGameModeDetail(const Snapshot& snap, const GmInfo& g);   // below

// The game modes UI. big = the Game Modes window: list on the left, the chosen mode on the right.
void DrawGameModes(const Snapshot& snap, bool big = false)
{
    std::vector<GmInfo> modes;
    for (const auto& r : snap.gameModes) { GmInfo g = GmParse(r); if (!g.id.empty()) modes.push_back(g); }
    // ---- list + new
    ImGui::TextUnformatted("Game modes");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150);
    if (ImGui::SmallButton("Refresh")) GmSend("SE|GMLIST");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ New game mode") || TourOnce("popup:gm_new")) ImGui::OpenPopup("New game mode");
    if (ImGui::BeginPopup("New game mode"))
    {
        ImGui::TextDisabled("A game mode is an area (about 78 x 95 x 19 m) with its own teams,\nrounds and scores. It is made around the point in front of the camera.");
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("Name", g_gmNewName, sizeof(g_gmNewName));
        ImGui::SetNextItemWidth(220);
        ImGui::SliderInt("Teams", &g_gmNewTeams, 1, 4);
        for (int t = 0; t < g_gmNewTeams; ++t)
        {
            ImGui::PushID(t);
            ImGui::SetNextItemWidth(120);
            ImGui::InputText("##tn", g_gmNewNames[t], sizeof(g_gmNewNames[t]));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("players max", &g_gmNewMax[t]);
            g_gmNewMax[t] = (std::max)(1, (std::min)(64, g_gmNewMax[t]));
            ImGui::PopID();
        }
        if (ImGui::Button("Create", ImVec2(120, 0)) && CanPlace())
        {
            const Vec3 at = InFront(snap, 3000.0);
            std::string maxes, names;
            for (int t = 0; t < g_gmNewTeams; ++t) { maxes += (t ? "," : "") + std::to_string(g_gmNewMax[t]); names += (t ? "," : "") + std::string(g_gmNewNames[t]); }
            char b[512];
            snprintf(b, sizeof(b), "SE|GMNEW|%s|%.0f,%.0f,%.0f|%d|%s|%s", g_gmNewName, at.x, at.y, at.z, g_gmNewTeams, maxes.c_str(), names.c_str());
            GmSend(b);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (modes.empty())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("Make team games: teams with team changers, rounds with a countdown and a time limit, scores, "
                           "traps that switch on during a round, walls that open, start and score buttons -- and your own Luau.");
        ImGui::TextDisabled("Click  + New game mode  (a level must be open).");
        return;
    }
    if (TourOnce("gm:select") && !modes.empty()) g_gmSel = modes.front().id;
    if (!big)
    {
        if (ImGui::Button("Open the Game Modes window", ImVec2(-1, 0))) g_gmWindow = true;
        if (ImGui::BeginListBox("##gms", ImVec2(-1, (std::min)(4, (int)modes.size()) * ImGui::GetTextLineHeightWithSpacing() + 6)))
        {
            for (const auto& g : modes)
            {
                std::string players;
                for (int t = 0; t < g.nTeams; ++t) players += (t ? " / " : "") + std::to_string(g.sizes[t]);
                const std::string label = g.name + "   " + g.state + "   players " + players + "##" + g.id;
                if (ImGui::Selectable(label.c_str(), g_gmSel == g.id)) g_gmSel = g.id;
            }
            ImGui::EndListBox();
        }
        const GmInfo* gp = nullptr;
        for (const auto& g : modes) if (g.id == g_gmSel) gp = &g;
        if (!gp) { ImGui::TextDisabled("Pick a game mode to edit it."); return; }
        DrawGameModeDetail(snap, *gp);
        return;
    }
    // ---- the window: modes on the left, the chosen one on the right
    ImGui::BeginChild("##gmleft", ImVec2(260, 0), ImGuiChildFlags_Borders);
    for (const auto& g : modes)
    {
        ImGui::PushID(g.id.c_str());
        const ImVec4 stc = g.state == "running" ? ImVec4(0.35f, 0.9f, 0.45f, 1) : g.state == "countdown" ? ImVec4(0.95f, 0.8f, 0.3f, 1)
                         : g.state == "ended" ? ImVec4(0.9f, 0.5f, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", g_gmSel == g.id, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2 + 6))) g_gmSel = g.id;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(at.x + 6, at.y + 2), IM_COL32(235, 235, 235, 255), g.name.c_str());
        std::string sub = g.state;
        for (int t = 0; t < g.nTeams; ++t) sub += (t ? " / " : "   ") + g.names[t] + " " + std::to_string(g.sizes[t]) + "p " + std::to_string(g.scores[t]);
        dl->AddText(ImVec2(at.x + 6, at.y + 4 + ImGui::GetTextLineHeight()), ImGui::GetColorU32(stc), sub.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##gmright", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (g_gmScrollReq >= 0.0f) { ImGui::SetScrollY(g_gmScrollReq); g_gmScrollReq = -1.0f; }
    const GmInfo* gp = nullptr;
    for (const auto& g : modes) if (g.id == g_gmSel) gp = &g;
    if (gp)
    {
        ImGui::PushFont(nullptr);
        ImGui::TextUnformatted(gp->name.c_str());
        ImGui::PopFont();
        ImGui::Separator();
        DrawGameModeDetail(snap, *gp);
    }
    else ImGui::TextDisabled("Pick a game mode on the left.");
    ImGui::EndChild();
}

void DrawGameModeDetail(const Snapshot& snap, const GmInfo& g)
{
    const std::string pre = "SE|GMSET|" + g.id + "|";

    // ---- status + controls
    const ImVec4 stc = g.state == "running" ? ImVec4(0.35f, 0.9f, 0.45f, 1) : g.state == "countdown" ? ImVec4(0.95f, 0.8f, 0.3f, 1)
                     : g.state == "ended" ? ImVec4(0.9f, 0.5f, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);
    ImGui::TextColored(stc, "%s", g.state.c_str());
    ImGui::SameLine();
    if (g.state == "countdown" || g.state == "running" || g.state == "ended") ImGui::Text("  %d s   round %d", g.time, g.round);
    else ImGui::Text("  round %d", g.round);
    if (g.state == "ended") { ImGui::SameLine(); ImGui::Text("   winner: %s", g.winner > 0 ? g.names[g.winner - 1].c_str() : "draw"); }
    for (int t = 0; t < g.nTeams; ++t)
    {
        if (t) ImGui::SameLine();
        ImGui::Text("%s %d/%d  (%d pts)%s", g.names[t].c_str(), g.sizes[t], g.maxes[t], g.scores[t], t + 1 < g.nTeams ? "  |" : "");
    }
    if (ImGui::Button("Start round")) GmSend("SE|GMCTL|" + g.id + "|start");
    ImGui::SameLine();
    if (ImGui::Button("End round")) GmSend("SE|GMCTL|" + g.id + "|end");
    ImGui::SameLine();
    if (ImGui::Button("Reset")) GmSend("SE|GMCTL|" + g.id + "|reset");
    ImGui::SameLine();
    if (ImGui::Button("Go to"))
    {
        Command c{ CmdType::FocusCamera }; c.loc = { g.at[0], g.at[1], g.at[2] }; State().Push(c);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete...") || TourOnce("popup:gm_delete")) ImGui::OpenPopup("##gmdel");
    if (ImGui::BeginPopup("##gmdel"))
    {
        ImGui::Text("Delete '%s' and every object in it?", g.name.c_str());
        if (ImGui::Button("Delete")) { GmSend("SE|GMDEL|" + g.id); g_gmSel.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_gmSel.empty()) return;

    // ---- teams
    if (ImGui::CollapsingHeader("Teams", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int t = 0; t < g.nTeams; ++t)
        {
            ImGui::PushID(t);
            char nb[32]; strncpy_s(nb, g.names[t].c_str(), _TRUNCATE);
            ImGui::SetNextItemWidth(110);
            if (ImGui::InputText("##n", nb, sizeof(nb), ImGuiInputTextFlags_EnterReturnsTrue)) GmSend(pre + "team" + std::to_string(t + 1) + ".name|" + nb);
            ImGui::SameLine();
            int mx = g.maxes[t];
            ImGui::SetNextItemWidth(80);
            if (GmIntCommit("max##m", &mx, 1)) GmSend(pre + "team" + std::to_string(t + 1) + ".max|" + std::to_string(mx));
            ImGui::SameLine();
            const ImVec2 ip = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(18, 18));
            IconFor(ImGui::GetWindowDrawList(), ip, 18, "BP_TeamChangeActor_C");
            ImGui::SameLine();
            if (ImGui::SmallButton("Place team changer") && CanPlace())
            {
                const Vec3 at = InFront(snap, 300.0);
                if (!GmInside(g, at)) Notes().Set("Look at a spot inside '" + g.name + "' (the orange box) to place its team changers.");
                else   // the server stands the door on the floor under that spot, walk-through direction = the camera's
                {
                    char b[180];
                    snprintf(b, sizeof(b), "SE|GMTEAM|%s|%d|%.0f,%.0f,%.0f|%.0f", g.id.c_str(), t + 1, at.x, at.y, at.z, snap.cameraRot.yaw);
                    GmSend(b);
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Walking into it puts a player on %s.", g.names[t].c_str());
            ImGui::PopID();
        }
    }
    // ---- rules
    if (ImGui::CollapsingHeader("Rules", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* modesTxt[] = { "manual", "button", "auto" };
        const std::string sm = GmSetting(g, "start_mode", "manual");
        int smi = sm == "button" ? 1 : sm == "auto" ? 2 : 0;
        ImGui::SetNextItemWidth(140);
        if (ImGui::Combo("Start", &smi, "Manual (this window / scripts)\0Start button\0Automatic (when every team has players)\0"))
            GmSend(pre + "start_mode|" + modesTxt[smi]);
        auto intRow = [&](const char* label, const char* key, const char* def, const char* tip) {
            int v = atoi(GmSetting(g, key, def).c_str());
            ImGui::SetNextItemWidth(110);
            if (GmIntCommit(label, &v, 10)) GmSend(pre + key + "|" + std::to_string((std::max)(0, v)));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        intRow("Players per team to start", "min_players", "1", "Automatic start: every team needs at least this many.");
        intRow("Countdown (s)", "countdown", "5", "Seconds between 'start' and the round beginning. 0 = straight away.");
        intRow("Round length (s)", "round_time", "180", "0 = no time limit (the round ends by score or a script).");
        intRow("Score to win", "score_to_win", "0", "A team reaching this wins at once. 0 = no score limit.");
        intRow("Result shown (s)", "end_delay", "10", "How long the result stays before the mode goes back to the lobby.");
        bool ar = GmSetting(g, "auto_restart", "0") == "1";
        if (ImGui::Checkbox("Restart automatically", &ar)) GmSend(pre + "auto_restart|" + (ar ? "1" : "0"));
        ImGui::SameLine();
        bool se = GmSetting(g, "stop_when_empty", "1") == "1";
        if (ImGui::Checkbox("End a round when every team is empty", &se)) GmSend(pre + "stop_when_empty|" + (se ? "1" : "0"));
        bool rg = GmSetting(g, "reset_after_goal", "1") == "1";
        if (ImGui::Checkbox("Ball back to its spawner after a goal", &rg)) GmSend(pre + "reset_after_goal|" + (rg ? "1" : "0"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("After a Goal or Score box gives a point, every Ball spawner's ball is put back (a kick-off).");
    }
    // ---- pieces (icons)
    if (ImGui::CollapsingHeader("Place pieces", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Placed in front of the camera (must be inside the mode's box); each gets its role at once.");
        const float tile = 78.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        const int perRow = (std::max)(1, static_cast<int>(avail / (tile + 8)));
        int col = 0;
        for (const auto& pc : kGmPieces)
        {
            const PaletteItem* it = FindItem(snap, pc.cls);
            if (!it) continue;
            if (col++ % perRow) ImGui::SameLine(0, 8);
            ImGui::PushID(pc.label);
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##pc", ImVec2(tile, tile + ImGui::GetTextLineHeight() * 2));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(q, ImVec2(q.x + tile, q.y + tile + ImGui::GetTextLineHeight() * 2), ImGui::IsItemHovered() ? IM_COL32(64, 64, 64, 255) : IM_COL32(40, 40, 40, 255), 3);
            IconFor(dl, ImVec2(q.x + 10, q.y + 4), tile - 20, it->name);
            // the label on up to two centred lines (split at the space nearest the middle)
            std::string l1 = pc.label, l2;
            if (ImGui::CalcTextSize(l1.c_str()).x > tile - 4)
            {
                size_t best = std::string::npos;
                for (size_t k = 0; k < l1.size(); ++k)
                    if (l1[k] == ' ' && (best == std::string::npos || std::abs((int)k - (int)l1.size() / 2) < std::abs((int)best - (int)l1.size() / 2))) best = k;
                if (best != std::string::npos) { l2 = l1.substr(best + 1); l1 = l1.substr(0, best); }
            }
            for (std::string* l : { &l1, &l2 }) while (l->size() > 3 && ImGui::CalcTextSize(l->c_str()).x > tile - 4) l->pop_back();
            const float lh = ImGui::GetTextLineHeight();
            dl->AddText(ImVec2(q.x + (tile - ImGui::CalcTextSize(l1.c_str()).x) * 0.5f, q.y + tile - 4), IM_COL32(220, 220, 220, 255), l1.c_str());
            if (!l2.empty()) dl->AddText(ImVec2(q.x + (tile - ImGui::CalcTextSize(l2.c_str()).x) * 0.5f, q.y + tile - 4 + lh), IM_COL32(220, 220, 220, 255), l2.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s\nRole: %s", pc.label, it->name.c_str(), GmRoleOf(std::string(pc.role).substr(0, std::string(pc.role).find(':')))->label);
            if (clicked && CanPlace())
            {
                const Vec3 at = InFront(snap, 400.0);
                if (!GmInside(g, at)) Notes().Set("Look at a spot inside '" + g.name + "' (the orange box) first.");
                else
                {
                    GmPendingRole pr;
                    pr.mode = g.id; pr.role = pc.role; pr.cls = it->name; pr.at = ImGui::GetTime();
                    for (const auto& o : snap.objects) pr.before.push_back(o.handle);
                    g_gmPendingRoles.push_back(pr);
                    SpawnAt(*it, at, 0.0);
                }
            }
            ImGui::PopID();
        }
    }
    // ---- objects + roles
    if (ImGui::CollapsingHeader("Objects and roles", ImGuiTreeNodeFlags_DefaultOpen))
    {
        std::map<std::string, int> counters;
        int shown = 0;
        for (const auto& o : snap.objects)
        {
            if (!GmInside(g, o.location)) continue;
            if (o.className.find("TeamChange") != std::string::npos) continue;   // teams section
            if (o.handle.size() < 36 || o.handle[8] != '-') continue;              // station props: only placed objects take roles
            ++shown;
            ImGui::PushID(o.handle.c_str());
            const ImVec2 ip = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(20, 20));
            IconFor(ImGui::GetWindowDrawList(), ip, 20, o.className);
            ImGui::SameLine();
            if (ImGui::SmallButton(PrettyName(o.className).c_str())) RequestUiSelect(o.handle);
            auto it = g.sets.find("role." + GmGuid(o.handle));
            const std::string val = it == g.sets.end() ? std::string() : it->second;
            const std::string key = val.substr(0, val.find(':'));
            const std::string param = val.find(':') == std::string::npos ? std::string() : val.substr(val.find(':') + 1);
            const bool isCtl = g.sets.count("controller") && o.handle.rfind(g.sets.at("controller"), 0) == 0;
            const bool isHost = g.sets.count("scripthost") && !g.sets.at("scripthost").empty() && o.handle.rfind(g.sets.at("scripthost"), 0) == 0;
            ImGui::SameLine(210);
            if (isHost) { ImGui::TextDisabled("script host (runs the mode's script)"); ImGui::PopID(); continue; }
            if (isCtl) { ImGui::TextDisabled(g.sets.count("scripthost") ? "controller (arena clock, state, score)" : "controller (runs the mode's script)"); ImGui::PopID(); continue; }
            ImGui::SetNextItemWidth(190);
            if (ImGui::BeginCombo("##role", GmRoleOf(key)->label))
            {
                for (const auto& r : kGmRoles)
                {
                    if (!GmRoleFits(r, o.className)) continue;
                    if (ImGui::Selectable(r.label, key == r.key))
                    {
                        // Keep the current number only when switching between roles that mean the same by it (a team,
                        // or seconds); otherwise start from that role's default.
                        const std::string rk = r.key;
                        auto isTeam = [](const std::string& k) { return k == "score" || k == "goal" || k == "score_zone"; };
                        const bool keep = !param.empty() && isTeam(rk) == isTeam(key);
                        const std::string def = rk == "score_zone" ? "0" : isTeam(rk) ? "1" : "5";
                        GmSend(pre + "role." + GmGuid(o.handle) + "|" + rk + (r.param ? ":" + (keep ? param : def) : std::string()));
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.help);
                }
                ImGui::EndCombo();
            }
            const GmRole* r = GmRoleOf(key);
            if (r->param)
            {
                ImGui::SameLine();
                int pv = atoi(param.c_str());
                ImGui::SetNextItemWidth(70);
                if (GmIntCommit(r->paramLabel, &pv, 1))
                    GmSend(pre + "role." + GmGuid(o.handle) + "|" + key + ":" + std::to_string((std::max)(key == "score_zone" ? 0 : 1, pv)));
                if (key == "score_zone" && pv == 0) { ImGui::SameLine(); ImGui::TextDisabled("(by side)"); }
            }
            if (!key.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", GmSlotName(key, ++counters[key]).c_str()); }
            ImGui::PopID();
        }
        if (!shown) ImGui::TextDisabled("Nothing placed inside this game mode yet.");
    }
    // ---- script
    if (ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("The mode's controller script is built from the roles above plus your code in GameModes\\%s.luau "
                           "(hooks like OnRoundStart / OnRoundEnd / OnScore, and Rigel.* - see the Game Modes guide).", GmFileStem(g.name).c_str());
        if (ImGui::Button("Apply script")) GmApply(snap, g);
        ImGui::SameLine();
        if (ImGui::Button("Edit my code"))
        {
            GmUserCode(g, true);
            const std::wstring arg = L"\"" + GmCodePath(g) + L"\"";
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"code", arg.c_str(), nullptr, SW_HIDE)) <= 32)
                ShellExecuteW(nullptr, L"open", GmCodePath(g).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        static std::wstring s_pickExample;
        if (ImGui::Button("Start from example...") || TourOnce("popup:gm_example")) ImGui::OpenPopup("##gmex");
        if (ImGui::BeginPopup("##gmex"))
        {
            ImGui::TextDisabled("Replaces your code for this mode with an example (each has a matching example level).");
            std::error_code ec;
            const std::wstring exDir = ScriptsDir() + L"\\examples\\gamemodes";
            if (std::filesystem::exists(exDir, ec))
                for (const auto& e : std::filesystem::directory_iterator(exDir, ec))
                {
                    if (e.path().extension() != L".luau") continue;
                    const std::string nm = e.path().stem().string();
                    if (ImGui::Selectable(nm.c_str())) s_pickExample = e.path().wstring();
                    if (ImGui::IsItemHovered())
                    {
                        const std::string src = ReadFileUtf8(e.path().wstring());
                        std::string head;
                        for (size_t i = 0, lines = 0; i < src.size() && lines < 14; ++i) { head += src[i]; if (src[i] == '\n') ++lines; }
                        ImGui::SetTooltip("%s", head.c_str());
                    }
                }
            else ImGui::TextDisabled("(no examples installed)");
            ImGui::EndPopup();
        }
        if (!s_pickExample.empty())
        {
            std::error_code ec;
            std::filesystem::copy_file(s_pickExample, GmCodePath(g), std::filesystem::copy_options::overwrite_existing, ec);
            Notes().Set(ec ? "Couldn't copy the example." : "Your code for '" + g.name + "' is now the example -- applying.");
            s_pickExample.clear();
            if (!ec) GmApply(snap, g);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-apply on save", &g_gmAutoApply);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show generated") || TourOnce("popup:gm_preview")) { std::vector<GmSlot> sl; g_gmPreview = GmBuildScript(snap, g, sl); ImGui::OpenPopup("##gmprev"); }
        if (ImGui::BeginPopup("##gmprev"))
        {
            ImGui::InputTextMultiline("##src", g_gmPreview.data(), g_gmPreview.size() + 1, ImVec2(620, 420), ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button("Copy")) ImGui::SetClipboardText(g_gmPreview.c_str());
            ImGui::EndPopup();
        }
        // auto-apply when the author saves their file
        if (g_gmAutoApply)
        {
            static double s_last = 0;
            if (ImGui::GetTime() - s_last > 1.0)
            {
                s_last = ImGui::GetTime();
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                if (GetFileAttributesExW(GmCodePath(g).c_str(), GetFileExInfoStandard, &fa))
                {
                    auto it = g_gmCodeWritten.find(g.id);
                    if (it != g_gmCodeWritten.end() && CompareFileTime(&it->second, &fa.ftLastWriteTime) != 0)
                    {
                        if (CheckScriptFile(GmCodePath(g), GmFileStem(g.name))) GmApply(snap, g);
                        else g_gmCodeWritten[g.id] = fa.ftLastWriteTime;
                    }
                }
            }
        }
    }
    // ---- text templates
    if (ImGui::CollapsingHeader("Text on signs"))
    {
        ImGui::TextWrapped("Put these in a Text object's Text (Game data) and they update live for everyone:");
        std::vector<std::string> toks = { g.name + ".state", g.name + ".time", g.name + ".round", g.name + ".winner", g.name + ".players" };
        for (int t = 1; t <= g.nTeams; ++t)
            for (const char* k : { "name", "score", "size", "max", "wins" }) toks.push_back(g.name + ".team" + std::to_string(t) + "." + k);
        for (const auto& t : toks)
        {
            const std::string tk = "{" + t + "}";
            ImGui::PushID(tk.c_str());
            if (ImGui::SmallButton("copy")) ImGui::SetClipboardText(tk.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(tk.c_str());
            ImGui::PopID();
        }
    }
    // ---- custom settings
    if (ImGui::CollapsingHeader("Custom settings"))
    {
        ImGui::TextDisabled("Your own values, read in scripts with Rigel.setting(\"name\").");
        for (const auto& kv : g.sets)
        {
            if (kv.first.rfind("custom.", 0) != 0) continue;
            ImGui::BulletText("%s = %s", kv.first.substr(7).c_str(), kv.second.c_str());
        }
        static char ck[32] = "", cv[64] = "";
        ImGui::SetNextItemWidth(120);
        ImGui::InputTextWithHint("##ck", "name", ck, sizeof(ck));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint("##cv", "value", cv, sizeof(cv));
        ImGui::SameLine();
        if (ImGui::SmallButton("Set") && ck[0]) { GmSend(pre + "custom." + ck + "|" + cv); ck[0] = cv[0] = 0; }
    }
}

// Test ops (tests/*.txt): gmrole <Class> x y z <mode> <role>  |  gmapply <mode>  |  gmpiece <mode> <piece label>  |
// expectgm <mode> <state> [team scores csv]
void GmTestOp(const Snapshot& snap, const std::string& line)
{
    std::vector<std::string> a;
    for (size_t b = 0; b < line.size();) { size_t e = line.find(' ', b); if (e == std::string::npos) e = line.size(); if (e > b) a.push_back(line.substr(b, e - b)); b = e + 1; }
    auto mode = [&](const std::string& n) -> GmInfo { for (const auto& r : snap.gameModes) { GmInfo g = GmParse(r); if (g.name == n) return g; } return GmInfo{}; };
    if (a[0] == "gmrole" && a.size() >= 4)      // gmrole <Class> <mode> <role> [n]: the n-th (by x) object of that class in the mode
    {
        const GmInfo g = mode(a[2]);
        std::vector<const SceneObject*> hits;
        for (const auto& o : snap.objects)
            if (!g.id.empty() && o.className.find(a[1]) != std::string::npos && GmInside(g, o.location)) hits.push_back(&o);
        std::sort(hits.begin(), hits.end(), [](const SceneObject* x, const SceneObject* y) { return x->location.x < y->location.x; });
        const size_t n = a.size() >= 5 ? static_cast<size_t>(atoi(a[4].c_str())) : 0;
        if (n >= hits.size()) { Log("[gmtest] FAIL gmrole: mode '%s' %s, %zu %s inside", a[2].c_str(), g.id.empty() ? "missing" : "ok", hits.size(), a[1].c_str()); return; }
        GmSend("SE|GMSET|" + g.id + "|role." + GmGuid(hits[n]->handle) + "|" + a[3]);
        Log("[gmtest] role %s -> %s (%s)", hits[n]->handle.c_str(), a[3].c_str(), g.id.c_str());
    }
    else if (a[0] == "gmapply" && a.size() >= 2)
    {
        const GmInfo g = mode(a[1]);
        if (g.id.empty()) { Log("[gmtest] FAIL gmapply: no mode '%s'", a[1].c_str()); return; }
        GmApply(snap, g);
        std::vector<GmSlot> sl;
        Log("[gmtest] generated script:\n%s", GmBuildScript(snap, g, sl).c_str());
    }
    else if (a[0] == "gmpiece" && a.size() >= 3)
    {
        const GmInfo g = mode(a[1]);
        std::string label = line.substr(line.find(a[2], line.find(a[1]) + a[1].size()));
        for (const auto& pc : kGmPieces)
        {
            if (label != pc.label) continue;
            const PaletteItem* it = FindItem(snap, pc.cls);
            const Vec3 at{ g.at[0] + 300.0, g.at[1] + 300.0, g.at[2] };   // the tests can't aim the camera at the mode
            if (!it || g.id.empty() || !GmInside(g, at)) { Log("[gmtest] FAIL gmpiece %s: item %d mode %d inside %d", pc.label, !!it, !g.id.empty(), !g.id.empty() && GmInside(g, at)); return; }
            GmPendingRole pr;
            pr.mode = g.id; pr.role = pc.role; pr.cls = it->name; pr.at = ImGui::GetTime();
            for (const auto& o : snap.objects) pr.before.push_back(o.handle);
            g_gmPendingRoles.push_back(pr);
            g_placeBypass = true; SpawnAt(*it, at, 0.0); g_placeBypass = false;
            Log("[gmtest] piece %s (%s) role %s at (%.0f,%.0f,%.0f)", pc.label, it->name.c_str(), pc.role, at.x, at.y, at.z);
            return;
        }
        Log("[gmtest] FAIL gmpiece: no piece '%s'", label.c_str());
    }
    else if (a[0] == "gmwin")                    // gmwin [mode] | gmwin off: the Game Modes window
    {
        if (a.size() >= 2 && a[1] == "off") g_gmWindow = false;
        else { g_gmWindow = true; if (a.size() >= 2) { const GmInfo g = mode(a[1]); if (!g.id.empty()) g_gmSel = g.id; } }
        Log("[gmtest] game modes window %s", g_gmWindow ? "open" : "closed");
    }
    else if (a[0] == "gmscroll" && a.size() >= 2) g_gmScrollReq = static_cast<float>(atof(a[1].c_str()));
    else if (a[0] == "gmpopup" && a.size() >= 2) g_tourForce = a[1];          // e.g. popup:gm_new, popup:saveas, popup:quickadd
    else if (a[0] == "gmtab" && a.size() >= 2) g_tabForce = line.substr(line.find(a[1]));   // Details / Quest Editor / Levels / Game Modes
    else if (a[0] == "expectgm" && a.size() >= 3)
    {
        const GmInfo g = mode(a[1]);
        std::string sc;
        for (int t = 0; t < g.nTeams; ++t) sc += (t ? "," : "") + std::to_string(g.scores[t]);
        const bool ok = !g.id.empty() && g.state == a[2] && (a.size() < 4 || sc == a[3]);
        std::string szs; for (int t = 0; t < g.nTeams; ++t) szs += (t ? "," : "") + std::to_string(g.sizes[t]);
        Log("[gmtest] %s expectgm %s: state %s scores %s sizes %s (want %s %s), %zu setting(s)", ok ? "PASS" : "FAIL", a[1].c_str(), g.state.c_str(), sc.c_str(), szs.c_str(),
            a[2].c_str(), a.size() >= 4 ? a[3].c_str() : "-", g.sets.size());
    }
}

void DrawDetailsPanel(const Snapshot& snap, const SceneObject* sel, ImVec2 pos, ImVec2 size)
{
    if (!BeginPanel("##details", pos, size)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("##dettabs"))
    {
        if (ImGui::BeginTabItem("Details", nullptr, TourTab("Details")))
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
                if (sel->lockedByOther)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1), "Locked by %s - read only",
                                       sel->lockOwner.empty() ? "another editor" : sel->lockOwner.c_str());
                else if (sel->handle.size() >= 36 && sel->handle[8] == '-' && sel->handle[23] == '-')   // a placed (sandbox) object
                {
                    // Owner lock, per object: on = only you (who placed it) can move, delete or edit it.
                    bool locked = sel->lockedByMe;
                    if (ImGui::Checkbox("Lock (only I can edit it)", &locked))
                    {
                        Command c{ CmdType::OwnLock }; c.str = sel->handle; c.str2 = locked ? "1" : "0"; State().Push(c);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Other editors can still see it, but can't move, delete, script or change it.\nOnly whoever placed the object can lock or unlock it.");
                }

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
                        UndoXf b = UndoXfOf(sel->handle), a = b;
                        a.loc = c.loc; a.rot = c.rot; a.scl = c.scale;
                        UndoRecordMove({ b }, { a }, true);     // one step per field edit, not per frame
                    }
                }
                if (ContainsCi("Snapping Grid", g_detailsFilter) && ImGui::CollapsingHeader("Snapping"))
                {
                    ImGui::Checkbox("Enabled", &g_snapEnabled);
                    ImGui::SetNextItemWidth(110); ImGui::DragFloat("Grid", &g_gridSnap, 1.0f, 1.0f, 1000.0f);
                    ImGui::SetNextItemWidth(110); ImGui::DragFloat("Rotation", &g_rotSnap, 1.0f, 1.0f, 90.0f);
                }
                if (ImGui::CollapsingHeader("Game data (synced to everyone)", ImGuiTreeNodeFlags_DefaultOpen))
                    DrawGameData(snap, sel);
                if (ImGui::CollapsingHeader("Luau script"))
                    DrawLuau(snap, sel);
                if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen))
                    DrawProperties(snap, sel);
                if (ContainsCi("Quest Checkpoint", g_detailsFilter) && ImGui::CollapsingHeader("Quest", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (g_questSel < 0 || g_questSel >= (int)g_quests.size())
                        ImGui::TextDisabled("Open the Quest Editor tab and make a quest to use this as a checkpoint.");
                    else
                    {
                        QuestDraft& q = g_quests[g_questSel];
                        const bool already = std::any_of(q.steps.begin(), q.steps.end(), [&](const QuestStep& st) { return st.objectHandle == sel->handle; });
                        ImGui::BeginDisabled(already);
                        if (ImGui::Button(already ? "Already a checkpoint" : "Add as checkpoint", ImVec2(-1, 0)))
                        {
                            q.steps.push_back({ sel->handle, PrettyName(sel->className) });
                            q.dirty = true;
                        }
                        ImGui::EndDisabled();
                        ImGui::TextDisabled("Quest: %s  (%d checkpoint%s)", q.title[0] ? q.title : "(untitled)",
                                            (int)q.steps.size(), q.steps.size() == 1 ? "" : "s");
                    }
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Quest Editor", nullptr, TourTab("Quest Editor")))
        {
            DrawQuestEditor(snap, sel);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Levels", nullptr, TourTab("Levels")))
        {
            DrawLevels(snap);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game Modes", nullptr, TourTab("Game Modes")))
        {
            DrawGameModes(snap);
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
    const std::string shown = g_cbFolder == "*fav" ? std::string("/All/Favorites") : "/All" + g_cbFolder;               // "/All/Game/A2/Prefabs"
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
        if (ImGui::SmallButton((part + "##c" + std::to_string(at)).c_str()) && target.size() >= 5) g_cbFolder = target == "/Favorites" ? std::string("*fav") : target;
        at = nx + 1;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 260);
    ImGui::SetNextItemWidth(250);
    ImGui::InputTextWithHint("##cf", "Search Content", g_contentFilter, sizeof(g_contentFilter));

    // Left: folder tree.
    ImGui::BeginChild("##cbtree", ImVec2(220, 0), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Header, kSelBlue);
    {
        const ImVec2 fp = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("      Favorites", g_cbFolder == "*fav", ImGuiSelectableFlags_SpanAvailWidth)) g_cbFolder = "*fav";
        DrawStar(ImGui::GetWindowDrawList(), ImVec2(fp.x + 10, fp.y + 8), 7.0f, true, IM_COL32(250, 200, 60, 255));
        const ImVec2 lp = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("      Levels", g_cbFolder == "*levels", ImGuiSelectableFlags_SpanAvailWidth)) g_cbFolder = "*levels";
        IconFolder(ImGui::GetWindowDrawList(), ImVec2(lp.x + 3, lp.y + 1), 15);
    }
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
    if (g_cbFolder == "*levels")
    {
        // Documents\RigelLevels: every .a2level as a tile. Double-click opens it (the open level closes first,
        // saved if it had changes); right-click: Open / Upload / Show in Explorer. First tile: New Level.
        static std::vector<std::wstring> s_files;
        static double s_listed = -100.0;
        if (ImGui::GetTime() - s_listed > 2.0)
        {
            s_listed = ImGui::GetTime();
            s_files.clear();
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((LevelsDir() + L"\\*.a2level").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do { std::wstring f = fd.cFileName; s_files.push_back(f.substr(0, f.size() - 8)); } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        nextTile();
        {
            const ImVec2 q = ImGui::GetCursorScreenPos();
            ImGui::BeginDisabled(!snap.inEditor);
            if (ImGui::InvisibleButton("##newlevel", ImVec2(tileW, tileH))) SceneNew();
            ImGui::EndDisabled();
            const bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(q, ImVec2(q.x + tileW, q.y + tileH), hov ? IM_COL32(64, 64, 64, 255) : IM_COL32(44, 44, 44, 255), 3);
            dl->AddText(ImGui::GetFont(), 40.0f, ImVec2(q.x + tileW * 0.5f - 11, q.y + 18), kDim, "+");
            dl->AddText(ImVec2(q.x + 6, q.y + 84), kFg, "New Level");
        }
        for (const auto& wf : s_files)
        {
            const std::string f(wf.begin(), wf.end());
            nextTile();
            ImGui::PushID(f.c_str());
            const ImVec2 q = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##lv", ImVec2(tileW, tileH));
            const bool hov = ImGui::IsItemHovered(), open = f == g_sceneName;
            if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && snap.inEditor) SceneOpen(f);
            if (hov) ImGui::SetTooltip("%s.a2level\nDouble-click: open (the current level is saved and closed)\nRight-click: Upload, Delete, ...", f.c_str());
            if (hov && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) g_lvDeleteAsk = f;
            if (ImGui::BeginPopupContextItem("##lvctx"))
            {
                if (open) { if (ImGui::MenuItem("Close", nullptr, false, snap.inEditor)) SceneClose(); }
                else if (ImGui::MenuItem("Open", nullptr, false, snap.inEditor)) SceneOpen(f);
                if (ImGui::MenuItem("Upload to Server", nullptr, false, snap.inEditor))
                {
                    if (f == g_sceneName) SceneUpload();
                    else { Command c{ CmdType::LevelImport }; c.str = SceneClean(f); c.str2 = "save"; c.str3 = SceneReadFile(f); State().Push(c); }
                }
                if (ImGui::MenuItem("Delete...")) g_lvDeleteAsk = f;
                if (ImGui::MenuItem("Show in Explorer"))
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + LevelsDir() + L"\\" + wf + L".a2level\"").c_str(), nullptr, SW_SHOWNORMAL);
                ImGui::EndPopup();
            }
            dl->AddRectFilled(q, ImVec2(q.x + tileW, q.y + tileH), hov ? IM_COL32(64, 64, 64, 255) : IM_COL32(44, 44, 44, 255), 3);
            if (open) dl->AddRect(q, ImVec2(q.x + tileW, q.y + tileH), IM_COL32(240, 160, 40, 255), 3, 0, 2.0f);
            // A level "map" glyph: a framed landscape.
            dl->AddRectFilled(ImVec2(q.x + 14, q.y + 12), ImVec2(q.x + tileW - 14, q.y + 70), IM_COL32(30, 60, 90, 255), 3);
            dl->AddTriangleFilled(ImVec2(q.x + 20, q.y + 64), ImVec2(q.x + 42, q.y + 30), ImVec2(q.x + 60, q.y + 64), IM_COL32(90, 150, 90, 255));
            dl->AddTriangleFilled(ImVec2(q.x + 44, q.y + 64), ImVec2(q.x + 62, q.y + 40), ImVec2(q.x + tileW - 20, q.y + 64), IM_COL32(70, 120, 70, 255));
            std::string n = f;
            if (ImGui::CalcTextSize(n.c_str()).x > tileW - 6)
            {
                while (n.size() > 3 && ImGui::CalcTextSize((n + "...").c_str()).x > tileW - 6) n.pop_back();
                n += "...";
            }
            dl->AddText(ImVec2(q.x + 4, q.y + 84), kFg, n.c_str());
            dl->AddText(ImVec2(q.x + 4, q.y + 100), open ? IM_COL32(240, 160, 40, 255) : kDim, open ? (g_sceneDirty ? "Open *" : "Open") : "Level");
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::End();
        return;
    }

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
        if (g_contentFilter[0] ? !ContainsCi(it.name, g_contentFilter)
                               : g_cbFolder == "*fav" ? !IsFav(it.path) : FolderOf(it.path) != g_cbFolder) continue;
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
        if (hov || IsFav(it.path)) FavStarButton(dl, ImVec2(q.x + tileW - 12, q.y + 12), 8.0f, it.path);
        if (ImGui::BeginPopupContextItem("##favctx"))
        {
            if (ImGui::MenuItem(IsFav(it.path) ? "Remove from Favorites" : "Add to Favorites")) ToggleFav(it.path);
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ++count;
    }
    ImGui::PopStyleColor();
    if (count == 0) ImGui::TextDisabled(g_contentFilter[0] ? "No matching assets." : g_cbFolder == "*fav" ? "No favorites yet - star an asset (or right-click it) to add it." : "This folder is empty.");
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
    const std::string note = Notes().Get(8000);
    if (!note.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "|  %s", note.c_str());
    else
        ImGui::TextDisabled("|  %s  |  %d prefabs  |  RMB: look / fly", snap.status.c_str(), static_cast<int>(snap.palette.size()));
    const char* hint = "F12 hides the editor";
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

// ---- undo / redo ------------------------------------------------------------------------------
// Client-side history of this editor's own edits. Every step is applied again as ordinary editor commands
// (move / delete / place), so it goes through the server like any edit -- owner locks included -- and
// other players see it. Objects are found again by handle, else by class near where they should be
// (a re-placed object comes back with a new handle).
const Snapshot* g_undoSnap = nullptr;    // this frame's snapshot (set by DrawEditorUI)
struct UndoExpect { size_t count = 0; std::string label; double until = 0.0; std::unordered_set<std::string> known; std::vector<UndoXf> found; };
std::vector<UndoExpect> g_undoExpect;

const SceneObject* UndoObj(const std::string& handle)
{
    if (!g_undoSnap) return nullptr;
    for (const auto& o : g_undoSnap->objects) if (o.handle == handle) return &o;
    return nullptr;
}
UndoXf UndoXfOf(const std::string& handle)
{
    UndoXf x; x.handle = handle;
    if (const SceneObject* o = UndoObj(handle))
    {
        x.cls = o->className; x.loc = o->location; x.rot = o->rotation; x.scl = o->scale;
        if (g_undoSnap) if (const PaletteItem* it = FindItem(*g_undoSnap, o->className)) x.path = it->path;
    }
    return x;
}
void UndoPush(UndoOp op)
{
    op.at = ImGui::GetTime();
    g_undo.push_back(std::move(op));
    if (g_undo.size() > 100) g_undo.erase(g_undo.begin());
    g_redo.clear();
}
void UndoRecordMove(const std::vector<UndoXf>& before, const std::vector<UndoXf>& after, bool coalesce)
{
    if (before.empty() || before[0].cls.empty()) return;
    bool moved = false;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i)
    {
        const Vec3 d = Sub(before[i].loc, after[i].loc);
        if (Dot(d, d) > 0.01 || std::fabs(before[i].rot.yaw - after[i].rot.yaw) > 0.01 || std::fabs(before[i].rot.pitch - after[i].rot.pitch) > 0.01 ||
            std::fabs(before[i].rot.roll - after[i].rot.roll) > 0.01 || std::fabs(before[i].scl.x - after[i].scl.x) > 1e-4 ||
            std::fabs(before[i].scl.y - after[i].scl.y) > 1e-4 || std::fabs(before[i].scl.z - after[i].scl.z) > 1e-4) moved = true;
    }
    if (!moved) return;
    // Details fields report every frame of a drag-edit: fold them into the step they started.
    if (coalesce && !g_undo.empty() && g_undo.back().coalesce && g_undo.back().kind == 1 && g_undo.back().after.size() == 1 &&
        g_undo.back().after[0].handle == after[0].handle && ImGui::GetTime() - g_undo.back().at < 1.0)
    {
        g_undo.back().after = after;
        g_undo.back().at = ImGui::GetTime();
        g_redo.clear();
        return;
    }
    UndoOp op; op.kind = 1; op.before = before; op.after = after; op.coalesce = coalesce;
    op.label = before.size() > 1 ? "Move " + std::to_string(before.size()) + " objects" : "Move";
    UndoPush(std::move(op));
}
void UndoRecordDelete(const std::vector<std::string>& handles)
{
    UndoOp op; op.kind = 3;
    for (const auto& h : handles) { UndoXf x = UndoXfOf(h); if (!x.cls.empty()) op.before.push_back(x); }
    if (op.before.empty()) return;
    op.label = op.before.size() > 1 ? "Delete " + std::to_string(op.before.size()) + " objects" : "Delete";
    UndoPush(std::move(op));
}
// Placing / duplicating: the new objects' handles are only known once they appear, so remember what
// existed and record the next `count` newcomers (within 8 s) as one step.
void UndoExpectNew(size_t count, const char* label)
{
    if (!g_undoSnap || !count) return;
    UndoExpect e; e.count = count; e.label = label; e.until = ImGui::GetTime() + 8.0;
    for (const auto& o : g_undoSnap->objects) e.known.insert(o.handle);
    g_undoExpect.push_back(std::move(e));
}
void UndoTick(const Snapshot& snap)
{
    for (size_t i = 0; i < g_undoExpect.size();)
    {
        UndoExpect& e = g_undoExpect[i];
        for (const auto& o : snap.objects)
        {
            if (e.found.size() >= e.count) break;
            if (e.known.count(o.handle)) continue;
            e.known.insert(o.handle);
            UndoXf x = UndoXfOf(o.handle);
            if (!x.cls.empty()) e.found.push_back(x);
        }
        if (e.found.size() >= e.count || ImGui::GetTime() > e.until)
        {
            if (!e.found.empty())
            {
                UndoOp op; op.kind = 2; op.after = e.found;
                op.label = e.found.size() > 1 ? e.label + " " + std::to_string(e.found.size()) + " objects" : e.label;
                UndoPush(std::move(op));
            }
            g_undoExpect.erase(g_undoExpect.begin() + i);
        }
        else ++i;
    }
}
// Find a recorded object again: same handle, same sandbox id (a rebuild adds "_n"), else the same class
// nearest to where it should be.
const SceneObject* UndoFind(const UndoXf& x, const Vec3& nearPt)
{
    if (!g_undoSnap) return nullptr;
    if (const SceneObject* o = UndoObj(x.handle)) return o;
    const std::string id = x.handle.substr(0, x.handle.find('_'));
    const SceneObject* best = nullptr;
    double bestD = 60.0 * 60.0;
    for (const auto& o : g_undoSnap->objects)
    {
        if (id.size() >= 32 && o.handle.rfind(id, 0) == 0) return &o;
        if (o.className != x.cls) continue;
        const Vec3 d = Sub(o.location, nearPt);
        const double dd = Dot(d, d);
        if (dd < bestD) { bestD = dd; best = &o; }
    }
    return best;
}
void UndoSetXf(const SceneObject& o, const UndoXf& to)
{
    Command c{ CmdType::SetTransform }; c.str = o.handle; c.loc = to.loc; c.rot = to.rot; c.scale = to.scl; State().Push(c);
}
void UndoDelete(const SceneObject& o) { Command c{ CmdType::DeleteObject }; c.str = o.handle; State().Push(c); }
bool UndoPlace(const UndoXf& x)
{
    if (x.path.empty()) return false;
    Command c{ CmdType::SpawnItem }; c.str = x.path; c.loc = x.loc; c.rot = x.rot; c.scale = x.scl; State().Push(c);
    return true;
}
// forward = redo. Returns false when there is nothing to do.
bool UndoApply(bool forward)
{
    std::vector<UndoOp>& from = forward ? g_redo : g_undo;
    std::vector<UndoOp>& to   = forward ? g_undo : g_redo;
    if (from.empty() || !g_undoSnap) return false;
    UndoOp op = from.back();
    from.pop_back();
    int missing = 0, cant = 0;
    if (op.kind == 1)
    {
        const auto& dst = forward ? op.after : op.before;
        const auto& src = forward ? op.before : op.after;
        for (size_t i = 0; i < dst.size() && i < src.size(); ++i)
            if (const SceneObject* o = UndoFind(src[i], src[i].loc)) UndoSetXf(*o, dst[i]); else ++missing;
    }
    else if ((op.kind == 2) != forward)       // undo a placement / redo a delete: remove them
    {
        const auto& list = op.kind == 2 ? op.after : op.before;
        for (const auto& x : list) if (const SceneObject* o = UndoFind(x, x.loc)) UndoDelete(*o); else ++missing;
    }
    else                                      // undo a delete / redo a placement: put them back
    {
        const auto& list = op.kind == 2 ? op.after : op.before;
        for (const auto& x : list) if (!UndoPlace(x)) ++cant;
    }
    Log("[undo] %s '%s' (%zu undo / %zu redo after this)%s", forward ? "redo" : "undo", op.label.c_str(),
        g_undo.size() + (forward ? 1 : 0), g_redo.size() + (forward ? 0 : 1), missing ? " -- some objects are gone" : "");
    if (cant) Notes().Set(std::to_string(cant) + " object(s) can't be put back: catalogue-only pieces have no palette entry to re-place.");
    else if (missing) Notes().Set(std::string(forward ? "Redo" : "Undo") + " '" + op.label + "': some objects no longer exist.");
    to.push_back(std::move(op));
    return true;
}

void HandleShortcuts()
{
    // While flying, W/E/Q/A/S/D steer the camera; they must not also switch the gizmo.
    if (ImGui::GetIO().WantTextInput || Cam().looking) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) g_gizmo = GizmoMode::Select;
    if (ImGui::IsKeyPressed(ImGuiKey_W)) g_gizmo = GizmoMode::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) g_gizmo = GizmoMode::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) g_gizmo = GizmoMode::Scale;
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SceneSaveOrAsk();   // Ctrl+S: save the open level
    if (ImGui::GetIO().KeyCtrl && g_dragAxis < 0)
    {
        const bool shift = ImGui::GetIO().KeyShift;
        if (ImGui::IsKeyPressed(ImGuiKey_Z) && !shift) UndoApply(false);                                   // Ctrl+Z
        if (ImGui::IsKeyPressed(ImGuiKey_Y) || (shift && ImGui::IsKeyPressed(ImGuiKey_Z))) UndoApply(true); // Ctrl+Y / Ctrl+Shift+Z
    }
    if (g_slotPick.on && ImGui::IsKeyPressed(ImGuiKey_Escape)) { g_slotPick.on = false; return; }   // cancel "click an object"
    if (g_coinPlaceQuest >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape)) { g_coinPlaceQuest = -1; return; }   // leave construction mode
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !g_selected.empty())
        ClearSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !g_selected.empty())
        DeleteSelected();
}

}  // namespace

static std::mutex g_uiSelectMx;
static std::vector<std::string> g_uiSelectReqs;            // test scripts / the MCP bridge: UI actions, one per frame
void RequestUiSelect(const std::string& handle) { std::lock_guard<std::mutex> lk(g_uiSelectMx); g_uiSelectReqs.push_back(handle); }
std::string CurrentSceneName() { return g_sceneName; }
bool CurrentSceneDirty() { return g_sceneDirty.load(); }

std::wstring LevelsDir()
{
    wchar_t docs[MAX_PATH] = {};
    std::wstring dir = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)) ? std::wstring(docs) + L"\\RigelLevels"
                                                                                               : std::wstring(L"C:\\RigelLevels");
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void DrawEditorUI()
{
    const Snapshot snap = State().ReadSnapshot();
    g_undoSnap = &snap;
    UndoTick(snap);
    if (g_tourStep >= 0 && ImGui::GetTime() - g_tourAt > 1.2)          // the UI tour: next step
    {
        g_tourAt = ImGui::GetTime();
        ++g_tourStep;
        if (!TourStep()[0])
        {
            g_tourStep = -1;
            ProblemBox::Item it;
            if (Problems().Front(it) && it.title == "UI tour") Problems().Pop();
            ImGui::CloseCurrentPopup();
            g_gmWindow = false;
            Log("[uitour] done: every tab and popup drawn");
        }
        else
        {
            Log("[uitour] step %d %s", g_tourStep, TourStep());
            if (TourIs("select:any") && !snap.objects.empty()) SelectHandle(snap.objects.front().handle);
            if (TourIs("select:scripted"))
                for (const auto& r : snap.gameModes)
                {
                    const GmInfo g = GmParse(r);
                    auto c = g.sets.find("controller");
                    if (c == g.sets.end()) continue;
                    for (const auto& o : snap.objects) if (o.handle.rfind(c->second, 0) == 0) { SelectHandle(o.handle); break; }
                    break;
                }
            if (TourIs("window:gm")) g_gmWindow = true;
            if (TourIs("popup:problem")) { ProblemBox::Item it; it.title = "UI tour"; it.text = "A test problem (the tour closes it)."; Problems().Push(it); }
        }
    }
    {
        std::string req;
        { std::lock_guard<std::mutex> lk(g_uiSelectMx); if (!g_uiSelectReqs.empty()) { req = g_uiSelectReqs.front(); g_uiSelectReqs.erase(g_uiSelectReqs.begin()); } }
        if (req.empty()) {}
        else if (req[0] == '+') { ToggleMultiSel(req.substr(1)); Log("[ui] selection toggle %s -> %zu selected", req.c_str() + 1, g_selected.empty() ? 0 : 1 + g_multiSel.size()); }
        else if (req.rfind("!expectscene ", 0) == 0)
        {
            char nm[80] = ""; int dirty = 0;
            sscanf_s(req.c_str() + 13, "%79s %d", nm, (unsigned)sizeof(nm), &dirty);
            const std::string want = strcmp(nm, "-") ? nm : "";
            const bool ok = want == g_sceneName && dirty == (int)g_sceneDirty.load();
            Log("[script] %s expectscene want='%s' dirty=%d have='%s' dirty=%d", ok ? "PASS" : "FAIL", want.c_str(), dirty,
                g_sceneName.c_str(), (int)g_sceneDirty.load());
        }
        else if (req == "!uitour") { g_tourStep = 0; g_tourAt = ImGui::GetTime(); g_uiVisible = true; Log("[uitour] step 0 %s", TourStep()); }
        else if (req == "!undo" || req == "!redo")
        {
            const bool ok = UndoApply(req == "!redo");
            Log("[ui] %s -> %s", req.c_str() + 1, ok ? "applied" : "nothing to do");
        }
        else if (req == "!del") DeleteSelected();
        // Edits made through the MCP (an agent) go on the same undo stack as the user's own, so undo / redo
        // (the tool and Ctrl+Z) work on them too. The bridge sends these BEFORE its command runs.
        else if (req.rfind("!undodel ", 0) == 0) UndoRecordDelete({ req.substr(9) });
        else if (req.rfind("!undonew ", 0) == 0) UndoExpectNew(1, req.substr(9).c_str());
        else if (req.rfind("!undomove ", 0) == 0)       // handle x y z pitch yaw roll sx sy sz
        {
            char h[160] = {};
            UndoXf a;
            if (sscanf_s(req.c_str() + 10, "%159s %lf %lf %lf %lf %lf %lf %lf %lf %lf", h, (unsigned)sizeof(h), &a.loc.x, &a.loc.y, &a.loc.z,
                         &a.rot.pitch, &a.rot.yaw, &a.rot.roll, &a.scl.x, &a.scl.y, &a.scl.z) == 10)
            {
                const UndoXf b = UndoXfOf(h);
                a.handle = b.handle; a.cls = b.cls; a.path = b.path;
                UndoRecordMove({ b }, { a }, false);
            }
        }
        else if (req.rfind("!scene ", 0) == 0)
        {
            const std::string rest = req.substr(7);
            const size_t sp = rest.find(' ');
            const std::string op = rest.substr(0, sp), arg = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
            if (op == "saveas") SceneSave(arg);
            else if (op == "save") SceneSaveOrAsk();
            else if (op == "open") SceneOpen(arg);
            else if (op == "new") SceneNew();
            else if (op == "close") SceneClose();
            else if (op == "upload") SceneUpload();
            else if (op == "delete") SceneDelete(arg);
            else if (op == "autosave") g_autosaveSec = atof(arg.c_str());
            Log("[ui] scene %s %s -> open='%s' dirty=%d", op.c_str(), arg.c_str(), g_sceneName.c_str(), (int)g_sceneDirty.load());
        }
        else if (req.rfind("!favclick ", 0) == 0)
        {
            const std::string cat = req.substr(10);
            g_placeCat = cat == "fav" ? std::string("*fav") : cat;
            g_paletteFilter[0] = 0;
            g_showPlace = true;
            g_favClickArm = 3;
            Log("[ui] favclick armed in '%s'", g_placeCat.c_str());
        }
        else if (req.rfind("!uidrag ", 0) == 0)
        {
            int a = 0, b = 0, c = 0, d = 0;
            sscanf_s(req.c_str() + 8, "%d %d %d %d", &a, &b, &c, &d);
            StartSyntheticDrag(a, b, c, d);
            Log("[ui] synthetic drag (%d,%d) -> (%d,%d)", a, b, c, d);
        }
        else if (req == "!dup") { g_placeBypass = true; DuplicateSelected(snap); g_placeBypass = false; Log("[ui] duplicate: %zu pending", g_dupPending.size()); }
        else if (req.rfind("!gm", 0) == 0 || req.rfind("!expectgm ", 0) == 0) GmTestOp(snap, req.substr(1));
        else if (req.rfind("!placeui ", 0) == 0)       // placeui <palette name>: place through the UI path (gated)
        {
            const PaletteItem* it = FindItem(snap, req.substr(9));
            if (it) SpawnInFront(snap, *it);
            Log("[ui] placeui %s -> %s (level '%s')", req.substr(9).c_str(), it ? (g_sceneName.empty() ? "REFUSED" : "placed") : "no such item",
                g_sceneName.c_str());
        }
        else if (req.rfind("!fav ", 0) == 0 || req.rfind("!expectfav ", 0) == 0)
        {
            const bool check = req[1] == 'e';
            std::string arg = req.substr(check ? 11 : 5), want;
            if (check) { const size_t sp = arg.rfind(' '); want = arg.substr(sp + 1); arg = arg.substr(0, sp); }
            const PaletteItem* hit = nullptr;
            for (const auto& it : snap.palette) if (ContainsCi(it.name, arg.c_str())) { hit = &it; break; }
            if (!hit) Log("[ui] FAIL fav: no palette item like '%s'", arg.c_str());
            else if (!check) { ToggleFav(hit->path); Log("[ui] fav %s -> %d (%zu favorite(s))", hit->name.c_str(), IsFav(hit->path), g_favorites.size()); }
            else
            {
                FavLoad();                                            // re-read the file: it must have been saved
                const bool on = IsFav(hit->path);
                bool listed = false;                                  // and it shows under Place Actors > Favorites
                for (const auto& p : g_favorites) if (p == hit->path && FindItem(snap, p)) listed = true;
                const bool ok = on == (want == "1") && listed == on;
                Log("[ui] %s expectfav %s: favorite=%d listed=%d (wanted %s)", ok ? "PASS" : "FAIL", hit->name.c_str(), on, listed, want.c_str());
            }
        }
        else if (req.rfind("!gmove ", 0) == 0)
        {
            Vec3 dv{};
            sscanf_s(req.c_str() + 7, "%lf %lf %lf", &dv.x, &dv.y, &dv.z);
            UiGroupMove(snap, dv);
        }
        else if (req == "!sel")
        {
            Log("[ui] SELECTION primary=%s extras=%zu", g_selected.c_str(), g_multiSel.size());
            for (const auto& o : snap.objects)
                if (o.handle == g_selected || IsMultiSel(o.handle))
                    Log("[ui]   %s %s at (%.0f,%.0f,%.0f) rot(%.0f,%.0f,%.0f) scale(%.2f,%.2f,%.2f)", o.handle == g_selected ? "*" : "+",
                        o.className.c_str(), o.location.x, o.location.y, o.location.z, o.rotation.pitch, o.rotation.yaw, o.rotation.roll,
                        o.scale.x, o.scale.y, o.scale.z);
        }
        else { SelectHandle(req); Log("[ui] selected %s (test script)", req.c_str()); }
    }

    const SceneObject* sel = nullptr;
    for (const auto& o : snap.objects)
        if (o.handle == g_selected) { sel = &o; break; }
    // Extras. A moved sandbox object is rebuilt by the server as a new actor (new handle) under the same id,
    // exactly like the gizmo's own object below: find it again -- same class, near where it was or was just
    // sent -- instead of dropping it from the selection. Only give up on one gone for 10 s (deleted).
    if (!snap.objects.empty())
    {
        const double t = ImGui::GetTime();
        for (auto it = g_multiSel.begin(); it != g_multiSel.end();)
        {
            const std::string& h = *it;
            if (h == g_selected) { it = g_multiSel.erase(it); continue; }
            const SceneObject* here = nullptr;
            for (const auto& o : snap.objects) if (o.handle == h) { here = &o; break; }
            MultiKnown& k = g_multiKnown[h];
            if (here) { k.cls = here->className; if (g_dragAxis < 0) k.at = here->location; k.lostAt = -1.0; ++it; continue; }
            if (k.lostAt < 0.0) k.lostAt = t;
            const SceneObject* best = nullptr;
            double bestD = 150.0 * 150.0;
            for (const auto& o : snap.objects)
            {
                if (o.className != k.cls || o.handle == g_selected || IsMultiSel(o.handle)) continue;
                const Vec3 d = Sub(o.location, k.at);
                if (Dot(d, d) < bestD) { bestD = Dot(d, d); best = &o; }
            }
            if (best)
            {
                MultiKnown moved = k;
                moved.lostAt = -1.0;
                g_multiKnown.erase(h);
                *it = best->handle;
                g_multiKnown[best->handle] = moved;
                ++it;
            }
            else if (t - k.lostAt > 10.0) { g_multiKnown.erase(h); it = g_multiSel.erase(it); }
            else ++it;
        }
    }
    // A rebuilt object (script attached / removed, slot wired) comes back as a new actor under the same id: keep
    // it selected -- find the same kind of object where the selection was -- so Details (and its Game data,
    // scripts and slots) stays up instead of going blank.
    {
        static std::string s_cls;
        static Vec3 s_at;
        static double s_lostAt = -1.0;
        if (sel) { s_cls = sel->className; s_at = sel->location; s_lostAt = -1.0; }
        else if (!g_selected.empty() && !s_cls.empty())
        {
            if (s_lostAt < 0.0) s_lostAt = ImGui::GetTime();
            if (ImGui::GetTime() - s_lostAt < 10.0)
            {
                const SceneObject* best = nullptr;
                double bestD = 100.0 * 100.0;
                for (const auto& o : snap.objects)
                {
                    if (o.className != s_cls || IsMultiSel(o.handle)) continue;   // not one of the extras
                    const Vec3 d = Sub(o.location, s_at);
                    if (Dot(d, d) < bestD) { bestD = Dot(d, d); best = &o; }
                }
                if (best)
                {
                    g_selected = best->handle;
                    Command c{ CmdType::SelectObject }; c.str = g_selected; State().Push(c);
                    sel = best;
                    s_lostAt = -1.0;
                }
            }
        }
    }

    HandleShortcuts();
    DupSelectTick(snap);
    SceneTick(snap);

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
    // Ctrl+D duplicates the selection in place and selects the copies, as in Unity.
    if (sel && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_D))
        DuplicateSelected(snap);

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
    if (g_gmWindow)                                     // the Game Modes window: roomy, resizable, its own thing
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2((std::min)(1100.0f, vp->WorkSize.x * 0.8f), (std::min)(780.0f, vp->WorkSize.y * 0.85f)), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Game Modes##gmwin", &g_gmWindow, ImGuiWindowFlags_NoCollapse)) DrawGameModes(snap, true);
        ImGui::End();
    }
    DrawGameModeBoxes(snap);
    HandleAssetDrag(snap);
    AdoptPreviewCoins(snap);
    ResendChangedScripts();
    WatchLuauErrors();
    DrawProblems();
}

// Test-script entry (se_game.cpp): the red coin run publish, through the UI's own path.
void ScriptCoinRun(const Snapshot& snap, const std::string& title, int seconds, int coins)
{
    ScriptPublishCoinRun(snap, title, seconds, coins);
}

// Test-script entries for the coin preview flow: place N preview coins (real objects) on a new red coin run
// draft, report how many were adopted, and publish it through PublishQuest.
void ScriptCoinPreview(const Snapshot& snap, const std::string& title, int coins)
{
    g_quests.emplace_back();
    QuestDraft& q = g_quests.back();
    strncpy_s(q.title, title.c_str(), _TRUNCATE);
    q.kind = 1;
    q.buttonAt = InFront(snap, 250.0);
    q.hasButton = true;
    g_questSel = (int)g_quests.size() - 1;
    const PaletteItem* coin = FindItem(snap, "LE_BP_RedCoin_C");
    for (int i = 0; coin && i < coins; ++i)
    {
        const Vec3 at{ q.buttonAt.x + 300.0 * (i + 1), q.buttonAt.y, q.buttonAt.z + 80.0 };
        Command c{ CmdType::SpawnItem }; c.str = coin->path; c.loc = at; State().Push(c);
        q.pendingCoins.push_back({ at, ImGui::GetTime() });
    }
}
int ScriptCoinPreviewAdopted()
{
    return g_quests.empty() ? -1 : (int)g_quests.back().coinObjs.size();
}
void ScriptCoinPreviewPublish()
{
    if (!g_quests.empty()) PublishQuest(g_quests.back());
}
// Test-script entry: construction mode on the last red coin run draft, and one click aimed at `target`
// (the same command a real click pushes).
void ScriptConstructClick(const Snapshot& snap, const Vec3& target)
{
    if (g_quests.empty() || g_quests.back().kind != 1) ScriptCoinPreview(snap, "Construction test", 0);
    if (g_coinPlaceQuest != (int)g_quests.size() - 1) { g_coinPlaceQuest = (int)g_quests.size() - 1; g_clickPlacedSeen = snap.clickPlaced.size(); }
    const PaletteItem* coin = FindItem(snap, "LE_BP_RedCoin_C");
    Vec3 d = Sub(target, snap.cameraPos);
    const double len = std::sqrt(Dot(d, d));
    if (!coin || len < 1.0) return;
    d = { d.x / len, d.y / len, d.z / len };
    Command c{ CmdType::PlaceTraced }; c.str = coin->path; c.loc = snap.cameraPos; c.dir = d; State().Push(c);
}

// Test-script entry: attach <RigelScripts>/<name>.luau to an object, exactly like the Attach button.
bool ScriptAttachFile(const std::string& handle, const std::string& name)
{
    ListScripts();
    for (const auto& f : g_scriptFiles)
        if (f.name == name) { SceneObject so; so.handle = handle; AttachScriptFile(&so, f, f.name); return true; }
    return false;
}

}  // namespace se
