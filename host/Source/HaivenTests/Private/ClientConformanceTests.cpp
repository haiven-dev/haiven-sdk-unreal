#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Dom/JsonObject.h"
#include "HaivenClient.h"
#include "HaivenSessionCore.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
	std::string ToUtf8(const FString& Value)
	{
		const FTCHARToUTF8 Converted(*Value);
		return std::string(Converted.Get(), Converted.Length());
	}

	FString FromUtf8(const std::string& Value)
	{
		return FString(UTF8_TO_TCHAR(Value.c_str()));
	}

	FString StateName(Haiven::ESessionState State)
	{
		switch (State)
		{
		case Haiven::ESessionState::Pending:
		case Haiven::ESessionState::Authenticating:
			return TEXT("pending");
		case Haiven::ESessionState::Ready:
			return TEXT("authenticated");
		case Haiven::ESessionState::Expired:
			return TEXT("expired");
		case Haiven::ESessionState::Closed:
			return TEXT("closed");
		}
		return TEXT("unknown");
	}

	FString SerializeObject(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Text;
	}

	TSharedPtr<FJsonValue> Resolve(
		const TSharedPtr<FJsonValue>& Value,
		const TMap<FString, FString>& Bound,
		FString& Error)
	{
		if (!Value.IsValid())
		{
			Error = TEXT("encountered an invalid JSON value");
			return nullptr;
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (Object->Values.Num() == 1 && Object->HasField(TEXT("ref")))
			{
				FString Alias;
				if (!Object->TryGetStringField(TEXT("ref"), Alias))
				{
					Error = TEXT("a ref is not a string");
					return nullptr;
				}
				const FString* RequestId = Bound.Find(Alias);
				if (RequestId == nullptr)
				{
					Error = FString::Printf(TEXT("a ref names nothing bound: %s"), *Alias);
					return nullptr;
				}
				return MakeShared<FJsonValueString>(*RequestId);
			}

			const TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
			for (const auto& Member : Object->Values)
			{
				TSharedPtr<FJsonValue> Resolved = Resolve(Member.Value, Bound, Error);
				if (!Resolved.IsValid()) return nullptr;
				Copy->SetField(Member.Key, MoveTemp(Resolved));
			}
			return MakeShared<FJsonValueObject>(Copy);
		}
		if (Value->Type == EJson::Array)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				TSharedPtr<FJsonValue> Resolved = Resolve(Item, Bound, Error);
				if (!Resolved.IsValid()) return nullptr;
				Items.Add(MoveTemp(Resolved));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}
		return Value;
	}

	class FScriptedSocket final : public Haiven::IWebSocketTransport
	{
	public:
		void Connect(const std::string&, Haiven::FWebSocketEvents InEvents) override
		{
			Events = std::move(InEvents);
		}

		void Send(const std::string& Text) override
		{
			Sent.push_back(Text);
		}

		void Close(int, const std::string&) override
		{
		}

		Haiven::FWebSocketEvents Events;
		std::vector<std::string> Sent;
	};

	FString RunScenario(const TSharedPtr<FJsonObject>& Scenario)
	{
		FScriptedSocket Socket;
		int Counter = 0;
		Haiven::FClientOptions Options;
		Options.RealtimeUrl = "wss://scripted.invalid/v0";
		Haiven::FRealtimeClient Client(
			Socket,
			Options,
			[] { return std::string("scripted-token"); },
			[&Counter] { return "rq-" + std::to_string(++Counter); });
		std::string ConnectError;
		if (!Client.Connect(ConnectError)) return TEXT("connect: ") + FromUtf8(ConnectError);

		Haiven::FSessionCore& Session = Client.Session();
		std::string Unparsed;
		Client.OnMalformedFrame = [&Unparsed](const std::string& Text) { Unparsed = Text; };
		std::vector<Haiven::FServerFrame> Delivered;
		Session.OnFrame = [&Delivered](const Haiven::FServerFrame& Frame, const std::string&)
		{
			Delivered.push_back(Frame);
		};
		const std::vector<std::string>& Written = Socket.Sent;

		TMap<FString, FString> Bound;
		std::size_t Read = 0;
		std::size_t Seen = 0;
		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		if (!Scenario->TryGetArrayField(TEXT("steps"), Steps) || Steps == nullptr)
			return TEXT("no steps");

		for (int32 Index = 0; Index < Steps->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Step = (*Steps)[Index]->AsObject();
			if (!Step.IsValid()) return FString::Printf(TEXT("step %d is not an object"), Index + 1);
			FString Verb;
			Step->TryGetStringField(TEXT("do"), Verb);
			const FString At = FString::Printf(TEXT("step %d (%s)"), Index + 1, *Verb);

			if (Verb == TEXT("open"))
			{
				if (!Socket.Events.OnOpen) return At + TEXT(": socket has no open callback");
				Socket.Events.OnOpen();
			}
			else if (Verb == TEXT("call"))
			{
				FString Type;
				Step->TryGetStringField(TEXT("type"), Type);
				std::optional<std::string> Payload;
				if (Step->HasField(TEXT("payload")))
				{
					const TSharedPtr<FJsonObject> Body = Step->GetObjectField(TEXT("payload"));
					Payload = ToUtf8(SerializeObject(Body));
				}
				Session.Send(ToUtf8(Type), Payload);
			}
			else if (Verb == TEXT("serve"))
			{
				const TSharedPtr<FJsonValue>* Frame = Step->Values.Find(TEXT("frame"));
				if (Frame == nullptr) return At + TEXT(": no frame");
				FString ResolveError;
				const TSharedPtr<FJsonValue> Resolved = Resolve(*Frame, Bound, ResolveError);
				if (!Resolved.IsValid()) return At + TEXT(": ") + ResolveError;
				if (Resolved->Type != EJson::Object) return At + TEXT(": frame is not an object");
				Unparsed.clear();
				Socket.Events.OnText(ToUtf8(SerializeObject(Resolved->AsObject())));
				if (!Unparsed.empty()) return At + TEXT(": the frame does not parse");
			}
			else if (Verb == TEXT("expectSent"))
			{
				FString ExpectedType;
				Step->TryGetStringField(TEXT("type"), ExpectedType);
				if (Read >= Written.size()) return At + TEXT(": expected a ") + ExpectedType + TEXT(" and nothing was written");
				TSharedPtr<FJsonObject> Frame;
				const FString WrittenText = FromUtf8(Written[Read++]);
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(WrittenText);
				if (!FJsonSerializer::Deserialize(Reader, Frame) || !Frame.IsValid())
					return At + TEXT(": wrote text that is not a JSON object");
				FString Type;
				FString RequestId;
				Frame->TryGetStringField(TEXT("type"), Type);
				Frame->TryGetStringField(TEXT("requestId"), RequestId);
				if (Type != ExpectedType) return At + TEXT(": wrote ") + Type;
				FString Alias;
				if (Step->TryGetStringField(TEXT("bind"), Alias)) Bound.Add(Alias, RequestId);
				if (Step->TryGetStringField(TEXT("same"), Alias))
				{
					const FString* ExpectedRequestId = Bound.Find(Alias);
					if (ExpectedRequestId == nullptr || *ExpectedRequestId != RequestId)
						return At + TEXT(": wrote ") + RequestId + TEXT(", not ") + Alias;
				}
			}
			else if (Verb == TEXT("expectQuiet"))
			{
				if (Read < Written.size()) return At + TEXT(": also wrote ") + FromUtf8(Written[Read]);
			}
			else if (Verb == TEXT("expectState"))
			{
				FString ExpectedState;
				Step->TryGetStringField(TEXT("state"), ExpectedState);
				const FString State = StateName(Session.GetState());
				if (State != ExpectedState) return At + TEXT(": is ") + State;
			}
			else if (Verb == TEXT("expectDelivered"))
			{
				FString ExpectedType;
				Step->TryGetStringField(TEXT("type"), ExpectedType);
				std::size_t Found = Seen;
				while (Found < Delivered.size() && FromUtf8(Delivered[Found].Type) != ExpectedType) ++Found;
				if (Found == Delivered.size()) return At + TEXT(": no ") + ExpectedType + TEXT(" was handed to the caller");
				const Haiven::FServerFrame& Frame = Delivered[Found];
				Seen = Found + 1;
				FString ExpectedError;
				if (Step->TryGetStringField(TEXT("error"), ExpectedError)
					&& FromUtf8(Frame.Error.value_or("(none)")) != ExpectedError)
				{
					return At + TEXT(": error ") + FromUtf8(Frame.Error.value_or("(none)"));
				}
			}
			else
			{
				return At + TEXT(": unknown step");
			}
		}
		return FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenClientConformance,
	"Haiven.Conformance.Client",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenClientConformance::RunTest(const FString& Parameters)
{
	bool bPassed = true;
	const FString SuitePath = FPaths::Combine(
		FPaths::ProjectContentDir(), TEXT("HaivenTestFixtures"), TEXT("gaming-client-v0.json"));
	FString SuiteText;
	if (!FFileHelper::LoadFileToString(SuiteText, *SuitePath))
	{
		AddError(TEXT("Could not load the staged gaming-client/v0 suite: ") + SuitePath);
		return false;
	}

	TSharedPtr<FJsonObject> Suite;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SuiteText);
	if (!FJsonSerializer::Deserialize(Reader, Suite) || !Suite.IsValid())
	{
		AddError(TEXT("The staged gaming-client/v0 suite is not a JSON object."));
		return false;
	}

	FString Lifecycle;
	Suite->TryGetStringField(TEXT("lifecycle"), Lifecycle);
	const TSet<FString> Claimed = {
		TEXT("unreal-automation"),
		TEXT("gaming/unreal"),
		Lifecycle,
	};
	const TArray<TSharedPtr<FJsonValue>>* Scenarios = nullptr;
	if (!Suite->TryGetArrayField(TEXT("scenarios"), Scenarios) || Scenarios == nullptr)
	{
		AddError(TEXT("The staged gaming-client/v0 suite has no scenarios."));
		return false;
	}

	int32 Ran = 0;
	for (const TSharedPtr<FJsonValue>& ScenarioValue : *Scenarios)
	{
		const TSharedPtr<FJsonObject> Scenario = ScenarioValue->AsObject();
		if (!Scenario.IsValid())
		{
			AddError(TEXT("A gaming-client/v0 scenario is not an object."));
			bPassed = false;
			continue;
		}
		FString Id;
		Scenario->TryGetStringField(TEXT("id"), Id);
		bool bWaived = false;
		if (Scenario->HasTypedField<EJson::Object>(TEXT("waivers")))
		{
			const TSharedPtr<FJsonObject> Waivers = Scenario->GetObjectField(TEXT("waivers"));
			for (const FString& Claim : Claimed)
			{
				if (!Waivers->HasField(Claim)) continue;
				FString Reason;
				if (!Waivers->TryGetStringField(Claim, Reason))
				{
					AddError(FString::Printf(TEXT("%s: waiver %s is not a string"), *Id, *Claim));
					bPassed = false;
					bWaived = true;
					break;
				}
				AddInfo(FString::Printf(TEXT("waived %s: %s"), *Id, *Reason));
				bWaived = true;
			}
		}
		if (bWaived) continue;

		++Ran;
		const FString Failure = RunScenario(Scenario);
		if (!Failure.IsEmpty())
		{
			AddError(Id + TEXT(": ") + Failure);
			bPassed = false;
		}
	}

	if (Ran == 0)
	{
		AddError(TEXT("The gaming-client/v0 suite has no scenario this runner can run."));
		return false;
	}
	AddInfo(FString::Printf(TEXT("Ran %d gaming-client/v0 scenarios inside Unreal Automation."), Ran));
	return bPassed;
}
#endif
