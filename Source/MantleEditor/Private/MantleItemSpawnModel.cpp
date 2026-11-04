// Copyright Joshua Eger. All Rights Reserved.
#include "MantleItemSpawnModel.h"

#include "Engine/StaticMesh.h"
#include "UObject/UObjectGlobals.h"

namespace MantleItemSpawnModelPrivate
{
	/** The place asset id shape the runtime parser holds an ItemSpawn's model to. */
	bool IsAssetIdShape(const FString& Id)
	{
		bool bShape = Id.Len() >= 8 && Id.Len() <= 44 && Id.StartsWith(TEXT("ast_"), ESearchCase::CaseSensitive);
		for (int32 Index = 4; bShape && Index < Id.Len(); ++Index)
		{
			const TCHAR Character = Id[Index];
			bShape = (Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_');
		}
		return bShape;
	}
}

bool FMantleItemSpawnModel::IsMeshPath(const FString& ModelId)
{
	return ModelId.StartsWith(TEXT("/"));
}

FString FMantleItemSpawnModel::PathOf(const UStaticMesh* Mesh)
{
	return Mesh ? Mesh->GetPathName() : FString();
}

UStaticMesh* FMantleItemSpawnModel::LoadMesh(const FString& ModelId)
{
	if (!IsMeshPath(ModelId))
	{
		return nullptr;
	}
	return LoadObject<UStaticMesh>(nullptr, *ModelId, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

FString FMantleItemSpawnModel::Problem(const FString& ModelId)
{
	if (ModelId.IsEmpty())
	{
		return FString();
	}
	if (IsMeshPath(ModelId))
	{
		return LoadMesh(ModelId) ? FString() : FString::Printf(TEXT("uses the model '%s', which is not a Static Mesh in your project"), *ModelId);
	}
	if (!MantleItemSpawnModelPrivate::IsAssetIdShape(ModelId))
	{
		return FString::Printf(TEXT("uses the model '%s', which is neither a Static Mesh nor a place asset id"), *ModelId);
	}
	return FString();
}
