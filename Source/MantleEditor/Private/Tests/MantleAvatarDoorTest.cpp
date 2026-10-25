// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticPanel.h"
#include "MantleAvatarStripFixer.h"
#include "MantleSettings.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The one Avatar door: the "what I see" line, As is greyed with why, and the route the door uses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAvatarDoorTest,
	"Mantle.Editor.CosmeticPanel.AvatarDoor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAvatarDoorTest::RunTest(const FString& Parameters)
{
	using EKind = FMantleAvatarSight::EKind;
	auto Sight = [](const EKind Kind, const FString& Name)
	{
		FMantleAvatarSight Out;
		Out.Kind = Kind;
		Out.Name = Name;
		return Out;
	};

	// The line.
	TestEqual(TEXT("nothing picked: no line"), FMantleConvertSection::WhatISee(FMantleAvatarSight()), FString());
	FMantleAvatarSight Skeletal = Sight(EKind::SkeletalMesh, TEXT("SK_Vampire"));
	Skeletal.Bones = 68;
	TestEqual(TEXT("skeletal mesh, before the look"), FMantleConvertSection::WhatISee(Skeletal), FString(TEXT("SK_Vampire: Skeletal mesh, 68 bones.")));
	Skeletal.bLooked = true;
	Skeletal.bCovered = true;
	TestEqual(TEXT("skeletal mesh, looked, covered"), FMantleConvertSection::WhatISee(Skeletal),
		FString(TEXT("SK_Vampire: Skeletal mesh, 68 bones, rigged. The body is covered by clothing.")));
	FMantleAvatarSight Weightless = Skeletal;
	Weightless.bCovered = false;
	Weightless.bUnrigged = true;
	TestEqual(TEXT("skeleton with no usable weights (chunk 4)"), FMantleConvertSection::WhatISee(Weightless),
		FString(TEXT("SK_Vampire: Skeletal mesh, 68 bones. Skeleton found, but no usable weights: we'll fit it to a MetaHuman body.")));
	FMantleAvatarSight OneBone = Sight(EKind::SkeletalMesh, TEXT("SK_Statue"));
	OneBone.Bones = 1;
	OneBone.bUnrigged = true;
	TestEqual(TEXT("one bone"), FMantleConvertSection::WhatISee(OneBone), FString(TEXT("SK_Statue: Skeletal mesh with one bone, no rig: we'll fit it to a MetaHuman body.")));
	const FMantleAvatarSight Static = Sight(EKind::StaticMesh, TEXT("SM_Anime"));
	TestEqual(TEXT("static mesh"), FMantleConvertSection::WhatISee(Static), FString(TEXT("SM_Anime: Static mesh, no skeleton.")));
	const FMantleAvatarSight Creator = Sight(EKind::CreatorCharacter, TEXT("MHC_MetaGoblin"));
	TestEqual(TEXT("Creator character"), FMantleConvertSection::WhatISee(Creator), FString(TEXT("MHC_MetaGoblin: MetaHuman Creator character.")));
	FMantleAvatarSight Blueprint = Sight(EKind::Blueprint, TEXT("BP_Ada"));
	TestEqual(TEXT("Blueprint, before the look"), FMantleConvertSection::WhatISee(Blueprint), FString(TEXT("BP_Ada: MetaHuman Blueprint.")));
	Blueprint.bLooked = true;
	Blueprint.Clothing = 3;
	Blueprint.Hair = 2;
	TestEqual(TEXT("Blueprint, looked"), FMantleConvertSection::WhatISee(Blueprint), FString(TEXT("BP_Ada: MetaHuman Blueprint: body, 3 clothing pieces, hair.")));

	// What the converter found feeds the line: unrigged (Convert route only) and covered (Strip's face-only line ticked).
	{
		FMantleConvertContext Context;
		Context.Route = EMantleAvatarRoute::Convert;
		Context.Parts.AddDefaulted_GetRef().bUnrigged = true;
		FMantleConvertFix Face;
		Face.FixerId = MantleAvatarFixerIds::Strip;
		Face.PartIndex = 0;
		Face.Kind = FMantleAvatarStripFixer::KindFaceOnly;
		Face.bEnabled = true;
		FMantleAvatarSight Seen = Sight(EKind::SkeletalMesh, TEXT("SK_Girl"));
		Seen.Bones = 70;
		FMantleConvertSection::SeeContext(Context, { Face }, Seen);
		TestTrue(TEXT("looked"), Seen.bLooked);
		TestTrue(TEXT("unrigged from the converter"), Seen.bUnrigged);
		TestTrue(TEXT("covered from Strip"), Seen.bCovered);

		Context.Route = EMantleAvatarRoute::AsIs;
		Context.Parts[0].bUnrigged = false;
		Face.bEnabled = false;
		FMantleConvertSection::SeeContext(Context, { Face }, Seen);
		TestTrue(TEXT("As-is never clears what Convert saw"), Seen.bUnrigged);
		TestFalse(TEXT("face-only line off: not covered"), Seen.bCovered);

		FMantleAvatarSight Assembled = Sight(EKind::Blueprint, TEXT("BP_Ada"));
		Face.bEnabled = true;
		FMantleConvertSection::SeeContext(Context, { Face }, Assembled);
		TestFalse(TEXT("a Blueprint's face-only line isn't about clothing"), Assembled.bCovered);
	}

	// As is greyed, with why.
	TestEqual(TEXT("Creator character: already a MetaHuman"), FMantleConvertSection::AsIsGreyedWhy(Creator), FString(TEXT("it's already a MetaHuman")));
	TestEqual(TEXT("static mesh it can't auto-rig: no rig to keep"), FMantleConvertSection::AsIsGreyedWhy(Static), FString(TEXT("no rig to keep")));
	TestEqual(TEXT("weightless skeleton it can't auto-rig: no rig to keep"), FMantleConvertSection::AsIsGreyedWhy(Weightless), FString(TEXT("no rig to keep")));
	// Chunk 7: a model with no rig goes As is when it can be auto-rigged here; else the auto-rig's own why.
	FMantleAvatarSight Rigs = Static;
	Rigs.bAutoRigs = true;
	TestEqual(TEXT("static mesh it can auto-rig: As is offered"), FMantleConvertSection::AsIsGreyedWhy(Rigs), FString());
	FMantleAvatarSight WeightlessRigs = Weightless;
	WeightlessRigs.bAutoRigs = true;
	TestEqual(TEXT("weightless skeleton it can auto-rig: As is offered"), FMantleConvertSection::AsIsGreyedWhy(WeightlessRigs), FString());
	FMantleAvatarSight NoRenderer = Static;
	NoRenderer.AutoRigNote = TEXT("It has no rig, and fitting one needs a renderer, which this editor runs without.");
	TestEqual(TEXT("can't auto-rig here: says why"), FMantleConvertSection::AsIsGreyedWhy(NoRenderer), NoRenderer.AutoRigNote);
	TestTrue(TEXT("static mesh it can auto-rig: As is used"), FMantleConvertSection::DoorRoute(EMantleAvatarRoute::AsIs, Rigs)
		== (UMantleSettings::AllowsAsIsAvatars() ? EMantleAvatarRoute::AsIs : EMantleAvatarRoute::Convert));
	TestEqual(TEXT("rigged skeletal mesh: As is offered"), FMantleConvertSection::AsIsGreyedWhy(Skeletal), FString());
	TestEqual(TEXT("Blueprint: As is offered"), FMantleConvertSection::AsIsGreyedWhy(Blueprint), FString());

	// The route: Convert is the default; As is only while offered and not greyed.
	TestTrue(TEXT("Convert picked"), FMantleConvertSection::DoorRoute(EMantleAvatarRoute::Convert, Skeletal) == EMantleAvatarRoute::Convert);
	TestTrue(TEXT("As is picked for a Creator character: Convert"), FMantleConvertSection::DoorRoute(EMantleAvatarRoute::AsIs, Creator) == EMantleAvatarRoute::Convert);
	TestTrue(TEXT("As is picked for a rigged mesh: As is only while allowed"), FMantleConvertSection::DoorRoute(EMantleAvatarRoute::AsIs, Skeletal)
		== (UMantleSettings::AllowsAsIsAvatars() ? EMantleAvatarRoute::AsIs : EMantleAvatarRoute::Convert));
	return true;
}

#endif
