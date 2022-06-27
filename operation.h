#ifndef OPERATION_H
#define OPERATION_H


#include <stdint.h>
#include "def_helper.h"


int uszram_init(void);

int uszram_exit(void);

int uszram_kv_put(kv_item item);

kv_item * uszram_kv_get(uint_least64_t key);

void print_status(void);


#endif
