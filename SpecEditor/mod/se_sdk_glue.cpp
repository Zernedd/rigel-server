// se_sdk_glue.cpp - the few SDK symbols we implement ourselves.
//
// Dumper7's Basic.cpp calls UKismetStringLibrary::Conv_StringToName from GetStaticName (the
// STATIC_NAME_IMPL macro every class carries). Its generated body lives in Engine_functions.cpp, which
// is 5 MB of code we would otherwise compile in full for this single function. Provide it here instead,
// the same way the generated code would: find the UFunction once and call it through ProcessEvent.
//
// The temporary FunctionFlags |= 0x400 is FUNC_Native, and it is what the generated SDK does too: it
// makes ProcessEvent dispatch straight to the native implementation rather than walking bytecode.

#include "../../HalcyonA2/HalcyonA2/gamesdk/22284/SDK.hpp"
#include "../../HalcyonA2/HalcyonA2/gamesdk/22284/SDK/Engine_parameters.hpp"

namespace SDK
{

class FName UKismetStringLibrary::Conv_StringToName(const class FString& InString)
{
    static class UFunction* fn = nullptr;
    if (!fn)
    {
        auto* cls = UObject::FindClassFast("KismetStringLibrary");
        fn = cls ? cls->GetFunction("KismetStringLibrary", "Conv_StringToName") : nullptr;
    }
    FName out{};
    if (!fn) return out;

    struct { FString InString; FName ReturnValue; } parms{};
    parms.InString = InString;

    const auto saved = fn->FunctionFlags;
    fn->FunctionFlags |= 0x400;                       // FUNC_Native
    UKismetStringLibrary::GetDefaultObj()->ProcessEvent(fn, &parms);
    fn->FunctionFlags = saved;
    return parms.ReturnValue;
}

// Engine-allocated string and text values for the property editor (se_reflect.h Write): the property
// takes ownership of what these return, so the engine must be the one that allocated it.
static UFunction* LibFn(const char* cls, const char* fn)
{
    auto* c = UObject::FindClassFast(cls);
    return c ? c->GetFunction(cls, fn) : nullptr;
}

class FString UKismetStringLibrary::Concat_StrStr(const class FString& A, const class FString& B)
{
    static UFunction* fn = LibFn("KismetStringLibrary", "Concat_StrStr");
    struct { FString A; FString B; FString ReturnValue; } parms{};
    parms.A = A; parms.B = B;
    if (!fn) return parms.ReturnValue;
    const auto saved = fn->FunctionFlags;
    fn->FunctionFlags |= 0x400;
    UKismetStringLibrary::GetDefaultObj()->ProcessEvent(fn, &parms);
    fn->FunctionFlags = saved;
    return parms.ReturnValue;
}

class FText UKismetTextLibrary::Conv_StringToText(const class FString& InString)
{
    static UFunction* fn = LibFn("KismetTextLibrary", "Conv_StringToText");
    Params::KismetTextLibrary_Conv_StringToText parms{};
    parms.InString = InString;
    if (!fn) return parms.ReturnValue;
    const auto saved = fn->FunctionFlags;
    fn->FunctionFlags |= 0x400;
    UKismetTextLibrary::GetDefaultObj()->ProcessEvent(fn, &parms);
    fn->FunctionFlags = saved;
    return parms.ReturnValue;
}

}  // namespace SDK
