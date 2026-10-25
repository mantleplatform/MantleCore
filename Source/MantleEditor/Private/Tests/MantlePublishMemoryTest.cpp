// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetUploadQueue.h"
#include "MantleGlbWriter.h"
#include "MantleHeadlessPublish.h"
#include "MantleMeshReduce.h"
#include "MantleMirrorBake.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlacePublishSession.h"
#include "MantlePng.h"
#include "MantlePreviewExport.h"
#include "MantlePublishCaches.h"
#include "MantlePublisher.h"
#include "MantleSha256.h"
#include "MantleSplineMeshBake.h"
#include "MantleVehicleSource.h"

#include "Animation/AnimSequence.h"
#include "Components/SplineMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IMeshReductionInterfaces.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "MeshReductionSettings.h"
#include "Misc/AutomationTest.h"
#include "Sound/SoundWave.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// A big level's publish holds each converted file once and lets it go: the set's bytes move into the upload queue instead of
// being copied, the queue drops a file's bytes once Mantle has it approved (or refused it), a finished session keeps no bytes, a
// publish reports its peak memory, a garbage collection between conversion steps cannot free what the next step converts, the bake
// caches go when the publish ends, and converted files can be kept on disk instead of in memory. Pure: a scripted Mantle and a fake
// clock, no network.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePublishMemoryTestPrivate
{
	TArray<uint8> Payload(int32 Seed, int32 Size)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Bytes[Index] = static_cast<uint8>((Seed * 37 + Index * 7 + (Index >> 8) + Seed) & 0xFF);
		}
		if (Size > 0)
		{
			Bytes[0] = static_cast<uint8>(Seed);
		}
		return Bytes;
	}

	void AddTexture(FMantlePlaceAssetSet& Set, const FString& Name, int32 Seed, int32 Size)
	{
		FMantleConvertedTexture Texture;
		Texture.Key = FString::Printf(TEXT("texture:color:%s"), *Name);
		Texture.Name = Name;
		Texture.Role = TEXT("color");
		Texture.Format = TEXT("png");
		Texture.Width = 4;
		Texture.Height = 4;
		Texture.Bytes = Payload(Seed, Size);
		Texture.Hash = FMantleSha256::AssetHash(Texture.Bytes);
		Set.AddTexture(MoveTemp(Texture));
	}

	void AddMesh(FMantlePlaceAssetSet& Set, const FString& Name, int32 Seed, int32 Size)
	{
		FMantleConvertedMesh Mesh;
		Mesh.Key = FString::Printf(TEXT("mesh:/Game/Test/%s"), *Name);
		Mesh.Name = Name;
		Mesh.Path = TEXT("/Game/Test/") + Name;
		Mesh.Bytes = Payload(Seed, Size);
		Mesh.Hash = FMantleSha256::AssetHash(Mesh.Bytes);
		Set.Meshes.Add(MoveTemp(Mesh));
	}

	/** One converted file of every kind that holds bytes, each of its own size; Original gets the bytes by key. */
	void AddOneOfEach(FMantlePlaceAssetSet& Set, TMap<FString, TArray<uint8>>& Original)
	{
		AddTexture(Set, TEXT("T_One"), 1, 900);
		AddTexture(Set, TEXT("T_Two"), 2, 1300);
		AddMesh(Set, TEXT("SM_One"), 3, 2100);
		AddMesh(Set, TEXT("SM_Two"), 4, 700);

		FMantleConvertedSound Sound;
		Sound.Key = TEXT("sound:/Game/Test/S_One");
		Sound.Name = TEXT("S_One");
		Sound.Bytes = Payload(5, 1500);
		Sound.Hash = FMantleSha256::AssetHash(Sound.Bytes);
		Set.Sounds.Add(MoveTemp(Sound));

		FMantleConvertedSkeletalMesh Skeletal;
		Skeletal.Key = TEXT("skeletalmesh:/Game/Test/SK_One");
		Skeletal.Name = TEXT("SK_One");
		Skeletal.Bytes = Payload(6, 1100);
		Skeletal.Hash = FMantleSha256::AssetHash(Skeletal.Bytes);
		Set.SkeletalMeshes.Add(MoveTemp(Skeletal));

		FMantleConvertedClip Clip;
		Clip.Key = TEXT("clip:/Game/Test/A_One");
		Clip.Name = TEXT("A_One");
		Clip.Bytes = Payload(7, 800);
		Clip.Hash = FMantleSha256::AssetHash(Clip.Bytes);
		Set.Clips.Add(MoveTemp(Clip));

		FMantleConvertedHeightfield Heightfield;
		Heightfield.Key = TEXT("heightfield:L_One");
		Heightfield.Name = TEXT("L_One");
		Heightfield.Bytes = Payload(8, 1700);
		Heightfield.Hash = FMantleSha256::AssetHash(Heightfield.Bytes);
		Set.Heightfields.Add(MoveTemp(Heightfield));

		for (const FMantleConvertedTexture& Each : Set.Textures) { Original.Add(Each.Key, Each.Bytes); }
		for (const FMantleConvertedMesh& Each : Set.Meshes) { Original.Add(Each.Key, Each.Bytes); }
		for (const FMantleConvertedSound& Each : Set.Sounds) { Original.Add(Each.Key, Each.Bytes); }
		for (const FMantleConvertedSkeletalMesh& Each : Set.SkeletalMeshes) { Original.Add(Each.Key, Each.Bytes); }
		for (const FMantleConvertedClip& Each : Set.Clips) { Original.Add(Each.Key, Each.Bytes); }
		for (const FMantleConvertedHeightfield& Each : Set.Heightfields) { Original.Add(Each.Key, Each.Bytes); }
	}

	const TArray<EMantlePlaceUploadStage>& ByteStages()
	{
		static const TArray<EMantlePlaceUploadStage> Stages = { EMantlePlaceUploadStage::Texture, EMantlePlaceUploadStage::Mesh,
			EMantlePlaceUploadStage::SkeletalMesh, EMantlePlaceUploadStage::AnimClip, EMantlePlaceUploadStage::Sound, EMantlePlaceUploadStage::Heightfield };
		return Stages;
	}

	/** Every converted file's bytes in the set. */
	int64 SetBytes(const FMantlePlaceAssetSet& Set)
	{
		int64 Total = 0;
		for (const FMantleConvertedTexture& Each : Set.Textures) { Total += Each.Bytes.Num(); }
		for (const FMantleConvertedMesh& Each : Set.Meshes) { Total += Each.Bytes.Num(); }
		for (const FMantleConvertedSound& Each : Set.Sounds) { Total += Each.Bytes.Num(); }
		for (const FMantleConvertedSkeletalMesh& Each : Set.SkeletalMeshes) { Total += Each.Bytes.Num(); }
		for (const FMantleConvertedClip& Each : Set.Clips) { Total += Each.Bytes.Num(); }
		for (const FMantleConvertedHeightfield& Each : Set.Heightfields) { Total += Each.Bytes.Num(); }
		return Total;
	}

	FString TestDirectory(const TCHAR* Leaf)
	{
		return FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantlePublishMemory"), Leaf, FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	FMantleApiResult OkResult(const FString& Body)
	{
		FMantleApiResult Result;
		Result.bSuccess = true;
		Result.StatusCode = 200;
		Result.Body = Body;
		return Result;
	}

	FMantleApiResult FailedResult(int32 Status, const FString& Code)
	{
		FMantleApiResult Result;
		Result.StatusCode = Status;
		Result.Code = Code;
		Result.Message = TEXT("scripted refusal");
		return Result;
	}

	FString AssetJson(const FString& Id, const FString& Hash, const FString& Kind, const FString& Status, bool bCreated)
	{
		const bool bApproved = Status == TEXT("approved");
		return FString::Printf(TEXT("{\"id\":\"%s\",\"hash\":\"%s\",\"kind\":\"%s\",\"status\":\"%s\",\"cookState\":\"%s\",")
			TEXT("\"moderationState\":\"%s\",\"created\":%s}"), *Id, *Hash, *Kind, *Status, bApproved ? TEXT("passed") : TEXT("processing"),
			bApproved ? TEXT("passed") : TEXT("pending"), bCreated ? TEXT("true") : TEXT("false"));
	}

	/**
	 * A scripted Mantle: stores uploads by hash, answers the diff from what it holds, and answers the place calls. Answers wait in
	 * Pending until the test flushes them. A copy of an upload's bytes is taken when it is sent, as an HTTP request does.
	 */
	class FFakeMantle : public IMantleUploadTransport
	{
	public:
		struct FPending
		{
			FMantleUploadCallRequest Request;
			TArray<uint8> Bytes;
			FMantleApiCallback Done;
		};

		TArray<FPending> Pending;
		TMap<FString, FString> IdByHash;
		TMap<FString, FString> KindByHash;
		TMap<FString, FString> StatusByHash;

		/** What a new upload is answered with unless UploadStatusByHash says otherwise. */
		FString UploadStatus = TEXT("approved");
		TMap<FString, FString> UploadStatusByHash;

		/** Uploads of these hashes are refused (422 too_large). */
		TSet<FString> RefuseHashes;

		/** The first uploads of these hashes answer 502, this many times. */
		TMap<FString, int32> FailFirstUploads;

		/** The next diff naming one of these forgets it (Mantle lost the file) and has the next upload of it answered approved. */
		TSet<FString> ForgetOnDiff;

		/** Accepted uploads by hash of the bytes received, and the size received. */
		TMap<FString, int32> UploadsByHash;
		TMap<FString, int32> ReceivedSizeByHash;
		int32 EmptyUploads = 0;
		int32 NextId = 1;
		int32 MaxUploadsInFlight = 0;
		int32 MaxLargeInFlight = 0;

		virtual void Send(const FMantleUploadCallRequest& Request, FMantleApiCallback OnDone) override
		{
			FPending& Item = Pending.AddDefaulted_GetRef();
			Item.Request = Request;
			Item.Request.Bytes = nullptr;
			if (Request.Bytes)
			{
				Item.Bytes = *Request.Bytes;
			}
			Item.Done = OnDone;
			if (Request.Call == EMantleUploadCall::UploadAsset)
			{
				// Uploads waiting for their answer are in flight; a large one is what Mantle gives a creator two slots for.
				int32 Uploads = 0;
				int32 Large = 0;
				for (const FPending& Each : Pending)
				{
					if (Each.Request.Call == EMantleUploadCall::UploadAsset)
					{
						++Uploads;
						Large += Each.Bytes.Num() >= 1024 * 1024 ? 1 : 0;
					}
				}
				MaxUploadsInFlight = FMath::Max(MaxUploadsInFlight, Uploads);
				MaxLargeInFlight = FMath::Max(MaxLargeInFlight, Large);
			}
		}

		/** Answers everything sent, and what the answers send in turn. */
		void Flush()
		{
			while (Pending.Num() > 0)
			{
				TArray<FPending> Batch = MoveTemp(Pending);
				Pending.Reset();
				for (FPending& Item : Batch)
				{
					Item.Done.ExecuteIfBound(Answer(Item));
				}
			}
		}

	private:
		FMantleApiResult Answer(const FPending& Item)
		{
			switch (Item.Request.Call)
			{
			case EMantleUploadCall::DiffAssets:
				{
					FString Known;
					FString Unknown;
					for (const FString& Hash : Item.Request.Hashes)
					{
						if (ForgetOnDiff.Contains(Hash))
						{
							ForgetOnDiff.Remove(Hash);
							IdByHash.Remove(Hash);
							UploadStatusByHash.Add(Hash, TEXT("approved"));
						}
						if (const FString* Id = IdByHash.Find(Hash))
						{
							const FString* Status = StatusByHash.Find(Hash);
							Known += FString::Printf(TEXT("%s%s"), Known.IsEmpty() ? TEXT("") : TEXT(","),
								*AssetJson(*Id, Hash, KindByHash.FindRef(Hash), Status ? *Status : FString(TEXT("approved")), false));
						}
						else
						{
							Unknown += FString::Printf(TEXT("%s\"%s\""), Unknown.IsEmpty() ? TEXT("") : TEXT(","), *Hash);
						}
					}
					return OkResult(FString::Printf(TEXT("{\"known\":[%s],\"unknown\":[%s]}"), *Known, *Unknown));
				}
			case EMantleUploadCall::UploadAsset:
				{
					const FString Hash = FMantleSha256::AssetHash(Item.Bytes);
					if (Item.Bytes.Num() == 0)
					{
						++EmptyUploads;
					}
					if (RefuseHashes.Contains(Hash))
					{
						return FailedResult(422, TEXT("too_large"));
					}
					if (int32* Left = FailFirstUploads.Find(Hash); Left && *Left > 0)
					{
						--*Left;
						return FailedResult(502, FString());
					}
					++UploadsByHash.FindOrAdd(Hash);
					ReceivedSizeByHash.Add(Hash, Item.Bytes.Num());
					const FString Id = IdByHash.Contains(Hash) ? IdByHash[Hash] : FString::Printf(TEXT("ast_u%032x"), NextId++);
					IdByHash.Add(Hash, Id);
					KindByHash.Add(Hash, Item.Request.Options.Kind);
					const FString Status = UploadStatusByHash.Contains(Hash) ? UploadStatusByHash[Hash] : UploadStatus;
					StatusByHash.Add(Hash, Status);
					return OkResult(AssetJson(Id, Hash, Item.Request.Options.Kind, Status, true));
				}
			case EMantleUploadCall::SaveDraft:
				return OkResult(TEXT("{\"number\":1}"));
			default:
				return OkResult(TEXT("{}"));
			}
		}
	};

	/** The queue on a fake clock over a FFakeMantle. */
	struct FQueueRig
	{
		double Now = 1000.0;
		TSharedRef<FFakeMantle> Mantle = MakeShared<FFakeMantle>();
		TSharedRef<FMantleAssetUploadQueue> Queue;

		FQueueRig()
			: Queue(MakeShared<FMantleAssetUploadQueue>(Mantle, [this] { return Now; }))
		{
			Queue->Settings.BusyJitter = 0.0f;
		}

		/** Ticks and answers, a step of the clock at a time, until Done says so, the run ends or the steps run out. */
		bool RunUntil(TFunctionRef<bool()> Done, int32 MaxSteps = 800)
		{
			for (int32 Index = 0; Index < MaxSteps; ++Index)
			{
				Queue->Tick();
				Mantle->Flush();
				if (Done())
				{
					return true;
				}
				if (!Queue->IsRunning())
				{
					break;
				}
				Now += 0.5;
			}
			return Done();
		}
	};

	FMantleQueuedUpload MakeQueued(const FString& Key, int32 Seed, int32 Size)
	{
		FMantleQueuedUpload Upload;
		Upload.Key = Key;
		Upload.Name = Key;
		Upload.Options.Kind = TEXT("mesh");
		Upload.Bytes = Payload(Seed, Size);
		return Upload;
	}

	/**
	 * A source with a ready-made conversion, so the whole flow runs with no world: step 0 converts a texture and a mesh, step 1 a
	 * texture; once the first Export asks for new assets, step 2 converts a third texture. Optionally it watches meshes it gathers
	 * only weakly, as the level's source does, and pins them like it.
	 */
	class FFakeSource : public IMantlePlacePublishSource
	{
	public:
		bool bChangeOnFirstExport = false;
		bool bLevelChanged = false;
		int32 ExportCalls = 0;
		FString SpillRoot;
		TMap<FString, int64> ExportedSizes;

		/** Meshes made in Prepare that nothing else references: one conversion step each, with no files. 0: the files above instead. */
		int32 WatchedMeshes = 0;
		bool bPin = false;
		FMantleDependencyPins Pins;
		TArray<TWeakObjectPtr<UStaticMesh>> Watched;
		int32 DeadWhenConverted = 0;
		bool bEnded = false;
		bool bConversionFinished = false;
		int32 Events = 0;
		int32 FinishedOrder = 0;
		int32 EndedOrder = 0;
		int32 BallastMB = 0;
		TArray<TArray<uint8>> Ballast;

		virtual FString GetLevelName() const override { return TEXT("MemoryTestLevel"); }

		virtual bool Prepare(FMantleValidationResult& OutValidation, FString& OutMessage) override
		{
			if (WatchedMeshes > 0 && Watched.Num() == 0)
			{
				FMantlePlaceDependencies Dependencies;
				for (int32 Index = 0; Index < WatchedMeshes; ++Index)
				{
					UObject* Outer = GetTransientPackage();
					UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("MemoryTestWatched"))), RF_Transient);
					Watched.Add(Mesh);
					Dependencies.Meshes.AddDefaulted_GetRef().Mesh = Mesh;
				}
				if (bPin)
				{
					Pins.Pin(Dependencies);
				}
			}
			return true;
		}

		virtual int32 NumConversionSteps() const override { return WatchedMeshes > 0 ? WatchedMeshes : (bLevelChanged ? 3 : 2); }
		virtual FString ConversionStepName(int32 Step) const override { return FString::Printf(TEXT("Step%d"), Step); }

		virtual void Convert(int32 Step, FMantlePlaceAssetSet& Set) override
		{
			if (WatchedMeshes > 0)
			{
				if (BallastMB > 0)
				{
					// Memory the process commits and keeps, so the commit grows by BallastMB a step.
					TArray<uint8>& Chunk = Ballast.AddDefaulted_GetRef();
					Chunk.Init(1, BallastMB * 1024 * 1024);
				}
				DeadWhenConverted += Watched.IsValidIndex(Step) && Watched[Step].IsValid() ? 0 : 1;
				return;
			}
			// As the converter does, a key already in the set is left alone.
			auto Texture = [&Set](const TCHAR* Name, int32 Seed, int32 Size)
			{
				if (!Set.FindTexture(FString::Printf(TEXT("texture:color:%s"), Name)))
				{
					AddTexture(Set, Name, Seed, Size);
				}
			};
			if (Step == 0)
			{
				Texture(TEXT("T_A"), 11, 1500);
				if (!Set.FindMesh(TEXT("mesh:/Game/Test/SM_A")))
				{
					AddMesh(Set, TEXT("SM_A"), 12, 2500);
				}
			}
			else if (Step == 1)
			{
				Texture(TEXT("T_B"), 13, 1800);
			}
			else
			{
				Texture(TEXT("T_C"), 14, 2200);
			}
		}

		virtual bool CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage) override
		{
			OutScripts.Reset();
			return true;
		}

		virtual EMantlePlaceExportOutcome Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
			const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues) override
		{
			++ExportCalls;
			if (bChangeOnFirstExport && ExportCalls == 1)
			{
				bLevelChanged = true;
				return EMantlePlaceExportOutcome::NeedsNewAssets;
			}
			// What the exporter writes into the place file: each uploaded asset's recorded size.
			for (const TPair<FString, FMantlePlaceUploadedRef>& Pair : Set.Uploaded)
			{
				ExportedSizes.Add(Pair.Key, Pair.Value.Bytes);
			}
			OutPlaceJson = TEXT("{}");
			return EMantlePlaceExportOutcome::Exported;
		}

		virtual FString FindPlaceId() override { return TEXT("plc_00000000000000000000000000000001"); }
		virtual void RememberPlaceId(const FString& PlaceId) override {}
		virtual void ForgetPlaceId() override {}
		virtual FString GetSpillDirectory() const override { return SpillRoot; }

		virtual void ConversionFinished() override
		{
			bConversionFinished = true;
			FinishedOrder = ++Events;
			Pins.Release();
		}

		virtual void EndPublish() override
		{
			bEnded = true;
			EndedOrder = ++Events;
			Pins.Release();
		}
	};

	/** A session over the fake source and Mantle on a fake clock. */
	struct FSessionRig
	{
		double Now = 1000.0;
		TSharedRef<FFakeMantle> Mantle = MakeShared<FFakeMantle>();
		TSharedRef<FFakeSource> Source = MakeShared<FFakeSource>();
		TSharedPtr<FMantlePlacePublishSession> Session;

		/** A save, which does not wait for Mantle's screening, so it ends with files still processing. */
		void Make(bool bGoLive = false)
		{
			Session = MakeShared<FMantlePlacePublishSession>(Source, Mantle, bGoLive, FString(), [this] { return Now; });
			// The whole conversion in one tick, so the phase after Start does not depend on how fast the machine is.
			Session->Timing.ConvertBudgetSeconds = 1.0e6;
		}

		/** Answers and ticks, until the session ends or Stop says so. */
		bool Run(TFunctionRef<bool()> Stop, int32 MaxSteps = 800)
		{
			for (int32 Index = 0; Index < MaxSteps && !Session->IsFinished(); ++Index)
			{
				Mantle->Flush();
				Session->Tick();
				if (Stop())
				{
					return true;
				}
				Now += 0.5;
			}
			return Session->IsFinished();
		}

		bool RunToEnd(int32 MaxSteps = 800)
		{
			return Run([] { return false; }, MaxSteps);
		}
	};

	/** A reducer that gives back what it is given, so a copy builds from a mesh that already fits. */
	class FPassThroughReducer : public IMeshReduction
	{
	public:
		virtual void ReduceMeshDescription(FMeshDescription& OutReducedMesh, float& OutMaxDeviation, const FMeshDescription& InMesh,
			const FOverlappingCorners& InOverlappingCorners, const struct FMeshReductionSettings& ReductionSettings) override
		{
			OutReducedMesh = InMesh;
			OutMaxDeviation = 0.0f;
		}

		virtual bool ReduceSkeletalMesh(class USkeletalMesh* SkeletalMesh, int32 LODIndex, const class ITargetPlatform* TargetPlatform) override { return false; }

		virtual const FString& GetVersionString() const override
		{
			static const FString Version = TEXT("MantlePublishMemoryTestPassThrough_1");
			return Version;
		}

		virtual bool IsSupported() const override { return true; }
		virtual bool IsReductionActive(const struct FMeshReductionSettings& ReductionSettings) const override { return true; }
		virtual bool IsReductionActive(const struct FMeshReductionSettings& ReductionSettings, uint32 NumVertices, uint32 NumTriangles) const override { return true; }
		virtual bool IsReductionActive(const struct FSkeletalMeshOptimizationSettings& ReductionSettings) const override { return false; }
		virtual bool IsReductionActive(const struct FSkeletalMeshOptimizationSettings& ReductionSettings, uint32 NumVertices, uint32 NumTriangles) const override { return false; }
	};

	/** A transient world with one actor to own a spline mesh over the engine cube. */
	struct FSplineWorld
	{
		UWorld* World = nullptr;
		AActor* Owner = nullptr;
		UStaticMesh* Cube = nullptr;

		FSplineWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
			if (World)
			{
				FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
				Context.SetCurrentWorld(World);
				Owner = World->SpawnActor<AActor>();
			}
			Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		}

		~FSplineWorld()
		{
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		bool IsReady() const { return World && Owner && Cube && Cube->GetRenderData() && Cube->GetRenderData()->LODResources.Num() > 0; }

		USplineMeshComponent* AddSpline() const
		{
			USplineMeshComponent* Component = NewObject<USplineMeshComponent>(Owner);
			Component->SetStaticMesh(Cube);
			Component->SetForwardAxis(ESplineMeshAxis::X, false);
			Component->SetStartAndEnd(FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(400.0, 0.0, 0.0), FVector(400.0, 0.0, 0.0), false);
			Component->SetStartScale(FVector2D(1.0, 1.0), false);
			Component->SetEndScale(FVector2D(1.0, 1.0), false);
			Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			return Component;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryTakeUploadsTest,
	"Mantle.Editor.PublishMemory.TakeUploadsMovesTheBytesOutOfTheSet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryTakeUploadsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	FMantlePlaceAssetSet Set;
	TMap<FString, TArray<uint8>> Original;
	AddOneOfEach(Set, Original);
	TMap<FString, FString> Hashes;
	for (const TPair<FString, TArray<uint8>>& Pair : Original)
	{
		Hashes.Add(Pair.Key, FMantleSha256::AssetHash(Pair.Value));
	}

	int32 Taken = 0;
	for (const EMantlePlaceUploadStage Stage : ByteStages())
	{
		TArray<FMantlePlaceUpload> Uploads;
		FString Error;
		TestTrue(TEXT("a stage is taken"), Set.TakeUploads(Stage, Uploads, Error));
		for (const FMantlePlaceUpload& Upload : Uploads)
		{
			++Taken;
			const TArray<uint8>* Want = Original.Find(Upload.Key);
			if (!TestNotNull(TEXT("an upload is one of the converted files"), Want))
			{
				continue;
			}
			TestTrue(TEXT("the upload carries the converted bytes"), Upload.Bytes == *Want);
			TestEqual(TEXT("and its hash"), Upload.Hash, Hashes[Upload.Key]);
			TestTrue(TEXT("and no file path"), Upload.File.IsEmpty());
		}
		TArray<FMantlePlaceUpload> Again;
		TestTrue(TEXT("a stage is taken again"), Set.TakeUploads(Stage, Again, Error));
		TestEqual(TEXT("and the second time leaves out what the first took"), Again.Num(), 0);
	}
	TestEqual(TEXT("every converted file was taken"), Taken, Original.Num());

	// The set's byte arrays are empty; its hashes and the sizes it recorded are as they were.
	TestEqual(TEXT("the set holds no bytes"), SetBytes(Set), static_cast<int64>(0));
	TestEqual(TEXT("a size is recorded for every file"), Set.TakenSizes.Num(), Original.Num());
	auto CheckKept = [this, &Set, &Original, &Hashes](const FString& Key, const FString& Hash)
	{
		TestEqual(TEXT("its hash is unchanged"), Hash, Hashes[Key]);
		TestEqual(TEXT("its size is what was taken"), Set.UploadSize(Key, 0), static_cast<int64>(Original[Key].Num()));
	};
	for (const FMantleConvertedTexture& Each : Set.Textures) { CheckKept(Each.Key, Each.Hash); }
	for (const FMantleConvertedMesh& Each : Set.Meshes) { CheckKept(Each.Key, Each.Hash); }
	for (const FMantleConvertedSound& Each : Set.Sounds) { CheckKept(Each.Key, Each.Hash); }
	for (const FMantleConvertedSkeletalMesh& Each : Set.SkeletalMeshes) { CheckKept(Each.Key, Each.Hash); }
	for (const FMantleConvertedClip& Each : Set.Clips) { CheckKept(Each.Key, Each.Hash); }
	for (const FMantleConvertedHeightfield& Each : Set.Heightfields) { CheckKept(Each.Key, Each.Hash); }

	// A size read for a file that was never converted falls back to what the caller has.
	TestEqual(TEXT("an unknown key answers the size given"), Set.UploadSize(TEXT("texture:color:Nothing"), 77), static_cast<int64>(77));

	// Releasing a set that was never taken frees every byte array and keeps the hashes.
	FMantlePlaceAssetSet Untaken;
	TMap<FString, TArray<uint8>> UntakenOriginal;
	AddOneOfEach(Untaken, UntakenOriginal);
	TestEqual(TEXT("an untaken set holds its bytes"), SetBytes(Untaken), 900 + 1300 + 2100 + 700 + 1500 + 1100 + 800 + 1700 + static_cast<int64>(0));
	Untaken.ReleaseBytes();
	TestEqual(TEXT("released, it holds none"), SetBytes(Untaken), static_cast<int64>(0));
	TestEqual(TEXT("and its textures are still listed"), Untaken.Textures.Num(), 2);
	TestEqual(TEXT("with their hashes"), Untaken.Textures[0].Hash, Hashes[Untaken.Textures[0].Key]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryTakeAfterRegatherTest,
	"Mantle.Editor.PublishMemory.AFileTakenBeforeIsNotTakenAgainAfterANewGather",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryTakeAfterRegatherTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	FMantlePlaceAssetSet Set;
	AddTexture(Set, TEXT("T_Old"), 21, 1000);
	const FString OldKey = Set.Textures[0].Key;
	const FString OldHash = Set.Textures[0].Hash;

	TArray<FMantlePlaceUpload> First;
	FString Error;
	TestTrue(TEXT("the first take"), Set.TakeUploads(EMantlePlaceUploadStage::Texture, First, Error));
	TestEqual(TEXT("takes the texture"), First.Num(), 1);

	// The level changed: the converter finds the old texture already in the set (and leaves it), and converts a new one.
	AddTexture(Set, TEXT("T_Old"), 21, 1000);
	AddTexture(Set, TEXT("T_New"), 22, 1200);
	TestEqual(TEXT("the old texture was not added twice"), Set.Textures.Num(), 2);

	TArray<FMantlePlaceUpload> Second;
	TestTrue(TEXT("the second take"), Set.TakeUploads(EMantlePlaceUploadStage::Texture, Second, Error));
	if (TestEqual(TEXT("takes only the new texture"), Second.Num(), 1))
	{
		TestTrue(TEXT("which is the new one"), Second[0].Key.EndsWith(TEXT("T_New")));
		TestEqual(TEXT("with its bytes"), Second[0].Bytes.Num(), 1200);
	}
	TestEqual(TEXT("the old texture keeps its hash"), Set.FindTexture(OldKey)->Hash, OldHash);
	TestEqual(TEXT("and its size, though its bytes are gone"), Set.UploadSize(OldKey, 0), static_cast<int64>(1000));
	TestTrue(TEXT("its bytes are gone"), Set.FindTexture(OldKey)->Bytes.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemorySpillSetTest,
	"Mantle.Editor.PublishMemory.SpillWritesFilesWithTheSameHashesAndTheUploadNamesThem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemorySpillSetTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	const FString Directory = TestDirectory(TEXT("SpillSet"));
	FMantlePlaceAssetSet Set;
	TMap<FString, TArray<uint8>> Original;
	AddOneOfEach(Set, Original);
	AddTexture(Set, TEXT("T_Tiny"), 30, 20);

	FString Error;
	// A file below the minimum stays in memory.
	const int32 Written = Set.SpillBytes(Directory, /*MinBytes*/ 100, Error);
	TestTrue(TEXT("no error"), Error.IsEmpty());
	TestEqual(TEXT("every file but the tiny one is written"), Written, Original.Num());
	TestEqual(TEXT("and kept as a path"), Set.Spilled.Num(), Original.Num());
	TestEqual(TEXT("the tiny one stays in memory"), SetBytes(Set), static_cast<int64>(20));

	for (const TPair<FString, TArray<uint8>>& Pair : Original)
	{
		const FMantleSpilledFile* File = Set.Spilled.Find(Pair.Key);
		if (!TestNotNull(TEXT("a spilled file"), File))
		{
			continue;
		}
		TArray<uint8> OnDisk;
		TestTrue(TEXT("it is on disk"), FFileHelper::LoadFileToArray(OnDisk, *File->Path));
		TestTrue(TEXT("with the converted bytes"), OnDisk == Pair.Value);
		TestEqual(TEXT("and the size is recorded"), File->Size, static_cast<int64>(Pair.Value.Num()));
		TestEqual(TEXT("and the size is what UploadSize says"), Set.UploadSize(Pair.Key, 0), File->Size);
	}

	// Taking uploads hands out the path, not bytes.
	int32 WithFile = 0;
	for (const EMantlePlaceUploadStage Stage : ByteStages())
	{
		TArray<FMantlePlaceUpload> Uploads;
		TestTrue(TEXT("a stage is taken"), Set.TakeUploads(Stage, Uploads, Error));
		for (const FMantlePlaceUpload& Upload : Uploads)
		{
			if (Original.Contains(Upload.Key))
			{
				++WithFile;
				TestTrue(TEXT("an upload of a spilled file has no bytes"), Upload.Bytes.IsEmpty());
				TestFalse(TEXT("and names its file"), Upload.File.IsEmpty());
				TestEqual(TEXT("with its size"), Upload.FileSize, static_cast<int64>(Original[Upload.Key].Num()));
				TestEqual(TEXT("and the hash of the converted bytes"), Upload.Hash, FMantleSha256::AssetHash(Original[Upload.Key]));
			}
			else
			{
				TestEqual(TEXT("the tiny one carries its bytes"), Upload.Bytes.Num(), 20);
			}
		}
	}
	TestEqual(TEXT("every spilled file was taken as a path"), WithFile, Original.Num());

	IFileManager::Get().DeleteDirectory(*Directory, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryQueueDropTest,
	"Mantle.Editor.PublishMemory.TheQueueDropsAFilesBytesOnApprovedAndRefusedAndKeepsThemWhileProcessing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryQueueDropTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	FQueueRig Rig;
	FMantleQueuedUpload Approved = MakeQueued(TEXT("approved"), 41, 3000);
	FMantleQueuedUpload Refused = MakeQueued(TEXT("refused"), 42, 4000);
	FMantleQueuedUpload Processing = MakeQueued(TEXT("processing"), 43, 5000);
	const FString ApprovedHash = FMantleSha256::AssetHash(Approved.Bytes);
	const FString RefusedHash = FMantleSha256::AssetHash(Refused.Bytes);
	const FString ProcessingHash = FMantleSha256::AssetHash(Processing.Bytes);
	Rig.Mantle->RefuseHashes.Add(RefusedHash);
	Rig.Mantle->UploadStatusByHash.Add(ProcessingHash, TEXT("processing"));
	Rig.Mantle->StatusByHash.Add(ProcessingHash, TEXT("processing"));

	TArray<FMantleQueuedUpload> Items;
	Items.Add(MoveTemp(Approved));
	Items.Add(MoveTemp(Refused));
	Items.Add(MoveTemp(Processing));
	Rig.Queue->Start(MoveTemp(Items));
	TestEqual(TEXT("before anything is sent the queue holds every file"), Rig.Queue->GetHeldBytes(), static_cast<int64>(12000));

	const bool bSettled = Rig.RunUntil([&Rig]
	{
		const FMantleQueuedUploadStatus* ApprovedNow = Rig.Queue->FindStatus(TEXT("approved"));
		const FMantleQueuedUploadStatus* RefusedNow = Rig.Queue->FindStatus(TEXT("refused"));
		const FMantleQueuedUploadStatus* ProcessingNow = Rig.Queue->FindStatus(TEXT("processing"));
		return ApprovedNow && RefusedNow && ProcessingNow && ApprovedNow->State == EMantleQueuedUploadState::Approved
			&& RefusedNow->State == EMantleQueuedUploadState::Refused && ProcessingNow->State == EMantleQueuedUploadState::Processing;
	});
	if (!TestTrue(TEXT("one file is approved, one refused, one processing"), bSettled))
	{
		return false;
	}
	TestEqual(TEXT("only the file still being processed holds its bytes"), Rig.Queue->GetHeldBytes(), static_cast<int64>(5000));

	// The statuses keep what the panel and the exporter read: hash, size and id.
	const FMantleQueuedUploadStatus* ApprovedStatus = Rig.Queue->FindStatus(TEXT("approved"));
	TestEqual(TEXT("an approved file keeps its hash"), ApprovedStatus->Hash, ApprovedHash);
	TestEqual(TEXT("its size"), ApprovedStatus->Bytes, static_cast<int64>(3000));
	TestTrue(TEXT("and its id"), ApprovedStatus->HasId());
	const FMantleQueuedUploadStatus* RefusedStatus = Rig.Queue->FindStatus(TEXT("refused"));
	TestEqual(TEXT("a refused file keeps its hash"), RefusedStatus->Hash, RefusedHash);
	TestEqual(TEXT("and its size"), RefusedStatus->Bytes, static_cast<int64>(4000));

	// Once Mantle approves the processing file, its bytes go too.
	Rig.Mantle->StatusByHash.Add(ProcessingHash, TEXT("approved"));
	const bool bDone = Rig.RunUntil([&Rig] { return !Rig.Queue->IsRunning(); });
	TestTrue(TEXT("the run ends"), bDone);
	TestEqual(TEXT("the queue completed"), static_cast<int32>(Rig.Queue->GetOutcome()), static_cast<int32>(EMantleUploadQueueOutcome::Completed));
	TestEqual(TEXT("and holds no bytes"), Rig.Queue->GetHeldBytes(), static_cast<int64>(0));
	TestEqual(TEXT("the last file keeps its size"), Rig.Queue->FindStatus(TEXT("processing"))->Bytes, static_cast<int64>(5000));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryQueueKeepTest,
	"Mantle.Editor.PublishMemory.TheQueueKeepsAFilesBytesAcrossARetryAndADiffThatForgetsIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryQueueKeepTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	FQueueRig Rig;
	FMantleQueuedUpload Retried = MakeQueued(TEXT("retried"), 51, 3500);
	FMantleQueuedUpload Forgotten = MakeQueued(TEXT("forgotten"), 52, 4500);
	const FString RetriedHash = FMantleSha256::AssetHash(Retried.Bytes);
	const FString ForgottenHash = FMantleSha256::AssetHash(Forgotten.Bytes);
	Rig.Mantle->FailFirstUploads.Add(RetriedHash, 1);
	Rig.Mantle->UploadStatusByHash.Add(ForgottenHash, TEXT("processing"));

	TArray<FMantleQueuedUpload> Items;
	Items.Add(MoveTemp(Retried));
	Items.Add(MoveTemp(Forgotten));
	Rig.Queue->Start(MoveTemp(Items));

	// The first send of 'retried' answers 502: its bytes are kept for the retry.
	const bool bFailedOnce = Rig.RunUntil([&Rig] { return Rig.Queue->FindStatus(TEXT("retried"))->Attempts > 0; });
	if (!TestTrue(TEXT("the first upload of 'retried' fails and is scheduled again"), bFailedOnce))
	{
		return false;
	}
	TestEqual(TEXT("both files' bytes are held: 'retried' for its retry, 'forgotten' while it is processed"), Rig.Queue->GetHeldBytes(), static_cast<int64>(8000));
	const bool bRetried = Rig.RunUntil([&Rig]
	{
		return Rig.Queue->FindStatus(TEXT("forgotten"))->State == EMantleQueuedUploadState::Processing
			&& Rig.Queue->FindStatus(TEXT("retried"))->State == EMantleQueuedUploadState::Approved;
	});
	TestTrue(TEXT("'retried' is approved after its retry while 'forgotten' is processing"), bRetried);
	TestEqual(TEXT("'retried' went through on the retry and dropped its bytes; 'forgotten' keeps its own"), Rig.Queue->GetHeldBytes(), static_cast<int64>(4500));
	TestEqual(TEXT("the retry was the only accepted upload of it"), Rig.Mantle->UploadsByHash.FindRef(RetriedHash), 1);

	// A look at the processing file finds Mantle has forgotten it: it is sent again, which needs its bytes.
	Rig.Mantle->ForgetOnDiff.Add(ForgottenHash);
	const bool bSentAgain = Rig.RunUntil([&Rig, &ForgottenHash] { return Rig.Mantle->UploadsByHash.FindRef(ForgottenHash) >= 2; });
	TestTrue(TEXT("the forgotten file is uploaded again"), bSentAgain);
	const bool bDone = Rig.RunUntil([&Rig] { return !Rig.Queue->IsRunning(); });
	TestTrue(TEXT("the run ends"), bDone);
	TestEqual(TEXT("with the same bytes both times"), Rig.Mantle->ReceivedSizeByHash.FindRef(ForgottenHash), 4500);
	TestEqual(TEXT("and no empty upload"), Rig.Mantle->EmptyUploads, 0);
	TestEqual(TEXT("and nothing held at the end"), Rig.Queue->GetHeldBytes(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryQueueLargeTest,
	"Mantle.Editor.PublishMemory.TheQueueSendsAtMostTwoLargeUploadsAtOnceAndSmallOnesKeepGoing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryQueueLargeTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	FQueueRig Rig;
	TArray<FMantleQueuedUpload> Items;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Items.Add(MakeQueued(FString::Printf(TEXT("large%d"), Index), 80 + Index, 1024 * 1024 + 4096));
	}
	for (int32 Index = 0; Index < 6; ++Index)
	{
		Items.Add(MakeQueued(FString::Printf(TEXT("small%d"), Index), 90 + Index, 2000));
	}
	Rig.Queue->Start(MoveTemp(Items));
	const bool bDone = Rig.RunUntil([&Rig] { return !Rig.Queue->IsRunning(); });
	TestTrue(TEXT("the run ends"), bDone);
	TestEqual(TEXT("every file is on Mantle"), Rig.Mantle->UploadsByHash.Num(), 11);
	TestEqual(TEXT("never more than two large uploads were out at once"), Rig.Mantle->MaxLargeInFlight, 2);
	TestTrue(TEXT("while small ones used the other request slots"), Rig.Mantle->MaxUploadsInFlight > 2);
	TestTrue(TEXT("and never more than the request limit"), Rig.Mantle->MaxUploadsInFlight <= Rig.Queue->Settings.MaxConcurrentRequests);
	TestEqual(TEXT("nothing was refused or left waiting"), static_cast<int32>(Rig.Queue->GetOutcome()), static_cast<int32>(EMantleUploadQueueOutcome::Completed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryQueueFileTest,
	"Mantle.Editor.PublishMemory.TheQueueSendsASpilledFileFromDiskAndHoldsNoBytes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryQueueFileTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	const FString Directory = TestDirectory(TEXT("QueueFile"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	FQueueRig Rig;
	const TArray<uint8> Bytes = Payload(61, 6000);
	const FString Hash = FMantleSha256::AssetHash(Bytes);
	const FString Path = FPaths::Combine(Directory, TEXT("spilled.bin"));
	FFileHelper::SaveArrayToFile(Bytes, *Path);

	FMantleQueuedUpload Spilled;
	Spilled.Key = TEXT("spilled");
	Spilled.Name = TEXT("spilled");
	Spilled.Options.Kind = TEXT("mesh");
	Spilled.File = Path;
	Spilled.FileSize = Bytes.Num();
	Spilled.Hash = Hash;
	FMantleQueuedUpload Missing = Spilled;
	Missing.Key = TEXT("missing");
	Missing.Name = TEXT("missing");
	Missing.File = FPaths::Combine(Directory, TEXT("gone.bin"));
	Missing.Hash = FMantleSha256::AssetHash(Payload(62, 10));

	// A file of the right size whose bytes are not the ones that were hashed: someone touched it.
	const FString ChangedPath = FPaths::Combine(Directory, TEXT("changed.bin"));
	FFileHelper::SaveArrayToFile(Payload(63, 6000), *ChangedPath);
	FMantleQueuedUpload Changed = Spilled;
	Changed.Key = TEXT("changed");
	Changed.Name = TEXT("changed");
	Changed.File = ChangedPath;
	Changed.Hash = FMantleSha256::AssetHash(Payload(64, 6000));

	TArray<FMantleQueuedUpload> Items;
	Items.Add(MoveTemp(Spilled));
	Items.Add(MoveTemp(Missing));
	Items.Add(MoveTemp(Changed));
	Rig.Queue->Start(MoveTemp(Items));
	TestEqual(TEXT("a file-backed item holds no bytes"), Rig.Queue->GetHeldBytes(), static_cast<int64>(0));
	TestEqual(TEXT("and its status has the file's size"), Rig.Queue->FindStatus(TEXT("spilled"))->Bytes, static_cast<int64>(6000));

	const bool bDone = Rig.RunUntil([&Rig] { return !Rig.Queue->IsRunning(); });
	TestTrue(TEXT("the run ends"), bDone);
	TestEqual(TEXT("Mantle was sent the file's bytes"), Rig.Mantle->UploadsByHash.FindRef(Hash), 1);
	TestEqual(TEXT("all of them"), Rig.Mantle->ReceivedSizeByHash.FindRef(Hash), 6000);
	TestEqual(TEXT("the spilled file is approved"), static_cast<int32>(Rig.Queue->FindStatus(TEXT("spilled"))->State), static_cast<int32>(EMantleQueuedUploadState::Approved));
	const FMantleQueuedUploadStatus* MissingStatus = Rig.Queue->FindStatus(TEXT("missing"));
	TestEqual(TEXT("a file that is gone is refused, not sent"), static_cast<int32>(MissingStatus->State), static_cast<int32>(EMantleQueuedUploadState::Refused));
	TestEqual(TEXT("with its own code"), MissingStatus->Code, FString(TEXT("unreadable")));
	const FMantleQueuedUploadStatus* ChangedStatus = Rig.Queue->FindStatus(TEXT("changed"));
	TestEqual(TEXT("a file whose bytes are not the hashed ones is refused too"), static_cast<int32>(ChangedStatus->State), static_cast<int32>(EMantleQueuedUploadState::Refused));
	TestEqual(TEXT("as unreadable"), ChangedStatus->Code, FString(TEXT("unreadable")));
	TestEqual(TEXT("and never sent"), Rig.Mantle->UploadsByHash.Num(), 1);
	TestEqual(TEXT("nothing is held"), Rig.Queue->GetHeldBytes(), static_cast<int64>(0));
	IFileManager::Get().DeleteDirectory(*Directory, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemorySessionReleaseTest,
	"Mantle.Editor.PublishMemory.AnEndedSessionHoldsNoBytes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemorySessionReleaseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	const int32 ActiveBefore = FMantlePublishCaches::NumActive();

	// Done: a save ends with files Mantle is still processing, whose bytes the queue keeps until then.
	{
		FSessionRig Rig;
		Rig.Mantle->UploadStatus = TEXT("processing");
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Start();
		TestEqual(TEXT("a running session holds the bake caches"), FMantlePublishCaches::NumActive(), ActiveBefore + 1);
		const bool bUploaded = Rig.Run([&Rig] { return Rig.Session->GetPhase() != EMantlePublishPhase::Uploading; });
		TestTrue(TEXT("the upload ends"), bUploaded);
		const FMantleAssetUploadQueue* Queue = Rig.Session->GetQueue();
		if (!TestNotNull(TEXT("the session has a queue"), Queue))
		{
			return false;
		}
		TestEqual(TEXT("the queue lets its bytes go as soon as its run is over, though Mantle is still processing the files"),
			Queue->GetHeldBytes(), static_cast<int64>(0));
		TestEqual(TEXT("the files are indeed still processing"), static_cast<int32>(Queue->GetStatuses()[0].State), static_cast<int32>(EMantleQueuedUploadState::Processing));
		TestTrue(TEXT("the session finishes"), Rig.RunToEnd());
		TestTrue(TEXT("and succeeds"), Rig.Session->Succeeded());
		TestEqual(TEXT("the queue holds no bytes"), Queue->GetHeldBytes(), static_cast<int64>(0));
		TestEqual(TEXT("the set holds no bytes"), SetBytes(Rig.Session->GetAssetSet()), static_cast<int64>(0));
		TestTrue(TEXT("the source was told the publish is over"), Rig.Source->bEnded);
		TestEqual(TEXT("the caches are no longer held"), FMantlePublishCaches::NumActive(), ActiveBefore);

		// What the panel still shows: the statuses with their sizes and ids, the set's names and recorded ids.
		TestEqual(TEXT("the queue still lists its files"), Queue->GetStatuses().Num(), 3);
		for (const FMantleQueuedUploadStatus& Status : Queue->GetStatuses())
		{
			TestTrue(TEXT("a status keeps its size"), Status.Bytes > 0);
			TestFalse(TEXT("its hash"), Status.Hash.IsEmpty());
			TestTrue(TEXT("and its id"), Status.HasId());
		}
		TestEqual(TEXT("the set still lists its textures"), Rig.Session->GetAssetSet().Textures.Num(), 2);
		TestEqual(TEXT("and has every upload recorded"), Rig.Session->GetAssetSet().Uploaded.Num(), 3);
	}

	// Failed: Mantle refuses a file; the others, still processing, let their bytes go.
	{
		FSessionRig Rig;
		Rig.Mantle->UploadStatus = TEXT("processing");
		FMantlePlaceAssetSet Probe;
		AddTexture(Probe, TEXT("T_A"), 11, 1500);
		Rig.Mantle->RefuseHashes.Add(Probe.Textures[0].Hash);
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Start();
		TestTrue(TEXT("the session ends"), Rig.RunToEnd());
		TestEqual(TEXT("in failure"), static_cast<int32>(Rig.Session->GetPhase()), static_cast<int32>(EMantlePublishPhase::Failed));
		TestEqual(TEXT("the queue holds no bytes"), Rig.Session->GetQueue()->GetHeldBytes(), static_cast<int64>(0));
		TestEqual(TEXT("the set holds no bytes"), SetBytes(Rig.Session->GetAssetSet()), static_cast<int64>(0));
		TestTrue(TEXT("what went wrong is still shown"), Rig.Session->GetAttention().Num() > 0);
		TestEqual(TEXT("the caches are no longer held"), FMantlePublishCaches::NumActive(), ActiveBefore);
	}

	// Cancelled.
	{
		FSessionRig Rig;
		Rig.Mantle->UploadStatus = TEXT("processing");
		Rig.Make(/*bGoLive*/ true);
		Rig.Session->Start();
		TestEqual(TEXT("a publish is uploading"), static_cast<int32>(Rig.Session->GetPhase()), static_cast<int32>(EMantlePublishPhase::Uploading));
		TestTrue(TEXT("it can be cancelled"), Rig.Session->Cancel());
		TestTrue(TEXT("and says so"), Rig.Session->GetResult().bCancelled);
		TestEqual(TEXT("the queue holds no bytes"), Rig.Session->GetQueue()->GetHeldBytes(), static_cast<int64>(0));
		TestEqual(TEXT("the set holds no bytes"), SetBytes(Rig.Session->GetAssetSet()), static_cast<int64>(0));
		TestEqual(TEXT("the caches are no longer held"), FMantlePublishCaches::NumActive(), ActiveBefore);
	}

	// A session dropped before it ended gives its hold on the caches back too.
	{
		FSessionRig Rig;
		Rig.Mantle->UploadStatus = TEXT("processing");
		Rig.Make(/*bGoLive*/ true);
		Rig.Session->Start();
		TestEqual(TEXT("held while it runs"), FMantlePublishCaches::NumActive(), ActiveBefore + 1);
		Rig.Session.Reset();
		TestEqual(TEXT("and given back when it is dropped"), FMantlePublishCaches::NumActive(), ActiveBefore);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemorySessionRegatherTest,
	"Mantle.Editor.PublishMemory.ARegatherUploadsNothingTwiceAndKeepsEverySize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemorySessionRegatherTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	FSessionRig Rig;
	Rig.Source->bChangeOnFirstExport = true;
	Rig.Make(/*bGoLive*/ false);
	Rig.Session->Start();
	TestTrue(TEXT("the session finishes"), Rig.RunToEnd());
	TestTrue(TEXT("and succeeds"), Rig.Session->Succeeded());
	TestEqual(TEXT("the level was exported twice: once asking for the new texture, once with it"), Rig.Source->ExportCalls, 2);

	// T_A, SM_A, T_B and the texture the level gained: each uploaded once, none empty.
	TestEqual(TEXT("four different files reached Mantle"), Rig.Mantle->UploadsByHash.Num(), 4);
	for (const TPair<FString, int32>& Pair : Rig.Mantle->UploadsByHash)
	{
		TestEqual(TEXT("each was uploaded once"), Pair.Value, 1);
	}
	TestEqual(TEXT("and nothing was sent empty"), Rig.Mantle->EmptyUploads, 0);

	// The sizes the place file records are the files' own, including the files uploaded before the level changed.
	const TMap<FString, int64>& Sizes = Rig.Source->ExportedSizes;
	TestEqual(TEXT("the first texture's size"), Sizes.FindRef(TEXT("texture:color:T_A")), static_cast<int64>(1500));
	TestEqual(TEXT("the mesh's size"), Sizes.FindRef(TEXT("mesh:/Game/Test/SM_A")), static_cast<int64>(2500));
	TestEqual(TEXT("the second texture's size"), Sizes.FindRef(TEXT("texture:color:T_B")), static_cast<int64>(1800));
	TestEqual(TEXT("the new texture's size"), Sizes.FindRef(TEXT("texture:color:T_C")), static_cast<int64>(2200));
	const FMantlePlaceAssetSet& Set = Rig.Session->GetAssetSet();
	TestEqual(TEXT("every file's recorded hash is its bytes'"), Set.Uploaded.FindChecked(TEXT("texture:color:T_A")).Hash,
		FMantleSha256::AssetHash(Payload(11, 1500)));
	TestEqual(TEXT("four files were taken from the set"), Set.TakenSizes.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemorySessionSpillTest,
	"Mantle.Editor.PublishMemory.SpillingSendsTheSameBytesFromDiskAndCleansUp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemorySessionSpillTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	const FString Root = TestDirectory(TEXT("SessionSpill"));

	// Off: the files stay in memory until they are taken.
	FSessionRig Plain;
	Plain.Make(/*bGoLive*/ false);
	Plain.Session->Start();
	TestTrue(TEXT("a publish that does not spill finishes"), Plain.RunToEnd());

	FSessionRig Spilled;
	Spilled.Source->SpillRoot = Root;
	Spilled.Make(/*bGoLive*/ false);
	Spilled.Session->Timing.bSpillConvertedBytes = true;
	Spilled.Session->Timing.SpillMinBytes = 1;
	Spilled.Session->Start();
	TestEqual(TEXT("it is uploading"), static_cast<int32>(Spilled.Session->GetPhase()), static_cast<int32>(EMantlePublishPhase::Uploading));
	TestEqual(TEXT("the set keeps no bytes once converted"), SetBytes(Spilled.Session->GetAssetSet()), static_cast<int64>(0));
	TestEqual(TEXT("the queue holds none before a single upload"), Spilled.Session->GetQueue()->GetHeldBytes(), static_cast<int64>(0));
	TestEqual(TEXT("the set names a file for each of the three"), Spilled.Session->GetAssetSet().Spilled.Num(), 3);
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.bin"), true, false);
	TestEqual(TEXT("and they are on disk"), Files.Num(), 3);

	TestTrue(TEXT("a spilling publish finishes"), Spilled.RunToEnd());
	TestTrue(TEXT("and succeeds"), Spilled.Session->Succeeded());

	// The same bytes reached Mantle: the same hashes, the same sizes, each once.
	TestEqual(TEXT("the same number of files"), Spilled.Mantle->UploadsByHash.Num(), Plain.Mantle->UploadsByHash.Num());
	for (const TPair<FString, int32>& Pair : Plain.Mantle->UploadsByHash)
	{
		TestEqual(TEXT("each file of the plain run was uploaded once by the spilling run"), Spilled.Mantle->UploadsByHash.FindRef(Pair.Key), 1);
		TestEqual(TEXT("with the same size"), Spilled.Mantle->ReceivedSizeByHash.FindRef(Pair.Key), Plain.Mantle->ReceivedSizeByHash.FindRef(Pair.Key));
	}
	TestEqual(TEXT("nothing was sent empty"), Spilled.Mantle->EmptyUploads, 0);
	for (const TPair<FString, FMantlePlaceUploadedRef>& Pair : Plain.Session->GetAssetSet().Uploaded)
	{
		const FMantlePlaceUploadedRef* Other = Spilled.Session->GetAssetSet().Uploaded.Find(Pair.Key);
		if (TestNotNull(TEXT("every upload is recorded by the spilling run"), Other))
		{
			TestEqual(TEXT("with the same hash"), Other->Hash, Pair.Value.Hash);
			TestEqual(TEXT("and size"), Other->Bytes, Pair.Value.Bytes);
		}
	}

	// The spilled files do not outlive the publish.
	Files.Reset();
	IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.bin"), true, false);
	TestEqual(TEXT("the files are deleted when it ends"), Files.Num(), 0);
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryOutcomeLineTest,
	"Mantle.Editor.PublishMemory.TheDoneLineCarriesThePeakMemory",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryOutcomeLineTest::RunTest(const FString& Parameters)
{
	FMantlePublishResult Live;
	Live.bSuccess = true;
	Live.bWentLive = true;
	Live.PlaceId = TEXT("plc_abc");
	Live.RevisionNumber = 3;
	FMantlePublishMemory Memory;
	Memory.PeakCommitMB = 18800;
	Memory.PeakWorkingSetMB = 9100;
	Memory.UsedCommitMB = 7000;

	TestEqual(TEXT("seconds, then the peak commit and the peak working set"), FMantleHeadlessPublish::OutcomeLine(Live, 12.34, Memory),
		FString(TEXT("MANTLE_PUBLISH_DONE plc_abc 3 seconds=12.3 peakMB=18800 peakWsMB=9100")));
	TestEqual(TEXT("without a measurement the line is as it was"), FMantleHeadlessPublish::OutcomeLine(Live, 12.34),
		FString(TEXT("MANTLE_PUBLISH_DONE plc_abc 3 seconds=12.3")));
	TestEqual(TEXT("the session line adds the commit now"), Memory.ToFields(/*bWithUsed*/ true), FString(TEXT(" peakMB=18800 peakWsMB=9100 usedMB=7000")));
	TestTrue(TEXT("a publish that did not go live reports no memory"), FMantleHeadlessPublish::OutcomeLine(FMantlePublishResult(), 1.0, Memory)
		.StartsWith(TEXT("MANTLE_PUBLISH_FAILED")));
	TestFalse(TEXT("and none in it"), FMantleHeadlessPublish::OutcomeLine(FMantlePublishResult(), 1.0, Memory).Contains(TEXT("peakMB")));

	// The real reading is in megabytes and consistent: the peak is not below what is used now.
	const FMantlePublishMemory Now = FMantlePublishMemory::Sample();
	TestTrue(TEXT("the process memory is measured"), Now.IsKnown());
	TestTrue(TEXT("the working set peak is a real number of megabytes"), Now.PeakWorkingSetMB > 0);
	TestTrue(TEXT("a line made from it has all three fields"), Now.ToFields(true).Contains(TEXT("peakMB=")) && Now.ToFields(true).Contains(TEXT("peakWsMB="))
		&& Now.ToFields(true).Contains(TEXT("usedMB=")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryPinTest,
	"Mantle.Editor.PublishMemory.CollectingGarbageBetweenStepsKeepsWhatLaterStepsConvert",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryPinTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	constexpr int32 Steps = 5;

	// The pins on their own: the control proves an object nothing references is collected.
	{
		UObject* Outer = GetTransientPackage();
		UStaticMesh* Loose = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("MemoryTestLoose"))), RF_Transient);
		UStaticMesh* Held = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("MemoryTestHeld"))), RF_Transient);
		TWeakObjectPtr<UStaticMesh> WeakLoose(Loose);
		TWeakObjectPtr<UStaticMesh> WeakHeld(Held);
		FMantlePlaceDependencies Dependencies;
		Dependencies.Meshes.AddDefaulted_GetRef().Mesh = Held;
		FMantleDependencyPins Pins;
		Pins.Pin(Dependencies);
		TestEqual(TEXT("one object is pinned"), Pins.Num(), 1);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestFalse(TEXT("control: an object nothing references is collected"), WeakLoose.IsValid());
		TestTrue(TEXT("a pinned object survives a collection"), WeakHeld.IsValid());
		Pins.Release();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestFalse(TEXT("and is collected once the pins are released"), WeakHeld.IsValid());
	}

	// The collection keeps what the editor keeps: a standalone asset (an open editor's unsaved edit) is not freed by it.
	if (GIsEditor)
	{
		UObject* Outer = GetTransientPackage();
		UStaticMesh* Edited = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("MemoryTestEdited"))), RF_Transient | RF_Standalone);
		TWeakObjectPtr<UStaticMesh> WeakEdited(Edited);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestTrue(TEXT("a standalone asset survives the publish's kind of collection"), WeakEdited.IsValid());
		if (Edited)
		{
			Edited->ClearFlags(RF_Standalone);
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestFalse(TEXT("and is collected once it is no longer standalone"), WeakEdited.IsValid());
	}

	// Without the pins, a collection after every step frees what the later steps convert.
	{
		FSessionRig Rig;
		Rig.Source->WatchedMeshes = Steps;
		Rig.Source->bPin = false;
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Timing.CollectGarbageEverySteps = 1;
		Rig.Session->Start();
		TestTrue(TEXT("the session finishes"), Rig.RunToEnd());
		TestTrue(TEXT("control: with no pins, a step finds its object gone"), Rig.Source->DeadWhenConverted > 0);
	}

	// With them, every step finds its object.
	{
		FSessionRig Rig;
		Rig.Source->WatchedMeshes = Steps;
		Rig.Source->bPin = true;
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Timing.CollectGarbageEverySteps = 1;
		Rig.Session->Start();
		TestTrue(TEXT("the session finishes"), Rig.RunToEnd());
		TestTrue(TEXT("and succeeds"), Rig.Session->Succeeded());
		TestEqual(TEXT("every step found its object, though a collection ran after each"), Rig.Source->DeadWhenConverted, 0);
		TestEqual(TEXT("all of them were converted"), Rig.Source->Watched.Num(), Steps);
		TestTrue(TEXT("the source released its pins when the publish ended"), Rig.Source->bEnded && Rig.Source->Pins.Num() == 0);
		TestTrue(TEXT("and as soon as conversion was done, not only at the end"), Rig.Source->bConversionFinished && Rig.Source->FinishedOrder < Rig.Source->EndedOrder);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		int32 Alive = 0;
		for (const TWeakObjectPtr<UStaticMesh>& Mesh : Rig.Source->Watched)
		{
			Alive += Mesh.IsValid() ? 1 : 0;
		}
		TestEqual(TEXT("and nothing stays alive for it afterwards"), Alive, 0);
	}

	// A cadence of 0 never collects: the panel's publish leaves it to the editor.
	{
		FSessionRig Rig;
		Rig.Source->WatchedMeshes = Steps;
		Rig.Source->bPin = false;
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Start();
		TestTrue(TEXT("the session finishes"), Rig.RunToEnd());
		TestEqual(TEXT("with no collection, every object is still there"), Rig.Source->DeadWhenConverted, 0);
	}

	// A step that grows the commit a lot brings a collection of its own, though the step count is far off.
	{
		FSessionRig Rig;
		Rig.Source->WatchedMeshes = Steps;
		Rig.Source->bPin = false;
		Rig.Source->BallastMB = 32;
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Timing.CollectGarbageEverySteps = 1000;
		Rig.Session->Timing.CollectGarbageGrowthBytes = 16LL * 1024 * 1024;
		Rig.Session->Start();
		TestTrue(TEXT("the session finishes"), Rig.RunToEnd());
		TestTrue(TEXT("growth of the commit collected garbage between steps"), Rig.Source->DeadWhenConverted > 0);
	}
	{
		FSessionRig Rig;
		Rig.Source->WatchedMeshes = Steps;
		Rig.Source->bPin = false;
		Rig.Source->BallastMB = 32;
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Timing.CollectGarbageEverySteps = 1000;
		Rig.Session->Timing.CollectGarbageGrowthBytes = 0;
		Rig.Session->Start();
		TestTrue(TEXT("the session finishes"), Rig.RunToEnd());
		TestEqual(TEXT("control: with growth switched off, the same steps collect nothing"), Rig.Source->DeadWhenConverted, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryPinAllTest,
	"Mantle.Editor.PublishMemory.PinsCoverEveryKindOfGatheredObject",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryPinAllTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	UObject* Outer = GetTransientPackage();
	auto Make = [Outer](UClass* Class, const TCHAR* Name) -> UObject*
	{
		return NewObject<UObject>(Outer, Class, MakeUniqueObjectName(Outer, Class, FName(Name)), RF_Transient);
	};

	// One object for each of the nine arrays (a clip names two), pinned; and the same kinds loose as the control.
	struct FKindObjects
	{
		TArray<TWeakObjectPtr<UObject>> Objects;
	};
	FKindObjects Pinned;
	FKindObjects Loose;
	FMantlePlaceDependencies Dependencies;
	FSplineWorld Fixture;
	for (int32 Round = 0; Round < 2; ++Round)
	{
		FKindObjects& Into = Round == 0 ? Pinned : Loose;
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Make(UStaticMesh::StaticClass(), TEXT("MemoryTestStaticMesh")));
		UMaterial* Material = Cast<UMaterial>(Make(UMaterial::StaticClass(), TEXT("MemoryTestMaterial")));
		UTexture2D* Texture = Cast<UTexture2D>(Make(UTexture2D::StaticClass(), TEXT("MemoryTestTexture")));
		USoundWave* Sound = Cast<USoundWave>(Make(USoundWave::StaticClass(), TEXT("MemoryTestSound")));
		USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Make(USkeletalMesh::StaticClass(), TEXT("MemoryTestSkeletal")));
		UAnimSequence* Sequence = Cast<UAnimSequence>(Make(UAnimSequence::StaticClass(), TEXT("MemoryTestSequence")));
		USkeletalMesh* ClipMesh = Cast<USkeletalMesh>(Make(USkeletalMesh::StaticClass(), TEXT("MemoryTestClipMesh")));
		UMantleVehicleSource* Vehicle = Cast<UMantleVehicleSource>(Make(UMantleVehicleSource::StaticClass(), TEXT("MemoryTestVehicle")));
		USkeletalMesh* WeaponMesh = Cast<USkeletalMesh>(Make(USkeletalMesh::StaticClass(), TEXT("MemoryTestWeaponMesh")));
		if (!TestTrue(TEXT("the objects are made"), StaticMesh && Material && Texture && Sound && Skeletal && Sequence && ClipMesh && Vehicle && WeaponMesh))
		{
			return false;
		}
		for (UObject* Object : TArray<UObject*>{ StaticMesh, Material, Texture, Sound, Skeletal, Sequence, ClipMesh, Vehicle, WeaponMesh })
		{
			Into.Objects.Add(Object);
		}
		if (Round == 0)
		{
			Dependencies.Meshes.AddDefaulted_GetRef().Mesh = StaticMesh;
			Dependencies.Materials.AddDefaulted_GetRef().Material = Material;
			Dependencies.Textures.AddDefaulted_GetRef().Texture = Texture;
			Dependencies.Sounds.AddDefaulted_GetRef().Sound = Sound;
			Dependencies.SkeletalMeshes.AddDefaulted_GetRef().Mesh = Skeletal;
			FMantleGatheredClip& Clip = Dependencies.Clips.AddDefaulted_GetRef();
			Clip.Sequence = Sequence;
			Clip.Mesh = ClipMesh;
			Dependencies.Vehicles.AddDefaulted_GetRef().Vehicle = Vehicle;
			Dependencies.Weapons.AddDefaulted_GetRef().Mesh = WeaponMesh;
		}
	}
	// The ninth array holds an actor, which the world owns: it is only counted.
	const bool bHasActor = Fixture.IsReady();
	if (bHasActor)
	{
		Dependencies.Terrains.AddDefaulted_GetRef().Landscape = Fixture.Owner;
	}

	FMantleDependencyPins Pins;
	Pins.Pin(Dependencies);
	TestEqual(TEXT("every object of the eight arrays, and the terrain's, is pinned (a clip's sequence and mesh are two)"), Pins.Num(), 9 + (bHasActor ? 1 : 0));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	int32 PinnedAlive = 0;
	for (const TWeakObjectPtr<UObject>& Object : Pinned.Objects) { PinnedAlive += Object.IsValid() ? 1 : 0; }
	int32 LooseAlive = 0;
	for (const TWeakObjectPtr<UObject>& Object : Loose.Objects) { LooseAlive += Object.IsValid() ? 1 : 0; }
	TestEqual(TEXT("every pinned kind survives a collection"), PinnedAlive, Pinned.Objects.Num());
	TestEqual(TEXT("control: the same kinds, unpinned, do not"), LooseAlive, 0);
	Pins.Release();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	int32 AfterRelease = 0;
	for (const TWeakObjectPtr<UObject>& Object : Pinned.Objects) { AfterRelease += Object.IsValid() ? 1 : 0; }
	TestEqual(TEXT("and once released they go"), AfterRelease, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryPruneTest,
	"Mantle.Editor.PublishMemory.TheBakeCachesGoWhenTheLastPublishEndsAndBakeAgain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryPruneTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	// Whatever an earlier test left counted (a session it never ended, say) must not decide this one.
	FMantlePublishCaches::ResetCountForTests();
	FMantlePublishCaches::Prune();
	FMantleMeshReduce::SetReducerForTests(nullptr);
	TStrongObjectPtr<UStaticMesh> Cube(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	if (!TestTrue(TEXT("the engine cube loads"), Cube.IsValid() && Cube->GetRenderData() && Cube->GetRenderData()->LODResources.Num() > 0))
	{
		return false;
	}
	FPassThroughReducer Reducer;
	FMantleMeshReduce::SetReducerForTests(&Reducer);
	const FMantlePlaceAssetLimits Limits;
	const FVector Mirror(-1.0, 1.0, 1.0);

	// Two publishes at once: the first to end must not take the caches from the other.
	FMantlePublishCaches::Enter();
	FMantlePublishCaches::Enter();
	FVector PositiveScale;
	FString Why;
	UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Cube.Get(), Mirror, PositiveScale, Why);
	UStaticMesh* Reduced = FMantleMeshReduce::ReducedCopy(Cube.Get(), Limits, Why);
	TestNotNull(TEXT("a mirrored copy"), Mirrored);
	TestNotNull(TEXT("a reduced copy"), Reduced);
	TWeakObjectPtr<UStaticMesh> WeakMirrored(Mirrored);
	TWeakObjectPtr<UStaticMesh> WeakReduced(Reduced);
	FMantlePublishCaches::Leave();
	TestEqual(TEXT("one publish still runs"), FMantlePublishCaches::NumActive(), 1);
	TestTrue(TEXT("so the mirrored copy is still the cached one"), FMantleMirrorBake::MirroredCopy(Cube.Get(), Mirror, PositiveScale, Why) == Mirrored);
	TestTrue(TEXT("and the reduced copy"), FMantleMeshReduce::ReducedCopy(Cube.Get(), Limits, Why) == Reduced);

	// The last one ends: the copies are let go, and the next publish bakes what it needs again.
	FMantlePublishCaches::Leave();
	TestEqual(TEXT("no publish runs"), FMantlePublishCaches::NumActive(), 0);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("the mirrored copy was collected"), WeakMirrored.IsValid());
	TestFalse(TEXT("and the reduced copy"), WeakReduced.IsValid());
	UStaticMesh* MirroredAgain = FMantleMirrorBake::MirroredCopy(Cube.Get(), Mirror, PositiveScale, Why);
	if (TestNotNull(TEXT("a mirrored copy is baked again"), MirroredAgain))
	{
		TestTrue(TEXT("with geometry"), MirroredAgain->GetRenderData() && MirroredAgain->GetRenderData()->LODResources.Num() > 0
			&& MirroredAgain->GetRenderData()->LODResources[0].GetNumTriangles() > 0);
	}
	UStaticMesh* ReducedAgain = FMantleMeshReduce::ReducedCopy(Cube.Get(), Limits, Why);
	if (TestNotNull(TEXT("a reduced copy is made again"), ReducedAgain))
	{
		TestTrue(TEXT("with geometry"), ReducedAgain->GetRenderData() && ReducedAgain->GetRenderData()->LODResources.Num() > 0
			&& ReducedAgain->GetRenderData()->LODResources[0].GetNumTriangles() > 0);
	}

	// The spline bake, the same way.
	{
		FSplineWorld Fixture;
		if (TestTrue(TEXT("a world with a spline mesh over the cube"), Fixture.IsReady()))
		{
			TArray<USplineMeshComponent*> Components;
			Components.Add(Fixture.AddSpline());
			FMantleSplineMeshBake::ClearCache();
			const FMantleSplineBakeResult First = FMantleSplineMeshBake::BakeComponents(Components, Limits, nullptr);
			TestEqual(TEXT("a spline mesh bakes"), First.Meshes.Num(), 1);
			FMantlePublishCaches::Enter();
			FMantlePublishCaches::Leave();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			const FMantleSplineBakeResult Second = FMantleSplineMeshBake::BakeComponents(Components, Limits, nullptr);
			if (TestEqual(TEXT("and bakes again after the caches went"), Second.Meshes.Num(), 1))
			{
				TestNotNull(TEXT("to a mesh"), Second.Meshes[0].Mesh);
			}
		}
	}

	FMantleSplineMeshBake::ClearCache();
	FMantleMirrorBake::ClearCache();
	FMantleMeshReduce::ClearCache();
	FMantleMeshReduce::SetReducerForTests(nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryRebakeHashTest,
	"Mantle.Editor.PublishMemory.ABakedMeshConvertsToTheSameHashAfterThePrune",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryRebakeHashTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	FMantlePublishCaches::ResetCountForTests();
	FMantlePublishCaches::Prune();
	TStrongObjectPtr<UStaticMesh> Cube(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	if (!TestTrue(TEXT("the engine cube loads"), Cube.IsValid() && Cube->GetRenderData() && Cube->GetRenderData()->LODResources.Num() > 0))
	{
		return false;
	}
	auto ConvertedHash = [this](UStaticMesh* Mesh) -> FString
	{
		FMantlePlaceAssetConverter Converter;
		Converter.Limits = FMantlePlaceAssetLimits();
		FMantlePlaceAssetSet Set;
		if (!TestTrue(TEXT("the baked mesh converts"), Converter.ConvertMesh(Mesh, Set)) || Set.Meshes.Num() != 1)
		{
			return FString();
		}
		return Set.Meshes[0].Hash;
	};

	// The mirror bake. The second bake happens while the first copy still exists, so the engine must give it another name.
	{
		FVector PositiveScale;
		FString Why;
		UStaticMesh* First = FMantleMirrorBake::MirroredCopy(Cube.Get(), FVector(-1.0, 1.0, 1.0), PositiveScale, Why);
		if (!TestNotNull(TEXT("a mirrored copy"), First))
		{
			return false;
		}
		const FName FirstName = First->GetFName();
		const FString FirstHash = ConvertedHash(First);
		FMantlePublishCaches::Enter();
		FMantlePublishCaches::Leave();
		UStaticMesh* Second = FMantleMirrorBake::MirroredCopy(Cube.Get(), FVector(-1.0, 1.0, 1.0), PositiveScale, Why);
		if (TestNotNull(TEXT("baked again"), Second))
		{
			TestTrue(TEXT("a different object"), Second != First);
			TestTrue(TEXT("with another name (the counter only grows)"), Second->GetFName() != FirstName);
			const FString SecondHash = ConvertedHash(Second);
			TestFalse(TEXT("the first conversion has a hash"), FirstHash.IsEmpty());
			TestEqual(TEXT("and the mirrored mesh hashes the same, so a second publish uploads nothing again"), SecondHash, FirstHash);
		}
	}

	// The spline bake, the same way.
	{
		FSplineWorld Fixture;
		if (TestTrue(TEXT("a world with a spline mesh over the cube"), Fixture.IsReady()))
		{
			TArray<USplineMeshComponent*> Components;
			Components.Add(Fixture.AddSpline());
			FMantleSplineMeshBake::ClearCache();
			const FMantleSplineBakeResult First = FMantleSplineMeshBake::BakeComponents(Components, FMantlePlaceAssetLimits(), nullptr);
			if (TestEqual(TEXT("a spline mesh bakes"), First.Meshes.Num(), 1))
			{
				const FName FirstName = First.Meshes[0].Mesh->GetFName();
				const FString FirstHash = ConvertedHash(First.Meshes[0].Mesh);
				FMantlePublishCaches::Enter();
				FMantlePublishCaches::Leave();
				const FMantleSplineBakeResult Second = FMantleSplineMeshBake::BakeComponents(Components, FMantlePlaceAssetLimits(), nullptr);
				if (TestEqual(TEXT("and bakes again"), Second.Meshes.Num(), 1))
				{
					TestTrue(TEXT("with another name"), Second.Meshes[0].Mesh->GetFName() != FirstName);
					TestFalse(TEXT("the first conversion has a hash"), FirstHash.IsEmpty());
					TestEqual(TEXT("and hashes the same"), ConvertedHash(Second.Meshes[0].Mesh), FirstHash);
				}
			}
		}
	}
	FMantlePublishCaches::Prune();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryBakedNameTest,
	"Mantle.Editor.PublishMemory.ABakedMeshKeepsItsIntendedNameAndTwoSplineChunksKeepDistinctOnes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryBakedNameTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	UObject* Outer = GetTransientPackage();

	// The helper: whatever the name ends in, the written name is the intended one, and two live copies of one name do not collide.
	TArray<UStaticMesh*> Made;
	for (const TCHAR* Intended : { TEXT("MemoryTestBaked_ab12cd34"), TEXT("MemoryTestBaked_ab12cd3_1"), TEXT("MemoryTestBaked_12345678"), TEXT("MemoryTestBaked_ab12cd34_0") })
	{
		UStaticMesh* First = NewObject<UStaticMesh>(Outer, FMantlePlaceAssets::UniqueBakedMeshName(Outer, Intended), RF_Transient);
		UStaticMesh* Second = NewObject<UStaticMesh>(Outer, FMantlePlaceAssets::UniqueBakedMeshName(Outer, Intended), RF_Transient);
		if (TestNotNull(TEXT("a baked mesh is made"), First) && TestNotNull(TEXT("and a second under the same intended name"), Second))
		{
			TestTrue(TEXT("their object names differ"), First->GetFName() != Second->GetFName());
			TestEqual(TEXT("the first is written under the intended name"), FMantlePlaceAssets::PublishedMeshName(*First), FString(Intended));
			TestEqual(TEXT("so is the second"), FMantlePlaceAssets::PublishedMeshName(*Second), FString(Intended));
		}
	}

	// Two chunks of one spline group differ by their chunk index, which the name keeps. The limits are small enough for two cubes to need two meshes.
	FSplineWorld Fixture;
	if (TestTrue(TEXT("a world with spline meshes over the cube"), Fixture.IsReady()))
	{
		FMantlePublishCaches::ResetCountForTests();
		FMantleSplineMeshBake::ClearCache();
		TArray<USplineMeshComponent*> Components;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			USplineMeshComponent* Component = Fixture.AddSpline();
			Component->SetWorldLocation(FVector(20000.0 * Index, 0.0, 0.0));
			Components.Add(Component);
		}
		// The bake cuts a group into chunks where adding a component would pass the triangle cap, which is the smaller of its own
		// 250,000 and the place limit (BakeComponents: TriangleCap, then the Morton-ordered chunking). A limit of one and a half cubes
		// lets each cube bake alone (a source over the cap is refused) and keeps two from sharing a mesh.
		const int32 CubeTriangles = static_cast<int32>(Fixture.Cube->GetRenderData()->LODResources[0].GetNumTriangles());
		FMantlePlaceAssetLimits Limits;
		Limits.MaxTriangles = CubeTriangles + CubeTriangles / 2;
		const FMantleSplineBakeResult Result = FMantleSplineMeshBake::BakeComponents(Components, Limits, nullptr);
		for (const FMantleSplineBakeProblem& Problem : Result.Problems)
		{
			AddError(FString::Printf(TEXT("the spline bake reported: '%s' %s"), *Problem.OwnerLabel, *Problem.What));
		}
		if (TestEqual(TEXT("two cubes over a limit of one and a half cubes bake into two meshes"), Result.Meshes.Num(), 2))
		{
			const FString NameA = FMantlePlaceAssets::PublishedMeshName(*Result.Meshes[0].Mesh);
			const FString NameB = FMantlePlaceAssets::PublishedMeshName(*Result.Meshes[1].Mesh);
			TestTrue(TEXT("the chunks are written under different names"), NameA != NameB);
			TestTrue(TEXT("each ends in its chunk index"), (NameA.EndsWith(TEXT("_0")) && NameB.EndsWith(TEXT("_1"))) || (NameA.EndsWith(TEXT("_1")) && NameB.EndsWith(TEXT("_0"))));
		}
		FMantleSplineMeshBake::ClearCache();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryReduceReasonTest,
	"Mantle.Editor.PublishMemory.AMeshThatCannotBeReducedSaysWhyInTheErrorsRemedy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryReduceReasonTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	TStrongObjectPtr<UStaticMesh> Cube(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	if (!TestTrue(TEXT("the engine cube loads"), Cube.IsValid() && Cube->GetRenderData() && Cube->GetRenderData()->LODResources.Num() > 0))
	{
		return false;
	}
	FMantleMeshReduce::ClearCache();
	FMantleMeshReduce::SetUnavailableForTests(true);
	FMantlePlaceAssetLimits Limits;
	Limits.MaxTriangles = 5;
	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMesh(Cube.Get(), FString(), Limits, Issues);
	FMantleMeshReduce::SetUnavailableForTests(false);
	FMantleMeshReduce::ClearCache();

	const FMantleValidationIssue* Error = Issues.FindByPredicate([](const FMantleValidationIssue& Issue) { return Issue.Severity == EMantleIssueSeverity::Error; });
	if (TestNotNull(TEXT("a cube over a 5 triangle limit with no reducer is an error"), Error))
	{
		TestTrue(TEXT("whose remedy still names the mesh editor's tools"), Error->Remedy.Contains(TEXT("LOD tools")));
		TestTrue(TEXT("and says in plain words why Mantle could not do it"),
			Error->Remedy.Contains(TEXT("Mantle couldn't reduce it automatically: the editor has no static mesh reducer.")));
	}
	return true;
}

namespace MantlePublishMemoryTestPrivate
{
	/** A flattener that writes a glTF with one base colour image, so a material converts with one texture and no renderer. */
	class FScriptedFlattener : public IMantleMaterialFlattener
	{
	public:
		TArray<uint8> Png;

		virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
			TArray<FString>& OutMessages, FString& OutError) override
		{
			const TSharedRef<FJsonObject> BaseColor = MakeShared<FJsonObject>();
			BaseColor->SetNumberField(TEXT("index"), 0);
			const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
			Pbr->SetObjectField(TEXT("baseColorTexture"), BaseColor);
			const TSharedRef<FJsonObject> GltfMaterial = MakeShared<FJsonObject>();
			GltfMaterial->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
			const TSharedRef<FJsonObject> GltfTexture = MakeShared<FJsonObject>();
			GltfTexture->SetNumberField(TEXT("source"), 0);
			const TSharedRef<FJsonObject> GltfImage = MakeShared<FJsonObject>();
			GltfImage->SetNumberField(TEXT("bufferView"), 0);
			GltfImage->SetStringField(TEXT("mimeType"), TEXT("image/png"));
			const TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
			View->SetNumberField(TEXT("buffer"), 0);
			View->SetNumberField(TEXT("byteOffset"), 0);
			View->SetNumberField(TEXT("byteLength"), Png.Num());

			auto One = [](const TSharedRef<FJsonObject>& Object)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				Values.Add(MakeShared<FJsonValueObject>(Object));
				return Values;
			};
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetArrayField(TEXT("materials"), One(GltfMaterial));
			Root->SetArrayField(TEXT("textures"), One(GltfTexture));
			Root->SetArrayField(TEXT("images"), One(GltfImage));
			Root->SetArrayField(TEXT("bufferViews"), One(View));
			OutDocument.Json = Root;
			OutDocument.Bin = Png;
			return true;
		}

		virtual FString Describe() const override { return TEXT("scripted publish memory test"); }
	};

	TArray<uint8> TestPng()
	{
		TArray<FColor> Pixels;
		for (int32 Y = 0; Y < 16; ++Y)
		{
			for (int32 X = 0; X < 16; ++X)
			{
				Pixels.Add(FColor(static_cast<uint8>(X * 15), static_cast<uint8>(Y * 15), static_cast<uint8>((X ^ Y) * 15), 255));
			}
		}
		return FMantlePng::EncodeRGBA8(16, 16, Pixels);
	}

	/** Every file under Directory, with its size: a cache that holds an empty texture file shows here. */
	void ZeroByteFiles(const FString& Directory, TArray<FString>& OutFiles, int32& OutTotal)
	{
		OutFiles.Reset();
		TArray<FString> All;
		IFileManager::Get().FindFilesRecursive(All, *Directory, TEXT("*.*"), true, false);
		OutTotal = All.Num();
		for (const FString& File : All)
		{
			if (IFileManager::Get().FileSize(*File) <= 0)
			{
				OutFiles.Add(File);
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryCacheTest,
	"Mantle.Editor.PublishMemory.TheConversionCacheNeverHoldsAnEmptyTextureAfterATakeOrASpill",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryCacheTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	if (!TestNotNull(TEXT("an engine material loads"), Material))
	{
		return false;
	}
	const TSharedRef<FScriptedFlattener> Flattener = MakeShared<FScriptedFlattener>();
	Flattener->Png = TestPng();
	const FString Root = TestDirectory(TEXT("MaterialCache"));

	// Control: a plain conversion fills the cache with a real texture, and the cache hands it back.
	FMantlePlaceAssetSet Reference;
	{
		FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(Root, TEXT("Plain")));
		if (!TestTrue(TEXT("the material converts"), Converter.ConvertMaterial(Material, Reference)) || !TestTrue(TEXT("with a texture"), Reference.Textures.Num() > 0))
		{
			return false;
		}
		TArray<FString> Empty;
		int32 Total = 0;
		ZeroByteFiles(FPaths::Combine(Root, TEXT("Plain")), Empty, Total);
		TestTrue(TEXT("control: the cache was written"), Total >= 2);
		TestEqual(TEXT("with no empty file"), Empty.Num(), 0);
	}

	for (const bool bSpill : { false, true })
	{
		const FString CacheDirectory = FPaths::Combine(Root, bSpill ? TEXT("AfterSpill") : TEXT("AfterTake"));
		// An earlier step converted the texture and the publish has since handed its bytes on.
		FMantlePlaceAssetSet Set;
		FMantleConvertedTexture Copy = Reference.Textures[0];
		Set.AddTexture(MoveTemp(Copy));
		FString Error;
		if (bSpill)
		{
			Set.SpillBytes(FPaths::Combine(Root, TEXT("Spilled")), /*MinBytes*/ 0, Error);
		}
		else
		{
			TArray<FMantlePlaceUpload> Uploads;
			Set.TakeUploads(EMantlePlaceUploadStage::Texture, Uploads, Error);
		}
		if (!TestTrue(TEXT("its bytes are no longer in the set"), Set.Textures[0].Bytes.IsEmpty()))
		{
			return false;
		}

		FMantlePlaceAssetConverter Converter(Flattener, CacheDirectory);
		TestTrue(TEXT("a material that uses it still converts"), Converter.ConvertMaterial(Material, Set));
		TArray<FString> Empty;
		int32 Total = 0;
		ZeroByteFiles(CacheDirectory, Empty, Total);
		TestEqual(bSpill ? TEXT("no empty cache file after a spill") : TEXT("no empty cache file after a take"), Empty.Num(), 0);

		// And what the cache holds still converts to a real texture.
		FMantlePlaceAssetSet Later;
		FMantlePlaceAssetConverter Again(Flattener, CacheDirectory);
		TestTrue(TEXT("the material converts again from that cache"), Again.ConvertMaterial(Material, Later));
		for (const FMantleConvertedTexture& Texture : Later.Textures)
		{
			TestFalse(TEXT("with a texture that has bytes"), Texture.Bytes.IsEmpty());
		}
	}
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemorySpillPolicyTest,
	"Mantle.Editor.PublishMemory.SpillTurnsOnByItselfSweepsOldFoldersAndAFileThatVanishedHasARemedy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemorySpillPolicyTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	const FString Root = TestDirectory(TEXT("SpillPolicy"));

	// Past the threshold, a publish that was not asked to spill does.
	{
		FSessionRig Rig;
		Rig.Source->SpillRoot = Root;
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Timing.SpillMinBytes = 1;
		Rig.Session->Timing.AutoSpillBytes = 2000;
		Rig.Session->Start();
		TestEqual(TEXT("it is uploading"), static_cast<int32>(Rig.Session->GetPhase()), static_cast<int32>(EMantlePublishPhase::Uploading));
		TestEqual(TEXT("the converted files (5,800 bytes) are over the threshold, so all three were spilled"), Rig.Session->GetAssetSet().Spilled.Num(), 3);
		TestEqual(TEXT("and the queue holds none"), Rig.Session->GetQueue()->GetHeldBytes(), static_cast<int64>(0));
		TestTrue(TEXT("the publish finishes"), Rig.RunToEnd());
		TestTrue(TEXT("and succeeds"), Rig.Session->Succeeded());
	}
	{
		FSessionRig Rig;
		Rig.Source->SpillRoot = Root;
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Timing.SpillMinBytes = 1;
		Rig.Session->Timing.AutoSpillBytes = 1LL << 30;
		Rig.Session->Start();
		TestEqual(TEXT("control: under the threshold nothing is spilled"), Rig.Session->GetAssetSet().Spilled.Num(), 0);
		TestTrue(TEXT("the publish finishes"), Rig.RunToEnd());
	}

	// Folders an earlier publish left behind go, young ones stay.
	{
		const FString OldFolder = FPaths::Combine(Root, TEXT("00000000000000000000000000000001"));
		const FString NewFolder = FPaths::Combine(Root, TEXT("00000000000000000000000000000002"));
		const FString OldFile = FPaths::Combine(OldFolder, TEXT("a.bin"));
		FFileHelper::SaveArrayToFile(Payload(70, 10), *OldFile);
		FFileHelper::SaveArrayToFile(Payload(71, 10), *FPaths::Combine(NewFolder, TEXT("b.bin")));
		IFileManager::Get().SetTimeStamp(*OldFile, FDateTime::UtcNow() - FTimespan::FromDays(3));
		TestEqual(TEXT("one stale folder is swept"), FMantlePlacePublishSession::SweepStaleSpillFolders(Root, FTimespan::FromDays(1)), 1);
		TestFalse(TEXT("the old one is gone"), IFileManager::Get().DirectoryExists(*OldFolder));
		TestTrue(TEXT("the new one stays"), IFileManager::Get().DirectoryExists(*NewFolder));

		// A session sweeps at its start.
		FFileHelper::SaveArrayToFile(Payload(72, 10), *OldFile);
		IFileManager::Get().SetTimeStamp(*OldFile, FDateTime::UtcNow() - FTimespan::FromDays(3));
		FSessionRig Rig;
		Rig.Source->SpillRoot = Root;
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Start();
		TestFalse(TEXT("a starting publish deletes what a killed one left"), IFileManager::Get().DirectoryExists(*OldFolder));
		TestTrue(TEXT("the publish finishes"), Rig.RunToEnd());
	}

	// A spilled file that vanishes before it is sent stops the publish with a row that says what to do.
	{
		const FString VanishRoot = TestDirectory(TEXT("SpillVanish"));
		FSessionRig Rig;
		Rig.Source->SpillRoot = VanishRoot;
		Rig.Make(/*bGoLive*/ false);
		Rig.Session->Timing.bSpillConvertedBytes = true;
		Rig.Session->Timing.SpillMinBytes = 1;
		Rig.Session->Start();
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *VanishRoot, TEXT("*.bin"), true, false);
		TestEqual(TEXT("the prepared files are on disk"), Files.Num(), 3);
		for (const FString& File : Files)
		{
			IFileManager::Get().Delete(*File, false, true, true);
		}
		TestTrue(TEXT("the session ends"), Rig.RunToEnd());
		TestEqual(TEXT("in failure"), static_cast<int32>(Rig.Session->GetPhase()), static_cast<int32>(EMantlePublishPhase::Failed));
		const FMantleValidationIssue* Row = Rig.Session->GetAttention().FindByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Remedy == TEXT("Publish again; it prepares the file again.");
		});
		TestNotNull(TEXT("a row tells the creator to publish again"), Row);
	}
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishMemoryPreviewScopeTest,
	"Mantle.Editor.PublishMemory.ThePreviewExportLeavesTheCachesBalanced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishMemoryPreviewScopeTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishMemoryTestPrivate;
	FMantlePublishCaches::ResetCountForTests();
	{
		const FMantlePublishCaches::FScope Outer;
		TestEqual(TEXT("a scope counts one"), FMantlePublishCaches::NumActive(), 1);
		{
			const FMantlePublishCaches::FScope Inner;
			TestEqual(TEXT("and a second, two"), FMantlePublishCaches::NumActive(), 2);
		}
		TestEqual(TEXT("leaving the inner one leaves the outer"), FMantlePublishCaches::NumActive(), 1);
	}
	TestEqual(TEXT("and none when both are gone"), FMantlePublishCaches::NumActive(), 0);

	// The preview export holds one for its whole run, whichever way it ends.
	FMantlePreviewExportOptions Options;
	FMantlePreviewExportResult Result;
	TestFalse(TEXT("with no level it refuses"), FMantlePreviewExport::Run(nullptr, Options, Result));
	TestEqual(TEXT("and leaves the count as it was"), FMantlePublishCaches::NumActive(), 0);
	FSplineWorld Fixture;
	if (TestNotNull(TEXT("a game world"), Fixture.World))
	{
		FMantlePreviewExportResult Other;
		TestFalse(TEXT("a world that is not the editor's it refuses"), FMantlePreviewExport::Run(Fixture.World, Options, Other));
		TestEqual(TEXT("and leaves the count as it was"), FMantlePublishCaches::NumActive(), 0);
	}

	// A preview running beside a publish: the publish ending does not take the bakes from it.
	{
		TStrongObjectPtr<UStaticMesh> Cube(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		if (Cube.IsValid())
		{
			const FMantlePublishCaches::FScope Preview;
			FVector PositiveScale;
			FString Why;
			UStaticMesh* Baked = FMantleMirrorBake::MirroredCopy(Cube.Get(), FVector(-1.0, 1.0, 1.0), PositiveScale, Why);
			FMantlePublishCaches::Enter();
			FMantlePublishCaches::Leave();
			TestTrue(TEXT("the bake is still the cached one"), FMantleMirrorBake::MirroredCopy(Cube.Get(), FVector(-1.0, 1.0, 1.0), PositiveScale, Why) == Baked);
		}
		TestEqual(TEXT("and everything is given back"), FMantlePublishCaches::NumActive(), 0);
	}
	return true;
}

#endif
