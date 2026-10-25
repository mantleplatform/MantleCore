// Copyright Joshua Eger. All Rights Reserved.
#include "MantleActorDecompose.h"
#include "MantleEffectExport.h"
#include "MantleCoverageSurvey.h"
#include "MantleMirrorBake.h"
#include "MantlePlaceExporter.h"
#include "MantleReflectionProbeExport.h"

#include "Components/ArrowComponent.h"
#include "Components/AudioComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/ReflectionCapture.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleActorDecomposePrivate
{
	namespace Cat = MantleCoverageCategory;

	/** The most event or function names a hint lists for one Blueprint; the rest are counted. */
	constexpr int32 MaxHintNames = 3;

	/** An actor's class as a creator names it: a Blueprint's generated class ends in "_C". */
	FString ClassNameOf(const AActor* Actor)
	{
		FString Name = Actor->GetClass()->GetName();
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	/** A Blueprint graph's wired event nodes, by name. */
	void CollectEvents(const UEdGraph* Graph, TArray<FString>& OutEvents)
	{
		if (!Graph)
		{
			return;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || !Node->IsNodeEnabled() || !FMantleActorDecompose::IsEventNode(Node))
			{
				continue;
			}
			// An event does something when its output is wired; a new Blueprint's greyed-out BeginPlay and Tick are not.
			const bool bWired = Node->Pins.ContainsByPredicate([](const UEdGraphPin* Pin)
			{
				return Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0;
			});
			if (bWired)
			{
				OutEvents.AddUnique(FMantleActorDecompose::EventNameOf(Node));
			}
		}
	}

	FString ListNames(const TArray<FString>& Names)
	{
		TArray<FString> Shown;
		for (int32 Index = 0; Index < Names.Num() && Index < MaxHintNames; ++Index)
		{
			Shown.Add(Names[Index]);
		}
		FString Text = FString::Join(Shown, TEXT(", "));
		if (Names.Num() > MaxHintNames)
		{
			Text += FString::Printf(TEXT(" and %d more"), Names.Num() - MaxHintNames);
		}
		return Text;
	}
}

bool FMantleActorDecompose::HasNegativeAxis(const FVector& Scale)
{
	return FMantleMirrorBake::HasNegativeAxis(Scale);
}

FMantleMirrorResult FMantleActorDecompose::ResolveMirror(UStaticMesh* Source, const FVector& Scale)
{
	FMantleMirrorResult Result;
	Result.PositiveScale = FVector(FMath::Abs(Scale.X), FMath::Abs(Scale.Y), FMath::Abs(Scale.Z));
	if (!Source || !HasNegativeAxis(Scale))
	{
		return Result;
	}
	Result.bNeeded = true;
	FVector Positive = Result.PositiveScale;
	Result.Mesh = FMantleMirrorBake::MirroredCopy(Source, Scale, Positive, Result.Why);
	Result.PositiveScale = Positive;
	Result.bOk = Result.Mesh != nullptr;
	if (!Result.bOk && Result.Why.IsEmpty())
	{
		Result.Why = TEXT("the mirrored copy could not be made");
	}
	return Result;
}

FMantleMirrorResult FMantleActorDecompose::MirrorForActor(const AActor* Actor)
{
	const AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor);
	const UStaticMeshComponent* Component = MeshActor ? MeshActor->GetStaticMeshComponent() : nullptr;
	if (!Component || !Component->GetStaticMesh())
	{
		return FMantleMirrorResult();
	}
	return ResolveMirror(Component->GetStaticMesh(), FMantlePlaceExporter::PublishedTransform(Actor).GetScale3D());
}

bool FMantleActorDecompose::EffectPropsFromJson(const FJsonObject& Properties, FMantleEffectProps& Out)
{
	// One reader for every key, the sprites archetype's included.
	return FMantleEffectExport::ReadProperties(Properties, Out);
}

bool FMantleActorDecompose::IsEventNode(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return false;
	}
	// By class name, so MantleEditor needs no dependency on the Blueprint graph module: K2Node_Event, K2Node_CustomEvent,
	// K2Node_ComponentBoundEvent, K2Node_InputAction and the like.
	const FString Name = Node->GetClass()->GetName();
	return Name.StartsWith(TEXT("K2Node_")) && (Name.Contains(TEXT("Event")) || Name.Contains(TEXT("Input")));
}

FString FMantleActorDecompose::EventNameOf(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}
	FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	int32 NewLine = INDEX_NONE;
	if (Title.FindChar(TEXT('\n'), NewLine))
	{
		Title.LeftInline(NewLine);
	}
	Title.TrimStartAndEndInline();
	if (Title.StartsWith(TEXT("Event "), ESearchCase::IgnoreCase))
	{
		Title.RightChopInline(6);
	}
	return Title.IsEmpty() ? Node->GetClass()->GetName() : Title;
}

bool FMantleActorDecompose::CollectBlueprintBehaviour(const AActor* Actor, TArray<FMantleBlueprintBehaviour>& Out)
{
	Out.Reset();
#if WITH_EDITORONLY_DATA
	if (!Actor)
	{
		return false;
	}
	TArray<UBlueprint*> Blueprints;
	UBlueprint::GetBlueprintHierarchyFromClass(Actor->GetClass(), Blueprints);
	for (const UBlueprint* Blueprint : Blueprints)
	{
		if (!Blueprint)
		{
			continue;
		}
		FMantleBlueprintBehaviour Level;
		Level.Blueprint = Blueprint->GetName();
		for (const UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			MantleActorDecomposePrivate::CollectEvents(Graph, Level.Events);
		}
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			// The construction script ran in the editor; what it made is already in the level.
			if (Graph && Graph->GetFName() != FName(TEXT("UserConstructionScript")) && FMantlePlaceExporter::GraphHasWiredNode(Graph))
			{
				Level.Functions.AddUnique(Graph->GetName());
			}
		}
		if (Level.Events.Num() > 0 || Level.Functions.Num() > 0)
		{
			Out.Add(MoveTemp(Level));
		}
	}
#endif
	return Out.Num() > 0;
}

FString FMantleActorDecompose::ComposeBlueprintHint(const TArray<FMantleBlueprintBehaviour>& Levels)
{
	using namespace MantleActorDecomposePrivate;
	TArray<FString> Parts;
	for (int32 Index = 0; Index < Levels.Num(); ++Index)
	{
		const FMantleBlueprintBehaviour& Level = Levels[Index];
		FString Text;
		if (Level.Events.Num() > 0)
		{
			Text = ListNames(Level.Events);
		}
		else if (Level.Functions.Num() > 0)
		{
			Text = TEXT("function ") + ListNames(Level.Functions);
		}
		else
		{
			continue;
		}
		// The first level is the actor's own Blueprint; the rest are the parents whose graphs run too.
		if (Index > 0)
		{
			Text += FString::Printf(TEXT(" from parent %s"), *Level.Blueprint);
		}
		Parts.Add(Text);
	}
	return FString::Join(Parts, TEXT("; "));
}

bool FMantleActorDecompose::DescribeBlueprintBehaviour(const AActor* Actor, FString& OutBlueprint, FString& OutDetail)
{
	OutBlueprint.Reset();
	OutDetail.Reset();
	TArray<FMantleBlueprintBehaviour> Levels;
	if (!CollectBlueprintBehaviour(Actor, Levels))
	{
		return false;
	}
	// The class the level's actor is: its own Blueprint, even when only a parent has the logic.
	TArray<UBlueprint*> Own;
#if WITH_EDITORONLY_DATA
	UBlueprint::GetBlueprintHierarchyFromClass(Actor->GetClass(), Own);
#endif
	OutBlueprint = Own.Num() > 0 && Own[0] ? Own[0]->GetName() : MantleActorDecomposePrivate::ClassNameOf(Actor);
	// A parent's logic is marked as the parent's: the leaf's own level is first only when it has some.
	if (Levels[0].Blueprint != OutBlueprint)
	{
		Levels.Insert(FMantleBlueprintBehaviour(), 0);
		Levels[0].Blueprint = OutBlueprint;
	}
	OutDetail = FString::Printf(TEXT("its behaviour isn't carried (e.g. %s)"), *ComposeBlueprintHint(Levels));
	return true;
}

bool FMantleActorDecompose::IsReflectionCapture(const AActor* Actor)
{
	return Actor && Actor->IsA(AReflectionCapture::StaticClass());
}

bool FMantleActorDecompose::IsVisualisation(const UActorComponent* Component)
{
	return Component && (Component->IsEditorOnly() || Component->IsA(UBillboardComponent::StaticClass()) || Component->IsA(UArrowComponent::StaticClass()));
}

bool FMantleActorDecompose::IsUnpublishedReflectionCapture(const AActor* Actor)
{
	return IsReflectionCapture(Actor) && !FMantleReflectionProbeExport::IsProbeActor(Actor);
}

bool FMantleActorDecompose::ReportIgnoredActor(const AActor* Actor, FMantleCoverage& Coverage)
{
	if (!IsUnpublishedReflectionCapture(Actor))
	{
		return false;
	}
	// A Box or Sphere capture publishes as a ReflectionProbe, captured on each client in the place's own lighting. A Plane
	// capture is a separate planar system the client does not carry, so it is left out.
#if WITH_EDITOR
	const FString Label = Actor->GetActorLabel();
#else
	const FString Label = Actor->GetName();
#endif
	Coverage.Add(MantleCoverageCategory::Lights, EMantleCoverageOutcome::Dropped, Label.IsEmpty() ? MantleActorDecomposePrivate::ClassNameOf(Actor) : Label,
		FString::Printf(TEXT("a %s has no place form: only Box and Sphere reflection captures publish (as reflection probes), so planar reflections are left out."),
			*MantleActorDecomposePrivate::ClassNameOf(Actor)));
	return true;
}

void FMantleActorDecompose::ReportActor(const AActor* Actor, EMantleEntityType Type, FMantleCoverage& Coverage)
{
	using namespace MantleActorDecomposePrivate;
	if (!Actor)
	{
		return;
	}

	FString Blueprint;
	FString Detail;
	if (DescribeBlueprintBehaviour(Actor, Blueprint, Detail))
	{
		Coverage.Add(Cat::BlueprintLogic, EMantleCoverageOutcome::Dropped, Blueprint, Detail);
	}

	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	// A Blueprint's billboards and arrows are editor icons: said once per class, not once per actor.
	if (Actor->GetClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint)
		&& Components.ContainsByPredicate([](const UActorComponent* Component) { return IsVisualisation(Component); }))
	{
		Coverage.Add(Cat::OtherComponents, EMantleCoverageOutcome::Info, ClassNameOf(Actor),
			TEXT("its billboard and arrow components are editor-only icons, so they are not published."));
	}

	if (Type != EMantleEntityType::Group)
	{
		return;
	}
	TArray<FMantlePrefabPart> Parts;
	FMantlePlaceExporter::CollectPrefabParts(Actor, Parts);
	if (Parts.Num() == 0)
	{
		return;
	}
	for (const UActorComponent* Component : Components)
	{
		// Parts publish; content a player could see is named by CollectPrefabParts' unsupported list; audio, particles
		// and instanced meshes are reported on their own; a plain scene component is only a transform.
		if (!Component || Component->HasAnyFlags(RF_Transient) || IsVisualisation(Component)
			|| Component->GetClass() == USceneComponent::StaticClass()
			|| Component->IsA(UPrimitiveComponent::StaticClass()) || Component->IsA(ULightComponentBase::StaticClass())
			|| Component->IsA(UAudioComponent::StaticClass())
			|| Parts.ContainsByPredicate([Component](const FMantlePrefabPart& Part) { return Part.Component == Component; }))
		{
			continue;
		}
		Coverage.Add(FMantleCoverageSurvey::ComponentCategory(Component), EMantleCoverageOutcome::Dropped, Component->GetClass()->GetName(),
			FString::Printf(TEXT("on %s: Mantle does not publish this component type."), *ClassNameOf(Actor)));
	}
}
