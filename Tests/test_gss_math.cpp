// Standalone test for Source/GaussianSplatSequence/Public/GSSMath.h (no Unreal needed).
//   g++ -std=c++17 -O2 -I../Source/GaussianSplatSequence/Public test_gss_math.cpp -o test_gss_math && ./test_gss_math

#include "GSSMath.h"

#include <cstdio>
#include <cstdlib>
#include <random>

using namespace GSS;

static int GFailures = 0;

#define CHECK_NEAR(A, B, Tol) \
	do { const double _a = (A), _b = (B); if (std::fabs(_a - _b) > (Tol)) { \
		std::printf("FAIL %s:%d  %s = %.9f, expected %.9f\n", __FILE__, __LINE__, #A, _a, _b); ++GFailures; } } while (0)

#define CHECK(Cond) \
	do { if (!(Cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); ++GFailures; } } while (0)

// Exact replica of Unreal's FRotationMatrix(FRotator(Pitch, Yaw, Roll)) axes (degrees).
static void UERotatorAxes(double Pitch, double Yaw, double Roll, Vec3& Fwd, Vec3& Right, Vec3& Up)
{
	const double D = 3.14159265358979323846 / 180.0;
	const double SP = std::sin(Pitch * D), CP = std::cos(Pitch * D);
	const double SY = std::sin(Yaw * D), CY = std::cos(Yaw * D);
	const double SR = std::sin(Roll * D), CR = std::cos(Roll * D);
	Fwd = Vec3(CP * CY, CP * SY, SP);
	Right = Vec3(SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP);
	Up = Vec3(-(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP);
}

static void TestIdentityCamera()
{
	// Camera at UE (100, 200, 300) cm, looking down +X: in OpenCV world axes the
	// camera frame equals the world frame.
	const CameraPose Pose = PoseFromUnreal(Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1),
		Vec3(100, 200, 300), EWorldAxes::OpenCV_YDown, 0.01);
	for (int R = 0; R < 3; ++R)
	{
		for (int C = 0; C < 3; ++C)
		{
			CHECK_NEAR(Pose.RotationC2W.M[R][C], R == C ? 1.0 : 0.0, 1e-12);
		}
	}
	CHECK_NEAR(Pose.Center.X, 2.0, 1e-12);
	CHECK_NEAR(Pose.Center.Y, -3.0, 1e-12);
	CHECK_NEAR(Pose.Center.Z, 1.0, 1e-12);

	const ColmapExtrinsics E = ToColmapExtrinsics(Pose);
	CHECK_NEAR(E.Qw, 1.0, 1e-12);
	CHECK_NEAR(E.Tx, -2.0, 1e-12);
	CHECK_NEAR(E.Ty, 3.0, 1e-12);
	CHECK_NEAR(E.Tz, -1.0, 1e-12);
}

static void TestRandomPoses()
{
	std::mt19937 Rng(1234);
	std::uniform_real_distribution<double> Angle(-180.0, 180.0);
	std::uniform_real_distribution<double> Pitch(-89.0, 89.0);
	std::uniform_real_distribution<double> Pos(-5000.0, 5000.0);
	std::uniform_real_distribution<double> Offset(-300.0, 300.0);
	std::uniform_real_distribution<double> Dist(50.0, 20000.0);

	const EWorldAxes AllAxes[] = { EWorldAxes::OpenCV_YDown, EWorldAxes::RightHanded_ZUp, EWorldAxes::OpenGL_YUp };

	for (int Iter = 0; Iter < 2000; ++Iter)
	{
		const EWorldAxes Axes = AllAxes[Iter % 3];
		CHECK_NEAR(WorldAxesMatrix(Axes).Determinant(), -1.0, 1e-12);

		Vec3 Fwd, Right, Up;
		UERotatorAxes(Pitch(Rng), Angle(Rng), Angle(Rng), Fwd, Right, Up);
		const Vec3 Loc(Pos(Rng), Pos(Rng), Pos(Rng));
		const double Scale = 0.01;
		const CameraPose Pose = PoseFromUnreal(Fwd, Right, Up, Loc, Axes, Scale);

		// Proper rotation.
		CHECK_NEAR(Pose.RotationC2W.Determinant(), 1.0, 1e-9);
		const Mat3 RtR = Pose.RotationC2W.Transposed() * Pose.RotationC2W;
		for (int R = 0; R < 3; ++R)
		{
			for (int C = 0; C < 3; ++C)
			{
				CHECK_NEAR(RtR.M[R][C], R == C ? 1.0 : 0.0, 1e-9);
			}
		}

		// COLMAP extrinsics round trip.
		const CameraPose Back = FromColmapExtrinsics(ToColmapExtrinsics(Pose));
		CHECK_NEAR((Back.Center - Pose.Center).Length(), 0.0, 1e-8);
		for (int R = 0; R < 3; ++R)
		{
			for (int C = 0; C < 3; ++C)
			{
				CHECK_NEAR(Back.RotationC2W.M[R][C], Pose.RotationC2W.M[R][C], 1e-9);
			}
		}

		// Semantic check: a UE point that is D cm in front, dx cm to the right and dz cm
		// above the camera must land right of / above the principal point.
		const PinholeIntrinsics K = IntrinsicsFromHorizontalFov(1920, 1080, 60.0);
		const double D = Dist(Rng), Dx = Offset(Rng), Dz = Offset(Rng);
		const Vec3 UEPoint = Loc + Fwd * D + Right * Dx + Up * Dz;
		const Vec3 WorldPoint = (WorldAxesMatrix(Axes) * UEPoint) * Scale;
		double U = 0, V = 0, Z = 0;
		CHECK(ProjectPoint(K, Pose, WorldPoint, U, V, Z));
		CHECK_NEAR(Z, D * Scale, 1e-7);
		CHECK_NEAR(U, K.Cx + K.Fx * Dx / D, 1e-6);
		CHECK_NEAR(V, K.Cy - K.Fy * Dz / D, 1e-6);

		// Depth unprojection reproduces the UE point for planar and radial depth.
		const double Px = U - 0.5, Py = V - 0.5; // pixel index whose centre is (U, V)
		const Vec3 FromPlanar = UnprojectPixel(K, Pose, Px, Py, D * Scale, EDepthType::Planar);
		CHECK_NEAR((FromPlanar - WorldPoint).Length(), 0.0, 1e-7);
		const double Radial = (UEPoint - Loc).Length() * Scale;
		const Vec3 FromRadial = UnprojectPixel(K, Pose, Px, Py, Radial, EDepthType::Radial);
		CHECK_NEAR((FromRadial - WorldPoint).Length(), 0.0, 1e-7);
	}
}

static void TestIntrinsics()
{
	const PinholeIntrinsics K = IntrinsicsFromHorizontalFov(1920, 1080, 90.0);
	CHECK_NEAR(K.Fx, 960.0, 1e-9);
	CHECK_NEAR(K.Fy, 960.0, 1e-9);
	const PinholeIntrinsics H = K.Rescaled(960, 540);
	CHECK_NEAR(H.Fx, 480.0, 1e-9);
	CHECK_NEAR(H.Cx, 480.0, 1e-9);
	CHECK_NEAR(H.Cy, 270.0, 1e-9);
}

int main()
{
	TestIdentityCamera();
	TestIntrinsics();
	TestRandomPoses();
	if (GFailures == 0)
	{
		std::printf("All GSSMath tests passed.\n");
		return EXIT_SUCCESS;
	}
	std::printf("%d failure(s).\n", GFailures);
	return EXIT_FAILURE;
}
