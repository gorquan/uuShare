#include <limits.h>

void f()
{
	int arr[ sizeof( unsigned int ) * CHAR_BIT == 32 ? 1 : -1 ];
}
