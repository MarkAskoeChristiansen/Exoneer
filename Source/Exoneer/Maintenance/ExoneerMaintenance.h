// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"

/**
 * Pure causal-maintenance math (SI). No UObject. Covered by
 * Tests/ExoneerMaintenanceTests.cpp. Spec: docs/design/maintenance.md.
 */
namespace ExoneerMaintenance
{
	/** New-tire tread (mm) when no spec override is present. */
	inline constexpr float DefaultNewTreadMm = 12.f;

	/**
	 * Tread wear (mm) for FrictionWorkJ joules of frictional work done in the
	 * contact patch: wear = k * (F_shear . v_slip) * dt.
	 *
	 * Rubber abrasion is an ENERGY process - mass leaves the tread in
	 * proportion to the work friction does on it (Archard, Schallamach). The
	 * previous law was k * |s| * W * dt, which carries no velocity at all: a
	 * wheel creeping at one percent slip at half a metre per second wore at
	 * exactly the rate of one at one percent slip at sixteen, and a pure
	 * sideways plow - huge lateral scrub, no longitudinal slip - wore nothing.
	 * Worse, it was calibrated against s ~ 0.4 at 2 kN as an EXCEPTIONAL case,
	 * while the authored clay needs s ~ 0.5 at 3 kN just to cruise, so an
	 * ordinary drive scrapped a new tire in about ninety seconds and left the
	 * rover on the mobilisation floor - grip gone everywhere, permanently.
	 *
	 * k is set against three measured operating points of the 6x6 test rover:
	 * a bogged wheel spinning against the ground at full slip (about 9 kW in
	 * the patch) spends a 12 mm tire in roughly forty minutes; cruising on
	 * soft soil (1-3 kW) in a few hours; rolling on firm ground, where the
	 * patch barely slides at all, in effectively never. Abuse is still
	 * punished, ordinary driving is not.
	 */
	inline float TreadWearMm(float FrictionWorkJ, float KMmPerJoule = 6.0e-7f)
	{
		if (FrictionWorkJ <= 0.f)
		{
			return 0.f;
		}
		return KMmPerJoule * FrictionWorkJ;
	}

	/**
	 * Fraction of new tread where the traction model has nothing left to give.
	 * One number, two readers: the mobilisation floor below, and the terminal
	 * tread reading that makes a spare legal.
	 */
	inline constexpr float TreadMobilisationFloor = 0.05f;

	/** Worn tread mobilises less shear. The floor keeps a bald tire scrap, not a NaN. */
	inline float TreadMobilisationScale(float TreadDepthMm, float NewTreadMm)
	{
		if (NewTreadMm <= KINDA_SMALL_NUMBER || TreadDepthMm < 0.f)
		{
			return 1.f;
		}
		return FMath::Clamp(TreadDepthMm / NewTreadMm, TreadMobilisationFloor, 1.f);
	}

	/**
	 * Terminal tread: the tire has stopped being a tire (maintenance.md 2, "a
	 * wheel with 0 mm tread is scrap rubber"), so the replace verb is legal and
	 * a spare may be spent. At or below the mobilisation floor, because that is
	 * exactly where more wear buys the driver nothing. A part with no tread
	 * reading is never terminal for this reason.
	 */
	inline bool IsTreadTerminal(float TreadDepthMm, float NewTreadMm)
	{
		if (TreadDepthMm < 0.f)
		{
			return false;
		}
		if (NewTreadMm <= KINDA_SMALL_NUMBER)
		{
			return true;
		}
		return TreadDepthMm <= TreadMobilisationFloor * NewTreadMm;
	}

	/** Storm dust on an exposed face. Full storm (~1) reaches opacity 1 in ~50 s. */
	inline float DustOpacityDelta(float StormIntensity, float DtSeconds, float RatePerSec = 0.02f)
	{
		if (DtSeconds <= 0.f || StormIntensity <= 0.f)
		{
			return 0.f;
		}
		return RatePerSec * FMath::Clamp(StormIntensity, 0.f, 1.f) * DtSeconds;
	}

	// --- Structure live load (V-SPAN) -------------------------------------

	/**
	 * Load ratio at or above which a deck fails at once instead of taking a
	 * permanent set: twice its rating. An UNRATED piece (no capacity) and a
	 * condemned deck both read this under any wheel at all.
	 */
	inline constexpr float GrossOverloadRatio = 2.f;

	/** Permanent set (mm/s) a deck takes at twice its rating; linear from the rating up. */
	inline constexpr float DeflectionRateMmPerSec = 6.f;

	/**
	 * Live wheel load over rated capacity. The rating is a MASS (kg), so the
	 * denominator is a WEIGHT on this planet - Capacity * g - and never a 9.81
	 * literal. An unrated piece is judged BEFORE any division: it is not a
	 * deck, so it collapses under any wheel.
	 */
	inline float LoadRatio(float LoadN, float LoadCapacityKg, float GravityMps2)
	{
		if (LoadCapacityKg <= 0.f)
		{
			return GrossOverloadRatio;
		}
		const float CapacityN = LoadCapacityKg * FMath::Max(GravityMps2, 0.f);
		if (CapacityN <= KINDA_SMALL_NUMBER)
		{
			return 0.f;   // a weightless world puts no load on anything
		}
		return FMath::Max(LoadN, 0.f) / CapacityN;
	}

	/**
	 * Permanent set (mm) added this step. ONE WAY: at or below the rating a
	 * deck takes nothing, and nothing ever gives back what it took - the verb
	 * for a sagged deck is rebuild, not repair. At 1.5x rating the 60 mm
	 * terminal reading arrives in 20 s.
	 */
	inline float DeflectionDeltaMm(float Ratio, float DtSeconds)
	{
		if (DtSeconds <= 0.f || Ratio <= 1.f)
		{
			return 0.f;
		}
		return DeflectionRateMmPerSec * (FMath::Min(Ratio, GrossOverloadRatio) - 1.f) * DtSeconds;
	}

	// --- Battery capacity fade -------------------------------------------

	/**
	 * Fade a pack can never exceed: 40 percent of its rated joules always
	 * remain, so a neglected battery is a shrinking pack the crew works
	 * around, never a brick the world quietly deletes. This same number is
	 * the terminal reading that makes a spare cell legal.
	 */
	inline constexpr float CapacityFadeFloor = 0.6f;

	/**
	 * Rate constant, in RatedJ of throughput: fade 1.0 would arrive after
	 * CapacityFadeCycles * RatedJ joules had passed through the pack. The
	 * floor lands first, at 0.6 * 400 = 240 RatedJ of throughput, which is
	 * 120 full charge-and-discharge cycles at nominal temperature.
	 */
	inline constexpr float CapacityFadeCycles = 400.f;

	/** Above this winding-side ambient (C) the cells age faster. */
	inline constexpr float CapacityFadeTempOnsetC = 35.f;

	/** Extra fade per degree over the onset: +5 percent of the base rate per C. */
	inline constexpr float CapacityFadeTempCoefficient = 0.05f;

	/**
	 * How far fade must move before the reading is republished. Fade advances
	 * by a millionth per sim step on a healthy pack, so writers bank it and
	 * spend it in visible steps (maintenance.md 6). One number, two writers:
	 * the vehicle ledger and the structure power network.
	 */
	inline constexpr float CapacityFadeDeadband = 0.005f;

	/**
	 * Capacity fade earned by cycling ThroughputJ joules through a pack rated
	 * RatedJ, at TempC.
	 *
	 *   dFade = ThroughputJ / (CapacityFadeCycles * RatedJ)
	 *           * (1 + 0.05 * max(0, TempC - 35))
	 *
	 * Throughput is |dStored| summed over BOTH directions, so one full charge
	 * plus the discharge after it is 2 * RatedJ. Hot cells age faster: at
	 * 55 C the rate is double. The caller adds this to the reading and clamps
	 * it with ApplyCapacityFade below.
	 */
	inline float CapacityFadeDelta(float ThroughputJ, float RatedJ, float TempC)
	{
		if (ThroughputJ <= 0.f || RatedJ <= KINDA_SMALL_NUMBER)
		{
			return 0.f;
		}
		const float TempScale = 1.f + CapacityFadeTempCoefficient * FMath::Max(0.f, TempC - CapacityFadeTempOnsetC);
		return ThroughputJ / (CapacityFadeCycles * RatedJ) * TempScale;
	}

	/** The reading after one step, held at the floor. One way: a pack never un-fades. */
	inline float ApplyCapacityFade(float Fade01, float DeltaFade)
	{
		return FMath::Clamp(Fade01 + FMath::Max(DeltaFade, 0.f), 0.f, CapacityFadeFloor);
	}

	/** Joules a rated pack can actually hold at this fade. */
	inline float EffectiveCapacityJ(float RatedJ, float Fade01)
	{
		return FMath::Max(RatedJ, 0.f) * (1.f - FMath::Clamp(Fade01, 0.f, CapacityFadeFloor));
	}

	/**
	 * Terminal battery: the pack is at the floor, so further use buys the
	 * crew nothing and the replace verb (a fabricated cell) is legal. Same
	 * shape as IsTreadTerminal - the reading decides, not a stored flag.
	 */
	inline bool IsCapacityTerminal(float Fade01)
	{
		return Fade01 >= CapacityFadeFloor - KINDA_SMALL_NUMBER;
	}

	/**
	 * Heat into the winding (W). LOSSES ONLY - shaft work leaves as motion and
	 * must never heat the motor. Three terms:
	 *   copper (I^2 R), quadratic in the torque fraction T/T_s;
	 *   friction and iron, the efficiency shortfall on shaft power;
	 *   the controller's constant electronics draw.
	 * The torque fraction is derate invariant, so a stalled motor keeps
	 * burning its full CopperLossAtStallW however far the thermal derate has
	 * pulled its torque capacity down - a bogged wheel cannot cool itself by
	 * being derated.
	 */
	inline float MotorLossPowerW(float DriveTorqueNm, float OmegaRadS, float StallTorqueNm,
		float Efficiency, float CopperLossAtStallW, float ControllerIdleDrawW)
	{
		const float IdleW = FMath::Max(ControllerIdleDrawW, 0.f);
		if (StallTorqueNm <= KINDA_SMALL_NUMBER)
		{
			return IdleW;   // freewheeling (undriven, or cut out): no torque, no loss
		}
		const float TorqueFraction = DriveTorqueNm / StallTorqueNm;
		const float CopperW = FMath::Max(CopperLossAtStallW, 0.f) * TorqueFraction * TorqueFraction;
		const float FrictionW = (1.f - FMath::Clamp(Efficiency, 0.1f, 1.f)) * FMath::Abs(DriveTorqueNm * OmegaRadS);
		return CopperW + FrictionW + IdleW;
	}

	/**
	 * One step of the lumped winding thermal model (C):
	 *   T' = T + (P_loss - k * (T - T_ambient)) * dt / C_th
	 * with k = CoolingWPerC (Newton cooling to ambient) and C_th the winding
	 * plus housing heat capacity. The equilibrium is T_ambient + P_loss / k,
	 * which is what the authored constants are chosen against. Unclamped: the
	 * caller bounds it against ambient and the trip temperature.
	 */
	inline float WindingTempStep(float TempC, float LossW, float AmbientC,
		float CoolingWPerC, float ThermalMassJPerC, float DtSeconds)
	{
		if (DtSeconds <= 0.f || ThermalMassJPerC <= KINDA_SMALL_NUMBER)
		{
			return TempC;
		}
		const float NetW = FMath::Max(LossW, 0.f) - FMath::Max(CoolingWPerC, 0.f) * (TempC - AmbientC);
		return TempC + NetW * DtSeconds / ThermalMassJPerC;
	}

	/** Torque capacity a hot winding can still make: 1 below onset, linear to this floor at trip. */
	inline constexpr float ThermalDerateFloor = 0.35f;

	/**
	 * Winding derate. Never reaches 0 - the cutout owns zero torque, so a
	 * derated motor still crawls and the driver feels the fade before the
	 * trip instead of a silent stall.
	 */
	inline float ThermalDerateScale(float TempC, float OnsetC, float TripC)
	{
		if (TempC <= OnsetC)
		{
			return 1.f;
		}
		if (TripC <= OnsetC + KINDA_SMALL_NUMBER)
		{
			return ThermalDerateFloor;
		}
		const float Fraction = FMath::Clamp((TempC - OnsetC) / (TripC - OnsetC), 0.f, 1.f);
		return FMath::Lerp(1.f, ThermalDerateFloor, Fraction);
	}

	/**
	 * Over-temp cutout latch with hysteresis: trips at TripC, holds until the
	 * winding falls back to ClearC. Hysteresis is why this one terminal state
	 * is stored on FPartCondition instead of derived from the reading.
	 */
	inline bool ThermalCutoutLatch(bool bWasCutout, float TempC, float TripC, float ClearC)
	{
		if (TempC >= TripC)
		{
			return true;
		}
		if (TempC <= ClearC)
		{
			return false;
		}
		return bWasCutout;
	}

	// --- Suit seal leak ---------------------------------------------------

	/** Visor emergency line (L/s): leak is hidden below this. */
	inline constexpr float SealEmergencyLps = 0.05f;

	/** Leak (L/s) after a patch, times (1 + PatchCount before the press). */
	inline constexpr float SealPatchBaseLps = 0.005f;

	/** Kits are refused at this count; only a fabricated spare resets the seal. */
	inline constexpr uint8 SealPatchCap = 3;

	/** Landing impact (m/s along the normal) below which the seal takes nothing. */
	inline constexpr float SealLandingSpeedMps = 8.f;

	/** Extra leak (L/s) per m/s of impact above the landing threshold. */
	inline constexpr float SealLandingLeakPerMps = 0.004f;

	/**
	 * Port makeup rate (L/s). One number, two readers: the umbilical port's
	 * default O2 transfer and the visor "LEAK EXCEEDS MAKEUP" line.
	 */
	inline constexpr float UmbilicalO2MakeupLps = 2.f;

	/** Port power draw / suit charge (W) while a suit is taking energy. */
	inline constexpr float UmbilicalPortPowerW = 600.f;

	/** Cockpit suit trickle (W). Charged by the construct's supply fraction. */
	inline constexpr float CockpitSuitChargeW = 300.f;

	/** Suit idle drain (W) that keeps time-to-empty identical to the old 100/0.03 units. */
	inline constexpr float SuitIdleDrainW = 540.f;

	/** Suit energy bank (kJ). */
	inline constexpr float SuitPowerCapacityKJ = 1800.f;

	/** Suit O2 bank (L). */
	inline constexpr float SuitO2CapacityL = 100.f;

	/** Metabolic O2 drain (L/s). */
	inline constexpr float MetabolicO2Lps = 0.05f;

	/**
	 * Storm leak growth (L/s) at this intensity and patch count:
	 *   0.0002 * Intensity * (1 + 0.5 * PatchCount)
	 * A fresh seal reaches the emergency line after about 250 s of full
	 * exposure; a thrice-patched one after about 100 s.
	 */
	inline float SealLeakGrowthLps(float StormIntensity, uint8 PatchCount)
	{
		const float Intensity = FMath::Clamp(StormIntensity, 0.f, 1.f);
		if (Intensity <= 0.f)
		{
			return 0.f;
		}
		return 0.0002f * Intensity * (1.f + 0.5f * float(PatchCount));
	}

	/** Leak after a successful patch, from the count BEFORE the increment. */
	inline float SealLeakAfterPatch(uint8 PatchCountBefore)
	{
		return SealPatchBaseLps * (1.f + float(PatchCountBefore));
	}

	/** Extra leak from a landing impact. Zero at or below the threshold. */
	inline float SealLandingLeakDeltaLps(float ImpactSpeedMps)
	{
		if (ImpactSpeedMps <= SealLandingSpeedMps)
		{
			return 0.f;
		}
		return SealLandingLeakPerMps * (ImpactSpeedMps - SealLandingSpeedMps);
	}
}
