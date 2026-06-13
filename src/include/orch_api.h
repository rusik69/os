#ifndef ORCH_API_H
#define ORCH_API_H

/* Orchestration API — shared types and forward declarations
 *
 * This header provides type definitions used by the container orchestration
 * subsystem: services, volumes, scheduler policies, and cluster state.
 */

#include "types.h"

/* Maximum resource name length */
#define ORCH_NAME_MAX          128

/* Service types */
#define SERVICE_TYPE_CLUSTERIP 0
#define SERVICE_TYPE_NODEPORT  1
#define SERVICE_TYPE_LOADBALANCER 2

/* Volume types */
#define VOLUME_TYPE_LOCAL      0
#define VOLUME_TYPE_BIND       1
#define VOLUME_TYPE_TMPFS      2

/* Forward declarations */
struct service;
struct volume;
struct configmap;
struct secret;

/* Orchestration subsystem API */
int orch_init(void);
int orch_shutdown(void);

#endif /* ORCH_API_H */
