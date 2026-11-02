#include "HaivenAsyncActions.h"

#include "HaivenSubsystem.h"

UHaivenHttpRequestAsyncAction* UHaivenHttpRequestAsyncAction::HttpRequest(
	UObject* WorldContextObject,
	UHaivenSubsystem* Subsystem,
	const FString& Method,
	const FString& Path,
	const FString& BodyJson)
{
	UHaivenHttpRequestAsyncAction* Action = NewObject<UHaivenHttpRequestAsyncAction>();
	Action->Owner = Subsystem;
	Action->Verb = Method;
	Action->RequestPath = Path;
	Action->Body = BodyJson;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UHaivenHttpRequestAsyncAction::Activate()
{
	if (!Owner)
	{
		FHaivenHttpResult Result;
		Result.Error = TEXT("The Haiven subsystem is unavailable.");
		Finish(Result);
		return;
	}
	Handle = Owner->Request(Verb, RequestPath, Body, FHaivenHttpComplete::CreateUObject(this, &UHaivenHttpRequestAsyncAction::Finish));
}

void UHaivenHttpRequestAsyncAction::Cancel()
{
	if (Owner && Handle.IsValid()) Owner->CancelHttpRequest(Handle);
}

void UHaivenHttpRequestAsyncAction::Finish(const FHaivenHttpResult& Result)
{
	if (Result.bSucceeded) Completed.Broadcast(Result);
	else Failed.Broadcast(Result);
	SetReadyToDestroy();
}

UHaivenConnectRealtimeAsyncAction* UHaivenConnectRealtimeAsyncAction::ConnectRealtime(
	UObject* WorldContextObject,
	UHaivenSubsystem* Subsystem)
{
	UHaivenConnectRealtimeAsyncAction* Action = NewObject<UHaivenConnectRealtimeAsyncAction>();
	Action->Owner = Subsystem;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UHaivenConnectRealtimeAsyncAction::Activate()
{
	if (!Owner)
	{
		Finish(false, TEXT("The Haiven subsystem is unavailable."));
		return;
	}
	Owner->ConnectRealtime(FHaivenConnectComplete::CreateUObject(this, &UHaivenConnectRealtimeAsyncAction::Finish));
}

void UHaivenConnectRealtimeAsyncAction::Cancel()
{
	if (Owner) Owner->DisconnectRealtime();
}

void UHaivenConnectRealtimeAsyncAction::Finish(bool bSucceeded, const FString& Error)
{
	if (bSucceeded) Connected.Broadcast();
	else Failed.Broadcast(Error.IsEmpty() ? TEXT("The Haiven realtime connection closed before authentication.") : Error);
	SetReadyToDestroy();
}

UHaivenRealtimeRequestAsyncAction* UHaivenRealtimeRequestAsyncAction::RealtimeRequest(
	UObject* WorldContextObject,
	UHaivenSubsystem* Subsystem,
	const FString& Type,
	const FString& PayloadJson)
{
	UHaivenRealtimeRequestAsyncAction* Action = NewObject<UHaivenRealtimeRequestAsyncAction>();
	Action->Owner = Subsystem;
	Action->FrameType = Type;
	Action->Payload = PayloadJson;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UHaivenRealtimeRequestAsyncAction::Activate()
{
	if (!Owner)
	{
		FHaivenRealtimeReply Reply;
		Reply.Error = TEXT("The Haiven subsystem is unavailable.");
		Finish(Reply);
		return;
	}
	RequestId = Owner->SendRealtimeRequest(
		FrameType,
		Payload,
		FHaivenRealtimeComplete::CreateUObject(this, &UHaivenRealtimeRequestAsyncAction::Finish));
}

void UHaivenRealtimeRequestAsyncAction::Cancel()
{
	if (Owner && !RequestId.IsEmpty()) Owner->CancelRealtimeRequest(RequestId);
}

void UHaivenRealtimeRequestAsyncAction::Finish(const FHaivenRealtimeReply& Reply)
{
	if (Reply.bSucceeded) Completed.Broadcast(Reply);
	else Failed.Broadcast(Reply);
	SetReadyToDestroy();
}
