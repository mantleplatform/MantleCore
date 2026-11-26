// Copyright Joshua Eger. All Rights Reserved.
// The extra seats of the Build a vehicle window's seats and paint step (SMantleVehicleBuilder, #449): the seats after the driver's and the passenger's, a row each
// (sit or stand, the door it is got into through, hidden, its firearm aim arc), added one at a time or a row across at once, copied and deleted. Where each is and
// the way it faces are dragged in the picture. Where they are, sit or stand and the doors are the frame's (FMantleVehicleShapeSession) and go into the build with it;
// the hidden ticks and the arcs go into the built vehicle (ApplyChoices).
#include "SMantleVehicleBuilder.h"

#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleShapeKind.h"
#include "SMantleBuilderViewport.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleVehicleBuilder"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBuilderSeatsPrivate
{
	/** How far behind the seat before a new seat or row goes, cm: about a car's row of seats. */
	constexpr double RowStep = 80.0;
	/** The vehicle document's room for a location, cm (extraSeats location in limits.json). */
	constexpr double MaxReach = 1000.0;
}

void SMantleVehicleBuilder::RebuildSeatRows()
{
	if (!SeatRowsBox.IsValid())
	{
		return;
	}
	SeatRowsBox->ClearChildren();
	SeatChoices.SetNum(Shape.GetExtraSeats().Num());
	if (bHaveModelInfo && !bUseSkin)
	{
		// A model with the type's own wheel bones is fitted on them, from the type's seats: they are shown in the picture, and moved later in the settings.
		SeatRowsBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.Text(FText::Format(LOCTEXT("SeatsOwnWheels", "Your model is fitted on its own wheels, so it gets the {0}'s {1} more {1}|plural(one=seat,other=seats). Change them after the build in the vehicle's settings (Show its settings, Extra Seats)."),
					SelectedPresetName, FText::AsNumber(Shape.GetExtraSeats().Num())))
		];
		return;
	}

	SeatRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
		[
			SNew(SButton)
				.Text(LOCTEXT("AddSeat", "Add a seat"))
				.ToolTipText(LOCTEXT("AddSeatTip", "Adds a seat a row behind the last one, by the same door. Drag it into place in the picture."))
				.IsEnabled_Lambda([this] { return Shape.GetExtraSeats().Num() < FMantleVehicleLimits::MaxExtraSeats; })
				.OnClicked(this, &SMantleVehicleBuilder::OnAddSeat)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
		[
			SNew(SButton)
				.Text(LOCTEXT("AddSeatRow", "Add a row of"))
				.ToolTipText(LOCTEXT("AddSeatRowTip", "Adds a row of seats side by side, across the vehicle's middle, a row behind the last seat: a bench, or a bus's row."))
				.IsEnabled_Lambda([this] { return Shape.GetExtraSeats().Num() < FMantleVehicleLimits::MaxExtraSeats; })
				.OnClicked(this, &SMantleVehicleBuilder::OnAddSeatRow)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(48.f)
			[
				SNew(SSpinBox<int32>)
					.MinValue(1)
					.MaxValue(8)
					.Value_Lambda([this] { return SeatRowCount; })
					.OnValueChanged_Lambda([this](int32 Value) { SeatRowCount = FMath::Clamp(Value, 1, 8); })
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("SeatRowSeats", "seats,"))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(56.f)
			[
				SNew(SSpinBox<float>)
					.MinValue(30.f)
					.MaxValue(150.f)
					.Delta(1.f)
					.Value_Lambda([this] { return SeatRowSpacing; })
					.OnValueChanged_Lambda([this](float Value) { SeatRowSpacing = FMath::Clamp(Value, 30.f, 150.f); })
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("SeatRowApart", "cm apart"))
		]
	];
	SeatRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([this]
			{
				return FText::Format(LOCTEXT("SeatCount", "{0} more {0}|plural(one=seat,other=seats); a vehicle has at most {1}."),
					FText::AsNumber(Shape.GetExtraSeats().Num()), FText::AsNumber(FMantleVehicleLimits::MaxExtraSeats));
			})
	];

	// One arc number: the seat's own while its arc is on.
	const auto ArcBox = [this](const int32 Index, float FMantleVehicleSourceWeaponArc::* Field, const float Min, const float Max, const FText& Tip)
	{
		return SNew(SSpinBox<float>)
			.MinValue(Min)
			.MaxValue(Max)
			.Delta(1.f)
			.ToolTipText(Tip)
			.IsEnabled_Lambda([this, Index] { return SeatChoices.IsValidIndex(Index) && SeatChoices[Index].Arc.bEnabled; })
			.Value_Lambda([this, Index, Field] { return SeatChoices.IsValidIndex(Index) ? SeatChoices[Index].Arc.*Field : 0.f; })
			.OnValueChanged_Lambda([this, Index, Field](float Value)
			{
				if (SeatChoices.IsValidIndex(Index))
				{
					SeatChoices[Index].Arc.*Field = Value;
				}
			})
			.OnEndSliderMovement_Lambda([this](float) { SeatChoiceChanged(); })
			.OnValueCommitted_Lambda([this, Index, Field](float Value, ETextCommit::Type)
			{
				if (SeatChoices.IsValidIndex(Index))
				{
					SeatChoices[Index].Arc.*Field = Value;
					SeatChoiceChanged();
				}
			});
	};

	for (int32 Index = 0; Index < Shape.GetExtraSeats().Num(); ++Index)
	{
		SeatRowsBox->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(SBox).WidthOverride(84.f)
					[
						SNew(STextBlock).Text(FText::Format(LOCTEXT("SeatRowLabel", "Extra seat {0}"), FText::AsNumber(Index + 1)))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SBox).WidthOverride(80.f)
					[
						SNew(SComboButton)
							.ToolTipText(LOCTEXT("SeatPoseTip", "Sits: a seat. Stands: a place to stand, on the floor where the box is, such as a bus's standing room."))
							.ButtonContent()
							[
								SNew(STextBlock).Text_Lambda([this, Index]
								{
									const bool bStand = Shape.GetExtraSeats().IsValidIndex(Index) && Shape.GetExtraSeats()[Index].Pose == TEXT("stand");
									return bStand ? LOCTEXT("SeatStands", "Stands") : LOCTEXT("SeatSits", "Sits");
								})
							]
							.OnGetMenuContent_Lambda([this, Index]
							{
								FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ true, nullptr);
								for (const bool bStand : { false, true })
								{
									Menu.AddMenuEntry(bStand ? LOCTEXT("SeatStands", "Stands") : LOCTEXT("SeatSits", "Sits"), FText::GetEmpty(), FSlateIcon(),
										FUIAction(FExecuteAction::CreateLambda([this, Index, bStand] { SetSeatStand(Index, bStand); }), FCanExecuteAction(),
											FIsActionChecked::CreateLambda([this, Index, bStand]
											{
												return Shape.GetExtraSeats().IsValidIndex(Index) && (Shape.GetExtraSeats()[Index].Pose == TEXT("stand")) == bStand;
											})),
										NAME_None, EUserInterfaceActionType::RadioButton);
								}
								return Menu.MakeWidget();
							})
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 6, 0)
				[
					SNew(SComboButton)
						.ToolTipText(LOCTEXT("SeatDoorTip", "The door people open to get into this seat. The list has the doors your model opens (the Doors step). No door: an open seat, got into from beside it."))
						.ButtonContent()
						[
							SNew(STextBlock).Text_Lambda([this, Index]
							{
								return Shape.GetExtraSeats().IsValidIndex(Index) ? SeatDoorLabel(Shape.GetExtraSeats()[Index].Door) : FText::GetEmpty();
							})
						]
						.OnGetMenuContent_Lambda([this, Index]
						{
							FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ true, nullptr);
							for (const FString& Door : SeatDoorChoices())
							{
								Menu.AddMenuEntry(SeatDoorLabel(Door), FText::GetEmpty(), FSlateIcon(),
									FUIAction(FExecuteAction::CreateLambda([this, Index, Door] { SetSeatDoor(Index, Door); }), FCanExecuteAction(),
										FIsActionChecked::CreateLambda([this, Index, Door]
										{
											return Shape.GetExtraSeats().IsValidIndex(Index) && Shape.GetExtraSeats()[Index].Door == Door;
										})),
									NAME_None, EUserInterfaceActionType::RadioButton);
							}
							return Menu.MakeWidget();
						})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(SCheckBox)
						.ToolTipText(LOCTEXT("SeatHiddenTip", "Turn this on if your model has nothing to sit on here: the person in this seat isn't drawn while they ride."))
						.IsChecked_Lambda([this, Index] { return SeatChoices.IsValidIndex(Index) && SeatChoices[Index].bHidden ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State)
						{
							if (SeatChoices.IsValidIndex(Index))
							{
								SeatChoices[Index].bHidden = State == ECheckBoxState::Checked;
								SeatChoiceChanged();
							}
						})
						[
							SNew(STextBlock).Text(LOCTEXT("SeatHidden", "Hidden"))
						]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
						.Text(LOCTEXT("SeatCopy", "Copy"))
						.ToolTipText(LOCTEXT("SeatCopyTip", "Adds a seat like this one, a row behind it."))
						.IsEnabled_Lambda([this] { return Shape.GetExtraSeats().Num() < FMantleVehicleLimits::MaxExtraSeats; })
						.OnClicked_Lambda([this, Index] { CopySeat(Index); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
						.Text(LOCTEXT("SeatDelete", "Delete"))
						.OnClicked_Lambda([this, Index] { RemoveSeat(Index); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(92, 2, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(SCheckBox)
						.ToolTipText(LOCTEXT("SeatArcTip", "Set where a gun held in this seat may aim, in degrees from straight ahead: left and right edges, then down and up. Off: Mantle works it out from where the seat sits."))
						.IsChecked_Lambda([this, Index] { return SeatChoices.IsValidIndex(Index) && SeatChoices[Index].Arc.bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State)
						{
							if (SeatChoices.IsValidIndex(Index))
							{
								SeatChoices[Index].Arc.bEnabled = State == ECheckBoxState::Checked;
								SeatChoiceChanged();
							}
						})
						[
							SNew(STextBlock).Text(LOCTEXT("SeatArc", "Aim arc"))
						]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					ArcBox(Index, &FMantleVehicleSourceWeaponArc::YawMin, -180.f, 180.f, LOCTEXT("SeatArcLeft", "The left edge: degrees from straight ahead, negative to the left."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					ArcBox(Index, &FMantleVehicleSourceWeaponArc::YawMax, -180.f, 180.f, LOCTEXT("SeatArcRight", "The right edge: degrees from straight ahead, positive to the right."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
				[
					ArcBox(Index, &FMantleVehicleSourceWeaponArc::PitchMin, -90.f, 90.f, LOCTEXT("SeatArcDown", "How far down: degrees below level, negative."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					ArcBox(Index, &FMantleVehicleSourceWeaponArc::PitchMax, -90.f, 90.f, LOCTEXT("SeatArcUp", "How far up: degrees above level."))
				]
			]
		];
	}
}

void SMantleVehicleBuilder::SetSeats(const TArray<FMantleChassisExtraSeat>& Seats, TArray<FSeatChoice> Choices)
{
	Shape.SetExtraSeats(Seats);
	// The frame holds at most FMantleVehicleLimits::MaxExtraSeats; the rows' choices stay one for each seat it kept.
	Choices.SetNum(Shape.GetExtraSeats().Num());
	SeatChoices = MoveTemp(Choices);
	SeatsChanged(/*bRows*/ true);
}

FReply SMantleVehicleBuilder::OnAddSeat()
{
	using namespace MantleVehicleBuilderSeatsPrivate;
	TArray<FMantleChassisExtraSeat> Seats = Shape.GetExtraSeats();
	FMantleChassisExtraSeat Added;
	if (Seats.Num() > 0)
	{
		Added = Seats.Last();
	}
	else
	{
		Added.Location = Shape.GetShaped().Seat;
	}
	Added.Location = (Added.Location - FVector(RowStep, 0.0, 0.0)).BoundToCube(MaxReach);
	Added.Hold = (Added.Hold - FVector(RowStep, 0.0, 0.0)).BoundToCube(MaxReach);
	Seats.Add(Added);
	TArray<FSeatChoice> Choices = SeatChoices;
	Choices.SetNum(Shape.GetExtraSeats().Num());
	Choices.AddDefaulted();
	SetSeats(Seats, MoveTemp(Choices));
	return FReply::Handled();
}

FReply SMantleVehicleBuilder::OnAddSeatRow()
{
	using namespace MantleVehicleBuilderSeatsPrivate;
	TArray<FMantleChassisExtraSeat> Seats = Shape.GetExtraSeats();
	// A row behind the rearmost seat (or the driver's), across the middle, at that seat's height and by the last seat's door.
	FVector Behind = Shape.GetShaped().Seat;
	FString Door;
	for (const FMantleChassisExtraSeat& Seat : Seats)
	{
		if (Seat.Location.X < Behind.X)
		{
			Behind = Seat.Location;
		}
	}
	if (Seats.Num() > 0)
	{
		Door = Seats.Last().Door;
	}
	const int32 Across = FMath::Min(SeatRowCount, FMantleVehicleLimits::MaxExtraSeats - Seats.Num());
	TArray<FSeatChoice> Choices = SeatChoices;
	Choices.SetNum(Seats.Num());
	for (int32 At = 0; At < Across; ++At)
	{
		FMantleChassisExtraSeat& Added = Seats.AddDefaulted_GetRef();
		Added.Location = FVector(Behind.X - RowStep, (At - 0.5 * (Across - 1)) * SeatRowSpacing, Behind.Z).BoundToCube(MaxReach);
		Added.Door = Door;
		Choices.AddDefaulted();
	}
	SetSeats(Seats, MoveTemp(Choices));
	return FReply::Handled();
}

void SMantleVehicleBuilder::CopySeat(const int32 Index)
{
	using namespace MantleVehicleBuilderSeatsPrivate;
	TArray<FMantleChassisExtraSeat> Seats = Shape.GetExtraSeats();
	if (!Seats.IsValidIndex(Index) || Seats.Num() >= FMantleVehicleLimits::MaxExtraSeats)
	{
		return;
	}
	FMantleChassisExtraSeat Copy = Seats[Index];
	Copy.Location = (Copy.Location - FVector(RowStep, 0.0, 0.0)).BoundToCube(MaxReach);
	Copy.Hold = (Copy.Hold - FVector(RowStep, 0.0, 0.0)).BoundToCube(MaxReach);
	Seats.Insert(Copy, Index + 1);
	TArray<FSeatChoice> Choices = SeatChoices;
	Choices.SetNum(Shape.GetExtraSeats().Num());
	const FSeatChoice CopiedChoice = Choices[Index];
	Choices.Insert(CopiedChoice, Index + 1);
	SetSeats(Seats, MoveTemp(Choices));
}

void SMantleVehicleBuilder::RemoveSeat(const int32 Index)
{
	TArray<FMantleChassisExtraSeat> Seats = Shape.GetExtraSeats();
	if (!Seats.IsValidIndex(Index))
	{
		return;
	}
	Seats.RemoveAt(Index);
	TArray<FSeatChoice> Choices = SeatChoices;
	Choices.SetNum(Shape.GetExtraSeats().Num());
	Choices.RemoveAt(Index);
	SetSeats(Seats, MoveTemp(Choices));
}

void SMantleVehicleBuilder::SetSeatStand(const int32 Index, const bool bStand)
{
	TArray<FMantleChassisExtraSeat> Seats = Shape.GetExtraSeats();
	if (!Seats.IsValidIndex(Index))
	{
		return;
	}
	Seats[Index].Pose = bStand ? TEXT("stand") : TEXT("sit");
	// Only a stand spot has a hand grip.
	Seats[Index].bHold = bStand && Seats[Index].bHold;
	if (Shape.SetExtraSeats(Seats))
	{
		SeatsChanged(/*bRows*/ false);
	}
}

void SMantleVehicleBuilder::SetSeatDoor(const int32 Index, const FString& Door)
{
	TArray<FMantleChassisExtraSeat> Seats = Shape.GetExtraSeats();
	if (!Seats.IsValidIndex(Index))
	{
		return;
	}
	Seats[Index].Door = Door;
	if (Shape.SetExtraSeats(Seats))
	{
		SeatsChanged(/*bRows*/ false);
	}
}

void SMantleVehicleBuilder::SeatChoiceChanged()
{
	// Already built: the vehicle takes the hidden ticks and the arcs at once, as it takes the paint.
	if (UMantleVehicleSource* Source = BuiltSource.Get())
	{
		ApplyChoices(*Source);
		SaveNow();
	}
}

void SMantleVehicleBuilder::SeatsChanged(const bool bRows)
{
	// A vehicle built before the change is not this one's, as after any change to the frame (ShapeChanged). The picture keeps its model: only the figures and the
	// seats' boxes move.
	if (BuiltSource.IsValid() || bHaveLastBuild)
	{
		ForgetBuild();
	}
	if (bUseSkin && Preview.bOk)
	{
		Preview.Spec.ExtraSeats = Shape.GetExtraSeats();
		PreviewKind->SetSpec(Preview.Spec);
	}
	PushLightsToPicture();
	if (bRows)
	{
		RebuildSeatRows();
	}
}

TArray<FString> SMantleVehicleBuilder::SeatDoorChoices() const
{
	TArray<FString> Out = { FString() };
	if (bHideRiders)
	{
		// No inside: every door stays shut (FMantleVehicleBuilder::SetHiddenRiders).
		return Out;
	}
	const TPair<EMantleVehicleHingePart, const TCHAR*> Doors[] = {
		{ EMantleVehicleHingePart::DriverDoor, TEXT("driverDoor") },
		{ EMantleVehicleHingePart::PassengerDoor, TEXT("passengerDoor") },
		{ EMantleVehicleHingePart::RearLeftDoor, TEXT("rearLeftDoor") },
		{ EMantleVehicleHingePart::RearRightDoor, TEXT("rearRightDoor") } };
	for (const TPair<EMantleVehicleHingePart, const TCHAR*>& Door : Doors)
	{
		// The passenger's door belongs to the passenger seat (ApplyHinges leaves it shut without one).
		const bool bSeat = Door.Key != EMantleVehicleHingePart::PassengerDoor || Shape.GetPassenger();
		if (bSeat && !HingeChoice.BoneOf(Door.Key).IsNone())
		{
			Out.Add(Door.Value);
		}
	}
	return Out;
}

FText SMantleVehicleBuilder::SeatDoorLabel(const FString& Door)
{
	if (Door == TEXT("driverDoor"))
	{
		return LOCTEXT("SeatDoorDriver", "Driver door");
	}
	if (Door == TEXT("passengerDoor"))
	{
		return LOCTEXT("SeatDoorPassenger", "Passenger door");
	}
	if (Door == TEXT("rearLeftDoor"))
	{
		return LOCTEXT("SeatDoorRearLeft", "Rear left door");
	}
	if (Door == TEXT("rearRightDoor"))
	{
		return LOCTEXT("SeatDoorRearRight", "Rear right door");
	}
	return LOCTEXT("SeatDoorNone", "No door (open seat)");
}

#undef LOCTEXT_NAMESPACE
