// Copyright (c) GaussianSplatSequence contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GSSBlueprintLibrary.generated.h"

/**
 * Blueprint / Editor Python access to the pipeline. All functions use the settings
 * from Window > Gaussian Splat Sequence (UGSSSettings).
 *
 * Python:
 *   import unreal
 *   lib = unreal.GSSBlueprintLibrary
 *   ok, msg = lib.export_camera_sequence()
 *   ok, msg = lib.convert_depth_for_all_frames()
 *   ok, msg = lib.launch_training()
 */
UCLASS()
class GAUSSIANSPLATSEQUENCE_API UGSSBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Exports the current camera (at the current Sequencer frame) as COLMAP. */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splat Sequence")
	static bool ExportCurrentCamera(FString& Message);

	/** Exports one COLMAP dataset per frame of the open level sequence. */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splat Sequence")
	static bool ExportCameraSequence(FString& Message);

	/** Converts the depth EXR of every exported frame into a point cloud. */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splat Sequence")
	static bool ConvertDepthForAllFrames(FString& Message);

	/** Converts the depth EXR of a single exported frame folder. */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splat Sequence")
	static bool ConvertDepthForFrame(const FString& FrameDirectory, int32 FrameNumber, FString& Message);

	/** Writes the LichtFeld Studio batch script. */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splat Sequence")
	static bool WriteTrainingScript(FString& ScriptPath, FString& Message);

	/** Writes the batch script and runs it in a new console window. */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splat Sequence")
	static bool LaunchTraining(FString& Message);
};
