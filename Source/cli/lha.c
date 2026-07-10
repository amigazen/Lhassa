/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha.c - LhA 2.x-compatible command-line archiver front end.
 */

#include <stdio.h>
#include <stdlib.h>

#include "lha_platform.h"
#include "lha_internal.h"

#ifdef __AMIGADATE__
const char version_tag[] = "\0$VER: Lhassa 2.15 (02.07.26)";
const char stack_cookie[] = "$STACK: 65536";
#endif

int main(int argc, char **argv)
{
    lha_args args;
    char *unknown;
    int rc;

    rc = lha_parse_args(argc, argv, &args, &unknown);
    if (rc < 0) {
        if (unknown) {
            fprintf(stderr, "\nUnknown option: %s\n", unknown);
            free(unknown);
        }
        lha_usage(argv[0]);
        return 1;
    }
    if (rc == 0) {
        lha_usage(argv[0]);
        return 1;
    }

    rc = lha_run_command(&args);
    lha_args_free(&args);
#ifndef HOST
    lh_utility_shutdown();
#endif
    return rc;
}
