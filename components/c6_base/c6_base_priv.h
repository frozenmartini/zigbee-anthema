#pragma once
/* c6_base, private to the component: what its own files share and an image never sees. */
#include <stdbool.h>

/* The station has an address (true) or lost it (false): the rollback check waits on it. */
void c6_base_got_ip(bool up);
