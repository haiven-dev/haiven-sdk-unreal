#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HaivenProtocol.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // This fixture maps Unreal JSON fields into the protocol core. It is not
    // an SDK JSON adapter or network transport implementation.
    bool ReadFixture(const FString& Json, Haiven::FServerFrame& Frame)
    {
        TSharedPtr<FJsonObject> Object;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid()) return false;
        FString Type;
        if (!Object->TryGetStringField(TEXT("type"), Type)) return false;
        Frame.Type = TCHAR_TO_UTF8(*Type);
        bool Success;
        if (Object->TryGetBoolField(TEXT("success"), Success)) Frame.Success = Success;
        FString RequestId;
        if (Object->TryGetStringField(TEXT("requestId"), RequestId)) Frame.RequestId = TCHAR_TO_UTF8(*RequestId);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHaivenPluginLoads, "Haiven.Plugin.Loads", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHaivenPluginLoads::RunTest(const FString& Parameters)
{
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("Haiven"));
    TestTrue(TEXT("plugin discovered"), Plugin.IsValid());
    if (Plugin.IsValid()) TestTrue(TEXT("plugin enabled"), Plugin->IsEnabled());
    TestTrue(TEXT("runtime module loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("Haiven")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHaivenRefusedReply, "Haiven.Protocol.RefusedReply", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHaivenRefusedReply::RunTest(const FString& Parameters)
{
    Haiven::FServerFrame Frame;
    if (!TestTrue(TEXT("parse fixture"), ReadFixture(TEXT("{\"type\":\"ping\",\"requestId\":\"req-1\",\"success\":false}"), Frame))) return false;
    TestTrue(TEXT("refused reply retains presence"), Haiven::IsReply(Frame));
    TestFalse(TEXT("refused reply is unsuccessful"), Haiven::Succeeded(Frame));
    TestFalse(TEXT("correlated reply is not server initiated"), Haiven::IsServerInitiatedReply(Frame));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHaivenEvent, "Haiven.Protocol.Event", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHaivenEvent::RunTest(const FString& Parameters)
{
    Haiven::FServerFrame Frame;
    if (!TestTrue(TEXT("parse fixture"), ReadFixture(TEXT("{\"type\":\"chat.message\"}"), Frame))) return false;
    TestFalse(TEXT("event is not a reply"), Haiven::IsReply(Frame));
    TestFalse(TEXT("event has no request id"), Frame.RequestId.has_value());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHaivenServerInitiatedReply, "Haiven.Protocol.ServerInitiatedReply", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHaivenServerInitiatedReply::RunTest(const FString& Parameters)
{
    Haiven::FServerFrame Frame;
    if (!TestTrue(TEXT("parse fixture"), ReadFixture(TEXT("{\"type\":\"auth.expired\",\"requestId\":\"\",\"success\":false}"), Frame))) return false;
    TestTrue(TEXT("empty present id denotes server initiation"), Haiven::IsServerInitiatedReply(Frame));
    return true;
}
#endif
