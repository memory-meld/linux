#ifndef __MM_HAGENT_SDH_H
#define __MM_HAGENT_SDH_H

#include "rheap.h"

struct sdh;
struct sdh *sdh_new(unsigned long w, unsigned long d, unsigned long k);
void sdh_drop(struct sdh *sdh);
unsigned sdh_get(struct sdh *sdh, unsigned long address);
unsigned sdh_add(struct sdh *sdh, unsigned long address);

void sdh_show_topk(struct sdh *sdh);

#endif
