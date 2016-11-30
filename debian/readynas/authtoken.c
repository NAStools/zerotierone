#include <stdio.h>
#include <stdlib.h>

int main() {
	FILE *f;
	char c;

	f=fopen("/apps/nastools-zerotier-one/var/authtoken.secret","rt");

	printf("Content-Type: text/plain\n\n");

	while ((c=fgetc(f))!=EOF)
		printf("%c", c);

	fclose(f);
	return 0;
}
