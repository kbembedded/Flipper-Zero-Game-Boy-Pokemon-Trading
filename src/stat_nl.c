#include <src/include/named_list.h>
#include <src/include/stat_nl.h>

static const NamedList stat_list[] = {
    {"Random IV, Zero EV", RANDIV_ZEROEV, 0},
    {"Random IV, Max EV / Level", RANDIV_LEVELEV, 0},
    {"Random IV, Max EV", RANDIV_MAXEV, 0},
    {"Max IV, Zero EV", MAXIV_ZEROEV, 0},
    {"Max IV, Max EV / Level", MAXIV_LEVELEV, 0},
    {"Max IV, Max EV", MAXIV_MAXEV, 0},
    {},
};

const NamedList* stat_nl_pointer_get() {
	return stat_list;
}
