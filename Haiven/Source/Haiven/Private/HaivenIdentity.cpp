#include "HaivenIdentity.h"

bool UHaivenBrowserContextAdapter::PrepareBrowserContext_Implementation(FString&, FString& Error)
{
	Error = TEXT("The browser-context adapter does not implement Prepare Browser Context.");
	return false;
}

bool UHaivenBrowserContextAdapter::PostIdentityLink_Implementation(const FString&, const FString&, FString& Error)
{
	Error = TEXT("The browser-context adapter does not implement Post Identity Link.");
	return false;
}

void UHaivenBrowserContextAdapter::CloseBrowserContext_Implementation(const FString&)
{
}
