// Copyright (c) GaussianSplatSequence contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GSSMath.h"

/** One registered image (= one exported camera) of a COLMAP sparse model. */
struct FGSSColmapImage
{
	GSS::PinholeIntrinsics Intrinsics;
	GSS::ColmapExtrinsics Extrinsics;
	FString ImageName;
};

struct FGSSColmapPoint
{
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	uint8 R = 0;
	uint8 G = 0;
	uint8 B = 0;
};

/**
 * Minimal COLMAP sparse model reader/writer (text + binary) and PLY writer.
 * Every image gets its own PINHOLE camera (camera id == image id).
 * Format reference: https://colmap.github.io/format.html
 */
namespace GSSColmapIO
{
	/** Writes cameras.{txt,bin} and images.{txt,bin} into SparseDir (e.g. <frame>/sparse/0). */
	bool WriteCameras(const FString& SparseDir, const TArray<FGSSColmapImage>& Images, bool bBinary, FString& OutError);

	/** Writes points3D.{txt,bin}. Tracks are left empty (the points come from ground-truth depth, not from matching). */
	bool WritePoints(const FString& SparseDir, const TArray<FGSSColmapPoint>& Points, bool bBinary, FString& OutError);

	/** Reads cameras.txt + images.txt from SparseDir. */
	bool ReadCameras(const FString& SparseDir, TArray<FGSSColmapImage>& OutImages, FString& OutError);

	/** Binary little-endian PLY with float xyz + uchar rgb. */
	bool WritePly(const FString& Path, const TArray<FGSSColmapPoint>& Points, FString& OutError);
}
