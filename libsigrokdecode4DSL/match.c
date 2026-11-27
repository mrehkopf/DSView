/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Matching the conditions of self.wait() against the sample data.
 *
 * Moved out of instance.c so that the matcher can be tested without Python
 * (DSView/tests/conditions). It makes no Python calls.
 */

#include "config.h"
#include "libsigrokdecode-internal.h" /* First, so we avoid a _POSIX_C_SOURCE warning. */
#include "libsigrokdecode.h"
#include <glib.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include "log.h"

static void match_prog_invalidate(struct srd_match_prog *prog);

/** @private */
SRD_PRIV void oldpins_array_seed(struct srd_decoder_inst *di)
{
	size_t count;
	GArray *arr;

	if (!di)
		return;
	if (di->old_pins_array)
		return;

	count = di->dec_num_channels;
	arr = g_array_sized_new(FALSE, TRUE, sizeof(uint8_t), count);
	g_array_set_size(arr, count);
	memset(arr->data, SRD_INITIAL_PIN_SAME_AS_SAMPLE0, count);
	di->old_pins_array = arr;
}

/**
 * Check whether the specified sample matches the specified term.
 *
 * In the case of SRD_TERM_SKIP, this function can modify
 * term->num_samples_already_skipped.
 *
 * @param old_sample The value of the previous sample (0/1).
 * @param sample The value of the current sample (0/1).
 * @param term The term that should be checked for a match. Must not be NULL.
 *
 * @retval TRUE The current sample matches the specified term.
 * @retval FALSE The current sample doesn't match the specified term, or an
 *               invalid term was provided.
 *
 * @private
 */
__attribute__((always_inline))
static inline gboolean sample_matches(uint8_t old_sample, uint8_t sample, struct srd_term *term)
{
	/* Caller ensures term != NULL. */

	switch (term->type) {
	case SRD_TERM_HIGH:
		if (sample == 1)
			return TRUE;
		break;
	case SRD_TERM_LOW:
		if (sample == 0)
			return TRUE;
		break;
	case SRD_TERM_RISING_EDGE:
		if (old_sample == 0 && sample == 1)
			return TRUE;
		break;
	case SRD_TERM_FALLING_EDGE:
		if (old_sample == 1 && sample == 0)
			return TRUE;
		break;
	case SRD_TERM_EITHER_EDGE:
		if ((old_sample == 1 && sample == 0) || (old_sample == 0 && sample == 1))
			return TRUE;
		break;
	case SRD_TERM_NO_EDGE:
		if ((old_sample == 0 && sample == 0) || (old_sample == 1 && sample == 1))
			return TRUE;
		break;
	case SRD_TERM_SKIP:
		if (term->num_samples_already_skipped == term->num_samples_to_skip)
			return TRUE;
		term->num_samples_already_skipped++;
		break;
	default:
		srd_err("Unknown term type %d.", term->type);
		break;
	}

	return FALSE;
}

/** @private */
SRD_PRIV void condition_list_free(struct srd_decoder_inst *di)
{
	GSList *l, *ll;

	if (!di)
		return;

	for (l = di->condition_list; l; l = l->next) {
		ll = l->data;
		if (ll)
			g_slist_free_full(ll, g_free);
	}

    g_slist_free(di->condition_list);
	di->condition_list = NULL;

	/* Kept for the next condition list, see struct srd_match_prog. */
	match_prog_invalidate(di->match_prog);
}

/**
 * Start the skip terms of the current conditions over, the way a new
 * condition list for the same conditions would start them. For wait()
 * calls that keep the condition list (type_decoder.c).
 *
 * @private
 */
SRD_PRIV void condition_list_reset_skips(struct srd_decoder_inst *di)
{
	GSList *l, *t;
	struct srd_term *term;

	for (l = di->condition_list; l; l = l->next) {
		for (t = l->data; t; t = t->next) {
			term = t->data;
			if (term->type == SRD_TERM_SKIP)
				term->num_samples_already_skipped = di->abs_cur_matched
					? (term->num_samples_to_skip != 0) : 0;
		}
	}
}

static gboolean have_non_null_conds(const struct srd_decoder_inst *di)
{
	GSList *l, *cond;

	if (!di)
		return FALSE;

	for (l = di->condition_list; l; l = l->next) {
		cond = l->data;
		if (cond)
			return TRUE;
	}

	return FALSE;
}

static void update_old_pins_array(struct srd_decoder_inst *di)
{
	uint8_t sample;
    int i, bit_offset;
    const uint8_t *sample_pos;

    if (!di || !di->dec_channelmap)
		return;

	oldpins_array_seed(di);
	for (i = 0; i < di->dec_num_channels; i++) {
        if (*(di->inbuf + i) == NULL) {
            sample = *(di->inbuf_const + i) ? 1 : 0;
            di->old_pins_array->data[i] = sample;
        } else {
            sample_pos = *(di->inbuf + i) + ((di->abs_cur_samplenum - di->abs_start_samplenum) / 8);
            bit_offset = (di->abs_cur_samplenum - di->abs_start_samplenum) % 8;
            sample = *sample_pos & (1 << bit_offset) ? 1 : 0;
            di->old_pins_array->data[i] = sample;
        }
	}
}

static void update_old_pins_array_initial_pins(struct srd_decoder_inst *di)
{
	uint8_t sample;
    int i, bit_offset;
	const uint8_t *sample_pos;

	if (!di || !di->dec_channelmap)
		return;

	oldpins_array_seed(di);
	for (i = 0; i < di->dec_num_channels; i++) {
		if (di->old_pins_array->data[i] != SRD_INITIAL_PIN_SAME_AS_SAMPLE0)
			continue;

        if (*(di->inbuf + i) == NULL) {
            sample = *(di->inbuf_const + i) ? 1 : 0;
            di->old_pins_array->data[i] = sample;
        } else {
            sample_pos = *(di->inbuf + i) + ((di->abs_cur_samplenum - di->abs_start_samplenum) / 8);
            bit_offset = (di->abs_cur_samplenum - di->abs_start_samplenum) % 8;
            sample = *sample_pos & (1 << bit_offset) ? 1 : 0;
            di->old_pins_array->data[i] = sample;
        }
	}
}

static gboolean term_matches(struct srd_decoder_inst *di,
        struct srd_term *term, gboolean *skip_allow)
{
	uint8_t old_sample, sample;
    int bit_offset, ch;
    const uint8_t *sample_pos;

	/* Caller ensures di, di->dec_channelmap, term, sample_pos != NULL. */

    *skip_allow = FALSE;
    if (term->type == SRD_TERM_SKIP) {
        if (di->abs_cur_matched && term->num_samples_to_skip == 0)
            di->skip_zero = TRUE;
		return sample_matches(0, 0, term);
    }

	ch = term->channel;
    if (*(di->inbuf + ch) == NULL) {
        sample = *(di->inbuf_const + ch) ? 1 : 0;
        *skip_allow = TRUE;
    } else {
        sample_pos = *(di->inbuf + ch) + ((di->abs_cur_samplenum - di->abs_start_samplenum) / 8);
        bit_offset = (di->abs_cur_samplenum - di->abs_start_samplenum) % 8;
        sample = *sample_pos & (1 << bit_offset) ? 1 : 0;
    }
	old_sample = di->old_pins_array->data[ch];

	return sample_matches(old_sample, sample, term);
}

static gboolean all_terms_match(struct srd_decoder_inst *di,
        const GSList *cond, gboolean *skip_allow)
{
	const GSList *l;
	struct srd_term *term;

	/* Caller ensures di, cond, sample_pos != NULL. */

	for (l = cond; l; l = l->next) {
		term = l->data;
        if (!term_matches(di, term, skip_allow))
			return FALSE;
	}

    if (di->skip_zero) {
        di->abs_cur_samplenum--;
        di->skip_zero = FALSE;
    }
	return TRUE;
}


/*
 * Word-wise scan.
 *
 * find_match() checks one sample at a time: every condition, every term,
 * for every sample, which dominates the decode time of captures with many
 * samples between matches or many conditions (a parallel bus without a
 * clock waits for an edge on any of its lines).
 *
 * Once the first sample of a find_match() call has been checked, the
 * per-sample loop keeps little state between samples: the previous pin
 * values and the counters of skip terms. When every condition consists only
 * of pin terms (h, l, r, f, e, n), or of a single skip term, match_scan()
 * finds the first sample at which any condition holds, 64 samples at a time,
 * and match_skip_to() brings that state up to that sample. The per-sample
 * code then checks it as before, so results are exactly those of the
 * per-sample loop. Conditions that mix skip and pin terms (their skip counter
 * only advances on samples where the pin terms before it hold) keep the
 * per-sample loop.
 */

struct match_pin_term {
	int type;
	int channel;
};

struct srd_match_prog {
	/* Compiled from the current condition list; cleared when it is freed. */
	gboolean valid;
	/* FALSE: some condition needs the per-sample loop. */
	gboolean usable;
	/* Conditions of pin terms only; condition i ends at terms[cond_ends[i]]. */
	int num_pin_conds;
	int *cond_ends;
	struct match_pin_term *terms;
	/* Conditions that are a single skip term (read live, they count). */
	int num_skip_terms;
	struct srd_term **skip_terms;
	/* Channels the pin terms use. */
	int num_channels;
	int *channels;
	/* Per decoder channel: the current word, the previous samples' word,
	 * the last sample of the previous word, and a scratch flag. */
	uint64_t *cur;
	uint64_t *prev;
	uint64_t *top;
	gboolean *used;
	/* Allocated lengths. The program lives as long as the decoder
	 * instance, so decoders whose conditions change on every wait()
	 * (a skip count per UART bit) don't allocate here. */
	int max_conds, max_terms, max_skips, max_channels;
};

static void match_prog_invalidate(struct srd_match_prog *prog)
{
	if (prog)
		prog->valid = FALSE;
}

/** @private */
SRD_PRIV void match_prog_destroy(struct srd_decoder_inst *di)
{
	struct srd_match_prog *prog = di->match_prog;

	if (!prog)
		return;
	g_free(prog->cond_ends);
	g_free(prog->terms);
	g_free(prog->skip_terms);
	g_free(prog->channels);
	g_free(prog->cur);
	g_free(prog->prev);
	g_free(prog->top);
	g_free(prog->used);
	g_free(prog);
	di->match_prog = NULL;
}

static gboolean is_pin_term(int type)
{
	switch (type) {
	case SRD_TERM_HIGH:
	case SRD_TERM_LOW:
	case SRD_TERM_RISING_EDGE:
	case SRD_TERM_FALLING_EDGE:
	case SRD_TERM_EITHER_EDGE:
	case SRD_TERM_NO_EDGE:
		return TRUE;
	default:
		return FALSE;
	}
}

/* Compile di->condition_list into di->match_prog. */
static void match_prog_compile(struct srd_decoder_inst *di)
{
	struct srd_match_prog *prog = di->match_prog;
	const GSList *l, *t;
	const struct srd_term *term;
	int num_conds = 0, num_terms = 0, num_skips = 0, n, k;

	if (!prog)
		prog = di->match_prog = g_malloc0(sizeof(*prog));
	prog->valid = TRUE;
	prog->usable = FALSE;
	prog->num_pin_conds = prog->num_skip_terms = prog->num_channels = 0;

	for (l = di->condition_list; l; l = l->next) {
		/* An empty condition never matches, find_match() skips it. */
		if (!(t = l->data))
			continue;
		term = t->data;
		if (!t->next && term->type == SRD_TERM_SKIP) {
			num_skips++;
			continue;
		}
		for (; t; t = t->next) {
			term = t->data;
			if (!is_pin_term(term->type) || term->channel < 0
					|| term->channel >= di->dec_num_channels)
				return;
			num_terms++;
		}
		num_conds++;
	}

	if (num_conds > prog->max_conds) {
		prog->cond_ends = g_renew(int, prog->cond_ends, num_conds);
		prog->max_conds = num_conds;
	}
	if (num_terms > prog->max_terms) {
		prog->terms = g_renew(struct match_pin_term, prog->terms, num_terms);
		prog->max_terms = num_terms;
	}
	if (num_skips > prog->max_skips) {
		prog->skip_terms = g_renew(struct srd_term *, prog->skip_terms, num_skips);
		prog->max_skips = num_skips;
	}
	n = di->dec_num_channels;
	if (n > prog->max_channels) {
		prog->channels = g_renew(int, prog->channels, n);
		prog->cur = g_renew(uint64_t, prog->cur, n);
		prog->prev = g_renew(uint64_t, prog->prev, n);
		prog->top = g_renew(uint64_t, prog->top, n);
		prog->used = g_renew(gboolean, prog->used, n);
		memset(prog->used, 0, n * sizeof(gboolean));
		prog->max_channels = n;
	}

	num_terms = 0;
	for (l = di->condition_list; l; l = l->next) {
		if (!(t = l->data))
			continue;
		term = t->data;
		if (!t->next && term->type == SRD_TERM_SKIP) {
			prog->skip_terms[prog->num_skip_terms++] = (struct srd_term *)term;
			continue;
		}
		for (; t; t = t->next) {
			term = t->data;
			prog->terms[num_terms].type = term->type;
			prog->terms[num_terms].channel = term->channel;
			num_terms++;
			if (!prog->used[term->channel]) {
				prog->used[term->channel] = TRUE;
				prog->channels[prog->num_channels++] = term->channel;
			}
		}
		prog->cond_ends[prog->num_pin_conds++] = num_terms;
	}
	for (k = 0; k < prog->num_channels; k++)
		prog->used[prog->channels[k]] = FALSE;

	prog->usable = TRUE;
}

/* The 64 samples of a channel's chunk buffer from sample 64 * w on (relative
 * to the buffer), bit n = sample 64 * w + n. Reads no byte at or past nbytes. */
static inline uint64_t load_word(const uint8_t *buf, uint64_t w, uint64_t nbytes)
{
	uint64_t off = w * 8, v = 0;
	unsigned int i;

	if (off + 8 <= nbytes) {
		memcpy(&v, buf + off, 8);
		return GUINT64_FROM_LE(v);
	}
	for (i = 0; off + i < nbytes; i++)
		v |= (uint64_t)buf[off + i] << (8 * i);
	return v;
}

static inline uint64_t term_bits(int type, uint64_t cur, uint64_t prev)
{
	switch (type) {
	case SRD_TERM_HIGH:
		return cur;
	case SRD_TERM_LOW:
		return ~cur;
	case SRD_TERM_RISING_EDGE:
		return ~prev & cur;
	case SRD_TERM_FALLING_EDGE:
		return prev & ~cur;
	case SRD_TERM_EITHER_EDGE:
		return prev ^ cur;
	default: /* SRD_TERM_NO_EDGE */
		return ~(prev ^ cur);
	}
}

/*
 * The first sample in [from, abs_end_samplenum) at which a condition of the
 * program holds, or abs_end_samplenum. Sample from - 1 must lie in the
 * current chunk: the edges at sample from are against it.
 */
static uint64_t match_scan(const struct srd_decoder_inst *di,
		struct srd_match_prog *prog, uint64_t from)
{
	uint64_t limit, at, base, nbytes, rel, rel_end, w, wstart, valid, any, v, m;
	const uint8_t *buf;
	int i, k, c, t;

	/* A skip term matches once its counter reaches the count. */
	limit = di->abs_end_samplenum;
	for (i = 0; i < prog->num_skip_terms; i++) {
		const struct srd_term *term = prog->skip_terms[i];
		at = from + (term->num_samples_to_skip - term->num_samples_already_skipped);
		if (at < limit)
			limit = at;
	}
	if (prog->num_pin_conds == 0 || limit <= from)
		return limit;

	base = di->abs_start_samplenum;
	nbytes = (di->abs_end_samplenum - base + 7) / 8;
	rel = from - base;
	rel_end = limit - base;
	w = rel / 64;

	for (k = 0; k < prog->num_channels; k++) {
		c = prog->channels[k];
		buf = di->inbuf[c];
		if (!buf) {
			/* Constant over the chunk, and sample from - 1 is in it. */
			prog->cur[c] = prog->prev[c] = di->inbuf_const[c] ? ~0ULL : 0;
		} else {
			/* The sample before the first word. Only used when that
			 * word starts at sample from, so it is in the chunk. */
			prog->top[c] = (w > 0) ? (buf[w * 8 - 1] >> 7) & 1 : 0;
		}
	}

	for (; (wstart = w * 64) < rel_end; w++) {
		valid = ~0ULL;
		if (rel > wstart)
			valid &= ~0ULL << (rel - wstart);
		if (rel_end - wstart < 64)
			valid &= ~(~0ULL << (rel_end - wstart));

		for (k = 0; k < prog->num_channels; k++) {
			c = prog->channels[k];
			if (!(buf = di->inbuf[c]))
				continue;
			v = load_word(buf, w, nbytes);
			prog->prev[c] = (v << 1) | prog->top[c];
			prog->top[c] = v >> 63;
			prog->cur[c] = v;
		}

		any = 0;
		t = 0;
		for (i = 0; i < prog->num_pin_conds; i++) {
			m = valid;
			for (; t < prog->cond_ends[i]; t++) {
				c = prog->terms[t].channel;
				m &= term_bits(prog->terms[t].type, prog->cur[c], prog->prev[c]);
			}
			any |= m;
		}
		if (any)
			return base + wstart + __builtin_ctzll(any);
	}

	return limit;
}

/*
 * Bring the per-sample state from abs_cur_samplenum to sample p, as if the
 * samples in between had been checked without a match.
 */
static void match_skip_to(struct srd_decoder_inst *di,
		const struct srd_match_prog *prog, uint64_t p)
{
	uint64_t n = p - di->abs_cur_samplenum;
	int i;

	for (i = 0; i < prog->num_skip_terms; i++)
		prog->skip_terms[i]->num_samples_already_skipped += n;

	di->abs_cur_samplenum = p - 1;
	update_old_pins_array(di);
	di->abs_cur_samplenum = p;
}

static gboolean 
find_match(struct srd_decoder_inst *di)
{
    uint64_t j, p;
	GSList *l, *cond;
    gboolean skip_allow;
    gboolean all_skip_allow = TRUE;
    gboolean first_sample = TRUE;

	/* Caller ensures di != NULL. */

	/* Check whether the condition list is NULL/empty. */
    if (!di->condition_list) {
        srd_dbg("NULL/empty condition list, automatic match.");
        return TRUE;
    }

	/* Check whether we have any non-NULL conditions. */
    if (!have_non_null_conds(di)) {
        srd_dbg("Only NULL conditions in list, automatic match.");
        return TRUE;
    }

    /* di->match_array is 0 here. Create a new GArray. */
    di->match_array = 0;

	/* Sample 0: Set di->old_pins_array for SRD_INITIAL_PIN_SAME_AS_SAMPLE0 pins. */
    if (di->first_pos) {
        di->first_pos = FALSE;
		update_old_pins_array_initial_pins(di);
    }

    if (di->abs_cur_matched)
        di->abs_cur_samplenum++;

    while (di->abs_cur_samplenum < di->abs_end_samplenum) {

        /*
         * After the first sample, jump to the next sample at which a
         * condition holds (see match_scan()). The first sample keeps the
         * per-sample path: it may follow a chunk boundary or a match.
         */
        if (!first_sample) {
            if (!di->match_prog || !di->match_prog->valid)
                match_prog_compile(di);
            if (di->match_prog->usable) {
                p = match_scan(di, di->match_prog, di->abs_cur_samplenum);
                if (p > di->abs_cur_samplenum) {
                    match_skip_to(di, di->match_prog, p);
                    if (p >= di->abs_end_samplenum)
                        break;
                }
            }
        }
        first_sample = FALSE;

        /* Check whether the current sample matches at least one of the conditions (logical OR). */
        /* IMPORTANT: We need to check all conditions, even if there was a match already! */
        for (l = di->condition_list, j = 0; l; l = l->next, j++) {
            cond = l->data;
            if (!cond)
                continue;

            /* All terms in 'cond' must match (logical AND). */
            if (all_terms_match(di, cond, &skip_allow)) {
                all_skip_allow = FALSE;
                if (j < 64)
                    di->match_array |= (1ULL << j);
            } else {
                all_skip_allow &= skip_allow;
            }
        }

        update_old_pins_array(di);

        /* If at least one condition matched we're done. */
        di->abs_cur_matched = (di->match_array != 0);
        if (di->abs_cur_matched)
            return TRUE;

        if (all_skip_allow)
            di->abs_cur_samplenum = di->abs_end_samplenum;
        else
            di->abs_cur_samplenum++;
    }

	return FALSE;
}

/**
 * Process available samples and check if they match the defined conditions.
 *
 * This function returns if there is an error, or when a match is found, or
 * when all samples have been processed (whether a match was found or not).
 * This function immediately terminates when the decoder's wait() method
 * invocation shall get terminated.
 *
 * @param di The decoder instance to use. Must not be NULL.
 * @param found_match Will be set to TRUE if at least one condition matched,
 *                    FALSE otherwise. Must not be NULL.
 *
 * @retval SRD_OK No errors occured, see found_match for the result.
 * @retval SRD_ERR_ARG Invalid arguments.
 *
 * @private
 */
SRD_PRIV int process_samples_until_condition_match(struct srd_decoder_inst *di, gboolean *found_match)
{
	if (!di || !found_match)
		return SRD_ERR_ARG;

	*found_match = FALSE;
	if (di->want_wait_terminate)
		return SRD_OK;

	/* Check if any of the current condition(s) match. */
	while (TRUE) {
		/* Feed the (next chunk of the) buffer to find_match(). */
        *found_match = find_match(di);

		/* Did we handle all samples yet? */
        if (di->abs_cur_samplenum >= di->abs_end_samplenum) {
			srd_dbg("Done, handled all samples (abs cur %" PRIu64
				" / abs end %" PRIu64 ").",
				di->abs_cur_samplenum, di->abs_end_samplenum);
			return SRD_OK;
		}

		/* If we didn't find a match, continue looking. */
		if (!(*found_match))
			continue;

		/* At least one condition matched, return. */
		return SRD_OK;
	}

	return SRD_OK;
}
