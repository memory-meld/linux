#ifndef __MM_HAGENT_RHEAP_H
#define __MM_HAGENT_RHEAP_H

#include <linux/types.h>
#define RHEAP_HASH_TOMESTONE (0x2ea1deadbeef)

typedef unsigned long rhkey_t;
typedef unsigned long rhvalue_t;

struct rheap;

struct rheap *rheap_new(unsigned long cap);
void rheap_drop(struct rheap *rh);
unsigned long rheap_len(struct rheap *rh);
void rheap_show_all(struct rheap *rh);

rhvalue_t rheap_get(struct rheap *rh, rhkey_t const key);
void rheap_peek(struct rheap const *rh, rhkey_t *key, rhvalue_t *value);
bool rheap_full(struct rheap const *rh);
// must call rheap_full() to ensure has space to push
void rheap_push(struct rheap *rh, rhkey_t const key, rhvalue_t const value);
// replace is for the special insertion that happens right after a deletion
bool rheap_replace(struct rheap *rh, rhkey_t const old_key, rhkey_t const key,
		   rhvalue_t const value);
bool rheap_update(struct rheap *rh, rhkey_t const key, rhvalue_t const value);
bool rheap_delete(struct rheap *rh, rhkey_t const key);

// void rheap_update(struct rheap *rh, rhde_t const *e);
// void rheap_replace(struct rheap *rh, rhde_t const *old, rhde_t const *new);

#endif
