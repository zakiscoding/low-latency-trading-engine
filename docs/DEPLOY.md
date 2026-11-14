# Deploying the website

The site is one Cloudflare Worker (`paper-trading`, on `workers.dev`) with static assets and four Durable
Objects. The home machine connects **out** to it; nothing at home listens publicly.

## 1. Worker

```bash
cd web/worker && npm ci && npm --prefix ../ui ci && npm run deploy
```

`npm run deploy` runs the unit tests, builds the UI (bundling `results/`), then `wrangler deploy`.

Secrets (each command prompts for the value; never paste them into chat or commit them):

```bash
npx wrangler secret put INGEST_SECRET
```

```bash
npx wrangler secret put SESSION_SECRET
```

Generate both with `openssl rand -hex 32`. `INGEST_SECRET` also goes into a file on the home machine
(mode 600) that is passed to telemetry as `--uplink-secret-file`.

Until the steps below are done, the site still works read-only: sign-in reports "isn't configured yet",
and `/admin` returns 503 (it fails closed).

## 2. Bot protection (Turnstile)

Create a Turnstile widget for the `workers.dev` hostname in the dashboard. Put its site key in
`TURNSTILE_SITE_KEY` in `wrangler.jsonc` (it's public), and store the secret with
`npx wrangler secret put TURNSTILE_SECRET`. Without the secret, sign-in, reset and delete are refused.

## 3. OAuth apps (no passwords are ever stored)

- **GitHub:** Settings → Developer settings → OAuth Apps → New. Set the callback URL to
  `https://paper-trading.<subdomain>.workers.dev/auth/github/callback`. Put the client ID in
  `GITHUB_CLIENT_ID` (`wrangler.jsonc`) and store the secret with `wrangler secret put GITHUB_CLIENT_SECRET`.
  The app requests no scopes: it reads only the public profile.
- **Google:** Google Cloud console → APIs & Services → Credentials → OAuth client ID (Web). Use the
  redirect `/auth/google/callback`, scopes `openid profile`, and store the secret as `GOOGLE_CLIENT_SECRET`.

Either provider is optional. The sign-in page shows only the configured ones.

## 4. Admin (Cloudflare Access)

In Zero Trust, add a self-hosted application for `paper-trading.<subdomain>.workers.dev` with path
`/admin`, allowing only the owner's email. Then set `ACCESS_TEAM_DOMAIN`
(`https://<team>.cloudflareaccess.com`) and `ACCESS_AUD` (the application's AUD tag) in `wrangler.jsonc`
and deploy. The Worker verifies the Access JWT on every `/admin` request.

## 5. Home machine: the iMac (always on)

The live stack runs natively on macOS. Colima's VM there drops most outbound connections, so only QuestDB
runs in Colima. Everything lives in `~/lle-live` on the internal disk. launchd agents can't read the
external drive without a privacy permission, and an attempt to run from it failed with exit 78.

Build on the iMac, using Apple clang and Homebrew's `openssl@3`; nothing else needs installing:

```bash
git archive HEAD engine | ssh imac-ts 'mkdir -p ~/lle-live/src && tar -x -C ~/lle-live/src'
```

```bash
ssh imac-ts 'cd ~/lle-live/src && O=/opt/homebrew/opt/openssl@3 && for t in crypto_engine paper_exchange recorder; do clang++ -std=c++20 -O3 -mcpu=apple-m1 -DNDEBUG -I engine/include -I $O/include engine/src/${t}_main.cpp -L $O/lib -lssl -lcrypto -o ../bin/$t; done'
```

Cross-compile telemetry on the laptop (`CGO_ENABLED=0 GOOS=darwin GOARCH=arm64 go build`) and copy it,
`tools/live.sh`, the plist template and `install.sh` with `ssh imac-ts 'cat > file' < file` (`scp` is
disabled on the iMac). Put the ingest secret in `~/lle-live/secrets/ingest` with mode 600, then:

```bash
ssh imac-ts 'bash ~/lle-live/install.sh ~/lle-live'
```

The agent (`com.lle.live`) starts at login and restarts the stack if it exits. Each process is also
restarted by `live.sh`. Logs go to `~/lle-live/logs`. To stop it:

```bash
ssh imac-ts 'launchctl bootout gui/$(id -u)/com.lle.live'
```

QuestDB (local only):

```bash
ssh imac-ts 'cd ~/1tb-drive/lle && docker-compose -p lle -f compose.imac.yml up -d'
```

Only one telemetry uplink may run at a time: the hub keeps the newest connection, so two hosts would keep
replacing each other.

Telemetry refuses `--venue coinbase` together with `--uplink`, because Coinbase's terms don't allow
showing its data publicly.

## Cost

Durable Objects on the Free plan allow 100,000 requests and 13,000 GB-s per day. At 10 Hz, the hub
receives about 864,000 WebSocket messages a day, billed as about 43,000 requests (20:1). It is active
around the clock (about 11,000 GB-s a day), which is close to the free limit. Lower `--hz` or keep the
hub paused when nobody is watching (the default) to stay well under it.
