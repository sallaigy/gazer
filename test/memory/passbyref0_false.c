//@expect fail
#include <stddef.h>

void klee_make_symbolic(void *addr, size_t nbytes, const char *name);
int __VERIFIER_nondet_int(void);
void __VERIFIER_error(void) __attribute__((noreturn));
void update(int* val);

int main(void)
{
    int loc = 0;

    klee_make_symbolic(&loc, sizeof(loc), "loc");

    if (loc != 0) {
        __VERIFIER_error();
    }

    return 0;
}
