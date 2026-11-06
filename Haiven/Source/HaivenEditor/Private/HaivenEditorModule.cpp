#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Framework/Docking/TabManager.h"
#include "HaivenSubsystem.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	const FName DiagnosticsTabName(TEXT("Haiven.Diagnostics"));

	UHaivenSubsystem* GetPlayingSubsystem()
	{
		if (!GEditor || !GEditor->PlayWorld) return nullptr;
		UGameInstance* GameInstance = GEditor->PlayWorld->GetGameInstance();
		return GameInstance ? GameInstance->GetSubsystem<UHaivenSubsystem>() : nullptr;
	}

	FText ConnectionStateText()
	{
		const UHaivenSubsystem* Subsystem = GetPlayingSubsystem();
		if (!Subsystem) return FText::FromString(TEXT("No Play In Editor session"));
		return UEnum::GetDisplayValueAsText(Subsystem->GetHealth().RealtimeState);
	}

	FText CredentialExpiryText()
	{
		const UHaivenSubsystem* Subsystem = GetPlayingSubsystem();
		if (!Subsystem) return FText::FromString(TEXT("No Play In Editor session"));
		const FHaivenHealth Health = Subsystem->GetHealth();
		if (!Health.bHasAccessTokenExpiry) return FText::FromString(TEXT("No credential expiry available"));
		const TCHAR* Status = Health.AccessTokenExpiresAtUtc > FDateTime::UtcNow() ? TEXT("valid") : TEXT("expired");
		return FText::FromString(FString::Printf(
			TEXT("%s UTC (%s)"), *Health.AccessTokenExpiresAtUtc.ToIso8601(), Status));
	}

	FText RecentFramesText()
	{
		const UHaivenSubsystem* Subsystem = GetPlayingSubsystem();
		if (!Subsystem) return FText::FromString(TEXT("No Play In Editor session"));
		const FHaivenHealth Health = Subsystem->GetHealth();
		const TArray<FHaivenRecentRealtimeFrame>& Frames = Health.RecentRealtimeFrames;
		if (Frames.IsEmpty()) return FText::FromString(TEXT("No realtime frames recorded"));

		FString Lines;
		for (const FHaivenRecentRealtimeFrame& Frame : Frames)
		{
			const TCHAR* Direction = Frame.Direction == EHaivenRealtimeFrameDirection::Sent ? TEXT("sent") : TEXT("received");
			Lines += FString::Printf(TEXT("%s  %s  %s\n"), *Frame.SeenAtUtc.ToIso8601(), Direction, *Frame.Type);
		}
		Lines.RemoveFromEnd(TEXT("\n"));
		return FText::FromString(Lines);
	}
}

class FHaivenEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			DiagnosticsTabName,
			FOnSpawnTab::CreateRaw(this, &FHaivenEditorModule::SpawnDiagnosticsTab))
			.SetDisplayName(FText::FromString(TEXT("Haiven Diagnostics")))
			.SetTooltipText(FText::FromString(TEXT("Inspect the active Haiven Play In Editor session.")))
			.SetMenuType(ETabSpawnerMenuType::Enabled);
	}

	virtual void ShutdownModule() override
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(DiagnosticsTabName);
	}

private:
	TSharedRef<SDockTab> SpawnDiagnosticsTab(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SBorder)
				.Padding(12.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Connection state")))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
						[
							SNew(STextBlock).Text_Lambda([]() { return ConnectionStateText(); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Credential expiry")))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
						[
							SNew(STextBlock).Text_Lambda([]() { return CredentialExpiryText(); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Recent realtime frames")))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
						[
							SNew(STextBlock).Text_Lambda([]() { return RecentFramesText(); })
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Known frame-type metadata only: unknown values are shown as <unrecognized>; payloads, request ids, chat content, and credentials are not retained.")))
							.AutoWrapText(true)
						]
					]
				]
			];
	}
};

IMPLEMENT_MODULE(FHaivenEditorModule, HaivenEditor)
