#!/usr/bin/env python3
"""Generate midisynth's baked fixed-point tables.

Every transcendental-derived constant in the synth comes from this script,
computed with Python's `decimal` at 60 digits and truncated (toward zero for
positives; all baked values here are non-negative except the sine table, which
is truncated toward zero symmetrically) into integer literals. Platform libm is
never consulted at runtime, so the tables are identical on every target by
construction -- the same approach core/ uses for its CORDIC atan table.

Output: src/synth/tables_baked.inc (checked in). Re-run and diff to verify.
"""

from decimal import Decimal, getcontext
import os

getcontext().prec = 60

PI = Decimal(
    "3.14159265358979323846264338327950288419716939937510582097494"
)
LN2 = Decimal(
    "0.69314718055994530941723212145817656807550013436025525412068"
)

SAMPLE_RATE = 48000
Q30 = 1 << 30
Q24 = 1 << 24
Q16 = 1 << 16


def dsin(x: Decimal) -> Decimal:
    """Taylor sine, argument already reduced to [-pi/4, pi/4] by callers'
    symmetry folding; converges far past 60 digits there."""
    term = x
    total = x
    x2 = x * x
    n = 1
    while abs(term) > Decimal(10) ** -55:
        term = -term * x2 / ((2 * n) * (2 * n + 1))
        total += term
        n += 1
    return total


def dcos(x: Decimal) -> Decimal:
    return dsin(PI / 2 - x)


def sin_turns(t: Decimal) -> Decimal:
    """sin(2*pi*t) via octant folding so dsin only sees small arguments."""
    t = t % 1
    if t >= Decimal("0.5"):
        return -sin_turns(t - Decimal("0.5"))
    if t > Decimal("0.25"):
        t = Decimal("0.5") - t
    x = 2 * PI * t
    if x > PI / 4:
        return dcos(PI / 2 - x)
    return dsin(x)


def dexp(x: Decimal) -> Decimal:
    term = Decimal(1)
    total = Decimal(1)
    n = 1
    while abs(term) > Decimal(10) ** -55:
        term = term * x / n
        total += term
        n += 1
    return total


def dexp2(x: Decimal) -> Decimal:
    return dexp(x * LN2)


def dtan_turns(t: Decimal) -> Decimal:
    """tan(2*pi*t) for t in (0, 0.25)."""
    s = sin_turns(t)
    c = sin_turns(t + Decimal("0.25"))
    return s / c


def trunc(x: Decimal, scale: int) -> int:
    v = x * scale
    return int(v.to_integral_value(rounding="ROUND_DOWN")) if v >= 0 else -int(
        (-v).to_integral_value(rounding="ROUND_DOWN")
    )


def emit(f, name, ctype, values, per_line=8):
    f.write(f"inline constexpr {ctype} {name}[{len(values)}] = {{\n")
    for i in range(0, len(values), per_line):
        row = ", ".join(str(v) for v in values[i : i + per_line])
        f.write(f"    {row},\n")
    f.write("};\n\n")


def main():
    out_path = os.path.join(
        os.path.dirname(__file__), "..", "src", "synth", "tables_baked.inc"
    )

    # --- Sine, full cycle, 4096 entries, Q2.30. Used for additive wavetable
    # generation (all needed arguments land exactly on k/4096 turns when the
    # generator works at harmonic*index mod N resolution), the pan law, and the
    # LFO sine shape.
    SINE_N = 4096
    sine = [trunc(sin_turns(Decimal(i) / SINE_N), Q30) for i in range(SINE_N)]

    # --- 2^(c/1200) for c = 0..1200, Q2.30 (1201 entries so lerp can read
    # c+1 at the top). Pitch: cents-within-octave to frequency ratio.
    cents = [trunc(dexp2(Decimal(c) / 1200), Q30) for c in range(1201)]

    # --- 2^(i/256) for i = 0..256, Q2.30. General exp2 fraction table for
    # cutoff-index math and anything needing a coarse exponential.
    exp2t = [trunc(dexp2(Decimal(i) / 256), Q30) for i in range(257)]

    # --- Phase increment at MIDI cents 0 (8.1758 Hz), Q16 extra fraction:
    # round(f0 / 48000 * 2^32 * 2^16). f0 = 440 * 2^(-69/12).
    f0 = Decimal(440) * dexp2(Decimal(-69) / 12)
    inc0 = trunc(f0 / SAMPLE_RATE * (1 << 32), 1 << 16)

    # --- Filter cutoff table: 257 entries, exponentially spaced cutoffs from
    # FILT_FMIN=20 Hz to FILT_FMAX=12000 Hz (fs/4 cap, Appendix D), storing
    # g = tan(pi * fc / fs) in Q2.30.
    FMIN, FMAX = Decimal(20), Decimal(12000)
    n_cut = 257
    lg_span = (FMAX / FMIN).ln() / LN2  # log2(fmax/fmin)
    cutoff_g = []
    for i in range(n_cut):
        fc = FMIN * dexp2(lg_span * i / (n_cut - 1))
        g = dtan_turns(fc / (2 * SAMPLE_RATE))  # tan(pi*fc/fs) = tan(2pi * fc/(2fs))
        cutoff_g.append(trunc(g, Q30))

    # --- Envelope curve tables: 257 entries, Q16.16, curve(0)=0, curve(1)=ONE.
    # Shapes over normalized phase p in [0,1]:
    #   linear:    p
    #   exp:       (1 - e^(-6p)) / (1 - e^-6)      convex, decay/release feel
    #   expAttack: (e^(6p) - 1) / (e^6 - 1)        concave rise... but a
    #              perceptually rising attack wants fast start: use inverse,
    #              1 - (1-p)^3 style handled by 'sCurve'? Keep the classic:
    #              expAttack is the fast-start convex rise (same as exp).
    #              To give authors a genuinely different attack we bake the
    #              slow-start concave curve too.
    #   sCurve:    smoothstep 3p^2 - 2p^3
    N_CURVE = 257

    def curve_exp(p):  # fast start, asymptotic finish (classic RC decay shape)
        return (1 - dexp(-6 * p)) / (1 - dexp(Decimal(-6)))

    def curve_exp_attack(p):  # slow start, accelerating finish
        return (dexp(6 * p) - 1) / (dexp(Decimal(6)) - 1)

    def curve_s(p):
        return 3 * p * p - 2 * p * p * p

    exp_c = [trunc(curve_exp(Decimal(i) / (N_CURVE - 1)), Q16) for i in range(N_CURVE)]
    expa_c = [
        trunc(curve_exp_attack(Decimal(i) / (N_CURVE - 1)), Q16) for i in range(N_CURVE)
    ]
    s_c = [trunc(curve_s(Decimal(i) / (N_CURVE - 1)), Q16) for i in range(N_CURVE)]
    # Force exact endpoints.
    for t in (exp_c, expa_c, s_c):
        t[0] = 0
        t[-1] = Q16

    with open(out_path, "w") as f:
        f.write("// GENERATED by scripts/gen_tables.py -- do not edit.\n")
        f.write("// Decimal-precision generation; identical on every target.\n\n")
        f.write(f"inline constexpr long long PITCH_INC0_Q16 = {inc0}LL;\n\n")
        emit(f, "SINE_Q30", "int", sine)
        emit(f, "CENTS_RATIO_Q30", "int", cents)
        emit(f, "EXP2_Q30", "int", exp2t)
        emit(f, "CUTOFF_G_Q30", "int", cutoff_g)
        emit(f, "CURVE_EXP_Q16", "int", exp_c)
        emit(f, "CURVE_EXPATT_Q16", "int", expa_c)
        emit(f, "CURVE_S_Q16", "int", s_c)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
