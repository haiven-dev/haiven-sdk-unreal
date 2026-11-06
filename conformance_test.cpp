// Runner #4 of the published gaming corpus, and the C++ one the vocabulary was
// being rationed for.
//
// `conformance/README.md` says the six verbs stayed six because each had to be
// implementable in GDScript and C++, "not only in TypeScript" — and until now
// the C++ half of that constraint was a prediction. Three runners agreeing
// proves the vocabulary survives three languages that all have a garbage
// collector, an event loop or both.
//
// WHAT THIS DRIVES. `Haiven::FSessionCore`, the engine-independent core the
// `IWebSocket` adapter drives in `HaivenNetworking.cpp` — not a raw socket. The
// node and dotnet runners drive a bare transcript client on purpose, so a
// binding bug cannot mask a server assertion; this one deliberately does the
// opposite, for the same reason `godot/conformance_test.gd` does: the question
// #sdkconformance names is whether the binding can be driven by the step
// vocabulary at all. A raw arm is kept anyway — `OnFrame` rather than
// `OnReply`/`OnEvent` — because the core consumes `auth.success` and
// `auth.expired` into semantic state, so a runner reading the classified arms
// would see nothing for the handshake and could not tell that from a server
// that sent nothing.
//
// WHY IT IS NOT AN AUTOMATION TEST TODAY. `gaming/v0/profiles/unreal.yaml`
// names `runner: unreal-automation` and requires
// `engine-independent-protocol-core`, and that second requirement is what this
// executes. An `FAutomationTestBase` case would need an engine, and no engine
// is installed on the machines this repository is checked on —
// `#sdkunrealeditorci` still owns provisioned runners. A conformance runner
// that could only run where they are would be the Unity WebGL transport again
// (F20): written, reviewed, never once executed. The engine-side half is the
// transport, and `host/` already builds and ticks under both pinned
// engines; what it has never done is contact a live fixture, which
// `README.md` records as the open engine-integration case.

#include "Haiven/Source/ThirdParty/HaivenCpp/include/HaivenProtocol.h"
#include "Haiven/Source/ThirdParty/HaivenCpp/include/HaivenSessionCore.h"
#include "conformance/Json.h"
#include "conformance/Transport.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using HaivenConformance::FJson;
using HaivenConformance::FUrl;
using HaivenConformance::FWebSocket;

namespace
{
	/// This runner's name in the corpus, from `gaming/v0/profiles/unreal.yaml`.
	constexpr const char* Runner = "unreal-automation";

	/// The published target profile this runner is.
	constexpr const char* Profile = "gaming/unreal";

	/// What this runner's token source and fixture can supply, declared rather
	/// than inferred. A scenario needing something absent from this list has to
	/// waive; without the declaration it would fail deep in a transcript as a
	/// protocol error and read as a server bug.
	const std::vector<std::string> Capabilities = {"token.valid", "token.expired", "fixture.push"};

	std::vector<std::string> Failures;
	int Checks = 0;
	int Executed = 0;

	void Check(const std::string& What, bool bCondition)
	{
		++Checks;
		if (!bCondition) Failures.push_back(What);
	}

	std::string Env(const char* Name)
	{
		const char* Value = std::getenv(Name);
		return Value != nullptr ? std::string(Value) : std::string();
	}

	long long NowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	/// Percent-encode a query value. Only `-1s` and `1h` pass through here, but
	/// spelling it out keeps a future ttl from silently truncating the query.
	std::string UrlEncode(const std::string& Value)
	{
		static const char* Digits = "0123456789ABCDEF";
		std::string Out;
		for (const unsigned char Char : Value)
		{
			const bool bSafe = (Char >= 'a' && Char <= 'z') || (Char >= 'A' && Char <= 'Z')
				|| (Char >= '0' && Char <= '9') || Char == '-' || Char == '_' || Char == '.' || Char == '~';
			if (bSafe)
			{
				Out.push_back(static_cast<char>(Char));
			}
			else
			{
				Out.push_back('%');
				Out.push_back(Digits[Char >> 4]);
				Out.push_back(Digits[Char & 0x0F]);
			}
		}
		return Out;
	}

	/// The engine's `ParseServerFrame`, restated in plain C++.
	///
	/// Restated rather than shared because the engine version is written against
	/// `FJsonObject`. Keeping the two in the same shape is the point: presence
	/// of `success` decides reply-versus-event, `requestId` is optional and its
	/// empty string is meaningful, and `payload` is observed only for presence.
	bool ParseServerFrame(const FJson& Object, Haiven::FServerFrame& OutFrame)
	{
		if (!Object.IsObject()) return false;
		const FJson* Type = Object.Find("type");
		if (Type == nullptr || Type->Kind != FJson::EKind::String) return false;
		OutFrame.Type = Type->String;

		if (const FJson* RequestId = Object.Find("requestId");
			RequestId != nullptr && RequestId->Kind == FJson::EKind::String)
		{
			OutFrame.RequestId = RequestId->String;
		}
		if (const FJson* Success = Object.Find("success");
			Success != nullptr && Success->Kind == FJson::EKind::Bool)
		{
			OutFrame.Success = Success->Bool;
		}
		if (const FJson* Error = Object.Find("error");
			Error != nullptr && Error->Kind == FJson::EKind::String)
		{
			OutFrame.Error = Error->String;
		}
		OutFrame.bHasPayload = Object.Has("payload");
		return true;
	}

	/// The engine's `WriteClientFrame`, restated in plain C++.
	///
	/// The `auth` special case is load-bearing and is why this cannot just
	/// serialize the payload uniformly: an auth frame's payload is the token,
	/// written as a JSON *string*, while every other payload is an embedded
	/// object. Getting that wrong authenticates nobody and reads as a server
	/// that rejects valid tokens.
	std::string SerializeClientFrame(const Haiven::FClientFrame& Frame)
	{
		FJson Object = FJson::MakeObject();
		Object.Set("type", FJson::MakeString(Frame.Type));
		Object.Set("requestId", FJson::MakeString(Frame.RequestId));
		if (Frame.PayloadJson.has_value())
		{
			if (Frame.Type == Haiven::AuthType)
			{
				Object.Set("payload", FJson::MakeString(*Frame.PayloadJson));
			}
			else
			{
				FJson Payload;
				std::string Error;
				if (!FJson::Parse(*Frame.PayloadJson, Payload, Error)) return std::string();
				Object.Set("payload", Payload);
			}
		}
		return Object.Dump();
	}

	/// One scenario's live connection: socket, core, and the raw inbox.
	///
	/// The inbox is filled from the core's raw arm and drained by `expect`,
	/// which is the adaptation the pull-shaped corpus needs from a push-shaped
	/// binding — the same one every other runner had to make.
	struct FRun
	{
		FWebSocket Socket;
		std::unique_ptr<Haiven::FSessionCore> Session;
		std::vector<FJson> Inbox;
		std::string Token;
		std::string NextRequestId;
		std::string Error;
	};

	bool Deliver(FRun& Run, const std::string& Raw)
	{
		FJson Object;
		std::string ParseError;
		if (!FJson::Parse(Raw, Object, ParseError))
		{
			Run.Error = "the server sent something that is not JSON: " + ParseError;
			return false;
		}
		Haiven::FServerFrame Frame;
		if (!ParseServerFrame(Object, Frame))
		{
			Run.Error = "the server sent a frame with no type: " + Raw;
			return false;
		}
		Run.Session->Receive(Frame, Raw);
		return true;
	}

	/// Keep receiving for `Ms`, which is the whole reason `expectSilence` is
	/// expressible here. A sleep would assert an absence by not looking.
	bool Pump(FRun& Run, int Ms)
	{
		const long long Deadline = NowMs() + Ms;
		do
		{
			std::vector<std::string> Messages;
			std::string Error;
			const int Slice = static_cast<int>(std::min<long long>(10, std::max<long long>(0, Deadline - NowMs())));
			if (!Run.Socket.Pump(Slice, Messages, Error))
			{
				Run.Error = Error;
				return false;
			}
			for (const std::string& Message : Messages)
			{
				if (!Deliver(Run, Message)) return false;
			}
		} while (NowMs() < Deadline);
		return true;
	}

	bool NextFrame(FRun& Run, FJson& Out, int Ms = 3000)
	{
		const long long Deadline = NowMs() + Ms;
		while (NowMs() < Deadline)
		{
			if (!Run.Inbox.empty())
			{
				Out = Run.Inbox.front();
				Run.Inbox.erase(Run.Inbox.begin());
				return true;
			}
			if (!Pump(Run, 10)) return false;
		}
		return false;
	}

	struct FFixture
	{
		FUrl Base;
		std::string WsUrl;

		/// A token of the kind a scenario asks for, never a ttl.
		///
		/// `valid` and `expired` are properties the server observes; a ttl is a
		/// fixture affordance. This source mints, so it supplies both — which is
		/// why the corpus waives exactly one scenario for `deployment` rather
		/// than all of them.
		std::string Token(const std::string& Kind)
		{
			const std::string Ttl = Kind == "expired" ? "-1s" : "1h";
			HaivenConformance::FHttpResponse Response;
			std::string Error;
			if (!HaivenConformance::HttpRequest(
					Base, "GET", "/control/token?ttl=" + UrlEncode(Ttl), "", "", Response, Error)
				|| Response.Status != 200)
			{
				Failures.push_back("could not mint a " + Kind + " token: "
					+ (Error.empty() ? std::to_string(Response.Status) : Error));
				return std::string();
			}
			FJson Body;
			std::string ParseError;
			if (!FJson::Parse(Response.Body, Body, ParseError) || !Body.IsObject())
			{
				Failures.push_back("the token endpoint did not answer an object");
				return std::string();
			}
			return Body.StringOr("token");
		}

		bool Push(const FJson& Frame)
		{
			HaivenConformance::FHttpResponse Response;
			std::string Error;
			if (!HaivenConformance::HttpRequest(
					Base, "POST", "/control/push", "application/json", Frame.Dump(), Response, Error))
			{
				return false;
			}
			// Any 2xx. The endpoint answers 202, not 200 — it hands the frame to
			// the broker and the write pump delivers it, so a runner testing for
			// equality with 200 reads a working push as a refusal.
			return Response.Status >= 200 && Response.Status < 300;
		}
	};

	std::string Join(const std::vector<std::string>& Values)
	{
		std::ostringstream Out;
		for (std::size_t Index = 0; Index < Values.size(); ++Index)
		{
			if (Index != 0) Out << ", ";
			Out << Values[Index];
		}
		return Out.str();
	}

	void RunScenario(FFixture& Fixture, const FJson& Scenario)
	{
		const std::string Id = Scenario.StringOr("id", "?");
		const FJson* Steps = Scenario.Find("steps");
		if (Steps == nullptr || Steps->Kind != FJson::EKind::Array)
		{
			Failures.push_back(Id + ": the scenario has no steps");
			return;
		}

		FRun Run;
		for (const FJson& Step : Steps->Items)
		{
			const std::string Verb = Step.StringOr("do");

			if (Verb == "connect")
			{
				// Genuinely a step of its own here, and that is the finding this
				// runner contributes. `godot/conformance_test.gd` has to defer
				// `connect` and do the work at `auth`, because an addon handed a
				// token callable handshakes itself the moment the socket opens.
				// `FSessionCore` does not: `Opened()` is an explicit call the
				// caller makes, so the socket can be open with nothing written
				// on it. The corpus's two-step shape is therefore expressible in
				// C++ without an adaptation, and the reason is the same property
				// the profile requires — an engine-independent core is one the
				// runner drives rather than one the transport drives.
				std::string Error;
				if (!Run.Socket.Connect(Fixture.WsUrl, Error))
				{
					Failures.push_back(Id + ": could not open " + Fixture.WsUrl + ": " + Error);
					return;
				}
				Run.Session = std::make_unique<Haiven::FSessionCore>(
					[&Run](const Haiven::FClientFrame& Frame)
					{
						const std::string Text = SerializeClientFrame(Frame);
						if (Text.empty())
						{
							Run.Error = "a client frame carried a payload that is not JSON";
							return;
						}
						std::string SendError;
						if (!Run.Socket.SendText(Text, SendError)) Run.Error = SendError;
					},
					[&Run]() { return Run.Token; },
					[&Run]() { return Run.NextRequestId; });
				Run.Session->OnFrame = [&Run](const Haiven::FServerFrame&, const std::string& Raw)
				{
					FJson Object;
					std::string ParseError;
					if (FJson::Parse(Raw, Object, ParseError)) Run.Inbox.push_back(Object);
				};
				Check(Id + ": the socket is open and nothing has been written",
					Run.Session->GetState() == Haiven::ESessionState::Pending);
			}
			else if (Verb == "auth")
			{
				if (Run.Session == nullptr)
				{
					Failures.push_back(Id + ": auth before connect");
					return;
				}
				Run.Token = Fixture.Token(Step.StringOr("token", "valid"));
				if (Run.Token.empty()) return;
				Run.NextRequestId = Step.StringOr("requestId");
				Run.Session->Opened();
				if (!Run.Error.empty())
				{
					Failures.push_back(Id + ": " + Run.Error);
					return;
				}
			}
			else if (Verb == "send")
			{
				const FJson* Frame = Step.Find("frame");
				if (Frame == nullptr || Run.Session == nullptr)
				{
					Failures.push_back(Id + ": send without a frame or a session");
					return;
				}
				Run.NextRequestId = Frame->StringOr("requestId");
				std::optional<std::string> Payload;
				if (const FJson* Body = Frame->Find("payload"); Body != nullptr)
				{
					Payload = Body->Dump();
				}
				Run.Session->Send(Frame->StringOr("type"), Payload);
				if (!Run.Error.empty())
				{
					Failures.push_back(Id + ": " + Run.Error);
					return;
				}
			}
			else if (Verb == "push")
			{
				const FJson* Frame = Step.Find("frame");
				if (Frame == nullptr || !Fixture.Push(*Frame))
				{
					Failures.push_back(Id + ": the fixture refused a push");
					return;
				}
			}
			else if (Verb == "expect")
			{
				FJson Arrived;
				if (!NextFrame(Run, Arrived))
				{
					Failures.push_back(Id + ": expected a " + Step.StringOr("type")
						+ " and nothing arrived" + (Run.Error.empty() ? "" : ": " + Run.Error));
					return;
				}
				Check(Id + ": expects a " + Step.StringOr("type"),
					Arrived.StringOr("type") == Step.StringOr("type"));

				if (const FJson* Wanted = Step.Find("keys");
					Wanted != nullptr && Wanted->Kind == FJson::EKind::Array)
				{
					std::vector<std::string> Expected;
					for (const FJson& Key : Wanted->Items) Expected.push_back(Key.String);
					std::vector<std::string> Actual = Arrived.Keys();
					std::sort(Expected.begin(), Expected.end());
					std::sort(Actual.begin(), Actual.end());
					Check(Id + ": exact key set, wanted {" + Join(Expected) + "} got {" + Join(Actual) + "}",
						Expected == Actual);
				}

				if (const FJson* Fields = Step.Find("fields"); Fields != nullptr && Fields->IsObject())
				{
					for (const auto& Field : Fields->Members)
					{
						const FJson* Actual = Arrived.Find(Field.first);
						Check(Id + ": " + Field.first,
							Actual != nullptr && Actual->Equals(Field.second));
					}
				}
			}
			else if (Verb == "expectSilence" || Verb == "expect_silence")
			{
				const int Ms = static_cast<int>(Step.NumberOr("ms", 500));
				if (!Pump(Run, Ms))
				{
					Failures.push_back(Id + ": " + Run.Error);
					return;
				}
				Check(Id + ": silence", Run.Inbox.empty());
			}
			else
			{
				Failures.push_back(Id + ": unknown step \"" + Verb + "\"");
				return;
			}
		}

		if (Run.Session != nullptr) Run.Session->Closed(1000, "done");
		Run.Socket.Close();
		++Executed;
	}
}

int main()
{
	const std::string SuitePath = Env("HAIVEN_CONFORMANCE_SUITE");
	const std::string BaseUrl = Env("HAIVEN_FIXTURE_BASE_URL");
	const std::string WsUrl = Env("HAIVEN_FIXTURE_WS_URL");

	if (SuitePath.empty() || BaseUrl.empty() || WsUrl.empty())
	{
		// Reported, never silent. A runner whose absence is indistinguishable
		// from its success is not a runner — the whole reason the local corpus
		// fork survived as long as it did.
		std::cout << "SKIP conformance: HAIVEN_CONFORMANCE_SUITE, HAIVEN_FIXTURE_BASE_URL "
					 "and HAIVEN_FIXTURE_WS_URL are required" << std::endl;
		return 0;
	}

	std::ifstream File(SuitePath);
	if (!File)
	{
		std::cout << "FAIL could not open the suite at " << SuitePath << std::endl;
		return 1;
	}
	std::ostringstream Raw;
	Raw << File.rdbuf();

	FJson Suite;
	std::string ParseError;
	if (!FJson::Parse(Raw.str(), Suite, ParseError) || !Suite.IsObject())
	{
		std::cout << "FAIL could not read the suite at " << SuitePath << ": " << ParseError << std::endl;
		return 1;
	}

	// Guarding the shape rather than assuming it: pointing this runner at the
	// http moderation suite would otherwise surface as a socket that never opens.
	Check("the suite is gaming/v0", Suite.StringOr("id") == "gaming/v0");
	const FJson* Fixture = Suite.Find("fixture");
	Check("over websocket", Fixture != nullptr && Fixture->StringOr("protocol") == "websocket");
	Check("corpus_version 1", Suite.NumberOr("corpus_version", -1) == 1);

	std::vector<std::string> Missing;
	if (Fixture != nullptr)
	{
		if (const FJson* Declared = Fixture->Find("capabilities");
			Declared != nullptr && Declared->Kind == FJson::EKind::Array)
		{
			for (const FJson& Capability : Declared->Items)
			{
				if (std::find(Capabilities.begin(), Capabilities.end(), Capability.String)
					== Capabilities.end())
				{
					Missing.push_back(Capability.String);
				}
			}
		}
	}
	Check("no capability this runner cannot supply: {" + Join(Missing) + "}", Missing.empty());

	// A waiver is a recorded gap, not a skip, and a runner answers to every name
	// it has: its runner id, its published profile, and the suite's own
	// lifecycle. Read from the document rather than hardcoded, so a waiver keyed
	// to a lifecycle lapses by itself when the suite is promoted — the trap
	// `moderation/v0` exposed, where both waivers are keyed by lifecycle because
	// what they record is a gap in the provider rather than in any one runner.
	const std::set<std::string> Claimed = {Runner, Profile, Suite.StringOr("lifecycle")};
	std::vector<const FJson*> Runnable;
	if (const FJson* Scenarios = Suite.Find("scenarios");
		Scenarios != nullptr && Scenarios->Kind == FJson::EKind::Array)
	{
		for (const FJson& Scenario : Scenarios->Items)
		{
			bool bWaived = false;
			if (const FJson* Waivers = Scenario.Find("waivers"); Waivers != nullptr && Waivers->IsObject())
			{
				for (const auto& Waiver : Waivers->Members)
				{
					if (Claimed.count(Waiver.first) == 0) continue;
					std::cout << "waived " << Scenario.StringOr("id") << ": "
							  << Waiver.second.String << std::endl;
					bWaived = true;
					break;
				}
			}
			if (!bWaived) Runnable.push_back(&Scenario);
		}
	}

	// A fully-waived suite passes while proving nothing.
	Check("has scenarios to run", !Runnable.empty());

	FFixture Live;
	std::string UrlError;
	if (!HaivenConformance::ParseUrl(BaseUrl, Live.Base, UrlError))
	{
		std::cout << "FAIL " << UrlError << std::endl;
		return 1;
	}
	Live.WsUrl = WsUrl;

	if (Failures.empty())
	{
		for (const FJson* Scenario : Runnable) RunScenario(Live, *Scenario);
	}

	if (Failures.empty())
	{
		std::cout << "ok   gaming/v0 through Haiven::FSessionCore, " << Executed << " scenarios ("
				  << Checks << " checks)" << std::endl;
		std::cout << "HAIVEN_TEST_PASS conformance " << Executed << std::endl;
		return 0;
	}

	for (const std::string& Failure : Failures) std::cout << "FAIL " << Failure << std::endl;
	return 1;
}
