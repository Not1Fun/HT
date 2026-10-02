/* @brief shell和屏幕共用请求队列，主线程复核许可并执行限幅、限时DAC输出。 */
#include "app/dds_bench.h"
#include "config/analog.h"
#include "platform/dds_io.h"

#include <errno.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#define REQUEST_COUNT 4u
#define MAX_MVPP 100u
#define MAX_SECONDS 60u
#define DEFAULT_SECONDS 10u

enum request_kind { REQUEST_START, REQUEST_STOP };

struct request {
	enum request_kind kind;
	uint32_t frequency_hz;
	uint16_t mvpp;
	uint16_t seconds;
	uint32_t epoch;
};

static struct k_spinlock guard;
static struct request requests[REQUEST_COUNT];
static unsigned int request_head;
static unsigned int request_count;
static uint32_t request_epoch;
static bool stop_pending;
static struct dds_bench_snapshot published;
static struct {
	struct dds_bench_snapshot value;
	bool initialized;
	int64_t started;
	int64_t deadline;
} bench;

static void clear_requests(void)
{
	k_spinlock_key_t key = k_spin_lock(&guard);
	request_head = 0;
	request_count = 0;
	k_spin_unlock(&guard, key);
}

static bool valid_frequency(uint32_t frequency)
{
	return frequency == 2000u || frequency == 5000u ||
	       frequency == 8000u || frequency == 10000u;
}

int dds_bench_request_start(uint32_t frequency_hz, uint16_t mvpp, uint16_t seconds)
{
	if (!valid_frequency(frequency_hz) || mvpp < 1u || mvpp > MAX_MVPP ||
	    seconds < 1u || seconds > MAX_SECONDS) {
		return -EINVAL;
	}
	if ((uint32_t)mvpp * 4095u / (2u * HT_VREF_MV) == 0) {
		return -ERANGE;
	}
	k_spinlock_key_t key = k_spin_lock(&guard);
	int rc = 0;

	if (published.faulted) {
		rc = -EIO;
	} else if (!published.ready || !published.permitted) {
		rc = -EACCES;
	} else if (published.running || stop_pending) {
		rc = -EBUSY;
	} else if (request_count == REQUEST_COUNT) {
		rc = -ENOSPC;
	} else {
		requests[(request_head + request_count) % REQUEST_COUNT] = (struct request){
			.kind = REQUEST_START, .frequency_hz = frequency_hz,
			.mvpp = mvpp, .seconds = seconds, .epoch = request_epoch
		};
		++request_count;
	}
	k_spin_unlock(&guard, key);
	return rc;
}

int dds_bench_request_stop(void)
{
	k_spinlock_key_t key = k_spin_lock(&guard);

	/* 停止不能被满队列挡住；epoch同时取消已取出但尚未完成的启动。 */
	++request_epoch;
	stop_pending = true;
	request_head = 0;
	request_count = 1;
	requests[0] = (struct request){.kind = REQUEST_STOP};
	k_spin_unlock(&guard, key);
	return 0;
}

static bool cancelled(const struct request *request)
{
	k_spinlock_key_t key = k_spin_lock(&guard);
	bool result = stop_pending || request->epoch != request_epoch;

	k_spin_unlock(&guard, key);
	return result;
}

static bool dequeue(struct request *request)
{
	k_spinlock_key_t key = k_spin_lock(&guard);
	bool found = request_count != 0;

	if (found) {
		*request = requests[request_head];
		request_head = (request_head + 1) % REQUEST_COUNT;
		--request_count;
	}
	k_spin_unlock(&guard, key);
	return found;
}

static void publish(void)
{
	if (bench.value.running) {
		bench.value.elapsed_seconds = (uint32_t)((k_uptime_get() - bench.started) / 1000);
	}
	k_spinlock_key_t key = k_spin_lock(&guard);

	published = bench.value;
	k_spin_unlock(&guard, key);
}

int dds_bench_snapshot(struct dds_bench_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&guard);

	*snapshot = published;
	k_spin_unlock(&guard, key);
	return 0;
}

static int stop_output(void)
{
	if (bench.value.running) {
		bench.value.elapsed_seconds = (uint32_t)((k_uptime_get() - bench.started) / 1000);
	}
	int rc = dds_io_stop();
	struct dds_snapshot output;
	int snapshot_rc = dds_io_snapshot(&output);

	if (snapshot_rc == 0) {
		bench.value.running = output.running;
		bench.value.frequency_hz = output.frequency_hz;
		bench.value.amplitude_code = output.amplitude;
		if (rc == 0 && (!output.ready || output.running || output.fault != DDS_FAULT_NONE)) {
			rc = -EIO;
		}
	} else if (rc == 0) {
		rc = snapshot_rc;
	}
	if (rc != 0) {
		dds_io_fault_stop();
		if (dds_io_snapshot(&output) == 0) {
			bench.value.running = output.running;
		}
		bench.value.ready = false;
		bench.value.faulted = true;
		bench.value.last_error = rc;
	}
	return rc;
}

static void begin_stop(void)
{
	k_spinlock_key_t key = k_spin_lock(&guard);

	++request_epoch;
	stop_pending = true;
	request_head = 0;
	request_count = 0;
	k_spin_unlock(&guard, key);
}

static void finish_stop(void)
{
	k_spinlock_key_t key = k_spin_lock(&guard);

	request_head = 0;
	request_count = 0;
	stop_pending = false;
	published = bench.value;
	k_spin_unlock(&guard, key);
}

static int fail(int error)
{
	begin_stop();
	dds_io_fault_stop();
	(void)stop_output();
	bench.value.ready = false;
	bench.value.faulted = true;
	bench.value.last_error = error;
	finish_stop();
	return error;
}

int dds_bench_init(void)
{
	if (bench.initialized) {
		return -EALREADY;
	}
	bench.initialized = true;
	begin_stop();
	int rc = dds_io_init();
	struct dds_snapshot output;

	if (rc == 0) {
		rc = dds_io_snapshot(&output);
	}
	if (rc == 0 && (!output.ready || output.running || output.fault != DDS_FAULT_NONE)) {
		rc = -EIO;
	}
	if (rc != 0) {
		return fail(rc);
	}
	bench.value.ready = true;
	finish_stop();
	return 0;
}

int dds_bench_stop(void)
{
	begin_stop();
	if (!bench.initialized) {
		finish_stop();
		return 0;
	}
	int rc = stop_output();

	finish_stop();
	return rc;
}

static int start_output(const struct request *request)
{
	if (!valid_frequency(request->frequency_hz) || request->mvpp < 1u ||
	    request->mvpp > MAX_MVPP || request->seconds < 1u || request->seconds > MAX_SECONDS) {
		return -EINVAL;
	}
	if (bench.value.running) {
		return -EBUSY;
	}
	/* 向下量化，不能把100mVpp舍入到71码而超过上限。 */
	uint16_t amplitude = (uint16_t)((uint32_t)request->mvpp * 4095u / (2u * HT_VREF_MV));

	if (amplitude == 0) {
		return -ERANGE;
	}
	if (cancelled(request)) {
		return -ECANCELED;
	}
	int rc = dds_io_configure(request->frequency_hz, amplitude);

	if (rc == 0 && cancelled(request)) {
		return -ECANCELED;
	}
	if (rc == 0) {
		rc = dds_io_start();
	}
	if (rc != 0) {
		return fail(rc);
	}
	if (cancelled(request)) {
		return -ECANCELED;
	}
	struct dds_snapshot output;

	rc = dds_io_snapshot(&output);
	if (rc != 0 || !output.ready || !output.running || output.fault != DDS_FAULT_NONE ||
	    output.frequency_hz != request->frequency_hz || output.amplitude != amplitude) {
		return fail(rc != 0 ? rc : -EIO);
	}
	bench.started = k_uptime_get();
	bench.deadline = bench.started + (int64_t)request->seconds * 1000;
	bench.value.running = true;
	bench.value.frequency_hz = output.frequency_hz;
	bench.value.requested_mvpp = request->mvpp;
	bench.value.amplitude_code = output.amplitude;
	bench.value.elapsed_seconds = 0;
	bench.value.duration_seconds = request->seconds;
	if (cancelled(request)) {
		return -ECANCELED;
	}
	return 0;
}

int dds_bench_poll(bool permitted)
{
	bench.value.permitted = permitted;
	k_spinlock_key_t key = k_spin_lock(&guard);

	/* 先撤销发布许可，避免失去许可时仍接受旧状态下的新请求。 */
	published.permitted = permitted;
	k_spin_unlock(&guard, key);
	if (!bench.initialized || !bench.value.ready || bench.value.faulted) {
		clear_requests();
		publish();
		return bench.value.last_error != 0 ? bench.value.last_error : -EACCES;
	}
	int rc = dds_io_check();
	struct dds_snapshot output;

	if (rc == 0) {
		rc = dds_io_snapshot(&output);
	}
	if (rc != 0 || !output.ready || output.fault != DDS_FAULT_NONE ||
	    output.running != bench.value.running ||
	    (output.running && (output.frequency_hz != bench.value.frequency_hz ||
			       output.amplitude != bench.value.amplitude_code))) {
		return fail(rc != 0 ? rc : -EIO);
	}
	if (!permitted || (bench.value.running && k_uptime_get() >= bench.deadline)) {
		if (!bench.value.running) {
			begin_stop();
			finish_stop();
			return 0;
		}
		return dds_bench_stop();
	}
	struct request request;

	if (dequeue(&request)) {
		if (request.kind == REQUEST_STOP) {
			return dds_bench_stop();
		}
		rc = start_output(&request);
		bench.value.last_error = rc;
		if (rc == -ECANCELED) {
			return dds_bench_stop();
		}
		if (bench.value.faulted) {
			return rc;
		}
		if (rc == 0) {
			key = k_spin_lock(&guard);
			bool stopped = stop_pending || request.epoch != request_epoch;

			if (!stopped) {
				request_head = 0;
				request_count = 0;
				published = bench.value;
			}
			k_spin_unlock(&guard, key);
			if (stopped) {
				bench.value.last_error = -ECANCELED;
				return dds_bench_stop();
			}
		}
	}
	publish();
	return 0;
}

static int parse_number(const char *text, uint32_t *value)
{
	uint32_t result = 0;

	if (text == NULL || *text == '\0') {
		return -EINVAL;
	}
	for (; *text != '\0'; ++text) {
		if (*text < '0' || *text > '9' || result > (UINT32_MAX - (uint32_t)(*text - '0')) / 10u) {
			return -EINVAL;
		}
		result = result * 10u + (uint32_t)(*text - '0');
	}
	*value = result;
	return 0;
}

static int command_start(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t frequency = 0, mvpp = 0, seconds = DEFAULT_SECONDS;

	if ((argc != 3 && argc != 4) || parse_number(argv[1], &frequency) != 0 ||
	    parse_number(argv[2], &mvpp) != 0 || (argc == 4 && parse_number(argv[3], &seconds) != 0) ||
	    !valid_frequency(frequency) || mvpp < 1u || mvpp > MAX_MVPP ||
	    seconds < 1u || seconds > MAX_SECONDS) {
		shell_error(shell, "dds start <2000|5000|8000|10000 Hz> <1..100 mVpp> [1..60 s, default 10]");
		return -EINVAL;
	}
	if (mvpp * 4095u / (2u * HT_VREF_MV) == 0) {
		shell_error(shell, "Requested amplitude rounds down to zero; output remains off");
		return -ERANGE;
	}
	int rc = dds_bench_request_start(frequency, (uint16_t)mvpp, (uint16_t)seconds);

	if (rc == 0) {
		shell_print(shell, "DDS start queued; execution still requires current permission");
	} else {
		shell_error(shell, "DDS start rejected: %d", rc);
	}
	return rc;
}

static int command_stop(const struct shell *shell, size_t argc, char **argv)
{
	(void)argc;
	(void)argv;
	int rc = dds_bench_request_stop();

	shell_print(shell, "DDS stop queued; pending starts cancelled");
	return rc;
}

static int command_status(const struct shell *shell, size_t argc, char **argv)
{
	(void)argc;
	(void)argv;
	struct dds_bench_snapshot value;

	(void)dds_bench_snapshot(&value);
	shell_print(shell, "DDS ready=%u running=%u permitted=%u fault=%u error=%d",
		(unsigned int)value.ready, (unsigned int)value.running, (unsigned int)value.permitted,
		(unsigned int)value.faulted, value.last_error);
	shell_print(shell, "configured=%u Hz requested=%u mVpp amplitude=%u code elapsed=%u/%u s",
		(unsigned int)value.frequency_hz, (unsigned int)value.requested_mvpp,
		(unsigned int)value.amplitude_code, (unsigned int)value.elapsed_seconds,
		(unsigned int)value.duration_seconds);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(dds_commands,
	SHELL_CMD_ARG(status, NULL, "Read cached DAC bench state", command_status, 1, 0),
	SHELL_CMD_ARG(start, NULL, "Start DAC only: Hz mVpp [seconds]", command_start, 3, 1),
	SHELL_CMD_ARG(stop, NULL, "Stop DAC and cancel pending starts", command_stop, 1, 0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(dds, &dds_commands, "DAC-only bench; relays remain off", NULL);
