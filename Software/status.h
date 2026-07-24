#ifndef STATUS_H
#define STATUS_H

const char *current_status = "Booting";

static inline void report_status(const char *msg)
{
	current_status = msg;
}

// The difference between "report status" and "report info" is that
// informational messages will not overwrite existing pending
// messages. So they update the current status only if it was NULL.
static inline void report_info(const char *msg)
{
	const char *no_message = NULL;
	__atomic_compare_exchange_n(&current_status, &no_message, msg,
		false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static const char *get_status(void)
{
	return __atomic_exchange_n(&current_status, NULL, __ATOMIC_RELAXED);
}

#endif
