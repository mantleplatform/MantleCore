// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarBakeFixer.h"

#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticValidation.h"
#include "MantleRuntime.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "MantleAssetCleanup.h"
#include "SkinnedAssetCompiler.h"

const FName FMantleAvatarBakeFixer::KindMesh(TEXT("Mesh"));
const FName FMantleAvatarBakeFixer::KindFace(TEXT("Face"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarBakePrivate
{
	/**
	 * The parts the game hides on an avatar (client MantleAvatarRoleMaterials' HiddenParts, which MantleCore cannot
	 * include): nothing of them shows, so nothing is baked.
	 */
	const TCHAR* const BakeHiddenParts[] = { TEXT("eyeshell"), TEXT("eyeedge"), TEXT("eyelash"), TEXT("saliva"), TEXT("cartilage") };

	/** The smallest bake: a flat colour needs little. */
	constexpr int32 MinBakeSize = 64;

	/** The section name the upload gives a slot (FMantleCosmeticExport's SlotName): the slot name, else the material's. */
	FString BakeSlotName(const TArray<FSkeletalMaterial>& Materials, int32 Slot)
	{
		if (!Materials.IsValidIndex(Slot))
		{
			return FString::Printf(TEXT("Material%d"), Slot);
		}
		if (!Materials[Slot].MaterialSlotName.IsNone())
		{
			return Materials[Slot].MaterialSlotName.ToString();
		}
		const UMaterialInterface* Material = Materials[Slot].MaterialInterface.Get();
		return Material ? Material->GetName() : FString::Printf(TEXT("Material%d"), Slot);
	}

	bool BakeIsHidden(const FString& Name)
	{
		for (const TCHAR* Part : BakeHiddenParts)
		{
			if (Name.Contains(Part, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** The texture cap for a bake: the context's, within the contract's. */
	int32 BakeMaxSize(const FMantleConvertContext& Context, const FMantleAvatarContract& Contract)
	{
		const int32 Max = Context.MaxTextureSize > 0 ? Context.MaxTextureSize : 2048;
		return FMath::Max(MinBakeSize, Contract.MaxTextureSize > 0 ? FMath::Min(Max, Contract.MaxTextureSize) : Max);
	}

	/** The material's largest texture as a power of two within [MinBakeSize, MaxSize]. */
	int32 BakeSizeOf(const UMaterialInterface* Material, int32 MaxSize)
	{
		const int32 Texels = FMath::Max(1, FMantleAvatarAtlasFixer::TexelsPerUVOf(Material, MaxSize));
		const int32 PowerOfTwo = static_cast<int32>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(Texels)));
		return FMath::Clamp(PowerOfTwo, FMath::Min(MinBakeSize, MaxSize), MaxSize);
	}

	/** Per material slot, where LOD's triangles of it lie on UV 0 (the upload's own reader). False when the LOD cannot be read. */
	bool BakeSlotTiles(const USkeletalMesh* Mesh, int32 LOD, TMap<int32, FMantleUVTiles>& OutTiles)
	{
		FMantleGlbMesh Glb;
		TArray<int32> Slots;
		int32 Triangles = 0, Capped = 0;
		float Largest = 0.0f;
		FString Error;
		if (!FMantleCosmeticExport::ReadSkeletalMesh(Mesh, LOD, Glb, Slots, Triangles, Capped, Largest, Error))
		{
			return false;
		}
		for (int32 Material = 0; Material < Glb.MaterialNames.Num() && Material < Slots.Num(); ++Material)
		{
			TArray<FVector2f> UVs;
			TArray<uint32> Indices;
			for (const FMantleGlbPrimitive& Primitive : Glb.Primitives)
			{
				if (Primitive.MaterialIndex != Material || Primitive.FirstVertex < 0 || Primitive.FirstVertex + Primitive.NumVertices > Glb.UVs.Num())
				{
					continue;
				}
				const uint32 First = static_cast<uint32>(UVs.Num());
				UVs.Append(Glb.UVs.GetData() + Primitive.FirstVertex, Primitive.NumVertices);
				for (const uint32 Index : Primitive.Indices)
				{
					Indices.Add(First + Index);
				}
			}
			OutTiles.Add(Slots[Material], FMantleCosmeticChecks::MeasureUVTiles(UVs, Indices));
		}
		return true;
	}

	/** The material slot of a lifted section (Part.Section) on LOD 0; INDEX_NONE for none; false when the section is gone. */
	bool BakeLiftedSlot(const USkeletalMesh* Mesh, int32 Section, int32& OutSlot)
	{
		OutSlot = INDEX_NONE;
		if (Section == INDEX_NONE)
		{
			return true;
		}
		const TArray<FMantleSectionSpan> Spans = FMantleCosmeticExport::ReadSectionSpans(Mesh, 0);
		if (!Spans.IsValidIndex(Section))
		{
			return false;
		}
		OutSlot = Spans[Section].Slot;
		return true;
	}
}

void FMantleAvatarBakeFixer::PlanMesh(const USkeletalMesh* Mesh, bool bAvatar, bool bFace, int32 OnlySlot, int32 MaxSize, const FMantleAtlasSettings& Settings,
	TArray<FMantleBakeTarget>& OutTargets, TArray<FString>& OutNotes)
{
	using namespace MantleAvatarBakePrivate;
	OutTargets.Reset();
	if (!Mesh)
	{
		return;
	}
	TArray<USkinnedAsset*> Compiling = { const_cast<USkeletalMesh*>(Mesh) };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();

	// Every slot some LOD draws, the first LOD that draws it, and the role the upload gives its name.
	TArray<int32> Order;
	TMap<int32, int32> FirstLOD;
	TMap<FString, FString> RoleByName;
	for (int32 LOD = 0; LOD < Mesh->GetLODNum(); ++LOD)
	{
		for (const FMantleSectionSpan& Span : FMantleCosmeticExport::ReadSectionSpans(Mesh, LOD))
		{
			if (!FirstLOD.Contains(Span.Slot))
			{
				FirstLOD.Add(Span.Slot, LOD);
				Order.Add(Span.Slot);
			}
		}
		const TArray<FString> Names = FMantleCosmeticExport::SectionMaterialNames(Mesh, LOD);
		const TArray<FString> Roles = FMantleCosmeticExport::DefaultSectionRoles(Mesh, LOD);
		for (int32 Index = 0; Index < Names.Num() && Index < Roles.Num(); ++Index)
		{
			if (!RoleByName.Contains(Names[Index]))
			{
				RoleByName.Add(Names[Index], Roles[Index]);
			}
		}
	}
	auto RoleOf = [&RoleByName, &Materials](int32 Slot)
	{
		const FString* Found = RoleByName.Find(BakeSlotName(Materials, Slot));
		return Found ? *Found : FString(TEXT("other"));
	};

	// A face's skin bakes from the skin material its LOD 0 draws, for the skin slots of every LOD.
	UMaterialInterface* FaceSkin = nullptr;
	int32 FaceSkinSlot = INDEX_NONE;
	if (bFace)
	{
		for (const FMantleSectionSpan& Span : FMantleCosmeticExport::ReadSectionSpans(Mesh, 0))
		{
			if (Materials.IsValidIndex(Span.Slot) && Materials[Span.Slot].MaterialInterface && RoleOf(Span.Slot).Equals(TEXT("skin"), ESearchCase::CaseSensitive)
				&& !BakeIsHidden(BakeSlotName(Materials, Span.Slot)))
			{
				FaceSkin = Materials[Span.Slot].MaterialInterface.Get();
				FaceSkinSlot = Span.Slot;
				break;
			}
		}
	}

	TMap<int32, TMap<int32, FMantleUVTiles>> TilesByLOD;
	auto TilesOf = [&TilesByLOD, Mesh](int32 Slot, int32 LOD, FMantleUVTiles& OutSlotTiles)
	{
		if (!TilesByLOD.Contains(LOD))
		{
			BakeSlotTiles(Mesh, LOD, TilesByLOD.Add(LOD));
		}
		const FMantleUVTiles* Found = TilesByLOD[LOD].Find(Slot);
		if (Found)
		{
			OutSlotTiles = *Found;
		}
		return Found != nullptr;
	};

	for (const int32 Slot : Order)
	{
		if ((OnlySlot != INDEX_NONE && Slot != OnlySlot) || !Materials.IsValidIndex(Slot))
		{
			continue;
		}
		const FString Name = BakeSlotName(Materials, Slot);
		const FString Role = RoleOf(Slot);
		if (BakeIsHidden(Name) || (bFace && !Role.Equals(TEXT("skin"), ESearchCase::CaseSensitive))
			|| (bAvatar && Role.Equals(TEXT("eyes"), ESearchCase::CaseSensitive)))
		{
			continue;
		}
		UMaterialInterface* Own = Materials[Slot].MaterialInterface.Get();
		const FMantleMaterialTextures OwnTextures = FMantleCosmeticExport::ResolveMaterialTextures(Own);
		if (OwnTextures.bPlain)
		{
			continue;
		}
		const bool bFromFaceSkin = bFace && FaceSkin != nullptr;
		UMaterialInterface* Material = bFromFaceSkin ? FaceSkin : Own;

		// The tile its triangles lie on (a face's skin: LOD 0's, whose UV layout every MetaHuman head LOD shares).
		FIntPoint Tile = FIntPoint::ZeroValue;
		FMantleUVTiles Tiles;
		const int32 TileSlot = bFromFaceSkin ? FaceSkinSlot : Slot;
		if (TilesOf(TileSlot, bFromFaceSkin ? 0 : FirstLOD.FindRef(Slot), Tiles))
		{
			if (Tiles.IsUdim())
			{
				OutNotes.Add(FString::Printf(TEXT("%s: \"%s\" is laid out on UDIM tiles %s, which the Atlas step bakes; it is not baked here"),
					*Mesh->GetName(), *Name, *Tiles.DescribeTiles()));
				continue;
			}
			if (Tiles.Tiles.Num() > 0)
			{
				Tile = Tiles.Tiles[0];
			}
		}

		// One bake per material and tile, whichever slots take it.
		FMantleBakeTarget* Target = OutTargets.FindByPredicate([Material, &Tile](const FMantleBakeTarget& Candidate)
		{
			return Candidate.Material.Get() == Material && Candidate.Tile == Tile;
		});
		if (!Target)
		{
			Target = &OutTargets.AddDefaulted_GetRef();
			Target->Name = Name;
			Target->Material = Material;
			Target->Why = OwnTextures.Why;
			Target->Tile = Tile;
			Target->Size = BakeSizeOf(Material, MaxSize);
			Target->bRendered = FMantleAvatarAtlasFixer::WouldRender(Material, Settings);
		}
		Target->Slots.Add(Slot);
	}
}

void FMantleAvatarBakeFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	for (int32 Part = 0; Part < Context.Parts.Num(); ++Part)
	{
		AnalyzePart(Context, Part, OutFixes);
	}
}

void FMantleAvatarBakeFixer::AnalyzePart(const FMantleConvertContext& Context, int32 PartIndex, TArray<FMantleConvertFix>& OutFixes,
	const FMantleAtlasSettings& Settings)
{
	using namespace MantleAvatarBakePrivate;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Contract || !Context.Parts.IsValidIndex(PartIndex))
	{
		return;
	}
	const FMantleConvertPart& Part = Context.Parts[PartIndex];
	EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
	// Hair cards keep their own atlases (the upload reads a groom's); accessories and emotes are not converted.
	if (!FMantleAvatarConverter::ClassOfSlot(Part.Slot, *Contract, Class) || Class == EMantleCosmeticClass::Hair)
	{
		return;
	}
	const bool bAvatar = Class == EMantleCosmeticClass::Avatar;
	const int32 MaxSize = BakeMaxSize(Context, *Contract);
	for (const bool bFace : { false, true })
	{
		const USkeletalMesh* Mesh = bFace ? Part.Face.Get() : Part.Mesh.Get();
		int32 OnlySlot = INDEX_NONE;
		if (!Mesh || (bFace && !bAvatar) || !BakeLiftedSlot(Mesh, bFace ? INDEX_NONE : Part.Section, OnlySlot))
		{
			continue;
		}
		// On a MetaHuman Convert built (Context.IsBuiltMetaHuman) this is the step that turns Epic's
		// virtual-textured skin, with its face wrinkle maps, into plain textures the upload can read, at Epic's centre skin
		// tone; the game tints that. It is not source-texture baking: the source's own
		// textures never reach a converted MetaHuman. Bake changes materials and textures only, never the geometry its DNA rigs.
		TArray<FMantleBakeTarget> Targets;
		TArray<FString> Notes;
		PlanMesh(Mesh, bAvatar, bFace, OnlySlot, MaxSize, Settings, Targets, Notes);
		if (Targets.Num() == 0)
		{
			continue;
		}
		FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
		Fix.FixerId = MantleAvatarFixerIds::Bake;
		Fix.PartIndex = PartIndex;
		Fix.Kind = bFace ? KindFace : KindMesh;
		TArray<FString> Reasons;
		int32 Sections = 0;
		int32 Largest = 0;
		int32 Sampled = 0;
		for (const FMantleBakeTarget& Target : Targets)
		{
			Reasons.Add(FString::Printf(TEXT("\"%s\": %s"), *Target.Name, *Target.Why));
			Sections += Target.Slots.Num();
			Largest = FMath::Max(Largest, Target.Size);
			Sampled += Target.bRendered ? 0 : 1;
		}
		Fix.Problem = FString::Printf(TEXT("%s: the upload reads plain textures, and %s its colour in the shader (%s)."), *Mesh->GetName(),
			Targets.Num() == 1 ? TEXT("this material builds") : TEXT("these materials build"), *FString::Join(Reasons, TEXT("; ")));
		const FString What = bFace ? FString::Printf(TEXT("the face's skin (%d section%s, every LOD)"), Sections, Sections == 1 ? TEXT("") : TEXT("s"))
			: FString::Printf(TEXT("%d section%s"), Sections, Sections == 1 ? TEXT("") : TEXT("s"));
		Fix.Action = FString::Printf(TEXT("Bakes %d material%s for %s into texture sets on their own UVs (up to %d%c; base colour, normal, ORM) on a converted copy%s."),
			Targets.Num(), Targets.Num() == 1 ? TEXT("") : TEXT("s"), *What, Largest, static_cast<TCHAR>(0x00B2),
			Sampled > 0 ? *FString::Printf(TEXT("; %d sampled from %s textures (no renderer), so shader tints are not kept"),
				Sampled, Sampled == 1 ? TEXT("its") : TEXT("their")) : TEXT(""));
	}
}

bool FMantleAvatarBakeFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	return ApplyToPart(Context, Fix.PartIndex, Fix.Kind == KindFace, FMantleAtlasSettings(), OutError);
}

bool FMantleAvatarBakeFixer::ApplyToPart(FMantleConvertContext& Context, int32 PartIndex, bool bFace, const FMantleAtlasSettings& Settings, FString& OutError)
{
	using namespace MantleAvatarBakePrivate;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Contract || !Context.Parts.IsValidIndex(PartIndex))
	{
		OutError = TEXT("the part it was for is gone");
		return false;
	}
	if (Context.OutputFolder.IsEmpty() || !FPackageName::IsValidLongPackageName(Context.OutputFolder, /*bIncludeReadOnlyRoots*/ true))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a folder to write converted assets to"), *Context.OutputFolder);
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[PartIndex];
	EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
	if (!FMantleAvatarConverter::ClassOfSlot(Part.Slot, *Contract, Class) || Class == EMantleCosmeticClass::Hair)
	{
		OutError = TEXT("only avatars and clothing are baked");
		return false;
	}
	USkeletalMesh* Source = bFace ? Part.Face.Get() : Part.Mesh.Get();
	int32 OnlySlot = INDEX_NONE;
	if (!Source)
	{
		OutError = bFace ? TEXT("the part has no face") : TEXT("the part has no mesh");
		return false;
	}
	if (!BakeLiftedSlot(Source, bFace ? INDEX_NONE : Part.Section, OnlySlot))
	{
		OutError = FString::Printf(TEXT("%s has no section %d"), *Source->GetName(), Part.Section);
		return false;
	}
	TArray<FMantleBakeTarget> Targets;
	PlanMesh(Source, Class == EMantleCosmeticClass::Avatar, bFace, OnlySlot, BakeMaxSize(Context, *Contract), Settings, Targets, Context.Notes);
	if (Targets.Num() == 0)
	{
		Context.Notes.Add(FString::Printf(TEXT("%s: every section's material is already plain textures"), *Source->GetName()));
		return true;
	}

	// Bake everything before writing anything.
	TArray<FMantleBakedTextures> Baked;
	Baked.SetNum(Targets.Num());
	for (int32 Index = 0; Index < Targets.Num(); ++Index)
	{
		const FMantleBakeTarget& Target = Targets[Index];
		UMaterialInterface* Material = Target.Material.Get();
		if (!Material)
		{
			OutError = FString::Printf(TEXT("the material of \"%s\" is gone"), *Target.Name);
			return false;
		}
		const FVector2f Low(static_cast<float>(Target.Tile.X), static_cast<float>(Target.Tile.Y));
		FMantleAvatarAtlasFixer::BakeMaterial(Material, FBox2f(Low, Low + FVector2f(1.0f, 1.0f)), Target.Size, Settings, Baked[Index], Context.Notes);
	}

	// A copy the context may change: the creator's mesh is never touched.
	const bool bNewCopy = !Context.CreatedAssets.ContainsByPredicate([Source](const TWeakObjectPtr<UObject>& Created) { return Created.Get() == Source; });
	USkeletalMesh* Work = FMantleAvatarConverter::MakeWritable(Context, Source, OutError);
	if (!Work)
	{
		return false;
	}
	TArray<UObject*> Made;
	auto Discard = [&Context, &Made, Work, bNewCopy]()
	{
		FMantleAvatarAtlasFixer::DiscardMade(Made);
		// A copy made for a fix that failed is not left behind in Converted/.
		if (bNewCopy)
		{
			Context.CreatedAssets.RemoveAll([Work](const TWeakObjectPtr<UObject>& Created) { return Created.Get() == Work; });
			const TArray<UObject*> Doomed = { Work };
			FMantleAssetCleanup::MoveAside(Doomed);
		}
	};
	TArray<FSkeletalMaterial> Materials = Work->GetMaterials();
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Targets.Num(); ++Index)
	{
		const FMantleBakeTarget& Target = Targets[Index];
		UMaterialInterface* From = Target.Material.Get();
		UMaterialInstanceConstant* Material = FMantleAvatarAtlasFixer::CreateBakedMaterial(Context.OutputFolder,
			FString::Printf(TEXT("%s_%s"), *Work->GetName(), *Target.Name), Baked[Index], FMantleAvatarAtlasFixer::BlendOf(From), From->IsTwoSided(), Made, OutError);
		if (!Material)
		{
			Discard();
			return false;
		}
		for (const int32 Slot : Target.Slots)
		{
			if (Materials.IsValidIndex(Slot))
			{
				// The slot keeps its names, so the upload's section names and roles stay as they were.
				Materials[Slot].MaterialInterface = Material;
			}
		}
		Lines.Add(FString::Printf(TEXT("\"%s\" from %s (%d%c, %s, %d slot%s)"), *Target.Name, *From->GetName(), Baked[Index].Size, static_cast<TCHAR>(0x00B2),
			Target.bRendered ? TEXT("rendered") : TEXT("sampled"), Target.Slots.Num(), Target.Slots.Num() == 1 ? TEXT("") : TEXT("s")));
	}
	Work->SetMaterials(Materials);
	Work->MarkPackageDirty();
	for (UObject* Object : Made)
	{
		Context.CreatedAssets.Add(Object);
	}
	if (bFace)
	{
		Part.Face = Work;
	}
	else
	{
		Part.Mesh = Work;
	}
	Context.Notes.Add(FString::Printf(TEXT("%s: baked %s in %s"), *Source->GetName(), *FString::Join(Lines, TEXT(", ")), *Work->GetPathName()));
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BAKE %s -> %s bakes=%d"), *Source->GetPathName(), *Work->GetPathName(), Targets.Num());
	return true;
}

void RegisterMantleAvatarBakeFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleAvatarBakeFixer>());
}
