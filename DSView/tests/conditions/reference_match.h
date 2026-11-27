#ifndef REFERENCE_MATCH_H
#define REFERENCE_MATCH_H

#include <glib.h>

struct srd_decoder_inst;

void ref_oldpins_array_seed(struct srd_decoder_inst *di);
void ref_condition_list_free(struct srd_decoder_inst *di);
int ref_process_samples_until_condition_match(struct srd_decoder_inst *di, gboolean *found_match);

#endif
