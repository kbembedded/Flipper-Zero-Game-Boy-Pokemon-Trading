#ifndef STAT_NL_H
#define STAT_NL_H

#pragma once

const NamedList* stat_nl_pointer_get(void);

typedef enum {
    RANDIV_ZEROEV,
    RANDIV_LEVELEV,
    RANDIV_MAXEV,
    MAXIV_ZEROEV,
    MAXIV_LEVELEV,
    MAXIV_MAXEV,
} EvIv;

#endif // STAT_NL_H
