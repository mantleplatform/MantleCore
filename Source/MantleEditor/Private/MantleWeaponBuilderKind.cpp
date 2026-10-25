// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderKind.h"

#include "MantleWeaponBuilderExport.h"
#include "MantleWeaponBuilderLogic.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"

#define LOCTEXT_NAMESPACE "MantleWeaponBuilder"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWeaponBuilderKindPrivate
{
	const FName GripR(TEXT("Grip_R"));
	const FName GripL(TEXT("Grip_L"));
	const FName Muzzle(TEXT("Muzzle"));
	const FName Sight(TEXT("Sight"));

	/** How far behind the sight the eye sits when aiming a shouldered gun (cm, in the weapon frame). */
	constexpr double EyeBehindSightCm = 8.0;

	/** A pistol is held out in both hands in front of the face, so the eye sits this far behind the sight. */
	constexpr double ExtendedEyeReliefCm = 32.0;

	/** Shouldered hold: the head's centre sits this far to the left of the bore line (cheek beside the stock) and this far behind the eye. */
	constexpr double CheekOffsetCm = 10.0;
	constexpr double HeadBehindEyeCm = 6.0;

	/** Shouldered hold: how far behind the stock's rear the shoulder joint sits, and how far the body turns (degrees) so the left shoulder comes forward to the handguard. */
	constexpr double ShoulderBehindStockCm = 3.0;

	/** Shouldered hold: the butt sits in the pocket just inside the shoulder joint, so the joint is this far to the right of the
	 *  butt's middle and this far behind it, at the butt's height (the shooter raises the shoulder to the stock). */
	constexpr double ShoulderJointRightOfButtCm = 5.0;
	constexpr double ShoulderJointBehindButtCm = 2.0;
	/** Shouldered hold: the body is turned this far about the right shoulder (degrees). */
	constexpr double ShoulderedBodyYawDegrees = 40.0;
	constexpr double MaxShoulderBehindHeadCm = 28.0;

	/** Aiming: the eye is about this far to the right of the head's centre, so the head's centre sits this far left of the eye (cm). */
	constexpr double AimHeadLeftOfEyeCm = 3.5;

	/** The surface samples are about this far apart (cm) and never more than this many. */
	constexpr double SampleSpacingCm = 1.0;
	constexpr int32 MaxSamples = 40000;

	bool IsTwoHanded(EMantleWeaponFitHold Hold)
	{
		return FMantleWeaponDocument::GetHoldInfo(static_cast<EMantleWeaponDocHold>(Hold)).bTwoHanded;
	}

	/** True for an arm capsule (upper arm, lower arm, hand) of a side whose arm is posed to the weapon: it is replaced by the solved arm. */
	bool ShouldSkipPosedArmCapsule(const FString& Name, bool bRightPosed, bool bLeftPosed)
	{
		const bool bArmPart = Name.StartsWith(TEXT("UpperArm_")) || Name.StartsWith(TEXT("LowerArm_")) || Name.StartsWith(TEXT("Hand_"));
		return bArmPart && ((bRightPosed && Name.EndsWith(TEXT("_R"))) || (bLeftPosed && Name.EndsWith(TEXT("_L"))));
	}

	/** Appends a mesh description's vertices and triangles. */
	void AppendDescription(const FMeshDescription& Description, TArray<FVector3f>& OutPositions, TArray<int32>& OutTriangles)
	{
		const FStaticMeshConstAttributes Attributes(Description);
		const TVertexAttributesConstRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TMap<FVertexID, int32> Index;
		Index.Reserve(Description.Vertices().Num());
		for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
		{
			Index.Add(Vertex, OutPositions.Num());
			OutPositions.Add(VertexPositions[Vertex]);
		}
		for (const FTriangleID Triangle : Description.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexInstanceID> Corners = Description.GetTriangleVertexInstances(Triangle);
			for (const FVertexInstanceID Corner : Corners)
			{
				const int32* Found = Index.Find(Description.GetVertexInstanceVertex(Corner));
				OutTriangles.Add(Found ? *Found : 0);
			}
		}
	}

	/** A mesh's LOD0 in mesh space (a skeletal mesh in its reference pose, which is what a mesh description holds). */
	bool ReadMesh(const UObject* Mesh, TArray<FVector3f>& OutPositions, TArray<int32>& OutTriangles)
	{
		if (const UStaticMesh* ConstStatic = Cast<UStaticMesh>(Mesh))
		{
			UStaticMesh* Static = const_cast<UStaticMesh*>(ConstStatic);
			TArray<UStaticMesh*> Compiling = { Static };
			FStaticMeshCompilingManager::Get().FinishCompilation(Compiling);
			if (Static->IsMeshDescriptionValid(0))
			{
				if (const FMeshDescription* Description = Static->GetMeshDescription(0))
				{
					AppendDescription(*Description, OutPositions, OutTriangles);
				}
			}
			if (OutPositions.Num() == 0)
			{
				const FStaticMeshRenderData* RenderData = Static->GetRenderData();
				if (RenderData && RenderData->LODResources.Num() > 0)
				{
					const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
					const uint32 NumVertices = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
					for (uint32 Vertex = 0; Vertex < NumVertices; ++Vertex)
					{
						OutPositions.Add(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Vertex));
					}
					const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
					for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
					{
						if (Indices[I] < NumVertices && Indices[I + 1] < NumVertices && Indices[I + 2] < NumVertices)
						{
							OutTriangles.Add(Indices[I]);
							OutTriangles.Add(Indices[I + 1]);
							OutTriangles.Add(Indices[I + 2]);
						}
					}
				}
			}
			return OutPositions.Num() > 0;
		}
		if (const USkeletalMesh* ConstSkeletal = Cast<USkeletalMesh>(Mesh))
		{
			USkeletalMesh* Skeletal = const_cast<USkeletalMesh*>(ConstSkeletal);
			TArray<USkinnedAsset*> Compiling = { Skeletal };
			FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
			FMeshDescription Description;
			bool bRead = Skeletal->HasMeshDescription(0) && Skeletal->CloneMeshDescription(0, Description) && Description.Triangles().Num() > 0;
			if (!bRead)
			{
				Description.Empty();
				const FSkeletalMeshModel* Model = Skeletal->GetImportedModel();
				if (Model && Model->LODModels.IsValidIndex(0) && Model->LODModels[0].NumVertices > 0)
				{
					Model->LODModels[0].GetMeshDescription(Skeletal, 0, Description);
					bRead = Description.Triangles().Num() > 0;
				}
			}
			if (bRead)
			{
				AppendDescription(Description, OutPositions, OutTriangles);
			}
			return OutPositions.Num() > 0;
		}
		return false;
	}

	/** One arm of the ghost, in the weapon frame. */
	struct FArm
	{
		bool bPosed = false;
		bool bReached = true;
		TArray<FMantleGhostCapsule> Capsules;
	};

	struct FPose
	{
		/** The ghost without the posed arms, then the posed arms; all in the weapon frame. */
		TArray<FMantleGhostCapsule> Capsules;
		FArm Right;
		FArm Left;
	};

	FMantleGhostCapsule MakeCapsule(const TCHAR* Part, const TCHAR* Side, const FVector& Start, const FVector& End, float Radius, bool bWarning)
	{
		FMantleGhostCapsule Capsule;
		Capsule.Part = FName(*FString::Printf(TEXT("%s_%s"), Part, Side));
		Capsule.Start = Start;
		Capsule.End = End;
		Capsule.RadiusCm = Radius;
		Capsule.bWarning = bWarning;
		return Capsule;
	}

	/** Solves one arm from the shoulder and adds its capsules to Arm; the elbow bends toward Outward and Down (the body's own down).
	 *  To a hand point (no Wrist): the forearm stops at the wrist and a hand capsule is centred on the point (the palm, where the
	 *  game puts it). To a Wrist (a hand drawn on its own, which starts there): the upper arm and forearm only, the forearm ending on
	 *  the wrist, so the arm runs into the hand with no gap. */
	void SolveArm(FArm& Arm, const TCHAR* Side, const FVector& Shoulder, const FVector& Hand, const FVector& Outward, const FVector* Wrist = nullptr,
		const FVector& Down = FVector(0.0, 0.0, -1.0))
	{
		FVector Mid, WristAt;
		const FVector Pole = (Outward + Down).GetSafeNormal();
		const double HandLength = FMantleBodyMeasurements::HandLengthCm();
		const FVector WristTarget = Wrist ? *Wrist : Hand - (Hand - Shoulder).GetSafeNormal() * (HandLength * 0.5);
		FMantleBodyMeasurements::SolveLimb(Shoulder, WristTarget, FMantleBodyMeasurements::UpperArmCm(), FMantleBodyMeasurements::ForearmCm(), Pole, Mid, WristAt, Arm.bReached);
		Arm.bPosed = true;
		const bool bWarn = !Arm.bReached;
		const float H = FMantleBodyMeasurements::HeightCm;
		Arm.Capsules.Add(MakeCapsule(TEXT("UpperArm"), Side, Shoulder, Mid, H * 0.025f, bWarn));
		Arm.Capsules.Add(MakeCapsule(TEXT("LowerArm"), Side, Mid, WristAt, H * 0.02f, bWarn));
		if (!Wrist)
		{
			// Through the hand point when the arm reaches; along the forearm when it falls short.
			const FVector Along = (Arm.bReached ? Hand - WristAt : WristAt - Mid).GetSafeNormal();
			Arm.Capsules.Add(MakeCapsule(TEXT("Hand"), Side, WristAt, WristAt + Along * HandLength, FMantleBodyMeasurements::HandWidthCm() * 0.5f, bWarn));
		}
	}

	/** Where the rear of the gun is along the bore (weapon frame): the fit's measured rear, never ahead of the right hand. */
	double RearX(const FMantleWeaponFitResult& Fit, double HandRX)
	{
		const double Rear = Fit.GripFromRearCm > 0.f ? -double(Fit.GripFromRearCm) : HandRX - 25.0;
		return FMath::Min(Rear, HandRX - 2.0);
	}

	/**
	 * The ghost holding the gun, in the weapon frame (bore +X, up +Z, +Y to the shooter's right), each arm solved to its hand point.
	 * Shouldered (rifle and the like): the right shoulder sits just behind the stock's rear on the bore line, the body is turned
	 * about it so the left shoulder comes forward, and the head leans in with the cheek beside the stock, the eye behind the sight.
	 * Otherwise (pistol, revolver): arms out in front, the head centred behind the gun with the eye well back from the sight.
	 */
	FPose BuildPose(const TArray<FMantleBuilderPoint>& Points, const FTransform& MeshToWeapon, bool bShouldered, double RearXCm, const FVector* AimEye = nullptr,
		const double* ButtZ = nullptr, const FVector* WristR = nullptr, const FVector* WristL = nullptr)
	{
		const FMantleBuilderPoint* PointR = MantleBuilderPoints::Find(Points, GripR);
		const FMantleBuilderPoint* PointL = MantleBuilderPoints::Find(Points, GripL);
		const FMantleBuilderPoint* PointMuzzle = MantleBuilderPoints::Find(Points, Muzzle);
		const FMantleBuilderPoint* PointSight = MantleBuilderPoints::Find(Points, Sight);

		const FVector HandR = PointR ? MeshToWeapon.TransformPosition(PointR->Transform.GetLocation()) : FVector::ZeroVector;
		const double Relief = bShouldered ? EyeBehindSightCm : ExtendedEyeReliefCm;
		FVector Eye;
		if (PointSight)
		{
			const FQuat SightRotation = MeshToWeapon.GetRotation() * PointSight->Transform.GetRotation();
			Eye = MeshToWeapon.TransformPosition(PointSight->Transform.GetLocation()) - SightRotation.GetForwardVector() * Relief;
		}
		else
		{
			// No sight yet: behind and above the right hand, about where a shooter's eye is.
			Eye = HandR + FVector(-Relief, 0.0, 15.0);
		}

		if (AimEye)
		{
			// Aiming: the eye is where the game puts it (measured from the rear of the gun), not the guess above.
			Eye = *AimEye;
		}

		// The bore line: through the muzzle, else the sight, else the right hand.
		double BoreY = HandR.Y;
		if (PointMuzzle)
		{
			BoreY = MeshToWeapon.TransformPosition(PointMuzzle->Transform.GetLocation()).Y;
		}
		else if (PointSight)
		{
			BoreY = MeshToWeapon.TransformPosition(PointSight->Transform.GetLocation()).Y;
		}

		const double ShoulderY = FMantleBodyMeasurements::ShoulderWidthCm() * 0.5;
		const double ShoulderZ = FMantleBodyMeasurements::ShoulderHeightCm();

		FVector HeadCenter;
		FVector RightShoulder;
		double YawDegrees = 0.0;
		if (bShouldered)
		{
			HeadCenter = FVector(Eye.X - HeadBehindEyeCm, BoreY - CheekOffsetCm, Eye.Z);
			if (AimEye)
			{
				// Aiming: the cheek is on the stock (the head a few cm to the left of the eye) and the shoulder is at the butt, so a stock that is too long shows through the shoulder.
				// Beside the stock, as without aiming: the ghost's round head would otherwise swallow the stock the cheek rests on.
				RightShoulder.X = FMath::Min(RearXCm, HeadCenter.X - 4.0);
			}
			else
			{
				RightShoulder.X = FMath::Clamp(RearXCm - ShoulderBehindStockCm, HeadCenter.X - MaxShoulderBehindHeadCm, HeadCenter.X - 4.0);
			}
			RightShoulder.Y = BoreY;
			if (ButtZ)
			{
				// The butt in the shoulder pocket: the joint just right of and behind the butt's middle, at its height.
				RightShoulder.X = FMath::Min(RearXCm - ShoulderJointBehindButtCm, HeadCenter.X - 4.0);
				RightShoulder.Y = BoreY + ShoulderJointRightOfButtCm;
			}
			YawDegrees = ShoulderedBodyYawDegrees;
		}
		else
		{
			HeadCenter = FVector(Eye.X - 5.0, AimEye ? Eye.Y - AimHeadLeftOfEyeCm : BoreY, Eye.Z);
			RightShoulder.X = HeadCenter.X - 4.0;
			RightShoulder.Y = BoreY + ShoulderY;
		}
		RightShoulder.Z = (bShouldered && ButtZ) ? *ButtZ : Eye.Z - (FMantleBodyMeasurements::EyeHeightCm() - ShoulderZ);

		// A point of the standing character (feet at the origin, right shoulder at (0, ShoulderY, ShoulderZ)) turned about the right shoulder.
		const FRotator Turn(0.0, YawDegrees, 0.0);
		auto ToWeapon = [&](const FVector& Character)
		{
			const FVector Flat = Turn.RotateVector(FVector(Character.X, Character.Y - ShoulderY, 0.0));
			return FVector(RightShoulder.X + Flat.X, RightShoulder.Y + Flat.Y, RightShoulder.Z - ShoulderZ + Character.Z);
		};

		FPose Pose;
		if (PointR)
		{
			SolveArm(Pose.Right, TEXT("R"), RightShoulder, HandR, Turn.RotateVector(FVector(0.0, 1.0, 0.0)), WristR);
		}
		if (PointL)
		{
			SolveArm(Pose.Left, TEXT("L"), ToWeapon(FVector(0.0, -ShoulderY, ShoulderZ)), MeshToWeapon.TransformPosition(PointL->Transform.GetLocation()), Turn.RotateVector(FVector(0.0, -1.0, 0.0)), WristL);
		}

		for (const FMantleGhostCapsule& Capsule : FMantleBodyMeasurements::StandingGhost())
		{
			const FString Name = Capsule.Part.ToString();
			if (ShouldSkipPosedArmCapsule(Name, Pose.Right.bPosed, Pose.Left.bPosed))
			{
				continue;
			}
			FMantleGhostCapsule Moved = Capsule;
			if (Name == TEXT("Head"))
			{
				// The head is placed on its own: it is upright, so only its height along the capsule matters.
				const double MidZ = (Capsule.Start.Z + Capsule.End.Z) * 0.5;
				Moved.Start = FVector(HeadCenter.X, HeadCenter.Y, HeadCenter.Z + Capsule.Start.Z - MidZ);
				Moved.End = FVector(HeadCenter.X, HeadCenter.Y, HeadCenter.Z + Capsule.End.Z - MidZ);
			}
			else
			{
				Moved.Start = ToWeapon(Capsule.Start);
				Moved.End = ToWeapon(Capsule.End);
			}
			Pose.Capsules.Add(Moved);
		}
		Pose.Capsules.Append(Pose.Right.Capsules);
		Pose.Capsules.Append(Pose.Left.Capsules);
		return Pose;
	}

	FMantleBuilderPointRole MakeRole(const TCHAR* Name, const FText& Label, const FText& Help, EMantleBuilderPointShape Shape, bool bRequired, bool bRotates)
	{
		FMantleBuilderPointRole Role;
		Role.Role = FName(Name);
		Role.Label = Label;
		Role.Help = Help;
		Role.Shape = Shape;
		Role.bRequired = bRequired;
		Role.bRotates = bRotates;
		Role.DefaultParent = NAME_None;
		return Role;
	}
}

FMantleWeaponBuilderKind::FMantleWeaponBuilderKind(EMantleWeaponFitHold InHold)
	: Hold(InHold)
{
}

TArray<FMantleBuilderPointRole> FMantleWeaponBuilderKind::GetRoles() const
{
	using namespace MantleWeaponBuilderKindPrivate;
	const bool bTwoHanded = IsTwoHanded(Hold);
	TArray<FMantleBuilderPointRole> Roles;
	if (Hold >= EMantleWeaponFitHold::Melee1h)
	{
		const auto& Info = FMantleWeaponDocument::GetHoldInfo(static_cast<EMantleWeaponDocHold>(Hold));
		auto Add = [&](EMantleWeaponDocSocket Socket, bool bRequired)
		{
			const FName Name = MantleWeaponBuilderLogic::SocketRole(Socket);
			const bool bHand = Socket == EMantleWeaponDocSocket::GripR || Socket == EMantleWeaponDocSocket::GripL || Socket == EMantleWeaponDocSocket::Grip;
			const FText Help = bHand ? LOCTEXT("HeldGripHelp", "The middle of the hand's palm on the handle. Check its position and orientation.")
				: LOCTEXT("HeldContactHelp", "The weapon's contact or launch point. The arrow points along the edge, cast, arrow or shield face.");
			Roles.Add(MakeRole(*Name.ToString(), FText::FromName(Name), Help, bHand ? EMantleBuilderPointShape::Hand : EMantleBuilderPointShape::Arrow, bRequired, true));
		};
		for (auto Socket : Info.Sockets) Add(Socket, true);
		for (auto Socket : Info.OptionalSockets) Add(Socket, false);
		return Roles;
	}
	Roles.Add(MakeRole(TEXT("Grip_R"), LOCTEXT("GripR", "Right hand"),
		LOCTEXT("GripRHelp", "The middle of the shooting hand's palm, on the grip. The trigger finger rests just ahead of it."),
		EMantleBuilderPointShape::Hand, true, true));
	Roles.Add(MakeRole(TEXT("Grip_L"), LOCTEXT("GripL", "Left hand"),
		bTwoHanded ? LOCTEXT("GripLHelp", "The middle of the other hand's palm, under the front of the gun where it supports it.") : LOCTEXT("GripLHelpOptional", "Optional for a pistol. Where the other hand supports the gun. Leave it out for a one-handed gun."),
		EMantleBuilderPointShape::Hand, bTwoHanded, true));
	Roles.Add(MakeRole(TEXT("Muzzle"), LOCTEXT("Muzzle", "Muzzle"),
		LOCTEXT("MuzzleHelp", "Where bullets and the muzzle flash come out. The arrow points the way the gun fires."),
		EMantleBuilderPointShape::Arrow, true, true));
	Roles.Add(MakeRole(TEXT("Sight"), LOCTEXT("Sight", "Sight"),
		LOCTEXT("SightHelp", "Put it on top of the sight you look over, usually the rear one nearest your eye (on a scope, the middle of its back end). How far forward it sits does not matter; its height sets the line you look along when aiming."),
		EMantleBuilderPointShape::Arrow, true, true));
	Roles.Add(MakeRole(TEXT("Magazine"), LOCTEXT("Magazine", "Magazine"),
		LOCTEXT("MagazineHelp", "Optional. The part that comes out when reloading."),
		EMantleBuilderPointShape::Dot, false, false));
	Roles.Add(MakeRole(TEXT("ShellEject"), LOCTEXT("ShellEject", "Shell ejection"),
		LOCTEXT("ShellEjectHelp", "Optional. Where empty cases fly out."),
		EMantleBuilderPointShape::Arrow, false, true));
	return Roles;
}

TArray<FMantleBuilderPoint> FMantleWeaponBuilderKind::GuessPoints(const UObject* Mesh) const
{
	TArray<FVector3f> Positions;
	TArray<int32> Triangles;
	if (!Mesh || !MantleWeaponBuilderKindPrivate::ReadMesh(Mesh, Positions, Triangles))
	{
		LastFit = FMantleWeaponFitResult();
		LastFit.Notes.Add(TEXT("The mesh has no shape to read, so there is nothing to guess from. Place the points by hand."));
		return {};
	}
	return GuessPointsFromPositions(Positions, Triangles);
}

TArray<FMantleBuilderPoint> FMantleWeaponBuilderKind::GuessPointsFromPositions(const TArray<FVector3f>& Positions, const TArray<int32>& Triangles) const
{
	FMantleWeaponFitInput In;
	In.Positions = Positions;
	In.Triangles = Triangles;
	In.Hold = Hold;
	In.GripRule = GripRule;
	In.LeftHandBelowCentreCm = FMantleWeaponFit::LeftHandBelowHandguardCm;
	// A blade's Flip swaps its handle and tip: the fit puts the grip at the other end, and only what is left of the turn
	// (a roll) is applied below. Turning the mesh about a grip that sat on the wrong end would leave the hand there.
	FQuat Turn = Flip;
	if ((Hold == EMantleWeaponFitHold::Melee1h || Hold == EMantleWeaponFitHold::Melee2h) && Flip.RotateVector(FVector::ForwardVector).X < 0.0)
	{
		In.bReverseLength = true;
		Turn = (Flip * FQuat(FVector::UpVector, UE_PI).Inverse()).GetNormalized();
	}
	LastFit = FMantleWeaponFit::Fit(In);
	if (!Turn.Equals(FQuat::Identity, 1.0e-6))
	{
		// The creator's correction turns the mesh about the grip, in the weapon frame. The fit's weapon-frame points stay
		// where they are, so after the turn they land on the other end of the mesh in the mesh's own space.
		LastFit.MeshToWeapon = LastFit.MeshToWeapon * FTransform(Turn);
	}
	if (ScaleOverride > 0.f && LastFit.Scale > UE_SMALL_NUMBER)
	{
		// The fit's translation and weapon-frame points are all in scaled units, so they follow the ratio.
		const float Ratio = ScaleOverride / LastFit.Scale;
		LastFit.MeshToWeapon.SetTranslation(LastFit.MeshToWeapon.GetTranslation() * Ratio);
		LastFit.MeshToWeapon.SetScale3D(FVector(ScaleOverride));
		for (TPair<FName, FTransform>& Pair : LastFit.Points)
		{
			Pair.Value.SetTranslation(Pair.Value.GetTranslation() * Ratio);
		}
		LastFit.GripFromRearCm *= Ratio;
		LastFit.Scale = ScaleOverride;
		LastFit.ScaleReason = TEXT("You set the size yourself.");
	}

	// The surface, sampled about 1 cm apart in the weapon frame's units, for the hand and sight checks.
	{
		const double Scale = FMath::Max(double(LastFit.MeshToWeapon.GetScale3D().X), 1.0e-6);
		MantleWeaponBuilderMath::BuildSurfaceSamples(Positions, Triangles, MantleWeaponBuilderKindPrivate::SampleSpacingCm / Scale, MantleWeaponBuilderKindPrivate::MaxSamples, Samples);
		Cache.bValid = false;
	}

	const FTransform WeaponToMesh = LastFit.MeshToWeapon.Inverse();
	TArray<FMantleBuilderPoint> Out;
	for (const FMantleBuilderPointRole& Role : GetRoles())
	{
		const FTransform* InWeapon = LastFit.Points.Find(Role.Role);
		if (!InWeapon)
		{
			continue;
		}
		// Location and rotation only: the fit's uniform scale is not part of a point.
		const FTransform InMesh(WeaponToMesh.GetRotation() * InWeapon->GetRotation(), WeaponToMesh.TransformPosition(InWeapon->GetLocation()));
		FMantleBuilderPoint& Point = Out.AddDefaulted_GetRef();
		Point.Role = Role.Role;
		Point.Parent = NAME_None;
		Point.Transform = InMesh;
		Point.Guess = InMesh;
		Point.Source = EMantleBuilderPointSource::Guess;
	}
	return Out;
}

namespace
{
	// A closed fist round a blade's handle, in the weapon frame: the handle runs along X, 2 cm on the +Z side of the palm's centre.
	// Side is +1 for the right hand and -1 for the left, which is the right hand mirrored across Y.
	TArray<FMantleGhostCapsule> MakeFistCapsules(const FVector& Palm, double Side, bool bWarning)
	{
		using namespace MantleWeaponBuilderMath;
		TArray<FMantleGhostCapsule> Out;
		auto Add = [&](const FString& Part, const FVector& Start, const FVector& End, double Radius)
		{
			FMantleGhostCapsule& Capsule = Out.AddDefaulted_GetRef();
			Capsule.Part = FName(*Part);
			Capsule.Start = Start;
			Capsule.End = End;
			Capsule.RadiusCm = float(Radius);
			Capsule.bWarning = bWarning;
		};
		const double HandleOffsetCm = 2.0;
		const double Pr = HandPalmRadiusCm;
		const double PalmSeg = 4.0 - Pr;
		const double WrapCm = HandleOffsetCm + HandFingerRadiusCm; // the finger ring's radius round the handle's centre line
		const FVector Handle = Palm + FVector(0.0, 0.0, HandleOffsetCm);
		// A point on a ring of radius R round the handle, Degrees round from the palm's side: 0 is under the handle, 90 on the +Side flank,
		// 180 over the top, 270 on the other flank.
		auto Ring = [&](double X, double R, double Degrees)
		{
			const double Rad = FMath::DegreesToRadians(Degrees);
			return FVector(X, Handle.Y + Side * R * FMath::Sin(Rad), Handle.Z - R * FMath::Cos(Rad));
		};
		// The palm: a slab against the handle's -Z side, three capsules across it.
		for (int32 I = 0; I < 3; ++I)
		{
			const double X = Palm.X + (I - 1) * 2.67;
			Add(FString::Printf(TEXT("Palm_%d"), I), FVector(X, Palm.Y - Side * PalmSeg, Palm.Z - Pr), FVector(X, Palm.Y + Side * PalmSeg, Palm.Z - Pr), Pr);
		}
		// Four fingers 2 cm apart, index nearest the blade, each leaving the palm's flank, going over the top and coming back to the palm.
		const double Knuckle[4] = { 60.0, 140.0, 220.0, 300.0 };
		for (int32 F = 0; F < 4; ++F)
		{
			const double X = Palm.X + (1.5 - F) * 2.0;
			for (int32 S = 0; S < 3; ++S)
			{
				Add(FString::Printf(TEXT("Finger_%d_%s"), F, S == 0 ? TEXT("A") : S == 1 ? TEXT("B") : TEXT("C")),
					Ring(X, WrapCm, Knuckle[S]), Ring(X, WrapCm, Knuckle[S + 1]), HandFingerRadiusCm);
			}
		}
		// The thumb leaves the other flank and wraps over the top of the fingers.
		const double ThumbR = WrapCm + HandFingerRadiusCm;
		Add(TEXT("Thumb_A"), Ring(Palm.X, ThumbR, 285.0), Ring(Palm.X, ThumbR, 215.0), HandThumbRadiusCm);
		Add(TEXT("Thumb_B"), Ring(Palm.X, ThumbR, 215.0), Ring(Palm.X, ThumbR, 160.0), HandThumbRadiusCm);
		// The wrist and forearm leave the palm away from the handle and a little toward the pommel.
		const FVector Away = FVector(-0.25, 0.0, -1.0).GetSafeNormal();
		const FVector Wrist = Palm - FVector(0.0, 0.0, Pr) + Away * 4.0;
		Add(TEXT("Wrist"), Palm - FVector(0.0, 0.0, Pr), Wrist, Pr);
		Add(TEXT("Forearm"), Wrist, Wrist + Away * 16.0, 2.0);
		return Out;
	}
}

// The real-size body holding the weapon, solid in the builder, so a creator sees the weapon's size and how it sits in
// the hands. The hands are part of it: each forearm ends on its hand's wrist.
namespace MantleWeaponBuilderKindHeld
{
	using namespace MantleWeaponBuilderKindPrivate;

	/** One drawn hand, in the weapon frame, and the wrist its forearm ends on. */
	struct FHand
	{
		bool bSet = false;
		TArray<FMantleGhostCapsule> Capsules;
		FVector Wrist = FVector::ZeroVector;
	};

	/** A closed fist round a handle running along HandleAxis, its palm on the PalmSide of the handle (both in the weapon frame):
	 *  the blade's fist turned onto that handle. Its own forearm stub is left out; the arm is solved to its wrist instead. */
	FHand MakeFist(const FVector& Palm, const FVector& HandleAxis, const FVector& PalmSide, double Side, bool bWarning)
	{
		// The fist is built with its handle along X and its palm on the handle's -Z side.
		const FVector X = HandleAxis.GetSafeNormal();
		const FVector Z = (-PalmSide + X * FVector::DotProduct(PalmSide, X)).GetSafeNormal();
		const FQuat Turn = FRotationMatrix::MakeFromXZ(X, Z).ToQuat();
		FHand Hand;
		Hand.bSet = true;
		for (FMantleGhostCapsule Capsule : MakeFistCapsules(FVector::ZeroVector, Side, bWarning))
		{
			if (Capsule.Part == FName(TEXT("Forearm")))
			{
				Hand.Wrist = Palm + Turn.RotateVector(Capsule.Start);
				continue;
			}
			Capsule.Start = Palm + Turn.RotateVector(Capsule.Start);
			Capsule.End = Palm + Turn.RotateVector(Capsule.End);
			Hand.Capsules.Add(Capsule);
		}
		return Hand;
	}

	/** A gun hand (the grip hand, or the support hand under the gun or cupping it) with its wrist behind the palm, and the back of
	 *  the hand from the wrist through the palm, so its middle is the hand point and the forearm runs into it. */
	FHand MakeGunHand(const FVector& Point, MantleWeaponBuilderMath::EHandStyle Style, double FarReach, bool bWarning, const TCHAR* Side)
	{
		using namespace MantleWeaponBuilderMath;
		FHand Hand;
		Hand.bSet = true;
		Hand.Capsules = MakeHandCapsules(Point, Style, FarReach, bWarning);
		// The palm is three capsules 2.67 cm apart; the wrist is 2 cm past its back edge: behind the grip hand (its fingers point
		// along the bore), on the far side from the fingers for a hand under the gun.
		const double Back = 2.67 + HandPalmRadiusCm + 2.0;
		const FQuat Roll = Style == EHandStyle::SupportCup ? FQuat(FVector::ForwardVector, FMath::DegreesToRadians(-25.0)) : FQuat::Identity;
		Hand.Wrist = Point + (Style == EHandStyle::GripRight ? FVector(-Back, 0.0, -1.0) : Roll.RotateVector(FVector(-1.0, -Back, -0.5)));
		Hand.Capsules.Add(MakeCapsule(TEXT("Hand"), Side, Hand.Wrist, Point * 2.0 - Hand.Wrist, float(HandPalmRadiusCm), bWarning));
		return Hand;
	}

	/** The body's forward, right and up in the weapon frame (a proper turn: Forward x Right = Up). */
	struct FFrame
	{
		FVector Forward;
		FVector Right;
		FVector Up;
	};

	/** How the body stands to a weapon of this hold, in the weapon frame. A gun's and a shield's body faces along the weapon
	 *  (+X), upright (+Z). An archer stands side-on, the target (+X) on their left, facing the string's side (+Y). Blades,
	 *  spears, staffs, throwables and fists are held upright: the weapon's +X is up, its +Z (each fist's palm, on the
	 *  handle's -Z side, faces the body) forward. */
	FFrame BodyFrame(EMantleWeaponFitHold Hold)
	{
		if (Hold == EMantleWeaponFitHold::Bow)
		{
			return { FVector(0.0, 1.0, 0.0), FVector(-1.0, 0.0, 0.0), FVector(0.0, 0.0, 1.0) };
		}
		if (Hold < EMantleWeaponFitHold::Melee1h || Hold == EMantleWeaponFitHold::Shield || Hold == EMantleWeaponFitHold::Consumable)
		{
			return { FVector(1.0, 0.0, 0.0), FVector(0.0, 1.0, 0.0), FVector(0.0, 0.0, 1.0) };
		}
		return { FVector(0.0, 0.0, 1.0), FVector(0.0, -1.0, 0.0), FVector(1.0, 0.0, 0.0) };
	}

	/** Adds the standing ghost, turned by Frame and placed so its body-space point AnchorBody (feet at the origin, X forward, Y
	 *  right, Z up) lands on AnchorWeapon, with each arm that has a hand solved to that hand's wrist. The elbows bend outward and
	 *  down; RightElbowDrop below 1 keeps the right elbow up (an archer's draw arm, its elbow level with the shoulder). */
	void AddBody(TArray<FMantleGhostCapsule>& Out, const FFrame& Frame, const FVector& AnchorBody, const FVector& AnchorWeapon, const FHand& Right, const FHand& Left,
		double RightElbowDrop = 1.0)
	{
		auto ToWeapon = [&](const FVector& Body)
		{
			const FVector D = Body - AnchorBody;
			return AnchorWeapon + Frame.Forward * D.X + Frame.Right * D.Y + Frame.Up * D.Z;
		};
		const double ShoulderY = FMantleBodyMeasurements::ShoulderWidthCm() * 0.5;
		const double ShoulderZ = FMantleBodyMeasurements::ShoulderHeightCm();
		FArm ArmR, ArmL;
		if (Right.bSet)
		{
			SolveArm(ArmR, TEXT("R"), ToWeapon(FVector(0.0, ShoulderY, ShoulderZ)), Right.Wrist, Frame.Right, &Right.Wrist, -Frame.Up * RightElbowDrop);
		}
		if (Left.bSet)
		{
			SolveArm(ArmL, TEXT("L"), ToWeapon(FVector(0.0, -ShoulderY, ShoulderZ)), Left.Wrist, -Frame.Right, &Left.Wrist, -Frame.Up);
		}
		for (const FMantleGhostCapsule& Capsule : FMantleBodyMeasurements::StandingGhost())
		{
			const FString Name = Capsule.Part.ToString();
			if (ShouldSkipPosedArmCapsule(Name, ArmR.bPosed, ArmL.bPosed))
			{
				continue;
			}
			FMantleGhostCapsule Moved = Capsule;
			Moved.Start = ToWeapon(Capsule.Start);
			Moved.End = ToWeapon(Capsule.End);
			Out.Add(Moved);
		}
		Out.Append(ArmR.Capsules);
		Out.Append(ArmL.Capsules);
	}

	/** Moves the body (AnchorBody, in body space) toward the hands until each shoulder is within reach of its hand's wrist, with
	 *  a little slack so no elbow is locked; hands too far apart for one body stay out of reach, and their arm is red. */
	FVector ReachBody(const FFrame& Frame, FVector AnchorBody, const FVector& AnchorWeapon, const FHand& Right, const FHand& Left)
	{
		const double ShoulderY = FMantleBodyMeasurements::ShoulderWidthCm() * 0.5;
		const double ShoulderZ = FMantleBodyMeasurements::ShoulderHeightCm();
		const double Reach = (FMantleBodyMeasurements::UpperArmCm() + FMantleBodyMeasurements::ForearmCm()) * 0.95;
		for (int32 Step = 0; Step < 16; ++Step)
		{
			FVector Move = FVector::ZeroVector;
			for (const TPair<const FHand*, double>& Arm : { TPair<const FHand*, double>(&Right, 1.0), TPair<const FHand*, double>(&Left, -1.0) })
			{
				if (!Arm.Key->bSet)
				{
					continue;
				}
				// The wrist in body space, from the shoulder.
				const FVector W = Arm.Key->Wrist - AnchorWeapon;
				const FVector WristBody = AnchorBody + FVector(FVector::DotProduct(W, Frame.Forward), FVector::DotProduct(W, Frame.Right), FVector::DotProduct(W, Frame.Up));
				const FVector ToWrist = WristBody - FVector(0.0, Arm.Value * ShoulderY, ShoulderZ);
				const double Over = ToWrist.Size() - Reach;
				if (Over > 0.0)
				{
					Move += ToWrist.GetSafeNormal() * Over;
				}
			}
			if (Move.IsNearlyZero(0.05))
			{
				break;
			}
			// The body moves toward the wrists: the anchor's place on the body moves the other way.
			AnchorBody -= Move;
		}
		return AnchorBody;
	}

	/**
	 * A held weapon's hands and body, in the weapon frame (its length or aim along +X, its up +Z).
	 * Blades, spears, staffs, throwables and fists stand upright, as the game holds a greatsword at rest: the weapon's +X is the
	 * body's up, each fist's palm is on the body's side of the handle, and the hands are in front of the body (one hand at its right
	 * side; a two-handed grip in front of the middle; a shaft at the right front). The bow is drawn: the body side-on with the bow
	 * arm out toward the target at shoulder height and the draw hand on the nock. The shield is on the left forearm in front of
	 * the chest, its face forward.
	 */
	void AddHeldPose(EMantleWeaponFitHold Hold, const TOptional<FVector>& HandR, const TOptional<FVector>& HandL, const TOptional<FVector>& Nock,
		bool bLeftInside, TArray<FMantleGhostCapsule>& Out)
	{
		const double ShoulderY = FMantleBodyMeasurements::ShoulderWidthCm() * 0.5;
		const double ShoulderZ = FMantleBodyMeasurements::ShoulderHeightCm();
		const double ArmCm = FMantleBodyMeasurements::UpperArmCm() + FMantleBodyMeasurements::ForearmCm();
		FHand Right, Left;
		if (Hold == EMantleWeaponFitHold::Bow)
		{
			if (!HandL.IsSet())
			{
				return;
			}
			// The riser and the string run up the bow (+Z); each palm is on the archer's side of what it holds.
			const FFrame Frame = BodyFrame(Hold);
			Left = MakeFist(HandL.GetValue(), FVector::UpVector, FVector(-1.0, 0.0, 0.0), -1.0, bLeftInside);
			if (Nock.IsSet())
			{
				Right = MakeFist(Nock.GetValue(), FVector::UpVector, FVector(-1.0, 0.0, 0.0), 1.0, false);
			}
			// The bow arm all but straight out to the left at shoulder height; the palm is about 5.5 cm past the wrist.
			const FVector Palm(4.0, -(ShoulderY + ArmCm * 0.97 + 5.5), ShoulderZ - 2.0);
			Out.Append(Left.Capsules);
			Out.Append(Right.Capsules);
			AddBody(Out, Frame, Palm, HandL.GetValue(), Right, Left, 0.15);
			return;
		}
		// A shield and a consumable are each one item in the left hand (the bow's clips use the right, but the ghost only needs a hand there).
		if (Hold == EMantleWeaponFitHold::Shield || Hold == EMantleWeaponFitHold::Consumable)
		{
			if (!HandL.IsSet())
			{
				return;
			}
			const FFrame Frame = BodyFrame(Hold);
			Left = MakeGunHand(HandL.GetValue(), MantleWeaponBuilderMath::EHandStyle::SupportCup, 3.0, bLeftInside, TEXT("L"));
			Out.Append(Left.Capsules);
			AddBody(Out, Frame, ReachBody(Frame, FVector(30.0, -ShoulderY * 0.5, ShoulderZ - 22.0), HandL.GetValue(), Right, Left), HandL.GetValue(), Right, Left);
			return;
		}
		if (!HandR.IsSet() && !HandL.IsSet())
		{
			return;
		}
		const FFrame Frame = BodyFrame(Hold);
		if (HandR.IsSet())
		{
			Right = MakeFist(HandR.GetValue(), FVector::ForwardVector, FVector(0.0, 0.0, -1.0), 1.0, false);
		}
		if (HandL.IsSet())
		{
			Left = MakeFist(HandL.GetValue(), FVector::ForwardVector, FVector(0.0, 0.0, -1.0), -1.0, bLeftInside);
		}
		Out.Append(Right.Capsules);
		Out.Append(Left.Capsules);
		const bool bShaft = Hold == EMantleWeaponFitHold::Spear || Hold == EMantleWeaponFitHold::Staff;
		if (HandR.IsSet() && HandL.IsSet())
		{
			// Two hands: their middle in front of the body; a shaft a little to the right and higher, so both arms reach along it.
			const FVector Body = bShaft ? FVector(30.0, ShoulderY * 0.3, ShoulderZ - 22.0) : FVector(34.0, 0.0, ShoulderZ - 36.0);
			const FVector Middle = (HandR.GetValue() + HandL.GetValue()) * 0.5;
			AddBody(Out, Frame, ReachBody(Frame, Body, Middle, Right, Left), Middle, Right, Left);
			return;
		}
		// One hand, in front of its own side, the forearm level.
		const double Sign = HandR.IsSet() ? 1.0 : -1.0;
		const FVector Hand = HandR.IsSet() ? HandR.GetValue() : HandL.GetValue();
		AddBody(Out, Frame, ReachBody(Frame, FVector(32.0, Sign * ShoulderY * 0.75, ShoulderZ - 30.0), Hand, Right, Left), Hand, Right, Left);
	}
}

TArray<FMantleGhostCapsule> FMantleWeaponBuilderKind::GhostPose(const TArray<FMantleBuilderPoint>& Points) const
{
	using namespace MantleWeaponBuilderKindPrivate;
	using namespace MantleWeaponBuilderKindHeld;
	const FTransform& MeshToWeapon = LastFit.MeshToWeapon;
	const FContactCache& Contact = Contacts(Points);
	TArray<FMantleGhostCapsule> InWeapon;
	if (Hold >= EMantleWeaponFitHold::Melee1h)
	{
		TOptional<FVector> NockAt;
		if (const FMantleBuilderPoint* Point = MantleBuilderPoints::Find(Points, TEXT("Nock")))
		{
			NockAt = MeshToWeapon.TransformPosition(Point->Transform.GetLocation());
		}
		AddHeldPose(Hold, Contact.HandR, Contact.HandL, NockAt, Contact.bLeftInside, InWeapon);
	}
	else
	{
		// The hands first, then the body, each arm ending on its hand's wrist.
		FHand Right, Left;
		if (Contact.HandR.IsSet())
		{
			Right = MakeGunHand(Contact.HandR.GetValue(), MantleWeaponBuilderMath::EHandStyle::GripRight, Contact.FarReachR, Contact.bRightInside, TEXT("R"));
		}
		if (Contact.HandL.IsSet())
		{
			Left = MakeGunHand(Contact.HandL.GetValue(), IsTwoHanded(Hold) ? MantleWeaponBuilderMath::EHandStyle::SupportUnder : MantleWeaponBuilderMath::EHandStyle::SupportCup,
				Contact.FarReachL, Contact.bLeftInside, TEXT("L"));
		}
		const FMantleBuilderPoint* PointR = MantleBuilderPoints::Find(Points, GripR);
		const double HandRX = PointR ? MeshToWeapon.TransformPosition(PointR->Transform.GetLocation()).X : 0.0;
		// A shouldered gun is always shown as the game holds it when aiming: the eye where the game puts it, the butt in the shoulder.
		const TOptional<FVector> Aim = (bShowAiming || IsTwoHanded(Hold)) ? AimEye(Points) : TOptional<FVector>();
		const TOptional<double> Butt = ButtCentreZ(RearX(LastFit, HandRX));
		const FPose Pose = BuildPose(Points, MeshToWeapon, IsTwoHanded(Hold), RearX(LastFit, HandRX), Aim.IsSet() ? &Aim.GetValue() : nullptr,
			Butt.IsSet() ? &Butt.GetValue() : nullptr, Right.bSet ? &Right.Wrist : nullptr, Left.bSet ? &Left.Wrist : nullptr);
		InWeapon.Append(Right.Capsules);
		InWeapon.Append(Left.Capsules);
		InWeapon.Append(Pose.Capsules);
	}

	// Back into the mesh's own unscaled space, the space the points are in.
	const double Scale = FMath::Max(MeshToWeapon.GetScale3D().X, 1.0e-6);
	TArray<FMantleGhostCapsule> Out;
	Out.Reserve(InWeapon.Num());
	for (const FMantleGhostCapsule& Capsule : InWeapon)
	{
		FMantleGhostCapsule Mapped = Capsule;
		Mapped.Start = MeshToWeapon.InverseTransformPosition(Capsule.Start);
		Mapped.End = MeshToWeapon.InverseTransformPosition(Capsule.End);
		Mapped.RadiusCm = Capsule.RadiusCm / Scale;
		Out.Add(Mapped);
	}
	return Out;
}

FQuat FMantleWeaponBuilderKind::ViewTurn() const
{
	// The view is the body's own frame (forward +X, right +Y, up +Z): mesh space to the weapon frame, then the weapon frame to
	// the body's (the inverse of the turn whose rows are the body's axes in the weapon frame).
	const MantleWeaponBuilderKindHeld::FFrame Frame = MantleWeaponBuilderKindHeld::BodyFrame(Hold);
	const FQuat BodyToWeapon(FMatrix(Frame.Forward, Frame.Right, Frame.Up, FVector::ZeroVector));
	return BodyToWeapon.Inverse() * LastFit.MeshToWeapon.GetRotation();
}

TArray<FText> FMantleWeaponBuilderKind::Check(const TArray<FMantleBuilderPoint>& Points) const
{
	using namespace MantleWeaponBuilderKindPrivate;
	TArray<FText> Warnings;
	if (Hold >= EMantleWeaponFitHold::Melee1h)
	{
		for (const auto& Role : GetRoles())
		{
			if (Role.bRequired && !MantleBuilderPoints::Find(Points, Role.Role))
				Warnings.Add(FText::Format(LOCTEXT("NeedHoldPoint", "Place the {0} point."), Role.Label));
		}
		return Warnings;
	}

	const FMantleBuilderPoint* PointR = MantleBuilderPoints::Find(Points, GripR);
	const FMantleBuilderPoint* PointL = MantleBuilderPoints::Find(Points, GripL);
	const FMantleBuilderPoint* PointMuzzle = MantleBuilderPoints::Find(Points, Muzzle);
	const FMantleBuilderPoint* PointSight = MantleBuilderPoints::Find(Points, Sight);

	if (!PointR)
	{
		Warnings.Add(LOCTEXT("NeedRight", "Place the right hand."));
	}
	if (!PointL && IsTwoHanded(Hold))
	{
		Warnings.Add(LOCTEXT("NeedLeft", "Place the left hand."));
	}
	if (!PointMuzzle)
	{
		Warnings.Add(LOCTEXT("NeedMuzzle", "Place the muzzle."));
	}
	if (!PointSight)
	{
		Warnings.Add(LOCTEXT("NeedSight", "Place the sight."));
	}

	const double HandRX = PointR ? LastFit.MeshToWeapon.TransformPosition(PointR->Transform.GetLocation()).X : 0.0;
	const TOptional<FVector> Aim = (bShowAiming || IsTwoHanded(Hold)) ? AimEye(Points) : TOptional<FVector>();
	const TOptional<double> Butt = ButtCentreZ(RearX(LastFit, HandRX));
	const FPose Pose = BuildPose(Points, LastFit.MeshToWeapon, IsTwoHanded(Hold), RearX(LastFit, HandRX), Aim.IsSet() ? &Aim.GetValue() : nullptr,
		Butt.IsSet() ? &Butt.GetValue() : nullptr);
	if (Pose.Right.bPosed && !Pose.Right.bReached)
	{
		Warnings.Add(LOCTEXT("RightOutOfReach", "The right hand can't reach that spot while aiming. Move the grip closer to the sight."));
	}
	if (Pose.Left.bPosed && !Pose.Left.bReached)
	{
		Warnings.Add(LOCTEXT("LeftOutOfReach", "The left hand can't reach that spot. Move it closer to the right hand."));
	}

	const FContactCache& Contact = Contacts(Points);
	if (Contact.bRightInside)
	{
		Warnings.Add(LOCTEXT("RightInside", "The right hand is inside the gun. Drag it back or down onto the grip."));
	}
	if (Contact.bLeftInside)
	{
		Warnings.Add(IsTwoHanded(Hold)
			? LOCTEXT("LeftInside", "The left hand is inside the gun. Drag it down until it sits under the front of the gun.")
			: LOCTEXT("LeftInsidePistol", "The left hand is inside the gun. Drag it out to the side of the grip."));
	}
	if (Contact.bSightInside)
	{
		Warnings.Add(LOCTEXT("SightInside", "The sight point is inside the gun. Move it up onto the top of the sight."));
	}

	if (PointR && PointMuzzle)
	{
		const FVector Fire = PointMuzzle->Transform.GetRotation().GetForwardVector();
		if (FVector::DotProduct(PointMuzzle->Transform.GetLocation() - PointR->Transform.GetLocation(), Fire) < 0.0)
		{
			Warnings.Add(LOCTEXT("MuzzleBehind", "The muzzle is behind the grip. Use Flip if the gun faces backwards."));
		}
	}
	if (PointMuzzle && PointSight)
	{
		const FVector Up = PointMuzzle->Transform.GetRotation().GetUpVector();
		if (FVector::DotProduct(PointSight->Transform.GetLocation() - PointMuzzle->Transform.GetLocation(), Up) < -0.01)
		{
			Warnings.Add(LOCTEXT("SightBelow", "The sight is below the barrel; aiming will look off."));
		}
	}

	if (LastFit.bOutsideTypicalRange && !LastFit.Suggestion.IsEmpty())
	{
		Warnings.Add(FText::FromString(LastFit.Suggestion));
	}
	return Warnings;
}

// ---- contact checks and the overlay --------------------------------------------------------------------------------------

MantleWeaponBuilderMath::FPalmBox MantleWeaponBuilderMath::MakePalmBox(const FVector& GripWeapon, bool bWrapGrip)
{
	FPalmBox Box;
	Box.Center = GripWeapon;
	Box.Rotation = FQuat::Identity;
	Box.HalfExtent = bWrapGrip
		? FVector(PalmWidthCm * 0.5, PalmThicknessCm * 0.5, PalmLengthCm * 0.5)
		: FVector(PalmLengthCm * 0.5, PalmWidthCm * 0.5, PalmThicknessCm * 0.5);
	return Box;
}

int32 MantleWeaponBuilderMath::CountSamplesInsidePalm(const TArray<FVector3f>& Samples, const FTransform& SamplesToWeapon, const FPalmBox& Box, double ShrinkCm)
{
	const FVector Limit = Box.HalfExtent - FVector(ShrinkCm);
	if (Limit.X <= 0.0 || Limit.Y <= 0.0 || Limit.Z <= 0.0)
	{
		return 0;
	}
	int32 Count = 0;
	for (const FVector3f& Sample : Samples)
	{
		const FVector Local = Box.Rotation.UnrotateVector(SamplesToWeapon.TransformPosition(FVector(Sample)) - Box.Center);
		if (FMath::Abs(Local.X) < Limit.X && FMath::Abs(Local.Y) < Limit.Y && FMath::Abs(Local.Z) < Limit.Z)
		{
			++Count;
		}
	}
	return Count;
}

TArray<FMantleGhostCapsule> MantleWeaponBuilderMath::MakeHandCapsules(const FVector& HandPoint, EHandStyle Style, double FarReachCm, bool bWarning)
{
	TArray<FMantleGhostCapsule> Out;
	auto Add = [&](const FString& Part, const FVector& Start, const FVector& End, double Radius)
	{
		FMantleGhostCapsule& Capsule = Out.AddDefaulted_GetRef();
		Capsule.Part = FName(*Part);
		Capsule.Start = Start;
		Capsule.End = End;
		Capsule.RadiusCm = float(Radius);
		Capsule.bWarning = bWarning;
	};
	// A two-segment digit from Base: the first segment along Dir1, the second bent so the tip is Target from Base along Across
	// (clamped to what the segment can reach), the rest of its length going along Along.
	auto Digit = [&](const FString& Part, const FVector& Base, const FVector& Dir1, const FVector& Across, const FVector& Along, double Target, double Radius)
	{
		const FVector Joint = Base + Dir1.GetSafeNormal() * HandSegment1Cm;
		const double Needed = Target - FVector::DotProduct(Joint - Base, Across);
		const double C = FMath::Clamp(Needed, -HandSegment2Cm, HandSegment2Cm);
		const FVector Tip = Joint + Across * C + Along * FMath::Sqrt(FMath::Max(0.0, FMath::Square(HandSegment2Cm) - C * C));
		Add(Part + TEXT("_A"), Base, Joint, Radius);
		Add(Part + TEXT("_B"), Joint, Tip, Radius);
	};
	const double Far = FarReachCm + 0.8;
	const double Pr = HandPalmRadiusCm;
	const double PalmSeg = 4.0 - Pr; // half a palm capsule's segment: the slab is about 8 cm each way
	// Fingers sit 2.2 cm apart along the palm's long side; index first.
	const double Spread[4] = { 3.3, 1.1, -1.1, -3.3 };
	const double Cos55 = FMath::Cos(FMath::DegreesToRadians(55.0));
	const double Sin55 = FMath::Sin(FMath::DegreesToRadians(55.0));
	const double Cos70 = FMath::Cos(FMath::DegreesToRadians(70.0));
	const double Sin70 = FMath::Sin(FMath::DegreesToRadians(70.0));

	if (Style == EHandStyle::GripRight)
	{
		// Palm slab in the X-Z plane, three capsules along Z, facing -Y.
		for (int32 I = 0; I < 3; ++I)
		{
			const double X = (I - 1) * 2.67;
			Add(FString::Printf(TEXT("Palm_%d"), I), HandPoint + FVector(X, 0.0, -PalmSeg), HandPoint + FVector(X, 0.0, PalmSeg), Pr);
		}
		// Index along the bore from the palm's front edge; the other three wrap the front to the far (left) side.
		const FVector Front = HandPoint + FVector(4.0, 0.0, 0.0);
		const FVector IndexBase = Front + FVector(0.0, 0.0, Spread[0]);
		Add(TEXT("Finger_0_A"), IndexBase, IndexBase + FVector(HandSegment1Cm, 0.0, 0.0), HandFingerRadiusCm);
		Add(TEXT("Finger_0_B"), IndexBase + FVector(HandSegment1Cm, 0.0, 0.0), IndexBase + FVector(HandSegment1Cm + HandSegment2Cm, 0.0, 0.0), HandFingerRadiusCm);
		for (int32 I = 1; I < 4; ++I)
		{
			Digit(FString::Printf(TEXT("Finger_%d"), I), Front + FVector(0.0, 0.0, Spread[I]), FVector(Cos55, -Sin55, 0.0), FVector(0.0, -1.0, 0.0), FVector(-1.0, 0.0, 0.0), Far, HandFingerRadiusCm);
		}
		// The thumb leaves the palm's back edge and wraps round the back to the left side.
		Digit(TEXT("Thumb"), HandPoint + FVector(-4.0, 0.0, Spread[0]), FVector(-Cos55, -Sin55, 0.0), FVector(0.0, -1.0, 0.0), FVector(1.0, 0.0, 0.0), Far, HandThumbRadiusCm);
		return Out;
	}

	// Under the gun: palm slab in the X-Y plane (three capsules along X), fingers up the +Y side, thumb up the -Y side.
	// The cupping hand is the same rolled 25 degrees about the bore so its palm faces up and to the right.
	const FQuat Roll = Style == EHandStyle::SupportCup ? FQuat(FVector::ForwardVector, FMath::DegreesToRadians(-25.0)) : FQuat::Identity;
	auto Local = [&](const FVector& Offset) { return HandPoint + Roll.RotateVector(Offset); };
	for (int32 I = 0; I < 3; ++I)
	{
		const double Y = (I - 1) * 2.67;
		Add(FString::Printf(TEXT("Palm_%d"), I), Local(FVector(-PalmSeg, Y, 0.0)), Local(FVector(PalmSeg, Y, 0.0)), Pr);
	}
	for (int32 I = 0; I < 4; ++I)
	{
		Digit(FString::Printf(TEXT("Finger_%d"), I), Local(FVector(Spread[I], 4.0, 0.0)), Roll.RotateVector(FVector(0.0, Cos70, Sin70)),
			Roll.RotateVector(FVector(0.0, 1.0, 0.0)), Roll.RotateVector(FVector(0.0, 0.0, 1.0)), Far - 4.0, HandFingerRadiusCm);
	}
	Digit(TEXT("Thumb"), Local(FVector(4.0, -4.0, 0.0)), Roll.RotateVector(FVector(0.0, -Cos70, Sin70)),
		Roll.RotateVector(FVector(0.0, -1.0, 0.0)), Roll.RotateVector(FVector(0.0, 0.0, 1.0)), Far - 4.0, HandThumbRadiusCm);
	return Out;
}

bool MantleWeaponBuilderMath::IsLeftPalmInsideGun(const TArray<FVector3f>& Samples, const FTransform& SamplesToWeapon, const FVector& GripL)
{
	// The gun cutting through the palm's box only: a palm wrapped round a vertical foregrip has the grip at its centre, which must
	// not be flagged, so a palm sunk wholly inside a thick handguard is not caught.
	return CountSamplesInsidePalm(Samples, SamplesToWeapon, MakePalmBox(GripL, false)) > LeftPalmInsideLimit;
}

bool MantleWeaponBuilderMath::IsSightInsideGun(const TArray<FVector3f>& Samples, const FTransform& SamplesToWeapon, const FVector& SightWeapon, double& OutTopZ)
{
	bool bAny = false;
	double Top = SightWeapon.Z;
	const double RadiusSq = FMath::Square(SightColumnRadiusCm);
	for (const FVector3f& Sample : Samples)
	{
		const FVector P = SamplesToWeapon.TransformPosition(FVector(Sample));
		if (FMath::Square(P.X - SightWeapon.X) + FMath::Square(P.Y - SightWeapon.Y) <= RadiusSq)
		{
			Top = bAny ? FMath::Max(Top, P.Z) : P.Z;
			bAny = true;
		}
	}
	OutTopZ = Top;
	return bAny && SightWeapon.Z < Top - SightSinkToleranceCm;
}

FVector MantleWeaponBuilderMath::AimEyeWeapon(double RearX, const FVector& SightWeapon, bool bTwoHanded)
{
	const double FromRear = bTwoHanded ? FMantleWeaponBuilderExport::LongGunEyeFromRearCm : FMantleWeaponBuilderExport::PistolEyeFromRearCm;
	return FVector(RearX + FromRear, SightWeapon.Y, SightWeapon.Z);
}

void MantleWeaponBuilderMath::BuildSurfaceSamples(const TArray<FVector3f>& Positions, const TArray<int32>& Triangles, double SpacingMeshUnits, int32 MaxSamples, TArray<FVector3f>& OutSamples)
{
	OutSamples.Reset();
	MaxSamples = FMath::Max(MaxSamples, 1);
	if (Positions.Num() == 0)
	{
		return;
	}
	if (Triangles.Num() < 3)
	{
		const int32 Stride = FMath::Max(1, FMath::CeilToInt(double(Positions.Num()) / MaxSamples));
		for (int32 I = 0; I < Positions.Num(); I += Stride)
		{
			OutSamples.Add(Positions[I]);
		}
		return;
	}
	double Spacing = FMath::Max(SpacingMeshUnits, 1.0e-4);
	for (int32 Pass = 0; Pass < 10; ++Pass)
	{
		OutSamples.Reset();
		bool bFull = false;
		for (int32 I = 0; I + 2 < Triangles.Num() && !bFull; I += 3)
		{
			if (!Positions.IsValidIndex(Triangles[I]) || !Positions.IsValidIndex(Triangles[I + 1]) || !Positions.IsValidIndex(Triangles[I + 2]))
			{
				continue;
			}
			const FVector3f A = Positions[Triangles[I]];
			const FVector3f B = Positions[Triangles[I + 1]];
			const FVector3f C = Positions[Triangles[I + 2]];
			const double Longest = FMath::Max3(double(FVector3f::Dist(A, B)), double(FVector3f::Dist(B, C)), double(FVector3f::Dist(C, A)));
			const int32 N = FMath::Clamp(FMath::CeilToInt(Longest / Spacing), 1, 24);
			for (int32 U = 0; U <= N && !bFull; ++U)
			{
				for (int32 V = 0; V <= N - U; ++V)
				{
					OutSamples.Add(A + (B - A) * (float(U) / N) + (C - A) * (float(V) / N));
					if (OutSamples.Num() >= MaxSamples)
					{
						bFull = true;
						break;
					}
				}
			}
		}
		if (!bFull)
		{
			return;
		}
		Spacing *= 2.0;
	}
}

const FMantleWeaponBuilderKind::FContactCache& FMantleWeaponBuilderKind::Contacts(const TArray<FMantleBuilderPoint>& Points) const
{
	using namespace MantleWeaponBuilderKindPrivate;
	const FTransform& MeshToWeapon = LastFit.MeshToWeapon;
	auto ToWeapon = [&](FName Role) -> TOptional<FVector>
	{
		const FMantleBuilderPoint* Point = MantleBuilderPoints::Find(Points, Role);
		return Point ? TOptional<FVector>(MeshToWeapon.TransformPosition(Point->Transform.GetLocation())) : TOptional<FVector>();
	};
	auto Same = [](const TOptional<FVector>& A, const TOptional<FVector>& B)
	{
		return A.IsSet() == B.IsSet() && (!A.IsSet() || A.GetValue() == B.GetValue());
	};
	const TOptional<FVector> HandR = ToWeapon(GripR);
	const TOptional<FVector> HandL = ToWeapon(Hold == EMantleWeaponFitHold::Shield || Hold == EMantleWeaponFitHold::Consumable ? FName(TEXT("Grip")) : GripL);
	const TOptional<FVector> SightLoc = ToWeapon(Sight);
	if (Cache.bValid && Cache.Hold == Hold && Cache.MeshToWeapon.Equals(MeshToWeapon, 0.f) && Same(Cache.HandR, HandR) && Same(Cache.HandL, HandL) && Same(Cache.Sight, SightLoc))
	{
		return Cache;
	}
	Cache.bValid = true;
	Cache.MeshToWeapon = MeshToWeapon;
	Cache.Hold = Hold;
	Cache.HandR = HandR;
	Cache.HandL = HandL;
	Cache.Sight = SightLoc;
	// Only a two-handed left palm is checked (see IsLeftPalmInsideGun): the right hand and a pistol's support hand wrap a grip,
	// so they would read as inside.
	Cache.bRightInside = false;
	// A gun's only: a fist round a blade's or a shaft's handle always has the handle inside its palm.
	Cache.bLeftInside = HandL.IsSet() && IsTwoHanded(Hold) && Hold < EMantleWeaponFitHold::Melee1h
		&& MantleWeaponBuilderMath::IsLeftPalmInsideGun(Samples, MeshToWeapon, HandL.GetValue());
	{
		// How far across the gun each hand's far surface is: the samples near the hand, on the grip (right) or the handguard above the palm (left).
		auto FarReach = [&](const TOptional<FVector>& Hand, bool bUnder, double Fallback, double Sign)
		{
			if (!Hand.IsSet())
			{
				return Fallback;
			}
			const FVector H = Hand.GetValue();
			double Reach = -1.0;
			for (const FVector3f& Sample : Samples)
			{
				const FVector P = MeshToWeapon.TransformPosition(FVector(Sample));
				const double Dz = P.Z - H.Z;
				if (FMath::Abs(P.X - H.X) <= (bUnder ? 6.0 : 3.0) && (bUnder ? (Dz >= 0.0 && Dz <= 9.0) : FMath::Abs(Dz) <= 5.0))
				{
					Reach = FMath::Max(Reach, (P.Y - H.Y) * Sign);
				}
			}
			return Reach < 0.0 ? Fallback : FMath::Clamp(Reach, 0.5, 12.0);
		};
		Cache.FarReachR = FarReach(HandR, false, 2.0, -1.0);
		Cache.FarReachL = FarReach(HandL, IsTwoHanded(Hold), 3.0, 1.0);
	}
	double TopZ = 0.0;
	Cache.bSightInside = SightLoc.IsSet() && MantleWeaponBuilderMath::IsSightInsideGun(Samples, MeshToWeapon, SightLoc.GetValue(), TopZ);
	return Cache;
}

TOptional<double> FMantleWeaponBuilderKind::ButtCentreZ(double RearXCm) const
{
	// The middle of the gun's height over its last 4 cm (weapon frame): where the butt meets the shoulder.
	double Low = TNumericLimits<double>::Max();
	double High = TNumericLimits<double>::Lowest();
	for (const FVector3f& Sample : Samples)
	{
		const FVector P = LastFit.MeshToWeapon.TransformPosition(FVector(Sample));
		if (P.X >= RearXCm - 0.5 && P.X <= RearXCm + 4.0)
		{
			Low = FMath::Min(Low, P.Z);
			High = FMath::Max(High, P.Z);
		}
	}
	return Low <= High ? TOptional<double>(0.5 * (Low + High)) : TOptional<double>();
}

TOptional<FVector> FMantleWeaponBuilderKind::AimEye(const TArray<FMantleBuilderPoint>& Points) const
{
	using namespace MantleWeaponBuilderKindPrivate;
	const FMantleBuilderPoint* PointSight = MantleBuilderPoints::Find(Points, Sight);
	if (!PointSight)
	{
		return TOptional<FVector>();
	}
	const FMantleBuilderPoint* PointR = MantleBuilderPoints::Find(Points, GripR);
	const double HandRX = PointR ? LastFit.MeshToWeapon.TransformPosition(PointR->Transform.GetLocation()).X : 0.0;
	return MantleWeaponBuilderMath::AimEyeWeapon(RearX(LastFit, HandRX), LastFit.MeshToWeapon.TransformPosition(PointSight->Transform.GetLocation()), IsTwoHanded(Hold));
}

FMantleBuilderOverlay FMantleWeaponBuilderKind::Overlay(const TArray<FMantleBuilderPoint>& Points) const
{
	using namespace MantleWeaponBuilderKindPrivate;
	FMantleBuilderOverlay Out;
	const FTransform& MeshToWeapon = LastFit.MeshToWeapon;
	const double Scale = FMath::Max(double(MeshToWeapon.GetScale3D().X), 1.0e-6);
	const FContactCache& Contact = Contacts(Points);

	// The hands are part of the ghost (GhostPose), drawn solid with the body; the overlay is only the aiming line.
	if (bShowAiming && Contact.Sight.IsSet())
	{
		const TOptional<FVector> Eye = AimEye(Points);
		if (Eye.IsSet())
		{
			const FVector SightLoc = Contact.Sight.GetValue();
			const FVector Along = (SightLoc - Eye.GetValue()).GetSafeNormal();
			FMantleBuilderOverlayMarker& Marker = Out.Markers.AddDefaulted_GetRef();
			Marker.Location = MeshToWeapon.InverseTransformPosition(Eye.GetValue());
			Marker.Label = LOCTEXT("EyeMarker", "Eye when aiming");
			Marker.Color = FLinearColor(0.2f, 1.f, 1.f);
			Marker.RadiusCm = float(3.0 / Scale);
			FMantleBuilderOverlayLine& Line = Out.Lines.AddDefaulted_GetRef();
			Line.Start = Marker.Location;
			// Through the sight and on ahead of the gun, so a creator sees where the shot lands.
			Line.End = MeshToWeapon.InverseTransformPosition(SightLoc + Along * 150.0);
			Line.Color = FLinearColor(0.2f, 1.f, 1.f);
		}
	}
	return Out;
}

#undef LOCTEXT_NAMESPACE
