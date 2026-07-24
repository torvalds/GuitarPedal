#ifndef STATUS_H
#define STATUS_H

const char *current_status = "Booting";

static inline void report_status(const char *msg)
{
	current_status = msg;
}

static const char *get_status(void)
{
	return __atomic_exchange_n(&current_status, NULL, __ATOMIC_RELAXED);
}

#endif
