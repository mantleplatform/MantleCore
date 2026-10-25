// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePreviewLaunch.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// The preview manifest's optional "weapons" section: read when present, empty when absent, and every malformed shape refused
// with a plain message.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePreviewWeaponsManifestTestPrivate
{
	const TCHAR* const AssetId = TEXT("ast_u0123456789abcdef0123456789abcdef");
	const TCHAR* const OtherAssetId = TEXT("ast_ufedcba9876543210fedcba9876543210");

	/** A bundle folder with a place file, a 4-byte asset file and a weapon file; Load reads a manifest around the weapons text. */
	struct FBundle
	{
		FString Dir;

		FBundle()
		{
			Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/PreviewWeapons") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
			FFileHelper::SaveStringToFile(TEXT("{}"), *(Dir / TEXT("place.json")));
			FFileHelper::SaveStringToFile(TEXT("abcd"), *(Dir / TEXT("a.msk")));
			FFileHelper::SaveStringToFile(TEXT("{\"name\":\"My_Rifle\"}"), *(Dir / TEXT("weapons/My_Rifle.weapon.json")));
		}

		~FBundle()
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		}

		FMantlePreviewManifest Load(const FString& WeaponsMember, const int32 Schema = 2) const
		{
			const FString Hash = TEXT("blake3:") + FString::ChrN(64, TEXT('a'));
			// Schema 1 has no assets section, so its manifest names none.
			const FString Assets = Schema == 1 ? FString() : FString::Printf(
				TEXT(",\"assets\":[{\"id\":\"%s\",\"kind\":\"skeletalmesh\",\"file\":\"a.msk\",\"cookedHash\":\"%s\",\"cookedBytes\":4}]"), AssetId, *Hash);
			const FString Text = FString::Printf(
				TEXT("{\"schemaVersion\":%d,\"placeFile\":\"place.json\",\"scripts\":[]%s%s}"), Schema, *Assets, *WeaponsMember);
			const FString Manifest = Dir / TEXT("manifest.json");
			FFileHelper::SaveStringToFile(Text, *Manifest);
			return MantlePreviewLaunch::LoadManifest(Manifest);
		}
	};

	FString Weapons(const FString& Entry)
	{
		return FString::Printf(TEXT(",\"weapons\":[%s]"), *Entry);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewWeaponsManifestReadTest,
	"Mantle.Runtime.PreviewWeapons.Manifest.ReadsWeaponsAndTheirAbsence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewWeaponsManifestReadTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewWeaponsManifestTestPrivate;

	const FBundle Bundle;

	const FMantlePreviewManifest None = Bundle.Load(FString());
	TestTrue(TEXT("a manifest without weapons loads"), None.bSuccess);
	TestEqual(TEXT("and has none"), None.Weapons.Num(), 0);

	const FMantlePreviewManifest Empty = Bundle.Load(TEXT(",\"weapons\":[]"));
	TestTrue(TEXT("an empty weapons array loads"), Empty.bSuccess);

	const FString Entry = FString::Printf(TEXT("{\"file\":\"weapons/My_Rifle.weapon.json\",\"assets\":{\"SKM_Rifle\":\"%s\"},\"equip\":true}"), AssetId);
	const FMantlePreviewManifest One = Bundle.Load(Weapons(Entry));
	if (!TestTrue(TEXT("a weapon loads"), One.bSuccess))
	{
		AddError(One.Error);
		return false;
	}
	if (!TestEqual(TEXT("one weapon"), One.Weapons.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("file"), One.Weapons[0].File, FString(TEXT("weapons/My_Rifle.weapon.json")));
	TestEqual(TEXT("text"), One.Weapons[0].Text, FString(TEXT("{\"name\":\"My_Rifle\"}")));
	TestTrue(TEXT("equip"), One.Weapons[0].bEquip);
	TestEqual(TEXT("the local name maps to the asset"), One.Weapons[0].Assets.FindRef(TEXT("SKM_Rifle")), FString(AssetId));

	const FString NoEquip = FString::Printf(TEXT("{\"file\":\"weapons/My_Rifle.weapon.json\",\"assets\":{}}"));
	const FMantlePreviewManifest Plain = Bundle.Load(Weapons(NoEquip));
	if (TestTrue(TEXT("equip and assets are optional in value"), Plain.bSuccess) && TestEqual(TEXT("one"), Plain.Weapons.Num(), 1))
	{
		TestFalse(TEXT("equip defaults off"), Plain.Weapons[0].bEquip);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewWeaponsManifestRefuseTest,
	"Mantle.Runtime.PreviewWeapons.Manifest.RefusesMalformedWeapons",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewWeaponsManifestRefuseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewWeaponsManifestTestPrivate;

	const FBundle Bundle;
	auto Entry = [](const TCHAR* File, const FString& Assets, const TCHAR* Extra = TEXT(""))
	{
		return FString::Printf(TEXT("{\"file\":\"%s\",\"assets\":%s%s}"), File, *Assets, Extra);
	};
	const FString GoodAssets = FString::Printf(TEXT("{\"SKM_Rifle\":\"%s\"}"), AssetId);

	auto Refuses = [&](const TCHAR* What, const FMantlePreviewManifest& Manifest, const TCHAR* ErrorPart)
	{
		TestFalse(What, Manifest.bSuccess);
		TestTrue(*FString::Printf(TEXT("%s says why (%s)"), What, *Manifest.Error), Manifest.Error.Contains(ErrorPart));
		TestEqual(*FString::Printf(TEXT("%s leaves no weapons"), What), Manifest.Weapons.Num(), 0);
	};

	Refuses(TEXT("an unknown inner key"), Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.weapon.json"), GoodAssets, TEXT(",\"slot\":1")))), TEXT("Unknown preview weapon field 'slot'"));
	Refuses(TEXT("a parent path"), Bundle.Load(Weapons(Entry(TEXT("../My_Rifle.weapon.json"), GoodAssets))), TEXT("relative"));
	Refuses(TEXT("an absolute path"), Bundle.Load(Weapons(Entry(TEXT("/weapons/My_Rifle.weapon.json"), GoodAssets))), TEXT("relative"));
	Refuses(TEXT("a backslash path"), Bundle.Load(Weapons(Entry(TEXT("weapons\\\\My_Rifle.weapon.json"), GoodAssets))), TEXT("relative"));
	Refuses(TEXT("a wrong extension"), Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.json"), GoodAssets))), TEXT(".weapon.json"));
	Refuses(TEXT("a missing file"), Bundle.Load(Weapons(Entry(TEXT("weapons/None.weapon.json"), GoodAssets))), TEXT("missing"));
	Refuses(TEXT("an asset id that is not an id"), Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.weapon.json"), TEXT("{\"SKM_Rifle\":\"nope\"}")))), TEXT("not an asset id"));
	Refuses(TEXT("an asset the bundle lacks"),
		Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.weapon.json"), FString::Printf(TEXT("{\"SKM_Rifle\":\"%s\"}"), OtherAssetId)))), TEXT("not an asset of this bundle"));
	Refuses(TEXT("assets that are not an object"), Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.weapon.json"), TEXT("[]")))), TEXT("needs a file and an assets object"));
	Refuses(TEXT("an equip that is not a boolean"), Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.weapon.json"), GoodAssets, TEXT(",\"equip\":\"yes\"")))), TEXT("equip"));
	Refuses(TEXT("a material that is not a material asset of the bundle"),
		Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.weapon.json"), GoodAssets, *FString::Printf(TEXT(",\"materials\":[\"%s\"]"), AssetId)))), TEXT("not a material asset"));
	Refuses(TEXT("materials that are not an array"),
		Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.weapon.json"), GoodAssets, TEXT(",\"materials\":{}")))), TEXT("array of at most 64"));
	Refuses(TEXT("weapons that are not an array"), Bundle.Load(TEXT(",\"weapons\":{}")), TEXT("must be an array"));
	Refuses(TEXT("the same file twice"),
		Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.weapon.json"), GoodAssets) + TEXT(",") + Entry(TEXT("weapons/my_rifle.weapon.json"), GoodAssets))), TEXT("more than once"));
	Refuses(TEXT("weapons in a schema 1 manifest"),
		Bundle.Load(Weapons(Entry(TEXT("weapons/My_Rifle.weapon.json"), TEXT("{}"))), /*Schema*/ 1), TEXT("Unknown preview manifest field 'weapons'"));
	return true;
}

// The preview manifest's optional "packages" section: each script package's storage name and package.json, so Test locally
// types and defaults Script Point parameters as a published place does. Read leniently like a stored package's manifest; every
// malformed entry, and a package the bundle carries no script for, is refused.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePreviewPackagesManifestTestPrivate
{
	/** A bundle folder with a place file and one KillBricks package script; Load reads a manifest around the packages text. */
	struct FPackagesBundle
	{
		FString Dir;

		FPackagesBundle()
		{
			Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/PreviewPackages") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
			FFileHelper::SaveStringToFile(TEXT("{}"), *(Dir / TEXT("place.json")));
			FFileHelper::SaveStringToFile(TEXT("print(Script.Package)"), *(Dir / TEXT("Scripts/Packages/KillBricks/Server/Main.luau")));
		}

		~FPackagesBundle()
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		}

		FMantlePreviewManifest Load(const FString& PackagesMember, const int32 Schema = 2) const
		{
			const FString Text = FString::Printf(
				TEXT("{\"schemaVersion\":%d,\"placeFile\":\"place.json\",\"scripts\":[{\"path\":\"Packages/KillBricks/Server/Main.luau\",\"side\":\"server\",")
					TEXT("\"file\":\"Scripts/Packages/KillBricks/Server/Main.luau\"}]%s}"),
				Schema, *PackagesMember);
			const FString Manifest = Dir / TEXT("manifest.json");
			FFileHelper::SaveStringToFile(Text, *Manifest);
			return MantlePreviewLaunch::LoadManifest(Manifest);
		}
	};

	FString PackagesMember(const FString& Entries)
	{
		return FString::Printf(TEXT(",\"packages\":[%s]"), *Entries);
	}

	/** KillBricks' manifest as the export writes it (FMantleScriptPackages::ManifestToJsonObject: every default as text). */
	const TCHAR* const KillBricksEntry =
		TEXT("{\"name\":\"KillBricks\",\"manifest\":{\"name\":\"Kill bricks\",\"description\":\"Touch to die\",\"parameters\":[")
			TEXT("{\"name\":\"Damage\",\"type\":\"number\",\"default\":\"100\",\"help\":\"How much\"},")
			TEXT("{\"name\":\"Instant\",\"type\":\"boolean\",\"default\":\"true\",\"help\":\"\"},")
			TEXT("{\"name\":\"Message\",\"type\":\"string\",\"default\":\"Ouch\",\"help\":\"\"}]}}");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewPackagesManifestReadTest,
	"Mantle.Runtime.PreviewPackages.Manifest.ReadsPackagesAndTheirAbsence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewPackagesManifestReadTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewPackagesManifestTestPrivate;

	const FPackagesBundle Bundle;

	const FMantlePreviewManifest None = Bundle.Load(FString());
	if (!TestTrue(TEXT("a bundle with package scripts and no packages section loads (as older bundles do)"), None.bSuccess))
	{
		AddError(None.Error);
		return false;
	}
	TestEqual(TEXT("and has no package manifests"), None.Packages.Num(), 0);
	TestTrue(TEXT("an empty packages array loads"), Bundle.Load(TEXT(",\"packages\":[]")).bSuccess);

	const FMantlePreviewManifest One = Bundle.Load(PackagesMember(KillBricksEntry));
	if (!TestTrue(TEXT("a package manifest loads"), One.bSuccess))
	{
		AddError(One.Error);
		return false;
	}
	if (!TestEqual(TEXT("one package"), One.Packages.Num(), 1))
	{
		return false;
	}
	const FMantlePreviewPackage& Package = One.Packages[0];
	TestEqual(TEXT("its storage name"), Package.Name, FString(TEXT("KillBricks")));
	TestEqual(TEXT("its manifest name"), Package.Manifest.Name, FString(TEXT("Kill bricks")));
	TestEqual(TEXT("its description"), Package.Manifest.Description, FString(TEXT("Touch to die")));
	if (TestEqual(TEXT("three parameters"), Package.Manifest.Parameters.Num(), 3))
	{
		const FMantleScriptPackageParameter* Damage = Package.Manifest.FindParameter(TEXT("Damage"));
		TestTrue(TEXT("Damage is a number with its text default"), Damage && Damage->Type == EMantleScriptPackageParameterType::Number
			&& Damage->bHasDefault && Damage->Default == TEXT("100"));
		const FMantleScriptPackageParameter* Instant = Package.Manifest.FindParameter(TEXT("Instant"));
		TestTrue(TEXT("Instant is a boolean defaulting true"), Instant && Instant->Type == EMantleScriptPackageParameterType::Boolean
			&& Instant->bHasDefault && Instant->Default.Equals(TEXT("true"), ESearchCase::CaseSensitive));
		const FMantleScriptPackageParameter* Message = Package.Manifest.FindParameter(TEXT("Message"));
		TestTrue(TEXT("Message is a string"), Message && Message->Type == EMantleScriptPackageParameterType::String && Message->Default == TEXT("Ouch"));
	}

	// Read leniently, as a stored package's manifest is: a bad parameter is left out, not the bundle refused.
	const FMantlePreviewManifest Lenient = Bundle.Load(PackagesMember(
		TEXT("{\"name\":\"KillBricks\",\"manifest\":{\"name\":\"K\",\"parameters\":[{\"name\":\"9bad\",\"type\":\"string\"},{\"name\":\"odd\",\"type\":\"vector\"},")
			TEXT("{\"name\":\"speed\",\"type\":\"number\",\"default\":4}],\"extra\":1}}")));
	if (TestTrue(TEXT("a manifest with bad parameters still loads"), Lenient.bSuccess) && TestEqual(TEXT("one package"), Lenient.Packages.Num(), 1))
	{
		TestEqual(TEXT("only the good parameter is kept"), Lenient.Packages[0].Manifest.Parameters.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewPackagesManifestRefuseTest,
	"Mantle.Runtime.PreviewPackages.Manifest.RefusesMalformedPackages",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewPackagesManifestRefuseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewPackagesManifestTestPrivate;

	const FPackagesBundle Bundle;
	auto Refuses = [&](const TCHAR* What, const FMantlePreviewManifest& Manifest, const TCHAR* ErrorPart)
	{
		TestFalse(What, Manifest.bSuccess);
		TestTrue(*FString::Printf(TEXT("%s says why (%s)"), What, *Manifest.Error), Manifest.Error.Contains(ErrorPart));
		TestEqual(*FString::Printf(TEXT("%s leaves no packages"), What), Manifest.Packages.Num(), 0);
	};

	Refuses(TEXT("a package with no script files in the bundle"),
		Bundle.Load(PackagesMember(TEXT("{\"name\":\"Other\",\"manifest\":{\"name\":\"O\"}}"))), TEXT("has no script files in the bundle"));
	Refuses(TEXT("a package name in another case than its scripts"),
		Bundle.Load(PackagesMember(TEXT("{\"name\":\"killbricks\",\"manifest\":{\"name\":\"K\"}}"))), TEXT("has no script files in the bundle"));
	Refuses(TEXT("a name that is not a storage name"),
		Bundle.Load(PackagesMember(TEXT("{\"name\":\"Kill Bricks\",\"manifest\":{\"name\":\"K\"}}"))), TEXT("storage name"));
	Refuses(TEXT("a missing manifest"), Bundle.Load(PackagesMember(TEXT("{\"name\":\"KillBricks\"}"))), TEXT("needs a name and a manifest object"));
	Refuses(TEXT("a manifest that is not an object"),
		Bundle.Load(PackagesMember(TEXT("{\"name\":\"KillBricks\",\"manifest\":\"{}\"}"))), TEXT("needs a name and a manifest object"));
	Refuses(TEXT("an unknown inner key"),
		Bundle.Load(PackagesMember(TEXT("{\"name\":\"KillBricks\",\"manifest\":{},\"files\":[]}"))), TEXT("Unknown preview script package field 'files'"));
	Refuses(TEXT("an entry that is not an object"), Bundle.Load(PackagesMember(TEXT("\"KillBricks\""))), TEXT("must be an object"));
	Refuses(TEXT("packages that are not an array"), Bundle.Load(TEXT(",\"packages\":{}")), TEXT("must be an array"));
	Refuses(TEXT("the same package twice"),
		Bundle.Load(PackagesMember(FString(KillBricksEntry) + TEXT(",") + KillBricksEntry)), TEXT("more than once"));
	Refuses(TEXT("packages in a schema 1 manifest"),
		Bundle.Load(PackagesMember(KillBricksEntry), /*Schema*/ 1), TEXT("Unknown preview manifest field 'packages'"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
