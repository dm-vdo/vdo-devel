# bash completion for vdostats
#
# %COPYRIGHT%
#
# %LICENSE%
#
# TODO :  Add device name at the end of completion of the commands.

_vdostats()
{
    local opts cur
    _init_completion || return
    COMPREPLY=()
    opts="--help --all --human-readable --si --verbose --version"
    cur="${COMP_WORDS[COMP_CWORD]}"
    case "${cur}" in
        *)
               COMPREPLY=( $(compgen -W "${opts}" -- ${cur}) )
            ;;
    esac
}

complete -F _vdostats vdostats
