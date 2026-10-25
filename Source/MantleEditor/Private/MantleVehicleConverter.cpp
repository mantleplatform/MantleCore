// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleConverter.h"
#include "MantleCosmeticExport.h"
#include "MantlePlaceAssets.h"
#include "MantleRuntime.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisDocument.h"
#include "MantleVehicleShape.h"
#include "MantleVehicleSource.h"

#include "Animation/PreviewAssetAttachComponent.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PreviewScene.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "HAL/IConsoleManager.h"
#include "ImageCore.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Sound/SoundBase.h"
#include "UObject/Package.h"

const TCHAR* FMantleVehicleConverter::ExtrasKey = TEXT("mantleVehicle");

namespace MantleVehicleConverterPrivate
{
	/** Every bone's reference pose in component space: the raw skeleton lists parents before children. */
	TArray<FTransform> VehicleComponentPose(const FReferenceSkeleton& Skeleton)
	{
		const TArray<FMeshBoneInfo>& Info = Skeleton.GetRawRefBoneInfo();
		const TArray<FTransform>& Pose = Skeleton.GetRawRefBonePose();
		TArray<FTransform> Out;
		Out.SetNum(Info.Num());
		for (int32 Bone = 0; Bone < Info.Num(); ++Bone)
		{
			const FTransform Local = Pose.IsValidIndex(Bone) ? Pose[Bone] : FTransform::Identity;
			const int32 Parent = Info[Bone].ParentIndex;
			Out[Bone] = Parent >= 0 && Parent < Bone ? Local * Out[Parent] : Local;
		}
		return Out;
	}

	/** Every glTF joint's reference pose in component space (joints are parent-first, as the reader writes them). */
	TArray<FTransform> VehicleJointPose(const FMantleGlbMesh& Mesh)
	{
		TArray<FTransform> Out;
		Out.SetNum(Mesh.Joints.Num());
		for (int32 Joint = 0; Joint < Mesh.Joints.Num(); ++Joint)
		{
			const int32 Parent = Mesh.Joints[Joint].Parent;
			Out[Joint] = Parent >= 0 && Parent < Joint ? Mesh.Joints[Joint].Local * Out[Parent] : Mesh.Joints[Joint].Local;
		}
		return Out;
	}

	/** 0.3 as "0.3": rounded to 4 decimals, trailing zeros dropped, so the same vehicle always writes the same bytes. */
	FString VehicleNumberText(double Value)
	{
		if (!FMath::IsFinite(Value))
		{
			Value = 0.0;
		}
		const double Rounded = FMath::RoundToDouble(Value * 10000.0) / 10000.0;
		FString Text = FString::Printf(TEXT("%.4f"), Rounded);
		while (Text.EndsWith(TEXT("0")))
		{
			Text.LeftChopInline(1);
		}
		if (Text.EndsWith(TEXT(".")))
		{
			Text.LeftChopInline(1);
		}
		return Text == TEXT("-0") ? FString(TEXT("0")) : Text;
	}

	TSharedRef<FJsonValue> VehicleNumber(double Value)
	{
		return MakeShared<FJsonValueNumberString>(VehicleNumberText(Value));
	}

	TSharedRef<FJsonValue> VehicleInteger(int32 Value)
	{
		return MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%d"), Value));
	}

	TSharedRef<FJsonValue> VehicleTriple(const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Items = { VehicleNumber(Value.X), VehicleNumber(Value.Y), VehicleNumber(Value.Z) };
		return MakeShared<FJsonValueArray>(Items);
	}

	/** A linear colour's RGB, as the schema's linearColor. */
	TSharedRef<FJsonValue> VehicleColor(const FLinearColor& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Items = { VehicleNumber(Value.R), VehicleNumber(Value.G), VehicleNumber(Value.B) };
		return MakeShared<FJsonValueArray>(Items);
	}

	/** A hinge (#546: a door, the trunk lid) as the schema's $defs.hinge, every key written. */
	TSharedRef<FJsonObject> VehicleHinge(const FMantleVehicleSourceHinge& Hinge)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("bone"), Hinge.Bone.ToString());
		Out->SetField(TEXT("pivot"), VehicleTriple(Hinge.Pivot));
		Out->SetField(TEXT("axis"), VehicleTriple(Hinge.Axis));
		Out->SetField(TEXT("scale"), VehicleNumber(Hinge.Scale));
		return Out;
	}

	/**
	 * The document keys after the collision, in the schema's order, then the horn (#546), whose sound's id is filled in at upload
	 * as theirs are (the cook reads the keys in any order); every writer rebuilds around them so they stay there.
	 */
	const TCHAR* const VehicleTailKeys[] = { TEXT("materials"), TEXT("paint"), TEXT("liveries"), TEXT("liveryUV"), TEXT("defaultLivery"),
		TEXT("linkage"), TEXT("parts"), TEXT("horn") };

	/** How near two bones must sit for a mesh on one to be a tire on the other (a visual wheel beside the wheel), cm. */
	constexpr double VehicleSamePlaceCm = 1.0;

	/** Whether Bone's name (any case) contains any of Words. */
	bool VehicleNameHas(const FName Bone, TConstArrayView<const TCHAR*> Words)
	{
		const FString Lower = Bone.ToString().ToLower();
		for (const TCHAR* Word : Words)
		{
			if (Lower.Contains(Word, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/** Whether Bone hangs (at any depth) from Ancestor, in a parent-first bone table. */
	bool VehicleHangsFrom(const TArray<FMeshBoneInfo>& Info, int32 Bone, const int32 Ancestor)
	{
		for (Bone = Info.IsValidIndex(Bone) ? Info[Bone].ParentIndex : INDEX_NONE; Bone != INDEX_NONE; Bone = Info[Bone].ParentIndex)
		{
			if (Bone == Ancestor)
			{
				return true;
			}
		}
		return false;
	}

	/** Words a hub's and a damper's bone names carry (DetectLinkage). */
	const TCHAR* const VehicleHubWords[] = { TEXT("hub"), TEXT("knuckle"), TEXT("upright"), TEXT("spindle"), TEXT("carrier") };
	const TCHAR* const VehicleDamperWords[] = { TEXT("damper"), TEXT("shock"), TEXT("strut"), TEXT("coilover"), TEXT("spring") };
	const TCHAR* const VehicleAxleWords[] = { TEXT("axle"), TEXT("beam") };
	const TCHAR* const VehicleWheelWords[] = { TEXT("wheel") };

	/** The schema's name for a linkage rule type. */
	const TCHAR* VehicleLinkageTypeName(const EMantleVehicleLinkageType Type)
	{
		return Type == EMantleVehicleLinkageType::Aim ? TEXT("aim") : (Type == EMantleVehicleLinkageType::Stretch ? TEXT("stretch") : TEXT("copy"));
	}

	/**
	 * Document with the tail keys in Replace set to its values (a null value removes the key) and the others kept, rebuilt
	 * rather than edited in place: a removed key's slot would take the next one added, out of order.
	 */
	TSharedRef<FJsonObject> VehicleRebuild(const TSharedRef<FJsonObject>& Document, const TMap<FString, TSharedPtr<FJsonValue>>& Replace)
	{
		TSharedRef<FJsonObject> Doc = MakeShared<FJsonObject>();
		for (const auto& Field : Document->Values)
		{
			const FString Key(Field.Key.ToView());
			bool bTail = false;
			for (const TCHAR* Tail : VehicleTailKeys)
			{
				bTail = bTail || Key.Equals(Tail, ESearchCase::CaseSensitive);
			}
			if (!bTail)
			{
				Doc->SetField(Key, Field.Value);
			}
		}
		for (const TCHAR* Tail : VehicleTailKeys)
		{
			const TSharedPtr<FJsonValue>* Replacement = Replace.Find(FString(Tail));
			const TSharedPtr<FJsonValue> Value = Replacement ? *Replacement : Document->TryGetField(FString(Tail));
			if (Value.IsValid())
			{
				Doc->SetField(FString(Tail), Value);
			}
		}
		return Doc;
	}

	/**
	 * Whether a livery's image has any alpha below opaque: where the paint shows through. Read from its source pixels, as
	 * the converter reads them; true when there are none to read (a cooked texture), which the converter refuses anyway.
	 */
	bool VehicleLiveryHasAlpha(const UTexture2D& Texture)
	{
		if (Texture.CompressionNoAlpha)
		{
			return false;
		}
		FImage Image;
		if (!Texture.Source.IsValid() || !const_cast<UTexture2D&>(Texture).Source.GetMipImage(Image, 0))
		{
			return true;
		}
		FImage Bgra;
		Image.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		for (const FColor& Pixel : Bgra.AsBGRA8())
		{
			if (Pixel.A < 255)
			{
				return true;
			}
		}
		return false;
	}

	/** "Wheel_FL" and the like, as a list for a message. */
	FString VehicleBoneList(TConstArrayView<FMantleVehicleWheelBone> Wheels)
	{
		TArray<FString> Names;
		for (const FMantleVehicleWheelBone& Wheel : Wheels)
		{
			Names.Add(Wheel.Bone.ToString());
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("none"));
	}

	/**
	 * The preset (#728) CheckSource is holding the vehicle to, or null: the single hook that narrows every range check below,
	 * so no check repeats itself. Set for the length of one CheckSource call by FVehiclePresetScope; the editor checks on one thread.
	 */
	const FMantleVehiclePreset* GVehicleCheckPreset = nullptr;

	struct FVehiclePresetScope
	{
		explicit FVehiclePresetScope(const FMantleVehiclePreset* Preset) : Previous(GVehicleCheckPreset) { GVehicleCheckPreset = Preset; }
		~FVehiclePresetScope() { GVehicleCheckPreset = Previous; }
		const FMantleVehiclePreset* Previous;
	};

	/** Holds Value to limits.json's range at Path, and to the preset's narrower one when a preset applies; an error naming What (and the source) when it is outside. */
	void VehicleCheckRange(const FString& Subject, const FString& What, const FString& Path, double Value, TArray<FMantleValidationIssue>& OutIssues)
	{
		double Min = 0.0;
		double Max = 0.0;
		if (!FMantleVehicleLimits::GetRange(Path, Min, Max))
		{
			return;
		}
		if (!FMath::IsFinite(Value) || Value < Min || Value > Max)
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
				FString::Printf(TEXT("%s: %s is %s, outside what a vehicle may have (%s to %s)."), *Subject, *What,
					*VehicleNumberText(Value), *VehicleNumberText(Min), *VehicleNumberText(Max)),
				FString::Printf(TEXT("Set %s between %s and %s in the vehicle asset."), *What, *VehicleNumberText(Min), *VehicleNumberText(Max)) });
			return;
		}
		FVector2D Narrow;
		if (GVehicleCheckPreset && FMantleVehiclePresets::RangeFor(*GVehicleCheckPreset, Path, Narrow) && (Value < Narrow.X || Value > Narrow.Y))
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
				FString::Printf(TEXT("%s: %s is %s, but a %s allows %s to %s."), *Subject, *What, *VehicleNumberText(Value),
					*GVehicleCheckPreset->Name, *VehicleNumberText(Narrow.X), *VehicleNumberText(Narrow.Y)),
				FString::Printf(TEXT("%s it, or build this vehicle from a different preset."), Value < Narrow.X ? TEXT("Raise") : TEXT("Lower")) });
		}
	}

	/** Caps a mesh's influences to Max per vertex: heaviest kept (the reader writes them heaviest first), renormalised. */
	void VehicleCapInfluences(FMantleGlbMesh& Mesh, int32 Max)
	{
		const int32 N = Mesh.NumInfluences;
		if (N <= Max || N <= 0)
		{
			return;
		}
		const int32 Count = Mesh.Positions.Num();
		TArray<uint16> Joints;
		TArray<float> Weights;
		Joints.SetNumZeroed(Count * Max);
		Weights.SetNumZeroed(Count * Max);
		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			float Total = 0.0f;
			for (int32 Slot = 0; Slot < Max; ++Slot)
			{
				Total += Mesh.InfluenceWeights[Vertex * N + Slot];
			}
			for (int32 Slot = 0; Slot < Max; ++Slot)
			{
				Joints[Vertex * Max + Slot] = Mesh.InfluenceJoints[Vertex * N + Slot];
				Weights[Vertex * Max + Slot] = Total > 0.0f ? Mesh.InfluenceWeights[Vertex * N + Slot] / Total : (Slot == 0 ? 1.0f : 0.0f);
			}
		}
		Mesh.NumInfluences = Max;
		Mesh.InfluenceJoints = MoveTemp(Joints);
		Mesh.InfluenceWeights = MoveTemp(Weights);
	}

	/** Any unit vector perpendicular to Normal, for a tire mesh read without tangents. */
	FVector VehiclePerpendicular(const FVector& Normal)
	{
		const FVector Axis = FMath::Abs(Normal.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector;
		return FVector::CrossProduct(Axis, Normal).GetSafeNormal();
	}

	/** Merges one tire mesh onto Joint, placed by Placement (tire space to component space), weighted to it alone. */
	bool VehicleMergeTire(FMantleGlbMesh& Body, TArray<UMaterialInterface*>& Materials, const UStaticMesh* Tire, int32 Joint,
		const FTransform& Placement, FString& OutError)
	{
		FMantleGlbMesh TireGlb;
		TArray<int32> Slots;
		int32 Triangles = 0;
		if (!FMantleCosmeticExport::ReadStaticMesh(Tire, 0, TireGlb, Slots, Triangles, OutError))
		{
			return false;
		}
		const FMatrix Matrix = Placement.ToMatrixWithScale();
		const FMatrix NormalMatrix = Matrix.Inverse().GetTransposed();
		const bool bMirrored = Matrix.Determinant() < 0.0;
		const int32 Base = Body.Positions.Num();
		const int32 Count = TireGlb.Positions.Num();
		const bool bBodyTangents = Body.Tangents.Num() > 0;
		const bool bTireTangents = TireGlb.Tangents.Num() == Count;

		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			Body.Positions.Add(FVector3f(FVector(Matrix.TransformPosition(FVector(TireGlb.Positions[Vertex])))));
			const FVector Normal = FVector(NormalMatrix.TransformVector(FVector(TireGlb.Normals.IsValidIndex(Vertex) ? TireGlb.Normals[Vertex] : FVector3f::UpVector))).GetSafeNormal();
			Body.Normals.Add(FVector3f(Normal));
			if (bBodyTangents)
			{
				if (bTireTangents)
				{
					const FVector4f& Tangent = TireGlb.Tangents[Vertex];
					const FVector Moved = FVector(Matrix.TransformVector(FVector(Tangent.X, Tangent.Y, Tangent.Z))).GetSafeNormal();
					Body.Tangents.Add(FVector4f(FVector3f(Moved), bMirrored ? -Tangent.W : Tangent.W));
				}
				else
				{
					Body.Tangents.Add(FVector4f(FVector3f(VehiclePerpendicular(Normal)), 1.0f));
				}
			}
			Body.UVs.Add(TireGlb.UVs.IsValidIndex(Vertex) ? TireGlb.UVs[Vertex] : FVector2f::ZeroVector);
			for (int32 Slot = 0; Slot < Body.NumInfluences; ++Slot)
			{
				Body.InfluenceJoints.Add(static_cast<uint16>(Slot == 0 ? Joint : 0));
				Body.InfluenceWeights.Add(Slot == 0 ? 1.0f : 0.0f);
			}
		}

		// The tire's materials: one already on the body is shared, a new one gets its own (unique) glTF material.
		const TArray<FStaticMaterial>& TireSlots = Tire->GetStaticMaterials();
		TArray<int32> BodyMaterialOf;
		for (int32 Index = 0; Index < TireGlb.MaterialNames.Num(); ++Index)
		{
			const int32 Slot = Slots.IsValidIndex(Index) ? Slots[Index] : INDEX_NONE;
			UMaterialInterface* Material = TireSlots.IsValidIndex(Slot) ? TireSlots[Slot].MaterialInterface.Get() : nullptr;
			int32 Found = Material ? Materials.IndexOfByKey(Material) : INDEX_NONE;
			if (Found == INDEX_NONE)
			{
				FString Name = TireGlb.MaterialNames[Index];
				for (int32 Suffix = 2; Body.MaterialNames.ContainsByPredicate([&Name](const FString& Existing) { return Existing.Equals(Name, ESearchCase::CaseSensitive); }); ++Suffix)
				{
					Name = FString::Printf(TEXT("%s_%d"), *TireGlb.MaterialNames[Index], Suffix);
				}
				Body.MaterialNames.Add(Name);
				Materials.Add(Material);
				Found = Body.MaterialNames.Num() - 1;
			}
			BodyMaterialOf.Add(Found);
		}

		for (const FMantleGlbPrimitive& Source : TireGlb.Primitives)
		{
			FMantleGlbPrimitive& Primitive = Body.Primitives.AddDefaulted_GetRef();
			Primitive.MaterialIndex = BodyMaterialOf.IsValidIndex(Source.MaterialIndex) ? BodyMaterialOf[Source.MaterialIndex] : 0;
			Primitive.FirstVertex = Base + Source.FirstVertex;
			Primitive.NumVertices = Source.NumVertices;
			Primitive.Indices = Source.Indices;
			if (bMirrored)
			{
				// A mirrored tire turns its triangles inside out: swap two corners of each back.
				for (int32 Corner = 0; Corner + 2 < Primitive.Indices.Num(); Corner += 3)
				{
					Swap(Primitive.Indices[Corner + 1], Primitive.Indices[Corner + 2]);
				}
			}
		}
		return true;
	}

	/**
	 * A bone name's corner in the word naming (wheel_front_l, Wheel_Rear_Right, left_front_wheel): its words split on "_", any
	 * case, a position word anywhere (front; middle or mid; rear or back) and a side word first or last (l, left, r, right).
	 */
	bool VehicleWordCorner(const FString& Bone, int32& OutAxle, bool& bOutLeft)
	{
		TArray<FString> Words;
		Bone.ToLower().ParseIntoArray(Words, TEXT("_"), /*InCullEmpty*/ true);
		if (Words.Num() < 2)
		{
			return false;
		}
		auto Side = [](const FString& Word, bool& bLeft)
		{
			bLeft = Word.Equals(TEXT("l"), ESearchCase::CaseSensitive) || Word.Equals(TEXT("left"), ESearchCase::CaseSensitive);
			return bLeft || Word.Equals(TEXT("r"), ESearchCase::CaseSensitive) || Word.Equals(TEXT("right"), ESearchCase::CaseSensitive);
		};
		if (!Side(Words.Last(), bOutLeft) && !Side(Words[0], bOutLeft))
		{
			return false;
		}
		for (const FString& Word : Words)
		{
			if (Word.Equals(TEXT("front"), ESearchCase::CaseSensitive))
			{
				OutAxle = 0;
				return true;
			}
			if (Word.Equals(TEXT("middle"), ESearchCase::CaseSensitive) || Word.Equals(TEXT("mid"), ESearchCase::CaseSensitive))
			{
				OutAxle = 1;
				return true;
			}
			if (Word.Equals(TEXT("rear"), ESearchCase::CaseSensitive) || Word.Equals(TEXT("back"), ESearchCase::CaseSensitive))
			{
				OutAxle = 2;
				return true;
			}
		}
		return false;
	}

	/**
	 * Whether a wheel-named bone is a helper beside the wheel rather than the wheel: a steering pivot (wheel_front_turn_l, a
	 * word with "turn", "steer" or "pivot"), a part that does not spin (wheel_rear_no_spin_l: "nospin", or "no"/"non" then
	 * "spin") or a hub (a word with a hub's word, VehicleHubWords).
	 */
	bool VehicleWheelHelper(const FString& Bone)
	{
		static const TCHAR* const Helpers[] = { TEXT("turn"), TEXT("steer"), TEXT("pivot"), TEXT("nospin"), TEXT("nonspin"), TEXT("hub"), TEXT("knuckle"),
			TEXT("upright"), TEXT("spindle"), TEXT("carrier") };
		TArray<FString> Words;
		Bone.ToLower().ParseIntoArray(Words, TEXT("_"), /*InCullEmpty*/ true);
		for (int32 Index = 0; Index < Words.Num(); ++Index)
		{
			for (const TCHAR* Helper : Helpers)
			{
				if (Words[Index].Contains(Helper, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			if ((Words[Index].Equals(TEXT("no"), ESearchCase::CaseSensitive) || Words[Index].Equals(TEXT("non"), ESearchCase::CaseSensitive))
				&& Words.IsValidIndex(Index + 1) && Words[Index + 1].Equals(TEXT("spin"), ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/** A wheel's helper named like a wheel (wheel_rear_no_spin_l): not a "SteeringArm_FL", which aims as an arm does. */
	bool VehicleNamedHelper(const FName Bone)
	{
		return VehicleWheelHelper(Bone.ToString()) && VehicleNameHas(Bone, MakeArrayView(VehicleWheelWords));
	}

	/** Whether a bone name is a steering pivot's: a word with "turn", "steer" or "pivot" (wheel_front_turn_l). */
	bool VehicleSteeringPivot(const FString& Bone)
	{
		static const TCHAR* const Pivots[] = { TEXT("turn"), TEXT("steer"), TEXT("pivot") };
		TArray<FString> Words;
		Bone.ToLower().ParseIntoArray(Words, TEXT("_"), /*InCullEmpty*/ true);
		return Words.ContainsByPredicate([](const FString& Word)
		{
			for (const TCHAR* Pivot : Pivots)
			{
				if (Word.Contains(Pivot, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		});
	}

	/** How far a tire's centre may sit off its wheel's spin axis (in X and Z), cm; how round its bounds must be; its least size across. */
	constexpr double VehicleTireAxisCm = 3.0;
	constexpr double VehicleTireRoundness = 0.15;
	constexpr double VehicleTireMinDiameterCm = 10.0;
	/** How far a tire's size across may be from twice a wheel's known radius, as a fraction of it. */
	constexpr double VehicleTireDiameterSlack = 0.2;

	/**
	 * Whether Placed (a mesh's bounds, placed in component space) is shaped and placed like a tire of the wheel at Hub: as wide in X
	 * as in Z (within VehicleTireRoundness), narrower in Y than across, at least VehicleTireMinDiameterCm across, its centre within
	 * VehicleTireAxisCm of the wheel's spin axis (X and Z) and within its radius of the hub along it (a tire modelled in car space,
	 * or outboard of a steering pivot), and, when Radius is known, across within VehicleTireDiameterSlack of 2 x Radius. Its size
	 * across when it is; 0 when not.
	 */
	double VehicleTireFit(const FBox& Placed, const FVector& Hub, const double Radius)
	{
		if (!Placed.IsValid)
		{
			return 0.0;
		}
		const FVector Size = Placed.GetSize();
		const FVector Offset = Placed.GetCenter() - Hub;
		const double Across = FMath::Max(Size.X, Size.Z);
		const bool bRound = Across >= VehicleTireMinDiameterCm && FMath::Abs(Size.X - Size.Z) <= VehicleTireRoundness * Across && Size.Y < Across;
		const bool bOnAxis = FMath::Abs(Offset.X) <= VehicleTireAxisCm && FMath::Abs(Offset.Z) <= VehicleTireAxisCm
			&& FMath::Abs(Offset.Y) <= FMath::Max(VehicleTireAxisCm, 0.5 * Across);
		const bool bSized = Radius <= 0.0 || FMath::Abs(Across - 2.0 * Radius) <= VehicleTireDiameterSlack * 2.0 * Radius;
		return bRound && bOnAxis && bSized ? Across : 0.0;
	}

	/** A numeric property's value on Container (float or double, by name: MantleCore takes no plugin dependency); 0 when it has none. */
	double VehicleNumberProperty(const UStruct* Type, const void* Container, const TCHAR* Name)
	{
		const FNumericProperty* Property = Type ? FindFProperty<FNumericProperty>(Type, Name) : nullptr;
		return Property && Container && Property->IsFloatingPoint() ? Property->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Container)) : 0.0;
	}

	// #612: GuessSeatContacts' first guesses, from the default avatar seated in the pack sedan: its
	// pelvis 34 cm under the seat's capsule centre and 4.3 cm ahead of it; the balls of its feet about 80 cm ahead and 70 cm down, a
	// hand's width either side of the seat's middle; its hands at ten and two on a steering wheel, or on its lap without one.
	const FVector VehicleGuessPelvis(4.3, 0.0, -34.0);
	const FVector VehicleGuessFoot(80.0, 12.0, -70.0);
	const FVector VehicleGuessLap(30.0, 15.0, 5.0);
	constexpr double VehicleGuessRimDegrees = 60.0;
	/** An exit's distance from the vehicle's centre line: past the collision's side by this, and at least VehicleGuessExitMin (the client's). */
	constexpr double VehicleGuessExitClear = 100.0;
	constexpr double VehicleGuessExitMin = 220.0;

	/** A contact point as the schema's $defs.contactPoint: location, the rotation as [pitch, yaw, roll] within +-180, and its bone when on one. */
	TSharedRef<FJsonObject> VehicleContact(const FMantleVehicleSourceContact& Point)
	{
		const FRotator Turn = Point.Rotation.GetNormalized();
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetField(TEXT("location"), VehicleTriple(Point.Location));
		Out->SetField(TEXT("rotation"), VehicleTriple(FVector(Turn.Pitch, Turn.Yaw, Turn.Roll)));
		if (!Point.Bone.IsNone())
		{
			Out->SetStringField(TEXT("bone"), Point.Bone.ToString());
		}
		return Out;
	}

	/** A seat's contact points (#612) into its object, after its door, each group only when set: every key the cook resolves written. */
	void WriteVehicleSeatContacts(const TSharedRef<FJsonObject>& Seat, const FMantleVehicleSourceSeatContacts& Contacts)
	{
		if (Contacts.bPelvis)
		{
			Seat->SetObjectField(TEXT("pelvis"), VehicleContact(Contacts.Pelvis));
		}
		if (Contacts.Hands != EMantleVehicleHandsKind::None)
		{
			TSharedRef<FJsonObject> Hands = MakeShared<FJsonObject>();
			Hands->SetStringField(TEXT("kind"), Contacts.Hands == EMantleVehicleHandsKind::Wheel ? TEXT("wheel") : TEXT("fixed"));
			Hands->SetObjectField(TEXT("left"), VehicleContact(Contacts.LeftHand));
			Hands->SetObjectField(TEXT("right"), VehicleContact(Contacts.RightHand));
			Seat->SetObjectField(TEXT("hands"), Hands);
		}
		if (Contacts.bFeet)
		{
			TSharedRef<FJsonObject> Feet = MakeShared<FJsonObject>();
			Feet->SetObjectField(TEXT("left"), VehicleContact(Contacts.LeftFoot));
			Feet->SetObjectField(TEXT("right"), VehicleContact(Contacts.RightFoot));
			Seat->SetObjectField(TEXT("feet"), Feet);
		}
		if (Contacts.Exits.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Exits;
			for (const FMantleVehicleSourceExit& Exit : Contacts.Exits)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetField(TEXT("location"), VehicleTriple(Exit.Location));
				Entry->SetField(TEXT("yaw"), VehicleNumber(FRotator::NormalizeAxis(Exit.Yaw)));
				Exits.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Seat->SetArrayField(TEXT("exits"), Exits);
		}
	}
}

FString FMantleVehicleConverter::VehicleKey(const UMantleVehicleSource* Source)
{
	return Source ? TEXT("vehicle:") + Source->GetPathName() : FString();
}

bool FMantleVehicleConverter::IsUploadedId(const FString& Id)
{
	if (Id.Len() != 37 || !Id.StartsWith(TEXT("ast_u"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	for (int32 Index = 5; Index < Id.Len(); ++Index)
	{
		const TCHAR Char = Id[Index];
		if (!((Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

bool FMantleVehicleConverter::ParseWheelBoneName(const FString& Bone, FMantleVehicleWheelBone& Out)
{
	using namespace MantleVehicleConverterPrivate;
	Out.bCentre = false;
	// A steering pivot, a part that does not spin or a hub named like its wheel is not the wheel: the one beside it without those words is.
	if (VehicleWheelHelper(Bone))
	{
		return false;
	}
	struct FCorner
	{
		const TCHAR* Suffix;
		int32 Axle;
		bool bLeft;
	};
	static const FCorner Corners[] = {
		{ TEXT("_fl"), 0, true }, { TEXT("_fr"), 0, false },
		{ TEXT("_ml"), 1, true }, { TEXT("_mr"), 1, false },
		{ TEXT("_bl"), 2, true }, { TEXT("_br"), 2, false },
		{ TEXT("_rl"), 2, true }, { TEXT("_rr"), 2, false },
	};
	const FString Lower = Bone.ToLower();
	for (const FCorner& Corner : Corners)
	{
		if (Lower.EndsWith(Corner.Suffix, ESearchCase::CaseSensitive)
			&& Lower.LeftChop(FCString::Strlen(Corner.Suffix)).Contains(TEXT("wheel"), ESearchCase::CaseSensitive))
		{
			Out.Bone = FName(*Bone);
			Out.Axle = Corner.Axle;
			Out.bLeft = Corner.bLeft;
			return true;
		}
	}
	// The word naming a common free car pack uses (wheel_front_l, wheel_rear_r): "wheel" in one of its words, and a corner.
	TArray<FString> Words;
	Lower.ParseIntoArray(Words, TEXT("_"), /*InCullEmpty*/ true);
	int32 Axle = 0;
	bool bLeft = false;
	const bool bWheelWord = Words.ContainsByPredicate([](const FString& Word) { return Word.Contains(TEXT("wheel"), ESearchCase::CaseSensitive); });
	if (bWheelWord && VehicleWordCorner(Bone, Axle, bLeft))
	{
		Out.Bone = FName(*Bone);
		Out.Axle = Axle;
		Out.bLeft = bLeft;
		return true;
	}
	// A wheel on the centre line (a motorcycle's two, a plane's nose wheel; #821): "wheel" and a position word and no side, as wheel_f and wheel_r, wheel_front and
	// wheel_rear. Front is the front axle; rear, with the other corners' _R, is the back one.
	if (bWheelWord)
	{
		const bool bFront = Words.Contains(TEXT("f")) || Words.Contains(TEXT("front"));
		const bool bRear = Words.Contains(TEXT("r")) || Words.Contains(TEXT("rear")) || Words.Contains(TEXT("back"));
		if (bFront != bRear)
		{
			Out.Bone = FName(*Bone);
			Out.Axle = bFront ? 0 : 2;
			Out.bLeft = false;
			Out.bCentre = true;
			return true;
		}
	}
	return false;
}

TArray<FMantleVehicleWheelBone> FMantleVehicleConverter::FindWheelBones(const FReferenceSkeleton& Skeleton)
{
	using namespace MantleVehicleConverterPrivate;
	const TArray<FMeshBoneInfo>& Info = Skeleton.GetRawRefBoneInfo();
	const TArray<FTransform> Pose = VehicleComponentPose(Skeleton);
	struct FFound
	{
		FMantleVehicleWheelBone Wheel;
		double X = 0.0;
	};
	TArray<FFound> Found;
	TArray<int32> FoundBones;
	// The root carries the chassis, never a wheel.
	for (int32 Bone = 1; Bone < Info.Num(); ++Bone)
	{
		FFound Entry;
		if (ParseWheelBoneName(Info[Bone].Name.ToString(), Entry.Wheel))
		{
			Entry.X = Pose[Bone].GetLocation().X;
			Found.Add(Entry);
			FoundBones.Add(Bone);
		}
	}
	// #435 (b): a visual copy of a wheel (the Vehicle template's VisWheel_FL, under its hub, where PhysWheel_FL is on the
	// root) is not a wheel of its own; the linkage copies it onto the wheel.
	TArray<FFound> Wheels;
	for (int32 A = 0; A < Found.Num(); ++A)
	{
		bool bCopy = false;
		for (int32 B = 0; B < Found.Num() && !bCopy; ++B)
		{
			bCopy = B != A && Found[A].Wheel.Axle == Found[B].Wheel.Axle && Found[A].Wheel.bLeft == Found[B].Wheel.bLeft
				&& Info[FoundBones[A]].ParentIndex != 0 && Info[FoundBones[B]].ParentIndex == 0
				&& FVector::Dist(Pose[FoundBones[A]].GetLocation(), Pose[FoundBones[B]].GetLocation()) < 1.0;
		}
		if (!bCopy)
		{
			// A wheel hanging from a steering pivot of its corner (a common free car pack's wheel_front_l under wheel_front_turn_l)
			// is carried by it, and no linkage may move a bone a wheel hangs from: the pivot is the wheel, as that pack's ChaosVehicles
			// wheel setups name it, so it steers and spins; DetectLinkage gives the named wheel its steering and travel, not its spin.
			const int32 Parent = Info[FoundBones[A]].ParentIndex;
			if (Parent > 0 && VehicleSteeringPivot(Info[Parent].Name.ToString()) && VehicleNameHas(Info[Parent].Name, MakeArrayView(VehicleWheelWords))
				&& CornerOf(Info[Parent].Name.ToString()) == CornerOf(Info[FoundBones[A]].Name.ToString()))
			{
				Found[A].Wheel.Bone = Info[Parent].Name;
				Found[A].X = Pose[Parent].GetLocation().X;
			}
			Wheels.Add(Found[A]);
		}
	}
	Found = MoveTemp(Wheels);
	Found.StableSort([](const FFound& A, const FFound& B)
	{
		if (A.Wheel.Axle != B.Wheel.Axle)
		{
			return A.Wheel.Axle < B.Wheel.Axle;
		}
		if (!FMath::IsNearlyEqual(A.X, B.X, 1.0))
		{
			return A.X > B.X;
		}
		return A.Wheel.bLeft && !B.Wheel.bLeft;
	});
	TArray<FMantleVehicleWheelBone> Out;
	for (const FFound& Entry : Found)
	{
		Out.Add(Entry.Wheel);
	}
	return Out;
}

FString FMantleVehicleConverter::CornerOf(const FString& Bone)
{
	static const TCHAR* const Corners[] = { TEXT("_fl"), TEXT("_fr"), TEXT("_ml"), TEXT("_mr"), TEXT("_bl"), TEXT("_br"), TEXT("_rl"), TEXT("_rr") };
	const FString Lower = Bone.ToLower();
	for (const TCHAR* Corner : Corners)
	{
		if (Lower.Len() > 3 && Lower.EndsWith(Corner, ESearchCase::CaseSensitive))
		{
			const FString Found(Corner);
			return Found == TEXT("_rl") ? FString(TEXT("_bl")) : (Found == TEXT("_rr") ? FString(TEXT("_br")) : Found);
		}
	}
	// The word naming (wheel_rear_no_spin_l is "_bl"), spelt as the suffix naming spells it.
	int32 Axle = 0;
	bool bLeft = false;
	if (MantleVehicleConverterPrivate::VehicleWordCorner(Bone, Axle, bLeft))
	{
		static const TCHAR* const Axles[] = { TEXT("_f"), TEXT("_m"), TEXT("_b") };
		return FString(Axles[Axle]) + (bLeft ? TEXT("l") : TEXT("r"));
	}
	return FString();
}

TArray<FMantleVehicleSourceLinkage> FMantleVehicleConverter::DetectLinkage(const FReferenceSkeleton& Skeleton, const TConstArrayView<FName> WheelBones,
	TArray<FString>& OutNotes)
{
	using namespace MantleVehicleConverterPrivate;
	const TArray<FMeshBoneInfo>& Info = Skeleton.GetRawRefBoneInfo();
	const TArray<FTransform> Pose = VehicleComponentPose(Skeleton);
	const int32 Num = Info.Num();
	TArray<FMantleVehicleSourceLinkage> Rules;
	if (Num < 2)
	{
		return Rules;
	}

	TArray<int32> Wheels;
	for (const FName Wheel : WheelBones)
	{
		const int32 Bone = Skeleton.FindRawBoneIndex(Wheel);
		if (Bone > 0)
		{
			Wheels.Add(Bone);
		}
	}
	// Moving: the wheels, every bone a rule moves, and everything hanging from them. Carrying: the bones a wheel hangs
	// from (and the root), which no rule may move.
	TArray<bool> Moving;
	TArray<bool> Carrying;
	Moving.Init(false, Num);
	Carrying.Init(false, Num);
	Carrying[0] = true;
	auto MarkMoving = [&Moving, &Info, Num](const int32 Bone)
	{
		Moving[Bone] = true;
		for (int32 Child = Bone + 1; Child < Num; ++Child)
		{
			Moving[Child] = Moving[Child] || (Info[Child].ParentIndex >= 0 && Moving[Info[Child].ParentIndex]);
		}
	};
	for (const int32 Wheel : Wheels)
	{
		MarkMoving(Wheel);
		for (int32 Bone = Info[Wheel].ParentIndex; Bone != INDEX_NONE; Bone = Info[Bone].ParentIndex)
		{
			Carrying[Bone] = true;
		}
	}
	auto Free = [&Moving, &Carrying](const int32 Bone) { return !Moving[Bone] && !Carrying[Bone]; };
	auto At = [&Pose](const int32 Bone) { return Pose[Bone].GetLocation(); };
	auto Add = [&Rules, &Info](const EMantleVehicleLinkageType Type, const int32 Bone)
	{
		FMantleVehicleSourceLinkage& Rule = Rules.AddDefaulted_GetRef();
		Rule.Type = Type;
		Rule.Bone = Info[Bone].Name;
		return &Rule;
	};

	// A wheel that is a steering pivot (FindWheelBones): the wheel-named bone hanging from it takes its travel and steering, not its
	// spin, so what rides it (a brake caliper) steers without spinning. It hangs from the wheel, so it is moving already.
	for (const int32 Wheel : Wheels)
	{
		const FString Corner = CornerOf(Info[Wheel].Name.ToString());
		FMantleVehicleWheelBone Named;
		for (int32 Bone = Wheel + 1; Bone < Num && VehicleSteeringPivot(Info[Wheel].Name.ToString()); ++Bone)
		{
			if (Info[Bone].ParentIndex == Wheel && !Wheels.Contains(Bone) && CornerOf(Info[Bone].Name.ToString()) == Corner
				&& ParseWheelBoneName(Info[Bone].Name.ToString(), Named))
			{
				FMantleVehicleSourceLinkage* Rule = Add(EMantleVehicleLinkageType::Copy, Bone);
				Rule->Source = Info[Wheel].Name;
				Rule->Axis = EMantleVehicleLinkageAxis::Z;
			}
		}
	}
	// Hubs copy their wheel's travel and steering, not its spin; so does a wheel's helper beside it (wheel_rear_no_spin_l, the brake
	// side of a common free car pack's wheel). A helper the wheel hangs from carries it and is never moved.
	for (const int32 Wheel : Wheels)
	{
		const FString Corner = CornerOf(Info[Wheel].Name.ToString());
		for (int32 Bone = 1; Bone < Num; ++Bone)
		{
			if (Free(Bone) && CornerOf(Info[Bone].Name.ToString()) == Corner
				&& (VehicleNameHas(Info[Bone].Name, MakeArrayView(VehicleHubWords)) || VehicleNamedHelper(Info[Bone].Name)))
			{
				FMantleVehicleSourceLinkage* Rule = Add(EMantleVehicleLinkageType::Copy, Bone);
				Rule->Source = Info[Wheel].Name;
				Rule->Axis = EMantleVehicleLinkageAxis::Z;
				MarkMoving(Bone);
			}
		}
	}
	// A visual wheel copies the wheel whole: after the hubs, so it wins over a hub carrying it. A helper (one that does not spin) is not one.
	for (const int32 Wheel : Wheels)
	{
		const FString Corner = CornerOf(Info[Wheel].Name.ToString());
		for (int32 Bone = 1; Bone < Num; ++Bone)
		{
			if (!Carrying[Bone] && !Wheels.Contains(Bone) && !VehicleHangsFrom(Info, Bone, Wheel) && CornerOf(Info[Bone].Name.ToString()) == Corner
				&& !VehicleWheelHelper(Info[Bone].Name.ToString())
				&& VehicleNameHas(Info[Bone].Name, MakeArrayView(VehicleWheelWords)) && FVector::Dist(At(Bone), At(Wheel)) < 1.0)
			{
				Add(EMantleVehicleLinkageType::Copy, Bone)->Source = Info[Wheel].Name;
				MarkMoving(Bone);
			}
		}
	}
	// An axle or beam at a wheel's place follows its travel and aims at the other wheel of its axle.
	for (const int32 Wheel : Wheels)
	{
		for (int32 Bone = 1; Bone < Num; ++Bone)
		{
			if (!Free(Bone) || !VehicleNameHas(Info[Bone].Name, MakeArrayView(VehicleAxleWords)) || FVector::Dist(At(Bone), At(Wheel)) >= 1.0)
			{
				continue;
			}
			FMantleVehicleSourceLinkage* Follow = Add(EMantleVehicleLinkageType::Copy, Bone);
			Follow->Source = Info[Wheel].Name;
			Follow->bRotation = false;
			for (const int32 Other : Wheels)
			{
				if (Other != Wheel && FMath::Abs(At(Other).X - At(Wheel).X) < 5.0 && (At(Other).Y > 0.0) != (At(Wheel).Y > 0.0))
				{
					Add(EMantleVehicleLinkageType::Aim, Bone)->Target = Info[Other].Name;
					break;
				}
			}
			MarkMoving(Bone);
		}
	}
	// The moving bone nearest one of Bone's children, within 2 cm or a tenth of the child's reach; INDEX_NONE for none.
	auto SitsOn = [&Info, &Moving, &At, Num](const int32 Bone, int32& OutChild)
	{
		int32 Best = INDEX_NONE;
		double BestDistance = 0.0;
		for (int32 Child = Bone + 1; Child < Num; ++Child)
		{
			if (Info[Child].ParentIndex != Bone)
			{
				continue;
			}
			const double Tolerance = FMath::Max(2.0, 0.1 * FVector::Dist(At(Child), At(Bone)));
			for (int32 Other = 1; Other < Num; ++Other)
			{
				const double Distance = FVector::Dist(At(Other), At(Child));
				if (Moving[Other] && !VehicleHangsFrom(Info, Other, Bone) && Distance <= Tolerance && (Best == INDEX_NONE || Distance < BestDistance))
				{
					Best = Other;
					BestDistance = Distance;
					OutChild = Child;
				}
			}
		}
		return Best;
	};
	// Arms and links aim at what their end sits on, until nothing more follows (a link on an arm follows the arm).
	for (bool bFound = true; bFound;)
	{
		bFound = false;
		for (int32 Bone = 1; Bone < Num; ++Bone)
		{
			int32 Child = INDEX_NONE;
			if (!Free(Bone) || CornerOf(Info[Bone].Name.ToString()).IsEmpty() || VehicleNameHas(Info[Bone].Name, MakeArrayView(VehicleDamperWords)))
			{
				continue;
			}
			const int32 Target = SitsOn(Bone, Child);
			if (Target != INDEX_NONE)
			{
				Add(EMantleVehicleLinkageType::Aim, Bone)->Target = Info[Target].Name;
				MarkMoving(Bone);
				bFound = true;
			}
		}
	}
	// Dampers stretch from their top to what their end sits on.
	for (int32 Bone = 1; Bone < Num; ++Bone)
	{
		int32 Child = INDEX_NONE;
		if (!Free(Bone) || CornerOf(Info[Bone].Name.ToString()).IsEmpty() || !VehicleNameHas(Info[Bone].Name, MakeArrayView(VehicleDamperWords)))
		{
			continue;
		}
		const int32 Target = SitsOn(Bone, Child);
		if (Target != INDEX_NONE)
		{
			FMantleVehicleSourceLinkage* Rule = Add(EMantleVehicleLinkageType::Stretch, Bone);
			Rule->End = Info[Child].Name;
			Rule->Target = Info[Target].Name;
			MarkMoving(Bone);
		}
	}

	TArray<FString> Left;
	for (int32 Bone = 1; Bone < Num; ++Bone)
	{
		if (Free(Bone) && !CornerOf(Info[Bone].Name.ToString()).IsEmpty())
		{
			Left.Add(Info[Bone].Name.ToString());
		}
	}
	if (Rules.Num() > 0)
	{
		int32 Copies = 0;
		int32 Aims = 0;
		int32 Stretches = 0;
		for (const FMantleVehicleSourceLinkage& Rule : Rules)
		{
			Copies += Rule.Type == EMantleVehicleLinkageType::Copy ? 1 : 0;
			Aims += Rule.Type == EMantleVehicleLinkageType::Aim ? 1 : 0;
			Stretches += Rule.Type == EMantleVehicleLinkageType::Stretch ? 1 : 0;
		}
		OutNotes.Add(FString::Printf(TEXT("Suspension linkage: %d rules found from how the model's parts are named and placed (%d follow a wheel, %d point at something, %d dampers stretch). Mantle does not copy the original's animation setup (its Control Rig or animation blueprint): compare the car with the original side by side, and edit the rules under Linkage in the vehicle asset. Anything else the original's setup does (IK chains, procedural effects) is not kept."),
			Rules.Num(), Copies, Aims, Stretches));
	}
	if (Left.Num() > 0)
	{
		OutNotes.Add(FString::Printf(TEXT("These parts of the model stay still: %s. If one should move with a wheel, add a rule for it under Linkage in the vehicle asset."),
			*FString::Join(Left, TEXT(", "))));
	}
	if (Rules.Num() > FMantleVehicleLimits::MaxLinkageRules)
	{
		OutNotes.Add(FString::Printf(TEXT("That is more than the %d rules a vehicle may have: remove some under Linkage before publishing."), FMantleVehicleLimits::MaxLinkageRules));
	}
	return Rules;
}

int32 FMantleVehicleConverter::JointIndex(const FMantleGlbMesh& Body, FName Bone)
{
	return Body.Joints.IndexOfByPredicate([Bone](const FMantleGlbJoint& Joint) { return Joint.Name == Bone; });
}

bool FMantleVehicleConverter::MeasureWheel(const FMantleGlbMesh& Body, FName Bone, double& OutRadius, double& OutWidth)
{
	using namespace MantleVehicleConverterPrivate;
	OutRadius = 0.0;
	OutWidth = 0.0;
	const int32 Joint = FMantleVehicleConverter::JointIndex(Body, Bone);
	const int32 N = Body.NumInfluences;
	if (Joint == INDEX_NONE || N <= 0)
	{
		return false;
	}
	// The cook's rule: the vertices whose heaviest influence is the bone at full weight (255 of 255).
	const float Full = 254.5f / 255.0f;
	FBox Extent(ForceInit);
	int32 Count = 0;
	for (int32 Vertex = 0; Vertex < Body.Positions.Num(); ++Vertex)
	{
		if (Body.InfluenceJoints.IsValidIndex(Vertex * N) && Body.InfluenceJoints[Vertex * N] == Joint && Body.InfluenceWeights[Vertex * N] >= Full)
		{
			Extent += FVector(Body.Positions[Vertex]);
			++Count;
		}
	}
	if (Count < WheelMinVertices)
	{
		return false;
	}
	const FVector Size = Extent.GetSize();
	OutRadius = FMath::Max(Size.X, Size.Z) / 2.0;
	OutWidth = Size.Y;
	return true;
}

FMantleVehicleConversion FMantleVehicleConverter::Convert(USkeletalMesh* Mesh, UMantleVehicleSource& Source)
{
	return Convert(Mesh, Source, Mesh ? SurroundingsOfMesh(*Mesh) : FMantleVehicleSurroundings());
}

FMantleVehicleConversion FMantleVehicleConverter::Convert(USkeletalMesh* Mesh, UMantleVehicleSource& Source, const FMantleVehicleSurroundings& Around)
{
	using namespace MantleVehicleConverterPrivate;
	FMantleVehicleConversion Result;
	if (!Mesh)
	{
		Result.Problems.Add(TEXT("Select the model to use as the vehicle's body (it needs a wheel part at the centre of each wheel), then convert it."));
		return Result;
	}
	const FString Name = Mesh->GetName();
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	const int32 NumBones = Skeleton.GetRawBoneNum();
	if (NumBones > FMantleVehicleLimits::MaxBones)
	{
		Result.Problems.Add(FString::Printf(TEXT("%s has %d joints (bones); a vehicle model has at most %d. Remove the joints the vehicle does not need (each wheel needs one at its centre) and re-import it."),
			*Name, NumBones, FMantleVehicleLimits::MaxBones));
	}

	const TArray<FMantleVehicleWheelBone> Wheels = FindWheelBones(Skeleton);
	if (Wheels.Num() < FMantleVehicleLimits::MinWheels || Wheels.Num() > FMantleVehicleLimits::MaxWheels)
	{
		Result.Problems.Add(FString::Printf(TEXT("%s has %d wheels (%s), but a vehicle has %d to %d. Name each wheel so its name contains \"wheel\" and ends in its corner, such as Wheel_FL, Wheel_FR, Wheel_BL and Wheel_BR (_ML/_MR for a middle axle) or wheel_front_l and wheel_rear_r, and re-import it."),
			*Name, Wheels.Num(), *VehicleBoneList(Wheels), FMantleVehicleLimits::MinWheels, FMantleVehicleLimits::MaxWheels));
	}
	else if (!Wheels.ContainsByPredicate([](const FMantleVehicleWheelBone& Wheel) { return Wheel.Axle == 0; }))
	{
		Result.Problems.Add(FString::Printf(TEXT("%s has no front wheels to steer with (the wheels found: %s). Name the front wheels so their names end in _FL and _FR, and re-import it."),
			*Name, *VehicleBoneList(Wheels)));
	}

	// Two wheel bones at one place is a hub and a wheel both named like wheels: the vehicle would have a wheel twice.
	const TArray<FTransform> Pose = VehicleComponentPose(Skeleton);
	for (int32 A = 0; A < Wheels.Num(); ++A)
	{
		for (int32 B = A + 1; B < Wheels.Num(); ++B)
		{
			const int32 BoneA = Skeleton.FindRawBoneIndex(Wheels[A].Bone);
			const int32 BoneB = Skeleton.FindRawBoneIndex(Wheels[B].Bone);
			if (Pose.IsValidIndex(BoneA) && Pose.IsValidIndex(BoneB) && FVector::Dist(Pose[BoneA].GetLocation(), Pose[BoneB].GetLocation()) < 1.0)
			{
				Result.Problems.Add(FString::Printf(TEXT("%s has two parts, %s and %s, that are named like wheels and sit in the same place (one is probably a hub or axle). Rename the one that is not the wheel's centre so its name does not end in a corner (_FL and the like), and re-import it."),
					*Name, *Wheels[A].Bone.ToString(), *Wheels[B].Bone.ToString()));
			}
		}
	}

	FMantleGlbMesh Body;
	TArray<UMaterialInterface*> Materials;
	int32 Triangles = 0;
	FString Error;
	if (!FMantleCosmeticExport::ReadSkeletalMesh(Mesh, 0, Body, Materials, Triangles, Error))
	{
		Result.Problems.Add(Error);
	}
	else
	{
		if (Triangles > FMantleVehicleLimits::MaxTriangles)
		{
			Result.Problems.Add(FString::Printf(TEXT("%s has %d triangles; a vehicle body has at most %d. Use a lower-detail version of it."),
				*Name, Triangles, FMantleVehicleLimits::MaxTriangles));
		}
		if (Body.MaterialNames.Num() > FMantleVehicleLimits::MaxSections)
		{
			Result.Problems.Add(FString::Printf(TEXT("%s has %d materials; a vehicle body has at most %d. Merge some of its materials and re-import it, or build it with Build a vehicle on a Mantle chassis, which carries the extra materials as parts for you."),
				*Name, Body.MaterialNames.Num(), FMantleVehicleLimits::MaxSections));
		}
	}
	if (Result.Problems.Num() > 0)
	{
		return Result;
	}

	// What the creator already set for a wheel (its steering, a tire mesh, a size) is kept.
	const TArray<FMantleVehicleSourceWheel> Previous = Source.Wheels;
	const bool bHasBack = Wheels.ContainsByPredicate([](const FMantleVehicleWheelBone& Wheel) { return Wheel.Axle > 0; });
	int32 LastAxle = 0;
	for (const FMantleVehicleWheelBone& Wheel : Wheels)
	{
		LastAxle = FMath::Max(LastAxle, Wheel.Axle);
	}
	TArray<FMantleVehicleSourceWheel> NewWheels;
	for (const FMantleVehicleWheelBone& Wheel : Wheels)
	{
		const FMantleVehicleSourceWheel* Kept = Previous.FindByPredicate([&Wheel](const FMantleVehicleSourceWheel& Entry) { return Entry.Bone == Wheel.Bone; });
		FMantleVehicleSourceWheel Entry;
		if (Kept)
		{
			Entry = *Kept;
		}
		else
		{
			Entry.bSteering = Wheel.Axle == 0;
			Entry.bDriven = Wheel.Axle > 0 || !bHasBack;
			Entry.bHandbrake = LastAxle > 0 && Wheel.Axle == LastAxle;
		}
		Entry.Bone = Wheel.Bone;
		NewWheels.Add(Entry);
	}
	// From here on it converts: the wheels are the source's, so the meshes riding the body can find theirs.
	Source.Body = Mesh;
	Source.Wheels = MoveTemp(NewWheels);

	// A ChaosVehicles movement component's wheel setups: a wheel without a size of its own takes its setup's (the setup naming its
	// bone, else one of its corner), and the tuning's mass and steering angle, still the defaults, take the component's.
	TArray<FString> SetupNotes;
	const FMantleVehicleSourceTuning Defaults;
	float SteerAngle = 0.f;
	for (FMantleVehicleSourceWheel& Entry : Source.Wheels)
	{
		const FString Corner = CornerOf(Entry.Bone.ToString());
		const FMantleVehicleWheelSetup* Setup = Around.WheelSetups.FindByPredicate([&Entry](const FMantleVehicleWheelSetup& Candidate) { return Candidate.Bone == Entry.Bone; });
		Setup = Setup ? Setup : Around.WheelSetups.FindByPredicate([&Corner](const FMantleVehicleWheelSetup& Candidate)
		{
			return !Corner.IsEmpty() && CornerOf(Candidate.Bone.ToString()) == Corner;
		});
		if (Setup && Entry.Radius <= 0.f && Entry.Width <= 0.f && Setup->Radius > 0.f && Setup->Width > 0.f)
		{
			Entry.Radius = Setup->Radius;
			Entry.Width = Setup->Width;
			SetupNotes.Add(FString::Printf(TEXT("%s takes its radius %s cm and width %s cm from the original vehicle's wheel settings."), *Entry.Bone.ToString(),
				*VehicleNumberText(Setup->Radius), *VehicleNumberText(Setup->Width)));
		}
		SteerAngle = FMath::Max(SteerAngle, Setup && Entry.bSteering ? Setup->MaxSteerAngle : 0.f);
	}
	if (Around.Mass > 0.f && Source.Tuning.Mass == Defaults.Mass)
	{
		Source.Tuning.Mass = FMath::Clamp(Around.Mass, 50.f, 20000.f);
		SetupNotes.Add(FString::Printf(TEXT("Mass: %s kg, taken from the original vehicle's settings. Change it under Tuning > Mass in the vehicle asset."), *VehicleNumberText(Source.Tuning.Mass)));
	}
	if (SteerAngle > 0.f && Source.Tuning.MaxSteeringAngle == Defaults.MaxSteeringAngle)
	{
		Source.Tuning.MaxSteeringAngle = FMath::Clamp(SteerAngle, 5.f, 70.f);
		SetupNotes.Add(FString::Printf(TEXT("Steering angle: %s degrees, taken from the original vehicle's wheel settings. Change it under Tuning > Max Steering Angle in the vehicle asset."),
			*VehicleNumberText(Source.Tuning.MaxSteeringAngle)));
	}

	// Attached parts (#435 item 2): the meshes riding the body, as its parts when it has none; the one shaped like a wheel's tire (else
	// the first on its bone) is that wheel's Tire Mesh when it has none. Before the wheels' notes, which say how a wheel with a tire is
	// measured. A tire's shape is checked against the wheel's size: as set, else measured from the body.
	TArray<FString> PartNotes;
	const bool bFreshParts = Source.Parts.IsEmpty();
	if (Around.Attached.Num() > 0)
	{
		TMap<FName, double> Measured;
		for (const FMantleVehicleSourceWheel& Entry : Source.Wheels)
		{
			double Radius = 0.0;
			double Width = 0.0;
			if (MeasureWheel(Body, Entry.Bone, Radius, Width))
			{
				Measured.Add(Entry.Bone, Radius);
			}
		}
		PartNotes.Add(FString::Printf(TEXT("Found %d extra model(s) attached to the body (%s)."), Around.Attached.Num(), *Around.From));
		AddParts(*Mesh, Around.Attached, Source, PartNotes, Measured);
	}
	else if (Source.Parts.Num() > 0)
	{
		PartNotes.Add(FString::Printf(TEXT("Attached parts: your %d are kept."), Source.Parts.Num()));
	}

	for (const FMantleVehicleSourceWheel& Entry : Source.Wheels)
	{
		FMantleVehicleWheelBone Wheel;
		Wheel.Bone = Entry.Bone;
		double Radius = 0.0;
		double Width = 0.0;
		const TCHAR* Role = Entry.bSteering ? (Entry.bDriven ? TEXT("steers and drives") : TEXT("steers"))
			: (Entry.bDriven ? TEXT("drives") : TEXT("rolls"));
		if (Entry.Radius > 0.f && Entry.Width > 0.f)
		{
			Result.Notes.Add(FString::Printf(TEXT("%s %s; radius %s cm and width %s cm as set."), *Wheel.Bone.ToString(), Role,
				*VehicleNumberText(Entry.Radius), *VehicleNumberText(Entry.Width)));
		}
		else if (MeasureWheel(Body, Wheel.Bone, Radius, Width))
		{
			Result.Notes.Add(FString::Printf(TEXT("%s %s; measured radius %s cm, width %s cm."), *Wheel.Bone.ToString(), Role,
				*VehicleNumberText(Radius), *VehicleNumberText(Width)));
		}
		else if (Entry.TireMesh.IsNull())
		{
			Result.Notes.Add(FString::Printf(TEXT("%s %s, but nothing in the model belongs to this wheel alone, so its size cannot be measured: set its Tire Mesh (a separate tire model), or its Radius and Width, in the vehicle asset."),
				*Wheel.Bone.ToString(), Role));
		}
		else
		{
			Result.Notes.Add(FString::Printf(TEXT("%s %s; measured from its tire mesh when published."), *Wheel.Bone.ToString(), Role));
		}
	}
	Result.Notes.Append(SetupNotes);
	Result.Notes.Append(PartNotes);

	// The chassis (#435): its collision boxes and centre of mass from the physics it simulates on, where the tuning sets neither.
	TArray<FName> WheelNames;
	for (const FMantleVehicleWheelBone& Wheel : Wheels)
	{
		WheelNames.Add(Wheel.Bone);
	}
	FMantleVehicleSourceTuning& Tuning = Source.Tuning;
	if (Tuning.Collision.IsEmpty() || !Tuning.bOverrideCenterOfMass)
	{
		TArray<FMantleVehicleSourceBox> Boxes;
		FVector Center = FVector::ZeroVector;
		const bool bPhysics = Around.PhysicsAsset && ReadChassisPhysics(*Around.PhysicsAsset, Skeleton, WheelNames, Boxes, Center, Result.Notes);
		if (bPhysics && Tuning.Collision.IsEmpty())
		{
			Tuning.Collision = Boxes;
		}
		if (!Tuning.bOverrideCenterOfMass && (Around.bCenterOfMassOverride || bPhysics))
		{
			// A movement component's override is in the root body's space; the physics' own centre is in the body's already.
			Tuning.bOverrideCenterOfMass = true;
			Tuning.CenterOfMass = Around.bCenterOfMassOverride && Pose.Num() > 0 ? Pose[0].TransformPosition(Around.CenterOfMassOverride) : Center;
			Result.Notes.Add(FString::Printf(TEXT("Centre of mass: (%s, %s, %s) cm, from %s. Change it under Tuning > Center Of Mass in the vehicle asset."),
				*VehicleNumberText(Tuning.CenterOfMass.X), *VehicleNumberText(Tuning.CenterOfMass.Y), *VehicleNumberText(Tuning.CenterOfMass.Z),
				Around.bCenterOfMassOverride ? TEXT("the original vehicle's settings") : TEXT("the original's collision shape")));
		}
	}
	Result.Notes.Add(Tuning.Collision.IsEmpty()
		? TEXT("The driver sits behind the middle on the left, and the vehicle's collision is one box around the body, unless you set Seat Location or Collision in the vehicle asset.")
		: TEXT("The driver sits behind the middle on the left unless you set Seat Location in the vehicle asset; the vehicle's collision is the boxes under Collision."));

	// #612: first-guess contact points for each placed seat that has none, from the seat, its steering wheel and the collision.
	for (const bool bPassenger : { false, true })
	{
		const bool bPlaced = bPassenger ? Tuning.bPassengerSeat : Tuning.bSeatLocation;
		FMantleVehicleSourceSeatContacts& Contacts = bPassenger ? Tuning.PassengerContacts : Tuning.DriverContacts;
		if (bPlaced && !Contacts.HasAny())
		{
			Contacts = GuessSeatContacts(Tuning, bPassenger);
			Result.Notes.Add(FString::Printf(TEXT("%s contact points: guessed from the seat%s. Move them under Tuning > %s in the vehicle asset."),
				bPassenger ? TEXT("Passenger") : TEXT("Driver"),
				Contacts.Hands == EMantleVehicleHandsKind::Wheel ? TEXT(", the hands on the steering wheel's rim") : TEXT(""),
				bPassenger ? TEXT("Passenger Contacts") : TEXT("Driver Contacts")));
		}
	}

	// Paint and liveries (#435 f, #488): the sections a livery can go on are the painted ones. Suggested on the sections that
	// read as paint (Clear Coat, Mantle's car paint, or a Paint Tint parameter) when none is set yet; the creator's own choice
	// is kept. A part's slots (#435 item 2: the OffroadCar's painted shell is a part) are suggested the same way, and on the
	// painted sections' own materials, when its parts were found just now.
	const TArray<FString> Sections = UniqueSectionNames(Body.MaterialNames);
	if (Source.Paint.Sections.IsEmpty())
	{
		for (int32 Index = 0; Index < Sections.Num(); ++Index)
		{
			if (Materials.IsValidIndex(Index) && LooksLikePaint(Materials[Index]))
			{
				Source.Paint.Sections.Add(Sections[Index]);
			}
		}
	}
	TArray<const UMaterialInterface*> BodyPaintMaterials;
	for (int32 Index = 0; Index < Sections.Num(); ++Index)
	{
		const FString& Section = Sections[Index];
		if (Materials.IsValidIndex(Index) && Materials[Index]
			&& Source.Paint.Sections.ContainsByPredicate([&Section](const FString& Painted) { return Painted.Equals(Section, ESearchCase::CaseSensitive); }))
		{
			BodyPaintMaterials.AddUnique(Materials[Index]);
		}
	}
	TArray<FString> PaintedSlots;
	for (FMantleVehicleSourcePart& Part : Source.Parts)
	{
		if (bFreshParts && Part.PaintSlots.IsEmpty())
		{
			Part.PaintSlots = SuggestPartPaintSlots(Part, BodyPaintMaterials);
		}
		for (const int32 Slot : Part.PaintSlots)
		{
			PaintedSlots.Add(FString::Printf(TEXT("%s slot %d"), *Part.Mesh.GetAssetName(), Slot));
		}
	}
	const bool bPainted = Source.Paint.Sections.Num() > 0 || PaintedSlots.Num() > 0;
	TArray<FString> PaintedList = Source.Paint.Sections;
	PaintedList.Append(PaintedSlots);
	Result.Notes.Add(FString::Printf(TEXT("Its sections: %s. %s"), *FString::Join(Sections, TEXT(", ")), !bPainted
		? TEXT("None is painted: to give it a colour players and scripts can change, and liveries, add sections to Paint > Sections (or a part's slots to its Paint Slots) in the vehicle asset.")
		: *FString::Printf(TEXT("Painted with Mantle's car paint, where liveries go: %s. Add liveries (images, alpha where the paint shows) under Paint > Liveries in the vehicle asset."),
			*FString::Join(PaintedList, TEXT(", ")))));

	// Suspension linkage (#435 b): found from the skeleton unless the creator has rules of their own.
	if (Source.Linkage.IsEmpty())
	{
		Source.Linkage = DetectLinkage(Skeleton, WheelNames, Result.Notes);
	}
	else
	{
		Result.Notes.Add(FString::Printf(TEXT("Suspension linkage: your %d rules are kept. Empty Linkage in the vehicle asset and convert again to have Mantle find them."),
			Source.Linkage.Num()));
	}

	Result.bSuccess = true;
	return Result;
}

UMantleVehicleSource* FMantleVehicleConverter::ConvertAsset(USkeletalMesh* Mesh, FMantleVehicleConversion& OutResult, const FMantleVehicleSurroundings* Around)
{
	OutResult = FMantleVehicleConversion();
	if (!Mesh)
	{
		OutResult.Problems.Add(TEXT("Select the model to use as the vehicle's body (it needs a wheel part at the centre of each wheel), then convert it."));
		return nullptr;
	}
	const FMantleVehicleSurroundings Surroundings = Around ? *Around : SurroundingsOfMesh(*Mesh);
	const FString AssetName = Mesh->GetName() + TEXT("_Vehicle");
	const FString PackageName = FPackageName::GetLongPackagePath(Mesh->GetOutermost()->GetName()) / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;

	UObject* Existing = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
	if (!Existing && FPackageName::DoesPackageExist(PackageName))
	{
		Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (Existing && !Existing->IsA<UMantleVehicleSource>())
	{
		OutResult.Problems.Add(FString::Printf(TEXT("%s already exists and is not a Mantle vehicle. Rename or move it, then convert again."), *PackageName));
		return nullptr;
	}

	if (UMantleVehicleSource* Source = Cast<UMantleVehicleSource>(Existing))
	{
		// Updated in place: its tuning, and whatever the creator set on a wheel that is still there, are kept.
		UMantleVehicleSource* Scratch = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Scratch->Wheels = Source->Wheels;
		Scratch->Tuning = Source->Tuning;
		Scratch->Paint.Sections = Source->Paint.Sections;
		Scratch->Linkage = Source->Linkage;
		Scratch->Parts = Source->Parts;
		OutResult = Convert(Mesh, *Scratch, Surroundings);
		if (!OutResult.bSuccess)
		{
			return nullptr;
		}
		Source->Modify();
		Source->Body = Scratch->Body;
		Source->Wheels = Scratch->Wheels;
		// Its tuning is the creator's; only a collision and centre of mass read from the physics, where it set neither, are new.
		Source->Tuning = Scratch->Tuning;
		// Its paint's finish and liveries are the creator's; only a suggestion for sections, when it had none, is new.
		Source->Paint.Sections = Scratch->Paint.Sections;
		// So is its linkage, found afresh only when it had none (#435 b), and its parts (#435 item 2).
		Source->Linkage = Scratch->Linkage;
		Source->Parts = Scratch->Parts;
		Source->MarkPackageDirty();
		return Source;
	}

	UMantleVehicleSource* Scratch = NewObject<UMantleVehicleSource>(GetTransientPackage());
	OutResult = Convert(Mesh, *Scratch, Surroundings);
	if (!OutResult.bSuccess)
	{
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackageName);
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	Source->Body = Scratch->Body;
	Source->Wheels = Scratch->Wheels;
	Source->Tuning = Scratch->Tuning;
	Source->Paint = Scratch->Paint;
	Source->Linkage = Scratch->Linkage;
	Source->Parts = Scratch->Parts;
	FAssetRegistryModule::AssetCreated(Source);
	Package->MarkPackageDirty();
	return Source;
}

FMantleVehicleSurroundings FMantleVehicleConverter::SurroundingsOfMesh(const USkeletalMesh& Mesh)
{
	FMantleVehicleSurroundings Out;
	Out.From = FString::Printf(TEXT("%s's preview attachments"), *Mesh.GetName());
	Out.PhysicsAsset = Mesh.GetPhysicsAsset();
#if WITH_EDITORONLY_DATA
	// Persona's Add Preview Asset: a mesh on a socket or bone of the mesh, or of its skeleton (shared by its meshes).
	auto AddFrom = [&Out](const FPreviewAssetAttachContainer& Container)
	{
		for (int32 Index = 0; Index < Container.Num(); ++Index)
		{
			const FPreviewAttachedObjectPair& Pair = Container[Index];
			const UStaticMesh* Static = Cast<UStaticMesh>(Pair.GetAttachedObject());
			const FName AttachTo = Pair.AttachedTo;
			if (!Static || Out.Attached.ContainsByPredicate([Static, AttachTo](const FMantleVehicleAttachedMesh& Seen)
				{
					return Seen.Mesh == Static && Seen.AttachTo == AttachTo;
				}))
			{
				continue;
			}
			FMantleVehicleAttachedMesh& Found = Out.Attached.AddDefaulted_GetRef();
			Found.Mesh = Static;
			Found.AttachTo = AttachTo;
			Found.Label = FString::Printf(TEXT("preview attachment on %s"), *AttachTo.ToString());
		}
	};
	AddFrom(Mesh.GetPreviewAttachedAssetContainer());
	if (const USkeleton* Skeleton = Mesh.GetSkeleton())
	{
		AddFrom(Skeleton->PreviewAttachedAssetContainer);
	}
#endif
	return Out;
}

FMantleVehicleSurroundings FMantleVehicleConverter::SurroundingsOfComponent(const USkeletalMeshComponent& Body, const TConstArrayView<const UActorComponent*> Components)
{
	FMantleVehicleSurroundings Out;
	const AActor* Owner = Body.GetOwner();
	Out.From = Owner ? FString::Printf(TEXT("actor %s"), *Owner->GetActorNameOrLabel()) : Body.GetName();
	Out.PhysicsAsset = Body.GetPhysicsAsset();
	for (const UActorComponent* Component : Components)
	{
		if (!Component || Component == &Body)
		{
			continue;
		}
		// A ChaosVehicles movement component's centre-of-mass override, mass and wheel setups (each a wheel class and the bone it
		// names), by their property names: MantleCore takes no plugin dependency.
		const FBoolProperty* Enable = FindFProperty<FBoolProperty>(Component->GetClass(), TEXT("bEnableCenterOfMassOverride"));
		const FStructProperty* Center = FindFProperty<FStructProperty>(Component->GetClass(), TEXT("CenterOfMassOverride"));
		const FArrayProperty* Setups = FindFProperty<FArrayProperty>(Component->GetClass(), TEXT("WheelSetups"));
		const FStructProperty* Setup = Setups ? CastField<FStructProperty>(Setups->Inner) : nullptr;
		if (Setup)
		{
			Out.Mass = static_cast<float>(MantleVehicleConverterPrivate::VehicleNumberProperty(Component->GetClass(), Component, TEXT("Mass")));
			const FObjectPropertyBase* WheelClass = FindFProperty<FObjectPropertyBase>(Setup->Struct, TEXT("WheelClass"));
			const FNameProperty* BoneName = FindFProperty<FNameProperty>(Setup->Struct, TEXT("BoneName"));
			FScriptArrayHelper Entries(Setups, Setups->ContainerPtrToValuePtr<void>(Component));
			for (int32 Index = 0; Index < Entries.Num() && BoneName; ++Index)
			{
				const uint8* Entry = Entries.GetRawPtr(Index);
				const UClass* Class = WheelClass ? Cast<UClass>(WheelClass->GetObjectPropertyValue(WheelClass->ContainerPtrToValuePtr<void>(Entry))) : nullptr;
				const UObject* Wheel = Class ? Class->GetDefaultObject() : nullptr;
				FMantleVehicleWheelSetup& Found = Out.WheelSetups.AddDefaulted_GetRef();
				Found.Bone = BoneName->GetPropertyValue_InContainer(Entry);
				if (Wheel)
				{
					const FBoolProperty* Steers = FindFProperty<FBoolProperty>(Class, TEXT("bAffectedBySteering"));
					Found.Radius = static_cast<float>(MantleVehicleConverterPrivate::VehicleNumberProperty(Class, Wheel, TEXT("WheelRadius")));
					Found.Width = static_cast<float>(MantleVehicleConverterPrivate::VehicleNumberProperty(Class, Wheel, TEXT("WheelWidth")));
					Found.MaxSteerAngle = Steers && Steers->GetPropertyValue_InContainer(Wheel)
						? static_cast<float>(MantleVehicleConverterPrivate::VehicleNumberProperty(Class, Wheel, TEXT("MaxSteerAngle"))) : 0.f;
				}
			}
		}
		if (Enable && Center && Center->Struct == TBaseStructure<FVector>::Get() && Enable->GetPropertyValue_InContainer(Component))
		{
			Out.bCenterOfMassOverride = true;
			Out.CenterOfMassOverride = *Center->ContainerPtrToValuePtr<FVector>(Component);
			continue;
		}
		if (Setup)
		{
			continue;
		}
		const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component);
		// Instanced meshes are scenery, and a mesh hidden in game is never seen.
		if (!Static || !Static->GetStaticMesh() || Static->IsA<UInstancedStaticMeshComponent>() || Static->bHiddenInGame)
		{
			continue;
		}
		// Up the attachment to the body: each parent's socket offset (a static mesh's socket) and relative transform folded in.
		FTransform Relative = Static->GetRelativeTransform();
		const USceneComponent* Parent = Static->GetAttachParent();
		FName Socket = Static->GetAttachSocketName();
		for (int32 Depth = 0; Parent && Parent != &Body && Depth < 64; ++Depth)
		{
			const UStaticMeshComponent* ParentMesh = Cast<UStaticMeshComponent>(Parent);
			const UStaticMeshSocket* ParentSocket = ParentMesh && ParentMesh->GetStaticMesh() && !Socket.IsNone() ? ParentMesh->GetStaticMesh()->FindSocket(Socket) : nullptr;
			if (ParentSocket)
			{
				Relative = Relative * FTransform(ParentSocket->RelativeRotation, ParentSocket->RelativeLocation, ParentSocket->RelativeScale);
			}
			Relative = Relative * Parent->GetRelativeTransform();
			Socket = Parent->GetAttachSocketName();
			Parent = Parent->GetAttachParent();
		}
		if (Parent != &Body)
		{
			continue;
		}
		FMantleVehicleAttachedMesh& Found = Out.Attached.AddDefaulted_GetRef();
		Found.Mesh = Static->GetStaticMesh();
		Found.AttachTo = Socket;
		Found.Relative = Relative;
		for (UMaterialInterface* Override : Static->OverrideMaterials)
		{
			Found.Overrides.Add(Override);
		}
		Found.Label = Static->GetName();
	}
	return Out;
}

bool FMantleVehicleConverter::ReadChassisPhysics(const UPhysicsAsset& Physics, const FReferenceSkeleton& Skeleton, const TConstArrayView<FName> WheelBones,
	TArray<FMantleVehicleSourceBox>& OutBoxes, FVector& OutCenterOfMass, TArray<FString>& OutNotes)
{
	using namespace MantleVehicleConverterPrivate;
	OutBoxes.Reset();
	OutCenterOfMass = FVector::ZeroVector;
	const TArray<FMeshBoneInfo>& Info = Skeleton.GetRawRefBoneInfo();
	const TArray<FTransform> Pose = VehicleComponentPose(Skeleton);
	TArray<int32> Wheels;
	for (const FName Wheel : WheelBones)
	{
		const int32 Bone = Skeleton.FindRawBoneIndex(Wheel);
		if (Bone != INDEX_NONE)
		{
			Wheels.Add(Bone);
		}
	}

	// Every shape of every chassis body (none on a wheel's bone or under one: a ChaosVehicles car's wheel bodies only mark
	// where its wheels are), as an axis-aligned box in component space, with its volume for the centre of mass.
	TArray<FBox> Shapes;
	FVector Weighted = FVector::ZeroVector;
	double Volume = 0.0;
	bool bWidened = false;
	int32 Bodies = 0;
	int32 NudgeBone = INDEX_NONE;
	FVector Nudge = FVector::ZeroVector;
	for (const USkeletalBodySetup* Body : Physics.SkeletalBodySetups)
	{
		const int32 Bone = Body ? Skeleton.FindRawBoneIndex(Body->BoneName) : INDEX_NONE;
		if (Bone == INDEX_NONE || Wheels.ContainsByPredicate([&Info, Bone](const int32 Wheel) { return Wheel == Bone || VehicleHangsFrom(Info, Bone, Wheel); }))
		{
			continue;
		}
		++Bodies;
		const FTransform& BoneTM = Pose[Bone];
		const bool bBoneTurned = !BoneTM.GetRotation().IsIdentity(0.0001);
		auto Add = [&Shapes, &Weighted, &Volume, &bWidened](const FBox& Box, double ElementVolume, bool bTurned)
		{
			if (!Box.IsValid)
			{
				return;
			}
			const double Weight = ElementVolume > 0.0 ? ElementVolume : Box.GetVolume();
			Shapes.Add(Box);
			Weighted += Box.GetCenter() * Weight;
			Volume += Weight;
			bWidened = bWidened || bTurned;
		};
		const FKAggregateGeom& Geom = Body->AggGeom;
		for (const FKBoxElem& Element : Geom.BoxElems)
		{
			Add(Element.CalcAABB(BoneTM, 1.f), Element.GetScaledVolume(FVector::OneVector), bBoneTurned || !Element.Rotation.IsNearlyZero(0.01));
		}
		for (const FKSphereElem& Element : Geom.SphereElems)
		{
			Add(Element.CalcAABB(BoneTM, 1.f), Element.GetScaledVolume(FVector::OneVector), false);
		}
		for (const FKSphylElem& Element : Geom.SphylElems)
		{
			Add(Element.CalcAABB(BoneTM, 1.f), Element.GetScaledVolume(FVector::OneVector), true);
		}
		for (const FKTaperedCapsuleElem& Element : Geom.TaperedCapsuleElems)
		{
			Add(Element.CalcAABB(BoneTM, 1.f), 0.0, true);
		}
		for (const FKConvexElem& Element : Geom.ConvexElems)
		{
			// A convex hull's volume is its bounds' here: close enough for where its weight sits.
			Add(Element.CalcAABB(BoneTM, FVector::OneVector), 0.0, true);
		}
		// The root-most body is the chassis Mantle simulates: its COM nudge moves the centre.
		if (NudgeBone == INDEX_NONE || Bone < NudgeBone)
		{
			NudgeBone = Bone;
			Nudge = BoneTM.TransformVector(Body->DefaultInstance.COMNudge);
		}
	}
	if (Shapes.IsEmpty())
	{
		OutNotes.Add(FString::Printf(TEXT("Collision: the model's collision shape (%s) has nothing for the body, only for the wheels, so the vehicle's collision is one box around the body."),
			*Physics.GetName()));
		return false;
	}

	TArray<FBox> Boxes = Shapes;
	if (Boxes.Num() > FMantleVehicleLimits::MaxCollisionBoxes)
	{
		FBox All(ForceInit);
		for (const FBox& Box : Shapes)
		{
			All += Box;
		}
		Boxes = { All };
		OutNotes.Add(FString::Printf(TEXT("Collision: the model's collision shape (%s) has %d pieces for the body, more than the %d boxes a vehicle may have, so it is one box around them all. Set Collision in the vehicle asset for a closer fit."),
			*Physics.GetName(), Shapes.Num(), FMantleVehicleLimits::MaxCollisionBoxes));
	}
	auto Rounded = [](const FVector& Value) { return FVector(FMath::RoundToDouble(Value.X * 100.0) / 100.0, FMath::RoundToDouble(Value.Y * 100.0) / 100.0,
		FMath::RoundToDouble(Value.Z * 100.0) / 100.0); };
	for (const FBox& Box : Boxes)
	{
		FMantleVehicleSourceBox& Out = OutBoxes.AddDefaulted_GetRef();
		Out.Center = Rounded(Box.GetCenter());
		Out.Extent = Rounded(Box.GetExtent()).ComponentMax(FVector(1.0));
		double CenterMin = 0.0;
		double CenterMax = 0.0;
		double ExtentMin = 0.0;
		double ExtentMax = 0.0;
		FMantleVehicleLimits::GetRange(TEXT("collision/*/center/*"), CenterMin, CenterMax);
		FMantleVehicleLimits::GetRange(TEXT("collision/*/extent/*"), ExtentMin, ExtentMax);
		if (Out.Center.GetMin() < CenterMin || Out.Center.GetMax() > CenterMax || Out.Extent.GetMax() > ExtentMax)
		{
			OutNotes.Add(FString::Printf(TEXT("Collision: the model's collision shape (%s) reaches farther than a vehicle's boxes may (%s cm from the model's origin at most), so the vehicle's collision is one box around the body. Check the model is in centimetres, or set Collision in the vehicle asset."),
				*Physics.GetName(), *VehicleNumberText(CenterMax)));
			OutBoxes.Reset();
			return false;
		}
	}
	OutCenterOfMass = Rounded((Volume > 0.0 ? Weighted / Volume : Boxes[0].GetCenter()) + Nudge);
	OutNotes.Add(FString::Printf(TEXT("Collision: %d box(es) taken from the model's collision shape (%s) (%d body part(s), %d piece(s))%s. Edit them under Tuning > Collision in the vehicle asset."),
		OutBoxes.Num(), *Physics.GetName(), Bodies, Shapes.Num(),
		bWidened ? TEXT("; a turned or round shape is widened to the box around it") : TEXT("")));
	return true;
}

void FMantleVehicleConverter::AddParts(const USkeletalMesh& Mesh, const TConstArrayView<FMantleVehicleAttachedMesh> Attached, UMantleVehicleSource& Source,
	TArray<FString>& OutNotes, const TMap<FName, double>& MeasuredRadii)
{
	using namespace MantleVehicleConverterPrivate;
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	const TArray<FMeshBoneInfo>& Info = Skeleton.GetRawRefBoneInfo();
	if (Info.IsEmpty())
	{
		return;
	}
	const TArray<FTransform> Pose = VehicleComponentPose(Skeleton);
	const bool bAddParts = Source.Parts.IsEmpty();
	TArray<FMantleVehicleSourcePart> Parts;
	TArray<FString> Tires;
	TArray<FString> Skipped;
	TArray<FString> Unspun;

	// Each wheel's bone, corner and known radius (as set, else measured from the body), and its hub: the bone of its corner nearest
	// it that DetectLinkage makes steer and travel with it without spinning (the named wheel under a steering pivot, or a hub or a
	// wheel's helper beside it). The root when it has none.
	struct FWheelAt
	{
		int32 Bone = INDEX_NONE;
		FString Corner;
		double Radius = 0.0;
		int32 Hub = 0;
	};
	TArray<FWheelAt> WheelsAt;
	for (const FMantleVehicleSourceWheel& Wheel : Source.Wheels)
	{
		FWheelAt& At = WheelsAt.AddDefaulted_GetRef();
		At.Bone = Skeleton.FindRawBoneIndex(Wheel.Bone);
		At.Corner = CornerOf(Wheel.Bone.ToString());
		const double* Measured = MeasuredRadii.Find(Wheel.Bone);
		At.Radius = Wheel.Radius > 0.f ? static_cast<double>(Wheel.Radius) : (Measured ? *Measured : 0.0);
		double Nearest = 0.0;
		for (int32 Bone = 1; At.Bone > 0 && !At.Corner.IsEmpty() && Bone < Info.Num(); ++Bone)
		{
			const FString Name = Info[Bone].Name.ToString();
			FMantleVehicleWheelBone Named;
			const bool bUnderPivot = Info[Bone].ParentIndex == At.Bone && VehicleSteeringPivot(Wheel.Bone.ToString()) && ParseWheelBoneName(Name, Named);
			const bool bBeside = !VehicleHangsFrom(Info, At.Bone, Bone) && !VehicleHangsFrom(Info, Bone, At.Bone)
				&& (VehicleNameHas(Info[Bone].Name, MakeArrayView(VehicleHubWords)) || VehicleNamedHelper(Info[Bone].Name));
			const double Distance = FVector::Dist(Pose[Bone].GetLocation(), Pose[At.Bone].GetLocation());
			if (Bone != At.Bone && CornerOf(Name) == At.Corner && (bUnderPivot || bBeside) && (At.Hub == 0 || Distance < Nearest))
			{
				At.Hub = Bone;
				Nearest = Distance;
			}
		}
	}

	// Every mesh where it hangs, in component space; which wheel's tire it is shaped like, if any.
	struct FPlaced
	{
		const FMantleVehicleAttachedMesh* Found = nullptr;
		int32 Bone = 0;
		FTransform Placement;
		int32 Round = INDEX_NONE;
		double Across = 0.0;
		bool bDone = false;
	};
	TArray<FPlaced> Placed;
	for (const FMantleVehicleAttachedMesh& Found : Attached)
	{
		if (!Found.Mesh)
		{
			continue;
		}
		// The point it hangs from, in component space: a socket (its bone and offset), a bone, or the body itself.
		int32 Bone = 0;
		FTransform Point = FTransform::Identity;
		if (const USkeletalMeshSocket* Socket = Found.AttachTo.IsNone() ? nullptr : Mesh.FindSocket(Found.AttachTo))
		{
			Bone = Skeleton.FindRawBoneIndex(Socket->BoneName);
			if (Bone != INDEX_NONE)
			{
				Point = Socket->GetSocketLocalTransform() * Pose[Bone];
			}
		}
		else if (!Found.AttachTo.IsNone())
		{
			Bone = Skeleton.FindRawBoneIndex(Found.AttachTo);
			if (Bone != INDEX_NONE)
			{
				Point = Pose[Bone];
			}
		}
		if (Bone == INDEX_NONE)
		{
			Skipped.Add(FString::Printf(TEXT("%s (%s, on %s, which is not a socket or bone of %s)"), *Found.Mesh->GetName(), *Found.Label,
				*Found.AttachTo.ToString(), *Mesh.GetName()));
			continue;
		}
		FPlaced& Entry = Placed.AddDefaulted_GetRef();
		Entry.Found = &Found;
		Entry.Bone = Bone;
		Entry.Placement = Found.Relative * Point;
		// Shaped and placed like a tire of a wheel it could belong to: hung from the root, the wheel's bone or a bone of its corner
		// (a steering pivot, a hub, or a tire modelled in car space and pinned to one).
		const FString Corner = CornerOf(Info[Bone].Name.ToString());
		const FBox Bounds = Found.Mesh->GetBoundingBox().TransformBy(Entry.Placement);
		for (int32 Index = 0; Index < WheelsAt.Num() && Entry.Round == INDEX_NONE; ++Index)
		{
			const FWheelAt& At = WheelsAt[Index];
			if (At.Bone != INDEX_NONE && (Bone == 0 || Bone == At.Bone || (!Corner.IsEmpty() && Corner == At.Corner)))
			{
				Entry.Across = VehicleTireFit(Bounds, Pose[At.Bone].GetLocation(), At.Radius);
				Entry.Round = Entry.Across > 0.0 ? Index : INDEX_NONE;
			}
		}
	}

	// On a wheel as the rule before shapes had it: on its bone, or on a bone of its corner at its place (the Vehicle template's VisWheel_FL).
	auto OnWheel = [&Info, &Pose](const FPlaced& Entry, const FWheelAt& At)
	{
		const FString Name = Info[Entry.Bone].Name.ToString();
		return Entry.Bone == At.Bone || (!At.Corner.IsEmpty() && CornerOf(Name) == At.Corner && !VehicleWheelHelper(Name)
			&& FVector::Dist(Pose[Entry.Bone].GetLocation(), Pose[At.Bone].GetLocation()) < VehicleSamePlaceCm);
	};
	for (int32 Index = 0; Index < WheelsAt.Num(); ++Index)
	{
		FMantleVehicleSourceWheel& Wheel = Source.Wheels[Index];
		const FWheelAt& At = WheelsAt[Index];
		if (At.Bone == INDEX_NONE)
		{
			continue;
		}
		// A tire the wheel has already (the creator's) is not a part as well.
		if (!Wheel.TireMesh.IsNull())
		{
			for (FPlaced& Entry : Placed)
			{
				Entry.bDone = Entry.bDone || ((Entry.Round == Index || OnWheel(Entry, At)) && Wheel.TireMesh.ToSoftObjectPath() == FSoftObjectPath(Entry.Found->Mesh));
			}
			continue;
		}
		// The widest mesh shaped like its tire, not the first one on its bone (a brake caliper can be); failing any, the first on it.
		FPlaced* Tire = nullptr;
		for (FPlaced& Entry : Placed)
		{
			if (!Entry.bDone && Entry.Round == Index && (!Tire || Entry.Across > Tire->Across))
			{
				Tire = &Entry;
			}
		}
		for (int32 Scan = 0; !Tire && Scan < Placed.Num(); ++Scan)
		{
			Tire = !Placed[Scan].bDone && Placed[Scan].Round == INDEX_NONE && OnWheel(Placed[Scan], At) ? &Placed[Scan] : nullptr;
		}
		if (Tire)
		{
			Tire->bDone = true;
			Wheel.TireMesh = const_cast<UStaticMesh*>(Tire->Found->Mesh);
			Wheel.TireTransform = Tire->Placement.GetRelativeTransform(Pose[At.Bone]);
			Tires.Add(FString::Printf(TEXT("%s is %s's Tire Mesh"), *Tire->Found->Mesh->GetName(), *Wheel.Bone.ToString()));
		}
	}

	for (const FPlaced& Entry : Placed)
	{
		if (Entry.bDone || !bAddParts)
		{
			continue;
		}
		// Shaped like a wheel's tire (a rim, a brake disc): spun with the wheel. Anything else on a wheel's bone (a brake caliper) must
		// not spin: it rides the wheel's hub, which steers with it, or the root.
		int32 Bone = Entry.Bone;
		const int32 OnWheelBone = WheelsAt.IndexOfByPredicate([&Entry](const FWheelAt& At) { return At.Bone == Entry.Bone; });
		if (Entry.Round != INDEX_NONE)
		{
			Bone = WheelsAt[Entry.Round].Bone;
		}
		else if (OnWheelBone != INDEX_NONE)
		{
			Bone = WheelsAt[OnWheelBone].Hub;
			Unspun.Add(FString::Printf(TEXT("%s onto %s"), *Entry.Found->Mesh->GetName(), *Info[Bone].Name.ToString()));
		}
		FMantleVehicleSourcePart& Part = Parts.AddDefaulted_GetRef();
		Part.Mesh = const_cast<UStaticMesh*>(Entry.Found->Mesh);
		Part.Bone = Info[Bone].Name;
		Part.Transform = Entry.Placement.GetRelativeTransform(Pose[Bone]);
		for (UMaterialInterface* Override : Entry.Found->Overrides)
		{
			Part.Materials.Add(Override);
		}
		while (Part.Materials.Num() > 0 && Part.Materials.Last().IsNull())
		{
			Part.Materials.Pop();
		}
		Part.FoundOn = Entry.Found->Label;
	}
	if (Unspun.Num() > 0)
	{
		OutNotes.Add(FString::Printf(TEXT("Not shaped like a tire, so it will not spin with the wheel: %s (a hub turns with its wheel's steering without spinning; the body does neither)."),
			*FString::Join(Unspun, TEXT("; "))));
	}

	if (Tires.Num() > 0)
	{
		OutNotes.Add(FString::Printf(TEXT("Tires: %s (measured from it when published, and spun with the wheel)."), *FString::Join(Tires, TEXT("; "))));
	}
	if (!bAddParts)
	{
		OutNotes.Add(FString::Printf(TEXT("Attached parts: your %d are kept. Empty Parts in the vehicle asset and convert again to have Mantle find them."),
			Source.Parts.Num()));
	}
	else if (Parts.Num() > 0)
	{
		TArray<FString> Names;
		for (const FMantleVehicleSourcePart& Part : Parts)
		{
			Names.Add(FString::Printf(TEXT("%s on %s"), *Part.Mesh.GetAssetName(), *Part.Bone.ToString()));
		}
		OutNotes.Add(FString::Printf(TEXT("Attached parts: %d (%s). Each publishes as its own model with its materials, attached to the wheel or body part it was found on and moving with it; a part does not collide (the chassis is the collision boxes). Edit them under Parts in the vehicle asset."),
			Parts.Num(), *FString::Join(Names, TEXT(", "))));
		if (Parts.Num() > FMantleVehicleLimits::MaxParts)
		{
			OutNotes.Add(FString::Printf(TEXT("That is more than the %d parts a vehicle may have: remove some under Parts before publishing."), FMantleVehicleLimits::MaxParts));
		}
		Source.Parts = MoveTemp(Parts);
	}
	if (Skipped.Num() > 0)
	{
		OutNotes.Add(FString::Printf(TEXT("Left out: %s. They are not attached to a part of the body that can carry them: attach them to one of the body's sockets or bones (its attachment points) to include them."), *FString::Join(Skipped, TEXT("; "))));
	}
}

UMaterialInterface* FMantleVehicleConverter::PartSlotMaterial(const FMantleVehicleSourcePart& Part, const UStaticMesh* Mesh, const int32 Slot)
{
	if (Part.Materials.IsValidIndex(Slot) && !Part.Materials[Slot].IsNull())
	{
		if (UMaterialInterface* Override = Part.Materials[Slot].LoadSynchronous())
		{
			return Override;
		}
	}
	return FMantlePlaceAssets::MeshSlotMaterial(Mesh, Slot);
}

bool FMantleVehicleConverter::LooksLikePaint(const UMaterialInterface* Material)
{
	if (!Material)
	{
		return false;
	}
	if (FMantlePlaceAssets::PublishesAsCarPaint(Material))
	{
		return true;
	}
	// The built-in cars' paint contract (UMantleVehicleDefinition::PaintParameter's default): the slot a car is recoloured on.
	FLinearColor Tint;
	return Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(TEXT("Paint Tint"))), Tint);
}

TArray<int32> FMantleVehicleConverter::SuggestPartPaintSlots(const FMantleVehicleSourcePart& Part, const TConstArrayView<const UMaterialInterface*> BodyPaintMaterials)
{
	TArray<int32> Out;
	const UStaticMesh* PartMesh = Part.Mesh.LoadSynchronous();
	if (!PartMesh)
	{
		return Out;
	}
	for (const int32 Slot : FMantlePlaceAssets::UploadSlots(PartMesh))
	{
		const UMaterialInterface* Material = PartSlotMaterial(Part, PartMesh, Slot);
		if (LooksLikePaint(Material) || BodyPaintMaterials.Contains(Material))
		{
			Out.Add(Slot);
		}
	}
	return Out;
}

TArray<int32> FMantleVehicleConverter::PartUploadPaintSlots(const FMantleVehicleSourcePart& Part, const UStaticMesh* Mesh)
{
	TArray<int32> Out;
	const TArray<int32> Slots = FMantlePlaceAssets::UploadSlots(Mesh);
	for (const int32 Slot : Part.PaintSlots)
	{
		const int32 Uploaded = Slots.IndexOfByKey(Slot);
		if (Uploaded != INDEX_NONE)
		{
			Out.AddUnique(Uploaded);
		}
	}
	return Out;
}

namespace MantleVehicleConverterPrivate
{
	/** A source's Size as the export applies it (#728): its FitScale, or 1 when that is not a size at all (zero or negative). */
	double VehicleExportScale(const UMantleVehicleSource& Source)
	{
		return Source.FitScale > 0.f ? static_cast<double>(Source.FitScale) : 1.0;
	}
}

bool FMantleVehicleConverter::ReadBody(const UMantleVehicleSource& Source, FMantleGlbMesh& OutBody, TArray<UMaterialInterface*>& OutMaterials, FString& OutError,
	const bool bApplyFitScale)
{
	using namespace MantleVehicleConverterPrivate;
	OutBody = FMantleGlbMesh();
	OutMaterials.Reset();
	USkeletalMesh* Mesh = Source.Body.LoadSynchronous();
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("%s has no body. Set its Body to the vehicle's skeletal mesh (or run Convert Vehicle on the mesh)."), *Source.GetName());
		return false;
	}
	int32 Triangles = 0;
	if (!FMantleCosmeticExport::ReadSkeletalMesh(Mesh, 0, OutBody, OutMaterials, Triangles, OutError))
	{
		return false;
	}
	VehicleCapInfluences(OutBody, FMantleVehicleLimits::MaxInfluences);
	OutBody.Name = Source.GetName();

	const TArray<FTransform> Pose = VehicleJointPose(OutBody);
	for (const FMantleVehicleSourceWheel& Wheel : Source.Wheels)
	{
		if (Wheel.TireMesh.IsNull())
		{
			continue;
		}
		const UStaticMesh* Tire = Wheel.TireMesh.LoadSynchronous();
		const int32 Joint = FMantleVehicleConverter::JointIndex(OutBody, Wheel.Bone);
		if (!Tire)
		{
			OutError = FString::Printf(TEXT("%s's wheel %s names a tire mesh that cannot be loaded (%s). Set its Tire Mesh again, or clear it."),
				*Source.GetName(), *Wheel.Bone.ToString(), *Wheel.TireMesh.ToString());
			return false;
		}
		if (Joint == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("%s's wheel %s is not a bone of %s. Run Convert Vehicle on the mesh again, or fix the wheel's Bone."),
				*Source.GetName(), *Wheel.Bone.ToString(), *Mesh->GetName());
			return false;
		}
		FString TireError;
		if (!VehicleMergeTire(OutBody, OutMaterials, Tire, Joint, Wheel.TireTransform * Pose[Joint], TireError))
		{
			OutError = FString::Printf(TEXT("%s's tire mesh for %s: %s"), *Source.GetName(), *Wheel.Bone.ToString(), *TireError);
			return false;
		}
	}

	// The vehicle's Size (#728): the whole body, tires merged on, grows about the origin, so the cook measures the scaled wheels. Only
	// lengths scale: positions and each bone's rest translation (a child's offset from its parent), never normals, tangents or weights.
	const double FitScale = VehicleExportScale(Source);
	if (bApplyFitScale && FitScale != 1.0)
	{
		const float Scale = static_cast<float>(FitScale);
		for (FVector3f& Position : OutBody.Positions)
		{
			Position *= Scale;
		}
		for (FMantleGlbJoint& Joint : OutBody.Joints)
		{
			Joint.Local.SetTranslation(Joint.Local.GetTranslation() * FitScale);
		}
	}
	return true;
}

FMantleVehicleSourceSeatContacts FMantleVehicleConverter::GuessSeatContacts(const FMantleVehicleSourceTuning& Tuning, const bool bPassenger)
{
	using namespace MantleVehicleConverterPrivate;
	double Min = -1000.0;
	double Max = 1000.0;
	FMantleVehicleLimits::GetRange(TEXT("seat/location/*"), Min, Max);
	const auto Held = [Min, Max](const FVector& At)
	{
		return FVector(FMath::Clamp(At.X, Min, Max), FMath::Clamp(At.Y, Min, Max), FMath::Clamp(At.Z, Min, Max));
	};
	const FVector Seat = bPassenger ? Tuning.PassengerSeatLocation : Tuning.SeatLocation;
	// The seat's own side: the side of the centre line it sits on, else the driver's left (-Y) and the passenger's right.
	const double Side = Seat.Y < 0.0 ? -1.0 : (Seat.Y > 0.0 ? 1.0 : (bPassenger ? 1.0 : -1.0));

	FMantleVehicleSourceSeatContacts Out;
	Out.bPelvis = true;
	Out.Pelvis.Location = Held(Seat + VehicleGuessPelvis);
	if (!bPassenger && Tuning.bSteeringWheel)
	{
		// Ten and two on the rim, in the hub's frame (X along the column, Y to the right, Z to the top of the rim), turned as the hub.
		Out.Hands = EMantleVehicleHandsKind::Wheel;
		const FTransform Hub(Tuning.SteeringWheelRotation, Tuning.SteeringWheelLocation);
		const double Angle = FMath::DegreesToRadians(VehicleGuessRimDegrees);
		const double Radius = Tuning.SteeringWheelRadius;
		Out.LeftHand.Location = Held(Hub.TransformPosition(FVector(0.0, -FMath::Sin(Angle) * Radius, FMath::Cos(Angle) * Radius)));
		Out.RightHand.Location = Held(Hub.TransformPosition(FVector(0.0, FMath::Sin(Angle) * Radius, FMath::Cos(Angle) * Radius)));
		Out.LeftHand.Rotation = Tuning.SteeringWheelRotation.GetNormalized();
		Out.RightHand.Rotation = Out.LeftHand.Rotation;
	}
	else
	{
		Out.Hands = EMantleVehicleHandsKind::Fixed;
		Out.LeftHand.Location = Held(Out.Pelvis.Location + FVector(VehicleGuessLap.X, -VehicleGuessLap.Y, VehicleGuessLap.Z));
		Out.RightHand.Location = Held(Out.Pelvis.Location + VehicleGuessLap);
	}
	Out.bFeet = true;
	Out.LeftFoot.Location = Held(Seat + FVector(VehicleGuessFoot.X, -VehicleGuessFoot.Y, VehicleGuessFoot.Z));
	Out.RightFoot.Location = Held(Seat + VehicleGuessFoot);

	// An exit on each side, the seat's own first, as far out as the client's exit spot (past the collision's side), on the
	// collision's floor (the runtime settles it on the ground it finds).
	double HalfWidth = 0.0;
	double Floor = Seat.Z + VehicleGuessFoot.Z;
	for (int32 Index = 0; Index < Tuning.Collision.Num(); ++Index)
	{
		const FMantleVehicleSourceBox& Box = Tuning.Collision[Index];
		HalfWidth = FMath::Max(HalfWidth, FMath::Abs(Box.Center.Y) + Box.Extent.Y);
		Floor = Index == 0 ? Box.Center.Z - Box.Extent.Z : FMath::Min(Floor, Box.Center.Z - Box.Extent.Z);
	}
	const double Reach = FMath::Max(VehicleGuessExitMin, HalfWidth + VehicleGuessExitClear);
	for (const double ExitSide : { Side, -Side })
	{
		FMantleVehicleSourceExit& Exit = Out.Exits.AddDefaulted_GetRef();
		Exit.Location = Held(FVector(Seat.X, ExitSide * Reach, Floor));
	}
	return Out;
}

const UMantleVehicleSource* FMantleVehicleConverter::MakeExportView(const UMantleVehicleSource& Source)
{
	using namespace MantleVehicleConverterPrivate;
	const double Scale = VehicleExportScale(Source);
	if (Scale == 1.0)
	{
		return &Source;
	}
	// Tuning lengths stay as they are on purpose: suspension travel and the chase camera come from the preset, which is tuned
	// for a vehicle already sized to fit the Mantle character, the size the builder proposes (#728); a creator who sizes it
	// far from that adjusts them in its settings.
	// A transient copy under an automatic name (a view made earlier may not be collected yet), so the asset itself is never touched.
	UMantleVehicleSource* View = DuplicateObject<UMantleVehicleSource>(&Source, GetTransientPackage());
	if (!View)
	{
		return &Source;
	}
	View->FitScale = 1.f;
	const auto Longer = [Scale](const FVector& Value) { return Value * Scale; };
	const auto Hinge = [&Longer](FMantleVehicleSourceHinge& Pivoted) { Pivoted.Pivot = Longer(Pivoted.Pivot); };
	const auto Contact = [&Longer](FMantleVehicleSourceContact& Point) { Point.Location = Longer(Point.Location); };
	const auto Seat = [&Longer, &Contact](FMantleVehicleSourceSeatContacts& Contacts)
	{
		Contact(Contacts.Pelvis);
		Contact(Contacts.LeftHand);
		Contact(Contacts.RightHand);
		Contact(Contacts.LeftFoot);
		Contact(Contacts.RightFoot);
		for (FMantleVehicleSourceExit& Exit : Contacts.Exits)
		{
			Exit.Location = Longer(Exit.Location);
		}
	};

	for (FMantleVehicleSourceWheel& Wheel : View->Wheels)
	{
		Wheel.Radius = static_cast<float>(Wheel.Radius * Scale);
		Wheel.Width = static_cast<float>(Wheel.Width * Scale);
		FTransform Tire = Wheel.TireTransform;
		Tire.SetTranslation(Longer(Tire.GetTranslation()));
		Tire.SetScale3D(Tire.GetScale3D() * Scale);
		Wheel.TireTransform = Tire;
	}

	FMantleVehicleSourceTuning& T = View->Tuning;
	T.CenterOfMass = Longer(T.CenterOfMass);
	T.SeatLocation = Longer(T.SeatLocation);
	T.SteeringWheelLocation = Longer(T.SteeringWheelLocation);
	T.SteeringWheelRadius = static_cast<float>(T.SteeringWheelRadius * Scale);
	Hinge(T.DriverDoor);
	T.PassengerSeatLocation = Longer(T.PassengerSeatLocation);
	Hinge(T.PassengerDoor);
	Hinge(T.TrunkLid);
	T.TrunkStand = Longer(T.TrunkStand);
	Seat(T.DriverContacts);
	Seat(T.PassengerContacts);
	for (FMantleVehicleSourceBox& Box : T.Collision)
	{
		Box.Center = Longer(Box.Center);
		Box.Extent = Longer(Box.Extent);
	}

	// A part rides a bone of the scaled body, so its placement and its size grow with it (a mirrored part stays mirrored).
	for (FMantleVehicleSourcePart& Part : View->Parts)
	{
		FTransform Placed = Part.Transform;
		Placed.SetTranslation(Longer(Placed.GetTranslation()));
		Placed.SetScale3D(Placed.GetScale3D() * Scale);
		Part.Transform = Placed;
	}
	return View;
}

TSharedRef<FJsonObject> FMantleVehicleConverter::MakeDocument(const UMantleVehicleSource& Authored, const TMap<FString, FString>& SectionMaterialIds, FString* OutError)
{
	using namespace MantleVehicleConverterPrivate;
	// The vehicle as it exports (#728): its Size applied. The copy is held while the document is written.
	const UMantleVehicleSource* View = MakeExportView(Authored);
	const TStrongObjectPtr<UMantleVehicleSource> HoldView(View == &Authored ? nullptr : const_cast<UMantleVehicleSource*>(View));
	const UMantleVehicleSource& Source = *View;
	const FMantleVehicleSourceTuning& T = Source.Tuning;
	TSharedRef<FJsonObject> Doc = MakeShared<FJsonObject>();
	Doc->SetField(TEXT("schemaVersion"), VehicleInteger(1));
	// The preset it was built from (#728): the cook holds the vehicle to that preset's ranges, then drops the key.
	if (!Source.Preset.IsNone())
	{
		Doc->SetStringField(TEXT("preset"), Source.Preset.ToString());
	}
	Doc->SetField(TEXT("mass"), VehicleNumber(T.Mass));
	Doc->SetField(TEXT("maxSpeed"), VehicleNumber(T.MaxSpeed));
	if (T.bOverrideCenterOfMass)
	{
		Doc->SetField(TEXT("centerOfMass"), VehicleTriple(T.CenterOfMass));
	}
	Doc->SetField(TEXT("dragCoefficient"), VehicleNumber(T.DragCoefficient));
	Doc->SetField(TEXT("frontalArea"), VehicleNumber(T.FrontalArea));

	TSharedRef<FJsonObject> Engine = MakeShared<FJsonObject>();
	Engine->SetField(TEXT("maxTorque"), VehicleNumber(T.MaxTorque));
	Engine->SetField(TEXT("maxRPM"), VehicleInteger(T.MaxRPM));
	Engine->SetField(TEXT("idleRPM"), VehicleInteger(T.IdleRPM));
	TArray<TSharedPtr<FJsonValue>> Curve;
	for (const FVector2D& Point : T.TorqueCurve)
	{
		TArray<TSharedPtr<FJsonValue>> Pair = { VehicleNumber(Point.X), VehicleNumber(Point.Y) };
		Curve.Add(MakeShared<FJsonValueArray>(Pair));
	}
	Engine->SetArrayField(TEXT("torqueCurve"), Curve);
	Doc->SetObjectField(TEXT("engine"), Engine);

	TSharedRef<FJsonObject> Transmission = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Gears;
	for (const float Ratio : T.GearRatios)
	{
		Gears.Add(VehicleNumber(Ratio));
	}
	Transmission->SetArrayField(TEXT("gearRatios"), Gears);
	Transmission->SetField(TEXT("reverseRatio"), VehicleNumber(T.ReverseRatio));
	Transmission->SetField(TEXT("finalDrive"), VehicleNumber(T.FinalDrive));
	if (T.ChangeUpRPM > 0)
	{
		Transmission->SetField(TEXT("changeUpRPM"), VehicleInteger(T.ChangeUpRPM));
	}
	if (T.ChangeDownRPM > 0)
	{
		Transmission->SetField(TEXT("changeDownRPM"), VehicleInteger(T.ChangeDownRPM));
	}
	Doc->SetObjectField(TEXT("transmission"), Transmission);

	TSharedRef<FJsonObject> Suspension = MakeShared<FJsonObject>();
	Suspension->SetField(TEXT("maxRaise"), VehicleNumber(T.SuspensionMaxRaise));
	Suspension->SetField(TEXT("maxDrop"), VehicleNumber(T.SuspensionMaxDrop));
	Suspension->SetField(TEXT("springRate"), VehicleNumber(T.SpringRate));
	Suspension->SetField(TEXT("springPreload"), VehicleNumber(T.SpringPreload));
	Suspension->SetField(TEXT("damping"), VehicleNumber(T.SpringDamping));
	Doc->SetObjectField(TEXT("suspension"), Suspension);

	TSharedRef<FJsonObject> Steering = MakeShared<FJsonObject>();
	Steering->SetField(TEXT("maxAngle"), VehicleNumber(T.MaxSteeringAngle));
	Steering->SetBoolField(TEXT("ackermann"), T.bAckermann);
	// #821: written only with a curve, so a car without one cooks to the bytes it did before.
	if (T.SteeringSpeedCurve.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SpeedCurve;
		for (const FVector2D& Point : T.SteeringSpeedCurve)
		{
			TArray<TSharedPtr<FJsonValue>> Pair = { VehicleNumber(Point.X), VehicleNumber(Point.Y) };
			SpeedCurve.Add(MakeShared<FJsonValueArray>(Pair));
		}
		Steering->SetArrayField(TEXT("speedCurve"), SpeedCurve);
	}
	Doc->SetObjectField(TEXT("steering"), Steering);

	TSharedRef<FJsonObject> Brakes = MakeShared<FJsonObject>();
	Brakes->SetField(TEXT("brakeTorque"), VehicleNumber(T.BrakeTorque));
	Brakes->SetField(TEXT("handbrakeTorque"), VehicleNumber(T.HandbrakeTorque));
	Doc->SetObjectField(TEXT("brakes"), Brakes);

	TSharedRef<FJsonObject> Tires = MakeShared<FJsonObject>();
	Tires->SetField(TEXT("friction"), VehicleNumber(T.TireFriction));
	if (T.bTireRearFriction)
	{
		Tires->SetField(TEXT("rearFriction"), VehicleNumber(T.TireRearFriction));
	}
	Doc->SetObjectField(TEXT("tires"), Tires);

	TArray<TSharedPtr<FJsonValue>> Wheels;
	for (const FMantleVehicleSourceWheel& Wheel : Source.Wheels)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("bone"), Wheel.Bone.ToString());
		Entry->SetBoolField(TEXT("steering"), Wheel.bSteering);
		Entry->SetBoolField(TEXT("driven"), Wheel.bDriven);
		Entry->SetBoolField(TEXT("handbrake"), Wheel.bHandbrake);
		if (Wheel.Radius > 0.f)
		{
			Entry->SetField(TEXT("radius"), VehicleNumber(Wheel.Radius));
		}
		if (Wheel.Width > 0.f)
		{
			Entry->SetField(TEXT("width"), VehicleNumber(Wheel.Width));
		}
		Wheels.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Doc->SetArrayField(TEXT("wheels"), Wheels);

	if (T.bSeatLocation)
	{
		TSharedRef<FJsonObject> Seat = MakeShared<FJsonObject>();
		Seat->SetField(TEXT("location"), VehicleTriple(T.SeatLocation));
		// #546: the steering wheel is part of the seat, only when set; its rotation as [pitch, yaw, roll], each within +-180.
		if (T.bSteeringWheel)
		{
			const FRotator Turn = T.SteeringWheelRotation.GetNormalized();
			TSharedRef<FJsonObject> Wheel = MakeShared<FJsonObject>();
			Wheel->SetField(TEXT("location"), VehicleTriple(T.SteeringWheelLocation));
			Wheel->SetField(TEXT("rotation"), VehicleTriple(FVector(Turn.Pitch, Turn.Yaw, Turn.Roll)));
			Wheel->SetField(TEXT("radius"), VehicleNumber(T.SteeringWheelRadius));
			if (!T.SteeringWheelBone.IsNone())
			{
				Wheel->SetStringField(TEXT("bone"), T.SteeringWheelBone.ToString());
			}
			Seat->SetObjectField(TEXT("steeringWheel"), Wheel);
		}
		// #546: the door beside the driver, part of the seat too.
		if (T.bHasDriverDoor)
		{
			Seat->SetObjectField(TEXT("door"), VehicleHinge(T.DriverDoor));
		}
		// #612: the driver's contact points, after the door.
		WriteVehicleSeatContacts(Seat, T.DriverContacts);
		// A model with no inside: written only when set, so every other document is unchanged.
		if (T.bHideDriver)
		{
			Seat->SetBoolField(TEXT("hidden"), true);
		}
		Doc->SetObjectField(TEXT("seat"), Seat);
	}

	// #546: the passenger seat (with the door beside it) and the trunk, only when set; the stand's yaw within +-180.
	if (T.bPassengerSeat)
	{
		TSharedRef<FJsonObject> Passenger = MakeShared<FJsonObject>();
		Passenger->SetField(TEXT("location"), VehicleTriple(T.PassengerSeatLocation));
		if (T.bHasPassengerDoor)
		{
			Passenger->SetObjectField(TEXT("door"), VehicleHinge(T.PassengerDoor));
		}
		WriteVehicleSeatContacts(Passenger, T.PassengerContacts);
		if (T.bHidePassenger)
		{
			Passenger->SetBoolField(TEXT("hidden"), true);
		}
		Doc->SetObjectField(TEXT("passengerSeat"), Passenger);
	}
	if (T.bTrunk)
	{
		TSharedRef<FJsonObject> Trunk = MakeShared<FJsonObject>();
		Trunk->SetObjectField(TEXT("lid"), VehicleHinge(T.TrunkLid));
		Trunk->SetField(TEXT("stand"), VehicleTriple(T.TrunkStand));
		Trunk->SetField(TEXT("yaw"), VehicleNumber(FRotator::NormalizeAxis(T.TrunkYaw)));
		Doc->SetObjectField(TEXT("trunk"), Trunk);
	}

	TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
	Camera->SetField(TEXT("armLength"), VehicleNumber(T.CameraArmLength));
	Camera->SetField(TEXT("height"), VehicleNumber(T.CameraHeight));
	Doc->SetObjectField(TEXT("camera"), Camera);

	if (T.Collision.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Boxes;
		for (const FMantleVehicleSourceBox& Box : T.Collision)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetField(TEXT("center"), VehicleTriple(Box.Center));
			Entry->SetField(TEXT("extent"), VehicleTriple(Box.Extent));
			Boxes.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Doc->SetArrayField(TEXT("collision"), Boxes);
	}

	// Paint (#435 f): only with sections, every finish value written. Liveries (#488): their ids go in at upload
	// (WithLiveries); the UV set and default ride with them.
	// Painted body sections or a part's painted slots (#435 item 2): either gives the car its paint, and sections may be empty.
	const FMantleVehicleSourcePaint& P = Source.Paint;
	const bool bPartPaint = Source.Parts.ContainsByPredicate([](const FMantleVehicleSourcePart& Part) { return Part.PaintSlots.Num() > 0; });
	if (P.Sections.Num() > 0 || bPartPaint)
	{
		TSharedRef<FJsonObject> Paint = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Sections;
		for (const FString& Section : P.Sections)
		{
			Sections.Add(MakeShared<FJsonValueString>(Section));
		}
		Paint->SetArrayField(TEXT("sections"), Sections);
		Paint->SetField(TEXT("color"), VehicleColor(P.Color));
		Paint->SetField(TEXT("metallic"), VehicleNumber(P.Metallic));
		Paint->SetField(TEXT("roughness"), VehicleNumber(P.Roughness));
		Paint->SetField(TEXT("flakeDensity"), VehicleNumber(P.FlakeDensity));
		Paint->SetField(TEXT("flakeSize"), VehicleNumber(P.FlakeSize));
		Paint->SetField(TEXT("flakeTint"), VehicleColor(P.FlakeTint));
		Paint->SetField(TEXT("clearCoat"), VehicleNumber(P.ClearCoat));
		Paint->SetField(TEXT("clearCoatRoughness"), VehicleNumber(P.ClearCoatRoughness));
		Doc->SetObjectField(TEXT("paint"), Paint);
	}
	if (P.Liveries.Num() > 0)
	{
		Doc->SetField(TEXT("liveryUV"), VehicleInteger(0));
		if (P.Liveries.IsValidIndex(P.DefaultLivery))
		{
			Doc->SetField(TEXT("defaultLivery"), VehicleInteger(P.DefaultLivery));
		}
	}

	// Suspension linkage (#435 b): only with rules, each with its type's keys in the schema's order, every one written.
	if (Source.Linkage.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Rules;
		for (const FMantleVehicleSourceLinkage& Rule : Source.Linkage)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("type"), VehicleLinkageTypeName(Rule.Type));
			Entry->SetStringField(TEXT("bone"), Rule.Bone.ToString());
			auto Name = [&Entry](const TCHAR* Key, const FName Bone)
			{
				if (!Bone.IsNone())
				{
					Entry->SetStringField(Key, Bone.ToString());
				}
			};
			switch (Rule.Type)
			{
			case EMantleVehicleLinkageType::Aim:
				Name(TEXT("target"), Rule.Target);
				Name(TEXT("up"), Rule.Up);
				break;
			case EMantleVehicleLinkageType::Stretch:
				Name(TEXT("end"), Rule.End);
				Name(TEXT("target"), Rule.Target);
				break;
			case EMantleVehicleLinkageType::Copy:
				Name(TEXT("source"), Rule.Source);
				Entry->SetBoolField(TEXT("rotation"), Rule.bRotation);
				Entry->SetBoolField(TEXT("translation"), Rule.bTranslation);
				if (Rule.Axis != EMantleVehicleLinkageAxis::All)
				{
					Entry->SetStringField(TEXT("axis"), Rule.Axis == EMantleVehicleLinkageAxis::X ? TEXT("x") : (Rule.Axis == EMantleVehicleLinkageAxis::Y ? TEXT("y") : TEXT("z")));
				}
				break;
			}
			Entry->SetField(TEXT("weight"), VehicleNumber(Rule.Weight));
			Rules.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Doc->SetArrayField(TEXT("linkage"), Rules);
	}

	// #821: the movement type, the seat kinds and the movement blocks (filled from the source by MergeSource). Blocks that do not read leave the document without them, which is
	// said to whoever asked (CheckSource and the build do) and never dropped silently.
	FString MergeError;
	if (!FMantleVehicleChassisDocument::MergeSource(Source, *Doc, &MergeError) && OutError)
	{
		*OutError = MergeError.IsEmpty() ? FString(TEXT("they could not be read")) : MergeError;
	}
	else if (OutError)
	{
		OutError->Reset();
	}

	return WithMaterials(Doc, SectionMaterialIds);
}

TSharedRef<FJsonObject> FMantleVehicleConverter::WithMaterials(const TSharedRef<FJsonObject>& Document, const TMap<FString, FString>& SectionMaterialIds)
{
	using namespace MantleVehicleConverterPrivate;
	TSharedPtr<FJsonValue> Value;
	if (SectionMaterialIds.Num() > 0)
	{
		TArray<FString> Sections;
		SectionMaterialIds.GetKeys(Sections);
		Sections.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
		TSharedRef<FJsonObject> Materials = MakeShared<FJsonObject>();
		for (const FString& Section : Sections)
		{
			Materials->SetStringField(Section, SectionMaterialIds[Section]);
		}
		Value = MakeShared<FJsonValueObject>(Materials);
	}
	TMap<FString, TSharedPtr<FJsonValue>> Replace;
	Replace.Add(TEXT("materials"), Value);
	return VehicleRebuild(Document, Replace);
}

TSharedRef<FJsonObject> FMantleVehicleConverter::WithLiveries(const TSharedRef<FJsonObject>& Document, const TArray<FString>& LiveryIds)
{
	using namespace MantleVehicleConverterPrivate;
	TSharedPtr<FJsonValue> Value;
	if (LiveryIds.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Ids;
		for (const FString& Id : LiveryIds)
		{
			Ids.Add(MakeShared<FJsonValueString>(Id));
		}
		Value = MakeShared<FJsonValueArray>(Ids);
	}
	TMap<FString, TSharedPtr<FJsonValue>> Replace;
	Replace.Add(TEXT("liveries"), Value);
	return VehicleRebuild(Document, Replace);
}

TSharedRef<FJsonObject> FMantleVehicleConverter::WithParts(const TSharedRef<FJsonObject>& Document, const TArray<FPartIds>& Parts)
{
	using namespace MantleVehicleConverterPrivate;
	TSharedPtr<FJsonValue> Value;
	if (Parts.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> List;
		for (const FPartIds& Part : Parts)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("mesh"), Part.Mesh);
			Entry->SetStringField(TEXT("bone"), Part.Bone.ToString());
			Entry->SetField(TEXT("location"), VehicleTriple(Part.Transform.GetLocation()));
			// One sign of the quaternion, W up, so the same turn always writes the same numbers.
			FQuat Turn = Part.Transform.GetRotation().GetNormalized();
			if (Turn.W < 0.0)
			{
				Turn = FQuat(-Turn.X, -Turn.Y, -Turn.Z, -Turn.W);
			}
			TArray<TSharedPtr<FJsonValue>> Rotation = { VehicleNumber(Turn.X), VehicleNumber(Turn.Y), VehicleNumber(Turn.Z), VehicleNumber(Turn.W) };
			Entry->SetArrayField(TEXT("rotation"), Rotation);
			Entry->SetField(TEXT("scale"), VehicleTriple(Part.Transform.GetScale3D()));
			if (Part.Materials.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Materials;
				for (const FString& Id : Part.Materials)
				{
					Materials.Add(Id.IsEmpty() ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()) : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Id)));
				}
				Entry->SetArrayField(TEXT("materials"), Materials);
			}
			if (Part.PaintSlots.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Painted;
				for (const int32 Slot : Part.PaintSlots)
				{
					Painted.Add(VehicleInteger(Slot));
				}
				Entry->SetArrayField(TEXT("paintSlots"), Painted);
			}
			List.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Value = MakeShared<FJsonValueArray>(List);
	}
	TMap<FString, TSharedPtr<FJsonValue>> Replace;
	Replace.Add(TEXT("parts"), Value);
	return VehicleRebuild(Document, Replace);
}

TSharedRef<FJsonObject> FMantleVehicleConverter::WithHorn(const TSharedRef<FJsonObject>& Document, const FString& HornSoundId)
{
	using namespace MantleVehicleConverterPrivate;
	TSharedPtr<FJsonValue> Value;
	if (!HornSoundId.IsEmpty())
	{
		TSharedRef<FJsonObject> Horn = MakeShared<FJsonObject>();
		Horn->SetStringField(TEXT("sound"), HornSoundId);
		Value = MakeShared<FJsonValueObject>(Horn);
	}
	TMap<FString, TSharedPtr<FJsonValue>> Replace;
	Replace.Add(TEXT("horn"), Value);
	return VehicleRebuild(Document, Replace);
}

TArray<FString> FMantleVehicleConverter::UniqueSectionNames(const TArray<FString>& MaterialNames)
{
	// Sections are bound by material name: unique (ignoring case, as the document's keys are) and non-empty.
	TSet<FString> Used;
	TArray<FString> Out;
	for (int32 Index = 0; Index < MaterialNames.Num(); ++Index)
	{
		FString Name = MaterialNames[Index].Left(200);
		if (Name.IsEmpty())
		{
			Name = FString::Printf(TEXT("material_%d"), Index);
		}
		FString Unique = Name;
		for (int32 Suffix = 2; Used.Contains(Unique); ++Suffix)
		{
			Unique = FString::Printf(TEXT("%s_%d"), *Name, Suffix);
		}
		Used.Add(Unique);
		Out.Add(Unique);
	}
	return Out;
}

TArray<FString> FMantleVehicleConverter::LiveryTextureKeys(const UMantleVehicleSource& Source)
{
	TArray<FString> Keys;
	for (const TSoftObjectPtr<UTexture2D>& Livery : Source.Paint.Liveries)
	{
		const UTexture2D* Texture = Livery.LoadSynchronous();
		Keys.Add(Texture ? FMantlePlaceAssets::DecalTextureKey(Texture, /*bNormal*/ false) : FString());
	}
	return Keys;
}

bool FMantleVehicleConverter::WriteUpload(const FMantleGlbMesh& Body, const TSharedRef<FJsonObject>& Document, TArray<uint8>& OutBytes, FString& OutError)
{
	FMantleGlbMesh Upload = Body;
	TSharedRef<FJsonObject> Extras = MakeShared<FJsonObject>();
	Extras->SetObjectField(ExtrasKey, Document);
	Upload.Extras = Extras;
	return FMantleGlbWriter::Write(Upload, OutBytes, OutError);
}

void FMantleVehicleConverter::CheckSource(const UMantleVehicleSource* Authored, const FString& UsedBy, TArray<FMantleValidationIssue>& OutIssues)
{
	using namespace MantleVehicleConverterPrivate;
	if (!Authored)
	{
		return;
	}
	// The vehicle as it exports (#728): its Size applied, so every number is checked as the cook will see it. The copy is held for the
	// whole check; the body is read from the original, which scales it by the same Size.
	const UMantleVehicleSource* Source = MakeExportView(*Authored);
	const TStrongObjectPtr<UMantleVehicleSource> HoldView(Source == Authored ? nullptr : const_cast<UMantleVehicleSource*>(Source));
	const FString Subject = UsedBy.IsEmpty() ? FString::Printf(TEXT("Vehicle '%s'"), *Authored->GetName())
		: FString::Printf(TEXT("Vehicle '%s' (%s)"), *Authored->GetName(), *UsedBy);
	auto AddError = [&OutIssues, &Subject](const FString& Problem, const FString& Remedy)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(), Subject + TEXT(" ") + Problem, Remedy });
	};

	// The preset the vehicle was built from (#728): its narrower ranges and wheel counts apply on top of the absolute limits.
	const FMantleVehiclePreset* Preset = nullptr;
	if (!Source->Preset.IsNone())
	{
		Preset = FMantleVehiclePresets::Find(Source->Preset.ToString());
		if (!Preset)
		{
			AddError(FString::Printf(TEXT("was built from the '%s' preset, which this version of the Mantle plugin doesn't know."), *Source->Preset.ToString()),
				TEXT("Update the plugin, or pick another preset."));
		}
	}
	const FVehiclePresetScope PresetScope(Preset);

	// The movement blocks (a plane's flight block, a bike's rider): a malformed one would export a vehicle without it, so it is an error and not a quiet drop.
	{
		FString MovementError;
		FMantleVehicleConverter::MakeDocument(*Authored, TMap<FString, FString>(), &MovementError);
		if (!MovementError.IsEmpty())
		{
			AddError(FString::Printf(TEXT("has movement settings that can't be read (%s), so it would be published without them."), *FMantleVehicleShape::PlainReason(MovementError)),
				TEXT("Build the vehicle again from the Build a vehicle window, or fix its Movement Blocks."));
		}
	}

	// The numbers, each to its range, and the rules between them (the cook's, validate.py's check_vehicle_rules).
	const FMantleVehicleSourceTuning& T = Source->Tuning;
	VehicleCheckRange(Subject, TEXT("Mass"), TEXT("mass"), T.Mass, OutIssues);
	VehicleCheckRange(Subject, TEXT("Max Speed"), TEXT("maxSpeed"), T.MaxSpeed, OutIssues);
	if (T.bOverrideCenterOfMass)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			VehicleCheckRange(Subject, TEXT("Center Of Mass"), TEXT("centerOfMass/*"), T.CenterOfMass[Axis], OutIssues);
		}
	}
	VehicleCheckRange(Subject, TEXT("Drag Coefficient"), TEXT("dragCoefficient"), T.DragCoefficient, OutIssues);
	VehicleCheckRange(Subject, TEXT("Frontal Area"), TEXT("frontalArea"), T.FrontalArea, OutIssues);
	VehicleCheckRange(Subject, TEXT("Max Torque"), TEXT("engine/maxTorque"), T.MaxTorque, OutIssues);
	VehicleCheckRange(Subject, TEXT("Max RPM"), TEXT("engine/maxRPM"), T.MaxRPM, OutIssues);
	VehicleCheckRange(Subject, TEXT("Idle RPM"), TEXT("engine/idleRPM"), T.IdleRPM, OutIssues);
	if (T.IdleRPM >= T.MaxRPM)
	{
		AddError(FString::Printf(TEXT("idles at %d RPM, not below its Max RPM (%d)."), T.IdleRPM, T.MaxRPM), TEXT("Lower Idle RPM or raise Max RPM."));
	}
	if (T.TorqueCurve.Num() < 2 || T.TorqueCurve.Num() > FMantleVehicleLimits::MaxTorqueCurvePoints)
	{
		AddError(FString::Printf(TEXT("has %d torque curve points; a vehicle has 2 to %d."), T.TorqueCurve.Num(), FMantleVehicleLimits::MaxTorqueCurvePoints),
			TEXT("Give Torque Curve at least a point at low RPM and one at high RPM."));
	}
	for (int32 Point = 0; Point < T.TorqueCurve.Num(); ++Point)
	{
		VehicleCheckRange(Subject, FString::Printf(TEXT("Torque Curve point %d's RPM"), Point), TEXT("engine/torqueCurve/*/0"), T.TorqueCurve[Point].X, OutIssues);
		VehicleCheckRange(Subject, FString::Printf(TEXT("Torque Curve point %d's torque"), Point), TEXT("engine/torqueCurve/*/1"), T.TorqueCurve[Point].Y, OutIssues);
		if (Point > 0 && T.TorqueCurve[Point].X <= T.TorqueCurve[Point - 1].X)
		{
			AddError(FString::Printf(TEXT("has a torque curve whose point %d does not rise in RPM past the one before."), Point),
				TEXT("List Torque Curve's points from low RPM to high."));
		}
		else if (Point > 0 && T.TorqueCurve[Point].X > T.MaxRPM)
		{
			AddError(FString::Printf(TEXT("has a torque curve point at %s RPM, past its Max RPM (%d)."), *VehicleNumberText(T.TorqueCurve[Point].X), T.MaxRPM),
				TEXT("Keep every Torque Curve point at or below Max RPM."));
		}
	}
	// #821: the steering's speed curve, when it has one, has 1 to 8 points rising in speed.
	if (T.SteeringSpeedCurve.Num() > FMantleVehicleLimits::MaxSteeringCurvePoints)
	{
		AddError(FString::Printf(TEXT("has %d steering curve points; a vehicle has at most %d."), T.SteeringSpeedCurve.Num(), FMantleVehicleLimits::MaxSteeringCurvePoints),
			TEXT("Give Steering Speed Curve 8 points or fewer, or none."));
	}
	for (int32 Point = 0; Point < T.SteeringSpeedCurve.Num(); ++Point)
	{
		VehicleCheckRange(Subject, FString::Printf(TEXT("Steering Speed Curve point %d's mph"), Point), TEXT("steering/speedCurve/*/0"), T.SteeringSpeedCurve[Point].X, OutIssues);
		VehicleCheckRange(Subject, FString::Printf(TEXT("Steering Speed Curve point %d's share"), Point), TEXT("steering/speedCurve/*/1"), T.SteeringSpeedCurve[Point].Y, OutIssues);
		if (Point > 0 && T.SteeringSpeedCurve[Point].X <= T.SteeringSpeedCurve[Point - 1].X)
		{
			AddError(FString::Printf(TEXT("has a steering curve whose point %d does not rise in mph past the one before."), Point),
				TEXT("List Steering Speed Curve's points from low speed to high."));
		}
	}
	if (T.bTireRearFriction)
	{
		VehicleCheckRange(Subject, TEXT("Tire Rear Friction"), TEXT("tires/rearFriction"), T.TireRearFriction, OutIssues);
	}
	if (T.GearRatios.Num() < 1 || T.GearRatios.Num() > FMantleVehicleLimits::MaxGears)
	{
		AddError(FString::Printf(TEXT("has %d gears; a vehicle has 1 to %d."), T.GearRatios.Num(), FMantleVehicleLimits::MaxGears), TEXT("Give Gear Ratios 1 to 8 entries, first gear first."));
	}
	for (int32 Gear = 0; Gear < T.GearRatios.Num(); ++Gear)
	{
		VehicleCheckRange(Subject, FString::Printf(TEXT("Gear Ratios %d"), Gear), TEXT("transmission/gearRatios/*"), T.GearRatios[Gear], OutIssues);
	}
	VehicleCheckRange(Subject, TEXT("Reverse Ratio"), TEXT("transmission/reverseRatio"), T.ReverseRatio, OutIssues);
	VehicleCheckRange(Subject, TEXT("Final Drive"), TEXT("transmission/finalDrive"), T.FinalDrive, OutIssues);
	const int32 ChangeUp = T.ChangeUpRPM > 0 ? T.ChangeUpRPM : static_cast<int32>(T.MaxRPM * FMantleVehicleConverter::DefaultChangeUpFraction);
	if (T.ChangeUpRPM > 0)
	{
		VehicleCheckRange(Subject, TEXT("Change Up RPM"), TEXT("transmission/changeUpRPM"), T.ChangeUpRPM, OutIssues);
		if (T.ChangeUpRPM > T.MaxRPM)
		{
			AddError(FString::Printf(TEXT("changes up at %d RPM, past its Max RPM (%d)."), T.ChangeUpRPM, T.MaxRPM), TEXT("Lower Change Up RPM, or set it to 0 for 85% of Max RPM."));
		}
	}
	if (T.ChangeDownRPM > 0)
	{
		VehicleCheckRange(Subject, TEXT("Change Down RPM"), TEXT("transmission/changeDownRPM"), T.ChangeDownRPM, OutIssues);
		if (T.ChangeDownRPM >= ChangeUp)
		{
			AddError(FString::Printf(TEXT("changes down at %d RPM, not below where it changes up (%d)."), T.ChangeDownRPM, ChangeUp),
				TEXT("Lower Change Down RPM, or set it to 0 for 40% of Max RPM."));
		}
	}
	VehicleCheckRange(Subject, TEXT("Suspension Max Raise"), TEXT("suspension/maxRaise"), T.SuspensionMaxRaise, OutIssues);
	VehicleCheckRange(Subject, TEXT("Suspension Max Drop"), TEXT("suspension/maxDrop"), T.SuspensionMaxDrop, OutIssues);
	VehicleCheckRange(Subject, TEXT("Spring Rate"), TEXT("suspension/springRate"), T.SpringRate, OutIssues);
	VehicleCheckRange(Subject, TEXT("Spring Preload"), TEXT("suspension/springPreload"), T.SpringPreload, OutIssues);
	VehicleCheckRange(Subject, TEXT("Spring Damping"), TEXT("suspension/damping"), T.SpringDamping, OutIssues);
	VehicleCheckRange(Subject, TEXT("Max Steering Angle"), TEXT("steering/maxAngle"), T.MaxSteeringAngle, OutIssues);
	VehicleCheckRange(Subject, TEXT("Brake Torque"), TEXT("brakes/brakeTorque"), T.BrakeTorque, OutIssues);
	VehicleCheckRange(Subject, TEXT("Handbrake Torque"), TEXT("brakes/handbrakeTorque"), T.HandbrakeTorque, OutIssues);
	VehicleCheckRange(Subject, TEXT("Tire Friction"), TEXT("tires/friction"), T.TireFriction, OutIssues);
	if (T.bSeatLocation)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			VehicleCheckRange(Subject, TEXT("Seat Location"), TEXT("seat/location/*"), T.SeatLocation[Axis], OutIssues);
		}
	}
	if (T.bHideDriver && !T.bSeatLocation)
	{
		// The document's seat needs its location, and hidden is a key of the seat (seat.hidden).
		AddError(TEXT("hides its driver but has no driver seat placed."),
			TEXT("Turn on Seat Location, or use Hide the driver in the Build a vehicle window, which places one for you."));
	}
	if (T.bSteeringWheel)
	{
		if (!T.bSeatLocation)
		{
			AddError(TEXT("has a Steering Wheel but no Seat Location; the wheel is part of the driver's seat."),
				TEXT("Tick Seat Location and place the driver behind the wheel, or untick Steering Wheel."));
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			VehicleCheckRange(Subject, TEXT("Steering Wheel Location"), TEXT("seat/steeringWheel/location/*"), T.SteeringWheelLocation[Axis], OutIssues);
		}
		VehicleCheckRange(Subject, TEXT("Steering Wheel Radius"), TEXT("seat/steeringWheel/radius"), T.SteeringWheelRadius, OutIssues);
	}
	// #546: the doors, the passenger seat and the trunk. Each hinge's numbers to their ranges and its axis a direction; the bones
	// the car clips turn are the body's, checked once the body is read.
	auto CheckHinge = [&Subject, &OutIssues, &AddError](const FString& What, const FString& Path, const FMantleVehicleSourceHinge& Hinge)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			VehicleCheckRange(Subject, What + TEXT(" Pivot"), Path + TEXT("/pivot/*"), Hinge.Pivot[Axis], OutIssues);
			VehicleCheckRange(Subject, What + TEXT(" Axis"), Path + TEXT("/axis/*"), Hinge.Axis[Axis], OutIssues);
		}
		VehicleCheckRange(Subject, What + TEXT(" Scale"), Path + TEXT("/scale"), Hinge.Scale, OutIssues);
		if (Hinge.Axis.Size() < FMantleVehicleLimits::HingeMinAxis)
		{
			AddError(FString::Printf(TEXT("has a %s Axis with no direction."), *What),
				FString::Printf(TEXT("Set %s Axis to the way its hinge runs, such as 0, 0, 1 for a door hinged upright."), *What));
		}
	};
	if (T.bHasDriverDoor)
	{
		if (!T.bSeatLocation)
		{
			AddError(TEXT("has a Driver Door but no Seat Location; the door is part of the driver's seat."),
				TEXT("Tick Seat Location and place the driver beside the door, or untick Has Driver Door."));
		}
		CheckHinge(TEXT("Driver Door"), TEXT("seat/door"), T.DriverDoor);
	}
	if (T.bPassengerSeat)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			VehicleCheckRange(Subject, TEXT("Passenger Seat Location"), TEXT("passengerSeat/location/*"), T.PassengerSeatLocation[Axis], OutIssues);
		}
		if (T.bHasPassengerDoor)
		{
			CheckHinge(TEXT("Passenger Door"), TEXT("passengerSeat/door"), T.PassengerDoor);
		}
	}
	if (T.bTrunk)
	{
		CheckHinge(TEXT("Trunk Lid"), TEXT("trunk/lid"), T.TrunkLid);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			VehicleCheckRange(Subject, TEXT("Trunk Stand"), TEXT("trunk/stand/*"), T.TrunkStand[Axis], OutIssues);
		}
	}
	// #612: each seat's contact points, as the cook holds them (vehicle.go seatContacts and contactRules); their bones are the body's,
	// checked once the body is read.
	auto CheckContacts = [&Subject, &OutIssues, &AddError, &T, Source](const TCHAR* Seat, const FString& Path, const FVector& SeatLocation,
		const FMantleVehicleSourceSeatContacts& Contacts, const bool bDriver)
	{
		struct FPoint
		{
			FString What;
			FString Path;
			const FMantleVehicleSourceContact* Point;
		};
		TArray<FPoint, TInlineAllocator<5>> Points;
		if (Contacts.bPelvis)
		{
			Points.Add({ FString::Printf(TEXT("%s Pelvis"), Seat), Path + TEXT("/pelvis"), &Contacts.Pelvis });
		}
		if (Contacts.Hands != EMantleVehicleHandsKind::None)
		{
			Points.Add({ FString::Printf(TEXT("%s Left Hand"), Seat), Path + TEXT("/hands/left"), &Contacts.LeftHand });
			Points.Add({ FString::Printf(TEXT("%s Right Hand"), Seat), Path + TEXT("/hands/right"), &Contacts.RightHand });
		}
		if (Contacts.bFeet)
		{
			Points.Add({ FString::Printf(TEXT("%s Left Foot"), Seat), Path + TEXT("/feet/left"), &Contacts.LeftFoot });
			Points.Add({ FString::Printf(TEXT("%s Right Foot"), Seat), Path + TEXT("/feet/right"), &Contacts.RightFoot });
		}
		for (const FPoint& Entry : Points)
		{
			// The rotation is written normalised, within its range.
			const FMantleVehicleSourceContact& Point = *Entry.Point;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				VehicleCheckRange(Subject, Entry.What + TEXT(" Location"), Entry.Path + TEXT("/location/*"), Point.Location[Axis], OutIssues);
			}
			if (!Point.Bone.IsNone())
			{
				// FName compares without regard to case, as the body's bone names do.
				if (Source->Wheels.ContainsByPredicate([&Point](const FMantleVehicleSourceWheel& Wheel) { return Wheel.Bone == Point.Bone; }))
				{
					AddError(FString::Printf(TEXT("has its %s on bone %s, a wheel's bone, which spins."), *Entry.What, *Point.Bone.ToString()),
						FString::Printf(TEXT("Put the %s on another bone, or clear its Bone for the vehicle's own space."), *Entry.What));
				}
				continue;
			}
			const double Reach = FVector::Dist(Point.Location, SeatLocation);
			if (Reach > FMantleVehicleLimits::ContactReach)
			{
				AddError(FString::Printf(TEXT("has its %s %s cm from the seat, farther than a seated body reaches (%s cm)."), *Entry.What,
					*VehicleNumberText(Reach), *VehicleNumberText(FMantleVehicleLimits::ContactReach)),
					FString::Printf(TEXT("Move the %s nearer the seat."), *Entry.What));
			}
		}
		if (Contacts.Hands == EMantleVehicleHandsKind::Wheel)
		{
			if (!bDriver)
			{
				AddError(FString::Printf(TEXT("has the %s's hands on a steering wheel; only the driver holds one."), Seat),
					FString::Printf(TEXT("Set the %s's Hands to Fixed."), Seat));
			}
			else if (!T.bSteeringWheel)
			{
				AddError(TEXT("has the Driver's hands on a steering wheel, but no Steering Wheel."), TEXT("Tick Steering Wheel and place it, or set the Driver's Hands to Fixed."));
			}
			else
			{
				for (const FMantleVehicleSourceContact* Hand : { &Contacts.LeftHand, &Contacts.RightHand })
				{
					const FString Which = Hand == &Contacts.LeftHand ? TEXT("Left Hand") : TEXT("Right Hand");
					if (!Hand->Bone.IsNone())
					{
						AddError(FString::Printf(TEXT("has the Driver's %s on bone %s; a hand on the steering wheel is in the vehicle's own space."), *Which, *Hand->Bone.ToString()),
							FString::Printf(TEXT("Clear the Driver's %s Bone: Mantle turns the hand round the hub with the wheel."), *Which));
						continue;
					}
					const FVector Axis = FRotator(T.SteeringWheelRotation.Pitch, T.SteeringWheelRotation.Yaw, 0.0).Vector();
					const FVector Offset = Hand->Location - T.SteeringWheelLocation;
					const double Along = FVector::DotProduct(Offset, Axis);
					const double Across = FMath::Sqrt(FMath::Max(0.0, Offset.SizeSquared() - Along * Along));
					const double Off = FMath::Sqrt(FMath::Square(Across - T.SteeringWheelRadius) + FMath::Square(Along));
					if (Off > FMantleVehicleLimits::WheelHandTolerance)
					{
						AddError(FString::Printf(TEXT("has the Driver's %s %s cm off the steering wheel's rim (at most %s)."), *Which, *VehicleNumberText(Off),
							*VehicleNumberText(FMantleVehicleLimits::WheelHandTolerance)),
							FString::Printf(TEXT("Put the Driver's %s on the rim, with the wheel straight ahead."), *Which));
					}
				}
			}
		}
		if (Contacts.Exits.Num() > FMantleVehicleLimits::MaxSeatExits)
		{
			AddError(FString::Printf(TEXT("has %d exits for the %s; a seat has at most %d."), Contacts.Exits.Num(), Seat, FMantleVehicleLimits::MaxSeatExits),
				TEXT("Keep the seat's own side first and remove the rest."));
		}
		for (int32 Index = 0; Index < Contacts.Exits.Num(); ++Index)
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				VehicleCheckRange(Subject, FString::Printf(TEXT("%s Exit %d Location"), Seat, Index), Path + TEXT("/exits/*/location/*"),
					Contacts.Exits[Index].Location[Axis], OutIssues);
			}
		}
	};
	if (T.DriverContacts.HasAny())
	{
		if (!T.bSeatLocation)
		{
			AddError(TEXT("has Driver Contacts but no Seat Location; the contact points are part of the driver's seat."),
				TEXT("Tick Seat Location and place the driver, or clear Driver Contacts."));
		}
		CheckContacts(TEXT("Driver"), TEXT("seat"), T.SeatLocation, T.DriverContacts, true);
	}
	if (T.bPassengerSeat && T.PassengerContacts.HasAny())
	{
		CheckContacts(TEXT("Passenger"), TEXT("passengerSeat"), T.PassengerSeatLocation, T.PassengerContacts, false);
	}
	VehicleCheckRange(Subject, TEXT("Camera Arm Length"), TEXT("camera/armLength"), T.CameraArmLength, OutIssues);
	VehicleCheckRange(Subject, TEXT("Camera Height"), TEXT("camera/height"), T.CameraHeight, OutIssues);
	if (T.Collision.Num() > FMantleVehicleLimits::MaxCollisionBoxes)
	{
		AddError(FString::Printf(TEXT("has %d collision boxes; a vehicle has at most %d."), T.Collision.Num(), FMantleVehicleLimits::MaxCollisionBoxes),
			TEXT("Cover the chassis with fewer, larger boxes."));
	}
	for (const FMantleVehicleSourceBox& Box : T.Collision)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			VehicleCheckRange(Subject, TEXT("a collision box's Center"), TEXT("collision/*/center/*"), Box.Center[Axis], OutIssues);
			VehicleCheckRange(Subject, TEXT("a collision box's Extent"), TEXT("collision/*/extent/*"), Box.Extent[Axis], OutIssues);
		}
	}

	// The wheels.
	const int32 NumWheels = Source->Wheels.Num();
	if (NumWheels < FMantleVehicleLimits::MinWheels || NumWheels > FMantleVehicleLimits::MaxWheels)
	{
		AddError(FString::Printf(TEXT("has %d wheels; a vehicle has %d to %d."), NumWheels, FMantleVehicleLimits::MinWheels, FMantleVehicleLimits::MaxWheels),
			TEXT("Run Convert Vehicle on its body to find the wheel bones, or edit Wheels."));
	}
	if (Preset && Preset->MaxWheels > 0 && (NumWheels < Preset->MinWheels || NumWheels > Preset->MaxWheels))
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("%s: A %s has %d to %d wheels; this vehicle has %d."), *Subject, *Preset->Name, Preset->MinWheels, Preset->MaxWheels, NumWheels),
			TEXT("Change its wheels, or build this vehicle from a different preset.") });
	}
	int32 Driven = 0;
	int32 Steered = 0;
	for (int32 Index = 0; Index < NumWheels; ++Index)
	{
		const FMantleVehicleSourceWheel& Wheel = Source->Wheels[Index];
		Driven += Wheel.bDriven ? 1 : 0;
		Steered += Wheel.bSteering ? 1 : 0;
		for (int32 Before = 0; Before < Index; ++Before)
		{
			if (Source->Wheels[Before].Bone == Wheel.Bone)
			{
				AddError(FString::Printf(TEXT("has two wheels on bone %s."), *Wheel.Bone.ToString()), TEXT("Give each wheel its own bone."));
			}
		}
		if (Wheel.Radius > 0.f)
		{
			VehicleCheckRange(Subject, FString::Printf(TEXT("%s's Radius"), *Wheel.Bone.ToString()), TEXT("wheels/*/radius"), Wheel.Radius, OutIssues);
		}
		if (Wheel.Width > 0.f)
		{
			VehicleCheckRange(Subject, FString::Printf(TEXT("%s's Width"), *Wheel.Bone.ToString()), TEXT("wheels/*/width"), Wheel.Width, OutIssues);
		}
	}
	if (NumWheels > 0 && Driven == 0)
	{
		AddError(TEXT("has no driven wheel."), TEXT("Tick Driven on at least one wheel."));
	}
	if (NumWheels > 0 && Steered == 0)
	{
		AddError(TEXT("has no steering wheel."), TEXT("Tick Steering on at least one wheel (usually the front ones)."));
	}

	// Paint (#435 f): its numbers to their ranges; its sections are the body's, checked once the body is read.
	// A part's painted slots (#435 item 2) give it paint as a body section does.
	const FMantleVehicleSourcePaint& Paint = Source->Paint;
	const bool bPaint = Paint.Sections.Num() > 0
		|| Source->Parts.ContainsByPredicate([](const FMantleVehicleSourcePart& Part) { return Part.PaintSlots.Num() > 0; });
	if (bPaint)
	{
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			VehicleCheckRange(Subject, TEXT("Paint Color"), TEXT("paint/color/*"), Paint.Color.Component(Channel), OutIssues);
			VehicleCheckRange(Subject, TEXT("Paint Flake Tint"), TEXT("paint/flakeTint/*"), Paint.FlakeTint.Component(Channel), OutIssues);
		}
		VehicleCheckRange(Subject, TEXT("Paint Metallic"), TEXT("paint/metallic"), Paint.Metallic, OutIssues);
		VehicleCheckRange(Subject, TEXT("Paint Roughness"), TEXT("paint/roughness"), Paint.Roughness, OutIssues);
		VehicleCheckRange(Subject, TEXT("Paint Flake Density"), TEXT("paint/flakeDensity"), Paint.FlakeDensity, OutIssues);
		VehicleCheckRange(Subject, TEXT("Paint Flake Size"), TEXT("paint/flakeSize"), Paint.FlakeSize, OutIssues);
		VehicleCheckRange(Subject, TEXT("Paint Clear Coat"), TEXT("paint/clearCoat"), Paint.ClearCoat, OutIssues);
		VehicleCheckRange(Subject, TEXT("Paint Clear Coat Roughness"), TEXT("paint/clearCoatRoughness"), Paint.ClearCoatRoughness, OutIssues);
		if (Paint.Sections.Num() > FMantleVehicleLimits::MaxSections)
		{
			AddError(FString::Printf(TEXT("paints %d sections; a vehicle body has at most %d."), Paint.Sections.Num(), FMantleVehicleLimits::MaxSections),
				TEXT("List each painted section of the body once under Paint > Sections."));
		}
	}

	// Liveries (#488), within limits.json placeAssets.livery.
	const int32 NumLiveries = Paint.Liveries.Num();
	if (NumLiveries > FMantleVehicleLimits::MaxLiveries)
	{
		AddError(FString::Printf(TEXT("has %d liveries; a vehicle has at most %d."), NumLiveries, FMantleVehicleLimits::MaxLiveries),
			TEXT("Remove some of its Liveries."));
	}
	if (NumLiveries > 0 && !bPaint)
	{
		AddError(TEXT("has liveries but no paint to lay them on."), TEXT("Add the body sections the liveries cover to Paint > Sections."));
	}
	if (Paint.DefaultLivery != INDEX_NONE && !Paint.Liveries.IsValidIndex(Paint.DefaultLivery))
	{
		AddError(FString::Printf(TEXT("wears livery %d by default, but has %d liveries."), Paint.DefaultLivery, NumLiveries),
			TEXT("Set Default Livery to -1 (none) or to one of its Liveries' index (the first is 0)."));
	}
	const FMantlePlaceAssetLimits TextureLimits = FMantlePlaceAssetLimits::FromContract();
	for (int32 Index = 0; Index < NumLiveries; ++Index)
	{
		const UTexture2D* Texture = Paint.Liveries[Index].LoadSynchronous();
		if (!Texture)
		{
			AddError(FString::Printf(TEXT("has livery %d empty or missing (%s)."), Index, *Paint.Liveries[Index].ToString()),
				TEXT("Set it to an imported image, or remove it from Liveries."));
			continue;
		}
		for (int32 Before = 0; Before < Index; ++Before)
		{
			if (Paint.Liveries[Before].ToSoftObjectPath() == Paint.Liveries[Index].ToSoftObjectPath())
			{
				AddError(FString::Printf(TEXT("lists livery '%s' twice (%d and %d)."), *Texture->GetName(), Before, Index), TEXT("List each livery once."));
			}
		}
		FMantleValidationIssue Downscale;
		if (FMantlePlaceAssets::TextureDownscaleIssue(FString::Printf(TEXT("%s's livery '%s'"), *Subject, *Texture->GetName()),
			FMantlePlaceAssets::TextureSize(Texture), TextureLimits.MaxTextureSize, Downscale))
		{
			OutIssues.Add(Downscale);
		}
		if (!VehicleLiveryHasAlpha(*Texture))
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
				FString::Printf(TEXT("%s's livery '%s' has no alpha, so it covers the paint everywhere its layout reaches."), *Subject, *Texture->GetName()),
				TEXT("Give the image alpha that is clear where the paint should show, unless the livery is meant to cover the whole body.") });
		}
	}

	// The horn (#546): a sound wave of the creator's own, which publishes with the vehicle as a SoundEmitter's does, so it is held
	// to the same rule (FMantlePlaceAssets::CheckSoundRule). A library one cannot be named: a car without a horn sound plays Mantle's built-in horn.
	if (!T.HornSound.IsNull())
	{
		const USoundBase* Horn = T.HornSound.LoadSynchronous();
		FString Why;
		FString Fix;
		if (!Horn)
		{
			AddError(FString::Printf(TEXT("has a Horn Sound that does not load (%s)."), *T.HornSound.ToString()),
				TEXT("Set Horn Sound to a sound wave of your project, or clear it to use Mantle's built-in horn."));
		}
		else if (FMantlePlaceAssets::IsLibraryAsset(Horn))
		{
			AddError(FString::Printf(TEXT("has Horn Sound '%s', a Mantle library sound, which a vehicle cannot name."),
				*Horn->GetName()), TEXT("Clear Horn Sound to use Mantle's built-in horn, or import a horn sound of your own from a WAV file and set it as the Horn Sound; it publishes with the vehicle."));
		}
		else if (!FMantlePlaceAssets::CheckSoundRule(Horn, FMantlePlaceAssetLimits::FromContract(), Why, Fix))
		{
			AddError(FString::Printf(TEXT("has Horn Sound '%s', which %s."), *Horn->GetName(), *Why), Fix);
		}
	}

	// The body, as it uploads: bones, triangles, sections, each wheel on a bone and measurable.
	FMantleGlbMesh Body;
	TArray<UMaterialInterface*> Materials;
	FString ReadError;
	if (!ReadBody(*Authored, Body, Materials, ReadError, /*bApplyFitScale*/ true))
	{
		AddError(TEXT("cannot be read for publishing: ") + ReadError, TEXT("Fix what it names, then check again."));
		return;
	}
	const TArray<FString> SectionNames = UniqueSectionNames(Body.MaterialNames);
	for (int32 Index = 0; Index < Paint.Sections.Num(); ++Index)
	{
		const FString& Section = Paint.Sections[Index];
		if (!SectionNames.ContainsByPredicate([&Section](const FString& Name) { return Name.Equals(Section, ESearchCase::CaseSensitive); }))
		{
			AddError(FString::Printf(TEXT("paints section '%s', which its body does not have (its sections: %s)."), *Section, *FString::Join(SectionNames, TEXT(", "))),
				TEXT("Name one of the body's sections under Paint > Sections, as Convert Vehicle lists them."));
		}
		for (int32 Before = 0; Before < Index; ++Before)
		{
			if (Paint.Sections[Before].Equals(Section, ESearchCase::CaseSensitive))
			{
				AddError(FString::Printf(TEXT("paints section '%s' twice."), *Section), TEXT("List each section once under Paint > Sections."));
			}
		}
	}
	if (Body.Joints.Num() > FMantleVehicleLimits::MaxBones)
	{
		AddError(FString::Printf(TEXT("has a body with %d bones; a vehicle body has at most %d."), Body.Joints.Num(), FMantleVehicleLimits::MaxBones),
			TEXT("Remove the bones the vehicle does not need and re-import the body."));
	}
	int32 Triangles = 0;
	for (const FMantleGlbPrimitive& Primitive : Body.Primitives)
	{
		Triangles += Primitive.Indices.Num() / 3;
	}
	if (Triangles > FMantleVehicleLimits::MaxTriangles)
	{
		AddError(FString::Printf(TEXT("has %d triangles with its tires; a vehicle has at most %d."), Triangles, FMantleVehicleLimits::MaxTriangles),
			TEXT("Use a lower-detail body or tire mesh."));
	}
	if (Body.MaterialNames.Num() > FMantleVehicleLimits::MaxSections)
	{
		AddError(FString::Printf(TEXT("has %d materials with its tires; a vehicle has at most %d."), Body.MaterialNames.Num(), FMantleVehicleLimits::MaxSections),
			TEXT("Merge some of its materials, or give the tires one of the body's. Build a vehicle on a Mantle chassis carries a model's extra materials as parts for you."));
	}
	for (int32 Index = 0; Index < Materials.Num(); ++Index)
	{
		if (Materials[Index] && FMantlePlaceAssets::IsLibraryAsset(Materials[Index]))
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
				FString::Printf(TEXT("%s draws section %s with Mantle library material '%s', which a vehicle cannot name: it is drawn with Mantle's default surface."),
					*Subject, *Body.MaterialNames[Index], *Materials[Index]->GetName()),
				TEXT("Give that section a material of your own to publish its look.") });
		}
	}
	for (const FMantleVehicleSourceWheel& Wheel : Source->Wheels)
	{
		const int32 Joint = FMantleVehicleConverter::JointIndex(Body, Wheel.Bone);
		if (Joint == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("has a wheel on bone %s, which its body does not have."), *Wheel.Bone.ToString()),
				TEXT("Run Convert Vehicle on the body again, or fix the wheel's Bone."));
			continue;
		}
		if (Body.Joints[Joint].Parent == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("has a wheel on the root bone %s, which carries the chassis."), *Wheel.Bone.ToString()),
				TEXT("Give each wheel a bone of its own under the root."));
			continue;
		}
		double Radius = 0.0;
		double Width = 0.0;
		const bool bMeasured = MeasureWheel(Body, Wheel.Bone, Radius, Width);
		if (!bMeasured && (Wheel.Radius <= 0.f || Wheel.Width <= 0.f))
		{
			AddError(FString::Printf(TEXT("cannot measure wheel %s: no geometry is weighted to its bone alone."), *Wheel.Bone.ToString()),
				TEXT("Set the wheel's Tire Mesh (a separate tire), or its Radius and Width."));
			continue;
		}
		if (bMeasured && Wheel.Radius <= 0.f)
		{
			VehicleCheckRange(Subject, FString::Printf(TEXT("%s's measured radius"), *Wheel.Bone.ToString()), TEXT("wheels/*/radius"), Radius, OutIssues);
		}
		if (bMeasured && Wheel.Width <= 0.f)
		{
			VehicleCheckRange(Subject, FString::Printf(TEXT("%s's measured width"), *Wheel.Bone.ToString()), TEXT("wheels/*/width"), Width, OutIssues);
		}
	}

	// Suspension linkage (#435 b), as the cook holds it (vehicle.go linkageRules and resolveLinkage) against the body.
	const TArray<FMantleVehicleSourceLinkage>& Linkage = Source->Linkage;
	if (Linkage.Num() > FMantleVehicleLimits::MaxLinkageRules)
	{
		AddError(FString::Printf(TEXT("has %d linkage rules; a vehicle has at most %d."), Linkage.Num(), FMantleVehicleLimits::MaxLinkageRules),
			TEXT("Remove the rules for bones that need not move under Linkage in the vehicle asset."));
	}
	const TArray<FTransform> JointPose = VehicleJointPose(Body);
	// The root and every bone a wheel hangs from (the wheels' own bones included): no rule may move them.
	TArray<bool> Carrying;
	Carrying.Init(false, Body.Joints.Num());
	for (int32 Joint = 0; Joint < Body.Joints.Num(); ++Joint)
	{
		Carrying[Joint] = Body.Joints[Joint].Parent == INDEX_NONE;
	}
	for (const FMantleVehicleSourceWheel& Wheel : Source->Wheels)
	{
		for (int32 Joint = FMantleVehicleConverter::JointIndex(Body, Wheel.Bone); Joint != INDEX_NONE; Joint = Body.Joints[Joint].Parent)
		{
			Carrying[Joint] = true;
		}
	}
	for (int32 Index = 0; Index < Linkage.Num(); ++Index)
	{
		const FMantleVehicleSourceLinkage& Rule = Linkage[Index];
		const bool bAim = Rule.Type == EMantleVehicleLinkageType::Aim;
		const bool bStretch = Rule.Type == EMantleVehicleLinkageType::Stretch;
		const bool bCopy = Rule.Type == EMantleVehicleLinkageType::Copy;
		const FString What = FString::Printf(TEXT("linkage rule %d (%s %s)"), Index, VehicleLinkageTypeName(Rule.Type), *Rule.Bone.ToString());
		VehicleCheckRange(Subject, What + TEXT("'s Weight"), TEXT("linkage/*/weight"), Rule.Weight, OutIssues);

		struct FLinkageName
		{
			const TCHAR* Key;
			FName Bone;
			bool bMoves;
			bool bRequired;
		};
		const FLinkageName Names[] = {
			{ TEXT("Bone"), Rule.Bone, true, true },
			{ TEXT("Target"), bCopy ? NAME_None : Rule.Target, false, bAim },
			{ TEXT("Up"), bAim ? Rule.Up : NAME_None, false, false },
			{ TEXT("End"), bStretch ? Rule.End : NAME_None, true, bStretch },
			{ TEXT("Source"), bCopy ? Rule.Source : NAME_None, false, bCopy },
		};
		TArray<int32, TInlineAllocator<5>> Joints;
		for (const FLinkageName& Named : Names)
		{
			Joints.Add(INDEX_NONE);
			if (Named.Bone.IsNone())
			{
				if (Named.bRequired)
				{
					AddError(FString::Printf(TEXT("has %s with no %s."), *What, Named.Key), FString::Printf(TEXT("Set its %s to a bone of the body, or remove the rule."), Named.Key));
				}
				continue;
			}
			const int32 Joint = FMantleVehicleConverter::JointIndex(Body, Named.Bone);
			if (Joint == INDEX_NONE)
			{
				AddError(FString::Printf(TEXT("has %s naming bone %s, which its body does not have."), *What, *Named.Bone.ToString()),
					TEXT("Name one of the body's bones, or empty Linkage and run Convert Vehicle again to find the rules."));
				continue;
			}
			Joints.Last() = Joint;
			if (Named.bMoves && Carrying[Joint])
			{
				AddError(FString::Printf(TEXT("has %s moving %s, which is the root, a wheel's bone or a bone a wheel hangs from."), *What, *Named.Bone.ToString()),
					TEXT("Move a bone of the suspension instead: the simulation moves the wheels and the chassis."));
			}
		}
		// FName compares without regard to case, as the body's bone names do.
		for (const FLinkageName& Read : Names)
		{
			if (!Read.bMoves && !Read.Bone.IsNone() && (Read.Bone == Rule.Bone || (bStretch && Read.Bone == Rule.End)))
			{
				AddError(FString::Printf(TEXT("has %s reading %s, a bone it moves."), *What, *Read.Bone.ToString()), FString::Printf(TEXT("Set its %s to another bone."), Read.Key));
			}
		}
		if (bCopy && !Rule.bRotation && !Rule.bTranslation)
		{
			AddError(FString::Printf(TEXT("has %s copying neither rotation nor translation."), *What), TEXT("Tick Rotation, Translation or both."));
		}
		// Aim: Bone to Target; stretch: Bone to End. Nearer than 0.1 cm there is no direction to keep.
		const int32 From = Joints[0];
		const int32 To = bAim ? Joints[1] : (bStretch ? Joints[3] : INDEX_NONE);
		if (From != INDEX_NONE && To != INDEX_NONE && FVector::Dist(JointPose[From].GetLocation(), JointPose[To].GetLocation()) < 0.1)
		{
			AddError(FString::Printf(TEXT("has %s whose two bones sit at one place, so there is no direction to aim along."), *What),
				TEXT("Aim at a bone some way off (for a control arm, the hub its end sits on)."));
		}
	}

	// The parts the car clips turn (#546), as the cook holds them (vehicle.go carRules and resolveCarParts) against the body: each
	// on a bone of its own, which is the body's and none the simulation moves.
	struct FCarPart
	{
		const TCHAR* What;
		FName Bone;
	};
	TArray<FCarPart, TInlineAllocator<4>> CarParts;
	if (T.bSteeringWheel && !T.SteeringWheelBone.IsNone())
	{
		CarParts.Add({ TEXT("Steering Wheel"), T.SteeringWheelBone });
	}
	if (T.bHasDriverDoor)
	{
		CarParts.Add({ TEXT("Driver Door"), T.DriverDoor.Bone });
	}
	if (T.bPassengerSeat && T.bHasPassengerDoor)
	{
		CarParts.Add({ TEXT("Passenger Door"), T.PassengerDoor.Bone });
	}
	if (T.bTrunk)
	{
		CarParts.Add({ TEXT("Trunk Lid"), T.TrunkLid.Bone });
	}
	for (int32 Index = 0; Index < CarParts.Num(); ++Index)
	{
		const FCarPart& Part = CarParts[Index];
		if (Part.Bone.IsNone())
		{
			AddError(FString::Printf(TEXT("has a %s with no Bone."), Part.What), FString::Printf(TEXT("Set the %s's Bone to the body's bone it is on, or untick it."), Part.What));
			continue;
		}
		const int32 Joint = FMantleVehicleConverter::JointIndex(Body, Part.Bone);
		if (Joint == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("has a %s on bone %s, which its body does not have."), Part.What, *Part.Bone.ToString()),
				FString::Printf(TEXT("Set the %s's Bone to the body's bone it is on."), Part.What));
		}
		else if (Carrying[Joint])
		{
			AddError(FString::Printf(TEXT("has a %s on bone %s, which is the root, a wheel's bone or a bone a wheel hangs from."), Part.What, *Part.Bone.ToString()),
				TEXT("Give the part a bone of its own under the root, away from the wheels: the simulation moves the wheels and the chassis."));
		}
		// FName compares without regard to case, as the body's bone names do.
		for (int32 Before = 0; Before < Index; ++Before)
		{
			if (CarParts[Before].Bone == Part.Bone)
			{
				AddError(FString::Printf(TEXT("turns bone %s for both its %s and its %s."), *Part.Bone.ToString(), CarParts[Before].What, Part.What),
					TEXT("Give each part the car clips turn a bone of its own."));
			}
		}
	}

	// #612: a contact point on a bone is on one of the body's (vehicle.go resolveContacts).
	for (const bool bPassenger : { false, true })
	{
		const bool bWritten = bPassenger ? T.bPassengerSeat : T.bSeatLocation;
		const FMantleVehicleSourceSeatContacts& Contacts = bPassenger ? T.PassengerContacts : T.DriverContacts;
		if (!bWritten)
		{
			continue;
		}
		const TPair<const TCHAR*, const FMantleVehicleSourceContact*> Points[] = {
			{ TEXT("Pelvis"), Contacts.bPelvis ? &Contacts.Pelvis : nullptr },
			{ TEXT("Left Hand"), Contacts.Hands != EMantleVehicleHandsKind::None ? &Contacts.LeftHand : nullptr },
			{ TEXT("Right Hand"), Contacts.Hands != EMantleVehicleHandsKind::None ? &Contacts.RightHand : nullptr },
			{ TEXT("Left Foot"), Contacts.bFeet ? &Contacts.LeftFoot : nullptr },
			{ TEXT("Right Foot"), Contacts.bFeet ? &Contacts.RightFoot : nullptr } };
		for (const TPair<const TCHAR*, const FMantleVehicleSourceContact*>& Entry : Points)
		{
			if (Entry.Value && !Entry.Value->Bone.IsNone() && FMantleVehicleConverter::JointIndex(Body, Entry.Value->Bone) == INDEX_NONE)
			{
				AddError(FString::Printf(TEXT("has the %s's %s on bone %s, which its body does not have."), bPassenger ? TEXT("Passenger") : TEXT("Driver"),
					Entry.Key, *Entry.Value->Bone.ToString()),
					TEXT("Set the point's Bone to the body's bone it is on, or clear it for the vehicle's own space."));
			}
		}
	}

	// Attached parts (#435 item 2), as the cook holds them (vehicle.go part, partRules and resolveParts) and the upload names them.
	const TArray<FMantleVehicleSourcePart>& Parts = Source->Parts;
	if (Parts.Num() > FMantleVehicleLimits::MaxParts)
	{
		AddError(FString::Printf(TEXT("has %d attached parts; a vehicle has at most %d."), Parts.Num(), FMantleVehicleLimits::MaxParts),
			TEXT("Remove the parts it can do without under Parts in the vehicle asset, or merge small ones into the body."));
	}
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		const FMantleVehicleSourcePart& Part = Parts[Index];
		const UStaticMesh* PartMesh = Part.Mesh.LoadSynchronous();
		const FString What = FString::Printf(TEXT("part %d (%s)"), Index, PartMesh ? *PartMesh->GetName() : *Part.Mesh.ToString());
		if (!PartMesh)
		{
			AddError(FString::Printf(TEXT("has %s with no mesh that loads."), *What), TEXT("Set the part's Mesh to a static mesh of your project, or remove the part."));
			continue;
		}
		if (FMantlePlaceAssets::IsLibraryAsset(PartMesh))
		{
			AddError(FString::Printf(TEXT("has %s, a Mantle library mesh, which a vehicle cannot name."), *What),
				TEXT("Use a mesh of your own project for the part; it publishes with the vehicle."));
		}
		if (FMantleVehicleConverter::JointIndex(Body, Part.Bone) == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("has %s on bone %s, which its body does not have."), *What, *Part.Bone.ToString()),
				TEXT("Set the part's Bone to one of the body's bones (for a socket, the bone it is on), or empty Parts and run Convert Vehicle again."));
		}
		const FVector Location = Part.Transform.GetLocation();
		const FVector Scale = Part.Transform.GetScale3D();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			VehicleCheckRange(Subject, What + TEXT("'s location"), TEXT("parts/*/location/*"), Location[Axis], OutIssues);
			VehicleCheckRange(Subject, What + TEXT("'s scale"), TEXT("parts/*/scale/*"), Scale[Axis], OutIssues);
		}
		if (Scale.GetAbsMin() < FMantleVehicleLimits::PartMinScale)
		{
			AddError(FString::Printf(TEXT("has %s scaled to nothing on an axis."), *What), TEXT("Give each axis of the part's scale a size (a negative one mirrors it)."));
		}
		const TArray<int32> Slots = FMantlePlaceAssets::UploadSlots(PartMesh);
		if (Slots.Num() > FMantleVehicleLimits::MaxPartMaterials)
		{
			AddError(FString::Printf(TEXT("has %s with %d materials; a part's mesh has at most %d."), *What, Slots.Num(), FMantleVehicleLimits::MaxPartMaterials),
				TEXT("Merge some of the mesh's materials and re-import it."));
		}
		for (const int32 Slot : Slots)
		{
			const UMaterialInterface* Material = PartSlotMaterial(Part, PartMesh, Slot);
			if (Material && FMantlePlaceAssets::IsLibraryAsset(Material))
			{
				OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
					FString::Printf(TEXT("%s draws %s's slot %d with Mantle library material '%s', which a vehicle cannot name: it is drawn with Mantle's default surface."),
						*Subject, *What, Slot, *Material->GetName()),
					TEXT("Give that slot a material of your own to publish its look.") });
			}
		}
		// Its painted slots: each a slot of the mesh that draws something, once.
		for (int32 Painted = 0; Painted < Part.PaintSlots.Num(); ++Painted)
		{
			const int32 PaintSlot = Part.PaintSlots[Painted];
			if (!Slots.Contains(PaintSlot))
			{
				AddError(FString::Printf(TEXT("has %s painting slot %d, which its mesh does not draw with (its slots: %d)."), *What, PaintSlot,
					PartMesh->GetStaticMaterials().Num()), TEXT("Name one of the mesh's material slots under the part's Paint Slots, the first being 0."));
			}
			for (int32 Before = 0; Before < Painted; ++Before)
			{
				if (Part.PaintSlots[Before] == PaintSlot)
				{
					AddError(FString::Printf(TEXT("has %s painting slot %d twice."), *What, PaintSlot), TEXT("List each slot once under the part's Paint Slots."));
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Mantle.Vehicle.WriteUpload <vehicle asset> <file.glb>: the vehicle's upload as publishing writes it, without its
// materials or liveries (they upload separately), for the recipe's parity run: the
// backend cooks it to MVH1 (TestCookVehicleForParityRun) and the client's probe drives that. Uploads nothing.

namespace MantleVehicleConverterPrivate
{
	void WriteVehicleUpload(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_UPLOAD usage: Mantle.Vehicle.WriteUpload <vehicle asset path> <out.glb>"));
			return;
		}
		const UMantleVehicleSource* Source = LoadObject<UMantleVehicleSource>(nullptr, *Args[0]);
		if (!Source)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_UPLOAD %s is not a Mantle vehicle asset"), *Args[0]);
			return;
		}
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		for (const FMantleValidationIssue& Issue : Issues)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_UPLOAD check: %s"), *Issue.ToString());
		}
		FMantleGlbMesh Body;
		TArray<UMaterialInterface*> Materials;
		FString Error;
		if (!FMantleVehicleConverter::ReadBody(*Source, Body, Materials, Error, /*bApplyFitScale*/ true))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_UPLOAD %s"), *Error);
			return;
		}
		// Its livery choices name textures this file does not carry.
		const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>());
		Document->RemoveField(TEXT("liveryUV"));
		Document->RemoveField(TEXT("defaultLivery"));
		// Paint only on parts' slots (#435 item 2) paints nothing without the parts, which this file leaves out.
		if (Source->Paint.Sections.IsEmpty())
		{
			Document->RemoveField(TEXT("paint"));
		}
		TArray<uint8> Bytes;
		const FString Out = FPaths::ConvertRelativePathToFull(Args[1]);
		if (!FMantleVehicleConverter::WriteUpload(Body, Document, Bytes, Error) || !FFileHelper::SaveArrayToFile(Bytes, *Out))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_UPLOAD could not write %s: %s"), *Out, *Error);
			return;
		}
		// Its attached parts (#435 item 2) name mesh uploads this file does not carry: they are left out, as the materials are. The
		// chassis's collision boxes and centre of mass, which the parity run drives on, are in the document.
		UE_LOG(LogMantle, Display, TEXT("MANTLE_VEHICLE_UPLOAD wrote %s: %d bytes, %d wheels, %d linkage rules, %d collision boxes%s (%d attached parts left out)"),
			*Out, Bytes.Num(), Source->Wheels.Num(), Source->Linkage.Num(), Source->Tuning.Collision.Num(),
			Source->Tuning.bOverrideCenterOfMass ? TEXT(", a centre of mass") : TEXT(""), Source->Parts.Num());
	}

	FAutoConsoleCommand WriteVehicleUploadCommand(
		TEXT("Mantle.Vehicle.WriteUpload"),
		TEXT("Writes a Mantle vehicle asset's upload (its body and vehicle document, no materials) to a .glb, for the vehicle recipe's parity run. Uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&WriteVehicleUpload));
}

// ---------------------------------------------------------------------------------------------
// Mantle.Vehicle.Convert <Blueprint, actor class or skeletal mesh>: Convert Vehicle without the editor's windows, so a commandlet
// (-run=pythonscript, through unreal.SystemLibrary.execute_console_command) can convert a vehicle pack. An actor (a ChaosVehicles
// pawn's Blueprint) is spawned in a preview world of its own, never the open level, and its root skeletal mesh component (else its
// first) converted with the components around it (SurroundingsOfComponent); a skeletal mesh with its own (SurroundingsOfMesh). Saves
// <Mesh>_Vehicle beside the mesh and logs each problem and note.

namespace MantleVehicleConverterPrivate
{
	void ConvertVehicleCommand(const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_CONVERT usage: Mantle.Vehicle.Convert <Blueprint, actor class or skeletal mesh object path>"));
			return;
		}
		// "/Game/Cars/BP_Car" names the object of its package's own name.
		FString Path = Args[0];
		if (!Path.Contains(TEXT(".")))
		{
			Path += TEXT(".") + FPackageName::GetShortName(Path);
		}
		UObject* Loaded = LoadObject<UObject>(nullptr, *Path);
		const UBlueprint* Blueprint = Cast<UBlueprint>(Loaded);
		UClass* ActorClass = Blueprint ? Blueprint->GeneratedClass.Get() : Cast<UClass>(Loaded);
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Loaded);
		if (!Mesh && !(ActorClass && ActorClass->IsChildOf(AActor::StaticClass())))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_CONVERT %s is not a skeletal mesh, an actor Blueprint or an actor class"), *Path);
			return;
		}

		FMantleVehicleConversion Result;
		UMantleVehicleSource* Source = nullptr;
		if (Mesh)
		{
			Source = FMantleVehicleConverter::ConvertAsset(Mesh, Result);
		}
		else
		{
			FPreviewScene Scene;
			FActorSpawnParameters Spawn;
			Spawn.ObjectFlags = RF_Transient;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AActor* Actor = Scene.GetWorld() ? Scene.GetWorld()->SpawnActor<AActor>(ActorClass, FTransform::Identity, Spawn) : nullptr;
			USkeletalMeshComponent* Body = Actor ? Cast<USkeletalMeshComponent>(Actor->GetRootComponent()) : nullptr;
			Body = Body ? Body : (Actor ? Actor->FindComponentByClass<USkeletalMeshComponent>() : nullptr);
			Mesh = Body ? Body->GetSkeletalMeshAsset() : nullptr;
			if (Mesh)
			{
				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				TArray<const UActorComponent*> Around;
				for (const UActorComponent* Component : Components)
				{
					Around.Add(Component);
				}
				const FMantleVehicleSurroundings Surroundings = FMantleVehicleConverter::SurroundingsOfComponent(*Body, Around);
				UE_LOG(LogMantle, Display, TEXT("MANTLE_VEHICLE_CONVERT %s: body %s, %d attached mesh(es), %d wheel setup(s)"), *Path, *Mesh->GetPathName(),
					Surroundings.Attached.Num(), Surroundings.WheelSetups.Num());
				Source = FMantleVehicleConverter::ConvertAsset(Mesh, Result, &Surroundings);
			}
			else
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_CONVERT %s: %s"), *Path, Actor ? TEXT("the actor has no skeletal mesh component with a mesh") : TEXT("the actor could not be spawned"));
			}
			if (Actor)
			{
				Actor->Destroy();
			}
			if (!Mesh)
			{
				return;
			}
		}

		for (const FString& Problem : Result.Problems)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_CONVERT problem: %s"), *Problem);
		}
		for (const FString& Note : Result.Notes)
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_VEHICLE_CONVERT note: %s"), *Note);
		}
		if (!Source)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_CONVERT %s did not convert"), *Mesh->GetPathName());
			return;
		}
		UPackage* Package = Source->GetOutermost();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Save;
		Save.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, Source, *File, Save))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_VEHICLE_CONVERT could not save %s"), *File);
			return;
		}
		UE_LOG(LogMantle, Display, TEXT("MANTLE_VEHICLE_CONVERT saved %s: %d wheels, %d parts, %d linkage rules, %d collision boxes"), *Source->GetPathName(),
			Source->Wheels.Num(), Source->Parts.Num(), Source->Linkage.Num(), Source->Tuning.Collision.Num());
	}

	FAutoConsoleCommand ConvertVehicleConsoleCommand(
		TEXT("Mantle.Vehicle.Convert"),
		TEXT("Converts a vehicle (an actor Blueprint or class, spawned in a preview world, or a skeletal mesh) to <Mesh>_Vehicle, saves it and logs the notes."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&ConvertVehicleCommand));
}
