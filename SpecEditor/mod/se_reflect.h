// se_reflect.h - property reflection shared by the Spec Editor client mod and the HalcyonA2 server.
//
// The Details panel lists an actor's editable properties -- the variables its Blueprint declares and the
// Edit / BlueprintVisible properties of its native parents -- and lets the author change them. The client
// writes the value locally for instant feedback and sends it to the server, which writes it again
// authoritatively. Both sides MUST agree on what a property is and how a value is encoded, so the code
// lives here and both builds include it. It depends only on the Dumper-7 SDK.
//
// Layout (22284 SDK, Basic.hpp / CoreUObject_classes.hpp):
//   UStruct: SuperStruct @0x40, ChildProperties (FField*) @0x50
//   FField:  ClassPrivate (FFieldClass*) @0x08, Next @0x18, Name @0x20       FFieldClass: Name @0x00
//   FProperty: ArrayDim @0x30, ElementSize @0x34, PropertyFlags @0x38, Offset @0x44
//   FBoolProperty: FieldSize/ByteOffset/ByteMask/FieldMask @0x70..0x73
//   FByteProperty: Enum @0x70   FEnumProperty: Underlying @0x70, Enum @0x78
//   FStructProperty: Struct @0x70   FObjectPropertyBase: PropertyClass @0x70
//   UEnum: Names TArray<TPair<FName,int64>> @0x40 (stride 0x10)
//   UChildActorComponent: ChildActor @0x238
//
// Value encoding on the wire: bool "0"/"1"; numbers as text; enum as its integer value; name as text;
// vector/rotator "x,y,z"; colour "r,g,b,a"; strings and text as themselves. Object references are shown,
// never written. A string or text value is never built in our own memory: the engine makes it
// (Concat_StrStr / Conv_StringToText return engine-allocated values) and the property takes ownership,
// so the engine can later free or reference-count it as its own. The old value is leaked, not freed --
// a few bytes per edit, and freeing it from outside the engine's allocator would be the real bug.
//
// Replication: CPF_Net (0x20) marks a property the server sends to every client -- Quest players included --
// and CPF_RepNotify its OnRep function (FProperty::RepNotifyFunc @0x48), which clients run on arrival and
// which the server must run itself after a write, as UE's own code does. Anything else lives only where
// it is written.

#pragma once

#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace sereflect {

enum class PType { Unsupported, Bool, Float, Double, Int, Int64, Byte, Enum, Name, Str, Vector, Rotator, Color, Vector2D, Object, Text };

constexpr uint64_t CPF_Edit = 0x1, CPF_BlueprintVisible = 0x4, CPF_Net = 0x20, CPF_RepNotify = 0x100000000ull;

struct Prop
{
    SDK::FProperty* p = nullptr;
    PType           t = PType::Unsupported;
    std::string     name, owner;           // property name, declaring class
};

template <typename T> inline T At(const void* base, uintptr_t off) { return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(base) + off); }

inline std::string FieldClassName(SDK::FField* f)
{
    return f && f->ClassPrivate ? f->ClassPrivate->Name.ToString() : std::string();
}

inline PType TypeOf(SDK::FProperty* p)
{
    const std::string c = FieldClassName(p);
    if (c == "BoolProperty")   return PType::Bool;
    if (c == "FloatProperty")  return PType::Float;
    if (c == "DoubleProperty") return PType::Double;
    if (c == "IntProperty")    return PType::Int;
    if (c == "Int64Property")  return PType::Int64;
    if (c == "ByteProperty")   return At<void*>(p, 0x70) ? PType::Enum : PType::Byte;
    if (c == "EnumProperty")   return PType::Enum;
    if (c == "NameProperty")   return PType::Name;
    if (c == "StrProperty")    return PType::Str;
    if (c == "TextProperty")   return PType::Text;
    if (c == "ObjectProperty" || c == "ObjectPtrProperty") return PType::Object;
    if (c == "StructProperty")
    {
        auto* s = At<SDK::UObject*>(p, 0x70);
        const std::string n = s ? s->GetName() : std::string();
        if (n == "Vector")      return PType::Vector;
        if (n == "Rotator")     return PType::Rotator;
        if (n == "LinearColor") return PType::Color;
        if (n == "Vector2D")    return PType::Vector2D;
    }
    return PType::Unsupported;
}

inline bool Writable(PType t)
{
    return t != PType::Unsupported && t != PType::Object;
}

// Engine base classes whose properties are internals, not things an author tunes.
inline bool IsEngineBase(const std::string& cls)
{
    static const char* skip[] = { "Object", "Actor", "ActorComponent", "SceneComponent", "PrimitiveComponent",
                                  "MeshComponent", "StaticMeshComponent", "SkinnedMeshComponent",
                                  "SkeletalMeshComponent", "ChildActorComponent", "Pawn", "Info" };
    for (const char* s : skip) if (cls == s) return true;
    return false;
}

// Every editable property on `obj`, most-derived class first.
inline void List(SDK::UObject* obj, std::vector<Prop>& out)
{
    if (!obj) return;
    for (SDK::UStruct* s = obj->Class; s; s = s->SuperStruct)
    {
        const std::string owner = s->GetName();
        if (IsEngineBase(owner)) continue;
        for (SDK::FField* f = s->ChildProperties; f; f = f->Next)
        {
            auto* p = static_cast<SDK::FProperty*>(f);
            if (p->ArrayDim != 1) continue;
            if (!(p->PropertyFlags & (CPF_Edit | CPF_BlueprintVisible))) continue;
            const PType t = TypeOf(p);
            if (t == PType::Unsupported) continue;
            out.push_back({ p, t, f->Name.ToString(), owner });
            if (out.size() >= 256) return;
        }
    }
}

inline SDK::FProperty* Find(SDK::UObject* obj, const std::string& name, PType* type = nullptr)
{
    std::vector<Prop> all;
    List(obj, all);
    for (const auto& pr : all)
        if (pr.name == name) { if (type) *type = pr.t; return pr.p; }
    return nullptr;
}

inline uint8_t* Addr(SDK::UObject* obj, SDK::FProperty* p) { return reinterpret_cast<uint8_t*>(obj) + p->Offset; }

inline bool Replicated(SDK::FProperty* p) { return (p->PropertyFlags & CPF_Net) != 0; }

// The OnRep function a RepNotify property names, or null.
inline SDK::UFunction* RepNotifyOf(SDK::UObject* obj, SDK::FProperty* p)
{
    if (!(p->PropertyFlags & CPF_RepNotify) || !obj || !obj->Class) return nullptr;
    const SDK::FName fn = At<SDK::FName>(p, 0x48);
    const std::string name = fn.ToString();
    if (name.empty() || name == "None") return nullptr;
    for (SDK::UStruct* s = obj->Class; s; s = s->SuperStruct)
        for (SDK::UField* f = s->Children; f; f = f->Next)
            if (f->Name.ToString() == name) return static_cast<SDK::UFunction*>(f);
    return nullptr;
}

inline SDK::UObject* EnumOf(SDK::FProperty* p)
{
    const std::string c = FieldClassName(p);
    return c == "EnumProperty" ? At<SDK::UObject*>(p, 0x78) : At<SDK::UObject*>(p, 0x70);
}

inline std::vector<std::pair<std::string, int64_t>> EnumNames(SDK::UObject* e)
{
    std::vector<std::pair<std::string, int64_t>> out;
    if (!e) return out;
    const uint8_t* data = At<const uint8_t*>(e, 0x40);
    const int n = At<int32_t>(e, 0x48);
    for (int i = 0; data && i < n && i < 256; ++i)
    {
        const SDK::FName nm = *reinterpret_cast<const SDK::FName*>(data + i * 0x10);
        std::string s = nm.ToString();
        const size_t c = s.rfind("::");
        if (c != std::string::npos) s = s.substr(c + 2);
        if (s.size() > 4 && s.compare(s.size() - 4, 4, "_MAX") == 0) continue;
        out.push_back({ s, *reinterpret_cast<const int64_t*>(data + i * 0x10 + 8) });
    }
    return out;
}

inline int64_t ReadInt(SDK::UObject* obj, SDK::FProperty* p)
{
    const uint8_t* a = Addr(obj, p);
    switch (p->ElementSize) { case 1: return *a; case 2: return *reinterpret_cast<const int16_t*>(a);
                              case 4: return *reinterpret_cast<const int32_t*>(a); default: return *reinterpret_cast<const int64_t*>(a); }
}

inline std::string Read(SDK::UObject* obj, SDK::FProperty* p, PType t)
{
    const uint8_t* a = Addr(obj, p);
    char b[160];
    switch (t)
    {
    case PType::Bool:
    {
        const uint8_t off = At<uint8_t>(p, 0x71), mask = At<uint8_t>(p, 0x72);
        return (a[off] & mask) ? "1" : "0";
    }
    case PType::Float:   snprintf(b, sizeof(b), "%g", *reinterpret_cast<const float*>(a));  return b;
    case PType::Double:  snprintf(b, sizeof(b), "%g", *reinterpret_cast<const double*>(a)); return b;
    case PType::Int:     return std::to_string(*reinterpret_cast<const int32_t*>(a));
    case PType::Int64:   return std::to_string(*reinterpret_cast<const int64_t*>(a));
    case PType::Byte:    return std::to_string(*a);
    case PType::Enum:    return std::to_string(ReadInt(obj, p));
    case PType::Name:    return reinterpret_cast<const SDK::FName*>(a)->ToString();
    case PType::Str:
    {
        const wchar_t* w = *reinterpret_cast<const wchar_t* const*>(a);
        std::string s;
        for (int i = 0; w && w[i] && i < 240; ++i) s.push_back(static_cast<char>(w[i] < 128 ? w[i] : '?'));
        return s;
    }
    case PType::Text:
    {
        const auto* td = *reinterpret_cast<SDK::FTextImpl::FTextData* const*>(a);
        const wchar_t* w = td ? *reinterpret_cast<const wchar_t* const*>(reinterpret_cast<const uint8_t*>(td) + 0x28) : nullptr;
        std::string s;
        for (int i = 0; w && w[i] && i < 240; ++i) s.push_back(static_cast<char>(w[i] < 128 ? w[i] : '?'));
        return s;
    }
    case PType::Vector: case PType::Rotator:
    {
        const double* d = reinterpret_cast<const double*>(a);
        snprintf(b, sizeof(b), "%g,%g,%g", d[0], d[1], d[2]);
        return b;
    }
    case PType::Vector2D:
    {
        const double* d = reinterpret_cast<const double*>(a);
        snprintf(b, sizeof(b), "%g,%g", d[0], d[1]);
        return b;
    }
    case PType::Color:
    {
        const float* f = reinterpret_cast<const float*>(a);
        snprintf(b, sizeof(b), "%g,%g,%g,%g", f[0], f[1], f[2], f[3]);
        return b;
    }
    case PType::Object:
    {
        auto* o = *reinterpret_cast<SDK::UObject* const*>(a);
        return o ? o->GetName() : "None";
    }
    default: return "";
    }
}

inline int SplitNums(const std::string& s, double* out, int max)
{
    int n = 0;
    const char* c = s.c_str();
    while (*c && n < max)
    {
        char* end = nullptr;
        out[n++] = strtod(c, &end);
        if (end == c) return n - 1;
        c = end;
        while (*c == ',' || *c == ' ') ++c;
    }
    return n;
}

// Parse and write. Returns false (and writes nothing) for anything not writable or not parseable.
inline bool Write(SDK::UObject* obj, SDK::FProperty* p, PType t, const std::string& v)
{
    if (!Writable(t) || v.size() > 256) return false;
    uint8_t* a = Addr(obj, p);
    double d[4]{};
    switch (t)
    {
    case PType::Bool:
    {
        const uint8_t off = At<uint8_t>(p, 0x71), mask = At<uint8_t>(p, 0x72);
        if (v == "1" || v == "true") a[off] |= mask; else a[off] &= static_cast<uint8_t>(~mask);
        return true;
    }
    case PType::Float:  *reinterpret_cast<float*>(a)   = static_cast<float>(atof(v.c_str())); return true;
    case PType::Double: *reinterpret_cast<double*>(a)  = atof(v.c_str()); return true;
    case PType::Int:    *reinterpret_cast<int32_t*>(a) = atoi(v.c_str()); return true;
    case PType::Int64:  *reinterpret_cast<int64_t*>(a) = _atoi64(v.c_str()); return true;
    case PType::Byte:   *a = static_cast<uint8_t>(atoi(v.c_str())); return true;
    case PType::Enum:
    {
        const int64_t x = _atoi64(v.c_str());
        bool known = false;                                  // only a value the enum actually defines
        for (const auto& e : EnumNames(EnumOf(p))) if (e.second == x) { known = true; break; }
        if (!known) return false;
        switch (p->ElementSize) { case 1: *a = static_cast<uint8_t>(x); break; case 2: *reinterpret_cast<int16_t*>(a) = static_cast<int16_t>(x); break;
                                  case 4: *reinterpret_cast<int32_t*>(a) = static_cast<int32_t>(x); break; default: *reinterpret_cast<int64_t*>(a) = x; }
        return true;
    }
    case PType::Name:
    {
        const std::wstring w(v.begin(), v.end());
        const SDK::FName n = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(w.c_str()));
        memcpy(a, &n, sizeof(n));
        return true;
    }
    case PType::Str:
    {
        const std::wstring w(v.begin(), v.end());
        SDK::FString made = SDK::UKismetStringLibrary::Concat_StrStr(SDK::FString(w.c_str()), SDK::FString(L""));
        memcpy(a, &made, sizeof(made));                      // the property now owns the engine's buffer
        return true;
    }
    case PType::Text:
    {
        const std::wstring w(v.begin(), v.end());
        SDK::FText made = SDK::UKismetTextLibrary::Conv_StringToText(SDK::FString(w.c_str()));
        if (!made.TextData) return false;
        memcpy(a, &made, sizeof(made));                      // ...and the engine's text reference
        return true;
    }
    case PType::Vector: case PType::Rotator:
        if (SplitNums(v, d, 3) != 3) return false;
        memcpy(a, d, 3 * sizeof(double));
        return true;
    case PType::Vector2D:
        if (SplitNums(v, d, 2) != 2) return false;
        memcpy(a, d, 2 * sizeof(double));
        return true;
    case PType::Color:
    {
        if (SplitNums(v, d, 4) != 4) return false;
        float* f = reinterpret_cast<float*>(a);
        for (int i = 0; i < 4; ++i) f[i] = static_cast<float>(d[i]);
        return true;
    }
    default: return false;
    }
}

// Follow one object-reference property to the thing it points at -- but only into something `obj`
// itself owns: one of its own components, or the actor a ChildActorComponent spawned. That is what
// lets the author reach a prefab's inner quest actor, and it is also the fence that keeps edits inside
// the placed prefab rather than anywhere a reference happens to lead.
inline SDK::UObject* Hop(SDK::UObject* obj, const std::string& prop)
{
    PType t{};
    SDK::FProperty* p = Find(obj, prop, &t);
    if (!p || t != PType::Object) return nullptr;
    SDK::UObject* target = *reinterpret_cast<SDK::UObject* const*>(Addr(obj, p));
    if (!target || target->Outer != obj) return nullptr;
    if (target->Class && target->Class->GetName() == "ChildActorComponent")
        return At<SDK::UObject*>(target, 0x238);       // the actor it spawned
    return target;
}

// "A.B" -> obj.A.B. Empty path = obj. At most three hops.
inline SDK::UObject* Resolve(SDK::UObject* root, const std::string& path)
{
    SDK::UObject* cur = root;
    size_t at = 0;
    for (int hops = 0; cur && at < path.size(); ++hops)
    {
        if (hops >= 3) return nullptr;
        const size_t dot = path.find('.', at);
        cur = Hop(cur, path.substr(at, dot == std::string::npos ? std::string::npos : dot - at));
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
    return cur;
}

// ---- making a write take effect ----------------------------------------------------------------
// Writing memory changes the value but not what it drives: a text component keeps drawing its old
// render proxy, and a RepNotify property never runs its OnRep. AfterWrite does what the engine's own
// setters would have done. Both builds call it: the server after an edit, every modded client when the
// server's broadcast of that edit arrives.

inline bool IsA(SDK::UObject* obj, const char* cls)
{
    for (SDK::UStruct* s = obj ? obj->Class : nullptr; s; s = s->SuperStruct)
        if (s->GetName() == cls) return true;
    return false;
}

inline SDK::UFunction* FindFn(SDK::UObject* obj, const char* name)
{
    for (SDK::UStruct* s = obj ? obj->Class : nullptr; s; s = s->SuperStruct)
        for (SDK::UField* f = s->Children; f; f = f->Next)
            if (f->Name.ToString() == name) return static_cast<SDK::UFunction*>(f);
    return nullptr;
}

// ProcessEvent with FUNC_Native set, as the generated SDK does, so native thunks run directly.
inline bool CallFn(SDK::UObject* obj, SDK::UFunction* fn, void* parms)
{
    if (!obj || !fn) return false;
    const auto saved = fn->FunctionFlags;
    fn->FunctionFlags |= 0x400;
    obj->ProcessEvent(fn, parms);
    fn->FunctionFlags = saved;
    return true;
}

// Rebuild a primitive's render proxy (what MarkRenderStateDirty does), via the one BP-callable route:
// a text component re-sets its own text; anything else is hidden and shown again.
inline void RefreshRender(SDK::UObject* comp)
{
    if (IsA(comp, "TextRenderComponent"))
    {
        PType tt{};
        SDK::FProperty* tp = Find(comp, "Text", &tt);
        if (!tp || tt != PType::Text) return;
        uint8_t parms[0x10];                                // K2_SetText(const FText&): the current text
        memcpy(parms, Addr(comp, tp), sizeof(parms));
        CallFn(comp, FindFn(comp, "K2_SetText"), parms);
        return;
    }
    if (!IsA(comp, "PrimitiveComponent")) return;
    struct { bool v; } isVis{};
    if (!CallFn(comp, FindFn(comp, "IsVisible"), &isVis)) return;
    struct { bool v, propagate; } set{ !isVis.v, false };
    SDK::UFunction* setVis = FindFn(comp, "SetVisibility");
    CallFn(comp, setVis, &set);
    set.v = isVis.v;
    CallFn(comp, setVis, &set);
}

// `root` is the placed actor, `obj` the object the property lives on (root or something it owns).
inline void AfterWrite(SDK::UObject* root, SDK::UObject* obj, SDK::FProperty* p)
{
    if (SDK::UFunction* onRep = RepNotifyOf(obj, p))
    {
        uint8_t zero[64]{};                                 // OnRep_X() or OnRep_X(OldValue): old value unknown
        CallFn(obj, onRep, zero);
    }
    // The LE text prefab: its Luau "Text" string is what the stock sandbox feeds the renderer from, but
    // that script only runs for prefabs the sandbox spawned itself -- so push the string to the
    // TextRender here, which is what players actually see.
    if (IsA(obj, "TextComponent") && FieldClassName(p) == "StrProperty")
    {
        if (SDK::UObject* tr = Hop(root, "TextRender"))
        {
            PType tt{};
            if (SDK::FProperty* tp = Find(tr, "Text", &tt); tp && tt == PType::Text)
            {
                Write(tr, tp, tt, Read(obj, p, PType::Str));
                RefreshRender(tr);
            }
        }
        return;
    }
    RefreshRender(obj);
}

inline const char* TypeLabel(PType t)
{
    switch (t)
    {
    case PType::Bool: return "Boolean"; case PType::Float: return "Float"; case PType::Double: return "Double";
    case PType::Int: return "Integer"; case PType::Int64: return "Integer64"; case PType::Byte: return "Byte";
    case PType::Enum: return "Enum"; case PType::Name: return "Name"; case PType::Str: return "String";
    case PType::Vector: return "Vector"; case PType::Rotator: return "Rotator"; case PType::Color: return "Color";
    case PType::Vector2D: return "Vector2D"; case PType::Object: return "Object"; case PType::Text: return "Text"; default: return "?";
    }
}

}  // namespace sereflect
