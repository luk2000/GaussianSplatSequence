// Copyright (c) GaussianSplatSequence contributors. MIT License.

#include "SGSSPanel.h"

#include "GSSPipeline.h"
#include "GSSSettings.h"

#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IDetailsView.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GaussianSplatSequence"

void SGSSPanel::Construct(const FArguments& InArgs)
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.bAllowSearch = true;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsView = PropertyEditor.CreateDetailView(DetailsArgs);
	DetailsView->SetObject(UGSSSettings::Get());

	auto SectionHeader = [](const FText& Text)
	{
		return SNew(STextBlock)
			.Text(Text)
			.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"));
	};

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			DetailsView.ToSharedRef()
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SectionHeader(LOCTEXT("Step1", "1. Cameras -> COLMAP"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SWrapBox)
					.UseAllottedSize(true)
					+ SWrapBox::Slot().Padding(2)
					[
						MakeButton(LOCTEXT("ExportCurrent", "Export Current Camera"),
							LOCTEXT("ExportCurrentTip", "Exports the camera at the current Sequencer frame (frame 0 without an open sequence)."),
							[]() { return GSSPipeline::ExportCurrentCamera(*UGSSSettings::Get()); })
					]
					+ SWrapBox::Slot().Padding(2)
					[
						MakeButton(LOCTEXT("ExportSequence", "Export Camera Sequence"),
							LOCTEXT("ExportSequenceTip", "Steps the open level sequence frame by frame and exports one COLMAP dataset per frame."),
							[]() { return GSSPipeline::ExportCameraSequence(*UGSSSettings::Get()); })
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
				[
					SectionHeader(LOCTEXT("Step2", "2. Render (Movie Render Queue/Graph), then Depth EXR -> Point Cloud"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SWrapBox)
					.UseAllottedSize(true)
					+ SWrapBox::Slot().Padding(2)
					[
						MakeButton(LOCTEXT("ConvertDepth", "Convert Depth For All Frames"),
							LOCTEXT("ConvertDepthTip", "Reads each frame's camera back from COLMAP, unprojects the depth EXR and writes points3D + points.ply. Copies the beauty image into images/."),
							[]() { return GSSPipeline::ConvertDepthForAllFrames(*UGSSSettings::Get()); })
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
				[
					SectionHeader(LOCTEXT("Step3", "3. Train Gaussian Splats in LichtFeld Studio"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SWrapBox)
					.UseAllottedSize(true)
					+ SWrapBox::Slot().Padding(2)
					[
						MakeButton(LOCTEXT("WriteScript", "Write Training Script"),
							LOCTEXT("WriteScriptTip", "Writes train_lichtfeld.bat/.sh into the output directory."),
							[]()
							{
								FString ScriptPath;
								return GSSPipeline::WriteTrainingScript(*UGSSSettings::Get(), ScriptPath);
							})
					]
					+ SWrapBox::Slot().Padding(2)
					[
						MakeButton(LOCTEXT("Train", "Start Training"),
							LOCTEXT("TrainTip", "Writes the script and runs it in a new console (one splat per frame, sequentially)."),
							[]() { return GSSPipeline::LaunchTraining(*UGSSSettings::Get()); })
					]
					+ SWrapBox::Slot().Padding(2)
					[
						MakeButton(LOCTEXT("OpenFolder", "Open Output Folder"),
							LOCTEXT("OpenFolderTip", "Opens the output directory in the file browser."),
							[]()
							{
								FString Dir = UGSSSettings::Get()->OutputDirectory.Path;
								if (Dir.IsEmpty())
								{
									return FGSSResult::Error(TEXT("Set an Output Directory first."));
								}
								Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Dir);
								IFileManager::Get().MakeDirectory(*Dir, true);
								FPlatformProcess::ExploreFolder(*Dir);
								return FGSSResult::Ok(Dir);
							})
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
				[
					SAssignNew(StatusText, STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("Ready", "Ready."))
				]
			]
		]
	];
}

TSharedRef<SWidget> SGSSPanel::MakeButton(const FText& Label, const FText& ToolTip, TFunction<FGSSResult()> Action)
{
	return SNew(SButton)
		.Text(Label)
		.ToolTipText(ToolTip)
		.OnClicked_Lambda([this, Action]()
		{
			ShowResult(Action());
			return FReply::Handled();
		});
}

void SGSSPanel::ShowResult(const FGSSResult& Result)
{
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(Result.Message));
		StatusText->SetColorAndOpacity(Result.bSuccess ? FSlateColor::UseForeground() : FSlateColor(FLinearColor(1.0f, 0.35f, 0.3f)));
	}

	if (Result.bSuccess)
	{
		UE_LOG(LogGaussianSplatSequence, Display, TEXT("%s"), *Result.Message);
	}
	else
	{
		UE_LOG(LogGaussianSplatSequence, Error, TEXT("%s"), *Result.Message);
	}

	FNotificationInfo Info(FText::FromString(Result.Message));
	Info.ExpireDuration = Result.bSuccess ? 4.0f : 8.0f;
	if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(Result.bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
}

#undef LOCTEXT_NAMESPACE
