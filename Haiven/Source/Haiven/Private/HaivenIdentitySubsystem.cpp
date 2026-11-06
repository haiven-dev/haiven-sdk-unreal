#include "HaivenSubsystem.h"

#include "Async/Async.h"
#include "HaivenRestApi.h"

#include <string>

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

	FHaivenMotd ToUnreal(const Haiven::MotdV0& Value)
	{
		FHaivenMotd Result;
		Result.Id = FromUtf8(Value.Id);
		Result.bHasTitle = Value.Title.has_value();
		if (Value.Title) Result.Title = FromUtf8(*Value.Title);
		Result.Body = FromUtf8(Value.Body);
		Result.bHasUrl = Value.Url.has_value();
		if (Value.Url) Result.Url = FromUtf8(*Value.Url);
		Result.StartsAt = FromUtf8(Value.StartsAt);
		Result.bHasEndsAt = Value.EndsAt.has_value();
		if (Value.EndsAt) Result.EndsAt = FromUtf8(*Value.EndsAt);
		return Result;
	}

	FHaivenSignInMethods ToUnreal(const Haiven::FSignInMethods& Value)
	{
		FHaivenSignInMethods Result;
		Result.bHasPassword = Value.bHasPassword;
		Result.Identities.Reserve(static_cast<int32>(Value.Identities.size()));
		for (const Haiven::FSignInIdentity& Identity : Value.Identities)
		{
			FHaivenSignInIdentity Converted;
			Converted.Provider = FromUtf8(Identity.Provider);
			Converted.Email = FromUtf8(Identity.Email);
			Result.Identities.Add(MoveTemp(Converted));
		}
		return Result;
	}

	FHaivenRestRequest ToUnreal(const Haiven::Rest::FRestRequest& Value)
	{
		FHaivenRestRequest Result;
		Result.Method = FromUtf8(Value.Method);
		Result.Path = FromUtf8(Value.Path);
		Result.Body = FromUtf8(Value.Body);
		Result.ContentType = FromUtf8(Value.ContentType);
		Result.ResponseSchema = FromUtf8(Value.ResponseSchema);
		Result.bNavigation = Value.bNavigation;
		return Result;
	}
}

void UHaivenSubsystem::ResetIdentity()
{
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	Identity = MakeUnique<Haiven::FIdentityCore>(
		[WeakThis](std::string& OutContext, std::string& OutError)
		{
			if (!IsInGameThread())
			{
				OutError = "haiven: browser contexts can only be prepared on the game thread";
				return false;
			}
			if (!WeakThis.IsValid() || !WeakThis->IdentityBrowserAdapter)
			{
				OutError = "haiven: identity linking requires a browser-context adapter";
				return false;
			}
			FString Context;
			FString Error;
			const bool bPrepared = WeakThis->IdentityBrowserAdapter->PrepareBrowserContext(Context, Error);
			OutContext = ToUtf8(Context);
			OutError = ToUtf8(Error);
			return bPrepared;
		},
		[WeakThis](const std::string& Context, const std::string& Url, std::string& OutError)
		{
			if (!IsInGameThread())
			{
				OutError = "haiven: browser navigation can only start on the game thread";
				return false;
			}
			if (!WeakThis.IsValid() || !WeakThis->IdentityBrowserAdapter)
			{
				OutError = "haiven: browser-context adapter became unavailable";
				return false;
			}
			FString Error;
			const bool bPosted = WeakThis->IdentityBrowserAdapter->PostIdentityLink(
				FromUtf8(Context), FromUtf8(Url), Error);
			OutError = ToUtf8(Error);
			return bPosted;
		},
		[WeakThis](const std::string& Context)
		{
			const FString UnrealContext = FromUtf8(Context);
			if (!IsInGameThread())
			{
				AsyncTask(ENamedThreads::GameThread, [WeakThis, UnrealContext]()
				{
					if (WeakThis.IsValid() && WeakThis->IdentityBrowserAdapter)
						WeakThis->IdentityBrowserAdapter->CloseBrowserContext(UnrealContext);
				});
				return;
			}
			if (WeakThis.IsValid() && WeakThis->IdentityBrowserAdapter)
			{
				WeakThis->IdentityBrowserAdapter->CloseBrowserContext(UnrealContext);
			}
		});
}

void UHaivenSubsystem::BroadcastIdentityDiagnosticOnGameThread(const FString& Error)
{
	if (IsInGameThread())
	{
		OnIdentityDiagnostic.Broadcast(Error);
		return;
	}
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Error]()
	{
		if (WeakThis.IsValid()) WeakThis->OnIdentityDiagnostic.Broadcast(Error);
	});
}

void UHaivenSubsystem::SetIdentityBrowserContextAdapter(UHaivenBrowserContextAdapter* Adapter)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
		TWeakObjectPtr<UHaivenBrowserContextAdapter> WeakAdapter(Adapter);
		const bool bClearAdapter = Adapter == nullptr;
		AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakAdapter, bClearAdapter]()
		{
			if (WeakThis.IsValid()) WeakThis->SetIdentityBrowserContextAdapter(
				bClearAdapter ? nullptr : WeakAdapter.Get());
		});
		return;
	}
	if (Identity) Identity->CancelLink();
	IdentityBrowserAdapter = Adapter;
}

bool UHaivenSubsystem::BeginIdentityLink(const FString& Provider, FString& Error)
{
	if (!IsInGameThread())
	{
		Error = TEXT("Identity linking must begin on the game thread.");
		return false;
	}
	if (!Identity)
	{
		Error = TEXT("The Haiven subsystem is not initialized.");
		return false;
	}
	if (RealtimeState != EHaivenRealtimeState::Ready)
	{
		Error = TEXT("Identity linking requires a ready realtime session for its completion event.");
		return false;
	}
	std::string CoreError;
	const bool bStarted = Identity->BeginLink(ToUtf8(ClientOptions.BaseUrl), ToUtf8(Provider.ToLower()), CoreError);
	Error = FromUtf8(CoreError);
	return bStarted;
}

void UHaivenSubsystem::CancelIdentityLink()
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis]()
		{
			if (WeakThis.IsValid()) WeakThis->CancelIdentityLink();
		});
		return;
	}
	if (Identity) Identity->CancelLink();
}

void UHaivenSubsystem::LoadMotd(const FString& IntegrationId)
{
	if (!IsInGameThread() || !Identity)
	{
		if (!IsInGameThread())
		{
			TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
			AsyncTask(ENamedThreads::GameThread, [WeakThis, IntegrationId]()
			{
				if (WeakThis.IsValid()) WeakThis->LoadMotd(IntegrationId);
			});
		}
		else
		{
			BroadcastIdentityDiagnosticOnGameThread(TEXT("Load MOTD requires an initialized subsystem."));
			OnMotdLoadFailed.Broadcast();
		}
		return;
	}
	if (IntegrationId.TrimStartAndEnd().IsEmpty())
	{
		BroadcastIdentityDiagnosticOnGameThread(TEXT("Load MOTD requires an integration id."));
		OnMotdLoadFailed.Broadcast();
		return;
	}
	const Haiven::FIdentityReadTicket Ticket = Identity->BeginMotdRead();
	const FHaivenRestRequest Operation = ToUnreal(Haiven::Rest::GetMotd(ToUtf8(IntegrationId)));
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	RequestPublic(Operation.Method, Operation.Path, Operation.Body, FHaivenHttpComplete::CreateLambda(
		[WeakThis, Ticket](const FHaivenHttpResult& Reply)
		{
			if (!WeakThis.IsValid() || !WeakThis->Identity) return;
			// A realtime message that arrived after this request owns the newer
			// revision. Its visible state has already been broadcast.
			if (!WeakThis->Identity->IsCurrent(Ticket)) return;
			bool bChanged = false;
			std::string Error;
			if (!WeakThis->Identity->CompleteMotdRead(
				Ticket, Reply.StatusCode, ToUtf8(Reply.Body), bChanged, Error))
			{
				WeakThis->BroadcastIdentityDiagnosticOnGameThread(FromUtf8(Error));
				WeakThis->OnMotdLoadFailed.Broadcast();
				return;
			}
			if (const Haiven::MotdV0* Visible = WeakThis->Identity->GetVisibleMotd())
			{
				const FHaivenMotd Message = ToUnreal(*Visible);
				WeakThis->OnMotdChanged.Broadcast(true, Message);
			}
			else
			{
				WeakThis->OnMotdChanged.Broadcast(false, FHaivenMotd());
			}
		}));
}

bool UHaivenSubsystem::DismissMotd()
{
	if (!IsInGameThread() || !Identity || !Identity->DismissMotd()) return false;
	OnMotdChanged.Broadcast(false, FHaivenMotd());
	return true;
}

bool UHaivenSubsystem::IsValidMotdActionUrl(const FString& Url)
{
	return Haiven::FIdentityCore::IsValidMotdActionUrl(ToUtf8(Url));
}

void UHaivenSubsystem::RefreshSignInMethods()
{
	if (!IsInGameThread() || !Identity)
	{
		if (!IsInGameThread())
		{
			TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
			AsyncTask(ENamedThreads::GameThread, [WeakThis]()
			{
				if (WeakThis.IsValid()) WeakThis->RefreshSignInMethods();
			});
		}
		else BroadcastIdentityDiagnosticOnGameThread(TEXT("Refresh Sign-In Methods requires an initialized subsystem."));
		return;
	}
	const Haiven::FIdentityReadTicket Ticket = Identity->BeginSignInMethodsRead();
	const FHaivenRestRequest Operation = ToUnreal(Haiven::Rest::GetUsersMeSignInMethods());
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	Request(Operation.Method, Operation.Path, Operation.Body, FHaivenHttpComplete::CreateLambda(
		[WeakThis, Ticket](const FHaivenHttpResult& Reply)
		{
			if (!WeakThis.IsValid() || !WeakThis->Identity) return;
			bool bChanged = false;
			std::string Error;
			if (!WeakThis->Identity->CompleteSignInMethodsRead(
				Ticket, Reply.StatusCode, ToUtf8(Reply.Body), bChanged, Error))
			{
				WeakThis->BroadcastIdentityDiagnosticOnGameThread(FromUtf8(Error));
				return;
			}
			if (bChanged)
				WeakThis->OnSignInMethodsChanged.Broadcast(ToUnreal(*WeakThis->Identity->GetSignInMethods()));
		}));
}

void UHaivenSubsystem::UnlinkIdentity(const FString& Provider)
{
	if (!IsInGameThread() || !Identity)
	{
		if (!IsInGameThread())
		{
			TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Provider]()
			{
				if (WeakThis.IsValid()) WeakThis->UnlinkIdentity(Provider);
			});
		}
		else BroadcastIdentityDiagnosticOnGameThread(TEXT("Unlink Identity requires an initialized subsystem."));
		return;
	}
	const std::string Normalized = ToUtf8(Provider.ToLower());
	if (!Haiven::FIdentityCore::IsProvider(Normalized))
	{
		BroadcastIdentityDiagnosticOnGameThread(TEXT("Identity provider must be google or apple."));
		return;
	}
	const Haiven::FIdentityMutationTicket Ticket = Identity->BeginAccountMutation();
	const FHaivenRestRequest Operation = ToUnreal(Haiven::Rest::DeleteUsersMeSignInMethodsSsoProvider(Normalized));
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	Request(Operation.Method, Operation.Path, Operation.Body, FHaivenHttpComplete::CreateLambda(
		[WeakThis, Ticket](const FHaivenHttpResult& Reply)
		{
			if (!WeakThis.IsValid() || !WeakThis->Identity || !WeakThis->Identity->IsCurrent(Ticket)) return;
			if (!Reply.bSucceeded)
			{
				WeakThis->BroadcastIdentityDiagnosticOnGameThread(Reply.Error);
				return;
			}
			// The write won. Invalidate every snapshot that began before it, even
			// if one of those GETs completed while DELETE was in flight, then issue
			// the authoritative post-mutation read.
			WeakThis->Identity->InvalidateSignInMethodsReads();
			WeakThis->RefreshSignInMethods();
		}));
}

bool UHaivenSubsystem::GetCurrentMotd(FHaivenMotd& Message) const
{
	if (!IsInGameThread() || !Identity) return false;
	const Haiven::MotdV0* Visible = Identity->GetVisibleMotd();
	if (Visible == nullptr) return false;
	Message = ToUnreal(*Visible);
	return true;
}

bool UHaivenSubsystem::GetCurrentSignInMethods(FHaivenSignInMethods& Methods) const
{
	if (!IsInGameThread() || !Identity || !Identity->GetSignInMethods()) return false;
	Methods = ToUnreal(*Identity->GetSignInMethods());
	return true;
}

void UHaivenSubsystem::HandleIdentityRealtimeEvent(const Haiven::FServerFrame& Frame, const std::string& Json)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Frame, Json]()
		{
			if (WeakThis.IsValid()) WeakThis->HandleIdentityRealtimeEvent(Frame, Json);
		});
		return;
	}
	if (!Identity) return;
	const Haiven::FIdentityEvent Event = Identity->HandleRealtimeEvent(Frame.Type, Json);
	if (Event.Kind == Haiven::EIdentityEventKind::Ignored) return;
	if (!Event.bValid)
	{
		BroadcastIdentityDiagnosticOnGameThread(FromUtf8(Event.Error));
		return;
	}
	if (Event.Kind == Haiven::EIdentityEventKind::Motd && Event.bChanged)
	{
		if (const Haiven::MotdV0* Visible = Identity->GetVisibleMotd())
			OnMotdChanged.Broadcast(true, ToUnreal(*Visible));
		else
			OnMotdChanged.Broadcast(false, FHaivenMotd());
	}
	else if (Event.Kind == Haiven::EIdentityEventKind::Linked && Event.Linked)
	{
		OnIdentityLinked.Broadcast(FromUtf8(Event.Linked->Provider), FromUtf8(Event.Linked->Email));
		RefreshSignInMethods();
	}
	else if (Event.Kind == Haiven::EIdentityEventKind::LinkFailed && Event.LinkFailed)
	{
		OnIdentityLinkFailed.Broadcast(FromUtf8(Event.LinkFailed->Provider), FromUtf8(Event.LinkFailed->Code));
	}
}
