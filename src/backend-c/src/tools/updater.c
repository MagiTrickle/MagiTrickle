#include <string.h>
#include "magitrickle/update.h"

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--worker")) { return 2; }
    return mt_update_worker();
}
