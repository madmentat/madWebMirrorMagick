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

The control plane (madUI/controller) distributes configuration and observes the system. Losing it must not stop already configured backups or failover.

The data plane serves traffic. Agents keep the last accepted configuration locally and continue autonomously when the controller is offline.

## Dual ingress (two Proxy-capable nodes)

A single proxy is a single point of failure, so public traffic should be represented as an **Ingress Group** with at least two proxy-capable nodes.

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

- controller/madUI fails -> existing routing and backup policy continue;
- one backend fails -> ingress routes to another healthy backend;
- active ingress fails -> standby ingress assumes public traffic;
- one whole node fails in a two-node combined web+proxy deployment -> the surviving node can own ingress and serve its local copy;
- all ingress nodes fail -> public service is unavailable.

This keeps the controller out of the critical traffic path and removes the single-proxy failure mode.
