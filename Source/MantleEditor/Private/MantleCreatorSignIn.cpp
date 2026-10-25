// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCreatorSignIn.h"
#include "MantleApiClient.h"
#include "MantleConnection.h"
#include "MantleRuntime.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Misc/App.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleCreatorSignIn"

namespace
{
	// Every name in here carries a CreatorSignIn prefix: a Unity build merges this anonymous
	// namespace with every other one in the same translation unit.

	/** Who the pasted token belongs to, from GET /me. In memory only, like the token. */
	FString CreatorSignInDisplayName;
	FDelegateHandle CreatorSignInRejectedHandle;

	/** Seconds the dialog waits on GET /me: shorter than a publish's budget, since the creator is watching it. */
	constexpr float CreatorSignInCheckSeconds = 15.0f;

	/**
	 * The account GET /me named, and for which token at which API (#394): a level's place is
	 * remembered per account and API, so a different token or address must ask again.
	 */
	FString CreatorSignInAccountId;
	FString CreatorSignInAccountFor;

	FString CreatorSignInAccountKey()
	{
		return FMantleConnection::ResolveAuthToken() + TEXT("|") + FMantleApiClient::BaseUrlResolver();
	}

	void CreatorSignInRememberAccount(const FMantleApiResult& Me)
	{
		const FString AccountId = FMantleCreatorSignIn::ParseAccountId(Me);
		if (!AccountId.IsEmpty())
		{
			CreatorSignInAccountId = AccountId;
			CreatorSignInAccountFor = CreatorSignInAccountKey();
		}
	}
}

FString FMantleCreatorSignIn::ParseAccountId(const FMantleApiResult& Me)
{
	if (!Me.bSuccess)
	{
		return FString();
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Me.Body);
	FString Id;
	if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
	{
		Object->TryGetStringField(TEXT("id"), Id);
	}
	return Id.TrimStartAndEnd();
}

bool FMantleCreatorSignIn::ResolveAccountId(FString& OutAccountId, FString& OutWhyNot)
{
	OutAccountId.Reset();
	if (!IsSignedIn())
	{
		OutWhyNot = TEXT("Not signed in to Mantle.");
		return false;
	}
	if (!CreatorSignInAccountId.IsEmpty() && CreatorSignInAccountFor == CreatorSignInAccountKey())
	{
		OutAccountId = CreatorSignInAccountId;
		return true;
	}

	// MANTLE_AUTH_TOKEN, or a different API since sign-in: ask who this token is.
	const FMantleApiResult Me = FMantleApiClient::Send(TEXT("GET"), TEXT("/me"), FString(), CreatorSignInCheckSeconds);
	CreatorSignInRememberAccount(Me);
	if (!CreatorSignInAccountId.IsEmpty() && CreatorSignInAccountFor == CreatorSignInAccountKey())
	{
		if (CreatorSignInDisplayName.IsEmpty())
		{
			CreatorSignInDisplayName = FMantleApiClient::ParseVerifiedDisplayName(Me);
		}
		OutAccountId = CreatorSignInAccountId;
		return true;
	}
	OutWhyNot = Me.bSuccess
		? FString::Printf(TEXT("Mantle at %s did not say which account is signed in."), *FMantleApiClient::BaseUrlResolver())
		: FString::Printf(TEXT("Couldn't check which Mantle account is signed in at %s: %s"), *FMantleApiClient::BaseUrlResolver(), *Me.Message);
	return false;
}

void FMantleCreatorSignIn::Startup()
{
	CreatorSignInRejectedHandle = FMantleConnection::OnSessionRejected().AddStatic(&FMantleCreatorSignIn::HandleSessionRejected);
}

void FMantleCreatorSignIn::Shutdown()
{
	FMantleConnection::OnSessionRejected().Remove(CreatorSignInRejectedHandle);
	CreatorSignInRejectedHandle.Reset();
	SignOut();
}

bool FMantleCreatorSignIn::IsSignedIn()
{
	return !FMantleConnection::ResolveAuthToken().IsEmpty();
}

bool FMantleCreatorSignIn::IsUsingEnvironmentToken()
{
	return FMantleConnection::IsAuthTokenFromEnvironment();
}

FText FMantleCreatorSignIn::DescribeSignedIn()
{
	if (IsUsingEnvironmentToken())
	{
		return CreatorSignInDisplayName.IsEmpty()
			? LOCTEXT("SignedInEnvironment", "Signed in with MANTLE_AUTH_TOKEN")
			: FText::Format(LOCTEXT("SignedInEnvironmentAs", "Signed in as {0} (MANTLE_AUTH_TOKEN)"), FText::FromString(CreatorSignInDisplayName));
	}
	if (!IsSignedIn())
	{
		return LOCTEXT("SignedOut", "Not signed in to Mantle");
	}
	return CreatorSignInDisplayName.IsEmpty()
		? LOCTEXT("SignedInUnnamed", "Signed in to Mantle")
		: FText::Format(LOCTEXT("SignedInAs", "Signed in as {0}"), FText::FromString(CreatorSignInDisplayName));
}

bool FMantleCreatorSignIn::EnsureSignedIn(const FText& Reason)
{
	if (IsSignedIn())
	{
		return true;
	}
	// A dialog nobody can answer would hang an automated run; the action reports why it stopped.
	if (FApp::IsUnattended() || IsRunningCommandlet() || !GEditor)
	{
		return false;
	}
	return OpenSignInDialog(Reason);
}

void FMantleCreatorSignIn::SignOut()
{
	FMantleConnection::ClearRuntimeAuthToken();
	CreatorSignInDisplayName.Reset();
	CreatorSignInAccountId.Reset();
	CreatorSignInAccountFor.Reset();
}

bool FMantleCreatorSignIn::CheckTokenShape(const FString& Pasted, FString& OutToken, FString& OutWhyNot)
{
	FString Token = Pasted.TrimStartAndEnd();
	if (Token.StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase))
	{
		Token.RightChopInline(7);
		Token.TrimStartInline();
	}
	if (Token.IsEmpty())
	{
		OutWhyNot = TEXT("Paste a creator token first.");
		return false;
	}
	for (const TCHAR Character : Token)
	{
		if (Character <= TEXT(' ') || Character > TEXT('~'))
		{
			OutWhyNot = TEXT("A creator token is one line of letters, digits, dots and underscores. Copy it again from Mantle's Settings > Account.");
			return false;
		}
	}
	if (!Token.StartsWith(TEXT("mct1."), ESearchCase::CaseSensitive))
	{
		OutWhyNot = TEXT("That isn't a creator token: they start with mct1. Create one in the Mantle game under Settings > Account > Creator tokens.");
		return false;
	}
	TArray<FString> Parts;
	Token.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty*/ false);
	if (Parts.Num() != 3 || !Parts[1].StartsWith(TEXT("ct_"), ESearchCase::CaseSensitive) || Parts[1].Len() <= 3 || Parts[2].IsEmpty())
	{
		OutWhyNot = TEXT("That creator token is incomplete. Copy the whole token again from Mantle's Settings > Account.");
		return false;
	}
	OutToken = MoveTemp(Token);
	return true;
}

bool FMantleCreatorSignIn::SignInWithToken(const FString& Token, FString& OutWhyNot)
{
	FMantleConnection::SetRuntimeAuthToken(Token);
	CreatorSignInDisplayName.Reset();

	// GET /me is on a creator token's allowed list: it proves the token and names its player.
	const FMantleApiResult Me = FMantleApiClient::Send(TEXT("GET"), TEXT("/me"), FString(), CreatorSignInCheckSeconds);
	if (Me.bSuccess)
	{
		CreatorSignInDisplayName = FMantleApiClient::ParseVerifiedDisplayName(Me);
		CreatorSignInRememberAccount(Me);
		UE_LOG(LogMantle, Log, TEXT("Signed in to Mantle with a creator token as '%s'"), *CreatorSignInDisplayName);
		return true;
	}

	SignOut();
	if (Me.StatusCode == 401)
	{
		OutWhyNot = Me.Code == TEXT("expired_token")
			? TEXT("That creator token has expired. Create a new one in Mantle's Settings > Account.")
			: Me.Code == TEXT("account_deleted")
				? TEXT("That creator token belongs to a Mantle account that has been deleted.")
				: TEXT("Mantle didn't accept that creator token: it may have been revoked. Create a new one in Mantle's Settings > Account.");
	}
	else
	{
		OutWhyNot = FString::Printf(TEXT("Couldn't check the token with Mantle at %s: %s"),
			*FMantleApiClient::BaseUrlResolver(), *Me.Message);
	}
	return false;
}

void FMantleCreatorSignIn::HandleSessionRejected(const FString& Message, const FString& RejectedToken)
{
	// MANTLE_AUTH_TOKEN cannot be forgotten from here, and takes precedence anyway.
	if (IsUsingEnvironmentToken())
	{
		return;
	}
	// A reply still in flight from a token already replaced says nothing about the current one.
	if (RejectedToken.IsEmpty() || RejectedToken != FMantleConnection::ResolveAuthToken())
	{
		return;
	}
	UE_LOG(LogMantle, Warning, TEXT("Mantle refused the pasted creator token (%s). The next action will ask for a new one."), *Message);
	SignOut();
}

bool FMantleCreatorSignIn::OpenSignInDialog(const FText& Reason)
{
	if (IsUsingEnvironmentToken())
	{
		return true;
	}
	if (!GEditor)
	{
		return false;
	}

	bool bSignedIn = false;
	TSharedPtr<SEditableTextBox> TokenBox;
	TSharedPtr<STextBlock> ErrorText;

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Sign in to Mantle"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false);
	const TWeakPtr<SWindow> WeakWindow = Window;

	// Captured by reference: the window is modal, so this function does not return until it has
	// closed and nothing can call these afterwards.
	auto TrySignIn = [&bSignedIn, &TokenBox, &ErrorText, WeakWindow]()
	{
		if (!TokenBox.IsValid() || !ErrorText.IsValid())
		{
			return;
		}
		FString Token;
		FString WhyNot;
		if (!CheckTokenShape(TokenBox->GetText().ToString(), Token, WhyNot) || !SignInWithToken(Token, WhyNot))
		{
			ErrorText->SetText(FText::FromString(WhyNot));
			ErrorText->SetVisibility(EVisibility::Visible);
			return;
		}
		bSignedIn = true;
		if (const TSharedPtr<SWindow> Open = WeakWindow.Pin())
		{
			Open->RequestDestroyWindow();
		}
	};

	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	if (!Reason.IsEmpty())
	{
		Column->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock).AutoWrapText(true).Text(Reason)
		];
	}
	Column->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
	[
		SNew(STextBlock).AutoWrapText(true)
		.Text(LOCTEXT("Explain",
			"Paste a creator token. Make one in the Mantle game, signed in as the player whose places these are: "
			"Settings > Account > Creator tokens.\n\n"
			"The editor keeps the token in memory until it closes. It is never saved to disk."))
	];
	Column->AddSlot().AutoHeight()
	[
		SAssignNew(TokenBox, SEditableTextBox)
		.IsPassword(true)
		.HintText(LOCTEXT("Hint", "mct1.ct_…"))
		.OnTextCommitted_Lambda([TrySignIn](const FText&, const ETextCommit::Type CommitType)
		{
			if (CommitType == ETextCommit::OnEnter)
			{
				TrySignIn();
			}
		})
	];
	Column->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
	[
		SAssignNew(ErrorText, STextBlock)
		.AutoWrapText(true)
		.ColorAndOpacity(FStyleColors::Error)
		.Visibility(EVisibility::Collapsed)
	];
	Column->AddSlot().AutoHeight().HAlign(HAlign_Right).Padding(0.0f, 12.0f, 0.0f, 0.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("SignIn", "Sign in"))
			.OnClicked_Lambda([TrySignIn]()
			{
				TrySignIn();
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("Cancel", "Cancel"))
			.OnClicked_Lambda([WeakWindow]()
			{
				if (const TSharedPtr<SWindow> Open = WeakWindow.Pin())
				{
					Open->RequestDestroyWindow();
				}
				return FReply::Handled();
			})
		]
	];

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SBox).WidthOverride(520.0f)
			[
				Column
			]
		]);
	Window->SetWidgetToFocusOnActivate(TokenBox);

	GEditor->EditorAddModalWindow(Window);
	return bSignedIn;
}

#undef LOCTEXT_NAMESPACE
