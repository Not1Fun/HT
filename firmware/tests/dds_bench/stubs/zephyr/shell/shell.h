#ifndef TEST_SHELL_H
#define TEST_SHELL_H
#include <stddef.h>
struct shell { int unused; };
typedef int (*test_shell_handler)(const struct shell *, size_t, char **);
struct test_shell_command { const char *name; test_shell_handler handler; };
void shell_print(const struct shell *shell, const char *format, ...);
void shell_error(const struct shell *shell, const char *format, ...);
#define SHELL_CMD_ARG(name, sub, help, handler, required, optional) {#name, handler}
#define SHELL_SUBCMD_SET_END {NULL, NULL}
#define SHELL_STATIC_SUBCMD_SET_CREATE(name, ...) \
  static const struct test_shell_command name[] = {__VA_ARGS__}
#define SHELL_CMD_REGISTER(name, commands, help, handler) \
  const struct test_shell_command *test_shell_commands = *(commands)
extern const struct test_shell_command *test_shell_commands;
#endif
