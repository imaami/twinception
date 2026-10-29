/** @file cli_socket.h
 * CLI network socket.
 */
#ifndef TWINCEPTION_SRC_CLI_SOCKET_H_
#define TWINCEPTION_SRC_CLI_SOCKET_H_

#include <curl/curl.h>

#include "compat.h"
#include "loop.h"

struct cli_socket {
	struct list      hook;
	struct loop_ref  ref;
	struct loop_step step;
	curl_socket_t    fd;
	int              events;
};

#define to_cli_socket(ptr) _Generic((typeof(ptr) *)0                    \
        , struct list **: container_of(ptr, struct cli_socket, hook)    \
        , struct loop_ref **: container_of(ptr, struct cli_socket, ref) \
        , struct loop_step **: container_of(ptr, struct cli_socket, step))

#endif /* TWINCEPTION_SRC_CLI_SOCKET_H_ */
