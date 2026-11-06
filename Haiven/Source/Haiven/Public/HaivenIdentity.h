#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "HaivenIdentity.generated.h"

/**
 * Host bridge for account linking. Implementations must keep one cookie jar for
 * Prepare Browser Context and Post Identity Link, and follow redirects in it.
 * A normal game HTTP request followed by opening a URL is not equivalent.
 */
UCLASS(Abstract, BlueprintType, Blueprintable)
class HAIVEN_API UHaivenBrowserContextAdapter : public UObject
{
	GENERATED_BODY()

public:
	/** Prepare/open the browser context now, before any request (popup-safe). */
	UFUNCTION(BlueprintNativeEvent, Category = "Haiven|Identity")
	bool PrepareBrowserContext(FString& ContextId, FString& Error);
	virtual bool PrepareBrowserContext_Implementation(FString& ContextId, FString& Error);

	/** POST-navigate the prepared context to Url and follow redirects there. */
	UFUNCTION(BlueprintNativeEvent, Category = "Haiven|Identity")
	bool PostIdentityLink(const FString& ContextId, const FString& Url, FString& Error);
	virtual bool PostIdentityLink_Implementation(const FString& ContextId, const FString& Url, FString& Error);

	UFUNCTION(BlueprintNativeEvent, Category = "Haiven|Identity")
	void CloseBrowserContext(const FString& ContextId);
	virtual void CloseBrowserContext_Implementation(const FString& ContextId);
};

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenMotd
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|MOTD") FString Id;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|MOTD") bool bHasTitle = false;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|MOTD") FString Title;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|MOTD") FString Body;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|MOTD") bool bHasUrl = false;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|MOTD") FString Url;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|MOTD") FString StartsAt;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|MOTD") bool bHasEndsAt = false;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|MOTD") FString EndsAt;
};

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenSignInIdentity
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Identity") FString Provider;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Identity") FString Email;
};

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenSignInMethods
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Identity") bool bHasPassword = false;
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Identity") TArray<FHaivenSignInIdentity> Identities;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHaivenMotdChanged, bool, bHasMessage, const FHaivenMotd&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FHaivenMotdLoadFailed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenSignInMethodsChanged, const FHaivenSignInMethods&, Methods);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHaivenIdentityLinked, const FString&, Provider, const FString&, Email);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHaivenIdentityLinkFailed, const FString&, Provider, const FString&, Code);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenIdentityDiagnostic, const FString&, Error);
