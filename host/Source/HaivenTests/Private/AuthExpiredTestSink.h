#pragma once

#include "CoreMinimal.h"
#include "AuthExpiredTestSink.generated.h"

class UHaivenSubsystem;

/** A game's OnAuthExpired listener: it supplies a new bearer from inside the broadcast. */
UCLASS()
class UHaivenAuthExpiredTestSink : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleAuthExpired();

	UPROPERTY()
	TObjectPtr<UHaivenSubsystem> Target;

	/** true: SetAccessToken (account replacement); false: RefreshAccessToken (same account). */
	bool bReplaceAccount = false;
	FString Token;
	FDateTime ExpiresAtUtc;
	int32 Calls = 0;
};
