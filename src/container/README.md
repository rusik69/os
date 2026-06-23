# Container — OCI container runtime implementation, namespaces, cgroups

Implements an in-kernel OCI container runtime with image management, storage drivers, networking, security policies, and lifecycle hooks. Supports the full OCI runtime-spec state model.

## Key Files

- **runtime.c** — Core container table, OCI directory layout, state machine (creating→created→running→stopped→deleted), ID generation.
- **config.c** — OCI config.json parser (recursive-descent JSON parser targeting the runtime-spec subset).
- **image.c** — OCI image management: manifest/config parsing, registry pull/push (v2 API), layer download, tag management, prune.
- **storage.c** — Storage driver abstraction: overlay mounts, layer management, diff/apply for image layers.
- **network.c** — Container networking: CNI plugin execution, veth pairs, IPAM, DNS config, loopback setup.
- **state.c** — JSON-based state persistence to `/run/containers/<id>/state.json`.
- **ext.c** — Lifecycle extensions: exec, attach, logs, pause/unpause, wait, stats, top, inspect.
- **checkpoint.c** — Checkpoint/restore (CRIU-like): freeze, dump memory/fds, restore state and processes.
- **scheduler_policy.c** — Resource quotas, limit ranges, pod priority/preemption, affinity/anti-affinity, taints/tolerations.
- **orch.c** — Orchestration API server (HTTP REST on port 8375), pod abstraction, service abstraction.
- **seccomp_notify.c** — User-space seccomp notification handler for policy-daemon-style syscall interception.
- **security_scan.c** — In-kernel CVE matching against image packages.
- **container_exec_enhanced.c** — Enhanced exec with PTY allocation, terminal resize, non-destructive detach.
- **service_proxy.c** — Service proxy (iptables/userspace modes), DNS discovery, volumes, ConfigMaps.

## Architecture

Fixed-size global container table with spinlock protection. State transitions follow the OCI runtime-spec lifecycle. JSON handling is done via a minimal custom parser. Image layers are stored under `/var/lib/containers/layers/` with overlay mounts for the container rootfs. Networking uses veth pairs and CNI-style plugin invocation. The design follows a layered approach: runtime core → storage/networking → lifecycle extensions → orchestration API.

## Cross-References

- **ipc/** — Used for signal delivery and process synchronisation within containers.
- **orch/** — Higher-level orchestration (pods, deployments) builds on this container runtime.
- **cluster/** — Container-related scheduling, network policies, and service discovery integrate here.
- **modules/** — Container runtime can load eBPF or kernel modules for security/observability.
