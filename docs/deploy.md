# Deploying the backtester web UI

How the public site runs, how to build it from nothing, how to operate it, and how
to take it down.
- **Why it's built this way:** [`ui-plan.md`](ui-plan.md), and every decision in
  [`decisions.md`](decisions.md).
- **Using the backtester:** [`backtesting.md`](backtesting.md).

```
browser ──https──> Caddy (:443, Let's Encrypt cert for <ip-with-dashes>.sslip.io)
                     │  per-IP connection limits (nftables, me-ratelimit.service)
                     └─> Streamlit (127.0.0.1:8501, me-web.service, user me-web)
                           │  per-IP run limits, queue (1 running, 10 waiting)
                           └─> isolate box per run: no network, own UID, cgroup limits
                                 python -m matching_engine.cli run ...  (/data read-only)

GitHub: push to main → CI → Deploy workflow → ssh me-deploy@VM "deploy <sha>" < bundle.tar
          (build ARM wheel, test it)            (server checks the new release, then switches)
```

| piece | where |
|---|---|
| VM | AWS EC2 **t4g.small** (2 vCPU, 2 GB, ARM), Ubuntu 24.04, 30 GB gp3, Elastic IP |
| app | `/opt/me/releases/<sha>/{src,venv}`, `/opt/me/current` → active release |
| data | `/srv/me/data/<date>/<SYMBOL>.mbo.parquet` (copied once, never through git or CI) |
| services | `me-web` (Streamlit), `caddy`, `isolate` (cgroup keeper), `me-ratelimit` (nftables) |
| users | `ubuntu` (admin), `me-web` (runs the app, no login), `me-deploy` (CI, one command), `isolate` (sandbox IDs) |
| deploy | `.github/workflows/deploy.yml`, `deploy/` |

---

## 1. Create the server (AWS console, once)

1. **Billing → Budgets:** a monthly cost budget (e.g. $35), with an email alert at 80%.
2. **EC2 → Key Pairs:** create an ED25519 key pair (`.pem`), or import your existing
   public key.
   - On WSL, move the downloaded `.pem` into `~/.ssh/` and `chmod 600` it.
3. **EC2 → Launch instance:**
   - AMI: **Ubuntu Server 24.04 LTS, 64-bit (Arm)**
   - Instance type: **t4g.small**
   - Your key pair
   - Storage: 30 GB gp3
   - Advanced details → **Credit specification: Standard**, so there are no surprise
     charges if the CPU runs hot for long
   - Security group with **SSH (22)** only, for now
4. **EC2 → Elastic IPs:** Allocate, then Associate with the instance. The public address
   then never changes, and the sslip.io name *is* that address. An Elastic IP left
   unattached is still billed: release it when you delete the instance.
5. **Security group inbound rules:**

   | port | source | why |
   |---|---|---|
   | 22 | Anywhere-IPv4 | GitHub's runners deploy over SSH and have no fixed addresses. SSH is keys-only, root login off, two users allowed, fail2ban on. |
   | 80 | Anywhere-IPv4 | Let's Encrypt validation and the redirect to https |
   | 443 | Anywhere-IPv4 | the site |

Check you can log in: `ssh -i ~/.ssh/<key>.pem ubuntu@<elastic-ip>`.

## 2. Provision it (once; safe to re-run)

From the repo, on your machine:

```bash
IP=52.65.150.242                      # your Elastic IP
KEY=~/.ssh/me-backtester-ed.pem
scp -i $KEY -r deploy ubuntu@$IP:/tmp/
ssh -i $KEY ubuntu@$IP "sudo SITE_ADDRESS=${IP//./-}.sslip.io bash /tmp/deploy/provision.sh"
```

`provision.sh` sets up everything except the app itself:
- **Packages:** build tools, Python, fail2ban, unattended upgrades, nftables.
- **A 2 GB swap file:** a safety net for the OS and the app; runs get no swap.
- **isolate v2.7:** built from source, with its subordinate-ID user and cgroup service.
- **Users and permissions:**
  - `me-web`, plus `/srv/me` (`root:me-web 750`), with the data inside readable so the
    boxes can read it
  - the `me-deploy` user, its forced-command key, one sudo rule, and SSH hardening
- **Connection limits and HTTPS:** the per-IP connection limits, and Caddy with a
  certificate for `SITE_ADDRESS`.

Optional environment variables:
- `DEPLOY_PUBKEY='ssh-ed25519 AAAA… github-actions-deploy'` installs the CI deploy key
  (see section 4).
- `SITE_ADDRESS` empty: no Caddy; reach the app through an SSH tunnel.

**Upload the data** (only the MBO files are needed):
```bash
ssh -i $KEY ubuntu@$IP 'sudo install -d -m 755 /srv/me/data/2026-09-29'
rsync -az -e "ssh -i $KEY" --rsync-path="sudo rsync" \
    data/databento/2026-09-29/*.mbo.parquet ubuntu@$IP:/srv/me/data/2026-09-29/
ssh -i $KEY ubuntu@$IP 'sudo find /srv/me/data -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +'
```

## 3. The first release (by hand)

Either merge to `main` and let CI deploy (section 4), or build on the VM from source:
```bash
git archive --format=tar.gz -o /tmp/src.tar.gz HEAD
scp -i $KEY /tmp/src.tar.gz ubuntu@$IP:/tmp/
ssh -i $KEY ubuntu@$IP "sudo /usr/local/sbin/me-install-release $(git rev-parse --short=12 HEAD) /tmp/src.tar.gz"
```
Building from source compiles the C++ extension on the VM, about 2 minutes.

What `me-install-release` does:
1. Builds `/opt/me/releases/<id>`.
2. Runs `deploy/sandbox_check.py` **against the new release**: one real backtest, plus
   hostile strategies (network, environment, host files, `sys.exit`, memory, CPU, wall
   time, fork bomb), each of which must be stopped. Any failure aborts, and the old
   release stays live.
3. Switches `/opt/me/current`.
4. Health-checks the site, and rolls back if it doesn't answer.
5. Keeps the last 3 releases.

## 4. Continuous deployment (GitHub Actions)

`.github/workflows/deploy.yml` runs after a **green CI run of a push to `main`**, or by
hand (Actions → Deploy → Run workflow):
1. **build** on `ubuntu-24.04-arm`:
   - `pip wheel . -C cmake.define.ME_NATIVE_ARCH=OFF`. The runner's CPU is newer than
     the t4g's Graviton2, and a `-march=native` build could crash there with SIGILL.
   - Run the Python tests against the wheel.
   - Bundle the wheel with `git archive` of the source.
2. **deploy:** `ssh me-deploy@VM "deploy <sha>" < bundle.tar`, then `curl $SITE_URL/_stcore/health`.

**The deploy key can do one thing.** Its `authorized_keys` line is
`restrict,command="/usr/local/bin/me-deploy-entry"`: no shell, no pty, no forwarding.
- `me-deploy-entry` accepts only `deploy <hex sha>`. It runs the single sudo rule,
  `me-deploy-receive`.
- `me-deploy-receive` unpacks the bundle and installs it with the **root-owned**
  `/usr/local/sbin/me-install-release`, never with code from the bundle.
- A leaked key can deploy a build (which still has to pass the sandbox check), and
  nothing else.

**GitHub setup** (Settings → Environments → `production`):
- **Deployment branches:** `main` only.
- **Secrets:**
  - `DEPLOY_SSH_KEY`: the private key. Generate it with
    `ssh-keygen -t ed25519 -N "" -C github-actions-deploy -f deploy_key`. Install the
    public half with `DEPLOY_PUBKEY="$(cat deploy_key.pub)"` in `provision.sh`, then
    delete the local private key once it is in GitHub.
  - `DEPLOY_HOST`: the Elastic IP.
  - `DEPLOY_KNOWN_HOSTS`: `ssh-keyscan -t ed25519 <ip>`. Check that its fingerprint
    (`ssh-keygen -lf`) matches the one you've been connecting to.
- **Variable:** `SITE_URL`, e.g. `https://52-65-150-242.sslip.io`.

To **rotate the deploy key**: generate a new pair, re-run `provision.sh` with the new
`DEPLOY_PUBKEY`, and update `DEPLOY_SSH_KEY`.

## 5. Limits and abuse protection

| layer | limit | where to change |
|---|---|---|
| per run | 120 s wall, 90 s CPU, 1 GB memory, 20 MB output, 64 processes/threads, no network | `ME_MEMORY_MB` in `me-web.service`; `Limits` in `webapp/runner.py` |
| server | 1 run at a time, 10 waiting | `ME_MAX_CONCURRENT`, `ME_MAX_QUEUE` |
| per IP, runs | 1 running or queued, 30 s apart, 20 an hour | `ME_COOLDOWN_S`, `ME_RUNS_PER_HOUR` |
| per IP, connections | 30 open, 60 new a minute (burst 120) to ports 80/443 | `deploy/ratelimit.nft` |
| uploads | 1 MB request body (Caddy), 1 MB uploads (Streamlit), 100 KB strategy code | `deploy/Caddyfile`, `me-web.service` |
| SSH | keys only, no root, `AllowUsers ubuntu me-deploy`, fail2ban | `deploy/sshd.me.conf` |

- **Where the per-IP run limits get the address:** `X-Forwarded-For`, set by Caddy.
  Caddy replaces any value a client sends (checked: a spoofed `6.6.6.6` arrives as the
  real address). Streamlit listens only on 127.0.0.1, so the header can't be forged
  from outside.
- **Changing the service's settings:** a change to `deploy/systemd/me-web.service` goes
  live with the next release.
- **Changing provision files** (`ratelimit.nft`, `Caddyfile`, `sshd.me.conf`): copy them
  over and re-run `provision.sh`.

## 6. Day to day

```bash
ssh -i $KEY ubuntu@$IP
systemctl status me-web caddy isolate me-ratelimit
journalctl -u me-web -f                         # the app
sudo tail -f /var/log/caddy/access.log          # requests
sudo nft list table inet me_ratelimit           # connection-limit counters
readlink -f /opt/me/current; ls /opt/me/releases
```

- **Re-run the sandbox check on the live release:**
  ```bash
  sudo -u me-web /opt/me/current/venv/bin/python /opt/me/current/src/deploy/sandbox_check.py
  ```
- **Roll back to an earlier release** (any of the last 3):
  ```bash
  sudo ln -sfn /opt/me/releases/<id> /opt/me/current && sudo systemctl restart me-web
  ```
- **Add a day of data:** run `scripts/fetch_databento.py` and `scripts/convert_mbo.py`
  locally, then rsync the new `<date>/` folder as in section 2. The UI lists it right
  away; no restart is needed.
- **See the site without the public address:**
  `ssh -i $KEY -N -L 8502:127.0.0.1:8501 ubuntu@$IP`, then open http://localhost:8502.

- **If the public IP ever changes** (a new Elastic IP, or a rebuilt server):
  1. Re-run `provision.sh` with the new `SITE_ADDRESS=<new-ip-with-dashes>.sslip.io`.
  2. In the GitHub `production` environment, update `DEPLOY_HOST`, `DEPLOY_KNOWN_HOSTS`
     (`ssh-keyscan` again; a new server has a new host key) and `SITE_URL`.
  3. Update the live link in `README.md` and `docs/backtesting.md`.

## 7. Things that went wrong once (and how they were fixed)

| symptom | cause | fix (already in the scripts) |
|---|---|---|
| isolate fails to build: `seccomp.h: No such file` | v2.7 filters system calls | `libseccomp-dev` |
| `isolate.service` fails: `User isolate not found in /etc/subuid` | v2.7 takes box IDs from a subordinate-ID range | `isolate` system user with `2000000-2000999` |
| swap and ASLR suddenly off | `isolate-check-environment --execute` applies contest-machine tweaks | run it without `--execute` (report only) |
| every run fails: `Permission denied` on `/data/...parquet` | boxes run under their own UIDs | data files world-readable inside the closed `/srv/me` |
| progress bar never moves under isolate | a box's files are unreadable from outside until the run ends | progress streams on stderr (`--progress -`) |
| Caddy won't start: can't open `access.log` | `caddy validate` as root created the log root-owned | `chown caddy` after validating |
| Oracle Cloud "out of capacity" for A1 | free ARM capacity in the home region | we moved to AWS |

## 8. Taking it down (after the month)

In the AWS console (**EC2**), in this order:
1. **Instances:** select it → Instance state → **Terminate**. With the default setting,
   this also deletes its 30 GB volume; check under Elastic Block Store → Volumes that
   none is left behind.
2. **Elastic IPs:** select it → Actions → **Release Elastic IP address**. An unattached
   IP is billed.
3. **Security Groups:** delete `me-backtester-sg`. **Key Pairs:** delete the key pair.
   Delete `~/.ssh/me-backtester-ed.pem` locally if you won't need it again.
4. **Billing:** confirm next day that the daily cost has dropped to zero, then delete
   the budget if you like.

On GitHub:
5. **Settings → Environments:** delete `production`, which removes its secrets and
   `SITE_URL`.
6. **Disable the Deploy workflow** (Actions → Deploy → … → Disable workflow), or delete
   `.github/workflows/deploy.yml`. Otherwise every push to `main` tries to deploy to a
   server that no longer exists.
7. **README:** keep a screenshot or short demo video, and remove the live link.
