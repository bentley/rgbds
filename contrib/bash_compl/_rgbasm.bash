#/usr/bin/env bash

# known bugs - no support for newlines in filenames
# reference: https://iridakos.com/programming/2018/03/01/bash-programmable-completion-tutorial

_rgbasm_completions() {
    local words_len=${#COMP_WORDS[@]} # the # counts the len, @ means all members
    local asm_files=( $(find -maxdepth 1 -type f -iname "*.inc" -o -iname "*.z80" -o -iname "*.asm" -o -iname "*.sm83"|sed -E "s_^./__") )
    # using *.z80 -> if no .z80 files are found it uses literal "*.z80"
    # using ./*.z80 would show options starting with an ugly ./
    # this is a hard decision... maybe use sed to filter ./*?

    COMPREPLY=()

    local current_word="${COMP_WORDS[$COMP_CWORD]}"
    local last_word="${COMP_WORDS[$COMP_CWORD-1]}"

    if [ "${current_word:0:1}-" == "--" ]
       # a flag- autocomplete to any flag
    then
	current_word=${current_word:1:10} # no flag is 10 chars long anyways
	local flags=()
	# generate a list of all flags without the leading minus
	# # compgen doesnt like getting minuses in the last argument - so we remove the minus and add it back later
	single_letter_flags="EhLVvwbDgiMoprW"
	for i in $(seq 1 ${#single_letter_flags})
	do
	    flags+=("${single_letter_flags:i-1:1}")
	done
	flags+=("MG" "MP" "MT" "MQ")

	local f="${flags[@]}"

	local completions=$( compgen -W "$f" "$current_word" )
	# add the - back to the flags...
	COMPREPLY+=( $(echo $completions | sed -E "s/(^| )/ -/g") )
    elif [ "$last_word" == "-i" ]
	 # set include path - suggest folders
    then
	COMPREPLY=( $( compgen -A directory $current_word ) ) # use builtin completion for directories in compgen
    elif [ "$last_word" == "-MQ" ] || [ "$last_word" == "-MT" ]
	 # autocomplete to all files in that case
    then
	COMPREPLY=( $( compgen -A file $current_word ) )
    elif [ "$last_word" == "-W" ]
	 # autocomplete to a warnig
    then 
	warnings=( "error" "all" "extra" "everything" "assert" "backwards-for" "builtin-args" "charmap-redef" )
	warnings+=( "div" "empty-macro-arg" "empty-strrpl" "large-constant" "long-string" "macro-shift" )
	warnings+=( "obsolete" "shift" "shift-amount" "truncation" "user" )

	local f="${warnings[@]}"
	COMPREPLY+=( $( compgen -W "$f" $current_word ) )
    elif [ $words_len==$(($COMP_CWORD+1)) ]
	 #autocomplete at the end of line (empty end) - only show files
    then
	COMPREPLY+=( $( compgen -A directory $current_word ) )

	local f="${asm_files[@]}"
	COMPREPLY+=( $( compgen -A file $current_word | grep -E "\.(sm83|z80|asm|inc)$") )
    fi
}

complete -F _rgbasm_completions rgbasm
