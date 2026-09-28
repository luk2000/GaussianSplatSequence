// Copyright (c) GaussianSplatSequence contributors. MIT License.

#include "GSSColmapIO.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	// COLMAP camera model id for PINHOLE (fx, fy, cx, cy).
	constexpr int32 ColmapPinholeModelId = 1;

	template <typename FmtType, typename... ArgTypes>
	void AppendF(TArray64<uint8>& Buffer, const FmtType& Fmt, ArgTypes... Args)
	{
		ANSICHAR Line[1024];
		const int32 Len = FCStringAnsi::Snprintf(Line, UE_ARRAY_COUNT(Line), Fmt, Args...);
		if (Len > 0)
		{
			Buffer.Append(reinterpret_cast<const uint8*>(Line), FMath::Min<int32>(Len, UE_ARRAY_COUNT(Line) - 1));
		}
	}

	template <typename T>
	void Put(TArray64<uint8>& Buffer, const T& Value)
	{
		const int64 Offset = Buffer.AddUninitialized(sizeof(T));
		FMemory::Memcpy(Buffer.GetData() + Offset, &Value, sizeof(T));
	}

	bool SaveBuffer(const TArray64<uint8>& Buffer, const FString& Path, FString& OutError)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (!FFileHelper::SaveArrayToFile(Buffer, *Path))
		{
			OutError = FString::Printf(TEXT("Could not write %s"), *Path);
			return false;
		}
		return true;
	}

	bool LoadLines(const FString& Path, TArray<FString>& OutLines, FString& OutError)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *Path))
		{
			OutError = FString::Printf(TEXT("Could not read %s"), *Path);
			return false;
		}
		Content.ParseIntoArrayLines(OutLines, /*bCullEmpty*/ true);
		OutLines.RemoveAll([](const FString& Line)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			return Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT("#"));
		});
		return true;
	}
}

bool GSSColmapIO::WriteCameras(const FString& SparseDir, const TArray<FGSSColmapImage>& Images, bool bBinary, FString& OutError)
{
	// ---- text
	TArray64<uint8> CamerasTxt;
	AppendF(CamerasTxt, "# Camera list with one line of data per camera:\n");
	AppendF(CamerasTxt, "#   CAMERA_ID, MODEL, WIDTH, HEIGHT, PARAMS[]\n");
	AppendF(CamerasTxt, "# Number of cameras: %d\n", Images.Num());

	TArray64<uint8> ImagesTxt;
	AppendF(ImagesTxt, "# Image list with two lines of data per image:\n");
	AppendF(ImagesTxt, "#   IMAGE_ID, QW, QX, QY, QZ, TX, TY, TZ, CAMERA_ID, NAME\n");
	AppendF(ImagesTxt, "#   POINTS2D[] as (X, Y, POINT3D_ID)\n");
	AppendF(ImagesTxt, "# Number of images: %d, mean observations per image: 0\n", Images.Num());

	for (int32 Index = 0; Index < Images.Num(); ++Index)
	{
		const FGSSColmapImage& Image = Images[Index];
		const int32 Id = Index + 1;
		const GSS::PinholeIntrinsics& K = Image.Intrinsics;
		const GSS::ColmapExtrinsics& E = Image.Extrinsics;

		AppendF(CamerasTxt, "%d PINHOLE %d %d %.10f %.10f %.10f %.10f\n", Id, K.Width, K.Height, K.Fx, K.Fy, K.Cx, K.Cy);
		AppendF(ImagesTxt, "%d %.12f %.12f %.12f %.12f %.12f %.12f %.12f %d %s\n",
			Id, E.Qw, E.Qx, E.Qy, E.Qz, E.Tx, E.Ty, E.Tz, Id, TCHAR_TO_UTF8(*Image.ImageName));
		AppendF(ImagesTxt, "\n"); // no 2D observations
	}

	if (!SaveBuffer(CamerasTxt, SparseDir / TEXT("cameras.txt"), OutError) ||
		!SaveBuffer(ImagesTxt, SparseDir / TEXT("images.txt"), OutError))
	{
		return false;
	}

	if (!bBinary)
	{
		return true;
	}

	// ---- binary
	TArray64<uint8> CamerasBin;
	TArray64<uint8> ImagesBin;
	Put<uint64>(CamerasBin, Images.Num());
	Put<uint64>(ImagesBin, Images.Num());
	for (int32 Index = 0; Index < Images.Num(); ++Index)
	{
		const FGSSColmapImage& Image = Images[Index];
		const uint32 Id = Index + 1;
		const GSS::PinholeIntrinsics& K = Image.Intrinsics;
		const GSS::ColmapExtrinsics& E = Image.Extrinsics;

		Put<uint32>(CamerasBin, Id);
		Put<int32>(CamerasBin, ColmapPinholeModelId);
		Put<uint64>(CamerasBin, K.Width);
		Put<uint64>(CamerasBin, K.Height);
		Put<double>(CamerasBin, K.Fx);
		Put<double>(CamerasBin, K.Fy);
		Put<double>(CamerasBin, K.Cx);
		Put<double>(CamerasBin, K.Cy);

		Put<uint32>(ImagesBin, Id);
		Put<double>(ImagesBin, E.Qw);
		Put<double>(ImagesBin, E.Qx);
		Put<double>(ImagesBin, E.Qy);
		Put<double>(ImagesBin, E.Qz);
		Put<double>(ImagesBin, E.Tx);
		Put<double>(ImagesBin, E.Ty);
		Put<double>(ImagesBin, E.Tz);
		Put<uint32>(ImagesBin, Id);
		const FTCHARToUTF8 Name(*Image.ImageName);
		ImagesBin.Append(reinterpret_cast<const uint8*>(Name.Get()), Name.Length());
		Put<uint8>(ImagesBin, 0);
		Put<uint64>(ImagesBin, 0); // num points2D
	}

	return SaveBuffer(CamerasBin, SparseDir / TEXT("cameras.bin"), OutError)
		&& SaveBuffer(ImagesBin, SparseDir / TEXT("images.bin"), OutError);
}

bool GSSColmapIO::WritePoints(const FString& SparseDir, const TArray<FGSSColmapPoint>& Points, bool bBinary, FString& OutError)
{
	TArray64<uint8> Txt;
	Txt.Reserve(static_cast<int64>(Points.Num()) * 64 + 256);
	AppendF(Txt, "# 3D point list with one line of data per point:\n");
	AppendF(Txt, "#   POINT3D_ID, X, Y, Z, R, G, B, ERROR, TRACK[] as (IMAGE_ID, POINT2D_IDX)\n");
	AppendF(Txt, "# Number of points: %d, mean track length: 0\n", Points.Num());
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const FGSSColmapPoint& P = Points[Index];
		AppendF(Txt, "%d %.6f %.6f %.6f %d %d %d 0\n", Index + 1, P.X, P.Y, P.Z, P.R, P.G, P.B);
	}
	if (!SaveBuffer(Txt, SparseDir / TEXT("points3D.txt"), OutError))
	{
		return false;
	}

	if (!bBinary)
	{
		return true;
	}

	TArray64<uint8> Bin;
	Bin.Reserve(static_cast<int64>(Points.Num()) * 51 + 8);
	Put<uint64>(Bin, Points.Num());
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const FGSSColmapPoint& P = Points[Index];
		Put<uint64>(Bin, Index + 1);
		Put<double>(Bin, P.X);
		Put<double>(Bin, P.Y);
		Put<double>(Bin, P.Z);
		Put<uint8>(Bin, P.R);
		Put<uint8>(Bin, P.G);
		Put<uint8>(Bin, P.B);
		Put<double>(Bin, 0.0); // reprojection error
		Put<uint64>(Bin, 0);   // track length
	}
	return SaveBuffer(Bin, SparseDir / TEXT("points3D.bin"), OutError);
}

bool GSSColmapIO::ReadCameras(const FString& SparseDir, TArray<FGSSColmapImage>& OutImages, FString& OutError)
{
	OutImages.Reset();

	TArray<FString> CameraLines;
	TArray<FString> ImageLines;
	if (!LoadLines(SparseDir / TEXT("cameras.txt"), CameraLines, OutError) ||
		!LoadLines(SparseDir / TEXT("images.txt"), ImageLines, OutError))
	{
		return false;
	}

	TMap<int32, GSS::PinholeIntrinsics> Cameras;
	for (const FString& Line : CameraLines)
	{
		TArray<FString> T;
		Line.ParseIntoArrayWS(T);
		if (T.Num() < 8)
		{
			continue;
		}
		GSS::PinholeIntrinsics K;
		K.Width = FCString::Atoi(*T[2]);
		K.Height = FCString::Atoi(*T[3]);
		if (T[1] == TEXT("PINHOLE"))
		{
			K.Fx = FCString::Atod(*T[4]);
			K.Fy = FCString::Atod(*T[5]);
			K.Cx = FCString::Atod(*T[6]);
			K.Cy = FCString::Atod(*T[7]);
		}
		else if (T[1] == TEXT("SIMPLE_PINHOLE") || T[1] == TEXT("SIMPLE_RADIAL"))
		{
			K.Fx = K.Fy = FCString::Atod(*T[4]);
			K.Cx = FCString::Atod(*T[5]);
			K.Cy = FCString::Atod(*T[6]);
		}
		else
		{
			OutError = FString::Printf(TEXT("Unsupported COLMAP camera model %s"), *T[1]);
			return false;
		}
		Cameras.Add(FCString::Atoi(*T[0]), K);
	}

	for (const FString& Line : ImageLines)
	{
		TArray<FString> T;
		Line.ParseIntoArrayWS(T);
		// Image lines have exactly 10 tokens; POINTS2D lines always have a multiple of 3.
		if (T.Num() != 10)
		{
			continue;
		}
		const GSS::PinholeIntrinsics* K = Cameras.Find(FCString::Atoi(*T[8]));
		if (!K)
		{
			OutError = FString::Printf(TEXT("images.txt references unknown camera %s"), *T[8]);
			return false;
		}
		FGSSColmapImage& Image = OutImages.AddDefaulted_GetRef();
		Image.Intrinsics = *K;
		Image.Extrinsics.Qw = FCString::Atod(*T[1]);
		Image.Extrinsics.Qx = FCString::Atod(*T[2]);
		Image.Extrinsics.Qy = FCString::Atod(*T[3]);
		Image.Extrinsics.Qz = FCString::Atod(*T[4]);
		Image.Extrinsics.Tx = FCString::Atod(*T[5]);
		Image.Extrinsics.Ty = FCString::Atod(*T[6]);
		Image.Extrinsics.Tz = FCString::Atod(*T[7]);
		Image.ImageName = T[9];
	}

	if (OutImages.Num() == 0)
	{
		OutError = FString::Printf(TEXT("No images found in %s"), *(SparseDir / TEXT("images.txt")));
		return false;
	}
	return true;
}

bool GSSColmapIO::WritePly(const FString& Path, const TArray<FGSSColmapPoint>& Points, FString& OutError)
{
	TArray64<uint8> Buffer;
	Buffer.Reserve(static_cast<int64>(Points.Num()) * 15 + 256);
	AppendF(Buffer, "ply\nformat binary_little_endian 1.0\nelement vertex %d\n", Points.Num());
	AppendF(Buffer, "property float x\nproperty float y\nproperty float z\n");
	AppendF(Buffer, "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n");
	for (const FGSSColmapPoint& P : Points)
	{
		Put<float>(Buffer, static_cast<float>(P.X));
		Put<float>(Buffer, static_cast<float>(P.Y));
		Put<float>(Buffer, static_cast<float>(P.Z));
		Put<uint8>(Buffer, P.R);
		Put<uint8>(Buffer, P.G);
		Put<uint8>(Buffer, P.B);
	}
	return SaveBuffer(Buffer, Path, OutError);
}
