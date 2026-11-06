#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HaivenChat.h"
#include "HaivenFrames.h"
#include "HaivenSubsystem.h"

#include <string>
#include <vector>

// The plugin's chat (FHaivenChat over the C++ SDK's FChatCore). The core's
// state machine is sdks/cpp/test/chat_test.cpp's; this proves the engine half:
// a chat.send reply decoded through the generated contract codec, the Blueprint
// projection of the outbox, a send that survives a closed socket, and the
// subsystem's reflected chat surface.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenChatBridge,
	"Haiven.Chat.Bridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace HaivenChatTestsDetail
{
	struct FWire
	{
		std::vector<Haiven::FClientFrame> Written;
		int Next = 0;
		std::string Prefix;
		Haiven::FSessionCore Session;

		explicit FWire(std::string InPrefix)
			: Prefix(std::move(InPrefix))
			, Session(
				  [this](const Haiven::FClientFrame& Frame) { Written.push_back(Frame); },
				  [] { return std::string("token"); },
				  [this] { return Prefix + std::to_string(++Next); })
		{
		}

		std::vector<Haiven::FClientFrame> Sends() const
		{
			std::vector<Haiven::FClientFrame> Out;
			for (const Haiven::FClientFrame& Frame : Written)
				if (Frame.Type == "chat.send") Out.push_back(Frame);
			return Out;
		}

		void Authenticate()
		{
			Session.Opened();
			Deliver(R"({"type":"auth.success","requestId":"","success":true})");
		}

		/** Parses Json as the plugin's socket handler does, then hands it to the session. */
		bool Deliver(const std::string& Json)
		{
			Haiven::FServerFrame Frame;
			if (!Haiven::ParseServerFrame(Json, Frame)) return false;
			Session.Receive(Frame, Json);
			return true;
		}
	};

	std::string Accepted(const std::string& RequestId, const std::string& ModerationState)
	{
		return R"({"type":"chat.send","requestId":")" + RequestId
			+ R"(","success":true,"payload":{"message":{"channelId":"ch-1","id":"m-)" + RequestId
			+ R"("},"moderationState":")" + ModerationState + R"("}})";
	}
}

bool FHaivenChatBridge::RunTest(const FString& Parameters)
{
	using namespace HaivenChatTestsDetail;
	int Ids = 0;
	FHaivenChat Chat([&Ids] { return "cm-" + std::to_string(++Ids); });
	TArray<FHaivenOutgoingChatMessage> Changes;
	Chat.OnChanged = [&Changes](const FHaivenOutgoingChatMessage& Message) { Changes.Add(Message); };

	FHaivenOutgoingChatMessage Message;
	FString Error;
	TestFalse(TEXT("an empty channel id sends nothing"), Chat.Send(FString(), TEXT("hi"), TOptional<FString>(), Message, Error));
	TestFalse(TEXT("and says why"), Error.IsEmpty());

	TestTrue(TEXT("a send before any session is kept"), Chat.Send(TEXT("ch-1"), TEXT("first"), TOptional<FString>(TEXT("p-1")), Message, Error));
	TestTrue(TEXT("as unconfirmed, with its parent"), Message.State == EHaivenOutgoingChatState::Unconfirmed
		&& Message.ClientMessageId == TEXT("cm-1") && Message.bHasParentMessageId && Message.ParentMessageId == TEXT("p-1"));

	TUniquePtr<FWire> First = MakeUnique<FWire>("a-");
	First->Session.OnReply = [&Chat](const Haiven::FServerFrame& Frame, const std::string& Json) { Chat.HandleReply(Frame, Json); };
	First->Session.OnClosed = [&Chat](int, const std::string&) { Chat.HandleClosed(); };
	Chat.Attach(First->Session);
	First->Authenticate();
	TestEqual(TEXT("attaching resends it after auth"), static_cast<int32>(First->Sends().size()), 1);
	if (First->Sends().size() != 1) return false;

	TestTrue(TEXT("an accepted reply parses"), First->Deliver(Accepted(First->Sends()[0].RequestId, "allowed")));
	TArray<FHaivenOutgoingChatMessage> Outbox = Chat.Outbox();
	TestTrue(TEXT("the decoded reply accepts it with the server's id"), Outbox.Num() == 1
		&& Outbox[0].State == EHaivenOutgoingChatState::Accepted
		&& Outbox[0].bHasMessageId && Outbox[0].MessageId == TEXT("m-a-1")
		&& Outbox[0].bHasModerationState && Outbox[0].ModerationState == TEXT("allowed"));

	Chat.Send(TEXT("ch-1"), TEXT("shadowed"), TOptional<FString>(), Message, Error);
	First->Deliver(Accepted(First->Sends()[1].RequestId, "quarantined"));
	TestTrue(TEXT("a moderation state this build does not know is held, kept verbatim"),
		Chat.Outbox()[1].State == EHaivenOutgoingChatState::Held && Chat.Outbox()[1].ModerationState == TEXT("quarantined"));

	Chat.Send(TEXT("ch-1"), TEXT("garbled"), TOptional<FString>(), Message, Error);
	First->Deliver(R"({"type":"chat.send","requestId":")" + First->Sends()[2].RequestId + R"(","success":true,"payload":{"message":7}})");
	TestTrue(TEXT("a success whose payload does not decode is held, never visible"),
		Chat.Outbox()[2].State == EHaivenOutgoingChatState::Held && !Chat.Outbox()[2].bHasMessageId);

	Chat.Send(TEXT("ch-1"), TEXT("cut off"), TOptional<FString>(), Message, Error);
	const FString Lost = Message.ClientMessageId;
	First->Session.Closed(1006, "network");
	TestTrue(TEXT("a send unanswered at close is unconfirmed"), Chat.Outbox()[3].State == EHaivenOutgoingChatState::Unconfirmed);
	First.Reset();

	FWire Second("b-");
	Second.Session.OnReply = [&Chat](const Haiven::FServerFrame& Frame, const std::string& Json) { Chat.HandleReply(Frame, Json); };
	Chat.Attach(Second.Session);
	Second.Authenticate();
	TestTrue(TEXT("the next session resends only it, under the same client message id"), Second.Sends().size() == 1
		&& Second.Sends()[0].PayloadJson.has_value()
		&& Second.Sends()[0].PayloadJson->find("\"clientMessageId\":\"" + std::string(TCHAR_TO_UTF8(*Lost)) + "\"") != std::string::npos);
	if (Second.Sends().size() != 1) return false;
	Second.Deliver(R"({"type":"chat.send","requestId":")" + Second.Sends()[0].RequestId + R"(","success":false,"error":"content_too_long"})");
	TestTrue(TEXT("a refusal is final and keeps its code"), Chat.Outbox()[3].State == EHaivenOutgoingChatState::Refused
		&& Chat.Outbox()[3].bHasError && Chat.Outbox()[3].Error == TEXT("content_too_long"));

	TestTrue(TEXT("every change reached OnChanged in Blueprint form"), Changes.Num() >= 9
		&& Changes.Last().State == EHaivenOutgoingChatState::Refused);
	Chat.Forget(Lost);
	TestEqual(TEXT("forgetting drops it from the outbox"), Chat.Outbox().Num(), 3);

	UClass* Subsystem = UHaivenSubsystem::StaticClass();
	for (const TCHAR* Name : {TEXT("SendChatMessage"), TEXT("GetChatOutbox"), TEXT("ForgetChatMessage")})
	{
		const UFunction* Function = Subsystem->FindFunctionByName(Name);
		TestTrue(FString::Printf(TEXT("%s is a Blueprint function"), Name), Function != nullptr && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}
	TestNotNull(TEXT("the outbox delegate is assignable"), Subsystem->FindPropertyByName(TEXT("OnOutgoingChatChanged")));
	const UFunction* Node = UHaivenSendChatMessageAsyncAction::StaticClass()->FindFunctionByName(TEXT("SendChatMessage"));
	TestTrue(TEXT("the send node is Blueprint-callable"), Node != nullptr && Node->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	for (const TCHAR* Pin : {TEXT("Sent"), TEXT("Settled"), TEXT("Refused"), TEXT("Failed")})
	{
		TestNotNull(FString::Printf(TEXT("the send node has a %s pin"), Pin), UHaivenSendChatMessageAsyncAction::StaticClass()->FindPropertyByName(Pin));
	}
	return true;
}
#endif
