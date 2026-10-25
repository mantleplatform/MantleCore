// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkeletonHash.h"

#include "MantleSha256.h"
#include "MantleSkinnedMesh.h"
#include "ReferenceSkeleton.h"

FString FMantleSkeletonHash::Compute(const TConstArrayView<FName> Names, const TConstArrayView<int32> Parents)
{
	if (Names.Num() != Parents.Num())
	{
		return FString();
	}
	TArray<uint8> Bytes;
	Bytes.Reserve(Names.Num() * 24);
	for (int32 BoneIndex = 0; BoneIndex < Names.Num(); ++BoneIndex)
	{
		const FTCHARToUTF8 Utf8(*Names[BoneIndex].ToString());
		for (int32 Byte = 0; Byte < Utf8.Length(); ++Byte)
		{
			uint8 Character = static_cast<uint8>(Utf8.Get()[Byte]);
			if (Character >= 'A' && Character <= 'Z')
			{
				Character = static_cast<uint8>(Character + ('a' - 'A'));
			}
			Bytes.Add(Character);
		}
		Bytes.Add(0);
		const uint32 Parent = static_cast<uint32>(Parents[BoneIndex]);
		Bytes.Add(static_cast<uint8>(Parent));
		Bytes.Add(static_cast<uint8>(Parent >> 8));
		Bytes.Add(static_cast<uint8>(Parent >> 16));
		Bytes.Add(static_cast<uint8>(Parent >> 24));
	}
	return FString(Prefix) + FMantleSha256::HexDigest(Bytes).Left(Digits);
}

FString FMantleSkeletonHash::Compute(const TConstArrayView<FMantleSkinnedMeshBone> Bones)
{
	TArray<FName> Names;
	TArray<int32> Parents;
	Names.Reserve(Bones.Num());
	Parents.Reserve(Bones.Num());
	for (const FMantleSkinnedMeshBone& Bone : Bones)
	{
		Names.Add(Bone.Name);
		Parents.Add(Bone.ParentIndex);
	}
	return Compute(Names, Parents);
}

FString FMantleSkeletonHash::Compute(const FReferenceSkeleton& Skeleton)
{
	const TArray<FMeshBoneInfo>& BoneInfo = Skeleton.GetRawRefBoneInfo();
	TArray<FName> Names;
	TArray<int32> Parents;
	Names.Reserve(BoneInfo.Num());
	Parents.Reserve(BoneInfo.Num());
	for (const FMeshBoneInfo& Bone : BoneInfo)
	{
		Names.Add(Bone.Name);
		Parents.Add(Bone.ParentIndex);
	}
	return Compute(Names, Parents);
}

bool FMantleSkeletonHash::IsHash(const FString& Value)
{
	const int32 PrefixLength = FCString::Strlen(Prefix);
	if (Value.Len() != PrefixLength + Digits || !Value.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return false;
	}
	for (int32 Index = PrefixLength; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}
