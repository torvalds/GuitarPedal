static unsigned int switch_val;

#define switch_pressed(x) (!!(switch_val & (1<<(x))))
#define switch_clear(x) __atomic_and_fetch(&switch_val, ~(1<<(x)), __ATOMIC_RELAXED)

#define LONGPRESS(x) ((x)+16)
