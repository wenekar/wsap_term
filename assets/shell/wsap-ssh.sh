# Opt-in: make `ssh` set up wsap_term on remote hosts (bash/zsh).
#   source /usr/share/wsap_term/shell/wsap-ssh.sh
ssh() { command wsap-ssh "$@"; }
