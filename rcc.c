/*
 * MOC - music on console
 * Copyright (C) 2005,2011 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>

#ifdef HAVE_RCC
# include <librcc.h>
#endif

#include <assert.h>

#include "rcc.h"

char *rcc_reencode (char *str)
{
	char *result = str;

	assert (str != NULL);

#ifdef HAVE_RCC
	rcc_string rccstring;

	rccstring = rccFrom (NULL, 0, str);
	if (rccstring) {
		if (*rccstring) {
			char *reencoded;

			reencoded = rccToCharset (NULL, "UTF-8", rccstring);
			if (reencoded) {
		    	free (result);
		    	result = reencoded;
			}
		}

		free (rccstring);
	}
#endif /* HAVE_RCC */

	return result;
}

void rcc_init ()
{
#ifdef HAVE_RCC
	rcc_class classes[] = {
		{"input", RCC_CLASS_STANDARD, NULL, NULL, "Input Encoding", 0},
		{"output", RCC_CLASS_KNOWN, NULL, NULL, "Output Encoding", 0},
		{NULL, 0, NULL, NULL, NULL, 0}
	};

	rccInit ();
	rccInitDefaultContext (NULL, 0, 0, classes, 0);
	rccLoad (NULL, "moc");
	rccSetOption (NULL, RCC_OPTION_TRANSLATE,
	                    RCC_OPTION_TRANSLATE_SKIP_PARRENT);
	rccSetOption (NULL, RCC_OPTION_AUTODETECT_LANGUAGE, 1);
#endif /* HAVE_RCC */
}

void rcc_cleanup ()
{
#ifdef HAVE_RCC
	rccFree ();
#endif /* HAVE_RCC */
}
