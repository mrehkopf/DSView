/*
 * The wait() condition matcher (libsigrokdecode4DSL/match.c) against the
 * per-sample matcher it replaced (reference_match.c).
 *
 * Each trial makes a random capture (constant, sparse, dense and clock-like
 * channels, unused optional channels), cuts it into chunks the way
 * DecoderStack::decode_data() feeds them (channels that are constant over a
 * chunk sometimes come as a NULL buffer and a constant), and runs a random
 * script of wait() calls through both matchers in lockstep, emulating
 * Decoder_wait() and srd_inst_decode(). After every wait() the returned
 * sample, self.matched, the pin values and the matcher state (previous pins,
 * skip counters, flags) must be equal.
 *
 * condition-test [TRIALS [SEED]]
 */

#include "libsigrokdecode-internal.h"
#include "libsigrokdecode.h"
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "reference_match.h"

/* ---- random numbers ---- */

static uint64_t rng = 88172645463325252ULL;

static uint64_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static uint64_t below(uint64_t n)
{
	return n ? rnd() % n : 0;
}

static int chance(int percent)
{
	return (int)below(100) < percent;
}

/* ---- capture and chunks ---- */

#define MAX_CH 40

struct chunk {
	uint64_t start, end;
	const uint8_t *inbuf[MAX_CH];
	uint8_t inbuf_const[MAX_CH];
	uint8_t *owned[MAX_CH];
};

struct capture {
	int nch;
	int channelmap[MAX_CH];
	uint64_t n;
	uint8_t *bits[MAX_CH];
	uint64_t decode_start;
	int nchunks;
	struct chunk *chunks;
};

static int get_bit(const uint8_t *b, uint64_t s)
{
	return (b[s / 8] >> (s % 8)) & 1;
}

static void set_bit(uint8_t *b, uint64_t s, int v)
{
	if (v)
		b[s / 8] |= 1 << (s % 8);
	else
		b[s / 8] &= ~(1 << (s % 8));
}

static void make_signal(uint8_t *b, uint64_t n)
{
	uint64_t s = 0, run, mean;
	int v = chance(50), kind = (int)below(5);

	mean = kind == 0 ? n + 1                      /* constant */
	     : kind == 1 ? 2 + below(below(100000) + 1) /* sparse */
	     : kind == 2 ? 1 + below(3)                /* dense */
	     : kind == 3 ? 1 + below(32)               /* clock: fixed half period */
	     : 0;                                      /* bursts */
	while (s < n) {
		if (kind == 3)
			run = mean;
		else if (kind == 4)
			run = chance(80) ? 1 + below(4) : 1 + below(5000);
		else
			run = 1 + below(2 * mean);
		for (; run && s < n; run--, s++)
			set_bit(b, s, v);
		v = !v;
	}
}

static int constant_over(const uint8_t *b, uint64_t start, uint64_t end, int *value)
{
	uint64_t s;
	int v = get_bit(b, start);

	for (s = start + 1; s < end; s++)
		if (get_bit(b, s) != v)
			return 0;
	*value = v;
	return 1;
}

static void make_capture(struct capture *cap)
{
	uint64_t start, size, base, nbytes;
	int c, cap_chunks = 0, v, r;

	memset(cap, 0, sizeof(*cap));
	cap->nch = 1 + (int)below(chance(25) ? 34 : 6);
	r = (int)below(100);
	cap->n = r < 70 ? 1 + below(20000) : r < 95 ? 1 + below(300000) : 1 + below(3000000);
	cap->decode_start = chance(20) ? below(cap->n < 100 ? cap->n : 100) : 0;

	for (c = 0; c < cap->nch; c++) {
		cap->channelmap[c] = (cap->nch > 1 && chance(10)) ? -1 : c;
		if (cap->channelmap[c] == -1)
			continue;
		cap->bits[c] = calloc(cap->n / 8 + 1, 1);
		make_signal(cap->bits[c], cap->n);
	}

	/* Chunks, like DecoderStack::decode_data(): consecutive, any size. */
	for (start = cap->decode_start; start < cap->n; ) {
		struct chunk *ch;

		if (cap->nchunks == cap_chunks) {
			cap_chunks = cap_chunks ? cap_chunks * 2 : 64;
			cap->chunks = realloc(cap->chunks, cap_chunks * sizeof(*cap->chunks));
		}
		ch = &cap->chunks[cap->nchunks++];
		memset(ch, 0, sizeof(*ch));
		r = (int)below(100);
		size = r < 30 ? 1 + below(64) : r < 80 ? 1 + below(20000) : 1 + below(cap->n);
		ch->start = start;
		ch->end = start + size < cap->n ? start + size : cap->n;
		base = start & ~7ULL;
		nbytes = (ch->end - base + 7) / 8;

		for (c = 0; c < cap->nch; c++) {
			if (cap->channelmap[c] == -1) {
				ch->inbuf[c] = NULL;
				ch->inbuf_const[c] = 0;
			} else if (constant_over(cap->bits[c], ch->start, ch->end, &v) && chance(70)) {
				ch->inbuf[c] = NULL;
				ch->inbuf_const[c] = v;
			} else {
				/* A buffer of exactly the chunk's bytes (ASan catches
				 * reads past it), with garbage in the bits before the
				 * chunk's first and after its last sample. */
				uint8_t *buf = malloc(nbytes);
				uint64_t s;

				memcpy(buf, cap->bits[c] + base / 8, nbytes);
				for (s = base; s < ch->start; s++)
					set_bit(buf, s - base, chance(50));
				for (s = ch->end; s < base + nbytes * 8; s++)
					set_bit(buf, s - base, chance(50));
				ch->inbuf[c] = buf;
				ch->owned[c] = buf;
			}
		}
		start = ch->end;
	}
}

static void free_capture(struct capture *cap)
{
	int i, c;

	for (i = 0; i < cap->nchunks; i++)
		for (c = 0; c < cap->nch; c++)
			free(cap->chunks[i].owned[c]);
	free(cap->chunks);
	for (c = 0; c < cap->nch; c++)
		free(cap->bits[c]);
}

/* ---- wait() conditions ---- */

#define MAX_CONDS 40
#define MAX_TERMS 4

struct tspec {
	int skip;       /* a 'skip' key */
	int type;
	int channel;
	uint64_t count;
};

struct cspec {
	int nterms;
	struct tspec t[MAX_TERMS];
};

struct wspec {
	int none;       /* self.wait() without conditions */
	int nconds;
	struct cspec c[MAX_CONDS];
};

static void make_pin_terms(struct cspec *cs, int nch, int nterms)
{
	int used[MAX_CH] = {0}, i, ch;

	if (nterms > nch)
		nterms = nch;
	for (i = 0; i < nterms; i++) {
		do
			ch = (int)below(nch);
		while (used[ch]);
		used[ch] = 1;
		cs->t[cs->nterms].skip = 0;
		cs->t[cs->nterms].type = chance(1) ? -1 : (int)below(SRD_TERM_SKIP);
		cs->t[cs->nterms].channel = ch;
		cs->nterms++;
	}
}

static uint64_t skip_count(void)
{
	int r = (int)below(100);
	return r < 40 ? below(4) : r < 95 ? below(200) : below(100000);
}

static void make_wspec(struct wspec *ws, int nch)
{
	int i, r, nonempty = 0;
	struct cspec *cs;

	memset(ws, 0, sizeof(*ws));
	if (chance(8)) {
		ws->none = 1;
		return;
	}
	r = (int)below(100);
	ws->nconds = r < 70 ? 1 + (int)below(3) : r < 95 ? 4 + (int)below(9) : 13 + (int)below(MAX_CONDS - 12);
	for (i = 0; i < ws->nconds; i++) {
		cs = &ws->c[i];
		r = (int)below(100);
		if (r < 8) {
			/* {}: never matches */
		} else if (r < 25) {
			cs->t[0].skip = 1;
			cs->t[0].count = skip_count();
			cs->nterms = 1;
		} else if (r < 30) {
			/* Skip and pin terms in one condition, skip anywhere (no
			 * decoder does this). Not 'skip': 0: when such a condition
			 * fails on its first sample, both matchers later step back
			 * from the next match, which before a chunk's first sample
			 * reads far outside the buffer. */
			int at;
			make_pin_terms(cs, nch, 1 + (int)below(2));
			at = (int)below(cs->nterms + 1);
			memmove(&cs->t[at + 1], &cs->t[at], (cs->nterms - at) * sizeof(cs->t[0]));
			cs->t[at].skip = 1;
			cs->t[at].count = 1 + skip_count();
			cs->nterms++;
		} else {
			make_pin_terms(cs, nch, 1 + (int)below(3));
		}
		nonempty |= cs->nterms > 0;
	}
	/* Only empty conditions would match every sample without advancing */
	if (!nonempty)
		make_pin_terms(&ws->c[0], nch, 1);

	/* At most one {'skip': 0}: right after a match each of them steps the
	 * current sample back by one (skip_zero), so two step back past the
	 * match, before sample 0 at the start (both matchers). */
	for (i = 0; i < ws->nconds; i++) {
		cs = &ws->c[i];
		if (cs->nterms == 1 && cs->t[0].skip && cs->t[0].count == 0) {
			if (nonempty == 2)
				cs->t[0].count = 1 + below(5);
			nonempty = 2;
		}
	}
}

/* ---- the two matchers ---- */

struct impl {
	const char *name;
	int (*process)(struct srd_decoder_inst *di, gboolean *found_match);
	void (*free_conds)(struct srd_decoder_inst *di);
	void (*seed)(struct srd_decoder_inst *di);
	void (*destroy)(struct srd_decoder_inst *di);   /* srd_inst_reset_state() */
	int keeps_conditions;   /* keeps the list for repeated conditions, as type_decoder.c does */
};

static const struct impl impls[2] = {
	{ "reference", ref_process_samples_until_condition_match, ref_condition_list_free,
	  ref_oldpins_array_seed, NULL, 0 },
	{ "match.c", process_samples_until_condition_match, condition_list_free,
	  oldpins_array_seed, match_prog_destroy, 1 },
};

struct result {
	int found;
	uint64_t samplenum;
	uint64_t matched;
	uint8_t pins[MAX_CH];
};

struct sim {
	const struct impl *impl;
	struct srd_decoder_inst di;
	const struct capture *cap;
	int next_chunk;
	int have_chunk;
	int ended;      /* 1: out of data, 2: chunk start != current sample */
	double seconds;
};

static void sim_init(struct sim *sim, const struct impl *impl, const struct capture *cap,
		const uint8_t *initial_pins)
{
	int c;

	memset(sim, 0, sizeof(*sim));
	sim->impl = impl;
	sim->cap = cap;
	sim->di.dec_num_channels = cap->nch;
	sim->di.dec_channelmap = (int *)cap->channelmap;
	impl->seed(&sim->di);
	for (c = 0; c < cap->nch; c++)
		sim->di.old_pins_array->data[c] = initial_pins[c];
	/* srd_inst_start() */
	sim->di.first_pos = TRUE;
	sim->di.abs_cur_matched = FALSE;
	sim->di.skip_zero = FALSE;
}

static void sim_free(struct sim *sim)
{
	sim->impl->free_conds(&sim->di);
	if (sim->impl->destroy)
		sim->impl->destroy(&sim->di);
	g_array_free(sim->di.old_pins_array, TRUE);
}

/* create_term_list() and set_new_condition_list() */
static void set_conditions(struct sim *sim, const struct wspec *ws, int same_as_before)
{
	struct srd_decoder_inst *di = &sim->di;
	int i, k;

	if (same_as_before && sim->impl->keeps_conditions) {
		condition_list_reset_skips(di);
		return;
	}
	sim->impl->free_conds(di);
	for (i = 0; i < ws->nconds; i++) {
		GSList *terms = NULL;
		for (k = 0; k < ws->c[i].nterms; k++) {
			const struct tspec *ts = &ws->c[i].t[k];
			struct srd_term *term = g_malloc0(sizeof(*term));
			if (ts->skip) {
				term->type = SRD_TERM_SKIP;
				term->num_samples_to_skip = ts->count;
				term->num_samples_already_skipped =
					di->abs_cur_matched ? (ts->count != 0) : 0;
			} else {
				term->type = ts->type;
				term->channel = ts->channel;
			}
			terms = g_slist_append(terms, term);
		}
		di->condition_list = g_slist_append(di->condition_list, terms);
	}
}

/* The condition-less branch of Decoder_wait() and set_skip_condition() */
static void set_no_conditions(struct sim *sim)
{
	struct srd_decoder_inst *di = &sim->di;
	struct wspec ws;
	uint64_t count;
	int same = 0;

	if (!di->first_pos && di->abs_cur_samplenum)
		count = 1;
	else if (!di->condition_list)
		count = 0;
	else
		count = 1;

	if (di->condition_list && !di->condition_list->next) {
		GSList *t = di->condition_list->data;
		struct srd_term *term = t ? t->data : NULL;
		same = t && !t->next && term->type == SRD_TERM_SKIP
			&& term->num_samples_to_skip == count;
	}
	memset(&ws, 0, sizeof(ws));
	ws.nconds = 1;
	ws.c[0].nterms = 1;
	ws.c[0].t[0].skip = 1;
	ws.c[0].t[0].count = count;
	set_conditions(sim, &ws, same);
}

static int sim_wait(struct sim *sim, const struct wspec *ws, int same_as_before, struct result *res)
{
	struct srd_decoder_inst *di = &sim->di;
	const struct capture *cap = sim->cap;
	gboolean found;
	clock_t t0;
	int c;

	memset(res, 0, sizeof(*res));
	if (sim->ended)
		return 0;
	if (ws->none)
		set_no_conditions(sim);
	else
		set_conditions(sim, ws, same_as_before);

	for (;;) {
		if (!sim->have_chunk) {
			const struct chunk *ch;
			if (sim->next_chunk == cap->nchunks) {
				sim->ended = 1;
				return 0;
			}
			/* srd_inst_decode() */
			ch = &cap->chunks[sim->next_chunk++];
			if (di->first_pos)
				di->abs_cur_samplenum = ch->start;
			if (ch->start != di->abs_cur_samplenum) {
				sim->ended = 2;
				return 0;
			}
			di->abs_start_samplenum = ch->start & ~7ULL;
			di->abs_end_samplenum = ch->end;
			di->inbuf = (const uint8_t **)ch->inbuf;
			di->inbuf_const = ch->inbuf_const;
			di->inbuflen = ch->end - ch->start;
			sim->have_chunk = 1;
		}

		found = FALSE;
		t0 = clock();
		sim->impl->process(di, &found);
		sim->seconds += (double)(clock() - t0) / CLOCKS_PER_SEC;

		if (found) {
			/* get_current_pinvalues() */
			res->found = 1;
			res->samplenum = di->abs_cur_samplenum;
			res->matched = di->match_array;
			for (c = 0; c < cap->nch; c++) {
				uint64_t rel = di->abs_cur_samplenum - di->abs_start_samplenum;
				if (cap->channelmap[c] == -1)
					res->pins[c] = 0xff;
				else if (!di->inbuf[c])
					res->pins[c] = di->inbuf_const[c] ? 1 : 0;
				else
					res->pins[c] = (di->inbuf[c][rel / 8] >> (rel % 8)) & 1;
			}
			return 1;
		}

		di->abs_start_samplenum = 0;
		di->abs_end_samplenum = 0;
		di->inbuf = NULL;
		di->inbuflen = 0;
		sim->have_chunk = 0;
	}
}

/* ---- comparison ---- */

static int failures;
static uint64_t ended_by[3];

static int same_state(const struct sim *a, const struct sim *b, char *why, size_t len)
{
	const struct srd_decoder_inst *x = &a->di, *y = &b->di;
	const GSList *l1, *l2, *t1, *t2;
	int c;

	if (a->ended != b->ended) {
		snprintf(why, len, "ended %d vs %d", a->ended, b->ended);
		return 0;
	}
	if (x->abs_cur_samplenum != y->abs_cur_samplenum) {
		snprintf(why, len, "current sample %" PRIu64 " vs %" PRIu64,
			x->abs_cur_samplenum, y->abs_cur_samplenum);
		return 0;
	}
	if (x->abs_cur_matched != y->abs_cur_matched || x->skip_zero != y->skip_zero
			|| x->first_pos != y->first_pos) {
		snprintf(why, len, "flags matched %d/%d skip_zero %d/%d first_pos %d/%d",
			x->abs_cur_matched, y->abs_cur_matched, x->skip_zero, y->skip_zero,
			x->first_pos, y->first_pos);
		return 0;
	}
	for (c = 0; c < a->cap->nch; c++) {
		if (x->old_pins_array->data[c] != y->old_pins_array->data[c]) {
			snprintf(why, len, "previous pin of channel %d: %d vs %d", c,
				x->old_pins_array->data[c], y->old_pins_array->data[c]);
			return 0;
		}
	}
	for (l1 = x->condition_list, l2 = y->condition_list; l1 || l2; l1 = l1->next, l2 = l2->next) {
		if (!l1 || !l2) {
			snprintf(why, len, "condition lists differ in length");
			return 0;
		}
		for (t1 = l1->data, t2 = l2->data; t1 || t2; t1 = t1->next, t2 = t2->next) {
			const struct srd_term *u, *v;
			if (!t1 || !t2) {
				snprintf(why, len, "conditions differ in length");
				return 0;
			}
			u = t1->data;
			v = t2->data;
			if (u->type != v->type || u->num_samples_already_skipped != v->num_samples_already_skipped) {
				snprintf(why, len, "skip counter %" PRIu64 " vs %" PRIu64,
					u->num_samples_already_skipped, v->num_samples_already_skipped);
				return 0;
			}
		}
	}
	return 1;
}

static void print_wspec(const struct wspec *ws)
{
	int i, k;
	static const char letters[] = "hlrfen";

	if (ws->none) {
		fprintf(stderr, "    wait()\n");
		return;
	}
	fprintf(stderr, "    wait([");
	for (i = 0; i < ws->nconds; i++) {
		fprintf(stderr, "%s{", i ? ", " : "");
		for (k = 0; k < ws->c[i].nterms; k++) {
			const struct tspec *t = &ws->c[i].t[k];
			if (t->skip)
				fprintf(stderr, "%s'skip': %" PRIu64, k ? ", " : "", t->count);
			else
				fprintf(stderr, "%s%d: '%c'", k ? ", " : "", t->channel,
					t->type >= 0 && t->type < 6 ? letters[t->type] : '?');
		}
		fprintf(stderr, "}");
	}
	fprintf(stderr, "])\n");
}

static int run_trial(int trial, double *seconds, uint64_t *waits, uint64_t *matches)
{
	struct capture cap;
	struct sim sims[2];
	struct wspec ws, prev;
	struct result res[2];
	uint8_t initial[MAX_CH];
	char why[200];
	int c, i, n, max_waits, same, ok = 1, have_prev = 0;

	make_capture(&cap);
	for (c = 0; c < cap.nch; c++)
		initial[c] = chance(70) ? SRD_INITIAL_PIN_SAME_AS_SAMPLE0 : (uint8_t)below(2);
	for (i = 0; i < 2; i++)
		sim_init(&sims[i], &impls[i], &cap, initial);

	max_waits = chance(50) ? 1 + (int)below(200) : 1 + (int)below(20000);
	for (n = 0; n < max_waits && !sims[0].ended; n++) {
		same = have_prev && chance(50);
		if (same)
			ws = prev;
		else
			make_wspec(&ws, cap.nch);
		for (i = 0; i < 2; i++)
			sim_wait(&sims[i], &ws, same, &res[i]);
		(*waits)++;
		*matches += res[0].found;

		why[0] = 0;
		if (res[0].found != res[1].found)
			snprintf(why, sizeof(why), "found %d vs %d", res[0].found, res[1].found);
		else if (res[0].found && res[0].samplenum != res[1].samplenum)
			snprintf(why, sizeof(why), "sample %" PRIu64 " vs %" PRIu64,
				res[0].samplenum, res[1].samplenum);
		else if (res[0].found && res[0].matched != res[1].matched)
			snprintf(why, sizeof(why), "matched 0x%" PRIx64 " vs 0x%" PRIx64,
				res[0].matched, res[1].matched);
		else if (res[0].found && memcmp(res[0].pins, res[1].pins, cap.nch))
			snprintf(why, sizeof(why), "pin values differ");
		else
			same_state(&sims[0], &sims[1], why, sizeof(why));
		if (why[0]) {
			fprintf(stderr, "FAIL trial %d, wait %d (%d channels, %" PRIu64
				" samples, %d chunks): %s\n", trial, n, cap.nch, cap.n, cap.nchunks, why);
			print_wspec(&ws);
			ok = 0;
			break;
		}
		prev = ws;
		have_prev = 1;
	}

	ended_by[sims[0].ended]++;
	for (i = 0; i < 2; i++) {
		seconds[i] += sims[i].seconds;
		sim_free(&sims[i]);
	}
	free_capture(&cap);
	return ok;
}

int main(int argc, char **argv)
{
	int trials = argc > 1 ? atoi(argv[1]) : 3000;
	uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	double seconds[2] = {0, 0};
	uint64_t waits = 0, matches = 0;
	int t;

	rng ^= seed * 0x9E3779B97F4A7C15ULL;
	if (!rng)
		rng = 1;

	for (t = 0; t < trials; t++) {
		if (!run_trial(t, seconds, &waits, &matches)) {
			failures++;
			break;
		}
	}

	printf("%d trials, %" PRIu64 " wait() calls, %" PRIu64 " matches; scripts that ran out of data: %"
		PRIu64 ", ended early: %" PRIu64 ", hit a chunk start mismatch: %" PRIu64 "\n",
		t, waits, matches, ended_by[1], ended_by[0], ended_by[2]);
	printf("matcher time: reference %.2f s, match.c %.2f s\n", seconds[0], seconds[1]);
	if (failures) {
		printf("FAILED\n");
		return 1;
	}
	printf("OK\n");
	return 0;
}
