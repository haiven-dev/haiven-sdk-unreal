// Does the discriminant survive into C++, and does the core stay compilable
// without the engine?
//
// Both halves matter. The first is the F10 question asked for the third time —
// C# needed `bool?` and an explicit presence check, GDScript needed `has()`, and
// this asks what C++ needs. The second is F20's lesson applied before it costs
// anything: the Unity WebGL transport was the only file here that nothing
// compiled, and an Unreal module written in engine types would have been the
// same file again, larger.
//
// So this compiles HaivenProtocol.h with an ordinary compiler and exercises it.
// The engine glue is not covered and does not pretend to be.

#include "Haiven/Source/ThirdParty/HaivenCpp/include/HaivenProtocol.h"
#include "Haiven/Source/ThirdParty/HaivenCpp/include/HaivenSessionCore.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{
	std::vector<std::string> Failures;

	void Check(const char* What, bool Condition)
	{
		if (!Condition)
		{
			Failures.emplace_back(What);
		}
	}

	/// A frame as it arrives from the wire, built the way a reader would build
	/// it: fields that were present are set, fields that were absent are not.
	Haiven::FServerFrame AcceptedReply()
	{
		Haiven::FServerFrame Frame;
		Frame.Type = "pong";
		Frame.RequestId = "rq-1";
		Frame.Success = true;
		return Frame;
	}

	Haiven::FServerFrame RefusedReply()
	{
		Haiven::FServerFrame Frame;
		Frame.Type = "chat.send";
		Frame.RequestId = "rq-2";
		Frame.Success = false;
		Frame.Error = "rate_limited";
		return Frame;
	}

	Haiven::FServerFrame Event()
	{
		Haiven::FServerFrame Frame;
		Frame.Type = "chat.message";
		Frame.bHasPayload = true;
		return Frame;
	}

	Haiven::FServerFrame ServerInitiatedReply()
	{
		Haiven::FServerFrame Frame;
		Frame.Type = "auth.expired";
		Frame.RequestId = std::string("");
		Frame.Success = false;
		return Frame;
	}
}

int main()
{
	const Haiven::FServerFrame Accepted = AcceptedReply();
	Check("an accepted reply is a reply", Haiven::IsReply(Accepted));
	Check("and reports its outcome", Haiven::Succeeded(Accepted));

	// The case a value-based discriminant destroys. This is the assertion that
	// fails first if `Success` is ever narrowed to `bool`, and the reason the
	// header carries a static_assert as well: a compile error names the cause,
	// where this only names the symptom.
	const Haiven::FServerFrame Refused = RefusedReply();
	Check("a refused reply is still a reply", Haiven::IsReply(Refused));
	Check("and did not read as success", !Haiven::Succeeded(Refused));
	Check("and carries a code rather than prose", Refused.Error == "rate_limited");

	const Haiven::FServerFrame Fanned = Event();
	Check("an event is not a reply", !Haiven::IsReply(Fanned));
	Check("because the field is absent", !Fanned.Success.has_value());
	Check("an event carries a payload", Fanned.bHasPayload);

	// Absent and empty are both on the wire and both mean something.
	const Haiven::FServerFrame Announced = ServerInitiatedReply();
	Check("a server-initiated reply is a reply", Haiven::IsReply(Announced));
	Check("with a present but empty request id", Haiven::IsServerInitiatedReply(Announced));
	Check("an event is not server-initiated, it is uncorrelated", !Haiven::IsServerInitiatedReply(Fanned));
	Check("and a correlated reply is not server-initiated", !Haiven::IsServerInitiatedReply(Accepted));

	// The collapse itself, written out so the cost is visible rather than
	// argued. This is what a binding that declared `bool Success` would compute.
	const bool CollapsedEvent = Fanned.Success.value_or(false);
	const bool CollapsedRefusal = Refused.Success.value_or(false);
	Check(
		"a bool cannot tell an event from a refused reply",
		CollapsedEvent == CollapsedRefusal && !Haiven::IsReply(Fanned) && Haiven::IsReply(Refused));

	// The engine-neutral session is the behavior reused by the IWebSocket adapter.
	// Keeping it here means auth ordering and re-auth remain measurable even when
	// no Epic engine installation is available.
	std::vector<Haiven::FClientFrame> Written;
	std::string Token = "token-one";
	int NextId = 0;
	Haiven::FSessionCore Session(
		[&](const Haiven::FClientFrame& Frame) { Written.push_back(Frame); },
		[&]() { return Token; },
		[&]() { return "session-" + std::to_string(++NextId); });
	int Authenticated = 0;
	int Expired = 0;
	int RawFrames = 0;
	int Replies = 0;
	int Events = 0;
	Session.OnAuthenticated = [&]() { ++Authenticated; };
	Session.OnExpired = [&]() { ++Expired; };
	Session.OnFrame = [&](const Haiven::FServerFrame&, const std::string&) { ++RawFrames; };
	Session.OnReply = [&](const Haiven::FServerFrame&, const std::string&) { ++Replies; };
	Session.OnEvent = [&](const Haiven::FServerFrame&, const std::string&) { ++Events; };

	const std::string QueuedId = Session.Send("chat.send", std::string("{\"body\":\"queued\"}"));
	Check("application frames queue before open", Written.empty());
	Session.Opened();
	Check("auth is the first frame", Written.size() == 1 && Written[0].Type == Haiven::AuthType);
	Check("auth reads the current token", Written[0].PayloadJson == "token-one");

	Haiven::FServerFrame AuthenticatedFrame;
	AuthenticatedFrame.Type = Haiven::AuthSuccessType;
	AuthenticatedFrame.RequestId = "session-2";
	AuthenticatedFrame.Success = true;
	Session.Receive(AuthenticatedFrame, "auth-success");
	Check("auth success makes the session ready", Session.GetState() == Haiven::ESessionState::Ready && Authenticated == 1);
	Check("queued frames flush after auth", Written.size() == 2 && Written[1].RequestId == QueuedId);

	Haiven::FServerFrame SessionRefused = Refused;
	SessionRefused.RequestId = QueuedId;
	Session.Receive(SessionRefused, "refused");
	Session.Receive(Fanned, "event");
	Check("refused replies and events stay distinct", Replies == 1 && Events == 1);
	Check("the raw arm observes auth and application frames", RawFrames == 3);

	const std::string InFlightId = Session.Send("presence.set", std::string("{}"));
	Token = "token-two";
	Session.Receive(Announced, "expired");
	Check("auth expiry transitions once", Session.GetState() == Haiven::ESessionState::Expired && Expired == 1);
	Check("re-auth rereads token ownership", Written.size() == 4 && Written[3].PayloadJson == "token-two");
	Session.Receive(Announced, "duplicate-expired");
	Check("an expiry episode sends one auth frame", Written.size() == 4 && Expired == 1);
	Haiven::FServerFrame Reauthenticated;
	Reauthenticated.Type = Haiven::AuthSuccessType;
	Reauthenticated.Success = true;
	Session.Receive(Reauthenticated, "reauthenticated");
	Check("a re-auth replays nothing until the server reports the frame dropped", Written.size() == 4 && !InFlightId.empty());
	Session.Closed(1000, "done");
	Check("close is terminal", Session.GetState() == Haiven::ESessionState::Closed);

	if (Failures.empty())
	{
		std::cout << "ok   C++ envelope and session core" << std::endl;
		return 0;
	}

	for (const std::string& Failure : Failures)
	{
		std::cout << "FAIL " << Failure << std::endl;
	}

	return 1;
}
