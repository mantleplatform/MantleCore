// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetUploadQueue.h"
#include "MantleApiClient.h"
#include "MantleCosmeticPackage.h"
#include "MantleCosmeticUpload.h"
#include "MantleProvenance.h"
#include "MantlePublisher.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// Provenance of public submissions and the moderation refusals, client side: the cleaning rules, the
// avatar item's request bodies, and the plain-words explanations. Pure: no network, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleProvenanceTestPrivate
{
	TSharedPtr<FJsonObject> ReadProvenanceObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Root);
		return Root;
	}

	/** The one-character string for a code unit, so the cases need no printf format for control characters. */
	FString Chr(const int32 Code)
	{
		FString Text;
		Text.AppendChar(static_cast<TCHAR>(Code));
		return Text;
	}

	int32 Utf8Bytes(const FString& Text)
	{
		return FTCHARToUTF8(*Text).Length();
	}

	/** The smallest emote package MakeItemJson accepts. */
	FMantleCosmeticPackage MakeEmotePackage(const bool bPublic)
	{
		FMantleCosmeticPackage Package;
		Package.Class = EMantleCosmeticClass::Emote;
		Package.Slot = TEXT("emote");
		Package.Name = TEXT("Wave");
		Package.Description = TEXT("A wave.");
		Package.ThumbnailKey = TEXT("thumbnail");
		Package.ClipKey = TEXT("clip");
		Package.Visibility = bPublic ? TEXT("public") : TEXT("private");
		Package.RightsDeclarationVersion = bPublic ? FString(MantleRights::DeclarationVersion) : FString();
		Package.Provenance = FMantleProvenance::Make(TEXT("/Game/Packs/Foo/A_Wave.A_Wave"), TEXT("D:\\Art\\Bought\\Foo\\A_Wave.fbx"));
		return Package;
	}

	TMap<FString, FString> EmoteIds()
	{
		TMap<FString, FString> Ids;
		Ids.Add(TEXT("thumbnail"), TEXT("ast_u11111111111111111111111111111111"));
		Ids.Add(TEXT("clip"), TEXT("ast_u22222222222222222222222222222222"));
		return Ids;
	}

	FMantleApiResult Refused(const int32 Status, const TCHAR* Code)
	{
		FMantleApiResult Result;
		Result.bSuccess = false;
		Result.StatusCode = Status;
		Result.Code = Code;
		Result.Message = TEXT("the server's own message");
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleProvenanceCleanTest,
	"Mantle.Cosmetic.Provenance.StripsControlCharactersAndCutsToTheLimits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleProvenanceCleanTest::RunTest(const FString& Parameters)
{
	using namespace MantleProvenanceTestPrivate;
	TestEqual(TEXT("control characters are removed"), FMantleProvenance::Clean(TEXT("A") + Chr(0x01) + TEXT("B") + Chr(0x0a) + TEXT("C") + Chr(0x7f) + TEXT("D"), 100), FString(TEXT("ABCD")));
	TestEqual(TEXT("a tab and a newline are control characters"), FMantleProvenance::Clean(TEXT("a\tb\r\nc"), 100), FString(TEXT("abc")));
	TestEqual(TEXT("C1 controls and line separators are removed"), FMantleProvenance::Clean(TEXT("a") + Chr(0x85) + TEXT("b") + Chr(0x2028) + TEXT("c") + Chr(0x2029) + TEXT("d"), 100), FString(TEXT("abcd")));
	TestEqual(TEXT("invisible format characters are removed"), FMantleProvenance::Clean(TEXT("a") + Chr(0x200b) + TEXT("b") + Chr(0xfeff) + TEXT("c") + Chr(0x202e) + TEXT("d") + Chr(0x00ad) + TEXT("e"), 100), FString(TEXT("abcde")));
	TestEqual(TEXT("inner spaces stay, the ends are trimmed"), FMantleProvenance::Clean(TEXT("  Pack Name  "), 100), FString(TEXT("Pack Name")));
	TestEqual(TEXT("ordinary text is unchanged"), FMantleProvenance::Clean(TEXT("/Game/Packs/Foo/SK_Foo"), 512), FString(TEXT("/Game/Packs/Foo/SK_Foo")));

	const FString Long = FString::ChrN(900, TEXT('x'));
	TestEqual(TEXT("a path is cut to 512"), FMantleProvenance::Make(Long, FString()).SourcePath.Len(), FMantleProvenance::MaxSourcePathBytes);
	TestEqual(TEXT("a file name is cut to 256"), FMantleProvenance::Make(FString(), Long).ImportFilename.Len(), FMantleProvenance::MaxImportFilenameBytes);

	// The limit counts UTF-8 bytes, so a cut never goes past it and never splits a character: 3-byte characters.
	const FString Wide = FString::ChrN(300, static_cast<TCHAR>(0x3042));
	const FString Cut = FMantleProvenance::Clean(Wide, FMantleProvenance::MaxImportFilenameBytes);
	TestTrue(TEXT("a wide-character name stays within 256 bytes"), Utf8Bytes(Cut) <= FMantleProvenance::MaxImportFilenameBytes);
	TestEqual(TEXT("and keeps as many whole characters as fit"), Cut.Len(), FMantleProvenance::MaxImportFilenameBytes / 3);

	// A surrogate pair (4 bytes) that would straddle the limit is dropped whole.
	FString Pair = FString::ChrN(2, TEXT('a'));
	Pair.AppendChar(static_cast<TCHAR>(0xd83d));
	Pair.AppendChar(static_cast<TCHAR>(0xde80));
	TestEqual(TEXT("a pair that does not fit is dropped whole"), FMantleProvenance::Clean(Pair, 4), FString(TEXT("aa")));
	TestEqual(TEXT("a pair that fits is kept"), FMantleProvenance::Clean(Pair, 6).Len(), 4);
	FString Lone = TEXT("ab");
	Lone.AppendChar(static_cast<TCHAR>(0xd83d));
	TestEqual(TEXT("a lone surrogate half is dropped"), FMantleProvenance::Clean(Lone, 100), FString(TEXT("ab")));

	// Make cleans both fields, whatever they held.
	const FMantleProvenance Made = FMantleProvenance::Make(TEXT("/Game/A") + Chr(0x07) + TEXT("/B"), TEXT("C:\\x\\y") + Chr(0x1b) + TEXT(".fbx"));
	TestEqual(TEXT("Make cleans the path"), Made.SourcePath, FString(TEXT("/Game/A/B")));
	TestEqual(TEXT("and the file name"), Made.ImportFilename, FString(TEXT("y.fbx")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleProvenanceFilenameTest,
	"Mantle.Cosmetic.Provenance.ImportFilenameIsTheNameWithNoFolders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleProvenanceFilenameTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a Windows path"), FMantleProvenance::CleanFilename(TEXT("D:\\Users\\jo\\Art\\Bought Packs\\Foo\\SK_Foo.fbx")), FString(TEXT("SK_Foo.fbx")));
	TestEqual(TEXT("a Unix path"), FMantleProvenance::CleanFilename(TEXT("/home/jo/art/SK_Foo.fbx")), FString(TEXT("SK_Foo.fbx")));
	TestEqual(TEXT("mixed separators"), FMantleProvenance::CleanFilename(TEXT("D:/Art\\Foo/SK_Foo.glb")), FString(TEXT("SK_Foo.glb")));
	TestEqual(TEXT("a bare name is kept"), FMantleProvenance::CleanFilename(TEXT("SK_Foo.fbx")), FString(TEXT("SK_Foo.fbx")));
	TestTrue(TEXT("a path that ends in a separator has no name"), FMantleProvenance::CleanFilename(TEXT("D:\\Art\\")).IsEmpty());
	TestTrue(TEXT("an empty path has none"), FMantleProvenance::CleanFilename(FString()).IsEmpty());

	const FMantleProvenance Made = FMantleProvenance::Make(TEXT("/Game/Packs/Foo/SK_Foo"), TEXT("D:\\Art\\Secret Folder\\SK_Foo.fbx"));
	TestFalse(TEXT("no folder of the creator's disk is kept"), Made.ImportFilename.Contains(TEXT("Secret")) || Made.ImportFilename.Contains(TEXT("\\")) || Made.ImportFilename.Contains(TEXT("/")));

	TestTrue(TEXT("a null asset has no import file"), FMantleProvenance::ImportFilenameOf(nullptr).IsEmpty());
	TestTrue(TEXT("and no provenance"), FMantleProvenance::FromAsset(nullptr).IsEmpty());

	TestEqual(TEXT("an object path loses its repeated name"), FMantleProvenance::AssetPathOf(TEXT("/Game/Packs/Foo/SK_Foo.SK_Foo")), FString(TEXT("/Game/Packs/Foo/SK_Foo")));
	TestEqual(TEXT("a different tail is kept"), FMantleProvenance::AssetPathOf(TEXT("/Game/Packs/Foo/SK_Foo.Other")), FString(TEXT("/Game/Packs/Foo/SK_Foo.Other")));
	TestEqual(TEXT("a package path is kept"), FMantleProvenance::AssetPathOf(TEXT("/Game/Packs/Foo/SK_Foo")), FString(TEXT("/Game/Packs/Foo/SK_Foo")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleProvenanceItemBodyTest,
	"Mantle.Cosmetic.Provenance.AvatarItemBodiesCarryItForPublicOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleProvenanceItemBodyTest::RunTest(const FString& Parameters)
{
	using namespace MantleProvenanceTestPrivate;
	FString Json;
	FString Error;

	const FMantleCosmeticPackage Public = MakeEmotePackage(true);
	if (!TestTrue(TEXT("a public item is made"), Public.MakeItemJson(EmoteIds(), Json, Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Item = ReadProvenanceObject(Json);
	const TSharedPtr<FJsonObject>* Provenance = nullptr;
	if (!TestTrue(TEXT("a public item carries its provenance"), Item.IsValid() && Item->TryGetObjectField(TEXT("provenance"), Provenance) && Provenance))
	{
		return false;
	}
	TestEqual(TEXT("the source path"), (*Provenance)->GetStringField(TEXT("sourcePath")), FString(TEXT("/Game/Packs/Foo/A_Wave")));
	TestEqual(TEXT("the file name only"), (*Provenance)->GetStringField(TEXT("importFilename")), FString(TEXT("A_Wave.fbx")));
	TestEqual(TEXT("and nothing else in it"), (*Provenance)->Values.Num(), 2);

	// The same on a new version of an existing item.
	if (TestTrue(TEXT("a public version patch is made"), Public.MakeItemPatchJson(EmoteIds(), Json, Error)))
	{
		Item = ReadProvenanceObject(Json);
		TestTrue(TEXT("it carries the provenance too"), Item.IsValid() && Item->HasTypedField<EJson::Object>(TEXT("provenance")));
	}

	// Private: absent, even when the package has it.
	const FMantleCosmeticPackage Private = MakeEmotePackage(false);
	TestFalse(TEXT("the private package does hold provenance"), Private.Provenance.IsEmpty());
	if (TestTrue(TEXT("a private item is made"), Private.MakeItemJson(EmoteIds(), Json, Error)))
	{
		Item = ReadProvenanceObject(Json);
		TestTrue(TEXT("it is JSON"), Item.IsValid());
		TestFalse(TEXT("a private item sends no provenance"), Item.IsValid() && Item->HasField(TEXT("provenance")));
	}
	if (TestTrue(TEXT("a private patch is made"), Private.MakeItemPatchJson(EmoteIds(), Json, Error)))
	{
		Item = ReadProvenanceObject(Json);
		TestFalse(TEXT("a private patch sends none"), Item.IsValid() && Item->HasField(TEXT("provenance")));
	}

	// Public with nothing to send: the field stays absent.
	FMantleCosmeticPackage Bare = MakeEmotePackage(true);
	Bare.Provenance = FMantleProvenance();
	if (TestTrue(TEXT("a public item with no provenance is made"), Bare.MakeItemJson(EmoteIds(), Json, Error)))
	{
		Item = ReadProvenanceObject(Json);
		TestFalse(TEXT("the field is absent"), Item.IsValid() && Item->HasField(TEXT("provenance")));
	}

	// Only a file name is known: the path stays empty, both keys are sent.
	FMantleCosmeticPackage NameOnly = MakeEmotePackage(true);
	NameOnly.Provenance = FMantleProvenance::Make(FString(), TEXT("C:\\a\\b.fbx"));
	if (TestTrue(TEXT("a public item with a file name only is made"), NameOnly.MakeItemJson(EmoteIds(), Json, Error)))
	{
		Item = ReadProvenanceObject(Json);
		if (TestTrue(TEXT("it has provenance"), Item.IsValid() && Item->TryGetObjectField(TEXT("provenance"), Provenance) && Provenance))
		{
			TestEqual(TEXT("the missing path is empty"), (*Provenance)->GetStringField(TEXT("sourcePath")), FString());
			TestEqual(TEXT("the file name is kept"), (*Provenance)->GetStringField(TEXT("importFilename")), FString(TEXT("b.fbx")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleProvenanceModerationTextTest,
	"Mantle.Cosmetic.Provenance.ModerationRefusalsAreExplainedInPlainWords",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleProvenanceModerationTextTest::RunTest(const FString& Parameters)
{
	using namespace MantleProvenanceTestPrivate;
	const FMantleApiResult Revoked = Refused(403, TEXT("public_publishing_revoked"));
	const FMantleApiResult Duplicate = Refused(409, TEXT("public_duplicate_claim"));

	TestEqual(TEXT("revoked public publishing"), FMantlePublisher::DescribeModerationFailure(Revoked),
		FString(TEXT("Public publishing is turned off for this account after repeated copyright strikes. You can still publish privately.")));
	TestEqual(TEXT("a duplicate claim"), FMantlePublisher::DescribeModerationFailure(Duplicate),
		FString(TEXT("Another account uploaded this content first. Publish it privately, or contact support if it is yours.")));
	TestTrue(TEXT("another refusal is not a moderation one"), FMantlePublisher::DescribeModerationFailure(Refused(403, TEXT("forbidden"))).IsEmpty());
	TestTrue(TEXT("a refusal with no code is not one"), FMantlePublisher::DescribeModerationFailure(Refused(403, TEXT(""))).IsEmpty());
	TestTrue(TEXT("the code is matched exactly"), FMantlePublisher::DescribeModerationFailure(Refused(403, TEXT("Public_Publishing_Revoked"))).IsEmpty());

	// The avatar panel's error text, for an item create and a version patch.
	const FString Create = FMantleCosmeticUploadSession::DescribeFailure(Revoked, TEXT("the item"));
	TestTrue(TEXT("the item create says public publishing is off"), Create.Contains(TEXT("Public publishing is turned off for this account")));
	TestTrue(TEXT("and that private still works"), Create.Contains(TEXT("You can still publish privately.")));
	TestFalse(TEXT("not the server's raw message"), Create.Contains(TEXT("the server's own message")));
	const FString Patch = FMantleCosmeticUploadSession::DescribeUpdateFailure(Duplicate, TEXT("itm_u11111111111111111111111111111111"));
	TestTrue(TEXT("a version patch says another account was first"), Patch.Contains(TEXT("Another account uploaded this content first.")));
	TestTrue(TEXT("and the way out"), Patch.Contains(TEXT("Publish it privately, or contact support if it is yours.")));

	// A place publish's asset uploads say the same.
	FString Code;
	const FString Upload = FMantleAssetUploadQueue::DescribeRefusal(Duplicate, TEXT("Truck"), Code);
	TestTrue(TEXT("an asset refusal names the item"), Upload.Contains(TEXT("Truck")));
	TestTrue(TEXT("and explains the duplicate"), Upload.Contains(TEXT("Another account uploaded this content first.")));
	TestEqual(TEXT("the code is kept for the log"), Code, FString(TEXT("public_duplicate_claim")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
