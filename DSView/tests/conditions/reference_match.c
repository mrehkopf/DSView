/*
 * The condition matcher as it was before the word-wise scan (instance.c,
 * libsigrokdecode4DSL), kept verbatim as the reference for conditiontest.c.
 * Only changes: functions renamed with a ref_ prefix, made non-static where the
 * test calls them, and the match_array bit uses 1ULL (the old "1 << j" was
 * undefined from 31 conditions on).
 */

#include "libsigrokdecode-internal.h"
#include "libsigrokdecode.h"
#include <glib.h>
#include <inttypes.h>
#include "log.h"
#include "reference_match.h"

void ref_oldpins_array_seed(struct srd_decoder_inst *di)
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

__attribute__((always_inline))
static inline gboolean ref_sample_matches(uint8_t old_sample, uint8_t sample, struct srd_term *term)
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
void ref_condition_list_free(struct srd_decoder_inst *di)
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
}

static gboolean ref_have_non_null_conds(const struct srd_decoder_inst *di)
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

static void ref_update_old_pins_array(struct srd_decoder_inst *di)
{
	uint8_t sample;
    int i, bit_offset;
    const uint8_t *sample_pos;

    if (!di || !di->dec_channelmap)
		return;

	ref_oldpins_array_seed(di);
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

static void ref_update_old_pins_array_initial_pins(struct srd_decoder_inst *di)
{
	uint8_t sample;
    int i, bit_offset;
	const uint8_t *sample_pos;

	if (!di || !di->dec_channelmap)
		return;

	ref_oldpins_array_seed(di);
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

static gboolean ref_term_matches(struct srd_decoder_inst *di,
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
		return ref_sample_matches(0, 0, term);
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

	return ref_sample_matches(old_sample, sample, term);
}

static gboolean ref_all_terms_match(struct srd_decoder_inst *di,
        const GSList *cond, gboolean *skip_allow)
{
	const GSList *l;
	struct srd_term *term;

	/* Caller ensures di, cond, sample_pos != NULL. */

	for (l = cond; l; l = l->next) {
		term = l->data;
        if (!ref_term_matches(di, term, skip_allow))
			return FALSE;
	}

    if (di->skip_zero) {
        di->abs_cur_samplenum--;
        di->skip_zero = FALSE;
    }
	return TRUE;
}

static gboolean 
ref_find_match(struct srd_decoder_inst *di)
{
    uint64_t j;
	GSList *l, *cond;
    gboolean skip_allow;
    gboolean all_skip_allow = TRUE;

	/* Caller ensures di != NULL. */

	/* Check whether the condition list is NULL/empty. */
    if (!di->condition_list) {
        srd_dbg("NULL/empty condition list, automatic match.");
        return TRUE;
    }

	/* Check whether we have any non-NULL conditions. */
    if (!ref_have_non_null_conds(di)) {
        srd_dbg("Only NULL conditions in list, automatic match.");
        return TRUE;
    }

    /* di->match_array is 0 here. Create a new GArray. */
    di->match_array = 0;

	/* Sample 0: Set di->old_pins_array for SRD_INITIAL_PIN_SAME_AS_SAMPLE0 pins. */
    if (di->first_pos) {
        di->first_pos = FALSE;
		ref_update_old_pins_array_initial_pins(di);
    }

    if (di->abs_cur_matched)
        di->abs_cur_samplenum++;

    while (di->abs_cur_samplenum < di->abs_end_samplenum) {

        /* Check whether the current sample matches at least one of the conditions (logical OR). */
        /* IMPORTANT: We need to check all conditions, even if there was a match already! */
        for (l = di->condition_list, j = 0; l; l = l->next, j++) {
            cond = l->data;
            if (!cond)
                continue;

            /* All terms in 'cond' must match (logical AND). */
            if (ref_all_terms_match(di, cond, &skip_allow)) {
                all_skip_allow = FALSE;
                if (j < 64)
                    di->match_array |= (1ULL << j);
            } else {
                all_skip_allow &= skip_allow;
            }
        }

        ref_update_old_pins_array(di);

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
int ref_process_samples_until_condition_match(struct srd_decoder_inst *di, gboolean *found_match)
{
	if (!di || !found_match)
		return SRD_ERR_ARG;

	*found_match = FALSE;
	if (di->want_wait_terminate)
		return SRD_OK;

	/* Check if any of the current condition(s) match. */
	while (TRUE) {
		/* Feed the (next chunk of the) buffer to ref_find_match(). */
        *found_match = ref_find_match(di);

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
