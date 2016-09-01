#include <stdio.h>
#include <stdlib.h>
#include "../../include/cdecode.h"
int main(int argc, char **argv)
{

	const char binput[] = "ab6aa2K5uhY+kBcodeoJ6/Ny+b8tuXhztYxCDbpQx8Q=";

	base64_decodestate state_in;

	char *buf = calloc(1, 33);
	buf[32] = 'B';

	base64_init_decodestate(&state_in);

	int retval = base64_decode_block(binput, sizeof(binput),
			buf, &state_in);

	fprintf(stderr, "retval: %i\n", retval);
	write(1, buf, retval);

	return 0;
}

