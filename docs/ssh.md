# Colors over ssh

## Why it breaks

Only `TERM` is guaranteed to survive an ssh connection — it rides in the
protocol's `pty-req`, so `AcceptEnv` never touches it. Every other variable
goes through an `env` request, which sshd **silently drops** unless it matches
its `AcceptEnv` directive. Both `SendEnv COLORTERM` and `SetEnv
COLORTERM=truecolor` are subject to this; `ssh_config(5)` names `TERM` as the
sole exception. Many distro sshd configs ship no `AcceptEnv` line at all.

So on a remote host, a wsap_term session has `TERM` and nothing else.

That matters because not every program reads terminfo. Node CLIs (Claude Code,
anything on chalk/`supports-color`) decide color support from environment
variables alone:

1. `COLORTERM=truecolor` → truecolor — dropped by ssh
2. `TERM=xterm-ghostty` (or `xterm-direct`, `xterm-kitty`, `wezterm`) → truecolor (Level 3)
3. `TERM` matching `-256(color)?$` → 256 colors (Level 2 — colors downsampled / washed out)
4. `TERM` matching `^screen|^xterm|^vt100|^vt220|^rxvt|color|ansi|cygwin|linux` → 16 colors
5. otherwise → **no color at all**

Because `supports-color` directly treats `xterm-ghostty` as truecolor-capable (even without `COLORTERM`), `wsap-ssh` pushes the `xterm-ghostty` terminfo definition to the remote and sets `TERM=xterm-ghostty`.

## What wsap_term does

**Default local `TERM` is `wsap_term-256color`** (`src/pty/plan.c`), while **`wsap-ssh`** handles remote SSH connections by pushing and activating `xterm-ghostty` terminfo:

* pushes `xterm-ghostty` (or `wsap_term`) terminfo to the remote on first connect
  (`infocmp -x | ssh … tic -x -o ~/.terminfo -`), cached per host in
  `$XDG_CACHE_HOME/wsap_term/ssh-hosts`.
* sets `TERM=xterm-ghostty` on the remote session, guaranteeing full 24-bit Truecolor in Claude Code, Neovim, tmux, and Node/Python CLI apps even when SSH drops `COLORTERM`.
* falls back to `TERM=xterm-256color` when the remote has no `tic`.
* forwards `COLORTERM` and `TERM_PROGRAM` via `SendEnv` — best effort, see above.
* reuses one connection (`ControlMaster`) so setup costs at most one auth prompt.

Enable it by shadowing `ssh`:

```fish
source /usr/share/wsap_term/shell/wsap-ssh.fish   # fish
```
```sh
. /usr/share/wsap_term/shell/wsap-ssh.sh          # bash/zsh
```

It passes through untouched when `TERM` isn't a `wsap_term*`, so it is safe
inside tmux, other terminals, and non-interactive use.

## Getting truecolor, in order of reliability

1. **Remote shell rc** — always works, no server config:

   ```sh
   WSAP_SSH_SETUP_RC=1 ssh host
   ```

   Appends `COLORTERM=truecolor` to the remote `.bashrc`/`.zshrc`/`.profile`/
   `config.fish`, guarded by a `WSAP_COLORTERM` marker so it is written once.

2. **Wrap the remote login shell** — no server config, per-session:

   ```sh
   WSAP_SSH_FORCE_ENV=1 ssh host
   ```

   Runs `exec env COLORTERM=truecolor $SHELL -l` on the far side. Skipped
   automatically when a remote command is given or stdin isn't a tty.

3. **`AcceptEnv`** — cleanest where you have root on the server:

   ```
   # /etc/ssh/sshd_config
   AcceptEnv COLORTERM TERM_PROGRAM
   ```

   `wsap-ssh` already sends them; this is the only missing half.

## Environment

| Variable | Effect |
| --- | --- |
| `WSAP_SSH=0` | bypass entirely, plain ssh |
| `WSAP_SSH_TERMINFO=0` | skip the terminfo push (falls back to `xterm-256color`) |
| `WSAP_SSH_TERMINFO_SRC` | terminfo description to push when the local database has none |
| `WSAP_SSH_FORCE_ENV=1` | wrap the remote login shell to set `COLORTERM` |
| `WSAP_SSH_SETUP_RC=1` | write the `COLORTERM` export into the remote shell rc |
| `WSAP_SSH_BIN` | ssh binary to use (default `ssh`) |

## Verifying

On the remote host:

```sh
echo $TERM $COLORTERM
tput colors
printf '\e[38;2;255;100;0mTRUECOLOR\e[0m \e[38;5;208m256\e[0m\n'
```
