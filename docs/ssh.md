# SSH setup

`wsap-ssh` installs remote terminfo on first connection.
It uses `TERM=xterm-ghostty` for remote color detection.
It falls back to `xterm-256color` if terminfo setup fails.
The local terminal uses `TERM=wsap_term-256color`.

## Enable the wrapper

For Bash or Zsh:

```sh
. ~/.local/share/wsap_term/shell/wsap-ssh.sh
```

For Fish:

```fish
source ~/.local/share/wsap_term/shell/wsap-ssh.fish
```

Replace `~/.local` with your install prefix.
You can also run `wsap-ssh host` directly.
The wrapper uses ordinary SSH outside supported terminal sessions.

## Remote color support

SSH servers can discard `COLORTERM` and `TERM_PROGRAM`.
The wrapper sends both variables through `SendEnv`.
To accept them, add this line to the server's SSH configuration:

```text
AcceptEnv COLORTERM TERM_PROGRAM
```

Alternatively, use `WSAP_SSH_FORCE_ENV=1 ssh host` for an interactive session.
Use `WSAP_SSH_SETUP_RC=1 ssh host` to save `COLORTERM` in the remote shell config.

## Environment

| Variable | Effect |
| --- | --- |
| `WSAP_SSH=0` | Use ordinary SSH. |
| `WSAP_SSH_TERMINFO=0` | Skip terminfo setup. |
| `WSAP_SSH_TERM` | Remote terminfo name. Default: `xterm-ghostty`. |
| `WSAP_SSH_TERMINFO_SRC` | Terminfo source when the local database has no entry. |
| `WSAP_SSH_FORCE_ENV=1` | Set `COLORTERM` through the remote login shell. |
| `WSAP_SSH_SETUP_RC=1` | Add `COLORTERM` to the remote shell config. |
| `WSAP_SSH_BIN` | SSH executable. Default: `ssh`. |

## Check the remote session

```sh
echo "$TERM" "$COLORTERM"
tput colors
printf '\033[38;2;255;100;0mTRUECOLOR\033[0m\n'
```
