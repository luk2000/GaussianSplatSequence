// Copyright (c) GaussianSplatSequence contributors. MIT License.

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "SGSSPanel.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "GaussianSplatSequence"

namespace
{
	const FName GSSTabName(TEXT("GaussianSplatSequence"));
}

class FGaussianSplatSequenceModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GSSTabName,
			FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
			{
				return SNew(SDockTab)
					.TabRole(ETabRole::NomadTab)
					[
						SNew(SGSSPanel)
					];
			}))
			.SetDisplayName(LOCTEXT("TabTitle", "Gaussian Splat Sequence"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Export cameras to COLMAP, convert depth EXRs to point clouds and train splats in LichtFeld Studio."))
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Cinematics"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGaussianSplatSequenceModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GSSTabName);
		}
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		FToolMenuSection& Section = Menu->FindOrAddSection("GaussianSplatSequence", LOCTEXT("SectionLabel", "Gaussian Splatting"));
		Section.AddMenuEntry(
			"OpenGaussianSplatSequence",
			LOCTEXT("MenuEntry", "Gaussian Splat Sequence"),
			LOCTEXT("MenuEntryTooltip", "Open the Gaussian Splat Sequence panel."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Cinematics"),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				FGlobalTabmanager::Get()->TryInvokeTab(GSSTabName);
			})));
	}
};

IMPLEMENT_MODULE(FGaussianSplatSequenceModule, GaussianSplatSequence)

#undef LOCTEXT_NAMESPACE
