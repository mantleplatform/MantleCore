// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarPropsFixer.h"
#include "MantleAvatarReposeFixer.h"
#include "MantleOutfitSections.h"
#include "MantleRuntime.h"

#include "Animation/MorphTarget.h"
#include "AssetToolsModule.h"
#include "ClothingAssetBase.h"
#include "Engine/SkeletalMesh.h"
#include "IAssetTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialInterface.h"
#include "Algo/Accumulate.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarPropsFixerPrivate
{
	bool PropsIsAvatar(const FMantleConvertPart& Part)
	{
		return Part.Mesh.IsValid() && Part.Slot.Equals(TEXT("avatar"), ESearchCase::CaseSensitive);
	}

	/**
	 * An arm or hand bone: upperarm_l, lowerArm_topPlateA_l, LeftForeArm, hand_r, bot_hand. A bare "arm" needs a side
	 * (Wraith's canon_arms is the shoulder cannon's mount, not a limb).
	 */
	/** Owner 2026-10-14: a chain or whip held in the hand is a weapon (Riktor's chain whip). Only for held props here. */
	bool PropsIsHeldWeaponWord(const FName Name)
	{
		for (const FString& Word : FMantleOutfitSections::Words(Name.ToString()))
		{
			if (Word == TEXT("chain") || Word == TEXT("chains") || Word == TEXT("whip"))
			{
				return true;
			}
		}
		return false;
	}

	bool PropsIsArm(const FName Name)
	{
		// A chain named after the arm it hangs from is the chain, not the arm (Riktor's arm_chain_long_r_08, l_hand_chain_09).
		if (PropsIsHeldWeaponWord(Name))
		{
			return false;
		}
		static const TCHAR* const LimbWords[] = { TEXT("upperarm"), TEXT("lowerarm"), TEXT("forearm"), TEXT("hand"), TEXT("wrist") };
		static const TCHAR* const SideWords[] = { TEXT("l"), TEXT("r"), TEXT("left"), TEXT("right") };
		bool bArm = false;
		bool bSide = false;
		for (const FString& Word : FMantleOutfitSections::Words(Name.ToString()))
		{
			for (const TCHAR* Limb : LimbWords)
			{
				if (Word.Equals(Limb, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			bArm |= Word.Equals(TEXT("arm"), ESearchCase::IgnoreCase);
			for (const TCHAR* Side : SideWords)
			{
				bSide |= Word.Equals(Side, ESearchCase::IgnoreCase);
			}
		}
		return bArm && bSide;
	}

	/** A head, jaw or mouth bone (head, jaw, C_jaw), unless its own name says prop. */
	bool PropsIsHead(const FName Name)
	{
		static const TCHAR* const HeadWords[] = { TEXT("head"), TEXT("jaw"), TEXT("mouth") };
		const FString Text = Name.ToString();
		if (FMantleOutfitSections::IsPropName(Text))
		{
			return false;
		}
		for (const FString& Word : FMantleOutfitSections::Words(Text))
		{
			for (const TCHAR* Head : HeadWords)
			{
				if (Word.Equals(Head, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	/** Something held in the mouth: Feng Mao's pipe. */
	bool PropsIsMouthProp(const FName Name)
	{
		static const TCHAR* const MouthWords[] = { TEXT("pipe"), TEXT("cigar"), TEXT("cigars"), TEXT("cigarette"), TEXT("cigarettes") };
		for (const FString& Word : FMantleOutfitSections::Words(Name.ToString()))
		{
			for (const TCHAR* Mouth : MouthWords)
			{
				if (Word.Equals(Mouth, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	void PropsReadSkeleton(const USkeletalMesh& Mesh, TArray<FName>& OutNames, TArray<int32>& OutParents)
	{
		for (const FMeshBoneInfo& Info : Mesh.GetRefSkeleton().GetRawRefBoneInfo())
		{
			OutNames.Add(Info.Name);
			OutParents.Add(Info.ParentIndex);
		}
	}

	/** LOD's geometry as a description the caller owns (the source is never touched); false when it has none. */
	bool PropsReadLOD(const USkeletalMesh& Mesh, const int32 LOD, FMeshDescription& Out)
	{
		if (Mesh.HasMeshDescription(LOD))
		{
			return Mesh.CloneMeshDescription(LOD, Out);
		}
		const FSkeletalMeshModel* Imported = const_cast<USkeletalMesh&>(Mesh).GetImportedModel();
		if (!Imported || !Imported->LODModels.IsValidIndex(LOD))
		{
			return false;
		}
		FSkeletalMeshAttributes(Out).Register();
		Imported->LODModels[LOD].GetMeshDescription(&Mesh, LOD, Out);
		return true;
	}

	/** The polygons of the sections Pick names deleted from Description (their groups too); how many triangles went. */
	int32 PropsDeleteSections(FMeshDescription& Description, TFunctionRef<bool(FName)> Pick, TArray<FName>& OutSections)
	{
		FSkeletalMeshAttributes Attributes(Description);
		const TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		TArray<FPolygonID> Polygons;
		TArray<FPolygonGroupID> Groups;
		int32 Triangles = 0;
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			if (Pick(SlotNames.Get(Group)))
			{
				Groups.Add(Group);
				OutSections.Add(SlotNames.Get(Group));
				for (const FPolygonID Polygon : Description.GetPolygonGroupPolygonIDs(Group))
				{
					Polygons.Add(Polygon);
					Triangles += Description.GetPolygonTriangles(Polygon).Num();
				}
			}
		}
		if (Groups.Num() > 0)
		{
			Description.DeletePolygons(Polygons);
			for (const FPolygonGroupID Group : Groups)
			{
				if (Description.IsPolygonGroupValid(Group) && Description.GetPolygonGroupPolygonIDs(Group).Num() == 0)
				{
					Description.DeletePolygonGroup(Group);
				}
			}
			FElementIDRemappings Remappings;
			Description.Compact(Remappings);
		}
		return Triangles;
	}

	/** A material graph value as far as it is known: a constant, or provably 0 everywhere (bZero) though not constant. */
	struct FPropsValue
	{
		TOptional<float> Value;
		bool bZero = false;

		static FPropsValue Of(const float In) { FPropsValue Out; Out.Value = In; Out.bZero = In == 0.0f; return Out; }
		static FPropsValue Zero() { FPropsValue Out; Out.bZero = true; return Out; }
		static FPropsValue Unknown() { return FPropsValue(); }
	};

	FPropsValue PropsEvaluate(const UMaterialInterface& Material, const UMaterialExpression* Expression, int32 OutputIndex, int32 Depth);

	/** An expression input: its expression's value, or the node's constant when nothing is connected. */
	FPropsValue PropsInput(const UMaterialInterface& Material, const FExpressionInput& Input, const float Fallback, const int32 Depth)
	{
		return Input.Expression ? PropsEvaluate(Material, Input.Expression, Input.OutputIndex, Depth + 1) : FPropsValue::Of(Fallback);
	}

	FPropsValue PropsInput(const UMaterialInterface& Material, const FExpressionInput& Input, const int32 Depth)
	{
		return Input.Expression ? PropsEvaluate(Material, Input.Expression, Input.OutputIndex, Depth + 1) : FPropsValue::Unknown();
	}

	/**
	 * #46: what a scalar node of a material's graph gives at the material's own parameter values, folded only as far as it
	 * can be proven: constants and scalar parameters, and the arithmetic that keeps a 0 a 0 (a product with a 0, a lerp
	 * between 0s, a depth fade of 0, a clamp or power of 0). Anything else (textures, functions, masks of vectors) is
	 * unknown, so a section is only ever called hidden when its graph proves it.
	 */
	FPropsValue PropsEvaluate(const UMaterialInterface& Material, const UMaterialExpression* Expression, const int32 OutputIndex, const int32 Depth)
	{
		if (!Expression || Depth > 64)
		{
			return FPropsValue::Unknown();
		}
		if (const UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
		{
			return FPropsValue::Of(Constant->R);
		}
		if (const UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
		{
			float Value = Parameter->DefaultValue;
			Material.GetScalarParameterValue(FHashedMaterialParameterInfo(Parameter->ParameterName), Value);
			return FPropsValue::Of(Value);
		}
		if (const UMaterialExpressionReroute* Reroute = Cast<UMaterialExpressionReroute>(Expression))
		{
			return PropsInput(Material, Reroute->Input, Depth);
		}
		if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
		{
			const FPropsValue A = PropsInput(Material, Multiply->A, Multiply->ConstA, Depth);
			const FPropsValue B = PropsInput(Material, Multiply->B, Multiply->ConstB, Depth);
			if (A.Value.IsSet() && B.Value.IsSet())
			{
				return FPropsValue::Of(A.Value.GetValue() * B.Value.GetValue());
			}
			return A.bZero || B.bZero ? FPropsValue::Zero() : FPropsValue::Unknown();
		}
		if (const UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
		{
			const FPropsValue A = PropsInput(Material, Divide->A, Divide->ConstA, Depth);
			const FPropsValue B = PropsInput(Material, Divide->B, Divide->ConstB, Depth);
			if (A.Value.IsSet() && B.Value.IsSet() && B.Value.GetValue() != 0.0f)
			{
				return FPropsValue::Of(A.Value.GetValue() / B.Value.GetValue());
			}
			return FPropsValue::Unknown();
		}
		if (const UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
		{
			const FPropsValue A = PropsInput(Material, Add->A, Add->ConstA, Depth);
			const FPropsValue B = PropsInput(Material, Add->B, Add->ConstB, Depth);
			if (A.Value.IsSet() && B.Value.IsSet())
			{
				return FPropsValue::Of(A.Value.GetValue() + B.Value.GetValue());
			}
			return A.bZero && B.bZero ? FPropsValue::Zero() : FPropsValue::Unknown();
		}
		if (const UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Expression))
		{
			const FPropsValue A = PropsInput(Material, Subtract->A, Subtract->ConstA, Depth);
			const FPropsValue B = PropsInput(Material, Subtract->B, Subtract->ConstB, Depth);
			if (A.Value.IsSet() && B.Value.IsSet())
			{
				return FPropsValue::Of(A.Value.GetValue() - B.Value.GetValue());
			}
			return A.bZero && B.bZero ? FPropsValue::Zero() : FPropsValue::Unknown();
		}
		if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
		{
			const FPropsValue A = PropsInput(Material, Lerp->A, Lerp->ConstA, Depth);
			const FPropsValue B = PropsInput(Material, Lerp->B, Lerp->ConstB, Depth);
			const FPropsValue Alpha = PropsInput(Material, Lerp->Alpha, Lerp->ConstAlpha, Depth);
			if (Alpha.Value.IsSet() && Alpha.Value.GetValue() == 0.0f)
			{
				return A;
			}
			if (Alpha.Value.IsSet() && Alpha.Value.GetValue() == 1.0f)
			{
				return B;
			}
			if (A.Value.IsSet() && B.Value.IsSet() && Alpha.Value.IsSet())
			{
				return FPropsValue::Of(FMath::Lerp(A.Value.GetValue(), B.Value.GetValue(), Alpha.Value.GetValue()));
			}
			return A.bZero && B.bZero ? FPropsValue::Zero() : FPropsValue::Unknown();
		}
		if (const UMaterialExpressionDepthFade* Fade = Cast<UMaterialExpressionDepthFade>(Expression))
		{
			// The opacity times a 0..1 fade by scene depth: 0 stays 0, anything else depends on the scene.
			return PropsInput(Material, Fade->InOpacity, Fade->OpacityDefault, Depth).bZero ? FPropsValue::Zero() : FPropsValue::Unknown();
		}
		if (const UMaterialExpressionSaturate* Saturate = Cast<UMaterialExpressionSaturate>(Expression))
		{
			const FPropsValue In = PropsInput(Material, Saturate->Input, Depth);
			return In.Value.IsSet() ? FPropsValue::Of(FMath::Clamp(In.Value.GetValue(), 0.0f, 1.0f)) : In.bZero ? FPropsValue::Zero() : FPropsValue::Unknown();
		}
		if (const UMaterialExpressionClamp* Clamp = Cast<UMaterialExpressionClamp>(Expression))
		{
			const FPropsValue In = PropsInput(Material, Clamp->Input, Depth);
			const FPropsValue Min = PropsInput(Material, Clamp->Min, Clamp->MinDefault, Depth);
			const FPropsValue Max = PropsInput(Material, Clamp->Max, Clamp->MaxDefault, Depth);
			if (!Min.Value.IsSet() || !Max.Value.IsSet())
			{
				return FPropsValue::Unknown();
			}
			const float Low = Clamp->ClampMode == CMODE_ClampMax ? -UE_BIG_NUMBER : Min.Value.GetValue();
			const float High = Clamp->ClampMode == CMODE_ClampMin ? UE_BIG_NUMBER : Max.Value.GetValue();
			if (In.Value.IsSet())
			{
				return FPropsValue::Of(FMath::Clamp(In.Value.GetValue(), Low, High));
			}
			return In.bZero && Low <= 0.0f && High >= 0.0f ? FPropsValue::Zero() : FPropsValue::Unknown();
		}
		if (const UMaterialExpressionPower* Power = Cast<UMaterialExpressionPower>(Expression))
		{
			const FPropsValue BaseValue = PropsInput(Material, Power->Base, Depth);
			const FPropsValue Exponent = PropsInput(Material, Power->Exponent, Power->ConstExponent, Depth);
			if (BaseValue.Value.IsSet() && Exponent.Value.IsSet())
			{
				return FPropsValue::Of(FMath::Pow(FMath::Max(BaseValue.Value.GetValue(), 0.0f), Exponent.Value.GetValue()));
			}
			return BaseValue.bZero && Exponent.Value.IsSet() && Exponent.Value.GetValue() > 0.0f ? FPropsValue::Zero() : FPropsValue::Unknown();
		}
		return FPropsValue::Unknown();
	}

	/** LOD 0's points on a grid, removed and kept, for matching a lower LOD's points to them (CutProps' Reference). */
	struct FPropsNearest
	{
		FPropsNearest(const FMantleAvatarPropsFixer::FPropCut& Reference, const float InReach)
			: Reach(InReach), Cell(FMath::Max(InReach * 0.5f, 0.5f))
		{
			auto Add = [this](const TArray<FVector3f>& From, const bool bRemoved)
			{
				for (const FVector3f& Point : From)
				{
					Cells.FindOrAdd(Key(Point)).Add(Points.Num());
					Points.Add(Point);
					Removed.Add(bRemoved);
				}
			};
			Add(Reference.RemovedPoints, true);
			Add(Reference.KeptPoints, false);
		}

		/** Whether Point goes with LOD 0's cut: its nearest removed and kept LOD 0 points, within Reach (IsWhereRemoved). */
		bool IsRemoved(const FVector3f& Point) const
		{
			const FIntVector Center = Key(Point);
			const int32 Rings = FMath::CeilToInt(Reach / Cell);
			float BestRemoved = Reach * Reach;
			float BestKept = Reach * Reach;
			bool bRemoved = false;
			bool bKept = false;
			for (int32 X = -Rings; X <= Rings; ++X)
			{
				for (int32 Y = -Rings; Y <= Rings; ++Y)
				{
					for (int32 Z = -Rings; Z <= Rings; ++Z)
					{
						if (const TArray<int32>* Found = Cells.Find(Center + FIntVector(X, Y, Z)))
						{
							for (const int32 Index : *Found)
							{
								const float Distance = FVector3f::DistSquared(Points[Index], Point);
								float& Best = Removed[Index] ? BestRemoved : BestKept;
								if (Distance <= Best)
								{
									Best = Distance;
									(Removed[Index] ? bRemoved : bKept) = true;
								}
							}
						}
					}
				}
			}
			return FMantleAvatarPropsFixer::IsWhereRemoved(bRemoved ? TOptional<float>(FMath::Sqrt(BestRemoved)) : TOptional<float>(),
				bKept ? TOptional<float>(FMath::Sqrt(BestKept)) : TOptional<float>(), Reach);
		}

	private:
		FIntVector Key(const FVector3f& Point) const
		{
			return FIntVector(FMath::FloorToInt(Point.X / Cell), FMath::FloorToInt(Point.Y / Cell), FMath::FloorToInt(Point.Z / Cell));
		}

		float Reach;
		float Cell;
		TMap<FIntVector, TArray<int32>> Cells;
		TArray<FVector3f> Points;
		TArray<bool> Removed;
	};

	/** The material a LOD's section (a polygon group, by its slot name) draws with; null when no slot has the name. */
	UMaterialInterface* PropsSectionMaterial(const USkeletalMesh& Mesh, const FName SlotName)
	{
		for (const FSkeletalMaterial& Material : Mesh.GetMaterials())
		{
			if (Material.ImportedMaterialSlotName == SlotName || Material.MaterialSlotName == SlotName)
			{
				return Material.MaterialInterface;
			}
		}
		return nullptr;
	}

	/** The sections of Description whose material draws nothing at its own values (FMantleAvatarPropsFixer::IsHidden). */
	TArray<FName> PropsHiddenSections(const USkeletalMesh& Mesh, FMeshDescription& Description, TArray<FString>* OutWhy = nullptr)
	{
		FSkeletalMeshAttributes Attributes(Description);
		const TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		TArray<FName> Hidden;
		TSet<FName> Seen;
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			const FName Slot = SlotNames.Get(Group);
			UMaterialInterface* Material = PropsSectionMaterial(Mesh, Slot);
			if (Description.GetPolygonGroupPolygonIDs(Group).Num() == 0 || !Material || Seen.Contains(Slot))
			{
				continue;
			}
			Seen.Add(Slot);
			const bool bMasked = IsMaskedBlendMode(Material->GetBlendMode());
			const TOptional<float> Opacity = FMantleAvatarPropsFixer::KnownOpacity(Material);
			if (Opacity.IsSet())
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASIS_HIDDEN %s %s (%s): its graph gives %s %.3f"), *Mesh.GetName(), *Slot.ToString(), *Material->GetName(),
					bMasked ? TEXT("an opacity mask of") : TEXT("an opacity of"), Opacity.GetValue());
			}
			if (Opacity.IsSet() && FMantleAvatarPropsFixer::IsHidden(bMasked, Opacity.GetValue(), Material->GetOpacityMaskClipValue()))
			{
				Hidden.Add(Slot);
				if (OutWhy)
				{
					OutWhy->Add(FString::Printf(TEXT("%s (%s: %s %.2f)"), *Slot.ToString(), *Material->GetName(),
						bMasked ? TEXT("opacity mask") : TEXT("opacity"), Opacity.GetValue()));
				}
			}
		}
		return Hidden;
	}

	/** A new asset path next to the output: <Folder>/<Base><Suffix>, made unique. */
	void PropsNewAssetPath(const FString& Folder, const FString& Base, FString& OutPackagePath, FString& OutAssetName)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		FString PackageName;
		AssetTools.CreateUniqueAssetName(Folder / Base, FString(), PackageName, OutAssetName);
		OutPackagePath = FPackageName::GetLongPackagePath(PackageName);
	}

	TArray<FName> PropsMorphNames(const USkeletalMesh& Mesh)
	{
		TArray<FName> Names;
		for (const UMorphTarget* Morph : Mesh.GetMorphTargets())
		{
			if (Morph)
			{
				Names.Add(Morph->GetFName());
			}
		}
		return Names;
	}

	FString PropsNameList(TConstArrayView<FName> Names, int32 Most);

	/**
	 * The morph targets Copy kept of Source's, logged; a lost one is named in the summary. Only a morph that moved nothing
	 * but removed geometry may go (a prop's own shape key); anything else is the step breaking the mesh.
	 */
	void PropsCheckMorphs(const USkeletalMesh& Source, USkeletalMesh& Copy, FMantleConvertContext& Context)
	{
		TArray<USkinnedAsset*> Compiling = { &Copy };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
		const TArray<FName> Before = PropsMorphNames(Source);
		const TArray<FName> After = PropsMorphNames(Copy);
		const TArray<FName> Lost = FMantleAvatarPropsFixer::LostMorphs(Before, After);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASIS_MORPHS %s -> %s: %d of %d morph targets kept%s%s"), *Source.GetName(), *Copy.GetName(), Before.Num() - Lost.Num(),
			Before.Num(), Lost.Num() > 0 ? TEXT("; lost ") : TEXT(""), *PropsNameList(Lost, 16));
		if (Lost.Num() > 0)
		{
			Context.Notes.Add(FString::Printf(TEXT("%s: %d morph target%s did not carry over to %s (%s): they moved only what was removed, or check the avatar's expressions."),
				*Source.GetName(), Lost.Num(), Lost.Num() == 1 ? TEXT("") : TEXT("s"), *Copy.GetName(), *PropsNameList(Lost, 8)));
		}
	}

	FString PropsNameList(TConstArrayView<FName> Names, const int32 Most)
	{
		FString Out;
		for (int32 Index = 0; Index < Names.Num() && Index < Most; ++Index)
		{
			Out += (Index > 0 ? TEXT(", ") : TEXT("")) + Names[Index].ToString();
		}
		if (Names.Num() > Most)
		{
			Out += FString::Printf(TEXT(" and %d more"), Names.Num() - Most);
		}
		return Out;
	}
}

TArray<bool> FMantleAvatarPropsFixer::PropBones(TConstArrayView<FName> Names, TConstArrayView<int32> Parents)
{
	using namespace MantleAvatarPropsFixerPrivate;
	TArray<bool> Prop;
	Prop.Init(false, Names.Num());
	for (int32 Bone = 0; Bone < Names.Num(); ++Bone)
	{
		// Up from the bone to the first arm or hand bone: a prop word on the way, and an arm at the end of it. Or up to the
		// head or jaw with a mouth-prop word on the way (a pipe; a helmet's "blade" is no mouth prop, so it stays).
		bool bPropWord = false;
		bool bMouthWord = false;
		for (int32 Walk = Bone, Steps = 0; Walk != INDEX_NONE && Names.IsValidIndex(Walk) && Steps < Names.Num(); ++Steps)
		{
			if (PropsIsArm(Names[Walk]))
			{
				Prop[Bone] = bPropWord;
				break;
			}
			if (PropsIsHead(Names[Walk]))
			{
				Prop[Bone] = bMouthWord;
				break;
			}
			// A shoulder blade is anatomy or armour, not a blade (Wukong's and Feng Mao's shoulder_blade_*).
			bPropWord |= (FMantleOutfitSections::IsPropName(Names[Walk].ToString()) && !FMantleOutfitSections::Words(Names[Walk].ToString()).Contains(TEXT("shoulder")))
				|| PropsIsHeldWeaponWord(Names[Walk]);
			bMouthWord |= PropsIsMouthProp(Names[Walk]);
			Walk = Parents.IsValidIndex(Walk) ? Parents[Walk] : INDEX_NONE;
		}
	}
	return Prop;
}

bool FMantleAvatarPropsFixer::IsWhereRemoved(const TOptional<float> RemovedCm, const TOptional<float> KeptCm, const float ReachCm, const float MarginCm)
{
	if (!RemovedCm.IsSet() || RemovedCm.GetValue() > ReachCm)
	{
		return false;
	}
	return !KeptCm.IsSet() || RemovedCm.GetValue() + MarginCm <= KeptCm.GetValue();
}

TArray<FName> FMantleAvatarPropsFixer::LostMorphs(TConstArrayView<FName> Before, TConstArrayView<FName> After)
{
	TArray<FName> Lost;
	for (const FName Morph : Before)
	{
		if (!After.Contains(Morph))
		{
			Lost.AddUnique(Morph);
		}
	}
	return Lost;
}

bool FMantleAvatarPropsFixer::IsHidden(const bool bMasked, const float MaxOpacity, const float ClipValue)
{
	// A masked section shows where its mask reaches the clip value; a translucent one at any opacity above nothing.
	return bMasked ? MaxOpacity < ClipValue : MaxOpacity <= HiddenOpacity;
}

TOptional<float> FMantleAvatarPropsFixer::KnownOpacity(const UMaterialInterface* Material)
{
	using namespace MantleAvatarPropsFixerPrivate;
	const UMaterial* Base = Material ? Material->GetMaterial_Concurrent() : nullptr;
	const UMaterialEditorOnlyData* Graph = Base ? Base->GetEditorOnlyData() : nullptr;
	if (!Graph)
	{
		return TOptional<float>();
	}
	const EBlendMode Blend = Material->GetBlendMode();
	if (Blend == BLEND_Opaque || Blend == BLEND_Additive || Blend == BLEND_Modulate || Base->bUseMaterialAttributes)
	{
		return TOptional<float>();
	}
	const FScalarMaterialInput& Input = IsMaskedBlendMode(Blend) ? Graph->OpacityMask : Graph->Opacity;
	if (!Input.Expression)
	{
		return Input.UseConstant ? TOptional<float>(Input.Constant) : TOptional<float>();
	}
	const FPropsValue Value = PropsEvaluate(*Material, Input.Expression, Input.OutputIndex, 0);
	return Value.bZero ? TOptional<float>(0.0f) : Value.Value;
}

bool FMantleAvatarPropsFixer::IsClothSimSlot(const FString& SlotName)
{
	bool bCloth = false;
	bool bSim = false;
	for (const FString& Word : FMantleOutfitSections::Words(SlotName))
	{
		bCloth |= Word == TEXT("cloth") || Word == TEXT("clothsim");
		bSim |= Word == TEXT("sim") || Word == TEXT("simulation") || Word == TEXT("clothsim");
	}
	return bCloth && bSim;
}

bool FMantleAvatarPropsFixer::CutProps(FMeshDescription& Description, TConstArrayView<FName> RefNames, TConstArrayView<int32> RefParents, const bool bDelete,
	FPropCut& OutCut, const FPropCut* Reference)
{
	using namespace MantleAvatarPropsFixerPrivate;
	OutCut = FPropCut();
	FSkeletalMeshAttributes Attributes(Description);
	FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights();
	if (!Weights.IsValid())
	{
		return false;
	}
	const TArray<bool> PropRef = PropBones(RefNames, RefParents);
	if (!PropRef.Contains(true) && !(Reference && Reference->RemovedPoints.Num() > 0))
	{
		return true;
	}
	// The description's own bones, when it names them, to reference bones by name; otherwise the same indices.
	TArray<int32> RefOfBone;
	const bool bNamedBones = Attributes.HasBoneNameAttribute() && Attributes.GetNumBones() > 0;
	if (bNamedBones)
	{
		FSkeletalMeshAttributes::FBoneNameAttributesRef BoneNames = Attributes.GetBoneNames();
		RefOfBone.Init(INDEX_NONE, Attributes.Bones().GetArraySize());
		for (const FBoneID Bone : Attributes.Bones().GetElementIDs())
		{
			RefOfBone[Bone.GetValue()] = RefNames.IndexOfByKey(BoneNames.Get(Bone));
		}
	}
	auto RefBone = [&RefOfBone, bNamedBones, &RefNames](const int32 Index)
	{
		const int32 Ref = bNamedBones ? (RefOfBone.IsValidIndex(Index) ? RefOfBone[Index] : INDEX_NONE) : Index;
		return Ref >= 0 && Ref < RefNames.Num() ? Ref : INDEX_NONE;
	};

	// Each vertex's weight on prop bones, and its whole weight.
	TArray<FVector2f> Share;
	Share.Init(FVector2f::ZeroVector, Description.Vertices().GetArraySize());
	TSet<int32> UsedProps;
	for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
	{
		for (const UE::AnimationCore::FBoneWeight Influence : Weights.Get(Vertex))
		{
			const int32 Ref = RefBone(Influence.GetBoneIndex());
			const float Weight = Influence.GetWeight();
			Share[Vertex.GetValue()].Y += Weight;
			if (Ref != INDEX_NONE && PropRef[Ref] && Weight > 0.0f)
			{
				Share[Vertex.GetValue()].X += Weight;
				UsedProps.Add(Ref);
			}
		}
	}
	auto CornerShare = [&Share](const FVertexID Vertex)
	{
		const FVector2f& S = Share[Vertex.GetValue()];
		return S.Y > 0.0f ? S.X / S.Y : 0.0f;
	};

	const TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	TArray<FPolygonID> Doomed;
	TArray<FPolygonGroupID> DoomedGroups;
	for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
	{
		double Prop = 0.0;
		double Total = 0.0;
		const TArray<FPolygonID> Polygons(Description.GetPolygonGroupPolygonIDs(Group));
		for (const FPolygonID Polygon : Polygons)
		{
			for (const FVertexInstanceID Corner : Description.GetPolygonVertexInstances(Polygon))
			{
				const FVector2f& S = Share[Description.GetVertexInstanceVertex(Corner).GetValue()];
				Prop += S.X;
				Total += S.Y;
			}
		}
		if (Total > 0.0 && Prop / Total >= PropSectionShare)
		{
			OutCut.Sections.Add(SlotNames.Get(Group));
			int32 Triangles = 0;
			for (const FPolygonID Polygon : Polygons)
			{
				Triangles += Description.GetPolygonTriangles(Polygon).Num();
			}
			OutCut.SectionTriangles.Add(Triangles);
			Doomed.Append(Polygons);
			DoomedGroups.Add(Group);
			continue;
		}
		for (const FPolygonID Polygon : Polygons)
		{
			bool bHeld = true;
			for (const FVertexInstanceID Corner : Description.GetPolygonVertexInstances(Polygon))
			{
				bHeld &= CornerShare(Description.GetVertexInstanceVertex(Corner)) >= PropCornerShare;
			}
			if (bHeld)
			{
				Doomed.Add(Polygon);
				OutCut.LooseTriangles += Description.GetPolygonTriangles(Polygon).Num();
			}
		}
	}
	// A lower LOD: what lies where LOD 0's props were (CutProps' Reference).
	if (Reference && Reference->RemovedPoints.Num() > 0)
	{
		const FPropsNearest Nearest(*Reference, ReferenceReachCm);
		TSet<FPolygonID> Already(Doomed);
		TVertexAttributesRef<FVector3f> Positions = FSkeletalMeshAttributes(Description).GetVertexPositions();
		for (const FPolygonID Polygon : Description.Polygons().GetElementIDs())
		{
			if (Already.Contains(Polygon))
			{
				continue;
			}
			bool bWhereProp = true;
			for (const FVertexInstanceID Corner : Description.GetPolygonVertexInstances(Polygon))
			{
				bWhereProp &= Nearest.IsRemoved(Positions.Get(Description.GetVertexInstanceVertex(Corner)));
				if (!bWhereProp)
				{
					break;
				}
			}
			if (bWhereProp)
			{
				Doomed.Add(Polygon);
				OutCut.LooseTriangles += Description.GetPolygonTriangles(Polygon).Num();
			}
		}
	}
	for (const int32 Ref : UsedProps)
	{
		OutCut.Bones.Add(RefNames[Ref]);
	}
	// Where the points were, for the lower LODs to match against.
	{
		TVertexAttributesRef<FVector3f> Positions = FSkeletalMeshAttributes(Description).GetVertexPositions();
		TSet<FVertexID> Gone;
		for (const FPolygonID Polygon : Doomed)
		{
			for (const FVertexInstanceID Corner : Description.GetPolygonVertexInstances(Polygon))
			{
				Gone.Add(Description.GetVertexInstanceVertex(Corner));
			}
		}
		for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
		{
			(Gone.Contains(Vertex) ? OutCut.RemovedPoints : OutCut.KeptPoints).Add(Positions.Get(Vertex));
		}
	}
	if (!bDelete || OutCut.IsEmpty())
	{
		return true;
	}

	// The props' polygons, then what only they used (edges, corners, points: the bounds the upload measures), then their
	// emptied sections.
	TArray<FEdgeID> Edges;
	TArray<FVertexInstanceID> Corners;
	TArray<FPolygonGroupID> Unused;
	for (const FPolygonID Polygon : Doomed)
	{
		Description.DeletePolygon(Polygon, &Edges, &Corners, &Unused);
	}
	// Corners before edges: an edge's points may only go once no corner uses them.
	TSet<FVertexID> Points;
	for (const FVertexInstanceID Corner : Corners)
	{
		Points.Add(Description.GetVertexInstanceVertex(Corner));
		Description.DeleteVertexInstance(Corner);
	}
	for (const FEdgeID Edge : Edges)
	{
		Points.Add(Description.GetEdgeVertex(Edge, 0));
		Points.Add(Description.GetEdgeVertex(Edge, 1));
		Description.DeleteEdge(Edge);
	}
	for (const FVertexID Point : Points)
	{
		if (Description.IsVertexValid(Point) && Description.GetVertexVertexInstanceIDs(Point).Num() == 0 && Description.GetVertexConnectedEdgeIDs(Point).Num() == 0)
		{
			Description.DeleteVertex(Point);
		}
	}
	TArray<FPolygonGroupID> Groups;
	for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
	{
		Groups.Add(Group);
	}
	for (const FPolygonGroupID Group : Groups)
	{
		if (Description.GetPolygonGroupPolygonIDs(Group).Num() == 0 && (DoomedGroups.Contains(Group) || Unused.Contains(Group)))
		{
			Description.DeletePolygonGroup(Group);
		}
	}
	FElementIDRemappings Remappings;
	Description.Compact(Remappings);
	return true;
}

void FMantleAvatarPropsFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	using namespace MantleAvatarPropsFixerPrivate;
	// Convert strips the body to skin itself; this is As-is's.
	if (Context.Route != EMantleAvatarRoute::AsIs)
	{
		return;
	}
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		const FMantleConvertPart& Part = Context.Parts[PartIndex];
		if (!PropsIsAvatar(Part))
		{
			continue;
		}
		const USkeletalMesh& Mesh = *Part.Mesh.Get();
		FMeshDescription Description;
		const bool bGeometry = PropsReadLOD(Mesh, 0, Description);

		const int32 ClothAssets = Mesh.GetMeshClothingAssets().Num();
		if (ClothAssets > 0)
		{
			TArray<FName> SimSections;
			if (bGeometry)
			{
				FSkeletalMeshAttributes Attributes(Description);
				const TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
				for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
				{
					if (IsClothSimSlot(SlotNames.Get(Group).ToString()))
					{
						SimSections.Add(SlotNames.Get(Group));
					}
				}
			}
			FMantleConvertFix Fix;
			Fix.FixerId = GetId();
			Fix.PartIndex = PartIndex;
			Fix.Kind = KindCloth;
			Fix.Problem = FString::Printf(TEXT("%s carries %d cloth simulation%s; avatars don't simulate cloth, and the Atlas step can't merge sections with it"),
				*Mesh.GetName(), ClothAssets, ClothAssets == 1 ? TEXT("") : TEXT("s"));
			Fix.Action = TEXT("drops the simulation: the cloth keeps its shape and moves with its bones");
			if (SimSections.Num() > 0)
			{
				Fix.Action += FString::Printf(TEXT(", and removes %s, which only drives the simulation"), *PropsNameList(SimSections, 4));
			}
			OutFixes.Add(Fix);
		}

		TArray<FString> HiddenWhy;
		if (bGeometry && PropsHiddenSections(Mesh, Description, &HiddenWhy).Num() > 0)
		{
			FMantleConvertFix Fix;
			Fix.FixerId = GetId();
			Fix.PartIndex = PartIndex;
			Fix.Kind = KindHidden;
			Fix.Problem = FString::Printf(TEXT("%s has parts its material keeps invisible until the character's ability shows them: %s; they still count toward its size"),
				*Mesh.GetName(), *FString::Join(HiddenWhy, TEXT(", ")));
			Fix.Action = TEXT("leaves them out of the avatar's copy (hidden until the character's ability shows it). Untick to keep them");
			OutFixes.Add(Fix);
		}

		TArray<FName> Names;
		TArray<int32> Parents;
		PropsReadSkeleton(Mesh, Names, Parents);
		FPropCut Cut;
		if (!bGeometry || !CutProps(Description, Names, Parents, /*bDelete*/ false, Cut) || Cut.IsEmpty())
		{
			continue;
		}
		TArray<FString> What;
		for (int32 Index = 0; Index < Cut.Sections.Num(); ++Index)
		{
			What.Add(FString::Printf(TEXT("%s (%s triangles)"), *Cut.Sections[Index].ToString(), *FText::AsNumber(Cut.SectionTriangles[Index]).ToString()));
		}
		if (Cut.LooseTriangles > 0)
		{
			What.Add(FString::Printf(TEXT("%s triangles in other sections"), *FText::AsNumber(Cut.LooseTriangles).ToString()));
		}
		FMantleConvertFix Fix;
		Fix.FixerId = GetId();
		Fix.PartIndex = PartIndex;
		Fix.Kind = KindProps;
		Fix.Problem = FString::Printf(TEXT("%s holds props (%s on %s); an avatar's weapons come from the game, and a held prop can make it too big to upload"),
			*Mesh.GetName(), *FString::Join(What, TEXT(", ")), *PropsNameList(Cut.Bones, 4));
		Fix.Action = TEXT("removes the props held in a hand or the mouth from the avatar's copy; gear worn on the back or hips stays. Untick to keep them");
		OutFixes.Add(Fix);
	}
}

bool FMantleAvatarPropsFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	using namespace MantleAvatarPropsFixerPrivate;
	if (Fix.FixerId != GetId() || !Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the props fix names no part");
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	USkeletalMesh* Mesh = Part.Mesh.Get();
	if (!Mesh)
	{
		OutError = TEXT("the part has no mesh");
		return false;
	}
	if (Context.OutputFolder.IsEmpty() || !FPackageName::IsValidLongPackageName(Context.OutputFolder, /*bIncludeReadOnlyRoots*/ true))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a folder the new mesh can go in"), *Context.OutputFolder);
		return false;
	}
	TArray<FName> Names;
	TArray<int32> Parents;
	PropsReadSkeleton(*Mesh, Names, Parents);
	const TArray<FTransform> Pose = Mesh->GetRefSkeleton().GetRawRefBonePose();
	FString PackagePath;
	FString AssetName;

	if (Fix.Kind == KindCloth)
	{
		if (Mesh->GetMeshClothingAssets().Num() == 0)
		{
			return true;
		}
		PropsNewAssetPath(Context.OutputFolder, Mesh->GetName() + TEXT("_NoCloth"), PackagePath, AssetName);
		USkeletalMesh* Copy = FMantleAvatarRepose::WriteWithPose(Mesh, Pose, PackagePath, AssetName,
			[](int32, FMeshDescription&, FString&) { return true; }, OutError);
		if (!Copy)
		{
			return false;
		}
		Context.CreatedAssets.Add(Copy);
		const int32 Assets = Copy->GetMeshClothingAssets().Num();
		TArray<FName> SimSections;
		int32 SimTriangles = 0;
		{
			// As the Clothing panel's Delete does: unbind each bound section, then let go of the assets.
			FScopedSkeletalMeshPostEditChange ScopedPostEditChange(Copy);
			Copy->PreEditChange(nullptr);
			if (const FSkeletalMeshModel* Imported = Copy->GetImportedModel())
			{
				for (int32 LOD = 0; LOD < Imported->LODModels.Num(); ++LOD)
				{
					for (int32 Section = Imported->LODModels[LOD].Sections.Num() - 1; Section >= 0; --Section)
					{
						if (Copy->GetSectionClothingAsset(LOD, Section))
						{
							Copy->RemoveClothingAsset(LOD, Section);
						}
					}
				}
			}
			for (UClothingAssetBase* Asset : Copy->GetMeshClothingAssets())
			{
				if (Asset)
				{
					Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
				}
			}
			Copy->GetMeshClothingAssets().Empty();
			// The sections that only drive the simulation go with it.
			USkeletalMesh::FCommitMeshDescriptionParams Params;
			Params.bForceUpdate = true;
			for (int32 LOD = 0; LOD < Copy->GetLODNum(); ++LOD)
			{
				FMeshDescription* Description = Copy->HasMeshDescription(LOD) ? Copy->GetMeshDescription(LOD) : nullptr;
				if (!Description)
				{
					continue;
				}
				TArray<FName> Sections;
				const int32 Triangles = PropsDeleteSections(*Description, [](const FName Slot) { return FMantleAvatarPropsFixer::IsClothSimSlot(Slot.ToString()); },
					Sections);
				if (Triangles > 0)
				{
					Copy->CommitMeshDescription(LOD, Params);
				}
				if (LOD == 0)
				{
					SimSections = Sections;
					SimTriangles = Triangles;
				}
			}
		}
		Copy->MarkPackageDirty();
		bool bBound = Copy->GetMeshClothingAssets().Num() > 0;
		if (const FSkeletalMeshModel* Imported = Copy->GetImportedModel())
		{
			for (const FSkeletalMeshLODModel& LODModel : Imported->LODModels)
			{
				for (const FSkelMeshSection& Section : LODModel.Sections)
				{
					bBound |= Section.HasClothingData();
				}
			}
		}
		if (bBound)
		{
			OutError = FString::Printf(TEXT("%s still has cloth simulation after it was removed"), *Copy->GetName());
			return false;
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASIS_CLOTH %s -> %s: %d clothing assets dropped, %d simulation-only triangles removed (%s)"),
			*Mesh->GetName(), *Copy->GetName(), Assets, SimTriangles, *PropsNameList(SimSections, 8));
		Context.Notes.Add(FString::Printf(TEXT("%s: cloth simulation dropped (%d asset%s) as %s."), *Mesh->GetName(), Assets, Assets == 1 ? TEXT("") : TEXT("s"),
			*Copy->GetName()));
		PropsCheckMorphs(*Mesh, *Copy, Context);
		Part.Mesh = Copy;
		return true;
	}

	if (Fix.Kind == KindHidden)
	{
		if (Mesh->GetMeshClothingAssets().Num() > 0)
		{
			OutError = FString::Printf(TEXT("%s still has cloth simulation, which removing geometry would break: keep the cloth line ticked"), *Mesh->GetName());
			return false;
		}
		// Read again on the mesh as it stands.
		TArray<FName> Hidden;
		{
			FMeshDescription Description;
			if (PropsReadLOD(*Mesh, 0, Description))
			{
				Hidden = PropsHiddenSections(*Mesh, Description);
			}
		}
		if (Hidden.Num() == 0)
		{
			return true;
		}
		// LOD 0 by its sections' names, recording where they were; a lower LOD by where LOD 0's went (its sections can draw with
		// other slots: Serath's wings are under "M10_EyeLash" from LOD 1, CutProps' Reference).
		int32 Removed = 0;
		FPropCut Base;
		PropsNewAssetPath(Context.OutputFolder, Mesh->GetName() + TEXT("_NoHidden"), PackagePath, AssetName);
		USkeletalMesh* Copy = FMantleAvatarRepose::WriteWithPose(Mesh, Pose, PackagePath, AssetName,
			[&Hidden, &Removed, &Base](int32 LOD, FMeshDescription& Description, FString& OutLODError)
			{
				if (LOD == 0)
				{
					FSkeletalMeshAttributes Attributes(Description);
					const TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
					TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
					TSet<FVertexID> Gone;
					for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
					{
						if (Hidden.Contains(SlotNames.Get(Group)))
						{
							for (const FPolygonID Polygon : Description.GetPolygonGroupPolygonIDs(Group))
							{
								for (const FVertexInstanceID Corner : Description.GetPolygonVertexInstances(Polygon))
								{
									Gone.Add(Description.GetVertexInstanceVertex(Corner));
								}
							}
						}
					}
					for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
					{
						(Gone.Contains(Vertex) ? Base.RemovedPoints : Base.KeptPoints).Add(Positions.Get(Vertex));
					}
				}
				// By name on LOD 0 only: a lower LOD's slot names needn't be the same parts (Serath's LOD 1 "M12_WingSplines" is a
				// 25 cm piece of her body).
				TArray<FName> Sections;
				const int32 Triangles = LOD == 0 ? PropsDeleteSections(Description, [&Hidden](const FName Slot) { return Hidden.Contains(Slot); }, Sections) : 0;
				int32 Near = 0;
				if (LOD > 0)
				{
					FPropCut Cut;
					CutProps(Description, TArray<FName>(), TArray<int32>(), /*bDelete*/ true, Cut, &Base);
					Near = Cut.LooseTriangles;
				}
				UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASIS_HIDDEN LOD %d: %d triangles by section, %d where LOD 0's hidden sections were"), LOD, Triangles, Near);
				Removed += LOD == 0 ? Triangles : 0;
				if (Description.Triangles().Num() == 0)
				{
					OutLODError = TEXT("nothing visible is left");
					return false;
				}
				return true;
			}, OutError);
		if (!Copy)
		{
			return false;
		}
		Context.CreatedAssets.Add(Copy);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASIS_HIDDEN %s -> %s: %d triangles of hidden sections left out on LOD 0 (%s)"), *Mesh->GetName(), *Copy->GetName(),
			Removed, *PropsNameList(Hidden, 16));
		Context.Notes.Add(FString::Printf(TEXT("%s: %s left out (hidden until the character's ability shows them) as %s."), *Mesh->GetName(),
			*PropsNameList(Hidden, 8), *Copy->GetName()));
		PropsCheckMorphs(*Mesh, *Copy, Context);
		Part.Mesh = Copy;
		return true;
	}

	// Props: measured again on the mesh as it stands (an earlier fixer may have replaced it since the fix list was made).
	if (Mesh->GetMeshClothingAssets().Num() > 0)
	{
		OutError = FString::Printf(TEXT("%s still has cloth simulation, which removing geometry would break: keep the cloth line ticked to remove its props"),
			*Mesh->GetName());
		return false;
	}
	FPropCut Before;
	{
		FMeshDescription Description;
		if (!PropsReadLOD(*Mesh, 0, Description) || !CutProps(Description, Names, Parents, /*bDelete*/ false, Before))
		{
			OutError = FString::Printf(TEXT("%s has no skin weights to find its props by"), *Mesh->GetName());
			return false;
		}
	}
	if (Before.IsEmpty())
	{
		return true;
	}
	// LOD 0 first (WriteWithPose goes in LOD order): the lower LODs match their points to its cut.
	TArray<FPropCut> Cuts;
	FPropCut Base;
	PropsNewAssetPath(Context.OutputFolder, Mesh->GetName() + TEXT("_NoProps"), PackagePath, AssetName);
	USkeletalMesh* Copy = FMantleAvatarRepose::WriteWithPose(Mesh, Pose, PackagePath, AssetName,
		[&Names, &Parents, &Cuts, &Base](int32 LOD, FMeshDescription& Description, FString& OutLODError)
		{
			FPropCut Cut;
			const bool bRead = CutProps(Description, Names, Parents, /*bDelete*/ true, Cut, LOD > 0 ? &Base : nullptr);
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASIS_PROPS LOD %d: %d triangles by bone in %d sections, %d more by bone or where LOD 0's props were"), LOD,
				Algo::Accumulate(Cut.SectionTriangles, 0), Cut.Sections.Num(), Cut.LooseTriangles);
			if (LOD == 0)
			{
				Base = Cut;
			}
			Cuts.Add(MoveTemp(Cut));
			if (!bRead)
			{
				OutLODError = TEXT("it has no skin weights");
				return false;
			}
			if (Description.Triangles().Num() == 0)
			{
				OutLODError = TEXT("nothing but props is left");
				return false;
			}
			return true;
		}, OutError);
	if (!Copy)
	{
		return false;
	}
	Context.CreatedAssets.Add(Copy);
	const FPropCut& Cut = Cuts.Num() > 0 ? Cuts[0] : Before;
	int32 Removed = Cut.LooseTriangles;
	for (const int32 Triangles : Cut.SectionTriangles)
	{
		Removed += Triangles;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ASIS_PROPS %s -> %s: %d triangles removed on LOD 0 (sections %s; %d loose); prop bones %s"),
		*Mesh->GetName(), *Copy->GetName(), Removed, *PropsNameList(Cut.Sections, 16), Cut.LooseTriangles, *PropsNameList(Cut.Bones, 32));
	Context.Notes.Add(FString::Printf(TEXT("%s: hand-held props removed (%s triangles) as %s."), *Mesh->GetName(), *FText::AsNumber(Removed).ToString(),
		*Copy->GetName()));
	PropsCheckMorphs(*Mesh, *Copy, Context);
	Part.Mesh = Copy;
	return true;
}

void RegisterMantleAvatarPropsFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleAvatarPropsFixer>());
}
