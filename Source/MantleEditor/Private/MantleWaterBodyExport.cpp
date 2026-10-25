// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWaterBodyExport.h"

#include "MantleCoverage.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SplineComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "JsonObjectConverter.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Math/InterpCurve.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWaterBodyExportPrivate
{
	// The Water plugin is Experimental and off by default, so none of its headers are included and its module is not linked:
	// everything is found by name, and a project without the plugin reads an empty level.
	const TCHAR* const BodyActorPath = TEXT("/Script/Water.WaterBody");
	const TCHAR* const ZoneActorPath = TEXT("/Script/Water.WaterZone");
	const TCHAR* const BodyComponentPath = TEXT("/Script/Water.WaterBodyComponent");
	const TCHAR* const OceanComponentPath = TEXT("/Script/Water.WaterBodyOceanComponent");
	const TCHAR* const LakeComponentPath = TEXT("/Script/Water.WaterBodyLakeComponent");
	const TCHAR* const RiverComponentPath = TEXT("/Script/Water.WaterBodyRiverComponent");
	const TCHAR* const CustomComponentPath = TEXT("/Script/Water.WaterBodyCustomComponent");

	/** The classes of the Water plugin the reader needs; Valid() is false when the plugin is not loaded. */
	struct FWaterClasses
	{
		UClass* BodyActor = nullptr;
		UClass* ZoneActor = nullptr;
		UClass* BodyComponent = nullptr;
		UClass* Ocean = nullptr;
		UClass* Lake = nullptr;
		UClass* River = nullptr;
		UClass* Custom = nullptr;

		FWaterClasses()
		{
			BodyActor = FindObject<UClass>(nullptr, BodyActorPath);
			ZoneActor = FindObject<UClass>(nullptr, ZoneActorPath);
			BodyComponent = FindObject<UClass>(nullptr, BodyComponentPath);
			Ocean = FindObject<UClass>(nullptr, OceanComponentPath);
			Lake = FindObject<UClass>(nullptr, LakeComponentPath);
			River = FindObject<UClass>(nullptr, RiverComponentPath);
			Custom = FindObject<UClass>(nullptr, CustomComponentPath);
		}

		bool Valid() const { return BodyActor && BodyComponent && Ocean && Lake && River && Custom; }
	};

	/** Reads properties of an object or struct by name, whatever its type is, and says so when one is missing or has changed type. */
	struct FReflect
	{
		const UStruct* Struct = nullptr;
		const void* Container = nullptr;

		explicit FReflect(const UObject* Object)
			: Struct(Object ? Object->GetClass() : nullptr), Container(Object)
		{
		}

		FReflect(const UStruct* InStruct, const void* InContainer)
			: Struct(InStruct), Container(InContainer)
		{
		}

		const void* Find(const TCHAR* Name, const FProperty*& OutProperty) const
		{
			OutProperty = Struct && Container ? Struct->FindPropertyByName(Name) : nullptr;
			return OutProperty ? OutProperty->ContainerPtrToValuePtr<void>(Container) : nullptr;
		}

		bool Bool(const TCHAR* Name, bool& Out) const
		{
			const FProperty* Property = nullptr;
			const void* Value = Find(Name, Property);
			if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
			{
				Out = BoolProperty->GetPropertyValue(Value);
				return true;
			}
			return false;
		}

		bool Number(const TCHAR* Name, double& Out) const
		{
			const FProperty* Property = nullptr;
			const void* Value = Find(Name, Property);
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
			{
				if (Numeric->IsFloatingPoint())
				{
					Out = Numeric->GetFloatingPointPropertyValue(Value);
					return true;
				}
				if (Numeric->IsInteger())
				{
					Out = static_cast<double>(Numeric->GetSignedIntPropertyValue(Value));
					return true;
				}
			}
			return false;
		}

		float Float(const TCHAR* Name, const float Default = 0.f) const
		{
			double Value = Default;
			return Number(Name, Value) ? static_cast<float>(Value) : Default;
		}

		int32 Int(const TCHAR* Name, const int32 Default = 0) const
		{
			double Value = Default;
			return Number(Name, Value) ? static_cast<int32>(Value) : Default;
		}

		bool BoolOr(const TCHAR* Name, const bool Default) const
		{
			bool Value = Default;
			return Bool(Name, Value) ? Value : Default;
		}

		const UObject* Object(const TCHAR* Name) const
		{
			const FProperty* Property = nullptr;
			const void* Value = Find(Name, Property);
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				return ObjectProperty->GetObjectPropertyValue(Value);
			}
			return nullptr;
		}

		/** A struct property of exactly the type T (FVector, FVector2D, FIntPoint...), or null. */
		template<typename T>
		const T* StructOf(const TCHAR* Name) const
		{
			const FProperty* Property = nullptr;
			const void* Value = Find(Name, Property);
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			return StructProperty && StructProperty->Struct == TBaseStructure<T>::Get() ? static_cast<const T*>(Value) : nullptr;
		}

		/** One of the Water plugin's float curves over the spline's points (a FInterpCurveFloat), or null. */
		const FInterpCurveFloat* Curve(const TCHAR* Name) const
		{
			const FProperty* Property = nullptr;
			const void* Value = Find(Name, Property);
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			return StructProperty && StructProperty->Struct && StructProperty->Struct->GetFName() == TEXT("InterpCurveFloat")
				? static_cast<const FInterpCurveFloat*>(Value) : nullptr;
		}

		/** The name of an enum property's value ("QueryOnly"), or empty. */
		FString EnumName(const TCHAR* Name) const
		{
			const FProperty* Property = nullptr;
			const void* Value = Find(Name, Property);
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				return EnumProperty->GetEnum()->GetNameStringByValue(EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
			}
			if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				return ByteProperty->Enum ? ByteProperty->Enum->GetNameStringByValue(*static_cast<const uint8*>(Value)) : FString();
			}
			return FString();
		}
	};

	/** A curve's value at spline point Index: the curve key at that point, else its value there. */
	float CurveAt(const FInterpCurveFloat& Curve, const int32 Index)
	{
		for (const FInterpCurvePoint<float>& Point : Curve.Points)
		{
			if (FMath::IsNearlyEqual(Point.InVal, static_cast<float>(Index)))
			{
				return Point.OutVal;
			}
		}
		return Curve.Eval(static_cast<float>(Index), 0.f);
	}

	const TCHAR* PointTypeName(const ESplinePointType::Type Type)
	{
		switch (Type)
		{
		case ESplinePointType::Linear: return TEXT("Linear");
		case ESplinePointType::Curve: return TEXT("Curve");
		case ESplinePointType::Constant: return TEXT("Constant");
		case ESplinePointType::CurveClamped: return TEXT("CurveClamped");
		case ESplinePointType::CurveCustomTangent: return TEXT("CurveCustomTangent");
		default: return TEXT("Curve");
		}
	}

	const TCHAR* CollisionEnabledName(const ECollisionEnabled::Type Type)
	{
		switch (Type)
		{
		case ECollisionEnabled::NoCollision: return TEXT("NoCollision");
		case ECollisionEnabled::QueryOnly: return TEXT("QueryOnly");
		case ECollisionEnabled::PhysicsOnly: return TEXT("PhysicsOnly");
		case ECollisionEnabled::QueryAndPhysics: return TEXT("QueryAndPhysics");
		default: return TEXT("QueryAndProbe");
		}
	}

	/** Names that say a material parameter shapes how the water looks. */
	bool IsLookParameter(const FName Name)
	{
		static const TCHAR* const Words[] = { TEXT("Absorption"), TEXT("Scatter"), TEXT("Color"), TEXT("Colour"), TEXT("Opacity"), TEXT("Fresnel"),
			TEXT("Foam"), TEXT("Refraction"), TEXT("Albedo"), TEXT("Roughness"), TEXT("Specular"), TEXT("Turbid"), TEXT("Murk"), TEXT("Depth"), TEXT("Caustic") };
		const FString Text = Name.ToString();
		for (const TCHAR* Word : Words)
		{
			if (Text.Contains(Word, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	void ReadLook(const FReflect& Body, FMantleWaterLook& Out)
	{
		if (const UMaterialInterface* Material = Cast<UMaterialInterface>(Body.Object(TEXT("WaterMaterial"))))
		{
			Out.MaterialName = Material->GetName();
			Out.MaterialPath = Material->GetPathName();
			if (const UMaterial* Master = Material->GetMaterial())
			{
				Out.MasterName = Master->GetName();
			}

			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Material->GetAllScalarParameterInfo(Infos, Ids);
			Out.TotalParams += Infos.Num();
			for (const FMaterialParameterInfo& Info : Infos)
			{
				float Value = 0.f;
				if (IsLookParameter(Info.Name) && Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Info), Value))
				{
					FMantleWaterMaterialParam& Param = Out.Params.AddDefaulted_GetRef();
					Param.Name = Info.Name;
					Param.Scalar = Value;
				}
			}
			Infos.Reset();
			Ids.Reset();
			Material->GetAllVectorParameterInfo(Infos, Ids);
			Out.TotalParams += Infos.Num();
			for (const FMaterialParameterInfo& Info : Infos)
			{
				FLinearColor Value = FLinearColor::Black;
				if (IsLookParameter(Info.Name) && Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Info), Value))
				{
					FMantleWaterMaterialParam& Param = Out.Params.AddDefaulted_GetRef();
					Param.Name = Info.Name;
					Param.bColour = true;
					Param.Colour = Value;
				}
				// The water master's three look colours, by the names it gives them.
				const FString Key = Info.Name.ToString().Replace(TEXT(" "), TEXT(""));
				if (Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Info), Value))
				{
					if (Key.Equals(TEXT("WaterAlbedo"), ESearchCase::IgnoreCase))
					{
						Out.Albedo = Value;
					}
					else if (Key.Equals(TEXT("Scattering"), ESearchCase::IgnoreCase))
					{
						Out.Scattering = Value;
					}
					else if (Key.Equals(TEXT("Absorption"), ESearchCase::IgnoreCase))
					{
						Out.Absorption = Value;
					}
				}
			}
		}

		static const TCHAR* const Others[] = { TEXT("WaterInfoMaterial"), TEXT("WaterStaticMeshMaterial"), TEXT("WaterHLODMaterial"),
			TEXT("UnderwaterPostProcessMaterial"), TEXT("LakeTransitionMaterial"), TEXT("OceanTransitionMaterial") };
		for (const TCHAR* Property : Others)
		{
			if (const UObject* Other = Body.Object(Property))
			{
				Out.OtherMaterials.Add(Property, Other->GetPathName());
			}
		}
	}

	/** The wave source (the body's own, or the wave asset it points at) as data. */
	void ReadWaves(const FReflect& Actor, const FReflect& Body, FMantleWaterWaves& Out)
	{
		const UObject* Source = Actor.Object(TEXT("WaterWaves"));
		if (!Source)
		{
			return;
		}
		Out.bHasWaves = true;
		Out.AttenuationDepth = Body.Float(TEXT("TargetWaveMaskDepth"));
		Out.MaxWaveHeightOffset = Body.Float(TEXT("MaxWaveHeightOffset"));

		// A wave asset reference hands over to the asset, whose own wave source holds the settings.
		if (const UObject* Asset = FReflect(Source).Object(TEXT("WaterWavesAsset")))
		{
			Out.AssetPath = Asset->GetPathName();
			Source = FReflect(Asset).Object(TEXT("WaterWaves"));
			if (!Source)
			{
				Out.bHasWaves = false;
				return;
			}
		}
		Out.WavesClass = Source->GetClass()->GetName();
		const FReflect Waves(Source);
		Out.MaxWaveHeight = Waves.Float(TEXT("MaxWaveHeight"));

		if (const UObject* Generator = Waves.Object(TEXT("GerstnerWaveGenerator")))
		{
			Out.GeneratorClass = Generator->GetClass()->GetName();
			TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
			if (FJsonObjectConverter::UStructToJsonObject(Generator->GetClass(), Generator, Settings))
			{
				Out.Generator = Settings;
			}
		}

		const FProperty* Property = nullptr;
		const void* Value = Waves.Find(TEXT("GerstnerWaves"), Property);
		const FArrayProperty* Array = CastField<FArrayProperty>(Property);
		const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
		if (Inner)
		{
			FScriptArrayHelper Helper(Array, Value);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				const FReflect One(Inner->Struct, Helper.GetRawPtr(Index));
				FMantleWaterWaveSample& Wave = Out.Waves.AddDefaulted_GetRef();
				Wave.WaveLength = One.Float(TEXT("WaveLength"));
				Wave.Amplitude = One.Float(TEXT("Amplitude"));
				Wave.Steepness = One.Float(TEXT("Steepness"));
				Wave.PhaseOffset = One.Float(TEXT("PhaseOffset"));
				if (const FVector* Direction = One.StructOf<FVector>(TEXT("Direction")))
				{
					Wave.Direction = *Direction;
				}
			}
		}
	}

	void ReadSpline(const FReflect& Actor, const bool bRiver, FMantleWaterBodyData& Out)
	{
		const USplineComponent* Spline = Cast<USplineComponent>(Actor.Object(TEXT("SplineComp")));
		if (!Spline)
		{
			return;
		}
		Out.SplineTransform = Spline->GetComponentTransform();
		Out.bClosedLoop = Spline->IsClosedLoop();

		// The metadata's curves hold one key for each spline point, by the point's index.
		const USplineMetadata* Metadata = Spline->GetSplinePointsMetadata();
		const FReflect Meta(Metadata);
		const FInterpCurveFloat* Width = Meta.Curve(TEXT("RiverWidth"));
		const FInterpCurveFloat* Depth = Meta.Curve(TEXT("Depth"));
		const FInterpCurveFloat* Velocity = Meta.Curve(TEXT("WaterVelocityScalar"));
		const FInterpCurveFloat* Audio = Meta.Curve(TEXT("AudioIntensity"));
		Out.bHasMetadata = Width || Depth || Velocity || Audio;

		const int32 Num = Spline->GetNumberOfSplinePoints();
		for (int32 Index = 0; Index < Num; ++Index)
		{
			FMantleWaterSplineSample& Point = Out.Points.AddDefaulted_GetRef();
			Point.Location = Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::Local);
			Point.ArriveTangent = Spline->GetArriveTangentAtSplinePoint(Index, ESplineCoordinateSpace::Local);
			Point.LeaveTangent = Spline->GetLeaveTangentAtSplinePoint(Index, ESplineCoordinateSpace::Local);
			Point.Rotation = Spline->GetRotationAtSplinePoint(Index, ESplineCoordinateSpace::Local);
			Point.Scale = Spline->GetScaleAtSplinePoint(Index);
			Point.Type = PointTypeName(Spline->GetSplinePointType(Index));
			Point.Width = bRiver && Width ? CurveAt(*Width, Index) : 0.f;
			Point.Depth = Depth ? CurveAt(*Depth, Index) : 0.f;
			Point.Velocity = Velocity ? CurveAt(*Velocity, Index) : 0.f;
			Point.AudioIntensity = Audio ? CurveAt(*Audio, Index) : 0.f;
		}
	}

	bool ReadBodyActor(const AActor* Actor, const FWaterClasses& Classes, FMantleWaterBodyData& Out)
	{
		const UActorComponent* Component = Actor->FindComponentByClass(Classes.BodyComponent);
		if (!Component)
		{
			return false;
		}
		if (Component->IsA(Classes.River))
		{
			Out.Type = EMantleWaterBodyKind::River;
		}
		else if (Component->IsA(Classes.Ocean))
		{
			Out.Type = EMantleWaterBodyKind::Ocean;
		}
		else if (Component->IsA(Classes.Custom))
		{
			Out.Type = EMantleWaterBodyKind::Custom;
		}
		else if (Component->IsA(Classes.Lake))
		{
			Out.Type = EMantleWaterBodyKind::Lake;
		}
		else
		{
			return false;
		}

		const FReflect ActorData(Actor);
		const FReflect Body(Component);
		Out.Name = Actor->GetActorLabel();
		Out.Path = Actor->GetPathName();
		Out.Transform = Actor->GetActorTransform();
		Out.SplineTransform = Out.Transform;

		ReadSpline(ActorData, Out.Type == EMantleWaterBodyKind::River, Out);
		ReadWaves(ActorData, Body, Out.Waves);
		ReadLook(Body, Out.Look);

		if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
		{
			Out.Collision.Enabled = CollisionEnabledName(Primitive->GetCollisionEnabled());
			Out.Collision.Profile = Primitive->GetCollisionProfileName().ToString();
			Out.Collision.bGenerateOverlapEvents = Primitive->GetGenerateOverlapEvents();
		}
		Out.Collision.HeightOffset = Body.Float(TEXT("CollisionHeightOffset"));
		if (const FVector* Extents = Body.StructOf<FVector>(TEXT("CollisionExtents")))
		{
			Out.Collision.OceanCollisionExtents = *Extents;
		}

		if (const UStaticMesh* Mesh = Cast<UStaticMesh>(Body.Object(TEXT("WaterMeshOverride"))))
		{
			Out.MeshPath = Mesh->GetPathName();
			Out.Mesh = const_cast<UStaticMesh*>(Mesh);
		}
		Out.bAffectsLandscape = Body.BoolOr(TEXT("bAffectsLandscape"), false) && Out.Type != EMantleWaterBodyKind::Custom;
		double Depth = 0.0;
		if (Body.Number(TEXT("FixedWaterDepth"), Depth))
		{
			Out.FixedWaterDepth = Depth;
		}
		if (const FVector2D* Extents = Body.StructOf<FVector2D>(TEXT("OceanExtents")))
		{
			Out.OceanExtents = *Extents;
		}
		Out.bOceanCentersOnZone = Body.BoolOr(TEXT("bCenterOnWaterZone"), false);
		return true;
	}

	void ReadZone(const AActor* Actor, FMantleWaterZoneData& Out)
	{
		const FReflect Zone(Actor);
		Out.Name = Actor->GetActorLabel();
		Out.Path = Actor->GetPathName();
		Out.Transform = Actor->GetActorTransform();
		if (const FVector2D* Extent = Zone.StructOf<FVector2D>(TEXT("ZoneExtent")))
		{
			Out.Extent = *Extent;
		}
		if (const FIntPoint* Resolution = Zone.StructOf<FIntPoint>(TEXT("RenderTargetResolution")))
		{
			Out.RenderTargetResolution = *Resolution;
		}
		Out.CaptureZOffset = Zone.Float(TEXT("CaptureZOffset"));
		Out.bHalfPrecisionTexture = Zone.BoolOr(TEXT("bHalfPrecisionTexture"), true);
		Out.VelocityBlurRadius = Zone.Int(TEXT("VelocityBlurRadius"));
		Out.OverlapPriority = Zone.Int(TEXT("OverlapPriority"));
		Out.bLocalTessellation = Zone.BoolOr(TEXT("bEnableLocalOnlyTessellation"), false);
		if (const FVector* Extent = Zone.StructOf<FVector>(TEXT("LocalTessellationExtent")))
		{
			Out.LocalTessellationExtent = *Extent;
		}
		if (const UObject* Mesh = Zone.Object(TEXT("WaterMesh")))
		{
			const FReflect MeshData(Mesh);
			Out.TileSize = MeshData.Float(TEXT("TileSize"));
			Out.TessellationFactor = MeshData.Int(TEXT("TessellationFactor"));
			Out.LodScale = MeshData.Float(TEXT("LODScale"), 1.f);
			Out.ForceCollapseDensityLevel = MeshData.Int(TEXT("ForceCollapseDensityLevel"), -1);
		}
	}

	const TCHAR* TypeName(const EMantleWaterBodyKind Type)
	{
		switch (Type)
		{
		case EMantleWaterBodyKind::Ocean: return TEXT("ocean");
		case EMantleWaterBodyKind::Lake: return TEXT("lake");
		case EMantleWaterBodyKind::River: return TEXT("river");
		default: return TEXT("custom");
		}
	}

	TArray<TSharedPtr<FJsonValue>> VectorArray(const FVector& Vector)
	{
		return { MakeShared<FJsonValueNumber>(Vector.X), MakeShared<FJsonValueNumber>(Vector.Y), MakeShared<FJsonValueNumber>(Vector.Z) };
	}

	TSharedRef<FJsonObject> TransformJson(const FTransform& Transform)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		const FRotator Rotation = Transform.Rotator();
		Json->SetArrayField(TEXT("location"), VectorArray(Transform.GetLocation()));
		Json->SetArrayField(TEXT("rotation"), VectorArray(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
		Json->SetArrayField(TEXT("scale"), VectorArray(Transform.GetScale3D()));
		return Json;
	}

	TSharedRef<FJsonObject> WavesJson(const FMantleWaterWaves& Waves)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("class"), Waves.WavesClass);
		if (!Waves.AssetPath.IsEmpty())
		{
			Json->SetStringField(TEXT("asset"), Waves.AssetPath);
		}
		Json->SetNumberField(TEXT("maxWaveHeight"), Waves.MaxWaveHeight);
		Json->SetNumberField(TEXT("attenuationDepth"), Waves.AttenuationDepth);
		Json->SetNumberField(TEXT("maxWaveHeightOffset"), Waves.MaxWaveHeightOffset);
		if (Waves.Generator.IsValid())
		{
			Json->SetStringField(TEXT("generatorClass"), Waves.GeneratorClass);
			Json->SetObjectField(TEXT("generator"), Waves.Generator);
		}
		TArray<TSharedPtr<FJsonValue>> List;
		for (const FMantleWaterWaveSample& Wave : Waves.Waves)
		{
			TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();
			One->SetNumberField(TEXT("waveLength"), Wave.WaveLength);
			One->SetNumberField(TEXT("amplitude"), Wave.Amplitude);
			One->SetNumberField(TEXT("steepness"), Wave.Steepness);
			One->SetArrayField(TEXT("direction"), VectorArray(Wave.Direction));
			List.Add(MakeShared<FJsonValueObject>(One));
		}
		Json->SetArrayField(TEXT("waves"), List);
		return Json;
	}

	TSharedRef<FJsonObject> LookJson(const FMantleWaterLook& Look)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("material"), Look.MaterialPath);
		Json->SetStringField(TEXT("master"), Look.MasterName);
		Json->SetNumberField(TEXT("parameterCount"), Look.TotalParams);
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		for (const FMantleWaterMaterialParam& Param : Look.Params)
		{
			if (Param.bColour)
			{
				Params->SetArrayField(Param.Name.ToString(), { MakeShared<FJsonValueNumber>(Param.Colour.R), MakeShared<FJsonValueNumber>(Param.Colour.G),
					MakeShared<FJsonValueNumber>(Param.Colour.B), MakeShared<FJsonValueNumber>(Param.Colour.A) });
			}
			else
			{
				Params->SetNumberField(Param.Name.ToString(), Param.Scalar);
			}
		}
		Json->SetObjectField(TEXT("parameters"), Params);
		TSharedRef<FJsonObject> Others = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Look.OtherMaterials)
		{
			Others->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("otherMaterials"), Others);
		return Json;
	}

	TSharedRef<FJsonObject> BodyJson(const FMantleWaterBodyData& Body)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("bodyType"), TypeName(Body.Type));
		Json->SetStringField(TEXT("name"), Body.Name);
		Json->SetStringField(TEXT("path"), Body.Path);
		Json->SetObjectField(TEXT("transform"), TransformJson(Body.Transform));

		TSharedRef<FJsonObject> Spline = MakeShared<FJsonObject>();
		Spline->SetBoolField(TEXT("closedLoop"), Body.bClosedLoop);
		Spline->SetBoolField(TEXT("hasMetadata"), Body.bHasMetadata);
		TArray<TSharedPtr<FJsonValue>> Points;
		for (const FMantleWaterSplineSample& Point : Body.Points)
		{
			TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();
			One->SetArrayField(TEXT("location"), VectorArray(Point.Location));
			One->SetArrayField(TEXT("arriveTangent"), VectorArray(Point.ArriveTangent));
			One->SetArrayField(TEXT("leaveTangent"), VectorArray(Point.LeaveTangent));
			One->SetArrayField(TEXT("rotation"), VectorArray(FVector(Point.Rotation.Pitch, Point.Rotation.Yaw, Point.Rotation.Roll)));
			One->SetStringField(TEXT("type"), Point.Type);
			if (Body.Type == EMantleWaterBodyKind::River)
			{
				One->SetNumberField(TEXT("width"), Point.Width);
			}
			One->SetNumberField(TEXT("depth"), Point.Depth);
			One->SetNumberField(TEXT("velocity"), Point.Velocity);
			One->SetNumberField(TEXT("audioIntensity"), Point.AudioIntensity);
			Points.Add(MakeShared<FJsonValueObject>(One));
		}
		Spline->SetArrayField(TEXT("points"), Points);
		Json->SetObjectField(TEXT("spline"), Spline);

		if (Body.Waves.bHasWaves)
		{
			Json->SetObjectField(TEXT("waves"), WavesJson(Body.Waves));
		}
		Json->SetObjectField(TEXT("look"), LookJson(Body.Look));

		TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
		Collision->SetStringField(TEXT("enabled"), Body.Collision.Enabled);
		Collision->SetStringField(TEXT("profile"), Body.Collision.Profile);
		Collision->SetBoolField(TEXT("generateOverlapEvents"), Body.Collision.bGenerateOverlapEvents);
		Collision->SetNumberField(TEXT("heightOffset"), Body.Collision.HeightOffset);
		if (Body.Type == EMantleWaterBodyKind::Ocean)
		{
			Collision->SetArrayField(TEXT("extents"), VectorArray(Body.Collision.OceanCollisionExtents));
		}
		Json->SetObjectField(TEXT("collision"), Collision);

		if (!Body.MeshPath.IsEmpty())
		{
			Json->SetStringField(TEXT("mesh"), Body.MeshPath);
		}
		Json->SetBoolField(TEXT("affectsLandscape"), Body.bAffectsLandscape);
		Json->SetNumberField(TEXT("fixedWaterDepth"), Body.FixedWaterDepth);
		if (Body.Type == EMantleWaterBodyKind::Ocean)
		{
			Json->SetArrayField(TEXT("oceanExtents"), { MakeShared<FJsonValueNumber>(Body.OceanExtents.X), MakeShared<FJsonValueNumber>(Body.OceanExtents.Y) });
			Json->SetBoolField(TEXT("centerOnWaterZone"), Body.bOceanCentersOnZone);
		}
		return Json;
	}

	TSharedRef<FJsonObject> ZoneJson(const FMantleWaterZoneData& Zone)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Zone.Name);
		Json->SetStringField(TEXT("path"), Zone.Path);
		Json->SetObjectField(TEXT("transform"), TransformJson(Zone.Transform));
		Json->SetArrayField(TEXT("extent"), { MakeShared<FJsonValueNumber>(Zone.Extent.X), MakeShared<FJsonValueNumber>(Zone.Extent.Y) });
		Json->SetArrayField(TEXT("renderTargetResolution"), { MakeShared<FJsonValueNumber>(Zone.RenderTargetResolution.X), MakeShared<FJsonValueNumber>(Zone.RenderTargetResolution.Y) });
		Json->SetNumberField(TEXT("tileSize"), Zone.TileSize);
		Json->SetNumberField(TEXT("tessellationFactor"), Zone.TessellationFactor);
		Json->SetNumberField(TEXT("lodScale"), Zone.LodScale);
		Json->SetNumberField(TEXT("forceCollapseDensityLevel"), Zone.ForceCollapseDensityLevel);
		Json->SetBoolField(TEXT("localTessellation"), Zone.bLocalTessellation);
		Json->SetArrayField(TEXT("localTessellationExtent"), VectorArray(Zone.LocalTessellationExtent));
		Json->SetNumberField(TEXT("captureZOffset"), Zone.CaptureZOffset);
		Json->SetBoolField(TEXT("halfPrecisionTexture"), Zone.bHalfPrecisionTexture);
		Json->SetNumberField(TEXT("velocityBlurRadius"), Zone.VelocityBlurRadius);
		Json->SetNumberField(TEXT("overlapPriority"), Zone.OverlapPriority);
		return Json;
	}
}

int32 FMantleWaterBodies::Count(const EMantleWaterBodyKind Type) const
{
	int32 Total = 0;
	for (const FMantleWaterBodyData& Body : Bodies)
	{
		Total += Body.Type == Type ? 1 : 0;
	}
	return Total;
}

FString FMantleWaterBodies::SummaryLine() const
{
	return FString::Printf(TEXT("MANTLE_CHECK_WATER ocean=%d lake=%d river=%d custom=%d zones=%d plugin=%d"),
		Count(EMantleWaterBodyKind::Ocean), Count(EMantleWaterBodyKind::Lake), Count(EMantleWaterBodyKind::River),
		Count(EMantleWaterBodyKind::Custom), Zones.Num(), bWaterPluginLoaded ? 1 : 0);
}

bool FMantleWaterBodyExport::IsWaterPluginLoaded()
{
	return MantleWaterBodyExportPrivate::FWaterClasses().Valid();
}

FMantleWaterBodies FMantleWaterBodyExport::Read(UWorld* World)
{
	using namespace MantleWaterBodyExportPrivate;

	FMantleWaterBodies Result;
	const FWaterClasses Classes;
	Result.bWaterPluginLoaded = Classes.Valid();
	if (!World || !Result.bWaterPluginLoaded)
	{
		return Result;
	}

	for (TActorIterator<AActor> It(World, Classes.BodyActor); It; ++It)
	{
		FMantleWaterBodyData Body;
		if (!It->IsTemplate() && ReadBodyActor(*It, Classes, Body))
		{
			Result.Bodies.Add(MoveTemp(Body));
		}
	}
	if (Classes.ZoneActor)
	{
		for (TActorIterator<AActor> It(World, Classes.ZoneActor); It; ++It)
		{
			if (!It->IsTemplate())
			{
				ReadZone(*It, Result.Zones.AddDefaulted_GetRef());
			}
		}
	}

	// By path, so the order does not depend on the order actors were spawned in.
	Result.Bodies.Sort([](const FMantleWaterBodyData& A, const FMantleWaterBodyData& B) { return A.Path < B.Path; });
	Result.Zones.Sort([](const FMantleWaterZoneData& A, const FMantleWaterZoneData& B) { return A.Path < B.Path; });
	return Result;
}

TSharedRef<FJsonObject> FMantleWaterBodyExport::ToJson(const FMantleWaterBodies& Water)
{
	using namespace MantleWaterBodyExportPrivate;

	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Bodies;
	for (const FMantleWaterBodyData& Body : Water.Bodies)
	{
		Bodies.Add(MakeShared<FJsonValueObject>(BodyJson(Body)));
	}
	TArray<TSharedPtr<FJsonValue>> Zones;
	for (const FMantleWaterZoneData& Zone : Water.Zones)
	{
		Zones.Add(MakeShared<FJsonValueObject>(ZoneJson(Zone)));
	}
	Json->SetArrayField(TEXT("bodies"), Bodies);
	Json->SetArrayField(TEXT("zones"), Zones);
	return Json;
}

FString FMantleWaterBodyExport::ToJsonString(const FMantleWaterBodies& Water)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(ToJson(Water), Writer);
	return Text;
}

int32 FMantleWaterBodyExport::CountBodiesCarving(const AActor* Landscape)
{
	using namespace MantleWaterBodyExportPrivate;

	UWorld* World = Landscape ? Landscape->GetWorld() : nullptr;
	const FWaterClasses Classes;
	if (!World || !Classes.Valid())
	{
		return 0;
	}
	const FBox Ground = Landscape->GetComponentsBoundingBox();
	int32 Count = 0;
	for (TActorIterator<AActor> It(World, Classes.BodyActor); It; ++It)
	{
		const UActorComponent* Component = It->IsTemplate() ? nullptr : It->FindComponentByClass(Classes.BodyComponent);
		const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
		if (!Primitive || Primitive->IsA(Classes.Custom) || !FReflect(Primitive).BoolOr(TEXT("bAffectsLandscape"), false))
		{
			continue;
		}
		// A lake's or river's reach is its spline (a river widened by its widest point); the component's own bounds can be larger
		// than that before the body has been built, so they are used only for a body with no spline (an ocean).
		FBox Reach(ForceInit);
		if (!Primitive->IsA(Classes.Ocean))
		{
			const FReflect BodyActor(*It);
			if (const USplineComponent* Spline = Cast<USplineComponent>(BodyActor.Object(TEXT("SplineComp"))))
			{
				float Widest = 0.f;
				if (Primitive->IsA(Classes.River))
				{
					if (const FInterpCurveFloat* Width = FReflect(Spline->GetSplinePointsMetadata()).Curve(TEXT("RiverWidth")))
					{
						for (const FInterpCurvePoint<float>& Point : Width->Points)
						{
							Widest = FMath::Max(Widest, Point.OutVal);
						}
					}
				}
				for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
				{
					Reach += Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World);
				}
				if (Reach.IsValid)
				{
					Reach = Reach.ExpandBy(FVector(Widest, Widest, 0.0));
				}
			}
		}
		if (!Reach.IsValid)
		{
			Reach = Primitive->Bounds.GetBox();
		}
		Count += Ground.IntersectXY(Reach) ? 1 : 0;
	}
	return Count;
}

bool FMantleWaterBodyExport::IsWaterBodyActor(const AActor* Actor)
{
	// One class lookup, not the whole set: this runs for every actor of a level.
	const UClass* BodyActor = Actor ? FindObject<UClass>(nullptr, MantleWaterBodyExportPrivate::BodyActorPath) : nullptr;
	return BodyActor && !Actor->IsTemplate() && Actor->IsA(BodyActor);
}

bool FMantleWaterBodyExport::ReadBody(const AActor* Actor, FMantleWaterBodyData& Out)
{
	const MantleWaterBodyExportPrivate::FWaterClasses Classes;
	return Actor && Classes.Valid() && MantleWaterBodyExportPrivate::ReadBodyActor(Actor, Classes, Out);
}

const FMantleWaterZoneData* FMantleWaterBodyExport::PrimaryZone(const FMantleWaterBodies& Water)
{
	const FMantleWaterZoneData* Best = nullptr;
	for (const FMantleWaterZoneData& Zone : Water.Zones)
	{
		if (!Best || Zone.OverlapPriority > Best->OverlapPriority)
		{
			Best = &Zone;
		}
	}
	return Best;
}

bool FMantleWaterBodyExport::BuildProps(const FMantleWaterBodyData& Body, FMantleWaterBodyProps& Props, FString& OutWhy, TArray<FString>& OutNotes)
{
	using EKind = EMantleWaterBodyKind;
	Props = FMantleWaterBodyProps();
	switch (Body.Type)
	{
	case EKind::Lake: Props.BodyType = EMantleWaterBodyType::Lake; break;
	case EKind::River: Props.BodyType = EMantleWaterBodyType::River; break;
	case EKind::Ocean: Props.BodyType = EMantleWaterBodyType::Ocean; break;
	default: Props.BodyType = EMantleWaterBodyType::Custom; break;
	}
	const bool bCustom = Body.Type == EKind::Custom;
	const bool bRiver = Body.Type == EKind::River;
	bool bClamped = false;
	auto Clamp = [&bClamped](const double Value, const double Min, const double Max)
	{
		const double Held = FMath::Clamp(Value, Min, Max);
		bClamped |= Held != Value;
		return Held;
	};

	if (bCustom)
	{
		if (Body.MeshPath.IsEmpty())
		{
			OutWhy = TEXT("is a custom water body with no Water Mesh Override, so there is no mesh to publish for it");
			return false;
		}
	}
	else
	{
		const int32 Min = bRiver ? FMantlePlaceLimits::MinWaterRiverPoints : FMantlePlaceLimits::MinWaterLoopPoints;
		if (Body.Points.Num() < Min)
		{
			OutWhy = FString::Printf(TEXT("has a spline of %d points; a %s needs at least %d"), Body.Points.Num(), bRiver ? TEXT("river") : TEXT("lake or ocean"), Min);
			return false;
		}
		if (Body.Points.Num() > FMantlePlaceLimits::MaxWaterSplinePointsPerBody)
		{
			OutWhy = FString::Printf(TEXT("has a spline of %d points; a water body may have at most %d"), Body.Points.Num(), FMantlePlaceLimits::MaxWaterSplinePointsPerBody);
			return false;
		}
		// The place format keeps points in the actor's own space; the spline component may sit offset from the actor.
		const FTransform ToActor = Body.SplineTransform * Body.Transform.Inverse();
		for (const FMantleWaterSplineSample& Sample : Body.Points)
		{
			FMantleWaterSplinePoint& Point = Props.SplinePoints.AddDefaulted_GetRef();
			Point.Location = ToActor.TransformPosition(Sample.Location);
			Point.ArriveTangent = ToActor.TransformVector(Sample.ArriveTangent);
			Point.LeaveTangent = ToActor.TransformVector(Sample.LeaveTangent);
			if (!MantleParseWaterSplinePointType(Sample.Type.Left(1).ToLower() + Sample.Type.Mid(1), Point.Interp))
			{
				Point.Interp = EMantleWaterSplinePointType::Curve;
			}
			Point.Depth = Clamp(Body.bHasMetadata ? Sample.Depth : Body.FixedWaterDepth, FMantlePlaceLimits::MinWaterSplineDepth, FMantlePlaceLimits::MaxWaterSplineDepth);
			if (bRiver)
			{
				Point.Width = Clamp(Sample.Width, FMantlePlaceLimits::MinWaterRiverWidth, FMantlePlaceLimits::MaxWaterRiverWidth);
				Point.Velocity = Clamp(Sample.Velocity, 0.0, FMantlePlaceLimits::MaxWaterRiverVelocity);
			}
		}
	}

	// Waves: the final Gerstner set, never a river's. The highest waves stay when there are more than a body holds.
	if (!bRiver && Body.Waves.bHasWaves && Body.Waves.Waves.Num() > 0)
	{
		TArray<FMantleWaterWaveSample> Waves = Body.Waves.Waves;
		if (Waves.Num() > FMantlePlaceLimits::MaxWaterWavesPerBody)
		{
			Waves.StableSort([](const FMantleWaterWaveSample& A, const FMantleWaterWaveSample& B) { return A.Amplitude > B.Amplitude; });
			OutNotes.Add(FString::Printf(TEXT("%d waves, of which the %d highest are kept"), Waves.Num(), FMantlePlaceLimits::MaxWaterWavesPerBody));
			Waves.SetNum(FMantlePlaceLimits::MaxWaterWavesPerBody);
		}
		for (const FMantleWaterWaveSample& Sample : Waves)
		{
			FMantleWaterWave& Wave = Props.Waves.AddDefaulted_GetRef();
			Wave.Wavelength = Clamp(Sample.WaveLength, FMantlePlaceLimits::MinWaterWavelength, FMantlePlaceLimits::MaxWaterWavelength);
			Wave.Amplitude = Clamp(Sample.Amplitude, 0.0, FMantlePlaceLimits::MaxWaterAmplitude);
			Wave.Steepness = Clamp(Sample.Steepness, 0.0, 1.0);
			// The wave's travel direction in world space, degrees from +X toward +Y.
			Wave.DirectionDeg = FMath::RadiansToDegrees(FMath::Atan2(Sample.Direction.Y, Sample.Direction.X));
			Wave.PhaseOffset = Clamp(Sample.PhaseOffset, -FMantlePlaceLimits::MaxWaterPhaseOffset, FMantlePlaceLimits::MaxWaterPhaseOffset);
		}
	}
	else if (!bRiver && Body.Waves.bHasWaves && Body.Waves.Generator.IsValid())
	{
		OutNotes.Add(TEXT("its wave generator has made no waves yet, so the water is flat"));
	}

	auto Unit = [](const FLinearColor& Color) { return FLinearColor(FMath::Clamp(Color.R, 0.f, 1.f), FMath::Clamp(Color.G, 0.f, 1.f), FMath::Clamp(Color.B, 0.f, 1.f)); };
	if (Body.Look.Albedo.IsSet()) { Props.Albedo = Unit(*Body.Look.Albedo); }
	if (Body.Look.Scattering.IsSet()) { Props.Scattering = Unit(*Body.Look.Scattering); }
	if (Body.Look.Absorption.IsSet()) { Props.Absorption = Unit(*Body.Look.Absorption); }

	Props.bCollision = Body.Collision.Enabled != TEXT("NoCollision");
	if (!FMath::IsNearlyZero(Body.Collision.HeightOffset))
	{
		Props.CollisionHeightOffset = Clamp(Body.Collision.HeightOffset, -FMantlePlaceLimits::MaxWaterCollisionShift, FMantlePlaceLimits::MaxWaterCollisionShift);
	}
	if (Body.Type == EKind::Ocean)
	{
		const FVector& Box = Body.Collision.OceanCollisionExtents;
		if (Box.X > 0.0 && Box.Y > 0.0 && Box.Z > 0.0)
		{
			Props.CollisionExtent = FVector(Clamp(Box.X, FMantlePlaceLimits::MinWaterExtent, FMantlePlaceLimits::MaxWaterExtent),
				Clamp(Box.Y, FMantlePlaceLimits::MinWaterExtent, FMantlePlaceLimits::MaxWaterExtent),
				Clamp(Box.Z, FMantlePlaceLimits::MinWaterExtent, FMantlePlaceLimits::MaxWaterExtent));
		}
		if (Body.OceanExtents.X > 0.0 && Body.OceanExtents.Y > 0.0)
		{
			Props.OceanExtent = FVector2D(Clamp(Body.OceanExtents.X, FMantlePlaceLimits::MinWaterExtent, FMantlePlaceLimits::MaxWaterExtent),
				Clamp(Body.OceanExtents.Y, FMantlePlaceLimits::MinWaterExtent, FMantlePlaceLimits::MaxWaterExtent));
		}
		Props.CenterOnZone = Body.bOceanCentersOnZone;
	}
	if (bClamped)
	{
		OutNotes.Add(TEXT("some values were held to the place format's ranges"));
	}
	return true;
}

FMantleWaterZone FMantleWaterBodyExport::BuildZone(const FMantleWaterZoneData& Zone)
{
	FMantleWaterZone Out;
	if (Zone.Extent.X > 0.0 && Zone.Extent.Y > 0.0)
	{
		Out.ZoneExtent = FVector2D(FMath::Clamp(Zone.Extent.X, FMantlePlaceLimits::MinWaterExtent, FMantlePlaceLimits::MaxWaterExtent),
			FMath::Clamp(Zone.Extent.Y, FMantlePlaceLimits::MinWaterExtent, FMantlePlaceLimits::MaxWaterExtent));
	}
	Out.ZoneLocation = Zone.Transform.GetLocation();
	Out.ZoneYawDeg = FRotator::NormalizeAxis(Zone.Transform.Rotator().Yaw);
	return Out;
}

bool FMantleWaterBodyExport::Admit(const AActor* Actor, const FString& Label, int32& Bodies, int32& SplinePoints, FMantleCoverage* Coverage)
{
	auto Report = [Coverage](const EMantleCoverageOutcome Outcome, const FString& Name, const FString& Detail)
	{
		if (Coverage)
		{
			Coverage->Add(MantleCoverageCategory::Water, Outcome, Name, Detail);
		}
	};
	FMantleWaterBodyData Body;
	if (!ReadBody(Actor, Body))
	{
		Report(EMantleCoverageOutcome::Dropped, Label, TEXT("a water body that could not be read."));
		return false;
	}
	FMantleWaterBodyProps Props;
	FString Why;
	TArray<FString> Notes;
	if (!BuildProps(Body, Props, Why, Notes))
	{
		Report(EMantleCoverageOutcome::Dropped, Label, FString::Printf(TEXT("left out: it %s."), *Why));
		return false;
	}
	if (Bodies >= FMantlePlaceLimits::MaxWaterBodiesPerPlace)
	{
		Report(EMantleCoverageOutcome::Dropped, Label, FString::Printf(TEXT("left out: a place holds at most %d water bodies."), FMantlePlaceLimits::MaxWaterBodiesPerPlace));
		return false;
	}
	if (SplinePoints + Props.SplinePoints.Num() > FMantlePlaceLimits::MaxWaterSplinePointsPerPlace)
	{
		Report(EMantleCoverageOutcome::Dropped, Label, FString::Printf(TEXT("left out: a place's water bodies hold at most %d spline points in all."), FMantlePlaceLimits::MaxWaterSplinePointsPerPlace));
		return false;
	}
	++Bodies;
	SplinePoints += Props.SplinePoints.Num();
	FString Detail = TEXT("published as a water body: its shape, depth, waves, collision and look colours. The water material's other settings (foam, caustics, flow maps) are not carried.");
	if (Notes.Num() > 0)
	{
		Detail += TEXT(" ") + FString::Join(Notes, TEXT("; ")) + TEXT(".");
	}
	Report(EMantleCoverageOutcome::Approximated, Label, Detail);
	return true;
}
