// Copyright (c) GaussianSplatSequence contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GSSMath.h"

class UGSSSettings;

DECLARE_LOG_CATEGORY_EXTERN(LogGaussianSplatSequence, Log, All);

/** Result of a pipeline step, shown in the panel and the log. */
struct FGSSResult
{
	bool bSuccess = false;
	FString Message;

	static FGSSResult Ok(const FString& InMessage) { return FGSSResult{ true, InMessage }; }
	static FGSSResult Error(const FString& InMessage) { return FGSSResult{ false, InMessage }; }
};

/**
 * Editor-side pipeline:
 *   1. ExportCurrentCamera / ExportCameraSequence -> <Output>/<frame>/sparse/0/{cameras,images}.txt
 *   2. ConvertDepth*                               -> <Output>/<frame>/images/<frame>.png + sparse/0/points3D.txt
 *   3. WriteTrainingScript / LaunchTraining        -> LichtFeld Studio, one splat per frame
 */
namespace GSSPipeline
{
	/** Camera of the current editor state (current Sequencer frame, if a sequence is open). */
	FGSSResult ExportCurrentCamera(const UGSSSettings& Settings);

	/** Steps the open level sequence through its frames and exports one COLMAP dataset per frame. */
	FGSSResult ExportCameraSequence(const UGSSSettings& Settings);

	/** Converts the depth EXR of one exported frame folder into points3D (+ copies the beauty image). */
	FGSSResult ConvertDepthForFrame(const UGSSSettings& Settings, const FString& FrameDir, int32 FrameNumber);

	/** Runs ConvertDepthForFrame for every exported frame folder under the output directory. */
	FGSSResult ConvertDepthForAllFrames(const UGSSSettings& Settings);

	/** Writes <Output>/train_lichtfeld.(bat|sh) that trains every frame sequentially. */
	FGSSResult WriteTrainingScript(const UGSSSettings& Settings, FString& OutScriptPath);

	/** Writes the script and starts it in a separate console. */
	FGSSResult LaunchTraining(const UGSSSettings& Settings);

	/** Frame folders (<prefix><number>) found under the output directory, sorted by frame. */
	TArray<TPair<int32, FString>> FindFrameFolders(const UGSSSettings& Settings);

	FString MakeFrameName(const UGSSSettings& Settings, int32 FrameNumber);
	FString ResolveRenderPattern(const UGSSSettings& Settings, const FString& Pattern, int32 FrameNumber);
}
