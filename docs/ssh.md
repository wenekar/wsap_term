# SSH setup

This guide explains how `wsap-ssh` sets up terminal capabilities for SSH sessions. The wrapper tries to install terminfo in the remote user's home directory unless it has cached a successful installation for the same destination and source. The remote host needs `tic` to compile the entry.

| Terminal setting | Value |
|---|---|
| Local `TERM` | `wsap_term-256color` |
| Remote `TERM` with the default terminfo entry | `xterm-ghostty` |
| Remote fallback when terminfo setup fails, is unavailable, or is disabled | `xterm-256color` |

## Enable the wrapper

Add the wrapper script to your shell startup configuration.

For Bash or Zsh:

```sh
. ~/.local/share/wsap_term/shell/wsap-ssh.sh
```

For Fish:

```fish
source ~/.local/share/wsap_term/shell/wsap-ssh.fish
```

Replace `~/.local` with your installation prefix.
You can also run `wsap-ssh host` directly.
The wrapper calls standard `ssh` outside supported terminal sessions.

## Remote color support

Remote SSH servers often discard `COLORTERM` and `TERM_PROGRAM`.
The wrapper asks SSH to forward both variables. To accept them, configure `AcceptEnv` on the remote SSH server, not in your local client configuration:

```text
AcceptEnv COLORTERM TERM_PROGRAM
```

Alternatively, run `WSAP_SSH_FORCE_ENV=1 ssh host` for an interactive session.
Run `WSAP_SSH_SETUP_RC=1 ssh host` to add `COLORTERM` to existing remote shell startup files. This modifies those files on the remote host.

## Environment

| Variable | Effect |
| --- | --- |
| `WSAP_SSH=0` | Use ordinary SSH. |
| `WSAP_SSH_TERMINFO=0` | Skip terminfo setup and use the fallback `TERM` |
| `WSAP_SSH_TERM` | Remote terminfo entry name. Default: `xterm-ghostty`. |
| `WSAP_SSH_TERMINFO_SRC` | Path to a source terminfo file if not found locally. |
| `WSAP_SSH_FORCE_ENV=1` | Force `COLORTERM` through the remote login shell. |
| `WSAP_SSH_SETUP_RC=1` | Add `COLORTERM` to the remote shell startup files. |
| `WSAP_SSH_BIN` | Custom SSH executable. Default: `ssh`. |

## Check the remote session

Verify terminal and color support in the remote shell:

```sh
echo "$TERM" "$COLORTERM"
tput colors
printf '\033[38;2;255;100;0mTRUECOLOR\033[0m\n'
```
