// Copyright (c) GaussianSplatSequence contributors. MIT License.
//
// Pure C++ (no Unreal dependencies) camera math shared by the camera exporter
// and the depth -> point cloud converter. Keeping both on the exact same code
// path is what guarantees that the point cloud is congruent with the exported
// COLMAP camera. The header is also compiled standalone by Tests/test_gss_math.cpp.
//
// Conventions
// -----------
// Unreal:  left-handed, X forward, Y right, Z up, centimetres.
//          A camera looks along its local +X, +Y is right, +Z is up.
// COLMAP:  right-handed. Camera looks along local +Z, +X is right, +Y is down
//          (OpenCV convention). images.txt stores the world->camera transform:
//          X_cam = R * X_world + t, quaternion as (qw, qx, qy, qz).
//          The centre of the top-left pixel is (0.5, 0.5).

#pragma once

#include <cmath>

namespace GSS
{
	struct Vec3
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;

		Vec3() = default;
		Vec3(double InX, double InY, double InZ) : X(InX), Y(InY), Z(InZ) {}

		Vec3 operator+(const Vec3& O) const { return Vec3(X + O.X, Y + O.Y, Z + O.Z); }
		Vec3 operator-(const Vec3& O) const { return Vec3(X - O.X, Y - O.Y, Z - O.Z); }
		Vec3 operator*(double S) const { return Vec3(X * S, Y * S, Z * S); }
		double Dot(const Vec3& O) const { return X * O.X + Y * O.Y + Z * O.Z; }
		double Length() const { return std::sqrt(Dot(*this)); }
	};

	// Row-major 3x3 matrix, M[Row][Col].
	struct Mat3
	{
		double M[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

		static Mat3 FromRows(const Vec3& R0, const Vec3& R1, const Vec3& R2)
		{
			Mat3 Out;
			Out.M[0][0] = R0.X; Out.M[0][1] = R0.Y; Out.M[0][2] = R0.Z;
			Out.M[1][0] = R1.X; Out.M[1][1] = R1.Y; Out.M[1][2] = R1.Z;
			Out.M[2][0] = R2.X; Out.M[2][1] = R2.Y; Out.M[2][2] = R2.Z;
			return Out;
		}

		static Mat3 FromColumns(const Vec3& C0, const Vec3& C1, const Vec3& C2)
		{
			return FromRows(C0, C1, C2).Transposed();
		}

		Mat3 Transposed() const
		{
			Mat3 Out;
			for (int R = 0; R < 3; ++R)
			{
				for (int C = 0; C < 3; ++C)
				{
					Out.M[R][C] = M[C][R];
				}
			}
			return Out;
		}

		Mat3 operator*(const Mat3& O) const
		{
			Mat3 Out;
			for (int R = 0; R < 3; ++R)
			{
				for (int C = 0; C < 3; ++C)
				{
					Out.M[R][C] = M[R][0] * O.M[0][C] + M[R][1] * O.M[1][C] + M[R][2] * O.M[2][C];
				}
			}
			return Out;
		}

		Vec3 operator*(const Vec3& V) const
		{
			return Vec3(
				M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z,
				M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
				M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z);
		}

		double Determinant() const
		{
			return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
				- M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
				+ M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
		}
	};

	// Target world axis convention for the exported dataset.
	enum class EWorldAxes : int
	{
		// x = UE right (Y), y = UE down (-Z), z = UE forward (X). Same axes as an
		// OpenCV camera with identity rotation; what most COLMAP/3DGS viewers expect.
		OpenCV_YDown = 0,
		// x = UE X, y = -UE Y, z = UE Z. Right-handed Z-up (Blender-like).
		RightHanded_ZUp = 1,
		// x = UE Y, y = UE Z, z = -UE X. Right-handed Y-up (OpenGL-like).
		OpenGL_YUp = 2,
	};

	// Maps a direction/position expressed in UE world axes into the target world axes.
	// All variants are reflections (det = -1) because UE is left-handed.
	inline Mat3 WorldAxesMatrix(EWorldAxes Axes)
	{
		switch (Axes)
		{
		case EWorldAxes::RightHanded_ZUp:
			return Mat3::FromRows(Vec3(1, 0, 0), Vec3(0, -1, 0), Vec3(0, 0, 1));
		case EWorldAxes::OpenGL_YUp:
			return Mat3::FromRows(Vec3(0, 1, 0), Vec3(0, 0, 1), Vec3(-1, 0, 0));
		case EWorldAxes::OpenCV_YDown:
		default:
			return Mat3::FromRows(Vec3(0, 1, 0), Vec3(0, 0, -1), Vec3(1, 0, 0));
		}
	}

	// Maps UE camera-local axes (forward, right, up) to OpenCV camera-local axes
	// (right, down, forward): cv = P * ue.
	inline Mat3 UECameraToCVCamera()
	{
		return Mat3::FromRows(Vec3(0, 1, 0), Vec3(0, 0, -1), Vec3(1, 0, 0));
	}

	struct PinholeIntrinsics
	{
		int Width = 0;
		int Height = 0;
		double Fx = 0.0;
		double Fy = 0.0;
		double Cx = 0.0;
		double Cy = 0.0;

		// Same camera, different pixel grid (e.g. depth rendered at half resolution).
		PinholeIntrinsics Rescaled(int NewWidth, int NewHeight) const
		{
			PinholeIntrinsics Out;
			const double Sx = static_cast<double>(NewWidth) / static_cast<double>(Width);
			const double Sy = static_cast<double>(NewHeight) / static_cast<double>(Height);
			Out.Width = NewWidth;
			Out.Height = NewHeight;
			Out.Fx = Fx * Sx;
			Out.Fy = Fy * Sy;
			Out.Cx = Cx * Sx;
			Out.Cy = Cy * Sy;
			return Out;
		}
	};

	// Unreal's FOV is horizontal (AspectRatioAxisConstraint = Maintain X FOV, the default).
	inline PinholeIntrinsics IntrinsicsFromHorizontalFov(int Width, int Height, double HorizontalFovDegrees)
	{
		const double Pi = 3.14159265358979323846;
		PinholeIntrinsics K;
		K.Width = Width;
		K.Height = Height;
		K.Fx = (0.5 * Width) / std::tan(0.5 * HorizontalFovDegrees * Pi / 180.0);
		K.Fy = K.Fx; // square pixels
		K.Cx = 0.5 * Width;
		K.Cy = 0.5 * Height;
		return K;
	}

	// Camera-to-world pose in the target (right-handed) world.
	struct CameraPose
	{
		Mat3 RotationC2W; // columns: camera right, down, forward in world
		Vec3 Center;      // camera centre in world (target units)
	};

	// UEForward/Right/Up: the camera's unit axes in UE world space.
	// UELocation: camera location in UE units. UnitScale: e.g. 0.01 for cm -> m.
	inline CameraPose PoseFromUnreal(const Vec3& UEForward, const Vec3& UERight, const Vec3& UEUp,
		const Vec3& UELocation, EWorldAxes Axes, double UnitScale)
	{
		const Mat3 W = WorldAxesMatrix(Axes);
		const Mat3 RUE = Mat3::FromColumns(UEForward, UERight, UEUp); // UE cam -> UE world
		CameraPose Pose;
		// cv cam -> ue cam -> ue world -> target world
		Pose.RotationC2W = W * RUE * UECameraToCVCamera().Transposed();
		Pose.Center = (W * UELocation) * UnitScale;
		return Pose;
	}

	// Quaternion (Hamilton, w first) from a proper rotation matrix.
	inline void RotationToQuaternion(const Mat3& R, double& Qw, double& Qx, double& Qy, double& Qz)
	{
		const double Trace = R.M[0][0] + R.M[1][1] + R.M[2][2];
		if (Trace > 0.0)
		{
			const double S = std::sqrt(Trace + 1.0) * 2.0;
			Qw = 0.25 * S;
			Qx = (R.M[2][1] - R.M[1][2]) / S;
			Qy = (R.M[0][2] - R.M[2][0]) / S;
			Qz = (R.M[1][0] - R.M[0][1]) / S;
		}
		else if (R.M[0][0] > R.M[1][1] && R.M[0][0] > R.M[2][2])
		{
			const double S = std::sqrt(1.0 + R.M[0][0] - R.M[1][1] - R.M[2][2]) * 2.0;
			Qw = (R.M[2][1] - R.M[1][2]) / S;
			Qx = 0.25 * S;
			Qy = (R.M[0][1] + R.M[1][0]) / S;
			Qz = (R.M[0][2] + R.M[2][0]) / S;
		}
		else if (R.M[1][1] > R.M[2][2])
		{
			const double S = std::sqrt(1.0 + R.M[1][1] - R.M[0][0] - R.M[2][2]) * 2.0;
			Qw = (R.M[0][2] - R.M[2][0]) / S;
			Qx = (R.M[0][1] + R.M[1][0]) / S;
			Qy = 0.25 * S;
			Qz = (R.M[1][2] + R.M[2][1]) / S;
		}
		else
		{
			const double S = std::sqrt(1.0 + R.M[2][2] - R.M[0][0] - R.M[1][1]) * 2.0;
			Qw = (R.M[1][0] - R.M[0][1]) / S;
			Qx = (R.M[0][2] + R.M[2][0]) / S;
			Qy = (R.M[1][2] + R.M[2][1]) / S;
			Qz = 0.25 * S;
		}

		const double Norm = std::sqrt(Qw * Qw + Qx * Qx + Qy * Qy + Qz * Qz);
		const double Sign = (Qw < 0.0) ? -1.0 : 1.0; // canonical: qw >= 0
		Qw *= Sign / Norm;
		Qx *= Sign / Norm;
		Qy *= Sign / Norm;
		Qz *= Sign / Norm;
	}

	inline Mat3 QuaternionToRotation(double Qw, double Qx, double Qy, double Qz)
	{
		const double Norm = std::sqrt(Qw * Qw + Qx * Qx + Qy * Qy + Qz * Qz);
		const double W = Qw / Norm, X = Qx / Norm, Y = Qy / Norm, Z = Qz / Norm;
		return Mat3::FromRows(
			Vec3(1 - 2 * (Y * Y + Z * Z), 2 * (X * Y - Z * W), 2 * (X * Z + Y * W)),
			Vec3(2 * (X * Y + Z * W), 1 - 2 * (X * X + Z * Z), 2 * (Y * Z - X * W)),
			Vec3(2 * (X * Z - Y * W), 2 * (Y * Z + X * W), 1 - 2 * (X * X + Y * Y)));
	}

	// The (qvec, tvec) pair COLMAP stores in images.txt / images.bin.
	struct ColmapExtrinsics
	{
		double Qw = 1.0, Qx = 0.0, Qy = 0.0, Qz = 0.0;
		double Tx = 0.0, Ty = 0.0, Tz = 0.0;
	};

	inline ColmapExtrinsics ToColmapExtrinsics(const CameraPose& Pose)
	{
		const Mat3 RW2C = Pose.RotationC2W.Transposed();
		const Vec3 T = (RW2C * Pose.Center) * -1.0;
		ColmapExtrinsics E;
		RotationToQuaternion(RW2C, E.Qw, E.Qx, E.Qy, E.Qz);
		E.Tx = T.X;
		E.Ty = T.Y;
		E.Tz = T.Z;
		return E;
	}

	inline CameraPose FromColmapExtrinsics(const ColmapExtrinsics& E)
	{
		const Mat3 RW2C = QuaternionToRotation(E.Qw, E.Qx, E.Qy, E.Qz);
		CameraPose Pose;
		Pose.RotationC2W = RW2C.Transposed();
		Pose.Center = (Pose.RotationC2W * Vec3(E.Tx, E.Ty, E.Tz)) * -1.0;
		return Pose;
	}

	// How the depth value in the EXR is measured.
	enum class EDepthType : int
	{
		// Distance along the camera forward axis (Unreal SceneDepth). Default.
		Planar = 0,
		// Euclidean distance from the camera centre to the surface point.
		Radial = 1,
	};

	// Unprojects pixel (PixelX, PixelY) (integer pixel index, top-left = 0,0) with a
	// depth that is already in target units. Returns the world position.
	inline Vec3 UnprojectPixel(const PinholeIntrinsics& K, const CameraPose& Pose,
		double PixelX, double PixelY, double Depth, EDepthType DepthType)
	{
		// Pixel centre in COLMAP convention.
		const Vec3 Ray((PixelX + 0.5 - K.Cx) / K.Fx, (PixelY + 0.5 - K.Cy) / K.Fy, 1.0);
		const double Planar = (DepthType == EDepthType::Radial) ? Depth / Ray.Length() : Depth;
		const Vec3 PCam = Ray * Planar;
		return Pose.RotationC2W * PCam + Pose.Center;
	}

	// Projects a world point into the image. Returns false when behind the camera.
	// OutU/OutV are continuous COLMAP image coordinates (pixel centre = index + 0.5).
	inline bool ProjectPoint(const PinholeIntrinsics& K, const CameraPose& Pose, const Vec3& World,
		double& OutU, double& OutV, double& OutPlanarDepth)
	{
		const Vec3 PCam = Pose.RotationC2W.Transposed() * (World - Pose.Center);
		OutPlanarDepth = PCam.Z;
		if (PCam.Z <= 0.0)
		{
			return false;
		}
		OutU = K.Fx * PCam.X / PCam.Z + K.Cx;
		OutV = K.Fy * PCam.Y / PCam.Z + K.Cy;
		return true;
	}
}
