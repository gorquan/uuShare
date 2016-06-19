#include <limits.h>

void f()
{
	int arr[ sizeof( unsigned int ) * CHAR_BIT == 64 ? 1 : -1 ];
}
