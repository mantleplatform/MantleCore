// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarAtlasFixer.h"

#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticValidation.h"
#include "MantlePlaceMaterial.h"
#include "MantleRuntime.h"

#include "Async/ParallelFor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "IMaterialBakingModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "LODUtilities.h"
#include "MaterialBakingStructures.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "RHIGlobals.h"
#include "RHIShaderPlatform.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "SkinnedAssetCompiler.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarAtlasFixerPrivate
{
	/** The smallest cell (inside its gutter) and the smallest atlas tried. */
	constexpr int32 MinCellPixels = 4;
	constexpr int32 MinAtlasSize = 64;

	/** Source texels per UV unit of a material with no texture (a flat colour needs few), and the floor for any. */
	constexpr int32 MinTexelsPerUV = 32;

	/** Pixels of dilation around every cell's gutter, into the atlas's unused space, so mips never pull in a default. */
	constexpr int32 DilatePasses = 4;

	/**
	 * The slots the game hides on an avatar (client MantleAvatarRoleMaterials' HiddenParts, which MantleCore cannot
	 * include): merged into an atlas they would show.
	 */
	const TCHAR* const HiddenParts[] = { TEXT("eyeshell"), TEXT("eyeedge"), TEXT("eyelash"), TEXT("saliva"), TEXT("cartilage") };

	/** Words that make a masked slot hair (cards, lashes, brows, facial hair, fur). */
	const TCHAR* const HairWords[] = { TEXT("hair"), TEXT("lash"), TEXT("brow"), TEXT("beard"), TEXT("mustache"), TEXT("moustache"),
		TEXT("fur"), TEXT("card"), TEXT("strand"), TEXT("fuzz"), TEXT("groom") };

	bool AtlasNameHasAny(const FString& Name, TConstArrayView<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Name.Contains(Word, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	EMantleAtlasBlend AtlasBlendOf(const UMaterialInterface* Material)
	{
		const EBlendMode Blend = Material ? Material->GetBlendMode() : BLEND_Opaque;
		return Blend == BLEND_Opaque ? EMantleAtlasBlend::Opaque : Blend == BLEND_Masked ? EMantleAtlasBlend::Masked : EMantleAtlasBlend::Translucent;
	}

	/** The section name the upload gives a slot (FMantleCosmeticExport's SlotName): the slot name, else the material's. */
	FString AtlasSlotName(const TArray<FSkeletalMaterial>& Materials, int32 Slot)
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

	/**
	 * The textures a material samples: GetUsedTextures's, read from its compiled shaders. A process without a renderer
	 * (-nullrhi) has none, and GetUsedTextures gives nothing there, so then the textures its parameters and its base
	 * material's graph name stand in.
	 */
	void AtlasMaterialTextures(const UMaterialInterface* Material, TArray<UTexture*>& Out)
	{
		Out.Reset();
		if (!Material)
		{
			return;
		}
		Material->GetUsedTextures(Out);
		Out.Remove(nullptr);
		if (Out.Num() > 0)
		{
			return;
		}
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Material->GetAllTextureParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			UTexture* Texture = nullptr;
			if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Info), Texture) && Texture)
			{
				Out.AddUnique(Texture);
			}
		}
		if (const UMaterial* Base = Material->GetMaterial())
		{
			for (const TObjectPtr<UMaterialExpression>& Expression : Base->GetExpressions())
			{
				const UMaterialExpressionTextureBase* Sample = Cast<UMaterialExpressionTextureBase>(Expression.Get());
				if (Sample && Sample->Texture)
				{
					Out.AddUnique(Sample->Texture.Get());
				}
			}
		}
	}

	/** The largest source texture a material samples (AtlasMaterialTextures), as texels per UV unit, within [MinTexelsPerUV, MaxSize]. */
	int32 AtlasTexelsPerUV(const UMaterialInterface* Material, int32 MaxSize)
	{
		int32 Best = 0;
		if (Material)
		{
			TArray<UTexture*> Used;
			AtlasMaterialTextures(Material, Used);
			for (const UTexture* Texture : Used)
			{
				if (!Texture)
				{
					continue;
				}
				if (Texture->Source.IsValid())
				{
					for (int32 Block = 0; Block < Texture->Source.GetNumBlocks(); ++Block)
					{
						FTextureSourceBlock Info;
						Texture->Source.GetBlock(Block, Info);
						Best = FMath::Max3(Best, Info.SizeX, Info.SizeY);
					}
				}
				else
				{
					Best = FMath::Max(Best, FMath::RoundToInt32(FMath::Max(Texture->GetSurfaceWidth(), Texture->GetSurfaceHeight())));
				}
			}
		}
		return FMath::Clamp(Best, MinTexelsPerUV, FMath::Max(MinTexelsPerUV, MaxSize));
	}

	/**
	 * Whether Material has what the editor's material baking dereferences: a render proxy, and a material resource
	 * for this platform. A material never compiled for rendering (built in memory and never PostEditChange'd) has none,
	 * and IMaterialBakingModule crashes on it (EXCEPTION_ACCESS_VIOLATION reading 0x0); it is sampled instead. OutWhy:
	 * what it lacks.
	 */
	bool AtlasHasRenderResource(const UMaterialInterface* Material, FString* OutWhy = nullptr)
	{
		if (!Material || !Material->GetRenderProxy())
		{
			if (OutWhy)
			{
				*OutWhy = TEXT("it has no render proxy");
			}
			return false;
		}
		if (!Material->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			if (OutWhy)
			{
				*OutWhy = TEXT("it was never compiled for rendering (no material resource)");
			}
			return false;
		}
		return true;
	}

	/**
	 * Whether the rendered bake takes Material: any material with a render resource (AtlasHasRenderResource), when this
	 * process renders and the settings allow it. A UDIM or virtual-textured material is rendered too. The bake
	 * renders over the cell's own UV rectangle, a UDIM cell's tile included, as the material draws it.
	 */
	bool AtlasCanRender(const UMaterialInterface* Material, const FMantleAtlasSettings& Settings)
	{
		return Material && Settings.bRenderedBake && !GUsingNullRHI && FApp::CanEverRender() && AtlasHasRenderResource(Material);
	}

	/**
	 * Before a material renders into a bake: its textures' compiles finished (a texture still compiling draws as the
	 * engine's placeholder, which would be baked).
	 */
	void AtlasFinishTextures(const UMaterialInterface* Material)
	{
		TArray<UTexture*> Textures;
		AtlasMaterialTextures(Material, Textures);
		if (Textures.Num() > 0)
		{
			FTextureCompilingManager::Get().FinishCompilation(Textures);
		}
	}

	const FMantleContractClass* AtlasClassOf(const FMantleAvatarContract& Contract, const FString& Slot, bool* bOutAvatar = nullptr)
	{
		const FMantleContractSlot* Found = Contract.FindSlot(Slot);
		if (bOutAvatar)
		{
			*bOutAvatar = Found && Found->ItemClass.Equals(TEXT("avatar_body"), ESearchCase::CaseSensitive);
		}
		if (!Found)
		{
			return nullptr;
		}
		if (Found->ItemClass.Equals(TEXT("avatar_body"), ESearchCase::CaseSensitive))
		{
			return &Contract.AvatarBody;
		}
		if (Found->ItemClass.Equals(TEXT("skinned_garment"), ESearchCase::CaseSensitive))
		{
			return &Contract.SkinnedGarment;
		}
		return nullptr;
	}

	int32 AtlasMaxTextureSize(const FMantleConvertContext& Context)
	{
		int32 Max = Context.MaxTextureSize > 0 ? Context.MaxTextureSize : 2048;
		if (const FMantleAvatarContract* Contract = FMantleAvatarContract::Get())
		{
			Max = FMath::Min(Max, Contract->MaxTextureSize);
		}
		return FMath::Max(Max, MinCellPixels * 4);
	}

	/** One slot's facts the planner needs, from the mesh's material list. */
	void FillAtlasSlot(FMantleAtlasSlot& Out, const TArray<FSkeletalMaterial>& Materials, int32 Slot, const TMap<FString, FString>& RoleByName, bool bAvatar)
	{
		const UMaterialInterface* Material = Materials.IsValidIndex(Slot) ? Materials[Slot].MaterialInterface.Get() : nullptr;
		Out.Slot = Slot;
		Out.Name = AtlasSlotName(Materials, Slot);
		Out.Blend = AtlasBlendOf(Material);
		Out.bTwoSided = Material && Material->IsTwoSided();
		const FString* Role = RoleByName.Find(Out.Name);
		Out.Role = bAvatar ? (Role ? *Role : FString(TEXT("other"))) : FString();
		const FString MaterialName = Material ? Material->GetName() : FString();
		Out.bHair = Out.Blend == EMantleAtlasBlend::Masked && (AtlasNameHasAny(Out.Name, HairWords) || AtlasNameHasAny(MaterialName, HairWords));
		Out.bHidden = AtlasNameHasAny(Out.Name.ToLower(), HiddenParts);
	}

	/** The role the upload gives each section name of LOD 0 (avatars): FMantleCosmeticExport::DefaultSectionRoles, by name. */
	TMap<FString, FString> AtlasRolesByName(const USkeletalMesh* Mesh)
	{
		TMap<FString, FString> Out;
		const TArray<FString> Names = FMantleCosmeticExport::SectionMaterialNames(Mesh, 0);
		const TArray<FString> Roles = FMantleCosmeticExport::DefaultSectionRoles(Mesh, 0);
		for (int32 Index = 0; Index < Names.Num() && Index < Roles.Num(); ++Index)
		{
			Out.Add(Names[Index], Roles[Index]);
		}
		return Out;
	}

	/** What a part looks like to the planner: LOD 0's slots and faces. */
	struct FAtlasSurvey
	{
		TArray<FMantleAtlasSlot> Slots;
		TArray<FMantleAtlasFace> Faces;
		/** Source texels per UV unit, by slot. */
		TMap<int32, int32> TexelsBySlot;
	};

	/** Tiles and texel need of every surveyed slot, from its faces' triangles. */
	void AtlasFinishSurvey(FAtlasSurvey& Survey, const TMap<int32, TPair<TArray<FVector2f>, TArray<uint32>>>& TrianglesBySlot)
	{
		for (FMantleAtlasSlot& Slot : Survey.Slots)
		{
			if (const TPair<TArray<FVector2f>, TArray<uint32>>* Triangles = TrianglesBySlot.Find(Slot.Slot))
			{
				Slot.Tiles = FMantleCosmeticChecks::MeasureUVTiles(Triangles->Key, Triangles->Value);
			}
			const double Texels = Survey.TexelsBySlot.FindRef(Slot.Slot);
			const FVector2f Extent = Slot.Tiles.Range.bIsValid ? Slot.Tiles.Range.GetSize() : FVector2f::ZeroVector;
			Slot.TexelNeed = Texels * Texels * FMath::Max(1e-6, static_cast<double>(Extent.X) * static_cast<double>(Extent.Y));
		}
	}

	/**
	 * The part's LOD 0 as the upload reads it (its render data, FMantleCosmeticExport::ReadSkeletalMesh): what Analyze
	 * describes. OnlySlot (INDEX_NONE for all) restricts it to a lifted section's slot.
	 */
	bool AtlasSurveyRendered(const USkeletalMesh* Mesh, int32 OnlySlot, bool bAvatar, int32 MaxSize, FAtlasSurvey& Out, FString& OutError)
	{
		FMantleGlbMesh Glb;
		TArray<int32> Slots;
		int32 Triangles = 0, Capped = 0;
		float Largest = 0.0f;
		if (!FMantleCosmeticExport::ReadSkeletalMesh(Mesh, 0, Glb, Slots, Triangles, Capped, Largest, OutError))
		{
			return false;
		}
		const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
		const TMap<FString, FString> Roles = AtlasRolesByName(Mesh);
		TMap<int32, TPair<TArray<FVector2f>, TArray<uint32>>> TrianglesBySlot;
		for (int32 Material = 0; Material < Glb.MaterialNames.Num() && Material < Slots.Num(); ++Material)
		{
			const int32 Slot = Slots[Material];
			if (OnlySlot != INDEX_NONE && Slot != OnlySlot)
			{
				continue;
			}
			FMantleAtlasSlot& Info = Out.Slots.AddDefaulted_GetRef();
			FillAtlasSlot(Info, Materials, Slot, Roles, bAvatar);
			Out.TexelsBySlot.Add(Slot, AtlasTexelsPerUV(Materials.IsValidIndex(Slot) ? Materials[Slot].MaterialInterface.Get() : nullptr, MaxSize));
			TPair<TArray<FVector2f>, TArray<uint32>>& Tris = TrianglesBySlot.FindOrAdd(Slot);
			for (const FMantleGlbPrimitive& Primitive : Glb.Primitives)
			{
				if (Primitive.MaterialIndex != Material || Primitive.FirstVertex < 0 || Primitive.FirstVertex + Primitive.NumVertices > Glb.UVs.Num())
				{
					continue;
				}
				const uint32 Base = static_cast<uint32>(Tris.Key.Num());
				Tris.Key.Append(Glb.UVs.GetData() + Primitive.FirstVertex, Primitive.NumVertices);
				for (int32 Index = 0; Index + 2 < Primitive.Indices.Num(); Index += 3)
				{
					FMantleAtlasFace& Face = Out.Faces.AddDefaulted_GetRef();
					Face.Slot = Slot;
					for (int32 Corner = 0; Corner < 3; ++Corner)
					{
						const uint32 Local = Primitive.Indices[Index + Corner];
						Tris.Value.Add(Base + Local);
						const int32 Vertex = Primitive.FirstVertex + static_cast<int32>(Local);
						Face.UVs.Add(Glb.UVs.IsValidIndex(Vertex) ? Glb.UVs[Vertex] : FVector2f::ZeroVector);
					}
				}
			}
		}
		AtlasFinishSurvey(Out, TrianglesBySlot);
		return true;
	}

	/** The mesh's material slot a polygon group draws with: by imported slot name, then slot name, then its index. */
	int32 AtlasSlotOfGroup(FName Name, int32 GroupIndex, const TArray<FSkeletalMaterial>& Materials)
	{
		if (!Name.IsNone())
		{
			for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
			{
				if (Materials[Slot].ImportedMaterialSlotName == Name)
				{
					return Slot;
				}
			}
			for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
			{
				if (Materials[Slot].MaterialSlotName == Name)
				{
					return Slot;
				}
			}
		}
		return Materials.IsValidIndex(GroupIndex) ? GroupIndex : INDEX_NONE;
	}

	/** Polygon group to material slot for a mesh description. */
	TMap<FPolygonGroupID, int32> AtlasGroupSlots(const FMeshDescription& Description, const TArray<FSkeletalMaterial>& Materials)
	{
		TMap<FPolygonGroupID, int32> Out;
		FSkeletalMeshConstAttributes Attributes(Description);
		const TPolygonGroupAttributesConstRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			Out.Add(Group, AtlasSlotOfGroup(SlotNames.IsValid() ? SlotNames.Get(Group) : NAME_None, Group.GetValue(), Materials));
		}
		return Out;
	}

	/** A polygon's UV0 corners (UVs: a const or mutable vertex instance UV attribute). */
	template <typename UVRefType>
	void AtlasPolygonUVs(const FMeshDescription& Description, const UVRefType& UVs, FPolygonID Polygon, TArray<FVector2f, TInlineAllocator<4>>& OutUVs)
	{
		OutUVs.Reset();
		for (const FVertexInstanceID Instance : Description.GetPolygonVertexInstances(Polygon))
		{
			OutUVs.Add(UVs.Get(Instance, 0));
		}
	}

	FVector2f AtlasCentroid(TConstArrayView<FVector2f> UVs)
	{
		FVector2f Sum = FVector2f::ZeroVector;
		for (const FVector2f& UV : UVs)
		{
			Sum += UV;
		}
		return UVs.Num() > 0 ? Sum / static_cast<float>(UVs.Num()) : Sum;
	}

	/** LOD 0's source geometry (its mesh description, what Apply rewrites): one face per polygon. */
	bool AtlasSurveyDescription(const USkeletalMesh* Mesh, const FMeshDescription& Description, int32 OnlySlot, bool bAvatar, int32 MaxSize,
		FAtlasSurvey& Out, FString& OutError)
	{
		FSkeletalMeshConstAttributes Attributes(Description);
		const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		if (!UVs.IsValid() || UVs.GetNumChannels() == 0)
		{
			OutError = FString::Printf(TEXT("%s has no UVs to lay an atlas out on"), *Mesh->GetName());
			return false;
		}
		const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
		const TMap<FPolygonGroupID, int32> GroupSlots = AtlasGroupSlots(Description, Materials);
		const TMap<FString, FString> Roles = AtlasRolesByName(Mesh);
		TMap<int32, TPair<TArray<FVector2f>, TArray<uint32>>> TrianglesBySlot;
		TArray<int32> Order;
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			const int32 Slot = GroupSlots.FindRef(Group);
			if (Slot == INDEX_NONE || (OnlySlot != INDEX_NONE && Slot != OnlySlot) || Description.GetPolygonGroupPolygonIDs(Group).Num() == 0)
			{
				continue;
			}
			Order.AddUnique(Slot);
			for (const FPolygonID Polygon : Description.GetPolygonGroupPolygonIDs(Group))
			{
				FMantleAtlasFace& Face = Out.Faces.AddDefaulted_GetRef();
				Face.Slot = Slot;
				AtlasPolygonUVs(Description, UVs, Polygon, Face.UVs);
				TPair<TArray<FVector2f>, TArray<uint32>>& Tris = TrianglesBySlot.FindOrAdd(Slot);
				for (const FTriangleID Triangle : Description.GetPolygonTriangles(Polygon))
				{
					for (const FVertexInstanceID Instance : Description.GetTriangleVertexInstances(Triangle))
					{
						Tris.Value.Add(static_cast<uint32>(Tris.Key.Num()));
						Tris.Key.Add(UVs.Get(Instance, 0));
					}
				}
			}
		}
		for (const int32 Slot : Order)
		{
			FMantleAtlasSlot& Info = Out.Slots.AddDefaulted_GetRef();
			FillAtlasSlot(Info, Materials, Slot, Roles, bAvatar);
			Out.TexelsBySlot.Add(Slot, AtlasTexelsPerUV(Materials.IsValidIndex(Slot) ? Materials[Slot].MaterialInterface.Get() : nullptr, MaxSize));
		}
		AtlasFinishSurvey(Out, TrianglesBySlot);
		return true;
	}

	/** Every group's cells packed; false with the reason when one cannot fit. */
	bool AtlasLayouts(const FAtlasSurvey& Survey, TConstArrayView<FMantleAtlasGroup> Groups, int32 MaxSize, int32 Padding,
		TArray<FMantleAtlasLayout>& OutLayouts, TArray<TArray<int32>>& OutFaceCells, FString& OutError)
	{
		OutLayouts.Reset();
		OutFaceCells.Reset();
		for (const FMantleAtlasGroup& Group : Groups)
		{
			FMantleAtlasLayout& Layout = OutLayouts.AddDefaulted_GetRef();
			Layout.Padding = FMath::Clamp(Padding, 1, 8);
			TArray<int32>& FaceCells = OutFaceCells.AddDefaulted_GetRef();
			FMantleAvatarAtlasFixer::BuildCells(Survey.Faces, Group,
				[&Survey](int32 Slot)
				{
					const FMantleAtlasSlot* Found = Survey.Slots.FindByPredicate([Slot](const FMantleAtlasSlot& Info) { return Info.Slot == Slot; });
					return Found && Found->Tiles.IsUdim();
				},
				[&Survey](int32 Slot) { return Survey.TexelsBySlot.FindRef(Slot); },
				Layout, FaceCells);
			if (Layout.Cells.Num() == 0)
			{
				OutError = TEXT("an atlas has no faces to bake");
				return false;
			}
			if (!FMantleAvatarAtlasFixer::PackLayout(Layout, MaxSize))
			{
				OutError = FString::Printf(TEXT("%d atlas cells do not fit a %d x %d texture"), Layout.Cells.Num(), MaxSize, MaxSize);
				return false;
			}
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Baking

	/** One cell's baked texels (its gutter included), row-major. */
	struct FAtlasCellPixels
	{
		FIntPoint Size = FIntPoint::ZeroValue;
		TArray<FColor> BaseColor;
		TArray<FColor> Normal;
		TArray<FColor> Orm;
	};

	/** A source texture's pixels for sampling: one block, or a UDIM texture's blocks by tile. */
	struct FAtlasTextureSampler
	{
		struct FBlock
		{
			FIntPoint Tile = FIntPoint::ZeroValue;
			int32 Width = 0;
			int32 Height = 0;
			TArray<FColor> Pixels;
		};
		TArray<FBlock> Blocks;
		bool bUdim = false;
		bool bClampU = false;
		bool bClampV = false;

		/** Source pixels as BGRA8 in the texture's own encoding (sRGB colour stays sRGB, data stays linear). */
		bool Load(const UTexture* Texture, bool bLinear, int32 MaxSize)
		{
			Blocks.Reset();
			if (!Texture || !Texture->Source.IsValid() || Texture->Source.GetNumSlices() > 1)
			{
				return false;
			}
			FTextureSource& Source = const_cast<UTexture*>(Texture)->Source;
			const int32 NumBlocks = Source.GetNumBlocks();
			bUdim = NumBlocks > 1;
			for (int32 Block = 0; Block < NumBlocks; ++Block)
			{
				FTextureSourceBlock Info;
				Source.GetBlock(Block, Info);
				FImage Image;
				if (!Source.GetMipImage(Image, Block, 0, 0))
				{
					continue;
				}
				// No change of encoding for 8-bit data; float sources are encoded for their use.
				const EGammaSpace Gamma = Image.GammaSpace != EGammaSpace::Linear ? Image.GammaSpace
					: (Image.Format == ERawImageFormat::BGRA8 || bLinear ? EGammaSpace::Linear : EGammaSpace::sRGB);
				FImage Bgra;
				Image.CopyTo(Bgra, ERawImageFormat::BGRA8, Gamma);
				FBlock& Out = Blocks.AddDefaulted_GetRef();
				Out.Tile = FIntPoint(Info.BlockX, Info.BlockY);
				Out.Width = Bgra.SizeX;
				Out.Height = Bgra.SizeY;
				const TArrayView64<FColor> View = Bgra.AsBGRA8();
				Out.Pixels = TArray<FColor>(View.GetData(), static_cast<int32>(View.Num()));
				if (MaxSize > 0 && FMath::Max(Out.Width, Out.Height) > MaxSize)
				{
					const float Scale = static_cast<float>(MaxSize) / FMath::Max(Out.Width, Out.Height);
					const int32 NewWidth = FMath::Max(1, FMath::RoundToInt32(Out.Width * Scale));
					const int32 NewHeight = FMath::Max(1, FMath::RoundToInt32(Out.Height * Scale));
					TArray<FColor> Resized;
					Resized.SetNumUninitialized(NewWidth * NewHeight);
					FImageUtils::ImageResize(Out.Width, Out.Height, TArrayView<const FColor>(Out.Pixels), NewWidth, NewHeight, TArrayView<FColor>(Resized),
						!bLinear, false);
					Out.Pixels = MoveTemp(Resized);
					Out.Width = NewWidth;
					Out.Height = NewHeight;
				}
				if (Out.Pixels.Num() != Out.Width * Out.Height || Out.Pixels.Num() == 0)
				{
					Blocks.Pop();
				}
			}
			if (const UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
			{
				bClampU = Texture2D->AddressX == TA_Clamp;
				bClampV = Texture2D->AddressY == TA_Clamp;
			}
			return Blocks.Num() > 0;
		}

		int32 LargestSide() const
		{
			int32 Out = 1;
			for (const FBlock& Block : Blocks)
			{
				Out = FMath::Max3(Out, Block.Width, Block.Height);
			}
			return Out;
		}

		/** Bilinear, channels 0..255 as (R, G, B, A). ForcedTile (a UDIM cell's) keeps a sample on its own tile. */
		FVector4f Sample(const FVector2f& UV, const FIntPoint* ForcedTile) const
		{
			const FBlock* Block = &Blocks[0];
			FVector2f Local = UV;
			bool bClampX = bClampU;
			bool bClampY = bClampV;
			if (bUdim)
			{
				const FIntPoint Tile = ForcedTile ? *ForcedTile : FIntPoint(FMath::FloorToInt32(UV.X), FMath::FloorToInt32(UV.Y));
				for (const FBlock& Candidate : Blocks)
				{
					if (Candidate.Tile == Tile)
					{
						Block = &Candidate;
						break;
					}
				}
				Local = FVector2f(FMath::Clamp(UV.X - Tile.X, 0.0f, 1.0f), FMath::Clamp(UV.Y - Tile.Y, 0.0f, 1.0f));
				bClampX = bClampY = true;
			}
			const float X = Local.X * Block->Width - 0.5f;
			const float Y = Local.Y * Block->Height - 0.5f;
			const int32 X0 = FMath::FloorToInt32(X);
			const int32 Y0 = FMath::FloorToInt32(Y);
			const float FX = X - X0;
			const float FY = Y - Y0;
			auto Wrap = [](int32 Index, int32 Size, bool bClamp)
			{
				return bClamp ? FMath::Clamp(Index, 0, Size - 1) : ((Index % Size) + Size) % Size;
			};
			const int32 XA = Wrap(X0, Block->Width, bClampX), XB = Wrap(X0 + 1, Block->Width, bClampX);
			const int32 YA = Wrap(Y0, Block->Height, bClampY), YB = Wrap(Y0 + 1, Block->Height, bClampY);
			auto At = [Block](int32 PX, int32 PY)
			{
				const FColor& C = Block->Pixels[PY * Block->Width + PX];
				return FVector4f(C.R, C.G, C.B, C.A);
			};
			const FVector4f Top = FMath::Lerp(At(XA, YA), At(XB, YA), FX);
			const FVector4f Bottom = FMath::Lerp(At(XA, YB), At(XB, YB), FX);
			return FMath::Lerp(Top, Bottom, FY);
		}
	};

	FColor AtlasToColor(const FVector4f& Value)
	{
		return FColor(static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(Value.X), 0, 255)), static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(Value.Y), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(Value.Z), 0, 255)), static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(Value.W), 0, 255)));
	}

	/** How an ORM-like texture packs its channels. SRMF (MetaHuman skin): specular, roughness, metallic, fuzz. */
	enum class EAtlasOrmLayout : uint8 { Orm, Rma, Srmf, RoughnessOnly };

	/**
	 * A material's textures found the way the upload finds them (FMantleCosmeticExport::ResolveMaterialTextures:
	 * the plain sample's texture, else base colour, normal and ORM by parameter name, never an animated wrinkle map or a
	 * tiled detail map, then the largest colour texture it uses); an opacity texture by
	 * FMantleCosmeticExport::PickOpacityTexture; flat values from its parameters otherwise.
	 */
	struct FAtlasResolved
	{
		UTexture* BaseColor = nullptr;
		UTexture* Normal = nullptr;
		UTexture* Orm = nullptr;
		UTexture* Opacity = nullptr;
		EAtlasOrmLayout OrmLayout = EAtlasOrmLayout::Orm;
		FLinearColor Flat = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);
		float Roughness = 0.5f;
		float Metallic = 0.0f;
	};

	FAtlasResolved AtlasResolve(UMaterialInterface* Material)
	{
		FAtlasResolved Out;
		if (!Material)
		{
			return Out;
		}
		const FMantleMaterialTextures Found = FMantleCosmeticExport::ResolveMaterialTextures(Material, /*bPickOpacity*/ true);
		Out.BaseColor = Found.BaseColor;
		Out.Normal = Found.Normal;
		Out.Orm = Found.Orm;
		Out.Opacity = Found.Opacity;
		const FString& OrmName = Found.OrmParameter;
		Out.OrmLayout = AtlasNameHasAny(OrmName, { TEXT("SRMF") }) ? EAtlasOrmLayout::Srmf
			: AtlasNameHasAny(OrmName, { TEXT("ORM"), TEXT("ARM"), TEXT("OcclusionRoughnessMetal") }) ? EAtlasOrmLayout::Orm
			: AtlasNameHasAny(OrmName, { TEXT("RMA") }) ? EAtlasOrmLayout::Rma : EAtlasOrmLayout::RoughnessOnly;
		for (const TCHAR* Name : { TEXT("BaseColor"), TEXT("Base Color"), TEXT("Color"), TEXT("Tint") })
		{
			FLinearColor Colour;
			if (Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(FName(Name))), Colour))
			{
				Out.Flat = Colour;
				break;
			}
		}
		for (const TCHAR* Name : { TEXT("Roughness"), TEXT("RoughnessFactor") })
		{
			float Value = 0.0f;
			if (Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(FName(Name))), Value))
			{
				Out.Roughness = FMath::Clamp(Value, 0.0f, 1.0f);
				break;
			}
		}
		for (const TCHAR* Name : { TEXT("Metallic"), TEXT("MetallicFactor") })
		{
			float Value = 0.0f;
			if (Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(FName(Name))), Value))
			{
				Out.Metallic = FMath::Clamp(Value, 0.0f, 1.0f);
				break;
			}
		}
		return Out;
	}

	/** The padded source rectangle a cell bakes: its own, grown by the gutter at the cell's scale. */
	FBox2f AtlasPaddedSource(const FMantleAtlasCell& Cell, int32 Padding)
	{
		const FVector2f Extent = Cell.Source.GetSize();
		const FVector2f PerPixel(Extent.X / FMath::Max(1, Cell.Inner.Width()), Extent.Y / FMath::Max(1, Cell.Inner.Height()));
		return FBox2f(Cell.Source.Min - PerPixel * static_cast<float>(Padding), Cell.Source.Max + PerPixel * static_cast<float>(Padding));
	}

	/** A slot's textures loaded for sampling, once per slot. */
	struct FAtlasSlotSamplers
	{
		FAtlasResolved Resolved;
		TOptional<FAtlasTextureSampler> BaseColor;
		TOptional<FAtlasTextureSampler> Normal;
		TOptional<FAtlasTextureSampler> Orm;
		TOptional<FAtlasTextureSampler> Opacity;
		EMantleOpacityChannel OpacityChannel = EMantleOpacityChannel::Red;
	};

	void AtlasLoadSamplers(UMaterialInterface* Material, int32 MaxSize, FAtlasSlotSamplers& Out)
	{
		Out.Resolved = AtlasResolve(Material);
		auto Load = [MaxSize](UTexture* Texture, bool bLinear, TOptional<FAtlasTextureSampler>& Into)
		{
			FAtlasTextureSampler Sampler;
			if (Texture && Sampler.Load(Texture, bLinear, MaxSize))
			{
				Into = MoveTemp(Sampler);
			}
		};
		Load(Out.Resolved.BaseColor, false, Out.BaseColor);
		Load(Out.Resolved.Normal, true, Out.Normal);
		Load(Out.Resolved.Orm, true, Out.Orm);
		Load(Out.Resolved.Opacity, true, Out.Opacity);
		if (Out.Opacity.IsSet())
		{
			Out.OpacityChannel = FMantleCosmeticExport::ChooseOpacityChannel(Out.Opacity->Blocks[0].Pixels);
		}
	}

	/** A cell by sampling its material's source textures directly (no renderer needed). */
	void AtlasSampleCell(const FAtlasSlotSamplers& Samplers, const FMantleAtlasCell& Cell, int32 Padding, bool bAtlasHasAlpha, bool bSourceHasAlpha,
		FAtlasCellPixels& Out)
	{
		const FBox2f Padded = AtlasPaddedSource(Cell, Padding);
		Out.Size = FIntPoint(Cell.Inner.Width() + 2 * Padding, Cell.Inner.Height() + 2 * Padding);
		const int32 Count = Out.Size.X * Out.Size.Y;
		Out.BaseColor.SetNumUninitialized(Count);
		Out.Normal.SetNumUninitialized(Count);
		Out.Orm.SetNumUninitialized(Count);
		const FVector2f PixelUV(Padded.GetSize().X / Out.Size.X, Padded.GetSize().Y / Out.Size.Y);
		// Box-filter a cell that shrinks its source: up to 4 x 4 samples per texel.
		const int32 SourceSide = Samplers.BaseColor.IsSet() ? Samplers.BaseColor->LargestSide() : 1;
		const int32 Taps = FMath::Clamp(FMath::CeilToInt32(FMath::Max(PixelUV.X, PixelUV.Y) * SourceSide), 1, 4);
		const FIntPoint* ForcedTile = Cell.bWholeSlot ? nullptr : &Cell.Tile;
		const FAtlasResolved& Resolved = Samplers.Resolved;
		const FColor Flat = Resolved.Flat.ToFColorSRGB();
		const uint8 Rough = static_cast<uint8>(FMath::RoundToInt32(Resolved.Roughness * 255.0f));
		const uint8 Metal = static_cast<uint8>(FMath::RoundToInt32(Resolved.Metallic * 255.0f));

		ParallelFor(Out.Size.Y, [&](int32 Row)
		{
			for (int32 Column = 0; Column < Out.Size.X; ++Column)
			{
				FVector4f Colour(0.0f, 0.0f, 0.0f, 0.0f), Normal(0.0f, 0.0f, 0.0f, 0.0f), Orm(0.0f, 0.0f, 0.0f, 0.0f);
				float Opacity = 0.0f;
				for (int32 TY = 0; TY < Taps; ++TY)
				{
					for (int32 TX = 0; TX < Taps; ++TX)
					{
						const FVector2f UV(Padded.Min.X + (Column + (TX + 0.5f) / Taps) * PixelUV.X, Padded.Min.Y + (Row + (TY + 0.5f) / Taps) * PixelUV.Y);
						Colour += Samplers.BaseColor.IsSet() ? Samplers.BaseColor->Sample(UV, ForcedTile) : FVector4f(Flat.R, Flat.G, Flat.B, 255.0f);
						Normal += Samplers.Normal.IsSet() ? Samplers.Normal->Sample(UV, ForcedTile) : FVector4f(128.0f, 128.0f, 255.0f, 255.0f);
						if (Samplers.Orm.IsSet())
						{
							const FVector4f Packed = Samplers.Orm->Sample(UV, ForcedTile);
							Orm += Resolved.OrmLayout == EAtlasOrmLayout::Orm ? FVector4f(Packed.X, Packed.Y, Packed.Z, 255.0f)
								: Resolved.OrmLayout == EAtlasOrmLayout::Rma ? FVector4f(Packed.Z, Packed.X, Packed.Y, 255.0f)
								: Resolved.OrmLayout == EAtlasOrmLayout::Srmf ? FVector4f(255.0f, Packed.Y, Packed.Z, 255.0f)
								: FVector4f(255.0f, Packed.X, Metal, 255.0f);
						}
						else
						{
							Orm += FVector4f(255.0f, Rough, Metal, 255.0f);
						}
						if (Samplers.Opacity.IsSet())
						{
							const FVector4f Mask = Samplers.Opacity->Sample(UV, ForcedTile);
							Opacity += Samplers.OpacityChannel == EMantleOpacityChannel::Alpha ? Mask.W : Mask.X;
						}
					}
				}
				const float Weight = 1.0f / (Taps * Taps);
				Colour *= Weight;
				// Opacity rides in the base colour's alpha; an opaque source (or an opaque atlas) is fully opaque.
				Colour.W = !bAtlasHasAlpha || !bSourceHasAlpha ? 255.0f : Samplers.Opacity.IsSet() ? Opacity * Weight : Colour.W;
				const int32 Index = Row * Out.Size.X + Column;
				Out.BaseColor[Index] = AtlasToColor(Colour);
				Out.Normal[Index] = AtlasToColor(Normal * Weight);
				Out.Orm[Index] = AtlasToColor(Orm * Weight);
			}
		});
	}

	/** One property's baked texels at Size (resampled nearest when the bake returned another size, e.g. a constant). */
	bool AtlasBakedProperty(const FBakeOutput& Output, EMaterialProperty Property, const FIntPoint& Size, TArray<FColor>& OutPixels)
	{
		const TArray<FColor>* Data = Output.PropertyData.Find(Property);
		const FIntPoint* DataSize = Output.PropertySizes.Find(Property);
		if (!Data || !DataSize || Data->Num() == 0 || DataSize->X * DataSize->Y != Data->Num())
		{
			return false;
		}
		OutPixels.SetNumUninitialized(Size.X * Size.Y);
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			const int32 SY = FMath::Min(DataSize->Y - 1, Y * DataSize->Y / Size.Y);
			for (int32 X = 0; X < Size.X; ++X)
			{
				const int32 SX = FMath::Min(DataSize->X - 1, X * DataSize->X / Size.X);
				OutPixels[Y * Size.X + X] = (*Data)[SY * DataSize->X + SX];
			}
		}
		// Colour must be sRGB like a creator's base colour texture.
		if (Property == MP_BaseColor && Output.PropertyIsLinearColor.FindRef(Property))
		{
			for (FColor& Pixel : OutPixels)
			{
				Pixel = FLinearColor(Pixel.R / 255.0f, Pixel.G / 255.0f, Pixel.B / 255.0f, Pixel.A / 255.0f).ToFColorSRGB();
			}
		}
		return true;
	}

	/** A request for the rendered bake: a material over a padded UV rectangle. */
	struct FAtlasBakeRequest
	{
		UMaterialInterface* Material = nullptr;
		FBox2f Rectangle;
		FIntPoint Size;
		/** Which opacity output rides in alpha: none (opaque), the mask, or the translucent opacity. */
		EMaterialProperty OpacityProperty = MP_MAX;
		int32 Cell = INDEX_NONE;
	};

	/**
	 * The editor's material baking (IMaterialBakingModule, the path GLTFExporter's bake uses) on a quad whose UVs span each
	 * request's rectangle, so any material bakes as it shades over those UVs. False for requests it could not bake.
	 */
	void AtlasBakeRendered(TConstArrayView<FAtlasBakeRequest> Requests, TArray<TOptional<FAtlasCellPixels>>& OutPixels)
	{
		OutPixels.Reset();
		OutPixels.SetNum(Requests.Num());
		if (Requests.Num() == 0)
		{
			return;
		}
		IMaterialBakingModule& Baking = FModuleManager::LoadModuleChecked<IMaterialBakingModule>(TEXT("MaterialBaking"));
		// Only requests whose material can render reach the module (one with no render resource crashes it), each
		// material held against garbage collection while it bakes. Skipped ones are left unset: the caller samples them.
		TArray<int32> Taken;
		TArray<TStrongObjectPtr<UMaterialInterface>> Held;
		for (int32 Index = 0; Index < Requests.Num(); ++Index)
		{
			if (AtlasHasRenderResource(Requests[Index].Material))
			{
				Taken.Add(Index);
				Held.Emplace(Requests[Index].Material);
			}
		}
		if (Taken.Num() == 0)
		{
			return;
		}
		TArray<FMaterialData> Materials;
		TArray<FMeshData> Meshes;
		Materials.SetNum(Taken.Num());
		Meshes.SetNum(Taken.Num());
		TArray<FMaterialData*> MaterialPointers;
		TArray<FMeshData*> MeshPointers;
		for (int32 Index = 0; Index < Taken.Num(); ++Index)
		{
			const FAtlasBakeRequest& Request = Requests[Taken[Index]];
			FMaterialData& Data = Materials[Index];
			Data.Material = Request.Material;
			for (const EMaterialProperty Property : { MP_BaseColor, MP_EmissiveColor, MP_Normal, MP_Roughness, MP_Metallic, MP_AmbientOcclusion })
			{
				Data.PropertySizes.Add(Property, Request.Size);
			}
			if (Request.OpacityProperty != MP_MAX)
			{
				Data.PropertySizes.Add(Request.OpacityProperty, Request.Size);
			}
			// The quad covers every texel, so there is no background to smear, and a flat result must keep its size.
			Data.bPerformBorderSmear = false;
			Data.bPerformShrinking = false;
			Data.bTangentSpaceNormal = false;
			FMeshData& Mesh = Meshes[Index];
			Mesh.MeshDescription = nullptr;
			Mesh.TextureCoordinateIndex = 0;
			Mesh.TextureCoordinateBox = FBox2D(FVector2D(Request.Rectangle.Min.X, Request.Rectangle.Min.Y), FVector2D(Request.Rectangle.Max.X, Request.Rectangle.Max.Y));
			MaterialPointers.Add(&Data);
			MeshPointers.Add(&Mesh);
		}
		TArray<FBakeOutput> Outputs;
		Baking.BakeMaterials(MaterialPointers, MeshPointers, Outputs);
		for (int32 Index = 0; Index < Taken.Num() && Index < Outputs.Num(); ++Index)
		{
			const FAtlasBakeRequest& Request = Requests[Taken[Index]];
			const FBakeOutput& Output = Outputs[Index];
			FAtlasCellPixels Pixels;
			Pixels.Size = Request.Size;
			TArray<FColor> Rough, Metal, Occlusion, Opacity;
			if (!AtlasBakedProperty(Output, MP_BaseColor, Request.Size, Pixels.BaseColor))
			{
				continue;
			}
			// An unlit (toon) material, or one with a black base colour, shows its emissive: that is its colour.
			TArray<FColor> Emissive;
			if (AtlasBakedProperty(Output, MP_EmissiveColor, Request.Size, Emissive))
			{
				if (Output.PropertyIsLinearColor.FindRef(MP_EmissiveColor))
				{
					for (FColor& Pixel : Emissive)
					{
						Pixel = FLinearColor(Pixel.R / 255.0f, Pixel.G / 255.0f, Pixel.B / 255.0f, 1.0f).ToFColorSRGB();
					}
				}
				const bool bUnlit = Request.Material->GetShadingModels().IsUnlit();
				const bool bEmissive = FMantleAvatarAtlasFixer::BakesEmissiveAsColour(bUnlit, Pixels.BaseColor, Emissive);
				if (bEmissive)
				{
					for (int32 Texel = 0; Texel < Pixels.BaseColor.Num(); ++Texel)
					{
						Pixels.BaseColor[Texel] = FColor(Emissive[Texel].R, Emissive[Texel].G, Emissive[Texel].B, Pixels.BaseColor[Texel].A);
					}
				}
			}
			// #167: the Hair and Eye shading models put other things in the Metallic and Normal pins (Scatter, the strand Tangent,
			// Curvature): they bake metal 0 and a flat normal, or the hair comes out near-black.
			const FMaterialShadingModelField Models = Request.Material->GetShadingModels();
			const bool bBakeNormal = FMantleAvatarAtlasFixer::BakesNormal(Models);
			const bool bBakeMetal = FMantleAvatarAtlasFixer::BakesMetallic(Models);
			if (!bBakeNormal || !bBakeMetal)
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_ATLAS %s: %s shading model, so its %s not baked (metal 0%s)"), *Request.Material->GetName(),
					Models.HasShadingModel(MSM_Hair) ? TEXT("Hair") : TEXT("Eye"), bBakeNormal ? TEXT("Metallic pin is") : TEXT("Metallic and Normal pins are"),
					bBakeNormal ? TEXT("") : TEXT(", a flat normal"));
			}
			if (!bBakeNormal || !AtlasBakedProperty(Output, MP_Normal, Request.Size, Pixels.Normal))
			{
				Pixels.Normal.Init(FColor(128, 128, 255, 255), Request.Size.X * Request.Size.Y);
			}
			const bool bRough = AtlasBakedProperty(Output, MP_Roughness, Request.Size, Rough);
			const bool bMetal = bBakeMetal && AtlasBakedProperty(Output, MP_Metallic, Request.Size, Metal);
			const bool bOcclusion = AtlasBakedProperty(Output, MP_AmbientOcclusion, Request.Size, Occlusion);
			const bool bOpacity = Request.OpacityProperty != MP_MAX && AtlasBakedProperty(Output, Request.OpacityProperty, Request.Size, Opacity);
			Pixels.Orm.SetNumUninitialized(Request.Size.X * Request.Size.Y);
			for (int32 Texel = 0; Texel < Pixels.Orm.Num(); ++Texel)
			{
				Pixels.Orm[Texel] = FColor(bOcclusion ? Occlusion[Texel].R : static_cast<uint8>(255), bRough ? Rough[Texel].R : static_cast<uint8>(128),
					bMetal ? Metal[Texel].R : static_cast<uint8>(0), static_cast<uint8>(255));
				Pixels.BaseColor[Texel].A = bOpacity ? Opacity[Texel].R : static_cast<uint8>(255);
				Pixels.Normal[Texel].A = 255;
			}
			OutPixels[Taken[Index]] = MoveTemp(Pixels);
		}
	}

	/** An atlas's three images. */
	struct FAtlasImages
	{
		int32 Size = 0;
		TArray<FColor> BaseColor;
		TArray<FColor> Normal;
		TArray<FColor> Orm;
		TBitArray<> Filled;

		void Init(int32 InSize)
		{
			Size = InSize;
			BaseColor.Init(FColor(128, 128, 128, 255), Size * Size);
			Normal.Init(FColor(128, 128, 255, 255), Size * Size);
			Orm.Init(FColor(255, 128, 0, 255), Size * Size);
			Filled.Init(false, Size * Size);
		}

		void Write(const FMantleAtlasCell& Cell, int32 Padding, const FAtlasCellPixels& Pixels)
		{
			const FIntPoint Origin(Cell.Inner.Min.X - Padding, Cell.Inner.Min.Y - Padding);
			for (int32 Y = 0; Y < Pixels.Size.Y; ++Y)
			{
				const int32 AY = Origin.Y + Y;
				if (AY < 0 || AY >= Size)
				{
					continue;
				}
				for (int32 X = 0; X < Pixels.Size.X; ++X)
				{
					const int32 AX = Origin.X + X;
					if (AX < 0 || AX >= Size)
					{
						continue;
					}
					const int32 From = Y * Pixels.Size.X + X;
					const int32 To = AY * Size + AX;
					BaseColor[To] = Pixels.BaseColor[From];
					Normal[To] = Pixels.Normal[From];
					Orm[To] = Pixels.Orm[From];
					Filled[To] = true;
				}
			}
		}

		/** Grows every cell's edge texels outwards into unused space, Passes texels deep. */
		void Dilate(int32 Passes)
		{
			TArray<int32> Grown;
			for (int32 Pass = 0; Pass < Passes; ++Pass)
			{
				Grown.Reset();
				for (int32 Y = 0; Y < Size; ++Y)
				{
					for (int32 X = 0; X < Size; ++X)
					{
						const int32 Index = Y * Size + X;
						if (Filled[Index])
						{
							continue;
						}
						FVector4f Sum[3] = { FVector4f(0.0f, 0.0f, 0.0f, 0.0f), FVector4f(0.0f, 0.0f, 0.0f, 0.0f), FVector4f(0.0f, 0.0f, 0.0f, 0.0f) };
						int32 Count = 0;
						const FIntPoint Neighbours[4] = { FIntPoint(X - 1, Y), FIntPoint(X + 1, Y), FIntPoint(X, Y - 1), FIntPoint(X, Y + 1) };
						for (const FIntPoint& Next : Neighbours)
						{
							if (Next.X < 0 || Next.Y < 0 || Next.X >= Size || Next.Y >= Size || !Filled[Next.Y * Size + Next.X])
							{
								continue;
							}
							const int32 From = Next.Y * Size + Next.X;
							Sum[0] += FVector4f(BaseColor[From].R, BaseColor[From].G, BaseColor[From].B, BaseColor[From].A);
							Sum[1] += FVector4f(Normal[From].R, Normal[From].G, Normal[From].B, Normal[From].A);
							Sum[2] += FVector4f(Orm[From].R, Orm[From].G, Orm[From].B, Orm[From].A);
							++Count;
						}
						if (Count > 0)
						{
							BaseColor[Index] = AtlasToColor(Sum[0] / static_cast<float>(Count));
							Normal[Index] = AtlasToColor(Sum[1] / static_cast<float>(Count));
							Orm[Index] = AtlasToColor(Sum[2] / static_cast<float>(Count));
							Grown.Add(Index);
						}
					}
				}
				if (Grown.Num() == 0)
				{
					break;
				}
				for (const int32 Index : Grown)
				{
					Filled[Index] = true;
				}
			}
		}
	};

	/** Bakes one group's atlas: each cell rendered through its material, or sampled from its textures. */
	void AtlasBakeGroup(const FMantleAtlasGroup& Group, const FMantleAtlasLayout& Layout, const TArray<FSkeletalMaterial>& Materials,
		const FMantleAtlasSettings& Settings, int32 MaxSize, FAtlasImages& Out, TArray<FString>& OutNotes)
	{
		Out.Init(Layout.Size);
		const bool bAtlasHasAlpha = Group.Blend != EMantleAtlasBlend::Opaque;
		TArray<FAtlasBakeRequest> Requests;
		TArray<int32> Sampled;
		for (int32 Cell = 0; Cell < Layout.Cells.Num(); ++Cell)
		{
			const FMantleAtlasCell& Info = Layout.Cells[Cell];
			UMaterialInterface* Material = Materials.IsValidIndex(Info.Slot) ? Materials[Info.Slot].MaterialInterface.Get() : nullptr;
			const bool bRenderable = AtlasCanRender(Material, Settings);
			FString NoResource;
			if (!bRenderable && Material && Settings.bRenderedBake && !GUsingNullRHI && FApp::CanEverRender() && !AtlasHasRenderResource(Material, &NoResource))
			{
				// Never hand IMaterialBakingModule a material it can't render (it crashes the editor).
				OutNotes.AddUnique(FString::Printf(TEXT("%s can't be rendered (%s); its textures are sampled instead"), *Material->GetName(), *NoResource));
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_ATLAS %s can't be rendered (%s); sampled instead"), *Material->GetPathName(), *NoResource);
			}
			if (bRenderable)
			{
				AtlasFinishTextures(Material);
				FAtlasBakeRequest& Request = Requests.AddDefaulted_GetRef();
				Request.Material = Material;
				Request.Rectangle = AtlasPaddedSource(Info, Layout.Padding);
				Request.Size = FIntPoint(Info.Inner.Width() + 2 * Layout.Padding, Info.Inner.Height() + 2 * Layout.Padding);
				const EMantleAtlasBlend SourceBlend = AtlasBlendOf(Material);
				Request.OpacityProperty = !bAtlasHasAlpha || SourceBlend == EMantleAtlasBlend::Opaque ? MP_MAX
					: SourceBlend == EMantleAtlasBlend::Masked ? MP_OpacityMask : MP_Opacity;
				Request.Cell = Cell;
			}
			else
			{
				Sampled.Add(Cell);
			}
		}
		TArray<TOptional<FAtlasCellPixels>> Rendered;
		AtlasBakeRendered(Requests, Rendered);
		for (int32 Index = 0; Index < Requests.Num(); ++Index)
		{
			if (Rendered[Index].IsSet())
			{
				Out.Write(Layout.Cells[Requests[Index].Cell], Layout.Padding, Rendered[Index].GetValue());
			}
			else
			{
				OutNotes.Add(FString::Printf(TEXT("%s could not be rendered into the atlas; its textures were sampled instead"),
					*GetNameSafe(Requests[Index].Material)));
				Sampled.Add(Requests[Index].Cell);
			}
		}
		TMap<int32, FAtlasSlotSamplers> SamplersBySlot;
		for (const int32 Cell : Sampled)
		{
			const FMantleAtlasCell& Info = Layout.Cells[Cell];
			UMaterialInterface* Material = Materials.IsValidIndex(Info.Slot) ? Materials[Info.Slot].MaterialInterface.Get() : nullptr;
			FAtlasSlotSamplers* Samplers = SamplersBySlot.Find(Info.Slot);
			if (!Samplers)
			{
				Samplers = &SamplersBySlot.Add(Info.Slot);
				AtlasLoadSamplers(Material, MaxSize, *Samplers);
				if (!Samplers->BaseColor.IsSet())
				{
					OutNotes.Add(FString::Printf(TEXT("%s has no base colour texture to sample; its cell is its flat colour"), *GetNameSafe(Material)));
				}
			}
			FAtlasCellPixels Pixels;
			AtlasSampleCell(*Samplers, Info, Layout.Padding, bAtlasHasAlpha, AtlasBlendOf(Material) != EMantleAtlasBlend::Opaque, Pixels);
			Out.Write(Info, Layout.Padding, Pixels);
		}
		Out.Dilate(DilatePasses);
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Assets

	/** A package path in Folder free for BaseName, "_2", "_3" appended while one is taken. */
	void AtlasUniqueAssetName(const FString& Folder, const FString& BaseName, FString& OutPackage, FString& OutAsset)
	{
		FString Clean = BaseName;
		for (TCHAR& Character : Clean)
		{
			Character = FChar::IsAlnum(Character) || Character == TEXT('_') ? Character : TEXT('_');
		}
		for (int32 Attempt = 1;; ++Attempt)
		{
			OutAsset = Attempt == 1 ? Clean : FString::Printf(TEXT("%s_%d"), *Clean, Attempt);
			OutPackage = Folder / OutAsset;
			if (!FindPackage(nullptr, *OutPackage) && !FPackageName::DoesPackageExist(OutPackage))
			{
				return;
			}
		}
	}

	UTexture2D* AtlasCreateTexture(const FString& Folder, const FString& BaseName, int32 Size, const TArray<FColor>& Pixels, bool bSRGB,
		TextureCompressionSettings Compression, TextureGroup Group, bool bAlpha)
	{
		FString PackageName, AssetName;
		AtlasUniqueAssetName(Folder, BaseName, PackageName, AssetName);
		UPackage* Package = CreatePackage(*PackageName);
		UTexture2D* Texture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->SRGB = bSRGB;
		Texture->CompressionSettings = Compression;
		Texture->LODGroup = Group;
		Texture->CompressionNoAlpha = !bAlpha;
		Texture->PostEditChange();
		FAssetRegistryModule::AssetCreated(Texture);
		Package->MarkPackageDirty();
		return Texture;
	}

	/** A texture parameter node of Material named Name, sampling Texture (its sampler type), into the material's graph. */
	UMaterialExpressionTextureSampleParameter2D* AtlasAddTextureParameter(UMaterial& Material, const TCHAR* Name, UTexture2D* Texture, const int32 Row)
	{
		UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(&Material, NAME_None, RF_Transactional);
		Sample->ParameterName = FName(Name);
		Sample->Texture = Texture;
		Sample->SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
		Sample->MaterialExpressionEditorX = -400;
		Sample->MaterialExpressionEditorY = Row * 250;
		Material.GetExpressionCollection().AddExpression(Sample);
		return Sample;
	}

	/**
	 * The bake's own parent material, for a project without Mantle's M_MantlePlace permutations (they live in the Mantle
	 * game's /Game/Mantle/Materials; the plugin ships no materials): a plain graph built here, in Folder, whose BaseColor,
	 * Normal and ORM texture parameters (the bake's textures by default) drive the inputs, so the upload reads them by name
	 * as it reads M_MantlePlace's.
	 */
	UMaterial* AtlasCreateParentMaterial(const FString& Folder, const FString& BaseName, const FMantleAtlasGroup& Group, UTexture2D* BaseColor,
		UTexture2D* Normal, UTexture2D* Orm)
	{
		FString PackageName, AssetName;
		AtlasUniqueAssetName(Folder, BaseName, PackageName, AssetName);
		UPackage* Package = CreatePackage(*PackageName);
		UMaterial* Material = NewObject<UMaterial>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		Material->BlendMode = Group.Blend == EMantleAtlasBlend::Masked ? BLEND_Masked : Group.Blend == EMantleAtlasBlend::Translucent ? BLEND_Translucent : BLEND_Opaque;
		Material->TwoSided = Group.bTwoSided;
		Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
		UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
		UMaterialExpressionTextureSampleParameter2D* Colour = AtlasAddTextureParameter(*Material, MantlePlaceMaterialParameters::BaseColor, BaseColor, 0);
		Inputs->BaseColor.Connect(0, Colour);
		if (Group.Blend == EMantleAtlasBlend::Masked)
		{
			Inputs->OpacityMask.Connect(4, Colour);
		}
		else if (Group.Blend == EMantleAtlasBlend::Translucent)
		{
			Inputs->Opacity.Connect(4, Colour);
		}
		Inputs->Normal.Connect(0, AtlasAddTextureParameter(*Material, MantlePlaceMaterialParameters::Normal, Normal, 1));
		// ORM: occlusion, roughness, metallic in R, G, B.
		UMaterialExpressionTextureSampleParameter2D* Masks = AtlasAddTextureParameter(*Material, MantlePlaceMaterialParameters::ORM, Orm, 2);
		Inputs->AmbientOcclusion.Connect(1, Masks);
		Inputs->Roughness.Connect(2, Masks);
		Inputs->Metallic.Connect(3, Masks);
		Material->PreEditChange(nullptr);
		Material->PostEditChange();
		FAssetRegistryModule::AssetCreated(Material);
		Package->MarkPackageDirty();
		return Material;
	}

	UMaterialInstanceConstant* AtlasCreateMaterial(const FString& Folder, const FString& BaseName, const FMantleAtlasGroup& Group,
		UTexture2D* BaseColor, UTexture2D* Normal, UTexture2D* Orm, FString& OutError, TArray<UObject*>* InOutMade = nullptr)
	{
		const EMantlePlaceAlphaMode Mode = Group.Blend == EMantleAtlasBlend::Masked ? EMantlePlaceAlphaMode::Mask
			: Group.Blend == EMantleAtlasBlend::Translucent ? EMantlePlaceAlphaMode::Blend : EMantlePlaceAlphaMode::Opaque;
		const FSoftObjectPath ParentPath = FMantlePlaceMaterials::PermutationPath(Mode, Group.bTwoSided);
		UMaterialInterface* Parent = FPackageName::DoesPackageExist(ParentPath.GetLongPackageName()) ? Cast<UMaterialInterface>(ParentPath.TryLoad()) : nullptr;
		if (!Parent)
		{
			// Outside the Mantle game project: a parent of the bake's own, next to it.
			FString ParentName = BaseName;
			ParentName.RemoveFromStart(TEXT("MI_"));
			UMaterial* Own = AtlasCreateParentMaterial(Folder, TEXT("M_") + ParentName, Group, BaseColor, Normal, Orm);
			if (!Own)
			{
				OutError = FString::Printf(TEXT("the bake's material for %s could not be made"), *BaseName);
				return nullptr;
			}
			if (InOutMade)
			{
				InOutMade->Add(Own);
			}
			UE_LOG(LogMantle, Log, TEXT("MANTLE_ATLAS %s is not in this project: the bake's own parent %s"), *ParentPath.GetAssetName(), *Own->GetPathName());
			Parent = Own;
		}
		FString PackageName, AssetName;
		AtlasUniqueAssetName(Folder, BaseName, PackageName, AssetName);
		UPackage* Package = CreatePackage(*PackageName);
		UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		Material->SetParentEditorOnly(Parent);
		// The names the upload finds its textures by (FMantleCosmeticExport's ResolveSectionTextures) and M_MantlePlace's own.
		Material->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(MantlePlaceMaterialParameters::BaseColor)), BaseColor);
		Material->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(MantlePlaceMaterialParameters::Normal)), Normal);
		Material->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(MantlePlaceMaterialParameters::ORM)), Orm);
		Material->PostEditChange();
		FAssetRegistryModule::AssetCreated(Material);
		Package->MarkPackageDirty();
		return Material;
	}

	/** Deletes what a failed Apply made, so the context and the output folder are as they were. */
	void AtlasDiscard(TArray<UObject*>& Made)
	{
		for (UObject* Object : Made)
		{
			if (Object)
			{
				FAssetRegistryModule::AssetDeleted(Object);
				Object->GetOutermost()->SetDirtyFlag(false);
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
			}
		}
		Made.Reset();
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Geometry

	/** One atlas as the rewrite needs it. */
	struct FAtlasRewrite
	{
		FMantleAtlasGroup Group;
		FMantleAtlasLayout Layout;
		FName SlotName;
	};

	FVertexInstanceID AtlasCopyVertexInstance(FMeshDescription& Description, FSkeletalMeshAttributes& Attributes, FVertexInstanceID From,
		TConstArrayView<FName> MorphTargets)
	{
		const FVertexInstanceID To = Description.CreateVertexInstance(Description.GetVertexInstanceVertex(From));
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		for (int32 Channel = 0; UVs.IsValid() && Channel < UVs.GetNumChannels(); ++Channel)
		{
			UVs.Set(To, Channel, UVs.Get(From, Channel));
		}
		if (TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals(); Normals.IsValid())
		{
			Normals.Set(To, Normals.Get(From));
		}
		if (TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents(); Tangents.IsValid())
		{
			Tangents.Set(To, Tangents.Get(From));
		}
		if (TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns(); Signs.IsValid())
		{
			Signs.Set(To, Signs.Get(From));
		}
		if (TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors(); Colors.IsValid())
		{
			Colors.Set(To, Colors.Get(From));
		}
		for (const FName& Morph : MorphTargets)
		{
			if (TVertexInstanceAttributesRef<FVector3f> Delta = Attributes.GetVertexInstanceMorphNormalDelta(Morph); Delta.IsValid())
			{
				Delta.Set(To, Delta.Get(From));
			}
		}
		return To;
	}

	/**
	 * Rewrites one LOD's source geometry: every polygon of a slot in an atlas moves to that atlas's polygon group, its
	 * UV0 into its cell (a vertex instance shared with a polygon of another cell, or one left as it was, is split first),
	 * and emptied groups go. bBaseLOD: the layout was made from these UVs; another LOD's UVs are clamped into their cell.
	 * OutGroupOrder: the slot names of the polygon groups after compaction, in order.
	 */
	bool AtlasRewriteDescription(FMeshDescription& Description, bool bBaseLOD, const TArray<FSkeletalMaterial>& OldMaterials,
		const TArray<FAtlasRewrite>& Rewrites, const TMap<int32, int32>& RewriteOfSlot, TArray<FName>& OutGroupOrder, FString& OutError)
	{
		FSkeletalMeshAttributes Attributes(Description);
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		if (!UVs.IsValid() || UVs.GetNumChannels() == 0 || !SlotNames.IsValid())
		{
			OutError = TEXT("its source geometry has no UVs or material slot names");
			return false;
		}
		const TArray<FName> MorphTargets = Attributes.GetMorphTargetNames();
		const TMap<FPolygonGroupID, int32> GroupSlots = AtlasGroupSlots(Description, OldMaterials);

		// (atlas << 32 | cell) for a polygon that moves; -1 for one left as it was.
		TMap<FPolygonID, int64> KeyOfPolygon;
		TArray<FPolygonGroupID> OldGroups;
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			OldGroups.Add(Group);
			const int32 Slot = GroupSlots.FindRef(Group);
			const int32* Rewrite = RewriteOfSlot.Find(Slot);
			if (!Rewrite)
			{
				continue;
			}
			TArray<FVector2f, TInlineAllocator<4>> Corners;
			for (const FPolygonID Polygon : Description.GetPolygonGroupPolygonIDs(Group))
			{
				AtlasPolygonUVs(Description, UVs, Polygon, Corners);
				const int32 Cell = FMantleAvatarAtlasFixer::FindCell(Rewrites[*Rewrite].Layout, Slot, AtlasCentroid(Corners));
				if (Cell == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("a lower LOD draws \"%s\", which LOD 0 does not"), *AtlasSlotName(OldMaterials, Slot));
					return false;
				}
				KeyOfPolygon.Add(Polygon, (static_cast<int64>(*Rewrite) << 32) | static_cast<int64>(Cell));
			}
		}

		// Each vertex instance takes one cell's UVs: split those two keys share.
		TMap<FVertexInstanceID, int64> Owner;
		TArray<FPolygonID> Polygons;
		for (const FPolygonID Polygon : Description.Polygons().GetElementIDs())
		{
			Polygons.Add(Polygon);
		}
		for (const FPolygonID Polygon : Polygons)
		{
			const int64* Found = KeyOfPolygon.Find(Polygon);
			const int64 Key = Found ? *Found : -1;
			const TArray<FVertexInstanceID> Perimeter = Description.GetPolygonVertexInstances(Polygon);
			for (int32 Corner = 0; Corner < Perimeter.Num(); ++Corner)
			{
				const int64* Existing = Owner.Find(Perimeter[Corner]);
				if (!Existing)
				{
					Owner.Add(Perimeter[Corner], Key);
				}
				else if (*Existing != Key)
				{
					const FVertexInstanceID Split = AtlasCopyVertexInstance(Description, Attributes, Perimeter[Corner], MorphTargets);
					Description.SetPolygonVertexInstance(Polygon, Corner, Split);
					Owner.Add(Split, Key);
				}
			}
		}
		for (const TPair<FVertexInstanceID, int64>& Pair : Owner)
		{
			if (Pair.Value < 0)
			{
				continue;
			}
			const FMantleAtlasLayout& Layout = Rewrites[static_cast<int32>(Pair.Value >> 32)].Layout;
			const int32 Cell = static_cast<int32>(Pair.Value & 0xffffffff);
			FVector2f UV = UVs.Get(Pair.Key, 0);
			if (!bBaseLOD)
			{
				const FBox2f& Source = Layout.Cells[Cell].Source;
				UV = FVector2f(FMath::Clamp(UV.X, Source.Min.X, Source.Max.X), FMath::Clamp(UV.Y, Source.Min.Y, Source.Max.Y));
			}
			UVs.Set(Pair.Key, 0, FMantleAvatarAtlasFixer::RemapUV(Layout, Cell, UV));
		}

		// Polygons into their atlas's group, one per atlas, created on first use.
		TArray<FPolygonGroupID> NewGroups;
		NewGroups.Init(FPolygonGroupID(INDEX_NONE), Rewrites.Num());
		for (const TPair<FPolygonID, int64>& Pair : KeyOfPolygon)
		{
			const int32 Rewrite = static_cast<int32>(Pair.Value >> 32);
			if (NewGroups[Rewrite].GetValue() == INDEX_NONE)
			{
				NewGroups[Rewrite] = Description.CreatePolygonGroup();
				SlotNames.Set(NewGroups[Rewrite], Rewrites[Rewrite].SlotName);
			}
			Description.SetPolygonPolygonGroup(Pair.Key, NewGroups[Rewrite]);
		}
		for (const FPolygonGroupID Group : OldGroups)
		{
			if (Description.GetPolygonGroupPolygonIDs(Group).Num() == 0)
			{
				Description.DeletePolygonGroup(Group);
			}
		}
		FElementIDRemappings Remappings;
		Description.Compact(Remappings);

		OutGroupOrder.Reset();
		const TPolygonGroupAttributesRef<FName> Compacted = Attributes.GetPolygonGroupMaterialSlotNames();
		for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
		{
			OutGroupOrder.Add(Compacted.Get(Group));
		}
		return true;
	}

	/** The fix's problem line in the upload checks' own words. */
	FString AtlasProblem(const FAtlasSurvey& Survey, int32 Limit)
	{
		TArray<FMantleCosmeticIssue> Issues;
		if (Limit > 0)
		{
			FMantleContractClass Sections;
			Sections.MaxSections = Limit;
			Sections.MaxInfluences = 0;
			FMantleCosmeticChecks::CheckMeshLimits(0, 0, Survey.Slots.Num(), 0, Sections, Issues);
		}
		for (const FMantleAtlasSlot& Slot : Survey.Slots)
		{
			FMantleCosmeticChecks::CheckUVTiles(FString::Printf(TEXT("section \"%s\""), *Slot.Name), Slot.Tiles, Issues);
		}
		TArray<FString> Messages;
		for (const FMantleCosmeticIssue& Issue : Issues)
		{
			Messages.Add(Issue.Message);
		}
		return FString::Join(Messages, TEXT(" "));
	}
}

int32 FMantleAvatarAtlasFixer::SectionLimitOf(const FString& Slot)
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	const FMantleContractClass* Class = Contract ? MantleAvatarAtlasFixerPrivate::AtlasClassOf(*Contract, Slot) : nullptr;
	if (!Class)
	{
		return 0;
	}
	return Class->MaxSections > 0 ? Class->MaxSections : Class->MaxMaterials;
}

bool FMantleAvatarAtlasFixer::PlanGroups(TConstArrayView<FMantleAtlasSlot> Slots, int32 Limit, bool bMergeOnlyToLimit, TArray<FMantleAtlasGroup>& OutGroups,
	FString& OutWhyNot)
{
	OutGroups.Reset();
	OutWhyNot.Reset();
	TArray<bool> Grouped;
	Grouped.Init(false, Slots.Num());
	auto AddGroup = [&OutGroups, &Grouped, Slots](TArray<int32> Members, const FString& Role, bool bHair)
	{
		Members.Sort();
		FMantleAtlasGroup& Group = OutGroups.AddDefaulted_GetRef();
		Group.Role = Role;
		Group.bHair = bHair;
		bool bMasked = false;
		bool bTranslucent = false;
		for (const int32 Member : Members)
		{
			Group.Slots.Add(Slots[Member].Slot);
			Group.bTwoSided |= Slots[Member].bTwoSided;
			bMasked |= Slots[Member].Blend == EMantleAtlasBlend::Masked;
			bTranslucent |= Slots[Member].Blend == EMantleAtlasBlend::Translucent;
			Grouped[Member] = true;
		}
		Group.Blend = bTranslucent ? EMantleAtlasBlend::Translucent : bMasked ? EMantleAtlasBlend::Masked : EMantleAtlasBlend::Opaque;
	};

	if (Limit > 0 && Slots.Num() > Limit)
	{
		struct FBucket
		{
			FString Role;
			EMantleAtlasBlend Blend = EMantleAtlasBlend::Opaque;
			bool bHair = false;
			TArray<int32> Members;
		};
		TArray<FBucket> Buckets;
		int32 Fixed = 0;
		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			const FMantleAtlasSlot& Slot = Slots[Index];
			if (Slot.Blend == EMantleAtlasBlend::Translucent || Slot.bHidden)
			{
				++Fixed;
				continue;
			}
			const bool bHair = Slot.Blend == EMantleAtlasBlend::Masked && Slot.bHair;
			FBucket* Bucket = Buckets.FindByPredicate([&Slot, bHair](const FBucket& Candidate)
			{
				return Candidate.Role.Equals(Slot.Role, ESearchCase::CaseSensitive) && Candidate.Blend == Slot.Blend && Candidate.bHair == bHair;
			});
			if (!Bucket)
			{
				Bucket = &Buckets.AddDefaulted_GetRef();
				Bucket->Role = Slot.Role;
				Bucket->Blend = Slot.Blend;
				Bucket->bHair = bHair;
			}
			Bucket->Members.Add(Index);
		}
		// Still over: a masked slot that is not hair joins the opaque atlas of its role, which turns masked.
		if (Fixed + Buckets.Num() > Limit)
		{
			for (int32 Index = Buckets.Num() - 1; Index >= 0 && Fixed + Buckets.Num() > Limit; --Index)
			{
				if (Buckets[Index].Blend != EMantleAtlasBlend::Masked || Buckets[Index].bHair)
				{
					continue;
				}
				const FString Role = Buckets[Index].Role;
				FBucket* Opaque = Buckets.FindByPredicate([&Role](const FBucket& Candidate)
				{
					return Candidate.Role.Equals(Role, ESearchCase::CaseSensitive) && Candidate.Blend == EMantleAtlasBlend::Opaque;
				});
				if (Opaque)
				{
					const TArray<int32> Members = Buckets[Index].Members;
					Opaque->Members.Append(Members);
					Buckets.RemoveAt(Index);
				}
			}
		}
		if (Fixed + Buckets.Num() > Limit)
		{
			OutWhyNot = FString::Printf(TEXT("%d material sections, the limit is %d, and merging leaves %d: translucent sections, parts the game hides ")
				TEXT("(eyelashes, eye shells), masked hair and each avatar role (skin, eyes, other) keep materials of their own."),
				Slots.Num(), Limit, Fixed + Buckets.Num());
			return false;
		}

		if (!bMergeOnlyToLimit)
		{
			for (const FBucket& Bucket : Buckets)
			{
				if (Bucket.Members.Num() >= 2)
				{
					AddGroup(Bucket.Members, Bucket.Role, Bucket.bHair);
				}
			}
		}
		else
		{
			// The biggest buckets first, their cheapest slots (fewest texels) first, until the limit is met.
			int32 Excess = Slots.Num() - Limit;
			TArray<int32> Order;
			for (int32 Index = 0; Index < Buckets.Num(); ++Index)
			{
				Order.Add(Index);
			}
			Order.StableSort([&Buckets](int32 A, int32 B) { return Buckets[A].Members.Num() > Buckets[B].Members.Num(); });
			for (const int32 Index : Order)
			{
				if (Excess <= 0)
				{
					break;
				}
				TArray<int32> Members = Buckets[Index].Members;
				if (Members.Num() < 2)
				{
					continue;
				}
				Members.StableSort([Slots](int32 A, int32 B) { return Slots[A].TexelNeed < Slots[B].TexelNeed; });
				const int32 Take = FMath::Min(Members.Num(), Excess + 1);
				Members.SetNum(Take);
				AddGroup(Members, Buckets[Index].Role, Buckets[Index].bHair);
				Excess -= Take - 1;
			}
		}
	}

	// A slot on UDIM tiles that no merge took is baked alone.
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (!Grouped[Index] && Slots[Index].Tiles.IsUdim())
		{
			AddGroup({ Index }, Slots[Index].Role, Slots[Index].Blend == EMantleAtlasBlend::Masked && Slots[Index].bHair);
		}
	}
	return true;
}

void FMantleAvatarAtlasFixer::BuildCells(TConstArrayView<FMantleAtlasFace> Faces, const FMantleAtlasGroup& Group, TFunctionRef<bool(int32 Slot)> bUdimSlot,
	TFunctionRef<int32(int32 Slot)> TexelsPerUV, FMantleAtlasLayout& OutLayout, TArray<int32>& OutFaceCell)
{
	OutLayout.Cells.Reset();
	OutFaceCell.Init(INDEX_NONE, Faces.Num());
	for (const int32 Slot : Group.Slots)
	{
		const bool bUdim = bUdimSlot(Slot);
		const int32 Texels = FMath::Max(1, TexelsPerUV(Slot));
		TMap<FIntPoint, int32> CellOfTile;
		for (int32 Face = 0; Face < Faces.Num(); ++Face)
		{
			if (Faces[Face].Slot != Slot || Faces[Face].UVs.Num() == 0)
			{
				continue;
			}
			const FVector2f Centroid = MantleAvatarAtlasFixerPrivate::AtlasCentroid(Faces[Face].UVs);
			const FIntPoint Tile = bUdim ? FIntPoint(FMath::FloorToInt32(Centroid.X), FMath::FloorToInt32(Centroid.Y)) : FIntPoint::ZeroValue;
			int32* Cell = CellOfTile.Find(Tile);
			if (!Cell)
			{
				FMantleAtlasCell& Made = OutLayout.Cells.AddDefaulted_GetRef();
				Made.Slot = Slot;
				Made.Tile = Tile;
				Made.bWholeSlot = !bUdim;
				Made.TexelsPerUV = Texels;
				Cell = &CellOfTile.Add(Tile, OutLayout.Cells.Num() - 1);
			}
			FMantleAtlasCell& Target = OutLayout.Cells[*Cell];
			for (const FVector2f& UV : Faces[Face].UVs)
			{
				Target.Source += UV;
			}
			OutFaceCell[Face] = *Cell;
		}
	}
	// Never an empty rectangle: at least one of the slot's own texels wide (UVs collapsed on a flat-colour section).
	for (FMantleAtlasCell& Cell : OutLayout.Cells)
	{
		const float MinExtent = 1.0f / Cell.TexelsPerUV;
		const FVector2f Center = Cell.Source.GetCenter();
		const FVector2f Extent = Cell.Source.GetSize();
		const FVector2f Half(FMath::Max(Extent.X, MinExtent) * 0.5f, FMath::Max(Extent.Y, MinExtent) * 0.5f);
		Cell.Source = FBox2f(Center - Half, Center + Half);
	}
}

bool FMantleAvatarAtlasFixer::PackLayout(FMantleAtlasLayout& Layout, int32 MaxSize)
{
	using namespace MantleAvatarAtlasFixerPrivate;
	const int32 Padding = FMath::Max(0, Layout.Padding);
	const int32 Max = FMath::Max(MaxSize, MinCellPixels + 2 * Padding);
	const int32 Cells = Layout.Cells.Num();
	if (Cells == 0)
	{
		return false;
	}
	// Each cell's size at full resolution (its source texels), within the largest atlas.
	TArray<FVector2f> Full;
	for (const FMantleAtlasCell& Cell : Layout.Cells)
	{
		const FVector2f Extent = Cell.Source.GetSize();
		Full.Add(FVector2f(FMath::Clamp(Extent.X * Cell.TexelsPerUV, 1.0f, static_cast<float>(Max - 2 * Padding)),
			FMath::Clamp(Extent.Y * Cell.TexelsPerUV, 1.0f, static_cast<float>(Max - 2 * Padding))));
	}
	TArray<FIntRect> Placed;
	Placed.SetNum(Cells);
	// Shelf packing, tallest first: false when the cells at Scale do not fit a Size x Size square.
	auto Pack = [&](float Scale, int32 Size) -> bool
	{
		TArray<FIntPoint> Sizes;
		TArray<int32> Order;
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Sizes.Add(FIntPoint(FMath::Max(MinCellPixels, FMath::FloorToInt32(Full[Index].X * Scale)), FMath::Max(MinCellPixels, FMath::FloorToInt32(Full[Index].Y * Scale))));
			Order.Add(Index);
		}
		Order.StableSort([&Sizes](int32 A, int32 B) { return Sizes[A].Y != Sizes[B].Y ? Sizes[A].Y > Sizes[B].Y : Sizes[A].X > Sizes[B].X; });
		int32 X = 0, Y = 0, Row = 0;
		for (const int32 Index : Order)
		{
			const int32 Width = Sizes[Index].X + 2 * Padding;
			const int32 Height = Sizes[Index].Y + 2 * Padding;
			if (X + Width > Size)
			{
				Y += Row;
				X = 0;
				Row = 0;
			}
			if (Width > Size || Y + Height > Size)
			{
				return false;
			}
			Placed[Index] = FIntRect(X + Padding, Y + Padding, X + Padding + Sizes[Index].X, Y + Padding + Sizes[Index].Y);
			X += Width;
			Row = FMath::Max(Row, Height);
		}
		return true;
	};
	auto Finish = [&](float Scale, int32 Size)
	{
		Layout.Size = Size;
		Layout.Scale = Scale;
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Layout.Cells[Index].Inner = Placed[Index];
		}
		return true;
	};
	// The smallest square that keeps every source texel...
	for (int32 Size = FMath::Min(MinAtlasSize, Max); Size < Max; Size *= 2)
	{
		if (Pack(1.0f, Size))
		{
			return Finish(1.0f, Size);
		}
	}
	if (Pack(1.0f, Max))
	{
		return Finish(1.0f, Max);
	}
	// ...else the largest scale at which the cells fit the largest square.
	float Low = 0.0f, High = 1.0f;
	for (int32 Step = 0; Step < 24; ++Step)
	{
		const float Middle = 0.5f * (Low + High);
		if (Pack(Middle, Max))
		{
			Low = Middle;
		}
		else
		{
			High = Middle;
		}
	}
	return Pack(Low, Max) && Finish(Low, Max);
}

FVector2f FMantleAvatarAtlasFixer::RemapUV(const FMantleAtlasLayout& Layout, int32 Cell, const FVector2f& UV)
{
	if (!Layout.Cells.IsValidIndex(Cell) || Layout.Size <= 0)
	{
		return UV;
	}
	const FMantleAtlasCell& Info = Layout.Cells[Cell];
	const FVector2f Extent = Info.Source.GetSize();
	const float U = Extent.X > 0.0f ? (UV.X - Info.Source.Min.X) / Extent.X : 0.5f;
	const float V = Extent.Y > 0.0f ? (UV.Y - Info.Source.Min.Y) / Extent.Y : 0.5f;
	const float Size = static_cast<float>(Layout.Size);
	return FVector2f((Info.Inner.Min.X + U * Info.Inner.Width()) / Size, (Info.Inner.Min.Y + V * Info.Inner.Height()) / Size);
}

int32 FMantleAvatarAtlasFixer::FindCell(const FMantleAtlasLayout& Layout, int32 Slot, const FVector2f& Centroid)
{
	const FIntPoint Tile(FMath::FloorToInt32(Centroid.X), FMath::FloorToInt32(Centroid.Y));
	int32 Nearest = INDEX_NONE;
	float NearestDistance = TNumericLimits<float>::Max();
	for (int32 Cell = 0; Cell < Layout.Cells.Num(); ++Cell)
	{
		const FMantleAtlasCell& Info = Layout.Cells[Cell];
		if (Info.Slot != Slot)
		{
			continue;
		}
		if (Info.bWholeSlot || Info.Tile == Tile)
		{
			return Cell;
		}
		const FVector2f Closest(FMath::Clamp(Centroid.X, Info.Source.Min.X, Info.Source.Max.X), FMath::Clamp(Centroid.Y, Info.Source.Min.Y, Info.Source.Max.Y));
		const float Distance = FVector2f::DistSquared(Closest, Centroid);
		if (Distance < NearestDistance)
		{
			NearestDistance = Distance;
			Nearest = Cell;
		}
	}
	return Nearest;
}

FString FMantleAvatarAtlasFixer::DescribeAction(TConstArrayView<FMantleAtlasSlot> Slots, TConstArrayView<FMantleAtlasGroup> Groups,
	TConstArrayView<FMantleAtlasLayout> Layouts)
{
	auto SlotOf = [Slots](int32 Slot) { return Slots.FindByPredicate([Slot](const FMantleAtlasSlot& Info) { return Info.Slot == Slot; }); };
	auto SizeText = [](const FMantleAtlasLayout* Layout)
	{
		if (!Layout)
		{
			return FString(TEXT("?"));
		}
		// "2048" and a superscript two.
		FString Text = FString::Printf(TEXT("%d%c"), Layout->Size, static_cast<TCHAR>(0x00B2));
		if (Layout->Scale < 0.995f)
		{
			Text += FString::Printf(TEXT(" at %d%% of the source resolution"), FMath::Max(1, FMath::RoundToInt32(Layout->Scale * 100.0f)));
		}
		return Text;
	};
	TArray<FString> Parts;
	int32 Merged = 0;
	int32 Atlases = 0;
	bool bOpacity = false;
	TArray<FString> Sizes;
	TArray<FString> Plain;
	for (int32 Index = 0; Index < Groups.Num(); ++Index)
	{
		const FMantleAtlasGroup& Group = Groups[Index];
		if (Group.Slots.Num() < 2)
		{
			continue;
		}
		Merged += Group.Slots.Num();
		++Atlases;
		bOpacity |= Group.Blend != EMantleAtlasBlend::Opaque;
		FString Label = Group.Role;
		if (Group.Blend == EMantleAtlasBlend::Masked)
		{
			Label += Group.bHair ? TEXT(" hair") : TEXT(" masked");
		}
		Label.TrimStartAndEndInline();
		const FString Size = SizeText(Layouts.IsValidIndex(Index) ? &Layouts[Index] : nullptr);
		Sizes.Add(Label.IsEmpty() ? Size : Label + TEXT(" ") + Size);
		Plain.Add(Size);
	}
	if (Atlases > 0)
	{
		// One atlas needs no label.
		if (Atlases == 1)
		{
			Sizes = Plain;
		}
		const FString Maps = bOpacity ? TEXT("base colour with opacity, normal, ORM") : TEXT("base colour, normal, ORM");
		Parts.Add(FString::Printf(TEXT("merges %d sections into %d atlas%s (%s; %s)"), Merged, Atlases, Atlases == 1 ? TEXT("") : TEXT("es"),
			*FString::Join(Sizes, TEXT(", ")), *Maps));
	}
	for (int32 Index = 0; Index < Groups.Num(); ++Index)
	{
		const FMantleAtlasGroup& Group = Groups[Index];
		if (Group.Slots.Num() != 1)
		{
			continue;
		}
		const FMantleAtlasSlot* Slot = SlotOf(Group.Slots[0]);
		FString Tiles = Slot ? Slot->Tiles.DescribeTiles() : FString();
		if (Slot && Slot->Tiles.Tiles.Num() > 1)
		{
			const int32 First = FMantleCosmeticChecks::UdimNumber(Slot->Tiles.Tiles[0]);
			const int32 Last = FMantleCosmeticChecks::UdimNumber(Slot->Tiles.Tiles.Last());
			if (First != INDEX_NONE && Last != INDEX_NONE && Last - First == Slot->Tiles.Tiles.Num() - 1)
			{
				Tiles = FString::Printf(TEXT("%d-%d"), First, Last);
			}
		}
		Parts.Add(FString::Printf(TEXT("bakes UDIM tiles %s of \"%s\" into one %s texture set (base colour, normal, ORM)"), *Tiles,
			Slot ? *Slot->Name : TEXT("?"), *SizeText(Layouts.IsValidIndex(Index) ? &Layouts[Index] : nullptr)));
	}
	return FString::Join(Parts, TEXT("; "));
}

void FMantleAvatarAtlasFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	for (int32 Part = 0; Part < Context.Parts.Num(); ++Part)
	{
		AnalyzePart(Context, Part, OutFixes);
	}
}

void FMantleAvatarAtlasFixer::AnalyzePart(const FMantleConvertContext& Context, int32 PartIndex, TArray<FMantleConvertFix>& OutFixes, const FMantleAtlasSettings& Settings)
{
	using namespace MantleAvatarAtlasFixerPrivate;
	if (!Context.Parts.IsValidIndex(PartIndex))
	{
		return;
	}
	const FMantleConvertPart& Part = Context.Parts[PartIndex];
	const USkeletalMesh* Mesh = Part.Mesh.Get();
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	bool bAvatar = false;
	if (!Mesh || !Contract || !AtlasClassOf(*Contract, Part.Slot, &bAvatar))
	{
		return;
	}
	TArray<USkinnedAsset*> Compiling = { const_cast<USkeletalMesh*>(Mesh) };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);

	// A lifted section (hair from a face) uploads alone: only its UV tiles matter.
	int32 OnlySlot = INDEX_NONE;
	if (Part.Section != INDEX_NONE)
	{
		const TArray<FMantleSectionSpan> Spans = FMantleCosmeticExport::ReadSectionSpans(Mesh, 0);
		if (!Spans.IsValidIndex(Part.Section))
		{
			return;
		}
		OnlySlot = Spans[Part.Section].Slot;
	}
	const int32 Limit = OnlySlot == INDEX_NONE ? SectionLimitOf(Part.Slot) : 0;
	const int32 MaxSize = AtlasMaxTextureSize(Context);
	FAtlasSurvey Survey;
	FString Error;
	if (!AtlasSurveyRendered(Mesh, OnlySlot, bAvatar, MaxSize, Survey, Error))
	{
		return;
	}
	const FString Problem = AtlasProblem(Survey, Limit);
	if (Problem.IsEmpty())
	{
		return;
	}

	FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
	Fix.FixerId = MantleAvatarFixerIds::Atlas;
	Fix.PartIndex = PartIndex;
	Fix.Problem = Problem;
	if (Context.IsBuiltMetaHuman(PartIndex))
	{
		// Epic's build is tied to its DNA (face rig, blend shapes): rebuilding its geometry into an atlas would break them.
		Fix.bCanFix = false;
		Fix.bEnabled = false;
		Fix.Action = TEXT("can't: a MetaHuman Convert built is never atlased here (its face rig and blend shapes would break)");
		return;
	}
	if (!Mesh->HasMeshDescription(0))
	{
		Fix.bCanFix = false;
		Fix.bEnabled = false;
		Fix.Action = FString::Printf(TEXT("can't: %s has no source geometry to rebuild (a generated or cooked mesh); reimport it"), *Mesh->GetName());
		return;
	}
	// #46: As-is drops the simulation in its Props step, before this one (FMantleAvatarPropsFixer).
	if (Mesh->GetMeshClothingAssets().Num() > 0 && Context.Route != EMantleAvatarRoute::AsIs)
	{
		Fix.bCanFix = false;
		Fix.bEnabled = false;
		Fix.Action = FString::Printf(TEXT("can't: %s has cloth simulation, which the atlas can't rebuild; remove its clothing data first"), *Mesh->GetName());
		return;
	}
	TArray<FMantleAtlasGroup> Groups;
	FString WhyNot;
	if (!PlanGroups(Survey.Slots, Limit, Settings.bMergeOnlyToLimit, Groups, WhyNot))
	{
		Fix.bCanFix = false;
		Fix.bEnabled = false;
		Fix.Action = TEXT("can't: ") + WhyNot;
		return;
	}
	TArray<FMantleAtlasLayout> Layouts;
	TArray<TArray<int32>> FaceCells;
	if (!AtlasLayouts(Survey, Groups, MaxSize, Settings.Padding, Layouts, FaceCells, Error))
	{
		Fix.bCanFix = false;
		Fix.bEnabled = false;
		Fix.Action = TEXT("can't: ") + Error;
		return;
	}
	Fix.Action = DescribeAction(Survey.Slots, Groups, Layouts);
}

bool FMantleAvatarAtlasFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	return ApplyToPart(Context, Fix.PartIndex, FMantleAtlasSettings(), OutError);
}

bool FMantleAvatarAtlasFixer::ApplyToPart(FMantleConvertContext& Context, int32 PartIndex, const FMantleAtlasSettings& Settings, FString& OutError)
{
	using namespace MantleAvatarAtlasFixerPrivate;
	if (!Context.Parts.IsValidIndex(PartIndex))
	{
		OutError = TEXT("the atlas fix names no part");
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[PartIndex];
	USkeletalMesh* Source = Part.Mesh.Get();
	if (!Source)
	{
		OutError = TEXT("the part has no mesh");
		return false;
	}
	if (Context.IsBuiltMetaHuman(PartIndex))
	{
		OutError = TEXT("a MetaHuman Convert built is never atlased (its face rig and blend shapes would break)");
		return false;
	}
	if (Context.OutputFolder.IsEmpty() || !FPackageName::IsValidLongPackageName(Context.OutputFolder, /*bIncludeReadOnlyRoots*/ true))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a folder to write converted assets to"), *Context.OutputFolder);
		return false;
	}
	TArray<USkinnedAsset*> Compiling = { Source };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	if (!Source->HasMeshDescription(0))
	{
		OutError = FString::Printf(TEXT("%s has no source geometry to rebuild (a generated or cooked mesh); reimport it"), *Source->GetName());
		return false;
	}
	if (Source->GetMeshClothingAssets().Num() > 0)
	{
		OutError = FString::Printf(TEXT("%s has cloth simulation, which the atlas can't rebuild; remove its clothing data first"), *Source->GetName());
		return false;
	}
	bool bAvatar = false;
	if (const FMantleAvatarContract* Contract = FMantleAvatarContract::Get())
	{
		AtlasClassOf(*Contract, Part.Slot, &bAvatar);
	}
	int32 OnlySlot = INDEX_NONE;
	if (Part.Section != INDEX_NONE)
	{
		const TArray<FMantleSectionSpan> Spans = FMantleCosmeticExport::ReadSectionSpans(Source, 0);
		if (!Spans.IsValidIndex(Part.Section))
		{
			OutError = FString::Printf(TEXT("%s has no section %d"), *Source->GetName(), Part.Section);
			return false;
		}
		OnlySlot = Spans[Part.Section].Slot;
	}
	const int32 Limit = OnlySlot == INDEX_NONE ? SectionLimitOf(Part.Slot) : 0;
	const int32 MaxSize = AtlasMaxTextureSize(Context);

	// Plan on LOD 0's source geometry, the one the rewrite changes.
	FMeshDescription Base;
	if (!Source->CloneMeshDescription(0, Base))
	{
		OutError = FString::Printf(TEXT("%s's LOD 0 source geometry could not be read"), *Source->GetName());
		return false;
	}
	FAtlasSurvey Survey;
	if (!AtlasSurveyDescription(Source, Base, OnlySlot, bAvatar, MaxSize, Survey, OutError))
	{
		return false;
	}
	TArray<FMantleAtlasGroup> Groups;
	FString WhyNot;
	if (!PlanGroups(Survey.Slots, Limit, Settings.bMergeOnlyToLimit, Groups, WhyNot))
	{
		OutError = WhyNot;
		return false;
	}
	if (Groups.Num() == 0)
	{
		Context.Notes.Add(FString::Printf(TEXT("%s: no material sections to merge and no UDIM tiles to bake"), *Source->GetName()));
		return true;
	}
	TArray<FMantleAtlasLayout> Layouts;
	TArray<TArray<int32>> FaceCells;
	if (!AtlasLayouts(Survey, Groups, MaxSize, Settings.Padding, Layouts, FaceCells, OutError))
	{
		return false;
	}

	// Slot names for the atlases, unique among the slots that stay.
	const TArray<FSkeletalMaterial> OldMaterials = Source->GetMaterials();
	TSet<FString> Taken;
	TSet<int32> Merging;
	for (const FMantleAtlasGroup& Group : Groups)
	{
		Merging.Append(Group.Slots);
	}
	for (int32 Slot = 0; Slot < OldMaterials.Num(); ++Slot)
	{
		if (!Merging.Contains(Slot))
		{
			Taken.Add(AtlasSlotName(OldMaterials, Slot).ToLower());
			Taken.Add(OldMaterials[Slot].ImportedMaterialSlotName.ToString().ToLower());
		}
	}
	TArray<FAtlasRewrite> Rewrites;
	TMap<int32, int32> RewriteOfSlot;
	for (int32 Index = 0; Index < Groups.Num(); ++Index)
	{
		const FMantleAtlasGroup& Group = Groups[Index];
		FString Name;
		if (Group.Slots.Num() == 1)
		{
			// A UDIM slot baked alone keeps its name, so the role the upload gives it by name is kept.
			Name = AtlasSlotName(OldMaterials, Group.Slots[0]);
		}
		else
		{
			// The role the upload reads from the name: "skin" (skin), "eyeball" (eyes), neither (other).
			Name = Group.Role.Equals(TEXT("skin"), ESearchCase::CaseSensitive) ? TEXT("Atlas_Skin")
				: Group.Role.Equals(TEXT("eyes"), ESearchCase::CaseSensitive) ? TEXT("Atlas_Eyeball") : TEXT("Atlas");
			if (Group.Blend == EMantleAtlasBlend::Masked)
			{
				Name += Group.bHair ? TEXT("_Hair") : TEXT("_Masked");
			}
		}
		FString Unique = Name;
		for (int32 Suffix = 2; Taken.Contains(Unique.ToLower()); ++Suffix)
		{
			Unique = FString::Printf(TEXT("%s_%d"), *Name, Suffix);
		}
		Taken.Add(Unique.ToLower());
		FAtlasRewrite& Rewrite = Rewrites.AddDefaulted_GetRef();
		Rewrite.Group = Group;
		Rewrite.Layout = Layouts[Index];
		Rewrite.SlotName = FName(*Unique);
		for (const int32 Slot : Group.Slots)
		{
			RewriteOfSlot.Add(Slot, Index);
		}
	}

	// The new mesh: a copy of the part's, never the part's itself.
	TArray<UObject*> Made;
	FString MeshPackage, MeshAsset;
	AtlasUniqueAssetName(Context.OutputFolder, Source->GetName() + TEXT("_Atlas"), MeshPackage, MeshAsset);
	UPackage* Package = CreatePackage(*MeshPackage);
	USkeletalMesh* Work = DuplicateObject<USkeletalMesh>(Source, Package, FName(*MeshAsset));
	if (!Work)
	{
		OutError = FString::Printf(TEXT("%s could not be copied"), *Source->GetName());
		return false;
	}
	Work->ClearFlags(RF_Transient);
	Work->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	Made.Add(Work);
	TArray<USkinnedAsset*> WorkCompiling = { Work };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(WorkCompiling);

	// The atlases.
	TArray<UMaterialInterface*> AtlasMaterials;
	for (FAtlasRewrite& Rewrite : Rewrites)
	{
		FAtlasImages Images;
		AtlasBakeGroup(Rewrite.Group, Rewrite.Layout, OldMaterials, Settings, MaxSize, Images, Context.Notes);
		const FString Stem = FString::Printf(TEXT("%s_%s"), *MeshAsset, *Rewrite.SlotName.ToString());
		const bool bAlpha = Rewrite.Group.Blend != EMantleAtlasBlend::Opaque;
		UTexture2D* BaseColor = AtlasCreateTexture(Context.OutputFolder, TEXT("T_") + Stem + TEXT("_BaseColor"), Images.Size, Images.BaseColor, true,
			TC_Default, TEXTUREGROUP_Character, bAlpha);
		UTexture2D* Normal = AtlasCreateTexture(Context.OutputFolder, TEXT("T_") + Stem + TEXT("_Normal"), Images.Size, Images.Normal, false,
			TC_Normalmap, TEXTUREGROUP_CharacterNormalMap, false);
		UTexture2D* Orm = AtlasCreateTexture(Context.OutputFolder, TEXT("T_") + Stem + TEXT("_ORM"), Images.Size, Images.Orm, false,
			TC_Masks, TEXTUREGROUP_CharacterSpecular, false);
		Made.Append({ BaseColor, Normal, Orm });
		UMaterialInstanceConstant* Material = AtlasCreateMaterial(Context.OutputFolder, TEXT("MI_") + Stem, Rewrite.Group, BaseColor, Normal, Orm, OutError, &Made);
		if (!Material)
		{
			AtlasDiscard(Made);
			return false;
		}
		Made.Add(Material);
		AtlasMaterials.Add(Material);
	}

	// Every LOD with source geometry, remapped from LOD 0's layout (read from the source, whose copy is the same).
	TArray<FName> BaseOrder;
	TArray<FName> OtherNames;
	bool bGeneratedLODs = false;
	for (int32 LOD = 0; LOD < Work->GetLODNum(); ++LOD)
	{
		FMeshDescription Description;
		if (!Source->HasMeshDescription(LOD) || !Source->CloneMeshDescription(LOD, Description))
		{
			bGeneratedLODs |= LOD > 0;
			continue;
		}
		TArray<FName> Order;
		FString Error;
		if (!AtlasRewriteDescription(Description, LOD == 0, OldMaterials, Rewrites, RewriteOfSlot, Order, Error))
		{
			OutError = FString::Printf(TEXT("%s LOD %d: %s"), *Source->GetName(), LOD, *Error);
			AtlasDiscard(Made);
			return false;
		}
		if (LOD == 0)
		{
			BaseOrder = Order;
		}
		else
		{
			for (const FName& Name : Order)
			{
				OtherNames.AddUnique(Name);
			}
		}
		Work->CreateMeshDescription(LOD, MoveTemp(Description));
		USkeletalMesh::FCommitMeshDescriptionParams Commit;
		Commit.bForceUpdate = true;
		Work->CommitMeshDescription(LOD, Commit);
	}

	// Material slots in LOD 0's polygon group order (so a build that maps groups by index agrees with one that maps them by
	// name), then any a lower LOD alone draws. Slots nothing draws any more are dropped.
	TArray<FSkeletalMaterial> NewMaterials;
	auto AddSlot = [&](const FName& Name)
	{
		if (NewMaterials.ContainsByPredicate([&Name](const FSkeletalMaterial& Slot) { return Slot.ImportedMaterialSlotName == Name; }))
		{
			return;
		}
		const int32 Rewrite = Rewrites.IndexOfByPredicate([&Name](const FAtlasRewrite& Candidate) { return Candidate.SlotName == Name; });
		if (Rewrite != INDEX_NONE)
		{
			NewMaterials.Add(FSkeletalMaterial(AtlasMaterials[Rewrite], Name, Name));
			return;
		}
		const int32 Old = AtlasSlotOfGroup(Name, INDEX_NONE, OldMaterials);
		FSkeletalMaterial Kept = OldMaterials.IsValidIndex(Old) ? OldMaterials[Old] : FSkeletalMaterial(nullptr, Name, Name);
		Kept.ImportedMaterialSlotName = Name;
		NewMaterials.Add(Kept);
	};
	for (const FName& Name : BaseOrder)
	{
		AddSlot(Name);
	}
	for (const FName& Name : OtherNames)
	{
		AddSlot(Name);
	}
	Work->SetMaterials(NewMaterials);
	for (int32 LOD = 0; LOD < Work->GetLODNum(); ++LOD)
	{
		if (FSkeletalMeshLODInfo* Info = Work->GetLODInfo(LOD))
		{
			// Per-section overrides named the old sections.
			Info->LODMaterialMap.Empty();
		}
		if (FSkeletalMeshModel* Model = Work->GetImportedModel(); Model && Model->LODModels.IsValidIndex(LOD))
		{
			Model->LODModels[LOD].UserSectionsData.Empty();
		}
	}
	Work->Build();
	FSkinnedAssetCompilingManager::Get().FinishCompilation(WorkCompiling);
	if (bGeneratedLODs)
	{
		// LODs the mesh generates by reduction are made again from the new LOD 0, so they draw the atlases too.
		FLODUtilities::RegenerateLOD(Work, GetTargetPlatformManagerRef().GetRunningTargetPlatform());
		FSkinnedAssetCompilingManager::Get().FinishCompilation(WorkCompiling);
	}
	Work->MarkPackageDirty();

	// The upload's own checks on the new mesh, before the part moves to it.
	const int32 PartSlotAfter = OnlySlot == INDEX_NONE ? INDEX_NONE
		: NewMaterials.IndexOfByPredicate([&](const FSkeletalMaterial& Slot) { return Slot.ImportedMaterialSlotName == Rewrites[RewriteOfSlot.FindRef(OnlySlot)].SlotName; });
	{
		FMantleGlbMesh Glb;
		TArray<int32> Slots;
		int32 Triangles = 0, Capped = 0;
		float Largest = 0.0f;
		FString ReadError;
		if (FMantleCosmeticExport::ReadSkeletalMesh(Work, 0, Glb, Slots, Triangles, Capped, Largest, ReadError))
		{
			TArray<FMantleCosmeticIssue> Issues;
			if (Limit > 0)
			{
				FMantleContractClass Sections;
				Sections.MaxSections = Limit;
				Sections.MaxInfluences = 0;
				FMantleCosmeticChecks::CheckMeshLimits(0, 0, Glb.MaterialNames.Num(), 0, Sections, Issues);
			}
			for (int32 Material = 0; Material < Glb.MaterialNames.Num(); ++Material)
			{
				if (PartSlotAfter != INDEX_NONE && (!Slots.IsValidIndex(Material) || Slots[Material] != PartSlotAfter))
				{
					continue;
				}
				TArray<FVector2f> UVs;
				TArray<uint32> Indices;
				for (const FMantleGlbPrimitive& Primitive : Glb.Primitives)
				{
					if (Primitive.MaterialIndex != Material || Primitive.FirstVertex + Primitive.NumVertices > Glb.UVs.Num())
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
				FMantleCosmeticChecks::CheckUVTiles(FString::Printf(TEXT("section \"%s\""), *Glb.MaterialNames[Material]),
					FMantleCosmeticChecks::MeasureUVTiles(UVs, Indices), Issues);
			}
			if (FMantleCosmeticChecks::HasErrors(Issues))
			{
				OutError = FString::Printf(TEXT("the rebuilt %s still fails the upload checks: %s"), *Source->GetName(), *Issues[0].Message);
				AtlasDiscard(Made);
				return false;
			}
		}
		else
		{
			Context.Notes.Add(FString::Printf(TEXT("%s: the upload checks could not read the rebuilt mesh (%s)"), *MeshAsset, *ReadError));
		}
	}

	// A lifted section is found again by its atlas slot.
	if (PartSlotAfter != INDEX_NONE)
	{
		const TArray<FMantleSectionSpan> Spans = FMantleCosmeticExport::ReadSectionSpans(Work, 0);
		const int32 Section = Spans.IndexOfByPredicate([PartSlotAfter](const FMantleSectionSpan& Span) { return Span.Slot == PartSlotAfter; });
		if (Section == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("the rebuilt %s lost the section the part lifts"), *Source->GetName());
			AtlasDiscard(Made);
			return false;
		}
		Part.Section = Section;
	}

	FAssetRegistryModule::AssetCreated(Work);
	Package->MarkPackageDirty();
	for (UObject* Object : Made)
	{
		Context.CreatedAssets.Add(Object);
	}
	Part.Mesh = Work;
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Rewrites.Num(); ++Index)
	{
		const FAtlasRewrite& Rewrite = Rewrites[Index];
		Lines.Add(FString::Printf(TEXT("%s (%d section%s, %d x %d%s)"), *Rewrite.SlotName.ToString(), Rewrite.Group.Slots.Num(),
			Rewrite.Group.Slots.Num() == 1 ? TEXT("") : TEXT("s"), Rewrite.Layout.Size, Rewrite.Layout.Size,
			Rewrite.Layout.Scale < 0.995f ? *FString::Printf(TEXT(", %d%% of the source resolution"), FMath::RoundToInt32(Rewrite.Layout.Scale * 100.0f)) : TEXT("")));
	}
	Context.Notes.Add(FString::Printf(TEXT("%s: %s now draws %d material section%s; atlases %s"), *Source->GetName(), *MeshAsset, NewMaterials.Num(),
		NewMaterials.Num() == 1 ? TEXT("") : TEXT("s"), *FString::Join(Lines, TEXT(", "))));
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_ATLAS %s -> %s slots=%d atlases=%d"), *Source->GetPathName(), *Work->GetPathName(), NewMaterials.Num(), Rewrites.Num());
	return true;
}

bool FMantleAvatarAtlasFixer::BakesMetallic(const FMaterialShadingModelField& Models)
{
	return !Models.HasShadingModel(MSM_Hair) && !Models.HasShadingModel(MSM_Eye);
}

bool FMantleAvatarAtlasFixer::BakesNormal(const FMaterialShadingModelField& Models)
{
	return !Models.HasShadingModel(MSM_Hair);
}

void FMantleAvatarAtlasFixer::BakeMaterial(UMaterialInterface* Material, const FBox2f& Rectangle, int32 Size, const FMantleAtlasSettings& Settings,
	FMantleBakedTextures& Out, TArray<FString>& OutNotes)
{
	using namespace MantleAvatarAtlasFixerPrivate;
	// One cell over the whole texture, no gutter: the section keeps its own UVs.
	Size = FMath::Max(Size, MinCellPixels);
	FMantleAtlasGroup Group;
	Group.Slots.Add(0);
	Group.Blend = AtlasBlendOf(Material);
	Group.bTwoSided = Material && Material->IsTwoSided();
	FMantleAtlasLayout Layout;
	Layout.Size = Size;
	Layout.Padding = 0;
	FMantleAtlasCell& Cell = Layout.Cells.AddDefaulted_GetRef();
	Cell.Slot = 0;
	Cell.bWholeSlot = true;
	Cell.Source = Rectangle;
	Cell.TexelsPerUV = Size;
	Cell.Inner = FIntRect(0, 0, Size, Size);
	TArray<FSkeletalMaterial> Materials;
	Materials.Add(FSkeletalMaterial(Material, NAME_None, NAME_None));
	FAtlasImages Images;
	AtlasBakeGroup(Group, Layout, Materials, Settings, Size, Images, OutNotes);
	Out.Size = Images.Size;
	Out.BaseColor = MoveTemp(Images.BaseColor);
	Out.Normal = MoveTemp(Images.Normal);
	Out.Orm = MoveTemp(Images.Orm);
}

bool FMantleAvatarAtlasFixer::BakesEmissiveAsColour(const bool bUnlit, const TConstArrayView<FColor> BaseColor, const TConstArrayView<FColor> Emissive)
{
	// Black: no channel past a few levels at any texel (a dark fabric texture still varies); coloured: some texel is lit.
	auto Brightest = [](const TConstArrayView<FColor> Pixels)
	{
		uint8 Max = 0;
		for (const FColor& Pixel : Pixels)
		{
			Max = FMath::Max(Max, FMath::Max3(Pixel.R, Pixel.G, Pixel.B));
		}
		return Max;
	};
	constexpr uint8 BlackLevel = 4;
	if (Emissive.Num() == 0 || Brightest(Emissive) <= BlackLevel)
	{
		return false;
	}
	return bUnlit || Brightest(BaseColor) <= BlackLevel;
}

bool FMantleAvatarAtlasFixer::WouldRender(const UMaterialInterface* Material, const FMantleAtlasSettings& Settings)
{
	using namespace MantleAvatarAtlasFixerPrivate;
	return MantleAvatarAtlasFixerPrivate::AtlasCanRender(Material, Settings);
}

UMaterialInstanceConstant* FMantleAvatarAtlasFixer::CreateBakedMaterial(const FString& Folder, const FString& Stem, const FMantleBakedTextures& Textures,
	EMantleAtlasBlend Blend, bool bTwoSided, TArray<UObject*>& InOutMade, FString& OutError)
{
	using namespace MantleAvatarAtlasFixerPrivate;
	const int32 Count = Textures.Size * Textures.Size;
	if (Textures.Size <= 0 || Textures.BaseColor.Num() != Count || Textures.Normal.Num() != Count || Textures.Orm.Num() != Count)
	{
		OutError = FString::Printf(TEXT("the bake of %s made no texture set"), *Stem);
		return nullptr;
	}
	const bool bAlpha = Blend != EMantleAtlasBlend::Opaque;
	UTexture2D* BaseColor = AtlasCreateTexture(Folder, TEXT("T_") + Stem + TEXT("_BaseColor"), Textures.Size, Textures.BaseColor, true, TC_Default,
		TEXTUREGROUP_Character, bAlpha);
	UTexture2D* Normal = AtlasCreateTexture(Folder, TEXT("T_") + Stem + TEXT("_Normal"), Textures.Size, Textures.Normal, false, TC_Normalmap,
		TEXTUREGROUP_CharacterNormalMap, false);
	UTexture2D* Orm = AtlasCreateTexture(Folder, TEXT("T_") + Stem + TEXT("_ORM"), Textures.Size, Textures.Orm, false, TC_Masks,
		TEXTUREGROUP_CharacterSpecular, false);
	InOutMade.Append({ BaseColor, Normal, Orm });
	FMantleAtlasGroup Group;
	Group.Blend = Blend;
	Group.bTwoSided = bTwoSided;
	UMaterialInstanceConstant* Material = AtlasCreateMaterial(Folder, TEXT("MI_") + Stem, Group, BaseColor, Normal, Orm, OutError, &InOutMade);
	if (Material)
	{
		InOutMade.Add(Material);
	}
	return Material;
}

void FMantleAvatarAtlasFixer::DiscardMade(TArray<UObject*>& Made)
{
	MantleAvatarAtlasFixerPrivate::AtlasDiscard(Made);
}

EMantleAtlasBlend FMantleAvatarAtlasFixer::BlendOf(const UMaterialInterface* Material)
{
	return MantleAvatarAtlasFixerPrivate::AtlasBlendOf(Material);
}

int32 FMantleAvatarAtlasFixer::TexelsPerUVOf(const UMaterialInterface* Material, int32 MaxSize)
{
	return MantleAvatarAtlasFixerPrivate::AtlasTexelsPerUV(Material, MaxSize);
}

void RegisterMantleAvatarAtlasFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleAvatarAtlasFixer>());
}
