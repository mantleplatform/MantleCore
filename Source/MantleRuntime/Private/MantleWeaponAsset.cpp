// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponAsset.h"

#include "Containers/StringConv.h"
#include "MantleWeaponDocument.h"

namespace MantleWeaponAssetPrivate
{
	void AddOnce(TArray<FString>& Into, const FString& Id)
	{
		if (!Id.IsEmpty())
		{
			Into.AddUnique(Id);
		}
	}
}

TArray<FString> FMantleWeaponAsset::NamedIds() const
{
	TArray<FString> Ids;
	MantleWeaponAssetPrivate::AddOnce(Ids, Mesh);
	MantleWeaponAssetPrivate::AddOnce(Ids, Icon);
	for (const TArray<FString>* List : { &Attachments, &Sounds, &Materials })
	{
		for (const FString& Id : *List)
		{
			MantleWeaponAssetPrivate::AddOnce(Ids, Id);
		}
	}
	for (const TPair<FString, TArray<FString>>& Attachment : AttachmentMaterials)
	{
		for (const FString& Id : Attachment.Value)
		{
			MantleWeaponAssetPrivate::AddOnce(Ids, Id);
		}
	}
	return Ids;
}

bool FMantleWeaponAsset::Read(const TConstArrayView<uint8> Bytes, FMantleWeaponAsset& Out, FString& OutError)
{
	Out = FMantleWeaponAsset();
	if (Bytes.IsEmpty() || Bytes.Num() > MaxBytes)
	{
		OutError = FString::Printf(TEXT("is %d bytes; a weapon document is 1 to %d"), Bytes.Num(), MaxBytes);
		return false;
	}
	int32 Start = 0;
	if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
	{
		Start = 3;
	}
	const auto Converted = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData() + Start), Bytes.Num() - Start);
	const FString Text = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());

	FMantleWeaponDesc Desc;
	if (!FMantleWeaponDocument::Parse(Text, Desc, OutError))
	{
		return false;
	}

	Out.Text = Text;
	Out.Name = Desc.Name;
	Out.Mesh = Desc.Mesh;
	Out.Icon = Desc.Icon;
	if (Desc.Attachments.bSet)
	{
		for (const FMantleWeaponDocAttachment& Attachment : Desc.Attachments.Value)
		{
			MantleWeaponAssetPrivate::AddOnce(Out.Attachments, Attachment.Mesh);
			if (Attachment.Materials.bSet && !Attachment.Mesh.IsEmpty() && !Out.AttachmentMaterials.Contains(Attachment.Mesh))
			{
				Out.AttachmentMaterials.Add(Attachment.Mesh, Attachment.Materials.Value);
			}
		}
	}
	// #905: the mesh a launcher's round or an arrow flies as is fetched and dressed as an attachment's mesh is.
	if (Desc.Projectile.bSet && Desc.Projectile.Value.Mesh.bSet)
	{
		const FMantleWeaponDocProjectile& Projectile = Desc.Projectile.Value;
		MantleWeaponAssetPrivate::AddOnce(Out.Attachments, Projectile.Mesh.Value);
		if (Projectile.Materials.bSet && !Out.AttachmentMaterials.Contains(Projectile.Mesh.Value))
		{
			Out.AttachmentMaterials.Add(Projectile.Mesh.Value, Projectile.Materials.Value);
		}
	}
	if (Desc.Sounds.bSet)
	{
		if (Desc.Sounds.Value.Fire.bSet)
		{
			MantleWeaponAssetPrivate::AddOnce(Out.Sounds, Desc.Sounds.Value.Fire.Value);
		}
		if (Desc.Sounds.Value.FireSuppressed.bSet)
		{
			MantleWeaponAssetPrivate::AddOnce(Out.Sounds, Desc.Sounds.Value.FireSuppressed.Value);
		}
	}
	if (Desc.Materials.bSet)
	{
		Out.Materials = Desc.Materials.Value;
	}
	return true;
}
