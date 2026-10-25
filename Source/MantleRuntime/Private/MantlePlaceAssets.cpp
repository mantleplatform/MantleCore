// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"

#include "Async/TaskGraphInterfaces.h"
#include "MantleAnimClip.h"
#include "MantleAssetLibrary.h"
#include "MantlePlaceAssetsCoverage.h"
#include "MantleRuntimeSkeletalMeshBuilder.h"
#include "MantleSkinnedMeshReader.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "MantleConnection.h"
#include "MantlePlaceStorage.h"
#include "MantleRuntime.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "MantleTerrainSurface.h"
#include "MantleTrace.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/ConfigCacheIni.h"
#include "PhysicsEngine/BodySetup.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include <atomic>

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceAssetsPrivate
{
	/** Every set alive, oldest first: the library asks the newest one that knows an id. Game thread. */
	TArray<FMantlePlaceAssets*> GLiveSets;

	/**
	 * Objects built by any set, by "<hash hex>:<kind>:<variant>", while something still holds them: a
	 * place rebuilt (a republish, a reconnect) reuses its meshes and textures without downloading,
	 * decoding or building them again. Weak, so nothing here keeps an object alive. Game thread.
	 */
	TMap<FString, TWeakObjectPtr<UObject>> GBuiltObjects;

	/** The batch routes are missing on this backend: remembered for the process, so the probe is paid once. */
	std::atomic<bool> GClientResolveRouteMissing{false};
	std::atomic<bool> GServerResolveRouteMissing{false};

	/** GET /assets/:id requests in flight at once when the batch route is missing. */
	constexpr int32 MaxConcurrentAssetGets = 8;

	/** Convex hulls of Body whose physics shape was cooked. */
	int32 CookedConvexHulls(const UBodySetup* Body)
	{
		int32 Cooked = 0;
		for (const FKConvexElem& Hull : Body->AggGeom.ConvexElems)
		{
			Cooked += Hull.GetChaosConvexMesh().IsValid() ? 1 : 0;
		}
		return Cooked;
	}

	/** Whether Body blocks anything: cooked triangles, or simple shapes (#470) that need no cooking or were cooked. */
	bool HasCookedCollision(const UBodySetup* Body)
	{
		return Body && (Body->TriMeshGeometries.Num() > 0 || Body->AggGeom.BoxElems.Num() > 0 || Body->AggGeom.SphereElems.Num() > 0
			|| Body->AggGeom.SphylElems.Num() > 0 || CookedConvexHulls(Body) > 0);
	}

	/** What Body's collision is, for the log: complex-as-simple triangles, or the simple shapes and the trace flag. */
	FString DescribeCookedCollision(const UBodySetup* Body)
	{
		if (!Body)
		{
			return TEXT("no body setup");
		}
		if (Body->CollisionTraceFlag == CTF_UseComplexAsSimple)
		{
			return FString::Printf(TEXT("%d triangle meshes"), Body->TriMeshGeometries.Num());
		}
		return FString::Printf(TEXT("simple: %d boxes, %d spheres, %d capsules, %d of %d convex hulls; complex queries on %s (%d triangle meshes)"),
			Body->AggGeom.BoxElems.Num(), Body->AggGeom.SphereElems.Num(), Body->AggGeom.SphylElems.Num(), CookedConvexHulls(Body),
			Body->AggGeom.ConvexElems.Num(), Body->CollisionTraceFlag == CTF_UseSimpleAsComplex ? TEXT("the simple shapes") : TEXT("the triangles"),
			Body->TriMeshGeometries.Num());
	}

	const TCHAR* RoleName(const EMantleTextureRole Role)
	{
		switch (Role)
		{
		case EMantleTextureRole::Normal: return TEXT("normal");
		case EMantleTextureRole::Data: return TEXT("data");
		case EMantleTextureRole::Color:
		default: return TEXT("color");
		}
	}

	EMantlePlaceTextureSlot SlotForRole(const EMantleTextureRole Role)
	{
		switch (Role)
		{
		case EMantleTextureRole::Normal: return EMantlePlaceTextureSlot::Normal;
		case EMantleTextureRole::Data: return EMantlePlaceTextureSlot::Orm;
		case EMantleTextureRole::Color:
		default: return EMantlePlaceTextureSlot::BaseColor;
		}
	}

	/** A cook_info textures key: a base slot's name, or a layer blend's (layerBaseColor, layerNormal, layerOrm; #533). */
	bool SlotFromName(const FString& Name, EMantlePlaceTextureSlot& Out)
	{
		for (int32 Slot = 0; Slot < MantlePlaceMaterialTextureCount; ++Slot)
		{
			if (Name.Equals(FMantlePlaceMaterials::SlotName(static_cast<EMantlePlaceTextureSlot>(Slot)), ESearchCase::CaseSensitive))
			{
				Out = static_cast<EMantlePlaceTextureSlot>(Slot);
				return true;
			}
		}
		return false;
	}

	double Megabytes(const int64 Bytes)
	{
		return static_cast<double>(Bytes) / (1024.0 * 1024.0);
	}

	/** What a decoded skinned mesh holds in memory until the game thread builds it (#469). */
	int64 SkinnedMeshBytes(const FMantleSkinnedMesh& Mesh)
	{
		SIZE_T Bytes = Mesh.Bones.GetAllocatedSize() + Mesh.ReferencePose.GetAllocatedSize();
		for (const FMantleSkinnedMeshLOD& LOD : Mesh.LODs)
		{
			Bytes += LOD.Positions.GetAllocatedSize() + LOD.Normals.GetAllocatedSize() + LOD.Tangents.GetAllocatedSize()
				+ LOD.UVs.GetAllocatedSize() + LOD.Indices.GetAllocatedSize() + LOD.InfluenceBones.GetAllocatedSize()
				+ LOD.InfluenceWeights.GetAllocatedSize() + LOD.HairCards.GetAllocatedSize();
		}
		return static_cast<int64>(Bytes);
	}

	/**
	 * #462: every uploaded mesh and material id the place's own set covers (Prepare): what its entities use, and
	 * what its vehicles' documents name (below), so a manifest-only set never takes one of them again.
	 */
	void AddLiveWeaponDependencies(const FString& WeaponId, TSet<FString>& Used)
	{
		FString Unread;
		if (const UMantleBuiltWeapon* Weapon = FMantlePlaceAssets::FindLiveWeapon(WeaponId, Unread))
		{
			for (const FString& Named : Weapon->Asset.NamedIds())
			{
				Used.Add(Named);
			}
		}
	}

	TSet<FString> UploadedIdsPlaceSetTakes(
		const FMantlePlace& Place,
		const TFunctionRef<void(const FString&, TSet<FString>&)> AddWeaponDependencies)
	{
		TSet<FString> Used;
		TArray<FString> VehicleIds;
		TArray<FString> WeaponIds;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			// A WeaponSpawn's weapon (#789): Prepare takes every weapon, spawned or listed, with what its document names.
			if (const FMantleWeaponSpawnProps* WeaponSpawn = Entity.Props.TryGet<FMantleWeaponSpawnProps>())
			{
				if (FMantlePlaceAssets::IsUploadedId(WeaponSpawn->Weapon))
				{
					Used.Add(WeaponSpawn->Weapon);
					WeaponIds.AddUnique(WeaponSpawn->Weapon);
				}
				continue;
			}
			// A VehicleSpawn's vehicle (#435): Prepare takes every vehicle, placed or listed.
			if (const FMantleVehicleSpawnProps* Spawn = Entity.Props.TryGet<FMantleVehicleSpawnProps>())
			{
				if (FMantlePlaceAssets::IsUploadedId(Spawn->Vehicle))
				{
					Used.Add(Spawn->Vehicle);
					VehicleIds.AddUnique(Spawn->Vehicle);
				}
				continue;
			}
			// A Scatter's meshes and materials (#530): Prepare takes them where something draws, and a
			// dedicated server, which draws no scatter, must not take them as manifest-only either.
			if (const FMantleScatterParams* Scatter = Entity.Props.TryGet<FMantleScatterParams>())
			{
				for (const FMantleScatterMesh& Mesh : Scatter->Meshes)
				{
					if (FMantlePlaceAssets::IsUploadedId(Mesh.MeshId))
					{
						Used.Add(Mesh.MeshId);
					}
					for (const FString& Material : Mesh.Materials)
					{
						if (FMantlePlaceAssets::IsUploadedId(Material))
						{
							Used.Add(Material);
						}
					}
				}
				continue;
			}
			// A Terrain's grass (#628): the meshes and materials Prepare takes where something draws.
			if (const FMantleTerrainProps* Terrain = Entity.Props.TryGet<FMantleTerrainProps>())
			{
				for (const FMantleTerrainGrass& Grass : Terrain->Grass)
				{
					if (FMantlePlaceAssets::IsUploadedId(Grass.Mesh))
					{
						Used.Add(Grass.Mesh);
					}
					for (const FString& Material : Grass.Materials)
					{
						if (FMantlePlaceAssets::IsUploadedId(Material))
						{
							Used.Add(Material);
						}
					}
				}
				continue;
			}
			// A custom WaterBody's surface mesh (#997): Prepare takes it on every machine, as a StaticMesh's.
			if (const FMantleWaterBodyProps* Water = Entity.Props.TryGet<FMantleWaterBodyProps>())
			{
				if (FMantlePlaceAssets::IsUploadedId(Water->MeshId))
				{
					Used.Add(Water->MeshId);
				}
				continue;
			}
			if (const FMantleStaticMeshProps* Props = MantleMeshPropsOf(Entity))
			{
				if (FMantlePlaceAssets::IsUploadedId(Props->Mesh))
				{
					Used.Add(Props->Mesh);
				}
				for (const FString& Material : Props->Materials)
				{
					if (FMantlePlaceAssets::IsUploadedId(Material))
					{
						Used.Add(Material);
					}
				}
			}
			// A SkeletalMesh's materials (#469) are prepared with it, and so is the clip it plays (so the
			// manifest-only set, which script API 1.18's Entity:PlayClip reads, never takes it again).
			else if (const FMantleSkeletalMeshProps* Skeletal = Entity.Props.TryGet<FMantleSkeletalMeshProps>())
			{
				if (FMantlePlaceAssets::IsUploadedId(Skeletal->Clip))
				{
					Used.Add(Skeletal->Clip);
				}
				for (const FString& Material : Skeletal->Materials)
				{
					if (FMantlePlaceAssets::IsUploadedId(Material))
					{
						Used.Add(Material);
					}
				}
			}
		}

		// A vehicle's part meshes and materials (#435 item 2), which only its document names: the place's set takes them
		// with the vehicle where something draws, and a dedicated server, which draws none of them, must not take them as
		// manifest-only either. Known once a live set has built the vehicle, which the place's set has by the time a
		// manifest-only set starts (after the place is built). A vehicle that failed names nothing.
		for (const FMantleAssetRef& Asset : Place.Assets)
		{
			if (Asset.Kind == EMantleAssetKind::Vehicle && FMantlePlaceAssets::IsUploadedId(Asset.Id))
			{
				VehicleIds.AddUnique(Asset.Id);
			}
			if (Asset.Kind == EMantleAssetKind::Weapon && FMantlePlaceAssets::IsUploadedId(Asset.Id))
			{
				Used.Add(Asset.Id);
				WeaponIds.AddUnique(Asset.Id);
			}
		}
		// Stored vehicles and weapons (#614 stage 5), whether or not a spawn uses them: Prepare takes a vehicle for
		// Vehicles:Spawn and a weapon for Backpack:Grant. Add their ids before expanding document-named dependencies below.
		for (const FString& StoredId : FMantleStorage::StoredVehicleAssetIds(Place))
		{
			Used.Add(StoredId);
			VehicleIds.AddUnique(StoredId);
		}
		for (const FString& StoredId : FMantleStorage::StoredWeaponAssetIds(Place))
		{
			Used.Add(StoredId);
			WeaponIds.AddUnique(StoredId);
		}
		// What a weapon's document names, once a live set has read it (as for a vehicle's parts): its meshes and materials.
		for (const FString& WeaponId : WeaponIds)
		{
			AddWeaponDependencies(WeaponId, Used);
		}
		for (const FString& VehicleId : VehicleIds)
		{
			FString Unbuilt;
			if (const UMantleBuiltVehicle* Vehicle = FMantlePlaceAssets::FindLiveVehicle(VehicleId, Unbuilt))
			{
				for (const FString& Mesh : Vehicle->Desc.PartMeshIds())
				{
					Used.Add(Mesh);
				}
				for (const FString& Material : Vehicle->Desc.MaterialIds())
				{
					Used.Add(Material);
				}
			}
		}
		return Used;
	}

	/** The developer token may stand in for a server token only outside Shipping, and only from the environment. */
	bool MayUseDeveloperToken()
	{
#if UE_BUILD_SHIPPING
		return false;
#else
		return FMantleConnection::IsAuthTokenFromEnvironment() && !FMantleConnection::ResolveAuthToken().IsEmpty();
#endif
	}

	/**
	 * The API's resolver. The client asks POST /assets/resolve, falling back to GET /assets/:id per id
	 * (bounded) when this backend has no batch route yet. The dedicated server asks POST
	 * /server/assets/resolve with its session token; without that route or token it can only use a
	 * developer's MANTLE_AUTH_TOKEN (non-Shipping, local runs), never a player credential.
	 */
	class FMantleApiAssetResolver final : public IMantleAssetResolver
	{
	public:
		explicit FMantleApiAssetResolver(const bool bInServer)
			: bServer(bInServer)
		{
		}

		virtual void Resolve(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone) override
		{
			if (bServer)
			{
				ResolveOnServer(Ids, MoveTemp(OnDone));
				return;
			}
			if (GClientResolveRouteMissing.load())
			{
				ResolveEach(Ids, MoveTemp(OnDone));
				return;
			}
			const TArray<FString> Asked = Ids;
			FMantleApiClient::ResolveAssetsAsync(Ids, FMantleApiCallback::CreateLambda(
				[Asked, OnDone](const FMantleApiResult& Response)
				{
					if (FMantleApiClient::IsMissingRoute(Response))
					{
						GClientResolveRouteMissing.store(true);
						UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS this API has no POST /assets/resolve; resolving each asset with GET /assets/:id"));
						ResolveEach(Asked, OnDone);
						return;
					}
					OnDone(Interpret(Response));
				}));
		}

	private:
		void ResolveOnServer(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone)
		{
			const bool bHasServerToken = !FMantleConnection::ResolveServerToken().IsEmpty();
			if (!bHasServerToken || GServerResolveRouteMissing.load())
			{
				if (MayUseDeveloperToken())
				{
					ResolveEach(Ids, MoveTemp(OnDone));
					return;
				}
				FMantleAssetResolveResult Refusal;
				Refusal.Error = bHasServerToken
					? TEXT("this Mantle API has no POST /server/assets/resolve, so a dedicated server cannot resolve uploaded assets")
					: TEXT("this dedicated server has no MANTLE_SERVER_TOKEN, so it cannot resolve uploaded assets");
				OnDone(Refusal);
				return;
			}
			const TArray<FString> Asked = Ids;
			FMantleApiClient::ResolveServerAssetsAsync(Ids, FMantleApiCallback::CreateLambda(
				[Asked, OnDone](const FMantleApiResult& Response)
				{
					if (FMantleApiClient::IsMissingRoute(Response))
					{
						GServerResolveRouteMissing.store(true);
						if (MayUseDeveloperToken())
						{
							UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_ASSETS this API has no POST /server/assets/resolve; resolving with the developer MANTLE_AUTH_TOKEN (local runs only)"));
							ResolveEach(Asked, OnDone);
							return;
						}
						FMantleAssetResolveResult Refusal;
						Refusal.StatusCode = Response.StatusCode;
						Refusal.Error = TEXT("this Mantle API has no POST /server/assets/resolve, so a dedicated server cannot resolve uploaded assets");
						OnDone(Refusal);
						return;
					}
					OnDone(Interpret(Response));
				}));
		}

		static FMantleAssetResolveResult Interpret(const FMantleApiResult& Response)
		{
			FMantleAssetResolveResult Resolved;
			Resolved.StatusCode = Response.StatusCode;
			if (!Response.bSuccess)
			{
				Resolved.Error = Response.Message.IsEmpty() ? FString::Printf(TEXT("the API answered %d"), Response.StatusCode) : Response.Message;
				return Resolved;
			}
			if (!FMantleApiClient::ParseResolvedAssets(Response.Body, Resolved.Assets, Resolved.Missing))
			{
				Resolved.Error = TEXT("the API's answer could not be read");
				return Resolved;
			}
			Resolved.bSuccess = true;
			return Resolved;
		}

		struct FPerIdState
		{
			TArray<FString> Queue;
			int32 InFlight = 0;
			bool bDone = false;
			FMantleAssetResolveResult Resolved;
			TFunction<void(const FMantleAssetResolveResult&)> OnDone;
		};

		static void ResolveEach(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone)
		{
			const TSharedRef<FPerIdState> State = MakeShared<FPerIdState>();
			State->Queue = Ids;
			State->OnDone = MoveTemp(OnDone);
			if (State->Queue.IsEmpty())
			{
				State->Resolved.bSuccess = true;
				State->bDone = true;
				State->OnDone(State->Resolved);
				return;
			}
			StartNext(State);
		}

		static void StartNext(const TSharedRef<FPerIdState>& State)
		{
			while (!State->bDone && State->InFlight < MaxConcurrentAssetGets && State->Queue.Num() > 0)
			{
				const FString Id = State->Queue.Pop();
				++State->InFlight;
				FMantleApiClient::GetAssetAsync(Id, FMantleApiCallback::CreateLambda([State, Id](const FMantleApiResult& Response)
				{
					--State->InFlight;
					if (Response.bSuccess)
					{
						FMantleUploadedAsset Asset;
						if (FMantleApiClient::ParseUploadedAsset(Response.Body, Asset))
						{
							State->Resolved.Assets.Add(MoveTemp(Asset));
						}
						else
						{
							State->Resolved.Failed.Add(Id, TEXT("the API's answer could not be read"));
						}
					}
					else if (Response.StatusCode == 404)
					{
						State->Resolved.Missing.Add(Id);
					}
					else
					{
						State->Resolved.Failed.Add(Id, Response.Message.IsEmpty() ? FString::Printf(TEXT("the API answered %d"), Response.StatusCode) : Response.Message);
					}

					if (!State->bDone && State->Queue.IsEmpty() && State->InFlight == 0)
					{
						State->bDone = true;
						State->Resolved.bSuccess = true;
						State->OnDone(State->Resolved);
						return;
					}
					StartNext(State);
				}));
			}
		}

		const bool bServer;
	};
}

// ---- Statics ------------------------------------------------------------------------------------------

bool FMantlePlaceAssets::IsUploadedId(const FString& Id)
{
	if (Id.Len() != 37 || !Id.StartsWith(TEXT("ast_u"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	for (int32 Index = 5; Index < Id.Len(); ++Index)
	{
		const TCHAR Character = Id[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

bool FMantlePlaceAssets::IsUploadedSoundId(const FString& Id)
{
	return IsUploadedId(Id) && !FMantleAssetLibrary::Get().IsKnown(Id);
}

bool FMantlePlaceAssets::HasUploadedAssets(const FMantlePlace& Place)
{
	// Entities, and the manifest's sounds: the manifest also lists things nothing in the world
	// builds (the official places' uploaded thumbnails), which must not turn a place with none into
	// an asynchronous load, but a sound it lists is one scripts may play (Sounds:Play, #459) even
	// when no emitter uses it. A dedicated server never hears a sound (#324), so an uploaded one
	// needs no preparing there.
	const bool bSoundsCount = !IsRunningDedicatedServer();
	if (bSoundsCount)
	{
		for (const FMantleAssetRef& Asset : Place.Assets)
		{
			if (Asset.Kind == EMantleAssetKind::Sound && IsUploadedId(Asset.Id))
			{
				return true;
			}
		}
		// #556 item 5: an image a UI document shows, which Start takes where anything draws.
		for (const FMantleUIDocument& Document : Place.UIDocuments)
		{
			if (Document.IsImage() && IsUploadedId(Document.Asset))
			{
				return true;
			}
		}
	}
	// A vehicle the manifest lists is prepared even when nothing parks it: a script may spawn it (#435).
	for (const FMantleAssetRef& Asset : Place.Assets)
	{
		if (Asset.Kind == EMantleAssetKind::Vehicle && IsUploadedId(Asset.Id))
		{
			return true;
		}
	}
	// A weapon the manifest lists (#789), likewise: a pickup somewhere may hand it out.
	for (const FMantleAssetRef& Asset : Place.Assets)
	{
		if (Asset.Kind == EMantleAssetKind::Weapon && IsUploadedId(Asset.Id))
		{
			return true;
		}
	}
	// So is a stored vehicle (#614 stage 5): a script spawns it by item id.
	if (FMantleStorage::StoredVehicleAssetIds(Place).Num() > 0)
	{
		return true;
	}
	// So is a stored weapon (#614 stage 5): Backpack:Grant needs its definition built before a script asks for it.
	if (FMantleStorage::StoredWeaponAssetIds(Place).Num() > 0)
	{
		return true;
	}
	for (const FMantleEntity& Entity : Place.Entities)
	{
		if (const FMantleWeaponSpawnProps* WeaponSpawn = Entity.Props.TryGet<FMantleWeaponSpawnProps>())
		{
			if (IsUploadedId(WeaponSpawn->Weapon))
			{
				return true;
			}
			continue;
		}
		if (const FMantleVehicleSpawnProps* Spawn = Entity.Props.TryGet<FMantleVehicleSpawnProps>())
		{
			if (IsUploadedId(Spawn->Vehicle))
			{
				return true;
			}
			continue;
		}
		if (const FMantleSoundEmitterProps* Emitter = Entity.Props.TryGet<FMantleSoundEmitterProps>())
		{
			if (bSoundsCount && IsUploadedSoundId(Emitter->Sound))
			{
				return true;
			}
			continue;
		}
		// A SkeletalMesh's mesh, materials and clip (#469): drawn only, so a dedicated server needs none.
		if (const FMantleSkeletalMeshProps* Skeletal = Entity.Props.TryGet<FMantleSkeletalMeshProps>())
		{
			if (bSoundsCount && (IsUploadedId(Skeletal->Mesh) || IsUploadedId(Skeletal->Clip)
				|| Skeletal->Materials.ContainsByPredicate([](const FString& Material) { return IsUploadedId(Material); })))
			{
				return true;
			}
			continue;
		}
		// A Scatter's meshes and materials (#530): drawn only, so a dedicated server needs none.
		if (const FMantleScatterParams* Scatter = Entity.Props.TryGet<FMantleScatterParams>())
		{
			if (bSoundsCount && Scatter->Meshes.ContainsByPredicate([](const FMantleScatterMesh& Mesh)
				{
					return IsUploadedId(Mesh.MeshId)
						|| Mesh.Materials.ContainsByPredicate([](const FString& Material) { return IsUploadedId(Material); });
				}))
			{
				return true;
			}
			continue;
		}
		// A Terrain's heightfield is always uploaded (#466), and the server needs it for collision.
		if (const FMantleTerrainProps* Terrain = Entity.Props.TryGet<FMantleTerrainProps>())
		{
			if (IsUploadedId(Terrain->Heightfield))
			{
				return true;
			}
			continue;
		}
		// A custom WaterBody's surface mesh (#997): the server needs it for the water's collision.
		if (const FMantleWaterBodyProps* Water = Entity.Props.TryGet<FMantleWaterBodyProps>())
		{
			if (IsUploadedId(Water->MeshId))
			{
				return true;
			}
			continue;
		}
		// A StaticMesh's, or an InstancedMesh's (#467), mesh and materials.
		if (const FMantleStaticMeshProps* Props = MantleMeshPropsOf(Entity))
		{
			if (IsUploadedId(Props->Mesh))
			{
				return true;
			}
			for (const FString& Material : Props->Materials)
			{
				if (IsUploadedId(Material))
				{
					return true;
				}
			}
		}
		// A Decal's textures (#472).
		if (const FMantleDecalProps* Decal = Entity.Props.TryGet<FMantleDecalProps>())
		{
			if (IsUploadedId(Decal->BaseColor) || IsUploadedId(Decal->Normal))
			{
				return true;
			}
		}
		// A sprites Effect's texture (#628).
		if (const FMantleEffectProps* Effect = Entity.Props.TryGet<FMantleEffectProps>())
		{
			if (IsUploadedId(Effect->TextureId))
			{
				return true;
			}
		}
	}
	return false;
}

FMantlePlaceAssetSettings FMantlePlaceAssetSettings::ForThisProcess()
{
	FMantlePlaceAssetSettings Settings;
	Settings.bRenderAssets = !IsRunningDedicatedServer();
	if (GConfig)
	{
		const TCHAR* Section = TEXT("Mantle.PlaceAssets");
		int32 Int = 0;
		double Double = 0.0;
		bool Bool = false;
		if (GConfig->GetInt(Section, TEXT("MaxConcurrentResolves"), Int, GGameIni) && Int > 0) { Settings.MaxConcurrentResolves = Int; }
		if (GConfig->GetInt(Section, TEXT("MaxOutstandingFetches"), Int, GGameIni) && Int > 0) { Settings.MaxOutstandingFetches = Int; }
		if (GConfig->GetInt(Section, TEXT("MaxConcurrentDecodes"), Int, GGameIni) && Int > 0) { Settings.MaxConcurrentDecodes = Int; }
		if (GConfig->GetInt(Section, TEXT("MaxPendingBuildMB"), Int, GGameIni) && Int > 0) { Settings.MaxPendingBuildBytes = static_cast<int64>(Int) * 1024 * 1024; }
		if (GConfig->GetInt(Section, TEXT("MaxTextureGpuMB"), Int, GGameIni) && Int > 0) { Settings.MaxTextureGpuBytes = static_cast<int64>(Int) * 1024 * 1024; }
		if (GConfig->GetDouble(Section, TEXT("BuildSliceMs"), Double, GGameIni) && Double > 0.0) { Settings.BuildSliceMs = Double; }
		if (GConfig->GetDouble(Section, TEXT("ProgressLogSeconds"), Double, GGameIni) && Double >= 0.0) { Settings.ProgressLogSeconds = Double; }
		if (GConfig->GetBool(Section, TEXT("AsyncCollisionCook"), Bool, GGameIni)) { Settings.bAsyncCollisionCook = Bool; }
		if (GConfig->GetInt(Section, TEXT("ManifestOnlyMaxAssets"), Int, GGameIni) && Int >= 0) { Settings.MaxManifestOnlyAssets = Int; }
		if (GConfig->GetInt(Section, TEXT("ManifestOnlyMaxMB"), Int, GGameIni) && Int >= 0) { Settings.MaxManifestOnlyBytes = static_cast<int64>(Int) * 1024 * 1024; }
	}
	return Settings;
}

FMantlePlaceAssetSettings FMantlePlaceAssetSettings::ForManifestOnly()
{
	FMantlePlaceAssetSettings Settings = ForThisProcess();
	Settings.MaxConcurrentResolves = 1;
	Settings.MaxOutstandingFetches = FMath::Min(Settings.MaxOutstandingFetches, 4);
	Settings.MaxConcurrentDecodes = FMath::Min(Settings.MaxConcurrentDecodes, 2);
	Settings.BuildSliceMs = FMath::Min(Settings.BuildSliceMs, 2.0);
	return Settings;
}

bool FMantlePlaceAssets::HasManifestOnlyAssets(const FMantlePlace& Place, const bool bRenderAssets)
{
	const TSet<FString> Used = MantlePlaceAssetsPrivate::UploadedIdsPlaceSetTakes(Place, MantlePlaceAssetsPrivate::AddLiveWeaponDependencies);
	for (const FMantleAssetRef& Asset : Place.Assets)
	{
		// A clip only where something draws it (Entity:PlayClip, script API 1.18): a dedicated server never animates.
		const bool bWanted = Asset.Kind == EMantleAssetKind::StaticMesh
			|| (bRenderAssets && (Asset.Kind == EMantleAssetKind::Material || Asset.Kind == EMantleAssetKind::AnimClip));
		if (bWanted && IsUploadedId(Asset.Id) && !Used.Contains(Asset.Id))
		{
			return true;
		}
	}
	return false;
}

TSharedRef<IMantleAssetResolver> FMantlePlaceAssets::CreateApiResolver(const bool bServer)
{
	return MakeShared<MantlePlaceAssetsPrivate::FMantleApiAssetResolver>(bServer);
}

TSharedRef<FMantlePlaceAssets> FMantlePlaceAssets::Prepare(const FMantlePlace& Place, const FMantlePlaceAssetSettings& Settings,
	FMantlePlaceAssetsDone OnDone)
{
	return PrepareWith(Place, Settings, MoveTemp(OnDone), /*bManifestOnly*/ false);
}

TSharedRef<FMantlePlaceAssets> FMantlePlaceAssets::PrepareManifestOnly(const FMantlePlace& Place, const FMantlePlaceAssetSettings& Settings,
	FMantlePlaceAssetsDone OnDone)
{
	return PrepareWith(Place, Settings, MoveTemp(OnDone), /*bManifestOnly*/ true);
}

TSharedPtr<FMantlePlaceAssets> FMantlePlaceAssets::PrepareManifestOnlyForThisProcess(const FMantlePlace& Place)
{
	const FMantlePlaceAssetSettings Settings = FMantlePlaceAssetSettings::ForManifestOnly();
	if (!HasManifestOnlyAssets(Place, Settings.bRenderAssets))
	{
		return nullptr;
	}
	const FString PlaceId = Place.PlaceId;
	return PrepareManifestOnly(Place, Settings, FMantlePlaceAssetsDone::CreateLambda([PlaceId](const FMantlePlaceAssetsResult& Result)
	{
		// Nothing waits on this set: a failure only means a script's spawn or material swap of
		// that id fails with the reason (GetLiveMeshState), so it is logged, never fatal.
		if (!Result.bSuccess)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_ASSETS manifest-only assets of place %s: %d did not load:\n%s"),
				*PlaceId, Result.Errors.Num(), *Result.ErrorSummary());
		}
	}));
}

TSharedRef<FMantlePlaceAssets> FMantlePlaceAssets::PrepareWith(const FMantlePlace& Place, const FMantlePlaceAssetSettings& Settings,
	FMantlePlaceAssetsDone OnDone, const bool bManifestOnly)
{
	check(IsInGameThread());
	TSharedRef<FMantlePlaceAssets> Set = MakeShareable(new FMantlePlaceAssets(Settings, MoveTemp(OnDone)));
	if (bManifestOnly)
	{
		Set->StartManifestOnly(Place);
	}
	else
	{
		Set->Start(Place);
	}
	if (Set->Settings.bAutoPump)
	{
		const TWeakPtr<FMantlePlaceAssets> Weak = Set;
		Set->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
		{
			if (const TSharedPtr<FMantlePlaceAssets> Pinned = Weak.Pin())
			{
				Pinned->Pump();
				// Past OnDone while sounds are still arriving: they bind to their emitters as they do.
				return !Pinned->IsComplete() || !Pinned->AreSoundsSettled();
			}
			return false;
		}));
	}
	return Set;
}

UStaticMesh* FMantlePlaceAssets::FindLiveMesh(const FString& Id, FString& OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (GLiveSets[Index]->FindRecord(Id, EAssetKind::Mesh))
		{
			return GLiveSets[Index]->FindMesh(Id, &OutError);
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded mesh no loaded place prepared (FMantlePlaceAssets::Prepare runs before the place is built)"), *Id);
	return nullptr;
}

UMaterialInterface* FMantlePlaceAssets::FindLiveMaterial(const FString& Id, FString& OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (GLiveSets[Index]->FindRecord(Id, EAssetKind::Material))
		{
			return GLiveSets[Index]->FindMaterial(Id, &OutError);
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded material no loaded place prepared (FMantlePlaceAssets::Prepare runs before the place is built)"), *Id);
	return nullptr;
}

USkeletalMesh* FMantlePlaceAssets::FindLiveSkeletalMesh(const FString& Id, FString& OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (GLiveSets[Index]->FindRecord(Id, EAssetKind::SkeletalMesh))
		{
			return GLiveSets[Index]->FindSkeletalMesh(Id, &OutError);
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded skeletal mesh no loaded place prepared (FMantlePlaceAssets::Prepare runs before the place is built)"), *Id);
	return nullptr;
}

TSharedPtr<const FMantleAnimClip> FMantlePlaceAssets::FindLiveClip(const FString& Id, FString& OutError, bool* bOutStillLoading)
{
	using namespace MantlePlaceAssetsPrivate;
	if (bOutStillLoading)
	{
		*bOutStillLoading = false;
	}
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (GLiveSets[Index]->FindRecord(Id, EAssetKind::AnimClip))
		{
			return GLiveSets[Index]->FindClip(Id, &OutError, bOutStillLoading);
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded animation clip no loaded place prepared (FMantlePlaceAssets::Prepare runs before the place is built)"), *Id);
	return nullptr;
}

UMantleBuiltVehicle* FMantlePlaceAssets::FindLiveVehicle(const FString& Id, FString& OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (GLiveSets[Index]->FindRecord(Id, EAssetKind::Vehicle))
		{
			return GLiveSets[Index]->FindVehicle(Id, &OutError);
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded vehicle no loaded place prepared (the place must list it or park it)"), *Id);
	return nullptr;
}

EMantleLiveAssetState FMantlePlaceAssets::GetLiveVehicleState(const FString& Id, FString* OutError)
{
	return LiveStateOf(Id, EAssetKind::Vehicle, OutError);
}

UMantleBuiltWeapon* FMantlePlaceAssets::FindLiveWeapon(const FString& Id, FString& OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (GLiveSets[Index]->FindRecord(Id, EAssetKind::Weapon))
		{
			return GLiveSets[Index]->FindWeapon(Id, &OutError);
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded weapon no loaded place prepared (the place must list it or spawn it)"), *Id);
	return nullptr;
}

EMantleLiveAssetState FMantlePlaceAssets::GetLiveWeaponState(const FString& Id, FString* OutError)
{
	return LiveStateOf(Id, EAssetKind::Weapon, OutError);
}

TSharedPtr<const FMantleSoundData> FMantlePlaceAssets::FindLiveSound(const FString& Id, FString& OutError, bool* bOutStillLoading)
{
	using namespace MantlePlaceAssetsPrivate;
	if (bOutStillLoading)
	{
		*bOutStillLoading = false;
	}
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (GLiveSets[Index]->FindRecord(Id, EAssetKind::Sound))
		{
			return GLiveSets[Index]->FindSound(Id, &OutError, bOutStillLoading);
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded sound no loaded place prepared (FMantlePlaceAssets::Prepare runs before the place is built)"), *Id);
	return nullptr;
}

bool FMantlePlaceAssets::WhenLiveSoundSettles(const FString& Id, TFunction<void(TSharedPtr<const FMantleSoundData> Sound, const FString& Error)> OnSettled)
{
	using namespace MantlePlaceAssetsPrivate;
	check(IsInGameThread());
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		FMantlePlaceAssets* Set = GLiveSets[Index];
		if (!Set->FindRecord(Id, EAssetKind::Sound))
		{
			continue;
		}
		FString Error;
		bool bStillLoading = false;
		TSharedPtr<const FMantleSoundData> Sound = Set->FindSound(Id, &Error, &bStillLoading);
		if (bStillLoading)
		{
			Set->SoundWaiters.FindOrAdd(Id).Add(MoveTemp(OnSettled));
			return false;
		}
		OnSettled(MoveTemp(Sound), Error);
		return true;
	}
	FString Error;
	OnSettled(FindLiveSound(Id, Error), Error);
	return true;
}

UTexture* FMantlePlaceAssets::FindLiveTexture(const FString& Id, const EMantleTextureRole Role, FString& OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	const FString Key = RecordKey(Id, EAssetKind::Texture, Role);
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (const FRecord* Record = GLiveSets[Index]->Records.Find(Key))
		{
			return Cast<UTexture>(GLiveSets[Index]->ObjectOf(Record, Id, EAssetKind::Texture, &OutError));
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded texture no loaded place prepared as a %s texture (FMantlePlaceAssets::Prepare runs before the place is built)"),
		*Id, RoleName(Role));
	return nullptr;
}

EMantleLiveAssetState FMantlePlaceAssets::GetLiveTextureState(const FString& Id, const EMantleTextureRole Role, FString* OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	const FString Key = RecordKey(Id, EAssetKind::Texture, Role);
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (const FRecord* Record = GLiveSets[Index]->Records.Find(Key))
		{
			return GLiveSets[Index]->StateOf(*Record, OutError);
		}
	}
	if (OutError)
	{
		*OutError = FString::Printf(TEXT("uploaded texture '%s' was not downloaded for this place as a %s texture"), *Id, RoleName(Role));
	}
	return EMantleLiveAssetState::NotPrepared;
}

EMantleLiveAssetState FMantlePlaceAssets::GetLiveMeshState(const FString& Id, FString* OutError)
{
	return LiveStateOf(Id, EAssetKind::Mesh, OutError);
}

EMantleLiveAssetState FMantlePlaceAssets::GetLiveMaterialState(const FString& Id, FString* OutError)
{
	return LiveStateOf(Id, EAssetKind::Material, OutError);
}

EMantleLiveAssetState FMantlePlaceAssets::GetLiveSkeletalMeshState(const FString& Id, FString* OutError)
{
	return LiveStateOf(Id, EAssetKind::SkeletalMesh, OutError);
}

EMantleLiveAssetState FMantlePlaceAssets::LiveStateOf(const FString& Id, const EAssetKind Kind, FString* OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (const FRecord* Record = GLiveSets[Index]->FindRecord(Id, Kind))
		{
			return GLiveSets[Index]->StateOf(*Record, OutError);
		}
	}
	if (OutError)
	{
		*OutError = FString::Printf(TEXT("uploaded %s '%s' was not downloaded for this place"), KindName(Kind), *Id);
	}
	return EMantleLiveAssetState::NotPrepared;
}

EMantleLiveAssetState FMantlePlaceAssets::StateOf(const FRecord& Record, FString* OutError) const
{
	auto Failed = [OutError, &Record](const FString& Why)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("uploaded %s '%s' %s"), KindName(Record.Kind), *Record.Id, *Why);
		}
		return EMantleLiveAssetState::Failed;
	};
	if (Record.bFailed)
	{
		return Failed(Record.Error);
	}
	const FBuild* Build = Record.Build.IsSet() ? Builds.Find(Record.Build.GetValue()) : nullptr;
	if (!Build)
	{
		// Still resolving (or resolving again after an expired URL); a completed set has nothing left to try.
		return bCompleted ? Failed(TEXT("was never built")) : EMantleLiveAssetState::Loading;
	}
	if (Build->State == EBuildState::Failed)
	{
		return Failed(Build->Error);
	}
	if (Build->State == EBuildState::Ready && Build->Object)
	{
		return EMantleLiveAssetState::Ready;
	}
	return EMantleLiveAssetState::Loading;
}

TSharedPtr<const FMantleHeightfield> FMantlePlaceAssets::FindLiveHeightfield(const FString& Id, FString& OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (GLiveSets[Index]->FindRecord(Id, EAssetKind::Heightfield))
		{
			return GLiveSets[Index]->FindHeightfield(Id, &OutError);
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded heightfield no loaded place prepared (FMantlePlaceAssets::Prepare runs before the place is built)"), *Id);
	return nullptr;
}

TSharedPtr<const FMantleTerrainWeights> FMantlePlaceAssets::FindLiveTerrainWeights(const FString& Id, FString& OutError)
{
	using namespace MantlePlaceAssetsPrivate;
	for (int32 Index = GLiveSets.Num() - 1; Index >= 0; --Index)
	{
		if (GLiveSets[Index]->FindRecord(Id, EAssetKind::TerrainWeights))
		{
			return GLiveSets[Index]->FindTerrainWeights(Id, &OutError);
		}
	}
	OutError = FString::Printf(TEXT("'%s' is an uploaded weightmap no loaded place prepared for its surfaces (FMantlePlaceAssets::Prepare runs before the place is built)"), *Id);
	return nullptr;
}

FString FMantlePlaceAssets::RecordKey(const FString& Id, const EAssetKind Kind, const EMantleTextureRole Role)
{
	return Kind == EAssetKind::Texture
		? FString::Printf(TEXT("%s:texture:%s"), *Id, MantlePlaceAssetsPrivate::RoleName(Role))
		: FString::Printf(TEXT("%s:%s"), *Id, KindName(Kind));
}

const TCHAR* FMantlePlaceAssets::KindName(const EAssetKind Kind)
{
	switch (Kind)
	{
	case EAssetKind::Material: return TEXT("material");
	case EAssetKind::Texture: return TEXT("texture");
	case EAssetKind::Sound: return TEXT("sound");
	case EAssetKind::Heightfield: return TEXT("heightfield");
	// #596: an uploaded texture read for its weights; ApplyResolved holds it to the API's "texture".
	case EAssetKind::TerrainWeights: return TEXT("weightmap");
	// The API's kinds (#469): what a resolved asset's kind must be.
	case EAssetKind::SkeletalMesh: return TEXT("skeletalmesh");
	case EAssetKind::AnimClip: return TEXT("animclip");
	case EAssetKind::Vehicle: return TEXT("vehicle");
	case EAssetKind::Weapon: return TEXT("weapon");
	case EAssetKind::Mesh:
	default: return TEXT("mesh");
	}
}

// ---- Lifetime -----------------------------------------------------------------------------------------

FMantlePlaceAssets::FMantlePlaceAssets(const FMantlePlaceAssetSettings& InSettings, FMantlePlaceAssetsDone InOnDone)
	: Settings(InSettings)
	, Store(InSettings.Store.IsValid() ? InSettings.Store.ToSharedRef() : FMantleAssetStore::Get().AsShared())
	, Resolver(InSettings.Resolver.IsValid() ? InSettings.Resolver.ToSharedRef() : CreateApiResolver(IsRunningDedicatedServer()))
	, Completions(MakeShared<FCompletionQueue, ESPMode::ThreadSafe>())
	, OnDone(MoveTemp(InOnDone))
{
	MantlePlaceAssetsPrivate::GLiveSets.Add(this);
}

FMantlePlaceAssets::~FMantlePlaceAssets()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
	MantlePlaceAssetsPrivate::GLiveSets.Remove(this);
	for (TPair<FBuildKey, FBuild>& Pair : Builds)
	{
		FBuild& Build = Pair.Value;
		if (Build.RequestId != 0)
		{
			Store->Cancel(Build.RequestId);
			Build.RequestId = 0;
		}
		if (Build.bPinned)
		{
			Store->Unpin(Build.Key.Hash);
			Build.bPinned = false;
		}
	}
}

void FMantlePlaceAssets::AddReferencedObjects(FReferenceCollector& Collector)
{
	for (TPair<FBuildKey, FBuild>& Pair : Builds)
	{
		if (Pair.Value.Object)
		{
			Collector.AddReferencedObject(Pair.Value.Object);
		}
	}
}

// ---- Start --------------------------------------------------------------------------------------------

void FMantlePlaceAssets::Start(const FMantlePlace& Place)
{
	using namespace MantlePlaceAssetsPrivate;

	StartSeconds = FPlatformTime::Seconds();
	NextProgressLogAt = StartSeconds + Settings.ProgressLogSeconds;

	TSet<FString> SkippedMaterials;
	TSet<FString> SkippedSounds;
	TSet<FString> SkippedSkeletal;
	int32 SkippedSlots = 0;
	TSet<FString> SkippedTextures;
	for (const FMantleEntity& Entity : Place.Entities)
	{
		// A SkeletalMesh's mesh, materials and clip (#469): nothing is drawn on a dedicated server.
		if (const FMantleSkeletalMeshProps* Skeletal = Entity.Props.TryGet<FMantleSkeletalMeshProps>())
		{
			const TPair<const FString*, EAssetKind> Parts[] = {
				{ &Skeletal->Mesh, EAssetKind::SkeletalMesh },
				{ &Skeletal->Clip, EAssetKind::AnimClip },
			};
			for (const TPair<const FString*, EAssetKind>& Part : Parts)
			{
				if (!IsUploadedId(*Part.Key))
				{
					continue;
				}
				if (Settings.bRenderAssets)
				{
					EnsureRecord(*Part.Key, Part.Value);
				}
				else
				{
					SkippedSkeletal.Add(*Part.Key);
				}
			}
			for (const FString& Material : Skeletal->Materials)
			{
				if (!IsUploadedId(Material))
				{
					continue;
				}
				if (Settings.bRenderAssets)
				{
					EnsureRecord(Material, EAssetKind::Material);
				}
				else
				{
					SkippedMaterials.Add(Material);
					++SkippedSlots;
				}
			}
			continue;
		}

		// A VehicleSpawn's vehicle (#435): built on every machine, a dedicated server for its physics.
		if (const FMantleVehicleSpawnProps* Spawn = Entity.Props.TryGet<FMantleVehicleSpawnProps>())
		{
			if (IsUploadedId(Spawn->Vehicle))
			{
				EnsureRecord(Spawn->Vehicle, EAssetKind::Vehicle);
			}
			continue;
		}

		// A WeaponSpawn's weapon (#789): read on every machine, a dedicated server for the definition it grants.
		if (const FMantleWeaponSpawnProps* WeaponSpawn = Entity.Props.TryGet<FMantleWeaponSpawnProps>())
		{
			if (IsUploadedId(WeaponSpawn->Weapon))
			{
				EnsureRecord(WeaponSpawn->Weapon, EAssetKind::Weapon);
			}
			continue;
		}

		// A SoundEmitter's uploaded sound (#324): nothing is heard on a dedicated server.
		if (const FMantleSoundEmitterProps* Emitter = Entity.Props.TryGet<FMantleSoundEmitterProps>())
		{
			if (IsUploadedSoundId(Emitter->Sound))
			{
				if (Settings.bRenderAssets)
				{
					EnsureRecord(Emitter->Sound, EAssetKind::Sound);
				}
				else
				{
					SkippedSounds.Add(Emitter->Sound);
				}
			}
			continue;
		}

		// A Decal's textures (#472), each in the role the decal reads it with. Nothing renders on a
		// dedicated server, which keeps the decal entity and loads neither.
		if (const FMantleDecalProps* Decal = Entity.Props.TryGet<FMantleDecalProps>())
		{
			for (const EMantleTextureRole Role : { EMantleTextureRole::Color, EMantleTextureRole::Normal })
			{
				const FString& TextureId = Role == EMantleTextureRole::Normal ? Decal->Normal : Decal->BaseColor;
				if (!IsUploadedId(TextureId))
				{
					continue;
				}
				if (Settings.bRenderAssets)
				{
					EnsureRecord(TextureId, EAssetKind::Texture, Role);
				}
				else
				{
					SkippedTextures.Add(TextureId);
				}
			}
			continue;
		}

		// A sprites Effect's texture (#628), read as a decal's base colour is: nothing renders on a dedicated server.
		if (const FMantleEffectProps* Effect = Entity.Props.TryGet<FMantleEffectProps>())
		{
			if (IsUploadedId(Effect->TextureId))
			{
				if (Settings.bRenderAssets)
				{
					EnsureRecord(Effect->TextureId, EAssetKind::Texture, EMantleTextureRole::Color);
				}
				else
				{
					SkippedTextures.Add(Effect->TextureId);
				}
			}
			continue;
		}

		// A Scatter's meshes and materials (#530): drawn only, so a dedicated server, which builds no
		// scatter, takes none of them. No collision is built for its meshes.
		if (const FMantleScatterParams* Scatter = Entity.Props.TryGet<FMantleScatterParams>())
		{
			if (!Settings.bRenderAssets)
			{
				for (const FMantleScatterMesh& Mesh : Scatter->Meshes)
				{
					for (const FString& Material : Mesh.Materials)
					{
						if (IsUploadedId(Material))
						{
							SkippedMaterials.Add(Material);
							++SkippedSlots;
						}
					}
				}
				continue;
			}
			for (const FMantleScatterMesh& Mesh : Scatter->Meshes)
			{
				if (IsUploadedId(Mesh.MeshId))
				{
					EnsureRecord(Mesh.MeshId, EAssetKind::Mesh);
				}
				for (const FString& Material : Mesh.Materials)
				{
					if (IsUploadedId(Material))
					{
						EnsureRecord(Material, EAssetKind::Material);
					}
				}
			}
			continue;
		}

		// A Terrain (#466): its heightfield everywhere (a dedicated server builds the terrain's
		// collision from it); its weightmaps (#628: the second holds layers 4 to 7), read as linear data,
		// and its layers' materials only where something renders.
		if (const FMantleTerrainProps* Terrain = Entity.Props.TryGet<FMantleTerrainProps>())
		{
			if (IsUploadedId(Terrain->Heightfield))
			{
				EnsureRecord(Terrain->Heightfield, EAssetKind::Heightfield);
			}
			if (IsUploadedId(Terrain->Weightmap) && Settings.bRenderAssets)
			{
				EnsureRecord(Terrain->Weightmap, EAssetKind::Texture, EMantleTextureRole::Data);
			}
			if (IsUploadedId(Terrain->Weightmap2) && Settings.bRenderAssets)
			{
				EnsureRecord(Terrain->Weightmap2, EAssetKind::Texture, EMantleTextureRole::Data);
			}
			// #596: the same weightmap's heaviest layers, on every net mode (a server resolves bullet
			// hits too), when a layer names a surface for them to pick between.
			if (IsUploadedId(Terrain->Weightmap) && UMantleTerrainSurfaceComponent::NeedsWeights(*Terrain))
			{
				EnsureRecord(Terrain->Weightmap, EAssetKind::TerrainWeights);
				// Layers 4 to 7 (#628): weighed against the first map's, so a hit on one of them names its surface.
				if (IsUploadedId(Terrain->Weightmap2))
				{
					EnsureRecord(Terrain->Weightmap2, EAssetKind::TerrainWeights);
				}
			}
			for (const FMantleTerrainLayer& Layer : Terrain->Layers)
			{
				if (!IsUploadedId(Layer.Material))
				{
					continue;
				}
				if (Settings.bRenderAssets)
				{
					EnsureRecord(Layer.Material, EAssetKind::Material);
				}
				else
				{
					SkippedMaterials.Add(Layer.Material);
					++SkippedSlots;
				}
			}
			// Its grass (#628): meshes and materials like a Scatter's, no collision, and each grass map read on
			// the CPU with its pixels kept. Drawn only, so a dedicated server takes none of them.
			for (const FMantleTerrainGrass& Grass : Terrain->Grass)
			{
				if (!Settings.bRenderAssets)
				{
					for (const FString& Material : Grass.Materials)
					{
						if (IsUploadedId(Material))
						{
							SkippedMaterials.Add(Material);
							++SkippedSlots;
						}
					}
					continue;
				}
				if (IsUploadedId(Grass.Map))
				{
					EnsureRecord(Grass.Map, EAssetKind::TerrainWeights).bKeepPixels = true;
				}
				if (IsUploadedId(Grass.Mesh))
				{
					EnsureRecord(Grass.Mesh, EAssetKind::Mesh);
				}
				for (const FString& Material : Grass.Materials)
				{
					if (IsUploadedId(Material))
					{
						EnsureRecord(Material, EAssetKind::Material);
					}
				}
			}
			continue;
		}

		// A custom WaterBody's surface mesh (#997), on every machine: its collision is the water's.
		if (const FMantleWaterBodyProps* Water = Entity.Props.TryGet<FMantleWaterBodyProps>())
		{
			if (IsUploadedId(Water->MeshId))
			{
				FRecord& Record = EnsureRecord(Water->MeshId, EAssetKind::Mesh);
				Record.bCollision = Record.bCollision || Water->bCollision;
			}
			continue;
		}

		// A StaticMesh's, or an InstancedMesh's (#467), mesh and materials.
		const FMantleStaticMeshProps* Props = MantleMeshPropsOf(Entity);
		if (!Props)
		{
			continue;
		}
		if (IsUploadedId(Props->Mesh))
		{
			FRecord& Record = EnsureRecord(Props->Mesh, EAssetKind::Mesh);
			Record.bCollision = Record.bCollision || Props->Collision != EMantleCollision::None;
		}
		for (const FString& Material : Props->Materials)
		{
			if (!IsUploadedId(Material))
			{
				continue;
			}
			if (Settings.bRenderAssets)
			{
				EnsureRecord(Material, EAssetKind::Material);
			}
			else
			{
				SkippedMaterials.Add(Material);
				++SkippedSlots;
			}
		}
	}

	int32 ManifestCount = 0;
	int64 ManifestBytes = 0;
	for (const FMantleAssetRef& Asset : Place.Assets)
	{
		if (IsUploadedId(Asset.Id))
		{
			++ManifestCount;
			ManifestBytes += FMath::Max<int64>(Asset.Bytes, 0);

			// A sound the place publishes is one its scripts may play (Sounds:Play, #459), whether or
			// not an emitter uses it: the same record an emitter's sound gets, so it is fetched once.
			if (Asset.Kind == EMantleAssetKind::Sound)
			{
				if (Settings.bRenderAssets)
				{
					EnsureRecord(Asset.Id, EAssetKind::Sound);
				}
				else
				{
					SkippedSounds.Add(Asset.Id);
				}
			}

			// Every vehicle the place lists comes with it (#435): a script may spawn one, and a
			// client must be able to build any vehicle the server replicates to it.
			if (Asset.Kind == EMantleAssetKind::Vehicle)
			{
				EnsureRecord(Asset.Id, EAssetKind::Vehicle);
			}

			// Every weapon the place lists comes with it (#789): a client must be able to build any weapon the server hands out.
			if (Asset.Kind == EMantleAssetKind::Weapon)
			{
				EnsureRecord(Asset.Id, EAssetKind::Weapon);
			}
		}
	}

	// Every vehicle the place's storage resolved (#614 stage 5), parked by a VehicleSpawn or not: a script spawns a
	// stored vehicle by its item id, so the server must have built it (physics) and a client must be able to build
	// the car the server replicates. Like the manifest's vehicles above, these are not held to the manifest-only cap.
	for (const FString& StoredVehicleId : FMantleStorage::StoredVehicleAssetIds(Place))
	{
		EnsureRecord(StoredVehicleId, EAssetKind::Vehicle);
	}

	// A stored weapon (#614 stage 5) is available to Backpack:Grant even when the place has no WeaponSpawn.
	for (const FString& StoredWeaponId : FMantleStorage::StoredWeaponAssetIds(Place))
	{
		EnsureRecord(StoredWeaponId, EAssetKind::Weapon);
	}

	// #556 item 5: the images the place's UI documents show, colour textures with straight alpha
	// as the document host draws them (MantleRml::ResolveAssetTexture). Only the uiDocuments entry
	// names them, so nothing above takes them; a dedicated server shows no UI and takes none.
	for (const FMantleUIDocument& Document : Place.UIDocuments)
	{
		if (!Document.IsImage() || !IsUploadedId(Document.Asset))
		{
			continue;
		}
		if (Settings.bRenderAssets)
		{
			EnsureRecord(Document.Asset, EAssetKind::Texture, EMantleTextureRole::Color);
		}
		else
		{
			SkippedTextures.Add(Document.Asset);
		}
	}

	int32 MeshCount = 0;
	int32 MaterialCount = 0;
	int32 SoundCount = 0;
	int32 TextureCount = 0;
	int32 SkeletalCount = 0;
	int32 HeightfieldCount = 0;
	int32 VehicleCount = 0;
	int32 WeaponCount = 0;
	for (const TPair<FString, FRecord>& Pair : Records)
	{
		const EAssetKind Kind = Pair.Value.Kind;
		(Kind == EAssetKind::Mesh ? MeshCount : Kind == EAssetKind::Material ? MaterialCount : Kind == EAssetKind::Sound ? SoundCount
			: (Kind == EAssetKind::SkeletalMesh || Kind == EAssetKind::AnimClip) ? SkeletalCount : Kind == EAssetKind::Heightfield ? HeightfieldCount
			: Kind == EAssetKind::Vehicle ? VehicleCount : Kind == EAssetKind::Weapon ? WeaponCount : TextureCount) += 1;
	}
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_PLACE_ASSETS preflight place %s: %d uploaded assets in the manifest (about %.1f MB as uploaded); entities use %d uploaded meshes, %d uploaded materials, %d uploaded decal textures and %d uploaded skeletal meshes and clips; %d uploaded sounds (emitters' and the manifest's)"),
		*Place.PlaceId, ManifestCount, Megabytes(ManifestBytes), MeshCount, MaterialCount, TextureCount, SkeletalCount, SoundCount);
	if (HeightfieldCount > 0)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS preflight place %s: terrain uses %d uploaded heightfield(s)"), *Place.PlaceId, HeightfieldCount);
	}
	if (VehicleCount > 0)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS preflight place %s: %d uploaded vehicle(s) (#435)"), *Place.PlaceId, VehicleCount);
	}
	if (WeaponCount > 0)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS preflight place %s: %d uploaded weapon(s) (#789)"), *Place.PlaceId, WeaponCount);
	}
	if (!Settings.bRenderAssets)
	{
		Result.SkippedMaterials = SkippedMaterials.Num();
		Result.SkippedSounds = SkippedSounds.Num();
		Result.SkippedSkeletal = SkippedSkeletal.Num();
		UE_LOG(LogMantle, Log,
			TEXT("MANTLE_PLACE_ASSETS skip %d uploaded materials (%d slots) and their textures, %d decal textures, %d uploaded sounds and %d uploaded skeletal meshes and clips on the dedicated server: nothing renders or is heard here, meshes are downloaded for collision"),
			SkippedMaterials.Num(), SkippedSlots, SkippedTextures.Num(), SkippedSounds.Num(), SkippedSkeletal.Num());
	}
	bStarted = true;
}

void FMantlePlaceAssets::StartManifestOnly(const FMantlePlace& Place)
{
	using namespace MantlePlaceAssetsPrivate;

	StartSeconds = FPlatformTime::Seconds();
	NextProgressLogAt = StartSeconds + Settings.ProgressLogSeconds;

	// The entities' own assets, and their vehicles' parts and materials, are the place set's (Prepare);
	// this one takes only what nothing placed uses, in manifest order, until either cap is reached.
	// Taking one again here would also shadow the place set's built object: the library asks the newest
	// set that knows an id, and this one is newer and still loading. Textures come with the materials
	// that name them, as in Prepare, and sounds are not built by this loader at all.
	const TSet<FString> Used = UploadedIdsPlaceSetTakes(Place, AddLiveWeaponDependencies);
	TSet<FString> Seen;
	int32 Taken = 0;
	int64 TakenBytes = 0;
	int32 OverCap = 0;
	int32 SkippedMaterials = 0;
	for (const FMantleAssetRef& Asset : Place.Assets)
	{
		const bool bMesh = Asset.Kind == EMantleAssetKind::StaticMesh;
		const bool bMaterial = Asset.Kind == EMantleAssetKind::Material;
		// A place clip no SkeletalMesh plays yet, for Entity:PlayClip (script API 1.18); drawn only, like a material.
		const bool bClip = Asset.Kind == EMantleAssetKind::AnimClip;
		if ((!bMesh && !bMaterial && !bClip) || !IsUploadedId(Asset.Id) || Used.Contains(Asset.Id) || Seen.Contains(Asset.Id))
		{
			continue;
		}
		Seen.Add(Asset.Id);
		if (bClip && !Settings.bRenderAssets)
		{
			continue;
		}
		if (bMaterial && !Settings.bRenderAssets)
		{
			++SkippedMaterials;
			continue;
		}
		const int64 Bytes = FMath::Max<int64>(Asset.Bytes, 0);
		if (Taken >= Settings.MaxManifestOnlyAssets || TakenBytes + Bytes > Settings.MaxManifestOnlyBytes)
		{
			++OverCap;
			UE_LOG(LogMantle, Warning,
				TEXT("MANTLE_PLACE_ASSETS skip manifest-only %s %s: over the cap of %d assets / %.1f MB a place may have downloaded for scripts"),
				bMesh ? TEXT("mesh") : bClip ? TEXT("clip") : TEXT("material"), *Asset.Id, Settings.MaxManifestOnlyAssets,
				Megabytes(Settings.MaxManifestOnlyBytes));
			continue;
		}
		++Taken;
		TakenBytes += Bytes;
		FRecord& Record = EnsureRecord(Asset.Id, bMesh ? EAssetKind::Mesh : bClip ? EAssetKind::AnimClip : EAssetKind::Material);
		// A script may spawn the mesh with collision on (the default), so it is built with it.
		Record.bCollision = Record.bCollision || bMesh;
	}

	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_PLACE_ASSETS manifest-only place %s: %d uploaded assets no entity or vehicle uses (about %.1f MB as uploaded), %d skipped over the cap, %d materials skipped on the dedicated server"),
		*Place.PlaceId, Taken, Megabytes(TakenBytes), OverCap, SkippedMaterials);
	Result.SkippedMaterials = SkippedMaterials;
	bStarted = true;
}

FMantlePlaceAssets::FRecord& FMantlePlaceAssets::EnsureRecord(const FString& Id, const EAssetKind Kind, const EMantleTextureRole Role)
{
	const FString Key = RecordKey(Id, Kind, Role);
	if (FRecord* Existing = Records.Find(Key))
	{
		return *Existing;
	}
	FRecord& Record = Records.Add(Key);
	Record.Id = Id;
	Record.Kind = Kind;
	Record.Role = Role;
	return Record;
}

void FMantlePlaceAssets::FailRecord(FRecord& Record, const FString& Error)
{
	Record.bFailed = true;
	Record.bResolving = false;
	Record.Error = Error;
	UE_LOG(LogMantle, Error, TEXT("MANTLE_PLACE_ASSETS FAIL %s %s: %s"), KindName(Record.Kind), *Record.Id, *Error);
}

// ---- Resolve ------------------------------------------------------------------------------------------

void FMantlePlaceAssets::FlushResolves()
{
	const double Now = FPlatformTime::Seconds();
	while (ResolvesInFlight < Settings.MaxConcurrentResolves)
	{
		TArray<FString> Ids;
		TSet<FString> Asked;
		for (TPair<FString, FRecord>& Pair : Records)
		{
			const FRecord& Record = Pair.Value;
			if (Record.bResolved || Record.bResolving || Record.bFailed || Record.NextResolveAt > Now || Asked.Contains(Record.Id))
			{
				continue;
			}
			if (Ids.Num() >= FMantleApiClient::MaxIdsPerResolve)
			{
				break;
			}
			Asked.Add(Record.Id);
			Ids.Add(Record.Id);
		}
		if (Ids.IsEmpty())
		{
			return;
		}
		for (TPair<FString, FRecord>& Pair : Records)
		{
			FRecord& Record = Pair.Value;
			if (!Record.bResolved && !Record.bResolving && !Record.bFailed && Asked.Contains(Record.Id))
			{
				Record.bResolving = true;
				++Record.ResolveAttempts;
			}
		}
		++ResolvesInFlight;
		const TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Queue = Completions;
		Resolver->Resolve(Ids, [Queue, Ids](const FMantleAssetResolveResult& Resolved)
		{
			Queue->Push([Ids, Resolved](FMantlePlaceAssets& Self) { Self.OnResolved(Ids, Resolved); });
		});
	}
}

void FMantlePlaceAssets::OnResolved(const TArray<FString>& Ids, const FMantleAssetResolveResult& Resolved)
{
	--ResolvesInFlight;
	const double Now = FPlatformTime::Seconds();

	TMap<FString, const FMantleUploadedAsset*> ById;
	for (const FMantleUploadedAsset& Asset : Resolved.Assets)
	{
		ById.Add(Asset.Id, &Asset);
		// Answers not asked for (a material's textures) are kept for the records they will become.
		ResolvedCache.Add(Asset.Id, Asset);
	}
	const TSet<FString> Missing(Resolved.Missing);
	const TSet<FString> Asked(Ids);

	// Keys first: applying an answer may add texture records, which must not happen mid-iteration.
	TArray<FString> Keys;
	for (const TPair<FString, FRecord>& Pair : Records)
	{
		if (Pair.Value.bResolving && Asked.Contains(Pair.Value.Id))
		{
			Keys.Add(Pair.Key);
		}
	}
	for (const FString& Key : Keys)
	{
		FRecord* Record = Records.Find(Key);
		if (!Record)
		{
			continue;
		}
		Record->bResolving = false;
		if (const FMantleUploadedAsset* const* Asset = ById.Find(Record->Id))
		{
			ApplyResolved(*Record, **Asset);
		}
		else if (Missing.Contains(Record->Id))
		{
			FailRecord(*Record, TEXT("does not exist (it may have been taken down)"));
		}
		else
		{
			FString Why = !Resolved.bSuccess ? Resolved.Error : Resolved.Failed.FindRef(Record->Id);
			if (Why.IsEmpty())
			{
				Why = TEXT("the API did not answer for it");
			}
			if (Record->ResolveAttempts >= Settings.MaxResolveAttempts)
			{
				FailRecord(*Record, FString::Printf(TEXT("could not be resolved: %s"), *Why));
			}
			else
			{
				Record->NextResolveAt = Now + static_cast<double>(Record->ResolveAttempts);
				UE_LOG(LogMantle, Verbose, TEXT("MANTLE_PLACE_ASSETS retrying %s %s: %s"), KindName(Record->Kind), *Record->Id, *Why);
			}
		}
	}
}

void FMantlePlaceAssets::ApplyResolved(FRecord& Record, const FMantleUploadedAsset& Asset)
{
	// A weapon's attachment (#789) may be a mesh or a skeletal mesh; the API says which. This record was made as a mesh: swap
	// it for a skeletal mesh one and let that one take the answer. Nothing touches Record after, which the swap removes.
	if (Record.Kind == EAssetKind::Mesh && Record.bMeshOrSkeletal && Asset.Kind.Equals(KindName(EAssetKind::SkeletalMesh), ESearchCase::CaseSensitive))
	{
		const FString SwappedId = Record.Id;
		const FMantleUploadedAsset Answer = Asset;
		Records.Remove(RecordKey(SwappedId, EAssetKind::Mesh, EMantleTextureRole::Color));
		ApplyResolved(EnsureRecord(SwappedId, EAssetKind::SkeletalMesh), Answer);
		return;
	}
	const TCHAR* Expected = Record.Kind == EAssetKind::TerrainWeights ? KindName(EAssetKind::Texture) : KindName(Record.Kind);
	if (!Asset.Kind.Equals(Expected, ESearchCase::CaseSensitive))
	{
		FailRecord(Record, FString::Printf(TEXT("is a %s, not a %s"), *Asset.Kind, Expected));
		return;
	}
	if (Asset.IsRejected())
	{
		FailRecord(Record, Asset.CookError.IsEmpty() ? FString(TEXT("was rejected")) : FString::Printf(TEXT("was rejected: %s"), *Asset.CookError));
		return;
	}
	if (!Asset.IsApproved())
	{
		FailRecord(Record, FString::Printf(TEXT("is not approved (status %s)"), *Asset.Status));
		return;
	}
	if (Asset.Url.IsEmpty())
	{
		FailRecord(Record, TEXT("has no download URL"));
		return;
	}
	FMantleContentHash Hash;
	FString HashError;
	if (!FMantleContentHash::Parse(Asset.CookedHash, Hash, &HashError))
	{
		FailRecord(Record, FString::Printf(TEXT("has no cooked hash to verify its download against (%s)"), *HashError));
		return;
	}

	FBuildKey Key;
	Key.Hash = Hash;
	Key.Kind = Record.Kind;
	Key.Variant = static_cast<uint8>(Record.Kind == EAssetKind::Mesh ? (Record.bCollision ? 1 : 0)
		: Record.Kind == EAssetKind::Texture ? static_cast<int32>(Record.Role)
		: Record.Kind == EAssetKind::TerrainWeights ? (Record.bKeepPixels ? 1 : 0) : 0);
	Record.bResolved = true;
	Record.Build = Key;
	const FString RecordId = Record.Id;
	const EAssetKind RecordKind = Record.Kind;
	UE_LOG(LogMantle, Verbose, TEXT("MANTLE_PLACE_ASSETS resolved %s %s %s (%lld bytes)"), KindName(RecordKind), *RecordId, *Asset.CookedHash, Asset.CookedBytes);

	if (RecordKind == EAssetKind::Material && Asset.MaterialTextures.Num() > 0)
	{
		// Start on the textures now, from cookInfo; the document confirms them once it is read.
		PendingMaterialTextures.Emplace(RecordId, Asset.MaterialTextures);
	}

	if (FBuild* Existing = Builds.Find(Key))
	{
		Existing->RecordIds.AddUnique(RecordId);
		if (Existing->State == EBuildState::Queued)
		{
			Existing->Ref.Url = Asset.Url;
		}
		return;
	}

	FBuild& Build = Builds.Add(Key);
	Build.Key = Key;
	Build.Id = RecordId;
	Build.RecordIds.Add(RecordId);
	Build.Ref.Hash = Hash;
	Build.Ref.Url = Asset.Url;
	Build.Ref.Bytes = FMath::Max<int64>(Asset.CookedBytes, 0);
	Store->Pin(Hash);
	Build.bPinned = true;

	// An object an earlier set built from the same bytes, still alive: nothing to download or build.
	// A sound builds no object (its emitters make their own waves), nor does a clip (its decoded keys
	// are the build, #469) or a heightfield (its decoded grid is the build, #466), or terrain weights
	// (#596, likewise), so none has anything to reuse.
	if (RecordKind != EAssetKind::Material && RecordKind != EAssetKind::Sound && RecordKind != EAssetKind::AnimClip
		&& RecordKind != EAssetKind::Heightfield && RecordKind != EAssetKind::TerrainWeights
		// A weapon is read again, not reused: queuing what its document names happens when it is decoded (#789).
		&& RecordKind != EAssetKind::Weapon)
	{
		const FString ObjectKey = FString::Printf(TEXT("%s:%d:%d"), *Hash.ToHex(), static_cast<int32>(Key.Kind), static_cast<int32>(Key.Variant));
		if (const TWeakObjectPtr<UObject>* Alive = MantlePlaceAssetsPrivate::GBuiltObjects.Find(ObjectKey); Alive && Alive->IsValid())
		{
			// A reused vehicle skips OnDecoded, where its materials and liveries are queued: queue them from the built
			// vehicle's document instead, at the next Pump (adding records here would move the one being applied).
			if (const UMantleBuiltVehicle* Reused = RecordKind == EAssetKind::Vehicle ? Cast<UMantleBuiltVehicle>(Alive->Get()) : nullptr)
			{
				FPendingVehicleLooks& Looks = PendingVehicleLooks.AddDefaulted_GetRef();
				Looks.VehicleId = RecordId;
				Looks.MaterialIds = Reused->Desc.MaterialIds();
				Looks.LiveryIds = Reused->Desc.Liveries;
				Looks.MeshIds = Reused->Desc.PartMeshIds();
				Looks.SoundIds = Reused->Desc.SoundIds();
			}
			Build.bReused = true;
			Build.Detail = TEXT("reused from an earlier load");
			MarkReady(Build, Alive->Get());
			return;
		}
	}
	(RecordKind == EAssetKind::Sound ? SoundFetchQueue : FetchQueue).Add(Key);
}

void FMantlePlaceAssets::QueueTexturesOf(const FString& MaterialId, const TMap<FString, FString>& SlotTextures)
{
	for (const TPair<FString, FString>& Pair : SlotTextures)
	{
		EMantlePlaceTextureSlot Slot = EMantlePlaceTextureSlot::BaseColor;
		if (!MantlePlaceAssetsPrivate::SlotFromName(Pair.Key, Slot) || !IsUploadedId(Pair.Value))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_ASSETS material %s names texture '%s' in slot '%s', which this build does not know; ignored"),
				*MaterialId, *Pair.Value, *Pair.Key);
			continue;
		}
		FRecord& Record = EnsureRecord(Pair.Value, EAssetKind::Texture, FMantlePlaceMaterials::RoleForSlot(Slot));
		if (!Record.bResolved && !Record.bResolving && !Record.bFailed)
		{
			if (const FMantleUploadedAsset* Cached = ResolvedCache.Find(Record.Id))
			{
				const FMantleUploadedAsset Asset = *Cached;
				ApplyResolved(Record, Asset);
			}
		}
	}
}

void FMantlePlaceAssets::QueueLiveriesOf(const FString& VehicleId, const TArray<FString>& TextureIds)
{
	if (TextureIds.IsEmpty())
	{
		return;
	}
	if (!Settings.bRenderAssets)
	{
		// A dedicated server draws nothing: it replicates a livery's index, never its pixels.
		return;
	}
	for (const FString& TextureId : TextureIds)
	{
		if (!IsUploadedId(TextureId))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_ASSETS vehicle %s names livery '%s', which is not an uploaded id; ignored"),
				*VehicleId, *TextureId);
			continue;
		}
		// A livery is colour with alpha as coverage (limits.json placeAssets.livery.role), read as a base colour is.
		FRecord& Record = EnsureRecord(TextureId, EAssetKind::Texture, EMantleTextureRole::Color);
		if (!Record.bResolved && !Record.bResolving && !Record.bFailed)
		{
			if (const FMantleUploadedAsset* Cached = ResolvedCache.Find(Record.Id))
			{
				const FMantleUploadedAsset Asset = *Cached;
				ApplyResolved(Record, Asset);
			}
		}
	}
}

void FMantlePlaceAssets::QueueMeshesOf(const FString& VehicleId, const TArray<FString>& MeshIds)
{
	if (MeshIds.IsEmpty() || !Settings.bRenderAssets)
	{
		// A dedicated server draws nothing: a part neither collides nor weighs anything, so it needs none of them.
		return;
	}
	for (const FString& MeshId : MeshIds)
	{
		if (!IsUploadedId(MeshId))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_ASSETS vehicle %s names part mesh '%s', which is not an uploaded id; ignored"),
				*VehicleId, *MeshId);
			continue;
		}
		// Its collision stays as the place asks for it (none, unless an entity uses the same mesh with collision).
		FRecord& Record = EnsureRecord(MeshId, EAssetKind::Mesh);
		if (!Record.bResolved && !Record.bResolving && !Record.bFailed)
		{
			if (const FMantleUploadedAsset* Cached = ResolvedCache.Find(Record.Id))
			{
				const FMantleUploadedAsset Asset = *Cached;
				ApplyResolved(Record, Asset);
			}
		}
	}
}

void FMantlePlaceAssets::QueueSoundsOf(const FString& VehicleId, const TArray<FString>& SoundIds)
{
	if (SoundIds.IsEmpty() || !Settings.bRenderAssets)
	{
		// A dedicated server hears nothing: its cars' horns sound only on the clients.
		Result.SkippedSounds += SoundIds.Num();
		return;
	}
	for (const FString& SoundId : SoundIds)
	{
		if (!IsUploadedId(SoundId))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_ASSETS vehicle %s names sound '%s', which is not an uploaded id; ignored"),
				*VehicleId, *SoundId);
			continue;
		}
		// The record an emitter's or the manifest's use of the same sound gets, so it is fetched once, after everything the
		// place waits for (it never holds the place up).
		FRecord& Record = EnsureRecord(SoundId, EAssetKind::Sound);
		if (!Record.bResolved && !Record.bResolving && !Record.bFailed)
		{
			if (const FMantleUploadedAsset* Cached = ResolvedCache.Find(Record.Id))
			{
				const FMantleUploadedAsset Asset = *Cached;
				ApplyResolved(Record, Asset);
			}
		}
	}
}

void FMantlePlaceAssets::QueueMaterialsOf(const FString& VehicleId, const TArray<FString>& MaterialIds)
{
	if (!Settings.bRenderAssets)
	{
		// A dedicated server draws nothing: the vehicle's body and physics are all it builds.
		Result.SkippedMaterials += MaterialIds.Num();
		return;
	}
	for (const FString& MaterialId : MaterialIds)
	{
		if (!IsUploadedId(MaterialId))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_ASSETS vehicle %s names material '%s', which is not an uploaded id; ignored"),
				*VehicleId, *MaterialId);
			continue;
		}
		FRecord& Record = EnsureRecord(MaterialId, EAssetKind::Material);
		if (!Record.bResolved && !Record.bResolving && !Record.bFailed)
		{
			if (const FMantleUploadedAsset* Cached = ResolvedCache.Find(Record.Id))
			{
				const FMantleUploadedAsset Asset = *Cached;
				ApplyResolved(Record, Asset);
			}
		}
	}
}

void FMantlePlaceAssets::QueueWeaponPartsOf(const FString& WeaponId, const FMantleWeaponAsset& Named)
{
	// The weapon's mesh and attachments are needed everywhere: the definition is built from them on a dedicated server too.
	// An attachment is a mesh or a skeletal mesh, whichever the API answers (ApplyResolved swaps the record).
	const auto QueuePart = [this, &WeaponId](const FString& Id, const EAssetKind Kind, const bool bMeshOrSkeletal)
	{
		if (!IsUploadedId(Id))
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_ASSETS weapon %s names '%s', which is not an uploaded id; ignored"), *WeaponId, *Id);
			return;
		}
		FRecord& Record = EnsureRecord(Id, Kind);
		if (!Record.bResolved && !Record.bResolving && !Record.bFailed)
		{
			Record.bMeshOrSkeletal = Record.bMeshOrSkeletal || bMeshOrSkeletal;
			if (const FMantleUploadedAsset* Cached = ResolvedCache.Find(Record.Id))
			{
				const FMantleUploadedAsset Asset = *Cached;
				ApplyResolved(Record, Asset);
			}
		}
	};
	QueuePart(Named.Mesh, EAssetKind::SkeletalMesh, false);
	for (const FString& Attachment : Named.Attachments)
	{
		// A mesh already prepared for the place, or a skeletal mesh already known, stays what it is.
		if (FindRecord(Attachment, EAssetKind::SkeletalMesh) || FindRecord(Attachment, EAssetKind::Mesh))
		{
			continue;
		}
		QueuePart(Attachment, EAssetKind::Mesh, true);
	}
	TArray<FString> MaterialIds;
	for (const FString& Material : Named.Materials)
	{
		if (!Material.IsEmpty())
		{
			MaterialIds.AddUnique(Material);
		}
	}
	// #898: the attachments' own materials load beside the weapon's.
	for (const TPair<FString, TArray<FString>>& Attachment : Named.AttachmentMaterials)
	{
		for (const FString& Material : Attachment.Value)
		{
			if (!Material.IsEmpty())
			{
				MaterialIds.AddUnique(Material);
			}
		}
	}
	QueueMaterialsOf(WeaponId, MaterialIds);
	QueueSoundsOf(WeaponId, Named.Sounds);
	// The hotbar icon is a colour texture, as a livery is; a dedicated server draws no hotbar and fetches none.
	if (!Named.Icon.IsEmpty())
	{
		QueueLiveriesOf(WeaponId, { Named.Icon });
	}
}

// ---- Fetch --------------------------------------------------------------------------------------------

void FMantlePlaceAssets::StartFetches()
{
	auto StartFrom = [this](TArray<FBuildKey>& Queue)
	{
		int32 Next = 0;
		while (Next < Queue.Num() && FetchesInFlight < Settings.MaxOutstandingFetches && PendingBuildBytes < Settings.MaxPendingBuildBytes)
		{
			const FBuildKey Key = Queue[Next++];
			FBuild* Build = Builds.Find(Key);
			if (!Build || Build->State != EBuildState::Queued)
			{
				continue;
			}
			Build->State = EBuildState::Fetching;
			++FetchesInFlight;
			const TWeakPtr<FMantlePlaceAssets> Weak = AsShared();
			Build->RequestId = Store->Request(Build->Ref, EMantleAssetLoad::Bytes, FMantleAssetDone::CreateLambda(
				[Weak, Key](const FMantleAssetResult& Fetched)
				{
					if (const TSharedPtr<FMantlePlaceAssets> Pinned = Weak.Pin())
					{
						Pinned->OnFetched(Key, Fetched);
					}
				}));
		}
		Queue.RemoveAt(0, Next, EAllowShrinking::No);
	};
	StartFrom(FetchQueue);
	// Sounds only once everything the place waits for has started (#324): audio never delays a join.
	if (FetchQueue.IsEmpty())
	{
		StartFrom(SoundFetchQueue);
	}
}

void FMantlePlaceAssets::OnFetched(const FBuildKey& Key, const FMantleAssetResult& Fetched)
{
	--FetchesInFlight;
	FBuild* Build = Builds.Find(Key);
	if (!Build || Build->State != EBuildState::Fetching)
	{
		return;
	}
	Build->RequestId = 0;
	if (!Fetched.bSuccess)
	{
		// A 403 or 404 from the CDN usually means the signed URL expired: resolve once more for a fresh one.
		if (Fetched.StatusCode == 403 || Fetched.StatusCode == 404)
		{
			bool bAnyRetried = false;
			for (TPair<FString, FRecord>& Pair : Records)
			{
				if (Pair.Value.Build.IsSet() && Pair.Value.Build.GetValue() == Key && Pair.Value.bReresolved)
				{
					bAnyRetried = true;
				}
			}
			if (!bAnyRetried)
			{
				for (TPair<FString, FRecord>& Pair : Records)
				{
					FRecord& Record = Pair.Value;
					if (Record.Build.IsSet() && Record.Build.GetValue() == Key)
					{
						Record.Build.Reset();
						Record.bResolved = false;
						Record.bReresolved = true;
						Record.NextResolveAt = 0.0;
					}
				}
				if (Build->bPinned)
				{
					Store->Unpin(Key.Hash);
				}
				UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS %s %s answered %d; resolving it again for a fresh URL"),
					KindName(Key.Kind), *Build->Id, Fetched.StatusCode);
				Builds.Remove(Key);
				return;
			}
		}
		FailBuild(*Build, FString::Printf(TEXT("could not be downloaded: %s"), *Fetched.Error));
		return;
	}
	Build->bFromCache = Fetched.bFromCache;
	if (!Fetched.bFromCache)
	{
		DownloadedBytes += Fetched.SizeBytes;
	}
	Build->Bytes = Fetched.Bytes;
	Build->PendingBytes = Fetched.SizeBytes;
	PendingBuildBytes += Build->PendingBytes;
	Build->State = EBuildState::Fetched;
	DecodeQueue.Add(Key);
}

// ---- Decode -------------------------------------------------------------------------------------------

void FMantlePlaceAssets::StartDecodes()
{
	using namespace MantlePlaceAssetsPrivate;

	int32 Next = 0;
	while (Next < DecodeQueue.Num() && DecodesInFlight < Settings.MaxConcurrentDecodes)
	{
		const FBuildKey Key = DecodeQueue[Next++];
		FBuild* Build = Builds.Find(Key);
		if (!Build || Build->State != EBuildState::Fetched)
		{
			continue;
		}
		Build->State = EBuildState::Decoding;
		++DecodesInFlight;

		const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes = MoveTemp(Build->Bytes);
		Build->Bytes.Reset();
		const int64 MaxTextureGpuBytes = Settings.MaxTextureGpuBytes;
		const TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Queue = Completions;
		UE::Tasks::Launch(TEXT("MantlePlaceAssetDecode"), [Queue, Bytes, Key, MaxTextureGpuBytes]()
		{
			TSharedPtr<FMantleStaticMeshData> MeshV2;
			TSharedPtr<FMantleRuntimeMesh> MeshV1;
			TSharedPtr<FMantleDecodedTexture> Texture;
			TSharedPtr<FMantlePlaceMaterialDesc> Material;
			TSharedPtr<const FMantleSoundData> Sound;
			TSharedPtr<const FMantleHeightfield> Heightfield;
			TSharedPtr<const FMantleTerrainWeights> TerrainWeights;
			TSharedPtr<FMantleSkinnedMesh> Skinned;
			TSharedPtr<const FMantleAnimClip> Clip;
			TSharedPtr<FMantleVehicleAsset> Vehicle;
			TSharedPtr<FMantleWeaponAsset> Weapon;
			FString Error;
			const TConstArrayView<uint8> View = Bytes.IsValid() ? TConstArrayView<uint8>(*Bytes) : TConstArrayView<uint8>();

			switch (Key.Kind)
			{
			case EAssetKind::Mesh:
				if (FMantleStaticMeshReader::HasMagic(View))
				{
					MeshV2 = MakeShared<FMantleStaticMeshData>();
					if (!FMantleStaticMeshReader::Read(View, *MeshV2, &Error))
					{
						MeshV2.Reset();
					}
				}
				else if (View.Num() >= 4 && FMemory::Memcmp(View.GetData(), "MRM1", 4) == 0 && Bytes.IsValid())
				{
					// Meshes cooked before MRM2 (F2): one section, the old axis convention.
					FMantleRuntimeMeshResult Old = FMantleRuntimeMeshReader::Read(*Bytes);
					if (Old.bSuccess)
					{
						MeshV1 = MakeShared<FMantleRuntimeMesh>(MoveTemp(Old.Mesh));
					}
					else
					{
						Error = Old.Error;
					}
				}
				else
				{
					Error = TEXT("is not a cooked Mantle mesh (MRM2 or MRM1)");
				}
				break;

			case EAssetKind::Material:
				Material = MakeShared<FMantlePlaceMaterialDesc>();
				if (!FMantlePlaceMaterials::ParseMaterial(View, *Material, &Error))
				{
					Material.Reset();
				}
				break;

			case EAssetKind::Texture:
			{
				FMantleTextureBuildParams Params;
				Params.Role = static_cast<EMantleTextureRole>(Key.Variant);
				Params.MaxGpuBytes = MaxTextureGpuBytes;
				Texture = MakeShared<FMantleDecodedTexture>();
				if (!FMantleRuntimeTextureBuilder::Decode(View, Params, *Texture, &Error)
					|| !FMantlePlaceMaterials::PrepareForSlot(SlotForRole(Params.Role), *Texture, &Error))
				{
					Texture.Reset();
				}
				break;
			}

			case EAssetKind::Sound:
			{
				// MAU1 (#324): the whole decode is a checked copy of the samples.
				const TSharedPtr<FMantleSoundData> Decoded = MakeShared<FMantleSoundData>();
				if (FMantleSoundReader::Read(View, *Decoded, &Error))
				{
					Sound = Decoded;
				}
				break;
			}

			case EAssetKind::SkeletalMesh:
			{
				// MSK1 role 3 (#469): a place skeletal mesh carrying its own skeleton. Avatar content is refused.
				FMantleSkinnedMeshReadResult Read = FMantleSkinnedMeshReader::Read(View);
				if (!Read.bSuccess)
				{
					Error = Read.Error;
				}
				else if (Read.Header.Role != EMantleSkinnedMeshRole::PlaceMesh)
				{
					Error = TEXT("is avatar content, not a place skeletal mesh");
				}
				else
				{
					Skinned = MakeShared<FMantleSkinnedMesh>(MoveTemp(Read.Mesh));
				}
				break;
			}

			case EAssetKind::AnimClip:
			{
				// MAN1 with its skeleton block (#469). An emote (no skeleton of its own) is refused.
				FMantleAnimClipReadResult Read = FMantleAnimClipReader::Read(View);
				if (!Read.bSuccess)
				{
					Error = Read.Error;
				}
				else if (!Read.Clip.HasSkeleton())
				{
					Error = TEXT("is an emote, not a place animation clip (it names no skeleton of its own)");
				}
				else
				{
					Clip = MakeShared<FMantleAnimClip>(MoveTemp(Read.Clip));
				}
				break;
			}

			case EAssetKind::Heightfield:
			{
				// MHF1 (#466): a checked copy of the heights; the place builder makes the meshes.
				const TSharedPtr<FMantleHeightfield> Decoded = MakeShared<FMantleHeightfield>();
				if (FMantleHeightfieldReader::Read(View, *Decoded, &Error))
				{
					Heightfield = Decoded;
				}
				break;
			}

			case EAssetKind::TerrainWeights:
			{
				// #596: the weightmap's top level on the CPU (no mips, nothing uploaded), reduced to the
				// heaviest layer per texel; the pixels go when this task ends, unless it is a grass map
				// (#628, variant 1), whose four channels are what the grass is sampled from.
				const bool bKeepPixels = Key.Variant == 1;
				FMantleTextureBuildParams Params;
				Params.Role = EMantleTextureRole::Data;
				Params.Mips = EMantleTextureMips::None;
				Params.MaxGpuBytes = MaxTextureGpuBytes;
				FMantleDecodedTexture Decoded;
				const TSharedPtr<FMantleTerrainWeights> Weights = MakeShared<FMantleTerrainWeights>();
				if (FMantleRuntimeTextureBuilder::Decode(View, Params, Decoded, &Error)
					&& FMantleTerrainWeights::FromDecodedTexture(Decoded, *Weights, &Error, bKeepPixels))
				{
					TerrainWeights = Weights;
				}
				break;
			}

			case EAssetKind::Vehicle:
			{
				// MVH1 (#435): the document and the body, read strictly; the game thread builds the mesh.
				const TSharedPtr<FMantleVehicleAsset> Read = MakeShared<FMantleVehicleAsset>();
				if (FMantleVehicleAssetReader::Read(View, *Read, Error))
				{
					Vehicle = Read;
				}
				break;
			}

			case EAssetKind::Weapon:
			{
				// MWP1 (#789): the weapon document, read strictly; the game thread wraps it.
				const TSharedPtr<FMantleWeaponAsset> Read = MakeShared<FMantleWeaponAsset>();
				if (FMantleWeaponAsset::Read(View, *Read, Error))
				{
					Weapon = Read;
				}
				break;
			}
			}

			Queue->Push([Key, MeshV2, MeshV1, Texture, Material, Sound, Skinned, Clip, Heightfield, TerrainWeights, Vehicle, Weapon, Error](FMantlePlaceAssets& Self)
			{
				Self.OnDecoded(Key, MeshV2, MeshV1, Texture, Material, Sound, Skinned, Clip, Heightfield, TerrainWeights, Vehicle, Weapon, Error);
			});
		});
	}
	DecodeQueue.RemoveAt(0, Next, EAllowShrinking::No);
}

void FMantlePlaceAssets::OnDecoded(const FBuildKey& Key, TSharedPtr<FMantleStaticMeshData> MeshV2, TSharedPtr<FMantleRuntimeMesh> MeshV1,
	TSharedPtr<FMantleDecodedTexture> Texture, TSharedPtr<FMantlePlaceMaterialDesc> Material, TSharedPtr<const FMantleSoundData> Sound,
	TSharedPtr<FMantleSkinnedMesh> Skinned, TSharedPtr<const FMantleAnimClip> Clip, TSharedPtr<const FMantleHeightfield> Heightfield,
	TSharedPtr<const FMantleTerrainWeights> TerrainWeights, TSharedPtr<FMantleVehicleAsset> Vehicle, TSharedPtr<FMantleWeaponAsset> Weapon, const FString& Error)
{
	--DecodesInFlight;
	FBuild* Build = Builds.Find(Key);
	if (!Build || Build->State != EBuildState::Decoding)
	{
		return;
	}
	if (!Error.IsEmpty()
		|| (!MeshV2 && !MeshV1 && !Texture && !Material && !Sound && !Skinned && !Clip && !Heightfield && !TerrainWeights && !Vehicle && !Weapon))
	{
		FailBuild(*Build, FString::Printf(TEXT("could not be read: %s"), Error.IsEmpty() ? TEXT("unknown error") : *Error));
		return;
	}

	// What the game thread now holds is the decoded form, not the download.
	ReleasePending(*Build);
	Build->PendingBytes = MeshV2 ? MeshV2->GetAllocatedBytes() : Texture ? Texture->GetGpuBytes() : Sound ? Sound->GetAllocatedBytes()
		: Skinned ? MantlePlaceAssetsPrivate::SkinnedMeshBytes(*Skinned) : Heightfield ? Heightfield->GetAllocatedBytes()
		: TerrainWeights ? static_cast<int64>(TerrainWeights->GetAllocatedBytes())
		: Vehicle && Vehicle->Body.LODs.Num() > 0 ? static_cast<int64>(Vehicle->Body.LODs[0].NumVertices()) * 64 : 0;
	PendingBuildBytes += Build->PendingBytes;
	Build->MeshV2 = MeshV2;
	Build->MeshV1 = MeshV1;
	Build->Texture = Texture;
	Build->Material = Material;
	Build->Sound = Sound;
	Build->Skinned = Skinned;
	Build->Clip = Clip;
	Build->Heightfield = Heightfield;
	Build->TerrainWeights = TerrainWeights;
	Build->Vehicle = Vehicle;
	Build->Weapon = Weapon;

	if (Key.Kind == EAssetKind::Weapon)
	{
		// The document names the weapon's mesh, attachments, sounds and materials: they load beside it, as a vehicle's do.
		// Queued after this build is last touched: a cached answer adds builds, which may move it.
		if (!IsUploadedId(Weapon->Mesh))
		{
			FailBuild(*Build, FString::Printf(TEXT("names mesh '%s', which is not an uploaded skeletal mesh id"), *Weapon->Mesh));
			return;
		}
		const FMantleWeaponAsset Named = *Weapon;
		const FString WeaponId = Build->Id;
		QueueBuild(Key);
		QueueWeaponPartsOf(WeaponId, Named);
		return;
	}
	if (Key.Kind == EAssetKind::Vehicle)
	{
		// The vehicle builds now; its materials (#435), its parts' meshes (#435 item 2) and its horn's sound (#546) load beside
		// it, as a material's textures do. Queued after this build is last touched: a cached answer adds builds, which may move it.
		const TArray<FString> MaterialIds = Vehicle->Desc.MaterialIds();
		const TArray<FString> LiveryIds = Vehicle->Desc.Liveries;
		const TArray<FString> MeshIds = Vehicle->Desc.PartMeshIds();
		const TArray<FString> SoundIds = Vehicle->Desc.SoundIds();
		const FString VehicleId = Build->Id;
		QueueBuild(Key);
		QueueMaterialsOf(VehicleId, MaterialIds);
		QueueLiveriesOf(VehicleId, LiveryIds);
		QueueMeshesOf(VehicleId, MeshIds);
		QueueSoundsOf(VehicleId, SoundIds);
		return;
	}
	if (Key.Kind != EAssetKind::Material)
	{
		QueueBuild(Key);
		return;
	}

	// The document is the truth about the material's textures: one record per slot (a layer blend's
	// included, #533), read with the slot's role. Their builds are added after this build is last
	// touched (adding may move it).
	TMap<FString, FString> SlotTextures;
	for (int32 Slot = 0; Slot < MantlePlaceMaterialTextureCount; ++Slot)
	{
		const FString& TextureId = Material->Textures[Slot];
		if (TextureId.IsEmpty())
		{
			continue;
		}
		const EMantlePlaceTextureSlot SlotEnum = static_cast<EMantlePlaceTextureSlot>(Slot);
		Build->TextureRecords[Slot] = RecordKey(TextureId, EAssetKind::Texture, FMantlePlaceMaterials::RoleForSlot(SlotEnum));
		SlotTextures.Add(FMantlePlaceMaterials::SlotName(SlotEnum), TextureId);
	}
	Build->State = EBuildState::WaitingForTextures;
	WaitingMaterials.Add(Key);
	const FString MaterialId = Build->Id;
	QueueTexturesOf(MaterialId, SlotTextures);
}

// ---- Build --------------------------------------------------------------------------------------------

void FMantlePlaceAssets::QueueBuild(const FBuildKey& Key)
{
	if (FBuild* Build = Builds.Find(Key))
	{
		Build->State = EBuildState::Building;
		BuildQueue.Add(Key);
	}
}

void FMantlePlaceAssets::PromoteMaterials()
{
	for (int32 Index = WaitingMaterials.Num() - 1; Index >= 0; --Index)
	{
		const FBuildKey Key = WaitingMaterials[Index];
		const FBuild* Build = Builds.Find(Key);
		if (!Build || Build->State != EBuildState::WaitingForTextures)
		{
			WaitingMaterials.RemoveAt(Index);
			continue;
		}
		bool bTexturesSettled = true;
		for (const FString& TextureRecord : Build->TextureRecords)
		{
			if (TextureRecord.IsEmpty())
			{
				continue;
			}
			const FRecord* Record = Records.Find(TextureRecord);
			if (!Record || Record->bFailed)
			{
				continue;
			}
			const FBuild* TextureBuild = Record->Build.IsSet() ? Builds.Find(Record->Build.GetValue()) : nullptr;
			if (!TextureBuild || !TextureBuild->IsTerminal())
			{
				bTexturesSettled = false;
				break;
			}
		}
		if (bTexturesSettled)
		{
			WaitingMaterials.RemoveAt(Index);
			QueueBuild(Key);
		}
	}
}

void FMantlePlaceAssets::RunBuilds()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceAssets_RunBuilds);
	const double Deadline = FPlatformTime::Seconds() + Settings.BuildSliceMs / 1000.0;
	int32 Next = 0;
	bool bFirst = true;
	while (Next < BuildQueue.Num() && (bFirst || FPlatformTime::Seconds() < Deadline))
	{
		const FBuildKey Key = BuildQueue[Next++];
		FBuild* Build = Builds.Find(Key);
		if (!Build || Build->State != EBuildState::Building)
		{
			continue;
		}
		bFirst = false;
		// The game-thread half of an uploaded asset (#126): the downloads and decodes ran off it.
		const double BuildStart = FPlatformTime::Seconds();
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceAssets_BuildOne);
			MANTLE_TRACE_SCOPE_DETAIL("MantlePlaceAssetBuild", FString::Printf(TEXT("%s %s"), KindName(Key.Kind), *Build->Id));
			BuildOne(*Build);
		}
		const double Elapsed = FPlatformTime::Seconds() - BuildStart;
		++GameThreadBuilds;
		GameThreadBuildSeconds += Elapsed;
		if (Elapsed > LongestBuildSeconds)
		{
			LongestBuildSeconds = Elapsed;
			// Builds is not added to while BuildOne runs, but look again rather than trust the pointer.
			const FBuild* Built = Builds.Find(Key);
			LongestBuildId = Built ? FString::Printf(TEXT("%s %s"), KindName(Key.Kind), *Built->Id) : FString(KindName(Key.Kind));
		}
	}
	BuildQueue.RemoveAt(0, Next, EAllowShrinking::No);
}

void FMantlePlaceAssets::BuildOne(FBuild& Build)
{
	FString Error;
	switch (Build.Key.Kind)
	{
	case EAssetKind::Mesh:
	{
		const bool bCollision = (Build.Key.Variant & 1) != 0;
		FMantleStaticMeshBuildParams Params;
		Params.bComplexCollision = bCollision;
		Params.bCookCollisionNow = !Settings.bAsyncCollisionCook;
		// #470: a dedicated server never draws, and its collision is LOD0's.
		Params.bBuildLowerLODs = !IsRunningDedicatedServer();
		Params.Name = TEXT("MantlePlaceMesh");
		UStaticMesh* Mesh = nullptr;
		if (Build.MeshV2)
		{
			const FMantleStaticMeshCollision& Simple = Build.MeshV2->Collision;
			const FString CollisionDetail = !bCollision ? FString(TEXT("none"))
				: !Simple.HasShapes() ? FString(TEXT("complex"))
				: FString::Printf(TEXT("simple%s (%d boxes, %d spheres, %d capsules, %d convex hulls)"), Simple.bSimpleAsComplex ? TEXT(" as complex") : TEXT(""),
					Simple.Boxes.Num(), Simple.Spheres.Num(), Simple.Capsules.Num(), Simple.Hulls.Num());
			TArray<FString> LODTriangles;
			for (int32 LOD = 0; LOD < Build.MeshV2->LODCount(); ++LOD)
			{
				LODTriangles.Add(FString::FromInt(Build.MeshV2->GetLOD(LOD).TriangleCount()));
			}
			Build.Detail = FString::Printf(TEXT("MRM2, %d vertices, %d triangles, %d sections, %d LOD(s) (%s triangles%s)%s, collision %s"),
				Build.MeshV2->VertexCount(), Build.MeshV2->TriangleCount(), Build.MeshV2->SlotCount(), Build.MeshV2->LODCount(),
				*FString::Join(LODTriangles, TEXT("/")), Params.bBuildLowerLODs || Build.MeshV2->LODCount() == 1 ? TEXT("") : TEXT(", LOD0 built on a server"),
				Build.MeshV2->UV1s.Num() > 0 ? TEXT(", UV1") : TEXT(""), *CollisionDetail);
			Mesh = FMantleRuntimeStaticMeshBuilder::Build(*Build.MeshV2, Params, &Error);
		}
		else if (Build.MeshV1)
		{
			Build.Detail = FString::Printf(TEXT("MRM1, %d vertices, %d triangles, 1 section, collision %s"),
				Build.MeshV1->VertexCount(), Build.MeshV1->TriangleCount(), bCollision ? TEXT("complex") : TEXT("none"));
			Mesh = FMantleRuntimeStaticMeshBuilder::Build(*Build.MeshV1, Params, &Error);
		}
		Build.MeshV2.Reset();
		Build.MeshV1.Reset();
		ReleasePending(Build);
		if (!Mesh)
		{
			FailBuild(Build, FString::Printf(TEXT("could not be built: %s"), *Error));
			return;
		}
		Build.Object = Mesh;
		UBodySetup* Body = Mesh->GetBodySetup();
		if (bCollision && Settings.bAsyncCollisionCook && Body)
		{
			Build.State = EBuildState::CookingCollision;
			const TWeakPtr<FMantlePlaceAssets> Weak = AsShared();
			const FBuildKey Key = Build.Key;
			// May finish at once (inside this call) when there is nothing to cook; Build is not touched after it.
			Body->CreatePhysicsMeshesAsync(FOnAsyncPhysicsCookFinished::CreateLambda([Weak, Key](const bool bSucceeded)
			{
				if (const TSharedPtr<FMantlePlaceAssets> Pinned = Weak.Pin())
				{
					Pinned->OnCollisionCooked(Key, bSucceeded);
				}
			}));
			return;
		}
		if (bCollision)
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS collision ready mesh %s (%s, cooked in the build)"),
				*Build.Id, *MantlePlaceAssetsPrivate::DescribeCookedCollision(Body));
		}
		MarkReady(Build, Mesh);
		return;
	}

	case EAssetKind::Texture:
	{
		if (!Build.Texture)
		{
			FailBuild(Build, TEXT("has nothing decoded to build"));
			return;
		}
		FMantleTextureBuildParams Params;
		Params.Role = static_cast<EMantleTextureRole>(Build.Key.Variant);
		Params.MaxGpuBytes = Settings.MaxTextureGpuBytes;
		Build.Detail = FString::Printf(TEXT("%s, %dx%d %s, %d mips"), MantlePlaceAssetsPrivate::RoleName(Params.Role),
			Build.Texture->GetWidth(), Build.Texture->GetHeight(), GetPixelFormatString(Build.Texture->Format), Build.Texture->Mips.Num());
		UTexture2D* Texture = FMantleRuntimeTextureBuilder::Build(MoveTemp(*Build.Texture), Params, &Error);
		Build.Texture.Reset();
		ReleasePending(Build);
		if (!Texture)
		{
			FailBuild(Build, FString::Printf(TEXT("could not be built: %s"), *Error));
			return;
		}
		MarkReady(Build, Texture);
		return;
	}

	case EAssetKind::Material:
	{
		if (!Build.Material)
		{
			FailBuild(Build, TEXT("has nothing decoded to build"));
			return;
		}
		UTexture* Textures[MantlePlaceMaterialTextureCount] = {};
		int32 Bound = 0;
		for (int32 Slot = 0; Slot < MantlePlaceMaterialTextureCount; ++Slot)
		{
			const FRecord* Record = Build.TextureRecords[Slot].IsEmpty() ? nullptr : Records.Find(Build.TextureRecords[Slot]);
			const FBuild* TextureBuild = Record && Record->Build.IsSet() ? Builds.Find(Record->Build.GetValue()) : nullptr;
			if (TextureBuild && TextureBuild->State == EBuildState::Ready)
			{
				Textures[Slot] = Cast<UTexture>(TextureBuild->Object.Get());
				Bound += Textures[Slot] ? 1 : 0;
			}
		}
		bool bFellBack = false;
		UMaterialInterface* Parent = FMantlePlaceMaterials::LoadPermutation(*Build.Material, &bFellBack);
		UMaterialInstanceDynamic* Instance = FMantlePlaceMaterials::BuildOnParent(Parent, *Build.Material, Textures, nullptr, NAME_None, &Error);
		Build.Detail = FString::Printf(TEXT("%s, %d textures%s"), *FMantlePlaceMaterials::PermutationName(*Build.Material),
			Bound, bFellBack ? TEXT(", on a fallback material (its permutation is missing)") : TEXT(""));
		Build.Material.Reset();
		ReleasePending(Build);
		if (!Instance)
		{
			FailBuild(Build, FString::Printf(TEXT("could not be built: %s"), *Error));
			return;
		}
		MarkReady(Build, Instance);
		return;
	}

	case EAssetKind::Sound:
	{
		if (!Build.Sound)
		{
			FailBuild(Build, TEXT("has nothing decoded to build"));
			return;
		}
		// Nothing to build here (#324): each emitter plays the shared samples through its own
		// UMantleSoundWave. The decoded samples are the build, and stay held while the set lives.
		Build.Detail = FString::Printf(TEXT("MAU1, %d Hz, %d channel(s), %.2f s, %.1f MB decoded"), Build.Sound->SampleRate,
			Build.Sound->Channels, Build.Sound->DurationSeconds(), MantlePlaceAssetsPrivate::Megabytes(Build.Sound->GetAllocatedBytes()));
		ReleasePending(Build);
		MarkReady(Build, nullptr);
		return;
	}

	case EAssetKind::SkeletalMesh:
	{
		if (!Build.Skinned)
		{
			FailBuild(Build, TEXT("has nothing decoded to build"));
			return;
		}
		// #469: on a transient skeleton of its own bones; nothing reads the vertices back, so the GPU
		// copy is the only one kept.
		FMantleSkinnedMeshBuildParams Params;
		Params.bOwnSkeleton = true;
		Params.bKeepCPUData = false;
		Params.Name = TEXT("MantlePlaceSkeletalMesh");
		const FMantleSkinnedMeshLOD& LOD0 = Build.Skinned->LODs[0];
		Build.Detail = FString::Printf(TEXT("MSK1, %d bones, %d vertices, %d triangles, %d sections"),
			Build.Skinned->Bones.Num(), LOD0.NumVertices(), LOD0.NumTriangles(), LOD0.Sections.Num());
		USkeletalMesh* Mesh = FMantleRuntimeSkeletalMeshBuilder::Build(*Build.Skinned, Params, &Error);
		Build.Skinned.Reset();
		ReleasePending(Build);
		if (!Mesh)
		{
			FailBuild(Build, FString::Printf(TEXT("could not be built: %s"), *Error));
			return;
		}
		MarkReady(Build, Mesh);
		return;
	}

	case EAssetKind::AnimClip:
	{
		if (!Build.Clip)
		{
			FailBuild(Build, TEXT("has nothing decoded to build"));
			return;
		}
		// Nothing to build (#469): each SkeletalMesh samples the shared decoded keys.
		Build.Detail = FString::Printf(TEXT("MAN1, %d frames at %.0f fps (%.2f s), %d tracks, skeleton %s"), Build.Clip->FrameCount,
			Build.Clip->FrameRate, Build.Clip->GetDuration(), Build.Clip->Tracks.Num(), *Build.Clip->SkeletonHash);
		ReleasePending(Build);
		MarkReady(Build, nullptr);
		return;
	}

	case EAssetKind::Heightfield:
	{
		if (!Build.Heightfield)
		{
			FailBuild(Build, TEXT("has nothing decoded to build"));
			return;
		}
		// Nothing to build here (#466): the place builder turns the heights into the terrain's chunk
		// meshes, with collision, as it spawns the Terrain entity. The decoded grid stays held.
		Build.Detail = FString::Printf(TEXT("MHF1, %d x %d samples, %g x %g cm apart, %.1f MB decoded"), Build.Heightfield->Width,
			Build.Heightfield->Height, Build.Heightfield->SpacingX, Build.Heightfield->SpacingY,
			MantlePlaceAssetsPrivate::Megabytes(Build.Heightfield->GetAllocatedBytes()));
		ReleasePending(Build);
		MarkReady(Build, nullptr);
		return;
	}

	case EAssetKind::TerrainWeights:
	{
		if (!Build.TerrainWeights)
		{
			FailBuild(Build, TEXT("has nothing decoded to build"));
			return;
		}
		// Nothing to build here either (#596): the heaviest layer per texel is the build, and the place
		// builder hands it to the terrain's UMantleTerrainSurfaceComponent. It stays held.
		Build.Detail = FString::Printf(TEXT("terrain weights, %d x %d texels, %.1f MB"), Build.TerrainWeights->Width,
			Build.TerrainWeights->Height, MantlePlaceAssetsPrivate::Megabytes(Build.TerrainWeights->GetAllocatedBytes()));
		ReleasePending(Build);
		MarkReady(Build, nullptr);
		return;
	}

	case EAssetKind::Vehicle:
	{
		if (!Build.Vehicle)
		{
			FailBuild(Build, TEXT("has nothing decoded to build"));
			return;
		}
		// The body on a skeleton of its own (#435); MantleGameplay makes the drivable definition from it.
		UMantleBuiltVehicle* Vehicle = UMantleBuiltVehicle::Build(*Build.Vehicle, GetTransientPackage(), &Error);
		const FMantleVehicleAsset& Read = *Build.Vehicle;
		Build.Detail = FString::Printf(TEXT("MVH1, %d wheels, %d bones, %d triangles, %.0f kg"), Read.Desc.Wheels.Num(), Read.Body.Bones.Num(),
			Read.Body.LODs.Num() > 0 ? Read.Body.LODs[0].NumTriangles() : 0, Read.Desc.Mass);
		Build.Vehicle.Reset();
		ReleasePending(Build);
		if (!Vehicle)
		{
			FailBuild(Build, FString::Printf(TEXT("could not be built: %s"), *Error));
			return;
		}
		MarkReady(Build, Vehicle);
		return;
	}

	case EAssetKind::Weapon:
	{
		if (!Build.Weapon)
		{
			FailBuild(Build, TEXT("has nothing decoded to build"));
			return;
		}
		// The document, kept (#789): MantleGameplay builds the weapon definition from it and what it names.
		UMantleBuiltWeapon* Weapon = NewObject<UMantleBuiltWeapon>(GetTransientPackage());
		Weapon->Asset = *Build.Weapon;
		Build.Detail = FString::Printf(TEXT("MWP1, \"%s\", mesh %s, %d attachment(s), %d sound(s), %d material slot(s)"), *Weapon->Asset.Name,
			*Weapon->Asset.Mesh, Weapon->Asset.Attachments.Num(), Weapon->Asset.Sounds.Num(), Weapon->Asset.Materials.Num());
		Build.Weapon.Reset();
		ReleasePending(Build);
		MarkReady(Build, Weapon);
		return;
	}
	}
}

void FMantlePlaceAssets::OnCollisionCooked(const FBuildKey& Key, const bool bSucceeded)
{
	FBuild* Build = Builds.Find(Key);
	if (!Build || Build->State != EBuildState::CookingCollision)
	{
		return;
	}
	const UStaticMesh* Mesh = Cast<UStaticMesh>(Build->Object.Get());
	const UBodySetup* Body = Mesh ? Mesh->GetBodySetup() : nullptr;
	if (!bSucceeded || !MantlePlaceAssetsPrivate::HasCookedCollision(Body))
	{
		// The mesh still renders; it just does not block. Reported, never silent.
		const FString Why = TEXT("its collision could not be cooked: players and traces will pass through it");
		UE_LOG(LogMantle, Error, TEXT("MANTLE_PLACE_ASSETS FAIL mesh %s: %s"), *Build->Id, *Why);
		CollisionErrors.Add(FMantleParseError{ FString::Printf(TEXT("uploaded mesh %s"), *Build->Id), Why });
	}
	else
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS collision ready mesh %s (%s)"), *Build->Id, *MantlePlaceAssetsPrivate::DescribeCookedCollision(Body));
	}
	MarkReady(*Build, Build->Object.Get());
}

void FMantlePlaceAssets::MarkReady(FBuild& Build, UObject* Object)
{
	Build.Object = Object;
	Build.State = EBuildState::Ready;
	if (Build.Key.Kind != EAssetKind::Material && Object)
	{
		const FString ObjectKey = FString::Printf(TEXT("%s:%d:%d"), *Build.Key.Hash.ToHex(),
			static_cast<int32>(Build.Key.Kind), static_cast<int32>(Build.Key.Variant));
		TMap<FString, TWeakObjectPtr<UObject>>& Built = MantlePlaceAssetsPrivate::GBuiltObjects;
		if (Built.Num() >= 4096)
		{
			// Forget what has been collected since, so the map tracks what is alive, not every hash ever loaded.
			for (auto It = Built.CreateIterator(); It; ++It)
			{
				if (!It.Value().IsValid())
				{
					It.RemoveCurrent();
				}
			}
		}
		Built.Add(ObjectKey, Object);
	}
	const TCHAR* Source = Build.bReused ? TEXT("reused") : Build.bFromCache ? TEXT("from cache") : TEXT("downloaded");
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS built %s %s: %s (%s, %lld bytes, %s)"),
		KindName(Build.Key.Kind), *FString::Join(Build.RecordIds, TEXT(",")), *Build.Detail, *Build.Key.Hash.ToString(), Build.Ref.Bytes, Source);
}

void FMantlePlaceAssets::FailBuild(FBuild& Build, const FString& Error)
{
	Build.State = EBuildState::Failed;
	Build.Error = Error;
	Build.Bytes.Reset();
	Build.MeshV2.Reset();
	Build.MeshV1.Reset();
	Build.Texture.Reset();
	Build.Material.Reset();
	Build.Sound.Reset();
	Build.Skinned.Reset();
	Build.Clip.Reset();
	Build.Heightfield.Reset();
	Build.TerrainWeights.Reset();
	Build.Vehicle.Reset();
	Build.Weapon.Reset();
	ReleasePending(Build);
	UE_LOG(LogMantle, Error, TEXT("MANTLE_PLACE_ASSETS FAIL %s %s: %s"), KindName(Build.Key.Kind), *FString::Join(Build.RecordIds, TEXT(",")), *Error);
}

void FMantlePlaceAssets::ReleasePending(FBuild& Build)
{
	PendingBuildBytes -= Build.PendingBytes;
	Build.PendingBytes = 0;
}

// ---- Pump ---------------------------------------------------------------------------------------------

void FMantlePlaceAssets::DrainCompletions()
{
	TArray<FCompletion> Items;
	{
		FScopeLock Guard(&Completions->Lock);
		Items = MoveTemp(Completions->Items);
		Completions->Items.Reset();
	}
	for (FCompletion& Item : Items)
	{
		Item(*this);
	}
}

void FMantlePlaceAssets::Pump()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceAssets_Pump);
	check(IsInGameThread());
	if (!bStarted || (bCompleted && bSoundsSettled && SoundWaiters.IsEmpty()))
	{
		return;
	}
	DrainCompletions();
	if (PendingVehicleLooks.Num() > 0)
	{
		const TArray<FPendingVehicleLooks> Pending = MoveTemp(PendingVehicleLooks);
		PendingVehicleLooks.Reset();
		for (const FPendingVehicleLooks& Looks : Pending)
		{
			QueueMaterialsOf(Looks.VehicleId, Looks.MaterialIds);
			QueueLiveriesOf(Looks.VehicleId, Looks.LiveryIds);
			QueueMeshesOf(Looks.VehicleId, Looks.MeshIds);
			QueueSoundsOf(Looks.VehicleId, Looks.SoundIds);
		}
	}
	while (PendingMaterialTextures.Num() > 0)
	{
		TArray<TPair<FString, TMap<FString, FString>>> Pending = MoveTemp(PendingMaterialTextures);
		PendingMaterialTextures.Reset();
		for (const TPair<FString, TMap<FString, FString>>& Material : Pending)
		{
			QueueTexturesOf(Material.Key, Material.Value);
		}
	}
	FlushResolves();
	StartFetches();
	StartDecodes();
	PromoteMaterials();
	RunBuilds();
	CheckComplete();
	SettleSounds();
	NotifySoundWaiters();
	LogProgress();
}

bool FMantlePlaceAssets::WaitUntilComplete(const float TimeoutSeconds)
{
	// The whole wait blocks the calling thread (#126): the downloads, the decodes and every build.
	TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceAssets_WaitUntilComplete);
	const TSharedRef<FMantlePlaceAssets> KeepAlive = AsShared();
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!bCompleted || !bSoundsSettled)
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			return false;
		}
		Store->Pump();
		Pump();
		if (Settings.bAsyncCollisionCook)
		{
			// The async collision cook finishes through a game-thread task.
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		}
		FPlatformProcess::Sleep(0.001f);
	}
	return true;
}

void FMantlePlaceAssets::CheckComplete()
{
	if (bCompleted || !bStarted || ResolvesInFlight > 0 || PendingMaterialTextures.Num() > 0 || PendingVehicleLooks.Num() > 0)
	{
		return;
	}
	for (const TPair<FString, FRecord>& Pair : Records)
	{
		const FRecord& Record = Pair.Value;
		// Sounds never hold the place up (#324): SettleSounds completes them.
		if (Record.bFailed || Record.Kind == EAssetKind::Sound)
		{
			continue;
		}
		const FBuild* Build = Record.Build.IsSet() ? Builds.Find(Record.Build.GetValue()) : nullptr;
		if (!Build || !Build->IsTerminal())
		{
			return;
		}
	}

	// Every record settled: the result, one error per asset that failed.
	TSet<FString> Counted;
	for (const TPair<FString, FRecord>& Pair : Records)
	{
		const FRecord& Record = Pair.Value;
		if (Record.Kind == EAssetKind::Sound)
		{
			continue;
		}
		const FString Path = FString::Printf(TEXT("uploaded %s %s"), KindName(Record.Kind), *Record.Id);
		const FBuild* Build = Record.Build.IsSet() ? Builds.Find(Record.Build.GetValue()) : nullptr;
		if (Record.bFailed)
		{
			Result.Errors.Add(FMantleParseError{ Path, Record.Error });
			continue;
		}
		if (!Build || Build->State != EBuildState::Ready)
		{
			Result.Errors.Add(FMantleParseError{ Path, Build ? Build->Error : FString(TEXT("was never built")) });
			continue;
		}
		if (Counted.Contains(Pair.Key))
		{
			continue;
		}
		Counted.Add(Pair.Key);
		switch (Record.Kind)
		{
		case EAssetKind::Mesh: ++Result.Meshes; break;
		case EAssetKind::Material: ++Result.Materials; break;
		case EAssetKind::Texture: ++Result.Textures; break;
		case EAssetKind::Vehicle: ++Result.Vehicles; break;
		case EAssetKind::Weapon: break;
		case EAssetKind::Sound: break;
		case EAssetKind::SkeletalMesh: ++Result.SkeletalMeshes; break;
		case EAssetKind::AnimClip: ++Result.Clips; break;
		case EAssetKind::Heightfield: ++Result.Heightfields; break;
		case EAssetKind::TerrainWeights: break; // the weightmap's second use, not an asset of its own (#596)
		}
	}
	Result.Errors.Append(CollisionErrors);
	for (const TPair<FBuildKey, FBuild>& Pair : Builds)
	{
		if (Pair.Key.Kind != EAssetKind::Sound)
		{
			Result.Bytes += Pair.Value.Ref.Bytes;
		}
	}
	Result.DownloadedBytes = DownloadedBytes;
	Result.Seconds = FPlatformTime::Seconds() - StartSeconds;
	Result.bSuccess = Result.Errors.Num() == 0;
	bCompleted = true;
	// Sounds that are already in (cached, or none at all) are reported with everything else.
	SettleSounds();

	if (Result.bSuccess)
	{
		UE_LOG(LogMantle, Log,
			TEXT("MANTLE_PLACE_ASSETS READY %d meshes, %d materials, %d textures; %.1f MB (%.1f MB downloaded) in %.2f s"),
			Result.Meshes, Result.Materials, Result.Textures, MantlePlaceAssetsPrivate::Megabytes(Result.Bytes),
			MantlePlaceAssetsPrivate::Megabytes(Result.DownloadedBytes), Result.Seconds);
	}
	else
	{
		UE_LOG(LogMantle, Error,
			TEXT("MANTLE_PLACE_ASSETS READY with %d failed: %d meshes, %d materials, %d textures built; %.1f MB (%.1f MB downloaded) in %.2f s\n%s"),
			Result.Errors.Num(), Result.Meshes, Result.Materials, Result.Textures, MantlePlaceAssetsPrivate::Megabytes(Result.Bytes),
			MantlePlaceAssetsPrivate::Megabytes(Result.DownloadedBytes), Result.Seconds, *Result.ErrorSummary());
	}
	// The game-thread share of that time (#126): the builds are sliced, so this is the sum, and the
	// longest single build is the worst stall the set added to one frame.
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_PLACE_ASSETS TIMING total_ms=%.2f builds=%d build_ms=%.2f longest_build_ms=%.2f longest_build=%s"),
		Result.Seconds * 1000.0, GameThreadBuilds, GameThreadBuildSeconds * 1000.0, LongestBuildSeconds * 1000.0,
		LongestBuildId.IsEmpty() ? TEXT("-") : *LongestBuildId);
	OnDone.ExecuteIfBound(Result);
}

FMantlePlaceAssetProgress FMantlePlaceAssets::GetProgress() const
{
	FMantlePlaceAssetProgress Progress;
	for (const TPair<FString, FRecord>& Pair : Records)
	{
		const FRecord& Record = Pair.Value;
		if (Record.Kind == EAssetKind::Sound)
		{
			continue;
		}
		++Progress.TotalCount;
		const FBuild* Build = Record.Build.IsSet() ? Builds.Find(Record.Build.GetValue()) : nullptr;
		if (Record.bFailed || (Build && Build->State == EBuildState::Failed))
		{
			++Progress.FailedCount;
		}
		else if (Build && Build->State == EBuildState::Ready)
		{
			++Progress.ReadyCount;
		}
	}
	for (const TPair<FBuildKey, FBuild>& Pair : Builds)
	{
		const FBuild& Build = Pair.Value;
		if (Pair.Key.Kind == EAssetKind::Sound)
		{
			continue;
		}
		Progress.TotalBytes += Build.Ref.Bytes;
		if (Build.State == EBuildState::Fetching)
		{
			Progress.DoneBytes += Store->GetInFlightBytes(Build.Key.Hash);
		}
		else if (Build.State != EBuildState::Queued)
		{
			Progress.DoneBytes += Build.Ref.Bytes;
		}
	}
	return Progress;
}

void FMantlePlaceAssets::LogProgress()
{
	if (bCompleted || Settings.ProgressLogSeconds <= 0.0)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Now < NextProgressLogAt)
	{
		return;
	}
	NextProgressLogAt = Now + Settings.ProgressLogSeconds;
	const FMantlePlaceAssetProgress Progress = GetProgress();
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS progress %d of %d assets ready (%d failed), %.1f of %.1f MB"),
		Progress.ReadyCount, Progress.TotalCount, Progress.FailedCount,
		MantlePlaceAssetsPrivate::Megabytes(Progress.DoneBytes), MantlePlaceAssetsPrivate::Megabytes(Progress.TotalBytes));
}

// ---- Sounds -------------------------------------------------------------------------------------------

bool FMantlePlaceAssets::AllSoundsSettled() const
{
	for (const TPair<FString, FRecord>& Pair : Records)
	{
		const FRecord& Record = Pair.Value;
		if (Record.Kind != EAssetKind::Sound || Record.bFailed)
		{
			continue;
		}
		const FBuild* Build = Record.Build.IsSet() ? Builds.Find(Record.Build.GetValue()) : nullptr;
		if (!Build || !Build->IsTerminal())
		{
			return false;
		}
	}
	return true;
}

void FMantlePlaceAssets::SettleSounds()
{
	// After OnDone's result exists, so the sounds' part is added to it exactly once.
	if (bSoundsSettled || !bCompleted || ResolvesInFlight > 0 || !AllSoundsSettled())
	{
		return;
	}
	bSoundsSettled = true;

	int32 SoundRecords = 0;
	int32 Failed = 0;
	for (const TPair<FString, FRecord>& Pair : Records)
	{
		const FRecord& Record = Pair.Value;
		if (Record.Kind != EAssetKind::Sound)
		{
			continue;
		}
		++SoundRecords;
		const FString Path = FString::Printf(TEXT("uploaded sound %s"), *Record.Id);
		const FBuild* Build = Record.Build.IsSet() ? Builds.Find(Record.Build.GetValue()) : nullptr;
		if (Record.bFailed)
		{
			Result.Errors.Add(FMantleParseError{ Path, Record.Error });
			++Failed;
		}
		else if (!Build || Build->State != EBuildState::Ready)
		{
			Result.Errors.Add(FMantleParseError{ Path, Build ? Build->Error : FString(TEXT("was never built")) });
			++Failed;
		}
		else
		{
			++Result.Sounds;
		}
	}
	int64 SoundBytes = 0;
	for (const TPair<FBuildKey, FBuild>& Pair : Builds)
	{
		if (Pair.Key.Kind == EAssetKind::Sound)
		{
			SoundBytes += Pair.Value.Ref.Bytes;
		}
	}
	Result.Bytes += SoundBytes;
	Result.DownloadedBytes = DownloadedBytes;
	Result.bSuccess = Result.Errors.Num() == 0;
	if (SoundRecords > 0)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_ASSETS SOUNDS %d of %d uploaded sounds ready (%d failed), %.1f MB, %.2f s after the place started loading"),
			Result.Sounds, SoundRecords, Failed, MantlePlaceAssetsPrivate::Megabytes(SoundBytes), FPlatformTime::Seconds() - StartSeconds);
	}
}

void FMantlePlaceAssets::NotifySoundWaiters()
{
	if (SoundWaiters.IsEmpty())
	{
		return;
	}
	// Taken out before any is called: a callback may ask this set for another sound.
	TArray<TPair<FString, TArray<FSoundSettled>>> Settled;
	for (auto It = SoundWaiters.CreateIterator(); It; ++It)
	{
		bool bStillLoading = false;
		FString Ignored;
		FindSound(It.Key(), &Ignored, &bStillLoading);
		if (!bStillLoading)
		{
			Settled.Emplace(It.Key(), MoveTemp(It.Value()));
			It.RemoveCurrent();
		}
	}
	for (TPair<FString, TArray<FSoundSettled>>& Entry : Settled)
	{
		FString Error;
		const TSharedPtr<const FMantleSoundData> Sound = FindSound(Entry.Key, &Error);
		for (FSoundSettled& Callback : Entry.Value)
		{
			Callback(Sound, Error);
		}
	}
}

// ---- Lookups ------------------------------------------------------------------------------------------

const FMantlePlaceAssets::FRecord* FMantlePlaceAssets::FindRecord(const FString& Id, const EAssetKind Kind) const
{
	return Records.Find(RecordKey(Id, Kind, EMantleTextureRole::Color));
}

UObject* FMantlePlaceAssets::FindObject(const FString& Id, const EAssetKind Kind, FString* OutError) const
{
	return ObjectOf(FindRecord(Id, Kind), Id, Kind, OutError);
}

UObject* FMantlePlaceAssets::ObjectOf(const FRecord* Record, const FString& Id, const EAssetKind Kind, FString* OutError) const
{
	auto Fail = [OutError](const FString& Error) -> UObject*
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return nullptr;
	};
	if (!Record)
	{
		return Fail(FString::Printf(TEXT("'%s' is not among this place's uploaded %ss"), *Id, KindName(Kind)));
	}
	if (Record->bFailed)
	{
		return Fail(FString::Printf(TEXT("uploaded %s '%s' %s"), KindName(Kind), *Id, *Record->Error));
	}
	const FBuild* Build = Record->Build.IsSet() ? Builds.Find(Record->Build.GetValue()) : nullptr;
	if (!Build || Build->State == EBuildState::Failed)
	{
		return Fail(FString::Printf(TEXT("uploaded %s '%s' %s"), KindName(Kind), *Id, Build ? *Build->Error : TEXT("was never built")));
	}
	if (Build->State != EBuildState::Ready || !Build->Object)
	{
		return Fail(FString::Printf(TEXT("uploaded %s '%s' is still loading"), KindName(Kind), *Id));
	}
	return Build->Object.Get();
}

UStaticMesh* FMantlePlaceAssets::FindMesh(const FString& Id, FString* OutError) const
{
	return Cast<UStaticMesh>(FindObject(Id, EAssetKind::Mesh, OutError));
}

UMaterialInterface* FMantlePlaceAssets::FindMaterial(const FString& Id, FString* OutError) const
{
	return Cast<UMaterialInterface>(FindObject(Id, EAssetKind::Material, OutError));
}

USkeletalMesh* FMantlePlaceAssets::FindSkeletalMesh(const FString& Id, FString* OutError) const
{
	return Cast<USkeletalMesh>(FindObject(Id, EAssetKind::SkeletalMesh, OutError));
}

TSharedPtr<const FMantleAnimClip> FMantlePlaceAssets::FindClip(const FString& Id, FString* OutError, bool* bOutStillLoading) const
{
	if (bOutStillLoading)
	{
		*bOutStillLoading = false;
	}
	auto Fail = [OutError](const FString& Error) -> TSharedPtr<const FMantleAnimClip>
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return nullptr;
	};
	const FRecord* Record = FindRecord(Id, EAssetKind::AnimClip);
	if (!Record)
	{
		return Fail(FString::Printf(TEXT("'%s' is not among this place's uploaded animation clips"), *Id));
	}
	if (Record->bFailed)
	{
		return Fail(FString::Printf(TEXT("uploaded animation clip '%s' %s"), *Id, *Record->Error));
	}
	const FBuild* Build = Record->Build.IsSet() ? Builds.Find(Record->Build.GetValue()) : nullptr;
	if (!Build && !bCompleted)
	{
		// Still resolving: the set has not finished, so it may yet arrive (StateOf's rule).
		if (bOutStillLoading)
		{
			*bOutStillLoading = true;
		}
		return Fail(FString::Printf(TEXT("uploaded animation clip '%s' is still loading"), *Id));
	}
	if (!Build || Build->State == EBuildState::Failed)
	{
		return Fail(FString::Printf(TEXT("uploaded animation clip '%s' %s"), *Id, Build ? *Build->Error : TEXT("was never built")));
	}
	if (Build->State != EBuildState::Ready || !Build->Clip.IsValid())
	{
		if (bOutStillLoading && Build->State != EBuildState::Ready)
		{
			*bOutStillLoading = true;
		}
		return Fail(FString::Printf(TEXT("uploaded animation clip '%s' is still loading"), *Id));
	}
	return Build->Clip;
}

TSharedPtr<const FMantleHeightfield> FMantlePlaceAssets::FindHeightfield(const FString& Id, FString* OutError) const
{
	auto Fail = [OutError](const FString& Error) -> TSharedPtr<const FMantleHeightfield>
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return nullptr;
	};
	const FRecord* Record = FindRecord(Id, EAssetKind::Heightfield);
	if (!Record)
	{
		return Fail(FString::Printf(TEXT("'%s' is not among this place's uploaded heightfields"), *Id));
	}
	if (Record->bFailed)
	{
		return Fail(FString::Printf(TEXT("uploaded heightfield '%s' %s"), *Id, *Record->Error));
	}
	const FBuild* Build = Record->Build.IsSet() ? Builds.Find(Record->Build.GetValue()) : nullptr;
	if (!Build || Build->State == EBuildState::Failed)
	{
		return Fail(FString::Printf(TEXT("uploaded heightfield '%s' %s"), *Id, Build ? *Build->Error : TEXT("was never built")));
	}
	if (Build->State != EBuildState::Ready || !Build->Heightfield.IsValid())
	{
		return Fail(FString::Printf(TEXT("uploaded heightfield '%s' is still loading"), *Id));
	}
	return Build->Heightfield;
}

TSharedPtr<const FMantleTerrainWeights> FMantlePlaceAssets::FindTerrainWeights(const FString& Id, FString* OutError) const
{
	auto Fail = [OutError](const FString& Error) -> TSharedPtr<const FMantleTerrainWeights>
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return nullptr;
	};
	const FRecord* Record = FindRecord(Id, EAssetKind::TerrainWeights);
	if (!Record)
	{
		return Fail(FString::Printf(TEXT("'%s' is not among this place's uploaded weightmaps read for surfaces"), *Id));
	}
	if (Record->bFailed)
	{
		return Fail(FString::Printf(TEXT("uploaded weightmap '%s' %s"), *Id, *Record->Error));
	}
	const FBuild* Build = Record->Build.IsSet() ? Builds.Find(Record->Build.GetValue()) : nullptr;
	if (!Build || Build->State == EBuildState::Failed)
	{
		return Fail(FString::Printf(TEXT("uploaded weightmap '%s' %s"), *Id, Build ? *Build->Error : TEXT("was never built")));
	}
	if (Build->State != EBuildState::Ready || !Build->TerrainWeights.IsValid())
	{
		return Fail(FString::Printf(TEXT("uploaded weightmap '%s' is still loading"), *Id));
	}
	return Build->TerrainWeights;
}

UMantleBuiltVehicle* FMantlePlaceAssets::FindVehicle(const FString& Id, FString* OutError) const
{
	return Cast<UMantleBuiltVehicle>(FindObject(Id, EAssetKind::Vehicle, OutError));
}

UMantleBuiltWeapon* FMantlePlaceAssets::FindWeapon(const FString& Id, FString* OutError) const
{
	return Cast<UMantleBuiltWeapon>(FindObject(Id, EAssetKind::Weapon, OutError));
}

TSharedPtr<const FMantleSoundData> FMantlePlaceAssets::FindSound(const FString& Id, FString* OutError, bool* bOutStillLoading) const
{
	if (bOutStillLoading)
	{
		*bOutStillLoading = false;
	}
	auto Fail = [OutError](const FString& Error) -> TSharedPtr<const FMantleSoundData>
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return nullptr;
	};
	const FRecord* Record = FindRecord(Id, EAssetKind::Sound);
	if (!Record)
	{
		return Fail(FString::Printf(TEXT("'%s' is not among this place's uploaded sounds"), *Id));
	}
	if (Record->bFailed)
	{
		return Fail(FString::Printf(TEXT("uploaded sound '%s' %s"), *Id, *Record->Error));
	}
	const FBuild* Build = Record->Build.IsSet() ? Builds.Find(Record->Build.GetValue()) : nullptr;
	if (Build && Build->State == EBuildState::Failed)
	{
		return Fail(FString::Printf(TEXT("uploaded sound '%s' %s"), *Id, *Build->Error));
	}
	if (!Build || Build->State != EBuildState::Ready)
	{
		// Not resolved yet, or on its way: the builder binds it when it arrives.
		if (bOutStillLoading)
		{
			*bOutStillLoading = true;
		}
		return Fail(FString::Printf(TEXT("uploaded sound '%s' is still loading"), *Id));
	}
	if (!Build->Sound.IsValid())
	{
		return Fail(FString::Printf(TEXT("uploaded sound '%s' has no samples"), *Id));
	}
	return Build->Sound;
}
