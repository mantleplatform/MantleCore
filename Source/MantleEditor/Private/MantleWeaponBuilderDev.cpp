// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderExport.h"
#include "MantleWeaponBuilderKind.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponSource.h"
#include "MantleRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"

// Dev command (#840): `Mantle.Dev.BuildWeapon <Name> <Hold> [MeshPath] [flip] [full] [stick] [pickup] [blast=<cm>] [fuse=<s>] [speed=<cm/s>] [linger=<s>] [lingerdps=<n>] [lingerradius=<cm>] [charges=<n>] [refill=<s>] [beamdps=<n>] [beamseconds=<s>]` makes a builder weapon without the panel, the way
// its Create does: a source with the hold's default tuning and the model, exported to
// /Game/MantleWeapons/<Name>. flip is the panel's Flip (front and back swapped); full turns on a melee weapon's whole
// kit (four-stage combo, charged heavy, block). A staff's choices: charges (how many casts), refill (seconds to get one back), beamdps and beamseconds
// (the held beam's damage a second and longest hold). For fixtures that Mantle.Preview.TestWeaponLocally <Name> then plays. Logs
// MANTLE_BUILD_WEAPON_OK or MANTLE_BUILD_WEAPON_FAILED with the builder's own messages. packframe (test guns made from the editor's pack
// gun meshes, #840 step 8) does what a creator does by hand in the panel for such a mesh: turns it so its bore (+Y) points forward
// and puts the hand and sight points on its own Grip_R, Grip_L and Sight sockets.
namespace MantleWeaponBuilderDev
{
	static void BuildWeapon(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_BUILD_WEAPON_FAILED usage: Mantle.Dev.BuildWeapon <Name> <Hold> [MeshPath] [flip] [full]"));
			return;
		}
		const int64 HoldValue = StaticEnum<EMantleWeaponHold>()->GetValueByNameString(Args[1]);
		if (HoldValue == INDEX_NONE)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_BUILD_WEAPON_FAILED unknown hold %s"), *Args[1]);
			return;
		}
		// A static mesh, or a skeletal one (the panel takes either; a gun's model is often skeletal).
		UObject* Model = nullptr;
		const bool bFlip = Args.Contains(TEXT("flip"));
		const bool bFull = Args.Contains(TEXT("full"));
		const bool bPackFrame = Args.Contains(TEXT("packframe"));
		// A bow's arrows: stick where they land (stick) and go back into the quiver when walked over (pickup).
		if (Args.Num() > 2 && Args[2] != TEXT("flip") && Args[2] != TEXT("full") && Args[2] != TEXT("stick") && Args[2] != TEXT("pickup")
			&& Args[2] != TEXT("packframe") && !Args[2].Contains(TEXT("=")))
		{
			Model = LoadObject<UObject>(nullptr, *Args[2]);
			if (!Cast<UStaticMesh>(Model) && !Cast<USkeletalMesh>(Model))
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_BUILD_WEAPON_FAILED no static or skeletal mesh at %s"), *Args[2]);
				return;
			}
		}
		const EMantleWeaponHold Hold = static_cast<EMantleWeaponHold>(HoldValue);
		UMantleWeaponSource* Source = NewObject<UMantleWeaponSource>();
		Source->DisplayName = Args[0];
		Source->Hold = Hold;
		Source->Tuning = MantleWeaponBuilderLogic::DefaultTuning(Hold);
		Source->Model = Model;
		Source->Projectile.bStick = Args.Contains(TEXT("stick"));
		Source->Projectile.bPickup = Args.Contains(TEXT("pickup"));
		if (bFlip)
		{
			Source->FlipRotation = FQuat(FVector::UpVector, UE_PI);
		}
		if (bPackFrame && Model)
		{
			// The panel's turn is applied after the fit's own (MeshToWeapon x turn): pick the one that ends at yaw -90.
			FMantleWeaponBuilderKind Kind(MantleWeaponBuilderLogic::ToFitHold(Hold));
			Kind.GuessPoints(Model);
			const FQuat Fitted = Kind.GetLastFit().MeshToWeapon.GetRotation();
			Source->FlipRotation = (FRotator(0.0, -90.0, 0.0).Quaternion() * Fitted.Inverse()).GetNormalized();
			if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Model))
			{
				for (const TCHAR* Role : {TEXT("Grip_R"), TEXT("Grip_L"), TEXT("Sight")})
				{
					if (Skeletal->FindSocket(Role))
					{
						FMantleBuilderPoint& Point = Source->Points.AddDefaulted_GetRef();
						Point.Role = Role;
						Point.Parent = Role;
						Point.Transform = FTransform::Identity;
					}
				}
			}
		}
		// #905: a gun's attack style first (delivery=projectile|beam: the panel's choice, with its defaults), so the numbers below win.
		for (const FString& Arg : Args)
		{
			FString Key, Value;
			if (Arg.Split(TEXT("="), &Key, &Value) && Key == TEXT("delivery"))
			{
				MantleWeaponBuilderLogic::SetDelivery(*Source, Value == TEXT("beam") ? EMantleWeaponSourceDelivery::Beam
					: Value == TEXT("projectile") ? EMantleWeaponSourceDelivery::Projectile : EMantleWeaponSourceDelivery::Hitscan);
			}
		}
		// A throwable's choices: blast (it explodes; radius, cm), fuse (seconds) and speed (cm/s). #905: gravity, round=<mesh path> (the
		// round's model), beamradius=, beamrange= (cm) and element=<style> (Laser, Fire, ...).
		for (const FString& Arg : Args)
		{
			FString Key, Value;
			if (!Arg.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			if (Key == TEXT("blast"))
			{
				Source->Projectile.BlastRadius = FCString::Atof(*Value);
			}
			else if (Key == TEXT("fuse"))
			{
				Source->Projectile.FuseSeconds = FCString::Atof(*Value);
			}
			else if (Key == TEXT("speed"))
			{
				Source->Projectile.Speed = FCString::Atof(*Value);
			}
			else if (Key == TEXT("linger"))
			{
				Source->Projectile.LingerSeconds = FCString::Atof(*Value);
			}
			else if (Key == TEXT("lingerdps"))
			{
				Source->Projectile.LingerDamagePerSecond = FCString::Atof(*Value);
			}
			else if (Key == TEXT("lingerradius"))
			{
				Source->Projectile.LingerRadius = FCString::Atof(*Value);
			}
			else if (Key == TEXT("charges"))
			{
				Source->Charges.Count = FCString::Atoi(*Value);
			}
			else if (Key == TEXT("refill"))
			{
				Source->Charges.RefillSeconds = FCString::Atof(*Value);
			}
			else if (Key == TEXT("beamdps"))
			{
				Source->Beam.DamagePerSecond = FCString::Atof(*Value);
			}
			else if (Key == TEXT("beamseconds"))
			{
				Source->Beam.MaxSeconds = FCString::Atof(*Value);
			}
			else if (Key == TEXT("beamradius"))
			{
				Source->Beam.Radius = FCString::Atof(*Value);
			}
			else if (Key == TEXT("beamrange"))
			{
				Source->Beam.Range = FCString::Atof(*Value);
			}
			else if (Key == TEXT("gravity"))
			{
				Source->Projectile.Gravity = FCString::Atof(*Value);
			}
			else if (Key == TEXT("round"))
			{
				Source->Projectile.Model = TSoftObjectPtr<UObject>(FSoftObjectPath(Value));
			}
			else if (Key == TEXT("element"))
			{
				const int64 Style = StaticEnum<EMantleWeaponSourceStyle>()->GetValueByNameString(Value);
				if (Style != INDEX_NONE)
				{
					Source->bOverrideStyle = true;
					Source->Style = static_cast<EMantleWeaponSourceStyle>(Style);
				}
			}
		}
		if (bFull)
		{
			Source->Sweep.ComboCount = 4;
			Source->Charge.bEnabled = true;
			Source->bBlock = true;
		}
		TArray<FText> Messages;
		const bool bExported = FMantleWeaponBuilderExport::Export(*Source, Messages);
		FString Words;
		for (const FText& Message : Messages)
		{
			Words += TEXT(" | ") + Message.ToString();
		}
		if (bExported)
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_BUILD_WEAPON_OK %s %s%s"), *Args[0], *Args[1], *Words);
		}
		else
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_BUILD_WEAPON_FAILED %s %s%s"), *Args[0], *Args[1], *Words);
		}
	}

	static FAutoConsoleCommand GBuildWeaponCommand(
		TEXT("Mantle.Dev.BuildWeapon"),
		TEXT("Mantle.Dev.BuildWeapon <Name> <Hold> [MeshPath] [flip] [full] [stick] [pickup] [packframe] -- the weapon builder's Create without the panel (default tuning, auto fit; flip: the panel's Flip; full: combo, charge and block; stick, pickup: a bow's arrows; charges=, refill=, beamdps=, beamseconds=: a staff's casts and beam; packframe: a pack gun mesh turned bore-forward on its own hand and sight sockets; ")
		TEXT("#905: delivery=projectile|beam on a gun, speed=, gravity=, blast=, round=<mesh path>, beamradius=, beamrange=, element=<Laser|Fire|...>), ")
		TEXT("exported to /Game/MantleWeapons/<Name>. Logs MANTLE_BUILD_WEAPON_OK or MANTLE_BUILD_WEAPON_FAILED."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&BuildWeapon));
}
