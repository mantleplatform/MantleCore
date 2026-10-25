// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticExport.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

// A mask or normal PNG carries its role in a text chunk, so it never shares exact bytes with a colour map (the API keeps
// one role per file); its pixels and decoding are unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePngRoleTagTest,
	"Mantle.Editor.CosmeticExport.PngRoleTag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePngRoleTagTest::RunTest(const FString& Parameters)
{
	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TArray<uint8> Pixels = { 255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255 };
	TSharedPtr<IImageWrapper> Writer = Module.CreateImageWrapper(EImageFormat::PNG);
	Writer->SetRaw(Pixels.GetData(), Pixels.Num(), 2, 2, ERGBFormat::RGBA, 8);
	const TArray64<uint8> Encoded = Writer->GetCompressed();
	const TArray<uint8> Plain(Encoded.GetData(), static_cast<int32>(Encoded.Num()));

	TArray<uint8> Data = Plain;
	TArray<uint8> Normal = Plain;
	TestTrue(TEXT("a PNG takes the tag"), FMantleCosmeticExport::TagPngRole(Data, TEXT("data")));
	FMantleCosmeticExport::TagPngRole(Normal, TEXT("normal"));
	TestNotEqual(TEXT("the tagged bytes differ from the plain colour map's"), Data, Plain);
	TestNotEqual(TEXT("and between roles"), Data, Normal);

	TSharedPtr<IImageWrapper> Reader = Module.CreateImageWrapper(EImageFormat::PNG);
	TArray64<uint8> Decoded;
	TestTrue(TEXT("the tagged PNG still decodes (its chunk CRC is valid)"),
		Reader->SetCompressed(Data.GetData(), Data.Num()) && Reader->GetRaw(ERGBFormat::RGBA, 8, Decoded));
	TestTrue(TEXT("to the same pixels"), Decoded.Num() == Pixels.Num() && FMemory::Memcmp(Decoded.GetData(), Pixels.GetData(), Pixels.Num()) == 0);

	TestFalse(TEXT("an orange image isn't grey"), FMantleCosmeticExport::IsGreyPng(Plain));
	const TArray<uint8> GreyPixels = { 128, 128, 128, 255, 128, 128, 128, 255, 90, 90, 90, 255, 128, 128, 128, 255 };
	Writer->SetRaw(GreyPixels.GetData(), GreyPixels.Num(), 2, 2, ERGBFormat::RGBA, 8);
	const TArray64<uint8> GreyEncoded = Writer->GetCompressed();
	TestTrue(TEXT("a grey image is grey (the API refuses it as a normal map)"),
		FMantleCosmeticExport::IsGreyPng(TArray<uint8>(GreyEncoded.GetData(), static_cast<int32>(GreyEncoded.Num()))));

	TArray<uint8> NotPng = { 1, 2, 3 };
	TestFalse(TEXT("anything else is left alone"), FMantleCosmeticExport::TagPngRole(NotPng, TEXT("data")));
	TestEqual(TEXT("unchanged"), NotPng.Num(), 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
