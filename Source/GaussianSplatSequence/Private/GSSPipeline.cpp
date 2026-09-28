// Copyright (c) GaussianSplatSequence contributors. MIT License.

#include "GSSPipeline.h"

#include "GSSColmapIO.h"
#include "GSSSettings.h"

#include "Camera/CameraComponent.h"
#include "CineCameraComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "MovieScene.h"
#include "MovieSceneSequencePlayer.h"

DEFINE_LOG_CATEGORY(LogGaussianSplatSequence);

#define LOCTEXT_NAMESPACE "GaussianSplatSequence"

namespace
{
	struct FCapturedCamera
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		double HorizontalFov = 90.0;
		double AspectRatio = 0.0;
		bool bConstrainAspectRatio = false;
		FString SourceName;
	};

	GSS::EWorldAxes ToMathAxes(EGSSWorldAxes Axes)
	{
		switch (Axes)
		{
		case EGSSWorldAxes::RightHanded_ZUp: return GSS::EWorldAxes::RightHanded_ZUp;
		case EGSSWorldAxes::OpenGL_YUp:      return GSS::EWorldAxes::OpenGL_YUp;
		case EGSSWorldAxes::OpenCV_YDown:
		default:                             return GSS::EWorldAxes::OpenCV_YDown;
		}
	}

	GSS::Vec3 ToVec3(const FVector& V)
	{
		return GSS::Vec3(V.X, V.Y, V.Z);
	}

	FString GetOutputRoot(const UGSSSettings& Settings)
	{
		FString Root = Settings.OutputDirectory.Path;
		if (Root.IsEmpty())
		{
			return FString();
		}
		if (FPaths::IsRelative(Root))
		{
			Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Root);
		}
		FPaths::NormalizeDirectoryName(Root);
		return Root;
	}

	// ------------------------------------------------------------------ camera capture

	void CaptureFromComponent(UCameraComponent* Camera, FCapturedCamera& Out)
	{
		FMinimalViewInfo View;
		Camera->GetCameraView(0.0f, View);
		Out.Location = View.Location;
		Out.Rotation = View.Rotation;
		Out.HorizontalFov = View.FOV;
		Out.AspectRatio = View.AspectRatio;
		Out.bConstrainAspectRatio = View.bConstrainAspectRatio;

		// CineCamera: derive the FOV straight from filmback + focal length so animated
		// focal lengths are exact even if the cached FieldOfView was not refreshed yet.
		if (const UCineCameraComponent* Cine = Cast<UCineCameraComponent>(Camera))
		{
			Out.HorizontalFov = Cine->GetHorizontalFieldOfView();
		}

		const AActor* Owner = Camera->GetOwner();
		Out.SourceName = Owner ? Owner->GetActorLabel() : Camera->GetName();
	}

	UCameraComponent* FindSelectedCamera()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (const AActor* Actor = Cast<AActor>(*It))
			{
				if (UCameraComponent* Camera = Actor->FindComponentByClass<UCameraComponent>())
				{
					return Camera;
				}
			}
		}
		return nullptr;
	}

	FLevelEditorViewportClient* FindLevelViewportClient()
	{
		if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsPerspective())
		{
			return GCurrentLevelEditingViewportClient;
		}
		if (GEditor)
		{
			for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
			{
				if (Client && Client->IsPerspective())
				{
					return Client;
				}
			}
		}
		return nullptr;
	}

	bool CaptureFromViewport(FCapturedCamera& Out, FString& OutError)
	{
		FLevelEditorViewportClient* Client = FindLevelViewportClient();
		if (!Client)
		{
			OutError = TEXT("No perspective level viewport found.");
			return false;
		}

		// Piloted camera or Sequencer camera cut lock (cinematic lock wins).
		if (AActor* Locked = Client->GetActiveActorLock().Get())
		{
			if (UCameraComponent* Camera = Locked->FindComponentByClass<UCameraComponent>())
			{
				CaptureFromComponent(Camera, Out);
				return true;
			}
		}

		Out.Location = Client->GetViewLocation();
		Out.Rotation = Client->GetViewRotation();
		Out.HorizontalFov = Client->ViewFOV;
		Out.AspectRatio = 0.0;
		Out.bConstrainAspectRatio = false;
		Out.SourceName = TEXT("Level Viewport");
		return true;
	}

	bool CaptureCamera(const UGSSSettings& Settings, UCameraComponent* FixedCamera, FCapturedCamera& Out, FString& OutError)
	{
		if (Settings.CameraSource == EGSSCameraSource::Viewport)
		{
			return CaptureFromViewport(Out, OutError);
		}
		if (!FixedCamera)
		{
			OutError = TEXT("Select a Camera or CineCamera actor in the level (Camera Source = Selected Camera Actor).");
			return false;
		}
		CaptureFromComponent(FixedCamera, Out);
		return true;
	}

	// ------------------------------------------------------------------ export

	FGSSColmapImage MakeColmapImage(const UGSSSettings& Settings, const FCapturedCamera& Camera, const FString& ImageName)
	{
		const FRotationMatrix Rotation(Camera.Rotation);
		const GSS::CameraPose Pose = GSS::PoseFromUnreal(
			ToVec3(Rotation.GetScaledAxis(EAxis::X)),
			ToVec3(Rotation.GetScaledAxis(EAxis::Y)),
			ToVec3(Rotation.GetScaledAxis(EAxis::Z)),
			ToVec3(Camera.Location),
			ToMathAxes(Settings.WorldAxes),
			Settings.UnitScale);

		FGSSColmapImage Image;
		Image.Intrinsics = GSS::IntrinsicsFromHorizontalFov(Settings.ImageWidth, Settings.ImageHeight, Camera.HorizontalFov);
		Image.Extrinsics = GSS::ToColmapExtrinsics(Pose);
		Image.ImageName = ImageName;
		return Image;
	}

	FGSSResult ExportFrame(const UGSSSettings& Settings, const FCapturedCamera& Camera, int32 FrameNumber)
	{
		const FString Root = GetOutputRoot(Settings);
		const FString FrameName = GSSPipeline::MakeFrameName(Settings, FrameNumber);
		const FString FrameDir = Root / FrameName;
		const FString SparseDir = FrameDir / TEXT("sparse") / TEXT("0");

		IFileManager::Get().MakeDirectory(*(FrameDir / TEXT("images")), true);
		IFileManager::Get().MakeDirectory(*SparseDir, true);

		const double ImageAspect = static_cast<double>(Settings.ImageWidth) / Settings.ImageHeight;
		if (Camera.bConstrainAspectRatio && Camera.AspectRatio > 0.0 && FMath::Abs(Camera.AspectRatio - ImageAspect) > 0.01)
		{
			UE_LOG(LogGaussianSplatSequence, Warning,
				TEXT("%s: camera aspect ratio %.4f differs from the image resolution %dx%d (%.4f). Match the filmback to the render resolution, otherwise the vertical field of view will not line up."),
				*Camera.SourceName, Camera.AspectRatio, Settings.ImageWidth, Settings.ImageHeight, ImageAspect);
		}

		TArray<FGSSColmapImage> Images;
		Images.Add(MakeColmapImage(Settings, Camera, FrameName + TEXT(".png")));

		FString Error;
		if (!GSSColmapIO::WriteCameras(SparseDir, Images, Settings.bWriteBinaryColmap, Error))
		{
			return FGSSResult::Error(Error);
		}

		// Keep the folder a loadable dataset even before the depth has been converted.
		if (!FPaths::FileExists(SparseDir / TEXT("points3D.txt")))
		{
			if (!GSSColmapIO::WritePoints(SparseDir, TArray<FGSSColmapPoint>(), Settings.bWriteBinaryColmap, Error))
			{
				return FGSSResult::Error(Error);
			}
		}

		// Human readable record of the source camera (useful for debugging / re-import).
		const FString UECameraInfo = FString::Printf(
			TEXT("source=%s\nframe=%d\nlocation_cm=%.6f %.6f %.6f\nrotation_pyr_deg=%.6f %.6f %.6f\nhfov_deg=%.8f\nresolution=%d %d\nunit_scale=%.10g\nworld_axes=%d\n"),
			*Camera.SourceName, FrameNumber,
			Camera.Location.X, Camera.Location.Y, Camera.Location.Z,
			Camera.Rotation.Pitch, Camera.Rotation.Yaw, Camera.Rotation.Roll,
			Camera.HorizontalFov, Settings.ImageWidth, Settings.ImageHeight,
			Settings.UnitScale, static_cast<int32>(Settings.WorldAxes));
		FFileHelper::SaveStringToFile(UECameraInfo, *(FrameDir / TEXT("ue_camera.txt")));

		return FGSSResult::Ok(FString::Printf(TEXT("Exported %s (%s)"), *FrameName, *Camera.SourceName));
	}

	// ------------------------------------------------------------------ sequencer

	bool GetSequencerFrame(int32& OutFrame)
	{
		if (!ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence())
		{
			return false;
		}
		const FMovieSceneSequencePlaybackParams Position = ULevelSequenceEditorBlueprintLibrary::GetGlobalPosition(EMovieSceneTimeUnit::DisplayRate);
		OutFrame = Position.Frame.FloorToFrame().Value;
		return true;
	}

	void SetSequencerFrame(int32 Frame)
	{
		FMovieSceneSequencePlaybackParams Params;
		Params.Frame = FFrameTime(FFrameNumber(Frame));
		Params.PositionType = EMovieScenePositionType::Frame;
		Params.UpdateMethod = EUpdatePositionMethod::Jump;
		ULevelSequenceEditorBlueprintLibrary::SetGlobalPosition(Params);
		// Moving the playhead alone does not always write the animated transforms back.
		ULevelSequenceEditorBlueprintLibrary::ForceUpdate();
	}

	// ------------------------------------------------------------------ images

	bool LoadImageAs(const FString& Path, ERawImageFormat::Type Format, EGammaSpace Gamma, FImage& OutImage, FString& OutError)
	{
		if (!FPaths::FileExists(Path))
		{
			OutError = FString::Printf(TEXT("File not found: %s"), *Path);
			return false;
		}
		FImage Loaded;
		if (!FImageUtils::LoadImage(*Path, Loaded))
		{
			OutError = FString::Printf(TEXT("Could not decode %s (multilayer EXRs are not supported, export the pass as its own EXR)"), *Path);
			return false;
		}
		Loaded.CopyTo(OutImage, Format, Gamma);
		return true;
	}

	float ReadChannel(const FLinearColor& C, EGSSDepthChannel Channel)
	{
		switch (Channel)
		{
		case EGSSDepthChannel::G: return C.G;
		case EGSSDepthChannel::B: return C.B;
		case EGSSDepthChannel::A: return C.A;
		case EGSSDepthChannel::R:
		default:                  return C.R;
		}
	}
}

// ====================================================================== public

FString GSSPipeline::MakeFrameName(const UGSSSettings& Settings, int32 FrameNumber)
{
	return FString::Printf(TEXT("%s%0*d"), *Settings.FramePrefix, Settings.FramePadding, FrameNumber);
}

FString GSSPipeline::ResolveRenderPattern(const UGSSSettings& Settings, const FString& Pattern, int32 FrameNumber)
{
	const int32 RenderFrame = FrameNumber + Settings.RenderFrameOffset;
	FString Result = Pattern.TrimStartAndEnd().TrimQuotes();
	Result.ReplaceInline(TEXT("{frame}"), *FString::Printf(TEXT("%0*d"), Settings.FramePadding, RenderFrame));
	Result.ReplaceInline(TEXT("{frame_raw}"), *FString::FromInt(RenderFrame));
	Result.ReplaceInline(TEXT("{frame_name}"), *MakeFrameName(Settings, FrameNumber));
	return Result;
}

FGSSResult GSSPipeline::ExportCurrentCamera(const UGSSSettings& Settings)
{
	if (GetOutputRoot(Settings).IsEmpty())
	{
		return FGSSResult::Error(TEXT("Set an Output Directory first."));
	}

	FCapturedCamera Camera;
	FString Error;
	if (!CaptureCamera(Settings, FindSelectedCamera(), Camera, Error))
	{
		return FGSSResult::Error(Error);
	}

	int32 Frame = 0;
	GetSequencerFrame(Frame);
	return ExportFrame(Settings, Camera, Frame);
}

FGSSResult GSSPipeline::ExportCameraSequence(const UGSSSettings& Settings)
{
	if (GetOutputRoot(Settings).IsEmpty())
	{
		return FGSSResult::Error(TEXT("Set an Output Directory first."));
	}

	ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
	if (!Sequence || !Sequence->GetMovieScene())
	{
		return FGSSResult::Error(TEXT("Open the level sequence of your shot in Sequencer first."));
	}

	UCameraComponent* FixedCamera = FindSelectedCamera();
	if (Settings.CameraSource == EGSSCameraSource::SelectedCameraActor && !FixedCamera)
	{
		return FGSSResult::Error(TEXT("Select the shot camera in the level (for spawnable cameras: select it while the sequence is open)."));
	}

	int32 First = Settings.StartFrame;
	int32 Last = Settings.EndFrame;
	if (Settings.bUseSequencePlaybackRange)
	{
		const UMovieScene* MovieScene = Sequence->GetMovieScene();
		const TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
		const FFrameRate TickRate = MovieScene->GetTickResolution();
		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
		First = ConvertFrameTime(FFrameTime(Range.GetLowerBoundValue()), TickRate, DisplayRate).FloorToFrame().Value;
		// Playback range upper bound is exclusive (same as Movie Render Queue).
		Last = ConvertFrameTime(FFrameTime(Range.GetUpperBoundValue()), TickRate, DisplayRate).FloorToFrame().Value - 1;
	}
	if (Last < First)
	{
		return FGSSResult::Error(FString::Printf(TEXT("Empty frame range [%d, %d]."), First, Last));
	}

	const int32 Step = FMath::Max(1, Settings.FrameStep);
	const int32 NumFrames = (Last - First) / Step + 1;

	int32 OriginalFrame = First;
	GetSequencerFrame(OriginalFrame);

	FScopedSlowTask Task(static_cast<float>(NumFrames), LOCTEXT("ExportingCameras", "Exporting camera sequence to COLMAP..."));
	Task.MakeDialog(/*bShowCancelButton*/ true);

	int32 Exported = 0;
	FString FirstError;
	for (int32 Frame = First; Frame <= Last; Frame += Step)
	{
		if (Task.ShouldCancel())
		{
			break;
		}
		Task.EnterProgressFrame(1.0f, FText::Format(LOCTEXT("ExportingFrame", "Frame {0}"), FText::AsNumber(Frame)));

		SetSequencerFrame(Frame);

		FCapturedCamera Camera;
		FString Error;
		FGSSResult Result = CaptureCamera(Settings, FixedCamera, Camera, Error)
			? ExportFrame(Settings, Camera, Frame)
			: FGSSResult::Error(Error);

		if (Result.bSuccess)
		{
			++Exported;
		}
		else
		{
			UE_LOG(LogGaussianSplatSequence, Error, TEXT("Frame %d: %s"), Frame, *Result.Message);
			if (FirstError.IsEmpty())
			{
				FirstError = Result.Message;
			}
		}
	}

	SetSequencerFrame(OriginalFrame);

	if (Exported == 0)
	{
		return FGSSResult::Error(FirstError.IsEmpty() ? TEXT("Cancelled.") : FirstError);
	}
	return FGSSResult::Ok(FString::Printf(TEXT("Exported %d camera(s), frames %d-%d, to %s"), Exported, First, Last, *GetOutputRoot(Settings)));
}

FGSSResult GSSPipeline::ConvertDepthForFrame(const UGSSSettings& Settings, const FString& FrameDir, int32 FrameNumber)
{
	const FString SparseDir = FrameDir / TEXT("sparse") / TEXT("0");
	const FString FrameName = FPaths::GetCleanFilename(FrameDir);

	// The camera is read back from the exported COLMAP files, so points and camera
	// are guaranteed to use the very same pose and intrinsics.
	TArray<FGSSColmapImage> Images;
	FString Error;
	if (!GSSColmapIO::ReadCameras(SparseDir, Images, Error))
	{
		return FGSSResult::Error(Error);
	}
	const FGSSColmapImage& Camera = Images[0];
	const GSS::CameraPose Pose = GSS::FromColmapExtrinsics(Camera.Extrinsics);

	// ---- depth
	const FString DepthPath = ResolveRenderPattern(Settings, Settings.DepthExrPattern, FrameNumber);
	FImage Depth;
	if (!LoadImageAs(DepthPath, ERawImageFormat::RGBA32F, EGammaSpace::Linear, Depth, Error))
	{
		return FGSSResult::Error(Error);
	}
	const int32 DW = Depth.SizeX;
	const int32 DH = Depth.SizeY;
	const TArrayView64<FLinearColor> DepthPixels = Depth.AsRGBA32F();

	if (FMath::Abs(static_cast<double>(DW) / DH - static_cast<double>(Camera.Intrinsics.Width) / Camera.Intrinsics.Height) > 0.01)
	{
		UE_LOG(LogGaussianSplatSequence, Warning, TEXT("%s: depth %dx%d has a different aspect ratio than the camera %dx%d."),
			*FrameName, DW, DH, Camera.Intrinsics.Width, Camera.Intrinsics.Height);
	}
	const GSS::PinholeIntrinsics K = Camera.Intrinsics.Rescaled(DW, DH);

	// ---- colour (optional)
	FImage Color;
	bool bHasColor = false;
	if (!Settings.ColorImagePattern.IsEmpty())
	{
		const FString ColorPath = ResolveRenderPattern(Settings, Settings.ColorImagePattern, FrameNumber);
		bHasColor = LoadImageAs(ColorPath, ERawImageFormat::BGRA8, EGammaSpace::sRGB, Color, Error);
		if (!bHasColor)
		{
			UE_LOG(LogGaussianSplatSequence, Warning, TEXT("%s: %s - points will be grey."), *FrameName, *Error);
		}
		else if (Settings.bCopyColorImage)
		{
			const FString Target = FrameDir / TEXT("images") / Camera.ImageName;
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), true);
			if (Color.SizeX != Camera.Intrinsics.Width || Color.SizeY != Camera.Intrinsics.Height)
			{
				UE_LOG(LogGaussianSplatSequence, Warning, TEXT("%s: beauty image is %dx%d but the camera was exported for %dx%d. Re-export the camera with the render resolution."),
					*FrameName, Color.SizeX, Color.SizeY, Camera.Intrinsics.Width, Camera.Intrinsics.Height);
			}
			if (!FImageUtils::SaveImageByExtension(*Target, Color))
			{
				return FGSSResult::Error(FString::Printf(TEXT("Could not write %s"), *Target));
			}
		}
	}
	const TArrayView64<FColor> ColorPixels = bHasColor ? Color.AsBGRA8() : TArrayView64<FColor>();

	// ---- unproject
	const GSS::EDepthType DepthType = Settings.DepthType == EGSSDepthType::Radial ? GSS::EDepthType::Radial : GSS::EDepthType::Planar;
	const int32 Stride = FMath::Max(1, Settings.PixelStride);
	const double ToUnreal = Settings.DepthToUnrealUnits;

	auto DepthAt = [&](int32 X, int32 Y) -> double
	{
		return static_cast<double>(ReadChannel(DepthPixels[static_cast<int64>(Y) * DW + X], Settings.DepthChannel)) * ToUnreal;
	};
	auto IsValidDepth = [&](double D)
	{
		return FMath::IsFinite(D) && D >= Settings.MinDepth && D <= Settings.MaxDepth;
	};

	TArray<FGSSColmapPoint> Points;
	Points.Reserve((DW / Stride + 1) * (DH / Stride + 1));

	for (int32 Y = 0; Y < DH; Y += Stride)
	{
		for (int32 X = 0; X < DW; X += Stride)
		{
			const double D = DepthAt(X, Y);
			if (!IsValidDepth(D))
			{
				continue;
			}

			if (Settings.EdgeThreshold > 0.0)
			{
				bool bEdge = false;
				const int32 NX[4] = { X - 1, X + 1, X, X };
				const int32 NY[4] = { Y, Y, Y - 1, Y + 1 };
				for (int32 N = 0; N < 4 && !bEdge; ++N)
				{
					if (NX[N] >= 0 && NX[N] < DW && NY[N] >= 0 && NY[N] < DH)
					{
						const double ND = DepthAt(NX[N], NY[N]);
						bEdge = !FMath::IsFinite(ND) || FMath::Abs(ND - D) > Settings.EdgeThreshold * D;
					}
				}
				if (bEdge)
				{
					continue;
				}
			}

			const GSS::Vec3 World = GSS::UnprojectPixel(K, Pose, X, Y, D * Settings.UnitScale, DepthType);

			FGSSColmapPoint& P = Points.AddDefaulted_GetRef();
			P.X = World.X;
			P.Y = World.Y;
			P.Z = World.Z;
			if (bHasColor)
			{
				const int32 CX = FMath::Clamp(static_cast<int32>((X + 0.5) * Color.SizeX / DW), 0, Color.SizeX - 1);
				const int32 CY = FMath::Clamp(static_cast<int32>((Y + 0.5) * Color.SizeY / DH), 0, Color.SizeY - 1);
				const FColor& C = ColorPixels[static_cast<int64>(CY) * Color.SizeX + CX];
				P.R = C.R;
				P.G = C.G;
				P.B = C.B;
			}
			else
			{
				P.R = P.G = P.B = 128;
			}
		}
	}

	const int32 NumValid = Points.Num();
	if (Settings.MaxPoints > 0 && Points.Num() > Settings.MaxPoints)
	{
		// Uniform random subset (partial Fisher-Yates), deterministic across frames.
		FRandomStream Random(1337);
		for (int32 Index = 0; Index < Settings.MaxPoints; ++Index)
		{
			Points.Swap(Index, Random.RandRange(Index, Points.Num() - 1));
		}
		Points.SetNum(Settings.MaxPoints);
	}

	if (Points.Num() == 0)
	{
		return FGSSResult::Error(FString::Printf(TEXT("%s: no valid depth pixels in %s (check Depth Channel, Min/Max Depth and Depth To Unreal Units)."), *FrameName, *DepthPath));
	}

	if (!GSSColmapIO::WritePoints(SparseDir, Points, Settings.bWriteBinaryColmap, Error))
	{
		return FGSSResult::Error(Error);
	}
	if (Settings.bWritePly && !GSSColmapIO::WritePly(FrameDir / TEXT("points.ply"), Points, Error))
	{
		return FGSSResult::Error(Error);
	}

	return FGSSResult::Ok(FString::Printf(TEXT("%s: %d points (%d valid pixels, depth %dx%d)"), *FrameName, Points.Num(), NumValid, DW, DH));
}

TArray<TPair<int32, FString>> GSSPipeline::FindFrameFolders(const UGSSSettings& Settings)
{
	TArray<TPair<int32, FString>> Result;
	const FString Root = GetOutputRoot(Settings);
	if (Root.IsEmpty())
	{
		return Result;
	}

	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(Root / (Settings.FramePrefix + TEXT("*"))), /*Files*/ false, /*Directories*/ true);
	for (const FString& Dir : Dirs)
	{
		const FString Number = Dir.RightChop(Settings.FramePrefix.Len());
		if (Number.IsEmpty() || !Number.IsNumeric())
		{
			continue;
		}
		const FString FrameDir = Root / Dir;
		if (FPaths::FileExists(FrameDir / TEXT("sparse") / TEXT("0") / TEXT("images.txt")))
		{
			Result.Emplace(FCString::Atoi(*Number), FrameDir);
		}
	}
	Result.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key < B.Key; });
	return Result;
}

FGSSResult GSSPipeline::ConvertDepthForAllFrames(const UGSSSettings& Settings)
{
	if (Settings.DepthExrPattern.IsEmpty())
	{
		return FGSSResult::Error(TEXT("Set the Depth EXR Pattern first."));
	}
	const TArray<TPair<int32, FString>> Frames = FindFrameFolders(Settings);
	if (Frames.Num() == 0)
	{
		return FGSSResult::Error(TEXT("No exported frames found. Export the cameras first."));
	}

	FScopedSlowTask Task(static_cast<float>(Frames.Num()), LOCTEXT("ConvertingDepth", "Converting depth to point clouds..."));
	Task.MakeDialog(/*bShowCancelButton*/ true);

	int32 Converted = 0;
	int64 TotalPoints = 0;
	FString FirstError;
	for (const TPair<int32, FString>& Frame : Frames)
	{
		if (Task.ShouldCancel())
		{
			break;
		}
		Task.EnterProgressFrame(1.0f, FText::FromString(FPaths::GetCleanFilename(Frame.Value)));

		const FGSSResult Result = ConvertDepthForFrame(Settings, Frame.Value, Frame.Key);
		UE_LOG(LogGaussianSplatSequence, Display, TEXT("%s"), *Result.Message);
		if (Result.bSuccess)
		{
			++Converted;
		}
		else if (FirstError.IsEmpty())
		{
			FirstError = Result.Message;
		}
	}

	if (Converted == 0)
	{
		return FGSSResult::Error(FirstError.IsEmpty() ? TEXT("Cancelled.") : FirstError);
	}
	FString Message = FString::Printf(TEXT("Converted %d/%d frame(s)."), Converted, Frames.Num());
	if (!FirstError.IsEmpty())
	{
		Message += TEXT(" First error: ") + FirstError;
	}
	return FGSSResult{ Converted == Frames.Num(), Message };
}

FGSSResult GSSPipeline::WriteTrainingScript(const UGSSSettings& Settings, FString& OutScriptPath)
{
	FString Exe = Settings.LichtFeldExecutable.FilePath;
	if (Exe.IsEmpty())
	{
		return FGSSResult::Error(TEXT("Set the LichtFeld Studio executable first."));
	}
	Exe = FPaths::ConvertRelativePathToFull(Exe);

	const TArray<TPair<int32, FString>> Frames = FindFrameFolders(Settings);
	if (Frames.Num() == 0)
	{
		return FGSSResult::Error(TEXT("No exported frames found."));
	}

	const FString Root = GetOutputRoot(Settings);
	const FString TrainedRoot = Root / Settings.TrainedSubfolder;

	constexpr bool bWindows = PLATFORM_WINDOWS != 0;
	OutScriptPath = Root / (bWindows ? TEXT("train_lichtfeld.bat") : TEXT("train_lichtfeld.sh"));

	auto Native = [](FString Path)
	{
		if (bWindows)
		{
			FPaths::MakePlatformFilename(Path);
		}
		return Path;
	};

	FString Script;
	if (bWindows)
	{
		Script += TEXT("@echo off\r\nsetlocal\r\n");
		Script += FString::Printf(TEXT("set LFS=\"%s\"\r\n"), *Native(Exe));
	}
	else
	{
		Script += TEXT("#!/usr/bin/env bash\nset -u\n");
		Script += FString::Printf(TEXT("LFS=\"%s\"\n"), *Exe);
	}
	const TCHAR* NL = bWindows ? TEXT("\r\n") : TEXT("\n");

	FString PrevName;
	FString PrevOutput;
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		const FString& FrameDir = Frames[Index].Value;
		const FString Name = FPaths::GetCleanFilename(FrameDir);
		const FString Output = TrainedRoot / Name;

		FString Args = Settings.TrainArguments;
		Args.ReplaceInline(TEXT("{data}"), *Native(FrameDir));
		Args.ReplaceInline(TEXT("{output}"), *Native(Output));
		Args.ReplaceInline(TEXT("{name}"), *Name);
		Args.ReplaceInline(TEXT("{iter}"), *FString::FromInt(Settings.Iterations));
		if (!Settings.Strategy.TrimStartAndEnd().IsEmpty() && !Args.Contains(TEXT("--strategy")))
		{
			Args += FString::Printf(TEXT(" --strategy %s"), *Settings.Strategy.TrimStartAndEnd());
		}
		if (Settings.MaxSplats > 0 && !Args.Contains(TEXT("--max-cap")))
		{
			Args += FString::Printf(TEXT(" --max-cap %d"), Settings.MaxSplats);
		}

		Script += FString::Printf(TEXT("echo [%d/%d] %s%s"), Index + 1, Frames.Num(), *Name, NL);
		if (bWindows)
		{
			Script += FString::Printf(TEXT("if not exist \"%s\" mkdir \"%s\"%s"), *Native(Output), *Native(Output), NL);
		}
		else
		{
			Script += FString::Printf(TEXT("mkdir -p \"%s\"%s"), *Output, NL);
		}

		if (Settings.bInitFromPreviousFrame && !PrevName.IsEmpty())
		{
			FString PrevSplat = Settings.PreviousSplatPattern;
			PrevSplat.ReplaceInline(TEXT("{prev_output}"), *PrevOutput);
			PrevSplat.ReplaceInline(TEXT("{prev_name}"), *PrevName);
			PrevSplat = Native(PrevSplat);
			if (bWindows)
			{
				Script += FString::Printf(TEXT("if exist \"%s\" (%s  %%LFS%% %s --init \"%s\"%s) else (%s  %%LFS%% %s%s)%s"),
					*PrevSplat, NL, *Args, *PrevSplat, NL, NL, *Args, NL, NL);
			}
			else
			{
				Script += FString::Printf(TEXT("if [ -f \"%s\" ]; then \"$LFS\" %s --init \"%s\"; else \"$LFS\" %s; fi%s"),
					*PrevSplat, *Args, *PrevSplat, *Args, NL);
			}
		}
		else
		{
			Script += bWindows
				? FString::Printf(TEXT("%%LFS%% %s%s"), *Args, NL)
				: FString::Printf(TEXT("\"$LFS\" %s%s"), *Args, NL);
		}

		PrevName = Name;
		PrevOutput = Output;
	}
	Script += FString::Printf(TEXT("echo Done.%s"), NL);
	if (bWindows)
	{
		Script += TEXT("pause\r\n");
	}

	if (!FFileHelper::SaveStringToFile(Script, *OutScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FGSSResult::Error(FString::Printf(TEXT("Could not write %s"), *OutScriptPath));
	}
	return FGSSResult::Ok(FString::Printf(TEXT("Wrote %s (%d frames)"), *OutScriptPath, Frames.Num()));
}

FGSSResult GSSPipeline::LaunchTraining(const UGSSSettings& Settings)
{
	FString ScriptPath;
	FGSSResult Result = WriteTrainingScript(Settings, ScriptPath);
	if (!Result.bSuccess)
	{
		return Result;
	}

#if PLATFORM_WINDOWS
	FString NativeScript = ScriptPath;
	FPaths::MakePlatformFilename(NativeScript);
	const FString Command = TEXT("cmd.exe");
	const FString Params = FString::Printf(TEXT("/c start \"LichtFeld training\" cmd /c \"\"%s\"\""), *NativeScript);
#else
	const FString Command = TEXT("/bin/bash");
	const FString Params = FString::Printf(TEXT("\"%s\""), *ScriptPath);
#endif

	FProcHandle Handle = FPlatformProcess::CreateProc(*Command, *Params, /*bLaunchDetached*/ true, /*bLaunchHidden*/ false,
		/*bLaunchReallyHidden*/ false, nullptr, 0, *GetOutputRoot(Settings), nullptr);
	if (!Handle.IsValid())
	{
		return FGSSResult::Error(FString::Printf(TEXT("Could not start %s"), *ScriptPath));
	}
	FPlatformProcess::CloseProc(Handle);
	return FGSSResult::Ok(FString::Printf(TEXT("Training started: %s"), *ScriptPath));
}

#undef LOCTEXT_NAMESPACE
