// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponDocument.h"
#include "MantlePlaceLimits.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleWeaponDocumentPrivate
{
	// The schema's spellings, in its order (Mantle.Weapon.DocumentText.HoldsMatchSchema holds them to weapon.schema.json).
	const TCHAR* const GWeaponDocArchetypeNames[] = {
		TEXT("pistol"), TEXT("revolver"), TEXT("rifle"), TEXT("smg"), TEXT("dmr"), TEXT("pump_shotgun"), TEXT("bolt_sniper"), TEXT("lmg")
	};
	const TCHAR* const GWeaponDocHoldNames[] = {
		TEXT("pistol"), TEXT("revolver"), TEXT("rifle"), TEXT("smg"), TEXT("dmr"), TEXT("pump_shotgun"), TEXT("bolt_sniper"), TEXT("lmg"),
		TEXT("melee_1h"), TEXT("melee_2h"), TEXT("spear"), TEXT("staff"), TEXT("bow"), TEXT("unarmed"), TEXT("shield"), TEXT("throwable"),
		TEXT("consumable")
	};
	const TCHAR* const GWeaponDocDeliveryNames[] = { TEXT("hitscan"), TEXT("projectile"), TEXT("sweep"), TEXT("beam"), TEXT("use") };
	const TCHAR* const GWeaponDocResourceNames[] = { TEXT("magazine"), TEXT("quiver"), TEXT("charges"), TEXT("none") };
	// #904: the firearms' three, then the elements (EMantleWeaponDocEffects).
	const TCHAR* const GWeaponDocEffectsNames[] = {
		TEXT("rifle"), TEXT("pistol"), TEXT("shotgun"), TEXT("laser"), TEXT("fire"), TEXT("ice"), TEXT("lightning"), TEXT("arcane"), TEXT("venom"),
		TEXT("water"), TEXT("wind"), TEXT("rock"), TEXT("sand"), TEXT("void"), TEXT("dark"), TEXT("light"), TEXT("generic"), TEXT("heal"), TEXT("lava"),
		TEXT("nature")
	};
	const TCHAR* const GWeaponDocFireModeNames[] = { TEXT("semi"), TEXT("burst"), TEXT("auto") };
	const TCHAR* const GWeaponDocReloadStyleNames[] = { TEXT("magazine"), TEXT("perRound"), TEXT("byRoundCount") };
	const TCHAR* const GWeaponDocRoleNames[] = {
		TEXT("bolt"), TEXT("trigger"), TEXT("hammer"), TEXT("slide"), TEXT("cylinder"), TEXT("pump"), TEXT("feed"), TEXT("magazine"),
		TEXT("magazine_release"), TEXT("shell"), TEXT("cartridge"), TEXT("speed_loader"), TEXT("round"), TEXT("arrow")
	};
	const TCHAR* const GWeaponDocActionNames[] = {
		TEXT("equip"), TEXT("holster"), TEXT("idle"), TEXT("fire"), TEXT("fire_last"), TEXT("reload_tactical"), TEXT("reload_empty"),
		TEXT("reload_start"), TEXT("reload_start_empty"), TEXT("reload_loop"), TEXT("reload_end"), TEXT("reload_partial"), TEXT("attack"),
		TEXT("heavy"), TEXT("block"), TEXT("throw"), TEXT("cast"), TEXT("nock"), TEXT("draw"), TEXT("release"), TEXT("use")
	};
	const TCHAR* const GWeaponDocSocketNames[] = {
		TEXT("sight"), TEXT("muzzle"), TEXT("gripR"), TEXT("gripL"), TEXT("magazine"), TEXT("shellEject"), TEXT("edgeBase"), TEXT("edgeTip"),
		TEXT("tip"), TEXT("focus"), TEXT("stringTop"), TEXT("stringBottom"), TEXT("nock"), TEXT("grip"), TEXT("face")
	};
	// #840: version 2's fields, which a version-1 document has none of.
	const TCHAR* const GWeaponDocV2Fields[] = {
		TEXT("hold"), TEXT("delivery"), TEXT("charge"), TEXT("block"), TEXT("resource"), TEXT("charges"), TEXT("projectile"), TEXT("sweep"),
		TEXT("beam"), TEXT("effectsColor"), TEXT("consumable")
	};
	// #1025: consumable.style, and what a consumable (delivery use) refuses: a weapon's numbers, and attachments (its mesh shows alone).
	const TCHAR* const GWeaponDocUseStyleNames[] = { TEXT("eatDrink"), TEXT("use") };
	const TCHAR* const GWeaponDocConsumableEffectNames[] = { TEXT("heal"), TEXT("none") };
	const TCHAR* const GWeaponDocUseRefused[] = {
		TEXT("fire"), TEXT("attachments"), TEXT("sounds"), TEXT("firstPerson"), TEXT("spread"), TEXT("sway"), TEXT("recoil")
	};
	const TCHAR* const GWeaponDocInterpNames[] = { TEXT("linear"), TEXT("constant"), TEXT("cubic") };

	/** A name table's length. */
	template <int32 N>
	constexpr int32 WeaponDocCount(const TCHAR* const (&)[N])
	{
		return N;
	}

	static_assert(WeaponDocCount(GWeaponDocArchetypeNames) == static_cast<int32>(EMantleWeaponDocArchetype::Count), "one name per archetype");
	static_assert(WeaponDocCount(GWeaponDocHoldNames) == static_cast<int32>(EMantleWeaponDocHold::Count), "one name per hold");
	static_assert(WeaponDocCount(GWeaponDocDeliveryNames) == static_cast<int32>(EMantleWeaponDocDelivery::Count), "one name per delivery");
	static_assert(WeaponDocCount(GWeaponDocResourceNames) == static_cast<int32>(EMantleWeaponDocResource::Count), "one name per resource");
	static_assert(static_cast<int32>(EMantleWeaponDocArchetype::Count) == static_cast<int32>(EMantleWeaponDocHold::Melee1h),
		"the archetypes are the first holds, in order");
	static_assert(WeaponDocCount(GWeaponDocEffectsNames) == static_cast<int32>(EMantleWeaponDocEffects::Count), "one name per effects set");
	static_assert(WeaponDocCount(GWeaponDocFireModeNames) == static_cast<int32>(EMantleWeaponDocFireMode::Count), "one name per fire mode");
	static_assert(WeaponDocCount(GWeaponDocReloadStyleNames) == static_cast<int32>(EMantleWeaponDocReloadStyle::Count), "one name per reload style");
	static_assert(WeaponDocCount(GWeaponDocRoleNames) == static_cast<int32>(EMantleWeaponDocRole::Count), "one name per role");
	static_assert(WeaponDocCount(GWeaponDocActionNames) == static_cast<int32>(EMantleWeaponDocAction::Count), "one name per action");
	static_assert(WeaponDocCount(GWeaponDocSocketNames) == static_cast<int32>(EMantleWeaponDocSocket::Count), "one name per socket");
	static_assert(WeaponDocCount(GWeaponDocInterpNames) == static_cast<int32>(EMantleWeaponDocInterp::Count), "one name per interpolation");
	static_assert(WeaponDocCount(GWeaponDocUseStyleNames) == static_cast<int32>(EMantleWeaponDocUseStyle::Count), "one name per use style");
	static_assert(WeaponDocCount(GWeaponDocConsumableEffectNames) == static_cast<int32>(EMantleWeaponDocConsumableEffect::Count), "one name per consumable effect");

	/** Names[Index], or "" past the table. */
	template <int32 N>
	const TCHAR* WeaponDocNameAt(const TCHAR* const (&Names)[N], const int32 Index)
	{
		return Index >= 0 && Index < N ? Names[Index] : TEXT("");
	}

	/** $defs vec2, vec3 and vec4: each number within this, either way. */
	constexpr double WeaponDocVectorLimit = 1000.0;
	/** $defs curveKey's numbers. */
	constexpr double WeaponDocCurveLimit = 100000.0;
	/** An attachment's scale on an axis. */
	constexpr double WeaponDocScaleLimit = 100.0;
	/** A rotation's numbers. */
	constexpr double WeaponDocRotationLimit = 1.0;
	/** Every name and local name, in characters. */
	constexpr int32 WeaponDocNameMax = 64;
	/** How far from 1 a rotation quaternion's length may be, and how near 0 an attachment's scale on an axis (the vehicle parts' rules). */
	constexpr double WeaponDocRotationTolerance = 0.01;
	constexpr double WeaponDocMinScale = 0.001;
	/** The writer puts a value on one line when it fits in this many columns. */
	constexpr int32 WeaponDocLineWidth = 120;

	template <int32 N>
	FString WeaponDocNameList(const TCHAR* const (&Names)[N])
	{
		FString Out;
		for (int32 Index = 0; Index < N; ++Index)
		{
			Out += Index == 0 ? TEXT("") : (Index == N - 1 ? TEXT(" or ") : TEXT(", "));
			Out += Names[Index];
		}
		return Out;
	}

	FString WeaponDocFormatG(const double Value, const int32 Digits)
	{
		return FString::Printf(TEXT("%.*g"), (Digits >= 1 && Digits <= 16) ? Digits : 17, Value);
	}

	/** A number in its shortest form that reads back exactly: whole numbers without a point, 0.1 as 0.1. */
	FString WeaponDocNumber(const double Value)
	{
		if (Value == 0.0)
		{
			return TEXT("0");
		}
		if (FMath::Abs(Value) < 1e15 && Value == FMath::RoundToDouble(Value))
		{
			return FString::Printf(TEXT("%lld"), static_cast<int64>(Value));
		}
		for (int32 Digits = 1; Digits < 17; ++Digits)
		{
			const FString Text = WeaponDocFormatG(Value, Digits);
			if (FCString::Atod(*Text) == Value)
			{
				return Text;
			}
		}
		return WeaponDocFormatG(Value, 17);
	}

	/** Text as a JSON string. */
	FString WeaponDocQuote(const FString& Text)
	{
		FString Out = TEXT("\"");
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Character = Text[Index];
			if (Character == TEXT('"'))
			{
				Out += TEXT("\\\"");
			}
			else if (Character == TEXT('\\'))
			{
				Out += TEXT("\\\\");
			}
			else if (Character == TEXT('\n'))
			{
				Out += TEXT("\\n");
			}
			else if (Character == TEXT('\r'))
			{
				Out += TEXT("\\r");
			}
			else if (Character == TEXT('\t'))
			{
				Out += TEXT("\\t");
			}
			else if (static_cast<uint32>(Character) < 0x20u)
			{
				Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character));
			}
			else
			{
				Out.AppendChar(Character);
			}
		}
		Out += TEXT("\"");
		return Out;
	}

	/** A string's length as JSON Schema counts it: code points, a surrogate pair once. */
	int32 WeaponDocCodePoints(const FString& Text)
	{
		int32 Count = 0;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const uint32 Unit = static_cast<uint32>(Text[Index]);
			Count += (Unit >= 0xDC00u && Unit <= 0xDFFFu) ? 0 : 1;
		}
		return Count;
	}

	bool WeaponDocIsWordChar(const TCHAR Character)
	{
		return (Character >= TEXT('A') && Character <= TEXT('Z')) || (Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_');
	}

	/** $defs.localName's pattern: ^[A-Za-z0-9_][A-Za-z0-9_.-]*$. */
	bool WeaponDocIsLocalName(const FString& Text)
	{
		if (Text.IsEmpty() || !WeaponDocIsWordChar(Text[0]))
		{
			return false;
		}
		for (int32 Index = 1; Index < Text.Len(); ++Index)
		{
			const TCHAR Character = Text[Index];
			if (!WeaponDocIsWordChar(Character) && Character != TEXT('.') && Character != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	/** $defs.attachment name's pattern: ^[A-Za-z0-9_]+$. */
	bool WeaponDocIsAttachmentName(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return false;
		}
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			if (!WeaponDocIsWordChar(Text[Index]))
			{
				return false;
			}
		}
		return true;
	}

	/** A JSON value as a message quotes it. */
	FString WeaponDocDescribe(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("missing");
		}
		switch (Value->Type)
		{
		case EJson::Null:
			return TEXT("null");
		case EJson::String:
			return FString::Printf(TEXT("\"%s\""), *Value->AsString().Left(WeaponDocNameMax + 16));
		case EJson::Number:
			return WeaponDocNumber(Value->AsNumber());
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Array:
			return TEXT("a list");
		case EJson::Object:
			return TEXT("an object");
		default:
			return TEXT("nothing");
		}
	}

	/** A firearm hold's entry: x-archetypes' row as it was before #840 (reload style, hands, effects, roles, actions), and
	 *  what every firearm shares: hitscan or projectile, no charge, a magazine, the sight, muzzle and gripR points (gripL,
	 *  magazine and shellEject optional) and a stowed round. */
	FMantleWeaponDocHoldInfo WeaponDocFirearm(const EMantleWeaponDocReloadStyle Style, const bool bTwoHanded,
		const EMantleWeaponDocEffects Effects, std::initializer_list<EMantleWeaponDocRole> Roles, std::initializer_list<EMantleWeaponDocAction> Actions)
	{
		using ESocket = EMantleWeaponDocSocket;
		FMantleWeaponDocHoldInfo Info;
		Info.bHasReloadStyle = true;
		Info.ReloadStyle = Style;
		Info.bTwoHanded = bTwoHanded;
		Info.bHasEffects = true;
		Info.Effects = Effects;
		Info.Roles = TArray<EMantleWeaponDocRole>(Roles);
		Info.StowedRoles = { EMantleWeaponDocRole::Round };
		Info.Actions = TArray<EMantleWeaponDocAction>(Actions);
		// #905: a gun may launch a round (a grenade or rocket launcher) or channel a beam (a laser, a flamethrower).
		Info.Deliveries = { EMantleWeaponDocDelivery::Hitscan, EMantleWeaponDocDelivery::Projectile, EMantleWeaponDocDelivery::Beam };
		Info.Charge = EMantleWeaponDocChargeRule::None;
		Info.Resources = { EMantleWeaponDocResource::Magazine };
		Info.BlockRule = EMantleWeaponDocBlockRule::None;
		Info.bMeshRequired = true;
		Info.Sockets = { ESocket::Sight, ESocket::Muzzle, ESocket::GripR };
		Info.OptionalSockets = { ESocket::GripL, ESocket::Magazine, ESocket::ShellEject };
		return Info;
	}

	/** A hold that holds no rounds and has no shot effects (#840): melee, magic, unarmed. The bow is one of these with its
	 *  quiver and reload style set after. */
	FMantleWeaponDocHoldInfo WeaponDocHold(const bool bTwoHanded, std::initializer_list<EMantleWeaponDocAction> Actions,
		std::initializer_list<EMantleWeaponDocDelivery> Deliveries, const EMantleWeaponDocChargeRule Charge,
		std::initializer_list<EMantleWeaponDocResource> Resources, const EMantleWeaponDocBlockRule BlockRule,
		std::initializer_list<EMantleWeaponDocSocket> Sockets, std::initializer_list<EMantleWeaponDocSocket> OptionalSockets)
	{
		FMantleWeaponDocHoldInfo Info;
		Info.bHasReloadStyle = false;
		Info.bTwoHanded = bTwoHanded;
		Info.bHasEffects = false;
		Info.Actions = TArray<EMantleWeaponDocAction>(Actions);
		Info.Deliveries = TArray<EMantleWeaponDocDelivery>(Deliveries);
		Info.Charge = Charge;
		Info.Resources = TArray<EMantleWeaponDocResource>(Resources);
		Info.BlockRule = BlockRule;
		Info.bMeshRequired = true;
		Info.Sockets = TArray<EMantleWeaponDocSocket>(Sockets);
		Info.OptionalSockets = TArray<EMantleWeaponDocSocket>(OptionalSockets);
		return Info;
	}

	/** weapon.schema.json x-holds, entry for entry, in the hold enum's order (Mantle.Weapon.DocumentText.HoldsMatchSchema). */
	TArray<FMantleWeaponDocHoldInfo> BuildWeaponDocHolds()
	{
		using ERole = EMantleWeaponDocRole;
		using EAction = EMantleWeaponDocAction;
		using EStyle = EMantleWeaponDocReloadStyle;
		using EEffects = EMantleWeaponDocEffects;
		using EDelivery = EMantleWeaponDocDelivery;
		using ECharge = EMantleWeaponDocChargeRule;
		using EResource = EMantleWeaponDocResource;
		using ESocket = EMantleWeaponDocSocket;
		using EBlock = EMantleWeaponDocBlockRule;
		const std::initializer_list<EAction> MagazineActions = {
			EAction::Equip, EAction::Holster, EAction::Idle, EAction::Fire, EAction::FireLast, EAction::ReloadTactical, EAction::ReloadEmpty
		};
		const std::initializer_list<EAction> PerRoundActions = {
			EAction::Equip, EAction::Holster, EAction::Idle, EAction::Fire, EAction::FireLast, EAction::ReloadStart, EAction::ReloadStartEmpty,
			EAction::ReloadLoop, EAction::ReloadEnd
		};
		const std::initializer_list<EAction> ByRoundCountActions = {
			EAction::Equip, EAction::Holster, EAction::Idle, EAction::Fire, EAction::FireLast, EAction::ReloadPartial, EAction::ReloadEmpty
		};
		const std::initializer_list<EAction> MeleeActions = {
			EAction::Equip, EAction::Holster, EAction::Idle, EAction::Attack, EAction::Heavy, EAction::Block
		};
		const std::initializer_list<EAction> SpearActions = {
			EAction::Equip, EAction::Holster, EAction::Idle, EAction::Attack, EAction::Heavy, EAction::Block, EAction::Throw
		};
		TArray<FMantleWeaponDocHoldInfo> Table;
		// pistol
		Table.Add(WeaponDocFirearm(EStyle::Magazine, false, EEffects::Pistol,
			{ ERole::Slide, ERole::Trigger, ERole::Hammer, ERole::Magazine, ERole::MagazineRelease }, MagazineActions));
		// revolver
		Table.Add(WeaponDocFirearm(EStyle::ByRoundCount, false, EEffects::Pistol,
			{ ERole::Cylinder, ERole::Trigger, ERole::Hammer, ERole::Cartridge, ERole::SpeedLoader }, ByRoundCountActions));
		// rifle, smg, dmr
		for (int32 Copy = 0; Copy < 3; ++Copy)
		{
			Table.Add(WeaponDocFirearm(EStyle::Magazine, true, EEffects::Rifle,
				{ ERole::Bolt, ERole::Trigger, ERole::Magazine, ERole::MagazineRelease }, MagazineActions));
		}
		// pump_shotgun
		Table.Add(WeaponDocFirearm(EStyle::PerRound, true, EEffects::Shotgun,
			{ ERole::Pump, ERole::Trigger, ERole::Feed, ERole::Shell }, PerRoundActions));
		// bolt_sniper
		Table.Add(WeaponDocFirearm(EStyle::Magazine, true, EEffects::Rifle,
			{ ERole::Bolt, ERole::Trigger, ERole::Magazine, ERole::MagazineRelease, ERole::Cartridge }, MagazineActions));
		// lmg
		Table.Add(WeaponDocFirearm(EStyle::Magazine, true, EEffects::Rifle,
			{ ERole::Bolt, ERole::Trigger, ERole::Feed, ERole::Magazine, ERole::MagazineRelease }, MagazineActions));
		// melee_1h
		Table.Add(WeaponDocHold(false, MeleeActions, { EDelivery::Sweep }, ECharge::Optional, { EResource::None }, EBlock::Optional,
			{ ESocket::GripR, ESocket::EdgeBase, ESocket::EdgeTip }, { ESocket::GripL }));
		// melee_2h
		Table.Add(WeaponDocHold(true, MeleeActions, { EDelivery::Sweep }, ECharge::Optional, { EResource::None }, EBlock::Optional,
			{ ESocket::GripR, ESocket::GripL, ESocket::EdgeBase, ESocket::EdgeTip }, {}));
		// spear
		Table.Add(WeaponDocHold(true, SpearActions, { EDelivery::Sweep, EDelivery::Projectile }, ECharge::Optional, { EResource::None }, EBlock::Optional,
			{ ESocket::GripR, ESocket::GripL, ESocket::Tip, ESocket::EdgeBase }, { ESocket::EdgeTip }));
		// staff
		Table.Add(WeaponDocHold(true, { EAction::Equip, EAction::Holster, EAction::Idle, EAction::Cast, EAction::Heavy },
			{ EDelivery::Projectile, EDelivery::Beam }, ECharge::Optional, { EResource::Charges, EResource::None }, EBlock::None,
			{ ESocket::GripR, ESocket::GripL, ESocket::Focus }, { ESocket::Sight }));
		// bow: its quiver holds arrows, nocked by one clip (the magazine reload style).
		{
			FMantleWeaponDocHoldInfo Bow = WeaponDocHold(true, { EAction::Equip, EAction::Holster, EAction::Idle, EAction::Nock, EAction::Draw, EAction::Release },
				{ EDelivery::Projectile }, ECharge::Required, { EResource::Quiver }, EBlock::None,
				{ ESocket::GripL, ESocket::StringTop, ESocket::StringBottom, ESocket::Nock }, { ESocket::Sight });
			Bow.bHasReloadStyle = true;
			Bow.ReloadStyle = EStyle::Magazine;
			Bow.StowedRoles = { ERole::Arrow };
			Table.Add(Bow);
		}
		// unarmed (#897): a fist weapon worn on the hand (knuckles, claws, a gauntlet), its mesh required like any; empty hands
		// are built into the game and are never a document.
		Table.Add(WeaponDocHold(false, MeleeActions, { EDelivery::Sweep }, ECharge::Optional, { EResource::None }, EBlock::Optional,
			{}, { ESocket::GripR, ESocket::GripL }));
		// shield: its own item in the left hand; it blocks and bashes (a sweep from grip to face), never charges.
		Table.Add(WeaponDocHold(false, { EAction::Equip, EAction::Holster, EAction::Block, EAction::Attack }, { EDelivery::Sweep },
			ECharge::None, { EResource::None }, EBlock::Required, { ESocket::Grip, ESocket::Face }, {}));
		// throwable: one hand, thrown overhand; its projectile is the weapon itself (a press throws, no charge).
		Table.Add(WeaponDocHold(false, { EAction::Equip, EAction::Holster, EAction::Idle, EAction::Throw }, { EDelivery::Projectile },
			ECharge::None, { EResource::None }, EBlock::None, { ESocket::GripR }, { ESocket::Tip }));
		// consumable (#1025): used on its holder, one hand; where the palm holds it is optional (grip).
		Table.Add(WeaponDocHold(false, { EAction::Use }, { EDelivery::Use }, ECharge::None, { EResource::None }, EBlock::None, {}, { ESocket::Grip }));
		check(Table.Num() == static_cast<int32>(EMantleWeaponDocHold::Count));
		return Table;
	}

	using FWeaponDocItems = TArray<TSharedPtr<FJsonValue>>;

	enum class EWeaponDocText : uint8
	{
		/** Any 1 to 64 characters (a display name, a bone, a trigger input). */
		Any,
		/** $defs.localName. */
		LocalName,
		/** An attachment's name. */
		AttachmentName,
	};

	/** Walks a document, adding a problem for everything the schema refuses; reads on past each one to find the rest. */
	class FWeaponDocReader
	{
	public:
		explicit FWeaponDocReader(TArray<FMantleWeaponDocError>& InErrors)
			: Errors(InErrors)
		{
		}

		TArray<FMantleWeaponDocError>& Errors;

		void Add(const FString& Path, const TCHAR* Keyword, const FString& Message)
		{
			FMantleWeaponDocError& Error = Errors.AddDefaulted_GetRef();
			Error.Path = Path;
			Error.Keyword = Keyword;
			Error.Message = Message;
		}

		static FString At(const FString& Path, const TCHAR* Key)
		{
			return Path.IsEmpty() ? FString(Key) : Path + TEXT("/") + Key;
		}

		static FString AtIndex(const FString& Path, const int32 Index)
		{
			return At(Path, *FString::FromInt(Index));
		}

		/** Obj's Key, spelled exactly (the object's own lookup ignores case). */
		static TSharedPtr<FJsonValue> Field(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key)
		{
			for (const auto& Pair : Obj->Values)
			{
				if (FString(Pair.Key.ToView()).Equals(Key, ESearchCase::CaseSensitive))
				{
					return Pair.Value;
				}
			}
			return nullptr;
		}

		/** Only the known keys, and every required one. */
		void KeysIn(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* const* Known, const int32 KnownCount,
			const TCHAR* const* Required, const int32 RequiredCount)
		{
			TArray<FString> Unknown;
			for (const auto& Pair : Obj->Values)
			{
				const FString Key(Pair.Key.ToView());
				bool bKnown = false;
				for (int32 Index = 0; Index < KnownCount; ++Index)
				{
					bKnown = bKnown || Key.Equals(Known[Index], ESearchCase::CaseSensitive);
				}
				if (!bKnown)
				{
					Unknown.Add(FString::Printf(TEXT("\"%s\""), *Key));
				}
			}
			if (Unknown.Num() > 0)
			{
				TArray<FString> KnownNames;
				for (int32 Index = 0; Index < KnownCount; ++Index)
				{
					KnownNames.Add(Known[Index]);
				}
				Add(Path, TEXT("additionalProperties"),
					FString::Printf(TEXT("has %s, which a weapon document does not have here: remove it, or correct its spelling (keys are case-sensitive: %s)"),
						*FString::Join(Unknown, TEXT(", ")), *FString::Join(KnownNames, TEXT(", "))));
			}
			for (int32 Index = 0; Index < RequiredCount; ++Index)
			{
				if (!Field(Obj, Required[Index]).IsValid())
				{
					Add(Path, TEXT("required"), FString::Printf(TEXT("is missing %s: add it"), Required[Index]));
				}
			}
		}

		void Keys(const TSharedPtr<FJsonObject>& Obj, const FString& Path, std::initializer_list<const TCHAR*> Known,
			std::initializer_list<const TCHAR*> Required = {})
		{
			KeysIn(Obj, Path, Known.begin(), static_cast<int32>(Known.size()), Required.begin(), static_cast<int32>(Required.size()));
		}

		/** Every one of Names, which the object's other rules do not require (#840: each version requires its own). */
		void Required(const TSharedPtr<FJsonObject>& Obj, const FString& Path, std::initializer_list<const TCHAR*> Names)
		{
			for (const TCHAR* Name : Names)
			{
				if (!Field(Obj, Name).IsValid())
				{
					Add(Path, TEXT("required"), FString::Printf(TEXT("is missing %s: add it"), Name));
				}
			}
		}

		bool Object(const TSharedPtr<FJsonValue>& Value, const FString& Path, TSharedPtr<FJsonObject>& Out)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
			{
				Add(Path, TEXT("type"), FString::Printf(TEXT("is %s, where an object goes: write it as { ... }"), *WeaponDocDescribe(Value)));
				return false;
			}
			Out = Value->AsObject();
			return true;
		}

		bool InRange(const double Number, const FString& Path, const double Min, const double Max, double& Out)
		{
			if (Number < Min)
			{
				Add(Path, TEXT("minimum"), FString::Printf(TEXT("is %s, below the least it may be: raise it to %s or more"),
					*WeaponDocNumber(Number), *WeaponDocNumber(Min)));
				return false;
			}
			if (Number > Max)
			{
				Add(Path, TEXT("maximum"), FString::Printf(TEXT("is %s, above the most it may be: lower it to %s or less"),
					*WeaponDocNumber(Number), *WeaponDocNumber(Max)));
				return false;
			}
			Out = Number;
			return true;
		}

		bool Number(const TSharedPtr<FJsonValue>& Value, const FString& Path, const double Min, const double Max, double& Out)
		{
			if (!Value.IsValid() || Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber()))
			{
				Add(Path, TEXT("type"), FString::Printf(TEXT("is %s, where a number goes: write a number from %s to %s"),
					*WeaponDocDescribe(Value), *WeaponDocNumber(Min), *WeaponDocNumber(Max)));
				return false;
			}
			return InRange(Value->AsNumber(), Path, Min, Max, Out);
		}

		bool Integer(const TSharedPtr<FJsonValue>& Value, const FString& Path, const int32 Min, const int32 Max, int32& Out)
		{
			const bool bNumber = Value.IsValid() && Value->Type == EJson::Number && FMath::IsFinite(Value->AsNumber());
			if (!bNumber || Value->AsNumber() != FMath::RoundToDouble(Value->AsNumber()))
			{
				Add(Path, TEXT("type"), FString::Printf(TEXT("is %s, where a whole number goes: write a whole number from %d to %d"),
					*WeaponDocDescribe(Value), Min, Max));
				return false;
			}
			double Whole = 0.0;
			if (!InRange(Value->AsNumber(), Path, static_cast<double>(Min), static_cast<double>(Max), Whole))
			{
				return false;
			}
			Out = static_cast<int32>(Whole);
			return true;
		}

		bool Boolean(const TSharedPtr<FJsonValue>& Value, const FString& Path, bool& Out)
		{
			if (!Value.IsValid() || Value->Type != EJson::Boolean)
			{
				Add(Path, TEXT("type"), FString::Printf(TEXT("is %s, where true or false goes: write true or false"), *WeaponDocDescribe(Value)));
				return false;
			}
			Out = Value->AsBool();
			return true;
		}

		bool Text(const TSharedPtr<FJsonValue>& Value, const FString& Path, const EWeaponDocText Rule, FString& Out)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				Add(Path, TEXT("type"), FString::Printf(TEXT("is %s, where a name goes: write it in quotes"), *WeaponDocDescribe(Value)));
				return false;
			}
			const FString String = Value->AsString();
			const int32 Length = WeaponDocCodePoints(String);
			bool bOk = true;
			if (Length < 1)
			{
				Add(Path, TEXT("minLength"), TEXT("is empty: give it a name"));
				bOk = false;
			}
			if (Length > WeaponDocNameMax)
			{
				Add(Path, TEXT("maxLength"), FString::Printf(TEXT("is %d characters long: shorten it to %d or fewer"), Length, WeaponDocNameMax));
				bOk = false;
			}
			if (Rule == EWeaponDocText::LocalName && !WeaponDocIsLocalName(String))
			{
				Add(Path, TEXT("pattern"), FString::Printf(TEXT("is \"%s\", which is not a local name: name the asset by its own name alone (letters, digits, _, . and -, not starting with . or -), never by a path"),
					*String.Left(WeaponDocNameMax + 16)));
				bOk = false;
			}
			if (Rule == EWeaponDocText::AttachmentName && !WeaponDocIsAttachmentName(String))
			{
				Add(Path, TEXT("pattern"), FString::Printf(TEXT("is \"%s\": rename it with letters, digits and _ only"), *String.Left(WeaponDocNameMax + 16)));
				bOk = false;
			}
			if (bOk)
			{
				Out = String;
			}
			return bOk;
		}

		template <typename EnumType, int32 N>
		bool Choice(const TSharedPtr<FJsonValue>& Value, const FString& Path, const TCHAR* const (&Names)[N], EnumType& Out)
		{
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				const FString String = Value->AsString();
				for (int32 Index = 0; Index < N; ++Index)
				{
					if (String.Equals(Names[Index], ESearchCase::CaseSensitive))
					{
						Out = static_cast<EnumType>(Index);
						return true;
					}
				}
			}
			Add(Path, TEXT("enum"), FString::Printf(TEXT("is %s, which is none of %s: use one of them, spelled exactly"),
				*WeaponDocDescribe(Value), *WeaponDocNameList(Names)));
			return false;
		}

		/** A list of Min to Max entries; the entries are the caller's to read. Null when it is not a list. */
		const FWeaponDocItems* Items(const TSharedPtr<FJsonValue>& Value, const FString& Path, const int32 Min, const int32 Max)
		{
			if (!Value.IsValid() || Value->Type != EJson::Array)
			{
				Add(Path, TEXT("type"), FString::Printf(TEXT("is %s, where a list goes: write it as [ ... ]"), *WeaponDocDescribe(Value)));
				return nullptr;
			}
			const FWeaponDocItems& List = Value->AsArray();
			if (List.Num() < Min)
			{
				Add(Path, TEXT("minItems"), FString::Printf(TEXT("has %d entries: give it at least %d"), List.Num(), Min));
			}
			if (List.Num() > Max)
			{
				Add(Path, TEXT("maxItems"), FString::Printf(TEXT("has %d entries: give it at most %d"), List.Num(), Max));
			}
			return &List;
		}

		/** uniqueItems on a list of names: the same spelling twice. */
		void UniqueStrings(const FWeaponDocItems& List, const FString& Path)
		{
			for (int32 Index = 1; Index < List.Num(); ++Index)
			{
				for (int32 Earlier = 0; Earlier < Index; ++Earlier)
				{
					const TSharedPtr<FJsonValue>& A = List[Index];
					const TSharedPtr<FJsonValue>& B = List[Earlier];
					if (A.IsValid() && B.IsValid() && A->Type == EJson::String && B->Type == EJson::String
						&& A->AsString().Equals(B->AsString(), ESearchCase::CaseSensitive))
					{
						Add(Path, TEXT("uniqueItems"), FString::Printf(TEXT("lists \"%s\" twice: list it once"), *A->AsString()));
						return;
					}
				}
			}
		}

		/** Exactly Count numbers within Limit either way. */
		bool Numbers(const TSharedPtr<FJsonValue>& Value, const FString& Path, const int32 Count, const double Limit, double* Out)
		{
			const FWeaponDocItems* List = Items(Value, Path, Count, Count);
			if (!List)
			{
				return false;
			}
			bool bOk = List->Num() == Count;
			for (int32 Index = 0; Index < List->Num(); ++Index)
			{
				double Component = 0.0;
				if (Number((*List)[Index], AtIndex(Path, Index), -Limit, Limit, Component))
				{
					if (Index < Count)
					{
						Out[Index] = Component;
					}
				}
				else
				{
					bOk = false;
				}
			}
			return bOk;
		}

		/** A list of names of Names, Min to Max, each once. */
		template <typename EnumType, int32 N>
		bool Choices(const TSharedPtr<FJsonValue>& Value, const FString& Path, const int32 Min, const int32 Max, const TCHAR* const (&Names)[N],
			TArray<EnumType>& Out)
		{
			const FWeaponDocItems* List = Items(Value, Path, Min, Max);
			if (!List)
			{
				return false;
			}
			UniqueStrings(*List, Path);
			for (int32 Index = 0; Index < List->Num(); ++Index)
			{
				EnumType Parsed = static_cast<EnumType>(0);
				if (Choice((*List)[Index], AtIndex(Path, Index), Names, Parsed))
				{
					Out.Add(Parsed);
				}
			}
			return true;
		}

		// Optional fields: read and set when the object has them.

		void OptNumber(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, const double Min, const double Max,
			TMantleWeaponDocValue<double>& Out)
		{
			double Parsed = 0.0;
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid() && Number(Value, At(Path, Key), Min, Max, Parsed))
			{
				Out.Set(Parsed);
			}
		}

		void OptInteger(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, const int32 Min, const int32 Max,
			TMantleWeaponDocValue<int32>& Out)
		{
			int32 Parsed = 0;
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid() && Integer(Value, At(Path, Key), Min, Max, Parsed))
			{
				Out.Set(Parsed);
			}
		}

		void OptBoolean(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, TMantleWeaponDocValue<bool>& Out)
		{
			bool bParsed = false;
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid() && Boolean(Value, At(Path, Key), bParsed))
			{
				Out.Set(bParsed);
			}
		}

		void OptText(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, const EWeaponDocText Rule,
			TMantleWeaponDocValue<FString>& Out)
		{
			FString Parsed;
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid() && Text(Value, At(Path, Key), Rule, Parsed))
			{
				Out.Set(Parsed);
			}
		}

		void OptVector2(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, TMantleWeaponDocValue<FVector2D>& Out)
		{
			double Parsed[2] = { 0.0, 0.0 };
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid() && Numbers(Value, At(Path, Key), 2, WeaponDocVectorLimit, Parsed))
			{
				Out.Set(FVector2D(Parsed[0], Parsed[1]));
			}
		}

		void OptVector3(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, const double Limit, TMantleWeaponDocValue<FVector>& Out)
		{
			double Parsed[3] = { 0.0, 0.0, 0.0 };
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid() && Numbers(Value, At(Path, Key), 3, Limit, Parsed))
			{
				Out.Set(FVector(Parsed[0], Parsed[1], Parsed[2]));
			}
		}

		void OptVector4(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, TMantleWeaponDocValue<FVector4>& Out)
		{
			double Parsed[4] = { 0.0, 0.0, 0.0, 0.0 };
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid() && Numbers(Value, At(Path, Key), 4, WeaponDocVectorLimit, Parsed))
			{
				Out.Set(FVector4(Parsed[0], Parsed[1], Parsed[2], Parsed[3]));
			}
		}

		/** [x, y, z, w]; its length is CheckRules'. */
		void OptRotation(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, TMantleWeaponDocValue<FQuat>& Out)
		{
			double Parsed[4] = { 0.0, 0.0, 0.0, 1.0 };
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid() && Numbers(Value, At(Path, Key), 4, WeaponDocRotationLimit, Parsed))
			{
				Out.Set(FQuat(Parsed[0], Parsed[1], Parsed[2], Parsed[3]));
			}
		}

		template <typename EnumType, int32 N>
		void OptChoice(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, const TCHAR* const (&Names)[N],
			TMantleWeaponDocValue<EnumType>& Out)
		{
			EnumType Parsed = static_cast<EnumType>(0);
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid() && Choice(Value, At(Path, Key), Names, Parsed))
			{
				Out.Set(Parsed);
			}
		}

		/** An object block the document may leave out, read by Read. */
		template <typename T>
		void OptBlock(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, TMantleWeaponDocValue<T>& Out,
			void (FWeaponDocReader::*Read)(const TSharedPtr<FJsonObject>&, const FString&, T&))
		{
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (!Value.IsValid())
			{
				return;
			}
			const FString BlockPath = At(Path, Key);
			TSharedPtr<FJsonObject> Block;
			if (!Object(Value, BlockPath, Block))
			{
				return;
			}
			T Parsed;
			(this->*Read)(Block, BlockPath, Parsed);
			Out.Set(Parsed);
		}

		/** A required object block (its absence is the parent's required error). */
		template <typename T>
		void ReqBlock(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, T& Out,
			void (FWeaponDocReader::*Read)(const TSharedPtr<FJsonObject>&, const FString&, T&))
		{
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			TSharedPtr<FJsonObject> Block;
			if (Value.IsValid() && Object(Value, At(Path, Key), Block))
			{
				(this->*Read)(Block, At(Path, Key), Out);
			}
		}

		// The schema's $defs.

		/** [time, value], [time, value, "linear" | "constant"] or [time, value, "cubic", arriveTangent, leaveTangent]. */
		void ReadCurveKey(const TSharedPtr<FJsonValue>& Value, const FString& Path, FMantleWeaponDocCurveKey& Out)
		{
			const FWeaponDocItems* List = Items(Value, Path, 2, 5);
			if (!List)
			{
				return;
			}
			const int32 Count = List->Num();
			const double Limit = WeaponDocCurveLimit;
			if (Count > 0)
			{
				Number((*List)[0], AtIndex(Path, 0), -Limit, Limit, Out.Time);
			}
			if (Count > 1)
			{
				Number((*List)[1], AtIndex(Path, 1), -Limit, Limit, Out.Value);
			}
			bool bForm = true;
			if (Count > 2)
			{
				bForm = Choice((*List)[2], AtIndex(Path, 2), GWeaponDocInterpNames, Out.Interp);
			}
			if (Count > 3)
			{
				Number((*List)[3], AtIndex(Path, 3), -Limit, Limit, Out.ArriveTangent);
			}
			if (Count > 4)
			{
				Number((*List)[4], AtIndex(Path, 4), -Limit, Limit, Out.LeaveTangent);
			}
			const bool bCubic = Out.Interp == EMantleWeaponDocInterp::Cubic;
			if (bForm && Count >= 2 && Count <= 5 && ((bCubic && Count != 5) || (!bCubic && Count > 3)))
			{
				Add(Path, TEXT("mantle"), FString::Printf(TEXT("has %d items, but a cubic key has five (time, value, \"cubic\", arriveTangent, leaveTangent) and a linear or constant one two or three (time, value, \"linear\" or \"constant\"): write it in one of those forms"),
					Count));
			}
		}

		void ReadCurve(const TSharedPtr<FJsonValue>& Value, const FString& Path, FMantleWeaponDocCurve& Out)
		{
			const FWeaponDocItems* List = Items(Value, Path, 0, 64);
			if (!List)
			{
				return;
			}
			for (int32 Index = 0; Index < List->Num(); ++Index)
			{
				FMantleWeaponDocCurveKey Key;
				ReadCurveKey((*List)[Index], AtIndex(Path, Index), Key);
				Out.Add(Key);
			}
		}

		void OptCurve(const TSharedPtr<FJsonObject>& Obj, const FString& Path, const TCHAR* Key, TMantleWeaponDocValue<FMantleWeaponDocCurve>& Out)
		{
			const TSharedPtr<FJsonValue> Value = Field(Obj, Key);
			if (Value.IsValid())
			{
				FMantleWeaponDocCurve Parsed;
				ReadCurve(Value, At(Path, Key), Parsed);
				Out.Set(Parsed);
			}
		}

		void ReadCurve3(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocCurve3& Out)
		{
			Keys(Obj, Path, { TEXT("x"), TEXT("y"), TEXT("z") }, { TEXT("x"), TEXT("y"), TEXT("z") });
			if (const TSharedPtr<FJsonValue> X = Field(Obj, TEXT("x")))
			{
				ReadCurve(X, At(Path, TEXT("x")), Out.X);
			}
			if (const TSharedPtr<FJsonValue> Y = Field(Obj, TEXT("y")))
			{
				ReadCurve(Y, At(Path, TEXT("y")), Out.Y);
			}
			if (const TSharedPtr<FJsonValue> Z = Field(Obj, TEXT("z")))
			{
				ReadCurve(Z, At(Path, TEXT("z")), Out.Z);
			}
		}

		void ReadSocket(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSocket& Out)
		{
			Keys(Obj, Path, { TEXT("bone"), TEXT("location"), TEXT("rotation") }, { TEXT("bone") });
			if (const TSharedPtr<FJsonValue> Bone = Field(Obj, TEXT("bone")))
			{
				Text(Bone, At(Path, TEXT("bone")), EWeaponDocText::Any, Out.Bone);
			}
			OptVector3(Obj, Path, TEXT("location"), WeaponDocVectorLimit, Out.Location);
			OptRotation(Obj, Path, TEXT("rotation"), Out.Rotation);
		}

		void ReadSockets(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDesc& Out)
		{
			// Which are required is the hold's (#840: CheckRules).
			KeysIn(Obj, Path, GWeaponDocSocketNames, WeaponDocCount(GWeaponDocSocketNames), nullptr, 0);
			for (int32 Index = 0; Index < static_cast<int32>(EMantleWeaponDocSocket::Count); ++Index)
			{
				OptBlock(Obj, Path, GWeaponDocSocketNames[Index], Out.Sockets[Index], &FWeaponDocReader::ReadSocket);
			}
		}

		void ReadPart(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocPart& Out)
		{
			Keys(Obj, Path, { TEXT("bone"), TEXT("shownDuring") }, { TEXT("bone") });
			if (const TSharedPtr<FJsonValue> Bone = Field(Obj, TEXT("bone")))
			{
				Text(Bone, At(Path, TEXT("bone")), EWeaponDocText::Any, Out.Bone);
			}
			if (const TSharedPtr<FJsonValue> Shown = Field(Obj, TEXT("shownDuring")))
			{
				TArray<EMantleWeaponDocAction> Actions;
				if (Choices(Shown, At(Path, TEXT("shownDuring")), 1, 12, GWeaponDocActionNames, Actions))
				{
					Out.ShownDuring.Set(Actions);
				}
			}
		}

		void ReadParts(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocParts& Out)
		{
			KeysIn(Obj, Path, GWeaponDocRoleNames, WeaponDocCount(GWeaponDocRoleNames), nullptr, 0);
			for (int32 Role = 0; Role < static_cast<int32>(EMantleWeaponDocRole::Count); ++Role)
			{
				OptBlock(Obj, Path, GWeaponDocRoleNames[Role], Out.Roles[Role], &FWeaponDocReader::ReadPart);
			}
		}

		void ReadAttachment(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocAttachment& Out)
		{
			Keys(Obj, Path, { TEXT("name"), TEXT("mesh"), TEXT("effect"), TEXT("bone"), TEXT("socket"), TEXT("location"), TEXT("rotation"),
				TEXT("scale"), TEXT("hidden"), TEXT("materials") }, { TEXT("name") });
			if (const TSharedPtr<FJsonValue> Name = Field(Obj, TEXT("name")))
			{
				Text(Name, At(Path, TEXT("name")), EWeaponDocText::AttachmentName, Out.Name);
			}
			if (const TSharedPtr<FJsonValue> Mesh = Field(Obj, TEXT("mesh")))
			{
				Text(Mesh, At(Path, TEXT("mesh")), EWeaponDocText::LocalName, Out.Mesh);
			}
			OptChoice(Obj, Path, TEXT("effect"), GWeaponDocEffectsNames, Out.Effect);
			OptText(Obj, Path, TEXT("bone"), EWeaponDocText::Any, Out.Bone);
			OptChoice(Obj, Path, TEXT("socket"), GWeaponDocSocketNames, Out.Socket);
			OptVector3(Obj, Path, TEXT("location"), WeaponDocVectorLimit, Out.Location);
			OptRotation(Obj, Path, TEXT("rotation"), Out.Rotation);
			OptVector3(Obj, Path, TEXT("scale"), WeaponDocScaleLimit, Out.Scale);
			OptBoolean(Obj, Path, TEXT("hidden"), Out.bHidden);
			ReadMaterials(Obj, Path, Out.Materials);
		}

		/** A materials list (the weapon's own, an attachment's): local names, at most MaxWeaponMaterials; set only when every entry reads. */
		void ReadMaterials(const TSharedPtr<FJsonObject>& Obj, const FString& Path, TMantleWeaponDocValue<TArray<FString>>& Out)
		{
			const TSharedPtr<FJsonValue> Materials = Field(Obj, TEXT("materials"));
			const FString ListPath = At(Path, TEXT("materials"));
			if (!Materials.IsValid())
			{
				return;
			}
			if (const FWeaponDocItems* List = Items(Materials, ListPath, 0, FMantlePlaceLimits::MaxWeaponMaterials))
			{
				TArray<FString> Parsed;
				bool bAllOk = true;
				for (int32 Index = 0; Index < List->Num(); ++Index)
				{
					FString Name;
					if (Text((*List)[Index], AtIndex(ListPath, Index), EWeaponDocText::LocalName, Name))
					{
						Parsed.Add(Name);
					}
					else
					{
						bAllOk = false;
					}
				}
				if (bAllOk)
				{
					Out.Set(Parsed);
				}
			}
		}

		void ReadFire(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocFire& Out)
		{
			Keys(Obj, Path, { TEXT("interval"), TEXT("automatic"), TEXT("modes"), TEXT("damage"), TEXT("maxRange"), TEXT("traceSweepRadius"),
				TEXT("pellets"), TEXT("pelletSpread"), TEXT("plainRecoil") }, { TEXT("interval") });
			if (const TSharedPtr<FJsonValue> Interval = Field(Obj, TEXT("interval")))
			{
				Number(Interval, At(Path, TEXT("interval")), 0.01, 10.0, Out.Interval);
			}
			OptBoolean(Obj, Path, TEXT("automatic"), Out.bAutomatic);
			if (const TSharedPtr<FJsonValue> Modes = Field(Obj, TEXT("modes")))
			{
				TArray<EMantleWeaponDocFireMode> Parsed;
				if (Choices(Modes, At(Path, TEXT("modes")), 1, 3, GWeaponDocFireModeNames, Parsed))
				{
					Out.Modes.Set(Parsed);
				}
			}
			OptNumber(Obj, Path, TEXT("damage"), 0.0, 1000.0, Out.Damage);
			OptNumber(Obj, Path, TEXT("maxRange"), 100.0, 100000.0, Out.MaxRange);
			OptNumber(Obj, Path, TEXT("traceSweepRadius"), 0.0, 50.0, Out.TraceSweepRadius);
			OptInteger(Obj, Path, TEXT("pellets"), 1, 32, Out.Pellets);
			OptNumber(Obj, Path, TEXT("pelletSpread"), 0.0, 10.0, Out.PelletSpread);
			OptNumber(Obj, Path, TEXT("plainRecoil"), 0.0, 30.0, Out.PlainRecoil);
		}

		void ReadMagazine(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocMagazine& Out)
		{
			Keys(Obj, Path, { TEXT("size"), TEXT("startingReserve") }, { TEXT("size") });
			if (const TSharedPtr<FJsonValue> Size = Field(Obj, TEXT("size")))
			{
				Integer(Size, At(Path, TEXT("size")), 1, 1000, Out.Size);
			}
			OptInteger(Obj, Path, TEXT("startingReserve"), 0, 10000, Out.StartingReserve);
		}

		void ReadReload(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocReload& Out)
		{
			Keys(Obj, Path, { TEXT("style"), TEXT("seconds") }, { TEXT("style") });
			if (const TSharedPtr<FJsonValue> Style = Field(Obj, TEXT("style")))
			{
				Choice(Style, At(Path, TEXT("style")), GWeaponDocReloadStyleNames, Out.Style);
			}
			OptNumber(Obj, Path, TEXT("seconds"), 0.01, 30.0, Out.Seconds);
		}

		void ReadSounds(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSounds& Out)
		{
			Keys(Obj, Path, { TEXT("fire"), TEXT("fireSuppressed"), TEXT("suppressed"), TEXT("trigger") });
			OptText(Obj, Path, TEXT("fire"), EWeaponDocText::LocalName, Out.Fire);
			OptText(Obj, Path, TEXT("fireSuppressed"), EWeaponDocText::LocalName, Out.FireSuppressed);
			OptBoolean(Obj, Path, TEXT("suppressed"), Out.bSuppressed);
			OptText(Obj, Path, TEXT("trigger"), EWeaponDocText::Any, Out.Trigger);
		}

		void ReadFirstPerson(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocFirstPerson& Out)
		{
			Keys(Obj, Path, { TEXT("sightDistance"), TEXT("walkFullWeightSpeed") });
			OptNumber(Obj, Path, TEXT("sightDistance"), 0.0, 100.0, Out.SightDistance);
			OptNumber(Obj, Path, TEXT("walkFullWeightSpeed"), 1.0, 2000.0, Out.WalkFullWeightSpeed);
		}

		void ReadSpreadState(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSpreadState& Out)
		{
			Keys(Obj, Path, { TEXT("multiplier"), TEXT("rate") });
			OptNumber(Obj, Path, TEXT("multiplier"), 0.0, 10.0, Out.Multiplier);
			OptNumber(Obj, Path, TEXT("rate"), 0.0, 100.0, Out.Rate);
		}

		void ReadSpread(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSpread& Out)
		{
			Keys(Obj, Path, { TEXT("exponent"), TEXT("heatToSpread"), TEXT("heatToHeatPerShot"), TEXT("heatToCooldownPerSecond"),
				TEXT("recoveryDelay"), TEXT("firstShotAccuracy"), TEXT("aiming"), TEXT("standingStill"), TEXT("crouching"),
				TEXT("jumpingOrFalling"), TEXT("standingStillSpeed"), TEXT("standingStillToMovingRange"), TEXT("damageFalloff"),
				TEXT("weakSpotMultiplier") });
			OptNumber(Obj, Path, TEXT("exponent"), 0.1, 10.0, Out.Exponent);
			OptCurve(Obj, Path, TEXT("heatToSpread"), Out.HeatToSpread);
			OptCurve(Obj, Path, TEXT("heatToHeatPerShot"), Out.HeatToHeatPerShot);
			OptCurve(Obj, Path, TEXT("heatToCooldownPerSecond"), Out.HeatToCooldownPerSecond);
			OptNumber(Obj, Path, TEXT("recoveryDelay"), 0.0, 10.0, Out.RecoveryDelay);
			OptBoolean(Obj, Path, TEXT("firstShotAccuracy"), Out.bFirstShotAccuracy);
			OptBlock(Obj, Path, TEXT("aiming"), Out.Aiming, &FWeaponDocReader::ReadSpreadState);
			OptBlock(Obj, Path, TEXT("standingStill"), Out.StandingStill, &FWeaponDocReader::ReadSpreadState);
			OptBlock(Obj, Path, TEXT("crouching"), Out.Crouching, &FWeaponDocReader::ReadSpreadState);
			OptBlock(Obj, Path, TEXT("jumpingOrFalling"), Out.JumpingOrFalling, &FWeaponDocReader::ReadSpreadState);
			OptNumber(Obj, Path, TEXT("standingStillSpeed"), 0.0, 2000.0, Out.StandingStillSpeed);
			OptNumber(Obj, Path, TEXT("standingStillToMovingRange"), 0.0, 2000.0, Out.StandingStillToMovingRange);
			OptCurve(Obj, Path, TEXT("damageFalloff"), Out.DamageFalloff);
			OptNumber(Obj, Path, TEXT("weakSpotMultiplier"), 0.0, 10.0, Out.WeakSpotMultiplier);
		}

		void ReadSwayVector(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSwayVector& Out)
		{
			Keys(Obj, Path, { TEXT("stiffness"), TEXT("damping"), TEXT("interpSpeed"), TEXT("smoothingSpeed"), TEXT("clamp"), TEXT("scale"),
				TEXT("aimScale") });
			OptVector3(Obj, Path, TEXT("stiffness"), WeaponDocVectorLimit, Out.Stiffness);
			OptVector3(Obj, Path, TEXT("damping"), WeaponDocVectorLimit, Out.Damping);
			OptVector3(Obj, Path, TEXT("interpSpeed"), WeaponDocVectorLimit, Out.InterpSpeed);
			OptVector3(Obj, Path, TEXT("smoothingSpeed"), WeaponDocVectorLimit, Out.SmoothingSpeed);
			OptVector3(Obj, Path, TEXT("clamp"), WeaponDocVectorLimit, Out.Clamp);
			OptVector3(Obj, Path, TEXT("scale"), WeaponDocVectorLimit, Out.Scale);
			OptNumber(Obj, Path, TEXT("aimScale"), 0.0, 10.0, Out.AimScale);
		}

		void ReadSwayBlock(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSwayBlock& Out)
		{
			Keys(Obj, Path, { TEXT("position"), TEXT("rotation") });
			OptBlock(Obj, Path, TEXT("position"), Out.Position, &FWeaponDocReader::ReadSwayVector);
			OptBlock(Obj, Path, TEXT("rotation"), Out.Rotation, &FWeaponDocReader::ReadSwayVector);
		}

		void ReadSway(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSway& Out)
		{
			Keys(Obj, Path, { TEXT("movement"), TEXT("look") });
			OptBlock(Obj, Path, TEXT("movement"), Out.Movement, &FWeaponDocReader::ReadSwayBlock);
			OptBlock(Obj, Path, TEXT("look"), Out.Look, &FWeaponDocReader::ReadSwayBlock);
		}

		void ReadRecoilKick(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocRecoilKick& Out)
		{
			Keys(Obj, Path, { TEXT("pitch"), TEXT("yaw"), TEXT("roll"), TEXT("back"), TEXT("right"), TEXT("up"), TEXT("pivot") });
			OptVector2(Obj, Path, TEXT("pitch"), Out.Pitch);
			OptVector4(Obj, Path, TEXT("yaw"), Out.Yaw);
			OptVector4(Obj, Path, TEXT("roll"), Out.Roll);
			OptVector2(Obj, Path, TEXT("back"), Out.Back);
			OptVector2(Obj, Path, TEXT("right"), Out.Right);
			OptVector2(Obj, Path, TEXT("up"), Out.Up);
			OptVector3(Obj, Path, TEXT("pivot"), WeaponDocVectorLimit, Out.Pivot);
		}

		void ReadSpringAmount(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSpringAmount& Out)
		{
			Keys(Obj, Path, { TEXT("amount"), TEXT("damping"), TEXT("interpSpeed") });
			OptNumber(Obj, Path, TEXT("amount"), -1000.0, 1000.0, Out.Amount);
			OptNumber(Obj, Path, TEXT("damping"), 0.0, 1000.0, Out.Damping);
			OptNumber(Obj, Path, TEXT("interpSpeed"), 0.0, 1000.0, Out.InterpSpeed);
		}

		void ReadRecoilCurves(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocRecoilCurves& Out)
		{
			Keys(Obj, Path, { TEXT("semiRotation"), TEXT("semiLocation"), TEXT("autoRotation"), TEXT("autoLocation") });
			OptBlock(Obj, Path, TEXT("semiRotation"), Out.SemiRotation, &FWeaponDocReader::ReadCurve3);
			OptBlock(Obj, Path, TEXT("semiLocation"), Out.SemiLocation, &FWeaponDocReader::ReadCurve3);
			OptBlock(Obj, Path, TEXT("autoRotation"), Out.AutoRotation, &FWeaponDocReader::ReadCurve3);
			OptBlock(Obj, Path, TEXT("autoLocation"), Out.AutoLocation, &FWeaponDocReader::ReadCurve3);
		}

		void ReadRecoilNoise(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocRecoilNoise& Out)
		{
			Keys(Obj, Path, { TEXT("target"), TEXT("interpSpeed"), TEXT("damping"), TEXT("aimScale") });
			OptVector2(Obj, Path, TEXT("target"), Out.Target);
			OptVector2(Obj, Path, TEXT("interpSpeed"), Out.InterpSpeed);
			OptVector2(Obj, Path, TEXT("damping"), Out.Damping);
			OptNumber(Obj, Path, TEXT("aimScale"), 0.0, 10.0, Out.AimScale);
		}

		void ReadRecoilProgress(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocRecoilProgress& Out)
		{
			Keys(Obj, Path, { TEXT("pitch"), TEXT("up"), TEXT("aimScale") });
			OptBlock(Obj, Path, TEXT("pitch"), Out.Pitch, &FWeaponDocReader::ReadSpringAmount);
			OptBlock(Obj, Path, TEXT("up"), Out.Up, &FWeaponDocReader::ReadSpringAmount);
			OptNumber(Obj, Path, TEXT("aimScale"), 0.0, 10.0, Out.AimScale);
		}

		void ReadRecoilSway(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocRecoilSway& Out)
		{
			Keys(Obj, Path, { TEXT("pitch"), TEXT("yaw"), TEXT("rollScale"), TEXT("damping"), TEXT("interpSpeed"), TEXT("aimScale"), TEXT("pivot") });
			OptVector2(Obj, Path, TEXT("pitch"), Out.Pitch);
			OptVector2(Obj, Path, TEXT("yaw"), Out.Yaw);
			OptNumber(Obj, Path, TEXT("rollScale"), -10.0, 10.0, Out.RollScale);
			OptNumber(Obj, Path, TEXT("damping"), 0.0, 1000.0, Out.Damping);
			OptNumber(Obj, Path, TEXT("interpSpeed"), 0.0, 1000.0, Out.InterpSpeed);
			OptNumber(Obj, Path, TEXT("aimScale"), 0.0, 10.0, Out.AimScale);
			OptVector3(Obj, Path, TEXT("pivot"), WeaponDocVectorLimit, Out.Pivot);
		}

		void ReadControllerRecoil(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocControllerRecoil& Out)
		{
			Keys(Obj, Path, { TEXT("yawStep"), TEXT("pitchStep"), TEXT("interpSpeed"), TEXT("damping"), TEXT("compensation") });
			OptVector2(Obj, Path, TEXT("yawStep"), Out.YawStep);
			OptVector2(Obj, Path, TEXT("pitchStep"), Out.PitchStep);
			OptVector2(Obj, Path, TEXT("interpSpeed"), Out.InterpSpeed);
			OptNumber(Obj, Path, TEXT("damping"), 0.0, 1000.0, Out.Damping);
			OptNumber(Obj, Path, TEXT("compensation"), 0.0, 1.0, Out.Compensation);
		}

		void ReadCameraShake(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocCameraShake& Out)
		{
			Keys(Obj, Path, { TEXT("curve"), TEXT("pitch"), TEXT("yaw"), TEXT("roll"), TEXT("smoothing"), TEXT("playRate") });
			OptBlock(Obj, Path, TEXT("curve"), Out.Curve, &FWeaponDocReader::ReadCurve3);
			OptVector2(Obj, Path, TEXT("pitch"), Out.Pitch);
			OptVector2(Obj, Path, TEXT("yaw"), Out.Yaw);
			OptVector2(Obj, Path, TEXT("roll"), Out.Roll);
			OptNumber(Obj, Path, TEXT("smoothing"), 0.0, 1000.0, Out.Smoothing);
			OptNumber(Obj, Path, TEXT("playRate"), 0.01, 10.0, Out.PlayRate);
		}

		void ReadRecoil(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocRecoil& Out)
		{
			Keys(Obj, Path, { TEXT("hip"), TEXT("aim"), TEXT("curves"), TEXT("smoothRotation"), TEXT("smoothLocation"), TEXT("autoRotationScale"),
				TEXT("autoLocationScale"), TEXT("alternateRoll"), TEXT("space"), TEXT("playRate"), TEXT("playbackOffset"), TEXT("burstEndSeconds"),
				TEXT("noise"), TEXT("pushback"), TEXT("progress"), TEXT("sway"), TEXT("controller"), TEXT("cameraShake") });
			OptBlock(Obj, Path, TEXT("hip"), Out.Hip, &FWeaponDocReader::ReadRecoilKick);
			OptBlock(Obj, Path, TEXT("aim"), Out.Aim, &FWeaponDocReader::ReadRecoilKick);
			OptBlock(Obj, Path, TEXT("curves"), Out.Curves, &FWeaponDocReader::ReadRecoilCurves);
			OptVector3(Obj, Path, TEXT("smoothRotation"), WeaponDocVectorLimit, Out.SmoothRotation);
			OptVector3(Obj, Path, TEXT("smoothLocation"), WeaponDocVectorLimit, Out.SmoothLocation);
			OptVector3(Obj, Path, TEXT("autoRotationScale"), WeaponDocVectorLimit, Out.AutoRotationScale);
			OptVector3(Obj, Path, TEXT("autoLocationScale"), WeaponDocVectorLimit, Out.AutoLocationScale);
			OptBoolean(Obj, Path, TEXT("alternateRoll"), Out.bAlternateRoll);
			OptVector3(Obj, Path, TEXT("space"), WeaponDocVectorLimit, Out.Space);
			OptNumber(Obj, Path, TEXT("playRate"), 0.01, 10.0, Out.PlayRate);
			OptNumber(Obj, Path, TEXT("playbackOffset"), 0.0, 10.0, Out.PlaybackOffset);
			OptNumber(Obj, Path, TEXT("burstEndSeconds"), 0.0, 10.0, Out.BurstEndSeconds);
			OptBlock(Obj, Path, TEXT("noise"), Out.Noise, &FWeaponDocReader::ReadRecoilNoise);
			OptBlock(Obj, Path, TEXT("pushback"), Out.Pushback, &FWeaponDocReader::ReadSpringAmount);
			OptBlock(Obj, Path, TEXT("progress"), Out.Progress, &FWeaponDocReader::ReadRecoilProgress);
			OptBlock(Obj, Path, TEXT("sway"), Out.Sway, &FWeaponDocReader::ReadRecoilSway);
			OptBlock(Obj, Path, TEXT("controller"), Out.Controller, &FWeaponDocReader::ReadControllerRecoil);
			OptBlock(Obj, Path, TEXT("cameraShake"), Out.CameraShake, &FWeaponDocReader::ReadCameraShake);
		}

		void ReadCharge(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocCharge& Out)
		{
			Keys(Obj, Path, { TEXT("seconds"), TEXT("required"), TEXT("releaseOnFull"), TEXT("cancelable") }, { TEXT("seconds") });
			if (const TSharedPtr<FJsonValue> Seconds = Field(Obj, TEXT("seconds")))
			{
				Number(Seconds, At(Path, TEXT("seconds")), 0.1, 10.0, Out.Seconds);
			}
			OptBoolean(Obj, Path, TEXT("required"), Out.bRequired);
			OptBoolean(Obj, Path, TEXT("releaseOnFull"), Out.bReleaseOnFull);
			OptBoolean(Obj, Path, TEXT("cancelable"), Out.bCancelable);
		}

		void ReadCharges(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocCharges& Out)
		{
			Keys(Obj, Path, { TEXT("count"), TEXT("refillSeconds") }, { TEXT("count"), TEXT("refillSeconds") });
			if (const TSharedPtr<FJsonValue> Count = Field(Obj, TEXT("count")))
			{
				Integer(Count, At(Path, TEXT("count")), 1, 100, Out.Count);
			}
			if (const TSharedPtr<FJsonValue> Refill = Field(Obj, TEXT("refillSeconds")))
			{
				Number(Refill, At(Path, TEXT("refillSeconds")), 0.1, 600.0, Out.RefillSeconds);
			}
		}

		void ReadProjectile(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocProjectile& Out)
		{
			Keys(Obj, Path, { TEXT("speed"), TEXT("gravity"), TEXT("blastRadius"), TEXT("fuseSeconds"), TEXT("thrown"), TEXT("stick"), TEXT("pickup"), TEXT("lingerSeconds"), TEXT("lingerDamagePerSecond"), TEXT("lingerRadius"), TEXT("mesh"), TEXT("materials") }, { TEXT("speed") });
			if (const TSharedPtr<FJsonValue> Speed = Field(Obj, TEXT("speed")))
			{
				Number(Speed, At(Path, TEXT("speed")), 100.0, 30000.0, Out.Speed);
			}
			OptNumber(Obj, Path, TEXT("gravity"), 0.0, 3.0, Out.Gravity);
			OptNumber(Obj, Path, TEXT("blastRadius"), 0.0, 2000.0, Out.BlastRadius);
			OptNumber(Obj, Path, TEXT("fuseSeconds"), 0.1, 30.0, Out.FuseSeconds);
			OptBoolean(Obj, Path, TEXT("thrown"), Out.bThrown);
			OptBoolean(Obj, Path, TEXT("stick"), Out.bStick);
			OptBoolean(Obj, Path, TEXT("pickup"), Out.bPickup);
			OptNumber(Obj, Path, TEXT("lingerSeconds"), 0.1, 30.0, Out.LingerSeconds);
			OptNumber(Obj, Path, TEXT("lingerDamagePerSecond"), 0.0, 1000.0, Out.LingerDamagePerSecond);
			OptNumber(Obj, Path, TEXT("lingerRadius"), 10.0, 2000.0, Out.LingerRadius);
			OptText(Obj, Path, TEXT("mesh"), EWeaponDocText::LocalName, Out.Mesh);
			ReadMaterials(Obj, Path, Out.Materials);
		}

		void ReadSweepEdge(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSweepEdge& Out)
		{
			Keys(Obj, Path, { TEXT("base"), TEXT("tip") }, { TEXT("base"), TEXT("tip") });
			if (const TSharedPtr<FJsonValue> Base = Field(Obj, TEXT("base")))
			{
				Choice(Base, At(Path, TEXT("base")), GWeaponDocSocketNames, Out.Base);
			}
			if (const TSharedPtr<FJsonValue> Tip = Field(Obj, TEXT("tip")))
			{
				Choice(Tip, At(Path, TEXT("tip")), GWeaponDocSocketNames, Out.Tip);
			}
		}

		void ReadSweep(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocSweep& Out)
		{
			Keys(Obj, Path, { TEXT("reach"), TEXT("edge"), TEXT("comboCount"), TEXT("heavyMultiplier") }, { TEXT("reach") });
			if (const TSharedPtr<FJsonValue> Reach = Field(Obj, TEXT("reach")))
			{
				Number(Reach, At(Path, TEXT("reach")), 30.0, 400.0, Out.Reach);
			}
			OptBlock(Obj, Path, TEXT("edge"), Out.Edge, &FWeaponDocReader::ReadSweepEdge);
			OptInteger(Obj, Path, TEXT("comboCount"), 1, 5, Out.ComboCount);
			OptNumber(Obj, Path, TEXT("heavyMultiplier"), 1.0, 5.0, Out.HeavyMultiplier);
		}

		void ReadBeam(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocBeam& Out)
		{
			Keys(Obj, Path, { TEXT("tickRate"), TEXT("damagePerSecond"), TEXT("maxSeconds"), TEXT("radius"), TEXT("range") }, { TEXT("damagePerSecond") });
			OptNumber(Obj, Path, TEXT("tickRate"), 1.0, 30.0, Out.TickRate);
			if (const TSharedPtr<FJsonValue> Damage = Field(Obj, TEXT("damagePerSecond")))
			{
				Number(Damage, At(Path, TEXT("damagePerSecond")), 0.0, 1000.0, Out.DamagePerSecond);
			}
			OptNumber(Obj, Path, TEXT("maxSeconds"), 0.1, 30.0, Out.MaxSeconds);
			OptNumber(Obj, Path, TEXT("radius"), 0.0, 200.0, Out.Radius);
			OptNumber(Obj, Path, TEXT("range"), 100.0, 100000.0, Out.Range);
		}

		void ReadConsumable(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocConsumable& Out)
		{
			Keys(Obj, Path, { TEXT("style"), TEXT("effect"), TEXT("heal"), TEXT("healSeconds"), TEXT("uses"), TEXT("useSeconds"), TEXT("cooldownSeconds"), TEXT("moveWhileUsing") },
				{ TEXT("style") });
			if (const TSharedPtr<FJsonValue> Style = Field(Obj, TEXT("style")))
			{
				Choice(Style, At(Path, TEXT("style")), GWeaponDocUseStyleNames, Out.Style);
			}
			OptChoice(Obj, Path, TEXT("effect"), GWeaponDocConsumableEffectNames, Out.Effect);
			OptNumber(Obj, Path, TEXT("heal"), 1.0, 1000.0, Out.Heal);
			OptNumber(Obj, Path, TEXT("healSeconds"), 0.0, 60.0, Out.HealSeconds);
			OptInteger(Obj, Path, TEXT("uses"), 1, 100, Out.Uses);
			OptNumber(Obj, Path, TEXT("useSeconds"), 0.5, 10.0, Out.UseSeconds);
			OptNumber(Obj, Path, TEXT("cooldownSeconds"), 0.0, 600.0, Out.CooldownSeconds);
			OptBoolean(Obj, Path, TEXT("moveWhileUsing"), Out.bMoveWhileUsing);
		}

		void ReadBlock(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleWeaponDocBlock& Out)
		{
			Keys(Obj, Path, { TEXT("enabled"), TEXT("breakOnHeavy") });
			OptBoolean(Obj, Path, TEXT("enabled"), Out.bEnabled);
			OptBoolean(Obj, Path, TEXT("breakOnHeavy"), Out.bBreakOnHeavy);
		}

		/**
		 * #840: the rules that depend on which keys the text has, on a document within the schema (CheckRules reads the
		 * values): version 1 has none of version 2's fields; version 2's archetype is its hold's alias; a weapon without a
		 * mesh has no points, parts, attachments or materials; a magazine and a reload exactly when the resource holds
		 * rounds.
		 */
		void CheckKeys(const TSharedPtr<FJsonObject>& Root, const FMantleWeaponDesc& D)
		{
			const int32 Version = D.ReadSchemaVersion;
			const EMantleWeaponDocHold Hold = D.GetHold();
			if (Version == 1)
			{
				for (const TCHAR* Key : GWeaponDocV2Fields)
				{
					if (Field(Root, Key).IsValid())
					{
						Add(Key, TEXT("mantle"), FString::Printf(TEXT("is a schemaVersion 2 field: write schemaVersion 2, or drop %s"), Key));
					}
				}
			}
			if (Version == 2)
			{
				EMantleWeaponDocArchetype Alias = EMantleWeaponDocArchetype::Rifle;
				const TSharedPtr<FJsonValue> ArchetypeValue = Field(Root, TEXT("archetype"));
				if (ArchetypeValue.IsValid() && ArchetypeValue->Type == EJson::String)
				{
					EMantleWeaponDocArchetype HoldArchetype = EMantleWeaponDocArchetype::Rifle;
					const bool bFirearm = FMantleWeaponDocument::ArchetypeOfHold(Hold, HoldArchetype);
					for (int32 Index = 0; Index < WeaponDocCount(GWeaponDocArchetypeNames); ++Index)
					{
						if (ArchetypeValue->AsString().Equals(GWeaponDocArchetypeNames[Index], ESearchCase::CaseSensitive))
						{
							Alias = static_cast<EMantleWeaponDocArchetype>(Index);
						}
					}
					if (!bFirearm || Alias != HoldArchetype)
					{
						Add(TEXT("archetype"), TEXT("mantle"), FString::Printf(TEXT("is %s, but hold is %s: archetype is the hold's alias; drop it"),
							*ArchetypeValue->AsString(), FMantleWeaponDocument::HoldName(Hold)));
					}
				}
			}
			// #1025: a consumable is used, never fired: no weapon numbers, and its mesh alone.
			if (D.GetDelivery() == EMantleWeaponDocDelivery::Use)
			{
				for (const TCHAR* Key : GWeaponDocUseRefused)
				{
					if (Field(Root, Key).IsValid())
					{
						Add(Key, TEXT("mantle"), FString::Printf(TEXT("is a weapon's, but a %s is used, not fired: drop it"), FMantleWeaponDocument::HoldName(Hold)));
					}
				}
			}
			if (!Field(Root, TEXT("mesh")).IsValid())
			{
				for (const TCHAR* Key : { TEXT("sockets"), TEXT("parts"), TEXT("attachments"), TEXT("materials") })
				{
					if (Field(Root, Key).IsValid())
					{
						Add(Key, TEXT("mantle"), TEXT("has no mesh to sit on: add mesh, or drop it"));
					}
				}
			}
			if (Version == 2)
			{
				const bool bRounds = D.UsesRounds();
				const TCHAR* Resource = FMantleWeaponDocument::ResourceName(D.GetResource());
				for (const TCHAR* Key : { TEXT("magazine"), TEXT("reload") })
				{
					const bool bPresent = Field(Root, Key).IsValid();
					if (bRounds && !bPresent)
					{
						Add(FString(), TEXT("mantle"), FString::Printf(TEXT("needs %s: a %s holds rounds"), Key, Resource));
					}
					if (!bRounds && bPresent)
					{
						Add(Key, TEXT("mantle"), FString::Printf(TEXT("is for rounds, but the resource is %s: drop it"), Resource));
					}
				}
			}
		}

		/** An sRGB colour, three integers 0..255 (color, effectsColor). */
		void OptColor(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TMantleWeaponDocValue<FColor>& Out)
		{
			const TSharedPtr<FJsonValue> Color = Field(Obj, Key);
			const FWeaponDocItems* List = Color ? Items(Color, Key, 3, 3) : nullptr;
			if (!List)
			{
				return;
			}
			int32 Channels[3] = { 0, 0, 0 };
			bool bOk = List->Num() == 3;
			for (int32 Index = 0; Index < List->Num(); ++Index)
			{
				int32 Channel = 0;
				if (Integer((*List)[Index], AtIndex(Key, Index), 0, 255, Channel))
				{
					if (Index < 3)
					{
						Channels[Index] = Channel;
					}
				}
				else
				{
					bOk = false;
				}
			}
			if (bOk)
			{
				Out.Set(FColor(static_cast<uint8>(Channels[0]), static_cast<uint8>(Channels[1]), static_cast<uint8>(Channels[2])));
			}
		}

		void ReadRoot(const TSharedPtr<FJsonObject>& Root, FMantleWeaponDesc& Out)
		{
			const FString Path;
			Keys(Root, Path,
				{ TEXT("schemaVersion"), TEXT("name"), TEXT("archetype"), TEXT("hold"), TEXT("delivery"), TEXT("twoHanded"), TEXT("effects"),
				  TEXT("color"), TEXT("effectsColor"), TEXT("mesh"), TEXT("materials"), TEXT("icon"), TEXT("sockets"), TEXT("parts"), TEXT("attachments"), TEXT("fire"),
				  TEXT("charge"), TEXT("magazine"), TEXT("reload"), TEXT("resource"), TEXT("charges"), TEXT("projectile"), TEXT("sweep"), TEXT("beam"),
				  TEXT("consumable"), TEXT("block"), TEXT("sounds"), TEXT("firstPerson"), TEXT("spread"), TEXT("sway"), TEXT("recoil") },
				{ TEXT("schemaVersion"), TEXT("name") });
			// #840: 1 is the firearm document (archetype), 2 names the hold and the delivery; each requires its own fields.
			int32 Version = 0;
			if (const TSharedPtr<FJsonValue> VersionValue = Field(Root, TEXT("schemaVersion")))
			{
				const bool bNumber = VersionValue->Type == EJson::Number;
				for (int32 Known = FMantleWeaponDocument::OldestSchemaVersion; bNumber && Known <= FMantleWeaponDocument::SchemaVersion; ++Known)
				{
					Version = VersionValue->AsNumber() == static_cast<double>(Known) ? Known : Version;
				}
				if (Version == 0)
				{
					Add(TEXT("schemaVersion"), TEXT("enum"), FString::Printf(TEXT("is %s, a version this build does not read: write %d (or %d, the firearm document)"),
						*WeaponDocDescribe(VersionValue), FMantleWeaponDocument::SchemaVersion, FMantleWeaponDocument::OldestSchemaVersion));
				}
			}
			if (Version == 1)
			{
				Required(Root, Path, { TEXT("archetype"), TEXT("mesh"), TEXT("sockets"), TEXT("magazine"), TEXT("reload") });
			}
			else if (Version == 2)
			{
				Required(Root, Path, { TEXT("hold"), TEXT("delivery") });
			}
			Out.ReadSchemaVersion = Version != 0 ? Version : FMantleWeaponDocument::SchemaVersion;
			if (const TSharedPtr<FJsonValue> Name = Field(Root, TEXT("name")))
			{
				Text(Name, TEXT("name"), EWeaponDocText::Any, Out.Name);
			}
			// The hold: version 1's archetype, version 2's hold (archetype then its alias, held to it by CheckKeys).
			EMantleWeaponDocArchetype Archetype = EMantleWeaponDocArchetype::Rifle;
			bool bArchetype = false;
			if (const TSharedPtr<FJsonValue> ArchetypeValue = Field(Root, TEXT("archetype")))
			{
				bArchetype = Choice(ArchetypeValue, TEXT("archetype"), GWeaponDocArchetypeNames, Archetype);
			}
			if (Version == 1 && bArchetype)
			{
				Out.Archetype = Archetype;
				Out.Hold.Set(FMantleWeaponDocument::HoldOfArchetype(Archetype));
				Out.Delivery.Set(EMantleWeaponDocDelivery::Hitscan);
			}
			if (const TSharedPtr<FJsonValue> HoldValue = Field(Root, TEXT("hold")))
			{
				EMantleWeaponDocHold Hold = EMantleWeaponDocHold::Rifle;
				if (Choice(HoldValue, TEXT("hold"), GWeaponDocHoldNames, Hold) && Version != 1)
				{
					Out.Hold.Set(Hold);
					FMantleWeaponDocument::ArchetypeOfHold(Hold, Out.Archetype);
				}
			}
			if (const TSharedPtr<FJsonValue> DeliveryValue = Field(Root, TEXT("delivery")))
			{
				EMantleWeaponDocDelivery Delivery = EMantleWeaponDocDelivery::Hitscan;
				if (Choice(DeliveryValue, TEXT("delivery"), GWeaponDocDeliveryNames, Delivery) && Version != 1)
				{
					Out.Delivery.Set(Delivery);
				}
			}
			// #1025: fire on every delivery but use (a version-1 document's hitscan included); use refuses it (CheckKeys).
			if (!(Out.Delivery.bSet && Out.Delivery.Value == EMantleWeaponDocDelivery::Use))
			{
				Required(Root, Path, { TEXT("fire") });
			}
			OptBoolean(Root, Path, TEXT("twoHanded"), Out.bTwoHanded);
			OptChoice(Root, Path, TEXT("effects"), GWeaponDocEffectsNames, Out.Effects);
			OptColor(Root, TEXT("color"), Out.Color);
			OptColor(Root, TEXT("effectsColor"), Out.EffectsColor);
			if (const TSharedPtr<FJsonValue> Mesh = Field(Root, TEXT("mesh")))
			{
				Text(Mesh, TEXT("mesh"), EWeaponDocText::LocalName, Out.Mesh);
			}
			ReadMaterials(Root, Path, Out.Materials);
			if (const TSharedPtr<FJsonValue> Icon = Field(Root, TEXT("icon")))
			{
				Text(Icon, TEXT("icon"), EWeaponDocText::LocalName, Out.Icon);
			}
			if (const TSharedPtr<FJsonValue> Sockets = Field(Root, TEXT("sockets")))
			{
				TSharedPtr<FJsonObject> SocketsObj;
				if (Object(Sockets, TEXT("sockets"), SocketsObj))
				{
					ReadSockets(SocketsObj, TEXT("sockets"), Out);
				}
			}
			OptBlock(Root, Path, TEXT("parts"), Out.Parts, &FWeaponDocReader::ReadParts);
			if (const TSharedPtr<FJsonValue> Attachments = Field(Root, TEXT("attachments")))
			{
				if (const FWeaponDocItems* List = Items(Attachments, TEXT("attachments"), 0, 16))
				{
					TArray<FMantleWeaponDocAttachment> Parsed;
					for (int32 Index = 0; Index < List->Num(); ++Index)
					{
						const FString AttachmentPath = AtIndex(TEXT("attachments"), Index);
						TSharedPtr<FJsonObject> AttachmentObj;
						FMantleWeaponDocAttachment& Attachment = Parsed.AddDefaulted_GetRef();
						if (Object((*List)[Index], AttachmentPath, AttachmentObj))
						{
							ReadAttachment(AttachmentObj, AttachmentPath, Attachment);
						}
					}
					Out.Attachments.Set(Parsed);
				}
			}
			ReqBlock(Root, Path, TEXT("fire"), Out.Fire, &FWeaponDocReader::ReadFire);
			OptBlock(Root, Path, TEXT("charge"), Out.Charge, &FWeaponDocReader::ReadCharge);
			ReqBlock(Root, Path, TEXT("magazine"), Out.Magazine, &FWeaponDocReader::ReadMagazine);
			ReqBlock(Root, Path, TEXT("reload"), Out.Reload, &FWeaponDocReader::ReadReload);
			OptChoice(Root, Path, TEXT("resource"), GWeaponDocResourceNames, Out.Resource);
			OptBlock(Root, Path, TEXT("charges"), Out.Charges, &FWeaponDocReader::ReadCharges);
			OptBlock(Root, Path, TEXT("projectile"), Out.Projectile, &FWeaponDocReader::ReadProjectile);
			OptBlock(Root, Path, TEXT("sweep"), Out.Sweep, &FWeaponDocReader::ReadSweep);
			OptBlock(Root, Path, TEXT("beam"), Out.Beam, &FWeaponDocReader::ReadBeam);
			OptBlock(Root, Path, TEXT("consumable"), Out.Consumable, &FWeaponDocReader::ReadConsumable);
			OptBlock(Root, Path, TEXT("block"), Out.Block, &FWeaponDocReader::ReadBlock);
			OptBlock(Root, Path, TEXT("sounds"), Out.Sounds, &FWeaponDocReader::ReadSounds);
			OptBlock(Root, Path, TEXT("firstPerson"), Out.FirstPerson, &FWeaponDocReader::ReadFirstPerson);
			OptBlock(Root, Path, TEXT("spread"), Out.Spread, &FWeaponDocReader::ReadSpread);
			OptBlock(Root, Path, TEXT("sway"), Out.Sway, &FWeaponDocReader::ReadSway);
			OptBlock(Root, Path, TEXT("recoil"), Out.Recoil, &FWeaponDocReader::ReadRecoil);
		}
	};

	/** A rotation's length within the tolerance of 1. */
	void CheckWeaponDocRotation(FWeaponDocReader& R, const FQuat& Rotation, const FString& Path)
	{
		const double Length = FMath::Sqrt(Rotation.X * Rotation.X + Rotation.Y * Rotation.Y + Rotation.Z * Rotation.Z + Rotation.W * Rotation.W);
		if (FMath::Abs(Length - 1.0) <= WeaponDocRotationTolerance)
		{
			return;
		}
		if (Length <= UE_DOUBLE_SMALL_NUMBER)
		{
			R.Add(Path, TEXT("mantle"), TEXT("is all zeros, not a rotation: write [0, 0, 0, 1] for none"));
			return;
		}
		R.Add(Path, TEXT("mantle"), FString::Printf(TEXT("is a quaternion of length %.3f, not a unit one: normalise it (divide each of its four numbers by %.3f)"),
			Length, Length));
	}

	/** A curve's keys in strictly rising time. */
	void CheckWeaponDocCurve(FWeaponDocReader& R, const FMantleWeaponDocCurve& Curve, const FString& Path)
	{
		for (int32 Index = 1; Index < Curve.Num(); ++Index)
		{
			if (Curve[Index].Time <= Curve[Index - 1].Time)
			{
				R.Add(FWeaponDocReader::At(FWeaponDocReader::AtIndex(Path, Index), TEXT("0")), TEXT("mantle"),
					FString::Printf(TEXT("is at time %s, not after the key before it (%s): put the keys in rising time, each at its own time"),
						*WeaponDocNumber(Curve[Index].Time), *WeaponDocNumber(Curve[Index - 1].Time)));
			}
		}
	}

	void CheckWeaponDocCurve3(FWeaponDocReader& R, const TMantleWeaponDocValue<FMantleWeaponDocCurve3>& Curve, const FString& Path)
	{
		if (Curve.bSet)
		{
			CheckWeaponDocCurve(R, Curve.Value.X, FWeaponDocReader::At(Path, TEXT("x")));
			CheckWeaponDocCurve(R, Curve.Value.Y, FWeaponDocReader::At(Path, TEXT("y")));
			CheckWeaponDocCurve(R, Curve.Value.Z, FWeaponDocReader::At(Path, TEXT("z")));
		}
	}

	/** Builds the document's text as a tree, then lays it out. */
	struct FWeaponDocNode
	{
		bool bObject = false;
		bool bArray = false;
		/** A scalar's JSON text. */
		FString Scalar;
		/** An object's keys, one per child. */
		TArray<FString> Keys;
		TArray<TSharedPtr<FWeaponDocNode>> Children;
	};

	using FWeaponDocNodePtr = TSharedPtr<FWeaponDocNode>;

	FWeaponDocNodePtr WeaponDocObjectNode()
	{
		FWeaponDocNodePtr Node = MakeShared<FWeaponDocNode>();
		Node->bObject = true;
		return Node;
	}

	FWeaponDocNodePtr WeaponDocArrayNode()
	{
		FWeaponDocNodePtr Node = MakeShared<FWeaponDocNode>();
		Node->bArray = true;
		return Node;
	}

	FWeaponDocNodePtr WeaponDocScalarNode(const FString& Text)
	{
		FWeaponDocNodePtr Node = MakeShared<FWeaponDocNode>();
		Node->Scalar = Text;
		return Node;
	}

	FWeaponDocNodePtr WeaponDocNumberNode(const double Value)
	{
		return WeaponDocScalarNode(WeaponDocNumber(Value));
	}

	FWeaponDocNodePtr WeaponDocBoolNode(const bool bValue)
	{
		return WeaponDocScalarNode(bValue ? TEXT("true") : TEXT("false"));
	}

	FWeaponDocNodePtr WeaponDocStringNode(const FString& Text)
	{
		return WeaponDocScalarNode(WeaponDocQuote(Text));
	}

	FWeaponDocNodePtr WeaponDocNumbersNode(std::initializer_list<double> Values)
	{
		FWeaponDocNodePtr Node = WeaponDocArrayNode();
		for (const double Value : Values)
		{
			Node->Children.Add(WeaponDocNumberNode(Value));
		}
		return Node;
	}

	void WeaponDocPut(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const FWeaponDocNodePtr& Value)
	{
		Obj->Keys.Add(Key);
		Obj->Children.Add(Value);
	}

	FString WeaponDocInline(const FWeaponDocNode& Node)
	{
		if (!Node.bObject && !Node.bArray)
		{
			return Node.Scalar;
		}
		if (Node.Children.Num() == 0)
		{
			return Node.bObject ? TEXT("{}") : TEXT("[]");
		}
		FString Out = Node.bObject ? TEXT("{ ") : TEXT("[");
		for (int32 Index = 0; Index < Node.Children.Num(); ++Index)
		{
			if (Index > 0)
			{
				Out += TEXT(", ");
			}
			if (Node.bObject)
			{
				Out += WeaponDocQuote(Node.Keys[Index]) + TEXT(": ");
			}
			Out += WeaponDocInline(*Node.Children[Index]);
		}
		Out += Node.bObject ? TEXT(" }") : TEXT("]");
		return Out;
	}

	/** Node on one line when it fits after the Used columns (and a comma), else one entry per line. */
	void WeaponDocPrint(const FWeaponDocNode& Node, const int32 Indent, const int32 Used, const bool bForceOpen, FString& Out)
	{
		const FString OneLine = WeaponDocInline(Node);
		const bool bContainer = Node.bObject || Node.bArray;
		if (!bContainer || Node.Children.Num() == 0 || (!bForceOpen && Used + OneLine.Len() + 1 <= WeaponDocLineWidth))
		{
			Out += OneLine;
			return;
		}
		const FString Pad = FString::ChrN(Indent + 2, TEXT(' '));
		Out += Node.bObject ? TEXT("{\n") : TEXT("[\n");
		for (int32 Index = 0; Index < Node.Children.Num(); ++Index)
		{
			const FString Prefix = Node.bObject ? WeaponDocQuote(Node.Keys[Index]) + TEXT(": ") : FString();
			Out += Pad;
			Out += Prefix;
			WeaponDocPrint(*Node.Children[Index], Indent + 2, Pad.Len() + Prefix.Len(), false, Out);
			if (Index + 1 < Node.Children.Num())
			{
				Out += TEXT(",");
			}
			Out += TEXT("\n");
		}
		Out += FString::ChrN(Indent, TEXT(' '));
		Out += Node.bObject ? TEXT("}") : TEXT("]");
	}

	/** The document as a tree: keys in the schema's order, only what is set. */
	class FWeaponDocWriter
	{
	public:
		static void Number(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<double>& Value)
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, WeaponDocNumberNode(Value.Value));
			}
		}

		static void Integer(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<int32>& Value)
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, WeaponDocNumberNode(static_cast<double>(Value.Value)));
			}
		}

		static void Boolean(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<bool>& Value)
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, WeaponDocBoolNode(Value.Value));
			}
		}

		static void Text(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<FString>& Value)
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, WeaponDocStringNode(Value.Value));
			}
		}

		/** A materials list (the weapon's own, an attachment's), when set. */
		static void Materials(const FWeaponDocNodePtr& Obj, const TMantleWeaponDocValue<TArray<FString>>& Value)
		{
			if (Value.bSet)
			{
				FWeaponDocNodePtr List = WeaponDocArrayNode();
				for (const FString& Name : Value.Value)
				{
					List->Children.Add(WeaponDocStringNode(Name));
				}
				WeaponDocPut(Obj, TEXT("materials"), List);
			}
		}

		static void Vector2(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<FVector2D>& Value)
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, WeaponDocNumbersNode({ Value.Value.X, Value.Value.Y }));
			}
		}

		static void Vector3(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<FVector>& Value)
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, WeaponDocNumbersNode({ Value.Value.X, Value.Value.Y, Value.Value.Z }));
			}
		}

		static void Vector4(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<FVector4>& Value)
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, WeaponDocNumbersNode({ Value.Value.X, Value.Value.Y, Value.Value.Z, Value.Value.W }));
			}
		}

		static void Rotation(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<FQuat>& Value)
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, WeaponDocNumbersNode({ Value.Value.X, Value.Value.Y, Value.Value.Z, Value.Value.W }));
			}
		}

		/** Linear keys as [t, v]; constant as [t, v, "constant"]; cubic with both tangents. */
		static FWeaponDocNodePtr Curve(const FMantleWeaponDocCurve& Keys)
		{
			FWeaponDocNodePtr Node = WeaponDocArrayNode();
			for (const FMantleWeaponDocCurveKey& Key : Keys)
			{
				FWeaponDocNodePtr KeyNode = WeaponDocNumbersNode({ Key.Time, Key.Value });
				if (Key.Interp == EMantleWeaponDocInterp::Constant)
				{
					KeyNode->Children.Add(WeaponDocStringNode(TEXT("constant")));
				}
				else if (Key.Interp == EMantleWeaponDocInterp::Cubic)
				{
					KeyNode->Children.Add(WeaponDocStringNode(TEXT("cubic")));
					KeyNode->Children.Add(WeaponDocNumberNode(Key.ArriveTangent));
					KeyNode->Children.Add(WeaponDocNumberNode(Key.LeaveTangent));
				}
				Node->Children.Add(KeyNode);
			}
			return Node;
		}

		static void OptCurve(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<FMantleWeaponDocCurve>& Value)
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, Curve(Value.Value));
			}
		}

		static void Curve3(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<FMantleWeaponDocCurve3>& Value)
		{
			if (Value.bSet)
			{
				FWeaponDocNodePtr Node = WeaponDocObjectNode();
				WeaponDocPut(Node, TEXT("x"), Curve(Value.Value.X));
				WeaponDocPut(Node, TEXT("y"), Curve(Value.Value.Y));
				WeaponDocPut(Node, TEXT("z"), Curve(Value.Value.Z));
				WeaponDocPut(Obj, Key, Node);
			}
		}

		/** A set block through Build. */
		template <typename T>
		static void Block(const FWeaponDocNodePtr& Obj, const TCHAR* Key, const TMantleWeaponDocValue<T>& Value,
			FWeaponDocNodePtr (*Build)(const T&))
		{
			if (Value.bSet)
			{
				WeaponDocPut(Obj, Key, Build(Value.Value));
			}
		}

		static FWeaponDocNodePtr Socket(const FMantleWeaponDocSocket& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("bone"), WeaponDocStringNode(In.Bone));
			Vector3(Node, TEXT("location"), In.Location);
			Rotation(Node, TEXT("rotation"), In.Rotation);
			return Node;
		}

		static FWeaponDocNodePtr Part(const FMantleWeaponDocPart& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("bone"), WeaponDocStringNode(In.Bone));
			if (In.ShownDuring.bSet)
			{
				FWeaponDocNodePtr Actions = WeaponDocArrayNode();
				for (const EMantleWeaponDocAction Action : In.ShownDuring.Value)
				{
					Actions->Children.Add(WeaponDocStringNode(WeaponDocNameAt(GWeaponDocActionNames, static_cast<int32>(Action))));
				}
				WeaponDocPut(Node, TEXT("shownDuring"), Actions);
			}
			return Node;
		}

		static FWeaponDocNodePtr Parts(const FMantleWeaponDocParts& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			for (int32 Role = 0; Role < static_cast<int32>(EMantleWeaponDocRole::Count); ++Role)
			{
				Block(Node, GWeaponDocRoleNames[Role], In.Roles[Role], &FWeaponDocWriter::Part);
			}
			return Node;
		}

		static FWeaponDocNodePtr Attachment(const FMantleWeaponDocAttachment& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("name"), WeaponDocStringNode(In.Name));
			if (!In.Mesh.IsEmpty())
			{
				WeaponDocPut(Node, TEXT("mesh"), WeaponDocStringNode(In.Mesh));
			}
			if (In.Effect.bSet)
			{
				WeaponDocPut(Node, TEXT("effect"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocEffectsNames, static_cast<int32>(In.Effect.Value))));
			}
			Text(Node, TEXT("bone"), In.Bone);
			if (In.Socket.bSet)
			{
				WeaponDocPut(Node, TEXT("socket"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocSocketNames, static_cast<int32>(In.Socket.Value))));
			}
			Vector3(Node, TEXT("location"), In.Location);
			Rotation(Node, TEXT("rotation"), In.Rotation);
			Vector3(Node, TEXT("scale"), In.Scale);
			Boolean(Node, TEXT("hidden"), In.bHidden);
			Materials(Node, In.Materials);
			return Node;
		}

		static FWeaponDocNodePtr Fire(const FMantleWeaponDocFire& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("interval"), WeaponDocNumberNode(In.Interval));
			Boolean(Node, TEXT("automatic"), In.bAutomatic);
			if (In.Modes.bSet)
			{
				FWeaponDocNodePtr Modes = WeaponDocArrayNode();
				for (const EMantleWeaponDocFireMode Mode : In.Modes.Value)
				{
					Modes->Children.Add(WeaponDocStringNode(WeaponDocNameAt(GWeaponDocFireModeNames, static_cast<int32>(Mode))));
				}
				WeaponDocPut(Node, TEXT("modes"), Modes);
			}
			Number(Node, TEXT("damage"), In.Damage);
			Number(Node, TEXT("maxRange"), In.MaxRange);
			Number(Node, TEXT("traceSweepRadius"), In.TraceSweepRadius);
			Integer(Node, TEXT("pellets"), In.Pellets);
			Number(Node, TEXT("pelletSpread"), In.PelletSpread);
			Number(Node, TEXT("plainRecoil"), In.PlainRecoil);
			return Node;
		}

		static FWeaponDocNodePtr Magazine(const FMantleWeaponDocMagazine& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("size"), WeaponDocNumberNode(static_cast<double>(In.Size)));
			Integer(Node, TEXT("startingReserve"), In.StartingReserve);
			return Node;
		}

		static FWeaponDocNodePtr Reload(const FMantleWeaponDocReload& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("style"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocReloadStyleNames, static_cast<int32>(In.Style))));
			Number(Node, TEXT("seconds"), In.Seconds);
			return Node;
		}

		static FWeaponDocNodePtr Charge(const FMantleWeaponDocCharge& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("seconds"), WeaponDocNumberNode(In.Seconds));
			Boolean(Node, TEXT("required"), In.bRequired);
			Boolean(Node, TEXT("releaseOnFull"), In.bReleaseOnFull);
			Boolean(Node, TEXT("cancelable"), In.bCancelable);
			return Node;
		}

		static FWeaponDocNodePtr Charges(const FMantleWeaponDocCharges& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("count"), WeaponDocNumberNode(static_cast<double>(In.Count)));
			WeaponDocPut(Node, TEXT("refillSeconds"), WeaponDocNumberNode(In.RefillSeconds));
			return Node;
		}

		static FWeaponDocNodePtr Projectile(const FMantleWeaponDocProjectile& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("speed"), WeaponDocNumberNode(In.Speed));
			Number(Node, TEXT("gravity"), In.Gravity);
			Number(Node, TEXT("blastRadius"), In.BlastRadius);
			Number(Node, TEXT("fuseSeconds"), In.FuseSeconds);
			Boolean(Node, TEXT("thrown"), In.bThrown);
			Boolean(Node, TEXT("stick"), In.bStick);
			Boolean(Node, TEXT("pickup"), In.bPickup);
			Number(Node, TEXT("lingerSeconds"), In.LingerSeconds);
			Number(Node, TEXT("lingerDamagePerSecond"), In.LingerDamagePerSecond);
			Number(Node, TEXT("lingerRadius"), In.LingerRadius);
			if (In.Mesh.bSet)
			{
				WeaponDocPut(Node, TEXT("mesh"), WeaponDocStringNode(In.Mesh.Value));
			}
			Materials(Node, In.Materials);
			return Node;
		}

		static FWeaponDocNodePtr SweepEdge(const FMantleWeaponDocSweepEdge& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("base"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocSocketNames, static_cast<int32>(In.Base))));
			WeaponDocPut(Node, TEXT("tip"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocSocketNames, static_cast<int32>(In.Tip))));
			return Node;
		}

		static FWeaponDocNodePtr Sweep(const FMantleWeaponDocSweep& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("reach"), WeaponDocNumberNode(In.Reach));
			Block(Node, TEXT("edge"), In.Edge, &FWeaponDocWriter::SweepEdge);
			Integer(Node, TEXT("comboCount"), In.ComboCount);
			Number(Node, TEXT("heavyMultiplier"), In.HeavyMultiplier);
			return Node;
		}

		static FWeaponDocNodePtr Beam(const FMantleWeaponDocBeam& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Number(Node, TEXT("tickRate"), In.TickRate);
			WeaponDocPut(Node, TEXT("damagePerSecond"), WeaponDocNumberNode(In.DamagePerSecond));
			Number(Node, TEXT("maxSeconds"), In.MaxSeconds);
			Number(Node, TEXT("radius"), In.Radius);
			Number(Node, TEXT("range"), In.Range);
			return Node;
		}

		static FWeaponDocNodePtr Consumable(const FMantleWeaponDocConsumable& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			WeaponDocPut(Node, TEXT("style"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocUseStyleNames, static_cast<int32>(In.Style))));
			if (In.Effect.bSet)
			{
				WeaponDocPut(Node, TEXT("effect"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocConsumableEffectNames, static_cast<int32>(In.Effect.Value))));
			}
			Number(Node, TEXT("heal"), In.Heal);
			Number(Node, TEXT("healSeconds"), In.HealSeconds);
			Integer(Node, TEXT("uses"), In.Uses);
			Number(Node, TEXT("useSeconds"), In.UseSeconds);
			Number(Node, TEXT("cooldownSeconds"), In.CooldownSeconds);
			Boolean(Node, TEXT("moveWhileUsing"), In.bMoveWhileUsing);
			return Node;
		}

		static FWeaponDocNodePtr BlockSettings(const FMantleWeaponDocBlock& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Boolean(Node, TEXT("enabled"), In.bEnabled);
			Boolean(Node, TEXT("breakOnHeavy"), In.bBreakOnHeavy);
			return Node;
		}

		static FWeaponDocNodePtr Sounds(const FMantleWeaponDocSounds& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Text(Node, TEXT("fire"), In.Fire);
			Text(Node, TEXT("fireSuppressed"), In.FireSuppressed);
			Boolean(Node, TEXT("suppressed"), In.bSuppressed);
			Text(Node, TEXT("trigger"), In.Trigger);
			return Node;
		}

		static FWeaponDocNodePtr FirstPerson(const FMantleWeaponDocFirstPerson& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Number(Node, TEXT("sightDistance"), In.SightDistance);
			Number(Node, TEXT("walkFullWeightSpeed"), In.WalkFullWeightSpeed);
			return Node;
		}

		static FWeaponDocNodePtr SpreadState(const FMantleWeaponDocSpreadState& State)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Number(Node, TEXT("multiplier"), State.Multiplier);
			Number(Node, TEXT("rate"), State.Rate);
			return Node;
		}

		static FWeaponDocNodePtr Spread(const FMantleWeaponDocSpread& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Number(Node, TEXT("exponent"), In.Exponent);
			OptCurve(Node, TEXT("heatToSpread"), In.HeatToSpread);
			OptCurve(Node, TEXT("heatToHeatPerShot"), In.HeatToHeatPerShot);
			OptCurve(Node, TEXT("heatToCooldownPerSecond"), In.HeatToCooldownPerSecond);
			Number(Node, TEXT("recoveryDelay"), In.RecoveryDelay);
			Boolean(Node, TEXT("firstShotAccuracy"), In.bFirstShotAccuracy);
			Block(Node, TEXT("aiming"), In.Aiming, &FWeaponDocWriter::SpreadState);
			Block(Node, TEXT("standingStill"), In.StandingStill, &FWeaponDocWriter::SpreadState);
			Block(Node, TEXT("crouching"), In.Crouching, &FWeaponDocWriter::SpreadState);
			Block(Node, TEXT("jumpingOrFalling"), In.JumpingOrFalling, &FWeaponDocWriter::SpreadState);
			Number(Node, TEXT("standingStillSpeed"), In.StandingStillSpeed);
			Number(Node, TEXT("standingStillToMovingRange"), In.StandingStillToMovingRange);
			OptCurve(Node, TEXT("damageFalloff"), In.DamageFalloff);
			Number(Node, TEXT("weakSpotMultiplier"), In.WeakSpotMultiplier);
			return Node;
		}

		static FWeaponDocNodePtr SwayVector(const FMantleWeaponDocSwayVector& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Vector3(Node, TEXT("stiffness"), In.Stiffness);
			Vector3(Node, TEXT("damping"), In.Damping);
			Vector3(Node, TEXT("interpSpeed"), In.InterpSpeed);
			Vector3(Node, TEXT("smoothingSpeed"), In.SmoothingSpeed);
			Vector3(Node, TEXT("clamp"), In.Clamp);
			Vector3(Node, TEXT("scale"), In.Scale);
			Number(Node, TEXT("aimScale"), In.AimScale);
			return Node;
		}

		static FWeaponDocNodePtr SwayBlock(const FMantleWeaponDocSwayBlock& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Block(Node, TEXT("position"), In.Position, &FWeaponDocWriter::SwayVector);
			Block(Node, TEXT("rotation"), In.Rotation, &FWeaponDocWriter::SwayVector);
			return Node;
		}

		static FWeaponDocNodePtr Sway(const FMantleWeaponDocSway& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Block(Node, TEXT("movement"), In.Movement, &FWeaponDocWriter::SwayBlock);
			Block(Node, TEXT("look"), In.Look, &FWeaponDocWriter::SwayBlock);
			return Node;
		}

		static FWeaponDocNodePtr RecoilKick(const FMantleWeaponDocRecoilKick& Kick)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Vector2(Node, TEXT("pitch"), Kick.Pitch);
			Vector4(Node, TEXT("yaw"), Kick.Yaw);
			Vector4(Node, TEXT("roll"), Kick.Roll);
			Vector2(Node, TEXT("back"), Kick.Back);
			Vector2(Node, TEXT("right"), Kick.Right);
			Vector2(Node, TEXT("up"), Kick.Up);
			Vector3(Node, TEXT("pivot"), Kick.Pivot);
			return Node;
		}

		static FWeaponDocNodePtr SpringAmount(const FMantleWeaponDocSpringAmount& Spring)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Number(Node, TEXT("amount"), Spring.Amount);
			Number(Node, TEXT("damping"), Spring.Damping);
			Number(Node, TEXT("interpSpeed"), Spring.InterpSpeed);
			return Node;
		}

		static FWeaponDocNodePtr RecoilCurves(const FMantleWeaponDocRecoilCurves& Curves)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Curve3(Node, TEXT("semiRotation"), Curves.SemiRotation);
			Curve3(Node, TEXT("semiLocation"), Curves.SemiLocation);
			Curve3(Node, TEXT("autoRotation"), Curves.AutoRotation);
			Curve3(Node, TEXT("autoLocation"), Curves.AutoLocation);
			return Node;
		}

		static FWeaponDocNodePtr RecoilNoise(const FMantleWeaponDocRecoilNoise& Noise)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Vector2(Node, TEXT("target"), Noise.Target);
			Vector2(Node, TEXT("interpSpeed"), Noise.InterpSpeed);
			Vector2(Node, TEXT("damping"), Noise.Damping);
			Number(Node, TEXT("aimScale"), Noise.AimScale);
			return Node;
		}

		static FWeaponDocNodePtr RecoilProgress(const FMantleWeaponDocRecoilProgress& Progress)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Block(Node, TEXT("pitch"), Progress.Pitch, &FWeaponDocWriter::SpringAmount);
			Block(Node, TEXT("up"), Progress.Up, &FWeaponDocWriter::SpringAmount);
			Number(Node, TEXT("aimScale"), Progress.AimScale);
			return Node;
		}

		static FWeaponDocNodePtr RecoilSway(const FMantleWeaponDocRecoilSway& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Vector2(Node, TEXT("pitch"), In.Pitch);
			Vector2(Node, TEXT("yaw"), In.Yaw);
			Number(Node, TEXT("rollScale"), In.RollScale);
			Number(Node, TEXT("damping"), In.Damping);
			Number(Node, TEXT("interpSpeed"), In.InterpSpeed);
			Number(Node, TEXT("aimScale"), In.AimScale);
			Vector3(Node, TEXT("pivot"), In.Pivot);
			return Node;
		}

		static FWeaponDocNodePtr ControllerRecoil(const FMantleWeaponDocControllerRecoil& Controller)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Vector2(Node, TEXT("yawStep"), Controller.YawStep);
			Vector2(Node, TEXT("pitchStep"), Controller.PitchStep);
			Vector2(Node, TEXT("interpSpeed"), Controller.InterpSpeed);
			Number(Node, TEXT("damping"), Controller.Damping);
			Number(Node, TEXT("compensation"), Controller.Compensation);
			return Node;
		}

		static FWeaponDocNodePtr CameraShake(const FMantleWeaponDocCameraShake& Shake)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Curve3(Node, TEXT("curve"), Shake.Curve);
			Vector2(Node, TEXT("pitch"), Shake.Pitch);
			Vector2(Node, TEXT("yaw"), Shake.Yaw);
			Vector2(Node, TEXT("roll"), Shake.Roll);
			Number(Node, TEXT("smoothing"), Shake.Smoothing);
			Number(Node, TEXT("playRate"), Shake.PlayRate);
			return Node;
		}

		static FWeaponDocNodePtr Recoil(const FMantleWeaponDocRecoil& In)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			Block(Node, TEXT("hip"), In.Hip, &FWeaponDocWriter::RecoilKick);
			Block(Node, TEXT("aim"), In.Aim, &FWeaponDocWriter::RecoilKick);
			Block(Node, TEXT("curves"), In.Curves, &FWeaponDocWriter::RecoilCurves);
			Vector3(Node, TEXT("smoothRotation"), In.SmoothRotation);
			Vector3(Node, TEXT("smoothLocation"), In.SmoothLocation);
			Vector3(Node, TEXT("autoRotationScale"), In.AutoRotationScale);
			Vector3(Node, TEXT("autoLocationScale"), In.AutoLocationScale);
			Boolean(Node, TEXT("alternateRoll"), In.bAlternateRoll);
			Vector3(Node, TEXT("space"), In.Space);
			Number(Node, TEXT("playRate"), In.PlayRate);
			Number(Node, TEXT("playbackOffset"), In.PlaybackOffset);
			Number(Node, TEXT("burstEndSeconds"), In.BurstEndSeconds);
			Block(Node, TEXT("noise"), In.Noise, &FWeaponDocWriter::RecoilNoise);
			Block(Node, TEXT("pushback"), In.Pushback, &FWeaponDocWriter::SpringAmount);
			Block(Node, TEXT("progress"), In.Progress, &FWeaponDocWriter::RecoilProgress);
			Block(Node, TEXT("sway"), In.Sway, &FWeaponDocWriter::RecoilSway);
			Block(Node, TEXT("controller"), In.Controller, &FWeaponDocWriter::ControllerRecoil);
			Block(Node, TEXT("cameraShake"), In.CameraShake, &FWeaponDocWriter::CameraShake);
			return Node;
		}

		static FWeaponDocNodePtr Root(const FMantleWeaponDesc& D)
		{
			FWeaponDocNodePtr Node = WeaponDocObjectNode();
			// Always version 2 (#840): hold and delivery, never the archetype alias.
			WeaponDocPut(Node, TEXT("schemaVersion"), WeaponDocNumberNode(static_cast<double>(FMantleWeaponDocument::SchemaVersion)));
			WeaponDocPut(Node, TEXT("name"), WeaponDocStringNode(D.Name));
			WeaponDocPut(Node, TEXT("hold"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocHoldNames, static_cast<int32>(D.GetHold()))));
			WeaponDocPut(Node, TEXT("delivery"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocDeliveryNames, static_cast<int32>(D.GetDelivery()))));
			Boolean(Node, TEXT("twoHanded"), D.bTwoHanded);
			if (D.Effects.bSet)
			{
				WeaponDocPut(Node, TEXT("effects"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocEffectsNames, static_cast<int32>(D.Effects.Value))));
			}
			if (D.Color.bSet)
			{
				WeaponDocPut(Node, TEXT("color"), WeaponDocNumbersNode({ static_cast<double>(D.Color.Value.R), static_cast<double>(D.Color.Value.G),
					static_cast<double>(D.Color.Value.B) }));
			}
			if (D.EffectsColor.bSet)
			{
				WeaponDocPut(Node, TEXT("effectsColor"), WeaponDocNumbersNode({ static_cast<double>(D.EffectsColor.Value.R),
					static_cast<double>(D.EffectsColor.Value.G), static_cast<double>(D.EffectsColor.Value.B) }));
			}
			if (!D.Mesh.IsEmpty())
			{
				WeaponDocPut(Node, TEXT("mesh"), WeaponDocStringNode(D.Mesh));
			}
			Materials(Node, D.Materials);
			if (!D.Icon.IsEmpty())
			{
				WeaponDocPut(Node, TEXT("icon"), WeaponDocStringNode(D.Icon));
			}
			FWeaponDocNodePtr Sockets = WeaponDocObjectNode();
			for (int32 Index = 0; Index < static_cast<int32>(EMantleWeaponDocSocket::Count); ++Index)
			{
				Block(Sockets, GWeaponDocSocketNames[Index], D.Sockets[Index], &FWeaponDocWriter::Socket);
			}
			// A fist weapon may place no points (#840: unarmed's are all optional): the sockets block only when there is one.
			if (Sockets->Children.Num() > 0)
			{
				WeaponDocPut(Node, TEXT("sockets"), Sockets);
			}
			Block(Node, TEXT("parts"), D.Parts, &FWeaponDocWriter::Parts);
			if (D.Attachments.bSet)
			{
				FWeaponDocNodePtr Attachments = WeaponDocArrayNode();
				for (const FMantleWeaponDocAttachment& Entry : D.Attachments.Value)
				{
					Attachments->Children.Add(Attachment(Entry));
				}
				WeaponDocPut(Node, TEXT("attachments"), Attachments);
			}
			// #1025: a consumable has no fire.
			if (D.GetDelivery() != EMantleWeaponDocDelivery::Use)
			{
				WeaponDocPut(Node, TEXT("fire"), Fire(D.Fire));
			}
			Block(Node, TEXT("charge"), D.Charge, &FWeaponDocWriter::Charge);
			// The magazine and the reload exactly when the resource holds rounds (#840).
			if (D.UsesRounds())
			{
				WeaponDocPut(Node, TEXT("magazine"), Magazine(D.Magazine));
				WeaponDocPut(Node, TEXT("reload"), Reload(D.Reload));
			}
			if (D.Resource.bSet)
			{
				WeaponDocPut(Node, TEXT("resource"), WeaponDocStringNode(WeaponDocNameAt(GWeaponDocResourceNames, static_cast<int32>(D.Resource.Value))));
			}
			Block(Node, TEXT("charges"), D.Charges, &FWeaponDocWriter::Charges);
			Block(Node, TEXT("projectile"), D.Projectile, &FWeaponDocWriter::Projectile);
			Block(Node, TEXT("sweep"), D.Sweep, &FWeaponDocWriter::Sweep);
			Block(Node, TEXT("beam"), D.Beam, &FWeaponDocWriter::Beam);
			Block(Node, TEXT("consumable"), D.Consumable, &FWeaponDocWriter::Consumable);
			Block(Node, TEXT("block"), D.Block, &FWeaponDocWriter::BlockSettings);
			Block(Node, TEXT("sounds"), D.Sounds, &FWeaponDocWriter::Sounds);
			Block(Node, TEXT("firstPerson"), D.FirstPerson, &FWeaponDocWriter::FirstPerson);
			Block(Node, TEXT("spread"), D.Spread, &FWeaponDocWriter::Spread);
			Block(Node, TEXT("sway"), D.Sway, &FWeaponDocWriter::Sway);
			Block(Node, TEXT("recoil"), D.Recoil, &FWeaponDocWriter::Recoil);
			return Node;
		}
	};
}

EMantleWeaponDocHold FMantleWeaponDesc::GetHold() const
{
	return Hold.bSet ? Hold.Value : FMantleWeaponDocument::HoldOfArchetype(Archetype);
}

EMantleWeaponDocDelivery FMantleWeaponDesc::GetDelivery() const
{
	return Delivery.bSet ? Delivery.Value : EMantleWeaponDocDelivery::Hitscan;
}

EMantleWeaponDocResource FMantleWeaponDesc::GetResource() const
{
	if (Resource.bSet)
	{
		return Resource.Value;
	}
	const FMantleWeaponDocHoldInfo& Info = FMantleWeaponDocument::GetHoldInfo(GetHold());
	return Info.Resources.Num() > 0 ? Info.Resources[0] : EMantleWeaponDocResource::None;
}

bool FMantleWeaponDesc::UsesRounds() const
{
	const EMantleWeaponDocResource Current = GetResource();
	return Current == EMantleWeaponDocResource::Magazine || Current == EMantleWeaponDocResource::Quiver;
}

bool FMantleWeaponDesc::IsTwoHanded() const
{
	return bTwoHanded.bSet ? bTwoHanded.Value : FMantleWeaponDocument::GetHoldInfo(GetHold()).bTwoHanded;
}

EMantleWeaponDocEffects FMantleWeaponDesc::GetEffects() const
{
	if (Effects.bSet)
	{
		return Effects.Value;
	}
	const FMantleWeaponDocHoldInfo& Info = FMantleWeaponDocument::GetHoldInfo(GetHold());
	return Info.bHasEffects ? Info.Effects : EMantleWeaponDocEffects::Rifle;
}

bool FMantleWeaponDesc::HasEffects() const
{
	return Effects.bSet ? !FMantleWeaponDocument::IsElement(Effects.Value) : FMantleWeaponDocument::GetHoldInfo(GetHold()).bHasEffects;
}

bool FMantleWeaponDesc::GetElement(EMantleWeaponDocEffects& OutElement) const
{
	if (Effects.bSet)
	{
		OutElement = Effects.Value;
		return FMantleWeaponDocument::IsElement(Effects.Value);
	}
	OutElement = EMantleWeaponDocEffects::Arcane;
	return GetHold() == EMantleWeaponDocHold::Staff;
}

const FMantleWeaponDocPart* FMantleWeaponDesc::FindPart(const EMantleWeaponDocRole Role) const
{
	if (!Parts.bSet || Role >= EMantleWeaponDocRole::Count)
	{
		return nullptr;
	}
	const TMantleWeaponDocValue<FMantleWeaponDocPart>& Part = Parts.Value[Role];
	return Part.bSet ? &Part.Value : nullptr;
}

FString FMantleWeaponDocError::ToString() const
{
	return FString::Printf(TEXT("%s %s"), Path.IsEmpty() ? TEXT("the weapon document") : *(TEXT("/") + Path), *Message);
}

bool FMantleWeaponDocument::Parse(const FString& Json, FMantleWeaponDesc& Out, TArray<FMantleWeaponDocError>& OutErrors)
{
	using namespace MantleWeaponDocumentPrivate;
	Out = FMantleWeaponDesc();
	OutErrors.Reset();
	FWeaponDocReader Reader(OutErrors);
	if (Json.Len() > MaxDocumentLength)
	{
		Reader.Add(FString(), TEXT("size"), FString::Printf(TEXT("is %d characters long: a weapon document is at most %d; write only what the weapon needs"),
			Json.Len(), MaxDocumentLength));
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(JsonReader, Root) || !Root.IsValid())
	{
		Reader.Add(FString(), TEXT("json"), FString::Printf(TEXT("is not a JSON object (%s): correct the text so that it is one { ... }"),
			*JsonReader->GetErrorMessage()));
		return false;
	}
	FMantleWeaponDesc Desc;
	Reader.ReadRoot(Root, Desc);
	// The rules across fields read a document whose every value is the schema's; before that they would only repeat its errors.
	if (OutErrors.IsEmpty())
	{
		Reader.CheckKeys(Root, Desc);
		CheckRules(Desc, OutErrors);
	}
	if (!OutErrors.IsEmpty())
	{
		return false;
	}
	Out = MoveTemp(Desc);
	return true;
}

bool FMantleWeaponDocument::Parse(const FString& Json, FMantleWeaponDesc& Out, FString& OutError)
{
	TArray<FMantleWeaponDocError> Errors;
	const bool bParsed = Parse(Json, Out, Errors);
	TArray<FString> Lines;
	for (const FMantleWeaponDocError& Error : Errors)
	{
		Lines.Add(Error.ToString());
	}
	OutError = FString::Join(Lines, TEXT("\n"));
	return bParsed;
}

void FMantleWeaponDocument::CheckRules(const FMantleWeaponDesc& D, TArray<FMantleWeaponDocError>& OutErrors)
{
	using namespace MantleWeaponDocumentPrivate;
	FWeaponDocReader R(OutErrors);
	const EMantleWeaponDocHold HoldValue = D.GetHold();
	const FMantleWeaponDocHoldInfo& Info = GetHoldInfo(HoldValue);
	const TCHAR* Hold = HoldName(HoldValue);
	const auto SocketList = [](const TArray<EMantleWeaponDocSocket>& Sockets)
	{
		TArray<FString> Names;
		for (const EMantleWeaponDocSocket Socket : Sockets)
		{
			Names.Add(SocketName(Socket));
		}
		return FString::Join(Names, TEXT(", "));
	};

	// The mesh (#840): every hold needs one. Unarmed's says so in a creator's words (#897): a fist weapon is worn on the hand.
	const bool bHasMesh = !D.Mesh.IsEmpty();
	if (Info.bMeshRequired && !bHasMesh)
	{
		R.Add(FString(), TEXT("mantle"), HoldValue == EMantleWeaponDocHold::Unarmed ? FString(FistWeaponNeedsModel)
			: FString::Printf(TEXT("needs mesh: a %s is held by its mesh; add it"), Hold));
	}

	// The points: the hold's required ones, its optional ones, no other.
	bool bAnySocket = false;
	for (int32 Index = 0; Index < static_cast<int32>(EMantleWeaponDocSocket::Count); ++Index)
	{
		bAnySocket = bAnySocket || D.Sockets[Index].bSet;
	}
	if (bHasMesh && bAnySocket)
	{
		for (const EMantleWeaponDocSocket Required : Info.Sockets)
		{
			if (!D.Socket(Required).bSet)
			{
				R.Add(TEXT("sockets"), TEXT("mantle"), FString::Printf(TEXT("has no %s: a %s needs %s; add it"), SocketName(Required), Hold,
					*SocketList(Info.Sockets)));
			}
		}
		for (int32 Index = 0; Index < static_cast<int32>(EMantleWeaponDocSocket::Count); ++Index)
		{
			const EMantleWeaponDocSocket Socket = static_cast<EMantleWeaponDocSocket>(Index);
			if (D.Sockets[Index].bSet && !Info.Sockets.Contains(Socket) && !Info.OptionalSockets.Contains(Socket))
			{
				R.Add(FString::Printf(TEXT("sockets/%s"), GWeaponDocSocketNames[Index]), TEXT("mantle"),
					FString::Printf(TEXT("is a point a %s does not have: drop it"), Hold));
			}
		}
	}
	else if (bHasMesh && Info.Sockets.Num() > 0)
	{
		R.Add(FString(), TEXT("mantle"), FString::Printf(TEXT("needs sockets: a %s needs %s; add them"), Hold, *SocketList(Info.Sockets)));
	}

	// Hands: a two-handed hold stays two-handed, and a two-handed weapon has gripL. On a one-handed weapon gripL is
	// optional where the hold allows it: present, the support hand of the two-handed hold (the left palm wraps the right);
	// absent, one hand. A one-handed hold that allows no gripL (a shield, in the left hand alone) cannot be made two-handed.
	if (Info.bTwoHanded && D.bTwoHanded.bSet && !D.bTwoHanded.Value)
	{
		R.Add(TEXT("twoHanded"), TEXT("mantle"), FString::Printf(TEXT("is false, but a %s is held in both hands (its clips put the left hand on the weapon): drop twoHanded, or pick a one-handed hold (pistol, revolver)"),
			Hold));
	}
	const bool bHoldAllowsGripL = Info.Sockets.Contains(EMantleWeaponDocSocket::GripL) || Info.OptionalSockets.Contains(EMantleWeaponDocSocket::GripL);
	const bool bOneHandOnly = !Info.bTwoHanded && !bHoldAllowsGripL && D.bTwoHanded.bSet && D.bTwoHanded.Value;
	if (bOneHandOnly)
	{
		R.Add(TEXT("twoHanded"), TEXT("mantle"), FString::Printf(TEXT("is true, but a %s is held in one hand: drop twoHanded"), Hold));
	}
	const bool bTwoHanded = D.IsTwoHanded() && !bOneHandOnly;
	const bool bGripL = D.Socket(EMantleWeaponDocSocket::GripL).bSet;
	if (bHasMesh && bTwoHanded && !bGripL && !Info.Sockets.Contains(EMantleWeaponDocSocket::GripL))
	{
		R.Add(TEXT("sockets"), TEXT("mantle"), Info.bTwoHanded
			? FString(TEXT("has no gripL, but the weapon is held in both hands: add gripL, where the left palm goes"))
			: FString(TEXT("has no gripL, but the weapon is held in both hands: add gripL, where the left palm goes, or drop twoHanded")));
	}
	for (int32 Index = 0; Index < static_cast<int32>(EMantleWeaponDocSocket::Count); ++Index)
	{
		const TMantleWeaponDocValue<FMantleWeaponDocSocket>& Socket = D.Sockets[Index];
		if (Socket.bSet && Socket.Value.Rotation.bSet)
		{
			CheckWeaponDocRotation(R, Socket.Value.Rotation.Value, FString::Printf(TEXT("sockets/%s/rotation"), GWeaponDocSocketNames[Index]));
		}
	}

	// Attachments: named once (ignoring case), unit rotations, no axis scaled to nothing.
	if (D.Attachments.bSet)
	{
		const TArray<FMantleWeaponDocAttachment>& Attachments = D.Attachments.Value;
		for (int32 Index = 0; Index < Attachments.Num(); ++Index)
		{
			const FMantleWeaponDocAttachment& Attachment = Attachments[Index];
			const FString Path = FString::Printf(TEXT("attachments/%d"), Index);
			for (int32 Earlier = 0; Earlier < Index; ++Earlier)
			{
				if (Attachments[Earlier].Name.Equals(Attachment.Name, ESearchCase::IgnoreCase))
				{
					R.Add(Path + TEXT("/name"), TEXT("mantle"), FString::Printf(TEXT("is \"%s\", which names attachment %d already (names ignore case): rename one of them"),
						*Attachment.Name, Earlier));
					break;
				}
			}
			// #904: a mesh, an element's effect or both; on a bone or one of the document's points, not both.
			if (Attachment.Mesh.IsEmpty() && !Attachment.Effect.bSet)
			{
				R.Add(Path, TEXT("anyOf"), TEXT("has neither mesh nor effect: give it one"));
			}
			if (Attachment.Effect.bSet && !IsElement(Attachment.Effect.Value))
			{
				R.Add(Path + TEXT("/effect"), TEXT("mantle"), FString::Printf(TEXT("is %s, a firearm style: an attachment's effect is an element"),
					EffectsName(Attachment.Effect.Value)));
			}
			if (Attachment.Socket.bSet)
			{
				if (Attachment.Bone.bSet)
				{
					R.Add(Path + TEXT("/socket"), TEXT("mantle"), TEXT("is set beside bone: an attachment rides a bone or a point, drop one"));
				}
				if (!D.Socket(Attachment.Socket.Value).bSet)
				{
					R.Add(Path + TEXT("/socket"), TEXT("mantle"), FString::Printf(TEXT("is %s, a point the weapon does not have: add sockets/%s or pick another"),
						SocketName(Attachment.Socket.Value), SocketName(Attachment.Socket.Value)));
				}
			}
			if (Attachment.Rotation.bSet)
			{
				CheckWeaponDocRotation(R, Attachment.Rotation.Value, Path + TEXT("/rotation"));
			}
			if (Attachment.Scale.bSet)
			{
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if (FMath::Abs(Attachment.Scale.Value[Axis]) < WeaponDocMinScale)
					{
						R.Add(Path + TEXT("/scale"), TEXT("mantle"), FString::Printf(TEXT("scales axis %d to nothing: make it at least %s away from 0 (1 leaves the mesh as it is)"),
							Axis, *WeaponDocNumber(WeaponDocMinScale)));
					}
				}
			}
		}
	}

	// Parts: only the hold's roles or its stowed roles (always with shownDuring), one role per bone (reported on the later
	// role), stowed only during its actions.
	if (D.Parts.bSet)
	{
		TArray<FString> KnownRoles;
		for (const EMantleWeaponDocRole Known : Info.Roles)
		{
			KnownRoles.Add(RoleName(Known));
		}
		for (const EMantleWeaponDocRole Known : Info.StowedRoles)
		{
			KnownRoles.Add(RoleName(Known));
		}
		TArray<FString> KnownActions;
		for (const EMantleWeaponDocAction Known : Info.Actions)
		{
			KnownActions.Add(ActionName(Known));
		}
		for (int32 Role = 0; Role < static_cast<int32>(EMantleWeaponDocRole::Count); ++Role)
		{
			const TMantleWeaponDocValue<FMantleWeaponDocPart>& Part = D.Parts.Value.Roles[Role];
			if (!Part.bSet)
			{
				continue;
			}
			const FString Path = FString::Printf(TEXT("parts/%s"), GWeaponDocRoleNames[Role]);
			const bool bStowedRole = Info.StowedRoles.Contains(static_cast<EMantleWeaponDocRole>(Role));
			if (!Info.Roles.Contains(static_cast<EMantleWeaponDocRole>(Role)) && !bStowedRole)
			{
				R.Add(Path, TEXT("mantle"), KnownRoles.Num() > 0
					? FString::Printf(TEXT("is a role a %s's clips do not move: drop it (a %s maps %s)"), Hold, Hold, *FString::Join(KnownRoles, TEXT(", ")))
					: FString::Printf(TEXT("is a role a %s's clips do not move: drop it (a %s maps no parts)"), Hold, Hold));
			}
			else if (bStowedRole && !Part.Value.ShownDuring.bSet)
			{
				R.Add(Path, TEXT("mantle"), FString::Printf(TEXT("is a %s's stowed %s, which the clips only bring to the weapon: give it shownDuring"),
					Hold, GWeaponDocRoleNames[Role]));
			}
			for (int32 Earlier = 0; Earlier < Role; ++Earlier)
			{
				const TMantleWeaponDocValue<FMantleWeaponDocPart>& Other = D.Parts.Value.Roles[Earlier];
				if (Other.bSet && Other.Value.Bone.Equals(Part.Value.Bone, ESearchCase::IgnoreCase))
				{
					R.Add(Path + TEXT("/bone"), TEXT("mantle"), FString::Printf(TEXT("is \"%s\", the %s already: a bone plays one role (bone names ignore case); give this role its own bone, or drop it"),
						*Part.Value.Bone, GWeaponDocRoleNames[Earlier]));
					break;
				}
			}
			if (Part.Value.ShownDuring.bSet)
			{
				const TArray<EMantleWeaponDocAction>& Shown = Part.Value.ShownDuring.Value;
				for (int32 At = 0; At < Shown.Num(); ++At)
				{
					if (!Info.Actions.Contains(Shown[At]))
					{
						R.Add(FString::Printf(TEXT("%s/shownDuring/%d"), *Path, At), TEXT("mantle"),
							FString::Printf(TEXT("is %s, an action a %s does not have: use one of its actions (%s)"),
								ActionName(Shown[At]), Hold, *FString::Join(KnownActions, TEXT(", "))));
					}
				}
			}
		}
	}

	// Effects (#840): a firearm style only on a hold with shot effects. #904: an element only on a projectile or beam weapon (#905: a
	// firearm's too, a laser rifle's beam or a launcher's trail; a hitscan gun keeps its firearm style); effectsColor only where the weapon
	// has an element.
	const bool bShoots = D.GetDelivery() == EMantleWeaponDocDelivery::Projectile || D.GetDelivery() == EMantleWeaponDocDelivery::Beam;
	if (D.Effects.bSet)
	{
		const TCHAR* Effects = EffectsName(D.Effects.Value);
		if (!IsElement(D.Effects.Value))
		{
			if (!Info.bHasEffects)
			{
				R.Add(TEXT("effects"), TEXT("mantle"), FString::Printf(TEXT("is set, but a %s has no shot effects: drop effects"), Hold));
			}
		}
		else if (Info.bHasEffects && !bShoots)
		{
			R.Add(TEXT("effects"), TEXT("mantle"), FString::Printf(TEXT("is %s, an element, but a %s with delivery %s takes a firearm style (rifle, pistol or shotgun); an element needs delivery projectile or beam"),
				Effects, Hold, DeliveryName(D.GetDelivery())));
		}
		else if (!bShoots)
		{
			R.Add(TEXT("effects"), TEXT("mantle"), FString::Printf(TEXT("is %s, an element, but a %s with delivery %s shoots nothing: drop effects"),
				Effects, Hold, DeliveryName(D.GetDelivery())));
		}
	}
	EMantleWeaponDocEffects Element = EMantleWeaponDocEffects::Arcane;
	if (D.EffectsColor.bSet && !D.GetElement(Element))
	{
		R.Add(TEXT("effectsColor"), TEXT("mantle"), TEXT("is set, but the weapon has no element: set effects to an element, or drop effectsColor"));
	}

	// The delivery (#840): one the hold allows; its block comes with it, and no other delivery's but a spear's throw beside
	// its sweep (or its sweep beside the throw), or a staff's cast spells beside its beam (or its beam beside the spells, #909).
	const EMantleWeaponDocDelivery Delivery = D.GetDelivery();
	const bool bDeliveryAllowed = Info.Deliveries.Contains(Delivery);
	if (!bDeliveryAllowed)
	{
		TArray<FString> Allowed;
		for (const EMantleWeaponDocDelivery Each : Info.Deliveries)
		{
			Allowed.Add(DeliveryName(Each));
		}
		R.Add(TEXT("delivery"), TEXT("mantle"), FString::Printf(TEXT("is %s, which a %s cannot do: use %s"), DeliveryName(Delivery), Hold,
			*FString::Join(Allowed, TEXT(" or "))));
	}
	const bool bThrows = Info.Actions.Contains(EMantleWeaponDocAction::Throw);
	const bool bBoth = Info.Actions.Contains(EMantleWeaponDocAction::Cast)
		&& (Delivery == EMantleWeaponDocDelivery::Beam || Delivery == EMantleWeaponDocDelivery::Projectile)
		&& Info.Deliveries.Contains(EMantleWeaponDocDelivery::Beam) && Info.Deliveries.Contains(EMantleWeaponDocDelivery::Projectile)
		&& D.Beam.bSet && D.Projectile.bSet;
	struct FWeaponDocDeliveryBlock
	{
		EMantleWeaponDocDelivery Delivery;
		bool bSet;
		const TCHAR* Key;
	};
	const FWeaponDocDeliveryBlock Blocks[] = {
		{ EMantleWeaponDocDelivery::Projectile, D.Projectile.bSet, TEXT("projectile") },
		{ EMantleWeaponDocDelivery::Sweep, D.Sweep.bSet, TEXT("sweep") },
		{ EMantleWeaponDocDelivery::Beam, D.Beam.bSet, TEXT("beam") },
		{ EMantleWeaponDocDelivery::Use, D.Consumable.bSet, TEXT("consumable") },
	};
	for (const FWeaponDocDeliveryBlock& Block : Blocks)
	{
		if (Block.Delivery == Delivery)
		{
			if (!Block.bSet && bDeliveryAllowed)
			{
				R.Add(FString(), TEXT("mantle"), FString::Printf(TEXT("needs %s: the %s delivery's numbers; add it"), Block.Key, DeliveryName(Delivery)));
			}
			continue;
		}
		if (!Block.bSet)
		{
			continue;
		}
		const bool bBlockAllowed = Info.Deliveries.Contains(Block.Delivery);
		const bool bBesideThrow = bThrows && Block.Delivery == EMantleWeaponDocDelivery::Sweep && Delivery == EMantleWeaponDocDelivery::Projectile && bBlockAllowed;
		const bool bAsThrow = bThrows && Block.Delivery == EMantleWeaponDocDelivery::Projectile && Delivery == EMantleWeaponDocDelivery::Sweep && bBlockAllowed;
		const bool bBesideBeam = bBoth && (Block.Delivery == EMantleWeaponDocDelivery::Beam || Block.Delivery == EMantleWeaponDocDelivery::Projectile);
		if (!bBesideThrow && !bAsThrow && !bBesideBeam)
		{
			R.Add(Block.Key, TEXT("mantle"), FString::Printf(TEXT("is the %s delivery's, but the weapon's delivery is %s: drop it"), DeliveryName(Block.Delivery),
				DeliveryName(Delivery)));
		}
	}
	if (D.Projectile.bSet)
	{
		const FMantleWeaponDocProjectile& Projectile = D.Projectile.Value;
		const bool bThrown = Projectile.bThrown.bSet && Projectile.bThrown.Value;
		if (bThrown && !bThrows)
		{
			R.Add(TEXT("projectile/thrown"), TEXT("mantle"), FString::Printf(TEXT("is true, but a %s is not thrown: only the holds that throw (spear, throwable) are; drop it"), Hold));
		}
		else if (bThrows && !bThrown)
		{
			R.Add(TEXT("projectile/thrown"), TEXT("mantle"), FString::Printf(TEXT("is not true, but a %s's projectile is the %s itself: write thrown true"), Hold, Hold));
		}
		if (Projectile.bPickup.bSet && Projectile.bPickup.Value && !bThrown && !Info.Resources.Contains(EMantleWeaponDocResource::Quiver))
		{
			R.Add(TEXT("projectile/pickup"), TEXT("mantle"), TEXT("is true, but only a thrown weapon or an arrow can be picked up: drop it"));
		}
		// #905: the mesh that flies; a thrown weapon flies as itself.
		if (Projectile.Mesh.bSet && bThrown)
		{
			R.Add(TEXT("projectile/mesh"), TEXT("mantle"), TEXT("is set, but a thrown weapon flies as its own mesh: drop it"));
		}
		if (Projectile.FuseSeconds.bSet && !(Projectile.BlastRadius.Value > 0.0))
		{
			R.Add(TEXT("projectile/fuseSeconds"), TEXT("mantle"), TEXT("is set, but only a projectile with a blast has a fuse: write blastRadius above 0, or drop it"));
		}
		if (!Projectile.LingerSeconds.bSet)
		{
			if (Projectile.LingerDamagePerSecond.bSet)
			{
				R.Add(TEXT("projectile/lingerDamagePerSecond"), TEXT("mantle"), TEXT("is set, but there is no lingerSeconds: the area it describes lasts that long; add it, or drop this"));
			}
			if (Projectile.LingerRadius.bSet)
			{
				R.Add(TEXT("projectile/lingerRadius"), TEXT("mantle"), TEXT("is set, but there is no lingerSeconds: the area it describes lasts that long; add it, or drop this"));
			}
		}
		else
		{
			if (!bThrown)
			{
				R.Add(TEXT("projectile/lingerSeconds"), TEXT("mantle"), TEXT("is set, but only a thrown projectile leaves a lingering area: write thrown true"));
			}
			if (Projectile.BlastRadius.Value > 0.0)
			{
				R.Add(TEXT("projectile/lingerSeconds"), TEXT("mantle"), TEXT("is set beside a blast: a thrown weapon explodes (blastRadius) or leaves an area (lingerSeconds), not both; drop one"));
			}
		}
	}
	if (D.Sweep.bSet && D.Sweep.Value.Edge.bSet)
	{
		const FMantleWeaponDocSweepEdge& Edge = D.Sweep.Value.Edge.Value;
		if (!D.Socket(Edge.Base).bSet)
		{
			R.Add(TEXT("sweep/edge/base"), TEXT("mantle"), FString::Printf(TEXT("is %s, a point the weapon does not have: add it to sockets, or name one it has"), SocketName(Edge.Base)));
		}
		if (!D.Socket(Edge.Tip).bSet)
		{
			R.Add(TEXT("sweep/edge/tip"), TEXT("mantle"), FString::Printf(TEXT("is %s, a point the weapon does not have: add it to sockets, or name one it has"), SocketName(Edge.Tip)));
		}
		if (Edge.Base == Edge.Tip)
		{
			R.Add(TEXT("sweep/edge/tip"), TEXT("mantle"), TEXT("is the base too: an edge runs between two points"));
		}
	}

	// #1025: a consumable whose effect is none does nothing itself, so it has no heal numbers.
	if (D.Consumable.bSet && D.Consumable.Value.Effect.Value == EMantleWeaponDocConsumableEffect::None)
	{
		if (D.Consumable.Value.Heal.bSet)
		{
			R.Add(TEXT("consumable/heal"), TEXT("mantle"), TEXT("is a heal's, but the effect is none: drop it, or write effect heal"));
		}
		if (D.Consumable.Value.HealSeconds.bSet)
		{
			R.Add(TEXT("consumable/healSeconds"), TEXT("mantle"), TEXT("is a heal's, but the effect is none: drop it, or write effect heal"));
		}
	}

	// The charge (#840): required (a bow's draw), optional or none (the firearms) by the hold.
	if (Info.Charge == EMantleWeaponDocChargeRule::None && D.Charge.bSet)
	{
		R.Add(TEXT("charge"), TEXT("mantle"), FString::Printf(TEXT("is set, but a %s's clips do not charge: drop charge"), Hold));
	}
	if (Info.Charge == EMantleWeaponDocChargeRule::Required)
	{
		if (!D.Charge.bSet)
		{
			R.Add(FString(), TEXT("mantle"), FString::Printf(TEXT("needs charge: a %s attacks only charged (its draw); add it with required true"), Hold));
		}
		else if (!(D.Charge.Value.bRequired.bSet && D.Charge.Value.bRequired.Value))
		{
			R.Add(TEXT("charge/required"), TEXT("mantle"), FString::Printf(TEXT("is not true, but a %s attacks only charged: write required true"), Hold));
		}
	}
	// Blocking (#840): none (drop block), optional, or required (a shield: block present and enabled).
	if (D.Block.bSet && Info.BlockRule == EMantleWeaponDocBlockRule::None)
	{
		R.Add(TEXT("block"), TEXT("mantle"), FString::Printf(TEXT("is set, but a %s does not block: drop block"), Hold));
	}
	if (Info.BlockRule == EMantleWeaponDocBlockRule::Required)
	{
		if (!D.Block.bSet)
		{
			R.Add(FString(), TEXT("mantle"), FString::Printf(TEXT("needs block: a %s blocks; add block"), Hold));
		}
		else if (D.Block.Value.bEnabled.bSet && !D.Block.Value.bEnabled.Value)
		{
			R.Add(TEXT("block/enabled"), TEXT("mantle"), FString::Printf(TEXT("is false, but a %s always blocks: write enabled true, or drop it"), Hold));
		}
	}

	// The resource (#840): one the hold allows; charges come with their numbers (the magazine and reload are CheckKeys').
	const EMantleWeaponDocResource Resource = D.GetResource();
	if (!Info.Resources.Contains(Resource))
	{
		TArray<FString> Allowed;
		for (const EMantleWeaponDocResource Each : Info.Resources)
		{
			Allowed.Add(ResourceName(Each));
		}
		R.Add(TEXT("resource"), TEXT("mantle"), FString::Printf(TEXT("is %s, which a %s does not use: use %s"), ResourceName(Resource), Hold,
			*FString::Join(Allowed, TEXT(" or "))));
	}
	if (Resource == EMantleWeaponDocResource::Charges && !D.Charges.bSet)
	{
		R.Add(FString(), TEXT("mantle"), TEXT("needs charges: how many, and how fast they refill"));
	}
	if (Resource != EMantleWeaponDocResource::Charges && D.Charges.bSet)
	{
		R.Add(TEXT("charges"), TEXT("mantle"), FString::Printf(TEXT("is for the charges resource, but the resource is %s: drop it"), ResourceName(Resource)));
	}

	// The reload style is the hold's: its clips reload that way.
	if (D.UsesRounds() && Info.bHasReloadStyle && D.Reload.Style != Info.ReloadStyle)
	{
		R.Add(TEXT("reload/style"), TEXT("mantle"), FString::Printf(TEXT("is %s, but a %s's clips reload %s: write %s"),
			ReloadStyleName(D.Reload.Style), Hold, ReloadStyleName(Info.ReloadStyle), ReloadStyleName(Info.ReloadStyle)));
	}

	// Every curve's keys in rising time.
	if (D.Spread.bSet)
	{
		const FMantleWeaponDocSpread& Spread = D.Spread.Value;
		struct FWeaponDocNamedCurve
		{
			const TMantleWeaponDocValue<FMantleWeaponDocCurve>* Curve;
			const TCHAR* Path;
		};
		const FWeaponDocNamedCurve SpreadCurves[] = {
			{ &Spread.HeatToSpread, TEXT("spread/heatToSpread") },
			{ &Spread.HeatToHeatPerShot, TEXT("spread/heatToHeatPerShot") },
			{ &Spread.HeatToCooldownPerSecond, TEXT("spread/heatToCooldownPerSecond") },
			{ &Spread.DamageFalloff, TEXT("spread/damageFalloff") },
		};
		for (const FWeaponDocNamedCurve& Entry : SpreadCurves)
		{
			if (Entry.Curve->bSet)
			{
				CheckWeaponDocCurve(R, Entry.Curve->Value, Entry.Path);
			}
		}
	}
	if (D.Recoil.bSet)
	{
		const FMantleWeaponDocRecoil& Recoil = D.Recoil.Value;
		if (Recoil.Curves.bSet)
		{
			CheckWeaponDocCurve3(R, Recoil.Curves.Value.SemiRotation, TEXT("recoil/curves/semiRotation"));
			CheckWeaponDocCurve3(R, Recoil.Curves.Value.SemiLocation, TEXT("recoil/curves/semiLocation"));
			CheckWeaponDocCurve3(R, Recoil.Curves.Value.AutoRotation, TEXT("recoil/curves/autoRotation"));
			CheckWeaponDocCurve3(R, Recoil.Curves.Value.AutoLocation, TEXT("recoil/curves/autoLocation"));
		}
		if (Recoil.CameraShake.bSet)
		{
			CheckWeaponDocCurve3(R, Recoil.CameraShake.Value.Curve, TEXT("recoil/cameraShake/curve"));
		}
	}
}

bool FMantleWeaponDocument::Validate(const FMantleWeaponDesc& Desc, TArray<FMantleWeaponDocError>& OutErrors)
{
	FMantleWeaponDesc Reread;
	return Parse(Write(Desc), Reread, OutErrors);
}

FString FMantleWeaponDocument::Write(const FMantleWeaponDesc& Desc)
{
	using namespace MantleWeaponDocumentPrivate;
	const FWeaponDocNodePtr Root = FWeaponDocWriter::Root(Desc);
	FString Out;
	WeaponDocPrint(*Root, 0, 0, true, Out);
	Out += TEXT("\n");
	return Out;
}

const FMantleWeaponDocHoldInfo& FMantleWeaponDocument::GetHoldInfo(const EMantleWeaponDocHold Hold)
{
	static const TArray<FMantleWeaponDocHoldInfo> Table = MantleWeaponDocumentPrivate::BuildWeaponDocHolds();
	return Table[FMath::Clamp(static_cast<int32>(Hold), 0, Table.Num() - 1)];
}

const FMantleWeaponDocArchetypeInfo& FMantleWeaponDocument::GetArchetypeInfo(const EMantleWeaponDocArchetype Archetype)
{
	return GetHoldInfo(HoldOfArchetype(Archetype));
}

bool FMantleWeaponDocument::IsFirearmHold(const EMantleWeaponDocHold Hold)
{
	return static_cast<int32>(Hold) < static_cast<int32>(EMantleWeaponDocArchetype::Count);
}

EMantleWeaponDocHold FMantleWeaponDocument::HoldOfArchetype(const EMantleWeaponDocArchetype Archetype)
{
	// The archetypes are the first holds, in order (static_assert by the name tables).
	return static_cast<EMantleWeaponDocHold>(FMath::Clamp(static_cast<int32>(Archetype), 0, static_cast<int32>(EMantleWeaponDocArchetype::Count) - 1));
}

bool FMantleWeaponDocument::ArchetypeOfHold(const EMantleWeaponDocHold Hold, EMantleWeaponDocArchetype& OutArchetype)
{
	if (!IsFirearmHold(Hold))
	{
		return false;
	}
	OutArchetype = static_cast<EMantleWeaponDocArchetype>(static_cast<uint8>(Hold));
	return true;
}

const TCHAR* FMantleWeaponDocument::ArchetypeName(const EMantleWeaponDocArchetype Archetype)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocArchetypeNames, static_cast<int32>(Archetype));
}

const TCHAR* FMantleWeaponDocument::HoldName(const EMantleWeaponDocHold Hold)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocHoldNames, static_cast<int32>(Hold));
}

const TCHAR* FMantleWeaponDocument::DeliveryName(const EMantleWeaponDocDelivery Delivery)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocDeliveryNames, static_cast<int32>(Delivery));
}

const TCHAR* FMantleWeaponDocument::ResourceName(const EMantleWeaponDocResource Resource)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocResourceNames, static_cast<int32>(Resource));
}

const TCHAR* FMantleWeaponDocument::RoleName(const EMantleWeaponDocRole Role)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocRoleNames, static_cast<int32>(Role));
}

const TCHAR* FMantleWeaponDocument::ActionName(const EMantleWeaponDocAction Action)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocActionNames, static_cast<int32>(Action));
}

const TCHAR* FMantleWeaponDocument::SocketName(const EMantleWeaponDocSocket Socket)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocSocketNames, static_cast<int32>(Socket));
}

const TCHAR* FMantleWeaponDocument::ReloadStyleName(const EMantleWeaponDocReloadStyle Style)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocReloadStyleNames, static_cast<int32>(Style));
}

const TCHAR* FMantleWeaponDocument::UseStyleName(const EMantleWeaponDocUseStyle Style)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocUseStyleNames, static_cast<int32>(Style));
}

const TCHAR* FMantleWeaponDocument::ConsumableEffectName(const EMantleWeaponDocConsumableEffect Effect)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocConsumableEffectNames, static_cast<int32>(Effect));
}

TConstArrayView<const TCHAR*> FMantleWeaponDocument::GetUseRefusedFields()
{
	return MantleWeaponDocumentPrivate::GWeaponDocUseRefused;
}

const TCHAR* FMantleWeaponDocument::EffectsName(const EMantleWeaponDocEffects Effects)
{
	return MantleWeaponDocumentPrivate::WeaponDocNameAt(MantleWeaponDocumentPrivate::GWeaponDocEffectsNames, static_cast<int32>(Effects));
}
