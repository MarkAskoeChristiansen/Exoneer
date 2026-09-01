# Exoneer wheel terramechanics – math specification v1

Scope reference: docs/GAME-SCOPE.md section 4.1 (the three core formulas are reproduced and extended here, per Wong, *Theory of Ground Vehicles*; Bekker, *Theory of Land Locomotion* / *Off-the-Road Locomotion*; Janosi & Hanamoto 1961). Everything below is SI internally. Conversion to UE units happens exactly once, at the API edge (section 9). Every formula is written so a C++ engineer can implement it symbol by symbol; every guard is stated with its numeric constant.

---

## 1. Conventions and the complete symbol table

Internal units: meter (m), kilogram (kg), second (s), Newton (N), Pascal (Pa), radian (rad), Watt (W). Angles authored in degrees are converted to radians at asset-load time, once. Gravity g = 9.8 m/s² (from `DefaultGravityZ = -980` cm/s²; derive it, do not hardcode a second copy).

Contact frame per wheel, rebuilt per substep:

- `n_g` – cached ground plane unit normal (world).
- `h` – wheel heading: block local forward rotated by steer angle `beta` about block local up, transformed to world.
- `x_hat = normalize(h - (h . n_g) n_g)` – longitudinal, in the ground plane.
- `y_hat = n_g × x_hat` – lateral.

### Authored wheel parameters (per wheel block definition, SI)

| Symbol | Unit | Meaning | Starter value |
|---|---|---|---|
| `r_w` | m | undeformed wheel radius | 0.30 |
| `b` | m | wheel width | 0.20 |
| `m_w` | kg | wheel + tire mass (the block Def Mass) | 40 |
| `I_w` | kg·m² | spin inertia = `0.6 · m_w · r_w²` (between solid cylinder 0.5 and hoop 1.0; tire mass sits at the rim) | 2.16 |
| `k_s` | N/m | suspension spring rate | 40 000 |
| `c_s` | N·s/m | suspension damper | 3 162 (zeta = 0.5, see 6.2) |
| `L0` | m | suspension rest length (mount to wheel center) | 0.35 |
| `x_t` | m | suspension travel (max compression) | 0.20 |
| `k_bump` | N/m | bump-stop spring, engaged past `x_t` | 400 000 |
| `p_i` | Pa | tire inflation pressure (live, CTIS lever) | 80 000 |
| `p_c` | Pa | carcass stiffness pressure (constant per tire) | 15 000 |
| `T_s` | N·m | hub motor stall torque | 250 |
| `omega_0` | rad/s | hub motor no-load speed | 40 |
| `eta` | – | motor drivetrain efficiency | 0.85 |
| `P_cu0` | W | copper loss at full stall torque | 2 000 |
| `T_bmax` | N·m | max brake torque | 400 |
| `beta_max` | rad | max steer angle | 0.6 (≈ 34°) |
| `beta_rate` | rad/s | steer slew rate | 1.5 |
| `f_r0` | – | internal rolling-resistance coefficient | 0.015 flexible / 0.008 rigid |

### Soil parameters (per substrate, SI at runtime; authored in the published kN units and multiplied by 1000 on load)

| Symbol | Unit (runtime) | Meaning |
|---|---|---|
| `k_c` | N/m^(n+1) | cohesive modulus of sinkage |
| `k_phi` | N/m^(n+2) | frictional modulus of sinkage |
| `n0` | – | sinkage exponent (zero slip) |
| `n1` | – | slip-sinkage coefficient (section 4.4) |
| `c` | Pa | cohesion |
| `phi` | rad | internal friction angle |
| `K` | m | longitudinal shear deformation modulus (Janosi) |
| `K_y` | m | lateral shear deformation modulus (default `1.2 K`) |
| `gamma_s` | N/m³ | soil unit weight (feeds bulldozing only) |

Derived per contact: `k_eq = k_c / b + k_phi` (units N/m^(n+2); note Bekker's known dimensional quirk – the units of `k_eq z^n` are Pa only because `z` is in meters; we always evaluate `z` in meters, so this is safe).

### Per-wheel state (continuous, server side; NOT in `FVehicleBlockRecord` – see agent2 blocker 3)

`omega` (rad/s, spin), `beta` (rad, steer), `z` (m, sinkage, previous substep), `C_prev` (m, compression, previous substep), `u_i` (m, active-suspension preload offset), plus per-frame cached ground plane and soil pointer.

---

## 2. Authored soil table (Wong's standard values)

From Wong, *Theory of Ground Vehicles*, terrain-value tables (Bekker/LLL measurements). These are the four v1 substrates as authored data; `n1`, `K_y`, and snow `K` are engineering choices flagged as tunable.

| Substrate | n0 | k_c [kN/m^(n+1)] | k_phi [kN/m^(n+2)] | c [kPa] | phi [deg] | K [m] | K_y [m] | gamma_s [kN/m³] | n1 |
|---|---|---|---|---|---|---|---|---|---|
| Dry sand (LLL) | 1.10 | 0.99 | 1528.43 | 1.04 | 28.0 | 0.025 | 0.030 | 15.7 | 0.9 |
| Sandy loam (LLL) | 0.70 | 5.27 | 1515.04 | 1.72 | 29.0 | 0.025 | 0.030 | 15.2 | 0.6 |
| Clayey soil (Thailand) | 0.50 | 13.19 | 692.15 | 4.14 | 13.0 | 0.010 | 0.012 | 16.8 | 0.4 |
| Snow (U.S.) | 1.60 | 4.37 | 196.72 | 1.03 | 19.7 | 0.040 | 0.048 | 2.0 | 1.0 |

Load-time conversion: `k_c`, `k_phi` × 1000 (kN → N); `c` × 1000 (kPa → Pa); `phi` deg → rad; `gamma_s` × 1000.

v1 substrate lookup: one soil per biome (add these nine fields to `UPlanetBiomeDataAsset`, resolved through the existing cached environment-manager accessor). The wheel query interface takes a world position so the later move to `UPhysicalMaterial`-resolved soils changes only the lookup, never the math.

---

## 3. Normal load W – suspension spring-damper

W never comes from a static mass split. It is the strut force, so load transfer under acceleration, braking, cornering, cargo placement, and CoM shifts emerges from rigid-body dynamics for free (agent2 obstacle 8 confirms only total mass is queryable, which is exactly what this model wants).

Per frame (game thread): raycast from the mount point `M` (block world transform) along suspension axis `a` (block local −Z, world), length `L_ray = L0 + r_w + z_margin` with `z_margin = 0.5 r_w`. Cache: hit flag, plane point `P_g`, normal `n_g`, soil pointer. Channel: `ECC_WorldStatic` (never `ECC_Visibility` – ghost boxes block it, agent0), ignore the own construct.

Per substep (physics thread, against the cached plane):

1. Distance from current mount point to the plane along `a`: `d = ((P_g - M) . n_g) / (a . n_g)`, guarded `a . n_g < -0.2` (wheel pointing sufficiently downward; otherwise treat as airborne).
2. Free length to the rigid-contact position: `d_free = d - (r_w - z_prev)` where `z_prev` is last substep's sinkage (rigid regime) or `d - (r_w - delta_prev)` (flexible). The one-substep lag on `z` breaks the geometric circularity; it is stable because `z` moves on suspension timescales (see 6.4).
3. Compression `C = clamp(L0 - u_i - d_free, 0, x_t + 0.05)`. If `C = 0` (or no hit): airborne wheel, `W = 0`, skip sections 4–7 soil math, still integrate `omega` (section 4.8) with drive and brake only.
4. Compression rate: `C_dot = -(v_M . a_eff)` where `v_M` is the body point velocity at `M` and `a_eff = a` projected sign-consistently; equivalently `C_dot = (C - C_prev)/dt_sub` – use the analytic point velocity, not the finite difference (finite difference injects one substep of noise into the damper).
5. Strut force, no-pull clamp:
   `F_s = max(0, k_s · C + c_s · C_dot)`; if `C > x_t`: add `k_bump · (C - x_t)`.
6. Normal load on soil: `W = F_s · (−a . n_g)` (equals `F_s` on flat ground with vertical strut). Clamp `W >= 0`.

`W` is applied to the body as `+W · n_g` at the contact point `P_c = C_w - n_g · r_stat`, where `C_w` is the wheel center and `r_stat = r_w - z` (rigid) or `r_w - delta` (flexible), clamped `>= 0.5 r_w`. Applying at the actual wheel-soil interface point gives correct rollover and pitch moments. Anti-dive strut geometry is explicitly out of v1.

---

## 4. Soil contact model

### 4.1 Rigid vs pneumatic regime (Wong's criterion)

A pneumatic tire on soft ground behaves as a rigid wheel when its combined carcass + inflation pressure exceeds the critical ground pressure the terrain can exert. Wong's criterion, with `D = 2 r_w`:

```
p_gcr = (k_eq)^(1/(2n+1)) · ( 3W / ((3 - n) · b · sqrt(D)) )^(2n/(2n+1))
```

- If `p_i + p_c >= p_gcr` → **rigid regime** (section 4.2/4.3 implicit solve). `delta = 0`, `r_eff = r_w`, `f_r = f_r0_rigid`.
- Else → **flexible regime**: the tire flattens until its contact pressure equals what it can carry, so ground pressure is simply
  `p = p_i + p_c` (this is where CTIS acts), and the scope's sinkage formula applies **explicitly, no iteration**:
  `z = (p / k_eq)^(1/n)`.
  Contact area `A = W / p`, patch length `l = A / b` (clamped `l <= 1.4 r_w`), deflection from the flattened-chord geometry:
  `delta = r_w - sqrt(r_w² - (l/2)²)` (clamped `[0, 0.35 r_w]`) – this drives the visual bulge and
  `r_eff = r_w - delta/3` (standard pneumatic effective-radius approximation). `f_r = f_r0_flexible`.

Evaluate the regime check once per substep with the current `W`. Hysteresis guard: switch regimes only when the comparison differs by more than 2 percent, to prevent flip-flapping at the boundary.

Note the emergent behavior this buys: on hard ground `k_eq` is huge, `p_gcr` is huge, so every tire is in the flexible regime with a finite patch – the model degrades gracefully to a normal tire on pavement instead of `l → 0` killing traction.

### 4.2 Contact geometry, rigid regime – chord, not arc

Wheel center sits `r_w - z` above the undeformed surface. The wheel circle intersects the surface at horizontal distance `sqrt(z (2 r_w - z))` from bottom dead center. We use the **chord (horizontal projection)**:

```
l(z) = sqrt( z · (2 r_w - z) )        [m]
A    = b · l                          [m²]
p    = W / A                          [Pa]
```

Why chord: Bekker's pressure-sinkage relation is a *plate* penetration law; the load-bearing analogue for a wheel is the projected bearing area under vertical load, which is the chord. The arc `r_w · acos(1 - z/r_w)` overestimates effective bearing length increasingly with `z` (by 15 percent at `z = 0.2 r_w`, by 57 percent at `z = r_w`), which would understate sinkage exactly in the deep-mud cases the game cares about. The chord also reduces to the standard small-sinkage form `l ≈ sqrt(2 r_w z)` used throughout Wong, and is exact and continuous up to `z = r_w`.

### 4.3 Pressure-sinkage solve, rigid regime (implicit)

Bekker: `p = k_eq · z^n`. But `p = W / (b · l(z))` and `l` depends on `z`, so `z` is implicit. Substituting:

```
g(z) = b · k_eq · z^n · sqrt(z (2 r_w - z)) - W = 0
```

**Numerical solve: Newton, closed-form seed, analytic derivative.**

Seed (small-z closed form, `l ≈ sqrt(2 r_w z)`):

```
z_seed = ( W / (b · k_eq · sqrt(2 r_w)) )^( 1 / (n + 0.5) )
```

Derivative:

```
g'(z) = b · k_eq · [ n · z^(n-1) · sqrt(z(2r_w - z))  +  z^n · (r_w - z) / sqrt(z(2r_w - z)) ]
```

Iteration: `z <- clamp(z - g(z)/g'(z), 1e-6, 0.95 r_w)`. Guard `g'(z) >= 1e-3` (cannot occur inside the bounds with valid soil data, but guard anyway). Terminate on `|dz| < 1e-5 m` or 8 iterations, whichever first. From the seed, 2–3 iterations reach `|g| < 1 N` in practice (test vectors 1, 2, 9 demonstrate this). Cost: ~3 `pow` calls per wheel per substep – negligible.

### 4.4 Slip sinkage

The scope's design consequence "raw throttle drives s toward 1.0 and you dig in" requires sinkage to grow with slip; the static Bekker equation alone does not produce it. Use the published exponent form (Ding et al., *Journal of Terramechanics* 2011): the sinkage exponent becomes slip-dependent,

```
n_eff = n0 + n1 · |s|
```

with `s` from the **previous substep** (breaks the z–s circularity; both lag one substep, both are stable). `n_eff` replaces `n` in the regime criterion, the sinkage solve, and the compaction resistance. This one line creates the dig-in death spiral: spinning wheels sink deeper (test vector 9: sand sinkage 0.0675 m at s = 0 grows to 0.163 m at s = 0.8, and total resistance grows 52 percent).

---

## 5. Slip, shear forces, and resistances

### 5.1 Slip ratio and slip angle – regularized, no NaN, no sign flapping

Contact-point velocity of the body at `P_c`: `v_c`; components `v_x = v_c . x_hat`, `v_y = v_c . y_hat`. Wheel surface speed `v_w = omega · r_eff`.

**Slip ratio** (single formula for driving and braking):

```
s = (v_w - v_x) / max( |v_w|, |v_x|, v_eps ),   v_eps = 0.1 m/s,   clamp s to [-1, 1]
```

Properties: at `v_x = v_w = 0`, `s = 0` exactly (0/0.1) – no NaN, no force at rest. For `|v| >> v_eps` it equals the standard SAE definition in both driving (`denominator = |v_w|`) and braking (`= |v_x|`) branches, continuously blended. Near rest the denominator floor makes `s` proportional to slip *velocity*, which converts the force law into a viscous-like law at rest – this is precisely the regularization that prevents sign flapping (see 6.5).

**Slip angle**:

```
alpha = atan( v_y / max(|v_x|, v_alpha_eps) ),   v_alpha_eps = 0.3 m/s
```

The larger floor keeps lateral forces from jittering while parked or creeping.

### 5.2 Janosi-Hanamoto shear, combined slip by resultant shear displacement

Shear stress builds along the patch as soil deforms: `tau(j) = tau_max · (1 - e^(-j/K))`, `tau_max = c + sigma tan(phi)`. Integrating `j(x) = s·x` over a uniform-pressure patch of length `l` gives exactly the scope's F formula. Define the saturation function with its small-argument guard:

```
E(u) = 1 - (1 - e^(-u)) / u        for u >= 1e-3
E(u) = u/2 - u²/6                  for u <  1e-3     (series; removes 0/0)
```

`E` is monotonic, `E(0) = 0`, `E(inf) = 1`.

**Shear budget** (the scope's prefactor): `F_bud = A · c + W · tan(phi)` [N].

**Combined slip – resultant shear displacement method** (Wong; used for planetary rovers by Ishigami et al.). Longitudinal and lateral shear are not independent: they share one soil shear budget. Build the slip-displacement vector with per-axis moduli:

```
u_x = |s| · l / K
u_y = |tan(alpha)| · l / K_y
u_r = sqrt(u_x² + u_y²)
```

If `u_r < 1e-6`: `F_x = F_y = 0`. Else:

```
F_r = F_bud · E(u_r)
F_x = F_r · (u_x / u_r) · sign(v_w - v_x)      (drives the wheel forward when s > 0)
F_y = F_r · (u_y / u_r) · (-sign(v_y))         (opposes lateral sliding)
```

By construction `sqrt(F_x² + F_y²) = F_r <= F_bud` – the friction budget is shared exactly (this is the friction-ellipse property, obtained from the physics rather than imposed). Understeer on soft soil emerges: steering demand (`u_y`) rotates the resultant away from longitudinal, and on low-`phi` soil `F_bud` is small, so the front wheels wash out realistically. Chosen over an imposed ellipse because it is the published terramechanics form, needs no extra tuning constants, and is continuous everywhere.

Drawbar pull (reported to HUD/dashboard, and the number the design consequences in the scope reason about): `DP = F_x - (R_c + R_b)` per wheel, summed over wheels.

### 5.3 Motion resistance

Three terms; the first two are external soil forces on the chassis, the third is an internal torque on the wheel spin DOF. Do not double-apply.

**Compaction** (scope formula, per wheel, uses `n_eff`):

```
R_c = b · k_eq · z^(n_eff + 1) / (n_eff + 1)      [N]
```

**Bulldozing** – v1 INCLUDES it. Recommendation and reason: deep sinkage is the core soft-terrain fantasy; without `R_b` the resistance at `z > 0.15 r_w` is far too forgiving and the CTIS/ballast gameplay loses its payoff. Use the Rankine passive-earth-pressure form (standard soil mechanics; the simplification of the Bekker/Hegedus bulldozing model that needs no bearing-capacity lookup tables):

```
K_p = tan²(pi/4 + phi/2)
R_b = 0.5 · gamma_s · z² · b · K_p  +  2 · c · z · b · sqrt(K_p)      [N]
```

Both terms vanish continuously as `z → 0`; no gate needed.

Application: `R_c + R_b` oppose the wheel's longitudinal ground velocity, with a rest taper to kill parked creep:

```
F_res = -(R_c + R_b) · clamp(v_x / v_eps, -1, 1) · x_hat
```

**Internal rolling resistance** (tire hysteresis + bearing): torque on the spin DOF only,

```
T_rr = f_r · W · r_eff        opposing omega, with the no-reversal rule of 5.4
```

### 5.4 Wheel rotational dynamics – where digging-in becomes real

Spin DOF per wheel (the wheel is not a separate rigid body; only its rotation is integrated, by us):

```
I_w · domega/dt = T_drive - T_brake - F_x · r_eff - T_rr
```

`F_x · r_eff` is the ground reaction torque: soil shear both propels the chassis and loads the motor. Stall in mud is then automatic: high `R_c + R_b` holds `v_x` near 0, torque spins the wheel up, `s → 1`, `n_eff` rises, `z` rises, the hole deepens.

Integration, per substep, **semi-implicit with two rules**:

1. Explicit torques (`T_drive`, `-F_x r_eff`) integrate normally: `omega += dt_sub · T_net_explicit / I_w`.
2. Dissipative torques (`T_brake`, `T_rr`) obey the **no-reversal rule**: compute the omega change they would cause; if it would cross zero, set `omega = 0` for that contribution instead of overshooting. This kills brake-torque limit cycles at rest.
3. **Slip-overshoot clamp**: after integration, limit the change of surface speed per substep:
   `|delta_omega · r_eff| <= max( 2 · |v_w - v_x|, 0.5 m/s )`.
   This prevents the explicit `F_x(omega)` coupling from oscillating across `s = 0` when `I_w` is small and the shear curve is steep (see 6.5).

`r_eff`: `r_w` in rigid regime (the shear interface is the rim; sinkage tilts the resultant, which we already carry as the separate horizontal `R_c + R_b`, so shrinking the lever arm too would double-count), `r_w - delta/3` in flexible regime.

### 5.5 Electric hub motor (scope module 4) and the power ledger

Linear torque-speed curve from stall torque and no-load speed (ideal PMDC at fixed voltage – the correct first-order physics for a hub motor, no gearbox in v1):

```
T_avail(omega, thr) = |thr| · T_s · clamp( 1 - (omega · sign(thr)) / omega_0, 0, 1 ) · PowerSupplyFraction
T_drive = T_avail · sign(thr) · G(s)          (G = slip governor, section 7.2; G = 1 without the talent)
```

The clamp's lower bound 0 caps plugging (torque applied against rotation) at `T_s` – linear extrapolation past `omega_0` and below 0 is non-physical.

Electrical draw – mechanical power over efficiency plus copper loss (current is proportional to torque, so resistive loss is quadratic in torque; this is what makes stalling in mud drain the batteries, which is the intended gameplay):

```
P_elec = max(T_drive · omega, 0) / eta  +  P_cu0 · (T_drive / T_s)²      [W]
```

At stall: `P_elec = P_cu0` at full torque, zero motion, pure heat. Ledger integration: the module accumulates `P_elec · dt_sub` over the frame's substeps and reports the frame-average through `GetCurrentDraw()`; the one-frame demand lag noted in agent2 section 3 is accepted (it is the same lag thrusters already have). Torque scaling by `PowerSupplyFraction` above closes the loop when batteries brown out.

**Regenerative braking: NO for v1.** Reasons: (a) it requires a signed-draw path through the ledger's deficit/surplus branches, which are currently one-directional per frame; (b) brake blending between regen and friction is a UX problem with no dashboard yet to express it; (c) at rover speeds and masses the recovered energy is small against solar/battery scale. The seam stays open: when added, regen is only `P_elec < 0` routed into the existing surplus path, no model change.

---

## 6. Per-tick algorithm and numerical stability

### 6.1 Exact order of operations

**Game thread, once per frame** (server, `TG_PrePhysics`, from the wheel module tick):

1. Read pilot intent → target steer `beta_t`, throttle `thr`, brake.
2. Slew steer: `beta += clamp(beta_t - beta, -beta_rate·dt, +beta_rate·dt)`.
3. Per wheel: raycast (section 3), soil lookup, refresh the cached contact struct `{bHit, P_g, n_g, soil*, beta, thr, brake, talentFlags}`.
4. Register `FBodyInstance::AddCustomPhysics` on the construct's `PhysicsRoot` (one delegate for all wheels, re-registered every frame as the engine requires).
5. Publish last frame's per-wheel outputs `{z, s, W, omega, delta, P_elec}` from the double-buffered snapshot to: power ledger draw, replicated side list (quantized, low rate), wheel visual transforms.

**Physics thread, per substep** (the delegate; scene queries forbidden here, everything reads the cached plane):

Per wheel, in this order – each step consumes only values already computed this substep or explicitly lagged one substep (`z_prev`, `s_prev`):

1. Wheel center, mount point, and point velocities from the current body state.
2. Suspension: `C`, `C_dot`, `F_s`, `W` (section 3). Airborne → step 8 with soil terms zero.
3. Regime check with `n_eff = n0 + n1·|s_prev|` (section 4.1); solve `z` (Newton, rigid) or explicit (flexible); `l`, `A`, `p`, `delta`, `r_eff`.
4. Contact frame `x_hat, y_hat`; `v_x, v_y`; slip `s`, `alpha` (section 5.1).
5. Shear: `u_x, u_y, u_r, E, F_bud, F_x, F_y` (section 5.2).
6. Resistances `R_c, R_b, T_rr` (section 5.3).
7. Motor torque with governor, brake torque (sections 5.5, 7.2).
8. Integrate `omega` with the three rules of 5.4.
9. Apply to body at `P_c`, converted N → UE at this line only:
   `F_world = W·n_g + (F_x + F_res_x)·x_hat + F_y·y_hat`.
10. Write `z, s, C` into the wheel state (becomes `_prev` next substep); accumulate `P_elec·dt_sub`; write the game-thread snapshot buffer.

### 6.2 What is cached per frame vs recomputed per substep, and why the split is sound

Cached per frame: ground plane, soil parameters, steer target, pilot intent, talent flags. These change on gameplay timescales; at 20 m/s and 60 fps the vehicle moves 0.33 m per frame, and the plane approximation error over that distance is zero on the current flat slab and second-order on future gentle heightfields (flag: revisit the cache when terrain gets curvature sharper than ~1/5 m⁻¹).

Recomputed per substep: everything driven by the two stiff subsystems – the suspension spring-damper (natural frequency ~2 Hz but damper stiffness demands substep evaluation, see 6.3) and the slip dynamics (`I_w` is small: time constant `I_w / (dF_x/ds / r_eff²·...)` can be a few milliseconds in high-grip conditions). Evaluating these once per variable-length frame is exactly the instability agent2 obstacle 1 predicts; per-substep evaluation at fixed `dt_sub <= 1/60` makes behavior frame-rate independent.

Config fix required: `Config/DefaultEngine.ini` must replace the dead key `PhysicSubstepDeltaTime` with `MaxSubstepDeltaTime=0.016667` (agent1/agent0 finding), keeping `bSubstepping=True`, `MaxSubsteps=4`.

### 6.3 Spring-damper stability bound

With semi-implicit (symplectic) Euler at step `dt`, sprung mass share `m_sh = M_total / N_wheels_in_contact`:

- Spring: require `omega_n · dt <= 0.5` with margin, `omega_n = sqrt(k_s / m_sh)` → **`k_s <= 0.25 · m_sh / dt²`**.
- Damper: explicit damping is unstable past `c_s > 2 m_sh / dt`; require **`c_s <= m_sh / dt`**.
- Author via damping ratio: `c_s = 2 · zeta · sqrt(k_s · m_sh)`, `zeta` in 0.4–0.7.

Numbers at `dt = 1/60`, 1000 kg rover, 4 wheels (`m_sh = 250 kg`): bound `k_s <= 225 000 N/m`, chosen 40 000 (`omega_n = 12.65 rad/s`, `omega_n·dt = 0.21`, comfortable); `c_crit = 6 325 N·s/m`, chosen `c_s = 3 162` (`zeta = 0.5`), bound 15 000. Validate authored wheel specs against these bounds at asset load and log a warning – this converts a physics explosion into a content error.

Related requirement: set `PhysicsRoot` `LinearDamping` and `AngularDamping` to ~0 for constructs with wheels (agent2 obstacle 2) – the real model now owns all motion resistance; the old damping would silently add a second, non-physical drag.

### 6.4 Clamp list and the failure mode each prevents

| Clamp / guard | Value | Prevents |
|---|---|---|
| `s` clamp | [-1, 1] | budget overshoot from velocity spikes (landing impacts) |
| slip denominator floor | `v_eps = 0.1 m/s` | NaN and force chatter at rest |
| alpha denominator floor | `v_alpha_eps = 0.3 m/s` | parked lateral jitter |
| `E(u)` series branch | `u < 1e-3` | 0/0 in the Janosi term at `s → 0` |
| `z` bounds | `[1e-6, 0.95 r_w]` | Newton divergence; wheel-center burial |
| Newton exit | `|dz| < 1e-5 m` or 8 iters | unbounded solver cost |
| `l` floor | `0.01 r_w` | division by zero in `u_x` on near-rigid contacts |
| `C` clamp + bump stop | `[0, x_t + 0.05]` | strut inversion; unbounded spring force |
| `F_s >= 0` | – | suspension pulling the chassis down (tires do not pull) |
| resistance rest taper | `clamp(v_x/v_eps, -1, 1)` | parked vehicles creeping backward from `R_c` |
| no-reversal rule | 5.4 | brake/rolling-resistance limit cycles at `omega ≈ 0` |
| slip-overshoot clamp | `max(2·|v_w - v_x|, 0.5)` per substep | `omega` oscillation across `s = 0` (small `I_w`, steep shear curve) |
| regime hysteresis | 2 % | rigid/flexible flip-flapping at the `p_gcr` boundary |
| one-substep lag on `z`, `s` in couplings | – | algebraic loops (z↔W, z↔s) without an inner solver |

### 6.5 Semi-implicit vs explicit, summarized

Body integration is Chaos's own (unchanged). Suspension uses the analytic point-velocity damper term evaluated at the current substep state – semi-implicit in effect. Wheel spin is explicit in the drive/reaction torques with the overshoot clamp (rule 3) and implicit-style handling of pure dissipation (rule 2). A fully implicit slip solve (Newton on `omega` inside the substep) is not needed at `dt = 1/60` with these clamps; note it as the escalation path if a future high-grip, low-inertia wheel spec ever shows residual oscillation.

---

## 7. Talent hooks – physical variables only

Per the scope section 7 hard constraint, each talent drives a named physical input of the equations above; nothing multiplies an output.

1. **Pressure Calibration (CTIS)** – drives `p_i`: `p_i = p_i_nominal · m_ctis`, `m_ctis` player-adjustable in `[0.4, 1.2]` (level 1 default preset 0.6). Effect path: lower `p_i + p_c` → flexible regime engages sooner → `p` drops → `z = (p/k_eq)^(1/n)` drops → `R_c ~ z^(n+1)` collapses (test vector 8: −44 percent sinkage, −63 percent compaction resistance at 0.6×). `p_i` is per-vehicle persistent state → new save field (agent2 save note).
2. **Shear Control (slip governor)** – drives the drive-torque cap. Proportional governor around target slip `s* = 0.20`, full cut at `s_hi = 0.30`:
   `G(s) = 1` for `|s| <= s*`; `G(s) = clamp( (s_hi - |s|) / (s_hi - s*), 0, 1 )` for `|s| > s*`.
   Applied multiplicatively to `T_drive` per substep (fast inner loop; stable because reducing torque directly reduces `ds/dt`). Never caps brake torque. Holds the wheel inside the scope's `s = 0.15..0.25` window at any throttle.
3. **Dynamic Dampening (active suspension load balancing)** – drives the per-wheel preload offset `u_i` (a rest-length shift, section 3 step 3):
   `du_i/dt = -k_bal · (W_i - W_avg)`, `k_bal = 1 / (tau · k_s)`, `tau = 1.5 s`; clamp `|u_i| <= 0.5 x_t`; subtract the mean of all `u_i` each frame so ride height does not drift. Equalizing `W_i` minimizes `max_i z_i` and therefore total `R` – exactly the scope's stated mechanism, achieved by moving a spring, not a number.

---

## 8. Unit test vectors

All values to 4 significant figures; intermediates shown so each assertion can bind to a function in isolation. Common wheel unless stated: `r_w = 0.3 m`, `b = 0.2 m`. Soil constants from the section 2 table (SI-converted). Tolerance for asserts: relative 1e-3 unless noted.

**T1 – rigid sinkage solve, dry sand, W = 3000 N, s = 0** (`k_eq = 990/0.2 + 1 528 430 = 1 533 380`):
seed `z_seed = (3000 / (0.2·1 533 380·sqrt(0.6)))^(1/1.6) = 0.06507 m`;
Newton iterates: `g(0.06507) = -167.4 N` → `z = 0.06757`; `g = +2.5 N` → **`z = 0.06753 m`** (2 iterations, `|g| < 1 N`).
Derived: `l = sqrt(0.06753·0.5325) = 0.1896 m`, `A = 0.03793 m²`, `p = 79 100 Pa`. Cross-check Bekker: `k_eq · z^1.1 = 79 100 Pa`. ✓

**T2 – rigid sinkage, clayey soil, same wheel and load** (`k_eq = 13 190/0.2 + 692 150 = 758 100`):
seed 0.02554; one Newton step → **`z = 0.02612 m`**; `l = 0.1224 m`, `p = 122 500 Pa`. Assert clay sinks far less than sand at equal load (0.02612 < 0.06753) – the low exponent stiffens shallow response.

**T3 – compaction resistance growth with load, dry sand** (superlinearity assert):
`W = 3000 N` (z from T1): `R_c = 0.2·1 533 380·0.06753^2.1 / 2.1 =` **`508.8 N`**.
`W = 6000 N`: solve → `z = 0.1067 m`, `R_c =` **`1329 N`**.
Assert ratio `1329/508.8 = 2.611 > 2`: doubling load more than doubles resistance ("heavy cargo explodes R").

**T4 – tractive effort vs slip, dry sand patch of T1** (`F_bud = A·c + W·tan(phi) = 39.44 + 1595 = 1635 N`; `u = s·l/K = s·7.585`):
- `s = 0.05`: `u = 0.3793`, `E = 0.1678`, **`F = 274.2 N`**
- `s = 0.20`: `u = 1.517`, `E = 0.4854`, **`F = 793.4 N`**
- `s = 0.80`: `u = 6.068`, `E = 0.8356`, **`F = 1366 N`**
Drawbar with static-z resistances (`R_c = 508.8`, `R_b` below, total `575.4 N`): `DP(0.05) = -301.2 N` (cannot crawl at 5 percent slip on this sand under this load), `DP(0.20) = +218.0 N`.

**T5 – bulldozing term, dry sand, z = 0.06753 m** (`K_p = tan²(59°) = 2.770`):
`R_b = 0.5·15 700·0.06753²·0.2·2.770 + 2·1040·0.06753·0.2·sqrt(2.770) = 19.83 + 46.76 =` **`66.59 N`**.

**T6 – slip regularization at rest** (`v_eps = 0.1`):
- `v_x = 0, omega·r_eff = 0` → `s = 0.000` exactly (no NaN).
- `v_x = 0, omega·r_eff = 0.05` → `s = 0.05/0.1 =` **`0.5000`** (softened, not 1.0 – intended).
- `v_x = 2.0, omega·r_eff = 1.0` → `s = -1.0/2.0 =` **`-0.5000`** (braking branch).

**T7 – motor curve** (`T_s = 250, omega_0 = 40, eta = 0.85, P_cu0 = 2000`, throttle 1, supply 1):
- stall `omega = 0`: **`T = 250.0 N·m`**, `P_mech = 0`, **`P_elec = 2000 W`** (pure copper loss).
- near no-load `omega = 36`: **`T = 25.00 N·m`**, `P_mech = 900.0 W`, copper `20.00 W`, **`P_elec = 1079 W`**.
- `omega = 40`: `T = 0`, `P_elec = 0`.

**T8 – regime criterion, flexible branch, CTIS. Sandy loam** (`k_eq = 26 350 + 1 515 040 = 1 541 390`, `n = 0.7`), `W = 3000 N`, `p_i = 80 kPa`, `p_c = 15 kPa`:
`p_gcr = k_eq^(1/2.4) · (9000/(2.3·0.2·sqrt(0.6)))^(1.4/2.4) =` **`140 100 Pa`**; `p_i + p_c = 95 000 < p_gcr` → **flexible**.
`p = 95 000 Pa`, **`z = (p/k_eq)^(1/0.7) = 0.01868 m`**, `A = W/p = 0.03158 m²`, `l = 0.1579 m`, **`delta = 0.3 - sqrt(0.09 - 0.07895²) = 0.01057 m`**, `r_eff = 0.2965 m`, `R_c = 208.2 N`.
CTIS 0.6×: `p = 63 000 Pa` → **`z = 0.01039 m`** (−44.4 percent), **`R_c = 77.08 N`** (−63.0 percent).

**T9 – slip sinkage (dig-in), dry sand, W = 3000 N, s = 0.8** (`n_eff = 1.1 + 0.9·0.8 = 1.82`):
Newton: seed 0.1519, iterates 0.1630 → **`z = 0.1626 m`** (2.4× the s = 0 sinkage of T1);
`R_c = 648.7 N`, `R_b = 227.6 N`, total `876.3 N` vs 575.4 N at s = 0. Assert monotonic growth of `z` and `R` with `|s|`.

**T10 – combined slip budget, sand patch of T1**, `s = 0.20`, `alpha = 8°` (`tan = 0.1405`), `K = 0.025`, `K_y = 0.030`:
`u_x = 1.517`, `u_y = 0.1405·0.1896/0.030 = 0.8884`, `u_r = 1.758`, `E_r = 0.5293`, `F_r = 1635·0.5293 =` **`865.1 N`**;
**`F_x = 746.5 N`**, **`F_y = 437.2 N`**; assert `sqrt(F_x² + F_y²) = 865.1 = F_r <= F_bud`, and `F_x < 793.4` (steering steals traction – understeer emerges).

**T11 – suspension force and no-pull clamp** (`k_s = 40 000, c_s = 3162`):
- `C = 0.05 m, C_dot = -0.2 m/s`: `F_s = 2000 - 632.4 =` **`1368 N`**.
- `C = 0.005 m, C_dot = -3.0 m/s`: raw `200 - 9486 < 0` → **`F_s = 0`** (clamped; a tire cannot pull the chassis down).

These map one-to-one onto UE Automation Tests (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`) against pure free functions – which is why the entire model above must live in a stateless math namespace (suggested `ExoneerTerramechanics.h/.cpp`, functions taking plain structs, no UObject) with the module as a thin caller.

---

## 9. UE unit boundary – conversions exist exactly once

One shared header (module-root relative, e.g. `Vehicles/ExoneerVehicleUnits.h`) owns every constant; the file-local `NewtonsToUEForce` in `VehicleModule.cpp:62` moves here (agent2 "also worth extracting"):

```
CmPerM            = 100.0f      // lengths, positions: SI -> UE multiply, UE -> SI divide
NewtonsToUEForce  = 100.0f      // N (kg·m/s²) -> UE force (kg·cm/s²)
NmToUETorque      = 10000.0f    // N·m -> UE torque (kg·cm²/s²), for any AddTorqueInRadians use
GravityMs2(World) = -World->GetGravityZ() / CmPerM   // single gravity source, currently 9.8
```

Kilograms are unchanged (UE mass is kg). The conversion sites are exactly: (a) reading body state into the substep (positions, velocities ÷100), (b) applying `F_world` (×100) and any torque (×10 000), (c) the raycast lengths (×100). All soil data, wheel specs, tests, and every formula in this document stay pure SI; no UE type appears in the math namespace.

---

## 10. Explicitly out of v1 (flagged, not silently dropped)

Multi-pass (repeated wheel tracks soften/harden soil), Ackermann steering geometry, tire relaxation length, gyroscopic wheel torques on the chassis, thermal tire model, regenerative braking (5.5), anti-dive strut geometry (3), terrain deformation visuals. Each slots behind an existing variable (`k_eq` per location, `beta_t` per wheel, first-order `F_y` filter, `P_elec` sign) without touching the model core. Additionally, the gyro `RotationTorquePerKg` free attitude authority and the pilot-input decay/40-percent-sag issue (agent2) must be resolved in the implementation plan or they will mask and contaminate this model's behavior during playtests.

---

### Critical Files for Implementation

- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Vehicles/VehicleModule.cpp (module tick pattern, force application precedent, the `NewtonsToUEForce` constant to extract)
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Vehicles/VehicleConstruct.cpp (tick order, `AddCustomPhysics` registration point, damping lines 140–141, `RebuildDerivedState` for wheel visuals/state lifecycle)
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Data/VehicleBlockDefinitionDataAsset.h (wheel spec fields, SI authored)
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Data/PlanetBiomeDataAsset.h (the nine soil fields of section 2)
- c:/Users/Mark/Documents/GitHub/Exoneer/Config/DefaultEngine.ini (fix `MaxSubstepDeltaTime` key; substep settings the stability bounds of 6.3 assume)