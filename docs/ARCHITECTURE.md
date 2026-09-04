# madWebMirrorMagick architecture

## Core rule: nodes, not fixed machine types

A physical or virtual machine is a **node**. A node may expose several capabilities at once:

- web backend;
- database;
- backup storage;
- proxy/ingress;
- controller;
- agent.

`main`, `mirror` and `proxy` are therefore roles/capabilities, not mutually exclusive machine types. The controller may run on either backend, on an ingress node, or on a completely separate LAN/WAN host.

## Control plane vs data plane

The control plane (madUI/controller) distributes configuration and observes the system. Losing it must not stop already configured failover. Distributed backup scheduling is not implemented yet: when the controller host is down, no new backup is produced.

The data plane serves traffic. The failover agent on the mirror keeps its last accepted configuration and route state locally and continues autonomously when the controller is offline. On every restart it reapplies the route selected from fresh health checks instead of trusting stale in-memory state.

## Dual ingress (two Proxy-capable nodes)

A single proxy is a single point of failure, so public traffic should be represented as an **Ingress Group** with at least two proxy-capable nodes.

This section describes the target design. The current release has redundant SSH
routes through Proxy A and Proxy B, but it does not yet transfer ownership of a
public HTTP/HTTPS ingress between two proxy nodes.

Example:

```text
                    Internet
                       |
                 Public ingress
                       |
              +--------+--------+
              |                 |
         ingress-A          ingress-B
          ACTIVE             STANDBY
              |                 |
              +--------+--------+
                       |
                 backend pool
                  /         \
              main         mirror
```

An ingress node can also be one of the backends. For a two-server installation both servers may be proxy-capable and web-capable:

```text
server-A: web + db + proxy + agent
server-B: web + db + proxy + agent
```

When both nodes share an L2 network, VRRP/keepalived with a floating virtual IP is the preferred ingress ownership mechanism. For nodes in different networks, the ingress ownership mechanism may instead be DNS failover or an external load balancer. madWebMirrorMagick must treat this as a strategy, not hard-code one network topology.

## SSH reachability, NAT and redundant Proxy tunnels

Management reachability is independent from node role. A web backend may live behind NAT and have no public SSH port at all. In that case either ingress node may also act as an SSH bastion.

For every managed target there are three transport modes:

```text
direct  controller -----------> target
jump    controller -> Proxy A -> target
                    \> Proxy B -> target   (fallback route)
auto    direct first, then Proxy A, then Proxy B
```

Proxy A and Proxy B are **alternative routes**. They are not a serial Proxy A -> Proxy B chain. Losing either proxy must not remove administrative access to a private backend if the other proxy still has a path to it.

Current flat-config compatibility fields are:

```ini
ssh_transport=jump
ssh_identity_file=/var/lib/madwebmirror/ssh/private-web

ssh_jump_primary=madbackup@proxy-a.example.net:22
ssh_jump_primary_identity_file=/var/lib/madwebmirror/ssh/proxy-a

ssh_jump_fallback=madbackup@proxy-b.example.net:22
ssh_jump_fallback_identity_file=/var/lib/madwebmirror/ssh/proxy-b
```

The target host may then be a private address such as `192.168.1.20`. The management connection first tries Proxy A and automatically retries through Proxy B when A is unavailable.

Jump connections use OpenSSH as the forwarding process because Ubuntu 24.04 currently ships libssh 0.10.x, while native libssh ProxyJump support arrived later. The jump command is generated only from validated `[user@]host[:port]` fields, runs in batch mode, requires strict host-key checking and does not support jump-host password authentication. Each jump route may use its own identity key.

The topology model contains a list of `SshTunnelSpec` objects for persistent LocalForward and RemoteForward definitions. The tunnel supervisor groups primary and fallback routes by tunnel ID, runs one route at a time and can be managed directly or through systemd and madUI.

Key policy:

- target and both Proxy nodes may use different keys;
- jump-host passwords are not stored for runtime use;
- first enrollment may use a temporary password only to install the generated public key;
- host keys for both proxies and the final target must be trusted explicitly;
- compromising one Proxy key must not automatically grant access to every other node.

## Runtime privilege model

Normal runtime processes should run as the dedicated unprivileged service account `madbackup`.

Administrative passwords are bootstrap credentials only and are not required during normal operation.

One-time bootstrap (sudo/root) performs tasks such as:

1. create the `madbackup` service account;
2. install systemd units;
3. install `/usr/local/libexec/madweb-helper` as root-owned executable;
4. create `/srv/madwebmirror` and `/var/lib/madwebmirror`;
5. install a sudoers rule that permits `madbackup` to invoke only the helper.

The runtime sudo rule is intentionally narrow:

```text
madbackup ALL=(root) NOPASSWD: /usr/local/libexec/madweb-helper *
```

This does **not** grant arbitrary root shell access: `madweb-helper` exposes a closed verb set and validates every argument.

Current helper verbs:

```text
prepare-site SITE_ID
route-nginx SITE_ID SERVER_NAME BACKEND_HOST PORT
route-apache SITE_ID SERVER_NAME BACKEND_HOST PORT
nginx-reload
apache-reload
tunnels-enable
tunnels-disable
tunnels-restart
tunnels-status
```

It must never expose a verb such as `exec`, `shell`, `run-command`, arbitrary destination paths, or arbitrary config text.

## Site storage

Site copies do not need to be created manually. The helper creates a controlled hierarchy:

```text
/srv/madwebmirror/sites/<site-id>/
    releases/
    shared/
```

The parent stays root-owned. `releases/` and `shared/` are owned by `madbackup`, so backups and deployments can operate without root privileges after bootstrap.

Runtime state is stored below:

```text
/var/lib/madwebmirror/sites/<site-id>/
```

## Web-server configuration

The agent must not be allowed to write arbitrary root-owned nginx/apache configuration. Instead it requests a narrowly defined routing operation from the helper. The helper generates a known template, validates identifiers/hostnames/ports, runs `nginx -t` or `apache2ctl configtest`, and reloads only after validation succeeds.

Future TLS/443 support should follow the same model: fixed templates and explicitly configured certificate references, never arbitrary shell/config injection.

## Failure expectations

Implemented now:

- controller/madUI fails -> the installed mirror watchdog and existing routing continue, but new backups stop until a distributed scheduler is implemented;
- the monitored primary backend fails -> the mirror watchdog invokes the fixed switch script and serves the last deployed local copy;
- one SSH bastion/Proxy fails -> management retries the private node through the other Proxy;

Target design, not implemented yet:

- one backend fails -> either ingress routes to another healthy backend;
- active ingress fails -> standby ingress assumes public traffic;
- one whole node fails in a two-node combined web+proxy deployment -> the surviving node owns ingress and serves its local copy;
- all ingress nodes fail -> public service is unavailable.

The current agent removes the controller from the already-installed failover
decision, and the dual SSH routes remove the single-bastion management failure
mode. Removing the single-proxy traffic failure mode still requires an ingress
ownership strategy such as VRRP, DNS failover or an external load balancer.
