// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleCosmeticExport.h"
#include "MantleOutfitSections.h"
#include "MantleReferenceBodies.h"
#include "MantleRuntime.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"

const FName FMantleAvatarAssemblyFixer::KindFace(TEXT("Face"));
const FName FMantleAvatarAssemblyFixer::KindSection(TEXT("Section"));
const FName FMantleAvatarAssemblyFixer::KindBody(TEXT("Body"));
const FName FMantleAvatarAssemblyFixer::KindInclude(TEXT("Include"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarAssemblyPrivate
{
	/** The words that say which hair slot a name is for, by slot (FMantleOutfitSections::Words splits the name). */
	struct FAssemblyHairWords
	{
		const TCHAR* Slot;
		TArray<const TCHAR*> Words;
	};

	const TArray<FAssemblyHairWords>& AssemblyHairWords()
	{
		// Eyebrows, eyelashes and facial hair first: "Eyebrow_Hair" is eyebrows, not hair.
		static const TArray<FAssemblyHairWords> Words = {
			{ TEXT("eyebrows"), { TEXT("eyebrow"), TEXT("eyebrows"), TEXT("brow"), TEXT("brows") } },
			{ TEXT("eyelashes"), { TEXT("eyelash"), TEXT("eyelashes"), TEXT("lash"), TEXT("lashes") } },
			{ TEXT("facial_hair"), { TEXT("beard"), TEXT("beards"), TEXT("mustache"), TEXT("moustache"), TEXT("goatee"), TEXT("stubble"), TEXT("sideburn"), TEXT("sideburns") } },
			{ TEXT("hair"), { TEXT("hair"), TEXT("hairs"), TEXT("ponytail"), TEXT("braid"), TEXT("braids") } },
		};
		return Words;
	}

	bool AssemblyHasWord(TConstArrayView<FString> Words, TConstArrayView<const TCHAR*> Wanted)
	{
		for (const FString& Word : Words)
		{
			for (const TCHAR* Candidate : Wanted)
			{
				if (Word.Equals(Candidate, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
		}
		return false;
	}

	const TArray<const TCHAR*>* AssemblySlotWords(const FString& Slot)
	{
		for (const FAssemblyHairWords& Entry : AssemblyHairWords())
		{
			if (Slot.Equals(Entry.Slot, ESearchCase::CaseSensitive))
			{
				return &Entry.Words;
			}
		}
		return nullptr;
	}

	/** The render sections of a hair mesh's LOD 0 as PickHairSection reads them: slot names, material names, triangles. */
	int32 AssemblyPickHairSection(const USkeletalMesh* Mesh, const FString& Slot, FString& OutWhy)
	{
		const TArray<FString> SlotNames = FMantleCosmeticExport::RenderSectionNames(Mesh, 0);
		const TArray<FMantleSectionSpan> Spans = FMantleCosmeticExport::ReadSectionSpans(Mesh, 0);
		TArray<FString> MaterialNames;
		TArray<int32> Triangles;
		const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
		for (int32 Section = 0; Section < SlotNames.Num(); ++Section)
		{
			const int32 MaterialSlot = Spans.IsValidIndex(Section) ? Spans[Section].Slot : INDEX_NONE;
			const UMaterialInterface* Material = Materials.IsValidIndex(MaterialSlot) ? Materials[MaterialSlot].MaterialInterface.Get() : nullptr;
			MaterialNames.Add(Material ? Material->GetName() : FString());
			Triangles.Add(Spans.IsValidIndex(Section) ? Spans[Section].NumTriangles : 0);
		}
		return FMantleAvatarAssemblyFixer::PickHairSection(SlotNames, MaterialNames, Triangles, Slot, OutWhy);
	}

	FString AssemblyNumber(int32 Value)
	{
		return FText::AsNumber(Value).ToString();
	}
}

FString FMantleAvatarAssemblyFixer::NameStem(const FString& AssetName)
{
	for (const TCHAR* Prefix : { TEXT("SKM_"), TEXT("SK_"), TEXT("SM_"), TEXT("CA_"), TEXT("CO_"), TEXT("BP_") })
	{
		if (AssetName.StartsWith(Prefix, ESearchCase::IgnoreCase) && AssetName.Len() > FCString::Strlen(Prefix))
		{
			return AssetName.RightChop(FCString::Strlen(Prefix));
		}
	}
	return AssetName;
}

FString FMantleAvatarAssemblyFixer::DisplayName(const FString& AssetName)
{
	FString Name = NameStem(AssetName).Replace(TEXT("_"), TEXT(" "));
	Name.TrimStartAndEndInline();
	return Name.IsEmpty() ? AssetName : Name;
}

FString FMantleAvatarAssemblyFixer::HairSlotForName(const FString& MeshName)
{
	using namespace MantleAvatarAssemblyPrivate;
	const TArray<FString> Words = FMantleOutfitSections::Words(MeshName);
	for (const FAssemblyHairWords& Entry : AssemblyHairWords())
	{
		if (AssemblyHasWord(Words, Entry.Words))
		{
			return Entry.Slot;
		}
	}
	return FString();
}

bool FMantleAvatarAssemblyFixer::IsHairName(const FString& MeshName)
{
	return !HairSlotForName(MeshName).IsEmpty();
}

FString FMantleAvatarAssemblyFixer::GarmentSlotForName(const FString& MeshName)
{
	FMantleOutfitSection Section;
	Section.SlotName = MeshName;
	const FString Slot = FMantleOutfitSections::PieceSlot(FMantleOutfitSections::Classify(Section));
	return Slot.IsEmpty() ? FString(TEXT("top")) : Slot;
}

int32 FMantleAvatarAssemblyFixer::PickHairSection(TConstArrayView<FString> SlotNames, TConstArrayView<FString> MaterialNames,
	TConstArrayView<int32> Triangles, const FString& Slot, FString& OutWhy)
{
	using namespace MantleAvatarAssemblyPrivate;
	OutWhy.Reset();
	if (SlotNames.Num() == 0)
	{
		OutWhy = TEXT("the mesh has no sections");
		return INDEX_NONE;
	}
	if (SlotNames.Num() == 1)
	{
		OutWhy = TEXT("its only section");
		return 0;
	}
	static const TArray<const TCHAR*> CardWords = { TEXT("card"), TEXT("cards") };
	// Never the hair: a scalp or helmet (the skin under the cards), peach fuzz, or parts of the head.
	static const TArray<const TCHAR*> AvoidWords = { TEXT("scalp"), TEXT("helmet"), TEXT("fuzz"), TEXT("peachfuzz"), TEXT("skin"), TEXT("head"),
		TEXT("eye"), TEXT("eyes"), TEXT("teeth"), TEXT("tongue"), TEXT("mouth") };
	const TArray<const TCHAR*>* OwnWords = AssemblySlotWords(Slot);

	int32 Best = INDEX_NONE;
	int32 BestScore = TNumericLimits<int32>::Lowest();
	bool bBestCards = false;
	for (int32 Section = 0; Section < SlotNames.Num(); ++Section)
	{
		TArray<FString> Words = FMantleOutfitSections::Words(SlotNames[Section]);
		if (MaterialNames.IsValidIndex(Section))
		{
			Words.Append(FMantleOutfitSections::Words(MaterialNames[Section]));
		}
		const bool bCards = AssemblyHasWord(Words, CardWords);
		int32 Score = bCards ? 4 : 0;
		if (OwnWords && AssemblyHasWord(Words, *OwnWords))
		{
			Score += 2;
		}
		for (const FAssemblyHairWords& Other : AssemblyHairWords())
		{
			if (!Slot.Equals(Other.Slot, ESearchCase::CaseSensitive) && AssemblyHasWord(Words, Other.Words))
			{
				Score -= 3;
			}
		}
		if (AssemblyHasWord(Words, AvoidWords))
		{
			Score -= 5;
		}
		const int32 Size = Triangles.IsValidIndex(Section) ? Triangles[Section] : 0;
		const int32 BestSize = Triangles.IsValidIndex(Best) ? Triangles[Best] : -1;
		if (Score > BestScore || (Score == BestScore && Size > BestSize))
		{
			Best = Section;
			BestScore = Score;
			bBestCards = bCards;
		}
	}
	if (BestScore <= 0)
	{
		// Nothing named for hair: the largest section is the cards more often than not.
		Best = 0;
		for (int32 Section = 1; Section < SlotNames.Num(); ++Section)
		{
			if (Triangles.IsValidIndex(Section) && Triangles.IsValidIndex(Best) && Triangles[Section] > Triangles[Best])
			{
				Best = Section;
			}
		}
		OutWhy = TEXT("the largest section; none is named for hair cards, so check it in the preview");
		return Best;
	}
	OutWhy = bBestCards ? TEXT("its name says hair cards") : FString::Printf(TEXT("its name says %s"), *Slot);
	return Best;
}

FString FMantleAvatarAssemblyFixer::AssemblyFolderOf(const UObject* Selection)
{
	if (!Selection)
	{
		return FString();
	}
	const FString Package = Selection->GetOutermost()->GetName();
	if (Selection->IsA<UBlueprint>())
	{
		// A MetaHuman Creator package puts the Blueprint one folder down (<Name>/<Name>/BP_<Name>) with Body/, Face/,
		// Clothing/ beside that folder: the character's folder is the nearest one up that has a Body/ folder.
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FString Folder = FPackageName::GetLongPackagePath(Package);
		for (int32 Up = 0; Up < 3 && !Folder.IsEmpty(); ++Up)
		{
			if (Registry.HasAssets(FName(*(Folder / TEXT("Body"))), /*bRecursive*/ true))
			{
				return Folder;
			}
			const int32 Slash = Folder.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			Folder = Slash > 0 ? Folder.Left(Slash) : FString();
		}
		return FPackageName::GetLongPackagePath(Package);
	}
	if (Selection->IsA<USkeletalMesh>())
	{
		return FMantleCosmeticExport::MetaHumanFolder(Package);
	}
	return FString();
}

bool FMantleAvatarAssemblyFixer::Scan(const FString& InFolder, FMantleAssemblyScan& Out, FString& OutError, const bool bAllowConvertedRoot)
{
	using namespace MantleAvatarAssemblyPrivate;
	Out = FMantleAssemblyScan();
	FString Folder = InFolder;
	while (Folder.EndsWith(TEXT("/")))
	{
		Folder.LeftChopInline(1);
	}
	if (Folder.IsEmpty() || !FPackageName::IsValidLongPackageName(Folder))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a content folder"), *InFolder);
		return false;
	}
	Out.Folder = Folder;
	Out.Name = FPackageName::GetShortName(Folder);

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Folder));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> All;
	Registry.GetAssets(Filter, All);
	// The converter's own output is never part of the character. A Convert build, itself in a Converted folder, is read
	// only when asked for (bAllowConvertedRoot); the Converted folders below it are still left out.
	const int32 Skip = bAllowConvertedRoot ? Folder.Len() : 0;
	All.RemoveAll([Skip](const FAssetData& Data)
	{
		return Data.PackagePath.ToString().RightChop(Skip).Contains(TEXT("/Converted"), ESearchCase::IgnoreCase);
	});
	All.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });

	TArray<FAssetData> Meshes;
	TArray<FString> ClothAssets;
	for (const FAssetData& Data : All)
	{
		const FString ClassName = Data.AssetClassPath.GetAssetName().ToString();
		if (ClassName.Equals(TEXT("SkeletalMesh"), ESearchCase::CaseSensitive))
		{
			Meshes.Add(Data);
		}
		// Chaos cloth (by class name: the plugin links no cloth module).
		else if (ClassName.Contains(TEXT("ClothAsset"), ESearchCase::CaseSensitive) || ClassName.Equals(TEXT("ChaosOutfitAsset"), ESearchCase::CaseSensitive))
		{
			ClothAssets.Add(Data.AssetName.ToString());
		}
	}

	// The body: on the MetaHuman body skeleton and not clothing; one under Body/ or named Body first.
	int32 Body = INDEX_NONE;
	int32 BodyScore = -1;
	TArray<FMantleFaceCandidate> Candidates;
	for (int32 Index = 0; Index < Meshes.Num(); ++Index)
	{
		const FAssetData& Data = Meshes[Index];
		FMantleFaceCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.PackageName = Data.PackageName.ToString();
		Candidate.AssetName = Data.AssetName.ToString();
		Data.GetTagValue(TEXT("Skeleton"), Candidate.Skeleton);
		if (!FMantleCosmeticExport::CouldBeAuthoringBody(Candidate.AssetName, Candidate.PackageName, Candidate.Skeleton)
			|| IsHairName(Candidate.AssetName) || Candidate.PackageName.Contains(TEXT("/Hair/"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		const int32 Score = (Candidate.PackageName.Contains(TEXT("/Body/"), ESearchCase::IgnoreCase) ? 2 : 0)
			+ (Candidate.AssetName.Contains(TEXT("Body"), ESearchCase::IgnoreCase) ? 1 : 0);
		if (Score > BodyScore)
		{
			Body = Index;
			BodyScore = Score;
		}
	}
	if (Body == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("%s has no MetaHuman body (a skeletal mesh on metahuman_base_skel that is not clothing)"), *Folder);
		return false;
	}
	const int32 Face = FMantleCosmeticExport::PickAuthoringFace(Meshes[Body].AssetName.ToString(), Candidates);
	if (Face == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("%s has a body but no MetaHuman face mesh, so it is not a MetaHuman character"), *Folder);
		return false;
	}
	Out.Body = Cast<USkeletalMesh>(Meshes[Body].GetAsset());
	Out.Face = Cast<USkeletalMesh>(Meshes[Face].GetAsset());
	if (!Out.Body.IsValid() || !Out.Face.IsValid())
	{
		OutError = FString::Printf(TEXT("the body or the face under %s could not be loaded"), *Folder);
		return false;
	}

	for (int32 Index = 0; Index < Meshes.Num(); ++Index)
	{
		if (Index == Body || Index == Face)
		{
			continue;
		}
		const FMantleFaceCandidate& Candidate = Candidates[Index];
		const bool bHair = IsHairName(Candidate.AssetName) || Candidate.PackageName.Contains(TEXT("/Hair/"), ESearchCase::IgnoreCase)
			|| Candidate.PackageName.Contains(TEXT("/Grooms/"), ESearchCase::IgnoreCase);
		// A folder with several characters (Mantle's own reference bodies): the others' bodies and faces are not clothing.
		const bool bOtherBody = !bHair && FMantleCosmeticExport::CouldBeAuthoringBody(Candidate.AssetName, Candidate.PackageName, Candidate.Skeleton)
			&& (Candidate.AssetName.Contains(TEXT("Body"), ESearchCase::IgnoreCase) || Candidate.PackageName.Contains(TEXT("/Body/"), ESearchCase::IgnoreCase));
		const bool bOtherFace = Candidate.AssetName.Contains(TEXT("Face"), ESearchCase::IgnoreCase) && Candidate.Skeleton.Contains(TEXT("Face"), ESearchCase::IgnoreCase);
		if (bOtherBody || bOtherFace)
		{
			Out.LeftOut.Add(FString::Printf(TEXT("%s: another %s, not this character's"), *Candidate.AssetName, bOtherBody ? TEXT("body") : TEXT("face")));
			continue;
		}
		const bool bBodySkeleton = Candidate.Skeleton.Contains(TEXT("metahuman_base_skel"), ESearchCase::IgnoreCase);
		const bool bClothingFolder = Candidate.PackageName.Contains(TEXT("/Clothing/"), ESearchCase::IgnoreCase)
			|| Candidate.PackageName.Contains(TEXT("/Outfit"), ESearchCase::IgnoreCase);
		if (!bHair && !bBodySkeleton && !bClothingFolder)
		{
			Out.LeftOut.Add(FString::Printf(TEXT("%s: not on the MetaHuman body skeleton, and not hair or clothing by its folder or name"), *Candidate.AssetName));
			continue;
		}
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Meshes[Index].GetAsset());
		if (!Mesh)
		{
			Out.LeftOut.Add(FString::Printf(TEXT("%s: could not be loaded"), *Candidate.AssetName));
			continue;
		}
		if (bHair)
		{
			FMantleAssemblyMesh& Entry = Out.Hair.AddDefaulted_GetRef();
			Entry.Mesh = Mesh;
			Entry.Slot = HairSlotForName(Candidate.AssetName);
			if (Entry.Slot.IsEmpty())
			{
				Entry.Slot = TEXT("hair");
			}
			continue;
		}

		// Its cloth simulation: a Chaos cloth asset named for it, or a clothing asset of its own.
		FString Cloth;
		const FString Stem = NameStem(Candidate.AssetName);
		for (const FString& ClothName : ClothAssets)
		{
			if (NameStem(ClothName).Equals(Stem, ESearchCase::IgnoreCase))
			{
				Cloth = ClothName;
				break;
			}
		}
		if (Cloth.IsEmpty() && Mesh->GetMeshClothingAssets().Num() > 0)
		{
			Cloth = FString::Printf(TEXT("%s's clothing simulation"), *Candidate.AssetName);
		}

		// One entry per garment slot its sections name (a one-piece outfit can name several), else its name's.
		TArray<int32> SectionSlots;
		TArray<FString> Slots;
		for (const FMantleOutfitSection& Section : FMantleCosmeticExport::ReadOutfitSections(Mesh, 0, SectionSlots))
		{
			const FString Slot = FMantleOutfitSections::PieceSlot(FMantleOutfitSections::Classify(Section));
			if (!Slot.IsEmpty() && !Slots.Contains(Slot))
			{
				Slots.Add(Slot);
			}
		}
		const TArray<FString> Order = { TEXT("top"), TEXT("bottom"), TEXT("footwear") };
		Slots.Sort([&Order](const FString& A, const FString& B) { return Order.IndexOfByKey(A) < Order.IndexOfByKey(B); });
		if (Slots.Num() == 0)
		{
			Slots.Add(GarmentSlotForName(Candidate.AssetName));
		}
		for (const FString& Slot : Slots)
		{
			FMantleAssemblyMesh& Entry = Out.Garments.AddDefaulted_GetRef();
			Entry.Mesh = Mesh;
			Entry.Slot = Slot;
			Entry.ClothAsset = Cloth;
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT scan %s: body=%s face=%s garments=%d hair=%d left out=%d"), *Folder, *Out.Body->GetName(),
		*Out.Face->GetName(), Out.Garments.Num(), Out.Hair.Num(), Out.LeftOut.Num());
	return true;
}

void FMantleAvatarAssemblyFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	using namespace MantleAvatarAssemblyPrivate;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (Context.AssemblyFolder.IsEmpty() || !Contract)
	{
		return;
	}
	FMantleAssemblyScan Found;
	FString Error;
	if (!FMantleAvatarAssemblyFixer::Scan(Context.AssemblyFolder, Found, Error, Context.IsHandedOffBuild(Context.AssemblyFolder)))
	{
		return;
	}
	USkeletalMesh* Body = Found.Body.Get();
	USkeletalMesh* Face = Found.Face.Get();
	// Once this conversion gave the avatar the reference topology, the converted body is the one the clothing is made on.
	USkeletalMesh* Wearer = FMantleAvatarConverter::ConvertedBodyFor(Context, Body, *Contract);
	const int32 BodyReferenceLOD = FMantleCosmeticExport::FindReferenceLOD(Wearer, *Contract);

	for (int32 Index = 0; Index < Context.Parts.Num(); ++Index)
	{
		const FMantleConvertPart& Part = Context.Parts[Index];
		EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
		if (!FMantleAvatarConverter::ClassOfSlot(Part.Slot, *Contract, Class))
		{
			continue;
		}
		const USkeletalMesh* Original = Part.Original.IsValid() ? Part.Original.Get() : Part.Mesh.Get();
		auto AddFix = [&OutFixes, Index](const FName Kind) -> FMantleConvertFix&
		{
			FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
			Fix.FixerId = MantleAvatarFixerIds::Assembly;
			Fix.PartIndex = Index;
			Fix.Kind = Kind;
			return Fix;
		};

		if (Class == EMantleCosmeticClass::Avatar)
		{
			// The face: without it a MetaHuman body is measured to its neck, and refused as too short.
			if (!Part.Face.IsValid() && Face && Original == Body)
			{
				FMantleConvertFix& Fix = AddFix(KindFace);
				const float BodyTop = static_cast<float>(Body->GetImportedBounds().GetBox().Max.Z);
				Fix.Problem = Contract->HeightMinCm > 0.0f
					? FString::Printf(TEXT("no face is chosen for %s. An avatar's height is measured with its head, and the body alone reaches %.1f cm (the least is %.0f cm)."),
						*Body->GetName(), BodyTop, Contract->HeightMinCm)
					: FString::Printf(TEXT("no face is chosen for %s. An avatar's height is measured with its head, and the body alone reaches %.1f cm."),
						*Body->GetName(), BodyTop);
				Fix.Action = FString::Printf(TEXT("Uses the character's own face, %s."), *Face->GetName());
			}
			continue;
		}

		// A cloth-simulated piece: a plain garment, only when the creator asks for it.
		if (!Part.bIncluded && !Part.ClothAsset.IsEmpty())
		{
			FMantleConvertFix& Fix = AddFix(KindInclude);
			Fix.bEnabled = false;
			Fix.Problem = FString::Printf(TEXT("%s is simulated cloth (%s), and Mantle uploads no cloth simulation."),
				Original ? *Original->GetName() : *Part.Name, *Part.ClothAsset);
			Fix.Action = FString::Printf(TEXT("Adds it as a plain %s garment that moves with the body's bones; the simulation is dropped. It takes the %s slot, so players wear it instead of another %s, not with it. Off unless you tick it."),
				*Part.Slot, *Part.Slot, *Part.Slot);
		}

		// Hair from a skeletal mesh lifts one section: the cards.
		if (Class == EMantleCosmeticClass::Hair && Part.Section == INDEX_NONE && Part.Mesh.IsValid())
		{
			FMantleConvertFix& Fix = AddFix(KindSection);
			FString Why;
			const int32 Pick = AssemblyPickHairSection(Part.Mesh.Get(), Part.Slot, Why);
			const TArray<FString> Names = FMantleCosmeticExport::RenderSectionNames(Part.Mesh.Get(), 0);
			Fix.Problem = FString::Printf(TEXT("hair from a skeletal mesh lifts one of its sections, and none of %s's %d is chosen."),
				*Part.Mesh->GetName(), Names.Num());
			Fix.bCanFix = Names.IsValidIndex(Pick);
			Fix.Action = Fix.bCanFix
				? FString::Printf(TEXT("Lifts section %d, \"%s\" (%s)."), Pick, *Names[Pick], *Why)
				: FString::Printf(TEXT("Can't: %s."), *Why);
		}

		// The body the piece was made on: the character's own, when it may be one.
		if (!Part.AuthoringBody.IsValid() && Body)
		{
			FMantleConvertFix& Fix = AddFix(KindBody);
			const FString What = Original ? Original->GetName() : Part.Name;
			if (BodyReferenceLOD != INDEX_NONE)
			{
				Fix.Problem = FString::Printf(TEXT("%s uploads with the body it was made on, which the upload would otherwise look for in the project."), *What);
				Fix.Action = Wearer == Body
					? FString::Printf(TEXT("Uses the character's own body, %s (its LOD %d has Mantle's reference topology), with its face %s."),
						*Body->GetName(), BodyReferenceLOD, Face ? *Face->GetName() : TEXT("(none)"))
					: FString::Printf(TEXT("Uses the character's own body as this conversion made it, %s (its LOD %d has Mantle's reference topology), with its face %s."),
						*Wearer->GetName(), BodyReferenceLOD, Face ? *Face->GetName() : TEXT("(none)"));
			}
			else
			{
				// The garment rules (FMantleCosmeticExport::Build): the authoring body must have the reference topology. The
				// Refit line's body: Mantle's reference body, the project's own build of it (the plugin ships no bodies).
				FMantleReferenceBody Reference;
				FString WhyNot;
				const USkeletalMesh* Fallback = FMantleReferenceBodies::GetClosest(*Body, *Contract, Reference, WhyNot) ? Reference.Body : nullptr;
				Fix.bCanFix = false;
				Fix.Problem = FString::Printf(TEXT("%s was made on %s, which has no LOD with Mantle's reference topology (%s vertices). Clothing and hair upload with a body that has one, so they fit every wearer."),
					*What, *Body->GetName(), *AssemblyNumber(Contract->TopologyVertexCount));
				Fix.Action = Fallback
					? FString::Printf(TEXT("Can't use the character's body. The Refit line for this piece refits it onto %s, Mantle's neutral reference body, instead (re-bound to that body's pose, its shape warped from %s), so the upload takes it. With Refit off, the upload looks for a body with the reference topology in the project, and refuses the piece when there is none or a weighted bone sits more than %.1f cm or %.0f degrees from it. To use the character's own body, bring the MetaHuman in again with a LOD at the reference topology (UE Optimized, Medium, High or Cinematic)."),
						*Fallback->GetName(), *Body->GetName(), Contract->BindToleranceCm, Contract->BindToleranceDegrees)
					: !FMantleReferenceBodies::IsBuilt()
					? FString(TEXT("Can't use the character's body. The Refit line for this piece first builds Mantle's neutral reference body in this project (once), then refits the piece onto it, so the upload takes it. With Refit off, the upload looks for a body with the reference topology in the project, and refuses the piece when there is none. To use the character's own body, bring the MetaHuman in again with a LOD at the reference topology (UE Optimized, Medium, High or Cinematic)."))
					: FString::Printf(TEXT("Can't use the character's body, so the upload refuses the piece. The Refit line can't help yet: %s. To use the character's own body, bring the MetaHuman in again with a LOD at the reference topology (UE Optimized, Medium, High or Cinematic)."),
						*WhyNot);
			}
		}
	}
}

bool FMantleAvatarAssemblyFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	using namespace MantleAvatarAssemblyPrivate;
	if (!Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the part it was for is gone");
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	FMantleAssemblyScan Found;
	if (!FMantleAvatarAssemblyFixer::Scan(Context.AssemblyFolder, Found, OutError, Context.IsHandedOffBuild(Context.AssemblyFolder)))
	{
		return false;
	}
	if (Fix.Kind == KindFace)
	{
		Part.Face = Found.Face;
		Context.Notes.Add(FString::Printf(TEXT("%s: the face %s."), *Part.Name, *Found.Face->GetName()));
		return true;
	}
	if (Fix.Kind == KindSection)
	{
		FString Why;
		const int32 Pick = Part.Mesh.IsValid() ? AssemblyPickHairSection(Part.Mesh.Get(), Part.Slot, Why) : INDEX_NONE;
		if (Pick == INDEX_NONE)
		{
			OutError = Why.IsEmpty() ? FString(TEXT("the hair mesh has no sections")) : Why;
			return false;
		}
		Part.Section = Pick;
		Context.Notes.Add(FString::Printf(TEXT("%s: lifts section %d (%s)."), *Part.Name, Pick, *Why));
		return true;
	}
	if (Fix.Kind == KindBody)
	{
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		// The converted body when this conversion replaced the character's body.
		USkeletalMesh* Wearer = Contract ? FMantleAvatarConverter::ConvertedBodyFor(Context, Found.Body.Get(), *Contract) : nullptr;
		if (!Contract || !Wearer || FMantleCosmeticExport::FindReferenceLOD(Wearer, *Contract) == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("%s has no LOD with Mantle's reference topology"), *Found.Body->GetName());
			return false;
		}
		Part.AuthoringBody = Wearer;
		Part.AuthoringFace = Found.Face;
		Context.Notes.Add(FString::Printf(TEXT("%s: made on %s, with the face %s."), *Part.Name, *Wearer->GetName(), *Found.Face->GetName()));
		return true;
	}
	if (Fix.Kind == KindInclude)
	{
		Part.bIncluded = true;
		Context.Notes.Add(FString::Printf(TEXT("%s: included as a plain %s garment; its cloth simulation (%s) is dropped."), *Part.Name, *Part.Slot, *Part.ClothAsset));
		return true;
	}
	OutError = FString::Printf(TEXT("the Assembly fixer has no fix of kind %s"), *Fix.Kind.ToString());
	return false;
}
