#include <stdio.h>
#include <stdlib.h>

int main() {
	FILE *f;
	char c[32];

	f=fopen("/apps/nastools-zerotier-one/var/authtoken.secret","r");
	if(!f)
		return 1;

	printf("Content-Type: text/plain\n\n");

	size_t ret = fread(c, 1, sizeof(c), f);
	if(!ret)
		return 1;
	fwrite(c, ret, 1, stdout);

	fclose(f);
	return 0;
}
