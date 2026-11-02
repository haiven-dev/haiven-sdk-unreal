#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HaivenSessionCore.h"

#include <vector>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenSessionAuthQueueReauth,
	"Haiven.Session.AuthQueueReauth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenSessionAuthQueueReauth::RunTest(const FString& Parameters)
{
	std::vector<Haiven::FClientFrame> Written;
	std::string Token = "one";
	int Id = 0;
	Haiven::FSessionCore Session(
		[&](const Haiven::FClientFrame& Frame) { Written.push_back(Frame); },
		[&]() { return Token; },
		[&]() { return "id-" + std::to_string(++Id); });

	const std::string QueuedId = Session.Send("chat.send", std::string("{}"));
	TestEqual(TEXT("application frame waits for auth"), static_cast<int32>(Written.size()), 0);
	Session.Opened();
	TestTrue(TEXT("auth is first"), Written.size() == 1 && Written[0].Type == Haiven::AuthType);

	Haiven::FServerFrame Success;
	Success.Type = Haiven::AuthSuccessType;
	Success.Success = true;
	Session.Receive(Success, "{}");
	TestTrue(TEXT("queued frame flushes"), Written.size() == 2 && Written[1].RequestId == QueuedId);

	const std::string InFlightId = Session.Send("presence.set", std::string("{}"));
	Token = "two";
	Haiven::FServerFrame Expired;
	Expired.Type = Haiven::AuthExpiredType;
	Expired.RequestId = std::string();
	Expired.Success = false;
	Session.Receive(Expired, "{}");
	TestTrue(TEXT("same session rereads token"), Written.size() == 4 && Written[3].PayloadJson == "two");
	Session.Receive(Expired, "{}");
	TestEqual(TEXT("one auth per expiry episode"), static_cast<int32>(Written.size()), 4);
	Haiven::FServerFrame LateReply;
	LateReply.Type = "chat.send";
	LateReply.RequestId = QueuedId;
	LateReply.Success = true;
	Session.Receive(LateReply, "{}");
	Session.Receive(Success, "{}");
	TestTrue(TEXT("a late answer is removed before replay"), Written.size() == 5
		&& Written[4].RequestId == InFlightId);
	return true;
}
#endif
