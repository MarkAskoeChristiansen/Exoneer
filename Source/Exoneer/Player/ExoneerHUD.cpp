// Copyright Exoneer contributors.
#include "Player/ExoneerHUD.h"
#include "Player/PlayerSurvivalCharacter.h"
#include "Components/SurvivalStatsComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/InteractionComponent.h"
#include "Components/BuildToolComponent.h"
#include "Components/CraftingComponent.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Vehicles/VehicleConstruct.h"
#include "Machines/MachinePiece.h"
#include "Interfaces/Interactable.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Engine.h"

namespace
{
	// Monochrome visor palette (GAME-SCOPE.md module 8).
	const FLinearColor VisorMain(0.55f, 0.9f, 1.f, 0.9f);
	const FLinearColor VisorDim(0.55f, 0.9f, 1.f, 0.45f);
	const FLinearColor VisorWarn(1.f, 0.35f, 0.2f, 0.95f);
	const FLinearColor VisorGood(0.4f, 1.f, 0.5f, 0.9f);

	const TCHAR* MachineStateName(EMachineState State)
	{
		switch (State)
		{
		case EMachineState::Processing: return TEXT("PROCESSING");
		case EMachineState::OutputFull: return TEXT("OUTPUT FULL");
		case EMachineState::LowPower:   return TEXT("LOW POWER");
		default:                        return TEXT("IDLE");
		}
	}
}

void AExoneerHUD::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Sample the real suit drain for the estimated-time-to-empty readout.
	const APlayerSurvivalCharacter* Engineer = Cast<APlayerSurvivalCharacter>(GetOwningPawn());
	if (Engineer && Engineer->Survival && DeltaSeconds > 0.f)
	{
		const float Power = Engineer->Survival->SuitPower;
		if (LastSuitPower >= 0.f)
		{
			const float Rate = (LastSuitPower - Power) / DeltaSeconds;   // + = draining
			SmoothedSuitDrain = FMath::FInterpTo(SmoothedSuitDrain, Rate, DeltaSeconds, 2.f);
		}
		LastSuitPower = Power;
	}

	// Smooth the pilot readouts here, not in DrawHUD - same pattern as the
	// suit drain, and exactly how a bouncing analog needle will behave later.
	if (Engineer && Engineer->PilotedConstruct && DeltaSeconds > 0.f)
	{
		const FVehicleDrivetrainSummary Drivetrain = Engineer->PilotedConstruct->GetDrivetrainSummary();
		SmoothedSpeedMS = FMath::FInterpTo(SmoothedSpeedMS, Drivetrain.SpeedMS, DeltaSeconds, 4.f);
		SmoothedSlip = FMath::FInterpTo(SmoothedSlip, Drivetrain.WorstSlipRatio, DeltaSeconds, 4.f);
	}
	else
	{
		SmoothedSpeedMS = 0.f;
		SmoothedSlip = 0.f;
	}
}

void AExoneerHUD::DrawHUD()
{
	Super::DrawHUD();

	APlayerSurvivalCharacter* Engineer = Cast<APlayerSurvivalCharacter>(GetOwningPawn());
	if (!Canvas || !Engineer)
	{
		return;
	}

	DrawCrosshair();
	DrawVitals(Engineer);
	if (Engineer->IsPiloting())
	{
		DrawPilotPanel(Engineer);
	}
	DrawToolPanel(Engineer);
	DrawInteractionPrompt(Engineer);
	DrawInventory(Engineer);
}

void AExoneerHUD::DrawPilotPanel(const APlayerSurvivalCharacter* Engineer)
{
	// Interim visor instrumentation reading ONLY the public drivetrain
	// summary, so the future diegetic dashboard swaps the renderer while the
	// data source stays. Scope module 8 owns the final form.
	const AVehicleConstruct* Construct = Engineer->PilotedConstruct;
	if (!Construct)
	{
		return;
	}
	const FVehicleDrivetrainSummary Drivetrain = Construct->GetDrivetrainSummary();

	float Y = 24.f + 16.f + 3.f * 15.f + 12.f;   // below the suit vitals block
	const float X = 24.f;
	DrawText(TEXT("== PILOT =="), VisorDim, X, Y, GEngine->GetSmallFont());
	Y += 16.f;

	const bool bGround = Construct->GetControlMode() == EPilotControlMode::Ground;
	// The V hint only appears when the craft has both wheels and thrusters -
	// a single-capability vehicle cannot leave its mode.
	FString ModeValue = bGround ? TEXT("GROUND") : TEXT("FLIGHT");
	if (Drivetrain.bCanDrive && Drivetrain.bCanFly)
	{
		ModeValue += TEXT(" (V)");
	}
	DrawReadout(X, Y, TEXT("MODE"), ModeValue, VisorMain);
	DrawReadout(X, Y, TEXT("SPEED"), FString::Printf(TEXT("%.1f m/s"), SmoothedSpeedMS), VisorMain);

	const float SupplyPct = Construct->PowerSupplyFraction * 100.f;
	DrawReadout(X, Y, TEXT("PWR SUPPLY"), FString::Printf(TEXT("%.0f%%"), SupplyPct), SupplyPct < 75.f ? VisorWarn : VisorMain);

	// Attitude authority is now physical: no gyro blocks, no rotation.
	if (Drivetrain.GyroTorqueNm > 0.f)
	{
		DrawReadout(X, Y, TEXT("GYRO"), FString::Printf(TEXT("%.0f Nm"), Drivetrain.GyroTorqueNm), VisorMain);
	}
	else if (Drivetrain.bCanFly)
	{
		DrawReadout(X, Y, TEXT("GYRO"), TEXT("NONE - no attitude ctrl"), VisorWarn);
	}

	if (Drivetrain.WheelCount > 0)
	{
		// Optimal traction window is s = 0.15..0.25 (GAME-SCOPE 4.1): green
		// inside, warn when traction is collapsing toward full spin.
		const FLinearColor SlipColor = SmoothedSlip > 0.35f ? VisorWarn
			: (SmoothedSlip >= 0.10f && SmoothedSlip <= 0.28f ? VisorGood : VisorMain);
		DrawReadout(X, Y, TEXT("SLIP"), FString::Printf(TEXT("%.0f%%  (best 15-25)"), SmoothedSlip * 100.f), SlipColor);
		DrawReadout(X, Y, TEXT("SINKAGE"), FString::Printf(TEXT("%.0f cm"), Drivetrain.MaxSinkageM * 100.f),
			Drivetrain.MaxSinkageM > 0.12f ? VisorWarn : VisorMain);
		DrawReadout(X, Y, TEXT("TIRES"), FString::Printf(TEXT("%.0f kPa"), Drivetrain.MinTirePressureKPa), VisorMain);
		DrawReadout(X, Y, TEXT("CONTACT"), FString::Printf(TEXT("%d/%d wheels"), Drivetrain.WheelsInContact, Drivetrain.WheelCount),
			Drivetrain.WheelsInContact < Drivetrain.WheelCount ? VisorDim : VisorMain);
		if (Drivetrain.bParkingBrake)
		{
			DrawReadout(X, Y, TEXT("BRAKE"), TEXT("PARKING ENGAGED"), VisorWarn);
		}
	}
}

void AExoneerHUD::DrawCrosshair()
{
	const float CX = Canvas->SizeX * 0.5f;
	const float CY = Canvas->SizeY * 0.5f;
	DrawLine(CX - 7.f, CY, CX - 2.f, CY, VisorMain, 1.f);
	DrawLine(CX + 2.f, CY, CX + 7.f, CY, VisorMain, 1.f);
	DrawLine(CX, CY - 7.f, CX, CY - 2.f, VisorMain, 1.f);
	DrawLine(CX, CY + 2.f, CX, CY + 7.f, VisorMain, 1.f);
}

void AExoneerHUD::DrawReadout(float X, float& Y, const FString& Label, const FString& Value, const FLinearColor& Color)
{
	UFont* Font = GEngine->GetSmallFont();
	DrawText(Label, VisorDim, X, Y, Font);
	DrawText(Value, Color, X + 92.f, Y, Font);
	Y += 15.f;
}

void AExoneerHUD::DrawVitals(const APlayerSurvivalCharacter* Engineer)
{
	const USurvivalStatsComponent* Stats = Engineer->Survival;
	if (!Stats)
	{
		return;
	}

	float Y = 24.f;
	const float X = 24.f;
	DrawText(TEXT("== SUIT =="), VisorDim, X, Y, GEngine->GetSmallFont());
	Y += 16.f;

	// Suit power with estimated time to empty from the MEASURED drain.
	const float PowerPct = Stats->GetSuitPowerNormalized() * 100.f;
	FString PowerValue = FString::Printf(TEXT("%.0f%%"), PowerPct);
	if (SmoothedSuitDrain > 0.01f)
	{
		const int32 SecondsLeft = FMath::RoundToInt(Stats->SuitPower / SmoothedSuitDrain);
		PowerValue += FString::Printf(TEXT("  (empty in %d:%02d)"), SecondsLeft / 60, SecondsLeft % 60);
	}
	else if (SmoothedSuitDrain < -0.01f)
	{
		PowerValue += TEXT("  (charging)");
	}
	DrawReadout(X, Y, TEXT("POWER"), PowerValue, PowerPct < 20.f ? VisorWarn : VisorMain);

	const float O2Pct = Stats->GetOxygenNormalized() * 100.f;
	DrawReadout(X, Y, TEXT("OXYGEN"), FString::Printf(TEXT("%.0f%%"), O2Pct), O2Pct < 20.f ? VisorWarn : VisorMain);

	const float Temp = Stats->GetBodyTemperature();
	const bool bTempDanger = Temp < Stats->MinSafeTempC + 3.f || Temp > Stats->MaxSafeTempC - 3.f;
	DrawReadout(X, Y, TEXT("BODY TEMP"), FString::Printf(TEXT("%.1f C"), Temp), bTempDanger ? VisorWarn : VisorMain);
}

void AExoneerHUD::DrawToolPanel(APlayerSurvivalCharacter* Engineer)
{
	float Y = 24.f;
	const float X = Canvas->SizeX - 280.f;
	UFont* Font = GEngine->GetSmallFont();

	const TCHAR* ToolName = TEXT("NONE");
	switch (Engineer->ToolMode)
	{
	case EPlayerToolMode::Mining: ToolName = TEXT("MINING BEAM"); break;
	case EPlayerToolMode::Build:  ToolName = TEXT("BUILD PLACEMENT"); break;
	case EPlayerToolMode::Weld:   ToolName = TEXT("ARC WELDER"); break;
	default: break;
	}
	DrawText(FString::Printf(TEXT("== TOOL: %s =="), ToolName), VisorDim, X, Y, Font);
	Y += 16.f;

	UBuildToolComponent* BuildTool = Engineer->BuildTool;

	// Quick bar (B cycles) with the current selection highlighted.
	const int32 Selected = Engineer->GetQuickBarIndex();
	for (int32 i = 0; i < Engineer->QuickBar.Num(); ++i)
	{
		FString Name = TEXT("?");
		if (const UPieceDefinitionDataAsset* Piece = Cast<UPieceDefinitionDataAsset>(Engineer->QuickBar[i]))
		{
			Name = Piece->DisplayName.ToString();
		}
		else if (const UVehicleBlockDefinitionDataAsset* Block = Cast<UVehicleBlockDefinitionDataAsset>(Engineer->QuickBar[i]))
		{
			Name = Block->DisplayName.ToString() + TEXT("  [vehicle]");
		}
		const bool bSel = i == Selected;
		DrawText(FString::Printf(TEXT("%s %s"), bSel ? TEXT(">") : TEXT(" "), *Name),
			bSel ? VisorMain : VisorDim, X, Y, Font);
		Y += 13.f;
	}
	Y += 6.f;

	if (BuildTool)
	{
		// Current aim of the selected vehicle block (R cycles a curated list:
		// six thrust directions for thrusters, four yaws for the rest).
		if (Engineer->ToolMode == EPlayerToolMode::Build)
		{
			const FString AimLabel = BuildTool->GetOrientationLabel();
			if (!AimLabel.IsEmpty())
			{
				DrawText(FString::Printf(TEXT("AIM  %s   [R]"), *AimLabel), VisorGood, X, Y, Font);
				Y += 15.f;
			}
		}

		// Placement validity / server rejection.
		if (BuildTool->GetMode() != EBuildToolMode::None)
		{
			const EBuildPlacementError Error = BuildTool->GetLastPreviewError();
			if (Error != EBuildPlacementError::None)
			{
				DrawText(FString::Printf(TEXT("PLACEMENT: %s"), *UEnum::GetDisplayValueAsText(Error).ToString()),
					VisorWarn, X, Y, Font);
				Y += 15.f;
			}
		}

		// Weld beam status from the last server feedback (fresh ones only).
		if (GetWorld() && GetWorld()->GetTimeSeconds() - BuildTool->LastWeldFeedbackSeconds < 1.2)
		{
			switch (BuildTool->LastWeldResult)
			{
			case 0:
				DrawText(FString::Printf(TEXT("WELDING  %d%%"), FMath::RoundToInt(BuildTool->LastWeldProgress01 * 100.f)), VisorGood, X, Y, Font);
				break;
			case 1: DrawText(TEXT("WELD FAULT: SUIT POWER EMPTY"), VisorWarn, X, Y, Font); break;
			case 2: DrawText(TEXT("WELD STALLED: MISSING MATERIALS"), VisorWarn, X, Y, Font); break;
			case 3: DrawText(TEXT("TARGET COMPLETE"), VisorDim, X, Y, Font); break;
			case 4: DrawText(TEXT("WELD BEAM: NO TARGET"), VisorDim, X, Y, Font); break;
			default: break;
			}
			Y += 15.f;
		}
	}
}

void AExoneerHUD::DrawInteractionPrompt(const APlayerSurvivalCharacter* Engineer)
{
	if (!Engineer->Interactor)
	{
		return;
	}
	AActor* Focus = Engineer->Interactor->GetFocusedActor();
	if (!Focus)
	{
		return;
	}

	UFont* Font = GEngine->GetSmallFont();
	const float CX = Canvas->SizeX * 0.5f;
	float Y = Canvas->SizeY * 0.5f + 34.f;

	const FText Prompt = IInteractable::Execute_GetInteractionPrompt(Focus);
	DrawText(FString::Printf(TEXT("[E]  %s"), *Prompt.ToString()), VisorMain, CX - 60.f, Y, Font);
	Y += 15.f;

	// Machines get a glance readout: state plus head-of-queue progress.
	if (const AMachinePiece* Machine = Cast<AMachinePiece>(Focus))
	{
		FString Line = MachineStateName(Machine->MachineState);
		if (const UCraftingComponent* Crafting = Machine->FindComponentByClass<UCraftingComponent>())
		{
			if (Crafting->GetQueueSize() > 0)
			{
				Line += FString::Printf(TEXT("   queue %d   %d%%"),
					Crafting->GetQueueSize(), FMath::RoundToInt(Crafting->GetProgress() * 100.f));
			}
		}
		DrawText(Line, Machine->MachineState == EMachineState::LowPower ? VisorWarn : VisorDim, CX - 60.f, Y, Font);
	}
}

void AExoneerHUD::DrawInventory(const APlayerSurvivalCharacter* Engineer)
{
	if (!Engineer->Inventory)
	{
		return;
	}
	const TArray<FInventoryStack>& Stacks = Engineer->Inventory->GetStacks();
	if (Stacks.Num() == 0)
	{
		return;
	}

	UFont* Font = GEngine->GetSmallFont();
	const float X = 24.f;
	float Y = Canvas->SizeY - 40.f - 13.f * FMath::Min(Stacks.Num(), 8);

	DrawText(FString::Printf(TEXT("== CARGO  %.0f%% =="), Engineer->Inventory->GetLoadFraction() * 100.f),
		VisorDim, X, Y, Font);
	Y += 15.f;
	for (int32 i = 0; i < Stacks.Num() && i < 8; ++i)
	{
		const FString Name = Stacks[i].Item ? Stacks[i].Item->DisplayName.ToString() : TEXT("?");
		DrawText(FString::Printf(TEXT("%3d  %s"), Stacks[i].Count, *Name), VisorMain, X, Y, Font);
		Y += 13.f;
	}
}
