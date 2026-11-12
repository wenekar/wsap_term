# SSH setup

The `wsap-ssh` wrapper installs remote terminfo on the first connection.
It uses `TERM=xterm-ghostty` for remote color detection.
It falls back to `xterm-256color` if terminfo installation fails.
The local terminal sets `TERM=wsap_term-256color`.

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
The wrapper sends both variables using `SendEnv`.
To accept these variables, add this line to the remote `/etc/ssh/sshd_config` or user SSH configuration:

```text
AcceptEnv COLORTERM TERM_PROGRAM
```

Alternatively, run `WSAP_SSH_FORCE_ENV=1 ssh host` for an interactive session.
Run `WSAP_SSH_SETUP_RC=1 ssh host` to save `COLORTERM` in the remote shell configuration.

## Environment

| Variable | Effect |
| --- | --- |
| `WSAP_SSH=0` | Use ordinary SSH. |
| `WSAP_SSH_TERMINFO=0` | Skip terminfo setup. |
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
