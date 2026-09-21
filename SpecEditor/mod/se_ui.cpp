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
    char        glyph[32] = "PKRClimb";
    int         repetition = 0;  // Once / Daily / Weekly / Monthly
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

// Draw the translate/rotate/scale gizmo over the viewport at the selection. This is a screen-space
// representation: we have the object's world transform from the snapshot but not the view matrix, so
// the gizmo is drawn at the screen centre as a manipulator whose drags map to world deltas along the
// chosen axis. That keeps it usable without duplicating the engine's projection.
float Snap(float v, float step, bool on);

// Interactive translate/rotate/scale gizmo drawn over the viewport at screen centre.
//
// We deliberately do NOT reproduce the engine's projection: the client mod has the object's world
// transform but not the view matrix, and guessing one would put the handles in the wrong place. So the
// gizmo is a screen-space manipulator - grab an axis handle and drag, and the drag maps to a world
// delta on that axis. That is exact for the axis you picked, which is what a gizmo is actually for,
// and it degrades honestly rather than drawing handles that do not line up with the object.
//
// Dragging emits a SetTransform command on release AND while moving, so other editors see it live
// through the server (the transform round-trips; it is not applied locally first).
int   g_dragAxis = -1;          // 0 X, 1 Y, 2 Z, -1 none
Vec3  g_dragStartLoc, g_dragStartScale;
Rot   g_dragStartRot;
ImVec2 g_dragStartMouse;

void EmitTransform(const SceneObject& o, const Vec3& loc, const Rot& rot, const Vec3& scale)
{
    Command c{ CmdType::SetTransform };
    c.str = o.handle;
    c.loc = loc; c.rot = rot; c.scale = scale;
    State().Push(c);
}

void DrawGizmoOverlay(const SceneObject* sel)
{
    if (!sel || g_gizmo == GizmoMode::Select) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 sz = ImGui::GetIO().DisplaySize;
    const ImVec2 c(sz.x * 0.5f, sz.y * 0.5f);
    const float  L = 90.0f;

    const ImU32 axisCol[3] = { IM_COL32(220, 70, 70, 255), IM_COL32(90, 200, 90, 255), IM_COL32(80, 140, 235, 255) };
    const ImU32 hot        = IM_COL32(255, 220, 90, 255);

    // Handle end points, in the familiar UE arrangement: X right, Y toward the viewer, Z up.
    const ImVec2 ends[3] = { ImVec2(c.x + L, c.y),
                             ImVec2(c.x + L * 0.62f, c.y + L * 0.62f),
                             ImVec2(c.x, c.y - L) };

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool uiWantsMouse = ImGui::GetIO().WantCaptureMouse;

    // Pick: nearest handle within a grab radius.
    int hover = -1;
    if (g_dragAxis < 0 && !uiWantsMouse)
    {
        float best = 18.0f;
        for (int i = 0; i < 3; ++i)
        {
            const float dx = mouse.x - ends[i].x, dy = mouse.y - ends[i].y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < best) { best = d; hover = i; }
        }
    }

    if (hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        g_dragAxis = hover;
        g_dragStartLoc = sel->location;
        g_dragStartRot = sel->rotation;
        g_dragStartScale = sel->scale;
        g_dragStartMouse = mouse;
    }
    if (g_dragAxis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_dragAxis = -1;

    if (g_dragAxis >= 0)
    {
        // Project the mouse delta onto the handle direction so dragging along the axis feels right.
        const ImVec2 dir(ends[g_dragAxis].x - c.x, ends[g_dragAxis].y - c.y);
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        const ImVec2 n(dir.x / len, dir.y / len);
        const ImVec2 md(mouse.x - g_dragStartMouse.x, mouse.y - g_dragStartMouse.y);
        const float along = md.x * n.x + md.y * n.y;

        Vec3 loc = g_dragStartLoc, scale = g_dragStartScale;
        Rot  rot = g_dragStartRot;
        if (g_gizmo == GizmoMode::Translate)
        {
            const double d = Snap(along * 2.0f, g_gridSnap, g_snapEnabled);
            if (g_dragAxis == 0) loc.x = g_dragStartLoc.x + d;
            if (g_dragAxis == 1) loc.y = g_dragStartLoc.y + d;
            if (g_dragAxis == 2) loc.z = g_dragStartLoc.z + d;
        }
        else if (g_gizmo == GizmoMode::Rotate)
        {
            const double d = Snap(along * 0.5f, g_rotSnap, g_snapEnabled);
            if (g_dragAxis == 0) rot.pitch = g_dragStartRot.pitch + d;
            if (g_dragAxis == 1) rot.yaw   = g_dragStartRot.yaw + d;
            if (g_dragAxis == 2) rot.roll  = g_dragStartRot.roll + d;
        }
        else
        {
            const double d = along * 0.01;
            if (g_dragAxis == 0) scale.x = (std::max)(0.01, g_dragStartScale.x + d);
            if (g_dragAxis == 1) scale.y = (std::max)(0.01, g_dragStartScale.y + d);
            if (g_dragAxis == 2) scale.z = (std::max)(0.01, g_dragStartScale.z + d);
        }
        if (!sel->lockedByOther) EmitTransform(*sel, loc, rot, scale);
    }

    if (g_gizmo == GizmoMode::Rotate)
    {
        for (int i = 0; i < 3; ++i)
            dl->AddCircle(c, L * (1.0f - 0.18f * i),
                          (g_dragAxis == i || hover == i) ? hot : axisCol[i], 48, 2.5f);
    }
    for (int i = 0; i < 3; ++i)
    {
        const ImU32 col = (g_dragAxis == i || hover == i) ? hot : axisCol[i];
        if (g_gizmo != GizmoMode::Rotate) dl->AddLine(c, ends[i], col, 3.0f);
        if (g_gizmo == GizmoMode::Scale)
            dl->AddRectFilled(ImVec2(ends[i].x - 5, ends[i].y - 5), ImVec2(ends[i].x + 5, ends[i].y + 5), col);
        else
            dl->AddCircleFilled(ends[i], 6.0f, col);
    }
    dl->AddCircleFilled(c, 4.0f, IM_COL32(230, 230, 230, 255));

    char buf[200];
    snprintf(buf, sizeof(buf), "%s  %s%s  |  %.1f, %.1f, %.1f", GizmoName(g_gizmo),
             g_worldSpace ? "World" : "Local",
             g_dragAxis >= 0 ? (g_dragAxis == 0 ? "  [X]" : g_dragAxis == 1 ? "  [Y]" : "  [Z]") : "",
             sel->location.x, sel->location.y, sel->location.z);
    dl->AddText(ImVec2(c.x + 14, c.y + 14), IM_COL32(235, 235, 235, 230), buf);
}

float Snap(float v, float step, bool on) { return (on && step > 0.0f) ? std::round(v / step) * step : v; }

void DrawMenuBar(const Snapshot& snap)
{
    if (!ImGui::BeginMainMenuBar()) return;
    ImGui::TextColored(ImVec4(0.35f, 0.65f, 1.0f, 1.0f), "SPEC EDITOR");
    ImGui::Separator();

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Refresh Palette")) State().Push({ CmdType::RefreshPalette });
        ImGui::Separator();
        if (ImGui::MenuItem(snap.inEditor ? "Exit Level Editor" : "Enter Level Editor"))
            State().Push({ snap.inEditor ? CmdType::ExitEditor : CmdType::EnterEditor });
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
        if (ImGui::MenuItem("Delete Selected", "Del", false, !g_selected.empty()))
        {
            Command c{ CmdType::DeleteObject }; c.str = g_selected;
            State().Push(c); g_selected.clear();
        }
        if (ImGui::MenuItem("Deselect", "Esc", false, !g_selected.empty()))
        {
            Command c{ CmdType::DeselectObject }; c.str = g_selected;
            State().Push(c); g_selected.clear();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Mode"))
    {
        if (ImGui::MenuItem("Select",   "Q", g_gizmo == GizmoMode::Select))    g_gizmo = GizmoMode::Select;
        if (ImGui::MenuItem("Move",     "W", g_gizmo == GizmoMode::Translate)) g_gizmo = GizmoMode::Translate;
        if (ImGui::MenuItem("Rotate",   "E", g_gizmo == GizmoMode::Rotate))    g_gizmo = GizmoMode::Rotate;
        if (ImGui::MenuItem("Scale",    "R", g_gizmo == GizmoMode::Scale))     g_gizmo = GizmoMode::Scale;
        ImGui::Separator();
        ImGui::MenuItem("World Space", nullptr, &g_worldSpace);
        ImGui::MenuItem("Snapping",    nullptr, &g_snapEnabled);
        ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::TextDisabled(snap.inEditor ? "EDITING" : "spectating");
    ImGui::Separator();
    ImGui::TextDisabled("%d objects", static_cast<int>(snap.objects.size()));
    ImGui::Separator();
    ImGui::TextDisabled("%s", snap.status.c_str());

    const float right = ImGui::GetWindowWidth() - 190.0f;
    ImGui::SameLine(right > 0 ? right : 0);
    ImGui::TextDisabled("INSERT hides | Q W E R");
    ImGui::EndMainMenuBar();
}

void DrawPalette(const Snapshot& snap)
{
    ImGui::Begin("Place Actors");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##pf", "Search LE items...", g_paletteFilter, sizeof(g_paletteFilter));
    ImGui::Separator();

    if (snap.palette.empty())
    {
        ImGui::TextWrapped("No palette yet. File > Refresh Palette once you are in a level.");
        ImGui::End();
        return;
    }

    std::string lastCat;
    ImGui::BeginChild("palette_list");
    for (const auto& it : snap.palette)
    {
        if (g_paletteFilter[0] && it.name.find(g_paletteFilter) == std::string::npos) continue;
        if (it.category != lastCat)
        {
            lastCat = it.category;
            ImGui::SeparatorText(lastCat.c_str());
        }
        ImGui::PushID(it.path.c_str());
        if (ImGui::Selectable(it.name.c_str(), false, 0, ImVec2(0, 20)))
        {
            // Spawn in front of the editor camera so it lands where you are looking.
            Command c{ CmdType::SpawnItem };
            c.str = it.path;
            const double yaw = snap.cameraRot.yaw * 3.14159265358979 / 180.0;
            c.loc.x = snap.cameraPos.x + std::cos(yaw) * 300.0;
            c.loc.y = snap.cameraPos.y + std::sin(yaw) * 300.0;
            c.loc.z = snap.cameraPos.z;
            if (g_snapEnabled)
            {
                c.loc.x = Snap(static_cast<float>(c.loc.x), g_gridSnap, true);
                c.loc.y = Snap(static_cast<float>(c.loc.y), g_gridSnap, true);
                c.loc.z = Snap(static_cast<float>(c.loc.z), g_gridSnap, true);
            }
            State().Push(c);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.path.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

void DrawOutliner(const Snapshot& snap)
{
    ImGui::Begin("World Outliner");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##of", "Search...", g_outlinerFilter, sizeof(g_outlinerFilter));
    ImGui::Separator();

    if (ImGui::BeginTable("outliner", 2,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type",  ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        for (const auto& o : snap.objects)
        {
            if (g_outlinerFilter[0] && o.label.find(g_outlinerFilter) == std::string::npos) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool isSel = (o.handle == g_selected);
            ImGui::PushID(o.handle.c_str());
            if (o.lockedByOther)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.55f, 0.2f, 1.0f));
                ImGui::Selectable(o.label.c_str(), isSel, ImGuiSelectableFlags_SpanAllColumns);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Locked by another editor");
            }
            else if (ImGui::Selectable(o.label.c_str(), isSel, ImGuiSelectableFlags_SpanAllColumns))
            {
                if (!g_selected.empty() && g_selected != o.handle)
                {
                    Command d{ CmdType::DeselectObject }; d.str = g_selected; State().Push(d);
                }
                g_selected = o.handle;
                Command c{ CmdType::SelectObject }; c.str = o.handle; State().Push(c);
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", o.className.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void DrawDetails(const Snapshot& snap, const SceneObject* sel)
{
    ImGui::Begin("Details");
    if (ImGui::BeginTabBar("details_tabs"))
    {
        if (ImGui::BeginTabItem("Details"))
        {
            g_activeTab = 0;
            if (!sel)
            {
                ImGui::TextDisabled("Select an object in the World Outliner.");
            }
            else
            {
                ImGui::Text("%s", sel->label.c_str());
                ImGui::TextDisabled("%s", sel->className.c_str());
                if (sel->lockedByOther)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1), "Locked by another editor - read only");
                ImGui::Separator();

                float loc[3] = { (float)sel->location.x, (float)sel->location.y, (float)sel->location.z };
                float rot[3] = { (float)sel->rotation.pitch, (float)sel->rotation.yaw, (float)sel->rotation.roll };
                float scl[3] = { (float)sel->scale.x, (float)sel->scale.y, (float)sel->scale.z };
                bool changed = false;
                ImGui::SeparatorText("Transform");
                changed |= ImGui::DragFloat3("Location", loc, 1.0f);
                changed |= ImGui::DragFloat3("Rotation", rot, 0.5f);
                changed |= ImGui::DragFloat3("Scale",    scl, 0.01f, 0.01f, 100.0f);

                if (changed && !sel->lockedByOther)
                {
                    Command c{ CmdType::SetTransform };
                    c.str = sel->handle;
                    c.loc = { Snap(loc[0], g_gridSnap, g_snapEnabled),
                              Snap(loc[1], g_gridSnap, g_snapEnabled),
                              Snap(loc[2], g_gridSnap, g_snapEnabled) };
                    c.rot = { Snap(rot[0], g_rotSnap, g_snapEnabled),
                              Snap(rot[1], g_rotSnap, g_snapEnabled),
                              Snap(rot[2], g_rotSnap, g_snapEnabled) };
                    c.scale = { scl[0], scl[1], scl[2] };
                    State().Push(c);
                }

                ImGui::SeparatorText("Snapping");
                ImGui::Checkbox("Enabled", &g_snapEnabled);
                ImGui::SetNextItemWidth(110); ImGui::DragFloat("Grid",     &g_gridSnap, 1.0f, 1.0f, 1000.0f);
                ImGui::SetNextItemWidth(110); ImGui::DragFloat("Rotation##s", &g_rotSnap, 1.0f, 1.0f, 90.0f);

                ImGui::SeparatorText("Quest");
                if (ImGui::Button("Add as Quest Step"))
                {
                    QuestStep s;
                    s.objectHandle = sel->handle;
                    s.label = sel->label;
                    g_quest.steps.push_back(s);
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Quests"))
        {
            g_activeTab = 1;
            ImGui::TextWrapped("Author a parkour quest from placed objects. Compiling sends the "
                               "definition to the server, so every client that joins sees and can play it.");
            ImGui::Separator();
            ImGui::InputText("Quest Id",  g_quest.name,  sizeof(g_quest.name));
            ImGui::InputText("Title",     g_quest.title, sizeof(g_quest.title));
            ImGui::InputText("Glyph Id",  g_quest.glyph, sizeof(g_quest.glyph));
            const char* reps[] = { "Once", "Daily", "Weekly", "Monthly" };
            ImGui::Combo("Repetition", &g_quest.repetition, reps, IM_ARRAYSIZE(reps));

            ImGui::SeparatorText("Steps");
            if (g_quest.steps.empty())
                ImGui::TextDisabled("Select an object and use Details > Add as Quest Step.");

            int remove = -1;
            for (int i = 0; i < (int)g_quest.steps.size(); ++i)
            {
                ImGui::PushID(i);
                ImGui::Text("%d.", i + 1);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(150);
                const char* kinds[] = { "Reach", "Collect", "Interact" };
                ImGui::Combo("##kind", &g_quest.steps[i].kind, kinds, IM_ARRAYSIZE(kinds));
                ImGui::SameLine();
                ImGui::TextDisabled("%s", g_quest.steps[i].label.c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - 60);
                if (ImGui::SmallButton("Remove")) remove = i;
                ImGui::PopID();
            }
            if (remove >= 0) g_quest.steps.erase(g_quest.steps.begin() + remove);

            ImGui::Separator();
            ImGui::BeginDisabled(g_quest.steps.empty());
            if (ImGui::Button("Compile & Publish to Server", ImVec2(-1, 28)))
            {
                for (const auto& s : g_quest.steps)
                {
                    Command c{ CmdType::QuestAddStep };
                    c.str = s.objectHandle;
                    c.str2 = std::to_string(s.kind);
                    State().Push(c);
                }
                Command c{ CmdType::QuestCompile };
                c.str = g_quest.name;
                c.str2 = g_quest.title;
                State().Push(c);
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void DrawToolbar()
{
    ImGui::Begin("Toolbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoScrollbar);
    struct { GizmoMode m; const char* label; } modes[] = {
        { GizmoMode::Select, "Select" }, { GizmoMode::Translate, "Move" },
        { GizmoMode::Rotate, "Rotate" }, { GizmoMode::Scale, "Scale" },
    };
    for (int i = 0; i < 4; ++i)
    {
        if (i) ImGui::SameLine();
        const bool on = g_gizmo == modes[i].m;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        if (ImGui::Button(modes[i].label, ImVec2(64, 0))) g_gizmo = modes[i].m;
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button(g_worldSpace ? "World" : "Local", ImVec2(60, 0))) g_worldSpace = !g_worldSpace;
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &g_snapEnabled);
    ImGui::End();
}

void HandleShortcuts()
{
    if (ImGui::GetIO().WantTextInput) return;
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
    HandleShortcuts();

    const SceneObject* sel = nullptr;
    for (const auto& o : snap.objects)
        if (o.handle == g_selected) { sel = &o; break; }

    DrawMenuBar(snap);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float top = vp->WorkPos.y;
    const float h   = vp->WorkSize.y;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, top), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, h * 0.72f), ImGuiCond_FirstUseEver);
    DrawPalette(snap);

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 360, top), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, h * 0.42f), ImGuiCond_FirstUseEver);
    DrawOutliner(snap);

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 360, top + h * 0.42f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, h * 0.58f), ImGuiCond_FirstUseEver);
    DrawDetails(snap, sel);

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 300, top + 6), ImGuiCond_FirstUseEver);
    DrawToolbar();

    DrawGizmoOverlay(sel);
}

}  // namespace se
