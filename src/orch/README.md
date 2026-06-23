# Orchestration — Pod management, container lifecycle, health checks

Implements Kubernetes-style orchestration: pod abstraction, deployment/replicaset controllers, service discovery, health probing, secret/config management, RBAC, monitoring, and distributed tracing.

## Key Files

- **manifest.c** — Kubernetes-style JSON manifest parser; dispatches create/update/delete for Pod, Service, Namespace, ReplicaSet, Deployment, DaemonSet, ConfigMap, Secret resources.
- **compose.c** — Docker Compose-style multi-service application definition and lifecycle management with service dependencies.
- **pod_health.c** — Liveness/readiness probes: EXEC (fork+exec inside container) and HTTP (GET check for 2xx/3xx) with configurable period and failure thresholds.
- **hooks.c** — Container lifecycle hooks: post-start and pre-stop with EXEC and HTTP modes; best-effort execution.
- **secrets.c** — Secret management: opaque, TLS, and Docker config secrets for image pull authentication.
- **rbac.c** — Role-Based Access Control: roles, bindings, service accounts, and authorization checks.
- **auth.c** — Bearer token authentication for the orchestration API server.
- **pod_security.c** — Pod Security Policies: admission control for privileged, host network/PID/IPC contexts.
- **namespace.c** — Namespace auto-provisioning with resource quotas and default network policies.
- **events.c** — Event recording with ring buffer storage; SSE event stream for real-time pod lifecycle changes.
- **metrics.c** — Prometheus-style counters/gauges/histograms, node monitoring with rolling aggregates (1h/6h/24h/7d).
- **alerting.c** — Rule-based alerting: rule registration, metric evaluation, dispatch via kprintf and webhook.
- **dashboard.c** — HTML/JSON health dashboard rendering with subsystem stats.
- **tracing.c** — OpenTelemetry-style distributed tracing: span creation, parent-child relationships, Jaeger/Zipkin-compatible export.
- **log_shipper.c** — Asynchronous log forwarding with buffering, retry, and exponential backoff.
- **log_aggregator.c** — Ring-buffer log storage, query by container/namespace/pod, HTTP ingest and query endpoints.

## Architecture

Follows the Kubernetes controller pattern: a reconciliation loop ensures actual state matches desired state for each resource type (ReplicaSet, Deployment, DaemonSet, etc.). All state is managed through the container runtime API. Monitoring and observability (metrics, tracing, alerting, logging) are first-class subsystems with their own persistence and query interfaces. The REST API server (on port 8375, managed in `container/orch.c`) provides the external control plane interface.

## Cross-References

- **container/** — Orchestration builds directly on the container runtime for pod/container lifecycle.
- **cluster/** — Cluster-level scheduling, HPA, node management, and ingress rely on orchestration state.
- **ipc/** — Event streams and inter-controller communication use IPC primitives.
