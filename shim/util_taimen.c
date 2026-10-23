/* The compiler may emit calls to these for struct copies and zeroing (MMU off: byte stores). */
#include <stdint.h>

void *memset(void *p, int c, unsigned long n)
{
	volatile uint8_t *b = p;
	for (unsigned long i = 0; i < n; i++)
		b[i] = (uint8_t)c;
	return p;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
	volatile uint8_t *d = dst;
	const volatile uint8_t *s = src;
	for (unsigned long i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}
