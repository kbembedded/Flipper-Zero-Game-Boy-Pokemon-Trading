#include <stdint.h>

/* This is quite a brute force operation, starting from 1, square, and compare
 * to val. Once it exceeds val, that means we've found ceil(sqrt(val)). This
 * is actually the same method that pokemon Gen I & II use to calculate sqrt().
 */
uint32_t tiny_sqrt(uint32_t val)
{
	uint32_t i;
	uint32_t tmp;

	if (val == 0)
		return 0;

	for (i = 1; ; i++) {
		tmp = i * i;
		if (tmp >= val)
			return i;
	}
}
