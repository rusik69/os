# Cluster — Clustering: node membership, service discovery, distributed state

Implements a Kubernetes-style cluster management stack with distributed consensus (Raft), gossip-based membership (SWIM), overlay networking, node management, orchestration controllers, and autoscaling.

## Key Files

- **raft.c** — Raft consensus: leader election (RequestVote RPC), log replication (AppendEntries), log compaction/snapshotting, membership changes via joint consensus, persistent log on disk.
- **raft_kv.c** — Distributed key-value store on top of Raft: cluster state store, watch support for key prefix monitoring, lease mechanism with TTL.
- **gossip.c** — SWIM gossip protocol: cluster membership, failure detection (ping/indirect-probe/suspicion), join/leave with seed nodes.
- **cluster.c** — Cluster orchestration: distributed lock manager (Raft KV CAS), barrier synchronisation, cluster-wide config, quorum health monitoring, split-brain detection.
- **node.c** — Node management: registration/health reporting, leader election for orchestration controller, service endpoint sync, pod assignment reconciliation.
- **node_problem.c** — Node problem detection: hardware errors, OOM, disk failures, kernel deadlocks, read-only filesystem.
- **overlay.c** — Overlay network: VXLAN/GENEVE tunnels for pod-to-pod routing across nodes, egress NAT, tunnel topology management.
- **network_policy.c** — Network policies: ingress/egress rules with pod selector, ingress controller (NodePort/LoadBalancer/HTTP), multi-tenant VXLAN isolation, encrypted overlay.
- **mesh.c** — Service mesh: bandwidth management (TBF/HTB), sidecar proxy injection, mTLS with SPIFFE, traffic splitting for canary, multi-cluster federation.
- **controllers.c** — Orchestration controllers: ReplicaSet, Deployment (rolling update/rollback), DaemonSet, StatefulSet, Job, CronJob, garbage collection.
- **hpa.c** — Horizontal/Vertical Pod Autoscaler: CPU/memory-based scaling, resource recommendations.
- **cluster_autoscaler.c** — Cluster autoscaler: monitors pending pods, triggers scale-up/down with cooldown.
- **cluster_descheduler.c** — Descheduler: evicts pods from under/over-utilised nodes.
- **cluster_ingress.c** — Cluster ingress: NodePort, LoadBalancer, HTTP host/path routing.
- **crd.c** — Custom Resource Definitions: resource versioning, admission webhooks, CRD API extension, operator framework.
- **upgrade.c** — Cluster rolling upgrades: cordon/drain/upgrade/uncordon with rollback.
- **runtime_security.c** — Container runtime security: user namespace mapping, capability management, seccomp filters, AppArmor, read-only rootfs, no-new-privs, OOM adjustment, device whitelist.
- **events.c** — Cluster-wide event bus: publish/subscribe with history replay for late joiners.

## Architecture

Distributed systems stack with clear layers: (1) **Membership layer** — SWIM gossip for node discovery and failure detection. (2) **Consensus layer** — Raft for consistent state replication and leader election. (3) **State layer** — Distributed KV store built on Raft for cluster configuration and coordination. (4) **Networking layer** — VXLAN/GENEVE overlay for pod connectivity across nodes. (5) **Orchestration layer** — Controllers (ReplicaSet, Deployment, DaemonSet, etc.) driven by reconciliation loops. (6) **Autoscaling layer** — HPA, cluster autoscaler, and descheduler for dynamic resource management. All layers use spinlock-protected fixed-size tables and communicate via the kernel's TCP/IP stack.

## Cross-References

- **container/** — Cluster scheduling and orchestration controllers manage container instances across nodes.
- **orch/** — Pod lifecycle, health checks, and secrets management integrate with cluster controllers.
- **ipc/** — Network policies and event bus use IPC mechanisms.
- **boot/** — Node hardware info (SMBIOS) is used for node health reporting.
