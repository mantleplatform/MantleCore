// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTerrainCollision.h"

#include "MantleRuntime.h"
#include "MantleTerrainSurface.h"

#include "AI/Navigation/NavigationTypes.h"
#include "AI/NavigationSystemHelpers.h"
#include "Chaos/HeightField.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Chaos/ParticleHandle.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeRWLock.h"
#include <atomic>
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Physics/PhysicsFiltering.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "Physics/PhysicsInterfaceScene.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PhysicsPublic.h"

// ---- The pure part --------------------------------------------------------------------------------------

void FMantleTerrainCollision::BlockHeights(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last, TArray<uint16>& Out)
{
	const int32 Columns = Last.X - First.X + 1;
	const int32 Rows = Last.Y - First.Y + 1;
	Out.Reset();
	if (Columns < 2 || Rows < 2 || First.X < 0 || First.Y < 0 || Last.X >= Field.Width || Last.Y >= Field.Height)
	{
		return;
	}
	Out.SetNumUninitialized(Columns * Rows);
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		FMemory::Memcpy(Out.GetData() + static_cast<int64>(Row) * Columns, Field.Heights.GetData() + static_cast<int64>(First.Y + Row) * Field.Width + First.X, sizeof(uint16) * Columns);
	}
}

void FMantleTerrainCollision::BlockMaterialIndices(const FIntPoint& First, const FIntPoint& Last, const FMantleTerrainHoleGrid* Holes, TArray<uint8>& Out)
{
	Out.Reset();
	const int32 Columns = Last.X - First.X + 1;
	const int32 Rows = Last.Y - First.Y + 1;
	bool bAnyHole = false;
	if (Holes && Columns >= 2 && Rows >= 2)
	{
		for (int32 Y = First.Y; Y < Last.Y && !bAnyHole; ++Y)
		{
			for (int32 X = First.X; X < Last.X; ++X)
			{
				if (Holes->QuadIsHole(X, Y))
				{
					bAnyHole = true;
					break;
				}
			}
		}
	}
	if (!bAnyHole)
	{
		Out.Add(0);
		return;
	}
	Out.SetNumUninitialized((Columns - 1) * (Rows - 1));
	for (int32 Row = 0; Row + 1 < Rows; ++Row)
	{
		for (int32 Column = 0; Column + 1 < Columns; ++Column)
		{
			Out[Row * (Columns - 1) + Column] = Holes->QuadIsHole(First.X + Column, First.Y + Row) ? HoleMaterialIndex : 0;
		}
	}
}

bool FMantleTerrainCollision::BlockIsAllHole(const FIntPoint& First, const FIntPoint& Last, const FMantleTerrainHoleGrid* Holes)
{
	if (!Holes)
	{
		return false;
	}
	for (int32 Y = First.Y; Y < Last.Y; ++Y)
	{
		for (int32 X = First.X; X < Last.X; ++X)
		{
			if (!Holes->QuadIsHole(X, Y))
			{
				return false;
			}
		}
	}
	return Last.X > First.X && Last.Y > First.Y;
}

double FMantleTerrainCollision::RoundingBoundCm(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last)
{
	if (First.X < 0 || First.Y < 0 || Last.X >= Field.Width || Last.Y >= Field.Height || Last.X < First.X || Last.Y < First.Y)
	{
		return 0.0;
	}
	uint16 Lowest = TNumericLimits<uint16>::Max();
	uint16 Highest = 0;
	for (int32 Y = First.Y; Y <= Last.Y; ++Y)
	{
		for (int32 X = First.X; X <= Last.X; ++X)
		{
			const uint16 Value = Field.Heights[Y * Field.Width + X];
			Lowest = FMath::Min(Lowest, Value);
			Highest = FMath::Max(Highest, Value);
		}
	}
	return static_cast<double>(Highest - Lowest) * static_cast<double>(Field.HeightScale) / 65535.0;
}

void FMantleTerrainCollision::BlockNavigationHeights(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last, TArray<int16>& Out)
{
	TArray<uint16> Values;
	BlockHeights(Field, First, Last, Values);
	Out.SetNumUninitialized(Values.Num());
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		Out[Index] = static_cast<int16>(static_cast<int32>(Values[Index]) - HeightBias);
	}
}

// ---- The component ---------------------------------------------------------------------------------------

struct UMantleTerrainCollisionComponent::FImpl
{
	Chaos::FHeightFieldPtr HeightField;
	int32 Rows = 0;
	int32 Columns = 0;
	uint16 LowestValue = 0;
	uint16 HighestValue = 0;
	int64 ShapeBytes = 0;
	/** The height field is in a body in the scene (or was): a body made again gets a shape of its own, so a shape in the scene is never changed (physics runs on another thread). */
	bool bShapeGivenToABody = false;

	/** What the navigation system reads (made once, on the game thread, before the first slice is asked for; read from other threads). */
	FRWLock NavigationLock;
	FNavHeightfieldSamples NavigationSamples;
	/** The component's transform as of the last PrepareGeometryExportSync (the game thread), so a slice gathered on another thread never reads it while a Movable terrain moves. */
	FTransform NavigationTransform;
};

namespace MantleTerrainCollisionPrivate
{
	/** A height field of a block's samples: the spacing and height scale on it, not yet in any scene. */
	Chaos::FHeightFieldPtr MakeBlockHeightField(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last, const FMantleTerrainHoleGrid* Holes, const int32 Rows, const int32 Columns)
	{
		TArray<uint16> Heights;
		FMantleTerrainCollision::BlockHeights(Field, First, Last, Heights);
		TArray<uint8> Materials;
		FMantleTerrainCollision::BlockMaterialIndices(First, Last, Holes, Materials);
		return Chaos::FHeightFieldPtr(new Chaos::FHeightField(TArrayView<const uint16>(Heights), TArrayView<const uint8>(Materials), Rows, Columns,
			Chaos::FVec3(Field.SpacingX, Field.SpacingY, Field.HeightScale)));
	}
}

UMantleTerrainCollisionComponent::UMantleTerrainCollisionComponent()
{
	// The collision of the chunks it replaced: block everything, complex and simple; no overlap events, no shadow, nothing drawn.
	SetCollisionProfileName(UCollisionProfile::CustomCollisionProfileName);
	SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	SetCollisionResponseToAllChannels(ECR_Block);
	SetGenerateOverlapEvents(false);
	CastShadow = false;
	bUseAsOccluder = false;
	bAllowCullDistanceVolume = false;
	Mobility = EComponentMobility::Static;
	bCanEverAffectNavigation = true;
	bHasCustomNavigableGeometry = EHasCustomNavigableGeometry::Yes;
}

void UMantleTerrainCollisionComponent::Init(TSharedPtr<const FMantleHeightfield> InField, const FIntPoint& InFirst, const FIntPoint& InLast, TSharedPtr<const FMantleTerrainHoleGrid> InHoles)
{
	Field = MoveTemp(InField);
	Holes = MoveTemp(InHoles);
	First = InFirst;
	Last = InLast;
	Impl = MakeShared<FImpl>();
	if (!Field.IsValid())
	{
		return;
	}
	TArray<uint16> Heights;
	FMantleTerrainCollision::BlockHeights(*Field, First, Last, Heights);
	if (Heights.Num() == 0)
	{
		return;
	}
	Impl->Columns = Last.X - First.X + 1;
	Impl->Rows = Last.Y - First.Y + 1;
	Impl->LowestValue = TNumericLimits<uint16>::Max();
	Impl->HighestValue = 0;
	for (const uint16 Value : Heights)
	{
		Impl->LowestValue = FMath::Min(Impl->LowestValue, Value);
		Impl->HighestValue = FMath::Max(Impl->HighestValue, Value);
	}
	// The shape's own scale is the terrain's spacing and height scale times the component's world scale; it is set when the body is made (the world scale is known then).
	Impl->HeightField = MantleTerrainCollisionPrivate::MakeBlockHeightField(*Field, First, Last, Holes.Get(), Impl->Rows, Impl->Columns);
	// The shape keeps the heights (2 bytes a sample), a min and a max for each block of 6 x 6, and the material indices.
	Impl->ShapeBytes = static_cast<int64>(Impl->HeightField->GeomData.Heights.GetAllocatedSize()) + Impl->HeightField->GeomData.LowResolutionHeights.GetAllocatedSize()
		+ Impl->HeightField->GeomData.MaterialIndices.GetAllocatedSize();
}

FVector UMantleTerrainCollisionComponent::GetRelativeOrigin() const
{
	if (!Field.IsValid())
	{
		return FVector::ZeroVector;
	}
	return FVector(static_cast<double>(First.X) * Field->SpacingX, static_cast<double>(First.Y) * Field->SpacingY,
		static_cast<double>(Field->HeightOffset) + static_cast<double>(FMantleTerrainCollision::HeightBias) * Field->HeightScale);
}

bool UMantleTerrainCollisionComponent::HasShape() const
{
	return Impl.IsValid() && Impl->HeightField.IsValid();
}

int64 UMantleTerrainCollisionComponent::GetShapeBytes() const
{
	return Impl.IsValid() ? Impl->ShapeBytes : 0;
}

#if !UE_BUILD_SHIPPING
double UMantleTerrainCollisionComponent::MeasureRoundingCm(double& OutBoundCm, bool& bOutAnyAbove) const
{
	OutBoundCm = 0.0;
	bOutAnyAbove = false;
	if (!HasShape() || !Field.IsValid())
	{
		return 0.0;
	}
	OutBoundCm = FMantleTerrainCollision::RoundingBoundCm(*Field, First, Last);
	double Largest = 0.0;
	for (int32 Row = 0; Row < Impl->Rows; ++Row)
	{
		for (int32 Column = 0; Column < Impl->Columns; ++Column)
		{
			const double InFile = (static_cast<double>(Field->Heights[static_cast<int64>(First.Y + Row) * Field->Width + First.X + Column]) - FMantleTerrainCollision::HeightBias) * Field->HeightScale;
			// The shape's scale carries the component's world scale (SetScale at the body's creation): the file's height has none, so it is taken out.
			const double InShape = Impl->HeightField->GetPointScaled(Row * Impl->Columns + Column).Z / FMath::Max(Impl->bShapeGivenToABody ? FMath::Abs(AppliedScale.Z) : 1.0, UE_DOUBLE_SMALL_NUMBER);
			bOutAnyAbove = bOutAnyAbove || InShape > InFile + 1.0e-4;
			Largest = FMath::Max(Largest, InFile - InShape);
		}
	}
	return Largest;
}
#endif

bool UMantleTerrainCollisionComponent::ShouldCreatePhysicsState() const
{
	return HasShape() && Super::ShouldCreatePhysicsState();
}

FBoxSphereBounds UMantleTerrainCollisionComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!Field.IsValid() || !Impl.IsValid() || Impl->Columns < 2)
	{
		return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.0);
	}
	// The block in its own space: from its first sample, with the height field's bias taken out of the heights (the component sits at the bias).
	const double Bias = FMantleTerrainCollision::HeightBias;
	const FVector Low(0.0, 0.0, (static_cast<double>(Impl->LowestValue) - Bias) * Field->HeightScale);
	const FVector High(static_cast<double>(Impl->Columns - 1) * Field->SpacingX, static_cast<double>(Impl->Rows - 1) * Field->SpacingY, (static_cast<double>(Impl->HighestValue) - Bias) * Field->HeightScale);
	return FBoxSphereBounds(FBox(Low, High).TransformBy(LocalToWorld));
}

void UMantleTerrainCollisionComponent::OnCreatePhysicsState()
{
	// The landscape's own steps (LandscapeCollision.cpp, ULandscapeHeightfieldCollisionComponent::OnCreatePhysicsState): the primitive component's body is skipped, because this component has no
	// body setup; one actor with one height-field shape is made and added to the scene instead.
	USceneComponent::OnCreatePhysicsState();
	if (BodyInstance.IsValidBodyInstance() || !HasShape() || !GetWorld())
	{
		return;
	}
	FPhysScene* PhysScene = GetWorld()->GetPhysicsScene();
	if (!PhysScene)
	{
		return;
	}
	const FTransform BlockToWorld = GetComponentTransform();
	const FVector WorldScale = BlockToWorld.GetScale3D();
	AppliedScale = WorldScale;
	// The body instance carries the scale the shape was made at: a later move (UPrimitiveComponent::SendPhysicsTransform) asks it to update its body scale, and with a scale other than 1
	// it would find no shape of its own to scale (a height field is only the landscape's) and trip an engine check.
	BodyInstance.Scale3D = WorldScale;
	const bool bStaticBody = Mobility == EComponentMobility::Static;

	auto CreateShapesAndActors = [this, &BlockToWorld, &WorldScale, PhysScene, bStaticBody]()
	{
		FActorCreationParams Params;
		Params.InitialTM = BlockToWorld;
		Params.InitialTM.SetScale3D(FVector(0));
		Params.bQueryOnly = false;
		Params.bStatic = bStaticBody;
		// A Movable terrain's body is kinematic: the component moves it (OnUpdateTransform), nothing simulates it.
		Params.bSimulatePhysics = false;
		Params.Scene = PhysScene;
		FPhysicsActorHandle PhysHandle;
		FPhysicsInterface::CreateActor(Params, PhysHandle);
		Chaos::FRigidBodyHandle_External& Body_External = PhysHandle->GetGameThreadAPI();

		// The spacing and height scale were given to the shape when it was made; the component's own world scale goes on top, as the landscape puts its scale on its shape.
		if (Impl->bShapeGivenToABody)
		{
			// A body made again (a rescale, a re-registered component): the old body may still be read by the physics thread, so its shape is left as it is and this body gets its own.
			Impl->HeightField = MantleTerrainCollisionPrivate::MakeBlockHeightField(*Field, First, Last, Holes.Get(), Impl->Rows, Impl->Columns);
		}
		Impl->bShapeGivenToABody = true;
		Impl->HeightField->SetScale(Chaos::FVec3(Field->SpacingX * WorldScale.X, Field->SpacingY * WorldScale.Y, Field->HeightScale * WorldScale.Z));
		Chaos::FImplicitObjectPtr ImplicitHeightField(Impl->HeightField);
		Chaos::FImplicitObjectPtr ChaosHeightField = MakeImplicitObjectPtr<Chaos::TImplicitObjectTransformed<Chaos::FReal, 3>>(ImplicitHeightField, Chaos::FRigidTransform3(FTransform::Identity));

		Chaos::FShapesArray ShapeArray;
		TUniquePtr<Chaos::FPerShapeData> NewShape = Chaos::FShapeInstanceProxy::Make(ShapeArray.Num(), ChaosHeightField);

		// Both the simple and the complex flag, as the landscape sets when it has no separate simple shape (a script's shape cast asks for simple collision), on the channel and
		// responses the chunks' own collision had.
		FPhysicsFilterBuilder Builder(PhysScene);
		Builder.SetOwnerID(GetOwner() ? GetOwner()->GetUniqueID() : 0);
		Builder.SetComponentID(GetUniqueID());
		Builder.SetCollisionChannelIndex(GetCollisionObjectType());
		Builder.SetResponses(GetCollisionResponseToChannels());
		Builder.SetFlags(Chaos::EFilterFlags::SimpleCollision, true);
		Builder.SetFlags(Chaos::EFilterFlags::ComplexCollision, true);
		Builder.SetFlags(Chaos::EFilterFlags::CCD, true);
		Builder.SetFlags(Chaos::EFilterFlags::StaticShape, bStaticBody);
		NewShape->SetFilterInstanceData(Builder.BuildInstanceData());
		NewShape->SetShapeFilterData(Builder.BuildShapeFilterData());
		// One physical material on the whole shape, the engine's default: what a chunk's collision had. With one material the engine never reads a quad's index for a hit.
		TArray<Chaos::FMaterialHandle> Materials;
		if (GEngine && GEngine->DefaultPhysMaterial)
		{
			Materials.Add(GEngine->DefaultPhysMaterial->GetPhysicsMaterial());
		}
		NewShape->SetMaterials(Materials);

		ShapeArray.Emplace(MoveTemp(NewShape));
		Body_External.SetGeometry(ChaosHeightField);
		for (auto& Shape : ShapeArray)
		{
			const Chaos::FRigidTransform3 WorldTransform = Chaos::FRigidTransform3(Body_External.X(), Body_External.R());
			Shape->UpdateShapeBounds(WorldTransform);
		}
		Body_External.MergeShapesArray(MoveTemp(ShapeArray));

		BodyInstance.PhysicsUserData = FPhysicsUserData(&BodyInstance);
		BodyInstance.OwnerComponent = this;
		BodyInstance.SetPhysicsActor(PhysHandle);
		Body_External.SetUserData(&BodyInstance.PhysicsUserData);
		return PhysHandle;
	};

	FPhysicsActorHandle PhysHandle = CreateShapesAndActors();
	FPhysicsCommand::ExecuteWrite(PhysScene, [&PhysHandle, PhysScene]()
	{
		TArray<FPhysicsActorHandle> Actors = { PhysHandle };
		const bool bImmediateAccelStructureInsertion = true;
		PhysScene->AddActorsToScene_AssumesLocked(Actors, bImmediateAccelStructureInsertion);
	});
	PhysScene->AddToComponentMaps(this, PhysHandle);
}

void UMantleTerrainCollisionComponent::OnDestroyPhysicsState()
{
	Super::OnDestroyPhysicsState();
	if (UWorld* World = GetWorld())
	{
		if (FPhysScene_Chaos* PhysScene = World->GetPhysicsScene())
		{
			FPhysicsActorHandle ActorHandle = BodyInstance.GetPhysicsActor();
			if (FPhysicsInterface::IsValid(ActorHandle))
			{
				PhysScene->RemoveFromComponentMaps(ActorHandle);
			}
		}
	}
}

void UMantleTerrainCollisionComponent::OnUpdateTransform(const EUpdateTransformFlags UpdateTransformFlags, const ETeleportType Teleport)
{
	// A terrain scaled while it stands needs its shape made again (the shape carries the scale); a moved or turned one only moves its kinematic body.
	// The new scale is looked for first: the body's own move would ask the body instance to scale a shape it cannot scale.
	const bool bRescale = BodyInstance.IsValidBodyInstance() && !GetComponentTransform().GetScale3D().Equals(AppliedScale, UE_KINDA_SMALL_NUMBER);
	Super::OnUpdateTransform(bRescale ? (UpdateTransformFlags | EUpdateTransformFlags::SkipPhysicsUpdate) : UpdateTransformFlags, Teleport);
	if (bRescale)
	{
		RecreatePhysicsState();
	}
}

void UMantleTerrainCollisionComponent::PrepareGeometryExportSync()
{
	if (!Impl.IsValid() || !Field.IsValid() || Impl->Columns < 2)
	{
		return;
	}
	FWriteScopeLock Lock(Impl->NavigationLock);
	Impl->NavigationTransform = GetComponentTransform();
	if (!Impl->NavigationSamples.IsEmpty())
	{
		return;
	}
	// The first of these in a process is logged: geometry is handed over by slice (GetGeometryGatheringMode is Lazy), a block's samples made only when navigation first asks for them.
	static std::atomic<int32> LoggedPrepared{ 0 };
	if (LoggedPrepared.fetch_add(1) < 3)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_NAV block %d,%d: navigation geometry is gathered by slice (mode lazy); %d samples made for it (%d bytes)"), First.X, First.Y,
			Impl->Rows * Impl->Columns, Impl->Rows * Impl->Columns * static_cast<int32>(sizeof(int16)));
	}
	TArray<int16> Heights;
	FMantleTerrainCollision::BlockNavigationHeights(*Field, First, Last, Heights);
	Impl->NavigationSamples.Heights.SetNumUninitialized(Heights.Num());
	FMemory::Memcpy(Impl->NavigationSamples.Heights.GetData(), Heights.GetData(), sizeof(int16) * Heights.Num());
	const int32 Quads = (Impl->Rows - 1) * (Impl->Columns - 1);
	Impl->NavigationSamples.Holes.SetNumUninitialized(Quads);
	for (int32 Row = 0; Row + 1 < Impl->Rows; ++Row)
	{
		for (int32 Column = 0; Column + 1 < Impl->Columns; ++Column)
		{
			Impl->NavigationSamples.Holes[Row * (Impl->Columns - 1) + Column] = Holes.IsValid() && Holes->QuadIsHole(First.X + Column, First.Y + Row);
		}
	}
}

void UMantleTerrainCollisionComponent::GatherGeometrySlice(FNavigableGeometryExport& GeomExport, const FBox& SliceBox) const
{
	// Can be called off the game thread: only what PrepareGeometryExportSync made (the samples and the transform) is read.
	if (!Impl.IsValid() || !Field.IsValid())
	{
		return;
	}
	FReadScopeLock Lock(Impl->NavigationLock);
	if (Impl->NavigationSamples.IsEmpty())
	{
		return;
	}
	static std::atomic<int32> LoggedSlices{ 0 };
	if (LoggedSlices.fetch_add(1) < 3)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN_NAV slice of block %d,%d asked for (box %s)"), First.X, First.Y, *SliceBox.ToString());
	}
	FTransform GridToWorld = Impl->NavigationTransform;
	GridToWorld.MultiplyScale3D(FVector(Field->SpacingX, Field->SpacingY, Field->HeightScale));
	GeomExport.ExportChaosHeightFieldSlice(Impl->NavigationSamples, Impl->Rows, Impl->Columns, GridToWorld, SliceBox);
}
