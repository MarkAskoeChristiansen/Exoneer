// Copyright Exoneer contributors.
#include "Player/ExoneerHUD.h"
#include "Player/PlayerSurvivalCharacter.h"
#include "Components/SurvivalStatsComponent.h"
#include "Components/InteractionComponent.h"
#include "Components/BuildToolComponent.h"
#include "Components/PowerComponent.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Vehicles/VehicleConstruct.h"
#include "Machines/MachinePiece.h"
#include "Machines/UmbilicalPortPiece.h"
#include "Building/BasePiece.h"
#include "Components/ConstructionComponent.h"
#include "Interfaces/Interactable.h"
#include "GameplayTagContainer.h"
#include "Blueprint/UserWidget.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Maintenance/ExoneerMaintenance.h"
#include "TextureResource.h"

namespace
{
	const FLinearColor Glow(0.72f, 0.95f, 1.00f, 0.94f);
	const FLinearColor GlowDim(0.42f, 0.76f, 0.82f, 0.66f);
	const FLinearColor GlowFaint(0.30f, 0.66f, 0.72f, 0.22f);
	const FLinearColor Warn(1.00f, 0.38f, 0.24f, 0.96f);
	const FLinearColor Good(0.48f, 0.96f, 0.78f, 0.94f);

	float Smooth01(float Value)
	{
		const float T = FMath::Clamp(Value, 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	UTexture2D* CreateVisorTexture(const TCHAR* Name, int32 Size)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Size, Size, PF_B8G8R8A8, Name);
		if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
		{
			return nullptr;
		}
		Texture->SRGB = false;
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->CompressionSettings = TC_VectorDisplacementmap;
		return Texture;
	}

	FString FormatClock(int32 Seconds)
	{
		Seconds = FMath::Max(0, Seconds);
		return FString::Printf(TEXT("%d:%02d"), Seconds / 60, Seconds % 60);
	}

	FString QuickBarName(const UPrimaryDataAsset* Asset)
	{
		if (const UPieceDefinitionDataAsset* Piece = Cast<UPieceDefinitionDataAsset>(Asset))
		{
			return Piece->DisplayName.ToString();
		}
		if (const UVehicleBlockDefinitionDataAsset* Block = Cast<UVehicleBlockDefinitionDataAsset>(Asset))
		{
			return Block->DisplayName.ToString();
		}
		return TEXT("SUIT TOOL");
	}
}

AExoneerHUD::AExoneerHUD()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AExoneerHUD::BeginPlay()
{
	Super::BeginPlay();
	EnsureVisorGlass();
	ApplySuitOptics();
	EnsureVisorWidget();
}

void AExoneerHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (VisorWidget)
	{
		VisorWidget->RemoveFromParent();
		VisorWidget = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void AExoneerHUD::EnsureVisorGlass()
{
	if (VisorGlass)
	{
		return;
	}

	const int32 Size = 256;
	VisorGlass = CreateVisorTexture(TEXT("ExoneerVisorGlass"), Size);
	if (!VisorGlass)
	{
		return;
	}

	FTexture2DMipMap& Mip = VisorGlass->GetPlatformData()->Mips[0];
	FColor* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
	if (!Pixels)
	{
		VisorGlass = nullptr;
		return;
	}

	for (int32 Y = 0; Y < Size; ++Y)
	{
		for (int32 X = 0; X < Size; ++X)
		{
			const float Nx = (static_cast<float>(X) + 0.5f) / static_cast<float>(Size) * 2.f - 1.f;
			const float Ny = (static_cast<float>(Y) + 0.5f) / static_cast<float>(Size) * 2.f - 1.f;
			const float Ellipse = FMath::Sqrt(FMath::Square(Nx / 0.98f) + FMath::Square(Ny / 0.86f));
			const float Dark = FMath::Pow(Smooth01((Ellipse - 0.72f) / 0.78f), 1.60f) * 0.30f;
			Pixels[Y * Size + X] = FColor(1, 4, 6, static_cast<uint8>(Dark * 255.f));
		}
	}
	Mip.BulkData.Unlock();
	VisorGlass->UpdateResource();
}

void AExoneerHUD::ApplySuitOpticsToCamera(UCameraComponent* Cam)
{
	if (!Cam)
	{
		return;
	}
	Cam->PostProcessBlendWeight = 1.f;
	FPostProcessSettings& PP = Cam->PostProcessSettings;
	PP.bOverride_SceneFringeIntensity = true;
	PP.SceneFringeIntensity = 0.18f;
	PP.bOverride_ChromaticAberrationStartOffset = true;
	PP.ChromaticAberrationStartOffset = 0.82f;
	PP.bOverride_VignetteIntensity = true;
	PP.VignetteIntensity = 0.16f;
	PP.bOverride_FilmGrainIntensity = true;
	PP.FilmGrainIntensity = 0.012f;
	// The visor supplies its own restrained highlights. Never rebalance the
	// whole world just to make HUD lines glow.
	PP.bOverride_BloomIntensity = false;
	PP.bOverride_BloomThreshold = false;
}

void AExoneerHUD::ApplySuitOptics()
{
	APlayerSurvivalCharacter* Engineer = Cast<APlayerSurvivalCharacter>(GetOwningPawn());
	if (!Engineer)
	{
		return;
	}
	ApplySuitOpticsToCamera(Engineer->Camera);
	ApplySuitOpticsToCamera(Engineer->ChaseCamera);
	OpticsOwner = Engineer;
	bOpticsApplied = true;
}

void AExoneerHUD::DrawVisorGlass()
{
	if (!Canvas || !VisorGlass)
	{
		return;
	}
	const float W = static_cast<float>(Canvas->SizeX);
	const float H = static_cast<float>(Canvas->SizeY);
	const float Scale = FMath::Clamp(H / 1080.f, 0.78f, 1.35f);
	const FVector2D Drift = VisorOffsetPixels * (Scale * 0.025f);
	const float Overscan = 3.f * Scale;
	DrawTexture(VisorGlass, -Overscan + Drift.X, -Overscan + Drift.Y,
		W + Overscan * 2.f, H + Overscan * 2.f, 0.f, 0.f, 1.f, 1.f,
		FLinearColor::White, BLEND_Translucent);
}

void AExoneerHUD::EnsureVisorWidget()
{
	if (VisorWidget || !PlayerOwner)
	{
		return;
	}
	if (!VisorWidgetClass)
	{
		VisorWidgetClass = LoadClass<UUserWidget>(
			nullptr, TEXT("/Game/Exoneer/UI/WBP_VisorHUD.WBP_VisorHUD_C"));
	}
	if (!VisorWidgetClass)
	{
		return;
	}
	VisorWidget = CreateWidget<UUserWidget>(PlayerOwner, VisorWidgetClass);
	if (VisorWidget)
	{
		VisorWidget->SetVisibility(ESlateVisibility::HitTestInvisible);
		VisorWidget->AddToViewport(20);
	}
}

void AExoneerHUD::SetWidgetText(FName WidgetName, const FString& Text, const FLinearColor& Color)
{
	if (VisorWidget)
	{
		if (UTextBlock* TextBlock = Cast<UTextBlock>(VisorWidget->GetWidgetFromName(WidgetName)))
		{
			TextBlock->SetText(FText::FromString(Text));
			TextBlock->SetColorAndOpacity(FSlateColor(Color));
		}
	}
}

void AExoneerHUD::SetWidgetVisible(FName WidgetName, bool bVisible)
{
	if (VisorWidget)
	{
		if (UWidget* Widget = VisorWidget->GetWidgetFromName(WidgetName))
		{
			Widget->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		}
	}
}

void AExoneerHUD::SetWidgetColor(FName WidgetName, const FLinearColor& Color)
{
	if (!VisorWidget)
	{
		return;
	}
	if (UBorder* Border = Cast<UBorder>(VisorWidget->GetWidgetFromName(WidgetName)))
	{
		Border->SetBrushColor(Color);
	}
	else if (UTextBlock* TextBlock = Cast<UTextBlock>(VisorWidget->GetWidgetFromName(WidgetName)))
	{
		TextBlock->SetColorAndOpacity(FSlateColor(Color));
	}
}

void AExoneerHUD::UpdateVisorMotion(float DeltaSeconds, APlayerSurvivalCharacter* Engineer)
{
	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 0.05f);
	if (!Engineer)
	{
		MotionOwner.Reset();
		bHasPreviousViewRotation = false;
		return;
	}
	if (MotionOwner.Get() != Engineer)
	{
		MotionOwner = Engineer;
		bHasPreviousViewRotation = false;
		SmoothedLocomotion01 = 0.f;
		VisorOffsetPixels = FVector2D::ZeroVector;
		VisorOffsetVelocity = FVector2D::ZeroVector;
		VisorRollDegrees = 0.f;
		VisorRollVelocity = 0.f;
	}
	if (Dt <= 0.f)
	{
		return;
	}

	const FRotator ViewRotation = PlayerOwner && PlayerOwner->PlayerCameraManager
		? PlayerOwner->PlayerCameraManager->GetCameraRotation()
		: Engineer->GetViewRotation();
	float YawRate = 0.f;
	float PitchRate = 0.f;
	if (bHasPreviousViewRotation && DeltaSeconds < 0.15f)
	{
		const float InvDt = 1.f / FMath::Max(DeltaSeconds, 0.001f);
		YawRate = FMath::FindDeltaAngleDegrees(PreviousViewRotation.Yaw, ViewRotation.Yaw) * InvDt;
		PitchRate = FMath::FindDeltaAngleDegrees(PreviousViewRotation.Pitch, ViewRotation.Pitch) * InvDt;
	}
	PreviousViewRotation = ViewRotation;
	bHasPreviousViewRotation = true;

	const FVector WorldVelocity = Engineer->GetVelocity();
	const float Speed = WorldVelocity.Size2D();
	const UCharacterMovementComponent* Movement = Engineer->GetCharacterMovement();
	const bool bWalkingOnGround = !Engineer->IsPiloting() && Movement && Movement->IsMovingOnGround();
	const float MoveTarget = bWalkingOnGround
		? FMath::Clamp(Speed / FMath::Max(Engineer->WalkSpeed * 0.55f, 1.f), 0.f, 1.f)
		: 0.f;
	SmoothedLocomotion01 = FMath::FInterpTo(SmoothedLocomotion01, MoveTarget, Dt, 7.f);
	const float SprintRange = FMath::Max(Engineer->SprintSpeed - Engineer->WalkSpeed * 0.85f, 1.f);
	const float Sprint01 = FMath::Clamp((Speed - Engineer->WalkSpeed * 0.85f) / SprintRange, 0.f, 1.f);
	if (bWalkingOnGround && Speed > 5.f)
	{
		const float SpeedCadence = FMath::Clamp(Speed / FMath::Max(Engineer->WalkSpeed, 1.f), 0.65f, 1.12f);
		const float FrequencyHz = FMath::Lerp(1.65f, 2.45f, Sprint01) * SpeedCadence;
		FootstepPhase = FMath::Fmod(FootstepPhase + Dt * FrequencyHz * 2.f * PI, 2.f * PI);
	}

	const float BobAmplitude = FMath::Lerp(WalkBobPixels, SprintBobPixels, Sprint01) * SmoothedLocomotion01;
	const FVector2D Bob(
		FMath::Sin(FootstepPhase) * BobAmplitude * 0.42f,
		-FMath::Cos(FootstepPhase * 2.f) * BobAmplitude * 0.52f);
	const float MaxLookTrail = MaxVisorOffsetPixels * 0.82f;
	FVector2D Target(
		FMath::Clamp(-YawRate * LookLagResponse, -MaxLookTrail, MaxLookTrail),
		FMath::Clamp(PitchRate * LookLagResponse * 0.72f, -MaxLookTrail, MaxLookTrail));
	Target += Bob;
	if (!bWalkingOnGround && !Engineer->IsPiloting())
	{
		Target.Y += FMath::Clamp(-WorldVelocity.Z * 0.0035f, -1.8f, 1.8f);
	}
	const float Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	Target += FVector2D(FMath::Sin(Time * 0.73f) * 0.16f, FMath::Sin(Time * 0.51f + 1.7f) * 0.12f);
	Target *= VisorMotionScale;
	if (Target.SizeSquared() > FMath::Square(MaxVisorOffsetPixels))
	{
		Target = Target.GetSafeNormal() * MaxVisorOffsetPixels;
	}

	const FVector Right = FRotationMatrix(FRotator(0.f, ViewRotation.Yaw, 0.f)).GetUnitAxis(EAxis::Y);
	const float Strafe01 = FVector::DotProduct(WorldVelocity, Right) / FMath::Max(Engineer->SprintSpeed, 1.f);
	const float TargetRoll = FMath::Clamp((
		-YawRate * 0.0032f - Strafe01 * 0.24f
		+ FMath::Sin(FootstepPhase) * SmoothedLocomotion01 * 0.055f) * VisorMotionScale,
		-MaxVisorRollDegrees, MaxVisorRollDegrees);

	const int32 Steps = FMath::Clamp(FMath::CeilToInt(Dt * 120.f), 1, 6);
	const float Step = Dt / static_cast<float>(Steps);
	constexpr float PositionStiffness = 72.f;
	constexpr float PositionDamping = 15.5f;
	constexpr float RollStiffness = 84.f;
	constexpr float RollDamping = 17.5f;
	for (int32 i = 0; i < Steps; ++i)
	{
		VisorOffsetVelocity += ((Target - VisorOffsetPixels) * PositionStiffness
			- VisorOffsetVelocity * PositionDamping) * Step;
		VisorOffsetPixels += VisorOffsetVelocity * Step;
		VisorRollVelocity += ((TargetRoll - VisorRollDegrees) * RollStiffness
			- VisorRollVelocity * RollDamping) * Step;
		VisorRollDegrees += VisorRollVelocity * Step;
	}
}

void AExoneerHUD::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Max(DeltaSeconds, 0.f);
	APlayerSurvivalCharacter* Engineer = Cast<APlayerSurvivalCharacter>(GetOwningPawn());
	UpdateVisorMotion(DeltaSeconds, Engineer);
	if (Engineer && Engineer->Survival && Dt > 0.f)
	{
		const float Power = Engineer->Survival->SuitPower;
		if (LastSuitPower >= 0.f)
		{
			SmoothedSuitDrain = FMath::FInterpTo(
				SmoothedSuitDrain, (LastSuitPower - Power) / Dt, Dt, 2.f);
		}
		LastSuitPower = Power;
	}
	if (Engineer && Engineer->PilotedConstruct && Dt > 0.f)
	{
		const FVehicleDrivetrainSummary Drivetrain = Engineer->PilotedConstruct->GetDrivetrainSummary();
		SmoothedSpeedMS = FMath::FInterpTo(SmoothedSpeedMS, Drivetrain.SpeedMS, Dt, 4.f);
		SmoothedSlip = FMath::FInterpTo(SmoothedSlip, Drivetrain.WorstSlipRatio, Dt, 4.f);
		if (Drivetrain.MaxWindingTempC > NoWindingReadingC)
		{
			SmoothedMotorTempC = SmoothedMotorTempC > NoWindingReadingC
				? FMath::FInterpTo(SmoothedMotorTempC, Drivetrain.MaxWindingTempC, Dt, 4.f)
				: Drivetrain.MaxWindingTempC;
		}
		else
		{
			SmoothedMotorTempC = NoWindingReadingC;
		}
	}
	else
	{
		SmoothedSpeedMS = 0.f;
		SmoothedSlip = 0.f;
		SmoothedMotorTempC = NoWindingReadingC;
	}

	EnsureVisorWidget();
	if (Engineer && VisorWidget)
	{
		UpdateVisorWidget(Engineer);
	}
}

void AExoneerHUD::UpdateVisorWidget(APlayerSurvivalCharacter* Engineer)
{
	if (!Engineer || !VisorWidget)
	{
		return;
	}

	if (UWidget* MotionLayer = VisorWidget->GetWidgetFromName(TEXT("MotionLayer")))
	{
		int32 ViewX = 1920;
		int32 ViewY = 1080;
		if (PlayerOwner)
		{
			PlayerOwner->GetViewportSize(ViewX, ViewY);
		}
		const float Scale = FMath::Clamp(static_cast<float>(ViewY) / 1080.f, 0.78f, 1.35f);
		MotionLayer->SetRenderTranslation(VisorOffsetPixels * Scale);
		MotionLayer->SetRenderTransformAngle(VisorRollDegrees);
		MotionLayer->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
	}

	const FRotator ViewRotation = PlayerOwner && PlayerOwner->PlayerCameraManager
		? PlayerOwner->PlayerCameraManager->GetCameraRotation()
		: Engineer->GetViewRotation();
	const int32 Heading = FMath::RoundToInt(FMath::Fmod(ViewRotation.Yaw + 360.f, 360.f)) % 360;
	SetWidgetText(TEXT("HeadingText"), FString::Printf(TEXT("%03d"), Heading), Glow);

	if (USurvivalStatsComponent* Stats = Engineer->Survival)
	{
		const float PowerFrac = FMath::Clamp(Stats->GetSuitPowerNormalized(), 0.f, 1.f);
		const float O2Frac = FMath::Clamp(Stats->GetOxygenNormalized(), 0.f, 1.f);
		const FLinearColor PowerColor = PowerFrac < 0.2f ? Warn : Glow;
		const FLinearColor O2Color = O2Frac < 0.2f ? Warn : Good;
		SetWidgetText(TEXT("PowerValueText"), FString::Printf(TEXT("%.0f kJ"), Stats->SuitPower), PowerColor);
		SetWidgetText(TEXT("OxygenValueText"), FString::Printf(TEXT("%.0f L"), Stats->Oxygen), O2Color);

		FString PowerEta = TEXT("STABLE");
		if (SmoothedSuitDrain > 0.01f)
		{
			PowerEta = FormatClock(FMath::RoundToInt(Stats->SuitPower / SmoothedSuitDrain)) + TEXT(" REM");
		}
		else if (SmoothedSuitDrain < -0.01f)
		{
			PowerEta = TEXT("CHARGING");
		}
		SetWidgetText(TEXT("PowerEtaText"), PowerEta, GlowDim);

		FString O2Eta = TEXT("STABLE");
		if (Engineer->UmbilicalSource && Stats->Oxygen < Stats->SuitO2CapacityL - 0.1f)
		{
			O2Eta = TEXT("UMBILICAL");
		}
		else if (Stats->GetOxygenDrainLps() > 0.001f)
		{
			O2Eta = FormatClock(FMath::RoundToInt(Stats->Oxygen / Stats->GetOxygenDrainLps())) + TEXT(" REM");
		}
		SetWidgetText(TEXT("OxygenEtaText"), O2Eta, GlowDim);
		if (UBorder* Fill = Cast<UBorder>(VisorWidget->GetWidgetFromName(TEXT("PowerBarFill"))))
		{
			Fill->SetRenderTransformPivot(FVector2D(0.f, 0.5f));
			Fill->SetRenderScale(FVector2D(PowerFrac, 1.f));
			Fill->SetBrushColor(PowerColor);
		}

		FString SuitWarning;
		FLinearColor SuitWarningColor = Good;
		if (Stats->SuitCondition.LeakRateLps > ExoneerMaintenance::UmbilicalO2MakeupLps)
		{
			SuitWarning = TEXT("SEAL LEAK EXCEEDS MAKEUP");
			SuitWarningColor = Warn;
		}
		else if (Stats->SuitCondition.LeakRateLps > ExoneerMaintenance::SealEmergencyLps)
		{
			SuitWarning = FString::Printf(TEXT("SEAL LEAK  %.2f L/s"), Stats->SuitCondition.LeakRateLps);
			SuitWarningColor = Warn;
		}
		else if (Engineer->UmbilicalSource)
		{
			SuitWarning = TEXT("UMBILICAL LINK");
		}
		SetWidgetVisible(TEXT("SuitWarningText"), !SuitWarning.IsEmpty());
		SetWidgetText(TEXT("SuitWarningText"), SuitWarning, SuitWarningColor);
	}

	const TCHAR* Tool = TEXT("STOWED");
	switch (Engineer->ToolMode)
	{
	case EPlayerToolMode::Mining: Tool = TEXT("MINING"); break;
	case EPlayerToolMode::Build: Tool = TEXT("BUILD"); break;
	case EPlayerToolMode::Weld: Tool = TEXT("WELD"); break;
	default: break;
	}
	SetWidgetText(TEXT("ToolModeText"), Tool, Glow);
	const int32 Selected = Engineer->GetQuickBarIndex();
	const FString Selection = Engineer->QuickBar.IsValidIndex(Selected)
		? QuickBarName(Engineer->QuickBar[Selected]).ToUpper()
		: TEXT("SUIT TOOL");
	SetWidgetText(TEXT("SelectionText"), Selection.Left(28), GlowDim);

	const int32 Window = 5;
	const int32 Count = Engineer->QuickBar.Num();
	const int32 Start = Count > Window
		? FMath::Clamp((Selected == INDEX_NONE ? 0 : Selected) - Window / 2, 0, Count - Window)
		: 0;
	for (int32 i = 0; i < Window; ++i)
	{
		const int32 Index = Start + i;
		const bool bOccupied = Index < Count;
		const bool bSelected = Index == Selected;
		SetWidgetColor(*FString::Printf(TEXT("Slot%d"), i),
			bSelected ? Glow : (bOccupied ? GlowFaint : FLinearColor(0.15f, 0.28f, 0.30f, 0.10f)));
	}

	FString WeldFeedback;
	FLinearColor WeldColor = Good;
	if (Engineer->BuildTool && GetWorld()
		&& GetWorld()->GetTimeSeconds() - Engineer->BuildTool->LastWeldFeedbackSeconds < 1.2f)
	{
		switch (Engineer->BuildTool->LastWeldResult)
		{
		case 0:
			if (Engineer->BuildTool->LastWeldTargetId != ExoneerConstruction::NoTargetId)
			{
				const float Progress = Engineer->BuildTool->bLiveWeldTargetValid
					? Engineer->BuildTool->LiveWeldProgress01 : Engineer->BuildTool->LastWeldProgress01;
				WeldFeedback = FString::Printf(TEXT("WELD  %d%%"),
					FMath::Clamp(FMath::FloorToInt32(Progress * 100.f), 0, 100));
			}
			break;
		case 1: WeldFeedback = TEXT("NO SUIT POWER"); WeldColor = Warn; break;
		case 2: WeldFeedback = TEXT("MATERIAL REQUIRED"); WeldColor = Warn; break;
		case 6: WeldFeedback = TEXT("COMPONENT REPLACED"); break;
		case 7: WeldFeedback = TEXT("SURFACE CLEAN"); break;
		default: break;
		}
	}
	SetWidgetVisible(TEXT("WeldFeedbackPanel"), !WeldFeedback.IsEmpty());
	SetWidgetText(TEXT("WeldFeedbackText"), WeldFeedback, WeldColor);

	bool bShowPrompt = false;
	if (!Engineer->IsPiloting() && Engineer->Interactor)
	{
		if (AActor* Focus = Engineer->Interactor->GetFocusedActor())
		{
			FText Prompt;
			if (const AUmbilicalPortPiece* Port = Cast<AUmbilicalPortPiece>(Focus))
			{
				Prompt = Port->GetPromptFor(Engineer);
			}
			else
			{
				Prompt = IInteractable::Execute_GetInteractionPrompt(Focus);
			}
			const FGameplayTagContainer InteractionTags = IInteractable::Execute_GetInteractionTags(Focus);
			const ABasePiece* FocusPiece = Cast<ABasePiece>(Focus);
			const bool bNeedsWeld = FocusPiece && FocusPiece->Construction && !FocusPiece->Construction->IsComplete();
			if (!InteractionTags.IsEmpty() || bNeedsWeld)
			{
				bShowPrompt = true;
				SetWidgetText(TEXT("PromptKeyText"), bNeedsWeld && InteractionTags.IsEmpty() ? TEXT("LMB") : TEXT("E"), Glow);
				SetWidgetText(TEXT("PromptActionText"), Prompt.ToString().ToUpper(), bNeedsWeld ? Good : Glow);
			}
		}
	}
	SetWidgetVisible(TEXT("PromptPanel"), bShowPrompt);

	bool bShowContext = false;
	if (Engineer->Interactor)
	{
		if (const ABasePiece* Piece = Cast<ABasePiece>(Engineer->Interactor->GetFocusedActor()))
		{
			if (Piece->Def && Piece->Construction && Piece->Construction->IsComplete())
			{
				bShowContext = true;
				const FString Title = Piece->Def->DisplayName.IsEmpty()
					? Piece->Def->PieceId.ToString() : Piece->Def->DisplayName.ToString();
				SetWidgetText(TEXT("ContextTitleText"), Title.ToUpper(), Glow);
				TArray<FString> Lines;
				TArray<FLinearColor> Colors;
				if (Piece->Def->LoadCapacityKg > 0.f)
				{
					const float G = GetWorld() ? FMath::Abs(GetWorld()->GetGravityZ()) / 100.f : 0.f;
					const float LoadKg = G > KINDA_SMALL_NUMBER ? Piece->LastLoadN / G : 0.f;
					Lines.Add(FString::Printf(TEXT("LOAD   %.0f / %.0f kg"), LoadKg, Piece->Def->LoadCapacityKg));
					Colors.Add(LoadKg > Piece->Def->LoadCapacityKg ? Warn : GlowDim);
				}
				if (Piece->IsLoadCondemned() || Piece->Condition.DeflectionMm > 0.4f)
				{
					Lines.Add(FString::Printf(TEXT("DEFLECTION   %.1f mm%s"), Piece->Condition.DeflectionMm,
						Piece->IsLoadCondemned() ? TEXT("  REBUILD") : TEXT("")));
					Colors.Add(Piece->IsLoadCondemned() ? Warn : GlowDim);
				}
				if (Piece->Def->EnergyStorage > 0.f)
				{
					const AMachinePiece* Machine = Cast<AMachinePiece>(Piece);
					const float StoredKJ = (Machine && Machine->Power ? Machine->Power->StoredEnergy : 0.f) / 1000.f;
					const float EffectiveKJ = ExoneerMaintenance::EffectiveCapacityJ(
						Piece->Def->EnergyStorage, Piece->Condition.CapacityFade01) / 1000.f;
					Lines.Add(FString::Printf(TEXT("ENERGY   %.0f / %.0f kJ"), StoredKJ, EffectiveKJ));
					Colors.Add(GlowDim);
				}
				for (int32 i = 0; i < 3; ++i)
				{
					const bool bHasLine = Lines.IsValidIndex(i);
					SetWidgetVisible(*FString::Printf(TEXT("ContextLine%dText"), i + 1), bHasLine);
					SetWidgetText(*FString::Printf(TEXT("ContextLine%dText"), i + 1),
						bHasLine ? Lines[i] : FString(), bHasLine ? Colors[i] : GlowDim);
				}
			}
		}
	}
	SetWidgetVisible(TEXT("ContextPanel"), bShowContext);

	const bool bPiloting = Engineer->IsPiloting() && Engineer->PilotedConstruct;
	SetWidgetVisible(TEXT("VehiclePanel"), bPiloting);
	if (bPiloting)
	{
		const FVehicleDrivetrainSummary D = Engineer->PilotedConstruct->GetDrivetrainSummary();
		const bool bGround = Engineer->PilotedConstruct->GetControlMode() == EPilotControlMode::Ground;
		const float StoredKJ = D.StoredEnergyWs / 1000.f;
		const float CapacityKJ = D.EnergyCapacityWs / 1000.f;
		SetWidgetText(TEXT("VehicleModeText"),
			CapacityKJ > KINDA_SMALL_NUMBER
				? FString::Printf(TEXT("%s   %.0f / %.0f kJ"),
					bGround ? TEXT("GROUND") : TEXT("FLIGHT"), StoredKJ, CapacityKJ)
				: (bGround ? TEXT("GROUND CONTROL") : TEXT("FLIGHT CONTROL")), GlowDim);
		SetWidgetText(TEXT("VehicleSpeedText"), FString::Printf(TEXT("%.1f m/s"), SmoothedSpeedMS), Glow);
		SetWidgetText(TEXT("VehicleAttitudeText"),
			FString::Printf(TEXT("BANK  %+.0f     PITCH  %+.0f"), D.BankDeg, D.PitchDeg),
			FMath::Max(FMath::Abs(D.BankDeg), FMath::Abs(D.PitchDeg)) > 30.f ? Warn : GlowDim);

		FString Status = TEXT("SYSTEMS NOMINAL");
		FLinearColor StatusColor = Good;
		const TCHAR* Axis[3] = { TEXT("ROLL"), TEXT("PITCH"), TEXT("YAW") };
		const float SecondsLeft = D.UntrimmedStandingMomentNm > 1.f
			? D.GyroMomentumCapacityNms / D.UntrimmedStandingMomentNm : -1.f;
		const bool bPowerLow = CapacityKJ > KINDA_SMALL_NUMBER && StoredKJ < CapacityKJ * 0.15f;
		if (D.bLiftInverted)
		{
			Status = bPowerLow ? TEXT("POWER LOST   LIFT INVERTED") : TEXT("LIFT VECTOR INVERTED");
			StatusColor = Warn;
		}
		else if (bPowerLow)
		{
			Status = FString::Printf(TEXT("BATTERY LOW   %.0f kJ"), StoredKJ);
			StatusColor = Warn;
		}
		else if (D.bLiftGovernorPinned)
		{
			Status = TEXT("LIFT AUTHORITY LIMITED");
			StatusColor = Warn;
		}
		else if (D.UntrimmedStandingMomentNm > 20.f && SecondsLeft > 0.f)
		{
			Status = FString::Printf(TEXT("UNBALANCED %s   %.0f s"),
				Axis[FMath::Clamp<int32>(D.UntrimmedWorstAxis, 0, 2)], SecondsLeft);
			StatusColor = Warn;
		}
		else if (FMath::Abs(D.StandingSideForceN) > 200.f)
		{
			Status = FString::Printf(TEXT("SIDE BIAS   %.1f kN %s"),
				FMath::Abs(D.StandingSideForceN) / 1000.f,
				D.StandingSideForceN > 0.f ? TEXT("RIGHT") : TEXT("LEFT"));
			StatusColor = Warn;
		}
		else if (D.GyroTorqueNm > 0.f && D.GyroSaturation01 > 0.8f)
		{
			Status = FString::Printf(TEXT("GYRO SATURATION   %s"),
				Axis[FMath::Clamp<int32>(D.GyroWorstAxis, 0, 2)]);
			StatusColor = Warn;
		}
		else if (D.WheelCount > 0 && SmoothedSlip > 0.35f)
		{
			Status = FString::Printf(TEXT("TRACTION SLIP   %.2f"), SmoothedSlip);
			StatusColor = Warn;
		}
		else if (D.bCanFly)
		{
			const TCHAR* Lift = D.bLiftDescending ? TEXT("DESCEND")
				: (D.bLiftGovernorActive ? TEXT("HOVER") : (D.LiftFraction01 > 0.f ? TEXT("ASCEND") : TEXT("IDLE")));
			Status = FString::Printf(TEXT("TWR %.2f   %s"), D.AscentTwr, Lift);
		}
		SetWidgetText(TEXT("VehicleStatusText"), Status, StatusColor);
	}
}

void AExoneerHUD::DrawHUD()
{
	Super::DrawHUD();
	APlayerSurvivalCharacter* Engineer = Cast<APlayerSurvivalCharacter>(GetOwningPawn());
	if (!Canvas || !Engineer || Canvas->SizeX < 8)
	{
		return;
	}
	if (!bOpticsApplied || OpticsOwner.Get() != Engineer)
	{
		ApplySuitOptics();
	}
	EnsureVisorGlass();
	DrawVisorGlass();
}
