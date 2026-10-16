# Opt-in: make `ssh` set up wsap_term on remote hosts.
#   source /usr/share/wsap_term/shell/wsap-ssh.fish
# or copy to ~/.config/fish/conf.d/.
function ssh --wraps ssh --description 'ssh with wsap_term terminfo setup'
    command wsap-ssh $argv
end
