// Copyright (c) GaussianSplatSequence contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "UObject/Object.h"
#include "GSSSettings.generated.h"

UENUM()
enum class EGSSWorldAxes : uint8
{
	OpenCV_YDown    UMETA(DisplayName = "COLMAP / OpenCV (Y down, recommended)"),
	RightHanded_ZUp UMETA(DisplayName = "Right-handed Z up (Blender-like)"),
	OpenGL_YUp      UMETA(DisplayName = "Right-handed Y up (OpenGL-like)"),
};

UENUM()
enum class EGSSCoordinateSpace : uint8
{
	/** Real camera path: the scene stays put, the camera moves through it. */
	World        UMETA(DisplayName = "World (scene fixed, camera moves)"),
	/** Every frame uses the same fixed camera pose; the scene is stored relative to the camera (3D film, like SHARP). */
	CameraLocked UMETA(DisplayName = "Camera Locked (camera fixed, 3D film)"),
};

/** LichtFeld --bg-mode. */
UENUM()
enum class EGSSBackgroundMode : uint8
{
	Color      UMETA(DisplayName = "Color (solid color)"),
	Modulation UMETA(DisplayName = "Modulation"),
	Image      UMETA(DisplayName = "Image"),
	Random     UMETA(DisplayName = "Random"),
};

UENUM()
enum class EGSSCameraSource : uint8
{
	/** First selected actor that has a Camera/CineCamera component. */
	SelectedCameraActor UMETA(DisplayName = "Selected Camera Actor"),
	/** Camera the level viewport is piloting / locked to (incl. Sequencer camera cuts), otherwise the free viewport camera. */
	Viewport            UMETA(DisplayName = "Level Viewport (piloted camera / camera cuts)"),
};

UENUM()
enum class EGSSDepthType : uint8
{
	/** Distance along the camera's view axis. This is what Unreal's SceneDepth outputs. */
	Planar UMETA(DisplayName = "Planar (SceneDepth, Z-depth)"),
	/** Euclidean distance from the camera centre. */
	Radial UMETA(DisplayName = "Radial (distance to camera)"),
};

UENUM()
enum class EGSSDepthChannel : uint8
{
	R, G, B, A
};

/**
 * All options of the Gaussian Splat Sequence pipeline. Stored per user and project
 * (EditorPerProjectUserSettings) and edited in Window > Gaussian Splat Sequence.
 */
UCLASS(config = EditorPerProjectUserSettings)
class GAUSSIANSPLATSEQUENCE_API UGSSSettings : public UObject
{
	GENERATED_BODY()

public:
	static UGSSSettings* Get() { return GetMutableDefault<UGSSSettings>(); }

	// ---------------------------------------------------------------- Output

	/** Dataset root. Every frame becomes <Output>/<FramePrefix><frame>/{images, sparse/0}. */
	UPROPERTY(EditAnywhere, config, Category = "1 | Output")
	FDirectoryPath OutputDirectory;

	UPROPERTY(EditAnywhere, config, Category = "1 | Output")
	FString FramePrefix = TEXT("frame_");

	/** Zero padding of frame numbers, used for folder names and the {frame} token. */
	UPROPERTY(EditAnywhere, config, Category = "1 | Output", meta = (ClampMin = "1", ClampMax = "10"))
	int32 FramePadding = 4;

	/** Axis convention of the exported world. COLMAP/OpenCV matches what splat trainers and viewers expect from real captures. */
	UPROPERTY(EditAnywhere, config, Category = "1 | Output")
	EGSSWorldAxes WorldAxes = EGSSWorldAxes::OpenCV_YDown;

	/**
	 * World: cameras keep their real path. Camera Locked: every frame gets the same camera pose
	 * (Locked Camera Location/Rotation) and the depth point cloud is expressed relative to it, so the
	 * splat sequence plays back like a film from one fixed viewpoint. Re-export cameras and re-convert depth after changing.
	 */
	UPROPERTY(EditAnywhere, config, Category = "1 | Output")
	EGSSCoordinateSpace CoordinateSpace = EGSSCoordinateSpace::World;

	/** Fixed camera location (Unreal units) used for every frame in Camera Locked mode. */
	UPROPERTY(EditAnywhere, config, Category = "1 | Output", meta = (EditCondition = "CoordinateSpace == EGSSCoordinateSpace::CameraLocked", EditConditionHides))
	FVector LockedCameraLocation = FVector::ZeroVector;

	/** Fixed camera rotation used for every frame in Camera Locked mode. */
	UPROPERTY(EditAnywhere, config, Category = "1 | Output", meta = (EditCondition = "CoordinateSpace == EGSSCoordinateSpace::CameraLocked", EditConditionHides))
	FRotator LockedCameraRotation = FRotator::ZeroRotator;

	/** Unreal units -> dataset units. 0.01 = centimetres to metres. Applied to camera positions and depth. */
	UPROPERTY(EditAnywhere, config, Category = "1 | Output", meta = (ClampMin = "0.000001"))
	double UnitScale = 0.01;

	/** Also write cameras.bin / images.bin / points3D.bin next to the .txt files. */
	UPROPERTY(EditAnywhere, config, Category = "1 | Output")
	bool bWriteBinaryColmap = true;

	// ---------------------------------------------------------------- Camera

	UPROPERTY(EditAnywhere, config, Category = "2 | Camera")
	EGSSCameraSource CameraSource = EGSSCameraSource::SelectedCameraActor;

	/** Resolution you render with (Movie Render Queue / Graph output resolution). */
	UPROPERTY(EditAnywhere, config, Category = "2 | Camera", meta = (ClampMin = "1"))
	int32 ImageWidth = 1920;

	UPROPERTY(EditAnywhere, config, Category = "2 | Camera", meta = (ClampMin = "1"))
	int32 ImageHeight = 1080;

	// ---------------------------------------------------------------- Sequence

	/** Use the playback range of the level sequence that is open in Sequencer. */
	UPROPERTY(EditAnywhere, config, Category = "3 | Sequence")
	bool bUseSequencePlaybackRange = true;

	/** First frame (display rate), inclusive. */
	UPROPERTY(EditAnywhere, config, Category = "3 | Sequence", meta = (EditCondition = "!bUseSequencePlaybackRange"))
	int32 StartFrame = 0;

	/** Last frame (display rate), inclusive. */
	UPROPERTY(EditAnywhere, config, Category = "3 | Sequence", meta = (EditCondition = "!bUseSequencePlaybackRange"))
	int32 EndFrame = 100;

	UPROPERTY(EditAnywhere, config, Category = "3 | Sequence", meta = (ClampMin = "1"))
	int32 FrameStep = 1;

	// ---------------------------------------------------------------- Render inputs

	/**
	 * Beauty pass of the render. Tokens: {frame} (padded, incl. RenderFrameOffset), {frame_raw}, {frame_name}.
	 * Example: D:/Renders/Shot/Shot.FinalImage.{frame}.png  (PNG, JPG or EXR)
	 */
	UPROPERTY(EditAnywhere, config, Category = "4 | Render Inputs")
	FString ColorImagePattern;

	/**
	 * Depth pass as single-layer EXR (disable "Multilayer" in the EXR output settings).
	 * Example: D:/Renders/Shot/Shot.MovieRenderQueue_WorldDepth.{frame}.exr
	 */
	UPROPERTY(EditAnywhere, config, Category = "4 | Render Inputs")
	FString DepthExrPattern;

	/** Added to the sequence frame number when resolving {frame} in the render patterns. */
	UPROPERTY(EditAnywhere, config, Category = "4 | Render Inputs")
	int32 RenderFrameOffset = 0;

	/** Copy the beauty image (converted to PNG) into <frame>/images so the frame folder is a complete dataset. */
	UPROPERTY(EditAnywhere, config, Category = "4 | Render Inputs")
	bool bCopyColorImage = true;

	// ---------------------------------------------------------------- Depth

	UPROPERTY(EditAnywhere, config, Category = "5 | Depth To Point Cloud")
	EGSSDepthType DepthType = EGSSDepthType::Planar;

	UPROPERTY(EditAnywhere, config, Category = "5 | Depth To Point Cloud")
	EGSSDepthChannel DepthChannel = EGSSDepthChannel::R;

	/** Multiplier that turns the EXR value into Unreal units (cm). 1 for SceneDepth, 100 if your depth is stored in metres. */
	UPROPERTY(EditAnywhere, config, Category = "5 | Depth To Point Cloud", meta = (ClampMin = "0.000001"))
	double DepthToUnrealUnits = 1.0;

	/** Pixels closer than this (Unreal units) are ignored. */
	UPROPERTY(EditAnywhere, config, Category = "5 | Depth To Point Cloud", meta = (ClampMin = "0"))
	double MinDepth = 1.0;

	/** Pixels farther than this (Unreal units) are ignored, e.g. sky. */
	UPROPERTY(EditAnywhere, config, Category = "5 | Depth To Point Cloud", meta = (ClampMin = "0"))
	double MaxDepth = 100000.0;

	/** Use every Nth pixel in x and y. 1 = one point per pixel. */
	UPROPERTY(EditAnywhere, config, Category = "5 | Depth To Point Cloud", meta = (ClampMin = "1", ClampMax = "64"))
	int32 PixelStride = 2;

	/**
	 * Upper limit of points per frame (0 = no limit). If more valid pixels remain after Pixel Stride and
	 * filtering, the cloud is thinned uniformly at random (deterministic, same seed every frame).
	 * Tip: Pixel Stride 1 + Max Points gives an exact point budget independent of the EXR resolution.
	 */
	UPROPERTY(EditAnywhere, config, Category = "5 | Depth To Point Cloud", meta = (ClampMin = "0"))
	int32 MaxPoints = 0;

	/** Drop pixels on depth discontinuities (relative depth jump to a neighbour). Removes "flying" points on silhouettes. 0 disables. */
	UPROPERTY(EditAnywhere, config, Category = "5 | Depth To Point Cloud", meta = (ClampMin = "0"))
	double EdgeThreshold = 0.05;

	/** Also write <frame>/points.ply for inspection (CloudCompare, LichtFeld, Blender...). */
	UPROPERTY(EditAnywhere, config, Category = "5 | Depth To Point Cloud")
	bool bWritePly = true;

	// ---------------------------------------------------------------- Training

	/** LichtFeld-Studio executable (LichtFeld-Studio.exe). */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training")
	FFilePath LichtFeldExecutable;

	/**
	 * Arguments per frame. Tokens: {data} frame dataset folder, {output} training output folder,
	 * {name} frame name, {iter} Iterations.
	 */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training")
	FString TrainArguments = TEXT("-d \"{data}\" -o \"{output}\" -i {iter} --headless --output-name {name}");

	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training", meta = (ClampMin = "1"))
	int32 Iterations = 7000;

	/** Maximum number of Gaussians per frame (LichtFeld --max-cap). 0 = LichtFeld default. Used by the MCMC strategy. */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training", meta = (ClampMin = "0"))
	int32 MaxSplats = 0;

	/** LichtFeld optimisation strategy (--strategy), e.g. mcmc, mrnf, igs+. Empty = LichtFeld default. Use mcmc for a hard Max Splats cap. */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training")
	FString Strategy = TEXT("mcmc");

	/**
	 * Max spherical-harmonics degree (--sh-degree), -1 = LichtFeld default (3).
	 * 0 = plain RGB per splat (56 bytes instead of 236) - recommended for single-view frames, since
	 * view-dependent colour cannot be learned from one camera anyway, and for streaming sequences.
	 */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training", meta = (ClampMin = "-1", ClampMax = "3"))
	int32 ShDegree = 0;

	/** Iterations between SH degree increases (--sh-degree-interval). 0 = LichtFeld default. Only relevant for SH Degree > 0. */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training", meta = (ClampMin = "0", EditCondition = "ShDegree != 0"))
	int32 ShDegreeInterval = 0;

	/** Background used while training (--bg-mode): solid Color, Modulation, Image or Random. */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training")
	EGSSBackgroundMode BackgroundMode = EGSSBackgroundMode::Color;

	/** Background color for mode Color (--bg-color, default black). */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training", meta = (EditCondition = "BackgroundMode == EGSSBackgroundMode::Color", EditConditionHides, HideAlphaChannel))
	FColor BackgroundColor = FColor::Black;

	/** Background image for mode Image (--bg-image-path). */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training", meta = (EditCondition = "BackgroundMode == EGSSBackgroundMode::Image", EditConditionHides))
	FFilePath BackgroundImage;

	/** Initialise frame N with the trained splat of frame N-1 (--init). Gives temporally more stable sequences and faster convergence. */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training")
	bool bInitFromPreviousFrame = false;

	/** Location of the previous frame's splat. Tokens: {prev_output} (= trained folder), {prev_name}. */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training", meta = (EditCondition = "bInitFromPreviousFrame"))
	FString PreviousSplatPattern = TEXT("{prev_output}/{prev_name}.ply");

	/** Final splats go flat to <Output>/<TrainedSubfolder>/<frame name>.ply. */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training")
	FString TrainedSubfolder = TEXT("trained");

	/** Keep LichtFeld's per-frame work folders (<Output>/_lichtfeld_work/<frame>, incl. checkpoints) instead of deleting them after copying the splat. */
	UPROPERTY(EditAnywhere, config, Category = "6 | LichtFeld Training", AdvancedDisplay)
	bool bKeepTrainingWorkFolders = false;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
	{
		Super::PostEditChangeProperty(PropertyChangedEvent);
		SaveConfig();
	}
#endif
};
