// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponAsset.h"
#include "MantleWeaponDocument.h"

#include "Containers/StringConv.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// MWP1 (#789 stage 4): the uploaded weapon document, read back into the text and the ids it names.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleWeaponAssetTestPrivate
{
	const TCHAR* const MeshId = TEXT("ast_u00000000000000000000000000000001");
	const TCHAR* const ScopeId = TEXT("ast_u00000000000000000000000000000002");
	const TCHAR* const SoundId = TEXT("ast_u00000000000000000000000000000003");
	const TCHAR* const MaterialA = TEXT("ast_u00000000000000000000000000000004");
	const TCHAR* const MaterialB = TEXT("ast_u00000000000000000000000000000005");

	/** A valid rifle document naming one of each kind of asset (the backend cooks exactly this shape to MWP1). */
	FString Document()
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"name\":\"Test rifle\",\"archetype\":\"rifle\",\"mesh\":\"%s\",\"materials\":[\"%s\",\"%s\"],")
				TEXT("\"attachments\":[{\"name\":\"Scope\",\"mesh\":\"%s\"},{\"name\":\"Scope2\",\"mesh\":\"%s\"}],")
				TEXT("\"sounds\":{\"fire\":\"%s\",\"fireSuppressed\":\"%s\"},")
				TEXT("\"sockets\":{\"sight\":{\"bone\":\"root\",\"location\":[1,0,2]},\"muzzle\":{\"bone\":\"root\",\"location\":[50,0,2]},")
				TEXT("\"gripR\":{\"bone\":\"root\",\"location\":[0,0,0]},\"gripL\":{\"bone\":\"root\",\"location\":[30,0,0]}},")
				TEXT("\"fire\":{\"interval\":0.1,\"automatic\":true,\"modes\":[\"semi\",\"auto\"],\"damage\":10,\"maxRange\":25000,")
				TEXT("\"traceSweepRadius\":5,\"pellets\":1,\"pelletSpread\":1,\"plainRecoil\":0.3},")
				TEXT("\"magazine\":{\"size\":30,\"startingReserve\":90},\"reload\":{\"style\":\"magazine\",\"seconds\":1.7}}"),
			MeshId, MaterialA, MaterialB, ScopeId, ScopeId, SoundId, SoundId);
	}

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponAssetReadTest,
	"Mantle.WeaponAsset.Mwp1.ReadsTheDocumentAndWhatItNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponAssetReadTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponAssetTestPrivate;
	const FString Text = Document();

	FMantleWeaponAsset Asset;
	FString Error;
	const bool bRead = FMantleWeaponAsset::Read(Utf8(Text), Asset, Error);
	if (!TestTrue(*FString::Printf(TEXT("a cooked weapon document reads (%s)"), *Error), bRead))
	{
		return false;
	}
	TestEqual(TEXT("the document text is kept as it was cooked"), Asset.Text, Text);
	TestEqual(TEXT("the name"), Asset.Name, FString(TEXT("Test rifle")));
	TestEqual(TEXT("the skeletal mesh"), Asset.Mesh, FString(MeshId));
	TestTrue(TEXT("an attachment mesh used twice is listed once"), Asset.Attachments == TArray<FString>({ FString(ScopeId) }));
	TestTrue(TEXT("a sound used for both shots is listed once"), Asset.Sounds == TArray<FString>({ FString(SoundId) }));
	TestTrue(TEXT("the slot materials keep their order"), Asset.Materials == TArray<FString>({ FString(MaterialA), FString(MaterialB) }));
	TestEqual(TEXT("every named id once"), Asset.NamedIds().Num(), 5);

	// A byte-order mark in front is skipped.
	TArray<uint8> WithBom = { 0xEF, 0xBB, 0xBF };
	WithBom.Append(Utf8(Text));
	FMantleWeaponAsset FromBom;
	TestTrue(TEXT("a UTF-8 byte-order mark is skipped"), FMantleWeaponAsset::Read(WithBom, FromBom, Error) && FromBom.Text == Text);

	// #898: an attachment's own materials, in slot order, by its mesh id (the first attachment naming a mesh wins); named once.
	const TCHAR* const PartMaterial = TEXT("ast_u00000000000000000000000000000007");
	const FString WithPartMaterials = Text.Replace(*FString::Printf(TEXT("{\"name\":\"Scope\",\"mesh\":\"%s\"}"), ScopeId),
		*FString::Printf(TEXT("{\"name\":\"Scope\",\"mesh\":\"%s\",\"materials\":[\"%s\",\"%s\"]}"), ScopeId, PartMaterial, MaterialA));
	FMantleWeaponAsset Dressed;
	if (TestTrue(*FString::Printf(TEXT("an attachment's materials read (%s)"), *Error), FMantleWeaponAsset::Read(Utf8(WithPartMaterials), Dressed, Error)))
	{
		const TArray<FString>* Part = Dressed.AttachmentMaterials.Find(ScopeId);
		TestTrue(TEXT("the scope's materials keep their order"), Part && *Part == TArray<FString>({ FString(PartMaterial), FString(MaterialA) }));
		TestEqual(TEXT("one attachment mesh has materials"), Dressed.AttachmentMaterials.Num(), 1);
		TestEqual(TEXT("its new material is named once more"), Dressed.NamedIds().Num(), 6);
		FMantleWeaponDesc Desc;
		FMantleWeaponDesc Again;
		TestTrue(TEXT("and survive a write and a read"), FMantleWeaponDocument::Parse(WithPartMaterials, Desc, Error)
			&& FMantleWeaponDocument::Parse(FMantleWeaponDocument::Write(Desc), Again, Error) && Again.Attachments.Value[0].Materials.bSet
			&& Again.Attachments.Value[0].Materials.Value == Desc.Attachments.Value[0].Materials.Value && !Again.Attachments.Value[1].Materials.bSet);
	}
	FMantleWeaponAsset BadPart;
	TestFalse(TEXT("an attachment material that is not a name is refused"), FMantleWeaponAsset::Read(Utf8(WithPartMaterials.Replace(PartMaterial, TEXT("a/b"))), BadPart, Error));
	TestTrue(*FString::Printf(TEXT("at its pointer (%s)"), *Error), Error.Contains(TEXT("attachments/0/materials/0")));

	// What the cook never stores is refused, with a reason.
	FMantleWeaponAsset Refused;
	FString Why;
	TestFalse(TEXT("no bytes"), FMantleWeaponAsset::Read(TConstArrayView<uint8>(), Refused, Why));
	TestFalse(TEXT("text that is not JSON"), FMantleWeaponAsset::Read(Utf8(TEXT("MWP1")), Refused, Why));
	TestFalse(TEXT("JSON that is not a weapon document"), FMantleWeaponAsset::Read(Utf8(TEXT("{\"name\":\"x\"}")), Refused, Why));
	TestFalse(TEXT("an error says why"), Why.IsEmpty());
	TArray<uint8> TooBig;
	TooBig.Init(' ', FMantleWeaponAsset::MaxBytes + 1);
	TestFalse(TEXT("over the cap"), FMantleWeaponAsset::Read(TooBig, Refused, Why));
	return true;
}

// #840: a version-2 weapon reads too; a shield is its own weapon whose mesh is its only id (decision 4), and an off-hand
// mesh inside a weapon is refused; fists name no mesh; the cap is on the bytes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponAssetVersion2Test,
	"Mantle.WeaponAsset.Mwp1.ReadsVersion2AndTheShield",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponAssetVersion2Test::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponAssetTestPrivate;
	const TCHAR* const ShieldId = TEXT("ast_u00000000000000000000000000000006");
	const FString Sword = FString::Printf(
		TEXT("{\"schemaVersion\":2,\"name\":\"Short sword\",\"hold\":\"melee_1h\",\"delivery\":\"sweep\",\"mesh\":\"%s\",")
			TEXT("\"sockets\":{\"gripR\":{\"bone\":\"root\"},\"edgeBase\":{\"bone\":\"root\",\"location\":[10,0,0]},")
			TEXT("\"edgeTip\":{\"bone\":\"root\",\"location\":[75,0,0]}},")
			TEXT("\"fire\":{\"interval\":0.5,\"damage\":25},\"sweep\":{\"reach\":110,\"comboCount\":4},\"block\":{}}"),
		MeshId);
	FMantleWeaponAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("a version-2 sword reads (%s)"), *Error), FMantleWeaponAsset::Read(Utf8(Sword), Asset, Error)))
	{
		return false;
	}
	TestEqual(TEXT("its mesh"), Asset.Mesh, FString(MeshId));
	TestTrue(TEXT("named once"), Asset.NamedIds() == TArray<FString>({ FString(MeshId) }));

	const FString Shield = FString::Printf(
		TEXT("{\"schemaVersion\":2,\"name\":\"Round shield\",\"hold\":\"shield\",\"delivery\":\"sweep\",\"mesh\":\"%s\",")
			TEXT("\"sockets\":{\"grip\":{\"bone\":\"root\",\"location\":[-4,0,0]},\"face\":{\"bone\":\"root\",\"location\":[6,0,0]}},")
			TEXT("\"fire\":{\"interval\":0.8,\"damage\":10},\"sweep\":{\"reach\":80},\"block\":{\"enabled\":true}}"),
		ShieldId);
	FMantleWeaponAsset ShieldAsset;
	if (TestTrue(*FString::Printf(TEXT("a shield reads (%s)"), *Error), FMantleWeaponAsset::Read(Utf8(Shield), ShieldAsset, Error)))
	{
		TestEqual(TEXT("its mesh is the weapon mesh"), ShieldAsset.Mesh, FString(ShieldId));
		TestTrue(TEXT("and the only id it names"), ShieldAsset.NamedIds() == TArray<FString>({ FString(ShieldId) }));
	}
	FMantleWeaponAsset WithOffhand;
	const FString SwordWithShield = Sword.Replace(TEXT("\"fire\":"),
		*FString::Printf(TEXT("\"offhand\":{\"mesh\":\"%s\",\"sockets\":{\"grip\":{\"bone\":\"root\"},\"face\":{\"bone\":\"root\"}}},\"fire\":"), ShieldId));
	TestFalse(TEXT("an off-hand mesh inside a weapon is refused"), FMantleWeaponAsset::Read(Utf8(SwordWithShield), WithOffhand, Error));
	TestTrue(TEXT("naming offhand"), Error.Contains(TEXT("offhand")));

	// #897: a fist weapon (unarmed) names its mesh like any; one with none is refused in a creator's words (empty hands are
	// built into the game, never a document).
	const FString Knuckles = FString::Printf(
		TEXT("{\"schemaVersion\":2,\"name\":\"Brass knuckles\",\"hold\":\"unarmed\",\"delivery\":\"sweep\",\"mesh\":\"%s\",")
			TEXT("\"fire\":{\"interval\":0.4,\"damage\":10},\"sweep\":{\"reach\":60,\"comboCount\":3},\"block\":{}}"),
		MeshId);
	FMantleWeaponAsset Worn;
	if (TestTrue(*FString::Printf(TEXT("a fist weapon reads (%s)"), *Error), FMantleWeaponAsset::Read(Utf8(Knuckles), Worn, Error)))
	{
		TestTrue(TEXT("naming its mesh alone"), Worn.Mesh == MeshId && Worn.NamedIds() == TArray<FString>({ FString(MeshId) }));
	}
	FMantleWeaponAsset Bare;
	TestFalse(TEXT("a fist weapon with no mesh is refused"),
		FMantleWeaponAsset::Read(Utf8(Knuckles.Replace(*FString::Printf(TEXT("\"mesh\":\"%s\","), MeshId), TEXT(""))), Bare, Error));
	TestTrue(*FString::Printf(TEXT("saying it needs a 3D model (%s)"), *Error), Error.Contains(TEXT("needs a 3D model")));

	// A version-1 document still reads (every published weapon is one).
	FMantleWeaponAsset Old;
	TestTrue(TEXT("a version-1 document reads"), FMantleWeaponAsset::Read(Utf8(Document()), Old, Error) && Old.Mesh == MeshId);

	// The cap is on the bytes: the same document padded to exactly MaxBytes reads, one byte more is refused.
	TArray<uint8> AtCap = Utf8(Sword);
	if (TestTrue(TEXT("the document is under the cap"), AtCap.Num() < FMantleWeaponAsset::MaxBytes))
	{
		AtCap.SetNumUninitialized(FMantleWeaponAsset::MaxBytes);
		FMemory::Memset(AtCap.GetData() + Utf8(Sword).Num(), ' ', FMantleWeaponAsset::MaxBytes - Utf8(Sword).Num());
		FMantleWeaponAsset Padded;
		TestTrue(TEXT("padded to the cap, it reads"), FMantleWeaponAsset::Read(AtCap, Padded, Error));
		AtCap.Add(' ');
		TestFalse(TEXT("one byte over, it is refused"), FMantleWeaponAsset::Read(AtCap, Padded, Error));
	}

	// Refused as the cook refuses: a melee hold that fires hitscan, a bow without its draw.
	FMantleWeaponAsset Refused;
	TestFalse(TEXT("a sword that fires hitscan"), FMantleWeaponAsset::Read(Utf8(Sword.Replace(TEXT("\"delivery\":\"sweep\""),
		TEXT("\"delivery\":\"hitscan\""))), Refused, Error));
	TestTrue(TEXT("refused at /delivery"), Error.Contains(TEXT("/delivery")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
