/** @file Public configuration and entry point for the experiment. */
#ifndef TWINCEPTION_SRC_APP_H_
#define TWINCEPTION_SRC_APP_H_

#include <stdint.h>

enum app_history_mode {
	APP_HISTORY_SPLIT,
	APP_HISTORY_SHARED_A,
	APP_HISTORY_SHARED_B
};

struct app_cfg {
	char const *url[2];
	char const *model[2];
	char const *system;
	char const *prompt;
	double      temperature;
	long        max_tokens;
	long        crosstalk_rounds;
	uint32_t    rapid_quantum;
	uint32_t    rapid_budget;
	enum app_history_mode history_mode;
	unsigned    debug : 1;
	unsigned    template_check : 1;
};

extern int
app_run (struct app_cfg const *cfg);

#endif /* TWINCEPTION_SRC_APP_H_ */
