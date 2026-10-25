// Copyright Joshua Eger. All Rights Reserved.
// The registry-driven node catalogue. "Nothing is listed by hand, so a new API member
// appears in graphs automatically" -- this walks FMantleApiManifest (mantle_api.json, generated from
// the game's API registry along with mantle.d.luau/.ts/.pyi) once, and
// turns every method, property and signal into a palette entry with typed pins. The compiler
// (MantleGraphCompiler) looks members up here instead of touching FMantleApiManifest directly, so the
// palette and the compiler can never see a different set of members.
#pragma once

#include "CoreMinimal.h"
#include "MantleScriptSide.h"

enum class EMantleGraphNodeCategory : uint8
{
	/** Opens a handler: a Signal, or a method whose last parameter is itself a handler
	 *  (World.OnTagged, Players.OnPlayer, State.Observe, ...). Graph node kind "event". */
	Event,
	/** An ordinary method call. Graph node kind "call". */
	Call,
	/** A property read. Graph node kind "get". */
	Get,
	/** A property write. Graph node kind "set". */
	Set,
};

struct MANTLEEDITOR_API FMantleGraphPinDef
{
	FString Name;
	/** The registry's own type annotation ("string", "Vector3", "Entity?", "{Entity}", "any"...). */
	FString Type;
	bool bOptional = false;
};

/** One palette entry: "ClassName.MemberName" carrying everything a node needs to be placed,
 *  type-checked and compiled without going back to FMantleApiManifest. */
struct MANTLEEDITOR_API FMantleGraphNodeDef
{
	EMantleGraphNodeCategory Category = EMantleGraphNodeCategory::Call;
	FString ClassName;
	FString MemberName;
	/** "ClassName.MemberName", the form graphs and the lab's compiler both use to name a member. */
	FString Dotted;
	/** True for a service (World, Players, State, ...): no "target" pin, the class name is the
	 *  receiver in the generated Luau. */
	bool bService = false;
	/** Event category only: true for a real Signal (`target.Name:Connect(function(...) end)`),
	 *  false for a handler-taking method (`target:Name(args, function(...) end)`), e.g.
	 *  World.OnTagged. The two compile to different Luau call shapes. */
	bool bIsSignal = false;
	/** Which side(s) may use this member. Both means no restriction. */
	EMantleSide Side = EMantleSide::Both;
	/** Data input pins, in call order. For Event category this excludes the trailing handler
	 *  parameter (its output pins, below, come from the handler's own parameter list instead). */
	TArray<FMantleGraphPinDef> Inputs;
	/** Call category only: true when extra arguments beyond Inputs are passed through
	 *  (Remote.FireClient/FireAllClients/FireServer, Signal.Fire). A node using such a member may
	 *  supply an "extra" array of values in its JSON, appended to the call after Inputs. */
	bool bVariadic = false;
	/** Data output pins: the handler's parameters for Event, a single "value" pin for Call/Get
	 *  (omitted when the method returns "()"), nothing for Set. */
	TArray<FMantleGraphPinDef> Outputs;
	FString Doc;
};

/**
 * Builds and caches the full node catalogue from FMantleApiManifest::Get() on first use. Rebuilding is
 * cheap (a few hundred entries) but there is no reason to redo it inside one compile, let alone
 * once per node -- FindByDotted is called once per member-referencing node.
 */
class MANTLEEDITOR_API FMantleGraphCatalogue
{
public:
	/** Every method/property-get/property-set/signal in the registry, as palette entries. Order
	 *  matches class registration order, then declaration order within a class, so it is
	 *  deterministic across runs. */
	static const TArray<FMantleGraphNodeDef>& All();

	/** Look up "ClassName.MemberName" for a specific node category (what the graph node's own
	 *  "kind" implies: "event" -> Event, "call" -> Call, "get" -> Get, "set" -> Set). Returns
	 *  nullptr, with OutError explaining why, when the class/member doesn't exist or exists but
	 *  isn't usable as that category (e.g. "call" pointed at a property). */
	static const FMantleGraphNodeDef* FindByDotted(const FString& Dotted, EMantleGraphNodeCategory Category, FString& OutError);

	/** True when a method's last parameter is itself a handler ("(...) -> ...", "(...any) -> ()"),
	 *  the shape that makes a method usable as an Event-category node (World.OnTagged, State.Observe). */
	static bool IsHandlerType(const FString& ParamType);

	/** Parses a doc-style signature "(name: Type, name2: Type2)" (FMantleApiManifestSignal::Args) or
	 *  "(name: Type) -> Returns" (a handler FMantleApiManifestParam::Type) into named/typed pins. Used to
	 *  build Event output pins and (via the second form) to validate a graph's declared output
	 *  count against what the handler actually receives. */
	static TArray<FMantleGraphPinDef> ParseHandlerSignature(const FString& Signature);
};
