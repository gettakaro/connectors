#!/usr/bin/env bash

SEMVER_PATTERN='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$'

is_valid_semver() {
  local LC_ALL=C
  local candidate="$1"
  local release_and_prerelease
  local prerelease
  local identifier
  local -a identifiers

  [[ "$candidate" =~ $SEMVER_PATTERN ]] || return 1
  release_and_prerelease="${candidate%%+*}"
  if [[ "$release_and_prerelease" == *-* ]]; then
    prerelease="${release_and_prerelease#*-}"
    IFS='.' read -r -a identifiers <<< "$prerelease"
    for identifier in "${identifiers[@]}"; do
      if [[ "$identifier" =~ ^[0-9]+$ && ${#identifier} -gt 1 && "$identifier" == 0* ]]; then
        return 1
      fi
    done
  fi
}

is_supported_dotnet_version_core() {
  local candidate="$1"
  local core="${candidate%%[-+]*}"
  local component
  local -a components

  IFS='.' read -r -a components <<< "$core"
  for component in "${components[@]}"; do
    if [ "${#component}" -gt 5 ] \
      || { [ "${#component}" -eq 5 ] && [ "$component" -gt 65534 ]; }; then
      return 1
    fi
  done
}

resolve_valheim_release_version() {
  local candidate="$1"

  is_valid_semver "$candidate" || return 1
  is_supported_dotnet_version_core "$candidate" || return 2
  # These globals are output values consumed by scripts that source this helper.
  # shellcheck disable=SC2034
  VALHEIM_RELEASE_VERSION="$candidate"
  VALHEIM_BEPINEX_VERSION="${candidate%%[-+]*}"
  # shellcheck disable=SC2034
  VALHEIM_ASSEMBLY_VERSION="${VALHEIM_BEPINEX_VERSION}.0"
}
