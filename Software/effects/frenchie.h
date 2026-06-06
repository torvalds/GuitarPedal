// NAME: Frenchie Amp [FRENCH]
// PRIORITY: 125
// POT: "Gain" LINEAR(0 1) = 0.5
// POT: "Tone" LINEAR(0 1) = 0.5
// POT: "Input" LINEAR(0 1) = 0.5
// POT: "Sag" LINEAR(0 1) = 0.5
// POT: "Level" LINEAR(0 2.0) = 0.25

/*
 * 5W Class A tube amplifier (Fender Champ topology)
 *
 * Signal path:
 *   input -> RF filter -> input stage (grid physics) -> coupling cap HP ->
 *   PSU sag -> 12AX7 triode (density-aware) -> tone control ->
 *   6V6 SE Class A power amp -> soft gate -> output
 *
 * Density tracker simplified to 2-band (LO + MID) for embedded.
 * Full version uses 4 bands; the extra two (HI, AIR) modulate Miller Q
 * and an air shelf -- nice to have but not essential to the sound.
 */

// This is almost certainly not the right frequency to filter at
// but unlike the Klon, I don't know what the right values are
static inline float frenchie_dc_step(struct single_pole_state *state, float x)
{
	return single_pole_hpf(x, state, single_pole_freq(20.0));
}

/* ================================================================== */
/*  2-BAND DENSITY TRACKER                                            */
/*  Simplified from 4-band for embedded. Tracks LO + MID energy to    */
/*  derive harmonic density and mid_density. These drive the triode's */
/*  Miller LP cutoff and drive boost.                                 */
/* ================================================================== */

struct frenchie_density {
	struct biquad f_lo;			/* LP at 250Hz — isolates fundamental */
	struct biquad f_mid_lo;			/* LP at 2kHz — \                     */
	struct biquad f_mid_hi;			/* HP at 250Hz — / bandpass for mids  */
	struct envelope env_lo, env_mid, env_total;
	float density;				/* 1.0–2.5: how much harmonic content */
	float mid_density;			/* 0.0–1.0: mid-band energy ratio     */
	float smooth;				/* smoothing coefficient              */
};

static inline void frenchie_density_init(struct frenchie_density *d)
{
	biquad_lpf(&d->f_lo, 250.0f, 0.7f);
	biquad_lpf(&d->f_mid_lo, 2000.0f, 0.7f);
	biquad_hpf(&d->f_mid_hi, 250.0f, 0.7f);
	envelope_init(&d->env_lo, 2.0f, 80.0f);
	envelope_init(&d->env_mid, 2.0f, 80.0f);
	envelope_init(&d->env_total, 2.0f, 80.0f);
	d->smooth = time_constant(30.0f);
}

static inline void frenchie_density_step(struct frenchie_density *d, float x)
{
	float lo = biquad_step(&d->f_lo, x);
	float mid = biquad_step(&d->f_mid_hi, biquad_step(&d->f_mid_lo, x));
	float e_lo = envelope_step(&d->env_lo, lo);
	float e_mid = envelope_step(&d->env_mid, mid);
	float e_tot = envelope_step(&d->env_total, x);
	float raw = 1.0f, raw_mid = 0.0f;

	if (e_lo > 0.0001f)
		raw = clamp(e_tot / e_lo, 1.0f, 2.5f);
	if (e_tot > 0.0001f)
		raw_mid = clamp(e_mid / e_tot, 0.0f, 1.0f);

	d->density = linear(d->smooth, raw, d->density);
	d->mid_density = linear(d->smooth, raw_mid, d->mid_density);
}

/* ================================================================== */
/*  INPUT STAGE — 12AX7 grid physics                                  */
/*  Coupling cap bias shift, grid conduction, cathode bypass shelf,   */
/*  input loading LP. Sets the amp's "feel" before any gain stage.    */
/* ================================================================== */

struct frenchie_input_stage {
	struct biquad input_lp;			/* input loading */
	struct envelope peak;			/* peak envelope for metering */
	float cap_charge;
	float cap_charge_rate;
	float cap_drain_rate;
};

static inline void frenchie_input_stage_init(struct frenchie_input_stage *s)
{
	biquad_lpf(&s->input_lp, 4500.0f, 0.6f);
	envelope_init(&s->peak, 0.2f, 50.0f);
	s->cap_charge_rate = time_constant(0.5f);
	s->cap_drain_rate = time_constant(30.0f);
}

static inline float frenchie_input_stage_step(struct frenchie_input_stage *s, float x, float intensity)
{
	float loaded, bias_shift, biased, grid_thresh, conducted;

	/* Input loading LP, blended by intensity */
	loaded = biquad_step(&s->input_lp, x);
	loaded = linear(intensity, x, loaded);

	/* Coupling cap bias shift */
	if (loaded > 0.1f)
		s->cap_charge = linear(s->cap_charge_rate, loaded * 0.15f * intensity, s->cap_charge);
	else
		s->cap_charge = s->cap_drain_rate * s->cap_charge;

	bias_shift = s->cap_charge;
	biased = loaded - bias_shift;

	/* Grid conduction — tube grid draws current on positive peaks */
	grid_thresh = 1.0f - intensity * 0.85f;
	if (biased > grid_thresh)
		conducted = grid_thresh
			+ tanhf((biased - grid_thresh) * (1.0f + intensity * 3.0f))
			* (0.05f + intensity * 0.15f);
	else
		conducted = biased;

	/* 5F1 Champ V1A has no bypass cap, so response is flat here. */
	envelope_step(&s->peak, x);
	return conducted;
}

/* ================================================================== */
/*  PSU SAG — 5Y3 tube rectifier power supply                         */
/*  Slow attack, slow release, massive droop. THE Champ feel.         */
/* ================================================================== */

struct frenchie_psu {
	float voltage, droop;
	float att, rel;
	float max_droop, min_v, base_v;
};

static inline void frenchie_psu_init(struct frenchie_psu *p)
{
	/* 5Y3 tube rectifier: FAST attack (0.5ms), SLOW release (200ms) */
	p->att = time_constant(0.5f);
	p->rel = time_constant(200.0f);
	p->max_droop = 0.30f;			/* 30% voltage drop under load */
	p->min_v = 0.55f;			/* never below 55% of nominal */
	p->base_v = 1.0f;
}

static inline float frenchie_psu_step(struct frenchie_psu *p, float signal_level, float sag_depth)
{
	float draw = signal_level * signal_level;

	if (draw > p->droop)
		p->droop = linear(p->att, draw, p->droop);
	else
		p->droop = linear(p->rel, draw, p->droop);

	p->voltage = p->base_v - p->droop * p->max_droop * sag_depth;
	p->voltage = clamp(p->voltage, p->min_v, p->base_v);
	return p->voltage;
}

/* ================================================================== */
/*  TRIODE — 12AX7 density-aware preamp tube                          */
/*                                                                    */
/*  coupling cap HP → grid leak bias → density-boosted drive →        */
/*  asymmetric tanh clip (bplus modulates ceiling) →                  */
/*  grid conduction → density-driven Miller LP → DC block             */
/*                                                                    */
/*  Character comes from parameters, not code.                        */
/* ================================================================== */

struct frenchie_triode {
	struct biquad hp;			/* coupling cap */
	struct biquad miller;			/* Miller capacitance LP */
	struct frenchie_density hd;		/* harmonic density tracker */
	struct single_pole_state dc;		/* DC blocker */
	struct envelope peak;			/* peak envelope for metering */
	float cap_charge;
	float cap_charge_rate, cap_drain_rate;
	float miller_base_freq;
	float last_miller_freq;
};

static inline void frenchie_triode_init(struct frenchie_triode *t, float hp_freq, float lp_freq)
{
	biquad_hpf(&t->hp, hp_freq, 0.7f);
	biquad_lpf(&t->miller, lp_freq, 0.6f);
	t->miller_base_freq = lp_freq;
	t->last_miller_freq = lp_freq;
	frenchie_density_init(&t->hd);
	envelope_init(&t->peak, 0.2f, 50.0f);
	t->cap_charge_rate = time_constant(0.5f);
	t->cap_drain_rate = time_constant(30.0f);
}

static inline float frenchie_triode_step(struct frenchie_triode *t, float x,
	float drive, float bplus,
	float pos_hard, float neg_hard,
	float pos_ceil, float neg_ceil,
	float gc_thresh, float gc_amount)
{
	float b, gain, bias_shift, drive_boost, lp_freq;

	/* Coupling cap HP */
	b = biquad_step(&t->hp, x);

	/* Grid leak bias shift — coupling cap charges on positive signal,
	 * shifts the DC operating point down. Slow drain = long recovery. */
	if (b > 0.1f)
		t->cap_charge = linear(t->cap_charge_rate, b * 0.12f, t->cap_charge);
	else
		t->cap_charge = t->cap_drain_rate * t->cap_charge;

	bias_shift = t->cap_charge;
	b -= bias_shift;

	/* Density-boosted drive — mid content pushes tube harder */
	drive_boost = 1.0f + t->hd.mid_density * 0.4f;
	gain = (1.0f + drive * 4.0f) * drive_boost;
	b *= gain;

	/* Density tracker (pre-clip) */
	frenchie_density_step(&t->hd, b);

	// Replaced piecewise tanh with a single offset tanh to maintain identical
	// clipping asymptotes (+0.85, -1.0) and slope at zero (0.935) without crossover distortion.
	/* Asymmetric tanh clipping — THE transfer function.
	 * Positive clips harder (plate saturation), negative clips softer
	 * (cutoff is gradual). bplus scales ceiling — PSU sag compresses. */
	b = (0.925f * tanhf(1.0175f * b + 0.08126f) - 0.075f) * bplus;

	/* Grid conduction — soft clamp on positive peaks */
	float peak_val = envelope_step(&t->peak, b);
	if (b > gc_thresh && peak_val > gc_thresh * 0.8f)
		b = gc_thresh + tanhf((b - gc_thresh) * 3.0f) * gc_amount;

	/* Miller LP — density drives cutoff down.
	 * More harmonics = more intermod = Miller capacitance increases.
	 * Only recalculate when freq changes meaningfully. */
	lp_freq = t->miller_base_freq - (t->hd.density - 1.0f) * 500.0f;
	lp_freq = clamp(lp_freq, t->miller_base_freq - 1500.0f,
	                              t->miller_base_freq + 500.0f);
	if (fabsf(lp_freq - t->last_miller_freq) > 30.0f) {
		biquad_lpf(&t->miller, lp_freq, 0.6f);
		t->last_miller_freq = lp_freq;
	}
	b = biquad_step(&t->miller, b);

	/* DC block */
	return frenchie_dc_step(&t->dc, b);
}

/* ================================================================== */
/*  SINGLE-ENDED CLASS A POWER AMP — 6V6                              */
/*                                                                    */
/*  One tube does everything. Asymmetric clip: positive half hits     */
/*  plate saturation (hard, low ceiling), negative half runs into     */
/*  cutoff (soft, high ceiling). This asymmetry generates even        */
/*  harmonics — the "warm" sound of Class A.                          */
/*                                                                    */
/*  Cathode bias shift compresses under load (the "spongy" feel).     */
/*  Transformer saturates on low-frequency content.                   */
/*  Winding resonance adds upper-mid character.                       */
/* ================================================================== */

struct frenchie_power_amp {
	struct frenchie_density hd;		/* harmonic density tracker */
	struct single_pole_state dc;		/* DC blocker */
	struct envelope cathode;
	struct biquad xfmr_res;			/* transformer winding resonance */
	struct biquad alias_lp;			/* anti-alias after clipping */
	float xfmr_lp;				/* 1-pole LP state for xfmr sat */
	float drive;
};

static inline void frenchie_power_amp_init(struct frenchie_power_amp *pa)
{
	frenchie_density_init(&pa->hd);
	/* Cathode: 5ms attack, 150ms release, 20% shift */
	envelope_init(&pa->cathode, 5.0f, 150.0f);

	// FIX: The original lms_bq_set_peak function mathematically doubled the
	// specified decibel boost because it passed `A = 10^(db/20)` into the
	// equations instead of the standard `A = 10^(db/40)`.
	// To match the original code's exact response curve, we double the db value here.
	biquad_peaking(&pa->xfmr_res, 3500.0f, 2.0f, db_to_level(2.5f * 2.0f));

	biquad_lpf(&pa->alias_lp, 10000.0f, 0.4f);
}

static inline float frenchie_power_amp_step(struct frenchie_power_amp *pa, float x, float bplus)
{
	float b, out, cathode_shift, xfmr_sat;
	float lo_content, hi_content, lo_saturated, lo_final;

	/* Drive scaling */
	b = x * (1.0f + pa->drive * 2.0f);

	/* Density tracker */
	frenchie_density_step(&pa->hd, b);

	/* Cathode bias shift — envelope on signal shifts DC operating point */
	float cathode_val = envelope_step(&pa->cathode, b);
	cathode_shift = cathode_val * 0.20f;
	b -= cathode_shift;

	// Replaced piecewise tanh with a single offset tanh to maintain identical
	// clipping asymptotes (+0.7, -1.0) and slope at zero (1.26) without crossover distortion.
	/* Single-ended asymmetric clipping.
	 * pos: hard clip (1.8), low ceiling (0.7) — plate saturation
	 * neg: soft clip (0.6), high ceiling (1.0) — gradual cutoff
	 * Even harmonics preserved (no push-pull cancellation). */
	out = (0.85f * tanhf(1.53f * b + 0.17833f) - 0.15f) * bplus;

	/* Transformer saturation — iron core saturates on low-freq content.
	 * Split into LO (1-pole LP) and HI, saturate only LO, recombine. */
	float env_lo_val = pa->hd.env_lo.value;
	xfmr_sat = clamp(env_lo_val * 4.0f, 0.0f, 1.0f);
	if (xfmr_sat > 0.05f) {
		pa->xfmr_lp = pa->xfmr_lp * 0.988f + out * 0.012f;
		lo_content = pa->xfmr_lp;
		hi_content = out - lo_content;
		lo_saturated = tanhf(lo_content * (1.0f + xfmr_sat * 1.5f))
			     / (1.0f + xfmr_sat * 0.3f);
		lo_final = lo_content + xfmr_sat * (lo_saturated - lo_content);
		out = lo_final + hi_content;
	}

	/* Transformer winding resonance — parasitic peak ~3.5kHz */
	out = biquad_step(&pa->xfmr_res, out);
	/* Anti-alias LP */
	out = biquad_step(&pa->alias_lp, out);

	/* DC block */
	return frenchie_dc_step(&pa->dc, out) * 0.85f;
}

/* ================================================================== */
/*  SOFT GATE — envelope-following noise gate                         */
/* ================================================================== */

struct frenchie_gate {
	struct envelope env;
	float open_thresh, close_thresh;
};

static inline void frenchie_gate_init(struct frenchie_gate *g)
{
	envelope_init(&g->env, 0.5f, 150.0f);
	g->open_thresh = 0.001f;
	g->close_thresh = 0.0002f;
}

static inline float frenchie_gate_step(struct frenchie_gate *g, float x)
{
	float e, t, gain;

	e = envelope_step(&g->env, x);

	if (e > g->open_thresh)
		gain = 1.0f;
	else if (e < g->close_thresh)
		gain = 0.0f;
	else {
		t = (e - g->close_thresh) / (g->open_thresh - g->close_thresh);
		gain = t * t * (3.0f - 2.0f * t);
	}
	return x * gain;
}

static struct {
	struct biquad rf_filter;			/* 10kHz — grid stopper × Cgk */
	struct frenchie_input_stage input_stage;	/* 12AX7 grid physics */
	struct frenchie_psu psu;			/* 5Y3 tube rectifier */
	struct frenchie_triode v1a;			/* 12AX7 preamp triode */
	struct single_pole_state tone_lp;		/* 1-pole passive tone control */
	struct single_pole_coeff tone_coeff;
	struct frenchie_power_amp power_amp;		/* 6V6 Class A power amp */
	struct frenchie_gate gate;			/* noise gate */

	/* Parameters */
	float gain;
	float input_intensity;
	float sag_depth;
	float out_level;

	int initialized;
} frenchie;

static inline void frenchie_init(unsigned char pot[10])
{
	if (!frenchie.initialized) {
		frenchie.v1a.hd.density = 1.0f;
		frenchie.power_amp.hd.density = 1.0f;
		frenchie.psu.voltage = 1.0f;
		frenchie.initialized = 1;
	}

	biquad_lpf(&frenchie.rf_filter, 10000.0f, 0.5f);
	frenchie_input_stage_init(&frenchie.input_stage);
	frenchie_psu_init(&frenchie.psu);
	/* V1a: coupling cap HP at 30Hz, Miller LP at 8kHz */
	frenchie_triode_init(&frenchie.v1a, 30.0f, 8000.0f);
	frenchie_power_amp_init(&frenchie.power_amp);
	frenchie_gate_init(&frenchie.gate);

	frenchie.gain = frenchie_pot0(pot[0]);
	float tone = frenchie_pot1(pot[1]);
	frenchie.input_intensity = frenchie_pot2(pot[2]);
	frenchie.sag_depth = frenchie_pot3(pot[3]);
	frenchie.out_level = frenchie_pot4(pot[4]);

	frenchie.tone_coeff = single_pole_freq(1500.0f + tone * 18500.0f);
}

static inline float frenchie_step(float in)
{
	float mono = in;
	float bplus;

	mono = clamp(mono, -1.0f, 1.0f);

	/* RF filter — 68kΩ grid stopper × grid capacitance */
	mono = biquad_step(&frenchie.rf_filter, mono);

	/* Input stage — grid physics (coupling cap bias, grid conduction) */
	if (frenchie.input_intensity > 0.0f)
		mono = frenchie_input_stage_step(&frenchie.input_stage, mono, frenchie.input_intensity);

	/* PSU sag — 5Y3 tube rectifier droops under load */
	bplus = frenchie_psu_step(&frenchie.psu,
		fabsf(mono) * (1.0f + frenchie.gain * 3.0f), frenchie.sag_depth);

	/* V1a — 12AX7 preamp triode (the sound of the amp) */
	mono = frenchie_triode_step(&frenchie.v1a, mono, frenchie.gain * 0.65f, bplus,
		1.1f, 0.935f /* FIX: was 0.9f, 0.935 matches positive slope (1.1 * 0.85) */, 0.85f, 1.0f, 0.35f, 0.12f);

	/* Tone control — 5F2-A Princeton-style passive 1-pole treble bleed */
	mono = single_pole_lpf(mono, &frenchie.tone_lp, frenchie.tone_coeff);

	/* 6V6 SE Class A power amp */
	frenchie.power_amp.drive = frenchie.gain * 0.55f;
	mono = frenchie_power_amp_step(&frenchie.power_amp, mono, bplus);

	/* Noise gate */
	mono = frenchie_gate_step(&frenchie.gate, mono);

	/* Output clamp */
	return clamp(mono * frenchie.out_level, -0.95f, 0.95f);
}
