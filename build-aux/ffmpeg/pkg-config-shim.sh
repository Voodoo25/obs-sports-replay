#!/usr/bin/env bash
# Minimal pkg-config for FFmpeg's configure on MSYS2 installs without one.
# Reads only the .pc files in PKG_CONFIG_PATH (x264 and ffnvcodec here) and
# supports what configure asks: --version, --exists with version
# constraints, --modversion, --cflags and --libs (with or without --static).

set -u

mode=""
static=0
args=()
for a in "$@"; do
	case "$a" in
	--version) echo "0.29.2"; exit 0 ;;
	--exists | --modversion | --cflags | --libs) mode=$a ;;
	--static) static=1 ;;
	--print-errors | --short-errors | --errors-to-stdout) ;;
	--*) ;;
	# configure passes "name >= version" as one argument: split it
	*) read -r -a words <<<"$a" && args+=("${words[@]}") ;;
	esac
done

find_pc() {
	local IFS=':'
	for d in ${PKG_CONFIG_PATH:-}; do
		[[ -f "$d/$1.pc" ]] && { echo "$d/$1.pc"; return 0; }
	done
	return 1
}

# pc_field <file> <Field>: value with ${var} references expanded
pc_field() {
	local file=$1 field=$2 line key val
	declare -A vars=()
	while IFS= read -r line || [[ -n $line ]]; do
		line=${line%$'\r'}
		if [[ $line =~ ^([A-Za-z0-9_.]+)=(.*)$ ]]; then
			key=${BASH_REMATCH[1]}
			val=${BASH_REMATCH[2]}
			while [[ $val =~ \$\{([A-Za-z0-9_.]+)\} ]]; do
				val=${val//\$\{${BASH_REMATCH[1]}\}/${vars[${BASH_REMATCH[1]}]:-}}
			done
			vars[$key]=$val
		elif [[ $line =~ ^([A-Za-z.]+):[[:space:]]*(.*)$ && ${BASH_REMATCH[1]} == "$field" ]]; then
			val=${BASH_REMATCH[2]}
			while [[ $val =~ \$\{([A-Za-z0-9_.]+)\} ]]; do
				val=${val//\$\{${BASH_REMATCH[1]}\}/${vars[${BASH_REMATCH[1]}]:-}}
			done
			echo "$val"
			return 0
		fi
	done <"$file"
}

# version_ok <have> <op> <want>
version_ok() {
	local lower
	lower=$(printf '%s\n%s\n' "$1" "$3" | sort -V | head -n1)
	case "$2" in
	">=") [[ $lower == "$3" ]] ;;
	">") [[ $lower == "$3" && $1 != "$3" ]] ;;
	"<=") [[ $lower == "$1" ]] ;;
	"<") [[ $lower == "$1" && $1 != "$3" ]] ;;
	"=") [[ $1 == "$3" ]] ;;
	*) return 1 ;;
	esac
}

# group the arguments into packages, each with optional constraints
pkgs=()
i=0
while ((i < ${#args[@]})); do
	name=${args[i]}
	pc=$(find_pc "$name") || { echo "Package $name was not found" >&2; exit 1; }
	have=$(pc_field "$pc" Version)
	if ((i + 2 < ${#args[@]})) && [[ ${args[i + 1]} =~ ^(>=|<=|>|<|=)$ ]]; then
		version_ok "$have" "${args[i + 1]}" "${args[i + 2]}" || {
			echo "Requested '$name ${args[i + 1]} ${args[i + 2]}' but version of $name is $have" >&2
			exit 1
		}
		i=$((i + 3))
	else
		i=$((i + 1))
	fi
	[[ " ${pkgs[*]-} " == *" $name "* ]] || pkgs+=("$name")
done

out=()
for name in "${pkgs[@]}"; do
	pc=$(find_pc "$name")
	case "$mode" in
	--modversion) pc_field "$pc" Version ;;
	--cflags) out+=("$(pc_field "$pc" Cflags)") ;;
	--libs)
		out+=("$(pc_field "$pc" Libs)")
		((static)) && out+=("$(pc_field "$pc" Libs.private)")
		;;
	esac
done
[[ ${#out[@]} -gt 0 ]] && echo "${out[*]}"
exit 0
