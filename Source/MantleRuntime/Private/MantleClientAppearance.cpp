// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticItem.h"
#include "MantleSkinnedMesh.h"
#include "Misc/Crc.h"

namespace MantleClientAppearancePrivate
{
	struct FBinding { const TCHAR* Id; const TCHAR* Role; bool bFace; const TCHAR* Material; };
	const FBinding Bindings[] = {
		{ TEXT("skin_body_v1"), TEXT("skin"), false, TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedBody.MI_MantleSharedBody") },
		{ TEXT("skin_face_v1"), TEXT("skin"), true, TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedFace.MI_MantleSharedFace") },
		{ TEXT("eye_left_v1"), TEXT("eyes"), true, TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedEye_I_Left.MI_MantleSharedEye_I_Left") },
		{ TEXT("eye_right_v1"), TEXT("eyes"), true, TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedEye_I_Right.MI_MantleSharedEye_I_Right") },
		{ TEXT("teeth_v1"), TEXT("other"), true, TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedTeeth.MI_MantleSharedTeeth") },
		{ TEXT("eye_occlusion_v1"), TEXT("other"), true, TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedEyeOcclusion.MI_MantleSharedEyeOcclusion") },
		{ TEXT("eye_shell_v1"), TEXT("other"), true, TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedEyeShell.MI_MantleSharedEyeShell") },
		{ TEXT("eyelashes_v1"), TEXT("other"), true, TEXT("/Game/Avatars/Appearance/Shared/MI_MantleSharedEyelashes.MI_MantleSharedEyelashes") },
	};
	bool HasAppearanceInput(const FMantleCosmeticMaterial& Material)
	{
		return Material.Albedo.IsPresent() || Material.Normal.IsPresent() || Material.ORM.IsPresent()
			|| Material.ColorMask.IsPresent() || !Material.ColorRegions.IsEmpty() || Material.HairColor.IsSet()
			|| Material.NativeEye.IsSet() || Material.NativeHair.IsSet() || !Material.NativeHairProfile.IsEmpty();
	}
	uint32 CrcPair(const uint32 Hash, const FIntPoint& Pair)
	{
		uint8 Bytes[8];
		for (int32 Byte = 0; Byte < 4; ++Byte)
		{
			Bytes[Byte] = static_cast<uint8>(static_cast<uint32>(Pair.X) >> (Byte * 8));
			Bytes[Byte + 4] = static_cast<uint8>(static_cast<uint32>(Pair.Y) >> (Byte * 8));
		}
		return FCrc::MemCrc32(Bytes, UE_ARRAY_COUNT(Bytes), Hash);
	}
	// Versioned UV layouts proven against native Medium male/female, reference
	// fixtures and retained converted identities. Sorting removes vertex ordering
	// and tangent-split differences; a second triangle-multiset check retains
	// connectivity while ignoring index order and triangle winding.
	bool HasSupportedFaceUVs(const FMantleSkinnedMeshLOD& LOD, const FMantleSkinnedMeshSection& Section)
	{
		int32 ExpectedCount = 0;
		uint32 ExpectedHash = 0;
		int32 ExpectedTriangles = 0;
		uint32 ExpectedTriangleHash = 0;
		uint32 AlternateTriangleHash = 0;
		if (Section.MaterialName == TEXT("skin_face_v1"))
			{ ExpectedCount = 2652; ExpectedHash = 0xe581f5fb; ExpectedTriangles = 5048; ExpectedTriangleHash = 0xfd434c83; AlternateTriangleHash = 0x2b130cc9; }
		else if (Section.MaterialName == TEXT("eye_left_v1") || Section.MaterialName == TEXT("eye_right_v1"))
			{ ExpectedCount = 122; ExpectedHash = 0xad78fd54; ExpectedTriangles = 216; ExpectedTriangleHash = 0x668f3d32; }
		else if (Section.MaterialName == TEXT("teeth_v1"))
			{ ExpectedCount = 523; ExpectedHash = 0x0041fedd; ExpectedTriangles = 796; ExpectedTriangleHash = 0x34c9b26b; }
		else if (Section.MaterialName == TEXT("eye_shell_v1"))
			{ ExpectedCount = 60; ExpectedHash = 0x44b3a8a1; ExpectedTriangles = 196; ExpectedTriangleHash = 0x487df82f; }
		else if (Section.MaterialName == TEXT("eyelashes_v1"))
			{ ExpectedCount = 374; ExpectedHash = 0x3db972cb; ExpectedTriangles = 250; ExpectedTriangleHash = 0x057a0143; }
		else if (Section.MaterialName == TEXT("eye_occlusion_v1"))
			{ ExpectedCount = 118; ExpectedHash = 0xb8e07242; ExpectedTriangles = 150; ExpectedTriangleHash = 0x895e5891; }
		else return false; // No atlas proof yet for saliva/cartilage geometry.
		TSet<FIntPoint> Unique;
		for (int32 Index = Section.FirstIndex; Index < Section.FirstIndex + Section.IndexCount; ++Index)
		{
			const FVector2f& UV = LOD.UVs[LOD.Indices[Index]];
			if (!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y) || UV.X < 0 || UV.X > 1 || UV.Y < 0 || UV.Y > 1) return false;
			Unique.Add(FIntPoint(FMath::RoundToInt(UV.X * 2048.f), FMath::RoundToInt(UV.Y * 2048.f)));
		}
		if (Unique.Num() != ExpectedCount) return false;
		const auto LessPair = [](const FIntPoint& A, const FIntPoint& B) { return A.X == B.X ? A.Y < B.Y : A.X < B.X; };
		TArray<FIntPoint> Pairs = Unique.Array();
		Pairs.Sort(LessPair);
		uint32 Hash = 0;
		for (const FIntPoint& Pair : Pairs)
		{
			Hash = CrcPair(Hash, Pair);
		}
		if (Hash != ExpectedHash || Section.IndexCount / 3 != ExpectedTriangles) return false;
		// Epic's face builds keep the skin UV set but split its quads differently per
		// character, and the skin shader samples by UV, so any triangulation of it is accepted.
		if (Section.MaterialName == TEXT("skin_face_v1") || Section.MaterialName == TEXT("eye_shell_v1") || Section.MaterialName == TEXT("eye_occlusion_v1")) return true;
        struct FUVTriangle { FIntPoint Corners[3]; };
        TArray<FUVTriangle> Triangles;
        Triangles.Reserve(ExpectedTriangles);
        for (int32 Index = Section.FirstIndex; Index < Section.FirstIndex + Section.IndexCount; Index += 3)
        {
            FUVTriangle Triangle;
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                const FVector2f& UV = LOD.UVs[LOD.Indices[Index + Corner]];
                Triangle.Corners[Corner] = FIntPoint(FMath::RoundToInt(UV.X * 2048.f), FMath::RoundToInt(UV.Y * 2048.f));
            }
            for (int32 A = 0; A < 2; ++A)
                for (int32 B = A + 1; B < 3; ++B)
                    if (LessPair(Triangle.Corners[B], Triangle.Corners[A])) Swap(Triangle.Corners[A], Triangle.Corners[B]);
            Triangles.Add(Triangle);
        }
        Triangles.Sort([&LessPair](const FUVTriangle& A, const FUVTriangle& B)
        {
            for (int32 Corner = 0; Corner < 3; ++Corner)
                if (A.Corners[Corner] != B.Corners[Corner]) return LessPair(A.Corners[Corner], B.Corners[Corner]);
            return false;
        });
        uint32 TriangleHash = 0;
        for (const FUVTriangle& Triangle : Triangles)
            for (const FIntPoint& Pair : Triangle.Corners)
            {
                TriangleHash = CrcPair(TriangleHash, Pair);
            }
        return TriangleHash == ExpectedTriangleHash || (AlternateTriangleHash != 0 && TriangleHash == AlternateTriangleHash);
	}
}

bool MantleCosmetic::IsClientMaterialBinding(const FString& Binding, const FString& Role, const bool bFace)
{
	for (const auto& Entry : MantleClientAppearancePrivate::Bindings)
		if (Binding == Entry.Id) return Role == Entry.Role && bFace == Entry.bFace;
	return false;
}

FString MantleCosmetic::ClientMaterialPath(const FString& Binding)
{
	for (const auto& Entry : MantleClientAppearancePrivate::Bindings)
		if (Binding == Entry.Id) return Entry.Material;
	return {};
}

bool MantleCosmetic::UsesClientMaterials(const FMantleCosmeticItem& Item)
{
	for (const auto* Sections : { &Item.Materials, &Item.FaceMaterials })
		for (const auto& Material : *Sections) if (!Material.ClientMaterial.IsEmpty()) return true;
	return false;
}

bool MantleCosmetic::ValidateClientMaterials(const FMantleCosmeticItem& Item, FString& OutWhyNot)
{
	OutWhyNot.Reset();
	if (!UsesClientMaterials(Item)) return true;
	if (Item.Kind != EMantleCosmeticKind::Avatar || Item.AvatarPublicationMode != TEXT("converted")
		|| !Item.bCustomizationEligible || Item.Skeleton != TEXT("metahuman") || !Item.bGarmentCompatible
		|| !Item.Mesh.IsLoadable() || !Item.FaceMesh.IsLoadable() || Item.Materials.IsEmpty() || Item.FaceMaterials.IsEmpty()
		|| Item.Texture.IsPresent() || Item.IrisMask.IsPresent() || Item.Clip.IsPresent()
		|| Item.SourceShape.IsPresent() || Item.Coverage.IsPresent() || !Item.GarmentVariants.IsEmpty())
	{
		OutWhyNot = TEXT("clientMaterial requires an eligible converted MetaHuman body/face with geometry-only dependencies");
		return false;
	}
	int32 Left = 0, Right = 0, Body = 0, Face = 0;
	for (const bool bFace : { false, true })
	{
		const auto& Materials = bFace ? Item.FaceMaterials : Item.Materials;
		TSet<FString> Seen;
		for (const auto& Material : Materials)
		{
			if (!IsClientMaterialBinding(Material.ClientMaterial, Material.Role, bFace)
				|| MantleClientAppearancePrivate::HasAppearanceInput(Material) || Seen.Contains(Material.ClientMaterial))
			{
				OutWhyNot = TEXT("clientMaterial forbids mixed, duplicate, unknown or uploaded appearance bindings");
				return false;
			}
			Seen.Add(Material.ClientMaterial);
			Left += Material.ClientMaterial == TEXT("eye_left_v1");
			Right += Material.ClientMaterial == TEXT("eye_right_v1");
			Body += Material.ClientMaterial == TEXT("skin_body_v1");
			Face += Material.ClientMaterial == TEXT("skin_face_v1");
		}
	}
	if (Left != 1 || Right != 1 || Body != 1 || Face != 1)
	{
		OutWhyNot = TEXT("clientMaterial requires body skin, face skin and one independent left/right eye pair");
		return false;
	}
	return true;
}

bool MantleCosmetic::ValidateClientMaterialGeometry(const FMantleSkinnedMesh& Mesh,
	const TArray<FMantleCosmeticMaterial>& Materials, const bool bFace, FString& OutWhyNot)
{
	if (!Mesh.Validate(OutWhyNot) || Mesh.LODs.IsEmpty() || Mesh.LODs[0].Sections.Num() != Materials.Num())
	{
		if (OutWhyNot.IsEmpty()) OutWhyNot = TEXT("client material bindings do not match the geometry section count");
		return false;
	}
	if (bFace && Mesh.LODs.Num() != 1)
	{
		OutWhyNot = TEXT("shared face appearance currently requires its one proven source LOD");
		return false;
	}
	for (const auto& LOD : Mesh.LODs)
	{
		TSet<FName> Names;
		int32 Left = 0, Right = 0, Skin = 0;
		for (const auto& Section : LOD.Sections)
		{
			if (!Materials.IsValidIndex(Section.MaterialIndex)
				|| Section.MaterialName != FName(*Materials[Section.MaterialIndex].ClientMaterial)
				|| !IsClientMaterialBinding(Materials[Section.MaterialIndex].ClientMaterial, Materials[Section.MaterialIndex].Role, bFace)
				|| Names.Contains(Section.MaterialName))
			{
				OutWhyNot = TEXT("decoded geometry has an unknown, duplicate or mismatched client material binding");
				return false;
			}
			Names.Add(Section.MaterialName);
			if (bFace && !MantleClientAppearancePrivate::HasSupportedFaceUVs(LOD, Section))
			{
				OutWhyNot = TEXT("decoded face does not use the supported client appearance UV layout and triangle connectivity");
				return false;
			}
			const bool bLeft = Section.MaterialName == TEXT("eye_left_v1"), bRight = Section.MaterialName == TEXT("eye_right_v1");
			Left += bLeft; Right += bRight;
			Skin += Section.MaterialName == (bFace ? TEXT("skin_face_v1") : TEXT("skin_body_v1"));
			if (bLeft || bRight)
			{
				const FName Expected(bLeft ? TEXT("FACIAL_L_EyeParallel") : TEXT("FACIAL_R_EyeParallel"));
				int64 ExpectedWeight = 0, TotalWeight = 0;
				TSet<uint32> Vertices;
				for (int32 Index = Section.FirstIndex; Index < Section.FirstIndex + Section.IndexCount; ++Index) Vertices.Add(LOD.Indices[Index]);
				for (const uint32 Vertex : Vertices)
					for (int32 Influence = 0; Influence < LOD.NumInfluences; ++Influence)
					{
						const int32 Slot = Vertex * LOD.NumInfluences + Influence;
						TotalWeight += LOD.InfluenceWeights[Slot];
						if (Mesh.Bones[LOD.InfluenceBones[Slot]].Name == Expected) ExpectedWeight += LOD.InfluenceWeights[Slot];
					}
				if (TotalWeight <= 0 || ExpectedWeight * 5 < TotalWeight * 4)
				{
					OutWhyNot = TEXT("decoded eye geometry does not belong to its declared anatomical side");
					return false;
				}
			}
		}
		if (Skin != 1 || (bFace && (Left != 1 || Right != 1)))
		{
			OutWhyNot = TEXT("every shared appearance LOD needs its skin section and complete independent eyes");
			return false;
		}
	}
	return true;
}
