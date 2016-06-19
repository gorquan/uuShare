#include <limits.h>

void f()
{
	int arr[ sizeof( long ) * CHAR_BIT == 64 ? 1 : -1 ];
}
