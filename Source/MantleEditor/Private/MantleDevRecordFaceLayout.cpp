// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.RecordFaceLayout [face=<package path of a built face>] [out=<path>]
// Records, once, the layout of a built MetaHuman face (Epic's UE Optimized / Medium LOD0, 6872 triangles): quantised UVs, triangles and render
// sections in the built order, no geometry. Default source is the converted Aera's face. Writes the plugin-only resource
// Resources/FaceLayout/face-layout-v1.json that FMantleFaceLayout::Reorder puts MetaHuman Creator's in-memory faces into.
#include "MantleBodyTopologyData.h"
#include "MantleFaceLayout.h"

#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleRecordFaceLayout, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevRecordFaceLayoutPrivate
{
	void Run(const TArray<FString>& Args)
	{
		FString FacePackage = TEXT("/Game/MetaHumans/Aera/Converted/Aera/Unpacked/Aera/Face/SKM_Aera_FaceMesh");
		FString OutPath = FMantleFaceLayout::DefaultPath();
		for (const FString& Arg : Args)
		{
			if (Arg.StartsWith(TEXT("face="), ESearchCase::IgnoreCase)) { FacePackage = Arg.Mid(5); }
			else if (Arg.StartsWith(TEXT("out="), ESearchCase::IgnoreCase)) { OutPath = Arg.Mid(4); }
		}
		const auto Finish = [](const FString& Error)
		{
			if (!Error.IsEmpty()) { UE_LOG(LogMantleRecordFaceLayout, Error, TEXT("MANTLE_FACE_LAYOUT error=%s"), *Error); }
			UE_LOG(LogMantleRecordFaceLayout, Display, TEXT("MANTLE_FACE_LAYOUT done"));
		};
		const FString ObjectPath = FacePackage.Contains(TEXT(".")) ? FacePackage : FString::Printf(TEXT("%s.%s"), *FacePackage, *FPackageName::GetShortName(FacePackage));
		const USkeletalMesh* Face = LoadObject<USkeletalMesh>(nullptr, *ObjectPath);
		if (!Face || OutPath.IsEmpty())
		{
			Finish(FString::Printf(TEXT("no face mesh at %s (or no output path)"), *ObjectPath));
			return;
		}
		FMantleFaceLayout Layout;
		FString Error;
		if (!FMantleFaceLayout::Record(Face, Layout, Error))
		{
			Finish(Error);
			return;
		}
		Layout.Source = ObjectPath;
		Layout.Generated = FDateTime::UtcNow().ToIso8601();
		TArray<FVector2f> UVs;
		for (const FIntPoint& UV : Layout.QuantizedUVs) { UVs.Add(FVector2f(UV.X / static_cast<float>(FMantleFaceLayout::Quantization), UV.Y / static_cast<float>(FMantleFaceLayout::Quantization))); }
		UE_LOG(LogMantleRecordFaceLayout, Display, TEXT("MANTLE_FACE_LAYOUT recorded %s LOD%d: %d vertices, %d triangles, uvhash %08x"), *ObjectPath, FMantleFaceLayout::FindFaceLOD(Face),
			Layout.QuantizedUVs.Num(), Layout.Indices.Num() / 3, UMantleBodyTopologyData::ComputeUVHash(UVs));
		for (const FMantleFaceLayoutSection& Section : Layout.Sections)
		{
			UE_LOG(LogMantleRecordFaceLayout, Display, TEXT("MANTLE_FACE_LAYOUT section %s: vertices %d..%d, %d triangles"), *Section.Material, Section.FirstVertex,
				Section.FirstVertex + Section.NumVertices - 1, Section.NumTriangles);
		}
		// Repeated UVs per section: the vertices the match tells apart by their neighbours.
		for (const FMantleFaceLayoutSection& Section : Layout.Sections)
		{
			TMap<FIntPoint, int32> Counts;
			for (int32 Vertex = 0; Vertex < Section.NumVertices; ++Vertex) { ++Counts.FindOrAdd(Layout.QuantizedUVs[Section.FirstVertex + Vertex]); }
			int32 Repeated = 0, Largest = 0;
			for (const TPair<FIntPoint, int32>& Pair : Counts) { if (Pair.Value > 1) { Repeated += Pair.Value; Largest = FMath::Max(Largest, Pair.Value); } }
			UE_LOG(LogMantleRecordFaceLayout, Display, TEXT("MANTLE_FACE_LAYOUT section %s: %d vertices share a UV with another (largest group %d)"), *Section.Material, Repeated, Largest);
		}
		if (!FFileHelper::SaveStringToFile(Layout.ToJson(), *OutPath))
		{
			Finish(FString::Printf(TEXT("could not write %s"), *OutPath));
			return;
		}
		UE_LOG(LogMantleRecordFaceLayout, Display, TEXT("MANTLE_FACE_LAYOUT written=%s"), *OutPath);
		Finish(FString());
	}

	FAutoConsoleCommand GMantleDevRecordFaceLayoutCommand(
		TEXT("Mantle.Dev.RecordFaceLayout"),
		TEXT("Mantle.Dev.RecordFaceLayout [face=<package>] [out=<path>] -- maintainer command: records the layout of a built face (default: the converted Aera's) to ")
		TEXT("Resources/FaceLayout/face-layout-v1.json. Logs MANTLE_FACE_LAYOUT lines."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
