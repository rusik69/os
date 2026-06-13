# Cluster Subsystem Architecture

## Overview

The cluster subsystem provides a Kubernetes-inspired container orchestration
platform built into the kernel. It implements Raft-based consensus, gossip
protocol for membership, node management, network policies, autoscaling,
and rolling upgrades.

## Architecture Diagram

```
┌──────────────────────────────────────────────────────────────┐
│                    Cluster Management                          │
├──────────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                   │
│  │   Raft   │  │  Gossip  │  │   Node   │  ┌──────────────┐ │
│  │ Consensus│◄─┤ Protocol │◄─┤  Manager │  │  Controllers │ │
│  └────┬─────┘  └──────────┘  └────┬─────┘  └──────┬───────┘ │
│       │                            │               │         │
│  ┌────▼─────┐  ┌──────────┐  ┌────▼─────┐  ┌──────▼───────┐ │
│  │  Raft KV │  │   Mesh   │  │ Network  │  │  HPA/VPA/    │ │
│  │  Store   │  │ Overlay  │  │ Policies │  │ Autoscaler   │ │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘ │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐ │
│  │   CRD    │  │ Upgrade  │  │  Node    │  │  Runtime     │ │
│  │  Engine  │  │ Manager  │  │ Problem  │  │  Security    │ │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘ │
└──────────────────────────────────────────────────────────────┘
         │                      │
         ▼                      ▼
┌─────────────────┐   ┌──────────────────┐
│ Container       │   │ Orchestration    │
│ Runtime (OCI)   │   │ API Server       │
└─────────────────┘   └──────────────────┘
```

## Key Components

### Raft Consensus (`raft.c`)
- Leader election with randomized election timeouts
- Log replication across cluster nodes
- Term-based consistency guarantees
- Snapshot and log compaction

### Raft KV Store (`raft_kv.c`)
- Key-value store backed by Raft consensus
- Linearizable read and write operations
- Distributed coordination primitive

### Gossip Protocol (`gossip.c`)
- SWIM-style membership protocol
- Suspicion-based failure detection
- State synchronization across nodes
- Infection-style message dissemination

### Node Management (`node.c`)
- Node registration with heartbeat monitoring
- Health status tracking (ready, not-ready, unknown)
- Resource capacity tracking (CPU, memory, pods)
- Leader election for orchestration controller
- Service endpoint synchronization
- Pod assignment reconciliation

### Cluster Networking (`overlay.c`, `mesh.c`)
- VXLAN overlay for inter-node pod networking
- WireGuard encrypted mesh
- Network namespace isolation

### Network Policies (`network_policy.c`)
- Ingress/egress rules with pod selectors
- Ingress controller: NodePort, LoadBalancer, HTTP routing
- Multi-tenant isolation via per-namespace VLAN/VXLAN
- DNS-based policy integration

### Autoscaling (`hpa.c`)
- Horizontal Pod Autoscaler (CPU/memory target utilization)
- Vertical Pod Autoscaler (resource recommendations)
- Cluster autoscaler (add/remove nodes based on pending pods)
- Descheduler (evict pods for better packing)

### Custom Resources (`crd.c`)
- Define custom resource types at runtime
- Store and manage custom resource instances

### Upgrade Manager (`upgrade.c`)
- Rolling upgrades: cordon, drain, upgrade, uncordon
- Rollback to previous version
- Phase-based state machine

### Node Problem Detection (`node_problem.c`)
- Monitor node health metrics
- Detect and report node issues
- Trigger remediation actions

### Runtime Security (`runtime_security.c`)
- Pod security policies
- Container runtime integrity
- Security event auditing

## Data Flow

1. **Pod Creation**: API request → Controller → Scheduler → Node assignment
   → Container runtime creates pod → Health monitoring starts
2. **Scaling**: HPA loop monitors metrics → Calculates desired replicas
   → Updates ReplicaSet → Scheduler assigns new pods
3. **Upgrade**: Upgrade manager initiates → Cordon node → Drain pods →
   Upgrade node software → Uncordon → Next node
4. **Cross-node Communication**: Pod → VXLAN tunnel → Remote node → Pod

## Files

| File | Component |
|------|-----------|
| `src/cluster/raft.c` | Raft consensus implementation |
| `src/cluster/raft_kv.c` | Raft-backed key-value store |
| `src/cluster/gossip.c` | Gossip protocol |
| `src/cluster/node.c` | Node management and reconciliation |
| `src/cluster/cluster.c` | Cluster-wide coordination |
| `src/cluster/overlay.c` | Cluster network overlay |
| `src/cluster/network_policy.c` | Network policies and ingress |
| `src/cluster/mesh.c` | WireGuard encrypted mesh |
| `src/cluster/controllers.c` | Cluster controllers |
| `src/cluster/hpa.c` | Autoscaler and descheduler |
| `src/cluster/crd.c` | Custom resource definitions |
| `src/cluster/upgrade.c` | Rolling upgrades |
| `src/cluster/node_problem.c` | Node problem detection |
| `src/cluster/runtime_security.c` | Runtime security policies |
