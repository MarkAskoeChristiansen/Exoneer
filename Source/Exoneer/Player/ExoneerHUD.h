// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ExoneerTypes.h"
#include "Vehicles/VehicleWheelState.h"
#include "ExoneerHUD.generated.h"

class APlayerSurvivalCharacter;
class UTexture2D;
class UCameraComponent;
class UUserWidget;

/**
 * Exosuit visor: glowing vector instrumentation projected onto curved glass.
 * The optical mask stays nearly helmet-locked while separate hologram depths
 * trail look and locomotion by a few pixels, preserving a stable boresight.
 */
UCLASS()
class EXONEER_API AExoneerHUD : public AHUD
{
	GENERATED_BODY()

public:
	AExoneerHUD();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void DrawHUD() override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	/** Master accessibility/taste control. Zero removes all visor drift. */
	UPROPERTY(EditDefaultsOnly, Category = "Visor|Motion", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float VisorMotionScale = 1.f;

	/** Design pixels of opposite-direction trail per degree/second of camera motion. */
	UPROPERTY(EditDefaultsOnly, Category = "Visor|Motion", meta = (ClampMin = "0.0", ClampMax = "0.25"))
	float LookLagResponse = 0.075f;

	UPROPERTY(EditDefaultsOnly, Category = "Visor|Motion", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float WalkBobPixels = 1.6f;

	UPROPERTY(EditDefaultsOnly, Category = "Visor|Motion", meta = (ClampMin = "0.0", ClampMax = "12.0"))
	float SprintBobPixels = 3.8f;

	UPROPERTY(EditDefaultsOnly, Category = "Visor|Motion", meta = (ClampMin = "1.0", ClampMax = "40.0"))
	float MaxVisorOffsetPixels = 16.f;

	UPROPERTY(EditDefaultsOnly, Category = "Visor|Motion", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float MaxVisorRollDegrees = 0.65f;

	float SmoothedSuitDrain = 0.f;
	float LastSuitPower = -1.f;
	float SmoothedSpeedMS = 0.f;
	float SmoothedSlip = 0.f;
	float SmoothedMotorTempC = NoWindingReadingC;
	float SmoothedLocomotion01 = 0.f;
	float FootstepPhase = 0.f;
	float VisorRollDegrees = 0.f;
	float VisorRollVelocity = 0.f;
	FVector2D VisorOffsetPixels = FVector2D::ZeroVector;
	FVector2D VisorOffsetVelocity = FVector2D::ZeroVector;
	FRotator PreviousViewRotation = FRotator::ZeroRotator;
	bool bHasPreviousViewRotation = false;
	TWeakObjectPtr<APlayerSurvivalCharacter> MotionOwner;
	TWeakObjectPtr<APlayerSurvivalCharacter> OpticsOwner;

	/** Designer-owned visual layer. Defaults to /Game/Exoneer/UI/WBP_VisorHUD. */
	UPROPERTY(EditDefaultsOnly, Category = "Visor|Visual")
	TSubclassOf<UUserWidget> VisorWidgetClass;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> VisorWidget;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> VisorGlass;

	bool bOpticsApplied = false;

	void EnsureVisorGlass();
	void ApplySuitOptics();
	void ApplySuitOpticsToCamera(UCameraComponent* Cam);
	void DrawVisorGlass();
	void UpdateVisorMotion(float DeltaSeconds, APlayerSurvivalCharacter* Engineer);
	void EnsureVisorWidget();
	void UpdateVisorWidget(APlayerSurvivalCharacter* Engineer);
	void SetWidgetText(FName WidgetName, const FString& Text, const FLinearColor& Color);
	void SetWidgetVisible(FName WidgetName, bool bVisible);
	void SetWidgetColor(FName WidgetName, const FLinearColor& Color);
};
