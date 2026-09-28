// Copyright (c) GaussianSplatSequence contributors. MIT License.

#include "GSSBlueprintLibrary.h"

#include "GSSPipeline.h"
#include "GSSSettings.h"

namespace
{
	bool Report(const FGSSResult& Result, FString& OutMessage)
	{
		OutMessage = Result.Message;
		UE_LOG(LogGaussianSplatSequence, Display, TEXT("%s"), *Result.Message);
		return Result.bSuccess;
	}
}

bool UGSSBlueprintLibrary::ExportCurrentCamera(FString& Message)
{
	return Report(GSSPipeline::ExportCurrentCamera(*UGSSSettings::Get()), Message);
}

bool UGSSBlueprintLibrary::ExportCameraSequence(FString& Message)
{
	return Report(GSSPipeline::ExportCameraSequence(*UGSSSettings::Get()), Message);
}

bool UGSSBlueprintLibrary::ConvertDepthForAllFrames(FString& Message)
{
	return Report(GSSPipeline::ConvertDepthForAllFrames(*UGSSSettings::Get()), Message);
}

bool UGSSBlueprintLibrary::ConvertDepthForFrame(const FString& FrameDirectory, int32 FrameNumber, FString& Message)
{
	return Report(GSSPipeline::ConvertDepthForFrame(*UGSSSettings::Get(), FrameDirectory, FrameNumber), Message);
}

bool UGSSBlueprintLibrary::WriteTrainingScript(FString& ScriptPath, FString& Message)
{
	return Report(GSSPipeline::WriteTrainingScript(*UGSSSettings::Get(), ScriptPath), Message);
}

bool UGSSBlueprintLibrary::LaunchTraining(FString& Message)
{
	return Report(GSSPipeline::LaunchTraining(*UGSSSettings::Get()), Message);
}
