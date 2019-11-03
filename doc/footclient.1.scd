footclient(1)

# NAME
footclient - start new terminals in a foot server

# SYNOPSIS
*foot* [_OPTIONS_]++
*foot* [_OPTIONS_] -- <command>

All trailing (non-option) arguments are treated as a command to
execute (instead of the shell).

# OPTIONS

*-t*,*--term*=_TERM_
	Value to set the environment variable _TERM_ to. Default: _foot_.

*-v*,*--version*
	Show the version number and quit

# DESCRIPTION

*footclient* is used together with *foot*(1) in *--server*
mode.

Running it without arguments will open a new terminal window (hosted
in the foot server), with your default shell. The exit code will be
that of the terminal (thus, *footclient* does not exit until the
terminal has terminated).

# SEE ALSO

*foot*(1)