// Copyright (c) GaussianSplatSequence contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

struct FGSSResult;
class IDetailsView;
class STextBlock;

/** Dock tab: settings on top, one button per pipeline step below. */
class SGSSPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGSSPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> MakeButton(const FText& Label, const FText& ToolTip, TFunction<FGSSResult()> Action);
	void ShowResult(const FGSSResult& Result);

	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<STextBlock> StatusText;
};
