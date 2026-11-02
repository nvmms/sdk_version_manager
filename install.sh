#!/bin/sh
# Install the SVM portable CLI from verified GitHub Release assets.
set -eu
set -f

svm_fail() { printf 'svm install: %s\n' "$*" >&2; exit 1; }
svm_fetch() {
    curl --fail --location --silent --show-error --retry 3 --connect-timeout 15 \
        --max-time 600 --proto '=https' --proto-redir '=https' --tlsv1.2 \
        --output "$2" "$1"
}
svm_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}
svm_quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
svm_version_valid() {
    printf '%s\n' "$1" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$'
}
svm_cleanup() {
    [ -z "${svm_profile_tmp:-}" ] || rm -f "$svm_profile_tmp"
    [ -z "${svm_binary_tmp:-}" ] || rm -f "$svm_binary_tmp"
    [ -z "${svm_lock:-}" ] || rmdir "$svm_lock" 2>/dev/null || :
    [ -z "${svm_work:-}" ] || rm -rf "$svm_work"
}
svm_profile() {
    svm_profile_path=$1
    # Preserve dotfile symlinks (including relative links), rather than replacing them.
    svm_link_depth=0
    while [ -L "$svm_profile_path" ]; do
        svm_link_depth=$((svm_link_depth + 1))
        [ "$svm_link_depth" -le 40 ] || svm_fail 'profile symlink loop'
        svm_link=$(readlink "$svm_profile_path")
        case "$svm_link" in /*) svm_profile_path=$svm_link ;; *) svm_profile_path=$(dirname "$svm_profile_path")/$svm_link ;; esac
    done
    mkdir -p "$(dirname "$svm_profile_path")"
    [ ! -e "$svm_profile_path" ] || [ -f "$svm_profile_path" ] || svm_fail "not a regular profile: $svm_profile_path"
    svm_before=missing
    svm_mode=600
    if [ -f "$svm_profile_path" ]; then
        svm_before=$(svm_sha256 "$svm_profile_path")
        if [ "$svm_os" = macos ]; then svm_mode=$(stat -f '%Lp' "$svm_profile_path")
        else svm_mode=$(stat -c '%a' "$svm_profile_path"); fi
        cat "$svm_profile_path" > "$svm_work/profile-original"
    else : > "$svm_work/profile-original"; fi
    # Reject malformed blocks and preserve all unrelated settings.
    awk '
      $0 == "# >>> SVM command path >>>" { if (inside || starts++) exit 2; inside=1; next }
      $0 == "# <<< SVM command path <<<" { if (!inside) exit 2; inside=0; ends++; next }
      !inside { print }
      END { if (inside || starts != ends) exit 2 }
    ' "$svm_work/profile-original" > "$svm_work/profile-next" || svm_fail "malformed SVM PATH block in $svm_profile_path"
    {
        printf '# >>> SVM command path >>>\n'
        printf 'export SVM_INSTALL_DIR=%s\n' "$(svm_quote "$svm_root")"
        printf '%s\n' 'case ":$PATH:" in *":$SVM_INSTALL_DIR/bin:"*) ;; *) export PATH="$SVM_INSTALL_DIR/bin:$PATH" ;; esac'
        printf '%s\n' 'if [ "${SVM_ORIGINAL_PATH+x}" = x ]; then case ":$SVM_ORIGINAL_PATH:" in *":$SVM_INSTALL_DIR/bin:"*) ;; *) export SVM_ORIGINAL_PATH="$SVM_INSTALL_DIR/bin:$SVM_ORIGINAL_PATH" ;; esac; fi'
        printf '# <<< SVM command path <<<\n'
    } >> "$svm_work/profile-next"
    svm_profile_tmp=$(mktemp "$(dirname "$svm_profile_path")/.svm-profile.XXXXXX")
    cat "$svm_work/profile-next" > "$svm_profile_tmp"
    chmod "$svm_mode" "$svm_profile_tmp"
    svm_after=missing
    [ ! -f "$svm_profile_path" ] || svm_after=$(svm_sha256 "$svm_profile_path")
    [ "$svm_before" = "$svm_after" ] || svm_fail "profile changed during installation; retry: $svm_profile_path"
    mv -f "$svm_profile_tmp" "$svm_profile_path"
    svm_profile_tmp=
    printf 'Configured PATH: %s\n' "$svm_profile_path"
}
svm_main() {
    svm_version=${SVM_VERSION:-latest}
    svm_no_profile=${SVM_NO_PROFILE:-0}
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --version) [ "$#" -ge 2 ] || svm_fail '--version requires a value'; svm_version=$2; shift 2 ;;
            --no-profile) svm_no_profile=1; shift ;;
            -h|--help)
                printf '%s\n' 'Usage: sh install.sh [--version v0.1.0] [--no-profile]' \
                    'SVM_INSTALL_DIR defaults to $HOME/.svm. SVM_PROFILE overrides the shell profile.'
                return ;;
            *) svm_fail "unknown argument: $1" ;;
        esac
    done
    [ -n "${HOME:-}" ] || svm_fail 'HOME is not set'
    case "$(uname -s)" in Darwin) svm_os=macos ;; Linux) svm_os=linux ;; *) svm_fail 'supported systems: macOS and Linux; use the portable ZIP on Windows' ;; esac
    case "$(uname -m)" in x86_64|amd64) svm_arch=x64 ;; arm64|aarch64) svm_arch=arm64 ;; *) svm_fail 'supported CPU architectures: x64 and ARM64' ;; esac
    for svm_command in curl tar awk sed grep mktemp stat; do command -v "$svm_command" >/dev/null 2>&1 || svm_fail "required command missing: $svm_command"; done
    command -v sha256sum >/dev/null 2>&1 || command -v shasum >/dev/null 2>&1 || svm_fail 'sha256sum or shasum is required'
    if [ "$svm_version" != latest ]; then
        case "$svm_version" in v*) ;; *) svm_version=v$svm_version ;; esac
        svm_version_valid "$svm_version" || svm_fail 'invalid release version; expected v0.1.0'
    fi
    svm_root=${SVM_INSTALL_DIR:-$HOME/.svm}
    case "$svm_root" in /*) ;; *) svm_fail 'SVM_INSTALL_DIR must be absolute' ;; esac
    # PATH and shell profile text cannot safely represent these directory names.
    case "$svm_root" in *:*|*'
'*) svm_fail 'installation directory must not contain a colon or newline' ;; esac
    mkdir -p "$svm_root"
    svm_root=$(cd "$svm_root" && pwd -P)
    [ "$svm_root" != / ] && [ "$svm_root" != "$(cd "$HOME" && pwd -P)" ] || svm_fail 'refusing to install directly into HOME or filesystem root'
    [ ! -L "$svm_root/bin" ] || svm_fail 'refusing a linked bin directory'
    mkdir -p "$svm_root/bin"
    svm_work=$(mktemp -d "${TMPDIR:-/tmp}/svm-install.XXXXXX")
    trap svm_cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM HUP
    svm_profile_tmp=; svm_binary_tmp=; svm_lock=
    mkdir "$svm_root/.installer-lock" 2>/dev/null || svm_fail 'another installer is active; if interrupted, confirm it has stopped before removing .installer-lock'
    svm_lock=$svm_root/.installer-lock
    svm_binary=$svm_root/bin/svm
    svm_owned=0
    [ ! -L "$svm_root/.installer-owned" ] || svm_fail 'refusing a linked ownership marker'
    [ ! -L "$svm_root/bin/svm.previous" ] || svm_fail 'refusing a linked backup executable'
    [ ! -L "$svm_binary" ] || svm_fail 'refusing to replace a linked svm executable'
    if [ -e "$svm_root/.installer-owned" ]; then
        [ -f "$svm_root/.installer-owned" ] && [ "$(cat "$svm_root/.installer-owned")" = nvmms/sdk_version_manager ] || svm_fail 'unrecognized installer ownership marker'
        svm_owned=1
    fi
    [ ! -e "$svm_root/bin/svm.previous" ] || [ -f "$svm_root/bin/svm.previous" ] || svm_fail 'backup path is not a regular file'
    if [ -e "$svm_binary" ]; then
        [ -f "$svm_binary" ] || svm_fail 'existing svm path is not a regular file'
        [ -f "$svm_root/.installer-owned" ] && [ "$(cat "$svm_root/.installer-owned")" = nvmms/sdk_version_manager ] || svm_fail 'existing svm is not owned by this installer; choose another SVM_INSTALL_DIR'
    fi
    svm_release=https://github.com/nvmms/sdk_version_manager/releases
    if [ "$svm_version" = latest ]; then svm_download=$svm_release/latest/download
    else svm_download=$svm_release/download/$svm_version; fi
    printf 'Fetching SVM %s for %s/%s…\n' "$svm_version" "$svm_os" "$svm_arch"
    svm_fetch "$svm_download/SHA256SUMS" "$svm_work/SHA256SUMS" || svm_fail 'release checksums unavailable; publish a Go CLI release with the packaging workflow first'
    svm_suffix=-$svm_os-$svm_arch.tar.gz
    svm_selected=$(awk -v suffix="$svm_suffix" '
      NF==2 && length($1)==64 && $1 !~ /[^0-9a-f]/ && index($2,"svm-")==1 && substr($2,length($2)-length(suffix)+1)==suffix { print $1 " " $2; found++ }
      END { if (found!=1) exit 1 }
    ' "$svm_work/SHA256SUMS") || svm_fail "release has no unique $svm_os/$svm_arch package"
    svm_expected=${svm_selected%% *}; svm_asset=${svm_selected#* }
    svm_asset_version=${svm_asset#svm-}; svm_asset_version=${svm_asset_version%"$svm_suffix"}
    svm_version_valid "$svm_asset_version" || svm_fail 'invalid archive version in checksum manifest'
    [ "$svm_version" = latest ] || [ "$svm_version" = "$svm_asset_version" ] || svm_fail 'release version and archive do not match'
    # Pin the actual tag resolved by SHA256SUMS to avoid races with a new latest release.
    svm_fetch "$svm_release/download/$svm_asset_version/$svm_asset" "$svm_work/package.tar.gz" || svm_fail 'archive download failed'
    [ "$(svm_sha256 "$svm_work/package.tar.gz")" = "$svm_expected" ] || svm_fail 'SHA-256 mismatch; existing installation was not changed'
    svm_package=${svm_asset%.tar.gz}
    tar -tzf "$svm_work/package.tar.gz" > "$svm_work/members"
    # Extract only known regular files to explicit paths, never unpack arbitrary archive paths or links.
    for svm_member in svm INSTALL.txt BUILD-INFO.json licenses/go-LICENSE.txt licenses/xz-LICENSE.txt; do
        grep -Fx "$svm_package/$svm_member" "$svm_work/members" >/dev/null || svm_fail "archive member missing: $svm_member"
        svm_type=$(tar -tvzf "$svm_work/package.tar.gz" "$svm_package/$svm_member" | cut -c1)
        [ "$svm_type" = - ] || svm_fail "archive member is not a regular file: $svm_member"
    done
    svm_binary_tmp=$(mktemp "$svm_root/bin/.svm-new.XXXXXX")
    tar -xOzf "$svm_work/package.tar.gz" "$svm_package/svm" > "$svm_binary_tmp"
    chmod 755 "$svm_binary_tmp"
    "$svm_binary_tmp" --help > "$svm_work/help" || svm_fail 'downloaded binary cannot run on this machine'
    grep -F 'SDK Version Manager' "$svm_work/help" >/dev/null || svm_fail 'unexpected executable in release archive'
    [ ! -L "$svm_root/licenses" ] || svm_fail 'refusing a linked licenses directory'
    mkdir -p "$svm_root/licenses"
    for svm_member in INSTALL.txt BUILD-INFO.json licenses/go-LICENSE.txt licenses/xz-LICENSE.txt; do
        [ ! -L "$svm_root/$svm_member" ] || svm_fail "refusing linked metadata: $svm_member"
        if [ -e "$svm_root/$svm_member" ]; then
            [ "$svm_owned" = 1 ] && [ -f "$svm_root/$svm_member" ] || svm_fail "refusing to replace unowned metadata: $svm_member"
        fi
        svm_profile_tmp=$(mktemp "$(dirname "$svm_root/$svm_member")/.svm-metadata.XXXXXX")
        tar -xOzf "$svm_work/package.tar.gz" "$svm_package/$svm_member" > "$svm_profile_tmp"
        chmod 644 "$svm_profile_tmp"
        mv -f "$svm_profile_tmp" "$svm_root/$svm_member"; svm_profile_tmp=

    done
    if [ -f "$svm_binary" ]; then
        svm_profile_tmp=$(mktemp "$svm_root/bin/.svm-backup.XXXXXX")
        cp -p "$svm_binary" "$svm_profile_tmp"
        mv -f "$svm_profile_tmp" "$svm_root/bin/svm.previous"; svm_profile_tmp=
    fi
    svm_profile_tmp=$(mktemp "$svm_root/.svm-owner.XXXXXX")
    printf 'nvmms/sdk_version_manager\n' > "$svm_profile_tmp"
    mv -f "$svm_profile_tmp" "$svm_root/.installer-owned"; svm_profile_tmp=

    mv -f "$svm_binary_tmp" "$svm_binary"; svm_binary_tmp=
    if [ "$svm_no_profile" != 1 ]; then
        if [ -n "${SVM_PROFILE:-}" ]; then
            svm_profile "$SVM_PROFILE"
        else
            svm_shell=${SHELL:-}
            case "${svm_shell##*/}" in
                zsh) svm_profile "${ZDOTDIR:-$HOME}/.zshrc" ;;
                bash)
                    svm_profile "$HOME/.bashrc"
                    svm_login=$HOME/.bash_profile
                    for svm_candidate in "$HOME/.bash_profile" "$HOME/.bash_login" "$HOME/.profile"; do
                        if [ -f "$svm_candidate" ]; then svm_login=$svm_candidate; break; fi
                    done
                    svm_profile "$svm_login" ;;
                *) printf 'Shell not auto-configured; add the directory below to PATH.\n' ;;
            esac
        fi
    fi
    printf '\nInstalled SVM %s: %s\n' "$svm_asset_version" "$svm_binary"
    printf 'Open a new terminal, or activate now:\n  export PATH=%s"$PATH"\n' "$(svm_quote "$svm_root/bin:")"
    printf 'Then run: svm --help\n'
}
svm_main "$@"
