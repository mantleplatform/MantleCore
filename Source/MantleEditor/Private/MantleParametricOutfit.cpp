// Copyright Joshua Eger. All Rights Reserved.
#include "MantleParametricOutfit.h"

#include "MantleAvatarContract.h"
#include "MantleEditorActions.h"
#include "MantleFitBodies.h"
#include "MantleParametricOutfitAuthoring.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MantleAvatarConverter.h"
#include "MantleBodyCorrespondence.h"
#include "MantleBodyTopologyData.h"
#include "MantleCosmeticExport.h"
#include "MantleAvatarRefitFixer.h"
#include "MantleGarmentOutfit.h"
#include "MantleOutfitSections.h"
#include "MantleAvatarConvertFixer.h"
#include "MantleNailSeating.h"
#include "MeshDescription.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "MantleHeadFit.h"
#include "MantleGlbWriter.h"
#include "StaticMeshAttributes.h"
#include "Engine/StaticMesh.h"
#include "MantleRuntimeSkeletalMeshBuilder.h"
#include "Animation/Skeleton.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Dataflow/DataflowObject.h"
#include "ChaosClothAsset/ClothAssetBase.h"
#include "ChaosClothAsset/CollectionClothFacade.h"
#include "ChaosOutfitAsset/CollectionOutfitFacade.h"
#include "ChaosOutfitAsset/Outfit.h"
#include "ChaosOutfitAsset/OutfitAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Materials/MaterialInterface.h"
#include "Editor.h"
#include "Item/MetaHumanOutfitPipeline.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCollection.h"
#include "MetaHumanCollectionEditorPipeline.h"
#include "MetaHumanDefaultEditorPipelineBase.h"
#include "MetaHumanDefaultPipelineBase.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogMantleParametricOutfit, Log, All);

namespace MantleParametricOutfitPrivate
{
	/** How far the closest twentieth of a garment's vertices may be from its authoring body's skin (cm). */
	constexpr float WrapMaxCloseCm = 1.5f;

	/** The least height (cm) a mesh spans, feet to head, to be a whole body; feet, hands and a torso alone fall short. */
	constexpr float WholeBodyMinCm = 120.0f;

	void Finish(USkinnedAsset* Asset)
	{
		USkinnedAsset* Assets[] = { Asset };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Assets);
	}

	/** Native authoring build only: the selected size is hosted by a transient copy of its target Collection. */
	USkeletalMesh* ResizeOntoCharacter(UChaosOutfitAsset& Single, FString& Selected,
		const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
		TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& Error)
	{
		UMetaHumanCharacter* Character = Draft.OutfitAuthoringCharacter.Get();
		USkeletalMesh* Body = Draft.AuthoringBody.Get();
		USkeletalMesh* Face = Draft.AuthoringFace.Get();
		// A standard body type made in memory (FMantleFitBodies) has no converted body/face and no build stamp, so the
		// converted-body ownership checks apply only to the converted route.
		const bool bInMemory = Draft.bOutfitAuthoringInMemory;
		if (!Character || (!bInMemory && (!FMantleAvatarConverter::IsConvertOutput(Body) || !FMantleAvatarConverter::IsConvertOutput(Face)
			|| FMantleCosmeticExport::FindReferenceLOD(Body, Contract) == INDEX_NONE)))
		{
			Error = TEXT("native resizing requires the Creator character and its complete converted authoring body and face");
			return nullptr;
		}
		if (!bInMemory)
		{
			const FString* BodyBuild = Body->GetPackage()->GetMetaData().FindValue(Body, FMantleAvatarConverter::BuiltKey);
			const FString* FaceBuild = Face->GetPackage()->GetMetaData().FindValue(Face, FMantleAvatarConverter::BuiltKey);
			const FString CharacterFolder = FPaths::GetPath(Character->GetOutermost()->GetName());
			if (!BodyBuild || !FaceBuild || *BodyBuild != *FaceBuild || !BodyBuild->StartsWith(CharacterFolder + TEXT("/Unpacked/")))
			{
				Error = TEXT("the converted authoring body/face do not belong to the selected Creator character's build");
				return nullptr;
			}
		}
		UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
		if (!Subsystem) { Error = TEXT("MetaHuman Creator's editor subsystem is unavailable"); return nullptr; }
		const bool bAlreadyEditing = Subsystem->IsObjectAddedForEditing(Character);
		if (!bAlreadyEditing && !Subsystem->TryAddObjectToEdit(Character))
		{
			Error = TEXT("MetaHuman Creator could not prepare the selected authoring character"); return nullptr;
		}
		struct FEditingLease
		{
			TWeakObjectPtr<UMetaHumanCharacterEditorSubsystem> Subsystem;
			TStrongObjectPtr<UMetaHumanCharacter> Character;
			bool bRemove = false;
			~FEditingLease()
			{
				if (!bRemove) { return; }
				if (IsInGameThread())
				{
					if (Subsystem.IsValid()) { Subsystem->RemoveObjectToEdit(Character.Get()); }
				}
				else
				{
					AsyncTask(ENamedThreads::GameThread, [WeakSubsystem = Subsystem, KeptCharacter = MoveTemp(Character)]()
					{
						if (WeakSubsystem.IsValid()) { WeakSubsystem->RemoveObjectToEdit(KeptCharacter.Get()); }
					});
				}
			}
		};
		TSharedRef<FEditingLease, ESPMode::ThreadSafe> EditingLease = MakeShared<FEditingLease, ESPMode::ThreadSafe>();
		EditingLease->Subsystem = Subsystem;
		EditingLease->Character.Reset(Character);
		EditingLease->bRemove = !bAlreadyEditing;
		FMetaHumanCharacterPreviewAssetOptions PreviewOptions;
		PreviewOptions.bGenerateMergedHeadAndBodyMesh = true;
		PreviewOptions.bGenerateBodyMeasurements = true;
		FMetaHumanCharacterPreviewAssets Preview;
		if (!Subsystem->TryGetCharacterPreviewAssets(Character, PreviewOptions, Preview) || !Preview.MergedHeadAndBodyMesh)
		{
			Error = TEXT("MetaHuman Creator could not provide the native merged target body/head"); return nullptr;
		}
		// Preview meshes belong to the subsystem. The build may read them, never modify them.
		KeepAlive.Emplace(Preview.MergedHeadAndBodyMesh.Get());
		const UMetaHumanCollection* Original = Character->GetInternalCollection();
		if (!Original) { Error = TEXT("the authoring character has no native Collection"); return nullptr; }
		UMetaHumanCollection* Collection = DuplicateObject<UMetaHumanCollection>(Original, GetTransientPackage());
		KeepAlive.Emplace(Collection);
		const UMetaHumanDefaultEditorPipelineBase* Pipeline = Cast<UMetaHumanDefaultEditorPipelineBase>(Collection->GetEditorPipeline());
		if (!Pipeline || !Pipeline->CostumeProperties.OutfitResizeDataflowAsset)
		{
			Error = TEXT("the authoring character's native pipeline has no Outfit resizing graph"); return nullptr;
		}
		const FMetaHumanPaletteItemKey CharacterKey = Character->GetInternalCollectionKey();
		TArray<FMetaHumanPaletteItemKey> Remove;
		bool bHasCharacter = false;
		for (const FMetaHumanCharacterPaletteItem& Item : Collection->GetItems())
		{
			const FMetaHumanPaletteItemKey Key = Item.GetItemKey();
			if (Key == CharacterKey) { bHasCharacter = true; }
			else { Remove.Add(Key); }
		}
		if (!bHasCharacter) { Error = TEXT("the native Collection does not contain its authoring character"); return nullptr; }
		for (const FMetaHumanPaletteItemKey& Key : Remove)
		{
			if (!Collection->TryRemoveItem(Key)) { Error = TEXT("could not isolate the native Outfit authoring build"); return nullptr; }
		}
		Collection->ClearBuiltData();
		Collection->SetQuality(EMetaHumanCharacterPaletteBuildQuality::Preview);
		Collection->RefreshBuildCacheGuid();
		FMetaHumanPaletteItemKey OutfitKey;
		if (!Collection->TryAddItemFromPrincipalAsset(UMetaHumanDefaultPipelineBase::OutfitsSlotName, FSoftObjectPath(&Single), OutfitKey))
		{
			Error = TEXT("the native Collection could not host the selected Outfit size"); return nullptr;
		}
		FInstancedStruct BuildInput = FInstancedStruct::Make<FMetaHumanBuildInputBase>();
		BuildInput.GetMutable<FMetaHumanBuildInputBase>().EditorPreviewCharacter = CharacterKey;
		struct FBuildState
		{
			std::atomic<bool> bDone{false};
			EMetaHumanBuildStatus Status = EMetaHumanBuildStatus::Failed;
			TStrongObjectPtr<UMetaHumanCollection> Collection;
			TSharedPtr<FEditingLease, ESPMode::ThreadSafe> EditingLease;
		};
		TSharedRef<FBuildState, ESPMode::ThreadSafe> State = MakeShared<FBuildState, ESPMode::ThreadSafe>();
		State->Collection.Reset(Collection);
		State->EditingLease = EditingLease;
		UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_NATIVE_RESIZE character=%s size=%s graph=%s"),
			*Character->GetPathName(), *Selected, *Pipeline->CostumeProperties.OutfitResizeDataflowAsset->GetPathName());
		Collection->Build(BuildInput, UMetaHumanCollection::FOnBuildComplete::CreateLambda([State](EMetaHumanBuildStatus Status)
		{
			State->Status = Status;
			State->bDone.store(true);
		}));
		const double Deadline = FPlatformTime::Seconds() + 900.0;
		while (!State->bDone.load() && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FTSTicker::GetCoreTicker().Tick(0.01f);
			FPlatformProcess::Sleep(0.01f);
		}
		if (!State->bDone.load() || State->Status != EMetaHumanBuildStatus::Succeeded || !Collection->GetBuiltData().IsValid())
		{
			Error = TEXT("native Outfit resizing did not complete successfully; no original-source fallback is uploaded"); return nullptr;
		}
		const FMetaHumanPipelineBuiltData* Built = Collection->GetBuiltData().PaletteBuiltData.ItemBuiltData.View().Find(FMetaHumanPaletteItemPath(OutfitKey));
		const FMetaHumanOutfitPipelineBuildOutput* Output = Built ? Built->BuildOutput.GetPtr<FMetaHumanOutfitPipelineBuildOutput>() : nullptr;
		const FMetaHumanOutfitGeneratedAssets* Assets = Output ? Output->CharacterAssets.Find(CharacterKey) : nullptr;
		if (!Assets || !Assets->Outfit || Assets->Outfit.Get() == &Single
			|| Assets->AutoSelectedSourceSize.IsNone() || !Assets->AvailableSourceSizes.Contains(Assets->AutoSelectedSourceSize)
			|| (!Selected.IsEmpty() && (Assets->AvailableSourceSizes.Num() != 1 || Assets->AutoSelectedSourceSize != FName(*Selected))))
		{
			Error = TEXT("native resizing returned an original Outfit fallback or did not report a supported selected source size"); return nullptr;
		}
		Selected = Assets->AutoSelectedSourceSize.ToString();
		KeepAlive.Emplace(Assets->Outfit.Get());
		USkeletalMesh* RenderMesh = Assets->OutfitMesh.Get();
		if (!RenderMesh)
		{
			// Native preview can render the resized Outfit directly. Mantle uploads its evaluated render mesh.
			FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ChaosClothAssetTools"));
			RenderMesh = NewObject<USkeletalMesh>();
			KeepAlive.Emplace(RenderMesh);
			if (!Assets->Outfit->ExportToSkeletalMesh(*RenderMesh))
			{
				Error = TEXT("the resized native Outfit could not export its render geometry"); return nullptr;
			}
		}
		else { KeepAlive.Emplace(RenderMesh); }
		Finish(RenderMesh);
		if (!RenderMesh->GetSkeleton())
		{
			USkeleton* Skeleton = NewObject<USkeleton>();
			KeepAlive.Emplace(Skeleton);
			Skeleton->MergeAllBonesToBoneTree(RenderMesh, false);
			RenderMesh->SetSkeleton(Skeleton);
		}
		return RenderMesh;
	}

	/** One or more whole primitives as a temporary skinned source, retaining source poses and named materials. */
	USkeletalMesh* MakePart(const FMantleGlbMesh& Mesh, const TArray<UMaterialInterface*>& Materials,
		TConstArrayView<int32> Primitives, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& Error)
	{
		FMantleSkinnedMesh Part;
		for (const FMantleGlbJoint& Joint : Mesh.Joints)
		{
			Part.Bones.Add({ Joint.Name, Joint.Parent });
			Part.ReferencePose.Add(Joint.Local);
		}
		FMantleSkinnedMeshLOD& LOD = Part.LODs.AddDefaulted_GetRef();
		LOD.NumInfluences = Mesh.NumInfluences;
		TArray<FSkeletalMaterial> PartMaterials;
		for (const int32 PrimitiveIndex : Primitives)
		{
			if (!Mesh.Primitives.IsValidIndex(PrimitiveIndex)) { Error = TEXT("outfit source part names no primitive"); return nullptr; }
			const FMantleGlbPrimitive& Primitive = Mesh.Primitives[PrimitiveIndex];
			if (!Mesh.MaterialNames.IsValidIndex(Primitive.MaterialIndex)) { Error = TEXT("outfit source part has no named material"); return nullptr; }
			const int32 First = LOD.Positions.Num();
			const int32 MaterialIndex = PartMaterials.Num();
			PartMaterials.Add(FSkeletalMaterial(Materials.IsValidIndex(Primitive.MaterialIndex) ? Materials[Primitive.MaterialIndex] : nullptr,
				FName(*Mesh.MaterialNames[Primitive.MaterialIndex])));
			FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
			Section.MaterialIndex = MaterialIndex;
			Section.MaterialName = FName(*Mesh.MaterialNames[Primitive.MaterialIndex]);
			Section.FirstIndex = LOD.Indices.Num();
			Section.IndexCount = Primitive.Indices.Num();
			for (const uint32 Index : Primitive.Indices) { LOD.Indices.Add(First + Index); }
			for (int32 Local = 0; Local < Primitive.NumVertices; ++Local)
			{
				const int32 Vertex = Primitive.FirstVertex + Local;
				LOD.Positions.Add(Mesh.Positions[Vertex]);
				LOD.Normals.Add(Mesh.Normals[Vertex]);
				LOD.UVs.Add(Mesh.UVs[Vertex]);
				if (!Mesh.Tangents.IsEmpty()) { LOD.Tangents.Add(Mesh.Tangents[Vertex]); }
				int32 Sum = 0, Largest = 0;
				const int32 Base = LOD.InfluenceWeights.Num();
				for (int32 Slot = 0; Slot < Mesh.NumInfluences; ++Slot)
				{
					const int32 Input = Vertex * Mesh.NumInfluences + Slot;
					const int32 Weight = FMath::Clamp(FMath::RoundToInt32(Mesh.InfluenceWeights[Input] * 255.0f), 0, 255);
					LOD.InfluenceBones.Add(Mesh.InfluenceJoints[Input]);
					LOD.InfluenceWeights.Add(static_cast<uint8>(Weight));
					if (Weight > LOD.InfluenceWeights[Base + Largest]) { Largest = Slot; }
					Sum += Weight;
				}
				const int32 Corrected = LOD.InfluenceWeights[Base + Largest] + 255 - Sum;
				if (Corrected < 0 || Corrected > 255) { Error = TEXT("outfit source weights cannot be represented by the upload format"); return nullptr; }
				LOD.InfluenceWeights[Base + Largest] = static_cast<uint8>(Corrected);
			}
		}
		FMantleSkinnedMeshBuildParams Params;
		Params.bOwnSkeleton = true;
		Params.Materials = MoveTemp(PartMaterials);
		USkeletalMesh* Result = FMantleRuntimeSkeletalMeshBuilder::Build(Part, Params, &Error);
		if (Result) { KeepAlive.Emplace(Result); }
		return Result;
	}

	/** One UV tile's triangles of a primitive, as a compact sub-mesh: Vertices[i] is the mesh vertex of local vertex i. */
	struct FTileSubset
	{
		TArray<int32> Vertices;
		TArray<FVector2f> UVs;
		TArray<uint32> Indices;
	};

	/** A primitive's triangles grouped by the UV tile their centroid falls in. A seller's export can weld body and head
	 * into one section with the body in its own tile (the bundled body of a Fab .mhpkg outfit did); each tile is then a
	 * candidate body. */
	TArray<FTileSubset> SplitByUVTile(const FMantleGlbMesh& Mesh, const FMantleGlbPrimitive& Primitive)
	{
		TMap<FIntPoint, int32> TileOf;
		TArray<FTileSubset> Tiles;
		TArray<TMap<int32, int32>> LocalOf;
		for (int32 First = 0; First + 2 < Primitive.Indices.Num(); First += 3)
		{
			FVector2f Centroid(0.0f, 0.0f);
			for (int32 Corner = 0; Corner < 3; ++Corner) { Centroid += Mesh.UVs[Primitive.FirstVertex + Primitive.Indices[First + Corner]] / 3.0f; }
			const FIntPoint Tile(FMath::FloorToInt32(Centroid.X), FMath::FloorToInt32(Centroid.Y));
			int32* Found = TileOf.Find(Tile);
			const int32 Index = Found ? *Found : TileOf.Add(Tile, Tiles.Num());
			if (!Found) { Tiles.AddDefaulted(); LocalOf.AddDefaulted(); }
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Vertex = Primitive.FirstVertex + Primitive.Indices[First + Corner];
				int32* Local = LocalOf[Index].Find(Vertex);
				if (!Local)
				{
					Local = &LocalOf[Index].Add(Vertex, Tiles[Index].Vertices.Num());
					Tiles[Index].Vertices.Add(Vertex);
					Tiles[Index].UVs.Add(Mesh.UVs[Vertex]);
				}
				Tiles[Index].Indices.Add(static_cast<uint32>(*Local));
			}
		}
		return Tiles;
	}

	/**
	 * The canonical body exported with half-precision UVs: every vertex lies within one 1/2048 step of its reference UV
	 * (exact Match quantizes at 1/2048, finer than a half float above 1.0). Each input vertex takes the one reference
	 * vertex within that step whose already-placed mesh neighbours are its reference neighbours; the result must be a
	 * complete bijection, and the mapped triangles may differ from the reference only by flipped quad diagonals (at most
	 * 1% of them). Outputs reference -> input vertex.
	 */
	bool MatchHalfPrecisionLayout(const FMantleBodyCorrespondence& Layout, TConstArrayView<FVector2f> UVs,
		TConstArrayView<uint32> InputIndices, TArray<int32>& OutInputOfReference, FString& Error)
	{
		const int32 NumVertices = Layout.QuantizedUVs.Num();
		if (UVs.Num() != NumVertices || InputIndices.Num() != Layout.Indices.Num())
		{
			Error = TEXT("the body has a different vertex or triangle count from the complete reference layout"); return false;
		}
		TMultiMap<FIntPoint, int32> ReferenceAt;
		for (int32 Reference = 0; Reference < NumVertices; ++Reference) { ReferenceAt.Add(Layout.QuantizedUVs[Reference], Reference); }
		const auto Neighbours = [NumVertices](TConstArrayView<uint32> Indices)
		{
			TArray<TSet<int32>> Out;
			Out.SetNum(NumVertices);
			for (int32 First = 0; First + 2 < Indices.Num(); First += 3)
			{
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					Out[Indices[First + Corner]].Add(Indices[First + (Corner + 1) % 3]);
					Out[Indices[First + Corner]].Add(Indices[First + (Corner + 2) % 3]);
				}
			}
			return Out;
		};
		for (const uint32 Index : InputIndices) { if (Index >= static_cast<uint32>(NumVertices)) { Error = TEXT("the body has an out-of-range index"); return false; } }
		const TArray<TSet<int32>> ReferenceNeighbours = Neighbours(Layout.Indices);
		const TArray<TSet<int32>> InputNeighbours = Neighbours(InputIndices);
		TArray<TArray<int32>> Candidates;
		Candidates.SetNum(NumVertices);
		for (int32 Input = 0; Input < NumVertices; ++Input)
		{
			const FVector2f& UV = UVs[Input];
			if (UV.ContainsNaN() || FMath::Abs(UV.X) > 16.0f || FMath::Abs(UV.Y) > 16.0f) { Error = TEXT("the body has invalid or out-of-range UVs"); return false; }
			const FIntPoint Quantized(FMath::RoundToInt32(UV.X * 2048.0f), FMath::RoundToInt32(UV.Y * 2048.0f));
			for (int32 DU = -1; DU <= 1; ++DU)
			{
				for (int32 DV = -1; DV <= 1; ++DV) { ReferenceAt.MultiFind(Quantized + FIntPoint(DU, DV), Candidates[Input]); }
			}
			if (Candidates[Input].IsEmpty()) { Error = TEXT("the body does not have the reference UV layout"); return false; }
		}
		TArray<int32> ReferenceOfInput;
		ReferenceOfInput.Init(INDEX_NONE, NumVertices);
		OutInputOfReference.Init(INDEX_NONE, NumVertices);
		int32 Placed = 0;
		for (bool bChanged = true; bChanged; )
		{
			bChanged = false;
			for (int32 Input = 0; Input < NumVertices; ++Input)
			{
				if (ReferenceOfInput[Input] != INDEX_NONE) { continue; }
				int32 Only = INDEX_NONE, Count = 0;
				for (const int32 Reference : Candidates[Input])
				{
					if (OutInputOfReference[Reference] != INDEX_NONE) { continue; }
					bool bFits = true;
					for (const int32 Neighbour : InputNeighbours[Input])
					{
						if (ReferenceOfInput[Neighbour] != INDEX_NONE && !ReferenceNeighbours[Reference].Contains(ReferenceOfInput[Neighbour])) { bFits = false; break; }
					}
					if (bFits) { Only = Reference; ++Count; }
				}
				if (Count == 1) { ReferenceOfInput[Input] = Only; OutInputOfReference[Only] = Input; ++Placed; bChanged = true; }
			}
		}
		// A flipped diagonal leaves its four vertices with one neighbour more or less than the reference: place the last
		// unplaced vertices by their only remaining candidate, and let the triangle check below judge the result.
		for (int32 Input = 0; Input < NumVertices; ++Input)
		{
			if (ReferenceOfInput[Input] != INDEX_NONE) { continue; }
			TArray<int32, TInlineAllocator<4>> Free;
			for (const int32 Reference : Candidates[Input]) { if (OutInputOfReference[Reference] == INDEX_NONE) { Free.Add(Reference); } }
			if (Free.Num() != 1) { Error = TEXT("the body's UV/connectivity correspondence is ambiguous"); return false; }
			ReferenceOfInput[Input] = Free[0]; OutInputOfReference[Free[0]] = Input; ++Placed;
		}
		if (Placed != NumVertices) { Error = TEXT("the body does not map completely onto the reference layout"); return false; }
		const auto Key = [](int32 A, int32 B, int32 C)
		{
			if (A > B) { Swap(A, B); } if (B > C) { Swap(B, C); } if (A > B) { Swap(A, B); }
			return FIntVector(A, B, C);
		};
		TMap<FIntVector, int32> Difference;
		for (int32 First = 0; First + 2 < Layout.Indices.Num(); First += 3)
		{
			++Difference.FindOrAdd(Key(Layout.Indices[First], Layout.Indices[First + 1], Layout.Indices[First + 2]));
			--Difference.FindOrAdd(Key(ReferenceOfInput[InputIndices[First]], ReferenceOfInput[InputIndices[First + 1]], ReferenceOfInput[InputIndices[First + 2]]));
		}
		TArray<FIntVector> ReferenceOnly, InputOnly;
		for (const TPair<FIntVector, int32>& Entry : Difference)
		{
			for (int32 N = 0; N < FMath::Abs(Entry.Value); ++N) { (Entry.Value > 0 ? ReferenceOnly : InputOnly).Add(Entry.Key); }
		}
		if (ReferenceOnly.Num() != InputOnly.Num() || ReferenceOnly.Num() > FMath::Max(4, Layout.Indices.Num() / 3 / 100) || ReferenceOnly.Num() % 2)
		{
			Error = TEXT("the mapped body triangles differ from the reference layout"); return false;
		}
		// Every differing pair must be the same quad with its other diagonal.
		const auto Corners = [](const FIntVector& T) { return TSet<int32>{ T.X, T.Y, T.Z }; };
		TArray<bool> InputUsed;
		InputUsed.Init(false, InputOnly.Num());
		for (int32 A = 0; A < ReferenceOnly.Num(); ++A)
		{
			TSet<int32> Quad = Corners(ReferenceOnly[A]);
			int32 Partner = INDEX_NONE;
			for (int32 B = 0; B < ReferenceOnly.Num() && Partner == INDEX_NONE; ++B)
			{
				if (B != A && Corners(ReferenceOnly[B]).Intersect(Quad).Num() == 2) { Partner = B; }
			}
			if (Partner == INDEX_NONE) { Error = TEXT("the mapped body triangles differ from the reference layout"); return false; }
			Quad.Append(Corners(ReferenceOnly[Partner]));
			int32 Inside = 0;
			for (int32 C = 0; C < InputOnly.Num(); ++C)
			{
				if (Corners(InputOnly[C]).Difference(Quad).IsEmpty()) { ++Inside; InputUsed[C] = true; }
			}
			if (Quad.Num() != 4 || Inside != 2) { Error = TEXT("the mapped body triangles differ from the reference layout"); return false; }
		}
		for (const bool bUsed : InputUsed) { if (!bUsed) { Error = TEXT("the mapped body triangles differ from the reference layout"); return false; } }
		Error.Reset();
		return true;
	}

	/** The source body in the reference layout's own order, UVs and triangles, carrying the input's positions, normals and
	 * skin. Downstream checks (FindReferenceLOD, body shape) then see the canonical topology exactly. */
	USkeletalMesh* MakeCanonicalBody(const FMantleGlbMesh& Mesh, const TArray<UMaterialInterface*>& Materials, int32 MaterialIndex,
		const FMantleBodyCorrespondence& Layout, TConstArrayView<int32> MeshVertexOfReference,
		TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& Error)
	{
		if (!Mesh.MaterialNames.IsValidIndex(MaterialIndex)) { Error = TEXT("outfit source part has no named material"); return nullptr; }
		FMantleSkinnedMesh Part;
		for (const FMantleGlbJoint& Joint : Mesh.Joints)
		{
			Part.Bones.Add({ Joint.Name, Joint.Parent });
			Part.ReferencePose.Add(Joint.Local);
		}
		FMantleSkinnedMeshLOD& LOD = Part.LODs.AddDefaulted_GetRef();
		LOD.NumInfluences = Mesh.NumInfluences;
		TArray<FSkeletalMaterial> PartMaterials;
		PartMaterials.Add(FSkeletalMaterial(Materials.IsValidIndex(MaterialIndex) ? Materials[MaterialIndex] : nullptr, FName(*Mesh.MaterialNames[MaterialIndex])));
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = 0;
		Section.MaterialName = FName(*Mesh.MaterialNames[MaterialIndex]);
		Section.FirstIndex = 0;
		Section.IndexCount = Layout.Indices.Num();
		LOD.Indices.Append(Layout.Indices);
		for (int32 Reference = 0; Reference < Layout.QuantizedUVs.Num(); ++Reference)
		{
			const int32 Vertex = MeshVertexOfReference[Reference];
			LOD.Positions.Add(Mesh.Positions[Vertex]);
			LOD.Normals.Add(Mesh.Normals[Vertex]);
			LOD.UVs.Add(FVector2f(Layout.QuantizedUVs[Reference].X / 2048.0f, Layout.QuantizedUVs[Reference].Y / 2048.0f));
			if (!Mesh.Tangents.IsEmpty()) { LOD.Tangents.Add(Mesh.Tangents[Vertex]); }
			int32 Sum = 0, Largest = 0;
			const int32 Base = LOD.InfluenceWeights.Num();
			for (int32 Slot = 0; Slot < Mesh.NumInfluences; ++Slot)
			{
				const int32 Input = Vertex * Mesh.NumInfluences + Slot;
				const int32 Weight = FMath::Clamp(FMath::RoundToInt32(Mesh.InfluenceWeights[Input] * 255.0f), 0, 255);
				LOD.InfluenceBones.Add(Mesh.InfluenceJoints[Input]);
				LOD.InfluenceWeights.Add(static_cast<uint8>(Weight));
				if (Weight > LOD.InfluenceWeights[Base + Largest]) { Largest = Slot; }
				Sum += Weight;
			}
			const int32 Corrected = LOD.InfluenceWeights[Base + Largest] + 255 - Sum;
			if (Corrected < 0 || Corrected > 255) { Error = TEXT("outfit source weights cannot be represented by the upload format"); return nullptr; }
			LOD.InfluenceWeights[Base + Largest] = static_cast<uint8>(Corrected);
		}
		FMantleSkinnedMeshBuildParams Params;
		Params.bOwnSkeleton = true;
		Params.Materials = MoveTemp(PartMaterials);
		USkeletalMesh* Result = FMantleRuntimeSkeletalMeshBuilder::Build(Part, Params, &Error);
		if (Result) { KeepAlive.Emplace(Result); }
		return Result;
	}

	/** Select a complete canonical body by geometry-free layout, never by material/asset names. */
	bool MakeSourceParts(USkeletalMesh& Combined, const FMantleAvatarContract& Contract,
		USkeletalMesh*& Body, USkeletalMesh*& Face, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& Error)
	{
		Finish(&Combined);
		if (FMantleCosmeticExport::FindReferenceLOD(&Combined, Contract) != INDEX_NONE)
		{
			Body = &Combined;
			return true;
		}
		const FMantleBodyCorrespondence* Layout = FMantleBodyCorrespondence::Get();
		if (!Layout) { Error = TEXT("the plugin's canonical body layout is missing"); return false; }
		for (int32 LOD = 0; LOD < UMantleBodyTopologyData::GetNumLODs(&Combined); ++LOD)
		{
			FMantleBodyLODTopology Topology;
			FString TopologyError;
			if (UMantleBodyTopologyData::ReadLOD(&Combined, LOD, Topology, nullptr, nullptr, &TopologyError))
			{
				UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_BODY asset=%s lod=%d raw=%s poseBones=%d"),
					*Combined.GetPathName(), LOD, *Topology.Describe(), Combined.GetRefSkeleton().GetNum());
			}
			else { UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_BODY asset=%s lod=%d rawError=%s"), *Combined.GetPathName(), LOD, *TopologyError); }
			FMantleGlbMesh Mesh;
			TArray<UMaterialInterface*> Materials;
			int32 Triangles = 0;
			if (!FMantleCosmeticExport::ReadSkeletalMesh(&Combined, LOD, Mesh, Materials, Triangles, Error))
			{
				UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_BODY asset=%s lod=%d readError=%s"), *Combined.GetPathName(), LOD, *Error);
				continue;
			}
			int32 FaceBones = 0;
			for (const FMantleGlbJoint& Joint : Mesh.Joints) { FaceBones += Joint.Name.ToString().StartsWith(TEXT("FACIAL_")) ? 1 : 0; }
			TArray<uint32> WholeIndices;
			for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
			{
				for (const uint32 Index : Primitive.Indices) { WholeIndices.Add(Primitive.FirstVertex + Index); }
			}
			TArray<int32> WholeMap;
			FString WholeError;
			const bool bWholeMatch = Layout->Match(Mesh.UVs, WholeIndices, WholeMap, WholeError);
			UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_BODY asset=%s lod=%d readVerts=%d readTris=%d primitives=%d materials=%s joints=%d facialBones=%d influences=%d wholeCanonical=%d reason=%s"),
				*Combined.GetPathName(), LOD, Mesh.Positions.Num(), Triangles, Mesh.Primitives.Num(), *FString::Join(Mesh.MaterialNames, TEXT(",")),
				Mesh.Joints.Num(), FaceBones, Mesh.NumInfluences, bWholeMatch, *WholeError);
			for (int32 Index = 0; Index < Mesh.Primitives.Num(); ++Index)
			{
				const FMantleGlbPrimitive& Primitive = Mesh.Primitives[Index];
				const TConstArrayView<FVector2f> UVs = MakeArrayView(Mesh.UVs).Slice(Primitive.FirstVertex, Primitive.NumVertices);
				FVector2f MinUV(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
				FVector2f MaxUV(TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest());
				for (const FVector2f& UV : UVs)
				{
					MinUV.X = FMath::Min(MinUV.X, UV.X); MinUV.Y = FMath::Min(MinUV.Y, UV.Y);
					MaxUV.X = FMath::Max(MaxUV.X, UV.X); MaxUV.Y = FMath::Max(MaxUV.Y, UV.Y);
				}
				TArray<int32> Map;
				FString MatchError;
				const bool bMatch = Layout->Match(UVs, Primitive.Indices, Map, MatchError);
				UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_SECTION asset=%s lod=%d primitive=%d first=%d verts=%d tris=%d materialIndex=%d material=%s uvHash=%s uvMin=(%.6f,%.6f) uvMax=(%.6f,%.6f) canonical=%d reason=%s"),
					*Combined.GetPathName(), LOD, Index, Primitive.FirstVertex, Primitive.NumVertices, Primitive.Indices.Num() / 3,
					Primitive.MaterialIndex, Mesh.MaterialNames.IsValidIndex(Primitive.MaterialIndex) ? *Mesh.MaterialNames[Primitive.MaterialIndex] : TEXT("<missing>"),
					*UMantleBodyTopologyData::FormatHash(UMantleBodyTopologyData::ComputeUVHash(UVs)), MinUV.X, MinUV.Y, MaxUV.X, MaxUV.Y, bMatch, *MatchError);
				if (!bMatch) { continue; }
				const int32 BodyPart[] = { Index };
				Body = MakePart(Mesh, Materials, BodyPart, KeepAlive, Error);
				if (!Body || FMantleCosmeticExport::FindReferenceLOD(Body, Contract) == INDEX_NONE) { return false; }
				// Other sections are the associated head in the documented combined-body/head input.
				// Only a source containing supported MetaHuman face bones can become an optional head proxy.
				const bool bHasFace = Mesh.Joints.ContainsByPredicate([](const FMantleGlbJoint& Joint)
				{
					return Joint.Name.ToString().StartsWith(TEXT("FACIAL_"));
				});
				if (bHasFace && Mesh.Primitives.Num() > 1)
				{
					TArray<int32> HeadParts;
					for (int32 Other = 0; Other < Mesh.Primitives.Num(); ++Other) { if (Other != Index) { HeadParts.Add(Other); } }
					Face = MakePart(Mesh, Materials, HeadParts, KeepAlive, Error);
					if (!Face) { return false; }
				}
				return true;
			}
		}
		// No exact section: a seller's export may weld body and head into one section (the body in its own UV tile) and
		// store UVs at half precision. Accept one tile that is the reference body within those limits, rebuilt in the
		// reference layout. The head tile is not used: a combined body without MetaHuman face bones has no face proxy.
		for (int32 LOD = 0; LOD < UMantleBodyTopologyData::GetNumLODs(&Combined); ++LOD)
		{
			FMantleGlbMesh Mesh;
			TArray<UMaterialInterface*> Materials;
			int32 Triangles = 0;
			FString ReadError;
			if (!FMantleCosmeticExport::ReadSkeletalMesh(&Combined, LOD, Mesh, Materials, Triangles, ReadError)) { continue; }
			for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
			{
				for (const FTileSubset& Tile : SplitByUVTile(Mesh, Primitive))
				{
					if (Tile.Vertices.Num() != Layout->QuantizedUVs.Num() || Tile.Indices.Num() != Layout->Indices.Num()) { continue; }
					TArray<int32> LocalOfReference;
					FString MatchError;
					const bool bMatch = MatchHalfPrecisionLayout(*Layout, Tile.UVs, Tile.Indices, LocalOfReference, MatchError);
					UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_TILE_BODY asset=%s lod=%d material=%s canonical=%d reason=%s"),
						*Combined.GetPathName(), LOD, Mesh.MaterialNames.IsValidIndex(Primitive.MaterialIndex) ? *Mesh.MaterialNames[Primitive.MaterialIndex] : TEXT("<missing>"),
						bMatch, *MatchError);
					if (!bMatch) { continue; }
					TArray<int32> MeshVertexOfReference;
					for (const int32 Local : LocalOfReference) { MeshVertexOfReference.Add(Tile.Vertices[Local]); }
					Body = MakeCanonicalBody(Mesh, Materials, Primitive.MaterialIndex, *Layout, MeshVertexOfReference, KeepAlive, Error);
					return Body && FMantleCosmeticExport::FindReferenceLOD(Body, Contract) != INDEX_NONE;
				}
			}
		}
		Error = FString::Printf(TEXT("outfit source body %s has no complete body section matching Mantle's canonical MetaHuman topology"), *Combined.GetName());
		return false;
	}

	/** Draft's Outfit narrowed to one authored size (the asset itself when Selected is empty). */
	UChaosOutfitAsset* IsolateSize(UChaosOutfitAsset* Asset, const FString& Selected, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError)
	{
		if (Selected.IsEmpty()) { return Asset; }
		UChaosOutfit* Filtered = NewObject<UChaosOutfit>();
		KeepAlive.Emplace(Filtered);
		Filtered->Append(*Asset->GetOutfit(), Selected);
		UChaosOutfitAsset* Single = NewObject<UChaosOutfitAsset>();
		KeepAlive.Emplace(Single);
		Single->Build(Filtered);
		Finish(Single);
		UE::Chaos::OutfitAsset::FCollectionOutfitConstFacade SingleCollection(Single->GetOutfitCollection());
		if (!SingleCollection.IsValid() || SingleCollection.GetNumBodySizes() != 1 || SingleCollection.GetBodySizeName(0) != Selected)
		{
			OutError = TEXT("the Outfit could not isolate the explicitly selected authored size"); return nullptr;
		}
		return Single;
	}

	bool ReduceToLimit(USkeletalMesh* Garment, const TArray<int32>* Keep, const FString& Slot, int32 Limit, int32& OutKept, int32& OutAfter, FString& OutError);

	/**
	 * Sections are identified by material name, and an Outfit often draws two pieces with one material (a left and right
	 * shoe). Numbers the repeats on this transient copy so each section keeps a name of its own.
	 */
	void NumberRepeatedSlots(USkeletalMesh& Garment)
	{
		TSet<FString> Names;
		TArray<FSkeletalMaterial>& Materials = Garment.GetMaterials();
		for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			const UMaterialInterface* Material = Materials[Slot].MaterialInterface.Get();
			const FString Name = !Materials[Slot].MaterialSlotName.IsNone() ? Materials[Slot].MaterialSlotName.ToString()
				: Material ? Material->GetName() : FString::Printf(TEXT("Material%d"), Slot);
			FString Unique = Name;
			for (int32 Suffix = 2; Names.Contains(Unique); ++Suffix) { Unique = FString::Printf(TEXT("%s_%d"), *Name, Suffix); }
			if (Unique != Name) { Materials[Slot].MaterialSlotName = FName(*Unique); }
			Names.Add(Unique);
		}
	}

	/**
	 * Single's skinned render geometry as a transient skeletal mesh: KeepSlots (Asset's material slots) bound to its own slots
	 * in OutKeepSlots, and the kept sections reduced toward 95% of Limit when over it (OutKept before, OutAfter after).
	 */
	USkeletalMesh* ExportGarment(UChaosOutfitAsset& Asset, UChaosOutfitAsset& Single, const TOptional<TArray<int32>>& KeepSlots, const FString& Slot,
		const int32 Limit, TOptional<TArray<int32>>& OutKeepSlots, int32& OutKept, int32& OutAfter, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError)
	{
		FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ChaosClothAssetTools"));
		USkeletalMesh* Garment = NewObject<USkeletalMesh>();
		KeepAlive.Emplace(Garment);
		if (!Single.ExportToSkeletalMesh(*Garment)) { OutError = TEXT("the selected Outfit source size could not export its skinned render geometry"); return nullptr; }
		Finish(Garment);
		USkeleton* Skeleton = NewObject<USkeleton>();
		KeepAlive.Emplace(Skeleton);
		Skeleton->MergeAllBonesToBoneTree(Garment, false);
		Garment->SetSkeleton(Skeleton);
		OutKeepSlots = KeepSlots;
		if (KeepSlots.IsSet())
		{
			// KeepSlots names the Outfit's material slots; the exported mesh numbers its own. Bind them by material.
			const TArray<FSkeletalMaterial>& SourceMaterials = Asset.GetMaterials();
			TSet<UMaterialInterface*> KeptMaterials;
			for (const int32 Kept : KeepSlots.GetValue())
			{
				if (!SourceMaterials.IsValidIndex(Kept) || !SourceMaterials[Kept].MaterialInterface) { OutError = TEXT("the selected Outfit material slot is missing"); return nullptr; }
				KeptMaterials.Add(SourceMaterials[Kept].MaterialInterface.Get());
			}
			TArray<int32> ExportedSlots;
			const TArray<FSkeletalMaterial>& ExportedMaterials = Garment->GetMaterials();
			for (int32 Exported = 0; Exported < ExportedMaterials.Num(); ++Exported)
			{
				if (KeptMaterials.Contains(ExportedMaterials[Exported].MaterialInterface.Get())) { ExportedSlots.Add(Exported); }
			}
			if (ExportedSlots.IsEmpty()) { OutError = TEXT("the selected Outfit materials are missing from its exported mesh"); return nullptr; }
			OutKeepSlots = MoveTemp(ExportedSlots);
		}
		if (!ReduceToLimit(Garment, OutKeepSlots.GetPtrOrNull(), Slot, Limit, OutKept, OutAfter, OutError)) { return nullptr; }
		return Garment;
	}

	/**
	 * An Outfit's render mesh is often far over a slot's budget (a Fab outfit was 25,846 triangles for a 6,000 slot):
	 * reduce Garment's LOD 0 as the converter's Triangles fix does, aiming the kept sections at 95% of Limit.
	 */
	/** LOD 0 of a resized garment pushed out of Body's skin (its reference LOD, else LOD 0), as the refit pushes a garment out. */
	bool PushResizedOutOfBody(USkeletalMesh& Garment, const USkeletalMesh& Body, const FMantleAvatarContract& Contract, FString& OutNote, FString& OutError)
	{
		const int32 BodyLOD = FMath::Max(0, FMantleCosmeticExport::FindReferenceLOD(&Body, Contract));
		FMantleSkinnedMeshLOD Surface;
		if (!FMantleNailSeating::ReadBodySurface(Body, BodyLOD, Surface, OutError)) { return false; }
		Finish(&Garment);
		FMeshDescription* Description = Garment.HasMeshDescription(0) ? Garment.GetMeshDescription(0) : nullptr;
		if (!Description && Garment.GetImportedModel() && Garment.GetImportedModel()->LODModels.IsValidIndex(0))
		{
			FMeshDescription Converted;
			Garment.GetImportedModel()->LODModels[0].GetMeshDescription(&Garment, 0, Converted);
			Description = Garment.CreateMeshDescription(0, MoveTemp(Converted));
		}
		if (!Description) { OutError = TEXT("the resized garment has no geometry to push out of the body"); return false; }
		const auto Clearance = [Description, &Surface]()
		{
			TArray<FVector3f> Points;
			const TVertexAttributesConstRef<FVector3f> Positions = Description->GetVertexPositions();
			for (const FVertexID Vertex : Description->Vertices().GetElementIDs()) { Points.Add(Positions[Vertex]); }
			return FMantleGarmentOutfit::DescribeClearance(Points, Surface);
		};
		const FString Before = Clearance();
		FMantleRefitPushOut Pushed;
		if (!FMantleAvatarRefit::PushOutOfBody(*Description, Surface, Pushed, OutError, /*bCoverSkin*/ true)) { return false; }
		UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_GARMENT_FIT resized onto %s: %s; pushed out: %s"), *Body.GetName(), *Before, *Clearance());
		OutNote = Pushed.ToString();
		if (Pushed.Moved > 0)
		{
			Garment.PreEditChange(nullptr);
			USkeletalMesh::FCommitMeshDescriptionParams Params;
			Params.bForceUpdate = true;
			Garment.CommitMeshDescription(0, Params);
			Garment.PostEditChange();
			Finish(&Garment);
		}
		return true;
	}

	bool ReduceToLimit(USkeletalMesh* Garment, const TArray<int32>* Keep, const FString& Slot, const int32 Limit, int32& OutKept, int32& OutAfter, FString& OutError)
	{
		OutKept = FMantleCosmeticExport::CountTriangles(Garment, 0, Keep);
		OutAfter = OutKept;
		if (Limit > 0 && OutKept > Limit)
		{
			// The reducer works on the whole mesh, and the kept sections need not shrink in proportion: aim the whole at the
			// kept share, then correct by what the kept sections came to (each pass reduces the source geometry afresh).
			const int32 Aim = FMantleAvatarTrianglesFixer::TargetTriangles(Limit, FMantleAvatarTrianglesFixer::TargetShare);
			int32 Target = FMath::Max(1, static_cast<int32>(static_cast<int64>(FMantleCosmeticExport::CountTriangles(Garment, 0, nullptr)) * Aim / OutKept));
			for (int32 Pass = 0; Pass < 4 && OutAfter > Aim; ++Pass)
			{
				FString ReduceError;
				if (!FMantleAvatarTrianglesFixer::ReduceLODZero(Garment, Target, ReduceError))
				{
					OutError = FString::Printf(TEXT("%d triangles, the limit is %d, and reducing it failed: %s"), OutKept, Limit, *ReduceError); return false;
				}
				OutAfter = FMantleCosmeticExport::CountTriangles(Garment, 0, Keep);
				UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_REDUCED slot=%s pass=%d kept=%d target=%d after=%d limit=%d"), *Slot, Pass, OutKept, Target, OutAfter, Limit);
				if (OutAfter <= 0) { break; }
				Target = FMath::Max(1, static_cast<int32>(static_cast<int64>(Target) * Aim / OutAfter));
			}
		}
		return true;
	}
}

USkeletalMesh* FMantleParametricOutfit::MakeMesh(const FMantleGlbMesh& Mesh, const TArray<UMaterialInterface*>& Materials,
	TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError)
{
	TArray<int32> Primitives;
	for (int32 Index = 0; Index < Mesh.Primitives.Num(); ++Index)
	{
		Primitives.Add(Index);
	}
	return MantleParametricOutfitPrivate::MakePart(Mesh, Materials, Primitives, KeepAlive, OutError);
}

bool FMantleParametricOutfit::IsSource(const UObject* Source)
{
	return Source && Source->IsA<UChaosOutfitAsset>();
}

bool FMantleParametricOutfit::IsGarmentSource(const UObject* Source)
{
	return Source && !IsSource(Source) && (Source->IsA<USkeletalMesh>() || Source->IsA<UChaosClothAssetBase>());
}

bool FMantleParametricOutfit::FindSourceBody(const USkeletalMesh* AuthoringBody, TSharedPtr<void>& OutLease, USkeletalMesh*& OutBody, FString& OutError)
{
	OutBody = nullptr;
	OutLease.Reset();
	const FString* Built = AuthoringBody && FMantleAvatarConverter::IsConvertOutput(AuthoringBody)
		? AuthoringBody->GetPackage()->GetMetaData().FindValue(AuthoringBody, FMantleAvatarConverter::BuiltKey) : nullptr;
	const int32 Cut = Built ? Built->Find(TEXT("/Unpacked/")) : INDEX_NONE;
	if (Cut == INDEX_NONE)
	{
		OutError = TEXT("the authoring body is not one Mantle converted from a MetaHuman Creator character");
		return false;
	}
	const FString* ConvertedFrom = AuthoringBody->GetPackage()->GetMetaData().FindValue(AuthoringBody, FMantleAvatarConverter::ConvertedFromKey);
	const FAssetData Source = ConvertedFrom ? FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(*ConvertedFrom))
		: FAssetData();
	if (IsThirdPartyConversion(Source.IsValid() ? Source.AssetClassPath : FTopLevelAssetPath()))
	{
		// A third-party character's body conformed to a MetaHuman: its clothing goes through Make Outfit from garment, which
		// poses and fits the garment to that body, not straight onto it.
		OutError = TEXT("the authoring body is a third-party character's, converted to a MetaHuman");
		return false;
	}
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Built->Left(Cut)));
	Filter.ClassPaths.Add(UMetaHumanCharacter::StaticClass()->GetClassPathName());
	TArray<FAssetData> Found;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Found);
	UMetaHumanCharacter* Character = Found.Num() == 1 ? Cast<UMetaHumanCharacter>(Found[0].GetAsset()) : nullptr;
	UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
	if (!Character || !Subsystem)
	{
		OutError = TEXT("the MetaHuman Creator character the authoring body was converted from was not found");
		return false;
	}
	struct FLease
	{
		TWeakObjectPtr<UMetaHumanCharacterEditorSubsystem> Subsystem;
		TStrongObjectPtr<UMetaHumanCharacter> Character;
		TStrongObjectPtr<USkeletalMesh> Merged;
		bool bRemove = false;
		~FLease()
		{
			if (bRemove && Subsystem.IsValid() && IsInGameThread()) { Subsystem->RemoveObjectToEdit(Character.Get()); }
		}
	};
	TSharedRef<FLease> Lease = MakeShared<FLease>();
	Lease->Subsystem = Subsystem;
	Lease->Character.Reset(Character);
	const bool bAlreadyEditing = Subsystem->IsObjectAddedForEditing(Character);
	if (!bAlreadyEditing && !Subsystem->TryAddObjectToEdit(Character))
	{
		OutError = TEXT("MetaHuman Creator could not open the authoring character");
		return false;
	}
	Lease->bRemove = !bAlreadyEditing;
	OutLease = Lease;
	FMetaHumanCharacterPreviewAssetOptions Options;
	Options.bGenerateMergedHeadAndBodyMesh = true;
	Options.bGenerateBodyMeasurements = true;
	FMetaHumanCharacterPreviewAssets Preview;
	if (!Subsystem->TryGetCharacterPreviewAssets(Character, Options, Preview) || !Preview.MergedHeadAndBodyMesh)
	{
		OutLease.Reset();
		OutError = TEXT("MetaHuman Creator could not provide the authoring character's merged body and head");
		return false;
	}
	// The subsystem owns the mesh; the lease keeps it from collection while the Outfit is in use.
	Lease->Merged.Reset(Preview.MergedHeadAndBodyMesh.Get());
	OutBody = Preview.MergedHeadAndBodyMesh.Get();
	return true;
}

USkeletalMesh* FMantleParametricOutfit::GarmentMesh(UObject* Garment, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError)
{
	if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Garment)) { return Mesh; }
	const UChaosClothAssetBase* Cloth = Cast<UChaosClothAssetBase>(Garment);
	if (!Cloth || Cloth->IsA<UChaosOutfitAsset>()) { OutError = TEXT("choose a skeletal mesh or a Chaos cloth asset"); return nullptr; }
	// Its render geometry as a skeletal mesh, the form the export reads.
	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("ChaosClothAssetTools"));
	USkeletalMesh* Mesh = NewObject<USkeletalMesh>(GetTransientPackage());
	KeepAlive.Emplace(Mesh);
	if (!Cloth->ExportToSkeletalMesh(*Mesh)) { OutError = TEXT("the cloth asset could not export its render geometry"); return nullptr; }
	MantleParametricOutfitPrivate::Finish(Mesh);
	if (!Mesh->GetSkeleton())
	{
		USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage());
		KeepAlive.Emplace(Skeleton);
		Skeleton->MergeAllBonesToBoneTree(Mesh, false);
		Mesh->SetSkeleton(Skeleton);
	}
	return Mesh;
}

UChaosOutfitAsset* FMantleParametricOutfit::WrapGarment(UObject* Garment, const TOptional<TArray<int32>>& KeepSlots, USkeletalMesh* SourceBody,
	TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError)
{
	USkeletalMesh* Mesh = GarmentMesh(Garment, KeepAlive, OutError);
	if (!Mesh) { return nullptr; }
	FMantleOutfitAuthoredSource Source;
	Source.Id = TEXT("authored");
	Source.Garment = Mesh;
	Source.CombinedBodyHead = SourceBody;
	Source.KeepSlots = KeepSlots;
	return FMantleParametricOutfitAuthoring::Create(MakeArrayView(&Source, 1), GetTransientPackage(), FName(*(TEXT("WrappedOutfit_") + Garment->GetName())),
		1500, KeepAlive, OutError);
}

UChaosOutfitAsset* FMantleParametricOutfit::WrapOnAuthoringBody(const FMantleCosmeticDraft& Draft, TArray<TStrongObjectPtr<UObject>>& KeepAlive,
	TSharedPtr<void>& Lease, FString& OutWhy)
{
	USkeletalMesh* SourceBody = nullptr;
	if (!Draft.AuthoringBody.IsValid())
	{
		OutWhy = TEXT("no MetaHuman authoring body was found for it");
		return nullptr;
	}
	if (!FindSourceBody(Draft.AuthoringBody.Get(), Lease, SourceBody, OutWhy)) { return nullptr; }
	// The authoring body is found by search, so check the garment really sits on it: a Chaos resize from the wrong body leaves
	// the garment floating or buried on every size. A garment made on its body touches it, so its closest vertices must
	// lie on the skin.
	USkeletalMesh* Worn = GarmentMesh(Draft.Source.Get(), KeepAlive, OutWhy);
	TArray<FVector3f> GarmentPoints, BodyPoints;
	if (!Worn) { return nullptr; }
	if (!FMantleFitBodies::ReadVertices(Worn, 0, GarmentPoints) || !FMantleFitBodies::ReadVertices(SourceBody, 0, BodyPoints) || BodyPoints.IsEmpty())
	{
		OutWhy = TEXT("the garment or its authoring body has no geometry to check the fit with");
		return nullptr;
	}
	TArray<float> Nearest;
	for (int32 Index = 0; Index < GarmentPoints.Num(); Index += 4)
	{
		float Best = TNumericLimits<float>::Max();
		for (const FVector3f& Point : BodyPoints) { Best = FMath::Min(Best, FVector3f::DistSquared(GarmentPoints[Index], Point)); }
		Nearest.Add(FMath::Sqrt(Best));
	}
	Nearest.Sort();
	auto Percentile = [&Nearest](const int32 Percent) { return Nearest.IsEmpty() ? TNumericLimits<float>::Max() : Nearest[FMath::Min(Nearest.Num() - 1, Nearest.Num() * Percent / 100)]; };
	UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_WRAP_FIT garment=%s body=%s p2=%.2f p5=%.2f p10=%.2f p20=%.2f p50=%.2f cm"), *Draft.Source->GetName(),
		*Draft.AuthoringBody->GetName(), Percentile(2), Percentile(5), Percentile(10), Percentile(20), Percentile(50));
	if (Percentile(5) > MantleParametricOutfitPrivate::WrapMaxCloseCm)
	{
		OutWhy = FString::Printf(TEXT("it does not sit on the authoring body %s found for it (its closest vertices are %.1f cm from the skin)"), *Draft.AuthoringBody->GetName(), Percentile(5));
		return nullptr;
	}
	return WrapGarment(Draft.Source.Get(), Draft.KeepSlots, SourceBody, KeepAlive, OutWhy);
}

bool FMantleParametricOutfit::IsThirdPartyConversion(const FTopLevelAssetPath& SourceClass)
{
	return !SourceClass.IsNull() && SourceClass != UMetaHumanCharacter::StaticClass()->GetClassPathName();
}

bool FMantleParametricOutfit::IsWholeBody(TConstArrayView<FVector3f> Points)
{
	if (Points.IsEmpty()) { return false; }
	float Low = Points[0].Z, High = Points[0].Z;
	for (const FVector3f& Point : Points) { Low = FMath::Min(Low, Point.Z); High = FMath::Max(High, Point.Z); }
	return High - Low >= MantleParametricOutfitPrivate::WholeBodyMinCm;
}

bool FMantleParametricOutfit::IsBareBody(TConstArrayView<FMantleOutfitSection> Sections, const FString& MeshName)
{
	bool bHasSkin = false;
	for (const FMantleOutfitSection& Section : Sections)
	{
		const EMantleOutfitPiece Piece = FMantleOutfitSections::Classify(Section);
		if (!FMantleOutfitSections::PieceSlot(Piece).IsEmpty()) { return false; }
		bHasSkin |= Piece == EMantleOutfitPiece::Skin;
	}
	const TArray<FString> Words = FMantleOutfitSections::Words(MeshName);
	const bool bNamedBody = Words.Contains(TEXT("body")) || Words.Contains(TEXT("nude")) || Words.Contains(TEXT("naked"));
	return (bHasSkin || bNamedBody) && !Words.Contains(TEXT("head")) && !Words.Contains(TEXT("face"));
}

USkeletalMesh* FMantleParametricOutfit::FindBodyBeside(USkeletalMesh* Garment)
{
	if (!Garment) { return nullptr; }
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FString Folder = FPackageName::GetLongPackagePath(Garment->GetOutermost()->GetName());
	// The garment's folder and up to two above it: a character's clothing sits in or under its body's folder.
	for (int32 Level = 0; Level < 3 && Folder.Len() > 1; ++Level, Folder = FPackageName::GetLongPackagePath(Folder))
	{
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Folder));
		Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
		TArray<FAssetData> Meshes;
		Registry.GetAssets(Filter, Meshes);
		// A body named for a body first; the Convert list's pairing decides in the end.
		Meshes.StableSort([](const FAssetData& A, const FAssetData& B)
		{
			return A.AssetName.ToString().Contains(TEXT("Body"), ESearchCase::IgnoreCase) && !B.AssetName.ToString().Contains(TEXT("Body"), ESearchCase::IgnoreCase);
		});
		for (const FAssetData& Data : Meshes)
		{
			if (Data.PackageName == Garment->GetPackage()->GetFName()) { continue; }
			USkeletalMesh* Body = Cast<USkeletalMesh>(Data.GetAsset());
			if (!Body || Body == Garment) { continue; }
			// A bare body: no clothing sections, a skin section, never a head (IsBareBody); the whole-body height below rules out a part such as feet or a torso.
			TArray<int32> Slots;
			if (!IsBareBody(FMantleCosmeticExport::ReadOutfitSections(Body, 0, Slots), Body->GetName()))
			{
				UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_BODY_BESIDE %s: not a bare body (clothing: %s)"), *Body->GetName(), *FMantleGarmentOutfit::WornClothing(*Body));
				continue;
			}
			// A whole body: feet, hands or a torso alone are not the body the clothing was made for.
			TArray<FVector3f> BodyPoints;
			if (!FMantleFitBodies::ReadVertices(Body, 0, BodyPoints) || !IsWholeBody(BodyPoints))
			{
				UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_BODY_BESIDE %s: not a whole body"), *Body->GetName());
				continue;
			}
			// The body names this garment among its separate clothing (the Convert list's own matching).
			TArray<FMantleConvertGarment> Garments;
			TArray<FString> NotOffered;
			FMantleGarmentOutfit::FindGarments(*Body, Garments, NotOffered);
			if (Garments.ContainsByPredicate([Garment](const FMantleConvertGarment& Entry) { return Entry.Mesh.Get() == Garment && Entry.Why.IsEmpty(); }))
			{
				UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_BODY_BESIDE %s is the body of %s"), *Body->GetName(), *Garment->GetName());
				return Body;
			}
			UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_BODY_BESIDE %s does not list %s"), *Body->GetName(), *Garment->GetName());
		}
	}
	return nullptr;
}

UChaosOutfitAsset* FMantleParametricOutfit::MakeOutfitBeside(const FMantleCosmeticDraft& Draft, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutWhy, FString* OutNote)
{
	USkeletalMesh* Garment = Cast<USkeletalMesh>(Draft.Source.Get());
	USkeletalMesh* Body = FindBodyBeside(Garment);
	if (!Body) { OutWhy = TEXT("no whole bare character body was found next to it"); return nullptr; }
	if (Draft.KeepSlots.IsSet()) { OutWhy = TEXT("Make Outfit from garment takes the whole mesh, not chosen sections"); return nullptr; }
	// Made once: an Outfit saved earlier from this garment by this version of Make is reused.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*(FPackageName::GetLongPackagePath(Body->GetOutermost()->GetName()) / TEXT("Converted"))));
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UChaosOutfitAsset::StaticClass()->GetClassPathName());
	TArray<FAssetData> Existing;
	Registry.GetAssets(Filter, Existing);
	for (const FAssetData& Data : Existing)
	{
		UChaosOutfitAsset* Outfit = Cast<UChaosOutfitAsset>(Data.GetAsset());
		const FString* MadeFrom = Outfit ? Outfit->GetPackage()->GetMetaData().FindValue(Outfit, FMantleGarmentOutfit::MadeFromGarmentKey) : nullptr;
		const FString* Version = Outfit ? Outfit->GetPackage()->GetMetaData().FindValue(Outfit, FMantleGarmentOutfit::MadeVersionKey) : nullptr;
		if (MadeFrom && *MadeFrom == Garment->GetPathName() && Version && *Version == FMantleGarmentOutfit::MadeVersion)
		{
			KeepAlive.Emplace(Outfit);
			return Outfit;
		}
	}
	FMantleGarmentOutfitResult Result;
	FString Json, Code;
	USkeletalMesh* Garments[] = { Garment };
	// Convert asks the creator to confirm the kept parts are bare skin. FindBodyBeside took a body with no clothing sections and a
	// skin section, which is what that line asserts, and Make works on copies of it, so the line is confirmed here.
	if (OutNote) { *OutNote = FString::Printf(TEXT("The bare-skin step was confirmed automatically for %s: it has no clothing sections and a skin section."), *Body->GetName()); }
	if (!FMantleGarmentOutfit::Make(*Body, Garments, FString(), TEXT("Strip.0.BareSkin"), Result, Json, Code, OutWhy)) { return nullptr; }
	const FMantleGarmentOutfitPiece* Piece = Result.Pieces.FindByPredicate([](const FMantleGarmentOutfitPiece& Entry) { return !Entry.Outfit.IsEmpty(); });
	UChaosOutfitAsset* Outfit = Piece ? Cast<UChaosOutfitAsset>(FSoftObjectPath(Piece->Outfit).TryLoad()) : nullptr;
	if (!Outfit)
	{
		OutWhy = Result.Pieces.IsEmpty() || Result.Pieces[0].Error.IsEmpty() ? FString(TEXT("Make Outfit from garment made nothing")) : Result.Pieces[0].Error;
		return nullptr;
	}
	KeepAlive.Emplace(Outfit);
	return Outfit;
}

bool FMantleParametricOutfit::PrepareWrapped(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract, FMantleCosmeticDraft& OutDraft,
	TArray<TStrongObjectPtr<UObject>>& KeepAlive, TSharedPtr<void>& Lease, FString& OutWhy, FString* OutRoute)
{
	// The first route that applies: the garment sits on a MetaHuman authoring body, or its character's bare body is beside it.
	FString WhyOnBody;
	UChaosOutfitAsset* Outfit = WrapOnAuthoringBody(Draft, KeepAlive, Lease, WhyOnBody);
	FString Route = TEXT("wrapped on its MetaHuman authoring body");
	if (!Outfit)
	{
		FString WhyBeside, NoteBeside;
		Outfit = MakeOutfitBeside(Draft, KeepAlive, WhyBeside, &NoteBeside);
		Route = FString::Printf(TEXT("made with Make Outfit from garment on its character's body. %s"), *NoteBeside);
		if (!Outfit)
		{
			OutWhy = FString::Printf(TEXT("%s; and %s"), *WhyOnBody, *WhyBeside);
			return false;
		}
	}
	if (OutRoute) { *OutRoute = Route; }
	OutDraft = Draft;
	OutDraft.Source = Outfit;
	OutDraft.KeepSlots.Reset();
	OutDraft.OutfitTargets.Reset();
	OutDraft.OutfitSourceSize.Reset();
	OutDraft.AuthoringBody.Reset();
	OutDraft.AuthoringFace.Reset();
	FMantleEditorActions::ApplyCosmeticFitTargetDefaults(OutDraft, &Contract);
	if (OutDraft.OutfitTargets.IsEmpty())
	{
		OutWhy = OutDraft.FitBodiesStatus.IsEmpty() ? FString(TEXT("the standard fit bodies were not made")) : OutDraft.FitBodiesStatus;
		return false;
	}
	return true;
}

bool FMantleParametricOutfit::ListSizes(UObject* Source, TArray<FMantleOutfitSourceSize>& OutSizes, FString& OutError)
{
	OutSizes.Reset();
	UChaosOutfitAsset* Asset = Cast<UChaosOutfitAsset>(Source);
	if (!Asset) { OutError = TEXT("choose a MetaHuman Outfit Asset"); return false; }
	MantleParametricOutfitPrivate::Finish(Asset);
	UE::Chaos::OutfitAsset::FCollectionOutfitConstFacade Collection(Asset->GetOutfitCollection());
	if (!Collection.IsValid() || !Asset->GetOutfit()) { OutError = TEXT("the Outfit has no evaluated source-size data; evaluate it in the Outfit editor first"); return false; }
	for (int32 SizeIndex = 0; SizeIndex < Collection.GetNumBodySizes(); ++SizeIndex)
	{
		FMantleOutfitSourceSize& Size = OutSizes.AddDefaulted_GetRef();
		Size.Name = Collection.GetBodySizeName(SizeIndex);
		for (const FSoftObjectPath& Path : Collection.GetBodySizeBodyPartsSkeletalMeshPaths(SizeIndex)) { Size.BodyParts.Add(Path); }
		for (const FGuid& Guid : Collection.GetOutfitGuids())
		{
			const TMap<FGuid, FString> Pieces = Collection.GetOutfitPieces(Guid, SizeIndex);
			if (!Pieces.IsEmpty()) { Size.OutfitGuids.Add(Guid); }
			for (const TPair<FGuid, FString>& Piece : Pieces) { Size.Pieces.AddUnique(Piece.Value); }
		}
		Size.Pieces.Sort();
	}
	OutSizes.Sort([](const FMantleOutfitSourceSize& A, const FMantleOutfitSourceSize& B) { return A.Name < B.Name; });
	if (OutSizes.IsEmpty()) { OutError = TEXT("the Outfit has no authored source sizes"); return false; }
	return true;
}

bool FMantleParametricOutfit::Prepare(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
	FMantleCosmeticDraft& OutDraft, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError, FString* OutNote)
{
	using namespace MantleParametricOutfitPrivate;
	OutDraft = Draft;
	TArray<FMantleOutfitSourceSize> Sizes;
	if (!ListSizes(Draft.Source.Get(), Sizes, OutError)) { return false; }
	FString Selected = Draft.OutfitSourceSize;
	if (Selected.IsEmpty() && Sizes.Num() == 1) { Selected = Sizes[0].Name; }
	FString AutoSizeNote;
	if (Selected.IsEmpty() && !Draft.OutfitAuthoringCharacter.IsValid())
	{
		// No Creator character to resize onto: export the first authored size whose body is the canonical MetaHuman body,
		// the one Mantle's runtime fit starts from. A creator who sold several sizes need not pick one.
		for (const FMantleOutfitSourceSize& Candidate : Sizes)
		{
			bool bCanonical = Candidate.BodyParts.Num() > 0;
			for (const FSoftObjectPath& Path : Candidate.BodyParts)
			{
				USkeletalMesh* Part = Cast<USkeletalMesh>(Path.TryLoad());
				USkeletalMesh* CandidateBody = nullptr;
				USkeletalMesh* CandidateFace = nullptr;
				TArray<TStrongObjectPtr<UObject>> Probe;
				FString ProbeError;
				if (!Part || !MakeSourceParts(*Part, Contract, CandidateBody, CandidateFace, Probe, ProbeError)) { bCanonical = false; break; }
			}
			if (bCanonical)
			{
				Selected = Candidate.Name;
				AutoSizeNote = FString::Printf(TEXT("Outfit size %s was chosen automatically: its body is the standard MetaHuman body."), *Selected);
				break;
			}
		}
	}
	const FMantleOutfitSourceSize* Size = Sizes.FindByPredicate([&Selected](const FMantleOutfitSourceSize& Candidate) { return Candidate.Name == Selected; });
	if (!Size && (!Selected.IsEmpty() || !Draft.OutfitAuthoringCharacter.IsValid()))
	{
		OutError = TEXT("choose a Creator authoring character for automatic native source-size selection"); return false;
	}
	UChaosOutfitAsset* Asset = CastChecked<UChaosOutfitAsset>(Draft.Source.Get());
	KeepAlive.Emplace(Asset);
	UChaosOutfitAsset* Single = IsolateSize(Asset, Selected, KeepAlive, OutError);
	if (!Single) { return false; }
	if (Draft.OutfitAuthoringCharacter.IsValid())
	{
		USkeletalMesh* Resized = ResizeOntoCharacter(*Single, Selected, Draft, Contract, KeepAlive, OutError);
		if (!Resized) { return false; }
		if (Draft.KeepSlots.IsSet())
		{
			// Native resizing changes slot numbers. Bind the selection to the chosen size's authored interfaces.
			UE::Chaos::OutfitAsset::FCollectionOutfitConstFacade SourceCollection(Asset->GetOutfitCollection());
			if (!SourceCollection.IsValid()) { OutError = TEXT("the native Outfit source collection is invalid"); return false; }
			const int32 SizeIndex = SourceCollection.FindBodySize(Selected);
			const FMantleOutfitSourceSize* SelectedSize = Sizes.FindByPredicate([&Selected](const FMantleOutfitSourceSize& Candidate) { return Candidate.Name == Selected; });
			if (SizeIndex == INDEX_NONE || !SelectedSize)
			{ OutError = TEXT("the resized Outfit did not identify a valid authored source size"); return false; }
			TSet<FGuid> PieceGuids, MatchedPieceGuids;
			for (const FGuid& OutfitId : SelectedSize->OutfitGuids)
			{
				for (const auto& Piece : SourceCollection.GetOutfitPieces(OutfitId, SizeIndex)) { PieceGuids.Add(Piece.Key); }
			}
			TSet<FSoftObjectPath> SizeMaterialPaths;
			const UChaosClothAssetBase& ClothAsset = *Asset;
			for (int32 Model = 0; Model < ClothAsset.GetNumClothSimulationModels(); ++Model)
			{
				const FGuid PieceGuid = ClothAsset.GetAssetGuid(Model);
				if (!PieceGuids.Contains(PieceGuid)) { continue; }
				MatchedPieceGuids.Add(PieceGuid);
				for (const auto& Collection : ClothAsset.GetCollections(Model))
				{
					UE::Chaos::ClothAsset::FCollectionClothConstFacade Cloth(Collection);
					if (!Cloth.IsValid()) { OutError = TEXT("the selected native Outfit model collection is invalid"); return false; }
					for (const FSoftObjectPath& Path : Cloth.GetRenderMaterialSoftObjectPathName()) { SizeMaterialPaths.Add(Path); }
				}
			}
			if (PieceGuids.IsEmpty() || MatchedPieceGuids.Num() != PieceGuids.Num())
			{ OutError = TEXT("the selected native Outfit size has missing piece models"); return false; }
			const TArray<FSkeletalMaterial>& SourceMaterials = Asset->GetMaterials();
			TSet<UMaterialInterface*> SelectedMaterials;
			for (const int32 Slot : Draft.KeepSlots.GetValue())
			{
				if (!SourceMaterials.IsValidIndex(Slot) || !SourceMaterials[Slot].MaterialInterface)
				{ OutError = TEXT("the selected native Outfit material slot is missing"); return false; }
				UMaterialInterface* Material = SourceMaterials[Slot].MaterialInterface.Get();
				if (SizeMaterialPaths.Contains(FSoftObjectPath(Material))) { SelectedMaterials.Add(Material); }
			}
			for (int32 Slot = 0; Slot < SourceMaterials.Num(); ++Slot)
			{
				if (SelectedMaterials.Contains(SourceMaterials[Slot].MaterialInterface.Get()) && !Draft.KeepSlots.GetValue().Contains(Slot))
				{ OutError = TEXT("select every native Outfit slot sharing a selected material before resizing"); return false; }
			}
			TArray<int32> ResizedSlots;
			TSet<UMaterialInterface*> MatchedMaterials;
			const TArray<FSkeletalMaterial>& ResizedMaterials = Resized->GetMaterials();
			for (int32 Slot = 0; Slot < ResizedMaterials.Num(); ++Slot)
			{
				UMaterialInterface* Material = ResizedMaterials[Slot].MaterialInterface.Get();
				if (SelectedMaterials.Contains(Material)) { ResizedSlots.Add(Slot); MatchedMaterials.Add(Material); }
			}
			if (SelectedMaterials.IsEmpty() || MatchedMaterials.Num() != SelectedMaterials.Num())
			{ OutError = TEXT("selected native Outfit materials are missing from its resized mesh"); return false; }
			OutDraft.KeepSlots = MoveTemp(ResizedSlots);
		}
		// The size made for this body is as heavy as the authored one: reduce it as the single-size route does.
		int32 Kept = 0, After = 0;
		if (!ReduceToLimit(Resized, OutDraft.KeepSlots.GetPtrOrNull(), Draft.Slot, Contract.SkinnedMaxTriangles(Draft.Slot), Kept, After, OutError)) { return false; }
		if (OutNote && After != Kept) { *OutNote = FString::Printf(TEXT("Reduced from %d to %d triangles to fit the %d limit."), Kept, After, Contract.SkinnedMaxTriangles(Draft.Slot)); }
		// An Outfit made from a third-party garment (FMantleGarmentOutfit) was pushed out of its authored body's skin; the resize
		// can sink it into a bigger body again, so each size is pushed out of its own body as well.
		const FString* MadeFrom = Asset->GetPackage()->GetMetaData().FindValue(Asset, FMantleGarmentOutfit::MadeFromGarmentKey);
		if (MadeFrom && Draft.AuthoringBody.IsValid())
		{
			FString PushNote;
			if (!PushResizedOutOfBody(*Resized, *Draft.AuthoringBody.Get(), Contract, PushNote, OutError)) { return false; }
			UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_PUSH_OUT size=%s body=%s %s"), *Selected, *Draft.AuthoringBody->GetName(), *PushNote);
		}
		NumberRepeatedSlots(*Resized);
		OutDraft.Source = Resized;
		OutDraft.OutfitSourceSize = Selected;
		return true;
	}
	const int32 Limit = Contract.SkinnedMaxTriangles(Draft.Slot);
	int32 Kept = 0;
	int32 After = 0;
	USkeletalMesh* Garment = ExportGarment(*Asset, *Single, Draft.KeepSlots, Draft.Slot, Limit, OutDraft.KeepSlots, Kept, After, KeepAlive, OutError);
	if (!Garment) { return false; }
	if (OutNote)
	{
		*OutNote = After == Kept ? AutoSizeNote
			: FString::Printf(TEXT("%s%sReduced from %d to %d triangles to fit the %d limit."), *AutoSizeNote, AutoSizeNote.IsEmpty() ? TEXT("") : TEXT(" "), Kept, After, Limit);
	}
	NumberRepeatedSlots(*Garment);
	USkeletalMesh* Body = nullptr;
	USkeletalMesh* Face = nullptr;
	for (const FSoftObjectPath& Path : Size->BodyParts)
	{
		USkeletalMesh* Part = Cast<USkeletalMesh>(Path.TryLoad());
		if (!Part) { OutError = FString::Printf(TEXT("outfit source body dependency is missing: %s"), *Path.ToString()); return false; }
		KeepAlive.Emplace(Part);
		USkeletalMesh* CandidateBody = nullptr;
		USkeletalMesh* CandidateFace = nullptr;
		if (MakeSourceParts(*Part, Contract, CandidateBody, CandidateFace, KeepAlive, OutError))
		{
			if (Body) { OutError = TEXT("the Outfit size contains multiple body sources; separate its pieces before uploading"); return false; }
			Body = CandidateBody;
			Face = CandidateFace;
		}
		else
		{
			// Refuse unknown body-part representations instead of silently dropping a required source/head.
			OutError = FString::Printf(TEXT("unsupported Outfit source body part %s: %s"), *Path.ToString(), *OutError);
			return false;
		}
	}
	if (!Body) { OutError = TEXT("the chosen Outfit size has no associated complete canonical MetaHuman source body"); return false; }
	OutDraft.Source = Garment;
	OutDraft.AuthoringBody = Body;
	OutDraft.AuthoringFace = Face;
	OutDraft.OutfitSourceSize = Selected;
	return true;
}

bool FMantleParametricOutfit::PrepareRigid(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
	FMantleCosmeticDraft& OutDraft, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& OutError, FString* OutNote)
{
	using namespace MantleParametricOutfitPrivate;
	OutDraft = Draft;
	TArray<FString> Notes;
	TArray<FMantleOutfitSourceSize> Sizes;
	if (!ListSizes(Draft.Source.Get(), Sizes, OutError)) { return false; }
	if (Sizes.IsEmpty()) { OutError = TEXT("the Outfit has no authored sizes"); return false; }
	// A rigid piece is carried by each wearer's socket (fitted to the head on the head bone), so any authored size will do: the chosen one, else the first.
	const FString Selected = Draft.OutfitSourceSize.IsEmpty() ? Sizes[0].Name : Draft.OutfitSourceSize;
	const FMantleOutfitSourceSize* Size = Sizes.FindByPredicate([&Selected](const FMantleOutfitSourceSize& Candidate) { return Candidate.Name == Selected; });
	if (!Size) { OutError = FString::Printf(TEXT("the Outfit has no size %s"), *Selected); return false; }
	UChaosOutfitAsset* Asset = CastChecked<UChaosOutfitAsset>(Draft.Source.Get());
	KeepAlive.Emplace(Asset);
	UChaosOutfitAsset* Single = IsolateSize(Asset, Sizes.Num() > 1 ? Selected : FString(), KeepAlive, OutError);
	if (!Single) { return false; }
	TOptional<TArray<int32>> KeepSlots;
	int32 Kept = 0;
	int32 After = 0;
	USkeletalMesh* Garment = ExportGarment(*Asset, *Single, Draft.KeepSlots, Draft.Slot, Contract.RigidAccessory.MaxTriangles, KeepSlots, Kept, After, KeepAlive, OutError);
	if (!Garment) { return false; }
	if (After != Kept) { Notes.Add(FString::Printf(TEXT("Reduced from %d to %d triangles to fit the %d limit."), Kept, After, Contract.RigidAccessory.MaxTriangles)); }

	FMantleGlbMesh Glb;
	TArray<UMaterialInterface*> GlbMaterials;
	int32 Triangles = 0;
	if (!FMantleCosmeticExport::ReadSkeletalMesh(Garment, 0, Glb, GlbMaterials, Triangles, OutError)) { return false; }
	FRigidPivot Pivot;
	if (!RigidPivot(Contract, Draft.Slot, Pivot, OutError)) { return false; }
	const int32 Bone = Glb.Joints.IndexOfByPredicate([&Pivot](const FMantleGlbJoint& Joint) { return Joint.Name.ToString().Equals(Pivot.Bone, ESearchCase::IgnoreCase); });
	if (Bone == INDEX_NONE) { OutError = FString::Printf(TEXT("the Outfit's skeleton has no %s bone to hang it from (%s's socket bone)"), *Pivot.Bone, *Draft.Slot); return false; }
	TArray<FTransform> Component;
	Component.SetNum(Glb.Joints.Num());
	for (int32 Index = 0; Index < Glb.Joints.Num(); ++Index)
	{
		const int32 Parent = Glb.Joints[Index].Parent;
		Component[Index] = Parent == INDEX_NONE ? Glb.Joints[Index].Local : Glb.Joints[Index].Local * Component[Parent];
	}

	// The socket bone's frame the piece is placed in: the authoring body's own bind pose of it (an Outfit's exported skeleton
	// can bind its head elsewhere, 6.8 cm off on one Fab cap), else the exported skeleton's.
	FTransform Pose = Component[Bone];
	FMantleHeadFit::FFit Fit;
	bool bMeasured = false;
	if (Pivot.bHeadFit)
	{
		// Hats are stored fitted to the reference head: the head it was authored on, measured, maps onto it.
		FBox3f Reference;
		if (!FMantleHeadFit::ReferenceCranium(Reference)) { OutError = TEXT("the plugin's head reference (Resources/AvatarConvert/head-reference.json) is missing"); return false; }
		for (const FSoftObjectPath& Path : Size->BodyParts)
		{
			USkeletalMesh* Part = Cast<USkeletalMesh>(Path.TryLoad());
			FBox3f Authoring;
			int32 Vertices = 0;
			FString Why;
			if (Part && FMantleHeadFit::MeasureCranium(Part, nullptr, FTransform::Identity, Authoring, Vertices, Why))
			{
				KeepAlive.Emplace(Part);
				Fit = FMantleHeadFit::FitTo(Reference, Authoring);
				FMantleHeadFit::HeadPose(Part, Pose);
				bMeasured = true;
				Notes.Add(FString::Printf(TEXT("Fitted from the head it was made on (%s, %s) to the reference head (%s)."), *Part->GetName(),
					*FMantleHeadFit::Describe(Authoring), *FMantleHeadFit::Describe(Reference)));
				break;
			}
		}
		if (!bMeasured) { Notes.Add(TEXT("The head it was made on could not be measured, so it is taken as the reference head's size.")); }
	}
	else
	{
		// Away from the head there is no per-avatar fit: each avatar's own bone carries the piece, at the size it was made.
		bool bPosed = false;
		for (const FSoftObjectPath& Path : Size->BodyParts)
		{
			USkeletalMesh* Part = Cast<USkeletalMesh>(Path.TryLoad());
			const int32 PartBone = Part ? Part->GetRefSkeleton().FindRawBoneIndex(FName(*Pivot.Bone)) : INDEX_NONE;
			if (PartBone == INDEX_NONE) { continue; }
			KeepAlive.Emplace(Part);
			Pose = FTransform::Identity;
			for (int32 Index = PartBone; Index != INDEX_NONE; Index = Part->GetRefSkeleton().GetRawParentIndex(Index))
			{
				Pose = Pose * Part->GetRefSkeleton().GetRawRefBonePose()[Index];
			}
			bPosed = true;
			Notes.Add(FString::Printf(TEXT("Hung from %s's %s bone (the %s socket), at the size it was made."), *Part->GetName(), *Pivot.Bone, *Draft.Slot));
			break;
		}
		if (!bPosed) { Notes.Add(FString::Printf(TEXT("Hung from the Outfit skeleton's %s bone (the %s socket), at the size it was made."), *Pivot.Bone, *Draft.Slot)); }
	}

	// The kept sections, by material, in the socket bone's frame (Mantle's socket sits there at identity), then onto the reference head.
	TSet<UMaterialInterface*> KeptMaterials;
	const TArray<FSkeletalMaterial>& GarmentMaterials = Garment->GetMaterials();
	for (int32 Slot = 0; Slot < GarmentMaterials.Num(); ++Slot)
	{
		if (!KeepSlots.IsSet() || KeepSlots.GetValue().Contains(Slot)) { KeptMaterials.Add(GarmentMaterials[Slot].MaterialInterface.Get()); }
	}
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	UStaticMesh* Rigid = NewObject<UStaticMesh>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(),
		FName(*FString::Printf(TEXT("%s_Rigid"), *Asset->GetName()))), RF_Transient);
	KeepAlive.Emplace(Rigid);
	for (const FMantleGlbPrimitive& Primitive : Glb.Primitives)
	{
		UMaterialInterface* Material = GlbMaterials.IsValidIndex(Primitive.MaterialIndex) ? GlbMaterials[Primitive.MaterialIndex] : nullptr;
		if (!KeptMaterials.Contains(Material) || Primitive.Indices.Num() < 3) { continue; }
		const FName SlotName(*FString::Printf(TEXT("%s_%d"), Material ? *Material->GetName() : TEXT("Material"), Rigid->GetStaticMaterials().Num()));
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		SlotNames[Group] = SlotName;
		Rigid->GetStaticMaterials().Add(FStaticMaterial(Material, SlotName, SlotName));
		TArray<FVertexInstanceID> Instances;
		Instances.SetNum(Primitive.NumVertices);
		for (int32 Local = 0; Local < Primitive.NumVertices; ++Local)
		{
			const int32 Vertex = Primitive.FirstVertex + Local;
			const FVertexID Id = Description.CreateVertex();
			Positions[Id] = RigidPosition(Glb.Positions[Vertex], Pose, Fit);
			Instances[Local] = Description.CreateVertexInstance(Id);
			Normals[Instances[Local]] = RigidNormal(Glb.Normals[Vertex], Pose, Fit);
			UVs.Set(Instances[Local], 0, Glb.UVs[Vertex]);
		}
		for (int32 Index = 0; Index + 2 < Primitive.Indices.Num(); Index += 3)
		{
			Description.CreateTriangle(Group, { Instances[Primitive.Indices[Index]], Instances[Primitive.Indices[Index + 1]], Instances[Primitive.Indices[Index + 2]] });
		}
	}
	if (Rigid->GetStaticMaterials().IsEmpty()) { OutError = TEXT("the selected Outfit sections have no triangles"); return false; }
	Rigid->bAllowCPUAccess = true;
	Rigid->SetNumSourceModels(1);
	FMeshBuildSettings& Settings = Rigid->GetSourceModel(0).BuildSettings;
	Settings.bRecomputeNormals = false;
	Settings.bRecomputeTangents = true;
	Settings.bGenerateLightmapUVs = false;
	Settings.bBuildReversedIndexBuffer = false;
	Settings.bUseFullPrecisionUVs = true;
	Settings.DistanceFieldResolutionScale = 0.0f;
	Settings.MaxLumenMeshCards = 0;
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bAllowCpuAccess = true;
	if (!Rigid->BuildFromMeshDescriptions({ &Description }, Params)) { OutError = TEXT("the engine could not build the rigid mesh"); return false; }
	UE_LOG(LogMantleParametricOutfit, Display, TEXT("MANTLE_OUTFIT_RIGID asset=%s size=%s bone=%s triangles=%d measured=%d scale=%s offset=%s bounds=%s"),
		*Asset->GetName(), *Selected, *Pivot.Bone, After, bMeasured ? 1 : 0, *Fit.Scale.ToString(), *Fit.Offset.ToString(), *Rigid->GetBoundingBox().ToString());
	OutDraft.Source = Rigid;
	OutDraft.AccessoryWorn = Pose.GetRotation();
	OutDraft.bAccessoryBackFaces = true;
	OutDraft.KeepSlots.Reset();
	OutDraft.OutfitSourceSize = Selected;
	if (OutNote) { *OutNote = FString::Join(Notes, TEXT(" ")); }
	return true;
}

bool FMantleParametricOutfit::RigidPivot(const FMantleAvatarContract& Contract, const FString& Slot, FRigidPivot& OutPivot, FString& OutError)
{
	const FMantleContractSlot* Entry = Contract.FindSlot(Slot);
	if (!Entry || Entry->ItemClass != TEXT("rigid_accessory") || Entry->SocketBone.IsEmpty())
	{
		OutError = FString::Printf(TEXT("\"%s\" is not an accessory slot with a socket bone"), *Slot);
		return false;
	}
	OutPivot.Bone = Entry->SocketBone;
	// Only the hat socket has a per-avatar fit (FMantleHeadFit::HatSocket moves and scales it onto each cranium at upload).
	OutPivot.bHeadFit = Entry->Socket == TEXT("acc_hat");
	return true;
}

FVector3f FMantleParametricOutfit::RigidPosition(const FVector3f& Position, const FTransform& Pose, const FMantleHeadFit::FFit& Fit)
{
	return (FVector3f(Pose.InverseTransformPositionNoScale(FVector(Position))) - Fit.Offset) / Fit.Scale;
}

FVector3f FMantleParametricOutfit::RigidNormal(const FVector3f& Normal, const FTransform& Pose, const FMantleHeadFit::FFit& Fit)
{
	// The inverse map's normals scale by the fit, not its inverse.
	return (FVector3f(Pose.InverseTransformVectorNoScale(FVector(Normal))) * Fit.Scale).GetSafeNormal();
}
