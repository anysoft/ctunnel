#include <stdlib.h>

int main(void) {
    void *allocation = malloc(1U);
    free(allocation);
    return allocation == NULL;
}
