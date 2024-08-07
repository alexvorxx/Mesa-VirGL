/*
 * Copyright © 2024 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

void compiler_rs_free(void *ptr);
long compiler_rs_ftell(FILE *f);
int compiler_rs_fseek(FILE *f, long offset, int whence);
