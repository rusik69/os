#ifndef SSH_CLIENT_H
#define SSH_CLIENT_H

#include "types.h"

/* SSH client session handle */
struct ssh_client;

/* Connection result callback: return 0 to continue, non-zero to abort */
typedef int (*ssh_output_fn)(const char *data, int len, void *ctx);
typedef void (*ssh_close_fn)(void *ctx);

/* Connect to an SSH server with password authentication.
 * Returns a session handle on success, NULL on failure.
 * The session runs synchronously - call ssh_client_poll() repeatedly.
 */
struct ssh_client *ssh_client_connect(const char *host, uint16_t port,
                                       const char *user, const char *pass,
                                       ssh_output_fn on_output,
                                       ssh_close_fn on_close,
                                       void *ctx);

/* Poll network for an SSH client session (must be called regularly) */
void ssh_client_poll(struct ssh_client *cl);

/* Send data over an established SSH session (sends as channel data) */
int ssh_client_send(struct ssh_client *cl, const char *data, int len);

/* Close an SSH session */
void ssh_client_close(struct ssh_client *cl);

/* Returns 1 if the session is still connected */
int ssh_client_connected(struct ssh_client *cl);

/* Returns 1 if the session is fully established (shell ready) */
int ssh_client_ready(struct ssh_client *cl);

#endif
