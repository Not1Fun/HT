/* @brief 只替代时钟、线程锁、硬件和shell注册；被测对象为生产dds_bench.c。 */
#include "app/dds_bench.h"
#include "platform/dds_io.h"
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int64_t now;
static struct dds_snapshot hardware;
static unsigned int configure_calls, start_calls, stop_calls, fault_calls;
static int init_error, configure_error, start_error, check_error, snapshot_error, stop_error;
static bool bad_running, bad_frequency;
static void (*configure_hook)(void), (*start_hook)(void), (*snapshot_hook)(void), (*stop_hook)(void);
static void (*check_hook)(void), (*unlock_hook)(void);
static unsigned int held_locks;

k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
	assert(lock->held == 0);
	lock->held = 1;
	++held_locks;
	return 0;
}

void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	(void)key;
	assert(lock->held == 1);
	lock->held = 0;
	--held_locks;
	if (unlock_hook != NULL) {
		void (*call)(void) = unlock_hook;

		unlock_hook = NULL;
		call();
	}
}

int64_t k_uptime_get(void) { return now; }
void shell_print(const struct shell *shell, const char *format, ...) { (void)shell; (void)format; }
void shell_error(const struct shell *shell, const char *format, ...) { (void)shell; (void)format; }

static void run_hook(void (**hook)(void))
{
	assert(held_locks == 0);
	if (*hook != NULL) {
		void (*call)(void) = *hook;

		*hook = NULL;
		call();
	}
}

int dds_io_init(void)
{
	assert(held_locks == 0);
	hardware.ready = init_error == 0;
	return init_error;
}

int dds_io_configure(uint32_t frequency, uint16_t amplitude)
{
	assert(held_locks == 0);
	++configure_calls;
	hardware.frequency_hz = frequency;
	hardware.amplitude = amplitude;
	run_hook(&configure_hook);
	return configure_error;
}

int dds_io_start(void)
{
	assert(held_locks == 0);
	++start_calls;
	hardware.running = !bad_running;
	if (bad_frequency) {
		++hardware.frequency_hz;
	}
	run_hook(&start_hook);
	return start_error;
}

int dds_io_stop(void)
{
	assert(held_locks == 0);
	++stop_calls;
	run_hook(&stop_hook);
	if (stop_error == 0) {
		hardware.running = false;
	}
	return stop_error;
}

int dds_io_check(void)
{
	assert(held_locks == 0);
	run_hook(&check_hook);
	return check_error;
}

int dds_io_snapshot(struct dds_snapshot *out)
{
	assert(held_locks == 0);
	if (hardware.running) {
		run_hook(&snapshot_hook);
	}
	if (snapshot_error != 0) {
		return snapshot_error;
	}
	*out = hardware;
	return 0;
}

void dds_io_fault_stop(void)
{
	assert(held_locks == 0);
	++fault_calls;
	hardware.ready = false;
	hardware.running = false;
	hardware.fault = DDS_FAULT_EXTERNAL;
}

static struct dds_bench_snapshot state(void)
{
	struct dds_bench_snapshot out;

	assert(dds_bench_snapshot(&out) == 0);
	return out;
}

static void ready(void)
{
	assert(dds_bench_init() == 0);
	assert(dds_bench_poll(true) == 0);
}

static void start(void)
{
	assert(dds_bench_request_start(5000, 100, 10) == 0);
	assert(dds_bench_poll(true) == 0);
	assert(state().running);
}

static void locked_fault(int error)
{
	struct dds_bench_snapshot out = state();

	assert(!out.ready && out.faulted && !out.running);
	assert(out.last_error == error);
	assert(fault_calls != 0 && !hardware.running);
	assert(dds_bench_request_start(2000, 100, 10) == -EIO);
	assert(dds_bench_init() == -EALREADY);
	assert(dds_bench_poll(true) == error);
}

static int shell_command(size_t argc, char **argv)
{
	for (const struct test_shell_command *command = test_shell_commands;
	     command->name != NULL; ++command) {
		if (strcmp(argv[0], command->name) == 0) {
			return command->handler(NULL, argc, argv);
		}
	}
	return -ENOENT;
}

static void defaults(void)
{
	assert(dds_bench_snapshot(NULL) == -EINVAL);
	assert(!state().ready && !state().running);
	assert(dds_bench_request_start(2000, 100, 10) == -EACCES);
	assert(dds_bench_request_stop() == 0);
	assert(dds_bench_stop() == 0 && stop_calls == 0);
	assert(dds_bench_init() == 0);
	assert(state().ready && !state().running && !state().permitted);
	assert(dds_bench_request_start(2000, 100, 10) == -EACCES);
	assert(dds_bench_init() == -EALREADY);
	assert(dds_bench_poll(true) == 0);
	assert(dds_bench_request_start(2000, 100, 10) == 0);
	assert(start_calls == 0);
}

static void invalid_requests(void)
{
	ready();
	assert(dds_bench_request_start(0, 100, 10) == -EINVAL);
	assert(dds_bench_request_start(2100, 100, 10) == -EINVAL);
	assert(dds_bench_request_start(UINT32_MAX, 100, 10) == -EINVAL);
	assert(dds_bench_request_start(2000, 0, 10) == -EINVAL);
	assert(dds_bench_request_start(2000, 1, 10) == -ERANGE);
	assert(dds_bench_request_start(2000, 101, 10) == -EINVAL);
	assert(dds_bench_request_start(2000, UINT16_MAX, 10) == -EINVAL);
	assert(dds_bench_request_start(2000, 100, 0) == -EINVAL);
	assert(dds_bench_request_start(2000, 100, 61) == -EINVAL);
	assert(dds_bench_poll(true) == 0 && start_calls == 0);
}

static void shared_queue(void)
{
	ready();
	char *argv[] = {"start", "2000", "100"};
	char *stop[] = {"stop"};

	assert(shell_command(3, argv) == 0);
	assert(dds_bench_request_start(5000, 50, 5) == 0);
	assert(dds_bench_request_start(8000, 50, 5) == 0);
	assert(dds_bench_request_start(10000, 50, 5) == 0);
	assert(shell_command(3, argv) == -ENOSPC);
	assert(shell_command(1, stop) == 0);
	assert(dds_bench_request_start(2000, 100, 10) == -EBUSY);
	assert(dds_bench_poll(true) == 0 && start_calls == 0);
	assert(dds_bench_poll(true) == 0 && start_calls == 0);
	assert(shell_command(3, argv) == 0);
	assert(dds_bench_poll(true) == 0 && start_calls == 1);
	assert(state().duration_seconds == 10);
	assert(dds_bench_request_stop() == 0);
	assert(dds_bench_poll(true) == 0 && !state().running);
}

static void actual_start(void)
{
	ready();
	now = 12345;
	assert(dds_bench_request_start(10000, 100, 60) == 0);
	assert(dds_bench_request_start(2000, 2, 1) == 0);
	assert(!state().running && start_calls == 0);
	assert(dds_bench_poll(true) == 0);
	struct dds_bench_snapshot out = state();

	assert(out.running && out.frequency_hz == 10000 && out.requested_mvpp == 100);
	assert(out.amplitude_code == 70 && out.duration_seconds == 60);
	assert((uint32_t)out.amplitude_code * 2u * 2900u <= 100u * 4095u);
	assert(dds_bench_request_start(2000, 2, 1) == -EBUSY);
	assert(dds_bench_poll(true) == 0 && configure_calls == 1);
	now += 1999;
	assert(dds_bench_poll(true) == 0 && state().elapsed_seconds == 1);
	assert(dds_bench_stop() == 0 && !state().running);
	assert(dds_bench_poll(true) == 0 && start_calls == 1);
	assert(dds_bench_request_start(2000, 2, 1) == 0);
	assert(dds_bench_poll(true) == 0 && state().amplitude_code == 1);
}

static void expiry(void)
{
	ready();
	now = 2147483000LL;
	assert(dds_bench_request_start(8000, 100, 60) == 0);
	assert(dds_bench_request_start(5000, 100, 60) == 0);
	assert(dds_bench_poll(true) == 0);
	now += 59999;
	assert(dds_bench_poll(true) == 0 && state().running);
	++now;
	assert(dds_bench_poll(true) == 0 && !state().running);
	assert(state().elapsed_seconds == 60);
	assert(dds_bench_poll(true) == 0 && start_calls == 1);
	now += 60000;
	assert(dds_bench_poll(true) == 0 && start_calls == 1);
}

static void permission_loss(void)
{
	ready();
	assert(dds_bench_request_start(2000, 100, 10) == 0);
	assert(dds_bench_poll(false) == 0 && start_calls == 0);
	assert(!state().permitted);
	assert(dds_bench_request_start(2000, 100, 10) == -EACCES);
	assert(dds_bench_poll(true) == 0 && start_calls == 0);
	start();
	now = 2300;
	assert(dds_bench_poll(false) == 0 && !state().running);
	assert(state().elapsed_seconds == 2);
	assert(dds_bench_poll(true) == 0 && start_calls == 1);
}

static void cancel_start(void)
{
	assert(dds_bench_request_stop() == 0);
	assert(dds_bench_request_start(10000, 100, 10) == -EBUSY);
}

static void cancel_after_unlock(void) { unlock_hook = cancel_start; }

static void stop_race(void (**hook)(void), unsigned int expected_starts)
{
	ready();
	*hook = cancel_start;
	assert(dds_bench_request_start(2000, 100, 10) == 0);
	assert(dds_bench_poll(true) == 0);
	assert(!state().running && !state().faulted && !hardware.running);
	assert(start_calls == expected_starts);
	assert(dds_bench_poll(true) == 0 && start_calls == expected_starts);
	assert(dds_bench_request_start(2000, 100, 10) == 0);
}

static void stop_configure(void) { stop_race(&configure_hook, 0); }
static void stop_start(void) { stop_race(&start_hook, 1); }
static void stop_snapshot(void) { stop_race(&snapshot_hook, 1); }

static void stop_dequeue(void)
{
	ready();
	check_hook = cancel_after_unlock;
	assert(dds_bench_request_start(2000, 100, 10) == 0);
	assert(dds_bench_poll(true) == 0 && !state().running && start_calls == 0);
	assert(dds_bench_poll(true) == 0 && start_calls == 0);
}

static void stop_commit(void)
{
	ready();
	snapshot_hook = cancel_after_unlock;
	assert(dds_bench_request_start(2000, 100, 10) == 0);
	assert(dds_bench_poll(true) == 0 && !state().running && start_calls == 1);
	assert(!hardware.running && !state().faulted);
	assert(dds_bench_poll(true) == 0 && start_calls == 1);
}

static void enqueue_during_stop(void)
{
	assert(dds_bench_request_stop() == 0);
	assert(dds_bench_request_start(2000, 100, 10) == -EBUSY);
}

static void stop_barrier(void)
{
	ready();
	start();
	stop_hook = enqueue_during_stop;
	assert(dds_bench_stop() == 0);
	assert(dds_bench_poll(true) == 0 && start_calls == 1);
	assert(dds_bench_request_start(2000, 100, 10) == 0);
}

static void init_failure(void)
{
	init_error = -ENODEV;
	assert(dds_bench_init() == -ENODEV);
	locked_fault(-ENODEV);
}

static void configure_failure(void)
{
	ready();
	configure_error = -EIO;
	assert(dds_bench_request_start(2000, 100, 10) == 0);
	assert(dds_bench_poll(true) == -EIO && start_calls == 0);
	locked_fault(-EIO);
}

static void start_failure(void)
{
	ready();
	start_error = -ETIMEDOUT;
	assert(dds_bench_request_start(2000, 100, 10) == 0);
	assert(dds_bench_poll(true) == -ETIMEDOUT);
	locked_fault(-ETIMEDOUT);
}

static void check_failure(void)
{
	ready();
	start();
	check_error = -EIO;
	assert(dds_bench_poll(true) == -EIO);
	locked_fault(-EIO);
}

static void inject_snapshot_error(void) { snapshot_error = -EIO; }

static void snapshot_failure(void)
{
	ready();
	start_hook = inject_snapshot_error;
	assert(dds_bench_request_start(2000, 100, 10) == 0);
	assert(dds_bench_poll(true) == -EIO);
	locked_fault(-EIO);
}

static void unexpected_state(void)
{
	ready();
	bad_frequency = true;
	assert(dds_bench_request_start(2000, 100, 10) == 0);
	assert(dds_bench_poll(true) == -EIO);
	locked_fault(-EIO);
}

static void running_drift(void)
{
	ready();
	start();
	++hardware.amplitude;
	assert(dds_bench_poll(true) == -EIO);
	locked_fault(-EIO);
}

static void stop_failure(void)
{
	ready();
	start();
	stop_error = -EIO;
	assert(dds_bench_stop() == -EIO);
	locked_fault(-EIO);
}

static void shell_input(void)
{
	ready();
	char *large[] = {"start", "4294967296", "100"};
	char *negative[] = {"start", "-1", "100"};
	char *zero[] = {"start", "2000", "1"};
	char *trailing[] = {"start", "2000x", "100"};
	char *seconds[] = {"start", "2000", "100", "65546"};
	char *empty[] = {"start", "", "100"};
	char *status[] = {"status"};

	assert(shell_command(3, large) == -EINVAL);
	assert(shell_command(3, negative) == -EINVAL);
	assert(shell_command(3, zero) == -ERANGE);
	assert(shell_command(3, trailing) == -EINVAL);
	assert(shell_command(4, seconds) == -EINVAL);
	assert(shell_command(3, empty) == -EINVAL);
	assert(shell_command(1, status) == 0);
	assert(dds_bench_poll(true) == 0 && start_calls == 0);
}

int main(int argc, char **argv)
{
	const struct { const char *name; void (*run)(void); } cases[] = {
		{"defaults", defaults}, {"invalid_requests", invalid_requests},
		{"shared_queue", shared_queue}, {"actual_start", actual_start}, {"expiry", expiry},
		{"permission_loss", permission_loss}, {"stop_configure", stop_configure},
		{"stop_start", stop_start}, {"stop_snapshot", stop_snapshot}, {"stop_barrier", stop_barrier},
		{"stop_dequeue", stop_dequeue}, {"stop_commit", stop_commit},
		{"init_failure", init_failure}, {"configure_failure", configure_failure},
		{"start_failure", start_failure}, {"check_failure", check_failure},
		{"snapshot_failure", snapshot_failure}, {"unexpected_state", unexpected_state},
		{"running_drift", running_drift},
		{"stop_failure", stop_failure}, {"shell_input", shell_input}
	};

	if (argc == 2) {
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
			if (strcmp(cases[i].name, argv[1]) == 0) {
				cases[i].run();
				assert(held_locks == 0);
				printf("PASS %s\n", argv[1]);
				return 0;
			}
		}
	}
	return 1;
}
