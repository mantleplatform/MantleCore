// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAccessoryPlacement.h"

namespace MantleAccessoryPlacement
{
	static FVector ClampPart(const FVector& Part, const double Min, const double Max)
	{
		return FVector(FMath::Clamp(Part.X, Min, Max), FMath::Clamp(Part.Y, Min, Max), FMath::Clamp(Part.Z, Min, Max));
	}

	constexpr double kMaxOffsetCm = 50.0;
	constexpr double kMinScale = 0.5;
	constexpr double kMaxScale = 2.0;

	static FVector ClampOffset(const FVector& Offset)
	{
		return ClampPart(Offset, -kMaxOffsetCm, kMaxOffsetCm);
	}

	static FVector ClampScale(const FVector& Scale)
	{
		return ClampPart(Scale, kMinScale, kMaxScale);
	}

	static FRotator ClampRotation(const FRotator& Part)
	{
		return FRotator(FMath::Clamp(Part.Pitch, -180.0, 180.0), FMath::Clamp(Part.Yaw, -180.0, 180.0), FMath::Clamp(Part.Roll, -180.0, 180.0));
	}

	FTransform ClampedAttach(const FMantleCosmeticTransform& Attach)
	{
		return FTransform(Attach.Rotation.IsSet() ? ClampRotation(Attach.Rotation.GetValue()) : FRotator::ZeroRotator,
			Attach.Location.IsSet() ? ClampOffset(Attach.Location.GetValue()) : FVector::ZeroVector,
			Attach.Scale.IsSet() ? ClampScale(Attach.Scale.GetValue()) : FVector::OneVector);
	}

	FTransform OverriddenSocket(const FTransform& SocketLocal, const FMantleCosmeticTransform* Override)
	{
		FTransform Out = SocketLocal;
		if (Override)
		{
			if (Override->Location.IsSet()) Out.SetLocation(ClampOffset(Override->Location.GetValue()));
			if (Override->Rotation.IsSet()) Out.SetRotation(ClampRotation(Override->Rotation.GetValue()).Quaternion());
			if (Override->Scale.IsSet()) Out.SetScale3D(ClampScale(Override->Scale.GetValue()));
		}
		return Out;
	}

	FTransform ComposeRelative(const FTransform& Attach, const FTransform& SocketLocal, const FMantleCosmeticTransform* Override)
	{
		return !Override || Override->IsEmpty() ? Attach : Attach * OverriddenSocket(SocketLocal, Override) * SocketLocal.Inverse();
	}

	float BoundsClampFactor(const FBox3f& Bounds, const FVector& Scale, const float MaxDimensionCm, const float MaxReachCm)
	{
		if (!Bounds.IsValid) return 1.0f;
		const FVector Min = FVector(Bounds.Min) * Scale.GetAbs();
		const FVector Max = FVector(Bounds.Max) * Scale.GetAbs();
		const double Edge = (Max - Min).GetMax();
		double Reach = 0.0;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Point((Corner & 1) ? Max.X : Min.X, (Corner & 2) ? Max.Y : Min.Y, (Corner & 4) ? Max.Z : Min.Z);
			Reach = FMath::Max(Reach, Point.Size());
		}
		double Factor = 1.0;
		if (Edge > MaxDimensionCm) Factor = FMath::Min(Factor, MaxDimensionCm / Edge);
		if (Reach > MaxReachCm) Factor = FMath::Min(Factor, MaxReachCm / Reach);
		return static_cast<float>(Factor);
	}

	bool FindSlotBounds(const FString& SlotId, float& OutMaxDimensionCm, float& OutMaxReachCm)
	{
		// Mirrors schema/avatar/limits.json rigid_accessory.boundsPerSlot; the two must be kept in step.
		struct FSlotBounds
		{
			const TCHAR* Slot;
			float MaxDimensionCm;
			float MaxReachCm;
		};
		static const FSlotBounds Table[] = {
			{ TEXT("acc_hat"), 60.0f, 50.0f },
			{ TEXT("acc_face"), 35.0f, 30.0f },
			{ TEXT("acc_neck"), 45.0f, 35.0f },
			{ TEXT("acc_shoulder_l"), 40.0f, 40.0f },
			{ TEXT("acc_shoulder_r"), 40.0f, 40.0f },
			{ TEXT("acc_front"), 60.0f, 50.0f },
			{ TEXT("acc_back"), 80.0f, 70.0f },
			{ TEXT("acc_waist"), 70.0f, 55.0f },
			{ TEXT("acc_eyes"), 25.0f, 20.0f },
			{ TEXT("acc_ear_l"), 12.0f, 12.0f },
			{ TEXT("acc_ear_r"), 12.0f, 12.0f },
			{ TEXT("acc_nose"), 5.0f, 5.0f },
			{ TEXT("acc_wrist_l"), 15.0f, 12.0f },
			{ TEXT("acc_wrist_r"), 15.0f, 12.0f },
			{ TEXT("acc_ring"), 6.0f, 5.0f },
		};
		for (const FSlotBounds& Row : Table)
		{
			if (SlotId.Equals(Row.Slot, ESearchCase::CaseSensitive))
			{
				OutMaxDimensionCm = Row.MaxDimensionCm;
				OutMaxReachCm = Row.MaxReachCm;
				return true;
			}
		}
		return false;
	}

	FTransform PlacedRelative(const FMantleCosmeticTransform& Attach, const FBox3f& Bounds, const FString& SlotId,
		const FTransform& SocketLocal, const FMantleCosmeticTransform* Override, float& OutFactor)
	{
		const FTransform Clamped = ClampedAttach(Attach);
		float MaxDimensionCm = 0.0f;
		float MaxReachCm = 0.0f;
		OutFactor = FindSlotBounds(SlotId, MaxDimensionCm, MaxReachCm) && Bounds.IsValid
			? BoundsClampFactor(Bounds, Clamped.GetScale3D(), MaxDimensionCm, MaxReachCm) : 1.0f;
		FTransform Placed = Clamped;
		Placed.SetScale3D(Clamped.GetScale3D() * OutFactor);
		return ComposeRelative(Placed, SocketLocal, Override);
	}
}
